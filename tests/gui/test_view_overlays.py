#!/usr/bin/env python3
"""(0.37) The 3D view's axis gizmo and the top bar's Retopology preview button, in the real window (Xvfb or
PATINA_SHOTS_DISPLAY, synthetic mouse).

    python3 tests/gui/test_view_overlays.py [path/to/patina]
"""
import math
import os
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import manual_shots  # noqa: E402
import manual_scenes  # noqa: E402
from manual_scenes import center, click, layout, rect  # noqa: E402

BIN = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "patina")
failures = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        failures.append(what)


def angle(a, b):
    """how far apart two angles are, the short way round"""
    d = (a - b) % (2 * math.pi)
    return min(d, 2 * math.pi - d)


def pixels(im):
    b = im.convert("RGB").tobytes()
    return zip(b[0::3], b[1::3], b[2::3])


def cyan(s):
    """pixels of the quads' cyan lines in the lower part of the view (the gizmo's blue Z is at the top)"""
    x0, y0, x1, y1 = manual_scenes.VIEW
    im = s.image().crop((x0, y0 + (y1 - y0) // 3, x1, y1))
    return sum(1 for r, g, b in pixels(im) if b - r > 80 and g - r > 60)


def red_in(s, box):
    im = s.image().crop(box)
    return sum(1 for r, g, b in pixels(im) if r > 200 and g < 120 and b < 130)


def main():
    s = manual_shots.Session(BIN, os.path.join("/tmp", "patina_overlays_test"), display=":97", port=7884)
    try:
        s("new_project", primitive="sphere", resolution=512)
        layout(s)
        s("set_tool", tool="paint")
        s("set_camera", view="front", frame=True)
        steps = lambda: len(s("history")["steps"])
        # ---- the axis gizmo
        n0 = steps()
        click(s, "gizmo.X")
        time.sleep(0.6)
        c = s("set_camera")
        check(angle(c["yaw"], math.pi / 2) < 1e-3 and abs(c["pitch"]) < 1e-3, "a click on X looks from the right (yaw %.3f)" % c["yaw"])
        click(s, "gizmo.X")
        time.sleep(0.6)
        c = s("set_camera")
        check(angle(c["yaw"], -math.pi / 2) < 1e-3, "a click on X again looks from the left (yaw %.3f)" % c["yaw"])
        click(s, "gizmo.Y")
        time.sleep(0.6)
        c = s("set_camera")
        check(c["pitch"] > 1.56, "a click on Y looks from the top (pitch %.3f)" % c["pitch"])
        click(s, "gizmo.Z")
        time.sleep(0.6)
        c = s("set_camera")
        check(angle(c["yaw"], 0) < 1e-3 and abs(c["pitch"]) < 1e-3, "a click on Z looks from the front")
        # a drag inside the gizmo (away from the ends) orbits
        x0, y0, x1, y1 = rect(s, "gizmo.view")
        cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
        r = (x1 - x0) / 2
        start = (cx - 0.15 * r, cy + 0.55 * r)
        s.x.drag([start] + [(start[0] + 4 * i, start[1]) for i in range(1, 12)], dt=0.04)
        time.sleep(0.3)
        c2 = s("set_camera")
        check(angle(c2["yaw"], c["yaw"]) > 0.05, "a drag on the gizmo orbits (yaw %.3f -> %.3f)" % (c["yaw"], c2["yaw"]))
        check(steps() == n0, "the gizmo's clicks and drags paint nothing (%d -> %d history steps)" % (n0, steps()))
        # set_view hides it
        box = (round(x0), round(y0), round(x1), round(y1))
        on = red_in(s, box)
        s("set_view", axis_gizmo=False)
        time.sleep(0.4)
        off = red_in(s, box)
        s("set_view", axis_gizmo=True)
        check(on > 30 and off < 5, "set_view {axis_gizmo: false} hides it (%d -> %d red pixels)" % (on, off))
        # with the panels hidden (Tab) the view still takes strokes and the gizmo its clicks (before 0.37 the empty
        # dock host was in front of it and took the mouse)
        s.x.move(*manual_scenes.vp(0.5, 0.6))
        s.x.key("Tab")
        time.sleep(1.5)
        n1 = steps()
        s.x.drag([(700 + 10 * i, 500) for i in range(10)], dt=0.04)
        time.sleep(0.5)
        check(steps() == n1 + 1, "with the panels hidden a stroke paints (%d -> %d history steps)" % (n1, steps()))
        click(s, "gizmo.X")
        time.sleep(0.6)
        c = s("set_camera")
        check(angle(c["yaw"], math.pi / 2) < 1e-3, "with the panels hidden the gizmo works (yaw %.3f)" % c["yaw"])
        s.x.key("Tab")
        time.sleep(1.0)
        layout(s)
        # ---- the Retopology preview button
        s("set_camera", view="front", frame=True)
        s("retopo_density", quads_per_m2=150)
        s("retopo_build")
        time.sleep(0.8)
        none = cyan(s)
        check(not s("retopo_view")["preview"], "the preview starts off")
        click(s, "btn.retopo_preview")
        time.sleep(0.8)
        shown = cyan(s)
        check(s("retopo_view")["preview"], "the button turns it on")
        check(none < 50 and shown > 1000, "the quads show with the Paint tool chosen (%d -> %d cyan pixels)" % (none, shown))
        click(s, "btn.retopo_preview")
        time.sleep(0.8)
        check(not s("retopo_view")["preview"] and cyan(s) < 50, "a second click hides them")
        # the arrow: what the preview shows
        click(s, "btn.retopo_preview.more")
        time.sleep(0.5)
        x0, y0, x1, y1 = rect(s, "popup.top")
        bx0, by0, bx1, by1 = rect(s, "btn.retopo_preview.more")
        check(y0 >= by1 - 2 and x0 <= bx1, "the arrow opens its popup under it")
        s.x.key("Escape")
        # MCP: retopo_view {preview}
        s("retopo_view", preview=True)
        time.sleep(0.8)
        check(cyan(s) > 1000, "retopo_view {preview: true} shows them too")
        s("retopo_view", preview=False)
    finally:
        s.close()
    print("view overlays: %d failed" % len(failures) if failures else "view overlays: all checks passed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
