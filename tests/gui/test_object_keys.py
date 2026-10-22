#!/usr/bin/env python3
"""(1.2.3) G, R and S in the Object tool, in the real window, as in Blender's Object Mode:
- S scales the object evenly with the pointer's distance from its middle (no handle to hit), a click keeps it, one
  undo step takes it back; S doesn't switch to the Select tool there;
- S then a typed 0.5 and Enter: exactly half;
- G, X, a typed 0.25 and Enter: a quarter along X only (X doesn't swap the colors there);
- R and a pointer going round the object turns it; a right-click puts it back, with no undo step;
- outside the Object tool S is the Select tool's key again.

    python3 tests/gui/test_object_keys.py [path/to/patina]
"""
import math
import os
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
sys.path.insert(0, os.path.join(ROOT, "tests", "gui"))
import manual_shots  # noqa: E402
from manual_scenes import click, layout, rect, tool  # noqa: E402
from test_scene_objects import character  # noqa: E402

BIN = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "patina")
failures = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        failures.append(what)


def near(a, b, tol=0.02):
    return all(abs(x - y) <= tol for x, y in zip(a, b))


def main():
    work = os.path.join("/tmp", "patina_object_keys_test")
    os.makedirs(work, exist_ok=True)
    model = os.path.join(work, "character.obj")
    character(model)
    s = manual_shots.Session(BIN, os.path.join("/tmp", "patina_object_keys_shots"), display=":95", port=7899)

    def hat():
        return s("list_objects")["objects"][1]

    def steps():
        return len(s("history")["steps"])

    def keys(*names):
        for n in names:
            s.x.key(n)
            time.sleep(0.05)
        s.settle(3)

    try:
        s("layout", mode="full")
        s("open_mesh", path=model, resolution=512)
        s.settle(6)
        layout(s)
        s("set_camera", view="front", frame=True)
        s.settle(3)
        click(s, "objects.row.Hat")
        s.settle(3)
        tool(s, "object")
        s.settle(3)
        layout(s)
        x0, y0, x1, y1 = rect(s, "gizmo.center")
        cx, cy = (x0 + x1) / 2, (y0 + y1) / 2

        # S: even scale with the pointer's distance; a click keeps it
        h0 = steps()
        s.x.move(int(cx + 80), int(cy))
        s.settle(2)
        keys("s")
        s.x.move(int(cx + 160), int(cy))
        s.settle(3)
        live = hat()["scale"]
        check(s("get_state").get("window_tool") == "object", "S in the Object tool doesn't switch to the Select tool")
        check(live[0] > 1.5 and near(live, [live[0]] * 3, 1e-4), "S scales evenly with the pointer (%s)" % live)
        s.x.click(int(cx + 160), int(cy))
        s.settle(3)
        kept = hat()["scale"]
        check(near(kept, live, 0.05) and steps() == h0 + 1, "a click keeps it, one undo step (%s, %d steps)" % (kept, steps() - h0))
        s("undo")
        s.settle(2)
        check(near(hat()["scale"], [1, 1, 1], 1e-4), "the undo takes it back (%s)" % hat()["scale"])

        # S 0.5 Enter: exactly half
        s.x.move(int(cx + 80), int(cy))
        keys("s", "0", "period", "5", "Return")
        check(near(hat()["scale"], [0.5, 0.5, 0.5], 1e-4), "S, 0.5, Enter: half (%s)" % hat()["scale"])
        s("undo")
        s.settle(2)

        # G X 0.25 Enter: along X only; X doesn't swap the colors here
        color0 = s("get_state")["material"]["flat"]["color"]
        loc0 = hat()["location"]
        keys("g", "x", "0", "period", "2", "5", "Return")
        loc = hat()["location"]
        check(near(loc, [loc0[0] + 0.25, loc0[1], loc0[2]], 1e-4), "G, X, 0.25, Enter: a quarter along X (%s -> %s)" % (loc0, loc))
        check(s("get_state")["material"]["flat"]["color"] == color0, "X held to the axis: the colors didn't swap")
        s("undo")
        s.settle(2)

        # R round the middle: it turns; a right-click puts it back, no undo step
        h0 = steps()
        r = 90
        s.x.move(int(cx + r), int(cy))
        s.settle(2)
        keys("r")
        for k in range(1, 10):
            a = math.radians(10 * k)
            s.x.move(int(cx + r * math.cos(a)), int(cy + r * math.sin(a)))
            time.sleep(0.04)
        s.settle(3)
        rot = hat()["rotation"]
        check(max(abs(v) for v in rot) > 60, "R and the pointer going round turn it (%s)" % rot)
        s.x.click(int(cx), int(cy - r), b=3)
        s.settle(3)
        check(near(hat()["rotation"], [0, 0, 0], 1e-3) and steps() == h0, "a right-click puts it back, no undo step (%s)" % hat()["rotation"])

        # outside the Object tool S is the Select tool's again
        tool(s, "paint")
        s.settle(2)
        x0, y0, x1, y1 = rect(s, "viewport.image")
        s.x.move(int((x0 + x1) / 2), int((y0 + y1) / 2))
        keys("s")
        check(s("get_state").get("window_tool") == "select", "outside the Object tool S picks the Select tool (%s)" % s("get_state").get("window_tool"))
    finally:
        s.close()
    print("object keys: %s" % ("all checks passed" if not failures else "%d FAILED" % len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
