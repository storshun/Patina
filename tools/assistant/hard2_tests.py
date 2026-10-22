"""A second hand-written test, written after round 4 was trained and never used to make data:
an honest measure of how well the Assistant understands people. Same scenes and checks as
hard_tests.py (held-out scenes, the real characters, checked by effect).

    python3 tools/assistant/hard2_tests.py          writes ~/patina-ml/data/hard2_test.jsonl
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
import common  # noqa: E402
from hard_tests import (ANY, ASK, BU, CH, COUNT, DENIM, DIRT, DUST, FIG, FU, GA, GRIME, KN, LEATHER, LH, MOSS, NAMED,  # noqa: E402,F401
                        NEW, NOT_ACT, ONE, RUST, SAY, SH, SNOW, SOOT, STATE, WEATHER, Case, L, build, setup_plan)

SLIGHT = {"t": "new_opacity", "max_range": [0.0, 0.6]}
STRONG = {"t": "new_opacity", "max_range": [0.6, 1.0]}

CASES = [
    # ---- looks
    Case(KN, "the boots need to look like he's been marching through mud for days", "look", [L(material=DIRT, region="slot:Boots_Metal"), ONE()]),
    Case(SH, "age the shield a bit", "look", [L(material=WEATHER), SLIGHT, ONE()]),
    Case(GA, "make his shoes look scuffed and dusty", "look", [L(material=DUST, region="slot:M_Shoes"), ONE()]),
    Case(FU, "give her suit a frosty winter look", "look", [L(material=SNOW, region="slot:suit_tex"), ONE()]),
    Case(BU, "I'd like the bust to look like an old forgotten statue covered in moss", "look", [L(material=MOSS), ONE()]),
    Case(LH, "make everything look slightly burnt", "look", [L(material=SOOT), SLIGHT, ONE()]),
    Case(CH, "can the head look really grimy", "look", [ANY({"material": GRIME + DIRT, "region": "region:head"}), STRONG, ONE()]),
    # ---- several at once
    Case(KN, "skin: fair skin, hair: black hair, straps: black leather", "multi",
         [L(material=["patina_skin_fair"], region="slot:Skin"), L(material=["patina_hair_black"], region="slot:Hair"),
          L(material=LEATHER, region="slot:Leather_Straps"), ONE()]),
    Case(GA, "turn the jacket into denim and add some dirt to the pants", "multi",
         [L(material=DENIM, region="slot:M_Jacket"), L(material=DIRT, region="slot:M_Pants"), ONE()]),
    Case(SH, "rust on the rim and moss on the wood", "multi", [L(material=RUST, region="slot:Rim_Iron"), L(material=MOSS, region="slot:Shield_Wood"), ONE()]),
    Case(BU, "make the head marble and the body bronze", "multi",
         [ANY({"material": ["patina_marble_white", "patina_marble_black"], "region": "slot:M_Head"},
              {"material": ["patina_marble_white", "patina_marble_black"], "region": "region:head"}),
          ANY({"material": ["patina_bronze_verdigris"], "region": "slot:M_Body"}, {"material": ["patina_bronze_verdigris"], "region": "region:body"}), ONE()]),
    # ---- colours
    Case(GA, "I want the pants in dark green fabric", "colour",
         [ANY({"tint": {"rgb": [0.08, 0.3, 0.1], "tol": 0.45}, "region": "slot:M_Pants"}, {"color": {"rgb": [0.08, 0.3, 0.1], "tol": 0.45}, "region": "slot:M_Pants"}), ONE()]),
    Case(KN, "white leather straps", "colour", [L(material=LEATHER, region="slot:Leather_Straps"), ONE()]),
    Case(SH, "make the emblem gold-coloured", "colour",
         [ANY({"material": ["patina_gold_polished"], "region": "slot:Emblem_Paint"}, {"color": {"rgb": [0.85, 0.65, 0.2], "tol": 0.4}, "region": "slot:Emblem_Paint"},
              {"tint": {"rgb": [0.85, 0.65, 0.2], "tol": 0.4}, "region": "slot:Emblem_Paint"}), ONE()]),
    # ---- painting by hand
    Case(GA, "I'm going to draw some tattoos on the arms, set me up", "paint_setup", [L(type="paint"), STATE("tool", eq="paint"), ONE()]),
    Case(KN, "prep a layer so I can hand paint rust spots on the armour", "paint_setup",
         [L(type="paint", region="slot:Armor_Plate"), STATE("tool", eq="paint"), ONE()]),
    Case(BU, "let me paint some makeup on the face", "paint_setup",
         [ANY({"type": "paint", "region": "slot:M_Head"}, {"type": "paint", "region": "region:head"}), STATE("tool", eq="paint"), ONE()]),
    # ---- adjustments
    Case(KN, "make the chainmail look less shiny", "adjust", [ANY({"adjust": {"roughness": [0.04, 1.0]}, "region": "slot:Chainmail"}), ONE()]),
    Case(FU, "boost the colours on her suit", "adjust", [ANY({"adjust": {"saturation": [1.05, 3.0]}, "region": "slot:suit_tex"}), ONE()]),
    Case(SH, "the rim should be brighter", "adjust",
         [ANY({"adjust": {"brightness": [1.05, 3.0]}, "region": "slot:Rim_Iron"}, {"color": {"rgb": [0.95, 0.95, 0.95], "tol": 0.4}, "region": "slot:Rim_Iron"}), ONE()]),
    Case(GA, "tone down the saturation of the hair a lot", "adjust", [ANY({"adjust": {"saturation": [0.0, 0.6]}, "region": "slot:M_Hair"}), ONE()]),
    # ---- layers by what they are
    Case(GA, "the edge wear is way too strong", "layer_ref", [NAMED("Edge wear", opacity=[0.0, 0.85]), ONE()]),
    Case(GA, "bring the dirt down to half", "layer_ref", [NAMED("Dirt", opacity=[0.4, 0.6]), ONE()]),
    Case(KN, "kill the grime layer", "layer_ref", [{"t": "layer_absent", "name": "Grime"}, ONE()]),
    Case(FU, "hide the base colour", "layer_ref", [NAMED("Base colour", visible=False), ONE()]),
    Case(SH, "switch the edge wear to multiply", "layer_ref", [NAMED("Edge wear", blend=["multiply"]), ONE()]),
    Case(BU, "turn the dust and the grime off", "layer_ref", [NAMED("Dust", visible=False), NAMED("Grime", visible=False), ONE()]),
    Case(LH, "rename hand painted to Details", "layer_ref", [NAMED("Details", type="paint"), ONE()]),
    # ---- everywhere except
    Case(KN, "snow on everything apart from his face", "except",
         [ANY({"material": SNOW, "region": ("except", "region:head")}, {"material": SNOW, "region": ("except", "slot:Skin")}), ONE()]),
    Case(GA, "darken everything but the skin slightly", "except",
         [ANY({"region": ("except", "slot:M_Body_Skin"), "adjust": {"brightness": [0.0, 0.95]}},
              *[{"region": ("except", "slot:M_Body_Skin"), "color": {"rgb": c, "tol": 0.3}} for c in
                ([0.04, 0.04, 0.04], [0.25, 0.15, 0.08], [0.15, 0.15, 0.16], [0.25, 0.25, 0.25], [0.5, 0.5, 0.5])]), ONE()]),
    # ---- the selection
    Case(KN, "make the selected part rusty", "selection", [L(material=RUST, region="slot:Chainmail"), ONE()], pre=[["select_part", {"part": "slot:Chainmail"}]]),
    Case(GA, "tint this blonde", "selection", [L(region="slot:M_Hair", color={"rgb": [0.95, 0.85, 0.4], "tol": 0.45}), ONE()],
         pre=[["select_part", {"part": "slot:M_Hair"}]]),
    Case(SH, "relax the uvs of what's selected", "selection", [{"t": "uv_changed"}, ONE()], pre=[["select_part", {"part": "slot:Rim_Iron"}]]),
    # ---- follow-ups
    Case(KN, "more", "followup", [NAMED("Dirt", opacity=[0.6, 1.0]), ONE()],
         setup=setup_plan("Add dirt in the creases of the boots",
                          ("add_material_layer", {"material": "patina_dirt_mud", "name": "Dirt", "strength": 0.45, "region": {"part": "slot:Boots_Metal"}, "mask": [{"type": "cavity"}]}))),
    Case(GA, "make it more orange", "followup", [NAMED("Red tint", color={"rgb": [0.95, 0.5, 0.1], "tol": 0.35}), ONE()],
         setup=setup_plan("Tint the jacket red", ("add_tint_layer", {"name": "Red tint", "color": [0.8, 0.12, 0.1], "strength": 0.5, "region": {"part": "slot:M_Jacket"}}))),
    Case(SH, "also do the rim", "followup", [NAMED("Moss", region=["slot:Shield_Wood", "slot:Rim_Iron"]), ONE()],
         setup=setup_plan("Add moss on top of the shield wood",
                          ("add_material_layer", {"material": "patina_moss", "name": "Moss", "region": {"part": "slot:Shield_Wood"}, "mask": [{"type": "facing_up"}]}))),
    Case(BU, "nah, delete it", "followup", [{"t": "layer_absent", "name": "Gold"}, ONE()],
         setup=setup_plan("Make the head gold", ("add_material_layer", {"material": "patina_gold_polished", "name": "Gold", "region": {"part": "slot:M_Head"}}))),
    Case(FU, "rename that to Suit", "followup", [NAMED("Suit", type="fill"), ONE()],
         setup=setup_plan("Make the suit leather", ("add_material_layer", {"material": "patina_leather_black", "name": "Leather", "region": {"part": "slot:suit_tex"}}))),
    # ---- the brush
    Case(GA, "double the brush size", "brush_tweak", [STATE("brush.size", range=[45, 75])], pre=[["set_brush", {"preset": "basic_soft", "size": 30}]]),
    Case(KN, "brush is too large, halve it", "brush_tweak", [STATE("brush.size", range=[25, 55])], pre=[["set_brush", {"preset": "basic_hard", "size": 80}]]),
    Case(SH, "I want a crisp hard edge on my brush", "brush_tweak", [STATE("brush.hardness", range=[0.5, 1.0])], pre=[["set_brush", {"preset": "basic_soft", "hardness": 0.2}]]),
    Case(FU, "only let me paint within one island", "brush_tweak", [STATE("brush.lock_island", eq=True)], pre=[["set_brush", {"lock_island": False}]]),
    Case(CH, "turn off the stroke smoothing", "brush_tweak", [STATE("brush.lazy_radius", eq=0)], pre=[["set_brush", {"lazy_radius": 30}]]),
    # ---- cleaning up
    Case(GA, "hide all the dirt and wear so I can see the base", "clean", [NAMED("Dirt", visible=False), NAMED("Rust", visible=False), NAMED("Edge wear", visible=False), ONE()]),
    Case(KN, "remove the grunge layers", "clean", [{"t": "layer_absent", "name": "Grime"}, ONE()]),
    # ---- the older kinds, new words
    Case(FU, "AO bake please", "bake", [{"t": "baked", "maps": ["ao"]}]),
    Case(LH, "show me the roughness", "view", [STATE("view_mode", eq="roughness")]),
    Case(GA, "I want to paint in plain white", "paint", [STATE("material.flat.color", rgb=[0.95, 0.95, 0.95], tol=0.25), STATE("material.id", eq="")]),
    Case(KN, "fill tool", "tool", [STATE("tool", eq="fill")]),
    Case(BU, "turn on symmetry", "symmetry", [STATE("symmetry.0", eq=True)]),
    Case(SH, "pack the uvs", "uv", [{"t": "uv_changed"}, ONE()]),
    Case(GA, "make the skin slightly rosier in the folds", "tint",
         [L(region="slot:M_Body_Skin", masks=["cavity"], masks_exact=False, color={"rgb": [0.88, 0.5, 0.52], "tol": 0.45}), ONE()]),
    Case(KN, "dust the top of the armour", "recipe", [L(material=DUST, region="slot:Armor_Plate", masks=["facing_up"], masks_exact=False), ONE()]),
    Case(FIG, "blacken the bottom with soot", "recipe", [L(material=SOOT, masks=["gradient_down"], masks_exact=False), ONE()]),
    # ---- not acting
    Case(GA, "can you model a backpack for him", "cant_do", NOT_ACT),
    Case(SH, "export a normal map", "window", [{"t": "any_of", "options": [NOT_ACT, [STATE("last_window", eq="export")]]}]),
    Case(FU, "make her prettier", "vague", NOT_ACT),
    Case(KN, "add rust to the shield", "unknown_part", NOT_ACT),
    Case(CH, "change the texture resolution to 2048", "resolution", [STATE("resolution", eq=2048)]),  # 0.12: done
    Case(LH, "render this out", "window", [{"t": "any_of", "options": [NOT_ACT, [STATE("last_window", eq="render")]]}]),
]


def main():
    rows = build(CASES)
    path = os.path.join(common.DATA, "hard2_test.jsonl")
    common.jsonl_write(path, rows)
    print("%d hand-written requests -> %s" % (len(rows), path))


if __name__ == "__main__":
    main()
