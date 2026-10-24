"""Mesh exchange: Blender object -> ``.ptmesh`` and ``.ptuv`` -> active UV layer.

All bulk data moves through ``foreach_get`` / ``foreach_set`` into numpy
arrays (with an ``array``-module fallback), so even large meshes are written
in a fraction of a second. Main thread only.
"""

from __future__ import annotations

import hashlib
import os
import re
from typing import Any, Dict, Optional, Tuple

import bpy

from . import protocol

try:
    import numpy as np  # Blender always bundles numpy
except Exception:  # pragma: no cover
    np = None  # type: ignore

DEFAULT_UV_NAME = "UVMap"
_UNSAFE = re.compile(r'[\\/:*?"<>|\x00-\x1f]')


# --------------------------------------------------------------------------- helpers


LIGHTMAP_UV_NAME = "Lightmap"


def safe_name(name: str) -> str:
    """Make an object name usable as a file/folder name (keeps it readable)."""
    cleaned = _UNSAFE.sub("_", name).strip().rstrip(".")
    return cleaned or "object"


def object_dir(exchange_dir: str, obj_name: str) -> str:
    return os.path.join(exchange_dir, safe_name(obj_name))


def ptmesh_path(exchange_dir: str, obj_name: str) -> str:
    return os.path.join(object_dir(exchange_dir, obj_name), safe_name(obj_name) + ".ptmesh")


def _alloc(kind: str, n: int):
    """A buffer whose format matches the RNA property type, so foreach_get takes the fast path."""
    if np is not None:
        dtype = {"f": np.float32, "i": np.intc, "b": np.bool_}[kind]
        return np.empty(n, dtype=dtype)
    import array

    if kind == "b":
        return [False] * n
    return array.array(kind, bytes(n * 4))


def _get(collection, attr: str, n: int, kind: str):
    buf = _alloc(kind, n)
    if n:
        collection.foreach_get(attr, buf)
    return buf


def _as_u32(buf):
    if np is not None:
        return np.asarray(buf).astype(np.uint32, copy=False)
    return [int(v) for v in buf]


def _as_u8(buf):
    if np is not None:
        return np.asarray(buf).astype(np.uint8, copy=False)
    return bytes(1 if v else 0 for v in buf)


def _check_contiguous_loops(mesh, F: int) -> None:
    """Patina assumes face f owns loops [sum(sizes[:f]), +size). Always true since Blender 3.6's
    offset-based polygons; verify it for older or unusual meshes."""
    if F == 0:
        return
    starts = _get(mesh.polygons, "loop_start", F, "i")
    totals = _get(mesh.polygons, "loop_total", F, "i")
    if np is not None:
        expected = np.zeros(F, dtype=np.int64)
        np.cumsum(totals[:-1], out=expected[1:])
        ok = bool(np.array_equal(expected, starts))
    else:
        acc, ok = 0, True
        for s, t in zip(starts, totals):
            if s != acc:
                ok = False
                break
            acc += t
    if not ok:
        raise ValueError("%s has non-contiguous face corners; enter and leave Edit Mode once to "
                         "re-order them, then send again" % mesh.name)


def in_edit_mode(obj) -> bool:
    """True if ``obj``'s mesh is held by an edit-mode BMesh.

    ``obj.mode`` alone is not enough: another object sharing the same mesh data
    may be the one in edit mode, and then ``obj.data`` is just as stale.
    """
    return obj.mode == "EDIT" or bool(getattr(obj.data, "is_editmode", False))


def _uv_views(layer):
    """``(collection, attribute)`` pairs to read/write a UV layer, fastest first.

    Blender 3.5+ stores UVs as a float2 attribute; ``MeshUVLoopLayer.uv``
    (``Float2AttributeValue.vector``) maps straight onto that array, while the
    legacy ``MeshUVLoopLayer.data`` (``MeshUVLoop.uv``) still works everywhere
    but goes through per-element getters.
    """
    views = []
    fast = getattr(layer, "uv", None)  # 3.5+
    if fast is not None:
        views.append((fast, "vector"))
    views.append((layer.data, "uv"))
    return views


def _edit_bmesh(mesh):
    """The edit-mode BMesh of ``mesh``, or None. (0.13) Blender 5.x leaves a mesh's attribute arrays (UV maps,
    color attributes) empty while it is in Edit Mode, even after ``update_from_editmode()``: only the BMesh holds
    them. Its iteration order is the mesh's loop order after that flush (see :func:`_apply_ptuv_bmesh`)."""
    if not getattr(mesh, "is_editmode", False):
        return None
    try:
        import bmesh

        return bmesh.from_edit_mesh(mesh)
    except Exception:  # noqa: BLE001
        return None


def _floats(values):
    if np is not None:
        return np.asarray(values, dtype=np.float32)
    import array

    return array.array("f", values)


def _bmesh_uvs(bm, name: str, L: int):
    """A UV map read from the edit-mode BMesh, in loop order, or None."""
    layer = bm.loops.layers.uv.get(name) if bm is not None and name else None
    if layer is None:
        return None
    flat = [c for f in bm.faces for lp in f.loops for c in lp[layer].uv]
    return _floats(flat) if len(flat) == 2 * L else None


def _srgb_to_linear(c: float) -> float:
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def _bmesh_colors(bm, ca, L: int):
    """A color attribute read from the edit-mode BMesh, per corner, linear RGBA, or None."""
    if bm is None or ca is None:
        return None
    on_loops = ca.domain == "CORNER"
    layers = bm.loops.layers if on_loops else bm.verts.layers
    layer = layers.float_color.get(ca.name)
    srgb = False
    if layer is None:
        layer = layers.color.get(ca.name)  # byte colors: stored as sRGB
        srgb = layer is not None
    if layer is None:
        return None
    flat = []
    for f in bm.faces:
        for lp in f.loops:
            c = lp[layer] if on_loops else lp.vert[layer]
            if srgb:
                flat += [_srgb_to_linear(c[0]), _srgb_to_linear(c[1]), _srgb_to_linear(c[2]), c[3]]
            else:
                flat += [c[0], c[1], c[2], c[3]]
    return _floats(flat) if len(flat) == 4 * L else None


def read_uvs(layer, L: int):
    last_exc: Optional[BaseException] = None
    for coll, attr in _uv_views(layer):
        try:
            return _get(coll, attr, L * 2, "f")
        except (AttributeError, TypeError, RuntimeError) as exc:
            last_exc = exc
    raise last_exc  # type: ignore[misc]


def write_uvs(layer, flat) -> None:
    last_exc: Optional[BaseException] = None
    for coll, attr in _uv_views(layer):
        try:
            coll.foreach_set(attr, flat)
            return
        except (AttributeError, TypeError, RuntimeError) as exc:
            last_exc = exc
    raise last_exc  # type: ignore[misc]


def has_active_modifiers(obj) -> bool:
    return any(getattr(m, "show_viewport", True) for m in getattr(obj, "modifiers", ()))


def loop_normals(mesh):
    """Per-corner normals as a flat float32 array.

    Blender 4.1+: ``mesh.corner_normals`` (``MeshNormalValue.vector``), always up to date
    (``calc_normals_split`` / ``use_auto_smooth`` were removed). Blender 3.6-4.0:
    ``calc_normals_split()`` fills ``MeshLoop.normal``; the temporary layer is freed again
    so sending never leaves data behind on the user's mesh.
    """
    n = len(mesh.loops)
    corner = getattr(mesh, "corner_normals", None)
    if corner is not None:  # Blender 4.1+
        return _get(corner, "vector", n * 3, "f")
    if hasattr(mesh, "calc_normals_split"):  # Blender <= 4.0
        mesh.calc_normals_split()
        try:
            return _get(mesh.loops, "normal", n * 3, "f")
        finally:
            if hasattr(mesh, "free_normals_split"):
                mesh.free_normals_split()
    return _get(mesh.loops, "normal", n * 3, "f")


# --------------------------------------------------------------------------- export


def loop_colors(mesh, L: int):
    """The active color attribute (Color Attributes panel) per face corner, linear RGBA, or None."""
    attrs = getattr(mesh, "color_attributes", None)
    if attrs is None or not len(attrs):
        return None
    ca = attrs.active_color or attrs[0]
    if ca is None or ca.domain not in ("POINT", "CORNER"):
        return None
    n = len(ca.data)
    if n == 0:  # (Edit Mode in Blender 5.x: see _edit_bmesh)
        return None
    buf = _get(ca.data, "color", n * 4, "f")
    if ca.domain == "CORNER":
        return buf if n == L else None
    lv = _get(mesh.loops, "vertex_index", L, "i")
    if np is not None:
        return np.asarray(buf).reshape(-1, 4)[np.asarray(lv)].reshape(-1).astype(np.float32)
    import array

    out = array.array("f", bytes(L * 16))
    for i, v in enumerate(lv):
        out[i * 4:i * 4 + 4] = buf[v * 4:v * 4 + 4]
    return out


def collect_mesh(mesh, include_normals: bool = True) -> Tuple[Dict[str, Any], Dict[str, Any]]:
    """Read every exported array from ``mesh``. Returns (block arrays, extra header fields).

    Without an active UV layer, zeros are sent and the header says ``"had_uvs": false``;
    the layer ("UVMap") is created only when Patina sends UVs back, so sending
    never modifies the Blender mesh.
    """
    V, F, L, E = len(mesh.vertices), len(mesh.polygons), len(mesh.loops), len(mesh.edges)
    _check_contiguous_loops(mesh, F)
    arrays: Dict[str, Any] = {
        "positions": _get(mesh.vertices, "co", V * 3, "f"),
        "face_sizes": _as_u32(_get(mesh.polygons, "loop_total", F, "i")),
        "loop_verts": _as_u32(_get(mesh.loops, "vertex_index", L, "i")),
    }
    uv_layer = mesh.uv_layers.active
    bm = None
    if uv_layer is not None and len(uv_layer.data) == L:
        arrays["loop_uvs"] = read_uvs(uv_layer, L)
        had_uvs = True
    else:
        # (0.13) in Edit Mode Blender 5.x keeps the UVs in the BMesh only: read them there, never send "no UVs"
        bm = _edit_bmesh(mesh)
        flat = _bmesh_uvs(bm, uv_layer.name, L) if uv_layer is not None else None
        if flat is not None:
            arrays["loop_uvs"] = flat
            had_uvs = True
        else:
            arrays["loop_uvs"] = _alloc("f", L * 2)
            if np is not None:
                arrays["loop_uvs"].fill(0.0)
            had_uvs = False
    if include_normals and L:
        try:
            arrays["loop_normals"] = loop_normals(mesh)
        except Exception as exc:  # normals are optional - Patina computes its own
            print("[Patina] could not read corner normals: %s" % exc)
    arrays["edges"] = _as_u32(_get(mesh.edges, "vertices", E * 2, "i"))
    arrays["edge_seams"] = _as_u8(_get(mesh.edges, "use_seam", E, "b"))
    try:
        arrays["edge_sharp"] = _as_u8(_get(mesh.edges, "use_edge_sharp", E, "b"))
    except (AttributeError, TypeError, RuntimeError):
        pass
    arrays["face_material"] = _as_u32(_get(mesh.polygons, "material_index", F, "i"))
    try:  # (0.14) the sculpt face sets, for Patina's face sets (left out when there are fewer than two)
        fs_attr = mesh.attributes.get(".sculpt_face_set") if hasattr(mesh, "attributes") else None
        if fs_attr is not None and getattr(fs_attr, "domain", "FACE") == "FACE" and len(fs_attr.data) == F:
            ids = _as_u32([max(0, int(v)) for v in _get(fs_attr.data, "value", F, "i")])
            if len(set(ids)) > 1:
                arrays["face_sets"] = ids
    except Exception as exc:  # noqa: BLE001 - optional
        print("[Patina] could not read the face sets: %s" % exc)
    try:  # a second UV map (lightmaps, 0.35): Patina keeps it and exports it; the first one other than the active
        second = next((l for l in mesh.uv_layers if l != uv_layer and len(l.data) == L), None)
        if second is not None:
            arrays["loop_uvs2"] = read_uvs(second, L)
        elif bm is not None:
            second = next((l for l in mesh.uv_layers if l != uv_layer), None)
            flat = _bmesh_uvs(bm, second.name, L) if second is not None else None
            if flat is not None:
                arrays["loop_uvs2"] = flat
    except Exception as exc:  # noqa: BLE001
        print("[Patina] could not read the second UV map: %s" % exc)
    try:  # vertex colors: Patina starts the object's paint from them (0.32); a nicety, never a reason to fail
        colors = loop_colors(mesh, L)
        if colors is None and bm is not None:
            attrs = getattr(mesh, "color_attributes", None)
            if attrs is not None and len(attrs):
                colors = _bmesh_colors(bm, attrs.active_color or attrs[0], L)
        if colors is not None:
            arrays["loop_colors"] = colors
    except Exception as exc:  # noqa: BLE001
        print("[Patina] could not read the color attribute: %s" % exc)
    extra = {
        "had_uvs": had_uvs,
        "uv_layer": uv_layer.name if uv_layer is not None else DEFAULT_UV_NAME,
        "source": "blender",
        "blender_version": bpy.app.version_string,
    }
    return arrays, extra


def vertex_groups(obj, mesh, min_weight: float = 0.5):
    """``[(name, [vertex indices])]`` for the object's vertex groups: the vertices whose weight is
    ``min_weight`` or more. Patina lists them as parts (group:<name>) that requests can name."""
    groups = list(getattr(obj, "vertex_groups", None) or [])
    if not groups:
        return []
    names = {g.index: g.name for g in groups}
    members: Dict[int, list] = {g.index: [] for g in groups}
    for v in mesh.vertices:
        for ge in getattr(v, "groups", ()):
            if ge.weight >= min_weight and ge.group in members:
                members[ge.group].append(v.index)
    return [(names[i], members[i]) for i in sorted(names) if members[i]]


def export_object(obj, exchange_dir: str, apply_modifiers: bool = False, depsgraph=None,
                  context=None) -> Dict[str, Any]:
    """Write ``obj`` to ``<exchange>/<object>/<object>.ptmesh``.

    Returns a dict with ``path``, ``vertices``, ``faces``, ``loops``, ``edges``,
    ``had_uvs`` and ``evaluated`` (True when modifiers changed the topology so
    returned UVs cannot be mapped back). Raises ``ValueError`` for unusable objects.

    Pass ``context`` rather than a ``depsgraph``: the evaluated depsgraph must be
    fetched *after* edit-mode changes were flushed, or the modifier stack is
    evaluated on the stale mesh.
    """
    if obj is None or obj.type != "MESH":
        raise ValueError("Select a mesh object")
    if in_edit_mode(obj):
        # Flush the edit-mode BMesh into obj.data. This writes faces/loops/edges in BMesh
        # iteration order, which is the order _apply_ptuv_bmesh walks when UVs come back.
        obj.update_from_editmode()

    evaluated = bool(apply_modifiers and has_active_modifiers(obj))
    eval_obj = None
    if evaluated:
        if depsgraph is None:  # fetched after the edit-mode flush so it sees the latest edits
            depsgraph = (context or bpy.context).evaluated_depsgraph_get()
        eval_obj = obj.evaluated_get(depsgraph)
        mesh = eval_obj.to_mesh(preserve_all_data_layers=True, depsgraph=depsgraph)
    else:
        mesh = obj.data
    try:
        if len(mesh.polygons) == 0:
            raise ValueError("%s has no faces" % obj.name)
        arrays, extra = collect_mesh(mesh)
        try:  # parts for the Patina Assistant (head, body...); a nicety, never a reason to fail
            groups = vertex_groups(obj, mesh)
            if groups:
                arrays["vertex_groups"] = groups
        except Exception as exc:  # noqa: BLE001
            print("[Patina] could not read vertex groups: %s" % exc)
        try:  # texture sets are named after the material slots
            from . import materials

            extra["material_names"] = materials.slot_names(obj)
        except Exception:  # noqa: BLE001 - names are a nicety
            pass
        try:  # (0.13) what each slot shows: Patina starts an object it doesn't know from it
            from . import materials

            extra["materials"] = materials.describe_slots(obj)
        except Exception as exc:  # noqa: BLE001 - a nicety too
            print("[Patina] could not read the materials: %s" % exc)
        if evaluated and _same_topology(arrays["loop_verts"], obj.data):
            # only deforming modifiers (Armature, Shrinkwrap, shape keys, ...): every face corner is
            # still the original mesh's, so UVs edited in Patina can come back
            evaluated = False
            extra["deformed"] = True
        extra["evaluated"] = evaluated
        path = ptmesh_path(exchange_dir, obj.name)
        header = protocol.write_ptmesh(path, obj.name, extra_header=extra, **arrays)
    finally:
        if eval_obj is not None:
            eval_obj.to_mesh_clear()
    return {
        "path": path,
        "vertices": header["vertex_count"],
        "faces": header["face_count"],
        "loops": header["loop_count"],
        "edges": header["edge_count"],
        "had_uvs": extra["had_uvs"],
        "evaluated": evaluated,
        "bytes": header.get("bytes", 0),
    }


def _same_topology(loop_verts, base_mesh) -> bool:
    """True when ``loop_verts`` (of an evaluated mesh) are the corners of ``base_mesh``, in order."""
    L = len(base_mesh.loops)
    if len(loop_verts) != L:
        return False
    return _same_loop_order(_get(base_mesh.loops, "vertex_index", L, "i"), loop_verts)


# --------------------------------------------------------------------------- import UVs


def _target_uv_layer(mesh):
    layer = mesh.uv_layers.active
    if layer is None:
        layer = mesh.uv_layers.new(name=DEFAULT_UV_NAME)
        if layer is None:
            raise RuntimeError("Cannot create a UV layer (the mesh has the maximum number of UV maps)")
        mesh.uv_layers.active = layer
        # Adding a layer reallocates the corner attribute arrays (3.5+): re-fetch, don't keep
        # a Python reference that may point at the old storage.
        layer = mesh.uv_layers.active or layer
    return layer


def sent_mesh_info(ptmesh: Optional[str]) -> Tuple[Any, bool]:
    """``(loop_verts, evaluated)`` of the ``.ptmesh`` last sent for an object.

    ``loop_verts`` is None when the file is missing or unreadable (nothing to compare).
    """
    if not ptmesh:
        return None, False
    try:
        st = os.stat(ptmesh)
    except OSError:
        return None, False
    key = (os.path.abspath(ptmesh), st.st_mtime_ns, st.st_size)
    cached = _sent_cache.get(key[0])
    if cached is not None and cached[0] == key:
        return cached[1]
    try:
        header, blocks = protocol.read_ptmesh(ptmesh)
    except (OSError, protocol.ProtocolError):
        return None, False
    blk = blocks.get("loop_verts")
    info = ((blk.to_array() if blk is not None else None), bool(header.get("evaluated", False)))
    if len(_sent_cache) > 16:
        _sent_cache.clear()
    _sent_cache[key[0]] = (key, info)
    return info


_sent_cache: Dict[str, Tuple[Any, Tuple[Any, bool]]] = {}


def sent_uvs(ptmesh: Optional[str]):
    """The UVs in the ``.ptmesh`` last sent for an object (None if unreadable, or when the object had no UVs then:
    the zeros sent in their place are not UVs anyone made, and Patina made the UVs it sends back)."""
    if not ptmesh or not os.path.isfile(ptmesh):
        return None
    try:
        header, blocks = protocol.read_ptmesh(ptmesh)
    except (OSError, protocol.ProtocolError):
        return None
    if header.get("had_uvs") is False:
        return None
    blk = blocks.get("loop_uvs")
    return blk.to_array() if blk is not None else None


def patina_kept_uvs(uv_path: str, sent_ptmesh: Optional[str]) -> bool:
    """(0.14.1) True when the UVs in ``uv_path`` are exactly the ones last sent to Patina. Its textures then fit
    Blender's UVs, also after the mesh changed in Blender since the send: every face left alone still has them."""
    sent = sent_uvs(sent_ptmesh)
    if sent is None or not os.path.isfile(uv_path):
        return False
    try:
        uv = protocol.read_ptuv(uv_path)
    except (OSError, protocol.ProtocolError):
        return False
    return len(uv.loop_uvs) == len(sent) and uv_hash(uv.loop_uvs) == uv_hash(sent)


BACKUP_UV_NAME = "UVMap before Patina"


def _unknown_uvs(obj, current, sent) -> bool:
    """True when ``current`` (Blender's UVs now) are UVs nobody told Patina about while Patina owns the UVs: the
    object had none when it was sent (``sent`` is None) and they are not the ones Patina wrote last. Patina's UVs
    go on top, so these are kept in a UV map of their own first (see :func:`_back_up_uvs`)."""
    if current is None or sent is not None or len(current) == 0:
        return False
    if obj.get(UV_HASH_PROP) == uv_hash(current):
        return False
    if np is not None:
        return bool(np.any(np.asarray(current, dtype=np.float32) != 0.0))
    return any(float(v) != 0.0 for v in current)


def _back_up_uvs(mesh, current) -> Optional[str]:
    """Blender's UVs into a new UV map (not the active one), unless a map already holds exactly them. Returns
    its name, or None when the mesh has no room for another UV map."""
    h = uv_hash(current)
    L = len(mesh.loops)
    for layer in mesh.uv_layers:
        if layer != mesh.uv_layers.active and len(layer.data) == L and uv_hash(read_uvs(layer, L)) == h:
            return layer.name
    active_name = mesh.uv_layers.active.name if mesh.uv_layers.active is not None else None
    layer = mesh.uv_layers.new(name=BACKUP_UV_NAME, do_init=False)
    if layer is None:
        return None
    name = layer.name
    # adding a layer reallocates the corner attributes: look the layers up again by name
    write_uvs(mesh.uv_layers[name], current)
    if active_name is not None and active_name in mesh.uv_layers:
        mesh.uv_layers.active = mesh.uv_layers[active_name]
    return name


def uv_hash(values) -> str:
    """Fingerprint of a UV array, exact to the float32 bits Blender stores."""
    if np is not None:
        raw = np.ascontiguousarray(np.asarray(values, dtype=np.float32)).tobytes()
    else:
        import array

        raw = array.array("f", [float(v) for v in values]).tobytes()
    return hashlib.blake2b(raw, digest_size=16).hexdigest()


UV_HASH_PROP = "patina_uv_hash"
_EDITED_MSG = ("UVs not applied: the UVs of %s were changed in Blender after it was sent. Use 'Update "
               "Mesh in Patina' (Patina moves the paint onto those UVs), then push again")


def _edited_in_blender(obj, current, sent) -> bool:
    """True when ``current`` (Blender's UVs now) are neither the sent ones nor the last ones Patina wrote."""
    if current is None or sent is None or len(current) != len(sent):
        return False
    h = uv_hash(current)
    return h != uv_hash(sent) and h != obj.get(UV_HASH_PROP)


def _same_loop_order(current, sent) -> bool:
    if sent is None:
        return True  # nothing to compare against (e.g. sent by an earlier Blender session)
    if np is not None:
        return bool(np.array_equal(np.asarray(current, dtype=np.int64), np.asarray(sent, dtype=np.int64)))
    return [int(v) for v in current] == [int(v) for v in sent]


_ORDER_MSG = ("UVs not applied: the face corners of %s no longer match the mesh that was sent (the "
              "topology changed) - use 'Update Mesh in Patina'")


def apply_ptuv(obj, uv_path: str, sent_ptmesh: Optional[str] = None) -> Tuple[bool, str]:
    """Write UVs and seams from ``uv_path`` into ``obj``'s active UV layer.

    ``sent_ptmesh`` is the ``.ptmesh`` written by the last send of ``obj``; when it is
    readable, the corner -> vertex mapping is compared with it so UVs are never written
    onto re-ordered corners (same loop count, different topology), and UVs of a mesh
    sent with modifiers applied are refused. Returns ``(ok, message)``; mismatches are
    reported, not raised.
    """
    if not os.path.isfile(uv_path):
        return False, "UV file not found: %s" % uv_path
    uv = protocol.read_ptuv(uv_path)
    mesh = obj.data
    sent_verts, sent_evaluated = sent_mesh_info(sent_ptmesh)
    if sent_evaluated:
        return False, ("UVs not applied: the mesh was sent with modifiers that change its faces applied - turn "
                       "off 'Apply Modifiers When Sending', send it again and repaint")

    if in_edit_mode(obj):
        # Writing obj.data now would be overwritten when edit mode is left.
        return _apply_ptuv_bmesh(obj, uv, sent_verts, sent_ptmesh)

    L, E = len(mesh.loops), len(mesh.edges)
    if uv.loop_count != L:
        return False, ("UVs not applied: Patina sent %d loops but %s has %d (the topology changed "
                       "- use 'Update Mesh in Patina')" % (uv.loop_count, obj.name, L))
    if sent_verts is not None and not _same_loop_order(_get(mesh.loops, "vertex_index", L, "i"), sent_verts):
        return False, _ORDER_MSG % obj.name
    data = uv.loop_uvs
    if np is not None:
        data = np.ascontiguousarray(data, dtype=np.float32)
    current = read_uvs(mesh.uv_layers.active, L) if mesh.uv_layers.active is not None else None
    msg = "UVs applied"
    if current is not None and uv_hash(current) == uv_hash(data):
        msg = "UVs unchanged"
    else:
        sent = sent_uvs(sent_ptmesh)
        if _edited_in_blender(obj, current, sent):
            return False, _EDITED_MSG % obj.name
        if _unknown_uvs(obj, current, sent):
            kept = _back_up_uvs(mesh, current)
            if kept is None:
                return False, ("UVs not applied: %s has UVs Patina did not make, and no room for another UV map to "
                               "keep them in - remove a UV map, or use 'Update Mesh in Patina'" % obj.name)
            msg = "UVs applied; Blender's UVs kept as UV map '%s'" % kept
        layer = _target_uv_layer(mesh)
        write_uvs(layer, data)
    obj[UV_HASH_PROP] = uv_hash(data)

    if uv.edge_seams is not None:
        if uv.edge_count == E:
            seams = np.asarray(uv.edge_seams, dtype=np.bool_) if np is not None else [bool(v) for v in uv.edge_seams]
            mesh.edges.foreach_set("use_seam", seams)
        else:
            msg = "UVs applied; seams skipped (edge count %d != %d)" % (uv.edge_count, E)
    slots_msg = _apply_face_slots(obj, uv, None)
    if slots_msg:
        msg = slots_msg if msg == "UVs unchanged" else msg + "; " + slots_msg
    mesh.update()
    return True, msg


def _apply_face_slots(obj, uv: "protocol.PtUV", bm) -> str:
    """Texture sets made in Patina: each face's material slot, with the slots Patina added (an empty
    material named after the set; Patina's material for it comes with the textures). ``bm``: the
    edit-mode BMesh, or None. Returns a message, or "" when the .ptuv has no slots."""
    if uv.face_slots is None:
        return ""
    import bpy

    slots = [int(v) for v in uv.face_slots]
    faces = bm.faces if bm is not None else obj.data.polygons
    if len(slots) != len(faces):
        return "material slots skipped (Patina sent %d faces, %s has %d)" % (len(slots), obj.name, len(faces))
    names = list(uv.slot_names)
    need = max([len(names)] + [s + 1 for s in slots])
    added = []
    while len(obj.material_slots) < need:
        i = len(obj.material_slots)
        name = names[i] if i < len(names) and names[i] else "Slot %d" % (i + 1)
        mat = bpy.data.materials.get(name)
        if mat is None:
            mat = bpy.data.materials.new(name)
        obj.data.materials.append(mat)
        added.append(name)
    if bm is not None:
        for f, s in zip(bm.faces, slots):
            f.material_index = s
    else:
        obj.data.polygons.foreach_set("material_index", slots)
    if added:
        return "texture sets %s added as material slots" % ", ".join(added)
    return "material slots updated"


def _apply_ptuv_bmesh(obj, uv: "protocol.PtUV", sent_verts=None, sent_ptmesh: Optional[str] = None) -> Tuple[bool, str]:
    """Edit-mode path: write through BMesh so leaving edit mode does not discard the UVs.

    Loop order: Patina's loops are Blender's mesh loop order at send time. Both BMesh
    <-> Mesh conversions use BMesh *iteration* order (faces in ``bm.faces`` order, each
    face's corners starting at ``face.loops[0]``): entering edit mode creates the BMesh
    in polygon/corner order, and ``update_from_editmode()`` (done by every send from
    edit mode) writes the mesh in iteration order. So iterating ``bm.faces`` /
    ``face.loops`` / ``bm.edges`` here reproduces the sent order exactly, as long as the
    topology was not edited in between - the same condition the object-mode path has,
    and the loop/edge-count checks below catch the common cases.
    """
    import bmesh

    bm = bmesh.from_edit_mesh(obj.data)
    L = sum(len(f.loops) for f in bm.faces)
    if uv.loop_count != L:
        return False, ("UVs not applied: Patina sent %d loops but %s has %d in edit mode (the topology "
                       "changed - use 'Update Mesh in Patina')" % (uv.loop_count, obj.name, L))
    if sent_verts is not None:
        bm.verts.index_update()  # = the vertex order the next BMesh -> Mesh write produces
        if not _same_loop_order([lp.vert.index for f in bm.faces for lp in f.loops], sent_verts):
            return False, _ORDER_MSG % obj.name
    layer = bm.loops.layers.uv.active
    uvs = uv.loop_uvs.tolist() if hasattr(uv.loop_uvs, "tolist") else list(uv.loop_uvs)
    kept = None
    if layer is not None:
        current = [c for f in bm.faces for lp in f.loops for c in lp[layer].uv]
        sent = sent_uvs(sent_ptmesh)
        if uv_hash(current) != uv_hash(uvs) and _edited_in_blender(obj, current, sent):
            return False, _EDITED_MSG % obj.name
        if uv_hash(current) != uv_hash(uvs) and _unknown_uvs(obj, current, sent):
            # (0.42) Patina owns these UVs (none when sent): Blender's are kept in a UV map of their own first
            active_name = layer.name
            backup = bm.loops.layers.uv.new(BACKUP_UV_NAME)
            layer = bm.loops.layers.uv.get(active_name)
            i = 0
            for face in bm.faces:
                for loop in face.loops:
                    loop[backup].uv = (current[i], current[i + 1])
                    i += 2
            kept = backup.name
    else:
        layer = bm.loops.layers.uv.new(DEFAULT_UV_NAME)
    i = 0
    for face in bm.faces:  # same order BMesh -> Mesh conversion uses
        for loop in face.loops:
            loop[layer].uv = (uvs[i], uvs[i + 1])
            i += 2
    msg = "UVs applied" if kept is None else "UVs applied; Blender's UVs kept as UV map '%s'" % kept
    if uv.edge_seams is not None:
        if uv.edge_count == len(bm.edges):
            for edge, s in zip(bm.edges, uv.edge_seams):
                edge.seam = bool(s)
        else:
            msg = "UVs applied; seams skipped (edge count mismatch)"
    slots_msg = _apply_face_slots(obj, uv, bm)
    if slots_msg:
        msg += "; " + slots_msg
    obj[UV_HASH_PROP] = uv_hash(uvs)
    bmesh.update_edit_mesh(obj.data)
    return True, msg


def vertex_count(obj) -> int:
    """The vertex count of ``obj``'s mesh as it is now (the edit-mode BMesh in Edit Mode)."""
    if in_edit_mode(obj):
        import bmesh

        return len(bmesh.from_edit_mesh(obj.data).verts)
    return len(obj.data.vertices)


def select_vertices(obj, indices) -> int:
    """Select exactly ``indices`` (vertex indices of ``obj``'s mesh), with the edges and faces
    they fully cover. Works in object and edit mode. Returns how many vertices were selected."""
    idx = {int(i) for i in indices}
    mesh = obj.data
    if in_edit_mode(obj):
        import bmesh

        bm = bmesh.from_edit_mesh(mesh)
        for f in bm.faces:
            f.select = False
        for e in bm.edges:
            e.select = False
        n = 0
        for i, v in enumerate(bm.verts):  # the position in the sequence, not BMVert.index (it can be stale)
            v.select = i in idx
            n += v.select
        bm.select_flush(True)
        bmesh.update_edit_mesh(mesh)
        return n
    n = 0
    for i, v in enumerate(mesh.vertices):
        v.select = i in idx
        n += bool(v.select)
    for e in mesh.edges:
        vs = getattr(e, "vertices", ())
        e.select = bool(vs) and all(int(k) in idx for k in vs)
    for p in mesh.polygons:
        vs = getattr(p, "vertices", None)
        if vs is not None:
            p.select = all(int(k) in idx for k in vs)
    mesh.update()
    return n


# --------------------------------------------------------------------------- Patina -> Blender meshes (0.9)


def mesh_from_ptmesh(path: str, name: str):
    """A new Blender mesh from Patina's ``.ptmesh`` (in Blender's axes): faces, UVs, material slot indices, seams
    and sharp edges. Returns ``(mesh, header, blocks)``; the caller adds the slots' materials and the vertex groups
    (they belong to the object). Main thread."""
    header, blocks = protocol.read_ptmesh(path)
    if str(header.get("up", "Z")).upper() != "Z":
        raise ValueError("%s is not in Blender's axes" % path)
    pos = blocks["positions"].to_array()
    sizes = blocks["face_sizes"].to_array()
    loops = blocks["loop_verts"].to_array()
    if np is not None:  # foreach_set wants Blender's own types (int32 indices)
        loops = np.asarray(loops, dtype=np.int32)
    V, F, L = len(pos) // 3, len(sizes), len(loops)
    mesh = bpy.data.meshes.new(name)
    mesh.vertices.add(V)
    mesh.vertices.foreach_set("co", pos)
    mesh.loops.add(L)
    mesh.loops.foreach_set("vertex_index", loops)
    mesh.polygons.add(F)
    if np is not None:
        size_arr = np.asarray(sizes, dtype=np.int32)
        starts = np.zeros(F, dtype=np.int32)
        if F > 1:
            starts[1:] = np.cumsum(size_arr)[:-1]
    else:
        size_arr = list(sizes)
        starts, k = [], 0
        for n in size_arr:
            starts.append(k)
            k += n
    mesh.polygons.foreach_set("loop_start", starts)
    try:  # Blender < 4.0 stores the size too (4.0+ derives it from the next start)
        mesh.polygons.foreach_set("loop_total", size_arr)
    except (AttributeError, RuntimeError, TypeError):
        pass
    if "face_material" in blocks and blocks["face_material"].count == F:
        fm = blocks["face_material"].to_array()
        mesh.polygons.foreach_set("material_index", np.asarray(fm, dtype=np.int32) if np is not None else fm)
    mesh.update(calc_edges=True)
    mesh.validate(clean_customdata=False)
    if "loop_uvs" in blocks and blocks["loop_uvs"].count == L * 2:
        layer = mesh.uv_layers.new(name=DEFAULT_UV_NAME)
        write_uvs(layer, blocks["loop_uvs"].to_array())
    if "loop_uvs2" in blocks and blocks["loop_uvs2"].count == L * 2:
        # Patina 0.35: the lightmap UVs, as a second UV map (the first stays the one textures use)
        layer2 = mesh.uv_layers.new(name=LIGHTMAP_UV_NAME)
        write_uvs(layer2, blocks["loop_uvs2"].to_array())
        if mesh.uv_layers.get(DEFAULT_UV_NAME) is not None:
            mesh.uv_layers.active = mesh.uv_layers[DEFAULT_UV_NAME]
    if "loop_colors" in blocks and blocks["loop_colors"].count == L * 4:
        try:  # Patina 0.32: vertex colors (linear), per face corner
            ca = mesh.color_attributes.new("Col", "FLOAT_COLOR", "CORNER")
            ca.data.foreach_set("color", blocks["loop_colors"].to_array())
        except Exception as exc:  # noqa: BLE001 - a nicety
            print("[Patina] could not add the vertex colors: %s" % exc)
    mesh.polygons.foreach_set("use_smooth", [True] * F if np is None else np.ones(F, dtype=bool))
    # seams and sharp edges: Blender numbered the edges itself, so they are matched by their two vertices
    if "edges" in blocks and blocks["edges"].count >= 2:
        ev = blocks["edges"].to_list()
        seams = blocks["edge_seams"].to_list() if "edge_seams" in blocks else []
        sharp = blocks["edge_sharp"].to_list() if "edge_sharp" in blocks else []
        flagged = {}
        for e in range(len(ev) // 2):
            s_ = seams[e] if e < len(seams) else 0
            h_ = sharp[e] if e < len(sharp) else 0
            if s_ or h_:
                a, b = ev[2 * e], ev[2 * e + 1]
                flagged[(min(a, b), max(a, b))] = (s_, h_)
        if flagged:
            for edge in mesh.edges:
                a, b = edge.vertices
                f_ = flagged.get((min(a, b), max(a, b)))
                if f_:
                    edge.use_seam = bool(f_[0])
                    if f_[1]:
                        try:
                            edge.use_edge_sharp = True
                        except AttributeError:
                            pass
    mesh.update()
    return mesh, header, blocks


def object_from_ptmesh(path: str, name: str, source=None, replace: bool = False):
    """Make (or update) the object ``name`` from a Patina mesh: next to ``source`` with its transform and
    collections, or, with ``replace``, as ``source``'s own mesh. Material slots get the materials of the same
    names (new ones when missing), vertex groups come along. Returns the object."""
    mesh, header, blocks = mesh_from_ptmesh(path, name)
    slot_names = list(header.get("material_names") or [])
    if replace and source is not None:
        obj = source
    else:
        obj = bpy.data.objects.get(name)
        if obj is not None and obj.type != "MESH":
            obj = None
    if obj is not None:
        old = obj.data
        obj.data = mesh
        if old is not None and old.users == 0:
            bpy.data.meshes.remove(old)
    else:
        obj = bpy.data.objects.new(name, mesh)
        cols = list(source.users_collection) if source is not None else []
        for c in cols or [bpy.context.scene.collection]:
            c.objects.link(obj)
    if source is not None and obj is not source:
        try:  # the source's matrix as it is now (a move since the last redraw included)
            bpy.context.view_layer.update()
        except Exception:  # noqa: BLE001 - no view layer in some contexts; the stored matrix is close
            pass
        obj.matrix_world = source.matrix_world.copy()
    # material slots, by name
    need = max([len(slot_names)] + ([max(blocks["face_material"].to_list()) + 1] if "face_material" in blocks and
                                    blocks["face_material"].count else [1]))
    mesh.materials.clear()
    for i in range(need):
        nm = slot_names[i] if i < len(slot_names) and slot_names[i] else ("Slot %d" % i)
        mat = bpy.data.materials.get(nm) or bpy.data.materials.new(nm)
        mesh.materials.append(mat)
    # vertex groups
    if "group_sizes" in blocks and "group_verts" in blocks:
        names = list(header.get("vertex_groups") or [])
        gs = blocks["group_sizes"].to_list()
        gv = blocks["group_verts"].to_list()
        k = 0
        for i, n in enumerate(gs):
            nm = names[i] if i < len(names) else "Group %d" % i
            vg = obj.vertex_groups.get(nm) or obj.vertex_groups.new(name=nm)
            idx = gv[k:k + n]
            if idx:
                vg.add(idx, 1.0, "REPLACE")
            k += n
    return obj
