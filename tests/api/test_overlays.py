#!/usr/bin/env python3
"""The wireframe and seams overlays (0.40.1) against a real headless Patina: lines on the far side stay hidden. A
sphere with a second, denser sphere 3% smaller inside it (a body under a layer of cloth, say) shows as many wireframe
and seam pixels as the outer sphere alone: framed, close up and orthographic. Up to 0.40.0 the lines were pulled toward
the camera by a share of the depth range, a third of the model and more, so the inner sphere's lines showed through.

    PATINA_BIN=build/patina python3 tests/api/test_overlays.py
"""
import base64
import math
import os
import shutil
import socket
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


def sphere_obj(spheres):
    """UV spheres (radius, columns, rows, UV islands side by side): the islands meet at seams, the columns wrap."""
    v, vt, f = [], [], []
    for r, nu, nv, islands in spheres:
        base = len(v)
        for j in range(nv + 1):
            th = math.pi * j / nv
            for i in range(nu):
                ph = 2 * math.pi * i / nu
                v.append((r * math.sin(th) * math.cos(ph), r * math.cos(th), r * math.sin(th) * math.sin(ph)))
        per = nu // islands
        for q in range(islands):
            tb = len(vt)
            for j in range(nv + 1):
                for k in range(per + 1):
                    vt.append(((q + 0.95 * k / per) / islands, 1 - j / nv))
            for j in range(nv):
                for k in range(per):
                    i = q * per + k
                    a, b = base + j * nu + i, base + j * nu + (i + 1) % nu
                    c, d = b + nu, a + nu
                    ta = tb + j * (per + 1) + k
                    f.append(((a, ta), (b, ta + 1), (c, ta + per + 2), (d, ta + per + 1)))
    out = ["v %.6f %.6f %.6f" % p for p in v] + ["vt %.6f %.6f" % t for t in vt]
    out += ["f " + " ".join("%d/%d" % (p + 1, t + 1) for p, t in face) for face in f]
    return "\n".join(out) + "\n"


def main():
    exe = os.path.abspath(os.environ.get("PATINA_BIN") or os.path.join(HERE, "..", "..", "build", "patina"))
    if not os.path.exists(exe):
        print("SKIP: patina binary not found (set PATINA_BIN)")
        return 0
    tmp = tempfile.mkdtemp(prefix="patina_overlays_test_")
    env = dict(os.environ, HOME=os.path.join(tmp, "home"), XDG_DATA_HOME=os.path.join(tmp, "data"),
               XDG_CONFIG_HOME=os.path.join(tmp, "config"), XDG_CACHE_HOME=os.path.join(tmp, "cache"), PATINA_EXE=exe)
    os.makedirs(env["HOME"], exist_ok=True)
    outer = (1.0, 24, 12, 1)
    inner = (0.97, 40, 20, 4)  # (denser, and with four seams)
    models = {"outer": os.path.join(tmp, "outer.obj"), "layered": os.path.join(tmp, "layered.obj")}
    open(models["outer"], "w").write(sphere_obj([outer]))
    open(models["layered"], "w").write(sphere_obj([outer, inner]))
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
        W, H = 400, 320

        def shot(cam):
            png = base64.b64decode(c.request("render_view", {"width": W, "height": H, "camera": cam})["image_png_base64"])
            w, h, n, rows = read_png(png)
            return [(rows[y][x * n], rows[y][x * n + 1], rows[y][x * n + 2]) for y in range(h) for x in range(w)]

        def lines(cam):
            c.request("set_view", {"wireframe": False, "seams": False})
            bare = shot(cam)
            c.request("set_view", {"wireframe": True})
            wire = shot(cam)
            c.request("set_view", {"wireframe": False, "seams": True})
            seam = shot(cam)
            nw = sum(1 for a, b in zip(bare, wire) if abs(a[0] - b[0]) + abs(a[1] - b[1]) + abs(a[2] - b[2]) > 60)
            ns = sum(1 for p in seam if p[0] > 170 and p[1] < 100 and p[2] < 100)
            return nw, ns

        views = {"framed": {"view": "persp", "frame": True, "ortho": False},
                 "close": {"view": "persp", "frame": True, "ortho": False, "distance": 1.6},
                 "orthographic": {"view": "front", "frame": True, "ortho": True},
                 "from below": {"yaw": 2.4, "pitch": -0.5, "frame": True, "ortho": False}}
        got = {}
        for name, path in models.items():
            r = c.request("open_mesh", {"path": path})
            c.request("uv_seams", {"mode": "from_islands"})
            c.request("set_view", {"mode": "material"})
            check(r["faces"] == (24 * 12 if name == "outer" else 24 * 12 + 40 * 20), "%s: %d faces" % (name, r["faces"]))
            for v, cam in views.items():
                got[name, v] = lines(cam)
        for v in views:
            (ow, os_), (lw, ls) = got["outer", v], got["layered", v]
            check(ow > 300, "%s: the outer sphere's wireframe shows (%d pixels)" % (v, ow))
            check(lw <= ow * 1.06 + 30, "%s: none of the inner sphere's wireframe shows through (%d pixels, %d alone)" % (v, lw, ow))
            check(ls <= os_ * 1.1 + 15, "%s: nor its seams (%d red pixels, %d alone)" % (v, ls, os_))
        check(any(got["outer", v][1] > 20 for v in views), "the outer sphere's seam shows from some side (%s)" %
              [got["outer", v][1] for v in views])
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
        shutil.rmtree(tmp, ignore_errors=True)
    print("overlays: %s" % ("all checks passed" if not failures else "%d FAILED" % len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
