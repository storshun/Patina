#!/usr/bin/env python3
"""Patina's material library in Blender's Asset Browser (Patina 0.25, add-on 0.8).

Plays Blender's live link against `patina --headless` (blender_material_library
reaches the add-on; export_material_maps writes what the add-on asks for), then,
when a `blender` is on PATH, builds the library with the add-on's real
library_build.py and checks the assets in it.

  PATINA_BIN=build/patina python3 tests/blender/test_material_library.py
"""
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ADDON = os.path.join(HERE, "..", "..", "blender", "patina_bridge")
sys.path.insert(0, ADDON)
import protocol  # noqa: E402  (bpy-free module of the add-on)


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def png_depth(path):
    with open(path, "rb") as f:
        head = f.read(26)
    assert head[:8] == b"\x89PNG\r\n\x1a\n", path
    return head[24]


CHECK_BLEND = r'''
import bpy, json, sys
lib = sys.argv[sys.argv.index("--") + 1]
bpy.ops.wm.open_mainfile(filepath=lib)
out = {}
for m in bpy.data.materials:
    ad = m.asset_data
    if ad is None:
        continue
    kinds = sorted(n.bl_idname for n in m.node_tree.nodes)
    imgs = sorted(n.image.filepath for n in m.node_tree.nodes if n.bl_idname == "ShaderNodeTexImage" and n.image)
    boxes = sorted({n.projection for n in m.node_tree.nodes if n.bl_idname == "ShaderNodeTexImage"})
    out[m.get("patina_material_id")] = {"name": m.name, "catalog": ad.catalog_id, "tags": [t.name for t in ad.tags],
        "description": ad.description, "nodes": kinds, "images": imgs, "projection": boxes,
        "preview": list(m.preview.image_size) if m.preview else [0, 0]}
print("CHECK_JSON " + json.dumps(out))
'''


CHECK_BRIDGE_MATERIAL = r'''
import bpy, sys, os
sys.path.insert(0, sys.argv[sys.argv.index("--") + 1])
from patina_bridge import materials
d = sys.argv[sys.argv.index("--") + 2]
bpy.ops.mesh.primitive_cube_add()
obj = bpy.context.object
tex = {k: os.path.join(d, k + ".png") for k in ["basecolor", "opacity", "sheen", "coat", "subsurface"]}
images, warn = materials.load_textures(obj.name, tex)
mat = materials.build_material(obj, images)
b = next(n for n in mat.node_tree.nodes if n.type == "BSDF_PRINCIPLED")
linked = [n for n in ["Alpha", "Sheen Weight", "Coat Weight", "Subsurface Weight"] if b.inputs[n].is_linked]
images2, _ = materials.load_textures(obj.name, {"basecolor": tex["basecolor"]})
materials.build_material(obj, images2)
after = [n for n in ["Alpha", "Sheen Weight", "Coat Weight", "Subsurface Weight"] if b.inputs[n].is_linked]
print("CHECK_JSON " + __import__("json").dumps({"linked": linked, "after": after,
      "method": getattr(mat, "surface_render_method", "")}))
'''


def main():
    exe = os.environ.get("PATINA_BIN") or os.path.join(HERE, "..", "..", "build", "patina")
    exe = os.path.abspath(exe)
    if not os.path.exists(exe):
        print("SKIP: patina binary not found (set PATINA_BIN)")
        return 0
    patina_port, blender_port = free_port(), free_port()
    tmp = tempfile.mkdtemp(prefix="patina_matlib_test_")
    env = dict(os.environ, XDG_CACHE_HOME=os.path.join(tmp, "cache"), XDG_DATA_HOME=os.path.join(tmp, "data"),
               XDG_CONFIG_HOME=os.path.join(tmp, "config"), HOME=os.path.join(tmp, "home"))
    os.makedirs(env["HOME"], exist_ok=True)
    proc = subprocess.Popen([exe, "--headless", "--port", str(patina_port)], stdout=subprocess.DEVNULL,
                            stderr=subprocess.PIPE, env=env)
    asked = []
    old_addon = {"on": False}

    def handler(cmd, args, msg):
        if cmd == "patina_material_library":
            if old_addon["on"]:
                raise protocol.CommandError("Unknown command %r" % cmd)
            asked.append(args)
            return {"started": True, "directory": "/somewhere/Patina Materials"}
        if cmd == "ping":
            return {"app": "blender-test"}
        raise protocol.CommandError("Unknown command %r" % cmd)

    server = protocol.PollingLineServer(handler, port=blender_port)
    server.start()
    stop = threading.Event()

    def pump():
        while not stop.is_set():
            server.poll()
            time.sleep(0.02)

    threading.Thread(target=pump, daemon=True).start()
    failures = 0

    def check(cond, what):
        nonlocal failures
        print(("  ok   " if cond else "  FAIL ") + what)
        failures += 0 if cond else 1

    def error_of(cmd, args):
        try:
            client.request(cmd, args)
        except protocol.RemoteError as e:
            return str(e)
        return ""

    try:
        client = protocol.JsonLineClient(port=patina_port, connect_timeout=1.0, response_timeout=300.0)
        for _ in range(100):
            try:
                client.connect()
                break
            except Exception:
                time.sleep(0.1)
        check(client.request("ping").get("app") == "patina", "ping")

        # ---- Patina asks Blender to build the library
        r = client.request("blender_material_library", {"port": blender_port, "size": 512, "mapping": "uv"})
        check(r.get("started") is True and r.get("port") == blender_port, "Blender's add-on starts the library")
        check(asked and asked[-1] == {"size": 512, "mapping": "uv", "which": "all"},
              "it gets the size, mapping and which (%s)" % (asked[-1] if asked else None))
        e = error_of("blender_material_library", {"port": free_port()})
        check("not listening" in e, "no Blender: says to start the live link (%s)" % e)
        old_addon["on"] = True
        e = error_of("blender_material_library", {"port": blender_port})
        check("older than 0.8" in e, "an older add-on: says to install the new one (%s)" % e)
        old_addon["on"] = False
        e = error_of("blender_material_library", {"port": blender_port, "size": 300})
        check("512, 1024 or 2048" in e, "a size it can't use is refused")
        client.request("features", {"id": "blender_library", "on": False})
        e = error_of("blender_material_library", {"port": blender_port})
        check("switched off" in e, "switched off with the feature")
        client.request("features", {"id": "blender_library", "on": True})

        # ---- what the add-on asks for: JPEG maps, 16-bit heights, built-in and own materials
        client.request("create_material", {"name": "Test Mine", "id": "user_test_mine", "category": "Mine",
                                           "type": "procedural", "generator": "dots", "params": {"scale": 6}})
        lib = os.path.join(tmp, "home", "Documents", "Patina Materials")
        maps = os.path.join(lib, "maps")
        ids = ["patina_leopard", "patina_gold_polished"]
        r = client.request("export_material_maps", {"directory": maps, "ids": ids, "size": 64, "format": "jpg",
                                                     "quality": 90, "height16": True})
        check(r.get("count") == 2, "two materials exported")
        man = json.load(open(os.path.join(maps, "manifest.json")))["materials"]
        leo = next(m for m in man if m["id"] == "patina_leopard")
        gold = next(m for m in man if m["id"] == "patina_gold_polished")
        check(leo["maps"]["color"].endswith(".jpg") and leo["maps"]["normal"].endswith(".jpg")
              and leo["maps"]["rmh"].endswith(".png"), "color and normal maps as JPEG, rmh as PNG")
        with open(os.path.join(maps, leo["maps"]["color"]), "rb") as f:
            check(f.read(3) == b"\xff\xd8\xff", "the color map is a JPEG")
        check(png_depth(os.path.join(maps, leo["maps"]["height"])) == 16, "the height map is a 16-bit PNG")
        check("height" not in gold["maps"], "a flat material has no height map")
        check(leo.get("builtin") is True and len(leo.get("average", {}).get("color", [])) == 3,
              "the manifest says built-in and the average color")
        r = client.request("export_material_maps", {"directory": os.path.join(tmp, "mine"), "which": "user",
                                                    "size": 32, "thumbnails": False})
        mine = json.load(open(os.path.join(tmp, "mine", "manifest.json")))["materials"]
        check(r.get("count") == 1 and mine[0]["id"] == "user_test_mine" and mine[0]["builtin"] is False,
              "which=user exports only your own materials")
        r = client.request("export_material_maps", {"directory": maps, "ids": ids + ["user_test_mine"], "size": 64,
                                                    "format": "jpg", "height16": True})
        check(r.get("count") == 3, "built-in and own materials together")

        # ---- the add-on's library build in a real Blender
        blender = shutil.which("blender")
        if not blender:
            print("  skip library build: no blender on PATH")
        else:
            script = os.path.abspath(os.path.join(ADDON, "library_build.py"))
            check_py = os.path.join(tmp, "check_blend.py")
            open(check_py, "w").write(CHECK_BLEND)
            blend = os.path.join(lib, "Patina Materials.blend")
            for mapping in ("UV", "BOX"):
                out = subprocess.run([blender, "-b", "--factory-startup", "--python", script, "--", maps, blend, mapping],
                                     capture_output=True, text=True, timeout=600, env=env).stdout
                check("PATINA_LIBRARY_DONE 3" in out, "%s: Blender builds a library of 3 materials" % mapping)
                out = subprocess.run([blender, "-b", "--factory-startup", "--python", check_py, "--", blend],
                                     capture_output=True, text=True, timeout=600, env=env).stdout
                line = next((l for l in out.splitlines() if l.startswith("CHECK_JSON ")), None)
                got = json.loads(line[len("CHECK_JSON "):]) if line else {}
                check(set(got) == {"patina_leopard", "patina_gold_polished", "user_test_mine"},
                      "%s: all three are assets (%s)" % (mapping, sorted(got)))
                if len(got) != 3:
                    continue
                cats = open(os.path.join(lib, "blender_assets.cats.txt")).read()
                check(all(got[k]["catalog"] in cats for k in got), "%s: each sits in a catalog" % mapping)
                check(":Patina/My materials/Mine:" in cats, "%s: own materials under Patina/My materials" % mapping)
                L = got["patina_leopard"]
                check(L["preview"][0] > 0, "%s: Patina's preview is the asset preview" % mapping)
                check(all(p.startswith("//maps/") for p in L["images"]) and len(L["images"]) >= 3,
                      "%s: maps stored relative to the library (%s)" % (mapping, L["images"]))
                if mapping == "UV":
                    check("ShaderNodeNormalMap" in L["nodes"] and L["projection"] == ["FLAT"],
                          "UV: Patina's normal map on a UV-mapped material")
                else:
                    check("ShaderNodeBump" in L["nodes"] and L["projection"] == ["BOX"]
                          and any(p.endswith("_height.png") for p in L["images"]),
                          "BOX: box projected, bump from the 16-bit height")
                G = got["patina_gold_polished"]
                check(not any(n == "ShaderNodeTexImage" for n in G["nodes"]), "%s: a flat material has no images" % mapping)
                check("Animals" in L["tags"] or "Patterns" in L["tags"], "%s: tagged with its category (%s)"
                      % (mapping, L["tags"]))
            # 0.26: the live link's material takes opacity, sheen, clearcoat and subsurface maps
            from PIL import Image
            mdir = os.path.join(tmp, "maps026")
            os.makedirs(mdir, exist_ok=True)
            for k in ("basecolor", "opacity", "sheen", "coat", "subsurface"):
                Image.new("RGB" if k == "basecolor" else "L", (8, 8), (200, 10, 10) if k == "basecolor" else 128).save(
                    os.path.join(mdir, k + ".png"))
            bm = os.path.join(tmp, "check_bridge.py")
            open(bm, "w").write(CHECK_BRIDGE_MATERIAL)
            out = subprocess.run([blender, "-b", "--factory-startup", "--python", bm, "--",
                                  os.path.abspath(os.path.join(ADDON, "..")), mdir],
                                 capture_output=True, text=True, timeout=600, env=env).stdout
            line = next((l for l in out.splitlines() if l.startswith("CHECK_JSON ")), None)
            got = json.loads(line[len("CHECK_JSON "):]) if line else {}
            check(got.get("linked") == ["Alpha", "Sheen Weight", "Coat Weight", "Subsurface Weight"],
                  "live link: opacity, sheen, clearcoat and subsurface maps reach the Principled BSDF (%s)" % got.get("linked"))
            check(got.get("after") == [], "and leave it when Patina no longer sends them")
            check(got.get("method") in ("DITHERED", ""), "see-through without sorting (%s)" % got.get("method"))
    finally:
        stop.set()
        server.stop()
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
