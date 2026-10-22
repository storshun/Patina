#!/usr/bin/env python3
"""The model to other programs (Patina 0.28, add-on 0.9): export_model writes a .glb that Blender's own glTF importer
reads with its textures; send_model_to_blender hands Blender the mesh and its textures (patina_new_object), the add-on
builds the object (UVs, material slots, Patina's material) and Patina stays linked to it.

  PATINA_BIN=build/patina python3 tests/blender/test_model_export.py
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


IMPORT_GLB = r'''
import bpy, json, sys
path = sys.argv[sys.argv.index("--") + 1]
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=path)
objs = [o for o in bpy.data.objects if o.type == "MESH"]
out = {"objects": len(objs)}
if objs:
    o = objs[0]
    out["faces"] = len(o.data.polygons)
    out["uv"] = len(o.data.uv_layers)
    mat = o.data.materials[0] if o.data.materials else None
    if mat and mat.node_tree:
        b = next((n for n in mat.node_tree.nodes if n.type == "BSDF_PRINCIPLED"), None)
        out["base_linked"] = bool(b and b.inputs["Base Color"].is_linked)
        out["normal_linked"] = bool(b and b.inputs["Normal"].is_linked)
        out["alpha_linked"] = bool(b and b.inputs["Alpha"].is_linked)
print("CHECK_JSON " + json.dumps(out))
'''

NEW_OBJECT = r'''
import bpy, json, sys
argv = sys.argv[sys.argv.index("--") + 1:]
sys.path.insert(0, argv[0])
args = json.loads(open(argv[1]).read())
bpy.ops.wm.read_factory_settings(use_empty=True)
import patina_bridge
from patina_bridge import server
# a source object, as if it were the linked one
me = bpy.data.meshes.new("Src")
src = bpy.data.objects.new("Src", me)
bpy.context.scene.collection.objects.link(src)
src.location = (2.0, 0.0, 0.0)
args["source"] = "Src"
r = server._handle_incoming("patina_new_object", args, {})
o = bpy.data.objects.get(args["name"])
out = {"reply": r, "made": o is not None}
if o is not None:
    out["faces"] = len(o.data.polygons)
    out["uv"] = [l.name for l in o.data.uv_layers]
    out["location"] = list(o.matrix_world.translation)
    out["slots"] = [m.name if m else "" for m in o.data.materials]
    out["sent"] = o.name in server.state.sent
    mats = [s.material for s in o.material_slots if s.material]
    pm = [m for m in mats if m.name.startswith("Patina_")]
    out["patina_material"] = bool(pm)
    if pm and pm[0].node_tree:
        b = next((n for n in pm[0].node_tree.nodes if n.type == "BSDF_PRINCIPLED"), None)
        out["base_linked"] = bool(b and b.inputs["Base Color"].is_linked)
print("CHECK_JSON " + json.dumps(out))
'''


def run_blender(blender, script, args, env, tmp):
    path = os.path.join(tmp, "script_%d.py" % abs(hash(script)))
    open(path, "w").write(script)
    out = subprocess.run([blender, "-b", "--factory-startup", "--python", path, "--"] + args,
                         capture_output=True, text=True, timeout=600, env=env).stdout
    line = next((l for l in out.splitlines() if l.startswith("CHECK_JSON ")), None)
    if not line:
        print(out[-3000:])
    return json.loads(line[len("CHECK_JSON "):]) if line else {}


def main():
    exe = os.environ.get("PATINA_BIN") or os.path.join(HERE, "..", "..", "build", "patina")
    exe = os.path.abspath(exe)
    if not os.path.exists(exe):
        print("SKIP: patina binary not found (set PATINA_BIN)")
        return 0
    patina_port, blender_port = free_port(), free_port()
    tmp = tempfile.mkdtemp(prefix="patina_model_test_")
    env = dict(os.environ, XDG_CACHE_HOME=os.path.join(tmp, "cache"), XDG_DATA_HOME=os.path.join(tmp, "data"),
               XDG_CONFIG_HOME=os.path.join(tmp, "config"), HOME=os.path.join(tmp, "home"))
    os.makedirs(env["HOME"], exist_ok=True)
    proc = subprocess.Popen([exe, "--headless", "--port", str(patina_port)], stdout=subprocess.DEVNULL,
                            stderr=subprocess.PIPE, env=env)
    got = {}

    def handler(cmd, args, msg):
        got.setdefault(cmd, []).append(args)
        if cmd == "patina_new_object":
            return {"queued": True, "object": args.get("name")}
        if cmd == "patina_textures_updated":
            return {"queued": True, "object": args.get("object")}
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

    try:
        client = protocol.JsonLineClient(port=patina_port, connect_timeout=1.0, response_timeout=300.0)
        for _ in range(100):
            try:
                client.connect()
                break
            except Exception:
                time.sleep(0.1)
        client.request("new_project", {"primitive": "sphere", "resolution": 512})
        client.request("add_layer", {"type": "fill", "material": {"id": "patina_bricks_red"}})
        glb = os.path.join(tmp, "out", "ball.glb")
        r = client.request("export_model", {"path": glb, "texture_size": 256})
        check(os.path.isfile(glb) and r.get("format") == "glb", "export_model writes ball.glb")
        # to Blender as a new object
        r = client.request("send_model_to_blender", {"port": blender_port, "name": "Ball_patina"})
        check(r.get("object") == "Ball_patina" and r.get("textures", 0) >= 4, "send_model_to_blender: %s" % r)
        a = (got.get("patina_new_object") or [{}])[-1]
        check(a.get("name") == "Ball_patina" and os.path.isfile(a.get("path", "")) and "basecolor" in a.get("textures", {}),
              "Blender is asked for the object, with the mesh and its textures")
        # Patina is linked to it now: the next push goes to the new object
        client.request("send_to_blender", {})
        time.sleep(0.5)
        up = (got.get("patina_textures_updated") or [{}])[-1]
        check(up.get("object") == "Ball_patina" and up.get("uv_path") is None,
              "later pushes go to the new object (no UV file: its UVs are Patina's)")
        e = ""
        try:
            client.request("send_model_to_blender", {"port": free_port()})
        except protocol.RemoteError as ex:
            e = str(ex)
        check("not listening" in e, "no Blender: says to start the live link")

        blender = shutil.which("blender")
        if not blender:
            print("  skip Blender checks: no blender on PATH")
        else:
            g = run_blender(blender, IMPORT_GLB, [glb], env, tmp)
            check(g.get("objects") == 1 and g.get("faces", 0) > 0 and g.get("uv") == 1,
                  "Blender's glTF importer reads the model (%s)" % g)
            check(g.get("base_linked") and g.get("normal_linked"), "with its base color and normal textures")
            argf = os.path.join(tmp, "new_object.json")
            open(argf, "w").write(json.dumps(a))
            n = run_blender(blender, NEW_OBJECT, [os.path.abspath(os.path.join(ADDON, "..")), argf], env, tmp)
            check(n.get("made") and n.get("faces", 0) > 0 and n.get("uv") == ["UVMap"],
                  "the add-on makes the object from Patina's mesh, with its UVs (%s)" % {k: n.get(k) for k in ("made", "faces", "uv")})
            check(n.get("sent") is True, "and links it (its mesh is what Patina sent)")
            loc = n.get("location") or [0, 0, 0]
            check(abs(loc[0] - 2.0) < 1e-4, "next to the source object, in its place")
            check(n.get("patina_material") and n.get("base_linked"), "Patina's textures on it")
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
