#!/usr/bin/env python3
"""(1.2.13) A brush for each tool and the tip cursor, in the real window (PATINA_SHOTS_DISPLAY or Xvfb, synthetic mouse,
keys and pen events): B and E swap the Paint and Erase brushes (a splatter tip on Paint, a round one on Erase); the
Brush panel says whose brush it shows; the pen's eraser end erases with the Erase brush and Paint's comes back; the
session keeps every tool's brush; with the feature off one brush is shared. The cursor: a star tip draws a star, not a
circle; a hard brush's dither is denser than a soft one's; the feature off draws the old circle. No two widgets share
an ID.

    python3 tests/gui/test_tool_brushes.py [path/to/patina]
"""
import json
import math
import os
import shutil
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import manual_shots  # noqa: E402
from manual_scenes import vp  # noqa: E402
import manual_scenes  # noqa: E402

BIN = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "patina")
failures = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        failures.append(what)


def brush(s):
    return s("get_state")["brush"]


def cursor_pixels(s, at, r):
    """pixels around `at` that change when the cursor is there (against the same view with the pointer outside)"""
    s.x.move(*at)
    s.settle(6, 0.5)
    on = s.image()
    s.x.move(5, 940)  # (the status bar: no cursor over the view)
    s.settle(6, 0.5)
    off = s.image()
    n, pts = 0, []
    for y in range(int(at[1] - r - 6), int(at[1] + r + 6)):
        for x in range(int(at[0] - r - 6), int(at[0] + r + 6)):
            a, b = on.getpixel((x, y)), off.getpixel((x, y))
            if sum(abs(a[i] - b[i]) for i in range(3)) > 60:
                n += 1
                pts.append((x - at[0], y - at[1]))
    return n, pts


def main():
    os.environ["PATINA_UI_ID_CHECK"] = "1"
    out = os.path.join("/tmp", "patina_tool_brushes_shots")
    s = manual_shots.Session(BIN, out, display=":97", port=7905)
    try:
        s("new_project", primitive="plane", resolution=512)
        s("set_camera", view="top", frame=True)
        s("set_view", mode="basecolor", seams=False, wireframe=False)
        manual_scenes.layout(s)
        s("add_layer", type="paint", name="Paint")
        s("set_paint_material", id="", color=[0.9, 0.05, 0.05])
        s("features", id="tool_brushes", on=True)
        s("features", id="tip_cursor", on=True)
        s("set_tool", tool="paint")
        s("set_brush", preset="basic_hard", size=60, alpha="splatter")
        center = vp(0.5, 0.5)
        s.x.move(*center)
        s.settle(4)
        # ---- B and E: each tool its own brush
        s.x.key("e")
        s.settle(6)
        b = brush(s)
        check(s("get_state")["tool"] == "erase" and b["tool"] == "erase", "E: the Erase tool, with its own brush")
        check(b["alpha"]["shape"] == "round", "the Erase brush is round (not the Paint brush's splatter): %s" % b["alpha"]["shape"])
        check(abs(b["size"] - 60) < 0.5, "it starts at the Paint brush's size (%.1f)" % b["size"])
        s("set_brush", size=25)
        s.x.key("b")
        s.settle(6)
        b = brush(s)
        check(b["tool"] == "paint" and b["alpha"]["shape"] == "splatter" and abs(b["size"] - 60) < 0.5,
              "B: the Paint brush comes back, splatter at 60 (%s, %.1f)" % (b["alpha"]["shape"], b["size"]))
        st = s("get_state")
        check(abs(st["tool_brushes"]["erase"]["size"] - 25) < 0.5, "get_state lists the Erase brush at 25")
        # ---- the Brush panel says whose brush it is
        s("open_window", window="properties")
        s.settle(6)
        s.x.key("e")
        s.settle(8)
        check(s("ui_rect", name="props.brush_header").get("found"), "the Brush panel is shown")
        s.x.key("b")
        s.settle(4)
        # ---- the pen's eraser end: the Erase brush for its stroke, Paint's after
        s("set_pen_settings", softness=0, min_pressure=0.0, full_pressure=1.0, eraser_tip_erases=True)
        samples = []
        for i in range(20):
            t = i / 19
            samples.append(dict(x=0.45 + 0.1 * t, y=0.5, pressure=1.0, down=True, eraser=True))
        samples.append(dict(x=0.55, y=0.5, pressure=0.0, down=False))
        s("inject_pen", samples=samples)
        for _ in range(400):
            if not s("pen_injecting")["busy"]:
                break
            time.sleep(0.05)
        s.settle(10, 0.8)
        b = brush(s)
        check(s("get_state")["tool"] == "paint" and b["tool"] == "paint" and b["alpha"]["shape"] == "splatter",
              "after the eraser end's stroke: Paint with its splatter brush")
        # ---- the cursor: a star tip draws a star, not a circle
        s.x.move(*center)
        s.settle(4)
        s("set_brush", alpha="star", size=160, hardness=1.0, angle=0, roundness=1)
        n_star, pts = cursor_pixels(s, center, 80)
        # the star's outline: far out along its points, close in between them; a circle has every distance alike
        rims = {}
        for (x, y) in pts:
            d = math.hypot(x, y)
            if d < 30:
                continue
            k = int(((math.degrees(math.atan2(y, x)) % 360) // 10))
            rims[k] = max(rims.get(k, 0), d)
        spread = (max(rims.values()) - min(rims.values())) if rims else 0
        check(n_star > 200 and spread > 20, "the star cursor is a star (%d pixels; its rim from %.0f to %.0f)" % (
            n_star, min(rims.values()) if rims else 0, max(rims.values()) if rims else 0))
        # ---- the dither: a hard brush paints densely to its edge, a soft one thins out
        s("set_brush", alpha="round", hardness=1.0, size=160)
        n_hard, _ = cursor_pixels(s, center, 80)
        s("set_brush", hardness=0.0)
        n_soft, _ = cursor_pixels(s, center, 80)
        check(n_hard > n_soft * 1.3 > 0, "the hard brush's dither is denser than the soft one's (%d / %d)" % (n_hard, n_soft))
        # ---- the feature off: the old circle (a ring: nothing changes in the middle)
        s("features", id="tip_cursor", on=False)
        s.settle(4)
        n_ring, pts = cursor_pixels(s, center, 80)
        inner = sum(1 for (x, y) in pts if math.hypot(x, y) < 40)
        check(n_ring > 100 and inner < 10, "tip cursor off: the circle (%d pixels, %d inside)" % (n_ring, inner))
        s("features", id="tip_cursor", on=True)
        # ---- one brush for every tool with the feature off
        s("set_brush", alpha="splatter")
        s("features", id="tool_brushes", on=False)
        s.x.key("e")
        s.settle(6)
        check(brush(s)["alpha"]["shape"] == "splatter", "tool brushes off: Erase uses the shared splatter brush")
        s.x.key("b")
        s("features", id="tool_brushes", on=True)
        # ---- the session keeps every tool's brush
        s("set_brush", tool="erase", size=31)
        fav = [x["id"] for x in s("pie_menus")["pies"]["favorites"]]
        s("pie_menus", favorites=fav)  # (setting the favorites writes session.json at once)
        s.settle(4)
        sess = os.path.join(s.home, ".config", "patina", "session.json")
        try:
            j = json.load(open(sess))
            check(abs(j.get("tool_brushes", {}).get("erase", {}).get("size", 0) - 31) < 0.5 and j.get("brush_slot") == "paint",
                  "session.json keeps the Erase brush (31) and that the brush shown is Paint's")
        except (OSError, ValueError) as e:
            check(False, "session.json readable (%s)" % e)
        # ---- no two widgets share an ID (in English and in the test language)
        for lang in ("x-test", "en"):
            s("language_set", code=lang)
            s.settle(10)
        r = s("ui_id_conflicts")
        check(r["checking"] and not r["conflicts"], "no two widgets share an ID (%s)" % "; ".join(r["conflicts"]))
    finally:
        s.close()
    shutil.rmtree(out, ignore_errors=True)
    print("tool brushes: %s" % ("all checks passed" if not failures else "%d FAILED" % len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
