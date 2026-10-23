#!/usr/bin/env python3
"""(1.2.5) A UV-projected brush in the 3D view, in the real window: it paints after the camera moved. Its dabs test
against the view's depth (only what the camera sees takes paint), but that depth was drawn only for Screen brushes
and fills through the view. So a UV brush tested against an older view (or none), and strokes painted nothing: no
undo step, "Nothing to paint", until a fill drew the depth again.

    python3 tests/gui/test_uv_brush_view.py [path/to/patina]
"""
import base64
import io
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import manual_shots  # noqa: E402
from manual_scenes import layout, vp  # noqa: E402
from PIL import Image, ImageChops, ImageStat  # noqa: E402

BIN = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "patina")
failures = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        failures.append(what)


def main():
    s = manual_shots.Session(BIN, os.path.join("/tmp", "patina_uv_brush_test"), display=":94", port=7897)

    def shot():
        r = s("render_view", width=200, height=200, mode="lit",
              camera={"view": "right", "target": [0, 0, 0], "distance": 3.5})
        return Image.open(io.BytesIO(base64.b64decode(r["image_png_base64"]))).convert("RGB")

    def changed(a, b):
        d = ImageChops.difference(a, b).convert("L").crop((40, 40, 160, 160)).point(lambda v: 255 if v > 40 else 0)
        return ImageStat.Stat(d).mean[0] / 255.0

    def steps():
        return len(s("history")["steps"])

    try:
        s("new_project", primitive="sphere", resolution=512)
        s("set_view", seams=False, wireframe=False)
        s("set_paint_material", color=[1, 0, 0])
        s("set_brush", projection="screen", occlusion=True, size=30, opacity=1)
        layout(s)
        # a Screen stroke up close from the front: the brush's depth is drawn for that view
        s("set_camera", view="front", target=[0, 0, 0], distance=1.8)
        s.settle(3)
        s.x.drag([vp(0.3 + 0.4 * i / 20, 0.8) for i in range(21)], dt=0.03)
        s.settle(3)
        # from the side, farther back, a UV brush: every stroke paints
        s("set_brush", projection="uv")
        s("set_camera", view="right", target=[0, 0, 0], distance=6.0)
        s.settle(3)
        n0, before = steps(), shot()
        for k in range(3):
            s.x.drag([vp(0.44 + 0.12 * i / 20, 0.45 + 0.05 * k) for i in range(21)], dt=0.03)
            s.settle(3)
        share = changed(before, shot())
        check(steps() == n0 + 3, "three UV strokes after the camera moved: three undo steps (%d)" % (steps() - n0))
        check(share > 0.03, "and they paint where they were drawn (%.1f%% of the middle)" % (share * 100))
    finally:
        s.close()
    print("uv brush view: %s" % ("all checks passed" if not failures else "%d FAILED" % len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
