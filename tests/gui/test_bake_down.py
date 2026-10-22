#!/usr/bin/env python3
"""(1.1) The bake-down in the real window (PATINA_SHOTS_DISPLAY or Xvfb, synthetic mouse and keys): Bake > Bake Down
to a Low-Poly Model with a model file; unsaved work is asked about first; the layers come onto the low-poly model,
which becomes the model in a new project; the window reports what was found; Show the check switches to the check
view and Compare with the high-poly shows the high as it was beside the low; the Guided layout's Bake step has it
too; no two widgets share an ID.

    python3 tests/gui/test_bake_down.py [path/to/patina]
"""
import math
import os
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import manual_shots  # noqa: E402
from manual_scenes import click  # noqa: E402

BIN = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "patina")
failures = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        failures.append(what)


def found(s, name):
    return s("ui_rect", name=name).get("found", False)


def sphere_obj(path, seg, rings, r, bumps):
    """a UV sphere (its own UVs); bumps: small waves over it, as a sculpt would have"""
    out = ["o Sphere"]
    for j in range(rings + 1):
        th = math.pi * j / rings
        for i in range(seg + 1):
            ph = 2 * math.pi * i / seg
            x, y, z = math.sin(th) * math.cos(ph), math.cos(th), math.sin(th) * math.sin(ph)
            k = r * (1 + (0.01 * math.sin(20 * x) * math.sin(20 * y) if bumps else 0))
            out.append("v %.6f %.6f %.6f" % (x * k, y * k, z * k))
            out.append("vt %.6f %.6f" % (i / seg, 1 - j / rings))
    w = seg + 1
    for j in range(rings):
        for i in range(seg):
            a, b, c, d = j * w + i + 1, j * w + i + 2, (j + 1) * w + i + 2, (j + 1) * w + i + 1
            out.append("f %d/%d %d/%d %d/%d %d/%d" % (a, a, d, d, c, c, b, b))
    open(path, "w").write("\n".join(out) + "\n")


def main():
    os.environ["PATINA_UI_ID_CHECK"] = "1"  # (widgets that share an ID are counted, see test_ui_ids.py)
    work = os.path.join("/tmp", "patina_bake_down_test")
    os.makedirs(work, exist_ok=True)
    hp, lp = os.path.join(work, "statue_high.obj"), os.path.join(work, "statue_low.obj")
    sphere_obj(hp, 192, 96, 1.0, True)
    sphere_obj(lp, 40, 20, 0.995, False)
    s = manual_shots.Session(BIN, os.path.join("/tmp", "patina_bake_down_shots"), display=":96", port=7886)
    try:
        s("layout", mode="full")
        s("open_mesh", path=hp)
        s.settle(5)
        high_faces = s("get_state")["mesh"]["faces"]
        s("set_camera", view="front", frame=True)
        s("add_layer", type="paint")
        s("set_paint_material", id="", color=[0.85, 0.25, 0.1])
        s("set_brush", size=80, opacity=1, flow=1, hardness=0.8)
        s("paint_stroke", points=[[0.35, 0.45], [0.65, 0.55]])
        s.settle(4)
        layers_before = [l["name"] for l in s("list_layers") if l.get("type") != "baked"]
        # ---- the window, with the low-poly file given
        s("open_window", window="bake_down", path=lp)
        s.settle(5)
        check(found(s, "window.Bake Down to a Low-Poly Model") and found(s, "bakedown.go"), "the window opens with its Bake down button")
        click(s, "bakedown.go")
        s.settle(5)
        check(found(s, "replace.save") and found(s, "replace.discard"), "unsaved work: asked first")
        click(s, "replace.discard")
        s.settle(12)
        st = s("get_state")
        check(st["mesh"]["faces"] == 40 * 20 and high_faces != st["mesh"]["faces"], "the low-poly is the model now (%s faces)" % st["mesh"]["faces"])
        layers_after = [l["name"] for l in s("list_layers") if l.get("type") != "baked"]
        check(layers_after == layers_before, "every layer came along (%s)" % layers_after)
        check(found(s, "bakedown.check") and found(s, "bakedown.compare"), "the window reports the bake-down, with its two looks")
        click(s, "bakedown.check")
        s.settle(4)
        check(s("get_state").get("view_mode") == "bake_check", "Show the check: the check view")
        click(s, "bakedown.check")
        s.settle(4)
        check(s("get_state").get("view_mode") == "material", "and back")
        click(s, "bakedown.compare")
        s.settle(4)
        cmp = s("set_compare")
        check(cmp["enabled"] and cmp["mode"] == "high_poly", "Compare with the high-poly: the split view (%s)" % cmp)
        click(s, "bakedown.compare")
        s.settle(4)
        check(not s("set_compare")["enabled"], "and off again")
        s.x.key("Escape")
        s.settle(4)
        check(not found(s, "window.Bake Down to a Low-Poly Model"), "Escape closes the window")
        # ---- the Guided layout's Bake step
        s("layout", mode="guided")
        s("layout", step="bake")
        s.settle(6)
        check(found(s, "guided.bake_down"), "the Bake step offers the bake-down")
        check(found(s, "bakedown.check"), "and shows the last one's report there")
        # ---- no two widgets share an ID (in English and in the test language)
        s("open_window", window="bake_down")
        for lang in ("x-test", "en"):
            s("language_set", code=lang)
            s.settle(10)
        r = s("ui_id_conflicts")
        check(r["checking"] and not r["conflicts"], "no two widgets share an ID (%s)" % "; ".join(r["conflicts"]))
    finally:
        s.close()
    shutil.rmtree(work, ignore_errors=True)
    shutil.rmtree(os.path.join("/tmp", "patina_bake_down_shots"), ignore_errors=True)
    print("bake-down: %s" % ("all checks passed" if not failures else "%d FAILED" % len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
