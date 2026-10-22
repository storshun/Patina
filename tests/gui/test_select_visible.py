#!/usr/bin/env python3
"""(0.45) The Select tool's Visible only, in the real window (Xvfb or PATINA_SHOTS_DISPLAY, synthetic mouse): a skin
(its own UV island) inside a shell open at the front. A click on the skin through the opening takes its whole island
as before; with Visible only on, just the part seen through the opening. A drag does the same; Ctrl+click takes
them off.

    python3 tests/gui/test_select_visible.py [path/to/patina]
"""
import importlib.util
import os
import shutil
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import manual_shots  # noqa: E402
import manual_scenes  # noqa: E402
from manual_scenes import click, rect  # noqa: E402

spec = importlib.util.spec_from_file_location("sv", os.path.join(ROOT, "tests", "api", "test_select_visible.py"))
sv = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sv)

BIN = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "patina")
failures = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        failures.append(what)


def main():
    work = tempfile.mkdtemp(prefix="patina_select_visible_gui_")
    obj = os.path.join(work, "skin_and_shell.obj")
    nskin, nshell = sv.skin_and_shell(obj)
    s = manual_shots.Session(BIN, os.path.join("/tmp", "patina_select_visible_test"), display=":97", port=7887)
    try:
        s("open_mesh", path=obj)
        s("set_tool", tool="select")
        manual_scenes.VIEW = rect(s, "viewport.image")
        s("set_camera", view="front", frame=True)
        s.settle(5)
        selected = lambda: s("select_faces", mode="add", faces=[])["selected"]
        mid = manual_scenes.vp(0.5, 0.5)
        hit = s("pick_face", point=[0.5, 0.5])
        check(hit.get("hit") and hit["face"] < nskin, "the middle of the view is the skin, through the opening")
        # Visible only off: a click takes the skin's whole island
        s.x.click(*mid)
        s.settle(4)
        check(selected() == nskin, "a click takes the whole island (%d of %d)" % (selected(), nskin))
        # on: the part seen through the opening
        click(s, "btn.select.visible")
        s.x.click(*mid)
        s.settle(4)
        n = selected()
        check(0.15 * nskin < n < 0.4 * nskin, "with Visible only, the part seen through the opening (%d of %d)" % (n, nskin))
        # a drag across the opening: the same island, the same faces
        s("select_faces", mode="clear")
        s.x.drag([manual_scenes.vp(0.45 + 0.01 * k, 0.5) for k in range(11)])
        s.settle(4)
        check(selected() == n, "a drag takes the same faces (%d)" % selected())
        # a Ctrl+click takes them off
        s.x.key("Control_L", True)
        s.x.click(*mid)
        s.x.key("Control_L", False)
        s.settle(4)
        check(selected() == 0, "Ctrl+click takes them off (%d left)" % selected())
    finally:
        s.close()
    shutil.rmtree(work, ignore_errors=True)
    shutil.rmtree(os.path.join("/tmp", "patina_select_visible_test"), ignore_errors=True)
    print("select visible: %s" % ("all checks passed" if not failures else "%d FAILED" % len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
