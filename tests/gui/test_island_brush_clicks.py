#!/usr/bin/env python3
"""Island brush in the real window (Xvfb, synthetic mouse and keys): the click that confirms the
brush size (F) doesn't paint a selection, each Island brush stroke is one undo step, and (0.43) the Screen and
Surface buttons: by the sphere's outline the circle on screen takes a long strip, Surface about a disc.

    python3 tests/gui/test_island_brush_clicks.py [path/to/patina]
"""
import json
import os
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import manual_shots  # noqa: E402
from manual_scenes import click, layout, tool, vp  # noqa: E402

BIN = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "patina")
failures = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        failures.append(what)


def main():
    # F sets the brush size here (as in the Blender keymap)
    real_mkdtemp = tempfile.mkdtemp

    def mkdtemp(prefix=""):
        d = real_mkdtemp(prefix=prefix)
        os.makedirs(os.path.join(d, ".config", "patina"), exist_ok=True)
        with open(os.path.join(d, ".config", "patina", "keymap.json"), "w") as f:
            json.dump({"version": 5, "preset": "patina", "bindings": {"brush.radius": "F", "view.frame": ""}}, f)
        return d

    tempfile.mkdtemp = mkdtemp
    s = manual_shots.Session(BIN, os.path.join("/tmp", "patina_island_click_test"), display=":97", port=7874)
    tempfile.mkdtemp = real_mkdtemp
    selected = lambda: s("select_faces", mode="add", faces=[])["selected"]
    try:
        s("new_project", primitive="sphere", resolution=512)
        layout(s)
        s("set_camera", view="front", frame=True)
        tool(s, "island")
        check(selected() == 0, "nothing selected to start with")
        # 1. F, move, click to confirm and keep the button down for a moment while moving
        s.x.move(*vp(0.5, 0.5))
        time.sleep(0.3)
        s.x.key("f")
        time.sleep(0.3)
        x, y = vp(0.56, 0.5)
        s.x.move(x, y)
        time.sleep(0.3)
        s.x.drag([(x, y), (x + 10, y + 5), (x + 20, y + 10), (x + 30, y + 15)])
        check(selected() == 0, "the click that confirms the brush size paints nothing (%d faces)" % selected())
        # 2. a stroke paints, and one Ctrl+Z takes the whole stroke back
        s.x.drag([vp(0.45, 0.45), vp(0.5, 0.48), vp(0.55, 0.5)])
        n = selected()
        check(n > 0, "an Island brush stroke selects faces (%d)" % n)
        check(s("history")["steps"][-1] == "Island brush", "the stroke is one undo step named 'Island brush'")
        s.x.key("Control_L", True)
        s.x.key("z")
        s.x.key("Control_L", False)
        time.sleep(0.5)
        check(selected() == 0, "Ctrl+Z takes the stroke back")
        s("redo")
        check(selected() == n, "redo brings it back")
        # 3. (0.43) Screen and Surface: a click by the outline of the sphere, where it is seen edge-on
        hit = lambda fx: s("pick_face", point=[fx, 0.5])["hit"]
        lo, hi = 0.5, 1.0
        for _ in range(12):
            mid = (lo + hi) / 2
            lo, hi = (mid, hi) if hit(mid) else (lo, mid)
        edge_x = 0.5 + (lo - 0.5) * 0.9
        s("set_brush", size=140)
        taken = {}
        for reach in ("screen", "surface"):
            click(s, "btn.island.reach_" + reach)
            s("select_faces", mode="clear")
            s.x.move(*vp(edge_x, 0.5))
            time.sleep(0.3)
            s.x.click(*vp(edge_x, 0.5))
            time.sleep(0.4)
            taken[reach] = selected()
        check(taken["screen"] > 1.3 * taken["surface"] > 0,
              "by the outline, Screen takes a strip and Surface less (%d vs %d faces)" % (taken["screen"], taken["surface"]))
    finally:
        s.close()
    print("island brush clicks: %d failed" % len(failures) if failures else "island brush clicks: all checks passed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
