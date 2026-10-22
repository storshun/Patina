"""Patina promo video: 1920x1080, 30 fps, silent. Frames are drawn with PIL from Patina's own renders and screenshots
and piped to ffmpeg.

    python3 scripts/promo/make_video.py [out.mp4]                # the video (build-promo/out/patina_promo.mp4)
    python3 scripts/promo/make_video.py --stills 1.5,8,20 DIR    # a few frames to look at
"""
import math
import multiprocessing as mp
import subprocess
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

_argv = sys.argv
sys.argv = [_argv[0]]
import make_banners as B  # noqa: E402
from paint_cube import STAGES  # noqa: E402

sys.argv = _argv

W, H, FPS = 1920, 1080, 30
XFADE = 0.4
R = B.R

# ---------------------------------------------------------------------------- small helpers


def clamp(x, a=0.0, b=1.0):
    return max(a, min(b, x))


def ease(x):
    x = clamp(x)
    return 1 - (1 - x) ** 3


def ease_io(x):
    x = clamp(x)
    return x * x * (3 - 2 * x)


def back(x):
    x = clamp(x)
    c = 1.6
    return 1 + (c + 1) * (x - 1) ** 3 + c * (x - 1) ** 2


def anim(t, start, dur=0.5):
    return ease((t - start) / dur)


def fade(im, a):
    if a >= 0.999:
        return im
    im = im.copy()
    im.putalpha(im.getchannel("A").point(lambda v: int(v * a)))
    return im


def put(canvas, im, x, y, a=1.0):
    """Composite im at (x, y) with opacity a, clipped to the canvas."""
    if a <= 0.003:
        return
    x, y = int(round(x)), int(round(y))
    l, t = max(0, -x), max(0, -y)
    r, b = min(im.width, canvas.width - x), min(im.height, canvas.height - y)
    if r <= l or b <= t:
        return
    if (l, t, r, b) != (0, 0, im.width, im.height):
        im = im.crop((l, t, r, b))
    canvas.alpha_composite(fade(im, a), (x + l, y + t))


def rise(canvas, im, x, y, t, start, dur=0.6, dy=24):
    k = anim(t, start, dur)
    put(canvas, im, x, y + (1 - k) * dy, k)


def txt(text, size, fill, weight="InterDisplay-SemiBold.ttf"):
    return runs([(text, fill)], size, weight)


def runs(parts, size, weight="InterDisplay-SemiBold.ttf"):
    f = B.font(weight, size)
    probe = ImageDraw.Draw(Image.new("RGBA", (1, 1)))
    w = int(sum(probe.textlength(p, font=f) for p, _ in parts)) + 4
    im = Image.new("RGBA", (w, int(size * 1.32)), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    x = 0
    for p, col in parts:
        d.text((x, 0), p, font=f, fill=col)
        x += d.textlength(p, font=f)
    return im


def para(text, size, width, col=B.MUTED, weight="Inter-Regular.ttf", spacing=1.42):
    f = B.font(weight, size)
    probe = ImageDraw.Draw(Image.new("RGBA", (1, 1)))
    lines = B.wrap(probe, text, f, width)
    im = Image.new("RGBA", (width + 4, int(len(lines) * size * spacing) + 8), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    for i, l in enumerate(lines):
        d.text((0, int(i * size * spacing)), l, font=f, fill=col)
    return im


def chip_row(labels, size, dot=B.TEAL):
    probe = ImageDraw.Draw(Image.new("RGBA", (1, 1)))
    f = B.font("Inter-Medium.ttf", size)
    w = sum(int(probe.textlength(l, font=f)) + int(size * 2.4) + 14 for l in labels)
    im = Image.new("RGBA", (w, int(size * 1.9) + 2), (0, 0, 0, 0))
    B.chips(ImageDraw.Draw(im), (0, 0), labels, size=size, dot=dot)
    return im


def bubble_img(text, size, max_w, mine, label):
    c = Image.new("RGBA", (max_w + 80, 600), (0, 0, 0, 0))
    x = max_w + 40 if mine else 40
    B.bubble(c, (x, 40), text, size, max_w, mine=mine, label=label)
    return c.crop(c.getbbox())


def with_shadows(bg, shadows):
    layer = Image.new("RGBA", bg.size, (0, 0, 0, 0))
    for cx, cy, w, h, a in shadows:
        ImageDraw.Draw(layer).ellipse((cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2), fill=(0, 0, 0, a))
        layer = layer.filter(ImageFilter.GaussianBlur(h * 0.6))
        bg.alpha_composite(layer)
        layer = Image.new("RGBA", bg.size, (0, 0, 0, 0))
    return bg


def lockup_parts(height):
    icon = B.ICON.resize((int(B.ICON.width * height / B.ICON.height), height), Image.LANCZOS)
    wh = int(height * 0.62)
    word = B.WORD.resize((int(B.WORD.width * wh / B.WORD.height), wh), Image.LANCZOS)
    return icon, word, int(height * 0.28), (height - wh) // 2 + int(height * 0.04)


def sphere(name, s):
    im = Image.open(R / "spheres" / (name + ".png")).convert("RGBA")
    return im.crop(im.getbbox()).resize((s, s), Image.LANCZOS)


# ---------------------------------------------------------------------------- assets


LAST = len(STAGES) - 1  # the finished cube's turntable


def turntable_box():
    box = None
    for s in (0, LAST):
        for i in (1, 40, 80, 120, 160, 200):
            b = Image.open(R / ("turn%d" % s) / ("cube_%04d.png" % i)).getbbox()
            box = b if box is None else (min(box[0], b[0]), min(box[1], b[1]), max(box[2], b[2]), max(box[3], b[3]))
    m = 12
    return (box[0] - m, box[1] - m, box[2] + m, box[3] + m)


TBOX = turntable_box()


def cube_frame(stage, f, width):
    """The turntable at a fractional frame f (240 frames to a turn), blended between its neighbors."""
    def one(i):
        return Image.open(R / ("turn%d" % stage) / ("cube_%04d.png" % (i % 240 + 1))).convert("RGBA").crop(TBOX)
    i0 = math.floor(f)
    k = f - i0
    im = one(i0)
    if k > 0.02:
        im = Image.blend(im, one(i0 + 1), k)
    return im.resize((width, int(im.height * width / im.width)), Image.LANCZOS)


HERO = 239  # the turntable frame that matches the icon: walnut on the left, copper on the right


def sway(t, amp, period):
    return HERO + amp * math.sin(2 * math.pi * t / period - 0.6)


class A:
    """Everything drawn once, before the frames."""


def build_assets():
    a = A()
    warm = (120, 84, 46)
    # intro
    a.intro_bg = B.background(W, H, glows=[(960, 470, 640, warm, 0.45), (960, 470, 300, (196, 150, 90), 0.12)])
    a.icon_big, a.word_big, a.gap_big, a.wdy_big = lockup_parts(170)
    a.intro_tag = txt(" ".join(t for t, _ in B.HEADLINE), 46, B.CREAM, "InterDisplay-Medium.ttf")
    a.intro_sub = txt(B.TAGLINE, 30, B.GOLD, "InterDisplay-Medium.ttf")
    a.lock_small = B.lockup(56)
    # cube
    a.cube_w = 860
    ch = int((TBOX[3] - TBOX[1]) * a.cube_w / (TBOX[2] - TBOX[0]))
    a.cube_xy = (1350 - a.cube_w // 2, 545 - ch // 2)
    a.cube_bg = with_shadows(B.background(W, H, glows=[(1350, 540, 620, warm, 0.55)]),
                             [(1350, 545 + ch * 0.44, 560, 70, 150)])
    a.cube_title = txt("Paint in layers.", 84, B.CREAM)
    a.cube_sub = para("Our logo, painted in Patina, one layer at a time.", 30, 640)
    # one row for each stage of paint_cube.STAGES
    rows = [("patina_wood_walnut", "Walnut", "A fill layer, its grain running down the sides"),
            ("patina_bronze_verdigris", "Copper and verdigris", "Masked by facing, ragged at the corners"),
            ("patina_marble_white", "Marble", "On top, its edge broken up by noise"),
            ("patina_gold_polished", "Gold", "A gradient band, with drips running down"),
            ("swoosh_teal", "Teal swoosh", "Two gradients and a facing mask, stacked")]
    a.rows = []
    for sp, name, line in rows:
        r = Image.new("RGBA", (640, 96), (0, 0, 0, 0))
        d = ImageDraw.Draw(r)
        d.rounded_rectangle((0, 0, 639, 95), 18, fill=B.CHIP + (235,), outline=(58, 70, 86), width=1)
        r.alpha_composite(sphere(sp, 66), (16, 15))
        d.text((104, 20), name, font=B.font("Inter-SemiBold.ttf", 27), fill=B.CREAM)
        d.text((104, 56), line, font=B.font("Inter-Regular.ttf", 20), fill=B.MUTED)
        hl = r.copy()
        ImageDraw.Draw(hl).rounded_rectangle((0, 0, 639, 95), 18, outline=B.GOLD, width=2)
        a.rows.append((r, hl))
    # spheres
    a.sph_bg = B.background(W, H, glows=[(960, 560, 820, (60, 74, 96), 0.5)])
    a.sph_title = runs([("Paint materials, ", B.CREAM), ("not colors.", B.GOLD)], 78)
    a.sph_sub = txt("One stroke lays down color, roughness, metal, height, and normal, straight across UV seams.", 29,
                    B.MUTED, "Inter-Regular.ttf")
    names = ["patina_gold_polished", "patina_copper", "patina_bronze_verdigris", "patina_marble_white",
             "patina_wood_walnut", "patina_leather_brown", "patina_quilted_leather", "patina_velvet_red",
             "patina_car_paint_blue", "patina_carbon_fiber", "patina_diamond_plate_steel", "patina_rust_heavy",
             "patina_knit_wool_cream", "patina_hex_tiles_rosette", "patina_damask_navy_silver", "patina_feathers_peacock"]
    a.sph_s = 190
    a.spheres = [sphere(n, a.sph_s) for n in names]
    a.sph_x0 = (W - (8 * a.sph_s + 7 * 30)) // 2
    a.sph_rows = (350, 600)
    a.sph_bg = with_shadows(a.sph_bg, [(a.sph_x0 + (i % 8) * (a.sph_s + 30) + a.sph_s / 2,
                                        a.sph_rows[i // 8] + a.sph_s * 0.97, a.sph_s * 0.7, a.sph_s * 0.12, 120)
                                       for i in range(16)])
    a.sph_chips = chip_row(["270 materials", "74 procedural generators", "22 smart materials", "Your own scans"], 24,
                           dot=B.GOLD)
    # develop
    a.dev_bg = B.background(W, H, glows=[(1330, 540, 640, (70, 90, 70), 0.35)])
    before = Image.open(R / "develop_before.png").convert("RGBA")
    after = Image.open(R / "develop_after.png").convert("RGBA")
    box = before.getbbox()
    dh = 800
    dw = int((box[2] - box[0]) * dh / (box[3] - box[1]))
    a.before = before.crop(box).resize((dw, dh), Image.LANCZOS)
    a.after = after.crop(box).resize((dw, dh), Image.LANCZOS)
    a.dev_xy = (1340 - dw // 2, 120)
    a.dev_bg = with_shadows(a.dev_bg, [(1340, 120 + dh - 10, dw * 0.55, 50, 130)])
    a.dev_title = [txt("Develop textures", 80, B.CREAM), txt("like photos.", 80, B.GOLD)]
    a.dev_lines = [para("Exposure, white balance, highlights, shadows, and vibrance on an adjustment layer.", 28, 640),
                   para("De-light takes out the shadows baked into scans and generated models.", 28, 640),
                   para("PBR Range marks colors no real material has.", 28, 640)]
    a.lab_before = txt("As captured", 28, B.CREAM, "Inter-SemiBold.ttf")
    a.lab_after = txt("Developed", 28, B.GOLD, "Inter-SemiBold.ttf")
    # app
    a.app_bg = B.background(W, H, glows=[(960, 760, 900, (90, 70, 50), 0.35)])
    shot = Image.open(B.SHOTS / "ui_main.png")
    a.app_card, a.app_pad = B.window(shot, 1300)
    a.app_scale = 1300 / shot.width
    a.app_title = txt("Everything in one window.", 66, B.CREAM)
    a.app_sub = txt("Layers, smart masks, UVs, bakes, a material library, and undo for every step.", 28, B.MUTED,
                    "Inter-Regular.ttf")
    a.app_tags = [(chip_row(["Layers, folders and smart masks"], 24), (1100, 120)),
                  (chip_row(["UV editor, live"], 24, dot=B.GOLD), (800, 560)),
                  (chip_row(["Material library"], 24, dot=B.COPPER), (300, 690))]
    # game-ready
    a.game_bg = B.background(W, H, glows=[(960, 700, 900, (40, 80, 76), 0.35)])
    a.game_title = txt("From download to game-ready.", 70, B.CREAM)
    a.game_sub = txt("Mend, reduce, add collision and lightmap UVs, then export for any engine.", 29, B.MUTED,
                     "Inter-Regular.ttf")
    cards = [(B.MANUAL / "mesh_check.png", "Mesh Check", "Finds holes, flipped faces, and loose parts, then mends them in one click."),
             (B.MANUAL / "collision.png", "Collision", "Boxes, hulls, or convex pieces, named for Unreal or Godot."),
             (B.MANUAL / "lightmap.png", "Lightmap UVs", "A second UV map where every face has its own space.")]
    a.game_cards = []
    for path, title, line in cards:
        im = Image.open(path).convert("RGBA")
        ph = 470
        im = im.resize((int(im.width * ph / im.height), ph), Image.LANCZOS)
        card, pad = B.window(im, im.width, radius=12)
        a.game_cards.append((card, pad, im.width, txt(title, 36, B.CREAM), para(line, 24, 520)))
    a.game_formats = txt("GLB  ·  glTF  ·  OBJ  ·  FBX in  ·  STL  ·  PLY  ·  Unreal  ·  Unity  ·  Godot  ·  Blender",
                         26, B.MUTED, "Inter-Medium.ttf")
    # batch, and an agent
    a.batch_bg = B.background(W, H, glows=[(600, 640, 760, (70, 64, 96), 0.35)])
    a.batch_title = runs([("Do it once. ", B.CREAM), ("Then do the whole folder.", B.GOLD)], 66)
    a.batch_card, a.batch_pad = B.window(Image.open(B.MANUAL / "batch_window.png"), 720)
    a.bub_you = bubble_img("Finish every model in ~/props for Godot: under 20,000 triangles, convex collision, two "
                           "levels of detail.", 27, 760, True, "YOU")
    a.bub_agent = bubble_img("Done. 24 models mended, reduced, and exported as GLB, each with its collision named for "
                             "Godot. Two had holes; both are closed now.", 27, 800, False, "AGENT")
    a.batch_foot = para("Recipes chain the steps. A batch runs one on hundreds of models in the background and never "
                        "touches the originals. Any MCP agent can drive all 166 of Patina's tools.", 26, 860)
    # end
    a.end_bg = with_shadows(B.background(W, H, glows=[(960, 430, 640, warm, 0.5)]), [(960, 640, 420, 56, 150)])
    a.end_cube_w = 520
    a.end_word = B.WORD.resize((int(B.WORD.width * 120 / B.WORD.height), 120), Image.LANCZOS)
    a.end_tag = txt(" ".join(t for t, _ in B.HEADLINE), 40, B.CREAM, "InterDisplay-Medium.ttf")
    a.end_sub = txt(B.TAGLINE, 28, B.GOLD, "InterDisplay-Medium.ttf")
    a.end_chips = chip_row(["Linux + Windows", "Blender add-on", "MCP ready"], 24)
    return a


# ---------------------------------------------------------------------------- scenes


def s_intro(a, t):
    c = a.intro_bg.copy()
    icon, word, gap, wdy = a.icon_big, a.word_big, a.gap_big, a.wdy_big
    total = icon.width + gap + word.width
    x0, y0 = (W - total) // 2, 330
    k = back((t - 0.15) / 0.8)
    s = 0.82 + 0.18 * k
    ic = icon.resize((max(1, int(icon.width * s)), max(1, int(icon.height * s))), Image.LANCZOS)
    put(c, ic, x0 + (icon.width - ic.width) / 2, y0 + (icon.height - ic.height) / 2, clamp((t - 0.15) / 0.5))
    kw = anim(t, 0.7, 0.8)
    put(c, word, x0 + icon.width + gap + (1 - kw) * 40, y0 + wdy, kw)
    rise(c, a.intro_tag, (W - a.intro_tag.width) / 2, 590, t, 1.4, 0.7)
    rise(c, a.intro_sub, (W - a.intro_sub.width) / 2, 672, t, 1.9, 0.7)
    return c


def s_cube(a, t):
    c = a.cube_bg.copy()
    starts = [0.3 + 2.4 * k for k in range(LAST + 1)]
    stage = max(0, sum(1 for s in starts if t >= s) - 1)
    f = sway(t, 22, 10.0)
    im = cube_frame(stage, f, a.cube_w)
    if stage > 0 and t < starts[stage] + 0.6:
        im = Image.blend(cube_frame(stage - 1, f, a.cube_w), im, ease_io((t - starts[stage]) / 0.6))
    put(c, im, *a.cube_xy, clamp(t / 0.4) if t < 0.4 else 1.0)
    put(c, a.lock_small, 110, 84)
    rise(c, a.cube_title, 110, 210, t, 0.1)
    rise(c, a.cube_sub, 112, 318, t, 0.3)
    # the layer stack: the newest row on top
    base, rh = 430, 112
    shown = max(k for k in range(LAST + 1) if k == 0 or t >= starts[k] + 0.55)
    for k in range(LAST + 1):
        if t < starts[k]:
            continue
        y = base + sum(rh * ease_io((t - starts[j]) / 0.4) for j in range(k + 1, LAST + 1) if t >= starts[j])
        ka = ease((t - starts[k] - (0.3 if k else 0.0)) / 0.5)
        newest = k == shown
        row, hl = a.rows[k]
        put(c, hl if newest else row, 110 - (1 - ka) * 30, y, ka)
    return c


def s_spheres(a, t):
    c = a.sph_bg.copy()
    rise(c, a.sph_title, (W - a.sph_title.width) / 2, 92, t, 0.0)
    rise(c, a.sph_sub, (W - a.sph_sub.width) / 2, 212, t, 0.25)
    s = a.sph_s
    drift = t * 6
    for i, sp in enumerate(a.spheres):
        st = 0.35 + (i % 8) * 0.07 + (i // 8) * 0.18
        k = back((t - st) / 0.55)
        if t < st:
            continue
        sc = 0.55 + 0.45 * k
        im = sp.resize((max(1, int(s * sc)), max(1, int(s * sc))), Image.LANCZOS)
        x = a.sph_x0 + (i % 8) * (s + 30) + (s - im.width) / 2 + (drift if i // 8 == 0 else -drift) * 0.5
        y = a.sph_rows[i // 8] + (s - im.height) / 2
        put(c, im, x, y, clamp((t - st) / 0.3))
    rise(c, a.sph_chips, (W - a.sph_chips.width) / 2, 880, t, 2.0)
    return c


def s_develop(a, t):
    c = a.dev_bg.copy()
    put(c, a.lock_small, 110, 84)
    rise(c, a.dev_title[0], 110, 230, t, 0.0)
    rise(c, a.dev_title[1], 110, 330, t, 0.12)
    y = 470
    for i, p in enumerate(a.dev_lines):
        rise(c, p, 112, y, t, 0.5 + i * 0.35)
        y += p.height + 14
    # the wipe: before, then the developed version sweeps across, then it settles half and half
    w, h = a.before.size
    if t < 1.0:
        f = 0.0
    elif t < 3.2:
        f = ease_io((t - 1.0) / 2.2)
    elif t < 4.0:
        f = 1.0
    else:
        f = 1.0 - 0.5 * ease_io((t - 4.0) / 0.9)
    lx = int(w * f)
    im = a.before.copy()
    if lx > 0:
        im.paste(a.after.crop((0, 0, lx, h)), (0, 0))
    x0, y0 = a.dev_xy
    put(c, im, x0, y0, clamp(t / 0.35))
    if 0.02 < f < 0.98:
        d = ImageDraw.Draw(c)
        d.line((x0 + lx, y0 + 10, x0 + lx, y0 + h - 10), fill=B.CREAM, width=3)
        d.ellipse((x0 + lx - 16, y0 + h / 2 - 16, x0 + lx + 16, y0 + h / 2 + 16), fill=B.CREAM)
        d.polygon([(x0 + lx - 5, y0 + h / 2), (x0 + lx - 11, y0 + h / 2 - 6), (x0 + lx - 11, y0 + h / 2 + 6)], fill=B.NAVY)
        d.polygon([(x0 + lx + 5, y0 + h / 2), (x0 + lx + 11, y0 + h / 2 - 6), (x0 + lx + 11, y0 + h / 2 + 6)], fill=B.NAVY)
    ly = y0 + h + 30
    put(c, a.lab_before, x0 + lx + 24, ly, clamp((w - lx - 40) / 220.0) * clamp(t / 0.5))
    put(c, a.lab_after, x0 + lx - 24 - a.lab_after.width, ly, clamp((lx - 40) / 220.0))
    return c


def s_app(a, t):
    c = a.app_bg.copy()
    rise(c, a.app_title, 110, 64, t, 0.0)
    rise(c, a.app_sub, 112, 152, t, 0.2)
    card = a.app_card
    sc = 1.0 + 0.045 * ease_io(t / 5.0)
    k = anim(t, 0.0, 0.7)
    cw, ch = int(card.width * sc), int(card.height * sc)
    im = card.resize((cw, ch), Image.BILINEAR) if sc > 1.001 else card
    x = (W - cw) / 2
    y = 230 - a.app_pad * sc + (1 - k) * 40
    put(c, im, x, y, k)
    for i, (tag, (sx, sy)) in enumerate(a.app_tags):
        px = x + (a.app_pad + sx * a.app_scale) * sc
        py = y + (a.app_pad + sy * a.app_scale) * sc
        kk = back((t - (1.2 + i * 0.45)) / 0.5)
        if t > 1.2 + i * 0.45:
            put(c, tag, px, py + (1 - kk) * 16, clamp((t - 1.2 - i * 0.45) / 0.3))
    return c


def s_game(a, t):
    c = a.game_bg.copy()
    rise(c, a.game_title, (W - a.game_title.width) / 2, 60, t, 0.0)
    rise(c, a.game_sub, (W - a.game_sub.width) / 2, 162, t, 0.2)
    cw, gap = 540, 60
    x = (W - (3 * cw + 2 * gap)) / 2
    for i, (card, pad, iw, title, line) in enumerate(a.game_cards):
        st = 0.45 + i * 0.3
        k = anim(t, st, 0.7)
        dy = (1 - k) * 60
        put(c, card, x + (cw - iw) / 2 - pad, 262 - pad + dy, k)
        put(c, title, x + (cw - iw) / 2, 760 + dy, k)
        put(c, line, x + (cw - iw) / 2 + 2, 812 + dy, k)
        x += cw + gap
    rise(c, a.game_formats, (W - a.game_formats.width) / 2, 990, t, 1.9)
    return c


def s_batch(a, t):
    c = a.batch_bg.copy()
    put(c, a.lock_small, 110, 70)
    rise(c, a.batch_title, 110, 170, t, 0.0)
    k = anim(t, 0.3, 0.7)
    put(c, a.batch_card, 110 - a.batch_pad, 300 - a.batch_pad + (1 - k) * 40, k)
    rx = 1810
    y = 330
    rise(c, a.bub_you, rx - a.bub_you.width, y, t, 0.9, 0.5, 18)
    y += a.bub_you.height + 40
    if 1.8 < t < 3.0:
        # the agent is working
        d = ImageDraw.Draw(c)
        bx, by = 930, y + 10
        al = int(255 * clamp((t - 1.8) / 0.2) * clamp((3.0 - t) / 0.15))
        lay = Image.new("RGBA", (130, 64), (0, 0, 0, 0))
        dd = ImageDraw.Draw(lay)
        dd.rounded_rectangle((0, 0, 129, 63), 22, fill=(246, 236, 224, 255))
        for j in range(3):
            o = math.sin((t * 7) - j * 0.8) * 5
            dd.ellipse((28 + j * 28 - 7, 32 - 7 - o, 28 + j * 28 + 7, 32 + 7 - o), fill=(120, 110, 100))
        put(c, lay, bx, by, al / 255)
    rise(c, a.bub_agent, 930 - 40, y - 26, t, 3.0, 0.5, 18)
    rise(c, a.batch_foot, 932, y + a.bub_agent.height + 30, t, 3.9)
    return c


def s_end(a, t, dur):
    c = a.end_bg.copy()
    k = anim(t, 0.0, 0.8)
    im = cube_frame(LAST, sway(t + 3.0, 14, 9.0), a.end_cube_w)
    put(c, im, (W - im.width) / 2, 338 - im.height / 2 + (1 - k) * 30, k)
    rise(c, a.end_word, (W - a.end_word.width) / 2, 652, t, 0.4, 0.7)
    rise(c, a.end_tag, (W - a.end_tag.width) / 2, 832, t, 0.9)
    rise(c, a.end_sub, (W - a.end_sub.width) / 2, 892, t, 1.15)
    rise(c, a.end_chips, (W - a.end_chips.width) / 2, 962, t, 1.4)
    out = dur - t
    if out < 0.6:
        black = Image.new("RGBA", c.size, B.NAVY_DEEP + (255,))
        c = Image.blend(c, black, ease_io(1 - out / 0.6))
    return c


SCENES = [("intro", s_intro, 3.6), ("cube", s_cube, 12.6), ("spheres", s_spheres, 6.0), ("develop", s_develop, 6.4),
          ("app", s_app, 5.4), ("game", s_game, 6.0), ("batch", s_batch, 7.4), ("end", None, 5.4)]


def timeline():
    t, out = 0.0, []
    for name, fn, d in SCENES:
        out.append((name, fn, t, d))
        t += d - XFADE
    return out, t + XFADE


TL, TOTAL = timeline()
ASSETS = None


def scene_frame(fn_name, fn, t, d):
    if fn_name == "end":
        return s_end(ASSETS, t, d)
    if fn_name == "intro" and t < 0.0:
        t = 0.0
    return fn(ASSETS, t)


def frame_at(T):
    active = [(n, fn, T - s, d) for n, fn, s, d in TL if s <= T < s + d]
    if not active:
        active = [TL[-1][:2] + (TL[-1][3] - 1e-3, TL[-1][3])]
    img = scene_frame(*active[0])
    if len(active) > 1:
        n, fn, lt, d = active[1]
        img = Image.blend(img, scene_frame(n, fn, lt, d), ease_io(lt / XFADE))
    return img


def render(i):
    return frame_at(i / FPS).convert("RGB").tobytes()


def init_worker():
    global ASSETS
    ASSETS = build_assets()


def main():
    args = sys.argv[1:]
    if args and args[0] == "--stills":
        times = [float(x) for x in args[1].split(",")]
        out = Path(args[2])
        out.mkdir(parents=True, exist_ok=True)
        init_worker()
        for T in times:
            frame_at(T).convert("RGB").save(out / ("still_%05.1f.png" % T))
        print("total %.1f s" % TOTAL)
        return
    path = Path(args[0] if args else B.WORK / "out" / "patina_promo.mp4")
    path.parent.mkdir(parents=True, exist_ok=True)
    n = int(TOTAL * FPS)
    print("frames", n, "seconds %.1f" % TOTAL, flush=True)
    ff = subprocess.Popen(["ffmpeg", "-y", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", "%dx%d" % (W, H),
                           "-r", str(FPS), "-i", "-", "-vf", "scale=out_color_matrix=bt709:out_range=tv,format=yuv420p",
                           "-c:v", "libx264", "-preset", "slow", "-crf", "18", "-profile:v", "high", "-tune", "film",
                           "-color_primaries", "bt709", "-color_trc", "bt709", "-colorspace", "bt709",
                           "-movflags", "+faststart", str(path)], stdin=subprocess.PIPE)
    with mp.Pool(16, initializer=init_worker) as pool:
        for k, buf in enumerate(pool.imap(render, range(n), chunksize=4)):
            ff.stdin.write(buf)
            if k % 150 == 0:
                print("frame", k, flush=True)
    ff.stdin.close()
    ff.wait()
    print("wrote", path)


if __name__ == "__main__":
    main()
