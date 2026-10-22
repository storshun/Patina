#!/usr/bin/env python3
"""No two widgets in a window share an ID (0.22.3): with the same label and nothing to tell them apart they
fight over clicks, and Dear ImGui only says so when the pointer is over one (the Retopology window's
Density tool and its Show: Density option did). Patina is started with PATINA_UI_ID_CHECK, which counts
every widget drawn each frame; this opens the tools, the docked tabs, every window, every menu and the
retopology tools with quads made, then asks for the clashes seen.

    python3 tests/gui/test_ui_ids.py [path/to/patina]
"""
import os
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import manual_scenes as ms  # noqa: E402
import manual_shots  # noqa: E402

BIN = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "patina")
MODEL = os.path.join(ROOT, "samples", "Suzanne.glb")
WINDOWS = ["export", "render", "texel_density", "symmetry", "high_poly", "new_texture_set", "texture_set", "blender",
           "preferences", "shortcuts", "macros", "retopology", "mesh_check", "export_model", "reduce", "batch", "transform"]
MENUS = ["file", "edit", "view", "bake", "uv", "mesh", "retopology", "blender", "help"]
TOOLS = ["paint", "erase", "blur", "smudge", "clone", "fill", "picker", "select", "seams", "island", "note"]
PANELS = ["console", "brushes", "materials", "blender", "history", "notes", "colors", "texture_set", "properties"]
failures = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        failures.append(what)


def main():
    os.environ["PATINA_UI_ID_CHECK"] = "1"
    s = manual_shots.Session(BIN, os.path.join("/tmp", "patina_ui_ids_test"), display=":96", port=7893)
    try:
        check(s("ui_id_conflicts")["checking"], "the check is on")
        s("open_file", path=MODEL)
        s("add_layer", type="fill", name="Fill", mask="white")
        s("add_layer", type="adjust", name="Adjust")
        s("add_layer", type="folder", name="Folder")
        s("set_camera", view="front", frame=True)
        time.sleep(1.5)
        ms.layout(s)

        def round_trip():
            for t in TOOLS:
                ms.tool(s, t)
                time.sleep(0.3)
            for p in PANELS:
                ms.panel(s, p)
            for m in MENUS:
                ms.menu(s, m)
                s.x.key("Escape")
                time.sleep(0.3)
            # (0.36) the 3D view's right-click menu
            s.x.move(*ms.vp(0.5, 0.45))
            time.sleep(0.4)
            s.x.button(3, True); time.sleep(0.2); s.x.button(3, False)
            time.sleep(0.8)
            s.x.key("Escape")
            time.sleep(0.3)

        round_trip()
        # (0.36) the same in the test language: translated labels keep their English IDs
        s("language_set", code="x-test")
        time.sleep(1.5)
        round_trip()
        r = s("ui_id_conflicts")
        check(not r["conflicts"], "no two widgets share an ID in the test language (%s)" % "; ".join(r["conflicts"]))
        s("language_set", code="en")
        time.sleep(1.0)
        for w in WINDOWS:
            s("open_window", window=w)
            time.sleep(0.8)
        # the retopology tools, with quads made (the window's result part)
        s("retopo_density", quads_per_m2=300)
        s("retopo_build")
        for t in ("zone_line", "patch", "flow", "density"):
            ms.tool(s, t)
            time.sleep(0.8)
        time.sleep(1.0)
        r = s("ui_id_conflicts")
        check(not r["conflicts"], "no two widgets share an ID (%s)" % "; ".join(r["conflicts"]))
    finally:
        s.close()
    print("widget IDs: %d failed" % len(failures) if failures else "widget IDs: all checks passed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
