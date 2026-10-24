#!/usr/bin/env python3
"""Zones for thin parts in the real window (0.18): a ball with a thin finger at 300 quads per m² leaves the
finger out (red marks and a message; at 400 a four-sided tube just covers it since 0.20.1); the window's
Zones for thin parts button draws a ring round the finger's root, gives its zone more quads (one undo step), and the finger is then covered.

    python3 tests/gui/test_retopo_thin.py [path/to/patina]
"""
import math
import os
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import manual_shots  # noqa: E402

BIN = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "patina")
OUT = os.path.join("/tmp", "patina_retopo_thin_test")
failures = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        failures.append(what)


def ball_with_finger(path, seg=64, rows=160):
    """A closed body of revolution along y: a ball of radius 0.3, a finger of radius 0.03 out of its top."""
    def radius(y):
        if y < 0.2:
            return math.sqrt(max(0.0, 0.09 - y * y))
        if y < 0.28:
            r0 = math.sqrt(0.09 - 0.04)
            return r0 + (0.03 - r0) * (y - 0.2) / 0.08
        if y < 0.67:
            return 0.03
        return math.sqrt(max(0.0, 0.0009 - (y - 0.67) ** 2))
    v = [(0.0, -0.3, 0.0)]
    for r in range(1, rows):
        y = -0.3 + 1.0 * r / rows
        for k in range(seg):
            a = 2 * math.pi * k / seg
            v.append((radius(y) * math.cos(a), y, radius(y) * math.sin(a)))
    v.append((0.0, 0.7, 0.0))
    top = len(v)  # (1-based in the file)
    idx = lambda r, k: 2 + (r - 1) * seg + (k % seg)
    f = [(1, idx(1, k + 1), idx(1, k)) for k in range(seg)]
    for r in range(1, rows - 1):
        for k in range(seg):
            f.append((idx(r, k), idx(r, k + 1), idx(r + 1, k + 1), idx(r + 1, k)))
    f += [(idx(rows - 1, k), idx(rows - 1, k + 1), top) for k in range(seg)]
    # (faces outwards: a positive volume)
    vol = 0.0
    for q in f:
        a = v[q[0] - 1]
        for j in range(1, len(q) - 1):
            b, c = v[q[j] - 1], v[q[j + 1] - 1]
            vol += (a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0]) + a[2] * (b[0] * c[1] - b[1] * c[0]))
    if vol < 0:
        f = [tuple(reversed(q)) for q in f]
    with open(path, "w") as fo:
        for p in v:
            fo.write("v %f %f %f\n" % p)
        for q in f:
            fo.write("f " + " ".join(str(i) for i in q) + "\n")


def wait_result(s, tries=80):
    for _ in range(tries):
        time.sleep(0.5)
        li = s("retopo_info")
        if li.get("result") and not li.get("building") and li.get("result_is_current"):
            return li
    return s("retopo_info")


def main():
    os.makedirs(OUT, exist_ok=True)
    model = os.path.join(OUT, "ball_finger.obj")
    ball_with_finger(model)
    s = manual_shots.Session(BIN, OUT, display=":97", port=7877)
    try:
        s("open_file", path=model)
        s("set_camera", view="front", frame=True)
        s("retopo_density", quads_per_m2=300)
        s("open_window", window="retopology")
        li = wait_result(s)
        problems = " ".join(li.get("result", {}).get("problems", []))
        check("too thin" in problems, "at 300 quads per m² the finger is too thin, and the window says so")
        # (1.2.9) the red marks over the finger hide and show again (counted in the 3D view: the model is pink and
        # the UV editor's island outlines are red)
        from manual_scenes import rect
        view = rect(s, "viewport.image")

        def red():
            im = s.image().crop(tuple(int(v) for v in view))
            return sum(1 for p in im.getdata() if p[0] > 200 and p[1] < 70 and p[2] < 70)
        s("retopo_view", preview=True, show="none", quads=True)
        s.settle()
        on = red()
        check(s("retopo_view", marks=False)["marks"] is False, "retopo_view marks false")
        s.settle()
        off = red()
        check(on > 200 and off < on * 0.05, "the red marks hide (%d red pixels, %d with them shown)" % (off, on))
        s("retopo_view", marks=True)
        zones0 = len(s("retopo_zones")["zones"])
        rect = s("ui_rect", name="retopo.thin_zones")
        check(rect.get("found") is True, "the window has a Zones for thin parts button")
        s.x.click(int(rect["x"] + rect["w"] / 2), int(rect["y"] + rect["h"] / 2))
        time.sleep(1.0)
        zones = s("retopo_zones")["zones"]
        check(len(zones) == zones0 + 1, "it splits the finger off as a zone of its own (%d zones, %d before)" % (len(zones), zones0))
        check(s("history")["steps"][-1] == "Zones for thin parts", "one undo step named 'Zones for thin parts'")
        res = s("retopo_zone", point=[0.0, 0.6, 0.03])["resolution"]
        check(res >= 2, "the finger's zone gets more quads (x%g)" % res)
        li = wait_result(s)
        problems = " ".join(li.get("result", {}).get("problems", []))
        check("too thin" not in problems, "the quads then cover the finger (%s)" % (problems or "no problems"))
        exp = os.path.join(OUT, "quads.obj")
        s("retopo_export", path=exp)
        verts, faces = [], 0
        for line in open(exp):
            if line.startswith("v "):
                verts.append(tuple(float(x) for x in line.split()[1:4]))
        on_finger = sum(1 for p in verts if p[1] > 0.35)
        check(on_finger >= 30, "the finger has vertices of its own (%d)" % on_finger)
        s("undo")
        check(len(s("retopo_zones")["zones"]) == zones0, "undo takes the zone away again")
        # (0.20.4) Shift+click with the Lines tool: a line right round the finger, across it, where clicked
        from manual_scenes import layout, tool, vp
        layout(s)
        tool(s, "zone_line")
        at = None
        for k in range(5, 60):
            h = s("pick_face", point=[0.5, k / 100])
            if h.get("hit") and 0.42 < h["position"][1] < 0.6:
                at = (k / 100, h["position"][1])
                break
        check(at is not None, "a point on the finger in the window")
        if at:
            lines0 = s("retopo_info")["lines"]
            s.x.key("Shift_L", True)
            s.x.move(*vp(0.5, at[0]))
            time.sleep(0.8)
            pr = s("ui_rect", name="retopo.ring_preview")
            check(pr.get("found") and pr["w"] > 5 and pr["h"] < pr["w"],
                  "with Shift held over the finger, the line a click would draw shows round it (%s)" % pr)
            s.x.click(*vp(0.5, at[0]))
            s.x.key("Shift_L", False)
            time.sleep(0.5)
            li = s("retopo_info", line_points=True)
            check(li["lines"] == lines0 + 1, "Shift+click adds one line (%d, %d before)" % (li["lines"], lines0))
            ring = li.get("line_points", [[]])[-1]
            pts = ring["points"] if isinstance(ring, dict) else ring
            ys = [p[1] for p in pts] or [0]
            r = [math.hypot(p[0], p[2]) for p in pts] or [0]
            check(len(pts) >= 8 and max(ys) - min(ys) < 0.01 and abs(sum(ys) / len(ys) - at[1]) < 0.06 and 0.025 < min(r) and max(r) < 0.035,
                  "it goes right round the finger where clicked (%d points, y %.3f to %.3f, %.3f from the middle)"
                  % (len(pts), min(ys), max(ys), sum(r) / len(r)))
            check(s("history")["steps"][-1] == "Ring line", "one undo step named 'Ring line'")
            # (0.22) on the ball there is no part to go round: Shift+click draws the cut through the point facing
            # the screen; Shift+wheel switches the preview to round the part (none here) and back
            bx, by = vp(0.5, 0.75)
            s.x.key("Shift_L", True)
            s.x.move(bx, by)
            time.sleep(1.2)
            check(s("ui_rect", name="retopo.ring_preview").get("found") is True, "with Shift held over the ball, the cut facing you shows")
            s.x.button(4, True), s.x.button(4, False)
            time.sleep(1.2)
            check(s("ui_rect", name="retopo.ring_preview").get("found") is False, "Shift+wheel: round the part instead (none on a ball)")
            s.x.button(4, True), s.x.button(4, False)
            time.sleep(1.2)
            check(s("ui_rect", name="retopo.ring_preview").get("found") is True, "Shift+wheel again: facing you")
            s.x.click(bx, by)
            s.x.key("Shift_L", False)
            time.sleep(0.8)
            li = s("retopo_info", line_points=True)
            check(li["lines"] == lines0 + 2, "Shift+click on the ball adds the cut facing you (%d lines)" % li["lines"])
            if li["lines"] == lines0 + 2:
                pts = li["line_points"][-1]["points"]
                zs = [p[2] for p in pts]
                xs = [p[0] for p in pts]
                check(max(zs) - min(zs) < 0.01 * (max(xs) - min(xs)) + 1e-4, "it lies in a plane facing the screen (z %.4f to %.4f)" % (min(zs), max(zs)))
        # (1.2.9) no live preview for a great many quads (it took all the memory): Make quads only
        for _ in range(20):
            if not s("retopo_info")["building"]:
                break
            time.sleep(0.5)
        s("retopo_density", quads_per_m2=500000)
        time.sleep(2.5)
        li = s("retopo_info")
        check(li["quads_asked"] > 150000 and li["preview_too_big"] and not li["building"],
              "about %d quads asked for: no live preview starts" % li["quads_asked"])
        s("retopo_density", quads_per_m2=300)
        time.sleep(2.5)
        check(not s("retopo_info")["preview_too_big"], "back to 300 per m²: the preview runs again")
    finally:
        s.close()
    print("zones for thin parts: %d failed" % len(failures) if failures else "zones for thin parts: all checks passed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
