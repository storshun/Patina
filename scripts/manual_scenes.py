"""Scenes for scripts/manual_shots.py, in the default 1600x960 layout. Where things are comes from the app itself
(the ui_rect command: menu.*, tools.*, btn.*, window.<name>, popup.top, viewport.image, uv.image), so the
scenes follow the layout; layout(s) reads the panels' places."""
import os
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SUZANNE = os.path.join(ROOT, "samples", "Suzanne.glb")

# the panels' places: read from the app by layout(s)
VIEW = (74, 106, 725, 675)            # viewport image
UV = (730, 106, 1200, 675)            # UV editor canvas
CROP_TOOLS = (0, 22, 72, 588)
CROP_LAYERS = (1202, 22, 1600, 434)
CROP_RIGHT_LOWER = (1202, 434, 1600, 940)
CROP_BOTTOM = (72, 678, 1202, 940)
CROP_VIEWPORT = (72, 22, 728, 678)
CROP_UV = (728, 22, 1202, 678)


def rect(s, name):
    """where a widget, window or the open popup is (window pixels: x0, y0, x1, y1)"""
    r = s("ui_rect", name=name)
    if not r.get("found"):
        raise RuntimeError("not on screen: " + name)
    return (round(r["x"]), round(r["y"]), round(r["x"] + r["w"]), round(r["y"] + r["h"]))


def center(s, name):
    x0, y0, x1, y1 = rect(s, name)
    return ((x0 + x1) // 2, (y0 + y1) // 2)


def click(s, name, b=1, hold=0.1):
    s.x.click(*center(s, name), b=b, hold=hold)
    s.settle(3, 0.3)


def tool(s, key):
    """a tool of the tool strip (tool.paint, tool.seams...)"""
    click(s, "tools.tool." + key)


def menu(s, key):
    """opens a menu of the menu bar (file, edit, view, bake, uv, mesh, retopology, blender, help)"""
    click(s, "menu." + key, hold=0.3)
    s.settle(4, 0.5)


def menu_crop(s, pad=6):
    """the menu bar up to the open menu, and the menu"""
    x0, y0, x1, y1 = rect(s, "popup.top")
    return (max(0, x0 - 60), 0, x1 + pad, y1 + pad)


def popup_crop(s, pad=4):
    x0, y0, x1, y1 = rect(s, "popup.top")
    return (max(0, x0 - pad), max(0, y0 - pad), x1 + pad, y1 + pad)


def window_crop(s, name, pad=2):
    s.settle(8, 0.8)  # (a window that sizes itself to its content takes a few frames)
    x0, y0, x1, y1 = rect(s, "window." + name)
    return (max(0, x0 - pad), max(0, y0 - pad), x1 + pad, y1 + pad)


def panel(s, name):
    """a docked panel's tab to the front (layers, properties, materials, brushes, console, history, notes, colors,
    texture_set, blender)"""
    s("open_window", window=name)
    s.settle(5, 0.6)


def layout(s):
    """the panels' places in the window"""
    global VIEW, UV, CROP_TOOLS, CROP_LAYERS, CROP_RIGHT_LOWER, CROP_BOTTOM, CROP_VIEWPORT, CROP_UV
    time.sleep(0.6)
    VIEW = rect(s, "viewport.image")
    UV = rect(s, "uv.image")
    CROP_VIEWPORT = rect(s, "window.Viewport")
    CROP_UV = rect(s, "window.UV Editor")
    CROP_LAYERS = rect(s, "window.Layers")
    t, last = rect(s, "window.Tools"), rect(s, "tools.tool.note")
    CROP_TOOLS = (t[0], t[1], t[2], last[3] + 8)
    panel(s, "materials")
    CROP_BOTTOM = rect(s, "window.Materials")
    panel(s, "properties")
    CROP_RIGHT_LOWER = rect(s, "window.Properties")


def uvxy(u, v):
    """a point of the 0..1 square in the default UV view"""
    side = 0.9 * min(UV[2] - UV[0], UV[3] - UV[1])
    cx, cy = (UV[0] + UV[2]) / 2, (UV[1] + UV[3]) / 2
    return cx + (u - 0.5) * side, cy - (v - 0.5) * side


def vp(fx, fy):
    """normalized viewport point -> window pixel"""
    return (VIEW[0] + fx * (VIEW[2] - VIEW[0]), VIEW[1] + fy * (VIEW[3] - VIEW[1]))


def build_suzanne(s):
    s("open_file", path=SUZANNE)
    s("set_resolution", resolution=2048)
    s("bake_mesh_maps", ao=True, curvature=True)
    s("set_camera", view="front", frame=True)
    s("set_camera", yaw=0.45, pitch=0.18)
    # painted steel, rust on the edges, dirt in the cavities, a gold stripe painted by hand
    s("add_layer", type="fill", name="Painted steel", material={"id": "patina_paint_chipped_red", "scale": 1.6})
    s("add_layer", type="fill", name="Edge rust", material={"id": "patina_rust_heavy", "scale": 1.2},
      generator={"type": "edges", "level": 0.56, "contrast": 0.55, "noise": 0.5, "noise_scale": 3})
    s("add_layer", type="fill", name="Cavity dirt", material={"id": "patina_dirt_mud", "scale": 2},
      generator={"type": "occlusion", "level": 0.55, "contrast": 0.5, "noise": 0.3, "noise_scale": 2})
    s("update_layer", opacity=0.8)
    s("add_layer", type="paint", name="Gold trim")
    s("set_paint_material", id="patina_gold_polished")
    s("set_brush", preset="basic_hard", size=34, hardness=0.85, flow=1, opacity=1)
    s("paint_stroke", points=[[0.34, 0.28], [0.42, 0.24], [0.5, 0.23], [0.58, 0.24], [0.66, 0.28]])
    s("set_paint_material", id="patina_rust_heavy")


def compose(paths_titles, out, cols=2, pad=8):
    """grid of images with captions"""
    from PIL import Image, ImageDraw
    ims = [(Image.open(p), t) for p, t in paths_titles]
    w = max(i.width for i, _ in ims)
    h = max(i.height for i, _ in ims) + 22
    rows = (len(ims) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * w + (cols + 1) * pad, rows * h + (rows + 1) * pad), (27, 28, 31))
    d = ImageDraw.Draw(sheet)
    for k, (im, t) in enumerate(ims):
        x = pad + (k % cols) * (w + pad)
        y = pad + (k // cols) * (h + pad)
        sheet.paste(im, (x, y + 22))
        d.text((x + 2, y + 4), t, fill=(220, 220, 220))
    sheet.save(out, optimize=True)


def run(s):
    out = s.out
    print("hero")
    build_suzanne(s)
    layout(s)
    s.x.move(*vp(0.62, 0.62))
    s.shot("overview")
    s.shot("tools", crop=CROP_TOOLS)
    s.shot("layers_panel", crop=CROP_LAYERS)
    s.shot("viewport", crop=CROP_VIEWPORT)
    s.shot("materials_panel", crop=CROP_BOTTOM)

    print("menus")
    menu(s, "file")
    s.shot("menu_file", crop=menu_crop(s))
    s.x.key("Escape")
    menu(s, "help")
    s.shot("menu_help", crop=menu_crop(s))
    s.x.key("Escape")

    print("tabs")
    panel(s, "brushes")
    s.shot("brushes_panel", crop=CROP_BOTTOM)
    panel(s, "console")
    s.x.click(300, CROP_BOTTOM[3] - 16)
    type_text(s, 'set_brush {"size": 48, "hardness": 0.6}')
    s.x.key("Return")
    time.sleep(0.4)
    s.shot("console_panel", crop=CROP_BOTTOM)
    panel(s, "materials")
    panel(s, "properties")
    s.shot("properties_panel", crop=CROP_RIGHT_LOWER)
    panel(s, "texture_set")
    s.shot("texture_set_panel", crop=CROP_RIGHT_LOWER)
    panel(s, "blender")
    s.shot("blender_panel", crop=CROP_RIGHT_LOWER)
    panel(s, "properties")

    print("painting in progress")
    s("select_layer", index=4)
    s("set_paint_material", id="patina_rust_heavy")
    s("set_brush", preset="basic_soft", size=70, hardness=0.5, flow=0.8, opacity=1)
    a, b = vp(0.42, 0.62), vp(0.6, 0.58)
    s.x.move(*a)
    time.sleep(0.3)
    s.x.button(1, True)
    for k in range(1, 13):
        s.x.move(a[0] + (b[0] - a[0]) * k / 12, a[1] + (b[1] - a[1]) * k / 12)
        time.sleep(0.06)
    s.shot("painting_stroke", crop=CROP_VIEWPORT)
    s.x.button(1, False)
    time.sleep(0.5)

    print("smart mask / layer properties")
    s("select_layer", index=2)  # Edge rust
    s.x.move(1400, 300)
    for _ in range(6):
        s.x.button(5, True); s.x.button(5, False); time.sleep(0.05)
    s.shot("smart_mask", crop=CROP_LAYERS)
    # the rust layer alone over the base: the edge-wear mask at work
    for i in (1, 3, 4):
        s("update_layer", index=i, visible=False)
    s.shot("smart_mask_view", crop=CROP_VIEWPORT)
    for i in (1, 3, 4):
        s("update_layer", index=i, visible=True)

    print("adjustment layer")
    s("select_layer", index=4)
    s("add_layer", type="adjust", name="Hue shift", adjust={"hue": 0.42, "saturation": 0.9})
    s.x.move(1400, 300)
    for _ in range(6):
        s.x.button(5, True); s.x.button(5, False); time.sleep(0.05)
    s.shot("adjust_layer", crop=CROP_LAYERS)
    s.shot("adjust_view", crop=CROP_VIEWPORT)
    s("remove_layer", index=5)

    print("view modes")
    names = []
    for m, t in [("basecolor", "Base color"), ("roughness", "Roughness"), ("normal", "Normal"),
                 ("curvature", "Curvature (baked)"), ("ao", "Ambient occlusion (baked)"), ("uv_checker", "UV checker")]:
        s("set_view", mode=m)
        s.shot("_vm_" + m, crop=(VIEW[0] + 60, VIEW[1] + 40, VIEW[2] - 60, VIEW[3] - 80), scale=0.5)
        names.append((os.path.join(out, "_vm_" + m + ".png"), t))
    compose(names, os.path.join(out, "view_modes.png"), cols=3)
    for p, _ in names:
        os.remove(p)
    s("set_view", mode="material")

    print("uv editor")
    click(s, "btn.uv.sel_island")
    s.x.click(*uvxy(0.47, 0.6))      # click an island
    s.shot("uv_islands", crop=CROP_UV)
    s.shot("uv_islands_model", crop=CROP_VIEWPORT)
    click(s, "btn.uv.sel_vertex")
    s.x.drag([uvxy(0.16, 0.85), uvxy(0.6, 0.5)], dt=0.15)
    s.x.move(*uvxy(0.82, 0.16))
    s.shot("uv_vertices", crop=CROP_UV)
    click(s, "btn.uv.sel_face")
    s("select_faces", mode="clear")
    click(s, "btn.uv.unwrap")        # the Unwrap menu
    p = rect(s, "popup.top")
    s.shot("uv_unwrap_menu", crop=(CROP_UV[0], CROP_UV[1], max(CROP_UV[2], p[2] + 6), max(CROP_UV[3], p[3] + 6)))
    s.x.key("Escape")
    click(s, "btn.uv.mode_paint")    # Paint mode
    s("set_paint_material", id="patina_gold_polished")
    s("set_brush", size=26, hardness=0.9)
    # a hand-drawn stroke under Xvfb runs at a few frames per second, so paint the
    # texture-space stroke through the command API (same code path as the UV paint mode)
    s("paint_stroke", space="uv", points=[[0.18, 0.42], [0.25, 0.47], [0.33, 0.5], [0.42, 0.51], [0.5, 0.5]],
      brush={"size": 22, "hardness": 0.9})
    s.x.move(*uvxy(0.6, 0.48))
    s.shot("uv_paint", crop=CROP_UV)
    click(s, "btn.uv.mode_edit")     # back to edit mode

    print("seams")
    tool(s, "seams")
    s("uv_seams", mode="from_islands")
    s("set_view", seams=True, wireframe=True)
    s.x.move(*vp(0.5, 0.45))
    s.shot("seams_tool", crop=CROP_VIEWPORT)
    s("set_view", seams=False, wireframe=False)

    print("clone")
    tool(s, "clone")
    s("select_layer", index=4)
    src = vp(0.5, 0.25)
    s.x.key("Control_L", True)
    s.x.click(*src)
    s.x.key("Control_L", False)
    s("set_brush", size=60, hardness=0.6)
    s.x.drag([vp(0.36, 0.72), vp(0.39, 0.73), vp(0.42, 0.74), vp(0.45, 0.745), vp(0.48, 0.75)], dt=0.25)
    s.x.move(*vp(0.6, 0.7))
    s.shot("clone_tool", crop=CROP_VIEWPORT)
    tool(s, "paint")

    print("dialogs")
    s.x.move(*vp(0.9, 0.9))
    s.x.key("Control_L", True); s.x.key("e"); s.x.key("Control_L", False)
    s.x.move(700, 400)
    s.shot("export_dialog", crop=window_crop(s, "Export Textures"))
    s.x.key("Escape")
    s.x.key("F1")
    s.x.move(*vp(0.9, 0.9))
    s.shot("help_window", crop=window_crop(s, "Patina Help"))
    s.x.key("Escape")
    s("open_window", window="about")
    s.x.move(*vp(0.9, 0.9))
    s.shot("about_window", crop=window_crop(s, "About Patina"))
    s.x.key("Escape")
    panel(s, "materials")
    click(s, "materials.tile:patina_rust_heavy", b=3)   # right-click Rust Heavy
    s.shot("material_context", crop=popup_crop(s))
    s.x.key("Escape")
    s("open_window", window="material_editor", material="patina_rust_heavy")
    time.sleep(0.8)
    s.x.move(*vp(0.9, 0.9))
    s.shot("material_editor", crop=window_crop(s, "Material Editor"))


def run_03(s):
    """Patina 0.3: folders, solo, color tags, smart materials, history, colors, shortcuts."""
    out = s.out
    print("0.3: folders")
    s.x.move(*vp(0.5, 0.5))
    s.x.key("Escape")                # closes the material editor left open by run()
    s("set_camera", view="front", frame=True)
    s("set_camera", yaw=0.45, pitch=0.18)
    s.x.move(1400, 300)              # scroll the Layers panel back to the top
    for _ in range(12):
        s.x.button(4, True); s.x.button(4, False); time.sleep(0.03)
    panel(s, "properties")
    s("select_layer", index=4)
    f = s("add_layer", type="folder", name="Wear")["index"]
    s("move_layer_into", index=2, folder=s("list_layers")[-1]["index"])   # Edge rust
    s("move_layer_into", index=2, folder=[l["index"] for l in s("list_layers") if l["name"] == "Wear"][0])  # Cavity dirt
    names = {l["name"]: l["index"] for l in s("list_layers")}
    s("update_layer", index=names["Wear"], color_tag=2)
    s("update_layer", index=names["Gold trim"], color_tag=3)
    s("update_layer", index=names["Painted steel"], color_tag=6)
    s("select_layer", index=names["Wear"])
    s.x.move(1400, 600)
    s.shot("layers_folders", crop=CROP_LAYERS)
    print("0.3: solo")
    s("solo_layer", index=names["Wear"])
    s.x.move(900, 800)
    s.shot("solo_layers", crop=CROP_LAYERS)
    s.shot("solo_view", crop=CROP_VIEWPORT, scale=0.7)
    s("solo_layer", index=-1)
    print("0.3: context menu")
    # the menu opens on release; under Xvfb frames are slow, so retry until it shows
    row = center(s, "layers.row:" + str([l for l in s("list_layers") if l["name"] == "Wear"][0]["uid"]))
    for _ in range(4):
        s.x.move(row[0] + 40, row[1])
        time.sleep(0.8)
        s.x.button(3, True); time.sleep(0.8); s.x.button(3, False)
        time.sleep(1.2)
        if s("ui_rect", name="popup.top").get("found"):
            break
        print("   context menu did not open, retrying")
    p = rect(s, "popup.top")
    s.shot("layer_context", crop=(CROP_LAYERS[0], CROP_LAYERS[1], CROP_LAYERS[2], max(CROP_LAYERS[3], p[3] + 6)), settle=0.3)
    s.x.key("Escape")
    time.sleep(0.6)
    print("0.3: smart materials menu")
    click(s, "btn.layer.smart")
    p = rect(s, "popup.top")
    s.shot("smart_menu", crop=(min(CROP_LAYERS[0], p[0] - 4), CROP_LAYERS[1], CROP_LAYERS[2], p[3] + 6))
    s.x.key("Escape")
    time.sleep(0.6)
    s.x.click(900, 950)              # the first click after closing a popup is swallowed
    print("0.3: history / colors")
    panel(s, "history")
    s("update_layer", index=names["Wear"], opacity=0.85)
    time.sleep(1.0)
    s.x.move(1400, 925)
    s.shot("history_panel", crop=CROP_RIGHT_LOWER)
    panel(s, "colors")
    s("set_paint_material", id="", color=[0.82, 0.2, 0.14])
    s.x.move(1400, 300)
    s.shot("colors_panel", crop=CROP_RIGHT_LOWER)
    panel(s, "properties")
    print("0.3: menus and dialogs")
    menu(s, "edit")
    s.shot("menu_edit", crop=menu_crop(s))
    s.x.key("Escape")
    menu(s, "view")
    s.shot("menu_view", crop=menu_crop(s))
    s.x.key("Escape")
    s("open_window", window="shortcuts")
    time.sleep(0.8)
    s.x.move(*vp(0.95, 0.95))
    s.shot("keymap_editor", crop=window_crop(s, "Keyboard shortcuts"))
    s.x.key("Escape")
    s("select_layer", index=names["Wear"])
    s.x.move(*vp(0.9, 0.9))
    s("open_window", window="export", what="layer")
    time.sleep(0.8)
    s.x.move(700, 400)
    s.shot("export_layer_dialog", crop=window_crop(s, "Export Textures"))
    s.x.key("Escape")
    print("0.3: distraction-free")
    s.x.move(*vp(0.5, 0.95))
    s.x.key("Tab")
    s.shot("focus_mode", scale=0.5)
    s.x.key("Tab")
    print("0.3: radial symmetry")
    s("new_project", primitive="sphere", resolution=2048)
    s("set_camera", view="top", frame=True)
    s("set_camera", pitch=1.2)
    time.sleep(0.5)
    s("set_symmetry", radial=8, radial_axis="y")
    s("set_paint_material", id="patina_gold_polished")
    s("set_brush", preset="basic_hard", size=30, hardness=0.8, flow=1, opacity=1, projection="surface")
    s("paint_stroke", points=[[0.5, 0.36], [0.53, 0.32], [0.56, 0.3], [0.6, 0.3], [0.62, 0.33]])
    s("set_paint_material", id="", color=[0.15, 0.35, 0.85])
    s("paint_stroke", points=[[0.5, 0.18], [0.47, 0.16], [0.44, 0.17]])
    s("set_symmetry", radial=1)
    s("set_brush", projection="screen")
    s("set_view", seams=False)
    tool(s, "paint")
    s.x.move(900, 800)
    s.shot("radial_symmetry", crop=CROP_VIEWPORT, scale=0.7)


def run_036(s):
    """Patina 0.36: the 3D view's right-click menu, the Language preferences."""
    print("0.36: the 3D view's right-click menu")
    build_suzanne(s)
    layout(s)
    p = vp(0.48, 0.42)
    for _ in range(4):
        s.x.move(*p)
        time.sleep(0.5)
        s.x.button(3, True); time.sleep(0.3); s.x.button(3, False)
        time.sleep(1.0)
        if s("ui_rect", name="popup.top").get("found"):
            break
        print("   the menu did not open, retrying")
    m = rect(s, "popup.top")
    s.x.move(m[0] + 70, m[1] + 54)     # over "Layers painted here": its list opens beside it
    time.sleep(1.2)
    sub = rect(s, "popup.top")
    x0, y0 = max(CROP_VIEWPORT[0], m[0] - 170), max(CROP_VIEWPORT[1], m[1] - 70)
    s.shot("view_context", crop=(x0, y0, max(m[2], sub[2]) + 10, max(m[3], sub[3]) + 10), settle=0.3)
    s.x.key("Escape")
    s.x.key("Escape")
    time.sleep(0.5)
    print("0.36: Preferences > Language")
    s("open_window", window="preferences", section="language")
    time.sleep(1.2)
    a, w = rect(s, "prefs.language"), rect(s, "window.Preferences")
    s.x.move(900, 900)
    s.shot("prefs_language", crop=(w[0], a[1] - 4, w[2], a[3] + 4), settle=0.5)
    s.x.key("Escape")
    time.sleep(0.4)


def run_037(s):
    """Patina 0.37: the axis gizmo in the 3D view's corner, the Retopology preview button."""
    print("0.37: the axis gizmo")
    s("open_file", path=SUZANNE)
    s("set_resolution", resolution=1024)
    layout(s)
    s("set_tool", tool="paint")
    s("set_camera", view="front", frame=True)
    s("set_camera", yaw=0.55, pitch=0.32)
    # (the panels hidden: the view fills the window, so the tooltip stays inside it)
    s.x.move(*vp(0.5, 0.6))
    s.x.key("Tab")
    time.sleep(4.5)   # (the "panels hidden" hint fades)
    g = rect(s, "gizmo.view")
    s.x.move(*center(s, "gizmo.X"))
    time.sleep(1.2)   # (its tooltip after a moment)
    s.shot("view_gizmo", crop=(g[0] - 330, g[1] - 10, g[2] + 10, g[3] + 72), settle=0.3)
    s.x.move(*vp(0.5, 0.6))
    s.x.key("Tab")
    time.sleep(1.0)
    layout(s)
    print("0.37: the Retopology preview button")
    s("set_camera", view="front", frame=True)
    s("set_camera", yaw=0.45, pitch=0.2)
    s("retopo_density", quads_per_m2=1500)
    s("retopo_build")
    s("retopo_view", preview=True, show="none", quads=True)
    click(s, "btn.retopo_preview.more")
    time.sleep(0.8)
    s.x.move(*vp(0.95, 0.95))
    time.sleep(0.4)
    s.shot("retopo_preview", crop=CROP_VIEWPORT, settle=0.5)
    s.x.key("Escape")
    s("retopo_view", preview=False)
    s("retopo_clear")
    time.sleep(0.4)


def run_0371(s):
    """Patina 0.37.1: pinning from the 3D view (the Select tool's Pin and Unpin, the pinned islands striped blue)."""
    print("0.37.1: pins in the 3D view")
    s("open_file", path=SUZANNE)
    s("set_resolution", resolution=1024)
    layout(s)
    s("uv_unwrap", method="smart")
    s("set_tool", tool="select")
    s("set_camera", view="front", frame=True)
    c = s("set_camera")
    s("set_camera", yaw=0.5, pitch=0.22, distance=c["distance"] * 0.8)
    time.sleep(0.6)

    def island_at(p):
        h = s("pick_face", point=p)
        return [h["face"]] if h.get("hit") else []
    # pin the ear's island and the side of the head's; select the ear's and a cheek's (one pinned, one not)
    s("select_faces", mode="clear")
    s("select_faces", mode="island", faces=island_at([0.9, 0.4]) + island_at([0.7, 0.4]))
    s("uv_pin", selection=True, pin=True)
    s("select_faces", mode="clear")
    s("select_faces", mode="island", faces=island_at([0.9, 0.4]) + island_at([0.5, 0.6]))
    s.x.move(*vp(0.05, 0.95))
    x0, y0, x1, y1 = VIEW
    s.shot("view_pins", crop=(x0, y0 - 62, x1, y1), settle=0.6)

def run_038(s):
    """Patina 0.38: the view pie (hold Z), a side view with the gizmo's Back button, the Solid view."""
    print("0.38: the view pie")
    s("open_file", path=SUZANNE)
    s("set_resolution", resolution=1024)
    layout(s)
    s("set_tool", tool="paint")
    s("set_camera", view="persp", frame=True, ortho=False)
    s("set_camera", yaw=0.55, pitch=0.25)
    s.x.move(*vp(0.5, 0.55))
    time.sleep(0.4)
    s.x.key("z", True)
    time.sleep(0.4)
    x0, y0, x1, y1 = rect(s, "pie.1")
    c = rect(s, "pie.center")
    s.x.move(c[0] + 70, c[1] + 4)  # (toward Solid, on the right)
    time.sleep(0.5)
    p0, p1 = rect(s, "pie.0"), rect(s, "pie.1")
    t, b = rect(s, "pie.3"), rect(s, "pie.2")
    s.shot("view_pie", crop=(p0[0] - 24, t[1] - 24, p1[2] + 24, b[3] + 24), settle=0.2)
    s.x.key("Escape")
    s.x.key("z", False)
    time.sleep(0.4)
    print("0.38: a side view and its Back button")
    s("set_view", mode="solid")
    s.x.move(*vp(0.5, 0.6))
    s.x.key("KP_3")
    time.sleep(0.5)
    g = rect(s, "gizmo.view")
    bk = rect(s, "gizmo.back")
    s.x.move(*center(s, "gizmo.back"))
    time.sleep(1.2)   # (its tooltip)
    s.shot("view_back", crop=(g[0] - 160, g[1] - 10, g[2] + 290, bk[3] + 60), settle=0.3)
    s.x.move(*vp(0.5, 0.6))
    s.x.key("KP_0")   # (back to the view before: the Solid view in perspective)
    s.x.move(*vp(0.97, 0.97))
    s.shot("view_solid", crop=CROP_VIEWPORT, settle=0.5)
    s("set_view", mode="material")
    time.sleep(0.4)

def run_039(s):
    """Patina 0.39: the Path traced view mode (against the raster view), its settings, the path traced render."""
    print("0.39: path tracing")
    s("open_file", path=SUZANNE)
    s("set_resolution", resolution=1024)
    layout(s)
    s("set_tool", tool="paint")
    s("set_camera", view="persp", frame=True, ortho=False)
    s("set_camera", yaw=0.62, pitch=0.22)
    s("add_layer", type="fill", material={"id": "patina_gold_polished"})
    seams = s("set_view")["seams"]
    s("set_view", mode="material", seams=False)
    s.x.move(800, 945)  # (the status bar: no brush circle in the view)
    s.shot("pt_raster", crop=CROP_VIEWPORT, settle=1.0)
    s("path_trace", max_samples=256, ground=True)
    s("set_view", mode="path_traced")
    for _ in range(60):
        time.sleep(0.5)
        if s("path_trace")["done"]:
            break
    s.shot("pt_traced", crop=CROP_VIEWPORT, settle=0.2)
    click(s, "btn.path_trace")
    time.sleep(0.6)
    p = rect(s, "popup.top")
    s.shot("pt_settings", crop=(p[0] - 12, p[1] - 44, p[2] + 12, p[3] + 12), settle=0.2)
    s.x.key("Escape")
    time.sleep(0.3)
    s("open_window", window="render")
    time.sleep(0.6)
    click(s, "render.path_traced")
    s.x.move(800, 945)
    w = window_crop(s, "Render image")
    t = rect(s, "render.path_traced")
    s.shot("render_pt", crop=(w[0], t[1] - 6, w[2], w[3]), settle=0.5)  # (from the options down: not the test's file path)
    s.x.key("Escape")
    s("set_view", mode="material", seams=seams)
    s("undo")
    time.sleep(0.4)

def run_040(s):
    """Patina 0.40: the edit mode pie (`), the channel pie (Shift+Z), Quick Favorites (Q) with its picker and the
    right-click menu, Preferences > Pie menus, the brush's flow / hardness / angle keys (Blender keymap)."""
    print("0.40: the pies")
    s("open_file", path=SUZANNE)
    s("set_resolution", resolution=1024)
    layout(s)
    s("set_tool", tool="paint")
    s("set_camera", view="persp", frame=True, ortho=False)
    s("set_camera", yaw=0.55, pitch=0.25)
    s("pie_menus", favorites=["select.none", "island.make", "sym.x", "color.swap", "view.back", "file.render", "", ""])

    def open_pie(key, toward, shift=False):
        s.x.move(*vp(0.5, 0.55))
        time.sleep(0.4)
        if shift:
            s.x.key("Shift_L", True)
        s.x.key(key, True)
        time.sleep(0.4)
        if shift:
            s.x.key("Shift_L", False)
        c = rect(s, "pie.center")
        s.x.move(c[0] + toward[0], c[1] + toward[1])
        time.sleep(0.5)

    def pie_shot(name):
        p0, p1 = rect(s, "pie.0"), rect(s, "pie.1")
        t, b = rect(s, "pie.3"), rect(s, "pie.2")
        s.shot(name, crop=(min(p0[0], rect(s, "pie.4")[0], rect(s, "pie.6")[0]) - 24, t[1] - 24,
                           max(p1[2], rect(s, "pie.5")[2], rect(s, "pie.7")[2]) + 24, b[3] + 24), settle=0.2)

    def close_pie(key):
        s.x.key("Escape")
        s.x.key(key, False)
        time.sleep(0.4)

    open_pie("grave", (4, 70))       # (toward Island, down)
    pie_shot("pie_edit")
    close_pie("grave")
    open_pie("z", (4, 70), shift=True)  # (toward Roughness, down)
    pie_shot("pie_channels")
    close_pie("z")
    open_pie("q", (-60, 60))         # (toward an empty place, down left)
    pie_shot("pie_favorites")
    s.x.key("q", False)              # (let go there: the picker, everything a pie can hold)
    time.sleep(0.6)
    w = rect(s, "favpick.view.wireframe")
    s.x.move(w[0] + 60, rect(s, "favpick.view.lit")[1] - 18)  # (on the search box: no row's tooltip)
    time.sleep(0.4)
    c = rect(s, "pie.center")
    s.shot("pie_picker", crop=(w[0] - 20, c[1] - 200, w[2] + 20, c[1] + 210), settle=0.3)
    s.x.key("Escape")
    time.sleep(0.4)
    # right-click an item of a pie: Add to Quick Favorites
    s.x.move(*vp(0.5, 0.55))
    time.sleep(0.3)
    s.x.key("z")
    time.sleep(0.5)
    p1 = rect(s, "pie.1")
    s.x.click(*center(s, "pie.1"), b=3)
    time.sleep(0.5)
    m = rect(s, "menu.favorite")
    s.x.move(*center(s, "menu.favorite"))
    time.sleep(0.3)
    s.shot("pie_menu_add", crop=(m[0] - 150, m[1] - 70, m[2] + 30, m[3] + 70), settle=0.2)
    s.x.key("Escape")
    time.sleep(0.4)
    print("0.40: Preferences > Pie menus")
    s("open_window", window="preferences", section="pies")
    time.sleep(0.8)
    s.x.move(800, 945)
    w = rect(s, "window.Preferences")
    a, b = rect(s, "prefs.pies"), rect(s, "prefs.pies_end")
    s.shot("prefs_pies", crop=(w[0], a[1] - 8, w[2], b[3] + 10), settle=0.4)
    s.x.move(*vp(0.5, 0.55))
    time.sleep(0.2)
    s.x.key("Escape")
    time.sleep(0.4)
    print("0.40: the brush keys")
    s("open_window", window="preferences", keymap="blender")
    time.sleep(0.5)
    s.x.move(*vp(0.5, 0.55))
    time.sleep(0.2)
    s.x.key("Escape")
    time.sleep(0.4)
    s("set_brush", flow=0.6, hardness=0.6, angle=0)

    def radial_shot(name, key, to, mods=()):
        c = vp(0.42, 0.5)
        s.x.move(*c)
        time.sleep(0.4)
        for k in mods:
            s.x.key(k, True)
        time.sleep(0.1)
        s.x.key(key)
        for k in mods:
            s.x.key(k, False)
        time.sleep(0.3)
        s.x.move(c[0] + to[0], c[1] + to[1])
        time.sleep(0.4)
        s.shot(name, crop=(int(c[0] - 260), int(c[1] - 170), int(c[0] + 120), int(c[1] + 170)), settle=0.4)
        s.x.key("Escape")  # (cancel: the value goes back)
        time.sleep(0.3)

    radial_shot("brush_hardness", "g", (-25, 0), mods=("Shift_L",))
    # (1.2.14: Ctrl+F's dial, the tip turning, is in run_1214's tip_cursor_dials)
    s("open_window", window="preferences", keymap="patina")
    time.sleep(0.5)
    s.x.move(*vp(0.5, 0.55))
    time.sleep(0.2)
    s.x.key("Escape")
    time.sleep(0.4)

def run_0410(s):
    """Patina 0.41: baked maps as layers. A rough AO bake on Suzanne (few samples: noisy, the eye sockets too dark)
    fixed on its layer: painted lighter in the sockets, a Blur adjustment over the noise; the layer list's Baked maps
    section and the AO layer's properties."""
    print("0.41: baked map layers")
    s("open_file", path=SUZANNE)
    s("set_resolution", resolution=1024)
    s("bake_mesh_maps", ao=True, curvature=True, ao_samples=6)
    layout(s)
    s("set_camera", view="front", frame=True, ortho=False)
    s("set_camera", yaw=0.35, pitch=0.12)
    s("set_camera", distance=s("set_camera")["distance"] * 0.82)
    s("baked_maps", map="ao", select=True)
    s.x.move(*vp(0.97, 0.97))
    time.sleep(0.8)
    view = tuple(int(v) for v in vp(0.04, 0.09) + vp(0.86, 0.93))  # (the head, without the gizmo and the caption)
    s.shot("baked_ao_before", crop=view, scale=0.62, settle=0.6)
    # the fixes: lighter eye sockets, then a blur over the noise
    s("set_tool", tool="paint")
    s("set_brush", size=70, hardness=0.3, opacity=0.6, flow=0.5, spacing=0.05, mask_value=1)
    for pts in ([[0.40, 0.38], [0.44, 0.40], [0.46, 0.45]], [[0.56, 0.38], [0.61, 0.40], [0.64, 0.45]]):
        s("paint_stroke", points=pts)
    uid = s("baked_maps")["maps"][0]["layer"]["index"]
    s("update_layer", index=uid, mask_effects=[{"type": "blur", "radius": 3}])
    time.sleep(0.8)
    s.shot("baked_ao_after", crop=view, scale=0.62, settle=0.6)
    # the layer list: the stack, then the Baked maps section
    L = rect(s, "window.Layers")
    rows = [rect(s, "layers.row:%d" % m["layer"]["uid"]) for m in s("baked_maps")["maps"] if m["layer"]]
    s.shot("baked_layers_list", crop=(L[0], L[1], L[2], max(r[3] for r in rows) + 14), settle=0.4)
    # the AO layer's properties (the panel scrolled down to them)
    s.x.move((L[0] + L[2]) // 2, L[3] - 40)
    time.sleep(0.2)
    for _ in range(11):
        s.x.button(5, True)
        s.x.button(5, False)
        time.sleep(0.03)
    time.sleep(0.6)
    v = rect(s, "baked.value")
    s.shot("baked_layer_props", crop=(L[0], max(L[1] + 42, v[1] - 238), L[2], min(L[3], v[3] + 210)), settle=0.4)
    s.x.move((L[0] + L[2]) // 2, L[1] + 120)
    for _ in range(14):
        s.x.button(4, True)
        s.x.button(4, False)
        time.sleep(0.03)
    s("select_layer", index=len(s("list_layers")) - 1)
    time.sleep(0.4)


def run_0430(s):
    """Patina 0.43: the Island brush's Screen and Surface. One click just inside the outline of a sphere, where the
    surface is seen edge-on: the circle on screen takes a long strip round the side, Surface a patch of the brush's
    size on the surface."""
    print("0.43: Island brush reach")
    s("new_project", primitive="sphere", resolution=1024)
    layout(s)
    s("set_view", mode="solid", wireframe=True, seams=False)
    s("set_camera", view="front", frame=True, ortho=False)
    s("set_camera", pitch=0.25)
    s("set_camera", distance=s("set_camera")["distance"] * 1.7)
    tool(s, "island")
    s("set_symmetry", x=False, y=False, z=False)
    s("set_brush", size=150)
    row = 0.5
    xs = [k / 200 for k in range(6, 195) if s("pick_face", point=[k / 200, row])["hit"]]
    x = xs[0] + (xs[-1] - xs[0]) * 0.95  # (just inside the right-hand outline)
    view = tuple(int(v) for v in vp(0.22, 0.14) + vp(0.84, 0.86))
    for reach in ("screen", "surface"):
        click(s, "btn.island.reach_" + reach)
        s("select_faces", mode="clear")
        s.x.move(*vp(x, row))
        time.sleep(0.3)
        s.x.click(*vp(x, row))
        time.sleep(0.5)
        print("   %s: %d faces" % (reach, s("select_faces", mode="add", faces=[])["selected"]))
        s.shot("island_reach_" + reach, crop=view, scale=0.75, settle=0.6)
    s("select_faces", mode="clear")
    s("set_view", mode="material", wireframe=False)


def write_skin_and_shell(folder, nu=48, nv=24):
    """A skin (a sphere, material skin) inside a shell open at the front and back (material shell), each a UV island
    of its own, as an OBJ with its MTL. Returns (path, skin faces)."""
    import math
    lines, nskin = ["mtllib skin_and_shell.mtl"], 0
    vbase = 0
    for name, radius, opening in (("skin", 0.8, None), ("shell", 1.0, 0.7)):
        verts, faces = [], []
        for j in range(nv + 1):
            th = math.pi * j / nv
            for i in range(nu + 1):
                ph = 2 * math.pi * i / nu
                verts.append(((radius * math.sin(th) * math.cos(ph), radius * math.cos(th), radius * math.sin(th) * math.sin(ph)),
                              (i / nu, 1 - j / nv)))
        for j in range(nv):
            for i in range(nu):
                q = (j * (nu + 1) + i, j * (nu + 1) + i + 1, (j + 1) * (nu + 1) + i + 1, (j + 1) * (nu + 1) + i)
                if opening is not None and abs(sum(verts[k][0][2] for k in q) / 4) > opening * radius:
                    continue
                faces.append(q)
        lines += ["v %.6f %.6f %.6f" % v[0] for v in verts] + ["vt %.6f %.6f" % v[1] for v in verts]
        lines += ["usemtl " + name] + ["f " + " ".join("%d/%d" % (vbase + k + 1, vbase + k + 1) for k in q) for q in faces]
        vbase += len(verts)
        nskin = nskin or len(faces)
    path = os.path.join(folder, "skin_and_shell.obj")
    open(path, "w").write("\n".join(lines) + "\n")
    open(os.path.join(folder, "skin_and_shell.mtl"), "w").write(
        "newmtl skin\nKd 0.85 0.62 0.5\nnewmtl shell\nKd 0.25 0.4 0.75\n")
    return path, nskin


def run_0450(s):
    """Patina 0.45: the Select tool's Visible only. A skin (its own UV island and texture set, shown in the UV editor)
    inside a shell open at the front: a click on the skin through the opening. Off: the skin's whole island is
    selected; on: only the part seen through the opening."""
    import shutil
    import tempfile
    print("0.45: Select, Visible only")
    folder = tempfile.mkdtemp(prefix="patina_shots_0450_")
    path, nskin = write_skin_and_shell(folder)
    s("open_mesh", path=path, resolution=1024)
    layout(s)
    tool(s, "select")
    s("set_view", mode="material", wireframe=False, seams=False, texture_set=0)  # (the UV editor shows the skin)
    s("set_camera", view="front", frame=True)
    s("set_camera", pitch=0.12)
    crop = (VIEW[0], VIEW[1], UV[2], VIEW[3])
    for on in (False, True):
        if on:
            click(s, "btn.select.visible")
        s("select_faces", mode="clear")
        s.x.click(*vp(0.5, 0.5))
        s.settle(6)
        n = s("select_faces", mode="add", faces=[])["selected"]
        print("   visible only %s: %d of %d skin faces" % (on, n, nskin))
        s.x.move(*vp(0.97, 0.97))
        s.shot("select_visible_" + ("on" if on else "off"), crop=crop, scale=0.55, settle=0.8)
    s("select_faces", mode="clear")
    shutil.rmtree(folder, ignore_errors=True)


def run_0460(s):
    """Patina 0.46: the Guided layout (the session starts in it). The start (the Model step with nothing open), then
    the painted Suzanne in each step; the Full layout of the same work, and the switch between them."""
    print("0.46: the Guided layout")
    W = s.image().size[0]
    s.x.move(900, 500)
    s.shot("guided_start", scale=0.6, settle=0.8)
    build_suzanne(s)
    s("layout", step="model")
    s.settle(8)
    for st in ("uvs", "bake", "paint", "export"):
        s("layout", step=st)
        s.settle(10)
        if st == "paint":
            VIEW_ = rect(s, "viewport.image")
            s.x.move(int(VIEW_[0] + 0.62 * (VIEW_[2] - VIEW_[0])), int(VIEW_[1] + 0.55 * (VIEW_[3] - VIEW_[1])))
        else:
            s.x.move(W - 40, 900)
        s.shot("guided_" + st, scale=0.6, settle=1.0)
    bar = rect(s, "guided.step.model")
    s.shot("guided_bar", crop=(0, 0, W, bar[3] + 6), scale=0.75)
    # the Full layout: every panel docked (the overview of the interface page)
    click(s, "layout.full")
    s.settle(10)
    layout(s)
    s.x.move(*vp(0.62, 0.62))
    s.shot("overview")
    g = rect(s, "layout.guided")
    s.shot("layout_switch", crop=(g[0] - 260, 0, W, g[3] + 4))


def run_0470(s):
    """Patina 0.47: the steps' colors (the Guided layout's figures again), the UV menu in groups, the export window
    with the files it writes."""
    run_0460(s)
    print("0.47: the UV menu, the export window")
    menu(s, "uv")
    s.shot("menu_uv", crop=menu_crop(s), settle=0.8)
    s.x.key("Escape")
    s.settle(4)
    s("open_window", window="export", preset="unreal")
    s.settle(10)
    s.x.move(20, 900)
    s.shot("export_dialog", crop=window_crop(s, "Export Textures"))
    s.x.key("Escape")


def run_1100(s):
    """Patina 1.1: the bake-down. Suzanne painted (copper, rust painted with its material, red strokes), made into
    quads by the retopology, and the layers baked down onto the quads: the window with its report, the check view,
    and the compare view (the high-poly's triangles left of the divider, the quads with the paint right of it)."""
    import shutil
    import tempfile
    print("1.1: the bake-down")
    work = tempfile.mkdtemp(prefix="patina_bakedown_fig_")
    try:
        high = os.path.join(work, "Suzanne.glb")  # (the quads are saved next to it: not into the samples folder)
        shutil.copy(SUZANNE, high)
        s("layout", mode="full")
        s("open_file", path=high)
        s("set_resolution", resolution=2048)
        s("set_camera", view="front", frame=True)
        s("add_layer", type="fill", material={"id": "patina_copper"})
        s("add_layer", type="paint")
        s("set_paint_material", id="patina_rust_heavy")
        s("set_brush", size=70, opacity=1, flow=1, hardness=0.6)
        s("paint_stroke", points=[[0.32, 0.30], [0.45, 0.36], [0.58, 0.33], [0.70, 0.30]])
        s("paint_stroke", points=[[0.40, 0.70], [0.50, 0.66], [0.60, 0.70]])
        s("add_layer", type="paint")
        s("set_paint_material", id="", color=[0.85, 0.12, 0.1], roughness=0.35, metallic=0)
        s("set_brush", size=18, opacity=1, flow=1, hardness=0.9)
        s("paint_stroke", points=[[0.26, 0.52], [0.36, 0.48], [0.46, 0.53], [0.56, 0.48], [0.66, 0.53], [0.76, 0.49]])
        s("retopo_density", quads_per_m2=900)
        s("retopo_build")
        r = s("retopo_use_as_model")
        print("   bake-down:", {k: r["bake_down"][k] for k in ("hit_fraction", "nearest_point_fraction", "guessed_fraction", "seconds")})
        s("set_camera", view="front", frame=True)
        s("open_window", window="bake_down")
        s.settle(10)
        s.x.move(20, 900)
        s.shot("bakedown_window", crop=window_crop(s, "Bake Down to a Low-Poly Model"))
        s.x.key("Escape")
        s.settle(4)
        s("set_view", mode="bake_check")
        s.shot("bakedown_check", crop=CROP_VIEWPORT, scale=0.7, settle=1.0)
        s("set_view", mode="material", wireframe=True, seams=False)
        s("set_compare", enabled=True, mode="high_poly", split=0.5)
        s.x.move(20, 900)
        s.shot("bakedown_compare", crop=CROP_VIEWPORT, settle=1.0)
        s("set_compare", enabled=False, mode="snapshot")
        s("set_view", wireframe=False, seams=True)
    finally:
        shutil.rmtree(work, ignore_errors=True)


def hat_obj(path, cx, cy, cz, r, h, seg=32):
    """a hat of quads: a crown (with a band of two rings), a flat top and a brim, its own UVs"""
    import math
    out, vt = ["o Hat"], []
    verts = []

    def v(x, y, z, u, w):
        verts.append((x, y, z))
        vt.append((u, w))
        return len(verts)

    rings = [(r * 1.18, 0.0, 0.0), (r * 1.18 * 0.8 + r * 0.2, 0.0, 0.1), (r, 0.0, 0.2), (r, 0.12 * h, 0.3),
             (r * 0.99, 0.24 * h, 0.4), (r * 0.97, 0.5 * h, 0.6), (r * 0.93, 0.8 * h, 0.8), (r * 0.88, h, 0.9),
             (r * 0.45, h * 1.02, 0.95), (r * 0.05, h * 1.03, 1.0)]
    ids = []
    for (rr, yy, vv) in rings:
        row = []
        for i in range(seg + 1):
            a = 2 * math.pi * i / seg
            row.append(v(cx + rr * math.cos(a), cy + yy, cz + rr * math.sin(a), i / seg, vv))
        ids.append(row)
    for j in range(len(ids) - 1):
        for i in range(seg):
            a, b, c, d = ids[j][i], ids[j][i + 1], ids[j + 1][i + 1], ids[j + 1][i]
            out.append("f %d/%d %d/%d %d/%d %d/%d" % (a, a, d, d, c, c, b, b))
    open(path, "w").write("\n".join(["v %f %f %f" % p for p in verts] + ["vt %f %f" % t for t in vt] + out) + "\n")


def run_1200(s):
    """Patina 1.2: Suzanne with a hat as two objects (the Objects list, the Object tool), the hat's face sets (a band
    by its loop, the brim by its flat region, the crown), and brushes of your own in the Brushes panel."""
    import shutil
    import tempfile
    print("1.2: objects, face sets, your brushes")
    work = tempfile.mkdtemp(prefix="patina_scene_fig_")
    try:
        head = os.path.join(work, "Suzanne.glb")
        shutil.copy(SUZANNE, head)
        s("layout", mode="full")
        s("open_file", path=head)
        s("set_resolution", resolution=2048)
        b = s("list_objects")["objects"][0]["bounds"]
        top, cx, cz = b["max"][1], (b["min"][0] + b["max"][0]) / 2, (b["min"][2] + b["max"][2]) / 2
        width = b["max"][0] - b["min"][0]
        hat = os.path.join(work, "hat.obj")
        hat_obj(hat, cx, top - 0.12 * width, cz - 0.05 * width, 0.33 * width, 0.42 * width)
        s("add_layer", type="fill", material={"id": "patina_skin_medium"})
        s("add_object", path=hat, select=True)
        s("set_object", object="Hat", rotation=[0, 0, -12], location=[0.04 * width, -0.02 * width, 0])
        s("add_layer", type="fill", material={"id": "patina_leather_brown"})
        # the hat's face sets: its band by a loop, its brim by its flat region, the rest of the crown
        hat_faces = s("list_objects")["objects"][1]["faces"]
        seg = 32
        s("select_loop", face=3 * seg + 5, edge=0)
        s("make_face_set", name="Band", color=[0.85, 0.2, 0.15])
        s("select_faces", mode="set", faces=list(range(0, 2 * seg)))
        s("make_face_set", name="Brim", color=[0.25, 0.55, 0.9])
        s("select_faces", mode="set", faces=[f for f in range(2 * seg, hat_faces) if not (3 * seg <= f < 4 * seg)])
        s("make_face_set", name="Crown", color=[0.95, 0.75, 0.25])
        s("select_faces", mode="clear")
        s("add_layer", type="fill", material={"id": "patina_velvet_red"})
        s("use_face_set", set="Band", action="mask")
        s("set_camera", view="front", frame=True)
        s("set_camera", yaw=0.35, pitch=0.18)
        s("frame_object", all=True)
        s("set_tool", tool="object")
        s.settle(8)
        s.x.move(20, 900)
        s.shot("scene_objects", scale=0.62, settle=1.0)
        s("set_tool", tool="select")
        s("set_view", mode="face_sets")
        s("open_window", window="face_sets")
        s.settle(6)
        s.shot("face_sets_view", crop=CROP_VIEWPORT, scale=0.7, settle=1.0)
        s.shot("face_sets_panel", crop=window_crop(s, "Face Sets"), settle=0.6)
        s("set_view", mode="material")
        # brushes of your own: three images in the brushes folder
        folder = s("reload_brushes")["folder"]
        os.makedirs(os.path.join(folder, "Weathering"), exist_ok=True)
        from PIL import Image, ImageDraw, ImageFilter  # noqa: PLC0415
        import random
        rnd = random.Random(7)
        for name, kind in (("Leaf", 0), ("Weathering/Speckles", 1), ("Weathering/Cracks", 2)):
            im = Image.new("L", (256, 256), 255)
            d = ImageDraw.Draw(im)
            if kind == 0:
                d.ellipse((60, 20, 196, 236), fill=0)
                d.line((128, 20, 128, 236), fill=255, width=6)
            elif kind == 1:
                for _ in range(120):
                    x, y, rr = rnd.randint(10, 246), rnd.randint(10, 246), rnd.randint(2, 9)
                    d.ellipse((x - rr, y - rr, x + rr, y + rr), fill=0)
            else:
                x, y = 128, 10
                for _ in range(18):
                    nx, ny = x + rnd.randint(-30, 30), y + rnd.randint(8, 18)
                    d.line((x, y, nx, ny), fill=0, width=4)
                    x, y = nx, ny
            im.filter(ImageFilter.GaussianBlur(1.2)).save(os.path.join(folder, name + ".png"))
        s("reload_brushes")
        s("open_window", window="brushes")
        s.settle(6)
        s.shot("brushes_yours", crop=window_crop(s, "Brushes"), settle=0.6)
        for name in ("Leaf", "Weathering/Speckles", "Weathering/Cracks"):
            os.remove(os.path.join(folder, name + ".png"))
        os.rmdir(os.path.join(folder, "Weathering"))
        s("reload_brushes")
    finally:
        shutil.rmtree(work, ignore_errors=True)


def run_blender_keymap(s):
    """0.3.1: Preferences window and the Blender keymap. (1.2.14: F's dial is in run_1214's tip_cursor_dials.)"""
    print("0.3.1: preferences / Blender keymap")
    s("set_camera", view="front", frame=True)
    s("open_window", window="preferences", keymap="blender")
    time.sleep(1.0)
    s.x.move(900, 800)
    w = rect(s, "window.Preferences")
    s.shot("preferences", crop=(w[0], w[1], w[2], min(w[3], w[1] + 330)), settle=0.5)
    s.x.key("Escape")
    time.sleep(0.5)


def run_welcome(s):
    """0.4: the welcome window on a fresh start."""
    print("0.4: welcome")
    s.x.move(900, 900)
    time.sleep(1.2)
    s.shot("welcome", crop=window_crop(s, "Welcome to Patina"), settle=0.2)


def write_spheres(folder):
    """a low-poly UV sphere and a 'golf ball' sculpt of it, as OBJ files"""
    import math
    def sphere(path, nu, nv, bump):
        dirs = []
        n = 90
        for i in range(n):  # fibonacci dimple centers
            y = 1 - 2 * (i + 0.5) / n
            r = math.sqrt(1 - y * y)
            a = i * math.pi * (3 - math.sqrt(5))
            dirs.append((r * math.cos(a), y, r * math.sin(a)))
        with open(path, "w") as f:
            for j in range(nv + 1):
                th = math.pi * j / nv
                for i in range(nu + 1):
                    ph = 2 * math.pi * i / nu
                    d = (math.sin(th) * math.cos(ph), math.cos(th), math.sin(th) * math.sin(ph))
                    r = 1.0
                    if bump:
                        best = max(d[0] * c[0] + d[1] * c[1] + d[2] * c[2] for c in dirs)
                        ang = math.acos(min(1.0, best))
                        r -= 0.035 * math.exp(-(ang / 0.09) ** 2)
                    f.write("v %.6f %.6f %.6f\n" % (d[0] * r, d[1] * r, d[2] * r))
                    f.write("vt %.6f %.6f\n" % (i / nu, 1 - j / nv))
            w = nu + 1
            for j in range(nv):
                for i in range(nu):
                    a, b, c, e = j * w + i + 1, j * w + i + 2, (j + 1) * w + i + 2, (j + 1) * w + i + 1
                    f.write("f %d/%d %d/%d %d/%d %d/%d\n" % (a, a, e, e, c, c, b, b))
    low, high = os.path.join(folder, "ball_low.obj"), os.path.join(folder, "ball_high.obj")
    sphere(low, 48, 24, False)
    sphere(high, 400, 200, True)
    return low, high


def run_04(s):
    """Patina 0.4: island brush, texel density, heat maps, smart masks, render, high-poly bake."""
    build_suzanne(s)
    layout(s)
    s("set_camera", view="front", frame=True)
    s.x.move(*vp(0.5, 0.5))
    time.sleep(0.4)
    print("0.4: UV menu")
    menu(s, "uv")
    s.shot("menu_uv", crop=menu_crop(s), settle=0.8)
    s.x.key("Escape")
    time.sleep(0.5)
    print("0.4: island brush")
    s.x.move(*vp(0.5, 0.5))
    time.sleep(0.3)
    s.x.key("l")
    time.sleep(0.6)
    s("set_brush", size=70)
    s("set_view", mode="uv_checker")
    a, b = vp(0.36, 0.30), vp(0.64, 0.30)
    s.x.drag([a, (a[0] + 25, a[1] - 2), (a[0] + 60, a[1] + 4), (a[0] + 100, a[1] + 6), (b[0] - 40, b[1] + 4), b], dt=0.3)
    time.sleep(1.0)
    s.x.move(*vp(0.55, 0.62))
    s.shot("island_painted", crop=CROP_VIEWPORT, scale=0.7, settle=0.8)
    s.x.key("Return")
    time.sleep(2.5)
    s.shot("island_made", crop=(CROP_VIEWPORT[0], CROP_VIEWPORT[1], CROP_UV[2], CROP_UV[3]), scale=0.75, settle=1.0)
    s("set_view", mode="material")
    print("0.4: texel density")
    s("open_window", window="texel_density")
    time.sleep(0.8)
    s("set_view", mode="texel_density")
    s.x.move(900, 900)
    s.shot("density_window", crop=window_crop(s, "Texel density"), settle=1.0)
    s.x.key("Escape")
    time.sleep(0.5)
    s("set_camera", yaw=0.45, pitch=0.18)
    s.shot("density_heatmap", crop=CROP_VIEWPORT, scale=0.7, settle=1.0)
    s("set_view", mode="uv_stretch")
    s.shot("stretch_heatmap", crop=CROP_VIEWPORT, scale=0.7, settle=1.0)
    s("set_view", mode="material")
    print("0.4: facing mask")
    s("undo")                         # the island: back to the original layout
    s("select_faces", mode="clear")
    s("add_layer", type="fill", name="Snow", material={"id": "patina_snow", "scale": 1.5},
      generator={"type": "facing", "direction": [0, 1, 0], "level": 0.62, "contrast": 0.7, "noise": 0.35, "noise_scale": 4})
    s("set_camera", yaw=0.45, pitch=0.35)
    s.x.move(900, 900)
    s.shot("facing_mask", crop=CROP_VIEWPORT, scale=0.7, settle=1.2)
    print("0.4: render dialog")
    s.x.move(*vp(0.5, 0.5))
    time.sleep(0.3)
    s.x.key("F12")
    time.sleep(1.0)
    s.x.move(900, 900)
    s.shot("render_dialog", crop=window_crop(s, "Render image"), settle=0.8)
    s.x.key("Escape")
    time.sleep(0.5)
    print("0.4: high-poly bake")
    low, high = write_spheres(s.home)
    s("open_file", path=low)
    s("set_resolution", resolution=2048)
    s("bake_mesh_maps", ao=True, curvature=True)
    s("add_layer", type="fill", name="Plastic", material={"id": "", "color": [0.92, 0.92, 0.9], "roughness": 0.35})
    r = s("bake_from_high_poly", path=high, distance=0.08)
    print("   ", {k: r.get(k) for k in ("hit_fraction", "seconds")})
    s("set_camera", view="front", frame=True)
    s("set_camera", yaw=0.3, pitch=0.25)
    s("open_window", window="high_poly")
    time.sleep(0.8)
    s.x.move(900, 900)
    s.shot("highpoly_dialog", crop=window_crop(s, "Bake from a high-poly mesh"), settle=0.8)
    s.x.key("Escape")
    time.sleep(0.5)
    s.shot("highpoly_result", crop=CROP_VIEWPORT, scale=0.7, settle=1.0)
    print("0.4: rendered image")
    s("render_image", path=os.path.join(s.out, "render_example.png"), width=640, height=400, supersample=2)


def run_041(s):
    """Patina 0.4.1: topology lock and island lock on the brush bar."""
    print("0.4.1: stroke locks")
    out = s.out
    s("open_file", path=SUZANNE)
    layout(s)
    s("set_resolution", resolution=2048)
    s("add_layer", type="fill", name="Base", material={"id": "", "color": [0.78, 0.78, 0.76], "roughness": 0.5})
    s("add_layer", type="paint", name="Paint")
    s("set_view", seams=False)
    s("set_symmetry", x=False, y=False, z=False, radial=1)
    s("set_tool", tool="paint")
    s("set_paint_material", id="", color=[0.85, 0.12, 0.08], roughness=0.4)
    s("set_brush", preset="basic_hard", size=46, hardness=0.85, flow=1, opacity=1, spacing=0.08,
      projection="screen", lock_topology=False, lock_island=False)
    s("set_camera", view="front", frame=True)
    s("set_camera", yaw=0.25, pitch=0.05)
    stroke = [[0.22, 0.40], [0.30, 0.41], [0.38, 0.42], [0.46, 0.43], [0.52, 0.44]]
    s("paint_stroke", points=stroke)
    s.x.move(900, 900)
    s.shot("lock_topology_off", crop=CROP_VIEWPORT, scale=0.6, settle=1.0)
    s("undo")
    s("set_brush", lock_topology=True)
    s("paint_stroke", points=stroke)
    s.shot("lock_topology_on", crop=CROP_VIEWPORT, scale=0.6, settle=1.0)
    s("undo")
    s("set_brush", lock_topology=False)
    compose([(os.path.join(out, "lock_topology_off.png"), "No lock: the stroke runs over the eye"),
             (os.path.join(out, "lock_topology_on.png"), "Topology lock: the eye is a separate piece and stays clean")],
            os.path.join(out, "lock_topology.png"))
    # island lock on a cube: every side is its own UV island
    s("new_project", primitive="cube", resolution=2048)
    s("add_layer", type="paint", name="Paint")
    s("set_view", seams=True)
    s("set_camera", view="front", frame=True)
    s("set_camera", yaw=0.6, pitch=0.35)
    s("set_paint_material", id="", color=[0.12, 0.4, 0.9], roughness=0.4)
    across = [[0.30, 0.55], [0.40, 0.55], [0.50, 0.56], [0.60, 0.57], [0.70, 0.57]]
    s("paint_stroke", points=across)
    s.shot("lock_island_off", crop=CROP_VIEWPORT, scale=0.6, settle=1.0)
    s("undo")
    s("set_brush", lock_island=True)
    s("paint_stroke", points=across)
    s.shot("lock_island_on", crop=CROP_VIEWPORT, scale=0.6, settle=1.0)
    s("undo")
    compose([(os.path.join(out, "lock_island_off.png"), "No lock: the stroke crosses the seam"),
             (os.path.join(out, "lock_island_on.png"), "Island lock: it stops at the edge of the island it started on")],
            os.path.join(out, "lock_island.png"))
    s("set_brush", lock_island=True, lock_topology=True)
    s.x.move(*center(s, "btn.lock_island"))
    b = rect(s, "btn.lock_island")
    s.shot("lock_bar", crop=(CROP_VIEWPORT[0], CROP_VIEWPORT[1], CROP_VIEWPORT[2], max(VIEW[1], b[3] + 40)), settle=1.0)
    s("set_brush", lock_island=False, lock_topology=False)
    for n in ("lock_topology_off", "lock_topology_on", "lock_island_off", "lock_island_on"):
        os.remove(os.path.join(out, n + ".png"))


def run_05(s):
    """Patina 0.5: the HTTP API section of Preferences."""
    print("0.5: HTTP API preferences")
    s("open_window", window="preferences", section="http")
    time.sleep(1.2)
    click(s, "prefs.http_enable", hold=0.3)   # Enable
    time.sleep(1.5)
    s.x.move(900, 900)
    a, w = rect(s, "prefs.http_section"), rect(s, "window.Preferences")
    s.shot("prefs_http", crop=(w[0], a[1] - 4, w[2], a[3] + 4), settle=0.8)
    click(s, "prefs.http_enable", hold=0.3)   # off again
    time.sleep(0.5)
    s.x.key("Escape")


def run_051(s):
    """Patina 0.5.1: the pen tablet test window, with synthetic pen strokes."""
    import math
    print("0.5.1: pen tablet test")
    s("open_window", window="pen_test")
    time.sleep(1.0)
    pad = rect(s, "pen.pad")         # inside the scribble pad (window pixels)
    strokes = []
    for k in range(4):
        y0 = pad[1] + 60 + k * 100
        pts = []
        for i in range(70):
            t = i / 69
            x = pad[0] + 20 + t * (pad[2] - pad[0] - 40)
            y = y0 + 22 * math.sin(t * math.pi * 2.5 + k)
            p = [t, 1 - t, math.sin(t * math.pi), 0.15 + 0.85 * abs(math.sin(t * math.pi * 3))][k]
            pts.append(dict(x=x, y=y, pressure=max(0.03, p), down=True, tilt_x=25.0, tilt_y=-10.0))
        pts.append(dict(x=x, y=y, pressure=0, down=False))
        strokes += pts
    s("inject_pen", space="window", samples=strokes)
    for _ in range(600):
        if not s("pen_injecting")["busy"]:
            break
        time.sleep(0.05)
    time.sleep(1.0)
    s.x.move(1500, 900)
    s.shot("pen_test", crop=window_crop(s, "Pen tablet test"), settle=0.8)
    s.x.key("Escape")


def run_06(s):
    """Patina 0.6: mask effect stacks, thumbnails and instances, smudge, seam blur, stencil and text, notes, compare."""
    import tempfile
    out = s.out
    print("0.6: layers")
    build_suzanne(s)
    layout(s)
    names = {l["name"]: l["index"] for l in s("list_layers")}
    s("select_layer", index=names["Edge rust"])
    s("add_mask_effect", effect={"type": "noise", "blend": "multiply", "scale": 3, "contrast": 0.6})
    s("add_mask_effect", effect={"type": "levels", "in_low": 0.1, "in_high": 0.8})
    s("add_mask_effect", effect={"type": "blur", "radius": 3})
    s("select_layer", index=names["Gold trim"])
    s("add_instance")
    s("select_layer", index=names["Edge rust"])
    time.sleep(1.5)
    s.x.move(1400, 300)
    s.shot("layers_thumbs", crop=CROP_LAYERS, settle=2.5)
    for _ in range(12):  # scroll the layer settings down to the mask stack
        s.x.move(1400, 380)
        s.x.button(5, True)
        s.x.button(5, False)
        time.sleep(0.05)
    s.x.move(1400, 380)
    s.shot("mask_stack", crop=CROP_LAYERS, settle=1.0)

    print("0.6: stencil and text")
    s("add_layer", type="paint", name="Decal")
    s("set_tool", tool="paint")
    s("set_stencil", text="PATINA", font="sans-bold", font_size=160, color=[0.95, 0.85, 0.2], outline=5,
      mode="decal", position=[0.5, 0.45], scale=0.12, rotation=-8, overlay_opacity=0.45)
    s.x.move(900, 900)
    s.shot("stencil_overlay", crop=CROP_VIEWPORT, settle=1.5)
    s("project_stencil")
    s("set_stencil", show_overlay=False)
    s.shot("stencil_projected", crop=CROP_VIEWPORT, settle=1.5)
    s("set_stencil", show_overlay=True)
    panel(s, "properties")
    click(s, "props.brush_header")      # collapse "Brush"
    time.sleep(0.5)
    click(s, "props.stencil_header")    # open "Stencil & text"
    time.sleep(0.5)
    s.x.move(1590, 930)
    s.shot("stencil_panel", crop=CROP_RIGHT_LOWER, settle=1.0)
    click(s, "props.stencil_header")    # close it again, open "Brush"
    time.sleep(0.4)
    click(s, "props.brush_header")
    s("set_stencil", clear=True)

    print("0.6: notes and compare")
    s("update_layer", index=names["Edge rust"], visible=False)
    s("set_compare", enabled=True, mode="snapshot", split=0.5)
    s("update_layer", index=names["Edge rust"], visible=True)
    s("set_compare", snapshot=True)
    s("update_layer", index=names["Edge rust"], visible=False)
    s("add_note", text="Edge rust: too strong on the brow, soften the mask", point=[0.45, 0.45], author="rob")
    s("add_note", text="Gold trim reads well", point=[0.52, 0.3], author="rob")
    n = s("add_note", text="Check the ear seam", point=[0.68, 0.5], author="rob")
    s("update_note", id=n["id"], resolved=True)
    s.x.move(900, 900)
    s.shot("compare_notes", crop=CROP_VIEWPORT, settle=2.0)
    panel(s, "notes")
    s.x.move(1400, 900)
    s.shot("notes_panel", crop=CROP_RIGHT_LOWER, settle=1.0)
    s("set_compare", enabled=False)
    s("update_layer", index=names["Edge rust"], visible=True)

    print("0.6: smudge")
    s("new_project", primitive="plane", resolution=1024)
    s("set_camera", view="top", frame=True)
    s("set_view", mode="basecolor", seams=False, wireframe=False)
    s("add_layer", type="paint", name="Stripes")
    s("set_tool", tool="paint")
    s("set_brush", preset="basic_hard", size=26, hardness=0.9, flow=1, opacity=1, spacing=0.05, projection="screen",
      lock_topology=False, lock_island=False)
    for k, col in enumerate([[0.85, 0.15, 0.1], [0.95, 0.75, 0.15], [0.15, 0.45, 0.85], [0.2, 0.7, 0.35]]):
        s("set_paint_material", id="", color=col)
        y = 0.38 + k * 0.08
        s("paint_stroke", points=[[0.28, y], [0.72, y]])
    s.x.move(900, 900)
    s.shot("smudge_before", crop=(200, 200, 600, 560), settle=1.0)
    s("set_tool", tool="smudge")
    s("set_brush", size=44, hardness=0.5, flow=0.6)
    for k in range(4):
        x = 0.36 + k * 0.09
        s("paint_stroke", points=[[x, 0.34], [x, 0.66]])
    s.shot("smudge_after", crop=(200, 200, 600, 560), settle=1.0)
    compose([(os.path.join(out, "smudge_before.png"), "Four painted stripes"),
             (os.path.join(out, "smudge_after.png"), "Four smudge strokes, top to bottom")],
            os.path.join(out, "smudge.png"))

    print("0.6: seam blur")
    s("new_project", primitive="sphere", resolution=1024)
    s("set_camera", view="right", frame=True)
    s("set_view", mode="material", seams=True, wireframe=False)
    s("add_layer", type="paint", name="Red")
    s("set_tool", tool="paint")
    s("set_paint_material", id="", color=[0.85, 0.12, 0.08])
    s("set_brush", preset="basic_hard", size=120, hardness=0.95, projection="surface", lock_island=False)
    # paint just one side of the seam (UV space keeps it to one island edge)
    s("set_brush", projection="uv", size=60)
    s("paint_stroke", space="uv", points=[[0.97, 0.35], [0.97, 0.65]])
    s("set_brush", projection="screen")
    s("set_tool", tool="blur", blur_radius=14, seam_aware=False)
    blur = [[0.5, 0.3], [0.5, 0.7]]
    s("paint_stroke", points=blur, brush={"size": 200, "hardness": 0.4})
    s.x.move(900, 900)
    s.shot("seam_blur_off", crop=(250, 180, 550, 580), settle=1.0)
    s("undo")
    s("set_tool", tool="blur", seam_aware=True)
    s("paint_stroke", points=blur, brush={"size": 200, "hardness": 0.4})
    s.shot("seam_blur_on", crop=(250, 180, 550, 580), settle=1.0)
    compose([(os.path.join(out, "seam_blur_off.png"), "Blur that stops at the seam"),
             (os.path.join(out, "seam_blur_on.png"), "Across UV seams (0.6)")], os.path.join(out, "seam_blur.png"))
    print("0.6: symmetric island")
    s("open_file", path=SUZANNE)
    s("set_resolution", resolution=1024)
    s("set_camera", view="front", frame=True)
    s("set_view", mode="uv_checker", seams=True, wireframe=False)
    s("set_symmetry", x=True)
    s.x.move(*vp(0.5, 0.95))
    time.sleep(0.3)
    s.x.key("l")  # Island brush
    time.sleep(0.4)
    s("set_brush", size=40)
    pts = []
    for k in range(4):
        y = 0.38 + k * 0.035
        pts += [vp(0.77, y), vp(0.84, y)]
    s.x.drag(pts, dt=0.06)
    time.sleep(0.4)
    s.x.move(*vp(0.5, 0.95))
    s.x.key("Return")  # make the island
    time.sleep(1.5)
    s.x.move(900, 900)
    s.shot("island_sym_view", crop=CROP_VIEWPORT, settle=1.0)
    s.shot("island_sym_uv", crop=CROP_UV, settle=0.5)
    compose([(os.path.join(out, "island_sym_view.png"), "Island brush with Sym X: both ears"),
             (os.path.join(out, "island_sym_uv.png"), "One island in the UV layout, both halves stacked")],
            os.path.join(out, "island_symmetry.png"))
    s("set_symmetry", x=False)
    for f in ("island_sym_view", "island_sym_uv"):
        os.remove(os.path.join(out, f + ".png"))
    for f in ("smudge_before", "smudge_after", "seam_blur_off", "seam_blur_on"):
        os.remove(os.path.join(out, f + ".png"))
    s("set_tool", tool="paint")


def write_belt_obj(path):
    """a belt: a flat strip of quads bent into a half ring, with uneven segment lengths"""
    import math
    n = 14
    vs, fs, vts = [], [], []
    for i in range(n + 1):
        a = math.pi * i / n + 0.12 * math.sin(i * 1.7)
        for s_ in range(2):
            r = 1.0 + s_ * 0.35
            vs.append((math.cos(a) * r, 0.04 * math.sin(i * 0.9), math.sin(a) * r))
    for i in range(n):
        a, b = i * 2 + 1, (i + 1) * 2 + 1
        fs.append((a, b, b + 1, a + 1))
    with open(path, "w") as f:
        for v in vs:
            f.write("v %f %f %f\n" % v)
        for q in fs:
            f.write("f %d %d %d %d\n" % q)


def run_07(s):
    """Patina 0.7: the beginner's guide to masks and smart materials, new bakes, rectify, export."""
    import base64
    import io
    import tempfile
    from PIL import Image
    out = s.out

    def view_image(mode=None, w=420, h=420):
        args = dict(width=w, height=h)
        if mode:
            args["mode"] = mode
        r = s("render_view", **args)
        return Image.open(io.BytesIO(base64.b64decode(r["image_png_base64"]))).convert("RGB")

    def save(im, name):
        p = os.path.join(out, name + ".png")
        im.save(p, optimize=True)
        print("  ", name, im.size)
        return p

    print("0.7: guide")
    s("set_view", mode="material", wireframe=False, seams=False)
    s("open_file", path=SUZANNE)
    layout(s)
    s("set_resolution", resolution=2048)
    s("bake_mesh_maps", ao=True, curvature=True)
    s("set_camera", view="front", frame=True)
    s("set_camera", yaw=0.45, pitch=0.18)
    # everything goes into one folder, so it can be saved as a smart material at the end
    s("add_layer", type="folder", name="Worn blue metal")
    s("add_layer", type="fill", name="Paint", material={"id": "patina_car_paint_blue", "scale": 1.5})
    steps = [save(view_image(), "_g1")]
    s("add_layer", type="fill", name="Bare steel", material={"id": "patina_steel_scratched", "scale": 1.5},
      generator={"type": "edges", "level": 0.66, "contrast": 0.85, "noise": 0.0, "noise_scale": 3})
    steps.append(save(view_image(), "_g2"))
    masks = [save(view_image("mask"), "_m1")]
    s("update_layer", generator={"type": "edges", "level": 0.64, "contrast": 0.7, "noise": 0.5, "noise_scale": 3})
    steps.append(save(view_image(), "_g3"))
    masks.append(save(view_image("mask"), "_m2"))
    s("add_layer", type="fill", name="Dirt", material={"id": "patina_dirt_mud", "scale": 2},
      generator={"type": "occlusion", "level": 0.2, "contrast": 0.5, "noise": 0.3, "noise_scale": 2})
    s("update_layer", opacity=0.85)
    steps.append(save(view_image(), "_g4"))
    masks.append(save(view_image("mask"), "_m3"))
    # a hand-painted mask: a fill layer with a black mask, then white paint in the mask
    s("add_layer", type="fill", name="Rust patch", material={"id": "patina_rust_heavy", "scale": 1.5})
    sel = [l for l in s("list_layers") if l.get("selected")][0]["index"]
    s("set_layer_mask", index=sel, value=0.0)
    s("select_layer", index=sel, edit_mask=True)
    s("set_brush", preset="basic_hard", size=70, hardness=0.2, flow=0.8, opacity=1)
    s("set_tool", tool="paint")
    s("paint_stroke", points=[[0.36, 0.62], [0.42, 0.66], [0.5, 0.67]])
    steps.append(save(view_image(), "_g5"))
    masks.append(save(view_image("mask"), "_m4"))
    compose([(steps[0], "1. Base: a fill layer of blue paint"),
             (steps[1], "2. Steel on the edges (Edges smart mask)"),
             (steps[2], "3. The same, with noise in the mask"),
             (steps[3], "4. Dirt in the creases (Occlusion mask)"),
             (steps[4], "5. Rust painted by hand into a black mask"),
             (masks[3], "...its mask: white where the brush went")], os.path.join(out, "guide_steps.png"), cols=3)
    compose([(masks[0], "Edges mask, no noise: too clean"), (masks[1], "Edges mask with noise: worn"),
             (masks[2], "Occlusion mask (the dirt)")], os.path.join(out, "guide_masks.png"), cols=3)
    time.sleep(0.6)
    s.x.move(800, 500)
    s.shot("guide_layers", crop=CROP_LAYERS, settle=1.5)
    # the painted patch only fits this model: leave it out of the smart material
    s("select_layer", index=sel, edit_mask=False)
    s("remove_layer", index=sel)
    s("set_view", mode="material")
    folder = [l for l in s("list_layers") if l.get("type") == "folder"]
    sm_path = None
    if folder:
        r = s("save_smart_material", index=folder[0]["index"], name="Worn blue metal (guide)")
        sm_path = r.get("path")
    shapes = []
    for prim in ("cube", "torus", "sphere"):
        s("new_project", primitive=prim, resolution=1024)
        s("set_camera", view="front", frame=True)
        s("set_camera", yaw=0.6, pitch=0.35)
        if sm_path:
            s("apply_smart_material", id=sm_path)
        s("set_view", seams=False)
        shapes.append(save(view_image(w=360, h=360), "_s_" + prim))
    compose([(shapes[0], "Cube"), (shapes[1], "Torus"), (shapes[2], "Sphere")],
            os.path.join(out, "guide_reuse.png"), cols=3)
    if sm_path and os.path.exists(sm_path):
        os.remove(sm_path)

    s("set_view", seams=True)

    print("0.7: bakes")
    s("open_file", path=SUZANNE)
    s("set_resolution", resolution=2048)
    s("bake_mesh_maps", ao=True, curvature=True, thickness=True)
    s("set_camera", view="front", frame=True)
    s("set_camera", yaw=0.6, pitch=0.15)
    b = [save(view_image(m, 360, 360), "_b_" + m) for m in ("ao", "curvature", "thickness", "bent_normal")]
    compose([(b[0], "Ambient occlusion"), (b[1], "Curvature"), (b[2], "Thickness (dark = thin)"),
             (b[3], "Bent normals")], os.path.join(out, "bakes_07.png"), cols=4)
    s("add_layer", type="fill", name="Thin parts", material={"id": "", "color": [0.9, 0.25, 0.15]},
      generator={"type": "thin", "level": 0.55, "contrast": 0.6})
    save(view_image(None, 420, 420), "thin_parts")

    print("0.7: rectify")
    tmp = tempfile.mkdtemp()
    belt = os.path.join(tmp, "belt.obj")
    write_belt_obj(belt)
    s("open_file", path=belt)
    s("set_resolution", resolution=1024)
    s("set_view", mode="uv_checker")
    s("set_camera", view="top", frame=True)
    s("uv_unwrap", method="planar")
    time.sleep(0.8)
    s.shot("_r_before", crop=CROP_UV, settle=1.0)
    r = s("uv_rectify")
    print("   rectify:", r)
    time.sleep(0.8)
    s.shot("_r_after", crop=CROP_UV, settle=1.0)
    compose([(os.path.join(out, "_r_before.png"), "A belt projected from above: a curved island"),
             (os.path.join(out, "_r_after.png"), "After Rectify: one straight strip")], os.path.join(out, "rectify.png"))
    s("set_view", mode="material")

    for f in os.listdir(out):
        if f.startswith("_") and f.endswith(".png"):
            os.remove(os.path.join(out, f))


def type_text(s, text):
    for ch in text:
        name = {" ": "space", "_": "underscore", "{": "braceleft", "}": "braceright", ":": "colon", '"': "quotedbl",
                ",": "comma", ".": "period", "-": "minus", "[": "bracketleft", "]": "bracketright"}.get(ch, ch)
        shift = ch in '_{}:"' or ch.isupper()
        if shift:
            s.x.key("Shift_L", True)
        s.x.key(name.lower() if len(name) == 1 else name)
        if shift:
            s.x.key("Shift_L", False)
        time.sleep(0.03)


def probe(s):
    menu(s, "file")
    s.shot("_menu_file", settle=0.5)
    s.x.key("Escape")


def run_071(s):
    """Patina 0.7.1: pinned islands, linked mirror halves, the UV editor's right-click menu."""
    out = s.out
    print("0.7.1: pins")
    s("set_view", mode="uv_checker", seams=True, wireframe=False)
    s("open_file", path=SUZANNE)
    layout(s)
    s("set_resolution", resolution=1024)
    s("set_camera", view="front", frame=True)
    s("set_symmetry", x=True)
    s.x.move(*vp(0.5, 0.95))
    time.sleep(0.3)
    s.x.key("l")  # Island brush
    time.sleep(0.4)
    s("set_brush", size=40)
    pts = []
    for k in range(4):
        y = 0.38 + k * 0.035
        pts += [vp(0.77, y), vp(0.84, y)]
    s.x.drag(pts, dt=0.06)
    time.sleep(0.4)
    s.x.move(*vp(0.5, 0.95))
    s.x.key("Return")  # make the island (pinned: the Pin box is on by default)
    time.sleep(1.5)
    s("set_symmetry", x=False)
    isl = [i for i in s("uv_islands")["islands"] if i["stacked_faces"]][0]
    b = isl["bounds"]
    cx, cy = uvxy((b[0] + b[2]) / 2, (b[1] + b[3]) / 2)
    # make the ear island bigger so the markers read well: unpin, scale up, pin again
    s("uv_pin", faces=[isl["first_face"]], pin=False)
    s("uv_transform", faces=[isl["first_face"]], islands=True, scale=2.2, pivot=[b[0], b[1]])
    s("uv_pack", free_space=True, faces=[isl["first_face"]])
    s("uv_pin", faces=[isl["first_face"]], pin=True)
    s("select_faces", mode="island", faces=[isl["first_face"]])
    isl = [i for i in s("uv_islands")["islands"] if i["stacked_faces"]][0]
    b = isl["bounds"]
    cx, cy = uvxy((b[0] + b[2]) / 2, (b[1] + b[3]) / 2)
    # try to drag it: the editor says it is pinned
    s.x.drag([(cx, cy), (cx + 12, cy), (cx + 40, cy + 15), (cx + 50, cy + 20)])
    s.x.move(cx, cy)
    time.sleep(0.5)
    s.shot("uv_pins", crop=CROP_UV, settle=0.6)
    print("0.7.1: right-click menu")
    import numpy as np
    before = np.asarray(s.image()).astype(int)
    s.x.click(cx, cy, b=3)
    time.sleep(0.8)
    im = s.image()
    # the menu is where the screen changed (ImGui moves it to stay on screen)
    diff = np.abs(np.asarray(im).astype(int) - before).sum(axis=2) > 30
    # the menu: the columns and rows where much changed (hover text elsewhere changes a little)
    cols = np.nonzero(diff.sum(axis=0) > 150)[0]
    rows = np.nonzero(diff[:, cols.min():cols.max() + 1].sum(axis=1) > 100)[0]
    box = (max(0, cols.min() - 1), max(0, rows.min() - 1), min(im.width, cols.max() + 2), min(im.height, rows.max() + 2))
    im.crop(box).save(os.path.join(out, "uv_context_menu.png"), optimize=True)
    print("   uv_context_menu")
    s.x.key("Escape")
    time.sleep(0.3)
    s("set_view", mode="material")


def _b64img(r):
    import base64
    import io
    from PIL import Image
    for v in r.values():
        if isinstance(v, str) and len(v) > 1000:
            return Image.open(io.BytesIO(base64.b64decode(v.split(",")[-1]))).convert("RGB")
    raise RuntimeError("no image in result")


def _label(im, text):
    from PIL import ImageDraw
    d = ImageDraw.Draw(im)
    d.rectangle((0, 0, im.width, 30), fill=(24, 26, 30))
    d.text((10, 8), text, fill=(235, 235, 235))
    return im


def _side_by_side(ims, path):
    from PIL import Image
    w = sum(i.width for i in ims) + 8 * (len(ims) - 1)
    h = max(i.height for i in ims)
    out = Image.new("RGB", (w, h), (24, 26, 30))
    x = 0
    for i in ims:
        out.paste(i, (x, 0))
        x += i.width + 8
    out.save(path, optimize=True)
    print("  ", os.path.basename(path), out.size)


def _tube_obj(path, seg=32, rows=12):
    """an arm-like tube: tapered and bent, with capped ends"""
    import math
    vs, fs = [], []
    for j in range(rows + 1):
        t = j / rows
        r = 0.35 * (1.0 - 0.35 * t)
        bend = 0.5 * t * t
        for i in range(seg):
            a = 2 * math.pi * i / seg
            vs.append((math.cos(a) * r + bend, t * 3.0 - 1.5, math.sin(a) * r))
    for j in range(rows):
        for i in range(seg):
            a, b = j * seg + i, j * seg + (i + 1) % seg
            fs.append((a, a + seg, b + seg, b))
    for end in (0, 1):
        c = len(vs)
        jr = rows if end else 0
        t = jr / rows
        vs.append((0.5 * t * t, t * 3.0 - 1.5, 0.0))
        for i in range(seg):
            a, b = jr * seg + i, jr * seg + (i + 1) % seg
            fs.append((c, b, a) if end else (c, a, b))
    with open(path, "w") as f:
        for v in vs:
            f.write("v %f %f %f\n" % v)
        for q in fs:
            f.write("f " + " ".join(str(k + 1) for k in q) + "\n")


def run_080(s):
    """Patina 0.8: seam brush, cylinder unwrap, relax, shape packing."""
    out = s.out
    print("0.8: seam brush")
    s("open_file", path=SUZANNE)
    s("set_resolution", resolution=1024)
    s("uv_seams", mode="clear")
    s("set_view", mode="material", seams=True, wireframe=True)
    s("set_camera", view="front", frame=True)
    layout(s)
    tool(s, "seams")
    import math
    # around the model's left eye socket, then Shift held to preview a continuation
    pts = [vp(0.64 + 0.10 * math.cos(a * 2 * math.pi / 24), 0.43 - 0.09 * math.sin(a * 2 * math.pi / 24)) for a in range(19)]
    s.x.drag(pts, dt=0.05)
    s.x.key("Shift_L", True)
    s.x.move(*vp(0.60, 0.70))
    time.sleep(0.8)
    s.shot("seam_brush", crop=CROP_VIEWPORT)
    s.x.key("Shift_L", False)

    print("0.8: cylinder unwrap")
    tube = os.path.abspath(os.path.join(s.home, "tube.obj"))
    _tube_obj(tube)
    s("open_file", path=tube)
    s("set_resolution", resolution=1024)
    s("set_view", mode="uv_checker", seams=True, wireframe=False)
    s("uv_unwrap", method="cylinder")
    v = _label(_b64img(s("render_view", width=520, height=560, camera={"frame": True, "yaw": 0.7, "pitch": 0.25})),
               "Cylinder unwrap on a bent, tapered tube")
    u = _label(_b64img(s("render_uv", width=560, height=560, channel="checker")), "Cut once and flattened (LSCM): bends and tapers stay even")
    _side_by_side([v, u], os.path.join(out, "uv_cylinder.png"))

    print("0.8: relax")
    s("open_file", path=SUZANNE)
    s("set_resolution", resolution=1024)
    s("uv_unwrap", method="smart")
    isl = max(s("uv_islands")["islands"], key=lambda i: i["faces"])
    f0 = isl["first_face"]
    s("uv_transform", faces=[f0], islands=True, scale=[1.7, 0.6])
    s("uv_transform", faces=[f0], islands=True, rotate=25)
    s("set_view", mode="uv_checker", seams=False, wireframe=False)
    cam = {"view": "front", "frame": True}
    a = _label(_b64img(s("render_view", width=480, height=480, camera=cam)), "Stretched island")
    s("uv_relax", faces=[f0])
    b = _label(_b64img(s("render_view", width=480, height=480, camera=cam)), "After Relax: square checker, same place and size")
    _side_by_side([a, b], os.path.join(out, "uv_relax.png"))

    print("0.8: packing")
    s("open_file", path=SUZANNE)
    s("set_resolution", resolution=1024)
    s("uv_unwrap", method="smart", angle=50)
    s("uv_pack", quality="fast")
    c1 = s("uv_info")["coverage"]
    a = _label(_b64img(s("render_uv", width=520, height=520, channel="checker")), "Boxes: %d%% of the texture used" % round(c1 * 100))
    s("uv_pack", quality="tight")
    c2 = s("uv_info")["coverage"]
    b = _label(_b64img(s("render_uv", width=520, height=520, channel="checker")), "Shapes: %d%% used" % round(c2 * 100))
    _side_by_side([a, b], os.path.join(out, "uv_pack_shapes.png"))
    print("   coverage boxes %.3f shapes %.3f" % (c1, c2))


def _bust_obj(path, seg=40, rings=20, udim=False):
    """a head (material Skin) on a torso (material Cloth); each laid out over the whole 0..1 square,
    the way characters' parts each get their own texture"""
    import math
    v, vt, faces = [], [], []

    def vert(p, uv):
        v.append(p)
        vt.append(uv)
        return len(v)

    # head: a sphere, longitude / latitude
    head = []
    for r in range(rings + 1):
        th = math.pi * r / rings
        row = []
        for k in range(seg + 1):
            ph = 2 * math.pi * k / seg
            p = (0.42 * math.sin(th) * math.cos(ph), 1.15 + 0.5 * math.cos(th), 0.42 * math.sin(th) * math.sin(ph))
            row.append(vert(p, (k / seg, 1 - r / rings)))
        head.append(row)
    for r in range(rings):
        for k in range(seg):
            faces.append(("Skin", [head[r][k], head[r][k + 1], head[r + 1][k + 1], head[r + 1][k]]))
    # torso: a tapered tube with shoulders
    body = []
    rows = 12
    for r in range(rows + 1):
        y = 0.62 - 1.4 * r / rows
        t = r / rows  # neck, shoulders, chest, waist
        rad = 0.16 + 0.42 * min(1.0, t * 5.0) - 0.12 * max(0.0, t - 0.35)
        row = []
        for k in range(seg + 1):
            ph = 2 * math.pi * k / seg
            row.append(vert((rad * math.cos(ph), y, 0.75 * rad * math.sin(ph)), ((1.0 if udim else 0.0) + k / seg, 1 - r / rows)))
        body.append(row)
    for r in range(rows):
        for k in range(seg):
            faces.append(("Cloth", [body[r][k], body[r][k + 1], body[r + 1][k + 1], body[r + 1][k]]))
    with open(path, "w") as f:
        f.write("mtllib bust.mtl\n")
        for p in v:
            f.write("v %.5f %.5f %.5f\n" % p)
        for t in vt:
            f.write("vt %.5f %.5f\n" % t)
        cur = None
        for mat, fv in faces:
            if udim:
                mat = "Statue"
            if mat != cur:
                f.write("usemtl %s\n" % mat)
                cur = mat
            f.write("f " + " ".join("%d/%d" % (i, i) for i in fv) + "\n")  # counter-clockwise: faces point out
    with open(os.path.join(os.path.dirname(path), "bust.mtl"), "w") as f:
        f.write("newmtl Skin\nKd 0.8 0.6 0.5\nnewmtl Cloth\nKd 0.2 0.3 0.6\nnewmtl Statue\nKd 0.7 0.7 0.7\n")


def run_090(s):
    """Patina 0.9: texture sets per material slot, UDIM tiles."""
    out = s.out
    print("0.9: texture sets")
    bust = os.path.abspath(os.path.join(s.home, "bust.obj"))
    _bust_obj(bust)
    s("open_file", path=bust)
    s("set_resolution", resolution=1024)
    t = s("texture_tiles")
    print("   tiles", t["mode"], [x["name"] for x in t["tiles"]])
    s("set_view", mode="material", seams=False, wireframe=False)
    s("set_camera", view="front", frame=True)
    s("set_paint_material", id="patina_skin_light")
    s("fill", mode="material", point=[0.5, 0.2])
    s("set_paint_material", id="patina_fabric_denim")
    s("fill", mode="material", point=[0.5, 0.7])
    cam = {"view": "front", "frame": True, "yaw": 0.35, "pitch": 0.1}
    v = _label(_b64img(s("render_view", width=520, height=560, camera=cam)), "Two material slots, a texture set each")
    a = _label(_b64img(s("render_uv", width=520, height=520, channel="basecolor", tile=0)), "Set 'Skin': its own 0-1 space")
    b = _label(_b64img(s("render_uv", width=520, height=520, channel="basecolor", tile=1)), "Set 'Cloth': the same UV area, other texels")
    _side_by_side([v, a, b], os.path.join(out, "texture_sets.png"))
    s("render_uv", width=64, height=64, tile=1)
    s.shot("texture_sets_window", settle=1.0)
    # the Texture Set panel: its tab sits in the lower right dock
    layout(s)
    panel(s, "texture_set")
    s.x.move(900, 400)
    time.sleep(0.6)
    w = rect(s, "window.Texture Set")
    s.shot("texture_sets_panel", crop=(w[0], w[1], w[2], min(w[3], w[1] + 260)))

    print("0.9: UDIM tiles")
    statue = os.path.abspath(os.path.join(s.home, "statue.obj"))
    _bust_obj(statue, udim=True)
    s("open_file", path=statue)
    s("set_resolution", resolution=1024)
    t = s("texture_tiles")
    print("   tiles", t["mode"], [x["name"] for x in t["tiles"]])
    s("set_paint_material", id="patina_marble_white")
    s("fill", mode="mesh", point=[0.5, 0.5])
    v = _label(_b64img(s("render_view", width=520, height=560, camera=cam)), "One material, UVs in UDIM 1001 and 1002")
    a = _label(_b64img(s("render_uv", width=520, height=520, channel="basecolor", tile=0)), "Tile 1001")
    b = _label(_b64img(s("render_uv", width=520, height=520, channel="basecolor", tile=1)), "Tile 1002: a texture of its own")
    _side_by_side([v, a, b], os.path.join(out, "udim_tiles.png"))


def _palette(s, text, enter=True):
    """open the command palette (Ctrl+Shift+P), type, and run the first hit"""
    s.x.move(400, 400)
    s.x.key("Control_L", True)
    s.x.key("Shift_L", True)
    s.x.key("p")
    s.x.key("Shift_L", False)
    s.x.key("Control_L", False)
    time.sleep(0.5)
    for ch in text:
        s.x.key("space" if ch == " " else ch)
        time.sleep(0.04)
    time.sleep(0.4)
    if enter:
        s.x.key("Return")
        time.sleep(0.8)


def _asym_bust(path):
    """the bust with one cheek pushed out and one shoulder raised: something to find"""
    import math
    _bust_obj(path)
    lines = open(path).read().splitlines()
    out = []
    for ln in lines:
        if ln.startswith("v "):
            x, y, z = map(float, ln.split()[1:4])
            if x > 0.12 and 1.0 < y < 1.35 and z > 0.0:  # cheek
                k = 1.0 + 0.06 * math.exp(-((y - 1.15) / 0.12) ** 2)
                x, z = x * k, z * k
            if x > 0.25 and 0.3 < y < 0.65:  # shoulder
                y += 0.035 * min(1.0, (x - 0.25) / 0.2)
            ln = "v %.6f %.6f %.6f" % (x, y, z)
        out.append(ln)
    open(path, "w").write("\n".join(out) + "\n")


def run_0100(s):
    """Patina 0.10: features, command palette, symmetry check, texture set tools, auto texture, macros."""
    out = s.out
    cam = {"view": "front", "frame": True, "yaw": 0.35, "pitch": 0.1}
    print("0.10: command palette")
    bust = os.path.abspath(os.path.join(s.home, "bust.obj"))
    _bust_obj(bust)
    s("open_file", path=bust)
    s("set_resolution", resolution=1024)
    s("set_camera", view="front", frame=True)
    _palette(s, "rust", enter=False)
    s.shot("command_palette", crop=window_crop(s, "##palette"), settle=0.3)
    s.x.key("Escape")
    time.sleep(0.4)

    print("0.10: features")
    s("open_window", window="preferences", section="features")
    time.sleep(1.2)
    a, w = rect(s, "prefs.features"), rect(s, "window.Preferences")
    s.shot("features_prefs", crop=(w[0], max(w[1], a[1] - 4), w[2], min(w[3], a[1] + 390)), settle=0.6)
    s.x.key("Escape")
    time.sleep(0.4)

    print("0.10: auto texture")
    _palette(s, "auto texture")
    s.shot("auto_texture_window", crop=window_crop(s, "Auto texture"), settle=0.8)
    s("auto_texture")
    s.x.key("Escape")
    time.sleep(0.4)
    v = _label(_b64img(s("render_view", width=520, height=560, camera=cam)), "Auto texture: Skin and Cloth")

    print("0.10: texture set isolate")
    s("set_view", isolate_set=True, texture_set=1)
    iso = _label(_b64img(s("render_view", width=520, height=560, camera=cam)), "Isolate: only the 'Cloth' set")
    s("set_view", isolate_set=False)
    _side_by_side([v, iso], os.path.join(out, "texture_set_isolate.png"))

    print("0.10: symmetry")
    asym = os.path.abspath(os.path.join(s.home, "asym_bust.obj"))
    _asym_bust(asym)
    s("open_file", path=asym)
    r = s("mesh_symmetry", show=True)
    print("   symmetry", r["verdict"], r["beyond"], "of", r["vertices"], "max", round(r["max"], 4))
    front = _label(_b64img(s("render_view", width=520, height=560, mode="symmetry",
                             camera={"view": "front", "frame": True})), "Symmetry heat map")
    side = _label(_b64img(s("render_view", width=520, height=560, mode="symmetry",
                            camera={"view": "front", "frame": True, "yaw": -0.6, "pitch": 0.15})),
                  "Yellow: off by a little; red: 10x more")
    _side_by_side([front, side], os.path.join(out, "symmetry_heatmap.png"))
    _palette(s, "symmetry check")
    s.shot("symmetry_window", crop=window_crop(s, "Mesh symmetry"), settle=1.0)
    s.x.key("Escape")
    time.sleep(0.4)
    s("set_view", mode="material")

    print("0.10: macro recorder")
    s("open_file", path=bust)
    s("macro_record", action="start")
    s("add_layer", type="fill", name="Base", material={"id": "patina_plastic_satin_gray"})
    s("add_layer", type="fill", name="Edge wear", material={"id": "patina_aluminum_brushed"},
      generator={"type": "edges", "level": 0.6})
    s("add_layer", type="fill", name="Dust", material={"id": "patina_dust"}, generator={"type": "occlusion"})
    s("macro_record", action="stop")
    _palette(s, "macro recorder")
    s.shot("macro_recorder", crop=window_crop(s, "Macro recorder"), settle=0.8)
    s.x.key("Escape")
    s("macro_record", action="clear")


def run_0110(s):
    """Patina 0.11: the Assistant window. The window is opened (and asked) through the assistant_panel
    command, so no clicks are needed; the pictures of results come from tools/assistant/manual_images.py."""
    print("0.11: the Assistant")
    bust = os.path.abspath(os.path.join(s.home, "bust.obj"))
    _bust_obj(bust)
    s("open_file", path=bust)
    s("set_camera", view="front", frame=True)
    s("assistant_model", action="load")
    s("assistant_panel", request="Set up a smart mask that makes the creases in my character's skin slightly more red "
                                 "than the rest of their skin")
    for _ in range(120):  # until the answer is in (the model is idle again)
        time.sleep(0.5)
        if s("assistant_model")["state"] == "ready":
            break
    s.shot("assistant_panel", crop=window_crop(s, "Assistant"), settle=1.5)
    s("assistant_panel", request="Set up the UVs so I have one UV island for the head and another for the body")
    for _ in range(120):
        time.sleep(0.5)
        if s("assistant_model")["state"] == "ready":
            break
    s.shot("assistant_panel_uv", crop=window_crop(s, "Assistant"), settle=1.5)
    s("assistant_panel", request="Make the cloth look battle-worn", preview=True)  # a look: a slider per layer
    for _ in range(120):
        time.sleep(0.5)
        if s("assistant_model")["state"] == "ready":
            break
    s.shot("assistant_panel_look", crop=window_crop(s, "Assistant"), settle=2.0)


def run_036b(s):
    """Windows from 0.13 to 0.35 again, with 0.36's interface: retopology, collision, lightmap UVs, batches,
    an adjustment layer's develop settings."""
    import shutil
    import tempfile
    print("0.36: the Retopology window")
    s("open_file", path=SUZANNE)
    s("set_resolution", resolution=1024)
    layout(s)
    s("set_camera", view="front", frame=True)
    s("retopo_density", quads_per_m2=1500)
    r = s("retopo_build")
    print("   quads", r.get("quads"))
    s("open_window", window="retopology")
    time.sleep(2.0)
    s.x.move(*vp(0.9, 0.9))
    s.shot("retopo_window", crop=window_crop(s, "Retopology"), settle=1.0)
    s("retopo_clear")
    panel(s, "properties")   # (the Retopology tab behind again: no zones shown)
    print("0.36: collision")
    s("new_project", primitive="torus", resolution=512)
    s("set_view", mode="material", seams=False, wireframe=False)
    s("set_tool", tool="paint")
    s("add_layer", type="fill", name="Metal", material={"id": "patina_steel_brushed", "scale": 1.5})
    s("set_camera", view="front", frame=True)
    s("set_camera", yaw=0.3, pitch=0.55)
    s("mesh_collision", mode="pieces", max_shapes=8)
    s("open_window", window="collision")
    time.sleep(1.2)
    s.x.move(*vp(0.9, 0.9))
    s.shot("collision", crop=CROP_VIEWPORT, settle=1.0)
    s("mesh_collision", clear=True)
    s.x.key("Escape")
    print("0.36: lightmap UVs")
    s("open_file", path=SUZANNE)
    s("mesh_lightmap_uvs", lightmap_size=512)
    s("open_window", window="lightmap")
    time.sleep(1.2)
    s.x.move(*vp(0.9, 0.9))
    s.shot("lightmap", crop=window_crop(s, "Lightmap UVs"), settle=1.0)
    s.x.key("Escape")
    print("0.36: batch")
    tmp = tempfile.mkdtemp(dir=s.home)
    files = []
    for n in ("Monkey.glb", "Suzanne.glb"):
        shutil.copy(SUZANNE, os.path.join(tmp, n))
        files.append(os.path.join(tmp, n))
    s("open_window", window="batch")
    time.sleep(0.8)
    s("batch_run", recipe="patina_game_asset", files=files, output=os.path.join(tmp, "out"))
    for _ in range(600):
        st = s("batch_status")
        if not st.get("running"):
            break
        time.sleep(0.5)
    time.sleep(1.5)
    s.x.move(*vp(0.9, 0.9))
    s.shot("batch_window", crop=window_crop(s, "Batch process"), settle=1.0)
    s.x.key("Escape")
    print("0.36: develop settings")
    build_suzanne(s)
    s("add_layer", type="adjust", name="Develop",
      adjust={"temperature": 0.45, "exposure": 0.5, "highlights": -0.4, "shadows": 0.6, "vibrance": 0.5, "delight": 0.7})
    s.x.move((CROP_LAYERS[0] + CROP_LAYERS[2]) // 2, CROP_LAYERS[3] - 30)   # below the list: the settings scroll
    time.sleep(0.3)
    for _ in range(14):
        s.x.button(5, True); s.x.button(5, False); time.sleep(0.05)
    s.shot("develop_panel", crop=CROP_LAYERS, settle=1.0)


def run_1213(s):
    """Patina 1.2.13: a brush for each tool (the Paint tool's splatter brush and the Erase tool's round one, each with
    the Brush panel saying whose it is) and the tip cursor (soft and hard round, a star, a splatter) over a painted
    plane."""
    print("1.2.13: tool brushes, the tip cursor")
    s("new_project", primitive="plane", resolution=1024)
    s("set_camera", view="top", frame=True)
    s("set_view", mode="material", seams=False, wireframe=False)
    layout(s)
    s("add_layer", type="fill", material={"id": "patina_paint_chipped_red", "scale": 2})
    s("add_layer", type="paint", name="Paint")
    s("set_tool", tool="paint")
    s("set_brush", preset="basic_hard", size=150, alpha="splatter", hardness=0.8, angle=0, roundness=1)
    panel(s, "properties")
    c = vp(0.5, 0.5)

    def cursor_crop(name, half=110):
        s.x.move(*c)
        s.settle(8, 0.6)
        s.shot(name, crop=(int(c[0] - half), int(c[1] - half), int(c[0] + half), int(c[1] + half)))
        return os.path.join(s.out, name + ".png")

    def brush_head(name):
        h = rect(s, "props.brush_header")
        w = rect(s, "window.Properties")
        s.shot(name, crop=(w[0], h[1] - 4, w[2], h[3] + 104))
        return os.path.join(s.out, name + ".png")

    shots = []
    s.x.move(*c)
    s.x.key("b")
    s.settle(6)
    shots += [cursor_crop("_tb_paint_cursor"), brush_head("_tb_paint_head")]
    s.x.move(*c)
    s.x.key("e")
    s.settle(6)
    shots += [cursor_crop("_tb_erase_cursor"), brush_head("_tb_erase_head")]
    s.x.move(*c)
    s.x.key("b")
    s.settle(4)
    from PIL import Image
    ims = [Image.open(os.path.join(s.out, n + ".png")) for n in ("_tb_paint_cursor", "_tb_paint_head", "_tb_erase_cursor", "_tb_erase_head")]
    W = max(ims[0].width, ims[1].width)
    pad = 10
    sheet = Image.new("RGB", (2 * W + 3 * pad, ims[0].height + ims[1].height + 3 * pad + 24), (27, 28, 31))
    from PIL import ImageDraw
    d = ImageDraw.Draw(sheet)
    for col, (cur, head, title) in enumerate(((ims[0], ims[1], "Paint (B): a splatter tip"),
                                              (ims[2], ims[3], "Erase (E): its own round brush"))):
        x = pad + col * (W + pad)
        d.text((x + 2, pad), title, fill=(225, 225, 225))
        sheet.paste(cur, (x + (W - cur.width) // 2, pad + 24))
        sheet.paste(head, (x, 2 * pad + 24 + cur.height))
    sheet.save(os.path.join(s.out, "tool_brushes.png"), optimize=True)
    # the tip cursor: four brushes
    cells = []
    for name, args, title in (("_tc_soft", dict(alpha="round", hardness=0.0), "Round, hardness 0"),
                              ("_tc_hard", dict(alpha="round", hardness=0.9), "Round, hardness 0.9"),
                              ("_tc_star", dict(alpha="star", hardness=0.6, angle=15), "Star, angle 15"),
                              ("_tc_splatter", dict(alpha="splatter", hardness=0.8), "Splatter")):
        s("set_brush", size=170, roundness=1, **args)
        cells.append((cursor_crop(name, 105), title))
    compose(cells, os.path.join(s.out, "tip_cursor.png"), cols=4)
    s("set_brush", alpha="round", hardness=0.8, angle=0)
    for n in ("_tb_paint_cursor", "_tb_paint_head", "_tb_erase_cursor", "_tb_erase_head", "_tc_soft", "_tc_hard",
              "_tc_star", "_tc_splatter"):
        try:
            os.remove(os.path.join(s.out, n + ".png"))
        except OSError:
            pass


def run_1214(s):
    """Patina 1.2.14: the tip cursor shows the opacity (a hard round brush at 1, 0.5 and 0.2) and the Blender keymap's
    dials draw the brush's tip: F at the size it sets, Ctrl+F turning a star, Shift+F the dither thinning."""
    import math
    print("1.2.14: the tip cursor's opacity, the dials")
    s("new_project", primitive="plane", resolution=1024)
    s("set_camera", view="top", frame=True)
    s("set_view", mode="material", seams=False, wireframe=False)
    layout(s)
    s("add_layer", type="fill", material={"id": "patina_paint_chipped_red", "scale": 2})
    s("add_layer", type="paint", name="Paint")
    s("set_tool", tool="paint")
    s("open_window", window="preferences", keymap="blender")  # (the dials are the Blender keymap's F keys)
    s.settle(4)
    c = vp(0.5, 0.5)
    s.x.move(*c)
    s.settle(4)
    s.x.key("Escape")
    s.settle(4)

    def crop(name, half):
        s.settle(8, 0.6)
        s.shot(name, crop=(int(c[0] - half), int(c[1] - half), int(c[0] + half), int(c[1] + half)))
        return os.path.join(s.out, name + ".png")

    def dial(at, to, mods=()):
        s.x.move(*at)
        s.settle(4)
        for m in mods:
            s.x.key(m, True)
        s.x.key("f")
        for m in mods:
            s.x.key(m, False)
        s.settle(4)
        s.x.move(*to)

    cells = []
    for name, op in (("_to_100", 1.0), ("_to_050", 0.5), ("_to_020", 0.2)):
        s("set_brush", preset="basic_hard", alpha="round", size=170, hardness=0.9, opacity=op, angle=0, roundness=1)
        s.x.move(*c)
        cells.append((crop(name, 150), "Opacity %.1f" % op))
    s("set_brush", alpha="star", size=170, hardness=0.6, opacity=1.0, angle=0)
    dial((c[0] + 85, c[1]), (c[0] + 110, c[1]))  # F: the middle where the pointer was, half the size to its left
    cells.append((crop("_td_size", 150), "F: the size (the tip, not a circle)"))
    s.x.click(c[0] + 110, c[1], b=3)  # (a right-click: the size as it was)
    s("set_brush", size=170)
    a = math.radians(36)
    dial((c[0] + 120, c[1]), (c[0] + 120 * math.cos(a), c[1] - 120 * math.sin(a)), mods=("Control_L",))
    cells.append((crop("_td_angle", 150), "Ctrl+F: the angle (the tip turning)"))
    s.x.click(c[0] + 120 * math.cos(a), c[1] - 120 * math.sin(a), b=3)
    s("set_brush", alpha="round", hardness=0.9, opacity=0.0)
    dial(c, (c[0], c[1] - 42), mods=("Shift_L",))  # (from opacity 0 the middle is the pointer: 42 px is 0.35)
    cells.append((crop("_td_opacity", 150), "Shift+F: the opacity (its dither)"))
    s.x.click(c[0], c[1] - 42, b=3)
    s("set_brush", opacity=1.0)
    compose(cells, os.path.join(s.out, "tip_cursor_dials.png"), cols=3)
    s("open_window", window="preferences", keymap="patina")
    s.settle(4)
    s.x.move(*c)
    s.x.key("Escape")
    for n in ("_to_100", "_to_050", "_to_020", "_td_size", "_td_angle", "_td_opacity"):
        try:
            os.remove(os.path.join(s.out, n + ".png"))
        except OSError:
            pass
