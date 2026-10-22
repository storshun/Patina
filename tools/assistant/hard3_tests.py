"""A third hand-written test, for round 6 (Patina 0.12): written before round 6's data and never
used to make it. The new abilities (texture sets, texture space, resolution, glow, selections,
windows, text stencils, questions) and the honest "I can't, but..." answers, on held-out scenes,
the real characters and Randall's own Mai body (with the requests from his 0.11 history).

    python3 tools/assistant/hard3_tests.py          writes ~/patina-ml/data/hard3_test.jsonl
"""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
import common  # noqa: E402
from hard_tests import ANY, ASK, BU, CH, FIG, FU, GA, KN, L, LH, NAMED, NOT_ACT, ONE, SAY, SH, STATE, Case, build  # noqa: E402,F401

MAI = "real_mai"


def TILE(part, j=0.85, name=None):
    d = {"t": "tile_faces", "part": part, "min_jaccard": j}
    if name:
        d["name"] = name
    return d


def MODE(m):
    return {"t": "tiles_mode", "mode": m}


def GAIN(part, lo=None, hi=None):
    d = {"t": "density_gain", "part": part}
    if lo is not None:
        d["min"] = lo
    if hi is not None:
        d["max"] = hi
    return d


def ONEOF(*options):
    return {"t": "any_of", "options": [list(o) for o in options]}


def SEL(part):
    return {"t": "selection", "part": part, "min_jaccard": 0.97}


def WIN(w):
    return STATE("last_window", eq=w)


def SAYS(*words):
    return {"t": "say_contains", "any": list(words)}


SUGG = {"t": "suggest", "min": 1}
CANT = SAY + [SUGG]
GLOW = {"type": "fill", "channel_mask": 32}

CASES = [
    # ---- Randall's own requests from 0.11, on his model
    Case(MAI, "select all the head faces", "select", [SEL("region:head")]),
    Case(MAI, "add the head to a new udim tile", "texset", [TILE("region:head", 0.8)]),
    Case(MAI, "give the head more UV space", "density", [ONEOF([GAIN("region:head", lo=1.3)], [TILE("region:head", 0.8)])]),
    Case(MAI, "give the head more texture space", "density", [ONEOF([GAIN("region:head", lo=1.3)], [TILE("region:head", 0.8)])]),
    Case(MAI, "put the head on a separate uv map", "texset", [TILE("region:head", 0.8)]),
    Case(MAI, "split the head UVs into their own texture map", "texset", [TILE("region:head", 0.8)]),
    # ---- texture sets
    Case(FIG, "can the head have its own texture?", "texset", [TILE("region:head")]),
    Case(LH, "I want her head on a separate texture from the body", "texset", [TILE("region:head")]),
    Case(MAI, "make me a second texture set just for the head", "texset", [TILE("region:head")]),
    Case(SH, "give the emblem its own texture set", "texset", [ONEOF([MODE("material_slots")], [TILE("slot:Emblem_Paint", 0.95)])]),
    Case(GA, "I'd like the hair on its own texture", "texset", CANT),  # it has one already (a set per slot)
    Case(FIG, "split the textures: head on one, body on another", "texset", [TILE("region:head")]),
    Case(SH, "one texture per material please", "layout", [MODE("material_slots")]),
    Case(KN, "put everything back onto a single texture", "layout", [MODE("single")]),
    # ---- texture space
    Case(FIG, "the head should take up more of the texture", "density", [GAIN("region:head", lo=1.3)]),
    Case(LH, "her face looks low-res, give it more texels", "density", [ONEOF([GAIN("region:head", lo=1.3)], [TILE("region:head", 0.8)])]),
    Case(FIG, "the body is hogging the texture, give it less space", "density", [GAIN("region:body", hi=0.85)]),
    Case(MAI, "triple the texel density of the head", "density", [GAIN("region:head", lo=1.8)]),
    # ---- resolution
    Case(CH, "bump the textures up to 2k", "resolution", [STATE("resolution", eq=2048)]),
    Case(GA, "I want 1024 textures", "resolution", [STATE("resolution", eq=1024)]),
    Case(SH, "make the textures 4K", "resolution", [STATE("resolution", eq=4096)]),
    # ---- glow
    Case(KN, "make the armour glow a soft blue", "glow", [L(region="slot:Armor_Plate", emissive={"rgb": [0.2, 0.4, 0.9], "tol": 0.5}, **GLOW), ONE()]),
    Case(FU, "her suit should glow pink", "glow", [L(region="slot:suit_tex", emissive={"rgb": [0.95, 0.55, 0.65], "tol": 0.4}, **GLOW), ONE()]),
    Case(SH, "light up the emblem in yellow", "glow", [L(region="slot:Emblem_Paint", emissive={"rgb": [0.95, 0.85, 0.15], "tol": 0.4}, **GLOW), ONE()]),
    Case(GA, "make the jacket glow red in the dark", "glow", [L(region="slot:M_Jacket", emissive={"rgb": [0.8, 0.12, 0.1], "tol": 0.4}, **GLOW), ONE()]),
    # ---- metallic
    Case(SH, "the rim needs to look more metallic", "metal", [ANY({"adjust": {"metallic": [0.04, 1.0]}, "region": "slot:Rim_Iron"}), ONE()]),
    Case(KN, "the chainmail is too metallic", "metal", [ANY({"adjust": {"metallic": [-1.0, -0.04]}, "region": "slot:Chainmail"}), ONE()]),
    # ---- selection
    Case(KN, "select the chainmail", "select", [SEL("slot:Chainmail")]),
    Case(GA, "select the jacket and the pants", "select", [SEL(["slot:M_Jacket", "slot:M_Pants"])]),
    Case(FIG, "clear my selection", "select", [{"t": "selection", "faces": [], "min_jaccard": 1.0}], pre=[["select_part", {"part": "region:head"}]]),
    Case(MAI, "invert the selection", "select", [SEL(("except", "region:head"))], pre=[["select_part", {"part": "region:head"}]]),
    # ---- windows
    Case(GA, "get the textures ready for unreal", "window", [WIN("export")]),
    Case(LH, "I'd like a render of her", "window", [WIN("render")]),
    Case(MAI, "send this back to blender", "window", [WIN("blender")]),
    Case(BU, "is this mesh symmetrical? check it", "window", [ONEOF([WIN("symmetry")], [STATE("view_mode", eq="symmetry")])]),
    Case(KN, "I want to bake my high poly sculpt", "window", [WIN("high_poly")]),
    Case(CH, "where are the keyboard shortcuts", "window", [WIN("shortcuts")]),
    # ---- text to paint through
    Case(SH, "write VALOR on the emblem", "text", [L(type="paint", region="slot:Emblem_Paint"), STATE("stencil.has_image", eq=True)]),
    Case(GA, "put the number 23 on the jacket in white", "text", [L(type="paint", region="slot:M_Jacket"), STATE("stencil.has_image", eq=True)]),
    Case(KN, "letter KNIGHT onto the armour plates", "text", [L(type="paint", region="slot:Armor_Plate"), STATE("stencil.has_image", eq=True)]),
    # ---- questions
    Case(GA, "how many polys is this?", "question", SAY + [SAYS("540")]),
    Case(MAI, "what's the face count", "question", SAY + [SAYS("15785", "15,785")]),
    Case(KN, "which parts can you see on this guy?", "question", SAY + [SAYS("armor plate", "chainmail", "armour")]),
    Case(BU, "what layers have I got?", "question", SAY + [SAYS("Grime", "Dust")]),
    Case(SH, "how big are the textures?", "question", SAY + [SAYS("512")]),
    Case(FIG, "did you find the head?", "question", SAY + [SAYS("yes", "neck", "head")]),
    # ---- can't, with something close it can do
    Case(GA, "give him a cape", "cant", CANT),
    Case(KN, "make the armour plates thicker", "cant", CANT),
    Case(FU, "make her blink", "cant", CANT),
    Case(LH, "paint a dragon on her back for me", "cant", CANT),
    Case(BU, "turn this photo into a texture", "cant", SAY),
    Case(CH, "add a second uv channel for lightmaps", "cant", CANT),
    Case(MAI, "make her hair flow in the wind", "cant", CANT),
    Case(GA, "upload it to sketchfab", "cant", SAY),
    Case(KN, "tell me a joke", "cant", SAY),
    Case(SH, "make the shield bigger", "cant", CANT),
    Case(FIG, "rig this character for animation", "cant", CANT),
    Case(MAI, "sculpt some abs on her", "cant", CANT),
    Case(GA, "make a seamless tiling texture out of this", "cant", SAY),
    Case(LH, "export the model as an fbx", "cant", SAY),
    Case(SH, "bake the textures into vertex colours", "cant", SAY),
    Case(MAI, "make a turntable video", "cant", SAY),
    Case(KN, "save the project", "cant", SAY),
    # ---- things it does that look like the ones it doesn't
    Case(KN, "I want to paint a tattoo on the skin myself", "paint_setup", [L(type="paint", region="slot:Skin"), STATE("tool", eq="paint"), ONE()]),
    Case(GA, "make the jacket look like worn leather", "material", [ANY({"material": ["patina_leather_brown", "patina_leather_black", "patina_quilted_leather", "patina_painted_leather"],
                                                                      "region": "slot:M_Jacket"}), ONE()]),
    Case(SH, "the rim could use some rust on the edges", "recipe", [ANY({"material": ["patina_rust_light", "patina_rust_heavy"], "region": "slot:Rim_Iron"}), ONE()]),
    # ---- vague, with ideas
    Case(FU, "make her look cooler", "vague", ASK),
    Case(SH, "spice it up a bit", "vague", ASK),
    Case(MAI, "what can you do for this model?", "help", SAY + [SUGG]),
]


def main():
    rows = build(CASES)
    path = os.path.join(common.DATA, "hard3_test.jsonl")
    common.jsonl_write(path, rows)
    print("%d hand-written requests -> %s" % (len(rows), path))


if __name__ == "__main__":
    main()
