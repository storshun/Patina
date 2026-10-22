#!/usr/bin/env python3
"""The Seams tool as a brush, in the real window (Xvfb, synthetic mouse and keys).

    python3 tests/gui/test_seam_brush.py [path/to/patina]
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import manual_shots  # noqa: E402
from manual_scenes import layout, tool, vp  # noqa: E402

BIN = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "patina")
failures = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        failures.append(what)


def main():
    s = manual_shots.Session(BIN, os.path.join("/tmp", "patina_seam_test"), display=":96", port=7873)
    try:
        s("new_project", primitive="sphere", resolution=1024)
        layout(s)
        s("uv_seams", mode="clear")
        s("set_camera", view="front", frame=True)
        s("set_view", seams=True)
        seams = lambda: s("uv_info")["seam_edges"]
        last = lambda: s("history")["steps"][-1]
        check(seams() == 0, "no seams to start with")
        tool(s, "seams")
        # 1. a drag lays a seam along the edges under the cursor
        pts = [vp(0.32 + 0.36 * i / 24, 0.5) for i in range(25)]
        s.x.drag(pts, dt=0.04)
        n1 = seams()
        check(n1 >= 6, "a drag across the sphere marks a seam (%d edges)" % n1)
        check(last() == "Seam brush", "one undo step named 'Seam brush' (%s)" % last())
        # 2. Shift+click continues from where it ended
        s.x.key("Shift_L", True)
        s.x.click(*vp(0.66, 0.3))
        s.x.key("Shift_L", False)
        n2 = seams()
        check(n2 > n1, "Shift+click continues the seam (%d -> %d edges)" % (n1, n2))
        check(last() == "Seam path", "the continuation is its own step (%s)" % last())
        # 3. Ctrl+drag over the first seam clears it again
        s.x.drag(pts, dt=0.04, mods=("Control_L",))
        n3 = seams()
        check(n3 < n2 and n3 <= n2 - n1 + 2, "Ctrl+drag clears the seam under it (%d -> %d)" % (n2, n3))
        # 4. a plain click toggles one edge
        s.x.click(*vp(0.45, 0.7))
        n4 = seams()
        check(n4 == n3 + 1, "a click toggles one edge (%d -> %d)" % (n3, n4))
        # 5. undo goes back step by step
        s("undo")
        check(seams() == n3, "undo removes the toggled edge")
        s("undo")
        check(seams() == n2, "undo brings the cleared seam back")
        s.shot("seam_brush", crop=(72, 22, 728, 678))
    finally:
        s.close()
    print("seam brush: %d failed" % len(failures) if failures else "seam brush: all checks passed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
