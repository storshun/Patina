"""Images and the ``Patina_<object>`` Principled BSDF material. Main thread only.

Images are named ``Patina_<object>_<key>`` and reloaded in place, so any other
material or node that uses them updates as well. The material is rebuilt
idempotently: nodes created by the add-on are found again by name, so user
tweaks to *other* nodes and to Principled inputs that Patina does not drive are
kept.
"""

from __future__ import annotations

import json
import os
from typing import Dict, List, Optional, Tuple

import bpy

# texture key -> color space (see docs/BRIDGE.md)
TEXTURE_KEYS: Dict[str, str] = {
    "basecolor": "sRGB",
    "roughness": "Non-Color",
    "metallic": "Non-Color",
    "normal": "Non-Color",
    "height": "Non-Color",
    "emission": "sRGB",
    "ao": "Non-Color",
    # Patina 0.26
    "opacity": "Non-Color",
    "sheen": "Non-Color",
    "coat": "Non-Color",
    "subsurface": "Non-Color",
}
_ALIASES = {"base_color": "basecolor", "albedo": "basecolor", "diffuse": "basecolor", "rough": "roughness",
            "metal": "metallic", "metalness": "metallic", "displacement": "height", "emissive": "emission",
            "occlusion": "ao", "ambient_occlusion": "ao", "alpha": "opacity", "clearcoat": "coat", "sss": "subsurface"}
# Patina 0.26 maps -> Principled BSDF inputs (Blender 4.0+ name, then the 3.x name)
_EXTRA_INPUTS = (("opacity", ("Alpha",)), ("sheen", ("Sheen Weight", "Sheen")), ("coat", ("Coat Weight", "Clearcoat")),
                 ("subsurface", ("Subsurface Weight", "Subsurface")))

NODE_PREFIX = "Patina "
_X_TEX = -720.0
_X_MID = -330.0


def image_name(obj_name: str, key: str, set_name: str = "") -> str:
    return "Patina_%s_%s_%s" % (obj_name, set_name, key) if set_name else "Patina_%s_%s" % (obj_name, key)


def material_name(obj_name: str, set_name: str = "") -> str:
    """``Patina_<object>``, or ``Patina_<object>_<set>`` for a texture set (material slot)."""
    return "Patina_%s_%s" % (obj_name, set_name) if set_name else "Patina_%s" % obj_name


def _find_tagged(collection, name: str, obj_name: str, key: str = ""):
    """Find a datablock by name, else by the custom properties the add-on stamps on it.

    Blender < 5.0 truncates ID names to 63 bytes, so long object names would not
    round-trip through ``Patina_<object>_<key>``; the tags make lookup reliable.
    """
    item = collection.get(name)
    if item is not None and item.library is None:
        return item
    for item in collection:
        if item.library is None and item.get("patina_object") == obj_name and item.get("patina_key", "") == key:
            return item
    return None


def _tag(item, obj_name: str, key: str = "") -> None:
    try:
        item["patina_object"] = obj_name
        if key:
            item["patina_key"] = key
    except (TypeError, AttributeError):
        pass


def _set_colorspace(image, wanted: str) -> None:
    """Set the color space, tolerating OCIO configs that name things differently."""
    candidates = [wanted]
    if wanted == "sRGB":
        candidates += ["sRGB 2.1", "sRGB IEC 61966-2-1"]
    else:
        candidates += ["Non-Color Data", "Raw", "Generic Data"]
    for name in candidates:
        try:
            if image.colorspace_settings.name != name:
                image.colorspace_settings.name = name
            return
        except TypeError:  # not an item of this OCIO config's enum
            continue
    if wanted != "sRGB":
        # Unknown config: ``is_data = True`` switches to whatever its "data" role is. (With a
        # name that matched above this is redundant, since is_data follows the color space.)
        try:
            image.colorspace_settings.is_data = True
        except (AttributeError, TypeError):
            pass


def load_texture(obj_name: str, key: str, path: str, set_name: str = ""):
    """Load or reload ``Patina_<object>[_<set>]_<key>`` from ``path``; returns the image."""
    name = image_name(obj_name, key, set_name)
    tag = "%s/%s" % (set_name, key) if set_name else key
    path = os.path.normpath(os.path.abspath(path))
    image = _find_tagged(bpy.data.images, name, obj_name, tag)
    if image is not None:
        if getattr(image, "packed_file", None) is not None:
            # A packed image would "reload" its packed copy and never show Patina's new file.
            try:
                image.unpack(method="REMOVE")
            except (RuntimeError, TypeError, AttributeError):
                pass
        if image.source != "FILE":
            image.source = "FILE"
        if os.path.normpath(bpy.path.abspath(image.filepath)) != path:
            image.filepath = path  # assigning the path already reloads the image
        else:
            image.reload()
    else:
        image = bpy.data.images.load(path, check_existing=False)
        image.name = name
        _tag(image, obj_name, tag)
    _set_colorspace(image, TEXTURE_KEYS.get(key, "Non-Color"))
    if key != "basecolor":  # data maps: ignore any alpha channel
        try:
            image.alpha_mode = "NONE"
        except (AttributeError, TypeError):
            pass
    image.update()
    return image


def udim_token_path(path: str, udim: int) -> str:
    """``/x/Mesh_basecolor.1001.png`` -> ``/x/Mesh_basecolor.<UDIM>.png`` (Blender's tile token)."""
    head, tail = os.path.split(path)
    needle = ".%d." % udim
    i = tail.rfind(needle)
    if i < 0:
        return path
    return os.path.join(head, tail[:i] + ".<UDIM>." + tail[i + len(needle):])


def load_udim_texture(obj_name: str, key: str, tiles: Dict[int, str], set_name: str = ""):
    """A tiled (UDIM) image from one file per tile, named ``<name>.<udim>.png``; returns the image."""
    name = image_name(obj_name, key, set_name)
    tag = "%s/%s" % (set_name, key) if set_name else key
    first = min(tiles)
    path = os.path.normpath(os.path.abspath(tiles[first]))
    token = udim_token_path(path, first)
    image = _find_tagged(bpy.data.images, name, obj_name, tag)
    if image is None:
        image = bpy.data.images.load(path, check_existing=False)
        image.name = name
        _tag(image, obj_name, tag)
    elif getattr(image, "packed_file", None) is not None:
        try:
            image.unpack(method="REMOVE")
        except (RuntimeError, TypeError, AttributeError):
            pass
    if image.source != "TILED":
        image.source = "TILED"
    if image.filepath != token:
        image.filepath = token
    have = {t.number for t in image.tiles}
    for u in sorted(tiles):
        if u not in have:
            image.tiles.new(tile_number=u)
    # tiles Patina no longer has (or the 1001 Blender adds to every new image) would show nothing
    for t in list(image.tiles):
        if t.number not in tiles and len(image.tiles) > 1:
            try:
                image.tiles.remove(t)
            except (RuntimeError, TypeError, AttributeError):
                pass
    image.reload()
    _set_colorspace(image, TEXTURE_KEYS.get(key, "Non-Color"))
    if key != "basecolor":
        try:
            image.alpha_mode = "NONE"
        except (AttributeError, TypeError):
            pass
    image.update()
    return image


def load_textures(obj_name: str, textures: Dict[str, str], set_name: str = "",
                  udim_tiles: Optional[Dict[str, Dict[int, str]]] = None
                  ) -> Tuple[Dict[str, "bpy.types.Image"], List[str]]:
    """Load every texture that exists on disk. Returns ``(images by key, warnings)``.

    ``udim_tiles`` (key -> {udim: path}) loads tiled images instead of ``textures``.
    """
    images: Dict[str, bpy.types.Image] = {}
    warnings: List[str] = []
    if udim_tiles:
        for raw_key, tiles in udim_tiles.items():
            key = _ALIASES.get(str(raw_key).lower(), str(raw_key).lower())
            ok = {int(u): p for u, p in tiles.items() if isinstance(p, str) and os.path.isfile(p)}
            for u, p in tiles.items():
                if not (isinstance(p, str) and os.path.isfile(p)):
                    warnings.append("missing %s texture %s" % (key, p))
            if not ok:
                continue
            try:
                images[key] = load_udim_texture(obj_name, key, ok, set_name)
            except Exception as exc:  # noqa: BLE001
                warnings.append("cannot load %s tiles (%s)" % (key, exc))
        return images, warnings
    for raw_key, path in (textures or {}).items():
        key = _ALIASES.get(str(raw_key).lower(), str(raw_key).lower())
        if not isinstance(path, str) or not path:
            continue
        if not os.path.isfile(path):
            warnings.append("missing %s texture %s" % (key, path))
            continue
        try:
            images[key] = load_texture(obj_name, key, path, set_name)
        except Exception as exc:  # noqa: BLE001 - one broken PNG must not stop the rest
            warnings.append("cannot load %s (%s)" % (os.path.basename(path), exc))
    return images, warnings


# --------------------------------------------------------------------------- node helpers


def _input(node, *names):
    """First matching input socket (Principled names changed in Blender 4.0)."""
    for n in names:
        sock = node.inputs.get(n)
        if sock is not None:
            return sock
    return None


def _node(nodes, name: str, bl_idname: str, location, label: Optional[str] = None):
    node = nodes.get(name)
    if node is not None and node.bl_idname != bl_idname:
        nodes.remove(node)
        node = None
    if node is None:
        node = nodes.new(bl_idname)
        node.name = name
        node.location = location
    node.label = label or name
    return node


def _remove(nodes, name: str) -> None:
    node = nodes.get(name)
    if node is not None:
        nodes.remove(node)


def _link(links, out_sock, in_sock) -> None:
    if out_sock is None or in_sock is None:
        return
    for lk in list(in_sock.links):  # copy: links are removed while iterating
        if lk.from_socket == out_sock:
            return
        links.remove(lk)
    links.new(out_sock, in_sock)


def _unlink(links, in_sock, only_from_prefix: bool = True) -> None:
    if in_sock is None:
        return
    for lk in list(in_sock.links):
        if not only_from_prefix or lk.from_node.name.startswith(NODE_PREFIX):
            links.remove(lk)


def _principled(nodes):
    node = nodes.get("Principled BSDF")
    if node is None or node.bl_idname != "ShaderNodeBsdfPrincipled":
        node = next((n for n in nodes if n.bl_idname == "ShaderNodeBsdfPrincipled"), None)
    if node is None:
        node = nodes.new("ShaderNodeBsdfPrincipled")
        node.name = "Principled BSDF"
        node.location = (0.0, 300.0)
    return node


def _output(nodes):
    node = next((n for n in nodes if n.bl_idname == "ShaderNodeOutputMaterial" and n.is_active_output), None)
    if node is None:
        node = next((n for n in nodes if n.bl_idname == "ShaderNodeOutputMaterial"), None)
    if node is None:
        node = nodes.new("ShaderNodeOutputMaterial")
        node.name = "Material Output"
        node.location = (360.0, 300.0)
    return node


def _socket(sockets, identifier: str, fallback: int):
    """Socket by its unique ``identifier`` (names repeat on ShaderNodeMix), else by index."""
    for sock in sockets:
        if sock.identifier == identifier:
            return sock
    return sockets[fallback]


def _multiply(nodes, links, a_out, b_out, location):
    """Color multiply node: ``ShaderNodeMix`` (3.4+) or legacy ``ShaderNodeMixRGB``.

    ShaderNodeMix carries one socket set per data type, all named "Factor"/"A"/"B"/
    "Result", so ``inputs["A"]`` would be the *float* socket. Sockets are picked by
    identifier (inputs: Factor_Float 0, Factor_Vector 1, A_Float 2, B_Float 3,
    A_Vector 4, B_Vector 5, A_Color 6, B_Color 7, [4.x: A/B_Rotation 8/9];
    outputs: Result_Float 0, Result_Vector 1, Result_Color 2, ...).
    """
    name = NODE_PREFIX + "AO Multiply"
    try:
        mix = _node(nodes, name, "ShaderNodeMix", location, label="Patina AO")
        mix.data_type = "RGBA"
        mix.blend_type = "MULTIPLY"
        fac, a, b = (_socket(mix.inputs, "Factor_Float", 0), _socket(mix.inputs, "A_Color", 6),
                     _socket(mix.inputs, "B_Color", 7))
        result = _socket(mix.outputs, "Result_Color", 2)
    except (RuntimeError, AttributeError, TypeError):  # Blender < 3.4
        mix = _node(nodes, name, "ShaderNodeMixRGB", location, label="Patina AO")
        mix.blend_type = "MULTIPLY"
        fac, a, b, result = mix.inputs[0], mix.inputs[1], mix.inputs[2], mix.outputs[0]
    fac.default_value = 1.0
    _link(links, a_out, a)
    _link(links, b_out, b)
    return result


def _texture(nodes, links, key: str, image, y: float, uv_out):
    tex = _node(nodes, NODE_PREFIX + key, "ShaderNodeTexImage", (_X_TEX, y), label="Patina " + key)
    tex.image = image
    tex.interpolation = "Linear"
    tex.extension = "REPEAT"
    _link(links, uv_out, tex.inputs.get("Vector"))
    return tex


# --------------------------------------------------------------------------- material


def build_material(obj, images: Dict[str, "bpy.types.Image"], use_displacement: bool = False, set_name: str = ""):
    """Create or update ``Patina_<object>[_<set>]`` from the loaded ``images``; returns the material."""
    name = material_name(obj.name, set_name)
    mat = _find_tagged(bpy.data.materials, name, obj.name, set_name)
    if mat is None:
        mat = bpy.data.materials.new(name)
        _tag(mat, obj.name, set_name)
    try:
        if not mat.use_nodes:
            mat.use_nodes = True  # deprecated (always on) in Blender 5.0
    except (AttributeError, TypeError):
        pass
    nt = mat.node_tree
    nodes, links = nt.nodes, nt.links

    bsdf = _principled(nodes)
    out = _output(nodes)
    _link(links, bsdf.outputs.get("BSDF"), out.inputs.get("Surface"))

    uv = _node(nodes, NODE_PREFIX + "UV", "ShaderNodeUVMap", (_X_TEX - 260.0, 0.0), label="Patina UV")
    uv_layer = obj.data.uv_layers.active if obj.type == "MESH" else None
    if uv_layer is not None:
        uv.uv_map = uv_layer.name
    uv_out = uv.outputs.get("UV")

    y = 600.0
    step = -290.0

    # Base color (optionally multiplied by AO)
    base_in = _input(bsdf, "Base Color")
    if "basecolor" in images:
        tex = _texture(nodes, links, "basecolor", images["basecolor"], y, uv_out)
        color_out = tex.outputs["Color"]
        if "ao" in images:
            ao = _texture(nodes, links, "ao", images["ao"], y + step * 6.0, uv_out)
            color_out = _multiply(nodes, links, color_out, ao.outputs["Color"], (_X_MID, y))
        else:
            _remove(nodes, NODE_PREFIX + "AO Multiply")
        _link(links, color_out, base_in)
    y += step

    if "roughness" in images:
        tex = _texture(nodes, links, "roughness", images["roughness"], y, uv_out)
        _link(links, tex.outputs["Color"], _input(bsdf, "Roughness"))
    y += step

    if "metallic" in images:
        tex = _texture(nodes, links, "metallic", images["metallic"], y, uv_out)
        _link(links, tex.outputs["Color"], _input(bsdf, "Metallic"))
    y += step

    if "normal" in images:
        tex = _texture(nodes, links, "normal", images["normal"], y, uv_out)
        nmap = _node(nodes, NODE_PREFIX + "Normal Map", "ShaderNodeNormalMap", (_X_MID, y), label="Patina Normal Map")
        nmap.space = "TANGENT"
        if uv_layer is not None:
            nmap.uv_map = uv_layer.name
        _link(links, tex.outputs["Color"], nmap.inputs.get("Color"))
        _link(links, nmap.outputs.get("Normal"), _input(bsdf, "Normal"))
    y += step

    if "emission" in images:
        tex = _texture(nodes, links, "emission", images["emission"], y, uv_out)
        _link(links, tex.outputs["Color"], _input(bsdf, "Emission Color", "Emission"))
        strength = _input(bsdf, "Emission Strength")
        if strength is not None and strength.default_value == 0.0:
            strength.default_value = 1.0
    y += step

    # opacity, sheen, clearcoat, subsurface (only sent when the texture set uses them)
    for key, names in _EXTRA_INPUTS:
        sock = _input(bsdf, *names)
        if key in images:
            tex = _texture(nodes, links, key, images[key], y, uv_out)
            _link(links, tex.outputs["Color"], sock)
        else:
            _unlink(links, sock)
            _remove(nodes, NODE_PREFIX + key)
        y += step
    _set_transparency(mat, "opacity" in images)

    disp_in = out.inputs.get("Displacement")
    if "height" in images:
        tex = _texture(nodes, links, "height", images["height"], y, uv_out)
        if use_displacement:
            disp = _node(nodes, NODE_PREFIX + "Displacement", "ShaderNodeDisplacement", (_X_MID, y),
                         label="Patina Displacement")
            disp.inputs["Midlevel"].default_value = 0.5
            if disp.inputs["Scale"].default_value == 1.0:  # new node: subtle default
                disp.inputs["Scale"].default_value = 0.05
            _link(links, tex.outputs["Color"], disp.inputs["Height"])
            _link(links, disp.outputs["Displacement"], disp_in)
            _set_displacement_method(mat)
        else:
            _remove(nodes, NODE_PREFIX + "Displacement")
            _unlink(links, disp_in)
    else:
        _remove(nodes, NODE_PREFIX + "Displacement")
        _unlink(links, disp_in)
    return mat


def _set_transparency(mat, on: bool) -> None:
    """See-through where the opacity map says so: dithered (4.2+) / hashed alpha, which needs no sorting."""
    if hasattr(mat, "surface_render_method"):  # Blender 4.2+: dithered is the default and handles alpha
        if on:
            try:
                mat.surface_render_method = "DITHERED"
            except (TypeError, AttributeError):
                pass
        return
    for attr in ("blend_method", "shadow_method"):
        if hasattr(mat, attr):
            try:
                setattr(mat, attr, "HASHED" if on else "OPAQUE")
            except (TypeError, AttributeError):
                pass


def _set_displacement_method(mat) -> None:
    """Cycles "Displacement and Bump"; EEVEE uses bump anyway. Property moved in 4.1/4.2."""
    for target, attr in ((mat, "displacement_method"), (getattr(mat, "cycles", None), "displacement_method")):
        if target is None or not hasattr(target, attr):
            continue
        try:
            if getattr(target, attr) == "BUMP":
                setattr(target, attr, "BOTH")
            return
        except (TypeError, AttributeError):
            continue


def _is_default_material(mat) -> bool:
    if mat is None:
        return True
    base = mat.name.rsplit(".", 1)[0] if mat.name[-4:-3] == "." and mat.name[-3:].isdigit() else mat.name
    return base in {"Material", "Default", "Default Material", "Material Default"}


ORIGINALS_PROP = "patina_original_materials"


def assign_material(obj, mat, replace_all: bool = False) -> str:
    """Make sure ``obj`` shows ``mat``. Returns 'present', 'replaced', 'set', 'replaced_all' or 'appended'.

    * No slots -> one is added ('set').
    * Every slot already holds it -> nothing changes ('present').
    * Exactly one slot that is empty or holds Blender's default material -> replaced.
    * ``replace_all``: Patina paints one texture set for the whole object, so every slot gets the
      material ('replaced_all'). The materials it replaces are remembered on the object (and kept
      in the file with a fake user) so :func:`restore_materials` can put them back.
    * Otherwise the material is appended as a new slot (faces stay on their material) - unless a
      slot already holds it ('present').
    """
    slots = list(obj.material_slots)
    data = obj.data
    if not slots:
        data.materials.append(mat)
        return "set"
    if all(slot.material == mat for slot in slots):
        return "present"
    if len(slots) == 1 and _is_default_material(slots[0].material):
        slots[0].material = mat
        return "replaced"
    if replace_all:
        if obj.get(ORIGINALS_PROP) is None:  # the first replacement holds the user's own materials
            names, kept = [], []
            for slot in slots:
                m = slot.material
                if m is None or m == mat:
                    names.append("")
                    continue
                names.append(m.name)
                if not getattr(m, "use_fake_user", True):
                    m.use_fake_user = True  # an unused material would be dropped when the file is saved
                    kept.append(m.name)
            obj[ORIGINALS_PROP] = json.dumps({"slots": names, "fake_user": kept})
        for slot in slots:
            slot.material = mat
        return "replaced_all"
    if any(slot.material == mat for slot in slots):
        return "present"
    data.materials.append(mat)
    return "appended"


def _remember_originals(obj, skip=None) -> None:
    """Record the object's own materials (once) so Restore Materials can put them back."""
    if obj.get(ORIGINALS_PROP) is not None:
        return
    names, kept = [], []
    for slot in obj.material_slots:
        m = slot.material
        if m is None or m == skip or str(getattr(m, "name", "")).startswith("Patina_"):
            names.append("")
            continue
        names.append(m.name)
        if not getattr(m, "use_fake_user", True):
            m.use_fake_user = True
            kept.append(m.name)
    obj[ORIGINALS_PROP] = json.dumps({"slots": names, "fake_user": kept})


def assign_slot_material(obj, slot_index: int, mat) -> str:
    """Texture sets: ``mat`` goes into material slot ``slot_index`` (the faces of that slot show it).
    Returns 'present', 'set', 'replaced' or 'missing' (no such slot)."""
    slots = list(obj.material_slots)
    if not slots and slot_index == 0:
        obj.data.materials.append(mat)
        return "set"
    if slot_index >= len(slots):
        return "missing"
    if slots[slot_index].material == mat:
        return "present"
    _remember_originals(obj, skip=mat)
    slots[slot_index].material = mat
    return "replaced"


def slot_names(obj) -> List[str]:
    """The material slots' names as Patina should call its texture sets: the user's own material
    names, also after Patina's materials replaced them."""
    rec = {}
    raw = obj.get(ORIGINALS_PROP)
    if raw is not None:
        try:
            rec = json.loads(raw)
        except (TypeError, ValueError):
            rec = {}
    originals = rec.get("slots", [])
    out = []
    for i, slot in enumerate(obj.material_slots):
        name = originals[i] if i < len(originals) and originals[i] else ""
        if not name and slot.material is not None:
            name = slot.material.name
            prefix = "Patina_%s_" % obj.name
            if name.startswith(prefix):
                name = name[len(prefix):]
            elif name == "Patina_%s" % obj.name:
                name = ""
        out.append(name or "Slot %d" % (i + 1))
    return out


def has_original_materials(obj) -> bool:
    return obj is not None and obj.get(ORIGINALS_PROP) is not None


def restore_materials(obj) -> int:
    """Put back the materials Patina's material replaced. Returns how many slots changed (-1: none recorded)."""
    raw = obj.get(ORIGINALS_PROP)
    if raw is None:
        return -1
    try:
        rec = json.loads(raw)
    except (TypeError, ValueError):
        rec = {}
    names = rec.get("slots", [])
    slots = list(obj.material_slots)
    changed = 0
    for slot, name in zip(slots, names):
        if not name:
            continue
        m = bpy.data.materials.get(name)
        if m is not None and slot.material != m:
            slot.material = m
            changed += 1
    for name in rec.get("fake_user", []):
        m = bpy.data.materials.get(name)
        if m is not None:
            m.use_fake_user = False
    del obj[ORIGINALS_PROP]
    return changed


# --------------------------------------------------------------------------- what a slot shows (0.13)
# Sent with the mesh, so Patina can start an object it doesn't know from the textures Blender shows (Patina's own
# from an earlier push, or any other): per material slot the Principled BSDF's values and the images behind its
# inputs. Read only: nothing in the material changes.

# key, Principled input names (Blender 4.0+, then 3.x)
_DESCRIBED_INPUTS = (("base_color", ("Base Color",)), ("roughness", ("Roughness",)), ("metallic", ("Metallic",)),
                     ("emission", ("Emission Color", "Emission")), ("alpha", ("Alpha",)),
                     ("coat", ("Coat Weight", "Clearcoat")), ("sheen", ("Sheen Weight", "Sheen")))


def _find_principled(mat):
    """The Principled BSDF the active output shows (else any), or None. Creates nothing."""
    nt = getattr(mat, "node_tree", None)
    if nt is None:
        return None
    outs = [n for n in nt.nodes if n.bl_idname == "ShaderNodeOutputMaterial"]
    out = next((n for n in outs if n.is_active_output), outs[0] if outs else None)
    surf = out.inputs.get("Surface") if out is not None else None
    if surf is not None and surf.is_linked and surf.links[0].from_node.bl_idname == "ShaderNodeBsdfPrincipled":
        return surf.links[0].from_node
    return next((n for n in nt.nodes if n.bl_idname == "ShaderNodeBsdfPrincipled"), None)


def _image_file(image) -> Tuple[Optional[str], str]:
    """(path, "") of an image read from a file, else (None, why not)."""
    if image is None:
        return None, "an image node without an image"
    if image.packed_file is not None:
        return None, "packed images"
    if image.source == "TILED":
        return None, "UDIM images"
    if image.source != "FILE":
        return None, "generated images (save them to a file)"
    path = os.path.normpath(bpy.path.abspath(image.filepath_raw or image.filepath, library=image.library))
    if not os.path.isfile(path):
        return None, "missing image files"
    return path, ""


def _color_input(node, index: int):
    """The A (index 0) or B color input of a Mix / MixRGB node."""
    if node.bl_idname == "ShaderNodeMix":
        return _socket(node.inputs, "A_Color" if index == 0 else "B_Color", 6 + index)
    return node.inputs[1 + index] if len(node.inputs) > 1 + index else None


def _trace(sock, notes: List[str], depth: int = 0) -> Optional[dict]:
    """What feeds a shader input: {"path", "channel", "invert"} for an image, {"vertex_colors": True}, or None
    (not linked, or a node Patina can't follow; noted)."""
    if sock is None or not sock.is_linked or depth > 8:
        return None
    link = sock.links[0]
    if getattr(link, "is_muted", False) or not link.is_valid:
        return None
    node, out = link.from_node, link.from_socket
    t = node.bl_idname
    if t == "ShaderNodeTexImage":
        path, why = _image_file(node.image)
        if path is None:
            notes.append(why)
            return None
        return {"path": path, "channel": 3 if out.name == "Alpha" else 0, "invert": False,
                "image": node.image.name}
    if t == "NodeReroute":
        return _trace(node.inputs[0], notes, depth + 1)
    if t in ("ShaderNodeSeparateColor", "ShaderNodeSeparateRGB"):
        r = _trace(node.inputs[0], notes, depth + 1)
        if r is not None and "path" in r:
            r["channel"] = min(2, list(node.outputs).index(out))
        return r
    if t == "ShaderNodeInvert":
        r = _trace(node.inputs.get("Color"), notes, depth + 1)
        if r is not None and "path" in r:
            r["invert"] = not r["invert"]
        return r
    if t in ("ShaderNodeMix", "ShaderNodeMixRGB"):  # Patina's AO multiply and the like: the color is A
        return _trace(_color_input(node, 0), notes, depth + 1)
    if t in ("ShaderNodeVertexColor", "ShaderNodeAttribute"):
        return {"vertex_colors": True}
    notes.append("%s nodes" % (node.bl_label or t))
    return None


def _value(sock, default):
    if sock is None:
        return default
    v = sock.default_value
    try:
        return [float(c) for c in v]
    except TypeError:
        return float(v)


def describe_slot(mat) -> Optional[dict]:
    """What a material shows, as Patina reads it (see docs/BRIDGE.md, "materials"). None for an empty slot."""
    if mat is None:
        return None
    notes: List[str] = []
    bsdf = _find_principled(mat)
    if bsdf is None:
        c = list(mat.diffuse_color)
        return {"name": mat.name, "principled": False, "base_color": [float(x) for x in c[:4]],
                "notes": ["%s: not a Principled BSDF (its viewport color only)" % mat.name]}
    d = {"name": mat.name, "principled": True, "maps": {}}
    defaults = {"base_color": [0.8, 0.8, 0.8, 1.0], "roughness": 0.5, "metallic": 0.0,
                "emission": [0.0, 0.0, 0.0, 1.0], "alpha": 1.0, "coat": 0.0, "sheen": 0.0}
    for key, names in _DESCRIBED_INPUTS:
        sock = _input(bsdf, *names)
        d[key] = _value(sock, defaults[key])
        r = _trace(sock, notes)
        if r is not None and r.get("vertex_colors"):
            if key == "base_color":
                d["vertex_colors"] = True
        elif r is not None:
            d["maps"][key] = r
    strength = _input(bsdf, "Emission Strength")
    if strength is not None and isinstance(d["emission"], list):
        s = float(strength.default_value)
        d["emission"] = [c * s for c in d["emission"][:3]]
    # normal: a Normal Map node with an image (OpenGL style, as Blender reads it), or a Bump node's heights
    nsock = _input(bsdf, "Normal")
    if nsock is not None and nsock.is_linked:
        node = nsock.links[0].from_node
        if node.bl_idname == "ShaderNodeBump":
            r = _trace(node.inputs.get("Height"), notes)
            if r is not None and "path" in r:
                d["maps"]["bump"] = r
            inner = node.inputs.get("Normal")
            node = inner.links[0].from_node if inner is not None and inner.is_linked else None
        if node is not None and node.bl_idname == "ShaderNodeNormalMap":
            if node.space != "TANGENT":
                notes.append("normal maps not in tangent space")
            else:
                r = _trace(node.inputs.get("Color"), notes)
                if r is not None and "path" in r:
                    r["strength"] = float(node.inputs["Strength"].default_value)
                    d["maps"]["normal"] = r
        elif node is not None and node.bl_idname != "ShaderNodeBump":
            notes.append("%s nodes" % (node.bl_label or node.bl_idname))
    # see-through: cut out at a threshold (Blender 4.1 and older) or blended
    if "alpha" in d["maps"] or (isinstance(d["alpha"], float) and d["alpha"] < 0.999):
        d["alpha_mode"] = "clip" if getattr(mat, "blend_method", "") == "CLIP" else "blend"
        d["alpha_cutoff"] = float(getattr(mat, "alpha_threshold", 0.5))
    if notes:
        d["notes"] = ["%s: %s left out" % (mat.name, n) for n in sorted(set(notes))]
    return d


def describe_slots(obj) -> List[Optional[dict]]:
    """describe_slot for each material slot of ``obj`` (index = the faces' material index)."""
    return [describe_slot(slot.material) for slot in obj.material_slots]
