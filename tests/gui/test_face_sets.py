#!/usr/bin/env python3
"""(1.2) Loops and face sets in the real window (a hidden display, synthetic mouse and keys): with the Select tool,
Alt+click by an edge of a torus selects a loop of quads right round it, Ctrl+Alt+click the ring the other way, an
Alt+drag still turns the view; Alt+click in the UV editor selects a loop too; the Face Sets panel makes a set of the
selection, its eye hides it from the 3D view, a click on its row selects its faces; the Select tool's Connected grows
the selection; no two widgets share an ID.

    python3 tests/gui/test_face_sets.py [path/to/patina]
"""
import os
import shutil
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import manual_shots  # noqa: E402
from manual_scenes import click, layout, rect, tool, uvxy, vp  # noqa: E402

BIN = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "patina")
failures = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        failures.append(what)


def selected(s):
    return s("select_faces", mode="add", faces=[])["selected"]


def alt_click(s, x, y, ctrl=False):
    if ctrl:
        s.x.key("Control_L", True)
    s.x.key("Alt_L", True)
    s.x.click(x, y)
    s.x.key("Alt_L", False)
    if ctrl:
        s.x.key("Control_L", False)
    time.sleep(0.4)


def main():
    os.environ["PATINA_UI_ID_CHECK"] = "1"
    s = manual_shots.Session(BIN, os.path.join("/tmp", "patina_face_sets_shots"), display=":95", port=7890)
    try:
        s("layout", mode="full")
        s("new_project", primitive="torus", resolution=512)
        s.settle(4)
        s("set_camera", view="top", frame=True)
        tool(s, "select")
        s.settle(3)
        layout(s)
        # a point on the torus, seen from the top: right of the middle
        pt = None
        for fx in [0.5 + k * 0.02 for k in range(5, 25)]:
            if s("pick_face", point=[fx, 0.5])["hit"]:
                pt = (fx + 0.03, 0.5)
                break
        check(pt is not None, "the torus is seen")
        x, y = vp(*pt)
        alt_click(s, x, y)
        a = selected(s)
        alt_click(s, x, y, ctrl=True)
        b = selected(s)
        check(sorted([a, b]) == [24, 64], "Alt+click and Ctrl+Alt+click: a loop and a ring right round (%d, %d)" % (a, b))
        cam = s("get_state")["camera"]
        s.x.key("Alt_L", True)
        s.x.drag([(x, y), (x + 20, y), (x + 60, y), (x + 100, y)])
        s.x.key("Alt_L", False)
        time.sleep(0.4)
        check(abs(s("get_state")["camera"]["yaw"] - cam["yaw"]) > 0.05, "an Alt+drag still turns the view")
        # ---- the UV editor: Alt+click by a UV edge
        s("select_faces", mode="clear")
        ux, uy = uvxy(0.51, 0.52)
        alt_click(s, ux, uy)
        n = selected(s)
        check(n in (24, 64), "Alt+click in the UV editor selects a loop (%d)" % n)
        # ---- the Face Sets panel
        s("open_window", window="face_sets")
        s.settle(4)
        click(s, "btn.facesets.new")
        s.settle(3)
        sets = s("list_face_sets")["face_sets"]
        check(len(sets) == 1 and sets[0]["faces"] == n, "New from the selection: a face set of it (%s)" % [x["faces"] for x in sets])
        s("select_faces", mode="clear")
        name = sets[0]["name"] if sets else ""
        click(s, "facesets.row." + name)
        s.settle(3)
        check(selected(s) == n, "a click on its row selects its faces (%d)" % selected(s))
        # its eye: the check box left of the row
        r = rect(s, "facesets.row." + name)
        box = rect(s, "facesets.list")
        s.x.click(box[0] + 18, (r[1] + r[3]) / 2)
        s.settle(3)
        check(s("list_face_sets")["face_sets"][0]["hidden"], "its eye hides it")
        # ---- Connected
        s("use_face_set", action="show_all")
        s("select_faces", mode="set", faces=[0])
        click(s, "btn.select.linked")
        s.settle(3)
        check(selected(s) == 1536, "Connected grows the selection to the whole torus (%d)" % selected(s))
        for lang in ("x-test", "en"):
            s("language_set", code=lang)
            s.settle(10)
        r = s("ui_id_conflicts")
        check(r["checking"] and not r["conflicts"], "no two widgets share an ID (%s)" % "; ".join(r["conflicts"]))
    finally:
        s.close()
    shutil.rmtree(os.path.join("/tmp", "patina_face_sets_shots"), ignore_errors=True)
    print("face sets: %s" % ("all checks passed" if not failures else "%d FAILED" % len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
