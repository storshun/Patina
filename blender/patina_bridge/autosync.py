"""Automatic mesh sync: re-send a mesh to Patina once modeling edits settle.

Off by default (preference *Auto Update Mesh*). Only objects already sent to Patina are
watched. A depsgraph handler notes geometry changes; the bridge's main-thread tick calls
:func:`tick`, which sends an object once it has been quiet for the settle delay and nothing
else is waiting for Patina.

What Patina has is remembered as two digests: the shape (positions and face corners) and the
active UV map. An object is only sent when one of them differs, so updates Blender makes on
its own (leaving Edit Mode, selecting vertices, the UVs Patina writes back) never cause a send,
and the two programs can't ping-pong. Digests are read without leaving Edit Mode, so reading
them never causes an update either (in Edit Mode they are read element by element, which takes
a moment on meshes with hundreds of thousands of vertices; it happens once per pause).
"""

import hashlib
import time
from array import array
from typing import Dict, Optional, Tuple

import bpy

from . import mesh_io, server

# object name -> monotonic time of its last geometry change not yet looked at
_dirty: Dict[str, float] = {}
# object name -> (shape digest, uv digest) of what Patina has
_base: Dict[str, Tuple[str, str]] = {}

Digests = Optional[Tuple[str, str]]


def enabled() -> bool:
    return bool(server.pref("auto_update_mesh", False))


def delay() -> float:
    try:
        return max(0.3, float(server.pref("auto_update_delay", 1.5)))
    except (TypeError, ValueError):
        return 1.5


def pending() -> Dict[str, float]:
    return dict(_dirty)


def _bytes(buf) -> bytes:
    if hasattr(buf, "tobytes"):  # numpy arrays, array.array
        return buf.tobytes()
    return repr(list(buf)).encode()


def mesh_digests(obj) -> Digests:
    """(shape, uvs) of ``obj``'s mesh; None if they can't be read (then it counts as changed)."""
    try:
        mesh = obj.data
        shape, uvs = hashlib.blake2b(digest_size=16), hashlib.blake2b(digest_size=16)
        if mesh_io.in_edit_mode(obj):
            import bmesh

            bm = bmesh.from_edit_mesh(mesh)
            # the same bytes as in Object Mode below, so leaving Edit Mode compares equal
            co, sizes, corners, uv = array("f"), array("i"), array("i"), array("f")
            for v in bm.verts:
                co.extend(v.co)
            if hasattr(bm.verts, "index_update"):
                bm.verts.index_update()  # BMVert.index can be stale after edits
            layer = bm.loops.layers.uv.active
            for f in bm.faces:
                sizes.append(len(f.loops))
                for lp in f.loops:
                    corners.append(lp.vert.index)
                    if layer is not None:
                        uv.extend(lp[layer].uv)
            shape.update(_bytes(sizes))
        else:
            V, L, F = len(mesh.vertices), len(mesh.loops), len(mesh.polygons)
            co = mesh_io._get(mesh.vertices, "co", V * 3, "f")
            corners = mesh_io._get(mesh.loops, "vertex_index", L, "i")
            sizes = mesh_io._get(mesh.polygons, "loop_total", F, "i")
            shape.update(_bytes(sizes))
            uv = array("f")
            layer = mesh.uv_layers.active
            if layer is not None:
                uv = mesh_io.read_uvs(layer, L)
        shape.update(_bytes(co))
        shape.update(_bytes(corners))
        uvs.update(_bytes(uv))
        return shape.hexdigest(), uvs.hexdigest()
    except Exception:  # noqa: BLE001
        return None


def sent(name: str, obj=None) -> None:
    """``name`` was just sent (by hand or automatically): Patina now has it as it is."""
    _dirty.pop(name, None)
    d = mesh_digests(obj) if obj is not None else None
    if d is not None:
        _base[name] = d
    else:
        _base.pop(name, None)


def send_failed(name: str) -> None:
    """The send didn't reach Patina: it still has the old mesh, so the next change sends again."""
    _base.pop(name, None)


def uvs_written(name: str, obj) -> None:
    """Patina's UVs were written into ``name``: Blender's UVs now match Patina's."""
    base = _base.get(name)
    d = mesh_digests(obj)
    if base is not None and d is not None:
        _base[name] = (base[0], d[1])


def note_change(name: str, now: float = None) -> bool:
    """Record a geometry change of ``name``. Returns whether it will be looked at."""
    if not enabled() or name not in server.state.sent:
        return False
    _dirty[name] = time.monotonic() if now is None else now
    return True


def _objects_using(mesh_data):
    for name in server.state.sent:
        obj = bpy.data.objects.get(name)
        if obj is not None and getattr(obj, "data", None) is mesh_data:
            yield name


def on_depsgraph_update(scene, depsgraph) -> None:
    if not enabled() or not server.state.sent:
        return
    try:
        for upd in depsgraph.updates:
            if not getattr(upd, "is_updated_geometry", False):
                continue
            idb = getattr(upd, "id", None)
            orig = getattr(idb, "original", idb)
            if orig is None:
                continue
            if getattr(orig, "type", None) == "MESH" and orig.name in server.state.sent:
                note_change(orig.name)
            elif type(orig).__name__ == "Mesh" or hasattr(orig, "polygons"):
                for name in _objects_using(orig):  # edit mode reports the mesh datablock
                    note_change(name)
    except Exception:  # noqa: BLE001 - a handler that raises spams every update
        pass


def on_load(*_args) -> None:
    """Another .blend: its objects are not the ones Patina has, even where the names match."""
    _dirty.clear()
    _base.clear()
    server.state.sent.clear()


for _fn in ("on_depsgraph_update", "on_load"):  # @persistent keeps them across file loads
    try:
        globals()[_fn] = bpy.app.handlers.persistent(globals()[_fn])
    except AttributeError:
        pass


def tick(now: float = None) -> None:
    """Called from the bridge's main-thread timer: send meshes whose edits have settled."""
    if not _dirty:
        return
    if not enabled():
        _dirty.clear()
        return
    now = time.monotonic() if now is None else now
    if server.worker.pending:  # one thing at a time: Patina is still busy with the last request
        return
    if server.state.patina_online is False:  # keep the changes until Patina answers again
        return
    wait = delay()
    for name, t in list(_dirty.items()):
        if now - t < wait:
            continue
        obj = bpy.data.objects.get(name)
        if obj is None or getattr(obj, "type", None) != "MESH":
            _dirty.pop(name, None)
            continue
        if getattr(obj, "mode", "OBJECT") == "SCULPT" and now - t < 3 * wait:
            continue  # sculpting: wait for a longer pause
        _dirty.pop(name, None)
        d = mesh_digests(obj)
        if d is not None and d == _base.get(name):
            continue  # an update that changed nothing Patina has (selection, mode switch, our UVs)
        from . import operators

        ok, msg = operators.send_object(obj, bpy.context, update=True, keep_layers=True, quiet=True, scene="update")
        if not ok:
            server.notify("WARNING", "Auto update of %s: %s" % (name, msg), popup=False)
        return  # one object per tick


def _handler_lists():
    handlers = getattr(bpy.app, "handlers", None)
    return [(getattr(handlers, "depsgraph_update_post", None), on_depsgraph_update),
            (getattr(handlers, "load_post", None), on_load)]


def register() -> None:
    for lst, fn in _handler_lists():
        if lst is not None and fn not in lst:
            lst.append(fn)


def unregister() -> None:
    for lst, fn in _handler_lists():
        if lst is not None:
            while fn in lst:
                lst.remove(fn)
    _dirty.clear()
    _base.clear()
