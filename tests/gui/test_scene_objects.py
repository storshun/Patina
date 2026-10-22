#!/usr/bin/env python3
"""(1.2) Objects in a scene in the real window (a hidden display, synthetic mouse and keys): a file with several
meshes lists an object each over the layers; a click on a row paints that object (its layers listed); a brush click
on another object in the 3D view paints nothing and says so, a double-click there paints it from then on; the Object
tool's gizmo moves the object along an arrow (one undo step), turns it with a ring, and a click on another object
picks it; the Local view key shows only the active one; no two widgets share an ID.

    python3 tests/gui/test_scene_objects.py [path/to/patina]
"""
import math
import os
import shutil
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import manual_shots  # noqa: E402
from manual_scenes import click, layout, rect, tool, vp  # noqa: E402

BIN = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "patina")
failures = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        failures.append(what)


def sphere(out, base, cx, cy, cz, r, seg=32, rings=16):
    w = seg + 1
    for j in range(rings + 1):
        th = math.pi * j / rings
        for i in range(seg + 1):
            ph = 2 * math.pi * i / seg
            out.append("v %f %f %f" % (cx + r * math.sin(th) * math.cos(ph), cy + r * math.cos(th), cz + r * math.sin(th) * math.sin(ph)))
            out.append("vt %f %f" % (i / seg, 1 - j / rings))
    for j in range(rings):
        for i in range(seg):
            a = base + j * w + i + 1
            b, c, d = a + 1, base + (j + 1) * w + i + 2, base + (j + 1) * w + i + 1
            out.append("f %d/%d %d/%d %d/%d %d/%d" % (a, a, b, b, c, c, d, d))
    return (rings + 1) * w


def character(path):
    out, n = [], 0
    for name, c, r, seg in (("Body", (0, 0, 0), 1.0, 32), ("Hat", (0, 1.25, 0), 0.35, 32), ("Eye", (0.32, 0.25, 0.9), 0.15, 16)):
        out.append("o " + name)
        n += sphere(out, n, *c, r, seg, seg // 2)
    open(path, "w").write("\n".join(out) + "\n")


def find(s, name, far_from=None):
    """a normalized viewport point where the object `name` is the first thing seen: the middle of where it is seen
    (or the point of it furthest from `far_from`)"""
    hits = []
    for j in range(4, 97, 4):
        for i in range(4, 97, 4):
            r = s("pick_object", point=[i / 100, j / 100])
            if r.get("hit") and r["name"] == name:
                hits.append((i / 100, j / 100))
    if not hits:
        return None
    if far_from:
        return max(hits, key=lambda p: (p[0] - far_from[0]) ** 2 + (p[1] - far_from[1]) ** 2)
    mx, my = sum(p[0] for p in hits) / len(hits), sum(p[1] for p in hits) / len(hits)
    return min(hits, key=lambda p: (p[0] - mx) ** 2 + (p[1] - my) ** 2)


def found(s, name):
    return s("ui_rect", name=name).get("found", False)


def active(s):
    return s("get_state")["active_object"]


def main():
    os.environ["PATINA_UI_ID_CHECK"] = "1"
    work = os.path.join("/tmp", "patina_scene_objects_test")
    os.makedirs(work, exist_ok=True)
    model = os.path.join(work, "character.obj")
    character(model)
    s = manual_shots.Session(BIN, os.path.join("/tmp", "patina_scene_objects_shots"), display=":96", port=7889)
    try:
        s("layout", mode="full")
        s("open_mesh", path=model, resolution=1024)
        s.settle(6)
        layout(s)
        s("set_camera", view="front", frame=True)
        s.settle(3)
        # ---- the Objects list
        check(all(found(s, "objects.row." + n) for n in ("Body", "Hat", "Eye")), "the list shows an object for each mesh")
        click(s, "objects.row.Hat")
        s.settle(4)
        check(active(s) == 1, "a click on a row paints that object (%d)" % active(s))
        s("add_layer", type="paint")
        s.settle(2)
        n_hat = len(s("list_layers"))
        click(s, "objects.row.Body")
        s.settle(4)
        check(active(s) == 0 and len(s("list_layers")) == n_hat - 1, "and the layers listed are its own")
        # ---- the 3D view: a brush on another object paints nothing; a double-click there paints it
        eye = find(s, "Eye")
        check(eye is not None, "the eye is seen in the view (%s)" % (eye,))
        if eye:
            tool(s, "paint")
            hist = len(s("history")["steps"])
            x, y = vp(*eye)
            s.x.click(x, y)
            s.settle(3)
            check(len(s("history")["steps"]) == hist and active(s) == 0, "a brush click on the eye paints nothing")
            s.x.click(x, y)
            s.x.click(x, y, hold=0.03)
            s.settle(4)
            check(active(s) == 2, "a double-click on it paints the eye from then on (%d)" % active(s))
        # ---- the Object tool
        click(s, "objects.row.Hat")
        s.settle(3)
        tool(s, "object")
        s.settle(3)
        layout(s)
        check(s("get_state").get("window_tool") == "object", "the Object tool")
        check(found(s, "gizmo.y") and found(s, "gizmo.center"), "its gizmo over the active object")
        before = s("list_objects")["objects"][1]["location"]
        gx0, gy0, gx1, gy1 = rect(s, "gizmo.y")
        sx, sy = (gx0 + gx1) / 2, (gy0 + gy1) / 2
        s.x.drag([(sx, sy), (sx, sy - 10), (sx, sy - 30), (sx, sy - 50)])
        s.settle(3)
        after = s("list_objects")["objects"][1]["location"]
        check(after[1] > before[1] + 0.05 and abs(after[0]) < 1e-4, "the up arrow moves it up (%s)" % after)
        check(s("history")["steps"][-1] == "Move the object", "as one undo step (%s)" % s("history")["steps"][-1])
        s("undo")
        check(s("list_objects")["objects"][1]["location"][1] < 1e-4, "which takes it back")
        click(s, "btn.object.turn")
        s.settle(3)
        rx0, ry0, rx1, ry1 = rect(s, "gizmo.z")
        cx0, cy0, cx1, cy1 = rect(s, "gizmo.center")
        px, py = (rx0 + rx1) / 2, (ry0 + ry1) / 2
        ccx, ccy = (cx0 + cx1) / 2, (cy0 + cy1) / 2
        ang = math.atan2(py - ccy, px - ccx)
        rad = math.hypot(px - ccx, py - ccy)
        pts = [(ccx + rad * math.cos(ang + t), ccy + rad * math.sin(ang + t)) for t in (0, 0.1, 0.25, 0.4, 0.5)]
        s.x.drag(pts)
        s.settle(3)
        rot = s("list_objects")["objects"][1]["rotation"]
        check(abs(rot[2]) > 10, "the Z ring turns it about Z (%s)" % rot)
        layout(s)  # (the tool's bar: the view's place again)
        hat_at = find(s, "Hat")
        body = find(s, "Body", far_from=hat_at)  # (away from the hat's gizmo)
        if body:
            s.x.click(*vp(*body))
            s.settle(4)
            check(active(s) == 0, "a click on another object with the tool picks it (%d)" % active(s))
        # ---- the Local view
        s.x.move(*vp(0.5, 0.5))
        s.x.key("slash")
        s.settle(3)
        check(s("list_objects")["isolated"], "the Local view key shows only the active object")
        check(find(s, "Eye") is None, "the others aren't picked then")
        s.x.key("slash")
        s.settle(3)
        check(not s("list_objects")["isolated"], "and again shows them all")
        # ---- no two widgets share an ID (in English and in the test language)
        for lang in ("x-test", "en"):
            s("language_set", code=lang)
            s.settle(10)
        r = s("ui_id_conflicts")
        check(r["checking"] and not r["conflicts"], "no two widgets share an ID (%s)" % "; ".join(r["conflicts"]))
    finally:
        s.close()
    shutil.rmtree(work, ignore_errors=True)
    shutil.rmtree(os.path.join("/tmp", "patina_scene_objects_shots"), ignore_errors=True)
    print("scene objects: %s" % ("all checks passed" if not failures else "%d FAILED" % len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
