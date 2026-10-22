#!/usr/bin/env python3
"""Retopology zones against a real headless Patina (0.19.6): a zone line drawn inside a zone with a
resolution of its own splits it into zones that keep that resolution (eyes drawn on a head at x8 are x8
too), one undo step takes both the line and that back. Fewer poles (0.21) is on by default and kept with the mesh.

    PATINA_BIN=build/patina python3 tests/api/test_retopo_zones.py
"""
import math
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tools", "assistant"))
import common  # noqa: E402

BIN = os.environ.get("PATINA_BIN", os.path.join(ROOT, "build", "patina"))
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


def ball(path, seg=64, rows=32, r=0.5):
    v = [(0.0, r, 0.0)]
    for i in range(1, rows):
        th = math.pi * i / rows
        for k in range(seg):
            ph = 2 * math.pi * k / seg
            v.append((r * math.sin(th) * math.cos(ph), r * math.cos(th), r * math.sin(th) * math.sin(ph)))
    v.append((0.0, -r, 0.0))
    idx = lambda i, k: 2 + (i - 1) * seg + (k % seg)
    f = [(1, idx(1, k + 1), idx(1, k)) for k in range(seg)]
    for i in range(1, rows - 1):
        for k in range(seg):
            f.append((idx(i, k), idx(i, k + 1), idx(i + 1, k + 1), idx(i + 1, k)))
    f += [(idx(rows - 1, k), idx(rows - 1, k + 1), len(v)) for k in range(seg)]
    with open(path, "w") as o:
        for p in v:
            o.write("v %f %f %f\n" % p)
        for q in f:
            o.write("f " + " ".join(str(i) for i in reversed(q)) + "\n")


def main():
    tmp = tempfile.mkdtemp(prefix="patina_retopo_zones_")
    env = dict(os.environ, HOME=tmp, XDG_DATA_HOME=tmp + "/data", XDG_CONFIG_HOME=tmp + "/config", XDG_CACHE_HOME=tmp + "/cache")
    model = os.path.join(tmp, "ball.obj")
    ball(model)
    port = free_port()
    p = subprocess.Popen([BIN, "--headless", "--port", str(port)], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        c = None
        for _ in range(150):
            try:
                c = common.Client(port, timeout=120)
                c.call("ping")
                break
            except Exception:
                time.sleep(0.2)
        c.call("open_file", {"path": model})
        c.call("retopo_density", {"quads_per_m2": 400})
        c.call("retopo_zone", {"point": [0, 0, 0.5], "resolution": 4})
        z = c.call("retopo_zones")["zones"]
        check(len(z) == 1 and z[0]["resolution"] == 4, "with no lines the whole ball is one zone, set to x4")
        y0 = 0.2
        r0 = math.sqrt(0.25 - y0 * y0)
        ring = [[r0 * math.cos(2 * math.pi * k / 48), y0, r0 * math.sin(2 * math.pi * k / 48)] for k in range(48)]
        c.call("retopo_zone_line", {"points": ring, "loop": True})
        z = c.call("retopo_zones")["zones"]
        check(len(z) == 2 and all(x["resolution"] == 4 for x in z),
              "a line round it splits it in two, and both keep x4 (%s)" % [x["resolution"] for x in z])
        c.call("undo")
        z = c.call("retopo_zones")["zones"]
        check(len(z) == 1 and z[0]["resolution"] == 4, "undo takes the line back: one zone at x4 again")
        c.call("redo")
        z = c.call("retopo_zones")["zones"]
        check(len(z) == 2 and all(x["resolution"] == 4 for x in z), "redo brings both back at x4")
        # a zone set apart keeps its own setting when another line splits the rest
        c.call("retopo_zone", {"point": [0, 0.5, 0], "resolution": 2})
        ring2 = [[r0 * math.cos(2 * math.pi * k / 48), -y0, r0 * math.sin(2 * math.pi * k / 48)] for k in range(48)]
        c.call("retopo_zone_line", {"points": ring2, "loop": True})
        res = sorted(x["resolution"] for x in c.call("retopo_zones")["zones"])
        check(res == [2, 4, 4], "the cap set to x2 keeps it; the rest, split again, keeps x4 (%s)" % res)
        # (0.21) Fewer poles: on by default, kept with the mesh's lines when turned off
        check(c.call("retopo_fewer_poles", {})["on"] is True, "Fewer poles is on by default")
        check(c.call("retopo_fewer_poles", {"on": False})["on"] is False, "Fewer poles turns off")
        r = c.call("retopo_build")
        check(r["quads"] > 0, "quads made with it off (%d)" % r["quads"])
        c.call("open_file", {"path": model})
        check(c.call("retopo_fewer_poles", {})["on"] is False, "off still, after the model is opened again")
        c.call("retopo_fewer_poles", {"on": True})
        r = c.call("retopo_build")
        check(r["quads"] > 0, "quads made with it on (%d)" % r["quads"])
    finally:
        p.terminate()
        try:
            p.wait(20)
        except Exception:
            p.kill()
        shutil.rmtree(tmp, ignore_errors=True)
    print("retopology zones: %d failed" % len(failures) if failures else "retopology zones: all checks passed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
