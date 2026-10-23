#!/usr/bin/env python3
"""(1.2.5) Symmetry with brushes stamped from the 3D view, against a real headless Patina: with X symmetry, a Screen
or UV brush paints the mirrored side the camera can't see. Mirror copies went through the screen, so a copy facing
away or off screen was dropped (or landed on whatever the view showed there). Now they go on the surface.
(The UV brush's own view test is in tests/gui/test_uv_brush_view.py: it needs the window's strokes.)

    PATINA_BIN=build/patina python3 tests/api/test_symmetry_unseen.py
"""
import base64
import io
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "blender", "patina_bridge"))
import protocol  # noqa: E402
from PIL import Image, ImageChops, ImageStat  # noqa: E402

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


def main():
    exe = os.path.abspath(os.environ.get("PATINA_BIN") or os.path.join(HERE, "..", "..", "build", "patina"))
    if not os.path.exists(exe):
        print("SKIP: patina binary not found (set PATINA_BIN)")
        return 0
    tmp = tempfile.mkdtemp(prefix="patina_symmetry_test_")
    env = dict(os.environ, HOME=os.path.join(tmp, "home"), XDG_DATA_HOME=os.path.join(tmp, "data"),
               XDG_CONFIG_HOME=os.path.join(tmp, "config"), XDG_CACHE_HOME=os.path.join(tmp, "cache"))
    os.makedirs(env["HOME"], exist_ok=True)
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

        def shot(view):
            r = c.request("render_view", {"width": 200, "height": 200, "mode": "lit",
                                          "camera": {"view": view, "target": [0, 0, 0], "distance": 3.5}})
            return Image.open(io.BytesIO(base64.b64decode(r["image_png_base64"]))).convert("RGB")

        def changed(a, b):
            """the share of the middle of the picture that changed"""
            d = ImageChops.difference(a, b).convert("L").crop((50, 50, 150, 150)).point(lambda v: 255 if v > 40 else 0)
            return ImageStat.Stat(d).mean[0] / 255.0

        def stroke(proj, y=0.5, size=40):
            return c.request("paint_stroke", {"points": [[0.4 + i / 100, y] for i in range(21)],
                                              "brush": {"size": size, "opacity": 1, "projection": proj, "occlusion": True},
                                              "material": {"color": [1, 0, 0]}, "tool": "paint"})

        # X symmetry, painting the +X side from the right: the -X side, out of view, gets its copy
        c.request("set_symmetry", {"x": True})
        for proj in ("screen", "uv"):
            c.request("new_project", {"primitive": "sphere", "resolution": 512})
            c.request("set_view", {"seams": False, "wireframe": False})
            c.request("set_camera", {"view": "right", "target": [0, 0, 0], "distance": 3.5})
            far0, near0 = shot("left"), shot("right")
            stroke(proj)
            near, far = changed(near0, shot("right")), changed(far0, shot("left"))
            check(near > 0.02 and far > 0.5 * near,
                  "%s brush, X symmetry: the side out of view gets the mirrored stroke (%.1f%% there, %.1f%% here)"
                  % (proj, far * 100, near * 100))
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
        shutil.rmtree(tmp, ignore_errors=True)
    print("symmetry unseen: %s" % ("all checks passed" if not failures else "%d FAILED" % len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
