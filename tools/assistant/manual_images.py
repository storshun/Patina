"""Pictures for the manual's Assistant page that need no window (headless Patina): a tint made
by a plan, before and after; the head found on a character; one island per part. With --model
the plans come from the Assistant itself, otherwise from the canonical plans.

    python3 tools/assistant/manual_images.py [--model ~/patina-ml/gguf/patina-assistant-r1-q4_k_m.gguf]
writes docs/manual/img/assistant_*.png
"""
import argparse
import base64
import io
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "scripts"))
import common  # noqa: E402
import manual_scenes as ms  # noqa: E402
import meshgen  # noqa: E402

TINT = ("Set up a smart mask that makes the creases in my character's skin slightly more red than the rest of their "
        "skin.")
UV = "Set up the UVs so I have one UV island for the head and another for the body."
LOOK = "Make it look battle-worn"


def plan_for(s, request, fallback, use_model):
    if use_model:
        r = s("assistant_ask", request=request)
        print("  %s\n  -> %s (%.1f s)" % (request, r["raw"], r["seconds"]))
        if r["ok"]:
            return r["plan"]
    return fallback


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model")
    ap.add_argument("--out", default=os.path.join(common.REPO, "docs", "manual", "img"))
    a = ap.parse_args()
    if a.model:
        os.environ["PATINA_ASSISTANT_MODEL"] = os.path.expanduser(a.model)
    tmp = tempfile.mkdtemp(prefix="patina-manual-")
    cam = {"view": "front", "frame": True, "yaw": 0.45, "pitch": 0.12}
    with common.Server(resolution=1024) as s:
        if a.model:
            s("assistant_model", action="load")
        # 1. the first example: a tint in the skin's creases
        model = meshgen.figure("Statue", seg=48, rows=80).write(os.path.join(tmp, "statue.ptmesh"))
        s("open_file", path=model)
        s("set_resolution", resolution=1024)
        s("add_material_layer", material="patina_skin_light", region={"part": "slot:Skin"})
        s("add_material_layer", material="patina_fabric_linen", region={"part": "slot:Body"})
        before = ms._label(ms._b64img(s("render_view", width=520, height=560, camera=cam)), "Before")
        plan = plan_for(s, TINT, {"say": "Adds a soft red tint in the skin's creases.", "steps": [
            {"cmd": "add_tint_layer", "args": {"blend": "soft_light", "color": [0.75, 0.18, 0.15],
                                               "mask": [{"contrast": 0.6, "level": 0.55, "type": "cavity"}],
                                               "name": "Skin crease redness", "region": {"part": "slot:Skin"},
                                               "strength": 0.25}}]}, bool(a.model))
        s("assistant_apply", plan=plan, request=TINT)
        after = ms._label(ms._b64img(s("render_view", width=520, height=560, camera=cam)), "After: " + plan.get("say", ""))
        ms._side_by_side([before, after], os.path.join(a.out, "assistant_tint.png"))
        # 2. the head found from the shape on a character with no named head
        model = meshgen.character("Hero", "maya", "A", groups=False).write(os.path.join(tmp, "hero.ptmesh"))
        s("open_file", path=model)
        s("uv_pack")
        s("select_part", part="region:head")
        s("set_view", mode="material", wireframe=False, seams=False)
        head = ms._label(ms._b64img(s("render_view", width=420, height=560, camera=cam)), "region:head")
        s("select_part", part="region:body")
        body = ms._label(ms._b64img(s("render_view", width=420, height=560, camera=cam)), "region:body")
        ms._side_by_side([head, body], os.path.join(a.out, "assistant_parts.png"))
        # 3. the second example: one island for the head and one for the body
        bust = os.path.join(tmp, "bust.obj")
        ms._bust_obj(bust)
        s("open_file", path=bust)
        s("texture_tiles", mode="single")
        s("uv_unwrap", method="smart")
        plan = plan_for(s, UV, {"say": "Rebuilds the UVs with one island each for the head and the body.", "steps": [
            {"cmd": "uv_island_per_part", "args": {"parts": ["region:head", "region:body"]}}]}, bool(a.model))
        s("select_faces", mode="clear")
        s("assistant_apply", plan=plan, request=UV)
        s("set_view", mode="uv_islands", wireframe=False, seams=True)
        v = ms._label(ms._b64img(s("render_view", width=520, height=560, camera=cam)), "Two islands, seams in red")
        u = ms._label(ms._b64img(s("render_uv", width=560, height=560)), "The UV layout")
        ms._side_by_side([v, u], os.path.join(a.out, "assistant_uv.png"))
        # 4. a whole look: a folder of weathering layers, one request
        model = meshgen.barrel("Barrel", seg=48).write(os.path.join(tmp, "barrel.ptmesh"))
        s("open_file", path=model)
        s("set_resolution", resolution=1024)
        s("set_view", mode="material", wireframe=False, seams=False)
        s("auto_texture")
        s("bake_mesh_maps", ao=True, curvature=True)
        bcam = {"view": "front", "frame": True, "yaw": 0.5, "pitch": 0.35}
        before = ms._label(ms._b64img(s("render_view", width=520, height=560, camera=bcam)), "Before")
        plan = plan_for(s, LOOK, {"say": "Makes the model look battle-worn.", "steps": [
            {"cmd": "add_material_layer", "args": {"material": "patina_steel_scratched", "name": "Edge wear", "strength": 0.6,
                                                   "mask": [{"type": "edges"}]}},
            {"cmd": "add_material_layer", "args": {"material": "patina_dirt_mud", "name": "Dirt", "strength": 0.45,
                                                   "mask": [{"type": "cavity"}]}},
            {"cmd": "add_material_layer", "args": {"material": "patina_soot", "name": "Soot", "strength": 0.25,
                                                   "mask": [{"type": "noise"}]}}]}, bool(a.model))
        s("assistant_apply", plan=plan, request=LOOK)
        after = ms._label(ms._b64img(s("render_view", width=520, height=560, camera=bcam)), "After: " + LOOK)
        ms._side_by_side([before, after], os.path.join(a.out, "assistant_look.png"))
    print("wrote assistant_tint.png, assistant_parts.png, assistant_uv.png, assistant_look.png in", a.out)


if __name__ == "__main__":
    main()
