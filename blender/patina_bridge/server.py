"""Networking for the Patina bridge, built so that Blender's UI never blocks.

Threading model
---------------
* **Main thread** (``bpy.app.timers``): a single persistent "tick" timer
  polls the non-blocking :class:`protocol.PollingLineServer` (incoming
  commands from Patina), drains the results of outgoing requests and runs
  their callbacks, sends a periodic status ``ping`` and reaps launched
  processes. Every ``bpy`` access happens here.
* **One worker thread** performs outgoing requests to Patina with the blocking
  :class:`protocol.JsonLineClient` (3 s connect / 60 s response timeout). It
  never touches ``bpy``: it takes jobs from ``Worker.jobs`` and puts
  ``(job, result, error)`` tuples on ``Worker.results``.
* Incoming ``patina_textures_updated`` messages are answered immediately
  (``{"queued": true}``) and the actual work - reading UVs, loading images,
  building the material - is deferred to a one-shot ``bpy.app.timers``
  callback. Repeated updates for the same object are coalesced.

In background mode (``blender -b``) timers do not run, so outgoing requests
are performed synchronously instead.
"""

from __future__ import annotations

import os
import queue
import shutil
import subprocess
import sys
import threading
import time
import traceback
from typing import Any, Callable, Dict, List, Optional, Tuple

import bpy

from . import protocol

ADDON_ID = __package__  # "patina_bridge" (legacy) or "bl_ext.<repo>.patina_bridge" (extension)

TICK_ACTIVE = 0.1
TICK_IDLE = 0.5
STATUS_PING_INTERVAL = 5.0
LAUNCH_TIMEOUT = 10.0
LAUNCH_RETRY_INTERVAL = 0.5
CONNECT_TIMEOUT = 3.0
RESPONSE_TIMEOUT = 60.0

DEFAULT_EXCHANGE_DIR = os.path.join("~", ".cache", "patina", "bridge")


# --------------------------------------------------------------------------- preferences access


def get_prefs(context=None):
    """Return the add-on preferences, or ``None`` if unavailable (e.g. during unregister)."""
    ctx = context or bpy.context
    try:
        return ctx.preferences.addons[ADDON_ID].preferences
    except (KeyError, AttributeError):
        return None


def pref(name: str, default: Any) -> Any:
    p = get_prefs()
    return getattr(p, name, default) if p is not None else default


def patina_port() -> int:
    return int(pref("patina_port", protocol.DEFAULT_PATINA_PORT))


def blender_port() -> int:
    return int(pref("blender_port", protocol.DEFAULT_BLENDER_PORT))


def exchange_dir() -> str:
    """Absolute exchange folder: preference, else ``$PATINA_BRIDGE_DIR``, else the default."""
    raw = (pref("exchange_dir", "") or "").strip()
    if not raw:
        raw = os.environ.get("PATINA_BRIDGE_DIR", "") or DEFAULT_EXCHANGE_DIR
    if raw.startswith("//"):
        raw = bpy.path.abspath(raw)
    return os.path.normpath(os.path.abspath(os.path.expanduser(raw)))


# --------------------------------------------------------------------------- state & notification


class BridgeState:
    """Everything the panel shows. Only mutated on the main thread."""

    def __init__(self) -> None:
        self.listener: Optional[protocol.PollingLineServer] = None
        self.listener_error = ""
        self.patina_online: Optional[bool] = None  # None = unknown yet
        self.patina_info: Dict[str, Any] = {}
        self.last_contact = 0.0  # monotonic time of the last message from/to Patina
        self.message = "Idle"
        self.message_level = "INFO"
        self.launch: Optional["_Launch"] = None
        self.processes: List[subprocess.Popen] = []
        self.last_ping = 0.0
        self.ping_in_flight = False
        # object name -> export info of the last send (used to decide whether UVs may come back)
        self.sent: Dict[str, Dict[str, Any]] = {}

    @property
    def listening(self) -> bool:
        return self.listener is not None and self.listener.running


state = BridgeState()


def tag_redraw() -> None:
    """Redraw 3D-viewport sidebars so the panel shows fresh status."""
    try:
        wm = bpy.context.window_manager
        for window in wm.windows:
            for area in window.screen.areas:
                if area.type == "VIEW_3D":
                    for region in area.regions:
                        if region.type == "UI":
                            region.tag_redraw()
    except Exception:
        pass


def notify(level: str, message: str, popup: bool = True) -> None:
    """Show ``message`` in the panel, print it and (if possible) in Blender's status bar.

    ``level`` is one of 'INFO', 'WARNING', 'ERROR'. Safe to call from timers.
    """
    state.message = message
    state.message_level = level
    print("[Patina] %s: %s" % (level, message))
    tag_redraw()
    if not popup or bpy.app.background:
        return
    try:
        wm = bpy.context.window_manager
        if not wm.windows:
            return
        window = bpy.context.window or wm.windows[0]
        # 'ERROR' reports raise inside bpy.ops; the operator downgrades them for display.
        with bpy.context.temp_override(window=window, screen=window.screen):
            bpy.ops.patina.report(level=level, message=message)
    except Exception:
        pass  # the panel + console already have the message


# --------------------------------------------------------------------------- worker thread


class _Job:
    __slots__ = ("cmd", "args", "port", "timeout", "on_done", "quiet")

    def __init__(self, cmd: str, args: Optional[Dict[str, Any]], port: int, timeout: float,
                 on_done: Optional[Callable[[Any, Optional[BaseException]], None]], quiet: bool):
        self.cmd = cmd
        self.args = args or {}
        self.port = port
        self.timeout = timeout
        self.on_done = on_done
        self.quiet = quiet


class Worker:
    """A daemon thread that performs blocking requests to Patina, one at a time.

    Each thread owns its job queue and its connections, so a thread that is still
    finishing a request after :meth:`stop` (e.g. during add-on reload) can never
    pick up jobs meant for its successor.
    """

    def __init__(self) -> None:
        self.jobs: "queue.Queue[Optional[_Job]]" = queue.Queue()
        self.results: "queue.Queue[tuple]" = queue.Queue()
        self._thread: Optional[threading.Thread] = None
        self._pending = 0
        self._lock = threading.Lock()

    @property
    def pending(self) -> int:
        with self._lock:
            return self._pending

    def submit(self, job: _Job) -> None:
        if self._thread is None or not self._thread.is_alive():
            self.jobs = queue.Queue()
            self._thread = threading.Thread(target=self._run, args=(self.jobs,), name="PatinaBridgeWorker",
                                            daemon=True)
            self._thread.start()
        with self._lock:
            self._pending += 1
        self.jobs.put(job)

    def _run(self, jobs: "queue.Queue[Optional[_Job]]") -> None:
        clients: Dict[int, protocol.JsonLineClient] = {}
        try:
            while True:
                job = jobs.get()
                if job is None:
                    break
                result, error = None, None
                try:
                    client = clients.get(job.port)
                    if client is None:
                        client = clients[job.port] = protocol.JsonLineClient(
                            protocol.LOCALHOST, job.port, CONNECT_TIMEOUT, RESPONSE_TIMEOUT)
                    result = client.request(job.cmd, job.args, timeout=job.timeout)
                except BaseException as exc:  # noqa: BLE001 - reported on the main thread
                    error = exc
                with self._lock:
                    self._pending = max(0, self._pending - 1)
                self.results.put((job, result, error))
        finally:
            for client in clients.values():
                client.close()

    def stop(self) -> None:
        """Ask the thread to exit after its current request; queued jobs are dropped."""
        if self._thread is not None and self._thread.is_alive():
            while True:
                try:
                    self.jobs.get_nowait()
                except queue.Empty:
                    break
            self.jobs.put(None)
            self._thread.join(timeout=0.5)
        self._thread = None
        with self._lock:
            self._pending = 0


worker = Worker()


def _record_contact(error: Optional[BaseException]) -> None:
    online = not isinstance(error, (protocol.BridgeConnectionError, ConnectionError, OSError))
    if state.patina_online != online:
        state.patina_online = online
        tag_redraw()
    if online:
        state.last_contact = time.monotonic()


def _finish_job(job: _Job, result: Any, error: Optional[BaseException]) -> None:
    _record_contact(error)
    if job.on_done is None:
        if error is not None and not job.quiet:
            notify("ERROR", describe_error(error))
        return
    try:
        job.on_done(result, error)
    except Exception as exc:  # never let a callback bug escape into the timer
        traceback.print_exc()
        notify("ERROR", "Internal error handling %s: %s" % (job.cmd, exc))


def describe_error(error: BaseException) -> str:
    """Human-readable text for an exception from a request."""
    if isinstance(error, protocol.RemoteError):
        return "Patina: %s" % error
    if isinstance(error, (protocol.BridgeConnectionError, ConnectionError)):
        return "Patina is not reachable on port %d - is it running? (%s)" % (patina_port(), error)
    return "%s: %s" % (type(error).__name__, error)


def send_command(cmd: str, args: Optional[Dict[str, Any]] = None,
                 on_done: Optional[Callable[[Any, Optional[BaseException]], None]] = None,
                 timeout: float = RESPONSE_TIMEOUT, quiet: bool = False) -> None:
    """Send ``cmd`` to Patina without blocking; ``on_done(result, error)`` runs on the main thread."""
    job = _Job(cmd, args, patina_port(), timeout, on_done, quiet)
    if bpy.app.background:  # no event loop -> no timers: do it synchronously
        result, error = None, None
        try:
            result = protocol.request_once(cmd, args, port=job.port, connect_timeout=CONNECT_TIMEOUT, timeout=timeout)
        except Exception as exc:  # noqa: BLE001
            error = exc
        _finish_job(job, result, error)
        return
    ensure_tick()
    worker.submit(job)


# --------------------------------------------------------------------------- incoming commands

_pending_updates: Dict[str, Dict[str, Any]] = {}


def _handle_incoming(cmd: str, args: Dict[str, Any], _msg: Dict[str, Any]) -> Any:
    """Handler for :class:`protocol.PollingLineServer` - runs on the main thread, must be quick."""
    state.last_contact = time.monotonic()
    if state.patina_online is not True:
        state.patina_online = True
        tag_redraw()
    if cmd == "ping":
        return {"app": "blender", "version": bpy.app.version_string, "addon": "patina_bridge",
                "addon_version": addon_version_string()}
    if cmd == "patina_textures_updated":
        name = args.get("object")
        if not isinstance(name, str) or not name:
            raise protocol.CommandError("'object' must be a non-empty string")
        obj = bpy.data.objects.get(name)
        if obj is None:
            raise protocol.CommandError("Blender has no object named %r" % name)
        if obj.type != "MESH":
            raise protocol.CommandError("Object %r is not a mesh" % name)
        textures = args.get("textures") or {}
        if not isinstance(textures, dict):
            raise protocol.CommandError("'textures' must be an object of key -> path")
        uv_path = args.get("uv_path") or None
        if uv_path is not None and not isinstance(uv_path, str):
            raise protocol.CommandError("'uv_path' must be a string or null")
        sets = args.get("texture_sets")
        if sets is not None and not isinstance(sets, list):
            raise protocol.CommandError("'texture_sets' must be a list")
        queue_result(name, textures, uv_path, sets, args.get("tile_mode") or {})
        return {"queued": True, "object": name}
    if cmd == "patina_retopo_mesh":
        # Patina's retopology: quads made over the linked object, as a new object in the same place
        src = args.get("object")
        path = args.get("path")
        new_name = args.get("name") or ("%s_retopo" % src)
        if not isinstance(src, str) or not src or bpy.data.objects.get(src) is None:
            raise protocol.CommandError("Blender has no object named %r" % src)
        if not isinstance(path, str) or not os.path.isfile(path):
            raise protocol.CommandError("no quads file at %r" % path)
        if not isinstance(new_name, str):
            raise protocol.CommandError("'name' must be a string")
        _pending_retopo.append((src, path, new_name))
        if bpy.app.background:
            _apply_retopo()
        elif not bpy.app.timers.is_registered(_apply_retopo):
            bpy.app.timers.register(_apply_retopo, first_interval=0.0, persistent=True)
        return {"queued": True, "object": new_name}
    if cmd == "patina_new_object":
        # Patina's model (mended, decimated...) as a Blender object, then its textures on it (add-on 0.9)
        path = args.get("path")
        name = args.get("name")
        src = args.get("source") or ""
        if not isinstance(path, str) or not os.path.isfile(path):
            raise protocol.CommandError("no mesh file at %r" % path)
        if not isinstance(name, str) or not name:
            raise protocol.CommandError("'name' must be a non-empty string")
        if src and bpy.data.objects.get(src) is None:
            raise protocol.CommandError("Blender has no object named %r" % src)
        _pending_new.append(dict(args))
        if bpy.app.background:
            _apply_new()
        elif not bpy.app.timers.is_registered(_apply_new):
            bpy.app.timers.register(_apply_new, first_interval=0.0, persistent=True)
        return {"queued": True, "object": name}
    if cmd == "patina_material_library":
        # Patina's "Add the material library to Blender": bake and build it from here
        from . import library

        size = args.get("size", 1024)
        mapping = str(args.get("mapping", "box")).upper()
        which = args.get("which", "all")
        if size not in (512, 1024, 2048) or mapping not in ("BOX", "UV") or which not in ("all", "builtin"):
            raise protocol.CommandError("size 512/1024/2048, mapping box/uv, which all/builtin")
        err = library.start(size, mapping, which)
        if err:
            raise protocol.CommandError(err)
        return {"started": True, "directory": library.library_dir()}
    raise protocol.CommandError("Unknown command %r" % cmd)


_pending_retopo: List[Tuple[str, str, str]] = []
_pending_new: List[Dict[str, Any]] = []


def _apply_new() -> None:
    """One-shot timer: make the objects Patina sent (patina_new_object), link them and put their textures on."""
    from . import mesh_io

    while _pending_new:
        a = _pending_new.pop(0)
        name, src_name = a["name"], a.get("source") or ""
        try:
            src = bpy.data.objects.get(src_name) if src_name else None
            obj = mesh_io.object_from_ptmesh(a["path"], name, src, bool(a.get("replace")))
            # from now on this object is the one linked to Patina: the mesh Patina sent is its "sent" mesh
            state.sent[obj.name] = {"path": a["path"], "faces": len(obj.data.polygons), "evaluated": False,
                                    "had_uvs": True, "from_patina": True}
            _undo_push("Patina model")
            notify("INFO", "Patina: %s (%d faces)" % (obj.name, len(obj.data.polygons)), popup=False)
            textures = a.get("textures") or {}
            if textures or a.get("texture_sets"):
                queue_result(obj.name, textures, None, a.get("texture_sets"), a.get("tile_mode") or {})
        except Exception as e:  # noqa: BLE001 - reported to the user, Blender keeps running
            traceback.print_exc()
            notify("ERROR", "Patina: the model could not be made: %s" % e)
    return None


def _apply_retopo() -> None:
    """One-shot timer: make (or update) the retopology objects queued by Patina."""
    while _pending_retopo:
        src_name, path, new_name = _pending_retopo.pop(0)
        try:
            obj = make_retopo_object(src_name, path, new_name)
            notify("INFO", "Patina: %s (%d quads)" % (obj.name, len(obj.data.polygons)), popup=False)
        except Exception as e:  # noqa: BLE001 - reported to the user, Blender keeps running
            notify("ERROR", "Patina: the quads could not be made: %s" % e)
    return None


def make_retopo_object(src_name: str, path: str, new_name: str):
    """Build the mesh from Patina's .ptmesh and put it next to ``src_name`` with the same transform.
    Sending again replaces the mesh of the object made last time."""
    header, blocks = protocol.read_ptmesh(path)
    pos = blocks["positions"].to_array(use_numpy=False)
    sizes = blocks["face_sizes"].to_array(use_numpy=False)
    loops = blocks["loop_verts"].to_array(use_numpy=False)
    verts = [(pos[i], pos[i + 1], pos[i + 2]) for i in range(0, len(pos), 3)]
    faces, k = [], 0
    for n in sizes:
        faces.append(tuple(loops[k:k + n]))
        k += n
    src = bpy.data.objects[src_name]
    mesh = bpy.data.meshes.new(new_name)
    mesh.from_pydata(verts, [], faces)
    mesh.validate()
    mesh.update()
    for p in mesh.polygons:
        p.use_smooth = True
    obj = bpy.data.objects.get(new_name)
    if obj is not None and obj.type == "MESH":
        old = obj.data
        obj.data = mesh
        if old.users == 0:
            bpy.data.meshes.remove(old)
    else:
        obj = bpy.data.objects.new(new_name, mesh)
        cols = list(src.users_collection) or [bpy.context.scene.collection]
        for c in cols:
            c.objects.link(obj)
    try:  # the source's matrix as it is now (a move since the last redraw included)
        bpy.context.view_layer.update()
    except Exception:  # noqa: BLE001 - no view layer in some contexts; the stored matrix is close
        pass
    obj.matrix_world = src.matrix_world.copy()
    _undo_push("Patina retopology")
    tag_redraw()
    return obj


def queue_result(obj_name: str, textures: Dict[str, Any], uv_path: Optional[str],
                 texture_sets: Optional[List[Any]] = None, tile_mode: Optional[Dict[str, Any]] = None) -> None:
    """Schedule applying textures/UVs to ``obj_name`` on the main thread (coalesces repeats)."""
    pending = _pending_updates.get(obj_name)
    if pending is None:
        _pending_updates[obj_name] = {"textures": dict(textures), "uv_path": uv_path,
                                      "texture_sets": texture_sets, "tile_mode": tile_mode or {}}
    else:
        pending["textures"].update(textures)
        if uv_path:
            pending["uv_path"] = uv_path
        pending["texture_sets"] = texture_sets  # the latest layout wins
        pending["tile_mode"] = tile_mode or {}
    if bpy.app.background:
        _apply_pending()
    elif not bpy.app.timers.is_registered(_apply_pending):
        # persistent: a file load in between must not strand the queued updates
        bpy.app.timers.register(_apply_pending, first_interval=0.0, persistent=True)


def _apply_pending() -> None:
    """One-shot timer: apply every queued update."""
    while _pending_updates:
        name, data = next(iter(_pending_updates.items()))
        del _pending_updates[name]
        apply_patina_result(name, data["textures"], data["uv_path"], data.get("texture_sets"), data.get("tile_mode"))
    return None


def _group_sets(texture_sets: List[Any], tile_mode: Dict[str, Any]) -> List[Dict[str, Any]]:
    """Patina's tiles -> one material each: per material slot (texture sets) or one for the object,
    with each map's UDIM tiles together. Returns [{"name", "slot", "textures", "udim_tiles"}]."""
    by_slot = bool(tile_mode.get("material_slots"))
    udim = bool(tile_mode.get("udim"))
    groups: Dict[Any, Dict[str, Any]] = {}
    for t in texture_sets:
        if not isinstance(t, dict):
            continue
        slot = int(t.get("slot", 0) or 0)
        key = slot if by_slot else None
        g = groups.setdefault(key, {"name": str(t.get("set") or t.get("name") or "") if by_slot else "",
                                    "slot": slot if by_slot else None, "textures": {}, "udim_tiles": {}})
        for k, p in (t.get("textures") or {}).items():
            if udim:
                g["udim_tiles"].setdefault(k, {})[int(t.get("udim", 1001) or 1001)] = p
            else:
                g["textures"][k] = p
    out = [groups[k] for k in sorted(groups, key=lambda k: -1 if k is None else k)]
    # the same material in two slots: names (and so images and materials) must still differ
    names = [g["name"] for g in out]
    for g in out:
        if g["name"] and names.count(g["name"]) > 1:
            g["name"] = "%s_%d" % (g["name"], g["slot"] + 1)
    return out


def apply_patina_result(obj_name: str, textures: Dict[str, Any], uv_path: Optional[str],
                        texture_sets: Optional[List[Any]] = None, tile_mode: Optional[Dict[str, Any]] = None) -> bool:
    """Apply a push from Patina (see :func:`_apply_patina_result`)."""
    return _apply_patina_result(obj_name, textures, uv_path, texture_sets, tile_mode)


def _apply_patina_result(obj_name: str, textures: Dict[str, Any], uv_path: Optional[str],
                         texture_sets: Optional[List[Any]] = None, tile_mode: Optional[Dict[str, Any]] = None) -> bool:
    """Apply UVs (if any) and textures from Patina to an object. Main thread only. Never raises.

    ``texture_sets`` (Patina with texture sets per material slot and / or UDIM tiles): a material
    per slot, tiled images for UDIM tiles."""
    from . import materials, mesh_io

    try:
        obj = bpy.data.objects.get(obj_name)
        if obj is None or obj.type != "MESH":
            notify("ERROR", "Patina sent textures for %r, but there is no such mesh object" % obj_name)
            return False
        warnings: List[str] = []
        notes: List[str] = []

        uv_applied = uv_changed = False
        uv_error = ""
        if uv_path:
            sent = state.sent.get(obj_name, {})
            # The .ptmesh of the last send (also after a restart of Blender: its header says
            # whether modifiers were applied, its loop_verts guard against changed topology).
            sent_path = sent.get("path") or mesh_io.ptmesh_path(exchange_dir(), obj_name)
            if sent.get("evaluated"):
                uv_error = ("the mesh was sent with modifiers that change its faces applied - turn off 'Apply "
                            "Modifiers When Sending', send it again and repaint")
            else:
                try:
                    uv_applied, msg = mesh_io.apply_ptuv(obj, uv_path, sent_ptmesh=sent_path)
                    if not uv_applied:
                        uv_error = msg[len("UVs not applied: "):] if msg.startswith("UVs not applied: ") else msg
                    else:
                        uv_changed = not msg.startswith("UVs unchanged")
                        if uv_changed:  # Blender's UVs are Patina's now: nothing to send back
                            from . import autosync

                            autosync.uvs_written(obj_name, obj)
                        for part in msg.split("; "):
                            if part in ("UVs applied", "UVs unchanged"):
                                continue
                            # new texture sets are news, not a problem
                            (warnings if "skipped" in part else notes).append(part)
                except Exception as exc:  # noqa: BLE001
                    traceback.print_exc()
                    uv_error = str(exc)
            topology = "loops but" in uv_error or "face corners" in uv_error  # not UVs edited in Blender
            if topology and not sent.get("evaluated") and mesh_io.patina_kept_uvs(uv_path, sent_path):
                # (0.14.1) the mesh changed in Blender since it was sent, but Patina kept the UVs it got: they are
                # still Blender's UVs, so the textures fit every face that was left alone. Refusing them made the
                # user send the mesh again (or fear 'Update Mesh') for a push that was safe.
                warnings.append("%s changed in Blender since it was sent to Patina (%s). The textures were loaded: "
                                "Patina kept the UVs, so they fit every face you didn't change. To paint the "
                                "changed faces, send it to Patina again: the paint carries over"
                                % (obj_name, uv_error.split(" (")[0] if "loops but" in uv_error else
                                   "its face corners are not the ones sent"))
                uv_error = ""
        if uv_error:
            # The textures are painted for Patina's UV layout: on the old UVs they would show up
            # scrambled, so nothing changes until the UVs can come across too.
            notify("ERROR", "%s not updated: Patina changed its UVs, and they could not be applied (%s). "
                            "The textures were left as they were, because they only fit Patina's UVs."
                   % (obj_name, uv_error))
            return False

        if texture_sets:
            n_images = 0
            groups = _group_sets(texture_sets, tile_mode or {})
            use_disp = bool(pref("use_displacement", False))
            missing = []
            for g in groups:
                imgs, tw = materials.load_textures(obj_name, g["textures"], g["name"],
                                                   udim_tiles=g["udim_tiles"] or None)
                warnings.extend(tw)
                if not imgs:
                    continue
                n_images += len(imgs)
                m = materials.build_material(obj, imgs, use_displacement=use_disp, set_name=g["name"])
                if g["slot"] is not None:
                    if materials.assign_slot_material(obj, g["slot"], m) == "missing":
                        missing.append(g["name"] or str(g["slot"]))
                else:
                    materials.assign_material(obj, m, replace_all=bool(pref("replace_materials", True)))
            if missing:
                warnings.append("no material slot for texture set(s) %s" % ", ".join(missing))
            what = "%d texture set(s)" % len(groups) if (tile_mode or {}).get("material_slots") else "UDIM tiles"
            summary = "Updated %s: %s, %d image(s)%s" % (obj_name, what, n_images, ", UVs" if uv_changed else "")
            if notes:
                summary += " (" + "; ".join(notes) + ")"
            if n_images or uv_changed:
                _undo_push("Patina: update %s" % obj_name)
            if warnings:
                notify("WARNING", summary + " - " + "; ".join(warnings))
            else:
                notify("INFO", summary, popup=False)
            return True

        images, tex_warnings = materials.load_textures(obj_name, textures)
        warnings.extend(tex_warnings)
        mat = None
        if images:
            mat = materials.build_material(obj, images, use_displacement=bool(pref("use_displacement", False)))
            assigned = materials.assign_material(obj, mat, replace_all=bool(pref("replace_materials", True)))
            if assigned == "appended" and len(obj.material_slots) > 1:
                warnings.append("Added material %s as slot %d; assign faces to it to see it"
                                % (mat.name, len(obj.material_slots)))
            elif assigned == "replaced_all":
                warnings.append("%s now shows %s on every material slot (Patina paints the whole object); "
                                "'Restore Materials' in the Patina panel puts the old ones back"
                                % (obj_name, mat.name))
        elif not uv_path:
            warnings.append("Patina sent no textures")

        summary = "Updated %s: %d texture(s)%s" % (obj_name, len(images), ", UVs" if uv_changed else "")
        if notes:
            summary += " (" + "; ".join(notes) + ")"
        if images or uv_changed:
            _undo_push("Patina: update %s" % obj_name)
        if warnings:
            notify("WARNING", summary + " - " + "; ".join(warnings))
        else:
            notify("INFO", summary, popup=False)
        return True
    except Exception as exc:  # noqa: BLE001 - never crash Blender from a timer
        traceback.print_exc()
        notify("ERROR", "Failed to apply Patina textures to %s: %s" % (obj_name, exc))
        return False


def _undo_push(message: str) -> None:
    """Make the update its own undo step (timers do not create one), so Ctrl+Z after a push
    does not also roll back the user's previous action. Best effort."""
    if bpy.app.background:
        return
    try:
        wm = bpy.context.window_manager
        if not wm.windows:
            return
        # Never push in the middle of a modal operator (e.g. a grab in progress); 4.2+ only.
        if any(getattr(w, "modal_operators", ()) for w in wm.windows):
            return
        window = bpy.context.window or wm.windows[0]
        with bpy.context.temp_override(window=window, screen=window.screen):
            bpy.ops.ed.undo_push(message=message)
    except Exception:  # noqa: BLE001
        pass


# --------------------------------------------------------------------------- listener


def start_listener(port: Optional[int] = None) -> bool:
    """Start the live-link listener on 127.0.0.1. Returns False (and records why) on failure."""
    if state.listening:
        return True
    if bpy.app.background:
        # No timers run in background mode, so nobody would poll the socket: Patina's pushes
        # would connect and then hang until they time out. Refusing makes them fail fast.
        state.listener_error = "The live link is not available in background mode"
        return False
    port = blender_port() if port is None else port
    server = protocol.PollingLineServer(_handle_incoming, protocol.LOCALHOST, port,
                                        on_error=lambda text: print("[Patina] listener: %s" % text))
    try:
        server.start()
    except OSError as exc:
        reason = exc.strerror or str(exc)
        if getattr(exc, "errno", None) in (98, 48, 10048):  # EADDRINUSE on Linux / macOS / Windows
            reason = "port %d is already in use (another Blender running the bridge?)" % port
        state.listener_error = "Cannot listen on %d: %s" % (port, reason)
        notify("ERROR", state.listener_error)
        return False
    state.listener = server
    state.listener_error = ""
    ensure_tick()
    notify("INFO", "Live link listening on 127.0.0.1:%d" % server.port, popup=False)
    return True


def stop_listener() -> None:
    if state.listener is not None:
        state.listener.stop()
    state.listener = None
    tag_redraw()


def restart_listener() -> None:
    if state.listening:
        stop_listener()
        start_listener()


# --------------------------------------------------------------------------- launching Patina


class _Launch:
    def __init__(self, proc: subprocess.Popen, exe: str, port: int) -> None:
        self.proc = proc
        self.exe = exe
        self.port = port
        self.deadline = time.monotonic() + LAUNCH_TIMEOUT
        self.ping_in_flight = False
        self.done = False


def resolve_executable(path: str) -> str:
    """Resolve the configured executable to an absolute, runnable path (raises FileNotFoundError)."""
    raw = (path or "patina").strip()
    if raw.startswith("//"):
        raw = bpy.path.abspath(raw)
    raw = os.path.expanduser(raw)
    if os.path.dirname(raw):
        candidate = os.path.abspath(raw)
        if os.path.isdir(candidate):
            for name in ("patina", "patina.exe", os.path.join("bin", "patina")):
                if os.path.isfile(os.path.join(candidate, name)):
                    candidate = os.path.join(candidate, name)
                    break
        if not os.path.isfile(candidate):
            raise FileNotFoundError("Patina executable not found: %s" % candidate)
        if os.name != "nt" and not os.access(candidate, os.X_OK):
            raise FileNotFoundError("Patina executable is not executable: %s" % candidate)
        return candidate
    found = shutil.which(raw)
    if not found and raw == "patina":
        if os.name == "nt":
            # the Windows zip unpacked to the usual places (install.ps1 uses %LOCALAPPDATA%\Programs\Patina)
            local = os.environ.get("LOCALAPPDATA", os.path.expanduser("~\\AppData\\Local"))
            cands = (os.path.join(local, "Programs", "Patina", "patina.exe"),
                     os.path.join(os.environ.get("ProgramFiles", "C:\\Program Files"), "Patina", "patina.exe"))
        else:
            # Blender started from a desktop menu often lacks ~/.local/bin on PATH
            cands = (os.path.expanduser("~/.local/bin/patina"), "/usr/local/bin/patina", "/usr/bin/patina",
                     "/opt/patina/bin/patina")
        for c in cands:
            if os.path.isfile(c) and (os.name == "nt" or os.access(c, os.X_OK)):
                found = c
                break
    if not found:
        raise FileNotFoundError("'%s' was not found on PATH - set the Patina executable in the add-on "
                                "preferences" % raw)
    return found


def launch_patina() -> str:
    """Start Patina detached and begin polling for it. Returns the executable path; raises on failure."""
    exe = resolve_executable(pref("patina_executable", "patina"))
    port = patina_port()
    exchange = exchange_dir()
    os.makedirs(exchange, exist_ok=True)
    env = dict(os.environ)
    env["PATINA_PORT"] = str(port)
    env["PATINA_BRIDGE_DIR"] = exchange
    kwargs: Dict[str, Any] = dict(stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                  env=env, cwd=os.path.dirname(exe) or None, close_fds=True)
    if os.name == "nt":
        kwargs["creationflags"] = (getattr(subprocess, "DETACHED_PROCESS", 0x8)
                                   | getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0x200))
    else:
        kwargs["start_new_session"] = True  # survives Blender and ignores its Ctrl+C
    proc = subprocess.Popen([exe, "--port", str(port)], **kwargs)
    state.processes.append(proc)
    state.launch = _Launch(proc, exe, port)
    if not bpy.app.timers.is_registered(_launch_tick):
        # persistent: if a file is opened meanwhile, a dropped timer would leave is_launching()
        # True forever (Launch grayed out, status pings suspended)
        bpy.app.timers.register(_launch_tick, first_interval=LAUNCH_RETRY_INTERVAL, persistent=True)
    return exe


def _launch_tick() -> Optional[float]:
    """Timer: ping the freshly launched Patina until it answers or ~10 s pass."""
    launch = state.launch
    if launch is None or launch.done:
        return None
    try:
        code = launch.proc.poll()
        if code is not None and code != 0:
            launch.done = True
            notify("ERROR", "Patina exited immediately with code %d (%s)" % (code, launch.exe))
            return None
        if time.monotonic() > launch.deadline:
            launch.done = True
            notify("ERROR", "Patina was started but did not answer on port %d within %d s"
                   % (launch.port, int(LAUNCH_TIMEOUT)))
            return None
        if not launch.ping_in_flight:
            launch.ping_in_flight = True

            def on_ping(result: Any, error: Optional[BaseException], launch=launch) -> None:
                launch.ping_in_flight = False
                if launch.done:
                    return
                if error is None:
                    launch.done = True
                    if isinstance(result, dict):
                        state.patina_info = result
                    notify("INFO", "Patina is running (port %d)" % launch.port)
                elif isinstance(error, protocol.RemoteError):
                    launch.done = True  # it answered, just not to ping
                    notify("INFO", "Patina is running (port %d)" % launch.port)

            send_command("ping", {}, on_done=on_ping, timeout=5.0, quiet=True)
    except Exception as exc:  # noqa: BLE001
        launch.done = True
        notify("ERROR", "Launch monitor failed: %s" % exc)
        return None
    return LAUNCH_RETRY_INTERVAL


def is_launching() -> bool:
    return state.launch is not None and not state.launch.done


# --------------------------------------------------------------------------- tick timer


def _status_ping_done(result: Any, error: Optional[BaseException]) -> None:
    state.ping_in_flight = False
    if error is None and isinstance(result, dict):
        if result != state.patina_info:
            state.patina_info = result
            tag_redraw()


def _tick() -> float:
    """Persistent main-thread timer: listener I/O, request results, status ping."""
    try:
        if state.listener is not None:
            try:
                state.listener.poll()
            except Exception as exc:  # noqa: BLE001
                traceback.print_exc()
                state.listener_error = "Listener failed: %s" % exc
                stop_listener()
                notify("ERROR", state.listener_error)

        for _ in range(32):
            try:
                job, result, error = worker.results.get_nowait()
            except queue.Empty:
                break
            _finish_job(job, result, error)

        now = time.monotonic()
        want_status = state.listening or state.patina_online or bool(state.sent)
        if (want_status and not state.ping_in_flight and worker.pending == 0 and not is_launching()
                and now - state.last_ping > STATUS_PING_INTERVAL):
            state.last_ping = now
            state.ping_in_flight = True
            send_command("ping", {}, on_done=_status_ping_done, timeout=5.0, quiet=True)

        if state.processes:
            state.processes = [p for p in state.processes if p.poll() is None]

        from . import autosync

        autosync.tick(now)
    except Exception:  # noqa: BLE001 - a timer that raises is unregistered by Blender
        traceback.print_exc()
    from . import autosync

    busy = (state.listening or worker.pending or not worker.results.empty() or is_launching()
            or bool(autosync.pending()))
    return TICK_ACTIVE if busy else TICK_IDLE


def ensure_tick() -> None:
    if bpy.app.background:
        return
    if not bpy.app.timers.is_registered(_tick):
        bpy.app.timers.register(_tick, first_interval=0.0, persistent=True)


def _autostart() -> None:
    if pref("auto_start", True) and not state.listening:
        start_listener()
    return None


def addon_version_string() -> str:
    try:
        return ".".join(str(v) for v in sys.modules[__package__].bl_info["version"])
    except Exception:
        return "0.1.0"


def register() -> None:
    if bpy.app.background:
        return
    from . import autosync

    autosync.register()
    ensure_tick()
    # Preferences are only reliably readable once registration has finished.
    if not bpy.app.timers.is_registered(_autostart):
        bpy.app.timers.register(_autostart, first_interval=0.5, persistent=True)


def unregister() -> None:
    from . import autosync

    autosync.unregister()
    for fn in (_tick, _autostart, _launch_tick, _apply_pending):
        try:
            if bpy.app.timers.is_registered(fn):
                bpy.app.timers.unregister(fn)
        except Exception:
            pass
    stop_listener()
    worker.stop()
    _pending_updates.clear()
    if state.launch is not None:  # its timer is gone; don't leave "Starting Patina..." stuck
        state.launch.done = True
    state.ping_in_flight = False
