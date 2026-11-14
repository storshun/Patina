#!/usr/bin/env python3
"""Solo (1.2.19), in the real window: one piece of the mesh (a UV island of the cube) shown alone, and painted alone:
- the solo command shows the island under a point alone; the faces of the others leave picking;
- a stroke across the soloed face and its neighbor paints the soloed face only (seen once everything shows again),
  and so does a fill of the whole mesh;
- one undo step each; kept in the saved project;
- the Solo picker: a click shows the piece alone and paints nothing, Ctrl+click hides it; the bar over the view says
  so, with Show All; Shift+H over the model shows the piece alone, Alt+H everything.

    python3 tests/gui/test_solo.py [path/to/patina]
"""
import os
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import manual_shots  # noqa: E402
from manual_scenes import rect  # noqa: E402

BIN = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "patina")
OUT = os.environ.get("PATINA_TEST_OUT") or os.path.join("/tmp", "patina_solo_test")
failures = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        failures.append(what)


def red(c):
    return c[0] > 0.6 and c[1] < 0.3 and c[2] < 0.3


def green(c):
    return c[1] > 0.6 and c[0] < 0.3 and c[2] < 0.3


def main():
    s = manual_shots.Session(BIN, OUT, display=":94", port=7911)
    tmp = tempfile.mkdtemp(prefix="patina_solo_")
    try:
        def scene():
            s("new_project", primitive="cube", resolution=1024)
            s("set_camera", view="persp", frame=True)
            s("set_camera", yaw=0.6, pitch=0.45)
            s.settle(5, 0.5)

        def state():
            return s("get_state")["solo_pieces"]

        def color(pt):
            return s("pick", point=pt)["color"]

        scene()
        # two points on different islands, well inside their faces
        pts = {}
        for y in range(30, 71, 4):
            for x in range(30, 71, 4):
                h = s("pick_face", point=[x / 100, y / 100])
                if h.get("hit"):
                    pts.setdefault(h["island"], []).append([x / 100, y / 100])
        isl = sorted(pts, key=lambda k: -len(pts[k]))[:2]
        check(len(isl) == 2, "the cube shows two islands to work with (%d)" % len(pts))
        A = pts[isl[0]][len(pts[isl[0]]) // 2]
        B = pts[isl[1]][len(pts[isl[1]]) // 2]

        # ---- solo by command
        r = s("solo", action="only", point=A)
        check(r["hidden_faces"] > 0 and r["shown_faces"] == r["piece_faces"],
              "solo shows the island alone (%d shown, %d hidden)" % (r["shown_faces"], r["hidden_faces"]))
        check(not s("pick_face", point=B).get("hit"), "the hidden island leaves picking")
        s.settle(5, 0.5)
        s.shot("solo_alone")

        # a stroke across both: only the shown one takes paint
        s("set_brush", size=160, hardness=1, spacing=0.05, projection="screen", lazy_radius=0)
        s("set_paint_material", id="", color=[1, 0, 0])
        s("paint_stroke", points=[[A[0] + (B[0] - A[0]) * k / 20, A[1] + (B[1] - A[1]) * k / 20] for k in range(21)])
        r = s("solo", action="show_all")
        check(r["hidden_faces"] == 0, "show_all shows everything")
        s.settle(5, 0.5)
        ca, cb = color(A), color(B)
        check(red(ca) and not red(cb), "the stroke painted the soloed island only (A %s, B %s)" %
              ([round(v, 2) for v in ca[:3]], [round(v, 2) for v in cb[:3]]))
        s.shot("solo_painted")
        # undo: everything hidden again
        s("undo")
        check(state()["hidden_faces"] > 0, "undo hides the pieces again")
        # a fill of the whole mesh fills the shown faces only
        s("fill", mode="mesh", material={"id": "", "color": [0, 1, 0]})
        s("solo", action="show_all")
        s.settle(5, 0.5)
        check(green(color(A)) and not green(color(B)), "a fill of the whole mesh fills the shown island only")

        # ---- kept in the project
        s("solo", action="hide", point=B)
        hid = state()["hidden_faces"]
        path = os.path.join(tmp, "solo.patina")
        s("save_project", path=path)
        s("new_project", primitive="sphere", resolution=512)
        s("open_file", path=path)
        check(state()["hidden_faces"] == hid > 0, "the project keeps what is hidden (%d faces)" % state()["hidden_faces"])
        s("solo", action="show_all")

        # ---- the picker and the keys, in the window
        scene()
        view = rect(s, "viewport.image")
        vw, vh = view[2] - view[0], view[3] - view[1]
        at = lambda p: (view[0] + p[0] * vw, view[1] + p[1] * vh)  # noqa: E731
        s("solo", picking=True)
        s.settle(5, 0.5)
        hist = s("history")["position"]
        s.x.click(*at(A))
        s.settle(5, 0.5)
        st = state()
        check(st["hidden_faces"] > 0 and st["picking"], "a click with the picker shows the piece alone")
        check(s("history")["position"] == hist + 1, "the click paints nothing (one Solo undo step)")
        bar = s("ui_rect", name="solo.bar")
        check(bar.get("found"), "the Solo bar shows over the view")
        s.shot("solo_picker")
        s.x.click(*at(B))  # (B hidden: not on the model, nothing changes)
        s.x.key("Escape")
        s.settle(5, 0.5)
        check(not state()["picking"], "Esc ends picking; the piece stays alone (%d hidden)" % state()["hidden_faces"])
        sa = s("ui_rect", name="solo.show_all")
        s.x.click(sa["x"] + sa["w"] / 2, sa["y"] + sa["h"] / 2)
        s.settle(5, 0.5)
        check(state()["hidden_faces"] == 0, "Show All on the bar shows everything")
        # Shift+H over B, then Alt+H
        s.x.move(*at(B))
        s.settle(3, 0.3)
        s.x.key("Shift_L", True)
        s.x.key("h")
        s.x.key("Shift_L", False)
        s.settle(5, 0.5)
        h1 = state()["hidden_faces"]
        s.x.key("Alt_L", True)
        s.x.key("h")
        s.x.key("Alt_L", False)
        s.settle(5, 0.5)
        check(h1 > 0 and state()["hidden_faces"] == 0, "Shift+H shows the piece under the pointer alone, Alt+H everything")
    finally:
        s.close()
    print("solo: %s" % ("all checks passed" if not failures else "%d FAILED" % len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
