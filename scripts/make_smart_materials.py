#!/usr/bin/env python3
"""Builds the built-in smart materials (smart_materials/patina_smart_*.json) with a headless Patina.

    python3 scripts/make_smart_materials.py [--patina build/patina] [--shots DIR] [--only id,id]
"""
import argparse
import base64
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(__file__))
from patina_client import Client  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

def fill(mid, name, gen=None, opacity=1.0, channels=None, scale=None, fx=None, blend=None):
    """A fill layer. mid: library id, or a dict of flat values {color, roughness, metallic}. fx: mask effect
    stack (update_layer mask_effects); blend: layer blend mode."""
    d = {"type": "fill", "name": name, "material": dict(mid) if isinstance(mid, dict) else {"id": mid}}
    if scale:
        d["material"]["scale"] = scale
    if gen:
        d["generator"] = gen
    upd = {}
    if channels:
        upd["channels"] = channels
    if fx:
        upd["mask_effects"] = fx
    if blend:
        upd["blend"] = blend
    return d, opacity, upd or None


def adjust(name, adj, gen=None, opacity=1.0, fx=None):
    d = {"type": "adjust", "name": name, "adjust": adj}
    if gen:
        d["generator"] = gen
    return d, opacity, {"mask_effects": fx} if fx else None


def flat(color, roughness, metallic=0.0):
    return {"color": color, "roughness": roughness, "metallic": metallic}


def gen(type_, level=0.5, contrast=0.6, noise=0.0, noise_scale=1.0, invert=False, **kw):
    return dict({"type": type_, "level": level, "contrast": contrast, "noise": noise, "noise_scale": noise_scale,
                 "invert": invert}, **kw)

SMART = [
    ("patina_smart_worn_painted_metal", "Worn painted metal",
     "Scratched steel under blue paint that wears off on the edges, with dirt in the cavities.",
     [fill("patina_steel_scratched", "Bare steel"),
      fill("patina_car_paint_blue", "Paint", {"type": "edges", "level": 0.58, "contrast": 0.75, "noise": 0.55, "noise_scale": 3.0, "invert": True}),
      fill("patina_dirt_mud", "Cavity dirt", {"type": "cavity", "level": 0.55, "contrast": 0.6, "noise": 0.35}, 0.75)]),
    ("patina_smart_rusted_iron", "Rusted iron",
     "Cast iron with heavy rust in occluded areas, lighter rust patches and worn bright edges.",
     [fill("patina_cast_iron", "Cast iron"),
      fill("patina_rust_light", "Light rust", {"type": "noise", "level": 0.45, "contrast": 0.5, "noise_scale": 1.5}),
      fill("patina_rust_heavy", "Heavy rust", {"type": "occlusion", "level": 0.35, "contrast": 0.55, "noise": 0.6, "noise_scale": 2.0}),
      fill("patina_steel_brushed", "Worn edges", {"type": "edges", "level": 0.7, "contrast": 0.8, "noise": 0.4, "noise_scale": 4.0}, 0.8)]),
    ("patina_smart_dusty_plastic", "Dusty plastic",
     "Satin gray plastic with dust settled on upward-facing parts and grime in the cavities.",
     [fill("patina_plastic_satin_gray", "Plastic"),
      fill("patina_dust", "Dust from above", {"type": "gradient_y", "level": 0.6, "contrast": 0.45, "noise": 0.45, "noise_scale": 2.5}, 0.85),
      fill("patina_dirt_mud", "Grime", {"type": "cavity", "level": 0.6, "contrast": 0.6, "noise": 0.3}, 0.6)]),
    ("patina_smart_weathered_wood", "Weathered wood",
     "Oak bleached by the weather, with dirt in the cracks and lighter worn edges.",
     [fill("patina_wood_oak", "Oak"),
      ({"type": "adjust", "name": "Bleach", "adjust": {"saturation": 0.55, "brightness": 1.12, "roughness": 0.15}}, 1.0, None),
      fill("patina_dirt_mud", "Dirt", {"type": "cavity", "level": 0.55, "contrast": 0.6, "noise": 0.4}, 0.8),
      fill("patina_wood_pine", "Worn edges", {"type": "edges", "level": 0.65, "contrast": 0.7, "noise": 0.5, "noise_scale": 3.0}, 0.6)]),
    ("patina_smart_mossy_stone", "Mossy stone",
     "Granite with moss growing on top and dark dirt in the crevices.",
     [fill("patina_stone_granite", "Granite"),
      fill("patina_dirt_mud", "Crevice dirt", {"type": "cavity", "level": 0.5, "contrast": 0.6, "noise": 0.3}, 0.85),
      fill("patina_moss_patchy", "Moss", {"type": "gradient_y", "level": 0.55, "contrast": 0.6, "noise": 0.65, "noise_scale": 2.0})]),
    ("patina_smart_aged_brass", "Aged brass",
     "Tarnished brass with verdigris in the cavities and polished edges where hands touch it.",
     [fill("patina_brass_aged", "Brass"),
      fill("patina_bronze_verdigris", "Verdigris", {"type": "cavity", "level": 0.5, "contrast": 0.65, "noise": 0.45}),
      fill("patina_gold_polished", "Polished edges", {"type": "edges", "level": 0.68, "contrast": 0.75, "noise": 0.35}, 0.7)]),
    ("patina_smart_snow_cover", "Snow cover",
     "An overlay: snow on everything that faces up. Apply it on top of any other material.",
     [fill("patina_snow", "Snow", {"type": "gradient_y", "level": 0.62, "contrast": 0.8, "noise": 0.4, "noise_scale": 3.0})]),
    ("patina_smart_edge_wear_overlay", "Edge wear + grime overlay",
     "An overlay that lightens worn edges and darkens cavities of whatever is below it.",
     [({"type": "adjust", "name": "Worn edges", "adjust": {"brightness": 1.45, "saturation": 0.8, "roughness": -0.2},
        "generator": {"type": "edges", "level": 0.62, "contrast": 0.75, "noise": 0.45, "noise_scale": 3.0}}, 1.0, None),
      ({"type": "adjust", "name": "Grime", "adjust": {"brightness": 0.55, "roughness": 0.25},
        "generator": {"type": "cavity", "level": 0.55, "contrast": 0.6, "noise": 0.3}}, 0.85, None)]),
    # ---- 0.8
    ("patina_smart_scratched_paint", "Scratched painted steel",
     "Red enamel over brushed steel: worn through on the edges and chipped in places, oily grime in the cavities.",
     [fill("patina_steel_brushed", "Bare steel"),
      fill(flat([0.55, 0.09, 0.07], 0.42), "Red enamel", fx=[
          {"type": "fill", "value": 1.0},
          {"type": "generator", "blend": "subtract", "generator": gen("edges", 0.64, 0.8, 0.45, 4.0)},
          {"type": "generator", "blend": "subtract", "generator": gen("noise", 0.74, 0.9, 0.0, 9.0)}]),
      fill("patina_grime_oily", "Grime", gen("cavity", 0.55, 0.6, 0.35), 0.8)]),
    ("patina_smart_oxidized_copper", "Oxidized copper",
     "Copper darkened by tarnish, chalky verdigris in the cavities and sheltered areas, rubbed bright on the edges.",
     [fill("patina_copper", "Copper"),
      adjust("Tarnish", {"brightness": 0.62, "saturation": 0.85, "roughness": 0.22},
             gen("noise", 0.42, 0.45, 0.0, 1.5)),
      fill("patina_verdigris_crust", "Verdigris", gen("occlusion", 0.3, 0.6, 0.55, 2.5)),
      fill("patina_verdigris_crust", "Verdigris in cavities", gen("cavity", 0.52, 0.7, 0.3)),
      fill("patina_copper", "Polished edges", gen("edges", 0.66, 0.75, 0.35, 3.0), 0.85)]),
    ("patina_smart_grimy_plastic", "Grimy plastic",
     "Off-white plastic yellowed with age, oily grime in the creases and smudged, duller patches.",
     [fill("patina_plastic_white", "Plastic"),
      fill(flat([0.8, 0.72, 0.5], 0.55), "Yellowing", gen("noise", 0.4, 0.3, 0.0, 1.2), 0.75),
      adjust("Smudges", {"brightness": 0.93, "roughness": 0.18}, gen("noise", 0.55, 0.6, 0.0, 4.0)),
      fill("patina_grime_oily", "Grime", gen("occlusion", 0.5, 0.55, 0.45, 3.0), 0.55),
      fill("patina_grime_oily", "Cavity grime", gen("cavity", 0.58, 0.7, 0.3), 0.8)]),
    ("patina_smart_worn_leather", "Worn leather",
     "Brown leather with dark creases, pale scuffed edges and lighter worn patches.",
     [fill("patina_leather_brown", "Leather"),
      adjust("Dark creases", {"brightness": 0.55, "saturation": 1.15, "roughness": 0.1}, gen("cavity", 0.52, 0.6, 0.3)),
      adjust("Scuffs", {"brightness": 1.18, "saturation": 0.85, "roughness": 0.1}, gen("noise", 0.58, 0.7, 0.0, 5.0), 0.7),
      adjust("Worn edges", {"brightness": 1.45, "saturation": 0.7, "roughness": 0.15},
             gen("edges", 0.62, 0.7, 0.5, 3.0))]),
    ("patina_smart_dirty_fabric", "Dirty fabric",
     "Denim faded on the edges, grime in the folds and mud splashed up from the ground.",
     [fill("patina_denim_twill", "Denim"),
      adjust("Faded", {"brightness": 1.35, "saturation": 0.6}, gen("edges", 0.6, 0.55, 0.45, 3.0)),
      fill("patina_dust", "Grime in folds", gen("occlusion", 0.35, 0.55, 0.4, 2.0), 0.75),
      fill("patina_dirt_mud", "Mud from below", gen("gradient_y", 0.22, 0.55, 0.55, 3.0, True))]),
    ("patina_smart_mossy_wood", "Mossy wood",
     "Damp oak darkened in sheltered places, dirt in the cracks and moss on the surfaces that face up.",
     [fill("patina_wood_oak", "Oak"),
      adjust("Damp", {"brightness": 0.62, "saturation": 0.8, "roughness": -0.1}, gen("occlusion", 0.3, 0.5, 0.4, 2.0)),
      fill("patina_dirt_mud", "Dirt", gen("cavity", 0.55, 0.6, 0.4), 0.8),
      fill("patina_moss", "Moss on top", gen("facing", 0.72, 0.6, 0.6, 2.5, direction=[0, 1, 0]))]),
    ("patina_smart_heat_tinted_metal", "Heat-tinted metal",
     "Brushed steel tinted by heat in bands from straw to purple to blue (bottom to top), soot in the cavities.",
     [fill("patina_steel_brushed", "Steel"),
      fill(flat([0.78, 0.62, 0.36], 0.3, 1.0), "Straw", gen("gradient_y", 0.3, 0.55, 0.35, 1.5)),
      fill(flat([0.5, 0.33, 0.58], 0.3, 1.0), "Purple", gen("gradient_y", 0.5, 0.55, 0.35, 1.5)),
      fill(flat([0.3, 0.42, 0.78], 0.3, 1.0), "Blue", gen("gradient_y", 0.68, 0.55, 0.35, 1.5)),
      fill("patina_soot", "Soot", gen("cavity", 0.6, 0.65, 0.4), 0.8)]),
    ("patina_smart_stylized_edges", "Stylized edge highlights",
     "A hand-painted overlay: crisp light edges, dark painted cavities and light from above. Put it on top of a "
     "toon or hand-painted material.",
     [adjust("Top light", {"brightness": 1.12}, gen("gradient_y", 0.6, 0.35)),
      adjust("Painted shadow", {"brightness": 0.6, "saturation": 1.15}, gen("cavity", 0.55, 0.9, 0.1)),
      adjust("Edge highlight", {"brightness": 1.4, "saturation": 0.85}, gen("edges", 0.62, 0.92, 0.15, 2.0))]),
    ("patina_smart_worn_gold_trim", "Worn gold trim",
     "Gilded trim, dulled and tarnished in patches, dark grime in the recesses and rubbed bright on the edges.",
     [fill("patina_gold_polished", "Gold"),
      adjust("Tarnish", {"brightness": 0.72, "saturation": 0.8, "roughness": 0.25}, gen("noise", 0.5, 0.5, 0.0, 2.0)),
      fill("patina_grime_oily", "Grime", gen("cavity", 0.52, 0.65, 0.3)),
      fill("patina_grime_oily", "Dark recesses", gen("occlusion", 0.35, 0.55, 0.3, 2.0), 0.7),
      adjust("Rubbed edges", {"brightness": 1.25, "roughness": -0.2}, gen("edges", 0.62, 0.75, 0.3, 3.0))]),
    ("patina_smart_scifi_panel_worn", "Worn sci-fi panels",
     "White hull paneling with gunmetal showing through on worn edges, grime in the grooves and dust on top.",
     [fill("patina_gunmetal", "Gunmetal"),
      fill("patina_panels_white", "Panels", gen("edges", 0.72, 0.85, 0.25, 4.0, True), scale=0.5),
      fill("patina_grime_oily", "Grime", gen("cavity", 0.55, 0.65, 0.3), 0.85),
      fill("patina_dust", "Dust on top", gen("facing", 0.75, 0.5, 0.4, 2.0, direction=[0, 1, 0]), 0.6)]),
    ("patina_smart_living_skin", "Living skin",
     "Light skin with redness in the creases and warm, translucent-looking thin parts (ears, nose, fingers).",
     [fill("patina_skin_light", "Skin"),
      fill(flat([0.78, 0.42, 0.36], 0.55), "Redness in creases", gen("occlusion", 0.3, 0.45, 0.3, 2.0), 0.45),
      fill(flat([0.85, 0.42, 0.34], 0.5), "Warm thin parts", gen("thin", 0.55, 0.5, 0.2, 2.0), 0.6)]),
    # 0.24: the new masks
    ("patina_smart_rain_streaked_concrete", "Rain-streaked concrete",
     "Concrete with dark rain streaks running down the sides (Drips) and dirt in the cavities.",
     [fill("patina_concrete", "Concrete"),
      fill(flat([0.16, 0.15, 0.13], 0.88), "Rain streaks", opacity=0.75, channels=["basecolor", "roughness"],
           fx=[{"type": "generator", "generator": gen("drips", level=0.25, contrast=0.5, noise_scale=1.2)},
               {"type": "blur", "radius": 2.0}]),
      fill("patina_dirt_mud", "Cavity dirt", {"type": "cavity", "level": 0.55, "contrast": 0.6, "noise": 0.35}, 0.6)]),
    ("patina_smart_chipped_paint_primer", "Chipped paint with primer",
     "Blue-gray paint chipped off the edges, a rim of red-oxide primer round each chip (Grow / shrink), steel in the middle.",
     [fill("patina_steel_scratched", "Bare steel"),
      fill(flat([0.5, 0.2, 0.13], 0.65), "Red-oxide primer",
           fx=[{"type": "generator", "generator": gen("edges", level=0.58, contrast=0.8, noise=0.55, noise_scale=3.0)},
               {"type": "grow", "amount": -3.0}, {"type": "invert"}]),
      fill(flat([0.3, 0.4, 0.48], 0.45), "Paint",
           fx=[{"type": "generator", "generator": gen("edges", level=0.58, contrast=0.8, noise=0.55, noise_scale=3.0)},
               {"type": "invert"}]),
      fill("patina_dirt_mud", "Cavity dirt", {"type": "cavity", "level": 0.55, "contrast": 0.6, "noise": 0.35}, 0.6)]),
    ("patina_smart_rust_runs", "Rust runs",
     "Cream paint with rust on the edges and runs of rust streaking down from them (Drips).",
     [fill(flat([0.78, 0.74, 0.62], 0.5), "Cream paint"),
      fill("patina_rust_heavy", "Edge rust", {"type": "edges", "level": 0.62, "contrast": 0.7, "noise": 0.5, "noise_scale": 2.5}),
      fill(flat([0.42, 0.18, 0.07], 0.85), "Runs of rust", opacity=0.85, channels=["basecolor", "roughness", "metallic"],
           fx=[{"type": "generator", "generator": gen("drips", level=0.25, contrast=0.6, noise_scale=1.0)},
               {"type": "blur", "radius": 1.5}])]),
]

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--patina", default=os.path.join(ROOT, "build", "patina"))
    ap.add_argument("--port", type=int, default=7871)
    ap.add_argument("--shots", default="")
    ap.add_argument("--only", default="", help="comma-separated ids (or parts of ids) to rebuild")
    a = ap.parse_args()
    todo = [sm for sm in SMART if not a.only or any(k in sm[0] for k in a.only.split(","))]
    home = tempfile.mkdtemp(prefix="patina-smart-")
    env = dict(os.environ, XDG_DATA_HOME=os.path.join(home, "data"), XDG_CONFIG_HOME=os.path.join(home, "cfg"),
               XDG_CACHE_HOME=os.path.join(home, "cache"))
    p = subprocess.Popen([a.patina, "--headless", "--port", str(a.port)], env=env, stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL)
    try:
        for _ in range(100):
            try:
                c = Client(a.port)
                break
            except OSError:
                time.sleep(0.1)
        out_dir = os.path.join(ROOT, "smart_materials")
        os.makedirs(out_dir, exist_ok=True)
        for sid, name, desc, layers in todo:
            c("new_project", primitive="torus", resolution=1024)
            c("bake_mesh_maps", ao=True, curvature=True)
            folder = c("add_layer", type="folder", name=name)["index"]
            for spec, opacity, upd in layers:
                folder = [l["index"] for l in c("list_layers") if l["name"] == name][0]
                r = c("add_layer", **dict(spec, parent=folder))
                if opacity != 1.0 or upd:
                    c("update_layer", index=r["index"], opacity=opacity, **(upd or {}))
            idx = [l["index"] for l in c("list_layers") if l["name"] == name][0]
            path = c("save_smart_material", index=idx, name=name, description=desc)["path"]
            dst = os.path.join(out_dir, sid + ".json")
            shutil.copy(path, dst)
            print("wrote", os.path.relpath(dst, ROOT))
        if a.shots:  # previews: apply each saved smart material to Suzanne
            os.makedirs(a.shots, exist_ok=True)
            env_dir = os.path.join(home, "data", "patina", "user", "smart_materials")
            shutil.rmtree(env_dir, ignore_errors=True)  # use the files in smart_materials/
            for sid, name, _, _ in todo:
                c("open_mesh", path=os.path.join(ROOT, "samples", "Suzanne.glb"), resolution=1024)
                c("bake_mesh_maps", ao=True, curvature=True)
                # overlays are shown on top of another smart material or a plain fill
                under = {"patina_smart_snow_cover": "patina_smart_mossy_stone",
                         "patina_smart_edge_wear_overlay": "patina_plastic_glossy_red",
                         "patina_smart_stylized_edges": "patina_toon_cloth_red"}.get(sid, "")
                if under.startswith("patina_smart_"):
                    c("apply_smart_material", id=os.path.join(out_dir, under + ".json"))
                elif under:
                    c("add_layer", type="fill", material={"id": under})
                c("apply_smart_material", id=os.path.join(out_dir, sid + ".json"))
                c("set_view", seams=False, wireframe=False)
                c("set_camera", view="front", frame=True)
                r = c("render_view", width=360, height=360, camera={"yaw": 0.45, "pitch": 0.2})
                with open(os.path.join(a.shots, sid + ".png"), "wb") as f:
                    f.write(base64.b64decode(r["image_png_base64"]))
    finally:
        p.kill()
        shutil.rmtree(home, ignore_errors=True)

if __name__ == "__main__":
    main()
