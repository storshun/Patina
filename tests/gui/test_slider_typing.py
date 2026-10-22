#!/usr/bin/env python3
"""(0.42) Right-click a slider to type its value, in the real window (Xvfb or PATINA_SHOTS_DISPLAY, synthetic mouse and
keys): the toolbar's brush size takes a typed 123, a typed value too big stays in the slider's range, Esc goes back,
a left click still sets the value where it lands, a slider in a panel (a baked map's paint value) works the same, and
with the feature off a right-click types nothing.

    python3 tests/gui/test_slider_typing.py [path/to/patina]
"""
import os
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import manual_shots  # noqa: E402
from manual_scenes import center, layout, rect, type_text  # noqa: E402

BIN = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "patina")
failures = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        failures.append(what)


def main():
    s = manual_shots.Session(BIN, os.path.join("/tmp", "patina_slider_typing_test"), display=":94", port=7899)
    try:
        s("new_project", primitive="sphere", resolution=256)
        layout(s)
        s("set_tool", tool="paint")
        s("set_brush", size=40, hardness=0.9)
        brush = lambda: s("set_brush")

        def typed(widget, text, key="Return"):
            s.x.click(*center(s, widget), b=3)
            time.sleep(0.4)
            type_text(s, text)
            time.sleep(0.2)
            s.x.key(key)
            time.sleep(0.4)

        typed("toolbar.slider:size", "123")
        check(abs(brush()["size"] - 123) < 1e-3, "right-click the brush size, type 123, Enter (%.2f)" % brush()["size"])
        typed("toolbar.slider:size", "5000")
        check(abs(brush()["size"] - 600) < 1e-3, "a typed value too big stays in the slider's range (%.2f)" % brush()["size"])
        typed("toolbar.slider:hard", "0.25", key="Escape")
        check(abs(brush()["hardness"] - 0.9) < 1e-3, "Esc goes back (%.3f)" % brush()["hardness"])
        typed("toolbar.slider:hard", "0.5")
        check(abs(brush()["hardness"] - 0.5) < 1e-3, "hardness 0.5 typed (%.3f)" % brush()["hardness"])
        # a left click still sets the value where it lands (near the left end: small)
        x0, y0, x1, y1 = rect(s, "toolbar.slider:hard")
        s.x.click(x0 + 3, (y0 + y1) // 2)
        time.sleep(0.4)
        check(brush()["hardness"] < 0.1, "a left click near the left end sets it low, as before (%.3f)" % brush()["hardness"])
        # a slider in a panel: a baked map layer's paint value
        s("bake_mesh_maps", ao=True, curvature=False, ao_samples=8)
        s("baked_maps", map="ao", select=True)
        time.sleep(0.8)
        L = rect(s, "window.Layers")
        for _ in range(30):  # (the properties are under the list: scrolled into sight)
            if rect(s, "baked.value")[3] < L[3] - 4:
                break
            s.x.move((L[0] + L[2]) // 2, L[3] - 30)
            s.x.button(5, True)
            s.x.button(5, False)
            time.sleep(0.15)
        typed("baked.value", "0.5")
        check(abs(brush()["mask_value"] - 0.5) < 1e-3, "the paint value in the layer's properties takes 0.5 (%.3f)" % brush()["mask_value"])
        # switched off: a right-click types nothing
        s("features", id="slider_typing", on=False)
        time.sleep(0.3)
        typed("toolbar.slider:size", "77")
        check(abs(brush()["size"] - 77) > 1e-3, "switched off, a right-click types nothing (%.2f)" % brush()["size"])
        s("features", id="slider_typing", on=True)
    finally:
        s.close()
    print("slider typing: %d failed" % len(failures) if failures else "slider typing: all checks passed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
