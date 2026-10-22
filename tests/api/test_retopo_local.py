#!/usr/bin/env python3
"""Retopology: a change remakes only the quads near it (0.23), against a real headless Patina. A loop drawn after
the quads were made leaves every quad away from it exactly as it was; retopo_build with fresh makes them all
again; with Keep quads away from changes off, every build makes them all again, and the setting is kept with
the mesh.

    PATINA_BIN=build/patina python3 tests/api/test_retopo_local.py
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
from test_retopo_mirror import quads  # noqa: E402


def ring(center, ang, r=0.5, n=64):
    c = [x / math.sqrt(sum(y * y for y in center)) for x in center]
    up = (0.0, 1.0, 0.0) if abs(c[1]) < 0.9 else (1.0, 0.0, 0.0)
    u = (c[1] * up[2] - c[2] * up[1], c[2] * up[0] - c[0] * up[2], c[0] * up[1] - c[1] * up[0])
    un = math.sqrt(sum(x * x for x in u))
    u = [x / un for x in u]
    v = (u[1] * c[2] - u[2] * c[1], u[2] * c[0] - u[0] * c[2], u[0] * c[1] - u[1] * c[0])
    pts = []
    for k in range(n):
        a = 2 * math.pi * k / n
        p = [math.cos(ang) * c[i] + math.sin(ang) * (math.cos(a) * u[i] + math.sin(a) * v[i]) for i in range(3)]
        pn = math.sqrt(sum(x * x for x in p))
        pts.append([r * x / pn for x in p])
    return pts


def main():
    tmp = tempfile.mkdtemp(prefix="patina_retopo_local_")
    env = dict(os.environ, HOME=tmp, XDG_DATA_HOME=tmp + "/data", XDG_CONFIG_HOME=tmp + "/config", XDG_CACHE_HOME=tmp + "/cache")
    model = os.path.join(tmp, "ball.obj")
    ball(model)
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

    L = 1 / math.sqrt(400)
    a, b = ring((1, 0.2, 0.3), 0.5), ring((-1, -0.1, 0.4), 0.45)
    out = os.path.join(tmp, "q.obj")
    p, c = start()
    try:
        c.call("open_file", {"path": model})
        c.call("retopo_density", {"quads_per_m2": 400})
        check(c.call("retopo_keep_away", {})["on"] is True, "Keep quads away from changes: on by default")
        c.call("retopo_zone_line", {"points": a, "loop": True})
        r1 = c.call("retopo_build")
        check(r1["kept"] == -1, "the first quads: all made (%d)" % r1["kept"])
        c.call("retopo_export", {"path": out})
        V1, _ = quads(out)
        c.call("retopo_zone_line", {"points": b, "loop": True})
        r2 = c.call("retopo_build")
        c.call("retopo_export", {"path": out})
        V2, F2 = quads(out)
        check(r2["kept"] > 0, "a second loop: only the quads near it made again (%d faces kept)" % r2["kept"])
        was = set(V1)
        far = [v for v in V2 if min(math.dist(v, q) for q in b) > 6 * L]
        moved = sum(1 for v in far if v not in was)
        check(len(far) > 100 and moved == 0, "every vertex further than six quads from it where it was (%d of %d moved)" % (moved, len(far)))
        uses = {}
        for f in F2:
            for i in range(len(f)):
                e = tuple(sorted((f[i], f[(i + 1) % len(f)])))
                uses[e] = uses.get(e, 0) + 1
        check(all(n == 2 for n in uses.values()), "the quads are closed: every edge on two faces")
        info = c.call("retopo_info")
        check(info["result"]["kept"] == r2["kept"], "retopo_info says how many were kept")
        r3 = c.call("retopo_build", {"fresh": True})
        check(r3["kept"] == -1, "fresh: all made again")
        # off: all made again after a change; kept with the mesh
        check(c.call("retopo_keep_away", {"on": False})["on"] is False, "switched off")
        c.call("retopo_zone_line", {"points": ring((0, 1, 0.2), 0.4), "loop": True})
        r4 = c.call("retopo_build")
        check(r4["kept"] == -1, "off: a change makes all the quads again")
        c.call("save_project", {"path": os.path.join(tmp, "t.patina")})
        time.sleep(3.5)  # (the retopology is kept a few seconds after a change)
        c.call("retopo_info")
    finally:
        stop(p)
    p, c = start()
    try:
        c.call("open_file", {"path": model})
        check(c.call("retopo_keep_away", {})["on"] is False, "still off when the mesh is opened again")
    finally:
        stop(p)
        shutil.rmtree(tmp, ignore_errors=True)
    print("retopology local: %d failed" % len(failures) if failures else "retopology local: all checks passed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
