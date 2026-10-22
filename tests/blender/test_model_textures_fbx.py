#!/usr/bin/env python3
"""A model's own textures (Patina 0.31) from files Blender writes: a plane with an image texture (color with an alpha
cut-out) and a roughness value, exported by Blender as FBX (textures embedded, and copied beside it), OBJ and GLB.
Patina opens each with its textures, and its color and opacity maps match the image. Then (0.32) the plane with
vertex colors as a PLY, and through the bridge: with its material, what the material shows (0.44, add-on 0.13),
without one, its color attribute.

  PATINA_BIN=build/patina python3 tests/blender/test_model_textures_fbx.py
"""
import base64
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "blender", "patina_bridge"))
sys.path.insert(0, os.path.join(HERE, "..", "api"))
import protocol  # noqa: E402
from test_model_textures import read_png, texel  # noqa: E402

MAKE = r'''
import bpy, os, sys
out = sys.argv[sys.argv.index("--") + 1]
bpy.ops.wm.read_factory_settings(use_empty=True)
W = 64
img = bpy.data.images.new("tex_color", W, W, alpha=True)
px = []
for y in range(W):          # Blender rows: y = 0 at the bottom
    for x in range(W):
        if x < W // 2: px += [0.7, 0.02, 0.02, 1.0]
        else: px += [0.02, 0.02, 0.7, 0.0 if y < W // 4 else 1.0]
img.pixels = px
img.filepath_raw = os.path.join(out, "tex_color.png"); img.file_format = "PNG"; img.save()
bpy.ops.mesh.primitive_plane_add(size=2.0)
o = bpy.context.object
m = bpy.data.materials.new("Painted"); m.use_nodes = True
nt = m.node_tree; b = next(n for n in nt.nodes if n.type == "BSDF_PRINCIPLED")
t = nt.nodes.new("ShaderNodeTexImage"); t.image = img
nt.links.new(t.outputs["Color"], b.inputs["Base Color"]); nt.links.new(t.outputs["Alpha"], b.inputs["Alpha"])
b.inputs["Roughness"].default_value = 0.35
o.data.materials.append(m)
os.makedirs(os.path.join(out, "copied"), exist_ok=True)
bpy.ops.export_scene.fbx(filepath=os.path.join(out, "embedded.fbx"), path_mode="COPY", embed_textures=True)
bpy.ops.export_scene.fbx(filepath=os.path.join(out, "copied", "beside.fbx"), path_mode="COPY", embed_textures=False)
bpy.ops.wm.obj_export(filepath=os.path.join(out, "plane.obj"), export_materials=True, path_mode="RELATIVE")
bpy.ops.export_scene.gltf(filepath=os.path.join(out, "plane.glb"), export_format="GLB")
# vertex colors: red on the left two corners, blue on the right two, as a PLY (Patina 0.32)
ca = o.data.color_attributes.new("Col", "BYTE_COLOR", "POINT")
for i, v in enumerate(o.data.vertices):
    ca.data[i].color_srgb = (0.9, 0.1, 0.1, 1.0) if v.co.x < 0 else (0.1, 0.1, 0.9, 1.0)
bpy.ops.wm.ply_export(filepath=os.path.join(out, "plane_colors.ply"), export_colors="SRGB", export_uv=True)
# and as the add-on sends it to Patina: (0.44, add-on 0.13) with its material, what the material shows; without
# one, (0.32) the color attribute
sys.path.insert(0, sys.argv[sys.argv.index("--") + 2])
from patina_bridge import mesh_io
r = mesh_io.export_object(o, os.path.join(out, "bridge_material"))
print("PTMESH_MAT " + r["path"])
o.data.materials.clear()
r = mesh_io.export_object(o, os.path.join(out, "bridge"))
print("PTMESH " + r["path"])
print("MADE")
'''


READ_PLY = r'''
import bpy, json, sys
p = sys.argv[sys.argv.index("--") + 1]
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.wm.ply_import(filepath=p)
o = bpy.context.selected_objects[0]
me = o.data
ca = me.color_attributes[0] if me.color_attributes else None
out = {"faces": len(me.polygons), "uv": len(me.uv_layers)}
if ca:
    out["colors"] = [list(d.color_srgb)[:3] for d in ca.data]
print("PLY_JSON " + json.dumps(out))
'''


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def main():
    exe = os.path.abspath(os.environ.get("PATINA_BIN") or os.path.join(HERE, "..", "..", "build", "patina"))
    blender = shutil.which("blender")
    if not os.path.exists(exe) or not blender:
        print("SKIP: needs the patina binary (PATINA_BIN) and blender on PATH")
        return 0
    tmp = tempfile.mkdtemp(prefix="patina_model_tex_fbx_")
    failures = 0

    def check(cond, what):
        nonlocal failures
        print(("  ok   " if cond else "  FAIL ") + what)
        failures += 0 if cond else 1

    env = dict(os.environ, HOME=os.path.join(tmp, "home"), XDG_DATA_HOME=os.path.join(tmp, "data"),
               XDG_CONFIG_HOME=os.path.join(tmp, "config"), XDG_CACHE_HOME=os.path.join(tmp, "cache"))
    os.makedirs(env["HOME"], exist_ok=True)
    out = os.path.join(tmp, "models")
    os.makedirs(out)
    script = os.path.join(tmp, "make.py")
    open(script, "w").write(MAKE)
    r = subprocess.run([blender, "-b", "--factory-startup", "--python", script, "--", out,
                        os.path.abspath(os.path.join(HERE, "..", "..", "blender"))], capture_output=True,
                       text=True, timeout=600, env=env)
    ptmesh = next((l[7:] for l in r.stdout.splitlines() if l.startswith("PTMESH ")), "")
    ptmesh_mat = next((l[11:] for l in r.stdout.splitlines() if l.startswith("PTMESH_MAT ")), "")
    if "MADE" not in r.stdout:
        print(r.stdout[-2000:], r.stderr[-2000:])
        check(False, "Blender made the models")
        return 1
    # the copied FBX must not find the image through the first folder
    port = free_port()
    proc = subprocess.Popen([exe, "--headless", "--port", str(port)], env=env, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    try:
        c = protocol.JsonLineClient(port=port, connect_timeout=1.0, response_timeout=300.0)
        for _ in range(100):
            try:
                c.connect()
                break
            except Exception:
                time.sleep(0.1)
        for name in ("embedded.fbx", os.path.join("copied", "beside.fbx"), "plane.obj", "plane.glb"):
            c.request("open_file", {"path": os.path.join(out, name)})
            rep = c.request("get_state", {})["mesh"].get("model_textures") or {}
            check(rep.get("textures", 0) >= 1 and not rep.get("missing"), "%s: its texture comes in (%s)" % (name, {
                k: rep.get(k) for k in ("textures", "missing", "channels")}))
            col = read_png(base64.b64decode(c.request("get_texture", {"channel": "basecolor", "size": 128})["image_png_base64"]))
            red, blue = texel(col, 0.25, 0.5), texel(col, 0.75, 0.5)
            check(red[0] > 0.7 and red[2] < 0.3 and blue[2] > 0.7 and blue[0] < 0.3,
                  "%s: red on the left, blue on the right, as in Blender (%s, %s)" % (name, red[:3], blue[:3]))
            op = read_png(base64.b64decode(c.request("get_texture", {"channel": "opacity", "size": 128})["image_png_base64"]))
            check(texel(op, 0.75, 0.1)[0] < 0.1 and texel(op, 0.75, 0.6)[0] > 0.9 and texel(op, 0.25, 0.1)[0] > 0.9,
                  "%s: the cut-out from the alpha (%s)" % (name, [texel(op, *p)[0] for p in ((0.75, 0.1), (0.75, 0.6), (0.25, 0.1))]))
            ro = read_png(base64.b64decode(c.request("get_texture", {"channel": "roughness", "size": 128})["image_png_base64"]))
            check(abs(texel(ro, 0.25, 0.5)[0] - 0.35) < 0.06, "%s: roughness 0.35 (%s)" % (name, texel(ro, 0.25, 0.5)[0]))
        # a PLY with vertex colors from Blender: the colors, on the right sides
        c.request("open_file", {"path": os.path.join(out, "plane_colors.ply")})
        rep = c.request("get_state", {})["mesh"].get("model_textures") or {}
        check(rep.get("vertex_colors") is True, "plane_colors.ply: Blender's vertex colors come in (%s)" % rep.get("channels"))
        col = read_png(base64.b64decode(c.request("get_texture", {"channel": "basecolor", "size": 128})["image_png_base64"]))
        left, right = texel(col, 0.03, 0.5), texel(col, 0.97, 0.5)
        check(left[0] > 0.8 and left[2] < 0.25 and right[2] > 0.8 and right[0] < 0.25,
              "red on the left, blue on the right (%s, %s)" % (left[:3], right[:3]))
        # from Blender through the bridge, with its material: the image it shows wins over the color attribute
        check(os.path.isfile(ptmesh_mat), "the add-on wrote the object with its material (%s)" % ptmesh_mat)
        if os.path.isfile(ptmesh_mat):
            r = c.request("bridge_open_mesh", {"path": ptmesh_mat, "object": "Plane"})
            mt = r.get("model_textures") or {}
            check(mt.get("textures") == 1 and "opacity" in mt.get("channels", []) and not mt.get("vertex_colors"),
                  "bridge: Blender's material comes in, its image and cut-out (%s)" % mt)
            col = read_png(base64.b64decode(c.request("get_texture", {"channel": "basecolor", "size": 128})["image_png_base64"]))
            red, blue = texel(col, 0.25, 0.5), texel(col, 0.75, 0.5)
            check(red[0] > 0.6 and red[2] < 0.3 and blue[2] > 0.6 and blue[0] < 0.3,
                  "bridge: the material's image (%s, %s)" % (red[:3], blue[:3]))
            c.request("new_project", {"primitive": "cube"})
        # without a material: the object's color attribute
        check(os.path.isfile(ptmesh), "the add-on wrote the object (%s)" % ptmesh)
        if os.path.isfile(ptmesh):
            r = c.request("bridge_open_mesh", {"path": ptmesh, "object": "Plane"})
            check((r.get("model_textures") or {}).get("vertex_colors") is True, "bridge: its vertex colors come in (%s)" % r.get("model_textures"))
            col = read_png(base64.b64decode(c.request("get_texture", {"channel": "basecolor", "size": 128})["image_png_base64"]))
            left, right = texel(col, 0.03, 0.5), texel(col, 0.97, 0.5)
            check(left[0] > 0.8 and left[2] < 0.25 and right[2] > 0.8 and right[0] < 0.25,
                  "bridge: red on the left, blue on the right (%s, %s)" % (left[:3], right[:3]))
            r = c.request("bridge_update_mesh", {"path": ptmesh, "object": "Plane"})
            names = [l["name"] for l in c.request("list_layers", {})]
            check(names.count("From the model") == 1, "an update keeps the layers and adds no second one (%s)" % names)
        # and back: Patina's PLY in Blender, with the colors
        out_ply = os.path.join(out, "from_patina.ply")
        c.request("export_model", {"path": out_ply})
        chk = os.path.join(tmp, "read_ply.py")
        open(chk, "w").write(READ_PLY)
        r = subprocess.run([blender, "-b", "--factory-startup", "--python", chk, "--", out_ply], capture_output=True,
                           text=True, timeout=600, env=env)
        line = next((l for l in r.stdout.splitlines() if l.startswith("PLY_JSON ")), None)
        got = json.loads(line[9:]) if line else {}
        check(got.get("faces") == 1 and got.get("colors"), "Blender reads Patina's PLY with its colors (%s)" % got)
        cols = got.get("colors") or [[0, 0, 0]]
        check(any(c[0] > 0.8 and c[2] < 0.25 for c in cols) and any(c[2] > 0.8 and c[0] < 0.25 for c in cols),
              "red and blue corners in Blender (%s)" % cols)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
        shutil.rmtree(tmp, ignore_errors=True)
    print("%s (%d failure%s)" % ("OK" if failures == 0 else "FAILED", failures, "" if failures == 1 else "s"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
