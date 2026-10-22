#!/usr/bin/env python3
"""(0.37.1) Pin and unpin from the 3D view: the Select and Island tools' Pin / Unpin buttons, Alt+P over the 3D view,
and the blue stripes on the pinned islands, in the real window (Xvfb or PATINA_SHOTS_DISPLAY, synthetic mouse).

    python3 tests/gui/test_viewport_pins.py [path/to/patina]
"""
import os
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import manual_shots  # noqa: E402
import manual_scenes  # noqa: E402
from manual_scenes import click, layout  # noqa: E402

BIN = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "patina")
failures = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        failures.append(what)


def blue(s):
    """pixels of the pinned islands' blue stripes in the view (below the axis gizmo)"""
    x0, y0, x1, y1 = manual_scenes.VIEW
    b = s.image().crop((x0, y0 + (y1 - y0) // 4, x1, y1)).convert("RGB").tobytes()
    return sum(1 for r, g, bl in zip(b[0::3], b[1::3], b[2::3]) if bl - r > 70 and g - r > 25)


def main():
    s = manual_shots.Session(BIN, os.path.join("/tmp", "patina_pins_test"), display=":97", port=7885)
    try:
        s("new_project", primitive="cube", resolution=512)
        layout(s)
        s("set_camera", view="front", frame=True)
        s("set_camera", yaw=0.6, pitch=0.45)
        islands = s("uv_info")["islands"]
        pinned = lambda: s("uv_info")["pinned_faces"]
        steps = lambda: len(s("history")["steps"])
        s("uv_pin", all=True, pin=True)
        all_pinned = pinned()
        check(islands > 1 and all_pinned > 0, "a cube with %d islands, every face pinned (%d)" % (islands, all_pinned))
        # the stripes: with the Select tool, not with the Paint tool
        s("set_tool", tool="paint")
        time.sleep(0.6)
        off = blue(s)
        s("set_tool", tool="select")
        time.sleep(0.6)
        on = blue(s)
        check(on > off + 2000, "pinned islands are striped with the Select tool (%d -> %d blue pixels)" % (off, on))
        # one face selected: Unpin unpins its island only, Pin pins it again
        s("select_faces", mode="set", faces=[0])
        time.sleep(0.4)
        n0 = steps()
        click(s, "btn.select.unpin")
        time.sleep(0.4)
        left = pinned()
        check(0 < left < all_pinned, "Unpin unpins the selected face's island (%d -> %d pinned faces)" % (all_pinned, left))
        check(steps() == n0 + 1, "as one undo step")
        time.sleep(0.4)
        check(blue(s) < on, "its stripes go (%d -> %d blue pixels)" % (on, blue(s)))
        click(s, "btn.select.unpin")  # (off now: nothing pinned in the selection)
        time.sleep(0.3)
        check(pinned() == left and steps() == n0 + 1, "Unpin is off with nothing pinned in the selection")
        click(s, "btn.select.pin")
        time.sleep(0.4)
        check(pinned() == all_pinned, "Pin pins it again (%d)" % pinned())
        # Alt+P over the 3D view (before 0.37.1 only the UV editor took it)
        s.x.move(*manual_scenes.vp(0.3, 0.85))
        time.sleep(0.3)
        s.x.key("Alt_L", True)
        s.x.key("p")
        s.x.key("Alt_L", False)
        time.sleep(0.5)
        check(pinned() == left, "Alt+P over the 3D view unpins (%d pinned faces)" % pinned())
        s.x.key("Control_L", True)
        s.x.key("z")
        s.x.key("Control_L", False)
        time.sleep(0.5)
        check(pinned() == all_pinned, "undo pins it again (%d)" % pinned())
        # the Island tool has Unpin too, for the painted faces
        s("set_tool", tool="island")
        s("select_faces", mode="set", faces=[0])
        time.sleep(0.5)
        click(s, "btn.island.unpin")
        time.sleep(0.4)
        check(pinned() == left, "the Island tool's Unpin (%d pinned faces)" % pinned())
        # the feature switch takes it all away
        s("set_tool", tool="select")
        s("features", id="viewport_pins", on=False)
        time.sleep(0.6)
        check(blue(s) < 200, "with viewport_pins off: no stripes (%d blue pixels)" % blue(s))
        s.x.move(*manual_scenes.vp(0.3, 0.85))
        s("uv_pin", all=True, pin=True)
        s.x.key("Alt_L", True)
        s.x.key("p")
        s.x.key("Alt_L", False)
        time.sleep(0.5)
        check(pinned() == all_pinned, "and Alt+P over the 3D view does nothing")
    finally:
        s.close()
    print("viewport pins: %d failed" % len(failures) if failures else "viewport pins: all checks passed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
