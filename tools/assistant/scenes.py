"""The scenes the Assistant's training requests run against: primitives, Suzanne, the bust from
scripts/manual_scenes.py, generated characters and props. Each is saved as a .patina project in
~/patina-ml/scenes/ with a few existing layers, plus a <id>.json describing it.

    python3 tools/assistant/scenes.py            # build all
    python3 tools/assistant/scenes.py --list     # show what would be built
"""
import argparse
import json
import os
import random
import sys

sys.path.insert(0, os.path.dirname(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "scripts"))
import common  # noqa: E402
import meshgen  # noqa: E402

# Scenes held out of training entirely (the test set's unseen scenes).
HELD_OUT = {"char_game_A", "figure_one_slot", "prop_shield", "char_knight_T_nogroups", "bust_renamed"}

EXISTING_LAYERS = [
    {"type": "fill", "name": "Base", "material": {"id": "patina_flat_grey"}},
    {"type": "fill", "name": "Base colour", "material": {"id": "patina_plastic_satin_grey"}},
    {"type": "fill", "name": "Dirt", "material": {"id": "patina_dirt_mud"}, "generator": {"type": "cavity"}},
    {"type": "fill", "name": "Edge wear", "material": {"id": "patina_steel_scratched"}, "generator": {"type": "edges"}},
    {"type": "fill", "name": "Dust", "material": {"id": "patina_dust"}, "generator": {"type": "facing"}},
    {"type": "paint", "name": "Details"},
    {"type": "paint", "name": "Hand painted"},
    {"type": "paint", "name": "Layer 3"},
    {"type": "fill", "name": "Rust", "material": {"id": "patina_rust_light"}, "generator": {"type": "occlusion"}},
    {"type": "adjust", "name": "Colour tweak", "adjust": {"saturation": 0.9}},
    {"type": "fill", "name": "Grime", "material": {"id": "patina_grime_oily"}, "generator": {"type": "cavity"}},
]


def scene_specs():
    """(id, kind, builder) for every scene."""
    specs = []
    for p in ("sphere", "cube", "torus", "plane"):
        specs.append(("prim_" + p, "primitive", {"primitive": p}))
    specs.append(("suzanne", "file", {"path": os.path.join(common.REPO, "samples", "Suzanne.glb")}))
    specs.append(("bust", "bust", {"names": ("Skin", "Cloth")}))
    specs.append(("bust_renamed", "bust", {"names": ("M_Head", "M_Body")}))
    for outfit in ("casual", "knight", "scifi", "game", "blender", "maya", "fantasy", "anime"):
        for pose in ("T", "A"):
            if (outfit, pose) in (("blender", "A"), ("maya", "T"), ("scifi", "A"), ("fantasy", "T")):
                continue
            specs.append(("char_%s_%s" % (outfit, pose), "character",
                          {"outfit": outfit, "pose": pose, "groups": outfit not in ("maya", "blender"),
                           "stylised": 2.0 if outfit == "anime" else 1.0, "hat": outfit == "fantasy"}))
    specs.append(("char_knight_T_nogroups", "character", {"outfit": "knight", "pose": "T", "groups": False}))
    specs.append(("figure_two_slots", "figure", {"split": True}))
    specs.append(("figure_one_slot", "figure", {"split": False}))
    for k in meshgen.PROPS:
        specs.append(("prop_" + k, "prop", {"prop": k}))
    # the user's own models (~/patina-ml/real/): test only, never trained on
    real = os.path.join(common.ML, "real")
    if os.path.isdir(real):
        for f in sorted(os.listdir(real)):
            if f.lower().endswith((".glb", ".gltf", ".fbx", ".obj", ".ptmesh")):
                sid = "real_" + os.path.splitext(f)[0]
                specs.append((sid, "file", {"path": os.path.join(real, f)}))
                HELD_OUT.add(sid)
    return specs


def mesh_file(sid, kind, a, out_dir):
    if kind == "file":
        return a["path"]
    if kind == "bust":
        import manual_scenes
        path = os.path.join(out_dir, sid + ".obj")
        manual_scenes._bust_obj(path)
        # the slot names, as other exporters would have them
        txt = open(path).read().replace("usemtl Skin", "usemtl " + a["names"][0]).replace(
            "usemtl Cloth", "usemtl " + a["names"][1])
        open(path, "w").write(txt.replace("mtllib bust.mtl", "mtllib %s.mtl" % sid))
        with open(os.path.join(out_dir, sid + ".mtl"), "w") as f:
            for n in a["names"]:
                f.write("newmtl %s\nKd 0.7 0.7 0.7\n" % n)
        return path
    if kind == "character":
        b = meshgen.character(sid, a["outfit"], a["pose"], a.get("stylised", 1.0), a.get("groups", True), a.get("hat", False))
    elif kind == "figure":
        b = meshgen.figure(sid, split_slots=a["split"])
    elif kind == "prop":
        b = meshgen.PROPS[a["prop"]](sid)
    else:
        return None
    return b.write(os.path.join(out_dir, sid + ".ptmesh"))


def build(server, sid, kind, a, rng, out_dir):
    if kind == "primitive":
        server("new_project", primitive=a["primitive"], resolution=512)
    else:
        server("open_file", path=mesh_file(sid, kind, a, out_dir))
        server("set_resolution", resolution=512)
    tiles = server("texture_tiles")
    # texture sets on for some multi-slot models, off for the others (a plain packed layout)
    multi = len(tiles.get("tiles") or []) > 1 or kind in ("character", "prop")
    want_sets = multi and rng.random() < 0.35 and tiles.get("mode") in (None, "single", "none")
    if want_sets:
        server("texture_tiles", mode="material_slots")
    elif tiles.get("mode") not in (None, "single", "none") and kind != "bust" and rng.random() < 0.45:
        server("texture_tiles", mode="single")  # one texture for everything (packed below)
    if kind in ("character", "prop", "figure", "bust"):
        server("uv_pack")  # pieces are unwrapped on their own: no two may share texels (per texture set)
    n = rng.choice([0, 0, 1, 2, 2, 3, 4, 6])
    for spec in rng.sample(EXISTING_LAYERS, n):
        server("add_layer", **spec)
    path = os.path.join(out_dir, sid + ".patina")
    server("save_project", path=path)
    parts = server("mesh_parts")
    st = server("get_state")
    meta = {"id": sid, "kind": kind, "path": path, "held_out": sid in HELD_OUT, "texture_sets": server("texture_tiles").get("mode") not in (None, "single", "none"),
            "parts": [{k: p[k] for k in ("id", "kind", "label", "faces")} for p in parts["parts"]],
            "head_found": parts["head_found"], "faces": st["mesh"]["faces"],
            "layers": [{"index": l["index"], "name": l["name"], "type": l["type"], "uid": l["uid"],
                        "material": l.get("material", {}).get("id", "")} for l in st["layers"]]}
    json.dump(meta, open(os.path.join(out_dir, sid + ".json"), "w"), indent=1)
    return meta


def load_scenes(out_dir=common.SCENES):
    out = []
    for f in sorted(os.listdir(out_dir)):
        if f.endswith(".json"):
            out.append(json.load(open(os.path.join(out_dir, f))))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--only", nargs="*")
    ap.add_argument("--seed", type=int, default=11)
    a = ap.parse_args()
    common.ensure_dirs()
    specs = scene_specs()
    if a.only:
        specs = [s for s in specs if s[0] in a.only]
    if a.list:
        for s in specs:
            print(s[0], s[1], "(held out)" if s[0] in HELD_OUT else "")
        return
    with common.Server() as server:
        for sid, kind, args in specs:
            rng = random.Random("%d-%s" % (a.seed, sid))
            meta = build(server, sid, kind, args, rng, common.SCENES)
            print("%-26s %6d faces  %2d parts  %d layers%s%s" % (sid, meta["faces"], len(meta["parts"]), len(meta["layers"]),
                  "  texture sets" if meta["texture_sets"] else "", "  (held out)" if meta["held_out"] else ""))


if __name__ == "__main__":
    main()
