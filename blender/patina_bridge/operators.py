"""Operators of the Patina bridge (``PATINA_OT_*``).

Operators never wait for Patina: they write files, queue a request on the
worker thread (see :mod:`server`) and return immediately. Results are applied
by callbacks that run on the main thread.
"""

import os
import time
import traceback
from typing import Any, Optional

import bpy
from bpy.props import BoolProperty, EnumProperty, StringProperty
from bpy.types import Operator

from . import mesh_io, protocol, server


def _active_mesh(context):
    obj = context.active_object
    if obj is not None and obj.type == "MESH":
        return obj
    for o in getattr(context, "selected_objects", ()) or ():
        if o.type == "MESH":
            return o
    return None


def _selected_meshes(context):
    """(0.14) The mesh objects to send: the active one first, then the other selected ones."""
    first = _active_mesh(context)
    out = [first] if first is not None else []
    for o in getattr(context, "selected_objects", ()) or ():
        if o.type == "MESH" and o not in out:
            out.append(o)
    return out


def _matrix(obj):
    """obj.matrix_world, column by column (Patina 1.2 places the object in its scene with it)."""
    m = getattr(obj, "matrix_world", None)
    if m is None:
        return None
    try:
        return [float(m[r][c]) for c in range(4) for r in range(4)]
    except (TypeError, IndexError):
        return None


def _resolution(context) -> int:
    try:
        return int(server.pref("default_resolution", "2048"))
    except (TypeError, ValueError):
        return 2048


class PATINA_OT_report(Operator):
    """Internal: show a message in Blender's status bar / info log"""

    bl_idname = "patina.report"
    bl_label = "Patina Report"
    bl_options = {"INTERNAL"}

    level: EnumProperty(items=[("INFO", "Info", ""), ("WARNING", "Warning", ""), ("ERROR", "Error", "")],
                        default="INFO")  # type: ignore[valid-type]
    message: StringProperty(default="")  # type: ignore[valid-type]

    def execute(self, context):
        # An 'ERROR' report makes bpy.ops raise when called from a timer, so errors are
        # shown as warnings here; the panel shows the real level.
        self.report({"WARNING" if self.level == "ERROR" else self.level}, self.message)
        return {"FINISHED"}


def send_object(obj, context, update: bool, keep_layers: bool, quiet: bool = False, scene: str = "auto"):
    """Export ``obj`` and open it in Patina (``update``: bridge_update_mesh). Returns (ok, message).

    ``scene`` (Patina 1.2, several objects in one project): "auto" updates the object where Patina's scene has it,
    else starts a scene of its own; "add" puts it beside the objects there; "update" only updates it where Patina
    has it (the automatic sync). Older Patina versions ignore it and the placement.

    Used by the Send / Update operators and by the automatic mesh sync (:mod:`autosync`)."""
    prefs = server.get_prefs(context)
    apply_mods = bool(getattr(prefs, "apply_modifiers", False))
    exchange = server.exchange_dir()
    if not server.state.listening and not server.state.listener_error:
        server.start_listener()  # Patina needs somewhere to push textures back to
    try:
        # context, not a depsgraph: it is evaluated after edit-mode changes are flushed
        info = mesh_io.export_object(obj, exchange, apply_modifiers=apply_mods, context=context)
    except (ValueError, OSError, protocol.ProtocolError) as exc:
        return False, "Export failed: %s" % exc
    except Exception as exc:  # noqa: BLE001
        traceback.print_exc()
        return False, "Export failed: %s" % exc

    server.state.sent[obj.name] = info
    from . import autosync

    autosync.sent(obj.name, obj)
    args = {
        "path": info["path"],
        "object": obj.name,
        "blender_port": server.state.listener.port if server.state.listening else server.blender_port(),
        "exchange_dir": exchange,
        "resolution": _resolution(context),
        "keep_layers": keep_layers,
        "scene": scene,
    }
    if _matrix(obj) is not None:
        args["matrix"] = _matrix(obj)
    cmd = "bridge_update_mesh" if update else "bridge_open_mesh"
    name = obj.name

    def on_done(result: Any, error: Optional[BaseException]) -> None:
        if error is not None:
            autosync.send_failed(name)
            server.notify("ERROR", "%s failed: %s" % (cmd, server.describe_error(error)))
            return
        r = result if isinstance(result, dict) else {}
        if r.get("skipped"):
            return
        server.notify("INFO", "%s %s in Patina (%s faces, %s islands)" % (
            "Updated" if update else "Opened", name, r.get("faces", info["faces"]), r.get("islands", "?")),
            popup=not quiet)

    server.send_command(cmd, args, on_done=on_done, quiet=quiet)
    note = ""
    if info["evaluated"]:
        note = " (modifiers applied - UV edits will not come back)"
    elif not info["had_uvs"]:
        note = " (no UVs - unwrap in Patina)"
    server.notify("INFO", "%s %s: %d faces%s" % ("Auto-updating" if quiet else "Sending", obj.name, info["faces"], note),
                  popup=False)
    return True, "Sent %s to Patina%s" % (obj.name, note)


class _SendBase:
    """Shared implementation of *Send* and *Update*."""

    update = False

    @classmethod
    def poll(cls, context):
        return _active_mesh(context) is not None

    def _send(self, context, keep_layers: bool, add: bool = False):
        objs = _selected_meshes(context)
        if not objs:
            self.report({"ERROR"}, "Select a mesh object")
            return {"CANCELLED"}
        # (0.14) every selected mesh: the active one first (a scene of its own unless Patina has it), the others
        # beside it as objects with their own layers
        msgs = []
        for i, obj in enumerate(objs):
            scene = "add" if add or i > 0 else "auto"
            ok, msg = send_object(obj, context, self.update, keep_layers, scene=scene)
            if not ok:
                self.report({"ERROR"}, msg)
                return {"CANCELLED"}
            msgs.append(msg)
        self.report({"INFO"}, msgs[0] if len(msgs) == 1 else "Sent %d objects to Patina" % len(msgs))
        return {"FINISHED"}


class PATINA_OT_send(_SendBase, Operator):
    """Export the selected mesh objects and open them in Patina, each an object with its own layers (a new project
unless Patina has them already)"""

    bl_idname = "patina.send"
    bl_label = "Send to Patina"
    bl_options = {"REGISTER"}

    keep_layers: BoolProperty(name="Keep Layers", default=True,
                              description="Keep painted layers if Patina already has this mesh with the same "
                                          "loop count")  # type: ignore[valid-type]

    def execute(self, context):
        return self._send(context, self.keep_layers)


class PATINA_OT_add_to_scene(_SendBase, Operator):
    """Add the selected mesh objects to the project open in Patina, beside its objects (Patina 1.2: a character's
clothes, eyes or hair, each with its own layers)"""

    bl_idname = "patina.add_to_scene"
    bl_label = "Add to the Patina Scene"
    bl_options = {"REGISTER"}

    def execute(self, context):
        return self._send(context, True, add=True)


class PATINA_OT_update_mesh(_SendBase, Operator):
    """Re-send the mesh after modeling changes; Patina keeps the layers and reprojects paint if the UVs changed"""

    bl_idname = "patina.update_mesh"
    bl_label = "Update Mesh in Patina"
    bl_options = {"REGISTER"}
    update = True

    def execute(self, context):
        return self._send(context, True)


class PATINA_OT_pull(Operator):
    """Ask Patina to export the textures (and edited UVs) of the active object and apply them here"""

    bl_idname = "patina.pull"
    bl_label = "Pull Textures"
    bl_options = {"REGISTER"}

    @classmethod
    def poll(cls, context):
        return _active_mesh(context) is not None

    def execute(self, context):
        obj = _active_mesh(context)
        name = obj.name

        def on_done(result: Any, error: Optional[BaseException]) -> None:
            if error is not None:
                server.notify("ERROR", "Pull failed: %s" % server.describe_error(error))
                return
            if not isinstance(result, dict):
                server.notify("ERROR", "Pull failed: unexpected reply %r" % (result,))
                return
            textures = result.get("textures") or {}
            if not isinstance(textures, dict):
                textures = {}
            server.apply_patina_result(name, textures, result.get("uv_path") or None,
                                       result.get("texture_sets"), result.get("tile_mode") or {})

        server.send_command("bridge_export", {"object": name}, on_done=on_done)
        server.notify("INFO", "Requesting textures for %s..." % name, popup=False)
        return {"FINISHED"}


class PATINA_OT_launch(Operator):
    """Start the Patina application (with --port) and connect to it"""

    bl_idname = "patina.launch"
    bl_label = "Launch Patina"
    bl_options = {"REGISTER"}

    @classmethod
    def poll(cls, context):
        return not server.is_launching()

    def execute(self, context):
        st = server.state
        if st.patina_online and st.last_contact and time.monotonic() - st.last_contact < 10.0:
            self.report({"INFO"}, "Patina is already running on port %d" % server.patina_port())
            return {"CANCELLED"}
        if not st.listening and not st.listener_error:
            server.start_listener()
        try:
            exe = server.launch_patina()
        except FileNotFoundError as exc:
            self.report({"ERROR"}, str(exc))
            server.notify("ERROR", str(exc), popup=False)
            return {"CANCELLED"}
        except OSError as exc:
            msg = "Cannot start Patina: %s" % (exc.strerror or exc)
            self.report({"ERROR"}, msg)
            server.notify("ERROR", msg, popup=False)
            return {"CANCELLED"}
        server.notify("INFO", "Starting %s on port %d..." % (os.path.basename(exe), server.patina_port()),
                      popup=False)
        return {"FINISHED"}


class PATINA_OT_live_link(Operator):
    """Start or stop the listener through which Patina pushes texture updates to Blender"""

    bl_idname = "patina.live_link"
    bl_label = "Live Link"
    bl_options = {"REGISTER"}

    action: EnumProperty(items=[("TOGGLE", "Toggle", ""), ("START", "Start", ""), ("STOP", "Stop", "")],
                         default="TOGGLE")  # type: ignore[valid-type]

    def execute(self, context):
        want = not server.state.listening if self.action == "TOGGLE" else self.action == "START"
        if want:
            server.state.listener_error = ""
            if not server.start_listener():
                self.report({"ERROR"}, server.state.listener_error)
                return {"CANCELLED"}
            self.report({"INFO"}, "Live link on (port %d)" % server.state.listener.port)
        else:
            server.stop_listener()
            server.notify("INFO", "Live link off", popup=False)
            self.report({"INFO"}, "Live link off")
        return {"FINISHED"}


class PATINA_OT_ping(Operator):
    """Check whether Patina is reachable"""

    bl_idname = "patina.ping"
    bl_label = "Check Connection"
    bl_options = {"REGISTER"}

    def execute(self, context):
        def on_done(result: Any, error: Optional[BaseException]) -> None:
            if error is not None:
                server.notify("WARNING", server.describe_error(error))
                return
            if isinstance(result, dict):
                server.state.patina_info = result
            server.notify("INFO", "Patina %s is connected" % (
                result.get("version", "") if isinstance(result, dict) else ""))

        server.send_command("ping", {}, on_done=on_done, timeout=5.0)
        return {"FINISHED"}


class PATINA_OT_open_exchange(Operator):
    """Open the exchange folder in the system file browser"""

    bl_idname = "patina.open_exchange"
    bl_label = "Open Exchange Folder"
    bl_options = {"REGISTER"}

    def execute(self, context):
        path = server.exchange_dir()
        try:
            os.makedirs(path, exist_ok=True)
        except OSError as exc:
            self.report({"ERROR"}, "Cannot create %s: %s" % (path, exc))
            return {"CANCELLED"}
        bpy.ops.wm.path_open(filepath=path)
        return {"FINISHED"}


class PATINA_OT_check_symmetry(Operator):
    """Ask Patina how far the mesh is from mirror-symmetric across X and select the vertices that are off
    (and those with no surface on the other side)"""

    bl_idname = "patina.check_symmetry"
    bl_label = "Select Asymmetric Vertices"
    bl_options = {"REGISTER"}

    include_unmatched: BoolProperty(name="Include Unmatched", default=True,
                                    description="Also select vertices with no surface at all on the other "
                                                "side")  # type: ignore[valid-type]

    @classmethod
    def poll(cls, context):
        obj = _active_mesh(context)
        return obj is not None and obj.name in server.state.sent

    def execute(self, context):
        obj = _active_mesh(context)
        sent = server.state.sent.get(obj.name, {})
        if sent.get("evaluated"):
            self.report({"ERROR"}, "%s was sent with modifiers applied: its vertices don't match Patina's" % obj.name)
            return {"CANCELLED"}
        name, unmatched = obj.name, self.include_unmatched
        on_symmetry_result = symmetry_result_handler(name, unmatched, sent.get("vertices"))
        server.send_command("mesh_symmetry", {"vertices": True}, on_done=on_symmetry_result)
        server.notify("INFO", "Measuring the symmetry of %s in Patina..." % name, popup=False)
        return {"FINISHED"}


def symmetry_result_handler(name: str, unmatched: bool, sent_vertices):
    """The callback of *Select Asymmetric Vertices* (main thread)."""

    def on_done(result: Any, error: Optional[BaseException]) -> None:
        if error is not None:
            server.notify("ERROR", "Symmetry check failed: %s" % server.describe_error(error))
            return
        r = result if isinstance(result, dict) else {}
        obj = bpy.data.objects.get(name)
        if obj is None:
            return
        if r.get("object") and r.get("object") != name:
            server.notify("WARNING", "Patina has %s open, not %s: send it first" % (r.get("object"), name))
            return
        have = mesh_io.vertex_count(obj)
        if r.get("vertices") not in (None, have) or (sent_vertices is not None and sent_vertices != have):
            server.notify("WARNING", "Patina's mesh has %s vertices, %s has %s now: update the mesh in Patina "
                          "and check again" % (r.get("vertices"), name, have))
            return
        idx = list(r.get("beyond_vertices", []))
        if unmatched:
            idx += list(r.get("unmatched_vertices", []))
        n = mesh_io.select_vertices(obj, idx)
        tol = r.get("tolerance", 0.0) or 0.0
        server.notify("INFO", "%s: %s - %d vertices selected (off by more than %.4g; worst %.4g)" % (
            name, r.get("verdict", "?"), n, tol, r.get("max", 0.0)))
        server.tag_redraw()

    return on_done


class PATINA_OT_restore_materials(Operator):
    """Put back the materials that Patina's material replaced on this object's slots"""

    bl_idname = "patina.restore_materials"
    bl_label = "Restore Materials"
    bl_options = {"REGISTER", "UNDO"}

    @classmethod
    def poll(cls, context):
        from . import materials

        return materials.has_original_materials(_active_mesh(context))

    def execute(self, context):
        from . import materials

        obj = _active_mesh(context)
        n = materials.restore_materials(obj)
        self.report({"INFO"}, "Restored %d material slot(s) on %s" % (max(n, 0), obj.name))
        return {"FINISHED"}


classes = (
    PATINA_OT_report,
    PATINA_OT_send,
    PATINA_OT_add_to_scene,
    PATINA_OT_update_mesh,
    PATINA_OT_pull,
    PATINA_OT_launch,
    PATINA_OT_live_link,
    PATINA_OT_ping,
    PATINA_OT_open_exchange,
    PATINA_OT_restore_materials,
    PATINA_OT_check_symmetry,
)
