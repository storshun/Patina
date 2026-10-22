"""Builds Blender's Patina material library: a .blend of asset-marked materials.

Runs in a background Blender started by the add-on (never in the user's session):

    blender -b --factory-startup --python library_build.py -- MAPS_DIR OUT_BLEND MAPPING

MAPS_DIR holds what Patina's ``export_material_maps`` wrote (manifest.json and
the maps); OUT_BLEND is the library file (its folder is the asset library);
MAPPING is ``BOX`` (like Patina: no UVs needed) or ``UV``.

Patina's procedural materials can't run in Blender, so each one becomes a
Principled BSDF over the maps Patina baked for it. Images are stored with paths
relative to the library, so the folder can be moved as a whole.

Prints ``PATINA_LIBRARY_DONE <count>`` at the end, or ``PATINA_LIBRARY_ERROR <text>``.
"""

import json
import os
import sys
import traceback
import uuid

import bpy

CATALOG_NS = uuid.UUID("6b1f3c62-6a0e-4f8e-9d55-70a7d1c1a0e5")  # stable catalog ids per category


def srgb_to_linear(c):
    c = max(0.0, min(1.0, float(c)))
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def color_of(v, default=(0.8, 0.8, 0.8)):
    if isinstance(v, str) and v.startswith("#") and len(v) >= 7:
        return tuple(int(v[i:i + 2], 16) / 255.0 for i in (1, 3, 5))
    if isinstance(v, (int, float)):
        return (float(v),) * 3
    if isinstance(v, (list, tuple)) and len(v) >= 3:
        return tuple(float(x) for x in v[:3])
    return default


def set_input(node, names, value):
    for n in names:
        sock = node.inputs.get(n)
        if sock is not None:
            try:
                sock.default_value = value
            except (TypeError, ValueError):
                sock.default_value = value[:3] if len(sock.default_value) == 3 else tuple(value) + (1.0,)
            return sock
    return None


def get_input(node, *names):
    for n in names:
        sock = node.inputs.get(n)
        if sock is not None:
            return sock
    return None


def colorspace(image, data):
    names = ["Non-Color", "Non-Color Data", "Raw", "Generic Data"] if data else ["sRGB", "sRGB 2.1",
                                                                                   "sRGB IEC 61966-2-1"]
    for n in names:
        try:
            image.colorspace_settings.name = n
            break
        except TypeError:
            continue
    if data:
        try:
            image.alpha_mode = "NONE"
        except (AttributeError, TypeError):
            pass


def load_image(path, data):
    rel = "//" + os.path.relpath(path, os.path.dirname(bpy.data.filepath)).replace(os.sep, "/")
    for im in bpy.data.images:
        if im.filepath == rel:
            return im
    im = bpy.data.images.load(path, check_existing=False)
    im.filepath = rel
    im.name = os.path.splitext(os.path.basename(path))[0]
    colorspace(im, data)
    return im


def separate_rgb(nodes, location):
    try:
        n = nodes.new("ShaderNodeSeparateColor")
        n.mode = "RGB"
    except RuntimeError:  # Blender < 3.3
        n = nodes.new("ShaderNodeSeparateRGB")
    n.location = location
    return n


def build_material(m, maps_dir, mapping):
    name = m.get("name") or m["id"]
    mat = bpy.data.materials.new(name)
    if mat.node_tree is None:
        mat.use_nodes = True
    mat["patina_material_id"] = m["id"]
    nt = mat.node_tree
    nodes, links = nt.nodes, nt.links
    bsdf = next(n for n in nodes if n.type == "BSDF_PRINCIPLED")
    bsdf.location = (300, 300)
    out = next(n for n in nodes if n.type == "OUTPUT_MATERIAL")
    out.location = (650, 300)
    maps = m.get("maps") or {}
    avg = m.get("average") or {}
    flat = m.get("type") == "flat"

    # what the viewport shows in Solid mode, and flat materials
    col = color_of(avg.get("color", m.get("color")), (0.8, 0.8, 0.8))
    lin = tuple(srgb_to_linear(c) for c in col)
    mat.diffuse_color = lin + (1.0,)
    mat.roughness = float(avg.get("roughness", m.get("roughness", 0.5)))
    mat.metallic = float(avg.get("metallic", m.get("metallic", 0.0)))

    # Patina 0.26: sheen, clearcoat and subsurface as values (the material's average), see-through parts below
    for key, names in (("sheen", ["Sheen Weight", "Sheen"]), ("coat", ["Coat Weight", "Clearcoat"]),
                       ("subsurface", ["Subsurface Weight", "Subsurface"])):
        v = float(avg.get(key, m.get(key, 0.0)) or 0.0)
        if v > 0.001:
            set_input(bsdf, names, v)
    op = float(avg.get("opacity", m.get("opacity", 1.0)) or 1.0)
    if flat and op < 0.999:
        set_input(bsdf, ["Alpha"], op)
        see_through(mat)

    if flat:
        set_input(bsdf, ["Base Color"], lin + (1.0,))
        set_input(bsdf, ["Roughness"], mat.roughness)
        set_input(bsdf, ["Metallic"], mat.metallic)
        em = color_of(m.get("emissive"), (0.0, 0.0, 0.0))
        if max(em) > 0.002:
            set_input(bsdf, ["Emission Color", "Emission"], tuple(srgb_to_linear(c) for c in em) + (1.0,))
            set_input(bsdf, ["Emission Strength"], 1.0)
        return mat

    scale = float(m.get("default_scale", 1.0) or 1.0)
    box = mapping == "BOX"
    coord = nodes.new("ShaderNodeTexCoord")
    coord.location = (-1300, 300)
    mp = nodes.new("ShaderNodeMapping")
    mp.name = "Patina Scale"
    mp.label = "Scale (tiles per meter)" if box else "Scale (tiles per UV square)"
    mp.location = (-1100, 300)
    mp.inputs["Scale"].default_value = (scale, scale, scale)
    links.new(coord.outputs["Object" if box else "UV"], mp.inputs["Vector"])

    def tex(key, data, y):
        f = maps.get(key)
        if not f or not os.path.isfile(os.path.join(maps_dir, f)):
            return None
        t = nodes.new("ShaderNodeTexImage")
        t.name = t.label = "Patina " + key
        t.location = (-800, y)
        t.image = load_image(os.path.join(maps_dir, f), data)
        t.interpolation = "Linear"
        t.extension = "REPEAT"
        if box:
            t.projection = "BOX"
            t.projection_blend = 0.2
        links.new(mp.outputs["Vector"], t.inputs["Vector"])
        return t

    c = tex("color", False, 600)
    if c:
        links.new(c.outputs["Color"], get_input(bsdf, "Base Color"))
    r = tex("rmh", True, 300)
    if r:
        sep = separate_rgb(nodes, (-450, 300))
        links.new(r.outputs["Color"], sep.inputs[0])
        links.new(sep.outputs[0], get_input(bsdf, "Roughness"))
        links.new(sep.outputs[1], get_input(bsdf, "Metallic"))
    ns = float((m.get("params") or {}).get("normal_strength", 1.0) or 1.0)
    if box:
        # Box projection has no tangent space for a normal map: bump from the height map (16 bit when
        # Patina wrote one). Patina makes its normal map with a slope of normal_strength / 128 per whole
        # tile of height, so a tile 1 / scale long has its height at normal_strength / (128 * scale).
        h = tex("height", True, 0) if maps.get("height") else None
        hsock = h.outputs["Color"] if h else (sep.outputs[2] if r else None)
        if hsock is not None:
            bump = nodes.new("ShaderNodeBump")
            bump.location = (-150, 0)
            set_input(bump, ["Strength"], 1.0)
            set_input(bump, ["Distance"], ns / (128.0 * scale))
            links.new(hsock, get_input(bump, "Height"))
            links.new(bump.outputs["Normal"], get_input(bsdf, "Normal"))
    else:
        n = tex("normal", True, 0)
        if n:
            nm = nodes.new("ShaderNodeNormalMap")
            nm.location = (-150, 0)
            links.new(n.outputs["Color"], nm.inputs["Color"])
            links.new(nm.outputs["Normal"], get_input(bsdf, "Normal"))
    o = tex("opacity", True, -600)
    if o:
        links.new(o.outputs["Color"], get_input(bsdf, "Alpha"))
        see_through(mat)
    e = tex("emissive", False, -300)
    if e:
        links.new(e.outputs["Color"], get_input(bsdf, "Emission Color", "Emission"))
        set_input(bsdf, ["Emission Strength"], 1.0)
    return mat


def see_through(mat):
    """Alpha that needs no sorting: dithered (Blender 4.2+) or hashed."""
    if hasattr(mat, "surface_render_method"):
        mat.surface_render_method = "DITHERED"
    elif hasattr(mat, "blend_method"):
        mat.blend_method = "HASHED"
        if hasattr(mat, "shadow_method"):
            mat.shadow_method = "HASHED"


def set_preview(mat, path):
    if not path or not os.path.isfile(path):
        return
    try:
        im = bpy.data.images.load(path, check_existing=False)
        w, h = im.size
        px = list(im.pixels)
        bpy.data.images.remove(im)
        prev = mat.preview_ensure()
        prev.image_size = (w, h)
        prev.image_pixels_float.foreach_set(px)
    except Exception:  # a missing preview is not worth failing the library for
        traceback.print_exc()


def write_catalogs(lib_dir, cats):
    lines = ["# This is an Asset Catalog Definition file for Blender.", "#",
             "# Written by Patina: the Patina material library's catalogs.", "", "VERSION 1", ""]
    lines.append("%s:Patina:Patina" % uuid.uuid5(CATALOG_NS, "Patina"))
    for c in sorted(cats):
        lines.append("%s:Patina/%s:Patina-%s" % (cats[c], c.replace(":", " "), c.replace(":", " ").replace("/", "-")))
    with open(os.path.join(lib_dir, "blender_assets.cats.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")


def main():
    argv = sys.argv[sys.argv.index("--") + 1:]
    maps_dir, out_blend, mapping = os.path.abspath(argv[0]), os.path.abspath(argv[1]), argv[2].upper()
    lib_dir = os.path.dirname(out_blend)
    with open(os.path.join(maps_dir, "manifest.json"), encoding="utf-8") as f:
        manifest = json.load(f)["materials"]
    bpy.ops.wm.read_factory_settings(use_empty=True)
    tmp = out_blend + ".building.blend"
    bpy.ops.wm.save_as_mainfile(filepath=tmp)  # so images can be stored relative to the library folder
    cats = {}
    count = 0
    for m in manifest:
        mat = build_material(m, maps_dir, mapping)
        mat.asset_mark()
        ad = mat.asset_data
        cat = m.get("category") or "Misc"
        if not m.get("builtin", True):
            cat = "My materials/" + cat
        cats.setdefault(cat, str(uuid.uuid5(CATALOG_NS, cat)))
        ad.catalog_id = cats[cat]
        ad.description = m.get("description") or ""
        ad.author = "Patina" if m.get("builtin", True) else ""
        for t in [cat.split("/")[-1], m.get("generator", "")] + list(m.get("tags") or []):
            if t and t not in [x.name for x in ad.tags]:
                ad.tags.new(t)
        set_preview(mat, os.path.join(maps_dir, m["thumbnail"]) if m.get("thumbnail") else None)
        count += 1
    write_catalogs(lib_dir, cats)
    bpy.ops.wm.save_as_mainfile(filepath=tmp, compress=True)
    os.replace(tmp, out_blend)
    for leftover in (tmp + "1", tmp):
        if os.path.isfile(leftover):
            os.remove(leftover)
    print("PATINA_LIBRARY_DONE %d" % count, flush=True)


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:  # noqa: BLE001 - the add-on reads this line
        traceback.print_exc()
        print("PATINA_LIBRARY_ERROR %s" % exc, flush=True)
        sys.exit(1)
