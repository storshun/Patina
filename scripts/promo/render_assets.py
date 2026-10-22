"""Patina's own renders for the promo: the logo cube (a hero still, and a turntable for each stage of its painting),
material spheres, and a develop before/after.

    python3 scripts/promo/render_assets.py [hero] [turntable] [spheres] [develop]
"""
import sys

from paint_cube import STAGES, SWOOSH_TEAL, idx, paint
from promo_client import Patina
from promo_paths import ROOT, WORK

R = WORK / "renders"
HDR = str(WORK / "hdr" / "courtyard.hdr")
what = sys.argv[1:] or ["hero", "turntable", "spheres", "develop"]
R.mkdir(parents=True, exist_ok=True)

p = Patina()
try:
    if "hero" in what or "turntable" in what:
        paint(p, 2048)
        p("set_environment", hdr_path=HDR, intensity=1.0)

        def aim():
            p("set_camera", yaw=0.8, pitch=0.5, frame=True)
            cam = p("get_state")["camera"]
            p("set_camera", distance=cam["distance"] * 1.3)

        aim()
        if "hero" in what:
            p("render_image", path=str(R / "cube_hero.png"), width=2000, height=2000, transparent=True, supersample=3)
        if "turntable" in what:
            # a stage for each step of the painting, its layers arriving one by one (paint_cube.STAGES)
            for si in range(len(STAGES)):
                for later in STAGES[1:]:
                    for name in later:
                        p("update_layer", index=idx(p, name), visible=STAGES.index(later) <= si)
                d = R / ("turn%d" % si)
                d.mkdir(exist_ok=True)
                aim()
                p("render_image", path=str(d / "cube.png"), width=1080, height=1080, transparent=True, supersample=2,
                  turntable_frames=240)
    if "develop" in what:
        p("open_file", path=str(ROOT / "samples" / "Suzanne.glb"))
        p("apply_smart_material", id="patina_smart_mossy_stone")
        p("bake_mesh_maps", ao=True)
        p("set_environment", hdr_path=str(WORK / "hdr" / "studio.hdr"), intensity=1.0)
        p("set_camera", view="front", frame=True)
        cam = p("get_state")["camera"]
        p("set_camera", distance=cam["distance"] * 0.8)
        p("select_layer", index=0)
        # a capture as scanners and generators deliver it: cold, dark and flat
        n = len(p("list_layers"))
        p("add_layer", type="adjust", name="As captured", index=n,
          adjust={"temperature": -0.55, "exposure": -0.8, "vibrance": -0.6, "contrast": 0.85})
        p("render_image", path=str(R / "develop_before.png"), width=1200, height=1200, transparent=True, supersample=3)
        n = len(p("list_layers"))
        p("add_layer", type="adjust", name="Develop", index=n,
          adjust={"temperature": 0.6, "exposure": 0.95, "shadows": 0.5, "highlights": -0.25, "vibrance": 0.9,
                  "delight": 0.8, "contrast": 1.12})
        p("render_image", path=str(R / "develop_after.png"), width=1200, height=1200, transparent=True, supersample=3)
    if "spheres" in what:
        mats = ["patina_gold_polished", "patina_copper", "patina_marble_white", "patina_wood_walnut",
                "patina_leather_brown", "patina_velvet_red", "patina_car_paint_blue", "patina_rust_heavy",
                "patina_carbon_fiber", "patina_bronze_verdigris", "patina_quilted_leather", "patina_damask_navy_silver",
                "patina_diamond_plate_steel", "patina_knit_wool_cream", "patina_hex_tiles_rosette", "patina_feathers_peacock"]
        p("new_project", primitive="sphere", resolution=1024)
        p("set_environment", hdr_path=HDR, intensity=1.0)
        p("set_camera", view="front", frame=True)
        d = R / "spheres"
        d.mkdir(exist_ok=True)
        for m in mats:
            r = p("add_layer", type="fill", name=m, material={"id": m})
            p("render_image", path=str(d / (m + ".png")), width=640, height=640, transparent=True, supersample=3)
            p("remove_layer", index=r["index"])
        r = p("add_layer", type="fill", name="swoosh_teal", material=SWOOSH_TEAL)
        p("render_image", path=str(d / "swoosh_teal.png"), width=640, height=640, transparent=True, supersample=3)
        p("remove_layer", index=r["index"])
finally:
    p.close()
