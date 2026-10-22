#!/usr/bin/env python3
"""(0.46) The Guided layout in the real window (PATINA_SHOTS_DISPLAY or Xvfb, synthetic mouse and keys): a first start
opens in it with the welcome window; the step bar's steps each show their own screen (the UV editor only in UVs, the
library shelf and layers in Paint) and their states (to do, done, needs a look); the Model, Bake and Export steps' view
only looks (a drag turns it, paints nothing); a tool key from another step goes to that step; the Bake, Next and
Export buttons work; a brush that would paint nothing says why; Full and Guided switch back and forth; and unsaved
work is never replaced without asking.

    python3 tests/gui/test_guided_layout.py [path/to/patina]
"""
import json
import os
import shutil
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import manual_shots  # noqa: E402
import manual_scenes  # noqa: E402
from manual_scenes import click  # noqa: E402

BIN = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "patina")
SAMPLE = os.path.join(ROOT, "samples", "Suzanne.glb")
failures = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        failures.append(what)


def found(s, name):
    return s("ui_rect", name=name).get("found", False)


def states(s):
    return {st["step"]: st["state"] for st in s("layout")["steps"]}


def view_point(s, fx, fy):
    manual_scenes.VIEW = manual_scenes.rect(s, "viewport.image")
    return [int(v) for v in manual_scenes.vp(fx, fy)]


def main():
    os.environ["PATINA_UI_ID_CHECK"] = "1"  # (widgets that share an ID are counted, see test_ui_ids.py)
    s = manual_shots.Session(BIN, os.path.join("/tmp", "patina_guided_test"), display=":97", port=7889, session=False)
    try:
        # ---- a first start: the Guided layout, the welcome window over it
        lay = s("layout")
        check(lay["layout"] == "guided" and lay["step"] == "model", "a first start opens in the Guided layout, at Model (%s)" % lay)
        check(found(s, "window.Welcome to Patina"), "with the welcome window the first time")
        check(all(v == "todo" for v in states(s).values()), "nothing is done yet (%s)" % states(s))
        # (0.47) the brush paints a color on a first start, not a material
        check(not s("get_state")["material"].get("id"), "a first start paints a color (%s)" % s("get_state")["material"].get("id"))
        # the menus of the work in the steps' order, the steps' tabs side by side
        xs = [manual_scenes.rect(s, "menu." + m)[0] for m in ("mesh", "retopology", "uv", "bake", "blender")]
        check(xs == sorted(xs), "the menus follow the steps: Mesh, Retopology, UV, Bake, Blender (%s)" % xs)
        tabs = [manual_scenes.rect(s, "guided.step." + k) for k in ("model", "uvs", "bake", "paint", "export")]
        check(all(tabs[k][0] < tabs[k + 1][0] for k in range(4)), "the five step tabs, left to right")
        s.x.key("Escape")
        s.settle(5)
        # ---- the model in: Model is done
        s("open_mesh", path=SAMPLE)
        s.settle(5)
        check(states(s)["model"] == "done", "an open model makes the Model step done")
        check(found(s, "window.Step") and not found(s, "window.UV Editor") and not found(s, "window.Layers"),
              "the Model step: the view and the step's panel, no UV editor or layers")
        # its view only looks: a drag turns the view and paints nothing
        cam0 = s("get_state")["camera"]
        undo0 = s("history")["position"]
        s.x.drag([view_point(s, 0.4 + 0.02 * k, 0.5) for k in range(8)])
        s.settle(5)
        cam1 = s("get_state")["camera"]
        check(cam0 != cam1 and s("history")["position"] == undo0, "there a left drag turns the view and paints nothing")
        # ---- UVs from the step bar: the UV editor beside the view, the UV tools only
        click(s, "guided.step.uvs")
        s.settle(5)
        check(s("layout")["step"] == "uvs", "a click on the bar's UVs goes to that step")
        check(found(s, "window.UV Editor") and found(s, "window.Viewport"), "the UVs step shows the UV editor beside the view")
        check(found(s, "tools.tool.select") and not found(s, "tools.tool.paint"), "its tool strip has the UV tools, not the paint tools")
        check(s("get_state").get("window_tool") == "select", "and the Select tool is chosen (%s)" % s("get_state").get("window_tool"))
        check(not found(s, "toolbar.slider:size"), "the Select tool's bar has no brush sliders")
        check(states(s)["uvs"] == "done", "the sample's UVs are clean (0.47: its tiny faces have UV area) (%s)" % states(s)["uvs"])
        # a paint tool's key goes to the Paint step
        s.x.move(*view_point(s, 0.5, 0.5))
        s.settle(3)
        s.x.key("b")
        s.settle(6)
        check(s("layout")["step"] == "paint", "the paint tool's key (B) goes to the Paint step (%s)" % s("layout")["step"])
        check(found(s, "window.Layers") and found(s, "window.Materials") and found(s, "window.Brushes"),
              "the Paint step shows the layers and the library shelf")
        check(found(s, "toolbar.slider:size"), "and the brush sliders")
        # ---- Bake: its button bakes; Next goes on to Paint
        click(s, "guided.step.bake")
        s.settle(5)
        check(states(s)["bake"] == "todo", "Bake is to do before a bake")
        click(s, "guided.bake")
        s.settle(20)
        maps = {m["map"]: m for m in s("baked_maps")["maps"]}
        check(maps["ao"]["baked"] and maps["curvature"]["baked"], "the Bake button bakes the mesh maps")
        check(states(s)["bake"] == "done", "and the Bake step is done")
        click(s, "guided.next")
        s.settle(5)
        check(s("layout")["step"] == "paint", "Next goes on to Paint")
        # ---- Paint: a stroke on the model; a hidden layer says why nothing shows
        s("add_layer", type="paint")
        s("set_brush", size=60, opacity=1, flow=1, hardness=1)
        s.x.drag([view_point(s, 0.44 + 0.02 * k, 0.45) for k in range(7)])
        s.settle(6)
        check(states(s)["paint"] == "done", "a stroke makes the Paint step done (%s)" % states(s)["paint"])
        sel = next(l for l in s("list_layers") if l["selected"])
        s("update_layer", index=sel["index"], visible=False)
        s.settle(4)
        check(found(s, "viewport.blocked"), "a hidden layer: the view says why strokes won't show")
        s("update_layer", index=sel["index"], visible=True)
        s.settle(4)
        check(not found(s, "viewport.blocked"), "shown again, the warning goes")
        # ---- Export: its button writes the textures
        click(s, "guided.step.export")
        s.settle(5)
        click(s, "guided.export")
        s.settle(10)
        out = os.path.join(s.home, "patina_export")
        files = sorted(os.listdir(out)) if os.path.isdir(out) else []
        check(any(f.endswith(".png") for f in files), "the Export button writes the textures (%s)" % files[:4])
        check(states(s)["export"] == "done", "and the Export step is done")
        # ---- Full and back: the docked panels, then the same step again
        click(s, "layout.full")
        s.settle(8)
        check(s("layout")["layout"] == "full" and found(s, "window.Layers") and found(s, "window.Properties"),
              "the menu bar's Full shows the docked panels")
        check(not found(s, "window.Step"), "and no step panel")
        click(s, "layout.guided")
        s.settle(8)
        check(s("layout")["layout"] == "guided" and s("layout")["step"] == "export", "Guided comes back at the same step")
        sess = json.load(open(os.path.join(s.home, ".config", "patina", "session.json")))
        check(sess.get("layout") == "guided", "the layout is remembered (session.json: %s)" % sess.get("layout"))
        # ---- unsaved work: asked before it is replaced
        s("open_window", window="welcome")
        s.settle(5)
        click(s, "welcome.new_sphere")
        s.settle(5)
        check(found(s, "replace.save") and found(s, "replace.discard"), "a new project over unsaved work asks first")
        click(s, "replace.cancel")
        s.settle(5)
        check(s("get_state")["mesh"]["faces"] > 3000, "Cancel keeps the work (the model is still there)")
        s("open_window", window="welcome")
        s.settle(5)
        click(s, "welcome.new_sphere")
        s.settle(5)
        click(s, "replace.discard")
        s.settle(8)
        check(s("get_state")["mesh"]["faces"] < 3000 and states(s)["paint"] == "todo", "Don't save starts the new project")
        # ---- no two widgets share an ID in any step (in English and in the test language)
        for lang in ("x-test", "en"):
            s("language_set", code=lang)
            s.settle(10)
            for st in ("model", "uvs", "bake", "paint", "export"):
                s("layout", step=st)
                s.settle(6)
                if st == "paint":
                    for tab in ("colors", "history", "scene", "properties"):
                        click(s, "guided.tab." + tab)
                        s.settle(4)
        r = s("ui_id_conflicts")
        check(r["checking"] and not r["conflicts"], "no two widgets share an ID in the steps (%s)" % "; ".join(r["conflicts"]))
        # ---- the command: a step by name, the feature off
        s("layout", step="uvs")
        check(s("layout")["step"] == "uvs", "the layout command goes to a step")
        s("features", id="guided_layout", on=False)
        s.settle(5)
        check(s("layout")["layout"] == "full" and found(s, "window.Layers"), "with the feature off, the Full layout")
        s("features", id="guided_layout", on=True)
    finally:
        s.close()
    shutil.rmtree(os.path.join("/tmp", "patina_guided_test"), ignore_errors=True)
    print("guided layout: %s" % ("all checks passed" if not failures else "%d FAILED" % len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
