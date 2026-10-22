"""A hand-written test of the Assistant: requests worded the way a person types them, not the way
the data generator writes them, on scenes the model is never trained on (the held-out scenes and
three real characters). The checks look at the effect, so any sensible plan passes.

    python3 tools/assistant/hard_tests.py           writes ~/patina-ml/data/hard_test.jsonl
    python3 tools/assistant/eval.py --model ... --name r3-hard --test ~/patina-ml/data/hard_test.jsonl
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
import common  # noqa: E402
import scenes  # noqa: E402

DIRT, DUST, GRIME, SOOT, SNOW = ["patina_dirt_mud"], ["patina_dust"], ["patina_grime_oily"], ["patina_soot"], ["patina_snow"]
RUST = ["patina_rust_light", "patina_rust_heavy"]
MOSS = ["patina_moss", "patina_moss_patchy"]
LEATHER = ["patina_leather_brown", "patina_leather_black", "patina_quilted_leather", "patina_painted_leather"]
DENIM = ["patina_fabric_denim", "patina_denim_twill", "patina_denim_black"]
STEEL = ["patina_steel_brushed", "patina_steel_scratched"]
WEATHER = DIRT + DUST + GRIME + SOOT + RUST + MOSS + ["patina_steel_scratched"]
DARK = [[0.04, 0.04, 0.04], [0.25, 0.15, 0.08], [0.15, 0.15, 0.16], [0.25, 0.25, 0.25], [0.45, 0.28, 0.15]]


class Case:
    def __init__(self, scene, request, task, checks, pre=None, setup=None):
        self.scene, self.request, self.task, self.checks = scene, request, task, checks
        self.pre, self.setup = pre or [], setup or []


def build(cases):
    metas = {m["id"]: m for m in scenes.load_scenes()}
    faces = {}
    with common.Server() as s:
        for sid in sorted({c.scene for c in cases}):
            s("open_file", path=metas[sid]["path"])
            fs = {p["id"]: p["face_indices"] for p in s("mesh_parts", faces=True)["parts"]}
            fs["all"] = list(range(metas[sid]["faces"]))
            faces[sid] = fs
    out = []
    for c in cases:
        F = faces[c.scene]

        def region(r):
            if r == "all":
                return "all"
            if isinstance(r, tuple) and r[0] == "except":
                drop = set(F[r[1]])
                return [f for f in F["all"] if f not in drop]
            if isinstance(r, list):
                return sorted(set().union(*[set(F[p]) for p in r]))
            return F[r]

        def fix(e):
            e = dict(e)
            if "region" in e:
                e["region"] = region(e["region"])
                e.setdefault("min_jaccard", 0.85)
            return e

        def resolve(spec):
            spec = dict(spec)
            if "expect" in spec:
                spec["expect"] = fix(spec["expect"])
            if "expects" in spec:
                spec["expects"] = [fix(e) for e in spec["expects"]]
            if spec["t"] == "uv_parts":
                spec["parts"] = [{"faces": F[p["part"]], "islands_max": p["islands_max"]} for p in spec["parts"]]
            if "part" in spec:  # tile_faces, density_gain, selection: the part's faces (round 6)
                spec["faces"] = region(spec.pop("part"))
            if spec["t"] == "any_of":
                spec["options"] = [[resolve(x) for x in opt] for opt in spec["options"]]
            return spec

        checks = [resolve(spec) for spec in c.checks]
        # commands the eval would otherwise count as harmful, where the request asks for them
        allowed = []
        if any(k["t"] == "layer_absent" for k in checks):
            allowed.append("remove_layer")
        if any(k["t"] in ("uv_parts", "uv_changed") for k in checks):
            allowed += ["uv_make_island", "uv_unwrap", "uv_island_per_part", "uv_pack"]
        flat = [k for k in checks] + [x for k in checks if k["t"] == "any_of" for opt in k["options"] for x in opt]
        if any(k["t"] in ("density_gain", "tile_faces", "tiles_mode") for k in flat):
            allowed += ["uv_density", "texture_set_from_faces", "texture_tiles", "uv_pack"]
        if any(k["t"] in ("uv_parts", "uv_changed") for k in flat):
            allowed += ["uv_density"]
        if any(k["t"] == "state" and k.get("path") == "resolution" for k in flat):
            allowed += ["set_resolution"]
        out.append({"task": c.task, "variant": "hard", "scene": c.scene, "request": c.request, "check": checks,
                    "setup": c.setup, "pre": c.pre, "plan": {"steps": [{"cmd": x} for x in allowed]}})
    return out


# ---- check helpers
def L(**e):
    return {"t": "layer", "which": "new", "expect": e}


def ANY(*es):
    return {"t": "layer_any", "expects": list(es)}


def COUNT(n, **e):
    return {"t": "layers_count", "min": n, "expect": e}


def NEW(lo, hi=99):
    return {"t": "new_layers", "min": lo, "max": hi}


def ONE():
    return {"t": "history", "delta": 1}


def NAMED(name, **e):
    return {"t": "layer", "which": {"name": name}, "expect": e}


def STATE(path, **k):
    d = {"t": "state", "path": path}
    d.update(k)
    return d


ASK = [{"t": "plan", "kind": "ask"}, {"t": "no_change"}]
SAY = [{"t": "plan", "kind": "say"}, {"t": "no_change"}]
NOT_ACT = [{"t": "no_change"}]


def setup_plan(request, *steps):
    return [{"request": request, "plan": {"say": "Setup.", "steps": [{"cmd": c, "args": a} for c, a in steps]}}]


GA, KN, SH, BU, FIG = "char_game_A", "char_knight_T_nogroups", "prop_shield", "bust_renamed", "figure_one_slot"
FU, LH, CH = "real_fubuki", "real_longhair", "real_chibi"

CASES = [
    # ---- whole looks
    Case(GA, "make the jacket look battle-worn", "look", [COUNT(2, region="slot:M_Jacket"), L(region="slot:M_Jacket", masks=["edges"], masks_exact=False), ONE()]),
    Case(KN, "can you make the armour plates look really old and rusty", "look", [COUNT(2, region="slot:Armor_Plate"), L(material=RUST, region="slot:Armor_Plate"), ONE()]),
    Case(SH, "I want the whole shield to look like it's been through a war", "look", [COUNT(2, region="all"), ONE()]),
    Case(FU, "make her look like she's been out in the snow", "look", [L(material=SNOW), ONE()]),
    Case(BU, "make it look old and neglected", "look", [COUNT(2, region="all"), ONE()]),
    Case(LH, "give the whole thing a dusty, abandoned feel", "look", [L(material=DUST, region="all"), ONE()]),
    Case(GA, "the shoes should look muddy, like after a long hike", "look", [L(material=DIRT, region="slot:M_Shoes"), ONE()]),
    Case(KN, "scorch the chainmail", "look", [L(material=SOOT, region="slot:Chainmail"), ONE()]),
    Case(SH, "make the rim look corroded", "look", [L(material=RUST, region="slot:Rim_Iron"), ONE()]),
    Case(FIG, "make it look overgrown with moss", "look", [L(material=MOSS, region="all"), ONE()]),
    Case(CH, "make the body look grimy", "look", [ANY({"material": GRIME + DIRT, "region": "region:body"}), ONE()]),
    Case(GA, "make the pants look slightly worn and dirty", "look", [L(material=WEATHER, region="slot:M_Pants"), {"t": "new_opacity", "max_range": [0.0, 0.5]}, ONE()]),
    Case(KN, "give everything a weathered look", "look", [COUNT(2, region="all"), ONE()]),
    Case(FU, "make the suit look like it's been dragged through mud", "look", [L(material=DIRT, region="slot:suit_tex"), ONE()]),
    # ---- several things in one request
    Case(GA, "make the jacket black leather, the pants blue denim and the shoes rubber", "multi",
         [L(material=LEATHER, region="slot:M_Jacket"), L(material=DENIM, region="slot:M_Pants"), L(material=["patina_rubber"], region="slot:M_Shoes"), ONE()]),
    Case(KN, "chainmail should be iron and the leather straps brown leather", "multi",
         [L(material=["patina_iron_raw", "patina_cast_iron"], region="slot:Chainmail"), L(material=LEATHER, region="slot:Leather_Straps"), ONE()]),
    Case(SH, "wood for the shield, gold for the emblem, and rust on the rim's edges", "multi",
         [L(material=["patina_wood_oak", "patina_wood_walnut", "patina_wood_pine", "patina_wood_cherry"], region="slot:Shield_Wood"),
          L(material=["patina_gold_polished"], region="slot:Emblem_Paint"), L(material=RUST, region="slot:Rim_Iron", masks=["edges"], masks_exact=False), ONE()]),
    Case(GA, "dirt on the shoes and dust on the hair please", "multi", [L(material=DIRT, region="slot:M_Shoes"), L(material=DUST, region="slot:M_Hair"), ONE()]),
    Case(FU, "make the suit red leather and tint her face slightly pink", "multi",
         [L(material=LEATHER, region="slot:suit_tex"), ANY({"color": {"rgb": [0.95, 0.55, 0.65], "tol": 0.45}, "region": "slot:Material_face"}), ONE()]),
    Case(BU, "put dust on the head and grime on the body", "multi",
         [ANY({"material": DUST, "region": "slot:M_Head"}, {"material": DUST, "region": "region:head"}),
          ANY({"material": GRIME, "region": "slot:M_Body"}, {"material": GRIME, "region": "region:body"}), ONE()]),
    # ---- coloured materials and colours
    Case(GA, "I want a red leather jacket", "colour", [L(material=LEATHER, region="slot:M_Jacket", tint={"rgb": [0.8, 0.12, 0.1], "tol": 0.45}), ONE()]),
    Case(KN, "make the hair dark brown", "colour",
         [ANY({"color": {"rgb": [0.25, 0.15, 0.08], "tol": 0.3}, "region": "slot:Hair"}, {"material": ["patina_hair_brown"], "region": "slot:Hair"}), ONE()]),
    Case(SH, "paint the emblem navy blue", "colour",
         [ANY({"color": {"rgb": [0.06, 0.1, 0.35], "tol": 0.35}, "region": "slot:Emblem_Paint"},
              {"tint": {"rgb": [0.06, 0.1, 0.35], "tol": 0.35}, "region": "slot:Emblem_Paint"}), ONE()]),
    Case(GA, "the pants should be green cloth", "colour", [L(region="slot:M_Pants", tint={"rgb": [0.15, 0.6, 0.2], "tol": 0.45}), ONE()]),
    # ---- getting ready to paint by hand
    Case(GA, "I want to paint some scratches on the jacket by hand", "paint_setup",
         [L(type="paint", region="slot:M_Jacket"), STATE("tool", eq="paint"), STATE("brush.preset", eq="scratches"), ONE()]),
    Case(FU, "get me set up to paint freckles on her face", "paint_setup",
         [ANY({"type": "paint", "region": "slot:Material_face"}, {"type": "paint", "region": "region:head"}), STATE("tool", eq="paint"), ONE()]),
    Case(KN, "let me hand paint dirt on the boots", "paint_setup",
         [L(type="paint", region="slot:Boots_Metal"), STATE("material.id", **{"in": DIRT}), STATE("tool", eq="paint"), ONE()]),
    Case(LH, "I'd like to paint some blood splatter myself", "paint_setup",
         [L(type="paint"), STATE("material.flat.color", rgb=[0.45, 0.05, 0.05], tol=0.35), STATE("tool", eq="paint"), ONE()]),
    Case(CH, "set up a brush and a layer for drawing cracks", "paint_setup", [L(type="paint"), STATE("brush.preset", eq="cracks"), ONE()]),
    # ---- layers by what they are
    Case(GA, "turn the rust down", "layer_ref", [NAMED("Rust", opacity=[0.0, 0.85]), ONE()]),
    Case(GA, "the dirt is too much", "layer_ref", [NAMED("Dirt", opacity=[0.0, 0.85]), ONE()]),
    Case(KN, "hide the grime for a sec", "layer_ref", [NAMED("Grime", visible=False), ONE()]),
    Case(BU, "get rid of the dust layer", "layer_ref", [{"t": "layer_absent", "name": "Dust"}, ONE()]),
    Case(FU, "turn off the dust", "layer_ref", [NAMED("Dust", visible=False), ONE()]),
    Case(SH, "delete the edge wear", "layer_ref", [{"t": "layer_absent", "name": "Edge wear"}, ONE()]),
    Case(GA, "rename the base layer to Undercoat", "layer_ref", [{"t": "layer_absent", "name": "Base"}, NAMED("Undercoat", type="fill"), ONE()]),
    Case(GA, "hide the dirt and the rust", "layer_ref", [NAMED("Dirt", visible=False), NAMED("Rust", visible=False), ONE()]),
    Case(LH, "set the base layer to 50% opacity", "layer_ref", [NAMED("Base", opacity=[0.49, 0.51]), ONE()]),
    # ---- everywhere except
    Case(GA, "dirt everywhere except the head", "except", [ANY({"material": DIRT, "region": ("except", "group:Head")}, {"material": DIRT, "region": ("except", "region:head")}), ONE()]),
    Case(KN, "dust over everything but the skin", "except", [L(material=DUST, region=("except", "slot:Skin")), ONE()]),
    Case(FU, "darken everything except the face a little", "except", [L(region=("except", "slot:Material_face")), ONE()]),
    # ---- the selection
    Case(GA, "make these gold", "selection", [L(material=["patina_gold_polished"], region="slot:M_Shoes"), ONE()], pre=[["select_part", {"part": "slot:M_Shoes"}]]),
    Case(KN, "add rust to the edges of what I selected", "selection", [L(material=RUST, region="slot:Leather_Straps"), ONE()], pre=[["select_part", {"part": "slot:Leather_Straps"}]]),
    Case(SH, "give this its own UV island", "selection", [{"t": "uv_parts", "parts": [{"part": "slot:Emblem_Paint", "islands_max": 1}], "overlaps_max": 99}, ONE()],
         pre=[["select_part", {"part": "slot:Emblem_Paint"}]]),
    Case(FU, "tint the selected faces slightly red", "selection", [L(region="piece:3", color={"rgb": [0.8, 0.12, 0.1], "tol": 0.35}, opacity=[0.1, 0.35]), ONE()],
         pre=[["select_part", {"part": "piece:3"}]]),
    # ---- follow-ups
    Case(GA, "a bit less", "followup", [NAMED("Red tint", opacity=[0.0, 0.5]), ONE()],
         setup=setup_plan("Tint the jacket noticeably red in the creases",
                          ("add_tint_layer", {"name": "Red tint", "color": [0.8, 0.12, 0.1], "strength": 0.55, "region": {"part": "slot:M_Jacket"}, "mask": [{"type": "cavity"}]}))),
    Case(KN, "actually remove that", "followup", [{"t": "layer_absent", "name": "Gold"}, ONE()],
         setup=setup_plan("Make the chainmail gold", ("add_material_layer", {"material": "patina_gold_polished", "name": "Gold", "region": {"part": "slot:Chainmail"}}))),
    Case(SH, "put it on the rim too", "followup", [NAMED("Dirt", region=["slot:Shield_Wood", "slot:Rim_Iron"]), ONE()],
         setup=setup_plan("Add dirt in the creases of the shield wood",
                          ("add_material_layer", {"material": "patina_dirt_mud", "name": "Dirt", "region": {"part": "slot:Shield_Wood"}, "mask": [{"type": "cavity"}]}))),
    Case(BU, "make it pink instead", "followup", [NAMED("Red tint", color={"rgb": [0.95, 0.55, 0.65], "tol": 0.35}), ONE()],
         setup=setup_plan("Tint the head slightly red", ("add_tint_layer", {"name": "Red tint", "color": [0.8, 0.12, 0.1], "strength": 0.25, "region": {"part": "slot:M_Head"}}))),
    Case(GA, "hide it for now", "followup", [NAMED("Leather", visible=False), ONE()],
         setup=setup_plan("Make the jacket leather", ("add_material_layer", {"material": "patina_leather_brown", "name": "Leather", "region": {"part": "slot:M_Jacket"}}))),
    # ---- the older kinds of request, worded differently
    Case(GA, "can u make the skin a lil redder in the creases", "tint", [L(region="slot:M_Body_Skin", masks=["cavity"], color={"rgb": [0.8, 0.12, 0.1], "tol": 0.45}), ONE()]),
    Case(KN, "slap some rust on the armor", "recipe", [L(material=RUST, region="slot:Armor_Plate"), ONE()]),
    Case(SH, "the wood needs to be darker", "tint", [ANY(*([{"color": {"rgb": c, "tol": 0.25}, "region": "slot:Shield_Wood"} for c in DARK] +
                                                         [{"adjust": {"brightness": [0.0, 0.95]}, "region": "slot:Shield_Wood"}])), ONE()]),
    Case(GA, "make the jacket a little less saturated", "adjust", [ANY({"adjust": {"saturation": [0.0, 0.95]}, "region": "slot:M_Jacket"}), ONE()]),
    Case(KN, "the armour plates are too shiny", "adjust", [ANY({"adjust": {"roughness": [0.04, 1.0]}, "region": "slot:Armor_Plate"}), ONE()]),
    Case(FU, "bake ambient occlusion and curvature please", "bake", [{"t": "baked", "maps": ["ao", "curvature"]}]),
    Case(LH, "show me the uv checker", "view", [STATE("view_mode", eq="uv_checker")]),
    Case(GA, "switch to the eraser", "tool", [STATE("tool", eq="erase")]),
    Case(KN, "give me a big soft brush", "brush", [STATE("brush.preset", eq="basic_soft")]),
    Case(BU, "paint with brushed steel", "paint", [STATE("material.id", eq="patina_steel_brushed")]),
    Case(GA, "mirror my painting left to right", "symmetry", [STATE("symmetry.0", eq=True)]),
    Case(FIG, "unwrap it automatically", "uv", [{"t": "uv_changed"}, ONE()]),
    Case(GA, "make the jacket's edges look worn so metal shows through", "recipe", [L(region="slot:M_Jacket", masks=["edges"], masks_exact=False), ONE()]),
    Case(BU, "put a soft red blush in the creases of the head", "tint",
         [ANY({"masks": ["cavity"], "region": "slot:M_Head"}, {"masks": ["cavity"], "region": "region:head"}), ONE()]),
    Case(KN, "heavy dirt near the ground", "recipe", [L(material=DIRT, masks=["gradient_down"], masks_exact=False, opacity=[0.6, 1.0]), ONE()]),
    Case(SH, "snow on top of the shield", "recipe", [ANY({"material": SNOW, "masks": ["facing_up"], "masks_exact": False}), ONE()]),
    Case(GA, "even out the texel density", "uv", [{"t": "uv_changed"}, ONE()]),
    # ---- round 4: the brush as it is now, cleaning up, what it can do
    Case(GA, "this brush is huge, shrink it", "brush_tweak", [STATE("brush.size", range=[10, 100])], pre=[["set_brush", {"preset": "basic_soft", "size": 120}]]),
    Case(KN, "can I get a slightly larger brush", "brush_tweak", [STATE("brush.size", range=[24, 80])], pre=[["set_brush", {"preset": "airbrush", "size": 20}]]),
    Case(SH, "my lines are wobbly, can you smooth them out", "brush_tweak", [STATE("brush.lazy_radius", range=[5, 200])]),
    Case(FU, "I need a softer brush tip", "brush_tweak", [STATE("brush.hardness", range=[0.0, 0.75])], pre=[["set_brush", {"preset": "basic_hard", "hardness": 0.9}]]),
    Case(GA, "strip all the grunge off it for now", "clean", [NAMED("Dirt", visible=False), NAMED("Rust", visible=False), ONE()]),
    Case(BU, "delete every weathering layer", "clean", [{"t": "layer_absent", "name": "Dust"}, {"t": "layer_absent", "name": "Grime"}, ONE()]),
    Case(CH, "what are you able to help me with?", "cant_do2", SAY),
    Case(GA, "can you undo what you just did", "cant_do2", NOT_ACT),
    # ---- knowing when not to act
    Case(SH, "export the textures for unity", "window", [STATE("last_window", eq="export")]),  # 0.12: opens the window
    Case(CH, "make it look cool", "vague", NOT_ACT),
    Case(GA, "add rust to the tail", "unknown_part", NOT_ACT),
    Case(FIG, "make the hands gold", "unknown_part", NOT_ACT),
    Case(KN, "save my work", "cant_do", SAY),
    Case(LH, "add a sword to her hand", "cant_do", NOT_ACT),
]


def main():
    rows = build(CASES)
    path = os.path.join(common.DATA, "hard_test.jsonl")
    common.jsonl_write(path, rows)
    print("%d hand-written requests -> %s" % (len(rows), path))


if __name__ == "__main__":
    main()
