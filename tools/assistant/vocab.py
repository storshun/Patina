"""The fixed vocabulary of the Assistant's training data: one mapping from vague words to numbers,
named colours to RGB, mask words to smart masks, and plain words to library materials. Every
generator uses these tables, so the model learns a single meaning for each word."""
import random

# ---------------------------------------------------------------------------- amounts
# Amount classes: (low, high) and the words that mean them. "slightly" is always 0.15-0.3,
# "a bit" 0.3-0.45, and so on (the handoff's table).
AMOUNTS = {
    "slight": ((0.15, 0.3), ["slightly", "a touch", "subtly", "faintly", "lightly", "just a little", "barely"]),
    "bit": ((0.3, 0.45), ["a bit", "somewhat", "a little", "moderately", "kind of"]),
    "noticeable": ((0.45, 0.65), ["noticeably", "clearly", "visibly", "quite", "fairly"]),
    "strong": ((0.65, 0.9), ["strongly", "heavily", "a lot", "very", "intensely", "really"]),
}
AMOUNT_WORDS = {w: k for k, (_, ws) in AMOUNTS.items() for w in ws}


def amount(rng, cls=None):
    """(class, word, value) with the value drawn inside the class's range."""
    cls = cls or rng.choice(list(AMOUNTS))
    (lo, hi), words = AMOUNTS[cls]
    return cls, rng.choice(words), round(rng.uniform(lo, hi), 2)


def amount_range(cls):
    return AMOUNTS[cls][0]


def amount_classes_in(text):
    """The amount classes a sentence uses (for checking that a paraphrase kept the amount)."""
    t = " " + text.lower() + " "
    found = set()
    for w, k in sorted(AMOUNT_WORDS.items(), key=lambda x: -len(x[0])):
        if (" " + w + " ") in t or (" " + w + ",") in t or (" " + w + ".") in t:
            found.add(k)
            t = t.replace(" " + w, " _")
    return found


# ---------------------------------------------------------------------------- colours
# sRGB 0..1. Character-art words (blush, bruise, sunburn) are colours too.
COLOURS = {
    "red": (0.80, 0.12, 0.10), "dark red": (0.45, 0.05, 0.05), "crimson": (0.70, 0.06, 0.18),
    "maroon": (0.40, 0.08, 0.12), "pink": (0.95, 0.55, 0.65), "salmon": (0.95, 0.55, 0.45),
    "orange": (0.95, 0.50, 0.10), "amber": (0.95, 0.65, 0.15), "yellow": (0.95, 0.85, 0.15),
    "gold": (0.85, 0.65, 0.20), "olive": (0.45, 0.45, 0.15), "lime": (0.60, 0.85, 0.20),
    "green": (0.15, 0.60, 0.20), "dark green": (0.08, 0.30, 0.10), "teal": (0.10, 0.50, 0.50),
    "cyan": (0.10, 0.80, 0.85), "turquoise": (0.20, 0.80, 0.75), "light blue": (0.55, 0.75, 0.95),
    "sky blue": (0.45, 0.70, 0.95), "blue": (0.15, 0.30, 0.85), "navy": (0.06, 0.10, 0.35),
    "indigo": (0.25, 0.12, 0.55), "purple": (0.50, 0.20, 0.65), "violet": (0.55, 0.30, 0.85),
    "magenta": (0.85, 0.15, 0.70), "lavender": (0.75, 0.65, 0.90), "brown": (0.45, 0.28, 0.15),
    "dark brown": (0.25, 0.15, 0.08), "tan": (0.80, 0.65, 0.45), "beige": (0.90, 0.85, 0.70),
    "cream": (0.95, 0.92, 0.80), "white": (0.95, 0.95, 0.95), "light grey": (0.75, 0.75, 0.75),
    "grey": (0.50, 0.50, 0.50), "dark grey": (0.25, 0.25, 0.25), "charcoal": (0.15, 0.15, 0.16),
    "black": (0.04, 0.04, 0.04), "blush": (0.85, 0.45, 0.45), "rosy": (0.88, 0.50, 0.52),
    "bruise": (0.35, 0.20, 0.40), "sunburn": (0.85, 0.35, 0.25), "pale": (0.92, 0.88, 0.85),
    "ochre": (0.80, 0.60, 0.20), "rust": (0.60, 0.25, 0.10), "olive drab": (0.35, 0.36, 0.20),
}
# colour families, for checking a paraphrase kept the colour
COLOUR_FAMILY = {
    "red": "red", "dark red": "red", "crimson": "red", "maroon": "red", "pink": "pink", "salmon": "pink",
    "orange": "orange", "amber": "orange", "yellow": "yellow", "gold": "yellow", "olive": "green", "lime": "green",
    "green": "green", "dark green": "green", "teal": "cyan", "cyan": "cyan", "turquoise": "cyan",
    "light blue": "blue", "sky blue": "blue", "blue": "blue", "navy": "blue", "indigo": "purple",
    "purple": "purple", "violet": "purple", "magenta": "purple", "lavender": "purple", "brown": "brown",
    "dark brown": "brown", "tan": "brown", "beige": "white", "cream": "white", "white": "white",
    "light grey": "grey", "grey": "grey", "dark grey": "grey", "charcoal": "black", "black": "black",
    "blush": "pink", "rosy": "pink", "bruise": "purple", "sunburn": "red", "pale": "white", "ochre": "yellow",
    "rust": "orange", "olive drab": "green",
}
FAMILY_WORDS = {
    "red": ["red", "crimson", "maroon", "scarlet", "ruby", "burgundy", "sunburn", "redder"],
    "pink": ["pink", "salmon", "blush", "rosy", "rose", "flush", "pinker"],
    "orange": ["orange", "amber", "rust", "copper", "tangerine"],
    "yellow": ["yellow", "gold", "golden", "ochre", "mustard"],
    "green": ["green", "olive", "lime", "emerald", "khaki", "greener"],
    "cyan": ["cyan", "teal", "turquoise", "aqua"],
    "blue": ["blue", "navy", "azure", "cobalt", "bluer"],
    "purple": ["purple", "violet", "magenta", "lavender", "indigo", "bruise", "plum", "lilac"],
    "brown": ["brown", "tan", "chocolate", "umber", "sepia", "browner"],
    "white": ["white", "cream", "beige", "ivory", "pale", "off-white", "lighter"],
    "grey": ["grey", "gray", "silver", "slate", "ash"],
    "black": ["black", "charcoal", "dark", "ebony", "darker"],
}


def colour(rng, names=None):
    name = rng.choice(names or list(COLOURS))
    return name, [round(c, 2) for c in COLOURS[name]]


def colour_families_in(text):
    t = text.lower()
    return {fam for fam, ws in FAMILY_WORDS.items() if any(w in t for w in ws)}


# ---------------------------------------------------------------------------- smart masks
MASK_PHRASES = {
    "cavity": ["in the creases", "in the crevices", "in the cavities", "in the wrinkles", "in the folds",
               "in the recesses", "in the grooves", "in the gaps", "in the pores"],
    "edges": ["on the edges", "along the edges", "on the corners", "on the ridges", "on the raised edges",
              "on the high points", "where it would wear"],
    "occlusion": ["in the sheltered places", "in the shadowed areas", "where light doesn't reach",
                  "in the occluded areas", "under the overhangs"],
    "facing_up": ["where dust settles", "on the top surfaces", "on the upward faces", "on top",
                  "where snow would sit"],
    "facing_down": ["on the underside", "on the downward faces", "underneath"],
    "gradient_down": ["towards the bottom", "near the ground", "rising from the bottom"],
    "gradient_up": ["towards the top", "fading in towards the top"],
    "noise": ["in patches", "patchy", "blotchy", "unevenly", "broken up"],
    "thin": ["on the thin parts", "where it's thin"],
}
MASK_DEFAULTS = {
    "cavity": {"level": 0.55, "contrast": 0.6}, "edges": {"level": 0.5, "contrast": 0.6},
    "occlusion": {"level": 0.5, "contrast": 0.5}, "facing_up": {"level": 0.5, "contrast": 0.5},
    "facing_down": {"level": 0.5, "contrast": 0.5}, "gradient_down": {"level": 0.5, "contrast": 0.3},
    "gradient_up": {"level": 0.5, "contrast": 0.3}, "noise": {"level": 0.5, "contrast": 0.5},
    "thin": {"level": 0.5, "contrast": 0.6},
}


def mask_spec(kind, rng=None, jitter=True):
    d = dict(MASK_DEFAULTS[kind])
    spec = {"type": kind}
    if jitter and rng is not None:
        spec["level"] = round(min(0.9, max(0.1, d["level"] + rng.uniform(-0.1, 0.1))), 2)
        spec["contrast"] = round(min(0.9, max(0.1, d["contrast"] + rng.uniform(-0.1, 0.1))), 2)
    else:
        spec.update(d)
    return spec


# ---------------------------------------------------------------------------- materials
# plain words -> library material ids (checked against the library when the data is made)
MATERIAL_WORDS = {
    "rust": ["patina_rust_heavy", "patina_rust_light"], "heavy rust": ["patina_rust_heavy"],
    "light rust": ["patina_rust_light"], "brown leather": ["patina_leather_brown"],
    "black leather": ["patina_leather_black"], "leather": ["patina_leather_brown", "patina_leather_black"],
    "denim": ["patina_fabric_denim", "patina_denim_twill"], "black denim": ["patina_denim_black"],
    "gold": ["patina_gold_polished"], "chrome": ["patina_chrome"], "copper": ["patina_copper"],
    "brass": ["patina_brass_aged"], "bronze": ["patina_bronze_verdigris"], "verdigris": ["patina_verdigris_crust"],
    "silver": ["patina_silver_tarnished"], "brushed steel": ["patina_steel_brushed"],
    "scratched steel": ["patina_steel_scratched"], "steel": ["patina_steel_brushed", "patina_steel_scratched"],
    "iron": ["patina_iron_raw", "patina_cast_iron"], "cast iron": ["patina_cast_iron"],
    "gunmetal": ["patina_gunmetal"], "aluminium": ["patina_aluminum_brushed"], "titanium": ["patina_titanium_anodized"],
    "oak": ["patina_wood_oak"], "walnut": ["patina_wood_walnut"], "pine": ["patina_wood_pine"],
    "wood": ["patina_wood_oak", "patina_wood_walnut", "patina_wood_pine"], "cherry wood": ["patina_wood_cherry"],
    "concrete": ["patina_concrete"], "plaster": ["patina_plaster"], "granite": ["patina_stone_granite"],
    "marble": ["patina_marble_white", "patina_marble_black"], "brick": ["patina_bricks_red", "patina_bricks_old"],
    "sandstone": ["patina_sandstone"], "slate": ["patina_slate"], "mud": ["patina_dirt_mud"],
    "dirt": ["patina_dirt_mud"], "dust": ["patina_dust"], "sand": ["patina_sand_desert"], "moss": ["patina_moss", "patina_moss_patchy"],
    "snow": ["patina_snow"], "soot": ["patina_soot"], "grime": ["patina_grime_oily"], "oily grime": ["patina_grime_oily"],
    "linen": ["patina_fabric_linen"], "canvas": ["patina_canvas"], "wool": ["patina_wool_grey"],
    "velvet": ["patina_velvet_red"], "satin": ["patina_satin_black", "patina_satin_red", "patina_satin_ivory"],
    "knitted wool": ["patina_knit_wool_cream"], "rubber": ["patina_rubber"], "carbon fibre": ["patina_carbon_fiber"],
    "glossy plastic": ["patina_plastic_glossy_red", "patina_plastic_glossy_blue"], "black plastic": ["patina_plastic_rough_black"],
    "white plastic": ["patina_plastic_white"], "chipped red paint": ["patina_paint_chipped_red"],
    "chipped paint": ["patina_paint_chipped_red", "patina_paint_chipped_yellow", "patina_paint_chipped_green"],
    "peeling paint": ["patina_paint_peeling_white"], "car paint": ["patina_car_paint_red", "patina_car_paint_blue"],
    "camo": ["patina_camo_woodland", "patina_camo_desert", "patina_camo_urban"], "dragon scales": ["patina_dragon_scales"],
    "scales": ["patina_scales", "patina_snake_scales"], "fair skin": ["patina_skin_fair"], "light skin": ["patina_skin_light"],
    "medium skin": ["patina_skin_medium"], "brown skin": ["patina_skin_brown"], "dark skin": ["patina_skin_deep"],
    "anime skin": ["patina_skin_anime"], "skin": ["patina_skin_light", "patina_skin_medium", "patina_skin_fair"],
    "black hair": ["patina_hair_black"], "brown hair": ["patina_hair_brown"], "blonde hair": ["patina_hair_blonde"],
    "ginger hair": ["patina_hair_ginger"], "plaid": ["patina_plaid_red", "patina_school_skirt_plaid"],
    "sci-fi panels": ["patina_panels_hull", "patina_panels_white", "patina_panels_dark"],
    "glowing cyan": ["patina_glow_cyan"], "glow": ["patina_glow_cyan", "patina_glow_magenta"],
}

SMART_WORDS = {
    "aged brass": "patina_smart_aged_brass", "dirty fabric": "patina_smart_dirty_fabric",
    "dusty plastic": "patina_smart_dusty_plastic", "edge wear": "patina_smart_edge_wear_overlay",
    "grimy plastic": "patina_smart_grimy_plastic", "heat-tinted metal": "patina_smart_heat_tinted_metal",
    "living skin": "patina_smart_living_skin", "mossy stone": "patina_smart_mossy_stone",
    "mossy wood": "patina_smart_mossy_wood", "oxidised copper": "patina_smart_oxidised_copper",
    "rusted iron": "patina_smart_rusted_iron", "worn sci-fi panels": "patina_smart_scifi_panel_worn",
    "scratched paint": "patina_smart_scratched_paint", "snow cover": "patina_smart_snow_cover",
    "stylised edges": "patina_smart_stylised_edges", "weathered wood": "patina_smart_weathered_wood",
    "worn gold trim": "patina_smart_worn_gold_trim", "worn leather": "patina_smart_worn_leather",
    "worn painted metal": "patina_smart_worn_painted_metal",
}

# ---------------------------------------------------------------------------- misc
VIEW_MODES = {
    "material": ["the full material", "the normal material view", "the shaded view"],
    "basecolor": ["the base colour", "just the albedo", "the colour channel"],
    "roughness": ["the roughness map", "roughness"], "metallic": ["the metallic channel", "metalness"],
    "normal": ["the normal map", "normals"], "height": ["the height map", "height"],
    "ao": ["the ambient occlusion", "the AO bake"], "curvature": ["the curvature map", "curvature"],
    "uv_checker": ["a UV checker", "the checker pattern"], "uv_islands": ["the UV islands in colour", "the islands"],
    "texel_density": ["the texel density", "texel density heat map"], "uv_stretch": ["the UV stretch", "stretching"],
    "mask": ["the layer mask", "the mask of the selected layer"],
}
CAMERA_VIEWS = {
    "front": ["the front", "the front view"], "back": ["the back", "the back view", "behind it"],
    "left": ["the left side", "the left view"], "right": ["the right side", "the right view"],
    "top": ["the top", "from above", "the top view"], "bottom": ["underneath", "the bottom view"],
}
BLEND_WORDS = {
    "multiply": ["multiply"], "screen": ["screen"], "overlay": ["overlay"], "soft_light": ["soft light"],
    "add": ["add", "additive"], "normal": ["normal"], "darken": ["darken"], "lighten": ["lighten"],
}
TOOLS = {"paint": ["the brush", "painting"], "erase": ["the eraser", "erasing"], "fill": ["the fill tool", "fill"],
         "blur": ["the blur tool", "blur"], "smudge": ["smudge", "the smudge tool"],
         "clone": ["the clone tool", "clone stamp"], "picker": ["the colour picker", "the eyedropper"]}
BRUSH_WORDS = {
    "basic_soft": ["a soft brush", "a soft round brush"], "basic_hard": ["a hard brush", "a hard round brush"],
    "airbrush": ["the airbrush", "an airbrush"], "pencil": ["the pencil", "a pencil brush"],
    "chalk": ["the chalk brush", "chalk"], "dry_brush": ["a dry brush"], "spray": ["the spray brush", "a spray can"],
    "splatter": ["the splatter brush"], "grunge": ["a grunge brush"], "scratches": ["the scratches brush"],
    "cracks": ["the cracks brush"], "rust_spots": ["the rust spots brush"], "edge_wear": ["the edge wear brush"],
    "dust_cloud": ["the dust brush"], "marker": ["a marker brush"], "ink": ["the ink brush"],
}


def pick(rng, xs):
    return rng.choice(list(xs))


def new_rng(seed):
    return random.Random(seed)
