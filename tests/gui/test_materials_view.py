#!/usr/bin/env python3
"""The Materials panel's tiles and list (1.2.3), in the real window:
- every tile fits inside the panel (the last column ran off the side before: the tiles' padding and spacing weren't
  counted), at the start and after Ctrl+mouse wheel made them bigger;
- the List button shows a row each, as wide as the panel, with a small picture; a click on a row paints with that
  material; the Smart materials tab shows its rows too;
- the view and the tiles' size are kept in the session.

    python3 tests/gui/test_materials_view.py [path/to/patina]
"""
import json
import os
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import manual_shots  # noqa: E402
from manual_scenes import panel  # noqa: E402

BIN = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "patina")
failures = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        failures.append(what)


def main():
    s = manual_shots.Session(BIN, os.path.join("/tmp", "patina_materials_view_test"), display=":97", port=7898)

    def rect(name):
        r = s("ui_rect", name=name)
        return r if r.get("found") else None

    def tiles(prefix="materials.tile:"):
        return s("ui_rect", prefix=prefix)["rects"]

    def inside(rs, win):
        return all(r["x"] >= win["x"] - 1 and r["x"] + r["w"] <= win["x"] + win["w"] + 1 for r in rs.values())

    try:
        s("new_project", primitive="sphere", resolution=512)
        panel(s, "materials")
        time.sleep(2.0)
        win = rect("window.Materials")
        t = tiles()
        check(len(t) >= 4 and inside(t, win), "%d tiles, all inside the panel (%.0f..%.0f; rightmost %.0f)" %
              (len(t), win["x"], win["x"] + win["w"], max(r["x"] + r["w"] for r in t.values())))
        w0 = next(iter(t.values()))["w"]

        # Ctrl+mouse wheel over the tiles: bigger, still inside
        first = min(t.values(), key=lambda r: (r["y"], r["x"]))
        s.x.move(int(first["x"] + first["w"] / 2), int(first["y"] + first["h"] / 2))
        s.x.key("Control_L", True)
        for _ in range(4):
            s.x.button(4, True); s.x.button(4, False); time.sleep(0.05)
        s.x.key("Control_L", False)
        time.sleep(0.8)
        t = tiles()
        w1 = next(iter(t.values()))["w"]
        check(w1 > w0 * 1.3, "Ctrl+mouse wheel makes the tiles bigger (%.0f -> %.0f)" % (w0, w1))
        check(inside(t, win), "and they still fit (rightmost %.0f)" % max(r["x"] + r["w"] for r in t.values()))

        # the list: a row each, as wide as the panel; a click paints with that material
        b = rect("btn.materials.view_list")
        s.x.click(int(b["x"] + b["w"] / 2), int(b["y"] + b["h"] / 2))
        time.sleep(0.8)
        t = tiles()
        rows = sorted(t.items(), key=lambda kv: kv[1]["y"])
        check(len(rows) >= 3 and all(r["w"] > win["w"] * 0.8 and r["h"] < w1 for _, r in rows),
              "the list: rows as wide as the panel, lower than the tiles (%d rows)" % len(rows))
        pick_id, pick = rows[1]
        pick_id = pick_id.split(":", 1)[1]
        s.x.click(int(pick["x"] + pick["w"] / 2), int(pick["y"] + pick["h"] / 2))
        time.sleep(0.6)
        check(s("get_state")["material"]["id"] == pick_id, "a click on a row paints with it (%s)" % pick_id)

        tab = rect("materials.tab:smart")
        s.x.click(int(tab["x"] + tab["w"] / 2), int(tab["y"] + tab["h"] / 2))
        time.sleep(1.0)
        st = tiles("smart.tile:")
        check(len(st) >= 2 and all(r["w"] > win["w"] * 0.8 for r in st.values()), "the smart materials as a list too (%d)" % len(st))

        # kept in the session (written every few seconds)
        time.sleep(6.0)
        sess = json.load(open(os.path.join(s.home, ".config", "patina", "session.json")))
        check(sess.get("materials_list") is True and sess.get("materials_tile", 0) > 84,
              "the view and the size are kept (%s, %s)" % (sess.get("materials_list"), sess.get("materials_tile")))
    finally:
        s.close()
    print("materials view: %s" % ("all checks passed" if not failures else "%d FAILED" % len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
