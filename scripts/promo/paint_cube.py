"""The Patina logo cube, painted in Patina: walnut sides, copper with verdigris on the right, a marble top, a gold
band with drips, and a teal enamel swoosh. Every material is masked by the way the surface faces or by
a gradient across the model, with noise breaking up the edges, so the materials blend across the corners."""
from promo_paths import WORK

# The layers that arrive at each stage of the turntable (render_assets.py) and of the video's layer list.
SWOOSH_TEAL = {"color": [0.015, 0.26, 0.24], "roughness": 0.34, "metallic": 0.0, "coat": 0.0}  # a satin teal enamel
STAGES = [["Walnut"], ["Copper folder"], ["Marble folder"], ["Gold drips", "Gold"], ["Swoosh", "Swoosh gold", "Grime"]]


def idx(p, name):
    return next(i for i, l in enumerate(p("list_layers")) if l["name"] == name)


def band(lo, hi):
    """Mask effects for a diagonal band down the copper side, ragged at the edges."""
    g = {"type": "gradient", "direction": [0, 1, -1], "contrast": 0.96, "noise": 0.12, "noise_scale": 1.3}
    return [{"type": "generator", "generator": dict(g, level=lo)},
            {"type": "generator", "blend": "multiply", "generator": dict(g, level=hi, invert=True)},
            {"type": "generator", "blend": "multiply", "generator": {"type": "facing", "direction": [1, 0, 0], "level": 0.6, "contrast": 0.6}}]


def paint(p, resolution=2048):
    p("open_mesh", path=str(WORK / "models" / "patina_cube.obj"), resolution=resolution)
    p("bake_mesh_maps", ao=True, curvature=True)

    def top():  # the next layer goes on top of the stack
        return len(p("list_layers"))

    p("add_layer", type="fill", name="Walnut", index=top(),
      material={"id": "patina_wood_walnut", "scale": 2.6, "rotation": 90, "height_scale": 0.04, "normal_strength": 0.25})
    if any(l["name"] == "Layer 1" for l in p("list_layers")):
        p("remove_layer", index=idx(p, "Layer 1"))
    # copper on the right side, ragged where it turns the corners
    p("add_layer", type="folder", name="Copper folder", index=top(),
      generator={"type": "facing", "direction": [1, 0, 0], "level": 0.66, "contrast": 0.7, "noise": 0.22, "noise_scale": 1.8})
    p("add_layer", type="fill", name="Copper", parent=idx(p, "Copper folder"),
      material={"id": "patina_copper", "roughness_offset": 0.3, "tint": [0.78, 0.6, 0.5], "mapping": "uv", "scale": 2})
    p("add_layer", type="fill", name="Verdigris", parent=idx(p, "Copper folder"),
      material={"id": "patina_verdigris_crust", "tint": [0.45, 0.78, 0.7], "mapping": "uv", "scale": 2},
      generator={"type": "noise", "noise_type": "clouds", "level": 0.56, "contrast": 0.72, "noise_scale": 1.6})
    # marble on top
    p("add_layer", type="folder", name="Marble folder", index=top(),
      generator={"type": "facing", "direction": [0, 1, 0], "level": 0.8, "contrast": 0.8, "noise": 0.2, "noise_scale": 2.2})
    p("add_layer", type="fill", name="Marble", parent=idx(p, "Marble folder"),
      material={"id": "patina_marble_white", "scale": 0.55})
    # a gold band over the top right edge, running down the copper side in drips
    p("add_layer", type="fill", name="Gold drips", index=top(), material={"id": "patina_gold_polished", "roughness_offset": 0.16})
    p("update_layer", index=idx(p, "Gold drips"), mask_effects=[
        {"type": "generator", "generator": {"type": "drips", "level": 0.3, "contrast": 0.7, "noise_scale": 1.0}},
        {"type": "generator", "blend": "multiply", "generator": {"type": "gradient", "direction": [1, 1, 0.25], "level": 0.66, "contrast": 0.35}},
        {"type": "generator", "blend": "multiply", "generator": {"type": "facing", "direction": [1, 0, 0], "level": 0.6, "contrast": 0.6}}])
    p("add_layer", type="fill", name="Gold", index=top(), material={"id": "patina_gold_polished", "roughness_offset": 0.16},
      generator={"type": "gradient", "direction": [1, 1, 0.25], "level": 0.85, "contrast": 0.9, "noise": 0.16, "noise_scale": 2.6})
    # the swoosh: teal enamel and a thread of gold down the copper side
    p("add_layer", type="fill", name="Swoosh", index=top(),
      material=SWOOSH_TEAL)
    p("update_layer", index=idx(p, "Swoosh"), mask_effects=band(0.42, 0.54))
    p("add_layer", type="fill", name="Swoosh gold", index=top(), material={"id": "patina_gold_polished"})
    p("update_layer", index=idx(p, "Swoosh gold"), mask_effects=band(0.55, 0.59))
    # a little grime in the low parts, over everything
    p("add_layer", type="fill", name="Grime", index=top(), opacity=0.25,
      material={"color": [0.05, 0.04, 0.03], "roughness": 0.9},
      generator={"type": "occlusion", "level": 0.35, "contrast": 0.4, "noise": 0.3})
    return [l["name"] for l in p("list_layers")]
