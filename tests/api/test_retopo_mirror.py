#!/usr/bin/env python3
"""Retopology with Mirror X against a real headless Patina (0.22.3): the quads are made on one side and mirrored,
so every vertex has its mirror image and the halves meet along an edge loop down the middle; a line drawn on one
side only comes out on both; switching Mirror off makes them on the whole model again; a model whose sides
differ is made whole and said so. Flow strokes that make edge loops: an option kept with the mesh, on by default
(0.22.4).

    PATINA_BIN=build/patina python3 tests/api/test_retopo_mirror.py
"""
import math
import os
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from test_retopo_zones import ball, check, failures, free_port, common, BIN  # noqa: E402


def quads(path):
    V, F = [], []
    for ln in open(path):
        if ln.startswith("v "):
            V.append(tuple(float(x) for x in ln.split()[1:4]))
        elif ln.startswith("f "):
            F.append([int(t.split("/")[0]) - 1 for t in ln.split()[1:]])
    return V, F


def mirror_gap(V):
    # (each vertex's mirror image: how far from the nearest vertex, at worst; a grid of cells for speed)
    cell = 0.02
    grid = {}
    for i, p in enumerate(V):
        grid.setdefault(tuple(int(math.floor(x / cell)) for x in p), []).append(i)
    worst = 0.0
    for p in V:
        q = (-p[0], p[1], p[2])
        k = tuple(int(math.floor(x / cell)) for x in q)
        best = 1e9
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                for dz in (-1, 0, 1):
                    for j in grid.get((k[0] + dx, k[1] + dy, k[2] + dz), []):
                        best = min(best, math.dist(q, V[j]))
        worst = max(worst, best)
    return worst


def open_edges(F):
    uses = {}
    for f in F:
        for i in range(len(f)):
            e = tuple(sorted((f[i], f[(i + 1) % len(f)])))
            uses[e] = uses.get(e, 0) + 1
    return sum(1 for n in uses.values() if n == 1)


def main():
    tmp = tempfile.mkdtemp(prefix="patina_retopo_mirror_")
    env = dict(os.environ, HOME=tmp, XDG_DATA_HOME=tmp + "/data", XDG_CONFIG_HOME=tmp + "/config", XDG_CACHE_HOME=tmp + "/cache")
    model = os.path.join(tmp, "ball.obj")
    ball(model)
    lop = os.path.join(tmp, "lopsided.obj")
    ball(lop)
    with open(lop) as f:
        lines = f.readlines()
    with open(lop, "w") as f:
        for ln in lines:
            if ln.startswith("v "):
                x, y, z = (float(t) for t in ln.split()[1:4])
                ln = "v %f %f %f\n" % (x * 1.4 if x > 0 else x, y, z)
            f.write(ln)
    port = free_port()

    def start():
        p = subprocess.Popen([BIN, "--headless", "--port", str(port)], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(150):
            try:
                c = common.Client(port, timeout=300)
                c.call("ping")
                return p, c
            except Exception:
                time.sleep(0.2)
        p.kill()
        raise SystemExit("patina did not start")

    def stop(p):
        p.terminate()
        try:
            p.wait(20)
        except Exception:
            p.kill()

    p, c = start()
    try:
        c.call("open_file", {"path": model})
        c.call("retopo_density", {"quads_per_m2": 400})
        # a loop on the right side only
        ring = []
        for k in range(48):
            a = 2 * math.pi * k / 48
            v = (1.0, 0.45 * math.cos(a), 0.45 * math.sin(a))
            n = math.sqrt(sum(x * x for x in v))
            ring.append([0.5 * x / n for x in v])
        c.call("retopo_zone_line", {"points": ring, "loop": True})
        c.call("set_symmetry", {"x": True})
        r = c.call("retopo_build")
        out = os.path.join(tmp, "on.obj")
        c.call("retopo_export", {"path": out})
        V, F = quads(out)
        gap = mirror_gap(V)
        L = 1 / math.sqrt(400)
        check(gap < 0.03 * L, "Mirror X: every vertex has its mirror image (within %.4f)" % gap)
        check(open_edges(F) == 0, "the halves meet: no open edges (%d)" % open_edges(F))
        middle = sum(1 for v in V if v[0] == 0.0)
        check(middle >= 0.7 * 2 * math.pi * 0.5 / L, "an edge loop down the middle (%d vertices on it)" % middle)
        right = sum(1 for v in V if min(math.dist(v, q) for q in ring) < 0.2 * L)
        left = sum(1 for v in V if min(math.dist(v, (-q[0], q[1], q[2])) for q in ring) < 0.2 * L)
        check(left >= 10 and left == right, "the loop drawn on the right comes out on the left too (%d / %d vertices along it and its image)" % (right, left))
        check(not any("Mirror" in x for x in r.get("problems", [])), "nothing said about it")
        # Mirror off: made on the whole model (not mirrored)
        c.call("set_symmetry", {"x": False})
        c.call("retopo_build")
        c.call("retopo_export", {"path": out})
        V2, _ = quads(out)
        check(mirror_gap(V2) > 0.05 * L, "Mirror off: made on the whole model (not mirrored)")
        # Strokes make edge loops: on by default (0.22.4), kept with the mesh
        check(c.call("retopo_flow_loops", {})["on"] is True, "Strokes make edge loops: on by default")
        check(c.call("retopo_flow_loops", {"on": False})["on"] is False, "switched off")
        c.call("save_project", {"path": os.path.join(tmp, "t.patina")})
        time.sleep(3.5)  # (the retopology is kept a few seconds after a change)
        c.call("retopo_info")
    finally:
        stop(p)
    p, c = start()
    try:
        c.call("open_file", {"path": model})
        check(c.call("retopo_flow_loops", {})["on"] is False, "Strokes make edge loops: still off when the mesh is opened again")
        # a lopsided ball: made on the whole model, and said so
        c.call("open_file", {"path": lop})
        c.call("retopo_density", {"quads_per_m2": 400})
        c.call("set_symmetry", {"x": True})
        r = c.call("retopo_build")
        said = [x for x in r.get("problems", []) if x.startswith("Mirror")]
        check(len(said) == 1 and "isn't the same on both sides" in said[0], "a lopsided model: made whole, and said so (%s)" % said)
        check(r["quads"] > 0, "it still gets its quads (%d)" % r["quads"])
    finally:
        stop(p)
        shutil.rmtree(tmp, ignore_errors=True)
    print("retopology mirror: %d failed" % len(failures) if failures else "retopology mirror: all checks passed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
