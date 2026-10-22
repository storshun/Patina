"""Round 6 (Patina 0.12): more requests the Assistant can set up, and an honest answer when it can't.

New things it does: a texture set of their own for a part (head / body, a UDIM tile), more or
less texture space for a part, the texture resolution and layout, glowing parts, metallic
adjustments, selections, opening the right window (export for Unreal, render, symmetry...),
text to paint through (a text stencil on a paint layer), and questions about the project answered
from what it is shown.

What it can't do (shape, animation, video, artwork, files, uploads, things outside Patina) gets one
sentence on why and up to three close requests it can do ("suggest"), as buttons in the panel.
Randall's own requests from 0.11 ("split the head UVs into their own texture map", "put the
head on a separate uv map", "add the head to a new udim tile", "give the head more texture
space") are among the wordings.
"""
import re

import vocab as V
from tasks import (_ids, _layer_word, _layers, _material, cap, ex, faces_of, new_layer_check, part_name, region_faces,
                   step, usable_parts)


def _bare(pid):
    """'Head' for region:head, 'Boots' for slot:Boots (a set or layer name)."""
    n = re.sub(r"^the ", "", part_name(pid))
    n = re.sub(r" slot$", "", n)
    return cap(n)


def _pn(pid):
    return part_name(pid)


def _named_parts(sc, kinds=("region", "group", "slot")):
    return [p for p in usable_parts(sc, kinds) if p["faces"] >= 2]


def _head(sc):
    return "region:head" if any(p["id"] == "region:head" for p in sc["parts"]) else None


# ============================================================================= texture sets
TEXSET_FORMS = [
    "Give {p} its own texture set", "Split {p} into its own texture map", "Put {p} on a separate UV map",
    "Make a separate texture for {p}", "I want {p} on its own texture", "{P} should have its own texture space",
    "Separate {p}'s texture from the rest", "Move {p} to a texture of its own", "Put {p} in its own texture set",
    "Split the UVs of {p} into their own texture map", "Put {p} on a second UV space", "Give {p} a texture set of its own",
    "Make {p} a separate texture set", "Can {p} get its own texture?", "{P} on its own texture map, please",
    "Make a second UV space and put {p} on it by itself", "I'd like a separate texture just for {p}",
    "Split the head and body textures", "I want the head and the body on different textures",
]
UDIM_FORMS = ["Put {p} on a new UDIM tile", "Add {p} to a new UDIM tile", "Move {p} to its own UDIM", "Give {p} a UDIM tile of its own",
              "Put {p} in a different UDIM"]


def texset(sc, rng):
    head = _head(sc)
    parts = _named_parts(sc, ("region", "group", "slot"))
    if not parts:
        return None
    form = rng.choice(TEXSET_FORMS + UDIM_FORMS[:2])
    two = "head and" in form
    if two:
        if not head:
            return None
        part = head
    elif "{p}" not in form:
        return None
    else:
        weights = [6 if p["id"] == head else 3 if p["id"].startswith("group:") else 2 for p in parts]
        part = rng.choices([p["id"] for p in parts], weights)[0]
    pn = _pn(part)
    req = form.replace("{p}", pn).replace("{P}", cap(pn))
    udim = rng.random() < 0.18 and not part.startswith("slot:")
    pre = [["texture_tiles", {"mode": "udim"}]] if udim else []
    if udim and rng.random() < 0.6:
        req = rng.choice(UDIM_FORMS).replace("{p}", pn).replace("{P}", cap(pn))
    faces = faces_of(sc, part)
    if len(faces) >= 0.97 * sc["faces"]:
        return None
    if part.startswith("slot:"):
        # a material slot is a texture set as soon as the layout is per slot
        if sc.get("texture_sets"):
            sug = ["Give the head its own texture set"] if head else []
            sug += ["Give %s more texture space" % pn, "Make %s %s" % (pn, rng.choice(["leather", "steel", "denim", "wood"]))]
            plan = {"say": "%s has a texture set of its own already: the textures are per material slot." % cap(pn),
                    "suggest": sug[:3]}
            return ex("texset_have", "texset", "slot", req, plan,
                      [{"t": "plan", "kind": "say"}, {"t": "no_change"}, {"t": "suggest", "min": 1}])
        steps = [step("texture_tiles", mode="material_slots")]
        say = "Gives every material slot a texture set of its own, %s among them (this clears the undo history)." % pn
        return ex("texset_slots", "texset", "slot", req, {"say": say, "steps": steps},
                  [{"t": "tiles_mode", "mode": "material_slots"}, {"t": "tile_faces", "faces": faces, "min_jaccard": 0.99}])
    if udim:
        steps = [step("texture_set_from_faces", part=part)]
        say = "Moves %s's islands to a UDIM tile of their own (this clears the undo history)." % pn
        e = ex("texset_udim", "texset", part.split(":")[0], req, {"say": say, "steps": steps},
               [{"t": "tile_faces", "faces": faces, "min_jaccard": 0.9}, {"t": "tiles_mode", "mode": "udim"}])
        e["pre"] = pre
        return e
    name = _bare(part)
    if any(t.lower() == name.lower() for t in [p["id"].split(":", 1)[1] for p in sc["parts"] if p["id"].startswith("slot:")]):
        name += " set"
    steps = [step("texture_set_from_faces", name=name, part=part)]
    say = "Gives %s a texture set of its own called %s, packed to fill it (this clears the undo history)." % (pn, name)
    return ex("texset_new", "texset", part.split(":")[0], req, {"say": say, "steps": steps},
              [{"t": "tile_faces", "faces": faces, "min_jaccard": 0.9, "name": name}])


# ============================================================================= texture space for a part
DENSITY_MORE = [("more", 2.0), ("a bit more", 1.5), ("much more", 3.0), ("twice the", 2.0), ("double the", 2.0),
                ("three times the", 3.0), ("more", 2.0)]
DENSITY_LESS = [("less", 0.5), ("a bit less", 0.75), ("half the", 0.5)]
DENSITY_FORMS = ["Give {p} {a} texture space", "Give {p} {a} UV space", "Give {p} {a} room in the UV layout",
                 "{P} needs {a} texture space", "Give {p} {a} texel density", "Give {p} {a} texture resolution"]
DENSITY_FIXED = [("Make {p}'s islands bigger in the UV map", 2.0), ("Increase the texel density of {p}", 2.0),
                 ("{P} should take up more of the texture", 2.0), ("Double the texel density of {p}", 2.0),
                 ("{P} looks blurry, give it more pixels", 2.0), ("{P} takes too much texture space", 0.5),
                 ("Shrink {p}'s share of the texture", 0.5), ("Make {p}'s UV islands smaller", 0.5),
                 ("Give the face more texture detail than the rest", 2.0)]


def density(sc, rng):
    parts = _named_parts(sc, ("region", "group", "slot"))
    head = _head(sc)
    if not parts:
        return None
    weights = [6 if p["id"] == head else 2 for p in parts]
    part = rng.choices([p["id"] for p in parts], weights)[0]
    faces = faces_of(sc, part)
    if len(faces) >= 0.8 * sc["faces"] or len(faces) < 4:
        return None
    pn = _pn(part)
    if rng.random() < 0.35:
        form, k = rng.choice(DENSITY_FIXED)
        if "the face" in form:
            if part != head:
                return None
            pn = "the face"
        req = form.replace("{p}", pn).replace("{P}", cap(pn))
    else:
        words, k = rng.choice(DENSITY_MORE if rng.random() < 0.75 else DENSITY_LESS)
        req = rng.choice(DENSITY_FORMS).replace("{p}", pn).replace("{P}", cap(pn)).replace("{a}", words)
    slot_same = [q["id"] for q in sc["parts"] if q["id"].startswith("slot:") and sorted(faces_of(sc, q["id"])) == sorted(faces)]
    if sc.get("texture_sets") and slot_same:
        # a texture set of its own already fills its square: only a higher resolution adds detail
        if k < 1:
            return None
        plan = {"say": "%s has a texture set of its own and fills it already; a higher texture resolution gives it more detail." % cap(pn),
                "suggest": ["Set the texture resolution to 2048", "Set the texture resolution to 4096"]}
        return ex("density_own_set", "uv", "slot", req, plan, [{"t": "plan", "kind": "say"}, {"t": "no_change"}, {"t": "suggest", "min": 1}])
    steps = [step("select_part", part=part), step("uv_density", scale=k, selection=True)]
    say = "%s and repacks the UVs." % ("Gives %s %s the texture detail" % (_pn(part), {1.5: "1.5 times", 2.0: "twice", 3.0: "three times",
                                                                                    0.5: "half", 0.75: "three quarters of"}[k]))
    gain = {"min": 1 + (k - 1) * 0.6} if k > 1 else {"max": 1 - (1 - k) * 0.6}
    check = [dict({"t": "density_gain", "faces": faces}, **gain), {"t": "uv_changed"}, {"t": "history", "delta": 1}]
    return ex("density_" + ("more" if k > 1 else "less"), "uv", part.split(":")[0], req, {"say": say, "steps": steps}, check)


# ============================================================================= resolution and layout
RES_FORMS = ["Set the texture resolution to {r}", "Make the textures {r}", "Use {r} textures", "I want {r} textures",
             "Change the texture size to {r}", "Bump the resolution up to {r}", "Textures at {r}, please",
             "Make the texture set {r}"]
RES_WORDS = {1024: ["1024", "1K", "1k"], 2048: ["2048", "2K", "2k"], 4096: ["4096", "4K", "4k"]}


def resolution(sc, rng):
    if rng.random() < 0.2:
        r = 1024
        req = rng.choice(["Double the texture resolution", "Make the textures twice as sharp", "Higher resolution textures, twice the size"])
    else:
        r = rng.choice([1024, 2048, 4096, 2048])
        req = rng.choice(RES_FORMS).replace("{r}", rng.choice(RES_WORDS[r]))
    if r == 4096 and sc.get("texture_sets") and len([p for p in sc["parts"] if p["id"].startswith("slot:")]) > 4:
        return None  # 3 x 3 sets at 4K: past the 16K atlas
    steps = [step("set_resolution", resolution=r)]
    say = "Sets the textures to %d px (this clears the undo history)." % r
    return ex("resolution", "texset", str(r), req, {"say": say, "steps": steps}, [{"t": "state", "path": "resolution", "eq": r}])


def layout_mode(sc, rng):
    slots = [p for p in sc["parts"] if p["id"].startswith("slot:")]
    if sc.get("texture_sets"):
        req = rng.choice(["Use one texture for everything", "Put everything back on one texture", "Turn off texture sets",
                          "No texture sets, just one texture", "Merge the texture sets into one"])
        mode, say = "single", "Puts the whole model back on one texture (this clears the undo history)."
    elif len(slots) >= 2:
        req = rng.choice(["Give every material its own texture set", "Use a texture set per material slot", "Turn on texture sets",
                          "One texture per material, please", "Split the textures by material"])
        mode, say = "material_slots", "Gives every material slot a texture set of its own (this clears the undo history)."
    else:
        return None
    return ex("layout_" + mode, "texset", mode, req, {"say": say, "steps": [step("texture_tiles", mode=mode)]},
              [{"t": "tiles_mode", "mode": mode}])


# ============================================================================= glow, metallic
GLOW_FORMS = ["Make {p} glow {c}", "Add a {c} glow to {p}", "{P} should glow {c}", "Make {p} emissive {c}", "Light up {p} in {c}",
              "Give {p} a {c} emission", "I want {p} glowing {c}", "Make {p} give off a {c} light", "{C} glowing {p}"]
GLOW_COLOURS = ["red", "orange", "amber", "yellow", "green", "lime", "cyan", "teal", "blue", "light blue", "purple", "violet",
                "magenta", "pink", "white"]


def glow(sc, rng):
    parts = _named_parts(sc, ("region", "group", "slot", "piece"))
    part = rng.choice(parts)["id"] if parts and rng.random() < 0.85 else None
    cname, rgb = V.colour(rng, GLOW_COLOURS)
    pn = _pn(part) if part else rng.choice(["the whole model", "everything", "it"])
    req = rng.choice(GLOW_FORMS).replace("{p}", pn).replace("{P}", cap(pn)).replace("{c}", cname).replace("{C}", cap(cname))
    strength = 1.0
    if rng.random() < 0.3:
        cls, aword, strength = V.amount(rng)
        req = req + ", " + aword
    args = {"color": rgb, "glow": True, "name": "%s glow" % cap(cname)}
    if strength != 1.0:
        args["strength"] = strength
    if part:
        args["region"] = {"part": part}
    expect = {"type": "fill", "channel_mask": 32, "emissive": {"rgb": rgb, "tol": 0.3}, "region": faces_of(sc, part) if part else "all"}
    say = "Makes %s glow %s." % (pn if part else "the model", cname)
    return ex("glow", "tint", "part" if part else "all", req.strip(), {"say": say, "steps": [step("add_tint_layer", **args)]},
              new_layer_check(expect))


def metal(sc, rng):
    parts = _named_parts(sc)
    part = rng.choice(parts)["id"] if parts and rng.random() < 0.8 else None
    more = rng.random() < 0.6
    cls, aword, _ = V.amount(rng)
    v = {"slight": 0.15, "bit": 0.3, "noticeable": 0.45, "strong": 0.65}[cls] * (1 if more else -1)
    pn = _pn(part) if part else rng.choice(["the whole model", "everything"])
    req = rng.choice(["Make {p} {a} more metallic", "{P} should look {a} more like metal", "Add {a} more metalness to {p}"] if more else
                     ["Make {p} {a} less metallic", "{P} looks too metallic, tone it down {a}", "Take {a} of the metalness off {p}"])
    req = req.replace("{p}", pn).replace("{P}", cap(pn)).replace("{a}", aword).replace("  ", " ")
    steps = [step("add_layer", type="adjust", name="More metallic" if more else "Less metallic", adjust={"metallic": v})]
    expect = {"type": "adjust", "adjust": {"metallic": [0.04, 1.0] if more else [-1.0, -0.04]}, "region": faces_of(sc, part) if part else "all"}
    if part:
        steps.append(step("update_layer_mask", index="$0.index", region={"part": part}))
    say = "Adds an adjustment layer that makes %s %s metallic." % (pn if part else "the model", "more" if more else "less")
    return ex("adjust_metal", "adjust", "part" if part else "all", req, {"say": say, "steps": steps}, new_layer_check(expect))


# ============================================================================= selection
def select2(sc, rng):
    parts = _named_parts(sc, ("region", "group", "slot", "piece"))
    if not parts:
        return None
    allf = faces_of(sc, "all")
    kind = rng.choice(["part", "part", "add", "remove", "all", "none", "invert"])
    p = rng.choice(parts)["id"]
    pn = _pn(p)
    pre = []
    if kind == "part":
        req = rng.choice(["Select {p}", "Select all the {q} faces", "Select every face of {p}", "Pick {p}", "Select {p} for me"])
        req = req.replace("{p}", pn).replace("{q}", re.sub(r"^the ", "", pn))
        steps, want = [step("select_part", part=p)], faces_of(sc, p)
        say = "Selects %s." % pn
    elif kind == "add":
        others = [x["id"] for x in parts if x["id"] != p]
        if not others:
            return None
        q = rng.choice(others)
        pre = [["select_part", {"part": q}]]
        req = rng.choice(["Also select {p}", "Add {p} to the selection", "Select {p} too"]).replace("{p}", pn)
        steps, want = [step("select_part", mode="add", part=p)], region_faces(sc, [q, p])
        say = "Adds %s to the selection." % pn
    elif kind == "remove":
        pre = [["select_faces", {"mode": "all"}]]
        req = rng.choice(["Deselect {p}", "Take {p} out of the selection", "Everything but {p} selected"]).replace("{p}", pn)
        steps, want = [step("select_part", mode="remove", part=p)], sorted(set(allf) - set(faces_of(sc, p)))
        say = "Takes %s out of the selection." % pn
    elif kind == "all":
        req = rng.choice(["Select everything", "Select all the faces", "Select the whole model"])
        steps, want = [step("select_faces", mode="all")], allf
        say = "Selects every face."
    elif kind == "none":
        pre = [["select_part", {"part": p}]]
        req = rng.choice(["Deselect everything", "Clear the selection", "Select nothing", "Drop the selection"])
        steps, want = [step("select_faces", mode="clear")], []
        say = "Clears the selection."
    else:
        pre = [["select_part", {"part": p}]]
        req = rng.choice(["Invert the selection", "Select everything else", "Flip the selection"])
        steps, want = [step("select_faces", mode="invert")], sorted(set(allf) - set(faces_of(sc, p)))
        say = "Inverts the selection."
    if kind != "none" and not want:
        return None
    e = ex("select_" + kind, "selection", kind, req, {"say": say, "steps": steps},
           [{"t": "selection", "faces": want, "min_jaccard": 0.99}])
    e["pre"] = pre
    return e


# ============================================================================= windows
WINDOWS = [
    ("export", "unreal", ["Export the textures for Unreal", "Export for Unreal Engine", "Get the textures ready for UE5",
                          "I need the maps for Unreal"], "Opens the export window set up for Unreal: choose a folder and press Export."),
    ("export", "unity", ["Export the textures for Unity", "Export for Unity", "I need the maps for Unity"],
     "Opens the export window set up for Unity: choose a folder and press Export."),
    ("export", "gltf", ["Export the textures for glTF", "Export glTF maps", "I need glTF textures"],
     "Opens the export window set up for glTF: choose a folder and press Export."),
    ("export", "blender", ["Export the textures for Blender", "Export the maps for Blender's Principled BSDF"],
     "Opens the export window set up for Blender: choose a folder and press Export."),
    ("export", None, ["Export the textures", "Save the texture maps out", "Export my maps"],
     "Opens the export window: choose the preset and the folder, then press Export."),
    ("render", None, ["Render an image of the model", "Take a nice picture of it", "Make a render of this", "Render a turntable"],
     "Opens the render window: pick the size and press Render."),
    ("texel_density", None, ["Open the texel density window", "Check the texel density", "Show me the texel density numbers"],
     "Opens the texel density window."),
    ("symmetry", None, ["Check if the mesh is symmetric", "Run the symmetry check", "Is the model symmetrical? Check it"],
     "Opens the mesh symmetry check."),
    ("high_poly", None, ["Bake from my high poly", "Bake normals from the sculpt", "I want to bake my high-poly mesh"],
     "Opens the high-poly bake window: choose the high-poly mesh there."),
    ("blender", None, ["Send the textures to Blender", "Update Blender with my textures", "Send it back to Blender"],
     "Opens the Blender link: press Send textures to Blender."),
    ("preferences", None, ["Open the preferences", "Change the autosave interval", "Where are the settings?"],
     "Opens the preferences."),
    ("shortcuts", None, ["Show me the keyboard shortcuts", "What are the hotkeys?", "Open the shortcut list"],
     "Opens the keyboard shortcuts."),
    ("macros", None, ["Record a macro", "I want to record what I do and replay it", "Open the macro recorder"],
     "Opens the macro recorder."),
    ("new_texture_set", None, ["Open the new texture set window", "I want to make a texture set myself"],
     "Opens the new texture set window: select the faces and name the set there."),
]


def window(sc, rng):
    w, preset, forms, say = rng.choice(WINDOWS)
    args = {"window": w}
    if preset:
        args["preset"] = preset
    return ex("window_" + w, "window", preset or w, rng.choice(forms), {"say": say, "steps": [step("open_window", **args)]},
              [{"t": "state", "path": "last_window", "eq": w}])


# ============================================================================= text to paint through
TEXTS = ["PATINA", "Hero", "No. 7", "DANGER", "Mai", "Property of the Guild", "LOVE", "Team Red", "07", "Handle with care",
         "ACME", "Kiri", "Fragile", "Rogue One", "XIII", "Hello", "Captain", "Sector 9", "WARNING", "Lucky"]
TEXT_FORMS = ["Write '{t}' on {p}", "Put the text {t} on {p}", "Add the word {t} to {p}", "Stencil '{t}' onto {p}",
              "I want to paint {t} on {p}", "Letter {t} on {p} in {c}", "Write {t} in {c} on {p}", "Paint the text '{t}' on {p}"]


def text_stencil(sc, rng):
    parts = _named_parts(sc, ("region", "group", "slot", "piece"))
    if not parts:
        return None
    part = rng.choice(parts)["id"]
    t = rng.choice(TEXTS)
    cname, rgb = V.colour(rng, ["white", "black", "red", "gold", "yellow", "blue", "cream"])
    form = rng.choice(TEXT_FORMS)
    pn = _pn(part)
    req = form.replace("{t}", t).replace("{p}", pn).replace("{c}", cname)
    if "{c}" not in form:
        cname, rgb = "white", [1.0, 1.0, 1.0]
    steps = [step("add_layer", name="Text %s" % t), step("update_layer_mask", index="$0.index", region={"part": part}),
             step("set_stencil", color=rgb, enabled=True, fit=True, mode="decal", text=t), step("set_tool", tool="paint")]
    say = "Puts the text '%s' over the view as a %s stencil, with a paint layer on %s: paint over the letters." % (t, cname, pn)
    check = [{"t": "layer", "which": "new", "expect": {"type": "paint", "region": faces_of(sc, part)}},
             {"t": "state", "path": "stencil.has_image", "eq": True}, {"t": "state", "path": "stencil.enabled", "eq": True},
             {"t": "state", "path": "tool", "eq": "paint"}]
    return ex("text_stencil", "paint", "part", req, {"say": say, "steps": steps}, check)


# ============================================================================= questions about the project
def question(sc, rng):
    kind = rng.choice(["faces", "parts", "layers", "resolution", "sets", "head"])
    plan_check = [{"t": "plan", "kind": "say"}, {"t": "no_change"}]
    n = sc["faces"]
    if kind == "faces":
        req = rng.choice(["How many faces does the model have?", "What's the poly count?", "How many polygons is this model?",
                          "How heavy is this mesh?"])
        say = "It has %d faces." % n
        return ex("question_faces", "question", kind, req, {"say": say}, plan_check + [{"t": "say_contains", "any": [str(n), "{:,}".format(n)]}])
    if kind == "parts":
        names = [_pn(p["id"]) for p in usable_parts(sc, ("slot", "group", "region"))][:8]
        if not names:
            return None
        req = rng.choice(["What parts can you see?", "Which parts does the model have?", "What can I call the parts?",
                          "List the parts of the model"])
        say = "I can see %s." % (", ".join(names[:-1]) + " and " + names[-1] if len(names) > 1 else names[0])
        return ex("question_parts", "question", kind, req, {"say": say}, plan_check + [{"t": "say_contains", "any": [names[0], names[-1]]}])
    if kind == "layers":
        ls = sorted(sc["layers"], key=lambda l: -l["index"])
        if not ls:
            return None
        names = [l["name"] for l in ls][:8]
        req = rng.choice(["What layers are there?", "List my layers", "Which layers do I have?"])
        say = "From the top: %s." % ", ".join(names)
        return ex("question_layers", "question", kind, req, {"say": say}, plan_check + [{"t": "say_contains", "any": names[:2]}])
    if kind == "resolution":
        req = rng.choice(["What resolution are the textures?", "How big are the textures?", "What's the texture size?"])
        say = "The textures are 512 px%s." % (" per texture set" if sc.get("texture_sets") else "")
        return ex("question_resolution", "question", kind, req, {"say": say}, plan_check + [{"t": "say_contains", "any": ["512"]}])
    if kind == "sets":
        req = rng.choice(["Does this model have texture sets?", "How many texture sets are there?", "Is everything on one texture?"])
        slots = [_bare(p["id"]) for p in sc["parts"] if p["id"].startswith("slot:")]
        if sc.get("texture_sets"):
            say = "Yes: a texture set per material slot (%s)." % ", ".join(slots)
            words = ["yes", "per material", slots[0] if slots else "set"]
        else:
            say = "No: the whole model is on one texture."
            words = ["one texture", "no"]
        return ex("question_sets", "question", kind, req, {"say": say}, plan_check + [{"t": "say_contains", "any": words}])
    req = rng.choice(["Did you find a head?", "Can you tell where the head is?", "Does it have a head?"])
    if sc["head_found"]:
        say = "Yes: the head is the part above the neck (region:head)."
        words = ["yes", "above the neck"]
    else:
        say = "No: I couldn't find a neck on this model, so there is no head part."
        words = ["no", "couldn't", "not"]
    return ex("question_head", "question", kind, req, {"say": say}, plan_check + [{"t": "say_contains", "any": words}])


# ============================================================================= can't, with suggestions
MATS = ["leather", "steel", "denim", "wood", "gold", "rubber", "brushed steel", "linen", "brass", "velvet"]


def _sugg_pool(sc, rng, pn=None):
    """Requests the Assistant can do on this model, about the part asked about when there is one."""
    parts = _named_parts(sc)
    p = pn or (_pn(rng.choice(parts)["id"]) if parts else None)
    pool = []
    if p:
        pool += ["Make %s %s" % (p, rng.choice(MATS)), "Set me up to paint on %s by hand" % p, "Add worn edges to %s" % p,
                 "Make %s look weathered" % p, "Add dirt in the creases of %s" % p, "Make %s a bit darker" % p]
    else:
        pool += ["Make it look weathered", "Add dirt in the creases", "Make it brushed steel", "Wear the paint off the edges"]
    rng.shuffle(pool)
    return pool


# (request forms, say, how to suggest): {p} a part name, {t} a thing
CANT = [
    ("shape", ["Make {p} bigger", "Make {p} longer", "Make {p} thicker", "Make {p} rounder", "Bevel the edges of {p}",
               "Add more polygons to {p}", "Subdivide {p}", "Cut a hole in {p}", "Remove {p} from the model",
               "Smooth out {p}", "Make the character taller", "Fix the topology", "Merge the vertices", "Make the sword sharper"],
     "Patina paints the model but doesn't change its shape; do that in Blender and send it again.", "paint"),
    ("add_object", ["Add a {t} to {p}", "Give {p} a {t}", "Put a {t} on {p}", "Add {t}s to {p}"],
     "Patina can't add objects or geometry; model the {t} in Blender, or paint one on.", "paint_thing"),
    ("sculpt", ["Sculpt wrinkles into {p}", "Sculpt more muscle definition", "Add sculpted scales to {p}", "Carve a pattern into {p}"],
     "Patina doesn't sculpt; it can fake the detail with paint, or bake it from a high-poly model.", "sculpt"),
    ("animate", ["Animate {p}", "Make {p} wave", "Rig the character", "Pose her arms down", "Make the cape flow in the wind",
                 "Add physics to the hair", "Make it walk", "Make the eyes blink", "Make the hair move"],
     "Patina doesn't animate, rig or pose models; do that in Blender.", "paint"),
    ("video", ["Render a video", "Make a turntable video", "Record a timelapse of my painting", "Render an animation"],
     "Patina renders still images (and turntable frames), not video.", "render"),
    ("artwork", ["Paint a dragon on {p} for me", "Draw a portrait on {p}", "Paint my face on {p}", "Draw a landscape on {p}",
                 "Paint a detailed tattoo on {p} for me", "Draw flowers all over {p}"],
     "I don't paint artwork myself; I can set {p} up for you to paint it by hand.", "paint_by_hand"),
    ("ai_image", ["Generate a texture of {t}", "Make an AI texture of {t}", "Turn this photo into a material",
                  "Make a texture from this picture", "Create a {t} texture from scratch"],
     "I don't make images; Patina's library has materials, and Materials > Import Image as Material turns a photo into one.", "material"),
    ("files", ["Save the project", "Open my last project", "Import a new model", "Save a copy to my desktop", "Load my other file"],
     "Saving and opening files is up to you: File > Save (Ctrl+S), File > Open.", None),
    ("export_mesh", ["Export the model as FBX", "Export the mesh to OBJ", "Save the model as glTF"],
     "Patina exports textures, not the model; export the mesh from Blender.", "export"),
    ("share", ["Upload this to Sketchfab", "Email the textures to my friend", "Post it online", "Put it on the asset store"],
     "Patina doesn't upload or send anything.", "export"),
    ("offtopic", ["Tell me a joke", "What's the weather like?", "Write me a poem", "Who made you?", "What's 2 plus 2?",
                  "Translate this into French"],
     "I only change this project in Patina: materials, layers, masks, UVs, bakes and the view.", "examples"),
    ("app", ["Change the theme to light mode", "Make the interface bigger", "Turn on dark mode", "Update Patina"],
     "That is in the preferences (the UI scale is there); I change the project, not Patina's settings.", "prefs"),
    ("uv2", ["Add a second UV channel", "Make lightmap UVs", "Generate UV2 for Unreal", "Copy the UVs to another UV map"],
     "Patina keeps one UV map per model; it can give a part a texture set of its own, or more texture space.", "uv"),
    ("vertex_colours", ["Bake the textures into vertex colours", "Paint vertex colours on {p}", "Use vertex colours instead"],
     "Patina paints textures, not vertex colours.", "bake"),
    ("tileable", ["Make a seamless tiling texture", "Make the texture tileable", "Make a trim sheet"],
     "Patina paints textures for this model's UVs; it doesn't make tiling textures or trim sheets.", "material"),
    ("hair", ["Groom the hair", "Add hair strands", "Make the hair fluffier", "Add fur to {p}"],
     "Patina doesn't make hair or fur geometry; it paints the textures.", "paint"),
    ("engine", ["Make it look good in Unreal", "Set up the material in Unity", "Make the shader in Unreal", "Set up subsurface scattering"],
     "Patina makes the textures; the material and shader set-up happen in the engine.", "export"),
    ("undo", ["Undo that", "Redo", "Go back three steps", "Undo the last thing"],
     "Press Ctrl+Z to undo the last step (Ctrl+Shift+Z redoes it); History shows every step.", None),
]
THINGS = ["shield", "hat", "sword", "belt", "horn", "spike", "button", "pocket", "zip", "badge", "scar"]
ART_THINGS = ["dragon scales", "lava", "marble veins", "a galaxy", "cracked earth", "tree bark"]


def _suggestions(kind, sc, rng, part, thing):
    pn = _pn(part) if part else None
    head = _head(sc)
    pool = _sugg_pool(sc, rng, pn)
    if kind == "paint":
        out = pool[:3]
    elif kind == "paint_thing":
        out = ["Set me up to paint a %s on %s by hand" % (thing, pn or "the model")] + pool[:2]
    elif kind == "sculpt":
        out = ["Bake from my high poly", "Set me up to paint on %s by hand" % (pn or "the model"), "Add dirt in the creases of %s" % (pn or "the model")]
    elif kind == "render":
        out = ["Render an image of the model", "Show the model from the front"]
    elif kind == "paint_by_hand":
        out = ["Set me up to paint on %s by hand" % (pn or "the model"), "Write 'Hero' on %s" % (pn or "the model")] + pool[:1]
    elif kind == "material":
        out = ["Make %s %s" % (pn or "the model", rng.choice(["dragon scales", "marble", "leather", "brushed steel"]))] + pool[:1]
    elif kind == "export":
        out = ["Export the textures for Unreal", "Export the textures"]
    elif kind == "examples":
        out = pool[:3]
    elif kind == "prefs":
        out = ["Open the preferences"]
    elif kind == "uv":
        p = head or part
        out = ["Give %s its own texture set" % _pn(p), "Give %s more texture space" % _pn(p)] if p else ["Pack the UVs", "Unwrap the UVs again"]
    elif kind == "bake":
        out = ["Bake ambient occlusion", "Set me up to paint on %s by hand" % (pn or "the model")]
    else:
        out = []
    seen, uniq = set(), []
    for s in out:
        if s.lower() not in seen:
            seen.add(s.lower())
            uniq.append(s)
    return uniq[:3]


def cant3(sc, rng):
    kind, forms, say, sug = rng.choice(CANT)
    form = rng.choice(forms)
    parts = _named_parts(sc)
    part = rng.choice(parts)["id"] if parts else None
    if "{p}" in form and not part:
        return None
    thing = rng.choice(THINGS if kind != "ai_image" else ART_THINGS)
    pn = _pn(part) if part else "the model"
    req = form.replace("{p}", pn).replace("{t}", thing)
    say_t = say.replace("{p}", pn).replace("{t}", thing)
    plan = {"say": say_t}
    if sug:
        s = _suggestions(sug, sc, rng, part if "{p}" in form or sug in ("paint_by_hand",) else None, thing)
        if s:
            plan["suggest"] = s
    check = [{"t": "plan", "kind": "say"}, {"t": "no_change"}] + ([{"t": "suggest", "min": 1}] if "suggest" in plan else [])
    return ex("cant3_" + kind, "none", kind, cap(req), plan, check)


def help_me(sc, rng):
    req = rng.choice(["What can you do?", "Help", "How do I use you?", "What kind of things can I ask?", "Give me some ideas"])
    say = ("I set things up for you: whole looks (weathered, rusty, muddy...), materials and tints on parts, wear and dirt, "
           "glowing parts, layers to paint on by hand or through text, texture sets and texture space, UVs, bakes, the brush "
           "and the view. Some ideas for this model:")
    plan = {"say": say, "suggest": _sugg_pool(sc, rng)[:3]}
    return ex("help", "none", "help", req, plan, [{"t": "plan", "kind": "say"}, {"t": "no_change"}, {"t": "suggest", "min": 2}])


VAGUE2 = ["Make it look better", "Fix it", "Improve the textures", "Make it pop", "Do something cool", "Make it nicer",
          "It looks wrong", "Make it more realistic", "Make it look good", "It looks boring", "Spice it up"]


def vague2(sc, rng):
    req = rng.choice(VAGUE2)
    ask = rng.choice(["What would you like to change? For example:", "Which part should change, and how? Some ideas:",
                      "Could you say a bit more? I could:"])
    plan = {"ask": ask, "suggest": _sugg_pool(sc, rng)[:3]}
    return ex("vague2", "none", "ask", req, plan, [{"t": "plan", "kind": "ask"}, {"t": "no_change"}, {"t": "suggest", "min": 2}])


TASKS_R6 = [
    (texset, 14), (density, 8), (resolution, 4), (layout_mode, 3), (glow, 6), (metal, 3), (select2, 6), (window, 7),
    (text_stencil, 5), (question, 6), (cant3, 18), (help_me, 2), (vague2, 3),
]


# ============================================================================= round 7: the weak spots of round 6
# Painting by hand something it would refuse to paint for you ("a tattoo ... myself"), two parts
# selected at once, more vague wordings, a render (the window) against a video (can't), second UV
# channels (can't), and "shiny" meaning roughness, not metal.
ART = [("a tattoo", "black", "ink", "Tattoo"), ("a dragon", "black", "ink", "Dragon"), ("flowers", "pink", "ink", "Flowers"),
       ("a logo", "white", "ink", "Logo"), ("tribal patterns", "black", "ink", "Pattern"), ("war paint", "red", "airbrush", "War paint"),
       ("stripes", "white", "ink", "Stripes"), ("a face", "black", "pencil", "Face"), ("graffiti", "magenta", "spray", "Graffiti"),
       ("scars", "dark red", "pencil", "Scars")]


def paint_art(sc, rng):
    parts = _named_parts(sc)
    if not parts:
        return None
    part = rng.choice(parts)["id"]
    art, cname, preset, lname = rng.choice(ART)
    pn = _pn(part)
    req = rng.choice(["I want to paint {a} on {p} myself", "Let me paint {a} on {p}", "I'll draw {a} on {p} by hand, set me up",
                      "Get me ready to paint {a} on {p}", "I'm going to paint {a} on {p}", "Set up a layer so I can paint {a} on {p}"])
    req = req.replace("{a}", art).replace("{p}", pn)
    rgb = [round(c, 2) for c in V.COLOURS[cname]]
    steps = [step("add_layer", name=lname), step("update_layer_mask", index="$0.index", region={"part": part}),
             step("set_paint_material", color=rgb, id=""), step("set_brush", preset=preset), step("set_tool", tool="paint")]
    check = [{"t": "layer", "which": "new", "expect": {"type": "paint", "region": faces_of(sc, part)}},
             {"t": "state", "path": "tool", "eq": "paint"}, {"t": "history", "delta": 1}]
    say = "Adds a paint layer for %s that only shows on %s, with %s paint and the %s brush ready." % (art, pn, cname, preset.replace("_", " "))
    return ex("paint_art", "paint", "part", cap(req), {"say": say, "steps": steps}, check)


def select_two(sc, rng):
    parts = _named_parts(sc, ("region", "group", "slot"))
    if len(parts) < 2:
        return None
    a, b = rng.sample([p["id"] for p in parts], 2)
    want = region_faces(sc, [a, b])
    if len(want) >= sc["faces"]:
        return None
    req = rng.choice(["Select {a} and {b}", "Select both {a} and {b}", "Pick {a} and {b}", "Select {a} as well as {b}"])
    req = req.replace("{a}", _pn(a)).replace("{b}", _pn(b))
    steps = [step("select_part", part=a), step("select_part", mode="add", part=b)]
    return ex("select_two", "selection", "two", cap(req), {"say": "Selects %s and %s." % (_pn(a), _pn(b)), "steps": steps},
              [{"t": "selection", "faces": want, "min_jaccard": 0.99}])


VAGUE3 = ["Spice it up a bit", "Jazz it up", "Make it awesome", "Make it more interesting", "Give it some character",
          "Make it look professional", "Make it look cool", "Make it less boring", "Do your magic", "Make it look finished",
          "Can you make it look good?", "It needs something"]


def vague3(sc, rng):
    req = rng.choice(VAGUE3)
    plan = {"ask": rng.choice(["What would you like to change? For example:", "Which part should change, and how? Some ideas:"]),
            "suggest": _sugg_pool(sc, rng)[:3]}
    return ex("vague2", "none", "ask", req, plan, [{"t": "plan", "kind": "ask"}, {"t": "no_change"}, {"t": "suggest", "min": 2}])


def render_or_video(sc, rng):
    if rng.random() < 0.6:
        req = rng.choice(["I'd like a render of it", "Render it", "Render her", "Make a render", "Render this out",
                          "Can I get a render?", "Render a picture of the model", "A nice render please"])
        return ex("window_render", "window", "render", req,
                  {"say": "Opens the render window: pick the size and press Render.", "steps": [step("open_window", window="render")]},
                  [{"t": "state", "path": "last_window", "eq": "render"}])
    req = rng.choice(["Render a video of it", "Make an animation of it spinning", "Render a timelapse", "Make a video turntable",
                      "Render an mp4", "Export a video"])
    plan = {"say": "Patina renders still images (and turntable frames), not video.", "suggest": ["Render an image of the model"]}
    return ex("cant3_video", "none", "video", req, plan, [{"t": "plan", "kind": "say"}, {"t": "no_change"}, {"t": "suggest", "min": 1}])


def uv2(sc, rng):
    head = _head(sc)
    req = rng.choice(["Add a second uv channel for lightmaps", "Make a lightmap UV set", "Generate UV2", "I need a second UV map",
                      "Add another UV map for baking lightmaps", "Make UV channel 2 for Unreal lightmaps", "Create lightmap UVs"])
    p = head or (rng.choice(_named_parts(sc))["id"] if _named_parts(sc) else None)
    sug = (["Give %s its own texture set" % _pn(p), "Give %s more texture space" % _pn(p)] if p else ["Pack the UVs"])
    plan = {"say": "Patina keeps one UV map per model; it can give a part a texture set of its own, or more texture space.",
            "suggest": sug}
    return ex("cant3_uv2", "none", "uv2", req, plan, [{"t": "plan", "kind": "say"}, {"t": "no_change"}, {"t": "suggest", "min": 1}])


SHINY = {"rougher": ["Tone down the shininess of {p}", "{P} is too shiny", "Make {p} less shiny", "{P} looks too glossy",
                     "Take the shine off {p}", "Make {p} more matte"],
         "glossier": ["Make {p} shinier", "{P} should be glossier", "Make {p} shine more", "Add more shine to {p}", "{P} looks too dull"]}


def shiny(sc, rng):
    kind = rng.choice(["rougher", "glossier"])
    parts = _named_parts(sc)
    part = rng.choice(parts)["id"] if parts and rng.random() < 0.8 else None
    pn = _pn(part) if part else "the whole model"
    req = rng.choice(SHINY[kind]).replace("{p}", pn).replace("{P}", cap(pn))
    v = 0.2 if kind == "rougher" else -0.2
    steps = [step("add_layer", type="adjust", name=cap(kind), adjust={"roughness": v})]
    if part:
        steps.append(step("update_layer_mask", index="$0.index", region={"part": part}))
    expect = {"type": "adjust", "adjust": {"roughness": [0.04, 1.0] if v > 0 else [-1.0, -0.04]}, "region": faces_of(sc, part) if part else "all"}
    say = "Adds an adjustment layer that makes %s %s." % (pn if part else "the model", kind)
    return ex("adjust_" + kind, "adjust", "part" if part else "all", req, {"say": say, "steps": steps}, new_layer_check(expect))


TASKS_R7 = [(paint_art, 6), (select_two, 4), (vague3, 3), (render_or_video, 4), (uv2, 3), (shiny, 6), (question, 6),
            (layout_mode, 3), (texset, 4)]
