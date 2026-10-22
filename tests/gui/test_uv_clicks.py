#!/usr/bin/env python3
"""UV editor clicks in the real window (Xvfb, synthetic mouse and keys), 0.11.1:
- a face whose UVs collapsed onto one point no longer catches every click;
- Shift+click on a selected island takes it off, on another one adds it (as in Blender).

    python3 tests/gui/test_uv_clicks.py [path/to/patina]
"""
import os
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import manual_shots  # noqa: E402
import manual_scenes  # noqa: E402

BIN = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "patina")
failures = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        failures.append(what)


def main():
    s = manual_shots.Session(BIN, os.path.join("/tmp", "patina_uv_click_test"), display=":98", port=7875)
    try:
        s("new_project", primitive="sphere", resolution=512)
        manual_scenes.layout(s)
        n = s("get_state")["mesh"]["faces"]
        last = n - 1
        # the last face's UVs collapse onto one point (its triangles come last, so they used to win)
        s("uv_transform", faces=[last], scale=[0.0, 0.0])
        check(s("uv_info")["faces_without_uv_area"] == 1, "uv_info reports the face with no UV area")
        x0, y0, x1, y1 = manual_scenes.UV
        cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
        s.x.click(cx, cy)
        sel = s("uv_islands", selection=True)["islands"]
        check(len(sel) == 1 and sel[0]["first_face"] != last, "a click selects the face under the cursor, not the collapsed one")
        # (Islands is the default selection mode: the sphere is one island, without the collapsed face)
        one = s("select_faces", mode="add", faces=[])["selected"]
        check(one == n - 1, "the island under the cursor is selected (%d faces)" % one)
        s.x.key("Shift_L", True)
        s.x.click(cx + 40, cy)
        s.x.key("Shift_L", False)
        off = s("select_faces", mode="add", faces=[])["selected"]
        check(off == 0, "Shift+click on a selected island takes it off (%d)" % off)
        s.x.key("Shift_L", True)
        s.x.click(cx + 40, cy)
        s.x.key("Shift_L", False)
        back = s("select_faces", mode="add", faces=[])["selected"]
        check(back == one, "Shift+click on it again adds it (%d)" % back)
    finally:
        s.close()
    print("uv clicks: %d failed" % len(failures) if failures else "uv clicks: all checks passed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
