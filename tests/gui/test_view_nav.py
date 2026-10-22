#!/usr/bin/env python3
"""(0.38) Side views and the view pie, in the real window (Xvfb or PATINA_SHOTS_DISPLAY, synthetic mouse and keys):
the numpad and the axis gizmo jump to side views (orthographic) and Numpad 0 / the Back button go back; Alt while
orbiting with the middle button snaps; the Z pie switches wireframe, solid, lit, unlit, seams and the UV checker
(0.40: the other pies are tests/gui/test_pies.py).

    python3 tests/gui/test_view_nav.py [path/to/patina]
"""
import math
import os
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import manual_shots  # noqa: E402
import manual_scenes  # noqa: E402
from manual_scenes import center, click, layout, rect, vp  # noqa: E402

BIN = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "patina")
failures = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        failures.append(what)


def angle(a, b):
    d = (a - b) % (2 * math.pi)
    return min(d, 2 * math.pi - d)


def near_view(c, yaw, pitch):
    return angle(c["yaw"], yaw) < 1e-3 and abs(c["pitch"] - pitch) < 1e-3


def main():
    s = manual_shots.Session(BIN, os.path.join("/tmp", "patina_view_nav_test"), display=":97", port=7886)
    try:
        s("new_project", primitive="cube", resolution=512)
        layout(s)
        s("set_tool", tool="paint")
        s("set_camera", view="persp", frame=True, ortho=False)
        s("set_camera", yaw=0.7, pitch=0.4)
        start = s("set_camera")
        cam = lambda: s("set_camera")
        steps = lambda: len(s("history")["steps"])
        n0 = steps()
        # ---- the numpad jumps, orthographic; Numpad 0 goes back, and again
        s.x.move(*vp(0.5, 0.6))
        time.sleep(0.3)
        s.x.key("KP_1")  # (the key, whatever Num Lock says)
        time.sleep(0.1)
        c = cam()
        check(near_view(c, 0, 0) and c["ortho"] and c["side_view"], "Numpad 1 jumps to the front, orthographic (%s)" % c)
        s.x.key("KP_0")
        time.sleep(0.3)
        c = cam()
        check(near_view(c, start["yaw"], start["pitch"]) and not c["ortho"],
              "Numpad 0 goes back to the view before (yaw %.3f, ortho %s)" % (c["yaw"], c["ortho"]))
        s.x.key("KP_0")
        time.sleep(0.3)
        c = cam()
        check(near_view(c, 0, 0) and c["ortho"], "and again to the front view")
        # orbiting away from it: perspective again
        s.x.drag([vp(0.5, 0.6)] + [(vp(0.5, 0.6)[0] + 6 * i, vp(0.5, 0.6)[1]) for i in range(1, 8)], b=2, dt=0.04)
        c = cam()
        check(not c["ortho"] and not c["side_view"] and angle(c["yaw"], 0) > 0.05, "orbiting away: perspective again (%s)" % c["ortho"])
        # ---- the gizmo jumps; the Back button under it goes back
        s("set_camera", yaw=0.7, pitch=0.4)
        free = cam()
        click(s, "gizmo.X")
        time.sleep(0.15)
        c = cam()
        check(near_view(c, math.pi / 2, 0) and c["ortho"], "a click on the gizmo's X jumps there (yaw %.4f)" % c["yaw"])
        time.sleep(0.4)
        click(s, "gizmo.back")
        time.sleep(0.3)
        c = cam()
        check(near_view(c, free["yaw"], free["pitch"]) and not c["ortho"], "its Back button goes back (yaw %.3f)" % c["yaw"])
        # ---- Alt while orbiting with the middle button: the nearest side view. (Patina's mouse preset: Alt+middle
        # pressed together pans, as in Substance and Maya, so Alt comes once the orbit has begun)
        s("set_camera", yaw=1.3, pitch=0.2)
        free = cam()
        p0 = vp(0.45, 0.6)
        s.x.move(*p0)
        time.sleep(0.2)
        s.x.button(2, True)
        for i in range(1, 4):
            s.x.move(p0[0] + 3 * i, p0[1])
            time.sleep(0.04)
        s.x.key("Alt_L", True)
        for i in range(4, 8):
            s.x.move(p0[0] + 3 * i, p0[1])
            time.sleep(0.04)
        time.sleep(0.2)
        c = cam()
        check(near_view(c, math.pi / 2, 0) and c["ortho"] and c["side_view"],
              "middle drag, then Alt: the nearest side view (yaw %.3f pitch %.3f)" % (c["yaw"], c["pitch"]))
        # on with Alt held: the next side view along
        for i in range(1, 31):  # (240 pixels: inside the view)
            s.x.move(p0[0] + 21 - 8 * i, p0[1])
            time.sleep(0.02)
        time.sleep(0.2)
        c = cam()
        on_axis = abs((c["yaw"] / (math.pi / 2)) - round(c["yaw"] / (math.pi / 2))) < 1e-3
        check(on_axis and angle(c["yaw"], math.pi / 2) > 1.0, "further on: another side view (yaw %.3f)" % c["yaw"])
        # Alt let go: free again from that side view (no jump back to where the drag alone would be)
        snapped = c["yaw"]
        s.x.key("Alt_L", False)
        for i in range(1, 4):
            s.x.move(p0[0] - 219 + 3 * i, p0[1])
            time.sleep(0.04)
        time.sleep(0.2)
        c = cam()
        check(0.005 < angle(c["yaw"], snapped) < 0.2 and not c["ortho"], "Alt let go: free from there (yaw %.3f)" % c["yaw"])
        s.x.button(2, False)
        time.sleep(0.3)
        s.x.key("KP_0")
        time.sleep(0.3)
        c = cam()
        check(near_view(c, free["yaw"], free["pitch"]), "Numpad 0 goes back to where the drag began (yaw %.3f)" % c["yaw"])
        check(steps() == n0, "no painting from any of it (%d -> %d history steps)" % (n0, steps()))
        # ---- the view pie: hold Z, point, let go
        def pie(slot, hold=True):
            s.x.move(*vp(0.5, 0.55))
            time.sleep(0.3)
            if hold:
                s.x.key("z", True)
                time.sleep(0.35)
            else:
                s.x.key("z")
                time.sleep(0.4)
            x, y = center(s, "pie.%d" % slot)
            s.x.move(x, y)
            time.sleep(0.3)
            if hold:
                s.x.key("z", False)
            else:
                s.x.button(1, True)
                time.sleep(0.08)
                s.x.button(1, False)
            time.sleep(0.4)
        v0 = s("set_view")
        pie(0)
        check(s("set_view")["wireframe"] != v0["wireframe"], "pie, held: Wireframe (left)")
        pie(0)
        check(s("set_view")["wireframe"] == v0["wireframe"], "and off again")
        pie(1, hold=False)
        check(s("set_view")["mode"] == "solid", "pie, tapped then clicked: Solid (right)")
        # solid: light gray (lit by the environment, so a little tinted), no textures
        s("set_camera", yaw=0.7, pitch=0.4, ortho=False)
        time.sleep(0.4)
        x0, y0, x1, y1 = manual_scenes.VIEW
        px = s.image().convert("RGB").getpixel((int((x0 + x1) / 2), int((y0 + y1) / 2)))
        check(max(px) - min(px) < 25 and min(px) > 60, "the model is gray in Solid (%s)" % (px,))
        pie(4)
        check(s("set_view")["mode"] == "basecolor", "Unlit (up left): the base color")
        pie(4)
        check(s("set_view")["mode"] == "solid", "again: back to the mode before (Solid)")
        pie(2)
        check(s("set_view")["mode"] == "material", "Lit (down): the material")
        sv = s("set_view")["seams"]
        pie(5)
        check(s("set_view")["seams"] != sv, "Seams (up right)")
        pie(5)
        pie(6)
        check(s("set_view")["mode"] == "uv_checker", "UV checker (down left)")
        pie(6)
        check(s("set_view")["mode"] == "material", "again: back to the material")
        # Escape closes it without choosing
        s.x.move(*vp(0.5, 0.55))
        s.x.key("z")
        time.sleep(0.4)
        s.x.key("Escape")
        time.sleep(0.3)
        s.x.move(*center(s, "pie.0"))
        s.x.button(1, True)
        time.sleep(0.05)
        s.x.button(1, False)
        time.sleep(0.4)
        check(s("set_view")["wireframe"] == v0["wireframe"], "Esc closes it")
        check(steps() <= n0 + 2, "the pie paints nothing (%d -> %d history steps)" % (n0, steps()))
        # ---- MCP: set_camera view=previous
        s("set_camera", yaw=0.9, pitch=0.3)
        s("set_camera", view="top")
        c = s("set_camera", view="previous")
        check(abs(c["yaw"] - 0.9) < 1e-3 and not c["side_view"], "set_camera {view: previous} goes back")
    finally:
        s.close()
    # ---- the Blender mouse preset: Alt+middle drag from the start snaps, as in Blender
    s = manual_shots.Session(BIN, os.path.join("/tmp", "patina_view_nav_test"), display=":97", port=7887,
                             session={"nav_preset": 1})
    try:
        s("new_project", primitive="cube", resolution=512)
        layout(s)
        s("set_camera", view="persp", frame=True, ortho=False)
        s("set_camera", yaw=-1.4, pitch=1.1)
        p0 = vp(0.45, 0.6)
        s.x.drag([p0] + [(p0[0] + 3 * i, p0[1]) for i in range(1, 6)], b=2, dt=0.04, mods=("Alt_L",))
        c = s("set_camera")
        check(near_view(c, -math.pi / 2, 1.5699) and c["ortho"],
              "Blender preset: Alt + middle drag snaps (to the top view here: yaw %.3f pitch %.3f)" % (c["yaw"], c["pitch"]))
    finally:
        s.close()
    print("view navigation: %d failed" % len(failures) if failures else "view navigation: all checks passed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
