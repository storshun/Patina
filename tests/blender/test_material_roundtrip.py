#!/usr/bin/env python3
"""Textures Patina pushed to Blender come back with the object (0.44, add-on 0.13), in a real (background) Blender
and a real headless Patina. Patina's own material (built as a push builds it: base color through the AO multiply,
roughness, a normal map, a black glow map) and a hand-made one (an image through Separate Color into Roughness, an
inverted image, a Color Attribute into Base Color), next to a slot without a Principled BSDF, are described in the
mesh Blender sends. A Patina without a project for the object then starts from them: its base color is the pushed
image, and the object's black Color attribute (that nothing shows) is left out.

    PATINA_BIN=build/patina python3 tests/blender/test_material_roundtrip.py    (needs blender on PATH, else skips)
"""
import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ADDON_PARENT = os.path.join(HERE, "..", "..", "blender")
sys.path.insert(0, os.path.join(ADDON_PARENT, "patina_bridge"))
import protocol  # noqa: E402

SCRIPT = r'''
import json, os, sys
argv = sys.argv[sys.argv.index("--") + 1:]
sys.path.insert(0, argv[0])
tmp = argv[1]
import bpy
from patina_bridge import materials, mesh_io

bpy.ops.wm.read_factory_settings(use_empty=True)
out = {}

def png(name):
    return os.path.join(tmp, name)

# an object Patina painted and pushed: a grid with UVs, a black Color attribute, Patina's material
bpy.ops.mesh.primitive_grid_add(x_subdivisions=8, y_subdivisions=8, size=2)
o = bpy.context.active_object
o.name = "Mai"
ca = o.data.color_attributes.new("Col", "FLOAT_COLOR", "CORNER")
for d in ca.data:
    d.color = (0.0, 0.0, 0.0, 1.0)
textures = {k: png("Mai_%s.png" % k) for k in ("basecolor", "roughness", "metallic", "normal", "emission", "ao")}
images, warnings = materials.load_textures(o.name, textures)
mat = materials.build_material(o, images)
materials.assign_material(o, mat)
out["pushed"] = materials.describe_slots(o)

# a hand-made material, and a slot that isn't a Principled BSDF
bpy.ops.mesh.primitive_grid_add(x_subdivisions=4, y_subdivisions=4, size=2, location=(4, 0, 0))
h = bpy.context.active_object
h.name = "Hand"
m = bpy.data.materials.new("Hand made")
m.use_nodes = True
nt = m.node_tree
bsdf = next(n for n in nt.nodes if n.type == "BSDF_PRINCIPLED")
orm = nt.nodes.new("ShaderNodeTexImage")
orm.image = bpy.data.images.load(png("orm.png"))
sep = nt.nodes.new("ShaderNodeSeparateColor")
nt.links.new(orm.outputs["Color"], sep.inputs[0])
nt.links.new(sep.outputs[1], bsdf.inputs["Roughness"])
gloss = nt.nodes.new("ShaderNodeTexImage")
gloss.image = bpy.data.images.load(png("Mai_metallic.png"))
inv = nt.nodes.new("ShaderNodeInvert")
nt.links.new(gloss.outputs["Color"], inv.inputs["Color"])
nt.links.new(inv.outputs["Color"], bsdf.inputs["Metallic"])
attr = nt.nodes.new("ShaderNodeVertexColor")
nt.links.new(attr.outputs["Color"], bsdf.inputs["Base Color"])
packed = nt.nodes.new("ShaderNodeTexImage")
packed.image = bpy.data.images.new("painted in Blender", 8, 8)
nt.links.new(packed.outputs["Color"], bsdf.inputs["Emission Color"])
h.data.materials.append(m)
toon = bpy.data.materials.new("Toon")
toon.use_nodes = True
tnt = toon.node_tree
for n in list(tnt.nodes):
    if n.type == "BSDF_PRINCIPLED":
        tnt.nodes.remove(n)
diffuse = tnt.nodes.new("ShaderNodeBsdfDiffuse")
outn = next(n for n in tnt.nodes if n.type == "OUTPUT_MATERIAL")
tnt.links.new(diffuse.outputs[0], outn.inputs["Surface"])
toon.diffuse_color = (0.2, 0.4, 0.9, 1.0)
h.data.materials.append(toon)
h.data.materials.append(None)
out["hand"] = materials.describe_slots(h)

# Send to Patina: the mesh carries them
info = mesh_io.export_object(o, tmp)
out["path"] = info["path"]
print("ROUNDTRIP_JSON " + json.dumps(out))
'''

failures = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        failures.append(what)


def png(path, w, h, rgb):
    raw = b"".join(b"\0" + bytes(rgb) * w for _ in range(h))
    chunk = lambda t, d: struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                           chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def main():
    blender = shutil.which("blender")
    if not blender:
        print("SKIP: blender not on PATH")
        return 0
    exe = os.environ.get("PATINA_BIN") or os.path.join(HERE, "..", "..", "build", "patina")
    tmp = tempfile.mkdtemp(prefix="patina_material_roundtrip_test_")
    try:
        colors = {"basecolor": (200, 60, 30), "roughness": (90, 90, 90), "metallic": (20, 20, 20),
                  "normal": (128, 128, 255), "emission": (0, 0, 0), "ao": (255, 255, 255)}
        for k, c in colors.items():
            png(os.path.join(tmp, "Mai_%s.png" % k), 32, 32, c)
        png(os.path.join(tmp, "orm.png"), 32, 32, (255, 128, 0))
        script = os.path.join(tmp, "roundtrip.py")
        open(script, "w").write(SCRIPT)
        env = dict(os.environ, HOME=os.path.join(tmp, "home"), XDG_CONFIG_HOME=os.path.join(tmp, "config"),
                   XDG_DATA_HOME=os.path.join(tmp, "data"), XDG_CACHE_HOME=os.path.join(tmp, "cache"))
        os.makedirs(env["HOME"], exist_ok=True)
        p = subprocess.run([blender, "-b", "--factory-startup", "--python", script, "--", os.path.abspath(ADDON_PARENT),
                            tmp], capture_output=True, text=True, timeout=600, env=env)
        line = next((l for l in p.stdout.splitlines() if l.startswith("ROUNDTRIP_JSON ")), None)
        if line is None:
            print(p.stdout[-3000:], p.stderr[-3000:])
            check(False, "Blender ran the script")
            return 1
        out = json.loads(line[len("ROUNDTRIP_JSON "):])
        same = lambda a, b: os.path.normpath(a or "") == os.path.normpath(b)
        d = out["pushed"][0]
        maps = d.get("maps", {})
        check(same(maps.get("base_color", {}).get("path"), os.path.join(tmp, "Mai_basecolor.png")),
              "Patina's base color is found behind the AO multiply (%s)" % maps.get("base_color"))
        check(same(maps.get("roughness", {}).get("path"), os.path.join(tmp, "Mai_roughness.png")) and
              same(maps.get("metallic", {}).get("path"), os.path.join(tmp, "Mai_metallic.png")),
              "and its roughness and metallic maps")
        check(same(maps.get("normal", {}).get("path"), os.path.join(tmp, "Mai_normal.png")) and
              abs(maps["normal"].get("strength", 0) - 1.0) < 1e-6, "and its normal map (%s)" % maps.get("normal"))
        check(not d.get("vertex_colors") and "alpha_mode" not in d, "it shows no vertex colors and isn't see-through")
        hand = out["hand"]
        hm = hand[0].get("maps", {})
        check(hm.get("roughness", {}).get("channel") == 1 and same(hm["roughness"].get("path"), os.path.join(tmp, "orm.png")),
              "a hand-made material: roughness from the green channel (%s)" % hm.get("roughness"))
        check(hm.get("metallic", {}).get("invert") is True, "an inverted image is read inverted")
        check(hand[0].get("vertex_colors") is True and "base_color" not in hm, "a Color Attribute into Base Color counts")
        check(any("generated images" in n for n in hand[0].get("notes", [])) and "emission" not in hm,
              "an image only in Blender is noted, not sent (%s)" % hand[0].get("notes"))
        check(hand[1].get("principled") is False and abs(hand[1]["base_color"][2] - 0.9) < 1e-4,
              "a slot without a Principled BSDF sends its viewport color")
        check(hand[2] is None, "an empty slot is null")

        # a Patina with no project for Mai starts from what Blender shows
        port = free_port()
        proc = subprocess.Popen([exe, "--headless", "--port", str(port)], env=dict(env, PATINA_EXE=exe),
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            c = protocol.JsonLineClient(port=port, connect_timeout=1.0, response_timeout=300.0)
            for _ in range(100):
                try:
                    c.connect()
                    break
                except Exception:
                    time.sleep(0.1)
            r = c.request("bridge_open_mesh", {"path": out["path"], "object": "Mai", "blender_port": free_port(),
                                               "exchange_dir": tmp, "resolution": 256})
            mt = r.get("model_textures") or {}
            check(mt.get("textures", 0) >= 3 and not mt.get("vertex_colors"),
                  "Patina brings Blender's textures in as a layer, without the black Color attribute (%s)" % mt)
            od = os.path.join(tmp, "out")
            os.makedirs(od, exist_ok=True)
            c.request("export_textures", {"directory": od, "preset": "blender", "size": 64, "base_name": "t"})
            from PIL import Image  # noqa: PLC0415
            f = next(os.path.join(od, n) for n in sorted(os.listdir(od)) if "basecolor" in n)
            px = Image.open(f).convert("RGB").getpixel((32, 32))
            check(all(abs(a - b) < 12 for a, b in zip(px, colors["basecolor"])), "showing the pushed base color (%s)" % (px,))
            f = next(os.path.join(od, n) for n in sorted(os.listdir(od)) if "roughness" in n)
            px = Image.open(f).convert("L").getpixel((32, 32))
            check(abs(px - 90) < 12, "and roughness (%s)" % px)
        finally:
            proc.terminate()
            try:
                proc.wait(10)
            except subprocess.TimeoutExpired:
                proc.kill()
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("material roundtrip: %s" % ("all checks passed" if not failures else "%d FAILED" % len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
