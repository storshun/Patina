#!/usr/bin/env python3
"""(1.2) Loops and face sets in the real window (a hidden display, synthetic mouse and keys): with the Select tool,
Alt+click by an edge of a torus selects a loop of quads right round it, Ctrl+Alt+click the ring the other way, an
Alt+drag still turns the view; Alt+click in the UV editor selects a loop too; the Face Sets panel makes a set of the
selection, (1.2.10) a click on its row makes it the active set and the buttons under the list add, remove, select and deselect, an emptied set stays, its trash deletes it, its eye hides it from the 3D view, Show All; the Select tool's Connected grows
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
        check(len(sets) == 1 and sets[0]["faces"] == n, "Create from Selection: a face set of it (%s)" % [x["faces"] for x in sets])
        name = sets[0]["name"] if sets else ""
        # (1.2.10) as Blender's vertex groups: a click on the row makes it active, Select adds its faces
        s("select_faces", mode="clear")
        click(s, "facesets.row." + name)
        s.settle(3)
        check(selected(s) == 0, "a click on its row only makes it the active set (%d selected)" % selected(s))
        click(s, "facesets.select")
        s.settle(3)
        check(selected(s) == n, "Select selects its faces (%d)" % selected(s))
        s("select_faces", mode="add", faces=[0, 1])
        click(s, "facesets.deselect")
        s.settle(3)
        check(selected(s) == 2, "Deselect takes only its faces out of the selection (%d left)" % selected(s))
        click(s, "facesets.deselect_all")
        s.settle(3)
        check(selected(s) == 0, "Deselect All clears it")
        # Add Selection, Remove Selection
        s("select_faces", mode="set", faces=[0, 1, 2])
        click(s, "facesets.assign")
        s.settle(3)
        check(s("list_face_sets")["face_sets"][0]["faces"] == n + 3, "Add Selection puts the selected faces in it")
        click(s, "facesets.remove")
        s.settle(3)
        check(s("list_face_sets")["face_sets"][0]["faces"] == n, "Remove Selection takes them out")
        # a new set of all its faces: the first set stays, empty, with its name and color
        before = s("list_face_sets")["face_sets"][0]
        s("use_face_set", set=before["id"], action="select")
        click(s, "btn.facesets.new")
        s.settle(3)
        sets = s("list_face_sets")["face_sets"]
        check(len(sets) == 2 and sets[0]["name"] == before["name"] and sets[0]["color"] == before["color"] and sets[0]["faces"] == 0,
              "Create from Selection of all its faces: the first set stays, empty, with its name and color (%s)"
              % [(x["name"], x["faces"]) for x in sets])
        click(s, "btn.facesets.delete." + name)
        s.settle(3)
        sets = s("list_face_sets")["face_sets"]
        check([x["name"] for x in sets] == [sets[0]["name"]] and name not in [x["name"] for x in sets],
              "its trash button deletes the set (%s)" % [x["name"] for x in sets])
        s("undo")
        s.settle(2)
        name = s("list_face_sets")["face_sets"][0]["name"]
        s("make_face_set", **{"from": "connected"})
        s.settle(2)
        name = s("list_face_sets")["face_sets"][0]["name"]
        # its eye: the check box left of the row
        r = rect(s, "facesets.row." + name)
        box = rect(s, "facesets.list")
        s.x.click(box[0] + 18, (r[1] + r[3]) / 2)
        s.settle(3)
        check(s("list_face_sets")["face_sets"][0]["hidden"], "its eye hides it")
        click(s, "facesets.show_all")
        s.settle(3)
        check(not s("list_face_sets")["face_sets"][0]["hidden"], "Show All, always under the Create buttons, shows it again")
        # ---- (1.2.10) the Blender layout's Properties pin: a tool chosen shows its tab, unless pinned
        s("layout", mode="blender", pin_properties=False, properties_tab="face_sets")
        s.settle(4)
        s("set_tool", tool="paint")
        s.settle(3)
        check(s("layout")["properties_tab"] == "layer", "unpinned: the Paint tool shows the Layer tab")
        s("layout", properties_tab="face_sets")
        s.settle(2)
        click(s, "btn.props.pin")
        check(s("layout")["pin_properties"] is True, "the pin above the tabs pins the Properties tab")
        s("set_tool", tool="select")
        s.settle(3)
        s("set_tool", tool="paint")
        s.settle(3)
        check(s("layout")["properties_tab"] == "face_sets", "pinned: Select, then Paint, and the Face Sets tab stays")
        click(s, "btn.props.pin")
        check(s("layout")["pin_properties"] is False, "a second click unpins it")
        s("layout", mode="full")
        s("set_tool", tool="select")
        s.settle(4)
        # ---- Connected
        s("use_face_set", action="show_all")
        s("select_faces", mode="set", faces=[0])
        click(s, "btn.select.linked")
        s.settle(3)
        check(selected(s) == 1536, "Connected grows the selection to the whole torus (%d)" % selected(s))
        # (1.2.10) A in the 3D view, with any tool: all, then none
        s("set_tool", tool="paint")
        s.x.move(*vp(0.5, 0.5))
        time.sleep(0.3)
        s.x.key("a")
        s.settle(3)
        check(selected(s) == 0, "A with some faces selected: none (Paint tool) (%d)" % selected(s))
        s.x.key("a")
        s.settle(3)
        check(selected(s) == 1536, "A again: every face (%d)" % selected(s))
        s.x.key("Alt_L", True)
        s.x.key("a")
        s.x.key("Alt_L", False)
        s.settle(3)
        check(selected(s) == 0, "Alt+A: none (%d)" % selected(s))
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
