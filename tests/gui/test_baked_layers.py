#!/usr/bin/env python3
"""(0.41) Baked maps as layers in the real window (Xvfb or PATINA_SHOTS_DISPLAY, synthetic mouse): the Baked maps
section under the stack, clicking the AO layer shows the AO in the 3D view (and another layer brings the view back),
mouse strokes paint fixes over the bake, the picker takes the map's value, the row's right-click menu resets the fixes
and deletes the layer, and the section's button gives the map a layer again.

    python3 tests/gui/test_baked_layers.py [path/to/patina]
"""
import os
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import manual_shots  # noqa: E402
from manual_scenes import center, click, layout, rect, vp  # noqa: E402

BIN = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "patina")
failures = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        failures.append(what)


def main():
    s = manual_shots.Session(BIN, os.path.join("/tmp", "patina_baked_layers_test"), display=":97", port=7895)
    try:
        s("new_project", primitive="sphere", resolution=512)
        s("bake_mesh_maps", ao=True, curvature=True, ao_samples=16)
        layout(s)
        s("set_camera", view="front", frame=True, ortho=False)
        time.sleep(0.6)
        maps = lambda: {m["map"]: m for m in s("baked_maps")["maps"]}
        mode = lambda: s("set_view")["mode"]
        m = maps()
        ao_uid = m["ao"]["layer"]["uid"]
        check(rect(s, "layers.baked") is not None, "the Baked maps section is in the layer list")
        # clicking the AO layer: selected, and the 3D view shows the AO
        x0, y0, x1, y1 = rect(s, "layers.row:%d" % ao_uid)
        s.x.click(x0 + (x1 - x0) // 2, (y0 + y1) // 2)
        time.sleep(0.6)
        check(m["ao"]["layer"]["index"] == next(l["index"] for l in s("list_layers") if l["selected"]), "a click selects the AO layer")
        check(mode() == "ao", "and the 3D view shows the AO (%s)" % mode())
        # a mouse stroke paints a fix (black) over the bake
        s("set_tool", tool="paint")
        s("set_brush", size=90, hardness=1, opacity=1, flow=1, mask_value=0)
        time.sleep(0.3)
        mean0 = maps()["ao"]["mean"]
        s.x.drag([vp(0.42, 0.5), vp(0.5, 0.5), vp(0.58, 0.5)])
        time.sleep(0.6)
        m = maps()
        check(m["ao"]["mean"] < mean0 - 0.005 and m["ao"]["layer"]["fixes"], "a mouse stroke paints a fix (%.4f -> %.4f)" % (mean0, m["ao"]["mean"]))
        # the picker takes the map's value under the cursor
        s("set_brush", mask_value=0.77)
        s("set_tool", tool="picker")
        time.sleep(0.3)
        s.x.click(*[int(v) for v in vp(0.5, 0.5)])
        time.sleep(0.5)
        picked = s("set_brush")["mask_value"]
        check(picked < 0.1, "the picker takes the painted value (%.3f)" % picked)
        s("set_tool", tool="paint")
        # right-click the row: Reset the fixes
        x0, y0, x1, y1 = rect(s, "layers.row:%d" % ao_uid)
        s.x.click(x0 + (x1 - x0) // 2, (y0 + y1) // 2, b=3)
        time.sleep(0.5)
        click(s, "menu.baked_reset")
        time.sleep(0.5)
        m = maps()
        check(not m["ao"]["layer"]["fixes"] and abs(m["ao"]["mean"] - m["ao"]["mean_baked"]) < 1e-4, "right-click > Reset the fixes")
        # another layer: the view comes back
        top = max(l["index"] for l in s("list_layers"))
        s("select_layer", index=top)
        time.sleep(0.6)
        check(mode() == "material", "selecting another layer brings the view back (%s)" % mode())
        # right-click > Delete; the section's button gives the map a layer again
        x0, y0, x1, y1 = rect(s, "layers.row:%d" % ao_uid)
        s.x.click(x0 + (x1 - x0) // 2, (y0 + y1) // 2, b=3)
        time.sleep(0.5)
        click(s, "menu.baked_delete")
        time.sleep(0.6)
        check(maps()["ao"]["layer"] is None and maps()["ao"]["baked"], "right-click > Delete keeps the bake")
        click(s, "baked.add:ao")
        time.sleep(0.6)
        check(maps()["ao"]["layer"] is not None, "Add a layer for the Ambient occlusion")
        # (0.44.2) a UV edit after the bake: the section says so, and its Re-bake button bakes again
        shown = lambda: s("ui_rect", name="baked.rebake").get("found", False)
        check(not shown(), "no Re-bake while the bakes fit the UVs")
        s("uv_transform", faces=[0], islands=True, rotate=90)
        s.settle(6)
        check(maps()["ao"]["out_of_date"] and shown(),
              "a UV edit: the Baked maps section offers Re-bake")
        click(s, "baked.rebake")
        s.settle(6)
        check(not maps()["ao"]["out_of_date"] and not shown(), "Re-bake: the maps fit the UVs again")
        # its checkbox switches the map off in the 3D view (the AO no longer shades the model)
        ao = maps()["ao"]["layer"]["index"]
        s("update_layer", index=ao, visible=False)
        check(maps()["ao"]["shown"] is False, "the AO layer's checkbox off: the map is off (shown false)")
        s("update_layer", index=ao, visible=True)
    finally:
        s.close()
    print("baked layers: %d failed" % len(failures) if failures else "baked layers: all checks passed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
