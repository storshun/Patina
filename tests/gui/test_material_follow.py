#!/usr/bin/env python3
"""The paint material's Rotation and Follow the stroke (1.2.16, smooth turns 1.2.17), in the real window:
- the material ball in the Brush panel turns with the Rotation slider (it stayed as it was);
- Follow the stroke lays a striped material along the stroke: an L-shaped stroke (across, then down) shows its stripes
  turned a quarter turn between the two legs, where without it both legs show them the same way;
- Send to Blender and Auto-Send are buttons on the menu bar (grayed out with no Blender object linked).

    python3 tests/gui/test_material_follow.py [path/to/patina]
"""
import math
import os
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import manual_shots  # noqa: E402
from manual_scenes import panel, rect  # noqa: E402

BIN = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "patina")
OUT = os.environ.get("PATINA_TEST_OUT") or os.path.join("/tmp", "patina_material_follow_test")
failures = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        failures.append(what)


def orientation(im):
    """The stripes' direction in a picture (degrees, 0..180) and how strongly it has one (0..1): the structure tensor."""
    g = im.convert("L")
    w, h = g.size
    px = g.load()
    sxx = syy = sxy = 0.0
    for y in range(1, h - 1):
        for x in range(1, w - 1):
            gx = px[x + 1, y] - px[x - 1, y]
            gy = px[x, y + 1] - px[x, y - 1]
            sxx += gx * gx
            syy += gy * gy
            sxy += gx * gy
    ang = 0.5 * math.degrees(math.atan2(2 * sxy, sxx - syy)) % 180.0  # (the gradient's: across the stripes)
    coh = math.hypot(sxx - syy, 2 * sxy) / max(sxx + syy, 1e-9)
    return ang, coh


def turn(a, b):
    d = abs(a - b) % 180.0
    return min(d, 180.0 - d)


def main():
    s = manual_shots.Session(BIN, OUT, display=":97", port=7909)
    try:
        # ---- the ball turns with the Rotation
        s("new_project", primitive="cube", resolution=1024)
        panel(s, "properties")
        s("set_paint_material", id="patina_breton_stripes", rotation=0, mapping="screen", scale=6)
        s.settle(10, 1.0)
        hdr = rect(s, "props.brush_header")  # (the Paint material further down the panel: scrolled to)
        s.x.move(hdr[0] + 60, hdr[1] + 120)
        for _ in range(12):
            s.x.button(5, True)
            s.x.button(5, False)
        s.settle(10, 1.0)
        ball = rect(s, "props.material_ball")
        b0 = s.image().crop(ball)
        s("set_paint_material", rotation=90)
        s.settle(10, 1.0)
        b90 = s.image().crop(ball)
        a0, c0 = orientation(b0)
        a90, c90 = orientation(b90)
        b0.save(os.path.join(OUT, "ball_0.png"))
        b90.save(os.path.join(OUT, "ball_90.png"))
        check(c0 > 0.2 and turn(a0, a90) > 45,
              "the material ball turns with the Rotation (stripes %.0f deg at 0, %.0f deg at 90)" % (a0, a90))

        # ---- follow the stroke: an L across then down the cube's front face
        s("set_camera", view="front", frame=True)
        s.settle(10, 1.0)
        view = rect(s, "viewport.image")
        vw, vh = view[2] - view[0], view[3] - view[1]

        def stroke_legs(follow, name):
            s("new_project", primitive="cube", resolution=1024)
            s("set_camera", view="front", frame=True)
            s("set_brush", size=70, hardness=1, spacing=0.05, projection="screen", lazy_radius=0)
            s("set_paint_material", id="patina_breton_stripes", rotation=0, mapping="screen", scale=6,
              follow_stroke=follow, follow_rope=10)
            pts = [[0.36 + 0.004 * k, 0.40] for k in range(0, 71)] + [[0.64, 0.40 + 0.004 * k] for k in range(1, 71)]
            s("paint_stroke", points=pts)
            s.settle(10, 1.0)
            im = s.image().crop(view)
            im.save(os.path.join(OUT, name + ".png"))
            # the middle of each leg, away from the corner
            across = im.crop((int(vw * 0.42), int(vh * 0.40 - 22), int(vw * 0.54), int(vh * 0.40 + 22)))
            down = im.crop((int(vw * 0.64 - 22), int(vh * 0.56), int(vw * 0.64 + 22), int(vh * 0.62)))
            return orientation(across), orientation(down)

        (fa, fca), (fd, fcd) = stroke_legs(True, "follow")
        (na, nca), (nd, ncd) = stroke_legs(False, "fixed")
        print("    follow: across %.0f (%.2f), down %.0f (%.2f); fixed: across %.0f (%.2f), down %.0f (%.2f)" %
              (fa, fca, fd, fcd, na, nca, nd, ncd))
        check(min(fca, fcd) > 0.2 and turn(fa, fd) > 60,
              "following the stroke, the stripes turn with it (%.0f deg between the legs)" % turn(fa, fd))
        check(min(nca, ncd) > 0.2 and turn(na, nd) < 20,
              "without it, both legs show them the same way (%.0f deg)" % turn(na, nd))
        # (1.2.17) a long sweeping arc: the stripes run smoothly along it (each dab's own frame made a step at every
        # dab: the stripes broke into stairs, coherence about 0.3-0.6)
        s("new_project", primitive="cube", resolution=2048)
        s("set_camera", view="front", frame=True)
        s("set_brush", size=110, hardness=1, spacing=0.05, projection="screen", lazy_radius=0)
        s("set_paint_material", id="patina_breton_stripes", rotation=0, mapping="screen", scale=4,
          follow_stroke=True, follow_rope=24)
        s("paint_stroke", points=[[0.5 + 0.17 * math.cos(math.radians(a)), 0.55 - 0.17 * math.sin(math.radians(a))]
                                  for a in range(200, -21, -1)])
        s.settle(10, 1.0)
        im = s.image().crop(view)
        im.save(os.path.join(OUT, "arc.png"))
        cohs = []
        for a in (150, 120, 90, 60, 30):
            cx, cy = vw * (0.5 + 0.17 * math.cos(math.radians(a))), vh * (0.55 - 0.17 * math.sin(math.radians(a)))
            cohs.append(orientation(im.crop((int(cx - 14), int(cy - 14), int(cx + 14), int(cy + 14))))[1])
        check(min(cohs) > 0.85, "round a long arc the stripes run on smoothly, no steps (coherence %s)" %
              ", ".join("%.2f" % c for c in cohs))
        st = s("get_state")
        check(st.get("material", {}).get("follow_stroke") is True, "get_state reports follow_stroke")

        # ---- the Blender buttons on the menu bar
        snd = s("ui_rect", name="menubar.blender_send")
        aut = s("ui_rect", name="menubar.blender_auto")
        check(snd.get("found") and aut.get("found") and snd["y"] < 30 and aut["y"] < 30,
              "Send to Blender and Auto-Send are on the menu bar")
    finally:
        s.close()
    print("material follow: %s" % ("all checks passed" if not failures else "%d FAILED" % len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
