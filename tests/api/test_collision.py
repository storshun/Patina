#!/usr/bin/env python3
"""Collision shapes (0.34) against a real headless Patina: each mode makes closed convex shapes; pieces fit an L
closely; the shapes show in green over the model; export_model writes them after the model with the names Unreal
(UCX_ / UBX_), Godot (-convcolonly) or plain names, in glTF and OBJ; changing the mesh drops them (export then makes
new ones); the Game asset recipe exports them.

    PATINA_BIN=build/patina python3 tests/api/test_collision.py
"""
import base64
import json
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
sys.path.insert(0, HERE)
import protocol  # noqa: E402
from test_model_textures import read_png  # noqa: E402

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


def box_obj(boxes):
    v, f = [], []
    for lo, hi in boxes:
        b = len(v)
        for i in range(8):
            v.append((hi[0] if i & 1 else lo[0], hi[1] if i & 2 else lo[1], hi[2] if i & 4 else lo[2]))
        for q in ((0, 2, 3, 1), (4, 5, 7, 6), (0, 1, 5, 4), (2, 6, 7, 3), (0, 4, 6, 2), (1, 3, 7, 5)):
            f.append(tuple(b + i + 1 for i in q))
    return "".join("v %g %g %g\n" % p for p in v) + "".join("f %d %d %d %d\n" % q for q in f)


def glb_json(path):
    b = open(path, "rb").read()
    n = struct.unpack("<I", b[12:16])[0]
    return json.loads(b[20:20 + n])


def main():
    exe = os.path.abspath(os.environ.get("PATINA_BIN") or os.path.join(HERE, "..", "..", "build", "patina"))
    if not os.path.exists(exe):
        print("SKIP: patina binary not found (set PATINA_BIN)")
        return 0
    tmp = tempfile.mkdtemp(prefix="patina_collision_test_")
    env = dict(os.environ, HOME=os.path.join(tmp, "home"), XDG_DATA_HOME=os.path.join(tmp, "data"),
               XDG_CONFIG_HOME=os.path.join(tmp, "config"), XDG_CACHE_HOME=os.path.join(tmp, "cache"), PATINA_EXE=exe)
    os.makedirs(env["HOME"], exist_ok=True)
    m = os.path.join(tmp, "models")
    os.makedirs(m)
    open(os.path.join(m, "ell.obj"), "w").write(box_obj([((0, 0, 0), (3, 1, 1)), ((0, 1, 0), (1, 3, 1))]))
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
        c.request("open_file", {"path": os.path.join(m, "ell.obj")})
        for mode, want in (("box", 1), ("boxes", 1), ("hull", 1), ("parts", 1), ("pieces", 2)):
            r = c.request("mesh_collision", {"mode": mode})
            check(r["count"] == want and all(s["triangles"] >= 4 for s in r["shapes"]),
                  "%s: %d closed shape(s) (%s)" % (mode, r["count"], [s["vertices"] for s in r["shapes"]]))
        r = c.request("mesh_collision", {"mode": "pieces", "max_vertices": 16})
        check(abs(r["model_volume"] - 5.0) < 1e-3 and 0.98 < r["fit"] < 1.1,
              "pieces fit the L closely (shapes / model %.3f)" % r["fit"])
        check(all(s["vertices"] <= 16 for s in r["shapes"]), "and keep to the corner limit")
        # shown in green over the model
        def greens(show):
            c.request("mesh_collision", {"show": show})
            img = read_png(base64.b64decode(c.request("render_view", {"width": 160, "height": 160, "camera": {"view": "front", "frame": True}})["image_png_base64"]))
            w, h, n, rows = img
            return sum(1 for y in range(h) for x in range(w)
                       if rows[y][x * n + 1] > 200 and rows[y][x * n] < 140 and rows[y][x * n + 2] < 160)
        on, off = greens(True), greens(False)
        check(on > 30 and off == 0, "the shapes show in green over the model (%d green pixels, %d hidden)" % (on, off))
        c.request("mesh_collision", {"show": True})

        # export: glTF names
        for naming, prefix in (("unreal", "UCX_ell_"), ("godot", "ell_col"), ("plain", "ell_collision_")):
            out = os.path.join(m, "out", "ell_%s.glb" % naming)
            r = c.request("export_model", {"path": out, "collision": naming, "texture_size": 256})
            g = glb_json(out)
            names = [nd["name"] for nd in g["nodes"]]
            coll = [nm for nm in names if nm.startswith(prefix)]
            check(len(coll) == 2 and r["collision"]["shapes"] == 2, "%s: the model and its 2 shapes (%s)" % (naming, names))
            if naming == "godot":
                check(all(nm.endswith("-convcolonly") for nm in coll), "Godot names end in -convcolonly")
            meshes = [mm for mm in g["meshes"] if mm["name"] in coll]
            check(all("material" not in p for mm in meshes for p in mm["primitives"]), "the shapes have no material")
        # boxes are UBX_
        c.request("mesh_collision", {"mode": "box"})
        g = glb_json(c.request("export_model", {"path": os.path.join(m, "out", "box.glb"), "collision": "unreal",
                                                "texture_size": 256})["path"])
        check([nd["name"] for nd in g["nodes"]][1:] == ["UBX_ell_00"], "a box is named UBX_ (%s)" % [nd["name"] for nd in g["nodes"]])
        # OBJ: objects of their own after the model
        c.request("mesh_collision", {"mode": "pieces"})
        out = os.path.join(m, "out", "ell.obj")
        c.request("export_model", {"path": out, "collision": "unreal", "texture_size": 256})
        text = open(out).read()
        objs = [l[2:] for l in text.splitlines() if l.startswith("o ")]
        check(objs == ["ell", "UCX_ell_00", "UCX_ell_01"], "OBJ: the model, then UCX_ell_00 and _01 (%s)" % objs)
        nv = sum(1 for l in text.splitlines() if l.startswith("v "))
        top = max(int(t.split("/")[0]) for l in text.splitlines() if l.startswith("f ") for t in l.split()[1:])
        check(top == nv, "and the shapes' faces use their own vertices (%d of %d)" % (top, nv))

        # the mesh changes: the shapes no longer fit and are dropped; export makes new ones that fit
        c.request("mesh_transform", {"scale": 2.0})
        e = c.request("mesh_collision", {"show": True})
        check(e["shapes"] == 0, "after a transform the old shapes are gone (%s)" % e)
        g = glb_json(c.request("export_model", {"path": os.path.join(m, "out", "big.glb"), "collision": "unreal",
                                                "texture_size": 256})["path"])
        top = max(g["accessors"][mm["primitives"][0]["attributes"]["POSITION"]]["max"][0]
                  for mm in g["meshes"] if mm["name"].startswith("UCX_"))
        check(top > 5.5, "export made new shapes for the model as it is (they reach x %.2f)" % top)

        # the Game asset recipe makes and exports them
        outdir = os.path.join(tmp, "batch_out")
        r = c.request("batch_run", {"recipe": "patina_game_asset", "files": [os.path.join(m, "ell.obj")], "output": outdir})
        for _ in range(600):
            st = c.request("batch_status", {})
            if not st.get("running"):
                break
            time.sleep(0.25)
        glb = os.path.join(outdir, "ell.glb")
        names = [nd["name"] for nd in glb_json(glb)["nodes"]] if os.path.isfile(glb) else []
        check(any(nm.startswith("UCX_") for nm in names), "the Game asset recipe exports UCX_ shapes (%s)" % names)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
        shutil.rmtree(tmp, ignore_errors=True)
    print("collision: %s" % ("all checks passed" if not failures else "%d FAILED" % len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
