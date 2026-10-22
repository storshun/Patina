#!/usr/bin/env python3
"""More procedural materials and masks (0.24) in the real window, with synthetic mouse drags:
- the material editor names a generator's own settings (Bond for bricks, Style for checks);
- a material dragged onto the mask's "Add a mask effect" box of a layer gives it a Pattern effect from that
  material (the simple mask becomes a stack, the pattern multiplied on it), its settings open;
- another material dragged onto the Pattern's Material box replaces its pattern, and adds nothing;
- one undo step takes the Pattern effect away again.

    python3 tests/gui/test_procedural.py [path/to/patina]
"""
import os
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import manual_shots  # noqa: E402
from manual_scenes import layout, panel  # noqa: E402

BIN = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "patina")
failures = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        failures.append(what)


def main():
    s = manual_shots.Session(BIN, os.path.join("/tmp", "patina_procedural_test"), display=":97", port=7899)

    def rect(name):
        r = s("ui_rect", name=name)
        return r if r.get("found") else None

    def center(r, fy=0.5):
        return int(r["x"] + r["w"] / 2), int(r["y"] + r["h"] * fy)

    def drag(a, b):
        pts = [a, (a[0] + 6, a[1] + 6)] + [(int(a[0] + (b[0] - a[0]) * k / 12), int(a[1] + (b[1] - a[1]) * k / 12)) for k in range(1, 13)] + [b]
        s.x.drag(pts, dt=0.06)
        time.sleep(0.8)

    def selected():
        return next(l for l in s("list_layers") if l["selected"])

    try:
        s("new_project", primitive="sphere", resolution=512)
        layout(s)
        s("set_camera", view="front", frame=True)
        panel(s, "properties")
        panel(s, "materials")
        s("add_material_layer", material="patina_rust_heavy")
        time.sleep(1.0)
        # (the layer's settings scrolled down to its mask)
        s.x.move(1390, 330)
        for _ in range(16):
            s.x.button(5, True); s.x.button(5, False); time.sleep(0.03)
        time.sleep(0.8)
        tiles = sorted(s("ui_rect", prefix="materials.tile:")["rects"].items(), key=lambda kv: (kv[1]["y"], kv[1]["x"]))
        check(len(tiles) >= 3, "the material tiles are there (%d)" % len(tiles))
        (k0, t0), (k1, t1) = tiles[0], tiles[1]
        id0, id1 = k0.split(":", 1)[1], k1.split(":", 1)[1]
        add = rect("mask.add_effect")
        check(add is not None and 40 < add["y"] < 440, "the mask's Add a mask effect box is in view")
        if add:
            drag(center(t0), center(add))
            fx = selected().get("mask_effects", [])
            pats = [e for e in fx if e["type"] == "pattern"]
            check(len(pats) == 1 and pats[0]["material"]["id"] == id0, "a material dropped on the mask: a Pattern effect from it (%s)" % [e["type"] for e in fx])
            check(len(pats) == 1 and pats[0]["blend"] == "multiply" and fx[0]["type"] == "paint", "multiplied on the mask as it was")
            time.sleep(0.6)
            s.x.move(1390, 330)
            for _ in range(16):
                s.x.button(5, True); s.x.button(5, False); time.sleep(0.03)
            time.sleep(0.8)
            box = rect("mask.pattern_material")
            check(box is not None and 40 < box["y"] < 440, "its settings are open, the Material box in view")
            if box:
                drag(center(t1), center(box))
                fx2 = selected().get("mask_effects", [])
                pats = [e for e in fx2 if e["type"] == "pattern"]
                check(len(fx2) == len(fx) and len(pats) == 1 and pats[0]["material"]["id"] == id1,
                      "another material dropped on the Material box replaces the pattern (%s)" % (pats[0]["material"]["id"] if pats else None))
                s.x.key("Control_L", True); s.x.key("z"); s.x.key("Control_L", False)
                time.sleep(0.8)
                pats = [e for e in selected().get("mask_effects", []) if e["type"] == "pattern"]
                check(len(pats) == 1 and pats[0]["material"]["id"] == id0, "one undo step puts the first pattern back")
            s.x.key("Control_L", True); s.x.key("z"); s.x.key("Control_L", False)
            time.sleep(0.8)
            sel = selected()
            check(sel["name"] == "Rust Heavy" and not any(e["type"] == "pattern" for e in sel.get("mask_effects", [])),
                  "another takes the Pattern effect away (%s: %s)" % (sel["name"], [e["type"] for e in sel.get("mask_effects", [])]))
        # the editor names the settings
        s("open_window", window="material_editor", generator="bricks")
        time.sleep(1.2)
        r1 = rect("editor.extra:1")
        check(r1 is not None and r1["w"] > 20, "the material editor draws the bricks' own settings (Bond)")
    finally:
        s.close()
    print("procedural: %d failed" % len(failures) if failures else "procedural: all checks passed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
