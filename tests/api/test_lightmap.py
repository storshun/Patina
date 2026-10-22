#!/usr/bin/env python3
"""Lightmap UVs (0.35) against a real headless Patina: a box whose faces share one UV square gets a second UV map
with no overlaps; it is saved with the project, exported as the glTF's TEXCOORD_1 and read back from a .glb;
a transform keeps it, a reduction drops it; Blender (when on PATH) imports the .glb with two UV maps, and an
object Patina sends gets the UV map Lightmap.

    PATINA_BIN=build/patina python3 tests/api/test_lightmap.py
"""
import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "blender", "patina_bridge"))
import protocol  # noqa: E402

failures = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        failures.append(what)


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


BOX = ("v -1 -1 -1\nv 1 -1 -1\nv 1 1 -1\nv -1 1 -1\nv -1 -1 1\nv 1 -1 1\nv 1 1 1\nv -1 1 1\n"
       "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n"
       "f 1/1 4/4 3/3 2/2\nf 5/1 6/2 7/3 8/4\nf 1/1 2/2 6/3 5/4\nf 2/1 3/2 7/3 6/4\nf 3/1 4/2 8/3 7/4\nf 4/1 1/2 5/3 8/4\n")

READ_GLB = r'''
import bpy, json, sys
p = sys.argv[sys.argv.index("--") + 1]
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=p)
o = [o for o in bpy.data.objects if o.type == "MESH"][0]
print("LM_JSON " + json.dumps({"uv": [l.name for l in o.data.uv_layers]}))
'''

NEW_OBJECT = r'''
import bpy, json, sys
argv = sys.argv[sys.argv.index("--") + 1:]
sys.path.insert(0, argv[0])
args = json.loads(open(argv[1]).read())
bpy.ops.wm.read_factory_settings(use_empty=True)
from patina_bridge import server
r = server._handle_incoming("patina_new_object", args, {})
o = bpy.data.objects.get(args["name"])
print("LM_JSON " + json.dumps({"uv": [l.name for l in o.data.uv_layers] if o else None,
                               "active": o.data.uv_layers.active.name if o and o.data.uv_layers.active else None}))
'''


def glb_json(path):
    b = open(path, "rb").read()
    n = struct.unpack("<I", b[12:16])[0]
    return json.loads(b[20:20 + n])


def run_blender(blender, script, args, env, tmp):
    path = os.path.join(tmp, "lm_%d.py" % abs(hash(script)))
    open(path, "w").write(script)
    out = subprocess.run([blender, "-b", "--factory-startup", "--python", path, "--"] + args, capture_output=True,
                         text=True, timeout=600, env=env).stdout
    line = next((l for l in out.splitlines() if l.startswith("LM_JSON ")), None)
    return json.loads(line[8:]) if line else {}


def main():
    exe = os.path.abspath(os.environ.get("PATINA_BIN") or os.path.join(HERE, "..", "..", "build", "patina"))
    if not os.path.exists(exe):
        print("SKIP: patina binary not found (set PATINA_BIN)")
        return 0
    tmp = tempfile.mkdtemp(prefix="patina_lightmap_test_")
    env = dict(os.environ, HOME=os.path.join(tmp, "home"), XDG_DATA_HOME=os.path.join(tmp, "data"),
               XDG_CONFIG_HOME=os.path.join(tmp, "config"), XDG_CACHE_HOME=os.path.join(tmp, "cache"))
    os.makedirs(env["HOME"], exist_ok=True)
    box = os.path.join(tmp, "box.obj")
    open(box, "w").write(BOX)
    port, blender_port = free_port(), free_port()
    proc = subprocess.Popen([exe, "--headless", "--port", str(port)], env=env, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    got = {}

    def handler(cmd, args, msg):
        got.setdefault(cmd, []).append(args)
        if cmd == "patina_new_object":
            return {"queued": True, "object": args.get("name")}
        raise protocol.CommandError("Unknown command %r" % cmd)

    server = protocol.PollingLineServer(handler, port=blender_port)
    server.start()
    stop = threading.Event()
    threading.Thread(target=lambda: [server.poll() or time.sleep(0.02) for _ in iter(stop.is_set, True)], daemon=True).start()
    try:
        c = protocol.JsonLineClient(port=port, connect_timeout=1.0, response_timeout=300.0)
        for _ in range(100):
            try:
                c.connect()
                break
            except Exception:
                time.sleep(0.1)
        c.request("open_file", {"path": box})
        check(not c.request("get_state", {})["mesh"]["lightmap_uvs"], "a model starts without lightmap UVs")
        r = c.request("mesh_lightmap_uvs", {"lightmap_size": 256, "padding": 4})
        check(r["islands"] == 6 and r["overlaps"] == 0, "the stacked faces get their own space (%s)" % r)
        check(c.request("get_state", {})["mesh"]["lightmap_uvs"], "the model has them now")
        # saved with the project
        proj = os.path.join(tmp, "box.patina")
        c.request("save_project", {"path": proj})
        c.request("new_project", {"primitive": "sphere"})
        c.request("open_file", {"path": proj})
        check(c.request("get_state", {})["mesh"]["lightmap_uvs"], "saved with the project")
        # exported as TEXCOORD_1, and read back
        glb = os.path.join(tmp, "box.glb")
        c.request("export_model", {"path": glb, "texture_size": 256})
        g = glb_json(glb)
        attrs = g["meshes"][0]["primitives"][0]["attributes"]
        check("TEXCOORD_1" in attrs, "exported as the glTF's TEXCOORD_1 (%s)" % list(attrs))
        c.request("open_file", {"path": glb})
        check(c.request("get_state", {})["mesh"]["lightmap_uvs"], "a .glb with TEXCOORD_1 opens with them")
        # a transform keeps them, a reduction drops them
        c.request("mesh_transform", {"scale": 2.0})
        check(c.request("get_state", {})["mesh"]["lightmap_uvs"], "a transform keeps them")
        c.request("open_file", {"path": os.path.join(HERE, "..", "..", "samples", "Suzanne.glb")})
        c.request("mesh_lightmap_uvs", {})
        c.request("mesh_reduce", {"ratio": 0.5})
        check(not c.request("get_state", {})["mesh"]["lightmap_uvs"], "a reduction drops them (make them again)")
        r = c.request("mesh_lightmap_uvs", {"source": "auto"})
        check(r["overlaps"] == 0 and r["islands"] > 3 and r["fill"] > 0.4,
              "new charts made for them (xatlas) (%s)" % {k: r[k] for k in ("islands", "overlaps", "fill")})
        # to Blender as a new object: the UV map Lightmap
        r = c.request("send_model_to_blender", {"port": blender_port, "name": "Suzanne_patina"})
        a = (got.get("patina_new_object") or [{}])[-1]
        blender = shutil.which("blender")
        if not blender:
            print("  skip Blender checks: no blender on PATH")
        else:
            out = run_blender(blender, READ_GLB, [glb], env, tmp)
            check(len(out.get("uv", [])) == 2, "Blender imports the .glb with two UV maps (%s)" % out.get("uv"))
            argf = os.path.join(tmp, "new_object.json")
            open(argf, "w").write(json.dumps(a))
            out = run_blender(blender, NEW_OBJECT, [os.path.abspath(os.path.join(HERE, "..", "..", "blender")), argf], env, tmp)
            check(out.get("uv") == ["UVMap", "Lightmap"] and out.get("active") == "UVMap",
                  "an object Patina sends has the UV map Lightmap, UVMap still active (%s)" % out)
    finally:
        stop.set()
        server.stop()
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
        shutil.rmtree(tmp, ignore_errors=True)
    print("lightmap: %s" % ("all checks passed" if not failures else "%d FAILED" % len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
