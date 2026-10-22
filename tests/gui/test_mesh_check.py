#!/usr/bin/env python3
"""The Mesh Check window (0.27) in the real window: a cube with a hole and an open seam shows both problems; the
window's Fix button for the holes closes them (the layers stay); "Fix everything" joins the seam; "Back to before the
fix" brings back the model from before that last fix. Then File > Export Model writes a .glb.

    python3 tests/gui/test_mesh_check.py [path/to/patina] [--shot OUT.png]
"""
import os
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import manual_shots  # noqa: E402

BIN = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith("--") else os.path.join(ROOT, "build", "patina")
SHOT = sys.argv[sys.argv.index("--shot") + 1] if "--shot" in sys.argv else None
failures = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        failures.append(what)


def main():
    tmp = tempfile.mkdtemp(prefix="patina_meshcheck_gui_")
    obj = os.path.join(tmp, "holey.obj")
    with open(obj, "w") as f:
        # a cube without its front face, and its top face on vertices of its own (an open seam)
        f.write("v -1 -1 -1\nv 1 -1 -1\nv 1 1 -1\nv -1 1 -1\nv -1 -1 1\nv 1 -1 1\nv 1 1 1\nv -1 1 1\n")
        f.write("v 1 1 -1\nv -1 1 -1\nv -1 1 1\nv 1 1 1\n")
        f.write("vt 0 0\nvt 0.45 0\nvt 0.45 0.45\nvt 0 0.45\nvt 0.5 0\nvt 0.95 0\nvt 0.95 0.45\nvt 0.5 0.45\n")
        f.write("vt 0 0.5\nvt 0.45 0.5\nvt 0.45 0.95\nvt 0 0.95\nvt 0.5 0.5\nvt 0.95 0.5\nvt 0.95 0.95\nvt 0.5 0.95\n")
        f.write("f 1/1 4/4 3/3 2/2\nf 5/5 6/6 7/7 8/8\nf 2/9 3/10 7/11 6/12\nf 4/13 1/14 5/15 8/16\nf 10/1 11/2 12/3 9/4\n")
    s = manual_shots.Session(BIN, os.path.join("/tmp", "patina_meshcheck_test"), display=":97", port=7897)

    def rect(name):
        r = s("ui_rect", name=name)
        return r if r.get("found") else None

    def click(r):
        s.x.click(int(r["x"] + r["w"] / 2), int(r["y"] + r["h"] / 2))
        time.sleep(1.2)

    try:
        s("open_file", path=obj)
        s("set_camera", view="front", frame=True)
        s("add_layer", type="fill", material={"id": "patina_bricks_red"})
        s("open_window", window="mesh_check")
        time.sleep(1.0)
        r = s("mesh_check", show=True)
        ids = {p["id"]: p["count"] for p in r["problems"]}
        check(ids.get("holes") == 3 and ids.get("open_seams") == 8, "the holes (the missing face, both sides of the seam) and the open seam are found (%s)" % ids)
        order = [p["id"] for p in r["problems"]]
        check(order.index("open_seams") < order.index("holes"), "open seams come before holes (join them first)")
        layers = len(s("list_layers"))
        time.sleep(0.8)
        if SHOT:
            s.shot(SHOT)
        fix = rect("meshcheck.fix:holes")
        check(fix is not None, "the hole has a Fix button")
        if fix:
            click(fix)
            r = s("mesh_check")
            ids = {p["id"]: p["count"] for p in r["problems"]}
            check(ids.get("holes", 0) < 2, "Fix closes the holes it can (%s)" % ids)
        faces_before = s("mesh_check")["stats"]["faces"]
        allb = rect("meshcheck.fix_all")
        check(allb is not None, "Fix everything is there while something can be fixed")
        if allb:
            click(allb)
            r = s("mesh_check")
            ids = {p["id"]: p["count"] for p in r["problems"]}
            check(r["stats"]["closed"] and "open_seams" not in ids and "holes" not in ids, "Fix everything leaves it closed (%s)" % ids)
            check(len(s("list_layers")) == layers, "the layers are still there")
        rv = rect("meshcheck.revert")
        check(rv is not None, "Back to before the fix is offered")
        if rv:
            click(rv)
            r = s("mesh_check")
            check(r["stats"]["faces"] == faces_before, "the model before the last fix is back (%d faces)" % r["stats"]["faces"])
        # 0.28: File > Export Model writes a .glb with the textures inside
        s("open_window", window="export_model")
        time.sleep(1.0)
        ex = rect("model_export.export")
        check(ex is not None, "the Export model window has its Export button")
        if ex:
            click(ex)
            time.sleep(1.5)
            out = os.path.join(s.home, "patina_export", "holey.glb")
            check(os.path.isfile(out) and open(out, "rb").read(4) == b"glTF", "Export writes the .glb (%s)" % out)
    finally:
        s.close()
    print("mesh check: " + ("all checks passed" if not failures else "%d FAILED" % len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
