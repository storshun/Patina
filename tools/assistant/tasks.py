"""Task generators for the Assistant's training data. Each takes a scene (its parts, layers and
the faces of every part) and a random generator, and returns an example:

  {"task", "family", "variant", "request" (the canonical sentence), "plan" ({say, steps} or
   {ask} / {say}), "check" (specs for checks.py), "setup" (earlier requests, for follow-ups)}

or None when the scene doesn't suit the task. Parts are named the way people name them ("the
shirt", "the left arm", "the head"); the plan uses the ids from the project description.
"""
import re

import vocab as V

# ----------------------------------------------------------------------------- part names
_MATERIAL_ADJ = {"leather", "metal", "plate", "wood", "glass", "cloth", "paint", "steel", "iron", "brass", "felt",
                 "panels", "rubber", "gold", "silver"}


def part_name(pid):
    """How a person would name a part in a request ("the leather boots", "the left arm")."""
    if pid == "selection":
        return "the selected faces"
    kind, _, name = pid.partition(":")
    if kind == "region":
        return "the " + name
    if kind == "piece":
        return "piece " + name
    side = ""
    m = re.match(r"^(.*?)[._](L|R|Left|Right)$", name)
    if m:
        name, side = m.group(1), "left " if m.group(2) in ("L", "Left") else "right "
    raw = re.sub(r"^(M|MI|MAT|mat|Mat)_", "", name)
    raw = re.sub(r"([a-z])([A-Z])", r"\1 \2", raw)
    words = [w.lower() for w in re.split(r"[_.\s-]+", raw) if w and not w.isdigit()]
    words = [w for w in words if not re.fullmatch(r"\d+", w)]
    if not words or any(re.fullmatch(r"(material|lambert|blinn|phong)\d*", w) for w in words):
        return "the %s slot" % name  # nothing to call it by but its name
    if len(words) == 2 and words[1] in _MATERIAL_ADJ and words[0] not in _MATERIAL_ADJ:
        words = [words[1], words[0]]
    return "the " + side + " ".join(words)


def usable_parts(sc, kinds=("slot", "group", "region")):
    return [p for p in sc["parts"] if p["id"].split(":")[0] in kinds and p["faces"] > 0]


def faces_of(sc, pid):
    return sc["part_faces"][pid]


def region_faces(sc, parts, except_=()):
    keep = set()
    for p in parts:
        keep |= set(faces_of(sc, p))
    for p in except_:
        keep -= set(faces_of(sc, p))
    return sorted(keep)


def region_arg(parts, except_=()):
    r = {"part": parts[0]} if len(parts) == 1 and not except_ else {"parts": list(parts)}
    if except_:
        r["except"] = list(except_)
    return r


def tidy(text):
    """'a ochre' -> 'an ochre', 'Put grime to' -> 'Put grime on'."""
    text = re.sub(r"\b([Aa]) ([aeio])", r"\1n \2", text)
    text = re.sub(r"^(Put [a-z ]+?) to ", r"\1 on ", text)
    return text


def ex(task, family, variant, request, plan, check, setup=None):
    request = tidy(request)
    if "say" in plan:
        plan["say"] = tidy(plan["say"])
    return {"task": task, "family": family, "variant": variant, "request": request, "plan": plan, "check": check,
            "setup": setup or []}


def step(cmd, **args):
    return {"cmd": cmd, "args": args}


def cap(s):
    return s[:1].upper() + s[1:]


def new_layer_check(expect, history=1):
    return [{"t": "layer", "which": "new", "expect": expect}, {"t": "history", "delta": history}]


def amount_opacity(cls):
    lo, hi = V.amount_range(cls)
    return [max(0.0, lo - 0.05), min(1.0, hi + 0.05)]


# ----------------------------------------------------------------------------- tints
NOUN_COLOURS = {"blush", "bruise", "sunburn", "rust"}


def more(cname):
    """'more red', or for colours that are things, 'more of a bruise colour'."""
    return "more of a %s colour" % cname if cname in NOUN_COLOURS else "more " + cname


def bare(phrase):
    """'in the creases' -> 'the creases' (for 'the creases of the skin')."""
    for p in ("in the ", "on the ", "along the ", "under the "):
        if phrase.startswith(p):
            return "the " + phrase[len(p):]
    return phrase


def tint_masked(sc, rng):
    parts = usable_parts(sc)
    if not parts:
        return None
    part = rng.choice(parts)["id"]
    mask = rng.choice(["cavity", "cavity", "edges", "occlusion", "facing_up", "gradient_down", "noise"])
    cname, rgb = V.colour(rng, ["red", "dark red", "pink", "blush", "rosy", "sunburn", "bruise", "purple", "blue",
                                "brown", "dark brown", "orange", "yellow", "green", "black", "grey", "white"])
    cls, aword, value = V.amount(rng)
    phrase = rng.choice(V.MASK_PHRASES[mask])
    blend = rng.choice(["normal", "normal", "soft_light", "overlay", "multiply"]) if cname not in ("white",) else "normal"
    name = "%s %s" % (cap(part_name(part)[4:] if part_name(part).startswith("the ") else part_name(part)), cname)
    pn = part_name(part)
    req = rng.choice([
        "Make %s %s %s %s than the rest" % (pn, aword, more(cname), phrase),
        "Add a %s tint to %s %s, %s" % (cname, pn, phrase, aword),
        "Tint %s %s %s %s" % (pn, aword, cname, phrase),
    ] + (["Set up a smart mask that makes %s of %s %s %s" % (bare(phrase), pn, aword, more(cname))]
         if bare(phrase) != phrase else []))
    args = {"name": name.strip(), "color": rgb, "strength": value, "blend": blend, "region": {"part": part},
            "mask": [V.mask_spec(mask, rng)]}
    say = "Adds a %s tint %s of %s." % (cname, phrase, part_name(part))
    check = new_layer_check({"type": "fill", "channel_mask": 1, "color": {"rgb": rgb, "tol": 0.3},
                             "opacity": amount_opacity(cls), "masks": [mask], "region": faces_of(sc, part)})
    return ex("tint_masked", "tint", "%s/%s" % (part.split(":")[0], mask), req, {"say": say, "steps": [step("add_tint_layer", **args)]}, check)


def tint_plain(sc, rng):
    parts = usable_parts(sc)
    if not parts:
        return None
    part = rng.choice(parts)["id"]
    cname, rgb = V.colour(rng)
    cls, aword, value = V.amount(rng)
    req = rng.choice(["Tint %s %s %s" % (part_name(part), aword, cname),
                      "Give %s a %s tint, %s" % (part_name(part), cname, aword),
                      "Make %s %s %s" % (part_name(part), aword, more(cname))])
    args = {"name": "%s tint" % cap(cname), "color": rgb, "strength": value, "region": {"part": part}}
    check = new_layer_check({"type": "fill", "channel_mask": 1, "color": {"rgb": rgb, "tol": 0.3},
                             "opacity": amount_opacity(cls), "masks": [], "region": faces_of(sc, part)})
    return ex("tint_plain", "tint", part.split(":")[0], req,
              {"say": "Adds a %s tint to %s." % (cname, part_name(part)), "steps": [step("add_tint_layer", **args)]}, check)


def tint_whole(sc, rng):
    mask = rng.choice(["cavity", "occlusion", "edges", "gradient_down", "facing_up"])
    if mask in ("cavity", "occlusion", "gradient_down"):
        cname, rgb = V.colour(rng, ["black", "dark brown", "charcoal", "dark grey"])
        verb = rng.choice(["Darken", "Dirty up", "Add a dark tint"])
    else:
        cname, rgb = V.colour(rng, ["white", "cream", "light grey", "beige"])
        verb = rng.choice(["Lighten", "Brighten", "Add a pale tint"])
    cls, aword, value = V.amount(rng)
    phrase = rng.choice(V.MASK_PHRASES[mask])
    obj = "" if verb.startswith("Add") else rng.choice([" everything", " the model", ""])
    req = "%s%s %s, %s" % (verb, obj, phrase, aword)
    args = {"name": "%s %s" % (cap(cname), mask.replace("_", " ")), "color": rgb, "strength": value,
            "blend": "multiply" if verb in ("Darken", "Dirty up") else "normal", "mask": [V.mask_spec(mask, rng)]}
    check = new_layer_check({"type": "fill", "channel_mask": 1, "color": {"rgb": rgb, "tol": 0.35},
                             "opacity": amount_opacity(cls), "masks": [mask], "region": "all"})
    return ex("tint_whole", "tint", mask, req,
              {"say": "Adds a %s tint %s." % (cname, phrase), "steps": [step("add_tint_layer", **args)]}, check)


def tint_explicit(sc, rng):
    """Numbers given: '40%', 'overlay', a hex colour."""
    parts = usable_parts(sc) or [{"id": None}]
    part = rng.choice(parts)["id"]
    cname, rgb = V.colour(rng)
    pct = rng.choice([10, 15, 20, 25, 30, 35, 40, 50, 60, 75, 80, 100])
    blend = rng.choice(["overlay", "soft_light", "multiply", "screen", "normal"])
    where = (" on " + part_name(part)) if part else ""
    req = "Add a %s tint layer%s at %d%% using %s" % (cname, where, pct, V.BLEND_WORDS[blend][0])
    args = {"name": "%s tint" % cap(cname), "color": rgb, "strength": pct / 100, "blend": blend}
    if part:
        args["region"] = {"part": part}
    check = new_layer_check({"type": "fill", "color": {"rgb": rgb, "tol": 0.3}, "opacity": [pct / 100 - 0.02, pct / 100 + 0.02],
                             "blend": [blend], "region": faces_of(sc, part) if part else "all"})
    return ex("tint_explicit", "tint", blend, req,
              {"say": "Adds a %s %s tint at %d%%%s." % (cname, blend.replace("_", " "), pct, where), "steps": [step("add_tint_layer", **args)]}, check)


# ----------------------------------------------------------------------------- materials
def _material(rng, words=None):
    word = rng.choice(words or list(V.MATERIAL_WORDS))
    return word, V.MATERIAL_WORDS[word]


def material_region(sc, rng):
    parts = usable_parts(sc)
    if not parts:
        return None
    part = rng.choice(parts)["id"]
    word, ids = _material(rng)
    mid = rng.choice(ids)
    req = rng.choice(["Make %s %s" % (part_name(part), word), "Put %s on %s" % (word, part_name(part)),
                      "Turn %s into %s" % (part_name(part), word), "Give %s a %s material" % (part_name(part), word)])
    check = new_layer_check({"type": "fill", "material": ids, "masks": [], "region": faces_of(sc, part)})
    return ex("material_region", "material", part.split(":")[0], req,
              {"say": "Fills %s with %s." % (part_name(part), word),
               "steps": [step("add_material_layer", material=mid, name=cap(word), region={"part": part})]}, check)


def material_whole(sc, rng):
    word, ids = _material(rng)
    mid = rng.choice(ids)
    req = rng.choice(["Make the whole thing %s" % word, "Cover everything in %s" % word, "Fill it all with %s" % word,
                      "Make it all %s" % word])
    check = new_layer_check({"type": "fill", "material": ids, "masks": [], "region": "all"})
    return ex("material_whole", "material", "all", req,
              {"say": "Fills the whole model with %s." % word, "steps": [step("add_material_layer", material=mid, name=cap(word))]}, check)


RECIPES = {
    # name: (material words, mask kinds, how it's asked; {w} is the material word)
    "rust": (["rust", "heavy rust", "light rust"], [["cavity"], ["edges", "noise"], ["occlusion"], ["cavity", "noise"]],
             ["Add {w} %s", "Put some {w} %s", "Rust it up %s"]),
    "dirt": (["dirt", "mud", "grime"], [["cavity"], ["occlusion"], ["gradient_down"], ["cavity", "noise"]],
             ["Add {w} %s", "Put {w} %s", "Dirty it up with {w} %s"]),
    "dust": (["dust", "sand"], [["facing_up"], ["facing_up", "noise"]], ["Add {w} %s", "Sprinkle {w} %s"]),
    "snow": (["snow"], [["facing_up"], ["facing_up", "noise"]], ["Add snow %s", "Put snow %s"]),
    "moss": (["moss"], [["facing_up"], ["cavity"], ["occlusion", "noise"]], ["Grow moss %s", "Add moss %s"]),
    "soot": (["soot"], [["gradient_up"], ["cavity"], ["occlusion"]], ["Add soot %s", "Blacken it with soot %s"]),
    "wear": (["brushed steel", "scratched steel", "iron", "gold", "brass", "copper"], [["edges"], ["edges", "noise"]],
             ["Wear the paint off %s to show {w}", "Add worn {w} on the edges%s"]),
}


def recipe(sc, rng):
    kind = rng.choice(list(RECIPES))
    words, masks, forms = RECIPES[kind]
    word = rng.choice(words)
    ids = V.MATERIAL_WORDS[word]
    mid = rng.choice(ids)
    mk = rng.choice(masks)
    parts = usable_parts(sc)
    part = rng.choice(parts)["id"] if parts and rng.random() < 0.6 else None
    phrase = " and ".join(rng.choice(V.MASK_PHRASES[m]) for m in mk if m != "noise")
    if "noise" in mk:
        phrase += ", " + rng.choice(V.MASK_PHRASES["noise"])
    form = rng.choice(forms).replace("{w}", word)
    where = " of " + part_name(part) if part else ""
    if kind == "wear":
        req = form % ((part_name(part) if part else "the edges") if "to show" in form else where)
    else:
        req = form % (("to " + part_name(part) + " " if part else "") + phrase)
    cls, aword, value = V.amount(rng)
    use_amount = rng.random() < 0.5
    if use_amount:
        req += ", " + aword
    args = {"material": mid, "name": cap(kind if kind != "wear" else "edge wear")}
    if use_amount:
        args["strength"] = value
    if part:
        args["region"] = {"part": part}
    args["mask"] = [V.mask_spec(m, rng) for m in mk]
    expect = {"type": "fill", "material": ids, "masks": mk, "region": faces_of(sc, part) if part else "all"}
    if "noise" not in mk:
        expect["masks_optional"] = ["noise"]  # wear and dirt broken up by noise is still what was asked
    if use_amount:
        expect["opacity"] = amount_opacity(cls)
    return ex("recipe_" + kind, "recipe", "+".join(mk) + ("/part" if part else ""), req,
              {"say": "Adds %s %s%s." % (word, phrase, where), "steps": [step("add_material_layer", **args)]},
              new_layer_check(expect))


def smart_material(sc, rng):
    word = rng.choice(list(V.SMART_WORDS))
    sid = V.SMART_WORDS[word]
    parts = usable_parts(sc)
    part = rng.choice(parts)["id"] if parts and rng.random() < 0.7 else None
    if part:
        req = rng.choice(["Make %s %s" % (part_name(part), word), "Apply the %s smart material to %s" % (word, part_name(part)),
                          "Use %s on %s" % (word, part_name(part))])
        args = {"id": sid, "region": {"part": part}}
    else:
        req = rng.choice(["Apply the %s smart material" % word, "Make it look like %s" % word, "Give it a %s look" % word])
        args = {"id": sid}
    check = [{"t": "new_layers", "min": 1}, {"t": "history", "delta": 1}]
    if part:
        check.append({"t": "layer", "which": "new", "expect": {"region": faces_of(sc, part)}})
    return ex("smart_material", "material", "part" if part else "all", req,
              {"say": "Applies the %s smart material%s." % (word, " to " + part_name(part) if part else ""),
               "steps": [step("apply_smart_material", **args)]}, check)


def auto_texture(sc, rng):
    slots = [p for p in sc["parts"] if p["id"].startswith("slot:")]
    if len(slots) < 2:
        return None
    req = rng.choice(["Texture each material slot from its name", "Guess materials for every slot",
                      "Auto texture it from the slot names", "Give every part a starting material based on its name"])
    return ex("auto_texture", "material", "slots", req,
              {"say": "Picks a library material for each material slot from its name.", "steps": [step("auto_texture")]},
              [{"t": "new_layers", "min": 1}, {"t": "history", "delta": 1}])


# ----------------------------------------------------------------------------- UVs
def uv_island_per_part(sc, rng):
    parts = usable_parts(sc)
    head = [p["id"] for p in parts if p["id"] in ("region:head", "group:Head")]
    body = [p["id"] for p in parts if p["id"] in ("region:body", "group:Torso")]
    if head and body and rng.random() < 0.6:
        chosen = [rng.choice(head), "region:body" if "region:body" in body else body[0]]
        if chosen[1] == "group:Torso":
            return None
        req = rng.choice(["Set up the UVs so I have one UV island for the head and another for the body",
                          "One UV island for the head and one for the body", "Give the head and the body an island each"])
    else:
        slots = [p["id"] for p in parts if p["id"].startswith("slot:")]
        if len(slots) < 2:
            return None
        chosen = rng.sample(slots, min(len(slots), rng.choice([2, 2, 3])))
        names = [part_name(p) for p in chosen]
        req = "Make one UV island for each of " + ", ".join(names[:-1]) + " and " + names[-1]
    check = [{"t": "uv_parts", "parts": [{"part": p} for p in chosen], "ref": True}, {"t": "history", "delta": 1}]
    return ex("uv_island_per_part", "uv", "head_body" if "region:body" in chosen else "slots", req,
              {"say": "Rebuilds the UVs with one island each for " + " and ".join(part_name(p) for p in chosen) + ".",
               "steps": [step("uv_island_per_part", parts=chosen)]}, check)


def uv_part_op(sc, rng):
    parts = usable_parts(sc)
    if not parts:
        return None
    part = rng.choice(parts)["id"]
    op = rng.choice(["unwrap", "relax", "island"])
    if op == "unwrap":
        req = rng.choice(["Unwrap %s again", "Re-unwrap the UVs of %s", "Redo the UVs on %s"]) % part_name(part)
        steps = [step("select_part", part=part), step("uv_unwrap", method="lscm", selection=True)]
        say = "Unwraps %s again." % part_name(part)
    elif op == "relax":
        req = rng.choice(["Relax the UVs on %s", "Smooth out the stretching on %s", "Relax %s's UVs"]) % part_name(part)
        steps = [step("select_part", part=part), step("uv_relax", selection=True)]
        say = "Relaxes the UVs of %s." % part_name(part)
    else:
        req = rng.choice(["Make %s its own UV island", "Give %s a single island", "Turn %s into one island"]) % part_name(part)
        steps = [step("select_part", part=part), step("uv_make_island", selection=True)]
        say = "Makes %s one UV island." % part_name(part)
    check = [{"t": "uv_changed"}, {"t": "selection", "faces": faces_of(sc, part)}, {"t": "history", "delta": 1}]
    if op == "island":
        check.append({"t": "uv_parts", "parts": [{"part": part}], "ref": True})
    return ex("uv_part_" + op, "uv", part.split(":")[0], req, {"say": say, "steps": steps}, check)


def uv_global(sc, rng):
    op = rng.choice(["pack", "density", "mirror", "overlaps", "smart"])
    if op == "pack":
        req = rng.choice(["Pack the UVs tighter", "Repack the UV islands", "Pack the islands to use more of the texture"])
        steps, say = [step("uv_pack")], "Packs the UV islands."
    elif op == "density":
        req = rng.choice(["Even out the texel density", "Make the texel density consistent", "Give every island the same texel density"])
        steps, say = [step("uv_density", set="auto")], "Scales every island to the same texel density."
    elif op == "mirror":
        req = rng.choice(["Mirror the UVs left to right", "Make both halves share the same UVs", "Stack the symmetrical UVs"])
        steps, say = [step("uv_mirror", axes=["x"])], "Mirrors the UVs so both halves share one place."
    elif op == "overlaps":
        req = rng.choice(["Fix the overlapping UVs", "Some islands overlap, sort that out", "Move overlapping islands apart"])
        steps, say = [step("uv_fix_overlaps")], "Moves overlapping islands into free space."
    else:
        req = rng.choice(["Smart unwrap the whole model", "Unwrap everything automatically", "Auto unwrap it"])
        steps, say = [step("uv_unwrap", method="smart")], "Unwraps the whole model with smart projection."
    return ex("uv_" + op, "uv", op, req, {"say": say, "steps": steps},
              [{"t": "uv_ref", "fields": ["islands", "coverage"], "ref": True}])


# ----------------------------------------------------------------------------- bakes
def bake(sc, rng):
    maps = rng.choice([["ao"], ["curvature"], ["ao", "curvature"], ["ao", "curvature", "thickness"], ["thickness"]])
    words = {"ao": "ambient occlusion", "curvature": "curvature", "thickness": "thickness"}
    hq = rng.random() < 0.3
    req = "Bake " + " and ".join(rng.choice([words[m], m.upper() if m == "ao" else words[m]]) for m in maps)
    if hq:
        req += rng.choice([" at high quality", " with more samples"])
    args = {"ao": "ao" in maps, "curvature": "curvature" in maps, "thickness": "thickness" in maps}
    if hq:
        args["ao_samples"] = 256
    return ex("bake", "bake", "+".join(maps), req,
              {"say": "Bakes " + " and ".join(words[m] for m in maps) + ".", "steps": [step("bake_mesh_maps", **args)]},
              [{"t": "baked", "maps": maps}])


# ----------------------------------------------------------------------------- layers
def _layers(sc, kinds=None):
    return [l for l in sc["layers"] if kinds is None or l["type"] in kinds]


def layer_op(sc, rng):
    ls = _layers(sc)
    if not ls:
        return None
    l = rng.choice(ls)
    op = rng.choice(["rename", "hide", "show", "opacity", "opacity_vague", "blend", "top", "bottom", "delete", "duplicate", "folder"])
    nm = "the %s layer" % l["name"] if rng.random() < 0.7 else 'the layer "%s"' % l["name"]
    i = l["index"]
    top = len(sc["layers"]) - 1
    if op == "rename":
        new = rng.choice(["Base", "Skin tone", "Dirt pass", "Wear", "Details", "Colour", "Grunge", "Highlights", "AO"])
        if new == l["name"]:
            return None
        return ex("layer_rename", "layers", "rename", "Rename %s to %s" % (nm, new),
                  {"say": 'Renames the layer to "%s".' % new, "steps": [step("update_layer", index=i, name=new)]},
                  [{"t": "layer", "which": {"uid": l["uid"]}, "expect": {"name_eq": new}}])
    if op in ("hide", "show"):
        req = ("Hide %s" if op == "hide" else "Show %s again") % nm
        return ex("layer_visibility", "layers", op, req,
                  {"say": ("Hides" if op == "hide" else "Shows") + " the layer.", "steps": [step("update_layer", index=i, visible=op == "show")]},
                  [{"t": "layer", "which": {"uid": l["uid"]}, "expect": {"visible": op == "show"}}])
    if op == "opacity":
        pct = rng.choice([10, 20, 25, 30, 40, 50, 60, 70, 75, 80, 90])
        return ex("layer_opacity", "layers", "number", "Set %s to %d%% opacity" % (nm, pct),
                  {"say": "Sets the layer's opacity to %d%%." % pct, "steps": [step("update_layer", index=i, opacity=pct / 100)]},
                  [{"t": "layer", "which": {"uid": l["uid"]}, "expect": {"opacity": [pct / 100 - 0.01, pct / 100 + 0.01]}}])
    if op == "opacity_vague":
        cls, aword, value = V.amount(rng)
        req = "Set %s to %s strength" % (nm, {"slight": "a slight", "bit": "a low", "noticeable": "a medium",
                                                "strong": "a strong"}[cls])
        return ex("layer_opacity", "layers", "vague", req,
                  {"say": "Sets the layer's opacity to %d%%." % round(value * 100), "steps": [step("update_layer", index=i, opacity=value)]},
                  [{"t": "layer", "which": {"uid": l["uid"]}, "expect": {"opacity": amount_opacity(cls)}}])
    if op == "blend":
        b = rng.choice(["multiply", "overlay", "screen", "soft_light", "normal", "add"])
        return ex("layer_blend", "layers", b, rng.choice(["Set the blend mode of %s to %s", "Change %s's blend mode to %s"])
                  % (nm, V.BLEND_WORDS[b][0]),
                  {"say": "Sets the blend mode to %s." % b.replace("_", " "), "steps": [step("update_layer", index=i, blend=b)]},
                  [{"t": "layer", "which": {"uid": l["uid"]}, "expect": {"blend": [b]}}])
    if op in ("top", "bottom"):
        to = top if op == "top" else 0
        if to == i or l["type"] == "folder":
            return None
        return ex("layer_move", "layers", op, "Move %s to the %s of the stack" % (nm, op),
                  {"say": "Moves the layer to the %s." % op, "steps": [step("move_layer", **{"from": i, "to": to})]},
                  [{"t": "layer_index", "uid": l["uid"], "index": to}])
    if op == "delete":
        return ex("layer_delete", "layers", "delete", rng.choice(["Delete %s", "Remove %s", "Get rid of %s"]) % nm,
                  {"say": "Deletes the layer.", "steps": [step("remove_layer", index=i)]},
                  [{"t": "layer_gone", "uid": l["uid"]}, {"t": "history", "delta": 1}])
    if op == "duplicate":
        return ex("layer_duplicate", "layers", "dup", "Duplicate %s" % nm,
                  {"say": "Duplicates the layer.", "steps": [step("duplicate_layer", index=i)]},
                  [{"t": "new_layers", "min": 1, "max": 1}])
    return ex("layer_folder", "layers", "folder", "Put %s in a folder" % nm,
              {"say": "Groups the layer into a new folder.", "steps": [step("group_into_folder", index=i)]},
              [{"t": "new_layers", "min": 1, "max": 1}, {"t": "layer", "which": "new", "expect": {"is_folder": True}}])


# ----------------------------------------------------------------------------- painting set-up and view
def brush(sc, rng):
    preset = rng.choice(list(V.BRUSH_WORDS))
    size = rng.choice([None, 10, 20, 30, 40, 60, 80, 120, 200])
    req = "Give me %s" % rng.choice(V.BRUSH_WORDS[preset])
    args = {"preset": preset}
    check = [{"t": "state", "path": "brush.preset", "eq": preset}]
    if size:
        req += rng.choice([" at %d pixels", " at size %d", ", %d px"]) % size
        args["size"] = size
        check.append({"t": "state", "path": "brush.size", "approx": size, "tol": 0.5})
    return ex("brush", "paint", preset, req, {"say": "Selects %s." % V.BRUSH_WORDS[preset][0], "steps": [step("set_brush", **args)]}, check)


def paint_with(sc, rng):
    if rng.random() < 0.5:
        word, ids = _material(rng)
        mid = rng.choice(ids)
        return ex("paint_material", "paint", "material", rng.choice(["Paint with %s", "Switch the brush to %s", "I want to paint %s"]) % word,
                  {"say": "Paints with %s." % word, "steps": [step("set_paint_material", id=mid)]},
                  [{"t": "state", "path": "material.id", "in": ids}])
    cname, rgb = V.colour(rng)
    return ex("paint_colour", "paint", "colour", rng.choice(["Paint in %s", "Set the paint colour to %s", "Use %s paint"]) % cname,
              {"say": "Paints in flat %s." % cname, "steps": [step("set_paint_material", id="", color=rgb)]},
              [{"t": "state", "path": "material.flat.color", "rgb": rgb, "tol": 0.3}, {"t": "state", "path": "material.id", "eq": ""}])


def tool(sc, rng):
    t = rng.choice(list(V.TOOLS))
    return ex("tool", "paint", t, rng.choice(["Switch to %s", "Use %s", "Give me %s"]) % V.TOOLS[t][0],
              {"say": "Switches to the %s tool." % t, "steps": [step("set_tool", tool=t)]}, [{"t": "state", "path": "tool", "eq": t}])


def view(sc, rng):
    mode = rng.choice(list(V.VIEW_MODES))
    return ex("view_mode", "view", mode, rng.choice(["Show me %s", "Switch the view to %s", "Let me see %s"]) % rng.choice(V.VIEW_MODES[mode]),
              {"say": "Shows %s." % V.VIEW_MODES[mode][0], "steps": [step("set_view", mode=mode)]},
              [{"t": "state", "path": "view_mode", "eq": mode}])


def camera(sc, rng):
    v = rng.choice(list(V.CAMERA_VIEWS))
    return ex("camera", "view", v, rng.choice(["Show me %s", "Look at %s", "Turn the camera to %s"]) % rng.choice(V.CAMERA_VIEWS[v]),
              {"say": "Turns the camera to the %s view." % v, "steps": [step("set_camera", view=v, frame=True)]},
              [{"t": "state", "path": "camera.yaw", "ref": True, "tol": 0.05}, {"t": "state", "path": "camera.pitch", "ref": True, "tol": 0.05}])


def symmetry(sc, rng):
    on = rng.random() < 0.7
    req = rng.choice(["Turn on X symmetry", "Paint symmetrically left to right", "Mirror my strokes across X"]) if on else \
        rng.choice(["Turn symmetry off", "Stop mirroring my strokes"])
    return ex("symmetry", "paint", "on" if on else "off", req,
              {"say": "Turns %s X symmetry." % ("on" if on else "off"), "steps": [step("set_symmetry", x=on)]},
              [{"t": "state", "path": "symmetry.0", "eq": on}])


# ----------------------------------------------------------------------------- follow-ups
def _made_tint(sc, rng):
    parts = usable_parts(sc)
    if not parts:
        return None
    part = rng.choice(parts)["id"]
    cname, rgb = V.colour(rng, ["red", "pink", "blue", "brown", "orange", "green", "purple"])
    cls, aword, value = V.amount(rng, rng.choice(["slight", "bit"]))
    req = "Tint %s %s %s %s" % (part_name(part), aword, cname, "in the creases")
    plan = {"say": "Adds a %s tint in the creases of %s." % (cname, part_name(part)),
            "steps": [step("add_tint_layer", name="%s tint" % cap(cname), color=rgb, strength=value, region={"part": part},
                           mask=[V.mask_spec("cavity", rng, jitter=False)])]}
    return part, cname, rgb, value, {"request": req, "plan": plan}


def followup(sc, rng):
    made = _made_tint(sc, rng)
    if not made:
        return None
    part, cname, rgb, value, setup = made
    idx = "@LAST"  # the index of the layer the setup request made (filled in when the example runs)
    kind = rng.choice(["stronger", "weaker", "colour", "region", "everywhere", "mask"])
    if kind in ("stronger", "weaker"):
        nv = round(min(0.95, value + 0.2), 2) if kind == "stronger" else round(max(0.05, value - 0.12), 2)
        req = rng.choice(["A bit stronger", "Make it stronger", "More of that"]) if kind == "stronger" else \
            rng.choice(["Too much, tone it down", "A bit more subtle", "Less of that"])
        steps = [step("update_layer", index=idx, opacity=nv)]
        check = [{"t": "layer", "which": {"name": "%s tint" % cap(cname)},
                  "expect": {"opacity": [value + 0.05, 1.0] if kind == "stronger" else [0.0, value - 0.03]}}]
        say = "Makes the tint %s." % ("stronger" if kind == "stronger" else "weaker")
    elif kind == "colour":
        n2, rgb2 = V.colour(rng, ["orange", "red", "pink", "purple", "blue", "yellow", "brown"])
        if n2 == cname:
            return None
        req = rng.choice(["Make it more %s instead", "Actually, %s", "Change the colour to %s"]) % n2
        steps = [step("update_layer", index=idx, material={"color": rgb2})]
        check = [{"t": "layer", "which": {"name": "%s tint" % cap(cname)}, "expect": {"color": {"rgb": rgb2, "tol": 0.3}}}]
        say = "Changes the tint to %s." % n2
    elif kind == "region":
        others = [p["id"] for p in usable_parts(sc) if p["id"] != part]
        if not others:
            return None
        p2 = rng.choice(others)
        req = rng.choice(["Only on %s", "Put it on %s instead", "Move that to %s"]) % part_name(p2)
        steps = [step("update_layer_mask", index=idx, region={"part": p2})]
        check = [{"t": "layer", "which": {"name": "%s tint" % cap(cname)}, "expect": {"region": faces_of(sc, p2), "masks": ["cavity"]}}]
        say = "Moves the tint to %s." % part_name(p2)
    elif kind == "everywhere":
        req = rng.choice(["Do that everywhere", "Apply it to the whole model", "Not just there, everywhere"])
        steps = [step("update_layer_mask", index=idx, everywhere=True)]
        check = [{"t": "layer", "which": {"name": "%s tint" % cap(cname)}, "expect": {"region": "all", "masks": ["cavity"]}}]
        say = "Spreads the tint over the whole model."
    else:
        mk = rng.choice(["edges", "occlusion", "noise"])
        ph = rng.choice(V.MASK_PHRASES[mk])
        req = ("Make it %s instead of following the creases" % ph if mk == "noise" else
               rng.choice(["Put it %s instead" % ph, "Put it %s instead of in the creases" % ph]))
        steps = [step("update_layer_mask", index=idx, mask=[V.mask_spec(mk, rng)])]
        check = [{"t": "layer", "which": {"name": "%s tint" % cap(cname)}, "expect": {"masks": [mk], "region": faces_of(sc, part)}}]
        say = "Puts the tint %s instead." % V.MASK_PHRASES[mk][0]
    check.append({"t": "history", "delta": 1})
    return ex("followup_" + kind, "followup", kind, req, {"say": say, "steps": steps}, check, setup=[setup])


# ----------------------------------------------------------------------------- answers without steps
CANT_DO = [
    ("Export this to Maya", "Patina exports textures (File > Export Textures), not models; open the model in Maya and use the textures from there."),
    ("Save the project", "Saving is up to you: use File > Save (Ctrl+S)."),
    ("Export the textures for Unreal", "Exporting is left to you: File > Export Textures with the Unreal preset."),
    ("Rig the character", "Patina paints textures; rigging is done in Blender or another 3D program."),
    ("Animate it walking", "Patina paints textures and can't animate models."),
    ("Add a hat to the model", "Patina paints the model you give it and can't add geometry; model the hat in Blender."),
    ("Sculpt more wrinkles into the face", "Patina doesn't sculpt; it can paint wrinkles as height detail with a brush, or bake them from a high-poly model."),
    ("Make the model higher poly", "Patina doesn't change the mesh; subdivide it in Blender and send it again."),
    ("Render a turntable video", "Rendering is up to you: File > Render Image has a turntable option."),
    ("Open my other project", "Opening files is up to you: File > Open."),
    ("Set the texture resolution to 4K", "Change the resolution in the Texture Set panel; it clears the undo history, so the Assistant leaves it to you."),
    ("Turn on texture sets per material", "Switch texture sets in the Texture Set panel; it clears the undo history, so the Assistant leaves it to you."),
    ("Send it back to Blender", "Use the Blender button in the toolbar (or Ctrl+B) to send the textures back."),
    ("Delete the mesh", "The Assistant doesn't remove models; close the project or open another model instead."),
    ("Upload this to Sketchfab", "Patina doesn't upload anything; export the textures and upload them yourself."),
    ("Write me a poem", "I can only change this project in Patina: materials, layers, masks, UVs, bakes and the view."),
    ("Change the topology", "Patina doesn't edit topology; do that in Blender."),
    ("Make a normal map from this photo", "Use Materials > Import Image as Material to make a material from a photo."),
]
VAGUE = ["Make it look better", "Fix it", "Improve the textures", "Make it pop", "Do something cool", "Make it nicer",
         "It looks wrong", "Make it more realistic", "Make it look good", "Can you help?"]


def cant_do(sc, rng):
    req, say = rng.choice(CANT_DO)
    return ex("cant_do", "none", "say", req, {"say": say}, [{"t": "plan", "kind": "say"}, {"t": "no_change"}])


def vague(sc, rng):
    req = rng.choice(VAGUE)
    ask = rng.choice(["What would you like to change: a colour, a material, wear and dirt, or the UVs?",
                      "Which part should change, and how? For example: add dirt in the creases, or make the jacket leather.",
                      "Could you say what you'd like? I can add materials, tints, wear and dirt, or rework the UVs."])
    return ex("vague", "none", "ask", req, {"ask": ask}, [{"t": "plan", "kind": "ask"}, {"t": "no_change"}])


ABSENT = ["the left ear", "the tail", "the wings", "the eyes", "the gloves", "the cape", "the left boot", "the sleeves",
          "the visor", "the teeth", "the horns", "the belt buckle", "the necklace"]


def unknown_part(sc, rng):
    names = {part_name(p["id"]) for p in sc["parts"]}
    absent = [a for a in ABSENT if a not in names and a.split()[-1] not in " ".join(names)]
    if not absent:
        return None
    a = rng.choice(absent)
    form = rng.choice(["Make %s red", "Add rust to %s", "Make %s leather", "Tint %s a bit darker"])
    listed = [p["id"] for p in usable_parts(sc)][:8]
    ask = ("I can't find %s in this model. The parts I can see are: %s. Which one did you mean?" % (a, ", ".join(listed))
           if listed else "I can't find %s: this model is a single part. Should I change the whole model?" % a)
    return ex("unknown_part", "none", "ask", form % a, {"ask": ask}, [{"t": "plan", "kind": "ask"}, {"t": "no_change"}])


def no_head(sc, rng):
    if sc["head_found"] or any(p["id"] in ("group:Head",) or "head" in p["id"].lower() for p in sc["parts"]):
        return None
    listed = [p["id"] for p in usable_parts(sc)][:8]
    req = rng.choice(["Make the head a bit redder", "Give the head its own UV island", "Add freckles to the head"])
    ask = ("Which part is the head? I can see: %s." % ", ".join(listed) if listed else
           "I can't find a head on this model; it is one part. Should I change the whole model?")
    return ex("no_head", "none", "ask", req, {"ask": ask}, [{"t": "plan", "kind": "ask"}, {"t": "no_change"}])


# ----------------------------------------------------------------------------- the list
TASKS = [
    (tint_masked, 10), (tint_plain, 4), (tint_whole, 4), (tint_explicit, 3),
    (material_region, 6), (material_whole, 2), (recipe, 10), (smart_material, 4), (auto_texture, 1),
    (uv_island_per_part, 4), (uv_part_op, 4), (uv_global, 3), (bake, 2),
    (layer_op, 8), (brush, 2), (paint_with, 2), (tool, 1), (view, 2), (camera, 1), (symmetry, 1),
    (followup, 6),
    (cant_do, 3), (vague, 2), (unknown_part, 2), (no_head, 1),
]


# ============================================================================= round 3
# Requests that set several things up at once, so the user drives the result with sliders
# instead of menus: whole looks (a folder of weathering layers), several changes in one
# sentence, coloured materials, getting ready to paint by hand, layers named by what they are,
# "everywhere except", the selection, and more follow-ups.

def _ids(word):
    return V.MATERIAL_WORDS[word]


def _amount_scale(rng):
    """An optional amount word for a whole look: (word or None, class, scale)."""
    r = rng.random()
    if r < 0.55:
        return None, None, 1.0
    cls = rng.choice(["slight", "bit", "strong", "strong"])
    word = rng.choice({"slight": ["slightly", "a little", "subtly", "lightly"], "bit": ["a bit", "somewhat"],
                       "strong": ["very", "really", "heavily"]}[cls])
    return word, cls, {"slight": 0.55, "bit": 0.75, "strong": 1.3}[cls]


# look: (words people use, [(material word, layer name, masks, strength)])
LOOKS = {
    "weathered": (["weathered", "aged", "old", "weather-beaten", "time-worn"],
                  [("dirt", "Dirt", ["cavity"], 0.5), ("dust", "Dust", ["facing_up"], 0.35),
                   ("grime", "Grime", ["occlusion"], 0.3)]),
    "battle-worn": (["battle-worn", "battle-damaged", "battered", "beaten up", "war-torn"],
                    [("scratched steel", "Edge wear", ["edges"], 0.6), ("dirt", "Dirt", ["cavity"], 0.45),
                     ("soot", "Soot", ["noise"], 0.25)]),
    "rusty": (["rusty", "rusted", "corroded", "rusted out"],
              [("light rust", "Rust", ["edges", "noise"], 0.5), ("heavy rust", "Heavy rust", ["cavity"], 0.6),
               ("grime", "Grime", ["occlusion"], 0.3)]),
    "abandoned": (["abandoned", "derelict", "neglected", "forgotten for years"],
                  [("dust", "Dust", ["facing_up"], 0.6), ("grime", "Grime", ["cavity"], 0.5),
                   ("moss", "Moss", ["occlusion", "noise"], 0.3)]),
    "muddy": (["muddy", "mud-splattered", "caked in mud"],
              [("mud", "Mud", ["gradient_down"], 0.7), ("mud", "Mud splatter", ["cavity", "noise"], 0.35)]),
    "burnt": (["burnt", "scorched", "charred", "fire-damaged"],
              [("soot", "Soot", ["gradient_up"], 0.6), ("soot", "Soot in the cracks", ["cavity"], 0.5)]),
    "snowy": (["snowy", "wintry", "snowed on", "frosty"],
              [("snow", "Snow", ["facing_up"], 0.75), ("snow", "Frost", ["cavity", "noise"], 0.3)]),
    "overgrown": (["overgrown", "mossy", "reclaimed by nature"],
                  [("moss", "Moss", ["facing_up", "noise"], 0.6), ("moss", "Moss in the cracks", ["cavity"], 0.4),
                   ("dirt", "Dirt", ["gradient_down"], 0.35)]),
    "dusty": (["dusty", "covered in dust", "like it sat in an attic"],
              [("dust", "Dust", ["facing_up"], 0.6), ("dust", "Dust in the cracks", ["cavity"], 0.3)]),
    "grimy": (["grimy", "filthy", "greasy"],
              [("grime", "Grime", ["cavity"], 0.55), ("dirt", "Dirt", ["gradient_down"], 0.4),
               ("dust", "Dust", ["facing_up"], 0.25)]),
}
LOOK_MATERIAL = {"dirt": "patina_dirt_mud", "mud": "patina_dirt_mud", "dust": "patina_dust", "grime": "patina_grime_oily",
                 "scratched steel": "patina_steel_scratched", "soot": "patina_soot", "light rust": "patina_rust_light",
                 "heavy rust": "patina_rust_heavy", "moss": "patina_moss", "snow": "patina_snow"}


def _look_words(comps):
    words = []
    for mword, _, masks, _ in comps:
        where = rng_free_phrase(masks)
        words.append("%s %s" % ("edge wear" if mword == "scratched steel" else mword.replace("light ", "").replace("heavy ", ""), where))
    return words


SAY_PLACE = {"cavity": "in the creases", "edges": "on the edges", "occlusion": "in the sheltered places", "facing_up": "on top",
             "gradient_down": "towards the bottom", "gradient_up": "towards the top"}


def rng_free_phrase(masks):
    core = [m for m in masks if m != "noise"]
    ph = " and ".join(SAY_PLACE[m] for m in core)
    if "noise" in masks:
        ph = (ph + ", " if ph else "") + "in patches"
    return ph


def look(sc, rng):
    key = rng.choice(list(LOOKS))
    words, comps = LOOKS[key]
    w = rng.choice(words)
    aword, cls, scale = _amount_scale(rng)
    parts = usable_parts(sc)
    part = rng.choice(parts)["id"] if parts and rng.random() < 0.5 else None
    target = part_name(part) if part else rng.choice(["it", "the model", "the whole thing", "everything"])
    adj = (aword + " " + w) if aword else w
    if rng.random() < 0.25 and not aword:
        req = "Give %s a %s look" % (target, w)
    else:
        req = rng.choice(["Make %s look %s", "I want %s to look %s", "Can you make %s look %s?"]) % (target, adj)
    steps, check = [], []
    for mword, lname, masks, strength in comps:
        args = {"material": LOOK_MATERIAL[mword], "name": lname, "strength": round(min(0.95, strength * scale), 2),
                "mask": [V.mask_spec(m, rng) for m in masks]}
        if part:
            args["region"] = {"part": part}
        steps.append(step("add_material_layer", **args))
        e = {"type": "fill", "material": [LOOK_MATERIAL[mword]], "masks": masks, "region": faces_of(sc, part) if part else "all"}
        if "noise" not in masks:
            e["masks_optional"] = ["noise"]
        check.append({"t": "layer", "which": "new", "expect": e})
    check += [{"t": "new_layers", "min": len(comps), "max": len(comps) + 1}, {"t": "history", "delta": 1}]
    if cls == "slight":
        check.append({"t": "new_opacity", "max_range": [0.0, 0.5]})
    elif cls == "strong":
        check.append({"t": "new_opacity", "max_range": [0.62, 1.0]})
    lw = _look_words(comps)
    say = "Makes %s look %s: %s." % (target if part else "the model", key.replace("-", " ") if key != "battle-worn" else key,
                                     ", ".join(lw[:-1]) + " and " + lw[-1])
    return ex("look_" + key.replace("-", "_"), "look", "part" if part else "all", req, {"say": say, "steps": steps}, check)


# ----------------------------------------------------------------------------- coloured materials
COLOURABLE = {  # word people say -> (material id, light enough to take a colour)
    "leather": "patina_leather_brown", "fabric": "patina_fabric_linen", "cloth": "patina_fabric_linen",
    "linen": "patina_fabric_linen", "canvas": "patina_canvas", "plastic": "patina_plastic_white",
    "metal": "patina_aluminum_brushed", "satin": "patina_satin_ivory", "wool": "patina_wool_grey",
}


def material_colour(sc, rng, part=None):
    parts = usable_parts(sc)
    if not parts:
        return None
    part = part or rng.choice(parts)["id"]
    word = rng.choice(list(COLOURABLE))
    mid = COLOURABLE[word]
    cname, rgb = V.colour(rng, ["red", "dark red", "blue", "navy", "green", "dark green", "black", "white", "brown",
                                "purple", "yellow", "orange", "pink", "grey", "teal", "cream", "tan", "maroon"])
    req = rng.choice(["Make %s %s %s" % (part_name(part), cname, word), "Give %s a %s %s material" % (part_name(part), cname, word),
                      "Turn %s into %s %s" % (part_name(part), cname, word), "%s should be %s %s" % (cap(part_name(part)), cname, word)])
    args = {"material": mid, "name": cap("%s %s" % (cname, word)), "tint": rgb, "region": {"part": part}}
    check = new_layer_check({"type": "fill", "material": [mid], "tint": {"rgb": rgb, "tol": 0.3}, "region": faces_of(sc, part)})
    return ex("material_colour", "material", word, req,
              {"say": "Fills %s with %s %s." % (part_name(part), cname, word), "steps": [step("add_material_layer", **args)]}, check)


# ----------------------------------------------------------------------------- several things at once
def _lower_first(s):
    return s[:1].lower() + s[1:] if not s.startswith("I ") else s


def multi(sc, rng):
    parts = [p["id"] for p in usable_parts(sc)]
    if len(parts) < 2:
        return None
    n = 3 if len(parts) >= 3 and rng.random() < 0.35 else 2
    chosen = rng.sample(parts, n)
    subs = []
    for p in chosen:
        kind = rng.choice(["material", "colour", "tint", "recipe", "recipe"])
        sub_sc = dict(sc)
        sub_sc["parts"] = [x for x in sc["parts"] if x["id"] == p]
        if kind == "material":
            e = material_region(sub_sc, rng)
        elif kind == "colour":
            e = material_colour(sub_sc, rng, part=p)
        elif kind == "tint":
            e = tint_plain(sub_sc, rng)
        else:
            e = None
            for _ in range(6):  # a recipe on that part (recipe() sometimes picks the whole model)
                e = recipe(sub_sc, rng)
                if e and all(st["args"].get("region") and "strength" not in st["args"] for st in e["plan"]["steps"]) \
                        and e["task"] != "recipe_wear":
                    break
                e = None
        if not e or "?" in e["request"]:
            return None
        subs.append(e)
    reqs = [s["request"].rstrip(".") for s in subs]
    req = reqs[0] + ", " + ", ".join(_lower_first(r) for r in reqs[1:-1]) + (", " if n > 2 else "") + "and " + _lower_first(reqs[-1])
    req = req.replace(", , ", ", ").replace(", and", " and" if n == 2 else ", and")
    steps, check = [], []
    for s in subs:
        steps += s["plan"]["steps"]
        check += [c for c in s["check"] if c["t"] == "layer"]
    check += [{"t": "new_layers", "min": n, "max": n + 1}, {"t": "history", "delta": 1}]
    says = [s["plan"]["say"].rstrip(".") for s in subs]
    say = says[0] + ", " + ", ".join(_lower_first(x) for x in says[1:-1]) + (", " if n > 2 else "") + "and " + _lower_first(says[-1]) + "."
    say = say.replace(", , ", ", ").replace(", and", " and" if n == 2 else ", and")
    return ex("multi", "multi", "+".join(s["task"].split("_")[0] for s in subs), req, {"say": say, "steps": steps}, check)


# ----------------------------------------------------------------------------- ready to paint by hand
# what to paint: (words, material word or None, colour or None, brush preset, layer name)
PAINT_SETUPS = [
    (["dirt", "some dirt", "grime marks"], "dirt", None, "grunge", "Painted dirt"),
    (["scratches", "some scratches"], "scratched steel", None, "scratches", "Scratches"),
    (["rust spots", "some rust"], "rust", None, "rust_spots", "Rust spots"),
    (["dust"], "dust", None, "dust_cloud", "Painted dust"),
    (["moss"], "moss", None, "grunge", "Painted moss"),
    (["soot marks", "soot"], "soot", None, "spray", "Soot"),
    (["freckles"], None, "brown", "splatter", "Freckles"),
    (["blood splatter", "blood"], None, "dark red", "splatter", "Blood"),
    (["blush", "rosy cheeks"], None, "blush", "airbrush", "Blush"),
    (["cracks"], None, "charcoal", "cracks", "Cracks"),
    (["stitching", "seams and stitches"], None, "black", "pencil", "Stitches"),
    (["edge wear"], "scratched steel", None, "edge_wear", "Edge wear"),
    (["makeup", "eyeliner"], None, "black", "ink", "Makeup"),
]


def paint_setup(sc, rng):
    words, mword, cname, preset, lname = rng.choice(PAINT_SETUPS)
    w = rng.choice(words)
    parts = usable_parts(sc)
    part = rng.choice(parts)["id"] if parts and rng.random() < 0.6 else None
    where = " on " + part_name(part) if part else ""
    req = rng.choice(["Set me up to paint %s%s by hand", "I want to hand-paint %s%s", "Get me ready to paint %s%s",
                      "Set up a layer and brush so I can paint %s%s", "I'd like to paint %s%s myself"]) % (w, where)
    steps = [step("add_layer", name=lname)]
    check = [{"t": "layer", "which": "new", "expect": {"type": "paint", "name_eq": lname}},
             {"t": "new_layers", "min": 1, "max": 1}, {"t": "history", "delta": 1}]
    if part:
        steps.append(step("update_layer_mask", index="$0.index", region={"part": part}))
        check[0]["expect"]["region"] = faces_of(sc, part)
    if mword:
        ids = _ids(mword)
        steps.append(step("set_paint_material", id=ids[0]))
        check.append({"t": "state", "path": "material.id", "in": ids})
    else:
        rgb = [round(c, 2) for c in V.COLOURS[cname]]
        steps.append(step("set_paint_material", id="", color=rgb))
        check.append({"t": "state", "path": "material.flat.color", "rgb": rgb, "tol": 0.3})
    steps.append(step("set_brush", preset=preset))
    steps.append(step("set_tool", tool="paint"))
    check += [{"t": "state", "path": "brush.preset", "eq": preset}, {"t": "state", "path": "tool", "eq": "paint"}]
    say = "Adds a paint layer for %s%s, with %s and %s ready." % (w, " that only shows" + where if part else "",
                                                                  (mword if mword else cname + " paint"), V.BRUSH_WORDS[preset][0])
    return ex("paint_setup", "paint", "part" if part else "all", req, {"say": say, "steps": steps}, check)


# ----------------------------------------------------------------------------- layers named by what they are
def _layer_word(l):
    """How a person names a layer: by its name, or by its material when the name says nothing."""
    n = l["name"]
    if re.fullmatch(r"(Layer|Fill|Paint|Tint)( \d+)?", n) and l.get("material"):
        m = re.sub(r"^patina_", "", l["material"]).split("_")[0]
        return "the %s" % m, "the %s layer" % m
    return "the %s" % n.lower(), "the %s layer" % n.lower()


def layer_ref(sc, rng):
    ls = [l for l in _layers(sc, ["fill", "paint"]) if l["name"] not in ("Layer 1",)]
    if not ls:
        return None
    l = rng.choice(ls)
    short, long_ = _layer_word(l)
    nm = rng.choice([short, long_])
    i = l["index"]
    kind = rng.choice(["weaker", "weaker", "stronger", "hide", "show", "remove"])
    pre = []
    if kind == "weaker":
        req = rng.choice(["Turn %s down", "Tone %s down a bit", "%s is too strong", "Fade %s out a bit", "Less of %s"]) % nm
        req = cap(req)
        steps = [step("update_layer", index=i, opacity=0.6)]
        check = [{"t": "layer", "which": {"uid": l["uid"]}, "expect": {"opacity": [0.0, 0.85]}}]
        say = "Turns %s down to 60%%." % long_
    elif kind == "stronger":
        pre = [["update_layer", {"index": i, "opacity": 0.4}]]
        req = cap(rng.choice(["Make %s stronger", "Turn %s up", "More of %s", "%s is too faint", "Bring %s up a bit"]) % nm)
        steps = [step("update_layer", index=i, opacity=0.7)]
        check = [{"t": "layer", "which": {"uid": l["uid"]}, "expect": {"opacity": [0.5, 1.0]}}]
        say = "Turns %s up to 70%%." % long_
    elif kind in ("hide", "show"):
        if kind == "show":
            pre = [["update_layer", {"index": i, "visible": False}]]
        req = cap(rng.choice(["Hide %s", "Turn off %s", "Switch %s off for now"] if kind == "hide" else
                             ["Show %s again", "Turn %s back on", "Bring back %s"]) % nm)
        steps = [step("update_layer", index=i, visible=kind == "show")]
        check = [{"t": "layer", "which": {"uid": l["uid"]}, "expect": {"visible": kind == "show"}}]
        say = ("Hides %s." if kind == "hide" else "Shows %s again.") % long_
    else:
        req = cap(rng.choice(["Get rid of %s", "Delete %s", "Remove %s, I don't like it"]) % nm)
        steps = [step("remove_layer", index=i)]
        check = [{"t": "layer_gone", "uid": l["uid"]}]
        say = "Deletes %s." % long_
    check.append({"t": "history", "delta": 1})
    e = ex("layer_ref_" + kind, "layers", "by_name" if short.split()[-1] in l["name"].lower() else "by_material",
           req, {"say": say, "steps": steps}, check)
    e["pre"] = pre
    return e


# ----------------------------------------------------------------------------- everywhere except
def except_part(sc, rng):
    parts = usable_parts(sc)
    if len(parts) < 2:
        return None
    part = rng.choice(parts)["id"]
    keep = region_faces(sc, ["all"], except_=[part])
    if not keep or len(keep) == len(faces_of(sc, "all")):
        return None
    if rng.random() < 0.5:
        kind = rng.choice(["dirt", "dust", "rust", "soot", "moss", "grime"])
        word = {"rust": "light rust"}.get(kind, kind)
        mk = {"dirt": "cavity", "dust": "facing_up", "rust": "edges", "soot": "cavity", "moss": "facing_up", "grime": "cavity"}[kind]
        req = rng.choice(["Add %s everywhere except %s", "Put %s on everything but %s", "%s on the whole model apart from %s"]) % (kind, part_name(part))
        req = cap(req)
        ids = _ids(word)
        args = {"material": ids[0], "name": cap(kind), "mask": [V.mask_spec(mk, rng)], "region": {"except": [part]}}
        steps = [step("add_material_layer", **args)]
        expect = {"type": "fill", "material": ids, "masks": [mk], "masks_optional": ["noise"], "region": keep}
        say = "Adds %s %s everywhere except %s." % (kind, V.MASK_PHRASES[mk][0], part_name(part))
    else:
        cname, rgb = V.colour(rng, ["dark brown", "black", "grey", "brown", "dark red", "blue", "green"])
        cls, aword, value = V.amount(rng)
        req = cap(rng.choice(["Tint everything %s %s except %s", "Make the whole model %s %s, but not %s"]) % (aword, cname, part_name(part)))
        steps = [step("add_tint_layer", name="%s tint" % cap(cname), color=rgb, strength=value, region={"except": [part]})]
        expect = {"type": "fill", "color": {"rgb": rgb, "tol": 0.3}, "opacity": amount_opacity(cls), "region": keep}
        say = "Adds a %s tint everywhere except %s." % (cname, part_name(part))
    return ex("except_part", "tint", "except", req, {"say": say, "steps": steps}, new_layer_check(expect))


# ----------------------------------------------------------------------------- the selection
SELECTION_NAMES = ["these faces", "the selection", "the selected faces", "what I selected", "the faces I selected",
                   "the bit I selected"]


def selection_op(sc, rng):
    parts = usable_parts(sc, ("slot", "group", "piece"))
    if not parts:
        return None
    src = rng.choice(parts)["id"]
    faces = faces_of(sc, src)
    if len(faces) < 4 or len(faces) > 0.8 * len(faces_of(sc, "all")):
        return None
    pre = [["select_part", {"part": src}]]
    nm = rng.choice(SELECTION_NAMES)
    kind = rng.choice(["material", "tint", "recipe", "island", "unwrap", "rust"])
    if kind == "material":
        word, ids = _material(rng)
        req = rng.choice(["Make %s %s", "Put %s on %s"])
        req = req % ((nm, word) if req.startswith("Make") else (word, nm))
        steps = [step("add_material_layer", material=ids[0], name=cap(word), region={"part": "selection"})]
        check = new_layer_check({"type": "fill", "material": ids, "region": faces, "min_jaccard": 0.85})
        say = "Fills the selected faces with %s." % word
    elif kind == "tint":
        cname, rgb = V.colour(rng)
        cls, aword, value = V.amount(rng)
        req = rng.choice(["Tint %s %s %s" % (nm, aword, cname), "Make %s %s %s" % (nm, aword, more(cname))])
        steps = [step("add_tint_layer", name="%s tint" % cap(cname), color=rgb, strength=value, region={"part": "selection"})]
        check = new_layer_check({"type": "fill", "color": {"rgb": rgb, "tol": 0.3}, "opacity": amount_opacity(cls), "region": faces,
                                 "min_jaccard": 0.85})
        say = "Adds a %s tint to the selected faces." % cname
    elif kind in ("recipe", "rust"):
        word, mk = ("light rust", "edges") if kind == "rust" else rng.choice([("dirt", "cavity"), ("dust", "facing_up"), ("moss", "facing_up"), ("soot", "cavity")])
        ids = _ids(word)
        req = rng.choice(["Add %s %s on %s", "Put some %s %s on %s"]) % (word.replace("light ", ""), V.MASK_PHRASES[mk][0], nm)
        steps = [step("add_material_layer", material=ids[0], name=cap(word.replace("light ", "")), mask=[V.mask_spec(mk, rng)],
                      region={"part": "selection"})]
        check = new_layer_check({"type": "fill", "material": ids, "masks": [mk], "masks_optional": ["noise"], "region": faces,
                                 "min_jaccard": 0.85})
        say = "Adds %s %s on the selected faces." % (word.replace("light ", ""), V.MASK_PHRASES[mk][0])
    elif kind == "island":
        req = rng.choice(["Make %s one UV island", "Give %s its own UV island", "Turn %s into a single island"]) % nm
        steps = [step("uv_make_island", selection=True)]
        check = [{"t": "uv_changed"}, {"t": "uv_parts", "parts": [{"faces": faces, "islands_max": 1}], "overlaps_max": 99},
                 {"t": "history", "delta": 1}]
        say = "Makes the selected faces one UV island."
    else:
        req = rng.choice(["Unwrap %s again", "Redo the UVs of %s", "Relax the UVs of %s"]) % nm
        cmd = "uv_relax" if req.startswith("Relax") else "uv_unwrap"
        steps = [step(cmd, selection=True) if cmd == "uv_relax" else step("uv_unwrap", method="lscm", selection=True)]
        check = [{"t": "uv_changed"}, {"t": "history", "delta": 1}]
        say = ("Relaxes" if cmd == "uv_relax" else "Unwraps") + " the UVs of the selected faces."
    e = ex("selection_" + ("uv" if kind in ("island", "unwrap") else "paint"), "selection", kind, cap(req), {"say": say, "steps": steps}, check)
    e["pre"] = pre
    return e


# ----------------------------------------------------------------------------- more follow-ups
def followup2(sc, rng):
    """After a material or wear layer on a part: remove it, add a part, rename, hide, stronger/weaker."""
    parts = usable_parts(sc)
    if len(parts) < 2:
        return None
    part = rng.choice(parts)["id"]
    kind0 = rng.choice(["material", "recipe"])
    if kind0 == "material":
        word, ids = _material(rng, ["leather", "denim", "wood", "steel", "gold", "rubber", "linen", "plaid", "brass", "concrete"])
        sreq = "Make %s %s" % (part_name(part), word)
        sargs = {"material": ids[0], "name": cap(word), "region": {"part": part}, "strength": 0.6}
        sname = cap(word)
    else:
        word, mk = rng.choice([("dirt", "cavity"), ("dust", "facing_up"), ("light rust", "edges"), ("moss", "facing_up"), ("soot", "cavity")])
        ids = _ids(word)
        sreq = "Add %s %s on %s" % (word.replace("light ", ""), V.MASK_PHRASES[mk][0], part_name(part))
        sname = cap(word.replace("light ", ""))
        sargs = {"material": ids[0], "name": sname, "region": {"part": part}, "strength": 0.6, "mask": [V.mask_spec(mk, rng, jitter=False)]}
    if any(l["name"] == sname for l in sc["layers"]):
        return None  # "remove that" is checked by the name being gone
    setup = {"request": sreq, "plan": {"say": "Adds %s to %s." % (word, part_name(part)), "steps": [step("add_material_layer", **sargs)]}}
    idx = "@LAST"
    kind = rng.choice(["remove", "also", "rename", "hide", "stronger", "weaker"])
    which = {"name": sname}
    if kind == "remove":
        req = rng.choice(["Actually, remove that", "Get rid of it", "Delete that layer", "No, take that away"])
        steps = [step("remove_layer", index=idx)]
        check = [{"t": "new_layers", "min": 0, "max": 0}, {"t": "layer_absent", "name": sname}]
        say = "Deletes the layer it just made."
    elif kind == "also":
        p2 = rng.choice([p["id"] for p in parts if p["id"] != part])
        req = rng.choice(["Put it on %s too", "Also on %s", "Do the same on %s as well"]) % part_name(p2)
        steps = [step("update_layer_mask", index=idx, region={"parts": [part, p2]})]
        check = [{"t": "layer", "which": which, "expect": {"region": region_faces(sc, [part, p2])}}]
        say = "Spreads it to %s as well." % part_name(p2)
    elif kind == "rename":
        new = rng.choice(["Base", "Main", "Details", "Wear pass", "Final", "Test"])
        req = rng.choice(["Call that layer %s", "Rename it to %s", "Name it %s"]) % new
        steps = [step("update_layer", index=idx, name=new)]
        check = [{"t": "layer", "which": {"name": new}, "expect": {"type": "fill"}}]
        say = 'Renames it to "%s".' % new
    elif kind == "hide":
        req = rng.choice(["Hide it for now", "Turn that off for a moment", "Switch it off"])
        steps = [step("update_layer", index=idx, visible=False)]
        check = [{"t": "layer", "which": which, "expect": {"visible": False}}]
        say = "Hides the layer."
    elif kind == "stronger":
        req = rng.choice(["Stronger", "Turn it up", "More of that, please", "Make it stronger"])
        steps = [step("update_layer", index=idx, opacity=0.85)]
        check = [{"t": "layer", "which": which, "expect": {"opacity": [0.65, 1.0]}}]
        say = "Makes it stronger."
    else:
        req = rng.choice(["Less of that", "Tone it down", "Too much, make it subtler", "Weaker"])
        steps = [step("update_layer", index=idx, opacity=0.35)]
        check = [{"t": "layer", "which": which, "expect": {"opacity": [0.0, 0.5]}}]
        say = "Makes it weaker."
    check.append({"t": "history", "delta": 1})
    return ex("followup2_" + kind, "followup", kind0 + "/" + kind, req, {"say": say, "steps": steps}, check, setup=[setup])


TASKS_R3 = [
    (look, 12), (material_colour, 5), (multi, 9), (paint_setup, 6), (layer_ref, 6), (except_part, 3),
    (selection_op, 5), (followup2, 7),
]


# ----------------------------------------------------------------------------- adjustments on a part
# kind: (layer name, adjust key, values for slight / a bit / noticeably / strongly, how people say it)
ADJUSTS = {
    "darker": ("Darker", "brightness", (0.85, 0.75, 0.62, 0.45), ["Make {p} {a}darker", "Darken {p}{a2}", "{P} should be {a}darker"]),
    "lighter": ("Lighter", "brightness", (1.15, 1.28, 1.42, 1.65), ["Make {p} {a}lighter", "Brighten {p}{a2}", "{P} should be {a}lighter"]),
    "saturated": ("More saturated", "saturation", (1.2, 1.35, 1.55, 1.85), ["Make {p} {a}more saturated", "Make the colours of {p} {a}more vivid",
                                                                               "Boost the saturation of {p}{a2}"]),
    "desaturated": ("Less saturated", "saturation", (0.8, 0.65, 0.48, 0.25), ["Make {p} {a}less saturated", "Desaturate {p}{a2}",
                                                                                "Make {p} {a}more muted"]),
    "contrast": ("More contrast", "contrast", (1.15, 1.3, 1.5, 1.8), ["Give {p} {a}more contrast", "Add contrast to {p}{a2}"]),
    "rougher": ("Rougher", "roughness", (0.1, 0.2, 0.32, 0.45), ["Make {p} {a}rougher", "Make {p} {a}less shiny", "Make {p} {a}more matte"]),
    "glossier": ("Glossier", "roughness", (-0.1, -0.2, -0.32, -0.45), ["Make {p} {a}glossier", "Make {p} {a}shinier", "Make {p} {a}less rough"]),
}
_ADJ_CLASS = ["slight", "bit", "noticeable", "strong"]


def adjust_part(sc, rng):
    kind = rng.choice(list(ADJUSTS))
    lname, key, values, forms = ADJUSTS[kind]
    parts = usable_parts(sc)
    part = rng.choice(parts)["id"] if parts and rng.random() < 0.7 else None
    pn = part_name(part) if part else rng.choice(["the whole model", "everything", "it"])
    use_amount = rng.random() < 0.6
    cls, aword, _ = V.amount(rng) if use_amount else ("bit", "", 0)
    form = rng.choice(forms)
    req = form.replace("{p}", pn).replace("{P}", cap(pn)).replace("{a}", (aword + " ") if use_amount else "") \
        .replace("{a2}", (" " + aword) if use_amount else "")
    v = values[_ADJ_CLASS.index(cls)]
    steps = [step("add_layer", type="adjust", name=lname, adjust={key: v})]
    lo_hi = {"brightness": (0.0, 0.95) if v < 1 else (1.05, 3.0), "saturation": (0.0, 0.95) if v < 1 else (1.05, 3.0),
             "contrast": (1.05, 3.0), "roughness": (0.04, 1.0) if v > 0 else (-1.0, -0.04)}[key]
    expect = {"type": "adjust", "adjust": {key: list(lo_hi)}, "region": faces_of(sc, part) if part else "all"}
    if part:
        steps.append(step("update_layer_mask", index="$0.index", region={"part": part}))
    say = "Adds an adjustment layer that makes %s %s." % (pn if part else "the model",
                                                          {"darker": "darker", "lighter": "lighter", "saturated": "more saturated",
                                                           "desaturated": "less saturated", "contrast": "more contrasty",
                                                           "rougher": "rougher", "glossier": "glossier"}[kind])
    return ex("adjust_" + kind, "adjust", "part" if part else "all", req, {"say": say, "steps": steps}, new_layer_check(expect))


def layer_ref2(sc, rng):
    """Two layers at once: 'hide the dirt and the rust'."""
    ls = [l for l in _layers(sc, ["fill", "paint"]) if l["name"] not in ("Layer 1",)]
    if len(ls) < 2:
        return None
    a, b = rng.sample(ls, 2)
    na, nb = _layer_word(a)[0], _layer_word(b)[0]
    kind = rng.choice(["hide", "hide", "remove", "weaker"])
    if kind == "hide":
        req = cap(rng.choice(["Hide %s and %s", "Turn off %s and %s", "Switch off %s and %s for now"]) % (na, nb))
        steps = [step("update_layer", index=l["index"], visible=False) for l in (a, b)]
        check = [{"t": "layer", "which": {"uid": l["uid"]}, "expect": {"visible": False}} for l in (a, b)]
        say = "Hides %s and %s." % (na, nb)
    elif kind == "remove":
        req = cap(rng.choice(["Delete %s and %s", "Get rid of %s and %s"]) % (na, nb))
        hi, lo = sorted((a, b), key=lambda l: -l["index"])  # the higher one first, so the other index stays
        steps = [step("remove_layer", index=hi["index"]), step("remove_layer", index=lo["index"])]
        check = [{"t": "layer_gone", "uid": a["uid"]}, {"t": "layer_gone", "uid": b["uid"]}]
        say = "Deletes %s and %s." % (na, nb)
    else:
        req = cap(rng.choice(["Turn down %s and %s", "Tone down %s and %s a bit"]) % (na, nb))
        steps = [step("update_layer", index=l["index"], opacity=0.6) for l in (a, b)]
        check = [{"t": "layer", "which": {"uid": l["uid"]}, "expect": {"opacity": [0.0, 0.85]}} for l in (a, b)]
        say = "Turns %s and %s down to 60%%." % (na, nb)
    check.append({"t": "history", "delta": 1})
    return ex("layer_ref2_" + kind, "layers", kind, req, {"say": say, "steps": steps}, check)


TASKS_R3 += [(adjust_part, 6), (layer_ref2, 3)]


# ============================================================================= round 4
# The brush, relative to how it is set now (the context's "Painting:" line); cleaning a model up
# again; more things Patina doesn't do, and what the Assistant can do.

def brush_tweak(sc, rng):
    preset = rng.choice(["basic_soft", "basic_hard", "airbrush", "grunge", "chalk", "spray", "pencil"])
    size = rng.choice([12, 20, 30, 40, 50, 60, 80, 100, 150])
    kind = rng.choice(["bigger", "bigger", "smaller", "smaller", "softer", "harder", "flow", "lazy_on", "lazy_off",
                       "island_on", "island_off", "through", "no_through"])
    pre_args = {"preset": preset, "size": size}
    if kind == "softer":
        pre_args["hardness"] = rng.choice([0.7, 0.8, 0.9])
    elif kind == "harder":
        pre_args["hardness"] = rng.choice([0.2, 0.3, 0.4])
    elif kind == "flow":
        pre_args["flow"] = rng.choice([0.8, 0.9, 1.0])
    elif kind == "lazy_off":
        pre_args["lazy_radius"] = rng.choice([15, 25, 40])
    elif kind == "no_through":
        pre_args["backface_culling"] = False
    pre = [["set_brush", pre_args]]
    if kind == "island_off":
        pre.append(["set_brush", {"lock_island": True}])
    if kind == "bigger":
        lots = rng.random() < 0.35
        req = rng.choice(["Make the brush a lot bigger", "Much bigger brush"] if lots else
                         ["Make the brush bigger", "Bigger brush please", "Increase the brush size", "My brush is too small"])
        new = round(size * (2.5 if lots else 1.6))
        args, check = {"size": new}, [{"t": "state", "path": "brush.size", "range": [size * (1.8 if lots else 1.2), size * 4]}]
        say = "Makes the brush %d px." % new
    elif kind == "smaller":
        lots = rng.random() < 0.35
        req = rng.choice(["Make the brush a lot smaller", "Much smaller brush"] if lots else
                         ["Make the brush smaller", "Smaller brush", "Shrink the brush", "My brush is too big"])
        new = max(2, round(size * (0.4 if lots else 0.6)))
        args, check = {"size": new}, [{"t": "state", "path": "brush.size", "range": [size * 0.2, size * (0.55 if lots else 0.85)]}]
        say = "Makes the brush %d px." % new
    elif kind == "softer":
        h = pre_args["hardness"]
        req = rng.choice(["Make the brush softer", "Softer edges on the brush", "I want a softer falloff"])
        args, check = {"hardness": round(h - 0.4, 2)}, [{"t": "state", "path": "brush.hardness", "range": [0.0, h - 0.15]}]
        say = "Makes the brush softer."
    elif kind == "harder":
        h = pre_args["hardness"]
        req = rng.choice(["Make the brush harder", "Crisper brush edges", "Give the brush a harder edge"])
        args, check = {"hardness": round(h + 0.45, 2)}, [{"t": "state", "path": "brush.hardness", "range": [h + 0.15, 1.0]}]
        say = "Makes the brush harder."
    elif kind == "flow":
        f = pre_args["flow"]
        req = rng.choice(["Lower the brush flow", "I want to build the paint up slowly", "Less flow on the brush"])
        args, check = {"flow": 0.3}, [{"t": "state", "path": "brush.flow", "range": [0.0, f - 0.2]}]
        say = "Lowers the flow so paint builds up slowly."
    elif kind == "lazy_on":
        req = rng.choice(["Turn on lazy mouse", "Smooth out my strokes", "Stabilise my brush strokes"])
        args, check = {"lazy_radius": 25}, [{"t": "state", "path": "brush.lazy_radius", "range": [5, 200]}]
        say = "Turns on lazy mouse to smooth the strokes."
    elif kind == "lazy_off":
        req = rng.choice(["Turn off lazy mouse", "Stop smoothing my strokes", "No stabiliser"])
        args, check = {"lazy_radius": 0}, [{"t": "state", "path": "brush.lazy_radius", "eq": 0}]
        say = "Turns lazy mouse off."
    elif kind == "island_on":
        req = rng.choice(["Only paint on one UV island at a time", "Turn on island lock", "Keep my strokes on one island"])
        args, check = {"lock_island": True}, [{"t": "state", "path": "brush.lock_island", "eq": True}]
        say = "Turns on island lock: a stroke stays on the island it starts on."
    elif kind == "island_off":
        req = rng.choice(["Turn off island lock", "Let strokes cross islands again"])
        args, check = {"lock_island": False}, [{"t": "state", "path": "brush.lock_island", "eq": False}]
        say = "Turns island lock off."
    elif kind == "through":
        req = rng.choice(["Paint through to the back too", "Let the brush paint the far side as well"])
        args, check = {"backface_culling": False}, [{"t": "state", "path": "brush.backface_culling", "eq": False}]
        say = "Lets the brush paint faces turned away too."
    else:
        req = rng.choice(["Don't paint through to the back", "Stop painting the far side"])
        args, check = {"backface_culling": True}, [{"t": "state", "path": "brush.backface_culling", "eq": True}]
        say = "Stops the brush painting faces turned away."
    e = ex("brush_tweak", "paint", kind, req, {"say": say, "steps": [step("set_brush", **args)]}, check)
    e["pre"] = pre
    return e


WEATHER_IDS = {"patina_dirt_mud", "patina_dust", "patina_grime_oily", "patina_rust_light", "patina_rust_heavy", "patina_soot",
               "patina_moss", "patina_moss_patchy", "patina_snow", "patina_steel_scratched"}


def clean(sc, rng):
    ws = [l for l in _layers(sc, ["fill"]) if l.get("material") in WEATHER_IDS]
    if not ws:
        return None
    kind = rng.choice(["hide", "hide", "remove"])
    if kind == "hide":
        req = rng.choice(["Make it look clean again", "Hide all the weathering", "Turn off all the wear and dirt",
                          "Show me the model without the grime", "Clean it up for now"])
        steps = [step("update_layer", index=l["index"], visible=False) for l in ws]
        check = [{"t": "layer", "which": {"uid": l["uid"]}, "expect": {"visible": False}} for l in ws]
        say = "Hides the weathering layers (%s)." % ", ".join(l["name"] for l in ws)
    else:
        req = rng.choice(["Remove all the weathering layers", "Delete all the dirt and wear", "Get rid of all the weathering"])
        steps = [step("remove_layer", index=l["index"]) for l in sorted(ws, key=lambda l: -l["index"])]
        check = [{"t": "layer_gone", "uid": l["uid"]} for l in ws]
        say = "Deletes the weathering layers (%s)." % ", ".join(l["name"] for l in ws)
    check.append({"t": "history", "delta": 1})
    return ex("clean_" + kind, "layers", kind, req, {"say": say, "steps": steps}, check)


CANT_DO_R4 = [
    ("Add a shield to his arm", "Patina paints the model you give it and can't add objects; model the shield in Blender and send it again."),
    ("Give him wings", "Patina can't add geometry; model the wings in Blender and send the model again."),
    ("Make the arms longer", "Patina doesn't change the shape of the model; do that in Blender."),
    ("Pose the character", "Patina doesn't pose models; pose it in Blender."),
    ("Undo that", "Press Ctrl+Z to undo the last step (Ctrl+Shift+Z redoes it)."),
    ("Undo the last thing", "Press Ctrl+Z to undo the last step (Ctrl+Shift+Z redoes it)."),
    ("What can you do?", "I set things up for you: whole looks (weathered, rusty, muddy...), materials and tints on parts, "
                         "wear and dirt where it belongs, layers to paint on by hand, adjustments, UVs, bakes and the brush."),
    ("Help", "Say what you'd like changed, for example: make the jacket leather, make the armour look battle-worn, "
             "or set me up to paint scratches on the helmet."),
    ("Make a texture from scratch with AI", "I don't generate images; I set up Patina's materials, masks and layers for you."),
    ("Make the model smaller", "Patina doesn't change the model's size; scale it in Blender."),
]


def cant_do_r4(sc, rng):
    req, say = rng.choice(CANT_DO_R4)
    return ex("cant_do2", "none", "say", req, {"say": say}, [{"t": "plan", "kind": "say"}, {"t": "no_change"}])


TASKS_R4 = [(brush_tweak, 6), (clean, 3), (cant_do_r4, 2)]


# ----------------------------------------------------------------------------- round 4: from round 3's misses
def look2(sc, rng):
    """Two looks at once ("old and rusty"): the layers of both, the same material and place once."""
    k1, k2 = rng.sample(list(LOOKS), 2)
    w1, w2 = rng.choice(LOOKS[k1][0]), rng.choice(LOOKS[k2][0])
    comps, seen = [], set()
    for c in LOOKS[k2][1][:2] + LOOKS[k1][1]:  # the second word's main layers first, they're what's asked last
        key = (LOOK_MATERIAL[c[0]], tuple(c[2]))
        if key in seen or LOOK_MATERIAL[c[0]] in {LOOK_MATERIAL[x[0]] for x in comps}:
            continue
        seen.add(key)
        comps.append(c)
    comps = comps[:4]
    parts = usable_parts(sc)
    part = rng.choice(parts)["id"] if parts and rng.random() < 0.5 else None
    target = part_name(part) if part else rng.choice(["it", "the model", "everything"])
    req = rng.choice(["Make %s look %s and %s", "I want %s to look %s and %s", "Can you make %s look %s and %s?"]) % (target, w1, w2)
    steps, check = [], []
    for mword, lname, masks, strength in comps:
        args = {"material": LOOK_MATERIAL[mword], "name": lname, "strength": strength, "mask": [{"type": m} for m in masks]}
        if part:
            args["region"] = {"part": part}
        steps.append(step("add_material_layer", **args))
        e = {"type": "fill", "material": [LOOK_MATERIAL[mword]], "masks": masks, "masks_optional": ["noise"],
             "region": faces_of(sc, part) if part else "all"}
        check.append({"t": "layer", "which": "new", "expect": e})
    check += [{"t": "new_layers", "min": len(comps), "max": len(comps) + 1}, {"t": "history", "delta": 1}]
    lw = _look_words(comps)
    say = "Makes %s look %s and %s: %s." % (target if part else "the model", k1.replace("-", " ") if k1 != "battle-worn" else k1,
                                            k2.replace("-", " ") if k2 != "battle-worn" else k2, ", ".join(lw[:-1]) + " and " + lw[-1])
    return ex("look2", "look", "part" if part else "all", req, {"say": say, "steps": steps}, check)


CASUAL = {"dirt": ("dirt", "cavity"), "rust": ("light rust", "edges"), "dust": ("dust", "facing_up"), "moss": ("moss", "facing_up"),
          "snow": ("snow", "facing_up"), "soot": ("soot", "cavity"), "grime": ("grime", "cavity"), "mud": ("mud", "gradient_down")}


def recipe_casual(sc, rng):
    kind = rng.choice(list(CASUAL))
    word, mk = CASUAL[kind]
    parts = usable_parts(sc)
    part = rng.choice(parts)["id"] if parts and rng.random() < 0.7 else None
    where = part_name(part) if part else rng.choice(["it", "the model", "everything"])
    req = rng.choice(["Slap some %s on %s", "Throw some %s on %s", "Chuck some %s on %s", "Splash some %s on %s",
                      "Bit of %s on %s", "Some %s on %s please"]) % (kind, where)
    ids = _ids(word)
    args = {"material": ids[0], "name": cap(kind), "mask": [{"type": mk}]}
    if part:
        args["region"] = {"part": part}
    expect = {"type": "fill", "material": ids, "masks": [mk], "masks_optional": ["noise"], "region": faces_of(sc, part) if part else "all"}
    say = "Adds %s %s%s." % (kind, SAY_PLACE[mk], " of " + part_name(part) if part else "")
    return ex("recipe_casual", "recipe", kind, cap(req), {"say": say, "steps": [step("add_material_layer", **args)]}, new_layer_check(expect))


def multi_for(sc, rng):
    """'oak for the barrel, iron for the hoops': materials listed against parts."""
    parts = [p["id"] for p in usable_parts(sc)]
    if len(parts) < 2:
        return None
    chosen = rng.sample(parts, min(len(parts), rng.choice([2, 2, 3])))
    words = rng.sample([w for w in V.MATERIAL_WORDS if " " not in w and w not in ("dirt", "dust", "mud", "snow", "soot", "moss", "grime")],
                       len(chosen))
    form = rng.choice(["{m} for {p}", "{p}: {m}", "{p} in {m}"])
    bits = [form.replace("{m}", w).replace("{p}", part_name(p)) for w, p in zip(words, chosen)]
    req = cap(", ".join(bits[:-1]) + " and " + bits[-1])
    steps, check = [], []
    for w, p in zip(words, chosen):
        ids = V.MATERIAL_WORDS[w]
        steps.append(step("add_material_layer", material=ids[0], name=cap(w), region={"part": p}))
        check.append({"t": "layer", "which": "new", "expect": {"type": "fill", "material": ids, "region": faces_of(sc, p)}})
    check += [{"t": "new_layers", "min": len(chosen), "max": len(chosen) + 1}, {"t": "history", "delta": 1}]
    say = "Fills " + ", ".join("%s with %s" % (part_name(p), w) for w, p in zip(words[:-1], chosen[:-1])) + \
          " and %s with %s." % (part_name(chosen[-1]), words[-1])
    return ex("multi_for", "multi", form.split()[1] if " " in form else "colon", req, {"say": say, "steps": steps}, check)


TASKS_R4 += [(look2, 3), (recipe_casual, 3), (multi_for, 3)]


# ============================================================================= round 5: more ways to say it
BRUSH_SAY = {
    "bigger": ["Make the brush bigger", "Bigger brush please", "Increase the brush size", "My brush is too small", "Brush up a size",
               "Can I have a larger brush?", "Scale the brush up", "Double the brush size", "Brush is tiny, make it larger"],
    "smaller": ["Make the brush smaller", "Smaller brush", "Shrink the brush", "My brush is too big", "Halve the brush size",
                "Brush down a size", "Reduce the brush size", "Brush is way too large", "Tiny brush please"],
    "softer": ["Make the brush softer", "Softer edges on the brush", "I want a softer falloff", "Soften the brush tip",
               "Feather the brush edge more", "Less hard brush"],
    "harder": ["Make the brush harder", "Crisper brush edges", "Give the brush a harder edge", "Sharper brush edge",
               "I need a hard-edged brush", "Less soft brush"],
    "flow": ["Lower the brush flow", "I want to build the paint up slowly", "Less flow on the brush", "Make each stroke lighter",
             "Reduce the flow", "Build up gradually"],
    "lazy_on": ["Turn on lazy mouse", "Smooth out my strokes", "Stabilise my brush strokes", "My lines are shaky, steady them",
                "Turn on stroke smoothing", "Add some stroke stabilisation"],
    "lazy_off": ["Turn off lazy mouse", "Stop smoothing my strokes", "No stabiliser", "Turn the stroke smoothing off",
                 "Disable lazy mouse"],
    "island_on": ["Only paint on one UV island at a time", "Turn on island lock", "Keep my strokes on one island",
                  "Lock painting to the island I start on", "Don't let strokes spill onto other islands", "Paint within one island only"],
    "island_off": ["Turn off island lock", "Let strokes cross islands again", "Unlock the island painting", "Stop locking to one island"],
    "through": ["Paint through to the back too", "Let the brush paint the far side as well", "Turn off backface culling",
                "Paint all the way through the model"],
    "no_through": ["Don't paint through to the back", "Stop painting the far side", "Turn on backface culling",
                   "Only paint the side facing me"],
}


def brush_tweak2(sc, rng):
    """brush_tweak with many more ways to ask (round 4 had a few sentences per kind)."""
    e = None
    for _ in range(4):
        e = brush_tweak(sc, rng)
        if e:
            break
    kind = e["variant"]
    lots_words = ("a lot", "Much", "Double", "Halve", "way", "Tiny", "tiny")
    choices = BRUSH_SAY[kind]
    if kind in ("bigger", "smaller"):
        lots = any(w in e["request"] for w in ("a lot", "Much"))
        choices = [c for c in choices if any(w in c for w in lots_words) == lots] or choices
    e["request"] = rng.choice(choices)
    e["task"] = "brush_tweak2"
    return e


BAKE_SAY = ["Bake {m}", "{M} bake please", "Can you bake {m}?", "Generate the {m} map", "I need {m} baked", "Bake me {m}",
            "Do a {m} bake", "Run the {m} bake"]


def bake2(sc, rng):
    maps = rng.choice([["ao"], ["ao"], ["curvature"], ["thickness"], ["ao", "curvature"], ["ao", "curvature", "thickness"]])
    words = {"ao": rng.choice(["AO", "ambient occlusion", "the AO"]), "curvature": rng.choice(["curvature", "the curvature"]),
             "thickness": rng.choice(["thickness", "the thickness"])}
    m = " and ".join(words[x] for x in maps)
    req = rng.choice(BAKE_SAY).replace("{m}", m).replace("{M}", cap(m))
    req = req.replace("the the", "the").replace("a the", "a").replace("the the", "the")
    args = {"ao": "ao" in maps, "curvature": "curvature" in maps, "thickness": "thickness" in maps}
    full = {"ao": "ambient occlusion", "curvature": "curvature", "thickness": "thickness"}
    return ex("bake2", "bake", "+".join(maps), cap(req), {"say": "Bakes " + " and ".join(full[x] for x in maps) + ".",
                                                         "steps": [step("bake_mesh_maps", **args)]}, [{"t": "baked", "maps": maps}])


def clean2(sc, rng):
    e = clean(sc, rng)
    if not e:
        return None
    if e["variant"] == "hide":
        e["request"] = rng.choice(["Hide all the dirt and wear", "Hide the dirt and wear so I can see the base", "Switch off the grunge",
                                   "Turn off all the weathering layers", "Show it without the dirt, rust and wear", "Hide the wear and tear"])
    else:
        e["request"] = rng.choice(["Delete the dirt and wear layers", "Remove the grunge layers", "Throw away all the weathering",
                                   "Delete all the wear and tear"])
    e["task"] = "clean2_" + e["variant"]
    return e


TASKS_R5 = [(brush_tweak2, 6), (bake2, 3), (clean2, 2)]
