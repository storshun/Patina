#!/usr/bin/env python3
"""Retopology lines against a real headless Patina (0.22): lines behind the model are shown, darkened or
hidden; lines can be confirmed (darker, kept with the mesh, one undo step, the zones unchanged); snapping
of new lines' ends can be turned off; (0.22.1) Shift+click's line where there is no part to go round is the
cut facing the camera.

    PATINA_BIN=build/patina python3 tests/api/test_retopo_lines.py
"""
import base64
import io
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


def pixels(c):
    from PIL import Image
    r = c.call("render_view", {"width": 400, "height": 400, "camera": {"view": "front", "frame": True}})
    return Image.open(io.BytesIO(base64.b64decode(r["image_png_base64"]))).convert("RGB")


def rgb(im):
    b = im.tobytes()
    return list(zip(b[0::3], b[1::3], b[2::3]))


def bright_yellow(im):
    return sum(1 for (r, g, b) in rgb(im) if r > 200 and g > 150 and b < 90)


def differ(a, b):
    return sum(1 for p, q in zip(rgb(a), rgb(b)) if sum(abs(x - y) for x, y in zip(p, q)) > 30)


def main():
    tmp = tempfile.mkdtemp(prefix="patina_retopo_lines_")
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
        asked = c.call("retopo_info")["quads_asked"]
        check(abs(asked - 400 * math.pi) < 0.05 * 400 * math.pi, "about 1257 quads asked for over the ball (%s)" % asked)
        # a small ring on the near side (+z, toward the front view) and a wide one on the far side
        def ring(z, n=64):
            r = math.sqrt(0.25 - z * z)
            return [[r * math.cos(2 * math.pi * k / n), r * math.sin(2 * math.pi * k / n), z] for k in range(n)]
        c.call("retopo_zone_line", {"points": ring(0.45), "loop": True})
        c.call("retopo_zone_line", {"points": ring(-0.3), "loop": True})
        c.call("retopo_view", {"show": "none", "quads": True})
        ims = {}
        for mode in ("shown", "darkened", "hidden"):
            check(c.call("retopo_view", {"lines_behind": mode})["lines_behind"] == mode, "lines behind the model: %s" % mode)
            ims[mode] = pixels(c)
        ys = {m: bright_yellow(ims[m]) for m in ims}
        print("  bright line pixels:", ys)
        check(ys["shown"] > 1.8 * ys["hidden"] and ys["hidden"] > 50, "shown: the far ring shows bright through the ball; hidden: only the near one")
        check(abs(ys["darkened"] - ys["hidden"]) < 0.1 * ys["hidden"] + 10, "darkened: no more bright pixels than hidden")
        d = differ(ims["darkened"], ims["hidden"])
        check(d > 0.3 * (ys["shown"] - ys["hidden"]), "darkened: the far ring is still there, dim (%d pixels differ from hidden)" % d)
        # confirming: darker, zones unchanged, one undo step, kept with the mesh
        zones = len(c.call("retopo_zones")["zones"])
        r = c.call("retopo_confirm_lines", {"point": ring(0.45)[0]})
        check(r["confirmed"] == 1, "the line nearest a point confirmed (%s)" % r)
        info = c.call("retopo_info", {"line_points": True})
        flags = [l["confirmed"] for l in info["line_points"]]
        check(flags == [True, False], "only the near ring is confirmed (%s)" % flags)
        check(len(c.call("retopo_zones")["zones"]) == zones, "the zones don't change")
        c.call("retopo_view", {"lines_behind": "hidden"})
        im = pixels(c)
        check(bright_yellow(im) < 0.2 * ys["hidden"], "a confirmed line is drawn darker (%d bright pixels, were %d)" % (bright_yellow(im), ys["hidden"]))
        c.call("undo")
        flags = [l["confirmed"] for l in c.call("retopo_info", {"line_points": True})["line_points"]]
        check(flags == [False, False], "undo takes the confirming back (%s)" % flags)
        c.call("redo")
        check(c.call("retopo_confirm_lines", {"confirmed": True})["confirmed"] == 2, "all lines confirmed")
        c.call("open_file", {"path": model})
        flags = [l["confirmed"] for l in c.call("retopo_info", {"line_points": True})["line_points"]]
        check(flags == [True, True], "still confirmed after the model is opened again (%s)" % flags)
        check(c.call("retopo_confirm_lines", {"confirmed": False, "line": 1})["confirmed"] == 1, "one line unconfirmed by its index")
        # snapping
        check(c.call("retopo_snap", {})["on"] is True, "line ends snap by default")
        check(c.call("retopo_snap", {"on": False})["on"] is False, "snapping turns off")
        check(c.call("retopo_snap", {"on": True})["on"] is True, "and back on")
        # (0.22.1) Shift+click's line on a ball: no part to go round, so the cut facing the camera
        c.call("set_camera", {"view": "front", "frame": True})
        n0 = c.call("retopo_info")["lines"]
        r = c.call("retopo_ring_line", {"point": [0.3, 0.1, 0.39]})
        check(r.get("made") == "facing", "on a ball, auto makes the cut facing the camera (%s)" % r.get("made"))
        pts = c.call("retopo_info", {"line_points": True})["line_points"][-1]["points"]
        zs = [q[2] for q in pts]
        rr = math.sqrt(0.25 - 0.39 ** 2)
        check(max(zs) - min(zs) < 0.002 and abs(sum(zs) / len(zs) - 0.39) < 0.01, "it lies in the plane through the point, facing the camera (z %.4f to %.4f)" % (min(zs), max(zs)))
        check(abs(r["length"] - 2 * math.pi * rr) < 0.05 * 2 * math.pi * rr, "a circle round the ball there (%.3f long, %.3f asked)" % (r["length"], 2 * math.pi * rr))
        try:
            c.call("retopo_ring_line", {"point": [0.3, 0.1, 0.39], "mode": "around"})
            check(False, "round the part only: refused on a ball")
        except Exception:
            check(c.call("retopo_info")["lines"] == n0 + 1, "round the part only: refused on a ball (no tube)")
        r = c.call("retopo_ring_line", {"point": [0.0, 0.45, 0.2], "mode": "facing", "direction": [0, -1, 0]})
        pts = c.call("retopo_info", {"line_points": True})["line_points"][-1]["points"]
        ys = [q[1] for q in pts]
        check(max(ys) - min(ys) < 0.002, "a direction of its own: the cut faces that way (y %.4f to %.4f)" % (min(ys), max(ys)))
    finally:
        p.terminate()
        try:
            p.wait(20)
        except Exception:
            p.kill()
        shutil.rmtree(tmp, ignore_errors=True)
    print("retopology lines: %d failed" % len(failures) if failures else "retopology lines: all checks passed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
