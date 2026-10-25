#!/usr/bin/env python3
"""(0.40) The pie menus and the brush keys, in the real window (Xvfb or PATINA_SHOTS_DISPLAY, synthetic mouse and
keys): ` the edit modes, Shift+Z the channels, Q Quick Favorites (the defaults, an empty place's picker, right-click
a pie item or a toolbar button to add or take out), MCP pie_menus; with the Blender keymap G / Shift+G / Ctrl+F set
the brush's flow, hardness and angle by moving the mouse (F / Shift+F size and opacity, as before).

    python3 tests/gui/test_pies.py [path/to/patina]
"""
import os
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import manual_shots  # noqa: E402
from manual_scenes import center, layout, type_text, vp  # noqa: E402

BIN = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "patina")
failures = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        failures.append(what)


def main():
    s = manual_shots.Session(BIN, os.path.join("/tmp", "patina_pies_test"), display=":97", port=7891)
    try:
        s("new_project", primitive="sphere", resolution=512)
        layout(s)
        s("open_window", window="preferences", keymap="blender")  # (the brush keys are the Blender keymap's)
        time.sleep(0.5)
        s.x.move(*vp(0.5, 0.55))
        time.sleep(0.2)
        s.x.key("Escape")
        time.sleep(0.3)
        s("set_tool", tool="paint")
        s("set_camera", view="persp", frame=True, ortho=False)
        steps = lambda: len(s("history")["steps"])
        tool = lambda: s("get_state")["window_tool"]
        mode = lambda: s("set_view")["mode"]
        favs = lambda: [i["id"] for i in s("pie_menus")["pies"]["favorites"]]
        n0 = steps()
        # ---- MCP: the pies and their keys
        r = s("pie_menus", catalog=True)
        check([i["id"] for i in r["pies"]["view"]] == ["view.wireframe", "view.solid", "view.lit", "view.path_traced", "view.unlit",
                                                       "view.seams", "mode.uv_checker", "view.frame_selected"], "the view pie's items")
        check(r["keys"] == {"view": "Z", "channels": "Shift+Z", "edit": "`", "favorites": "Q"}, "the keys (%s)" % r["keys"])
        ids = {c["id"] for c in r["catalog"]}
        check({"tool.paint", "mode.roughness", "view.wireframe", "sym.x", "file.render", "brush.flow"} <= ids and
              "file.quit" not in ids and "view.pie" not in ids, "the catalog (%d items)" % len(ids))
        check(favs()[:6] == ["select.none", "island.make", "sym.x", "color.swap", "view.back", "file.render"] and favs()[6:] == ["", ""],
              "Quick Favorites start with six, two places empty (%s)" % favs())

        def pie(key, slot, hold=True, shift=False):
            s.x.move(*vp(0.5, 0.55))
            time.sleep(0.3)
            if shift:
                s.x.key("Shift_L", True)
            if hold:
                s.x.key(key, True)
                time.sleep(0.35)
            else:
                s.x.key(key)
                time.sleep(0.4)
            if shift:
                s.x.key("Shift_L", False)
            x, y = center(s, "pie.%d" % slot)
            s.x.move(x, y)
            time.sleep(0.3)
            if hold:
                s.x.key(key, False)
            else:
                s.x.button(1, True)
                time.sleep(0.08)
                s.x.button(1, False)
            time.sleep(0.4)

        # ---- the edit mode pie: ` (the key under Esc)
        pie("grave", 1)
        check(tool() == "select", "` pie, held: Select (right)")
        pie("grave", 2, hold=False)
        check(tool() == "island", "tapped then clicked: Island (down)")
        pie("grave", 3)
        check(tool() == "seams", "Seams (up)")
        pie("grave", 6)
        check(tool() == "zone_lines", "Retopo lines (down left)")
        pie("grave", 7)
        check(tool() == "flow", "Retopo edge loops (down right)")
        pie("grave", 5)
        check(tool() == "fill", "Fill (up right)")
        pie("grave", 4)
        check(tool() == "erase", "Erase (up left)")
        pie("grave", 0)
        check(tool() == "paint", "Paint (left)")
        # ---- the channel pie: Shift+Z
        pie("z", 2, shift=True)
        check(mode() == "roughness", "Shift+Z pie: Roughness (down)")
        pie("z", 1, shift=True)
        check(mode() == "normal", "Normal (right)")
        pie("z", 1, shift=True)
        check(mode() == "roughness", "again: the view before")
        s("set_view", mode="material")
        # ---- Quick Favorites: Q
        s("select_faces", mode="all")
        pie("q", 0)
        check(s("select_faces", mode="add", faces=[])["selected"] == 0, "Q pie: Clear selection (left)")
        x0 = s("pie_menus")["pies"]["favorites"][2]["on"]
        pie("q", 2)
        check(s("pie_menus")["pies"]["favorites"][2]["on"] != x0, "X mirror (down)")
        pie("q", 2)
        # an empty place: the picker, search, Enter
        pie("q", 6)
        time.sleep(0.3)
        type_text(s, "wireframe")
        time.sleep(0.2)
        s.x.key("Return")
        time.sleep(0.4)
        check(favs()[6] == "view.wireframe", "an empty place opens the picker: Wireframe chosen (%s)" % favs()[6])
        w0 = s("set_view")["wireframe"]
        pie("q", 6)
        check(s("set_view")["wireframe"] != w0, "and the Q pie runs it")
        pie("q", 6)
        # right-click an item of another pie: Add to Quick Favorites
        s.x.move(*vp(0.5, 0.55))
        time.sleep(0.3)
        s.x.key("z")
        time.sleep(0.4)
        s.x.click(*center(s, "pie.1"), b=3)
        time.sleep(0.3)
        s.x.click(*center(s, "menu.favorite"))
        check(favs()[7] == "view.solid", "right-click Solid in the Z pie > Add to Quick Favorites (%s)" % favs()[7])
        # full now: a toolbar button's menu says so; the Wireframe button's offers to take it out
        s.x.click(*center(s, "btn.wire"), b=3)
        time.sleep(0.3)
        s.x.click(*center(s, "menu.favorite"))
        check("view.wireframe" not in favs(), "right-click the Wireframe button > Remove from Quick Favorites")
        s.x.click(*center(s, "btn.lock_island"), b=3)
        time.sleep(0.3)
        s.x.click(*center(s, "menu.favorite"))
        check(favs()[6] == "brush.lock_island", "a toolbar button > Add to Quick Favorites fills the empty place (%s)" % favs())
        # right-click a Quick Favorite: Remove
        s.x.move(*vp(0.5, 0.55))
        time.sleep(0.3)
        s.x.key("q")
        time.sleep(0.4)
        s.x.click(*center(s, "pie.3"), b=3)
        time.sleep(0.3)
        s.x.click(*center(s, "menu.favorite"))
        check(favs()[3] == "", "right-click a Quick Favorite > Remove (%s)" % favs())
        # MCP: set them, run one, a bad id
        r = s("pie_menus", favorites=["tool.blur", "mode.height"])
        check([i["id"] for i in r["pies"]["favorites"]][:3] == ["tool.blur", "mode.height", ""], "pie_menus {favorites}")
        s("pie_menus", run="mode.height")
        check(mode() == "height", "pie_menus {run}")
        s("pie_menus", run="view.lit")
        try:
            s("pie_menus", favorites=["no.such"])
            check(False, "an unknown id is refused")
        except Exception as e:
            check("unknown pie item" in str(e), "an unknown id is refused (%s)" % str(e)[:60])
        # ---- the brush keys (Blender keymap): move the mouse, click to keep, right-click to go back
        s("set_brush", flow=0.5, hardness=0.5, opacity=0.5, angle=0)
        brush = lambda: s("set_brush")
        p = vp(0.45, 0.5)

        def radial(key, dx, dy, confirm=True, mods=()):
            s.x.move(*p)
            time.sleep(0.3)
            for m in mods:
                s.x.key(m, True)
            time.sleep(0.1)  # (the modifier down first, as a hand does)
            s.x.key(key)
            for m in mods:
                s.x.key(m, False)
            time.sleep(0.2)
            for i in range(1, 6):
                s.x.move(p[0] + dx * i / 5, p[1] + dy * i / 5)
                time.sleep(0.04)
            time.sleep(0.2)
            s.x.click(p[0] + dx, p[1] + dy, b=1 if confirm else 3)
            time.sleep(0.3)

        radial("g", 30, 0)
        b = brush()
        check(0.6 < b["flow"] < 0.95, "G: further out, more flow (%.2f)" % b["flow"])
        radial("g", -30, 0, mods=("Shift_L",))
        b = brush()
        check(0.1 < b["hardness"] < 0.4, "Shift+G: nearer, less hardness (%.2f)" % b["hardness"])
        radial("g", 40, 0, confirm=False, mods=("Shift_L",))
        check(abs(brush()["hardness"] - b["hardness"]) < 1e-4, "right-click keeps the hardness it had")
        radial("f", 0, -50, mods=("Control_L",))
        a = brush()["angle"]
        check(8 < a < 60, "Ctrl+F (after a right-click cancel, which opens no menu): the angle turns counterclockwise (%.1f)" % a)
        radial("f", 30, 0, mods=("Shift_L",))
        check(brush()["opacity"] > 0.6, "Shift+F: opacity (%.2f)" % brush()["opacity"])
        check(steps() <= n0 + 2, "no painting from any of it (%d -> %d history steps)" % (n0, steps()))
    finally:
        s.close()
    print("pies: %d failed" % len(failures) if failures else "pies: all checks passed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
