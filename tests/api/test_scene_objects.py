#!/usr/bin/env python3
"""(1.2) Several objects in one project, against a real headless Patina: a file with several meshes opens as an object
each (their texture sets matched to the first one's texel density); each object has its own layers; one moved,
turned and scaled is still painted where the brush aims; the project keeps every object, its layers and its place,
and the one active; a model added to the startup sphere's scene takes its place; the model export puts them all in
one file (glTF: a node each with its matrix), the texture export each object's maps; Blender sends several objects
(the first starts a scene, the others are added beside it, an update goes to its own object, the automatic sync
leaves out an object the scene doesn't have), with Blender's placement; copies, removals and the Local view.

    PATINA_BIN=build/patina python3 tests/api/test_scene_objects.py
"""
import json
import math
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
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


def sphere(out, base, cx, cy, cz, r, seg=32, rings=16):
    """a UV sphere with its own UVs (the front pole at u 0.25, v 0.5), wound outward, after `base` vertices"""
    w = seg + 1
    for j in range(rings + 1):
        th = math.pi * j / rings
        for i in range(seg + 1):
            ph = 2 * math.pi * i / seg
            out.append("v %f %f %f" % (cx + r * math.sin(th) * math.cos(ph), cy + r * math.cos(th), cz + r * math.sin(th) * math.sin(ph)))
            out.append("vt %f %f" % (i / seg, 1 - j / rings))
    for j in range(rings):
        for i in range(seg):
            a = base + j * w + i + 1
            b, c, d = a + 1, base + (j + 1) * w + i + 2, base + (j + 1) * w + i + 1
            out.append("f %d/%d %d/%d %d/%d %d/%d" % (a, a, b, b, c, c, d, d))
    return (rings + 1) * w


def character(path):
    out, n = [], 0
    for name, c, r, seg in (("Body", (0, 0, 0), 1.0, 32), ("Hat", (0, 1.25, 0), 0.35, 32), ("Eye", (0.32, 0.25, 0.9), 0.12, 16)):
        out.append("o " + name)
        n += sphere(out, n, *c, r, seg, seg // 2)
    open(path, "w").write("\n".join(out) + "\n")


def sheet(path, name, n=4, size=1.0):
    """an n x n sheet in Blender's XY plane (Z up) as a .ptmesh"""
    pos, fs, lv, uv = [], [], [], []
    for j in range(n + 1):
        for i in range(n + 1):
            pos += [size * (i / n - 0.5), size * (j / n - 0.5), 0.0]
    for j in range(n):
        for i in range(n):
            q = [j * (n + 1) + i, j * (n + 1) + i + 1, (j + 1) * (n + 1) + i + 1, (j + 1) * (n + 1) + i]
            fs.append(4)
            lv += q
            for v in q:
                uv += [(v % (n + 1)) / n, (v // (n + 1)) / n]
    protocol.write_ptmesh(path, name, pos, fs, lv, extra_header={"had_uvs": True, "evaluated": False}, loop_uvs=uv,
                          face_material=[0] * len(fs))


def pixel(path, u, v):
    from PIL import Image  # noqa: PLC0415
    im = Image.open(path).convert("RGB")
    return im.getpixel((min(im.width - 1, int(u * im.width)), min(im.height - 1, int((1 - v) * im.height))))


def glb_json(path):
    d = open(path, "rb").read()
    n = struct.unpack("<I", d[12:16])[0]
    return json.loads(d[20:20 + n])


class Patina:
    def __init__(self, exe, env):
        self.port = free_port()
        self.proc = subprocess.Popen([exe, "--headless", "--port", str(self.port)], env=env,
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.c = protocol.JsonLineClient(port=self.port, connect_timeout=1.0, response_timeout=300.0)
        for _ in range(100):
            try:
                self.c.connect()
                break
            except Exception:
                time.sleep(0.1)

    def __call__(self, cmd, **a):
        return self.c.request(cmd, a)

    def error(self, cmd, **a):
        try:
            self.c.request(cmd, a)
        except Exception as e:  # noqa: BLE001
            return str(e)
        return None

    def stop(self):
        self.proc.terminate()
        try:
            self.proc.wait(10)
        except subprocess.TimeoutExpired:
            self.proc.kill()


def main():
    exe = os.environ.get("PATINA_BIN") or os.path.join(HERE, "..", "..", "build", "patina")
    tmp = tempfile.mkdtemp(prefix="patina_scene_test_")
    env = dict(os.environ, HOME=os.path.join(tmp, "home"), XDG_DATA_HOME=os.path.join(tmp, "data"),
               XDG_CONFIG_HOME=os.path.join(tmp, "config"), XDG_CACHE_HOME=os.path.join(tmp, "cache"), PATINA_EXE=exe)
    os.makedirs(env["HOME"], exist_ok=True)
    model = os.path.join(tmp, "character.obj")
    character(model)
    try:
        p = Patina(exe, env)
        try:
            # ---- a file with several meshes: an object each
            r = p("open_mesh", path=model, resolution=2048)
            objs = r.get("objects", [])
            check([o["name"] for o in objs] == ["Body", "Hat", "Eye"], "the file's meshes are objects (%s)" % [o["name"] for o in objs])
            res = {o["name"]: o["resolution"] for o in objs}
            check(res.get("Body") == 2048 and res.get("Hat") == 1024 and res.get("Eye") == 512,
                  "texture sets matched to the body's texel density (%s)" % res)
            one = p("open_mesh", path=model, resolution=512, separate=False)
            check("objects" not in one and one["faces"] == 512 + 512 + 128, "or as one object (%s faces)" % one["faces"])
            p("open_mesh", path=model, resolution=512)
            # ---- each object its own layers
            p("select_object", object="Hat")
            p("fill", mode="mesh", material={"id": "", "color": [0.1, 0.2, 0.95], "roughness": 0.5})
            painted = lambda: [l for l in p("list_layers") if l.get("painted_channels")]
            hat_painted = len(painted())
            p("select_object", object="Body")
            check(hat_painted == 1 and not painted(), "the fill went into the hat's layers only (%d / %d)" % (hat_painted, len(painted())))
            st = p("get_state")
            check(st["active_object"] == 0 and len(st["objects"]) == 3, "get_state lists the scene")
            # ---- moved, turned and scaled: the brush still lands where it aims (paint_world_points: the hat's front)
            p("select_object", object="Hat")
            p("add_layer", type="paint")
            moved = p("set_object", object="Hat", location=[0.4, 0.2, 0.0], rotation=[0, 0, 30], scale=[1.5, 1.0, 1.0])
            check(abs(moved["location"][0] - 0.4) < 1e-6 and moved["rotation"][2] == 30, "set_object places it (%s)" % moved["location"])
            p("set_camera", view="front", frame=True)
            p("paint_world_points", points=[[0, 1.25, 0.35], [0.0, 1.27, 0.35]],
              brush={"size": 30, "opacity": 1, "flow": 1, "hardness": 1},
              material={"id": "", "color": [0.95, 0.1, 0.1]})
            out = os.path.join(tmp, "hat")
            files = p("export_textures", directory=out, size=128)["files"]
            col = pixel(files["basecolor"], 0.25, 0.5)
            check(col[0] > 180 and col[2] < 90, "the stroke on the moved hat is at its front (%s)" % (col,))
            near = lambda a, b: all(abs(x - y) < 1e-5 for x, y in zip(a, b))
            p("undo")
            back = p("list_objects")["objects"][1]
            check(near(back["location"], [0.4, 0.2, 0.0]), "the undo takes the stroke back, not the move")
            p("undo")
            check(p("list_objects")["objects"][1]["location"] == [0.0, 0.0, 0.0], "the move is one undo step of the hat's own")
            p("redo")
            # ---- the project keeps the scene
            project = os.path.join(tmp, "scene.patina")
            r = p("save_project", path=project)
            check(r.get("objects") == 3, "saved with its objects")
            p("new_project", primitive="cube")
            check(len(p("list_objects")["objects"]) == 1, "a new project is one object")
            p("open_file", path=project)
            lo = p("list_objects")
            names = [(o["name"], o["layers"], o["location"]) for o in lo["objects"]]
            check([n for n, _, _ in names] == ["Body", "Hat", "Eye"] and near(names[1][2], [0.4, 0.2, 0.0]),
                  "reopened with their names and places (%s)" % names)
            check(lo["active"] == 1, "and the one that was active (%s)" % lo["active"])
            files = p("export_textures", directory=os.path.join(tmp, "hat2"), size=128)["files"]
            col = pixel(files["basecolor"], 0.7, 0.3)
            check(col[2] > 180, "the hat's fill came back (%s)" % (col,))
            # ---- export: one model file for the scene, each object's maps
            glb = os.path.join(tmp, "exp", "scene.glb")
            r = p("export_model", path=glb, texture_size=128)
            check(len(r.get("objects", [])) == 3, "the model export writes every object (%s)" % r.get("objects"))
            g = glb_json(glb)
            nodes = {n["name"]: n for n in g["nodes"]}
            check(set(nodes) == {"Body", "Hat", "Eye"} and len(g["materials"]) == 3, "a node and a material each (%s)" % list(nodes))
            m = nodes.get("Hat", {}).get("matrix")
            # (its middle, turned and scaled about, ends up moved by the location: (0, 1.25, 0) -> (0.4, 1.45, 0))
            mid = [m[4 + k] * 1.25 + m[12 + k] for k in range(3)] if m else None
            check(m is not None and near(mid, [0.4, 1.45, 0.0]) and "matrix" not in nodes["Body"], "the hat's node is where it is (%s)" % mid)
            r = p("export_textures", directory=os.path.join(tmp, "all"), size=64, objects="all")
            check(len(r.get("objects", [])) == 3 and any(k.startswith("Eye/") for k in r["files"]),
                  "the texture export writes each object's maps (%d files)" % len(r["files"]))
            names = sorted(os.listdir(os.path.join(tmp, "all")))
            check(any(n.startswith("Hat_") for n in names) and any(n.startswith("Body_") for n in names), "named after their objects")
            # ---- copies, removals, the Local view, a path traced picture of them all
            r = p("duplicate_object", object="Eye")
            check(r["name"] == "Eye.001" and r["index"] == 3, "a copy right after it (%s)" % r["name"])
            p("set_object", object="Eye.001", location=[-0.64, 0.0, 0.0])
            r = p("remove_object", object="Body")
            check(r["count"] == 3 and p("list_objects")["objects"][0]["name"] == "Hat", "a removal (%s)" % r)
            check(p("isolate_object", on=True)["isolated"], "the Local view")
            p("isolate_object", on=False)
            img = p("render_view", width=96, height=72, mode="path_traced", samples=2)
            check(img.get("width") == 96, "the path tracer traces the scene")
            p("remove_object", object="Hat")
            p("remove_object", object="Eye")
            check(p.error("remove_object", object="Eye.001") is not None, "the last object can't go")
        finally:
            p.stop()

        # ---- a model added to the startup sphere's scene takes its place
        p = Patina(exe, env)
        try:
            r = p("add_object", path=model)
            lo = p("list_objects")
            check([o["name"] for o in lo["objects"]] == ["Body", "Hat", "Eye"], "the sphere Patina starts with gave way (%s)" % [o["name"] for o in lo["objects"]])
            r = p("add_object", path=model, separate=False)
            check(p("list_objects")["objects"][-1]["name"] == "character", "a file added as one object (%s)" % r)
        finally:
            p.stop()

        # ---- Blender: several objects
        p = Patina(exe, env)
        try:
            ex = os.path.join(tmp, "exchange")
            paths = {}
            for name in ("Body", "Shirt"):
                os.makedirs(os.path.join(ex, name), exist_ok=True)
                paths[name] = os.path.join(ex, name, name + ".ptmesh")
                sheet(paths[name], name)
            port = free_port()

            def send(cmd, name, **kw):
                a = {"path": paths[name], "object": name, "blender_port": port, "exchange_dir": ex, "resolution": 256}
                a.update(kw)
                return p(cmd, **a)
            # Blender's matrix_world of Body: moved to (2, 3, 4) (Blender's axes, column by column)
            mw = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 2, 3, 4, 1]
            send("bridge_open_mesh", "Body", scene="auto", matrix=mw)
            send("bridge_open_mesh", "Shirt", scene="add")
            lo = p("list_objects")
            check([o["name"] for o in lo["objects"]] == ["Body", "Shirt"] and lo["active"] == 0,
                  "the first object starts the scene, the second comes in beside it (%s)" % [o["name"] for o in lo["objects"]])
            b = lo["objects"][0]["bounds"]
            mid = [(b["min"][k] + b["max"][k]) / 2 for k in range(3)]
            check(all(abs(mid[k] - (2, 4, -3)[k]) < 1e-4 for k in range(3)), "in Blender's place, Y up (%s)" % mid)
            check(lo["objects"][1].get("blender_object") == "Shirt", "each linked to its Blender object")
            p("add_layer", type="paint")
            send("bridge_update_mesh", "Shirt", scene="auto")
            lo = p("list_objects")
            check(len(lo["objects"]) == 2 and lo["active"] == 0 and lo["objects"][0]["layers"] == 2,
                  "an update goes to its own object, the one painted stays so (%s)" % [(o["name"], o["layers"]) for o in lo["objects"]])
            r = send("bridge_update_mesh", "Body", scene="update", object="Gone")
            check("skipped" in r and len(p("list_objects")["objects"]) == 2, "the sync leaves out an object the scene doesn't have")
            r = p("bridge_export", object="Shirt")
            check(r.get("object") == "Shirt", "a pull answers for the object asked for (%s)" % r.get("object"))
            r = p("send_to_blender")
            check(len(r.get("objects", [])) == 2, "send_to_blender pushes every linked object")
        finally:
            p.stop()
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("scene objects: %s" % ("all checks passed" if not failures else "%d FAILED" % len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
