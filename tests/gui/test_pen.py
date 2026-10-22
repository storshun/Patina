#!/usr/bin/env python3
"""Pen tablet input tests in the real window (Xvfb), with synthetic SDL pen events.

    python3 tests/gui/test_pen.py [path/to/patina]

The events follow what SDL3 sends for a real tablet: pen down / motion / axis / up events
plus the mouse events SDL synthesizes from them (which = SDL_PEN_MOUSEID).
"""
import os
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import manual_shots  # noqa: E402

BIN = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "patina")
failures = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        failures.append(what)


def wait_injected(s, extra=0.8):
    for _ in range(400):
        if not s("pen_injecting")["busy"]:
            break
        time.sleep(0.05)
    time.sleep(extra)


def stroke(y, x0=0.2, x1=0.8, n=60, p0=1.0, p1=1.0, **kw):
    pts = []
    for i in range(n):
        t = i / (n - 1)
        pts.append(dict(x=x0 + (x1 - x0) * t, y=y, pressure=p0 + (p1 - p0) * t, down=True, **kw))
    pts.append(dict(x=x1, y=y, pressure=0.0, down=False, **{k: v for k, v in kw.items() if k == "mouse"}))
    return pts


def red_extent(img, vx, y_lo, y_hi):
    """height in pixels of the red run in column vx between y_lo and y_hi"""
    n = 0
    for y in range(y_lo, y_hi):
        r, g, b = img.getpixel((vx, y))[:3]
        if r > 150 and g < 90 and b < 90:
            n += 1
    return n


def main():
    out = os.path.join("/tmp", "patina_pen_test")
    s = manual_shots.Session(BIN, out)
    try:
        s("new_project", primitive="plane", resolution=1024)
        s("set_camera", view="top", frame=True)
        s("set_view", mode="basecolor", seams=False, wireframe=False)
        s("add_layer", type="paint", name="Pen")
        s("set_tool", tool="paint")
        s("set_paint_material", id="", color=[0.9, 0.05, 0.05])
        s("set_brush", preset="basic_hard", size=40, hardness=0.9, flow=1, opacity=1, spacing=0.05,
          projection="screen", lock_topology=False, lock_island=False)
        s("set_brush", pressure_size=True, pressure_flow=False, pressure_opacity=False)
        s("set_pen_settings", softness=0, min_pressure=0.0, full_pressure=1.0, eraser_tip_erases=True,
          button1="right", button2="middle")
        st = s("get_state")
        vp = st.get("viewport", {})
        print("viewport", vp)

        # 1. pressure ramp: thin at the start, thick at the end
        s("inject_pen", samples=stroke(0.5, p0=0.1, p1=1.0))
        wait_injected(s)
        img = s.image()
        info = s("pen_info")
        check(info["pen_seen"] and info["events"]["motion"] >= 60, "pen events arrive (%d motion)" % info["events"]["motion"])
        # the 3D view's rectangle in window pixels (as the app reports it)
        x0, y0 = [int(round(v)) for v in info["viewport_origin"]]
        vw, vh = info["viewport_size"]
        x1, y1 = x0 + vw, y0 + vh
        cy = int(y0 + 0.5 * (y1 - y0))

        def col(fx):
            return int(x0 + fx * (x1 - x0))

        thin = red_extent(img, col(0.27), cy - 40, cy + 40)
        thick = red_extent(img, col(0.73), cy - 40, cy + 40)
        check(thick > 0 and thin > 0, "the stroke was painted (thin %d px, thick %d px)" % (thin, thick))
        check(thick > 2.2 * thin, "pressure sets the width: %d px at low, %d px at high pressure" % (thin, thick))
        hist = s("history")["steps"]
        check(hist[-1].startswith("Paint"), "one undo step for the stroke (%s)" % hist[-1])

        # 2. the eraser end erases (tool stays Paint)
        s("inject_pen", samples=stroke(0.5, x0=0.45, x1=0.55, n=20, eraser=True))
        wait_injected(s)
        img = s.image()
        check(red_extent(img, col(0.5), cy - 40, cy + 40) == 0, "the eraser end erased the middle")
        check(red_extent(img, col(0.73), cy - 40, cy + 40) > 0, "the rest of the stroke is still there")
        check(s("get_state").get("tool") == "paint", "the tool is still Paint after the eraser stroke")

        # 3. a mouse stroke right after the pen: full width everywhere
        s("inject_pen", samples=stroke(0.3, n=30, mouse=True))
        wait_injected(s)
        img = s.image()
        cy3 = int(y0 + 0.3 * (y1 - y0))
        a, b = red_extent(img, col(0.27), cy3 - 40, cy3 + 40), red_extent(img, col(0.73), cy3 - 40, cy3 + 40)
        check(a > 0 and abs(a - b) <= 3 and a >= thick - 4, "mouse strokes get full pressure (%d / %d px)" % (a, b))
        check(s("pen_info")["state"]["last_input"] == "mouse", "the last input is reported as the mouse")

        # 4. the pressure curve: softer makes light pressure count more
        s("set_pen_settings", softness=1.0)
        s("inject_pen", samples=stroke(0.7, p0=0.25, p1=0.25, n=30))
        wait_injected(s)
        img = s.image()
        cy4 = int(y0 + 0.7 * (y1 - y0))
        soft = red_extent(img, col(0.5), cy4 - 40, cy4 + 40)
        s("undo")
        s("set_pen_settings", softness=0.0)
        s("inject_pen", samples=stroke(0.7, p0=0.25, p1=0.25, n=30))
        wait_injected(s)
        img = s.image()
        lin = red_extent(img, col(0.5), cy4 - 40, cy4 + 40)
        check(soft > lin * 1.4, "a softer curve widens light strokes (%d px soft, %d px linear)" % (soft, lin))
        s("undo")

        # 5. barrel button mapped to swap colors
        s("set_pen_settings", button1="swap_colors")
        before = s("get_state")["material"]["flat"]["color"]
        s("inject_pen", samples=[dict(x=0.9, y=0.9, down=False, pressure=0, button=1),
                                 dict(x=0.9, y=0.9, down=False, pressure=0, button=0)])
        wait_injected(s, 0.6)
        after = s("get_state")["material"]["flat"]["color"]
        check(before != after, "pen button 1 swapped the colors (%s -> %s)" % (before, after))
        s("set_pen_settings", button1="right")
        s("set_paint_material", id="", color=[0.9, 0.05, 0.05])

        # 6. fast strokes keep every sample: a zig-zag at 3 samples per frame stays sharp
        zz = []
        for i in range(48):
            zz.append(dict(x=0.28 + 0.44 * i / 47, y=0.58 if i % 2 == 0 else 0.64, pressure=1.0, down=True,
                           tilt_x=30.0, tilt_y=-20.0))
        zz.append(dict(x=0.72, y=0.64, pressure=0, down=False))
        s("set_brush", size=6)
        s("inject_pen", samples=zz)
        wait_injected(s)
        img = s.image()
        # every corner (one per sample) reaches the top line of the zig-zag
        top_y = int(y0 + 0.58 * (y1 - y0))
        hits = 0
        for i in range(0, 48, 2):
            px = col(0.28 + 0.44 * i / 47)
            if red_extent(img, px, top_y - 3, top_y + 4) > 0:
                hits += 1
        check(hits >= 22, "zig-zag: %d of 24 sharp corners painted (per-sample input, not per-frame)" % hits)
        t = s("pen_info")
        check(t["tilt_seen_degrees"] >= 29.5, "tilt is read (up to %.0f degrees)" % t["tilt_seen_degrees"])
        s.shot("pen_test_result", settle=0.2)
        print("diagnostics:", {k: info[k] for k in ("platform", "video_driver", "motion_rate_hz", "pressure_seen")})
    finally:
        s.close()
    print("%d failure(s)" % len(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
