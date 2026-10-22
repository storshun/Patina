#!/usr/bin/env python3
"""Retopology tools in the real window (synthetic mouse and keys), 0.14:
- the Retopology window brings the Lines, Zone, Flow and Density tools;
- a Lines drag draws a zone line on the surface (one undo step); one ended where it began closes and splits
  off a zone; one begun beside another starts exactly on it; Ctrl+click takes a line away; with Sym X, a
  line ended near the middle joins its mirror image; (0.22) Ctrl+Shift+click confirms a line and new lines
  don't snap to it; with Snap line ends off nothing snaps;
- the Zone tool chooses the zone under a click, and its slider sets the zone's resolution (one undo step);
- a Flow drag makes a direction stroke; Ctrl+click takes it away;
- the density brush paints a multiplier, Ctrl puts it back;
- the live preview makes the quads by itself and follows a change;
- Make quads builds along the flow (in the background) and the quads show over the model.

    python3 tests/gui/test_retopo_brushes.py [path/to/patina]
"""
import os
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import manual_shots  # noqa: E402
from manual_scenes import layout, tool, vp  # noqa: E402

BIN = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "patina")
failures = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        failures.append(what)


def seg_dist(p, a, b):
    ab = [b[i] - a[i] for i in range(3)]
    l2 = sum(x * x for x in ab) or 1e-30
    t = max(0.0, min(1.0, sum((p[i] - a[i]) * ab[i] for i in range(3)) / l2))
    return sum((p[i] - a[i] - ab[i] * t) ** 2 for i in range(3)) ** 0.5


def info(s):
    time.sleep(1.0)  # (frames can be slow under software GL)
    return s("retopo_info")


def ctrl_z(s):
    s.x.key("Control_L", True)
    s.x.key("z")
    s.x.key("Control_L", False)
    time.sleep(0.8)


def main():
    s = manual_shots.Session(BIN, os.path.join("/tmp", "patina_retopo_test"), display=":96", port=7876)
    try:
        s("new_project", primitive="sphere", resolution=512)
        layout(s)
        s("set_camera", view="front", frame=True)
        s("set_brush", size=60)
        s("retopo_density", quads_per_m2=300)
        s("open_window", window="retopology")
        time.sleep(0.8)
        # the live preview: with the window open, the quads come by themselves
        for _ in range(60):
            time.sleep(0.5)
            li = s("retopo_info")
            if li.get("result") and not li.get("building"):
                break
        check(li.get("result_is_preview") is True and li["result"]["quads"] > 100,
              "the live preview makes the quads by itself (%s)" % (li.get("result", {}).get("quads")))
        # Make quads keeps an up-to-date preview at once (it is made the same way)
        rect = s("ui_rect", name="retopo.make_quads")
        q0 = li["result"]["quads"]
        s.x.click(int(rect["x"] + rect["w"] / 2), int(rect["y"] + rect["h"] / 2))
        time.sleep(0.6)
        li = s("retopo_info")
        check(li.get("result_is_preview") is False and li.get("result_is_current") is True and li["result"]["quads"] == q0 and not li.get("building"),
              "Make quads keeps the preview at once (%s quads, preview %s)" % (li["result"]["quads"], li.get("result_is_preview")))
        # 1. a zone line round the front of the sphere: a closed loop splits off a zone
        tool(s, "zone_line")
        ring = [vp(0.5 + 0.18 * __import__("math").cos(a / 12 * 6.2832), 0.5 + 0.18 * __import__("math").sin(a / 12 * 6.2832))
                for a in range(13)]
        s.x.drag(ring, dt=0.08)
        z = s("retopo_zones")["zones"]
        check(len(z) == 2, "a closed zone line splits off a zone (%d zones)" % len(z))
        check(s("history")["steps"][-1] == "Zone line", "the line is one undo step named 'Zone line'")
        for _ in range(60):
            time.sleep(0.5)
            li = s("retopo_info")
            if li.get("result_is_current") and not li.get("building"):
                break
        check(li.get("result_is_current") is True, "the preview follows the new line")
        ctrl_z(s)
        check(len(s("retopo_zones")["zones"]) == 1, "Ctrl+Z takes the line back")
        s("redo")
        check(len(s("retopo_zones")["zones"]) == 2, "redo brings it back")
        # a line begun just beside the ring starts on it exactly; Ctrl+click takes it away
        li = s("retopo_info", line_points=True)
        ring3 = li["line_points"][0]["points"]
        check(li["line_points"][0]["loop"], "the ring, ended where it began, is a closed line")
        x0, y0 = vp(0.68, 0.5)
        s.x.drag([(x0 + 5, y0 + 4), (x0 + 30, y0 + 6), (x0 + 60, y0 + 8), (x0 + 90, y0 + 10)], dt=0.08)
        time.sleep(1.0)
        li = s("retopo_info", line_points=True)
        check(li["lines"] == 2, "a second line (%d lines)" % li["lines"])
        if li["lines"] == 2:
            start = li["line_points"][1]["points"][0]
            d = min(seg_dist(start, ring3[k], ring3[(k + 1) % len(ring3)]) for k in range(len(ring3)))
            check(d < 1e-4, "it starts exactly on the ring (%.6f away)" % d)
        s.x.key("Control_L", True)
        s.x.click(x0 + 60, y0 + 8)
        s.x.key("Control_L", False)
        time.sleep(1.0)
        check(s("retopo_info")["lines"] == 1, "Ctrl+click takes it away")
        check(s("history")["steps"][-1] == "Remove zone line", "one undo step named 'Remove zone line'")
        # and one ended just short of the ring ends on it exactly
        s.x.drag([(x0 + 90, y0 - 30), (x0 + 60, y0 - 20), (x0 + 30, y0 - 10), (x0 + 6, y0 - 3)], dt=0.08)
        time.sleep(1.0)
        li = s("retopo_info", line_points=True)
        if li["lines"] == 2:
            end = li["line_points"][1]["points"][-1]
            d = min(seg_dist(end, ring3[k], ring3[(k + 1) % len(ring3)]) for k in range(len(ring3)))
            check(d < 1e-4, "a line ended beside the ring ends on it exactly (%.6f away)" % d)
        else:
            check(False, "a line ended beside the ring (%d lines)" % li["lines"])
        ctrl_z(s)
        # (0.22) Ctrl+Shift+click confirms the ring: a line begun beside it then stays where it is drawn
        rx, ry = vp(0.68, 0.5)
        s.x.key("Control_L", True), s.x.key("Shift_L", True)
        s.x.click(rx, ry)
        s.x.key("Shift_L", False), s.x.key("Control_L", False)
        time.sleep(1.0)
        li = s("retopo_info", line_points=True)
        check([l["confirmed"] for l in li["line_points"]] == [True], "Ctrl+Shift+click confirms the ring")
        check(s("history")["steps"][-1] == "Confirm zone line", "one undo step named 'Confirm zone line'")
        def begun_beside():
            s.x.drag([(x0 + 5, y0 + 4), (x0 + 30, y0 + 6), (x0 + 60, y0 + 8), (x0 + 90, y0 + 10)], dt=0.08)
            time.sleep(1.0)
            li = s("retopo_info", line_points=True)
            if li["lines"] != 2:
                return -1.0
            start = li["line_points"][1]["points"][0]
            return min(seg_dist(start, ring3[k], ring3[(k + 1) % len(ring3)]) for k in range(len(ring3)))
        d = begun_beside()
        check(d > 1e-3, "a line begun beside a confirmed line doesn't snap to it (%.4f away)" % d)
        ctrl_z(s)
        s.x.key("Control_L", True), s.x.key("Shift_L", True)
        s.x.click(rx, ry)
        s.x.key("Shift_L", False), s.x.key("Control_L", False)
        time.sleep(1.0)
        check([l["confirmed"] for l in s("retopo_info", line_points=True)["line_points"]] == [False], "Ctrl+Shift+click again unconfirms it")
        s("retopo_snap", on=False)
        d = begun_beside()
        check(d > 1e-3, "with Snap line ends off, a line begun beside the ring stays where it is drawn (%.4f away)" % d)
        ctrl_z(s)
        s("retopo_snap", on=True)
        # with Sym X, a line ending near the middle joins its mirror image: one symmetric line across it
        s("set_symmetry", x=True)
        n0 = s("retopo_info")["lines"]
        s.x.drag([vp(0.36, 0.3), vp(0.40, 0.29), vp(0.45, 0.285), vp(0.497, 0.283)], dt=0.08)
        time.sleep(1.0)
        li = s("retopo_info", line_points=True)
        check(li["lines"] == n0 + 1, "with Sym X, a line ended near the middle is one line (%d new)" % (li["lines"] - n0))
        if li["lines"] == n0 + 1:
            xs = [p[0] for p in li["line_points"][-1]["points"]]
            check(abs(min(xs) + max(xs)) < 0.02 * (max(xs) - min(xs)), "and it is symmetric (x %.3f to %.3f)" % (min(xs), max(xs)))
        s.x.key("Control_L", True)
        s.x.click(*vp(0.40, 0.29))
        s.x.key("Control_L", False)
        time.sleep(1.0)
        check(s("retopo_info")["lines"] == n0, "Ctrl+click takes it away")
        s("set_symmetry", x=False)
        s.shot("lines")
        # 2. the Zone tool chooses the zone under a click; its resolution by the window's slider (one undo step)
        tool(s, "patch")
        s.x.click(*vp(0.5, 0.5))
        time.sleep(1.0)
        rc = s("ui_rect", name="retopo.zone_resolution")
        check(rc.get("found"), "the zone resolution slider is in the window")
        sx, sy, sw = int(rc.get("x", 0)), int(rc.get("y", 0) + rc.get("h", 0) / 2), int(rc.get("w", 1))
        s.x.drag([(sx + int(sw * f), sy) for f in (0.3, 0.4, 0.5, 0.58)], dt=0.15)
        time.sleep(1.5)
        res = lambda: [round(z["resolution"], 2) for z in s("retopo_zones")["zones"] if abs(z["resolution"] - 1) > 1e-3]
        r = res()
        check(len(r) == 1 and r[0] > 1.5, "the slider gives the zone a resolution of its own (%s)" % r)
        check(s("history")["steps"][-1] == "Zone resolution", "the drag is one undo step named 'Zone resolution'")
        ctrl_z(s)
        check(res() == [], "Ctrl+Z puts it back")
        s("redo")
        time.sleep(0.5)
        check(res() == r, "redo brings it back")
        s.shot("zone")
        # 3. a flow stroke, then Ctrl+click on it takes it away
        tool(s, "flow")
        s.x.drag([vp(0.25, 0.3), vp(0.3, 0.32), vp(0.35, 0.34), vp(0.4, 0.36)], dt=0.08)
        check(info(s)["strokes"] == 1, "a Flow drag makes a direction stroke")
        check(s("history")["steps"][-1] == "Flow stroke", "one undo step named 'Flow stroke'")
        s.shot("flow")
        s.x.key("Control_L", True)
        s.x.click(*vp(0.3, 0.32))
        s.x.key("Control_L", False)
        check(info(s)["strokes"] == 0, "Ctrl+click on the stroke takes it away")
        ctrl_z(s)
        check(info(s)["strokes"] == 1, "Ctrl+Z brings it back")
        # 4. density brush
        tool(s, "density")
        s.x.drag([vp(0.3, 0.6), vp(0.35, 0.62), vp(0.4, 0.64)])
        dv = info(s)["density_painted_vertices"]
        check(dv > 0, "the density brush paints (%d vertices)" % dv)
        s.x.drag([vp(0.3, 0.6), vp(0.35, 0.62), vp(0.4, 0.64)], mods=["Control_L"])
        check(info(s)["density_painted_vertices"] < dv, "Ctrl paints it back to normal")
        # 5. make quads along the flow
        s("retopo_density", quads_per_m2=300)
        r = s("retopo_build")
        q = r["quality"]
        check(r["quads"] > 100 and q["quads_pct"] >= 99.5, "the model is covered with quads (%d, %.1f%% of the faces)" % (r["quads"], q["quads_pct"]))
        check(q["regular_pct"] > 85, "most vertices have four edges (%.1f%%)" % q["regular_pct"])
        tool(s, "patch")
        s.shot("quads")
    finally:
        s.close()
    print("retopology tools: %d failed" % len(failures) if failures else "retopology tools: all checks passed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
