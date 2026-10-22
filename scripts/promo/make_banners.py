"""Patina promo images: banners and posts composed from Patina's own renders and screenshots, with the icon and
the wordmark (docs/brand) as the logo.

    python3 scripts/promo/make_banners.py [out folder] [hero paint app develop game_ready batch x_header ...]
"""
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

from promo_paths import BRAND, MANUAL, WORK

OUT = Path(sys.argv[1]) if len(sys.argv) > 1 else WORK / "out"
R = WORK / "renders"
SHOTS = WORK / "shots"
F = "/usr/share/fonts/rsms-inter-fonts/"

NAVY = (21, 28, 36)
NAVY_DEEP = (12, 16, 22)
CREAM = (241, 223, 205)
ROSE = (241, 217, 195)  # the wordmark's color
GOLD = (226, 184, 104)
COPPER = (205, 120, 72)
TEAL = (72, 176, 156)
MUTED = (164, 172, 184)
CHIP = (36, 45, 57)

HEADLINE = [("Post-process your models", CREAM), ("the way you would", CREAM), ("a photo.", CREAM)]
TAGLINE = "Non-destructive, layered, and scriptable."
CHIPS = ["Linux + Windows", "Blender bridge", "MCP ready"]


def font(name, size):
    return ImageFont.truetype(F + name, size)


# ---------------------------------------------------------------------------- the logo


def load_icon():
    im = Image.open(BRAND / "patina_icon.webp").convert("RGBA")
    return im.crop(im.getbbox())


def load_wordmark():
    """The wordmark's letters on transparency (from the navy panel it came on), at twice its size."""
    im = Image.open(BRAND / "patina_wordmark.png").convert("RGB")
    im = im.resize((im.width * 2, im.height * 2), Image.LANCZOS)
    a = np.asarray(im).astype(np.float32)
    bg = np.array(NAVY, np.float32)
    lum = a.mean(2)
    alpha = np.clip((lum - 34.0) / (205.0 - 34.0), 0, 1)
    alpha[:6, :] = alpha[-6:, :] = 0
    alpha[:, :6] = alpha[:, -6:] = 0
    col = (a - bg[None, None] * (1 - alpha[..., None])) / np.maximum(alpha[..., None], 1e-3)
    col = np.clip(col, 0, 255)
    rgba = np.dstack([col, alpha * 255]).astype(np.uint8)
    out = Image.fromarray(rgba, "RGBA")
    return out.crop(out.getbbox())


ICON = load_icon()
WORD = load_wordmark()


def lockup(height, gap=0.28):
    """Icon + wordmark side by side, `height` tall (the icon's height)."""
    icon = ICON.resize((int(ICON.width * height / ICON.height), height), Image.LANCZOS)
    wh = int(height * 0.62)
    word = WORD.resize((int(WORD.width * wh / WORD.height), wh), Image.LANCZOS)
    g = int(height * gap)
    im = Image.new("RGBA", (icon.width + g + word.width, height), (0, 0, 0, 0))
    im.alpha_composite(icon, (0, 0))
    im.alpha_composite(word, (icon.width + g, (height - wh) // 2 + int(height * 0.04)))
    return im


# ---------------------------------------------------------------------------- backgrounds and pieces


def background(w, h, glows=(), seed=1):
    y = np.linspace(0, 1, h)[:, None, None]
    top, bot = np.array(NAVY, np.float32), np.array(NAVY_DEEP, np.float32)
    img = np.broadcast_to(top * (1 - y) + bot * y, (h, w, 3)).copy()
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    for (cx, cy, r, col, strength) in glows:
        d = np.sqrt((xx - cx) ** 2 + (yy - cy) ** 2) / r
        k = np.exp(-d * d * 1.6)[..., None] * strength
        img = img * (1 - k) + np.array(col, np.float32) * k
    # vignette and a fine grain
    vx, vy = (xx / w - 0.5), (yy / h - 0.5)
    img *= (1 - 0.35 * (vx * vx + vy * vy))[..., None]
    rng = np.random.default_rng(seed)
    img += rng.normal(0, 2.2, (h, w, 1))
    return Image.fromarray(np.clip(img, 0, 255).astype(np.uint8), "RGB").convert("RGBA")


def shadow_under(canvas, cx, cy, w, h, alpha=150):
    sh = Image.new("RGBA", canvas.size, (0, 0, 0, 0))
    ImageDraw.Draw(sh).ellipse((cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2), fill=(0, 0, 0, alpha))
    canvas.alpha_composite(sh.filter(ImageFilter.GaussianBlur(h * 0.6)))


def place(canvas, im, box_w, xy, anchor="lt"):
    im = im.resize((box_w, int(im.height * box_w / im.width)), Image.LANCZOS)
    x, y = xy
    if anchor[0] == "m":
        x -= im.width // 2
    if anchor[0] == "r":
        x -= im.width
    if anchor[1] == "m":
        y -= im.height // 2
    if anchor[1] == "b":
        y -= im.height
    canvas.alpha_composite(im, (int(x), int(y)))
    return (int(x), int(y), int(x) + im.width, int(y) + im.height)


def window(shot, width, radius=14, border=(70, 80, 94)):
    """A screenshot as a window card: rounded corners, a hairline border, a soft shadow."""
    im = shot.convert("RGBA").resize((width, int(shot.height * width / shot.width)), Image.LANCZOS)
    m = Image.new("L", im.size, 0)
    ImageDraw.Draw(m).rounded_rectangle((0, 0, im.width - 1, im.height - 1), radius, fill=255)
    im.putalpha(m)
    pad = 60
    card = Image.new("RGBA", (im.width + pad * 2, im.height + pad * 2), (0, 0, 0, 0))
    sh = Image.new("RGBA", card.size, (0, 0, 0, 0))
    ImageDraw.Draw(sh).rounded_rectangle((pad, pad + 14, pad + im.width, pad + im.height + 14), radius, fill=(0, 0, 0, 170))
    card.alpha_composite(sh.filter(ImageFilter.GaussianBlur(22)))
    card.alpha_composite(im, (pad, pad))
    ImageDraw.Draw(card).rounded_rectangle((pad, pad, pad + im.width - 1, pad + im.height - 1), radius, outline=border, width=1)
    return card, pad


def text_lines(d, xy, lines, size, weight="InterDisplay-SemiBold.ttf", spacing=1.18):
    """lines: [(text, color)] or [[(text, color), ...]] for runs on one line."""
    x, y = xy
    f = font(weight, size)
    for line in lines:
        runs = line if isinstance(line, list) else [line]
        cx = x
        for t, col in runs:
            d.text((cx, y), t, font=f, fill=col)
            cx += d.textlength(t, font=f)
        y += int(size * spacing)
    return y


def wrap(d, text, f, width):
    words, lines, cur = text.split(), [], ""
    for w in words:
        t = (cur + " " + w).strip()
        if d.textlength(t, font=f) <= width:
            cur = t
        else:
            lines.append(cur)
            cur = w
    if cur:
        lines.append(cur)
    return lines


def paragraph(d, xy, text, size, width, col=MUTED, weight="Inter-Regular.ttf", spacing=1.45):
    f = font(weight, size)
    x, y = xy
    for line in wrap(d, text, f, width):
        d.text((x, y), line, font=f, fill=col)
        y += int(size * spacing)
    return y


def chips(d, xy, labels, size=19, dot=TEAL):
    x, y = xy
    f = font("Inter-Medium.ttf", size)
    h = int(size * 1.9)
    for label in labels:
        w = int(d.textlength(label, font=f)) + int(size * 2.4)
        d.rounded_rectangle((x, y, x + w, y + h), h // 2, fill=CHIP, outline=(58, 70, 86), width=1)
        r = size * 0.22
        cy = y + h / 2
        d.ellipse((x + size * 0.75 - r, cy - r, x + size * 0.75 + r, cy + r), fill=dot)
        d.text((x + size * 1.35, cy), label, font=f, fill=CREAM, anchor="lm")
        x += w + 12
    return y + h


def bubble(canvas, xy, text, size, max_w, mine=False, label=None):
    d = ImageDraw.Draw(canvas)
    f = font("Inter-Medium.ttf", size)
    lines = wrap(d, text, f, max_w - 40)
    w = int(max(d.textlength(l, font=f) for l in lines)) + 40
    h = int(len(lines) * size * 1.4) + 28
    x, y = xy
    if mine:
        x -= w
    fill = (52, 66, 82) if mine else (246, 236, 224)
    ink = CREAM if mine else (28, 32, 40)
    sh = Image.new("RGBA", canvas.size, (0, 0, 0, 0))
    ImageDraw.Draw(sh).rounded_rectangle((x, y + 6, x + w, y + h + 6), 18, fill=(0, 0, 0, 120))
    canvas.alpha_composite(sh.filter(ImageFilter.GaussianBlur(10)))
    d = ImageDraw.Draw(canvas)
    d.rounded_rectangle((x, y, x + w, y + h), 18, fill=fill)
    ty = y + 14
    for l in lines:
        d.text((x + 20, ty), l, font=f, fill=ink)
        ty += int(size * 1.4)
    if label:
        lf = font("Inter-SemiBold.ttf", 14)
        d.text((x + (w - 4 if mine else 4), y - 8), label, font=lf, fill=MUTED, anchor="rb" if mine else "lb")
    return w, h


def save(im, name):
    OUT.mkdir(parents=True, exist_ok=True)
    p = OUT / name
    im.convert("RGB").save(p, optimize=True)
    print("wrote", p, im.size)


CUBE = Image.open(R / "cube_hero.png").convert("RGBA")
CUBE = CUBE.crop(CUBE.getbbox())


# ---------------------------------------------------------------------------- the images


def hero():
    W, H = 1600, 900
    c = background(W, H, glows=[(1180, 470, 520, (120, 84, 46), 0.55), (1180, 470, 260, (196, 150, 90), 0.18)])
    shadow_under(c, 1185, 772, 520, 70)
    place(c, CUBE, 600, (1185, 455), "mm")
    d = ImageDraw.Draw(c)
    c.alpha_composite(lockup(84), (96, 128))
    d = ImageDraw.Draw(c)
    y = text_lines(d, (96, 250), HEADLINE, 56)
    d.text((98, y + 12), TAGLINE, font=font("InterDisplay-Medium.ttf", 30), fill=GOLD)
    y = paragraph(d, (98, y + 70), "Paint materials, mend meshes, develop textures, and export game-ready assets in one "
                  "native app built around Blender.", 23, 640)
    chips(d, (96, y + 26), CHIPS)
    d.text((1185, 842), "Our logo, painted in Patina.", font=font("Inter-Medium.ttf", 18), fill=MUTED, anchor="mm")
    save(c, "hero.png")


def paint():
    W, H = 1600, 900
    c = background(W, H, glows=[(1130, 450, 560, (60, 74, 96), 0.5)])
    files = ["patina_gold_polished", "patina_copper", "patina_bronze_verdigris", "patina_marble_white",
             "patina_wood_walnut", "patina_leather_brown", "patina_quilted_leather", "patina_velvet_red",
             "patina_car_paint_blue", "patina_carbon_fiber", "patina_diamond_plate_steel", "patina_rust_heavy",
             "patina_knit_wool_cream", "patina_hex_tiles_rosette", "patina_damask_navy_silver", "patina_feathers_peacock"]
    s, gap, x0, y0 = 158, 22, 780, 112
    for i, f in enumerate(files):
        sp = Image.open(R / "spheres" / (f + ".png")).convert("RGBA")
        sp = sp.crop(sp.getbbox()).resize((s, s), Image.LANCZOS)
        x, y = x0 + (i % 4) * (s + gap), y0 + (i // 4) * (s + gap)
        shadow_under(c, x + s / 2, y + s * 0.97, s * 0.7, s * 0.12, 120)
        c.alpha_composite(sp, (x, y))
    d = ImageDraw.Draw(c)
    c.alpha_composite(lockup(46), (96, 88))
    d = ImageDraw.Draw(c)
    y = text_lines(d, (96, 230), [("Paint materials,", CREAM), ("not colors.", GOLD)], 66)
    y = paragraph(d, (98, y + 22), "One stroke lays down color, roughness, metal, height, and normal at once, straight "
                  "across UV seams.", 23, 600)
    y = paragraph(d, (98, y + 14), "270 materials from 74 procedural generators, your own scans, or new ones from the "
                  "material editor.", 23, 560)
    chips(d, (96, y + 26), ["270 materials", "22 smart materials", "Tablet pressure"], dot=GOLD)
    save(c, "paint.png")


def app():
    W, H = 1600, 900
    c = background(W, H, glows=[(800, 640, 700, (90, 70, 50), 0.35)])
    shot = Image.open(SHOTS / "ui_main.png")
    card, pad = window(shot, 1110)
    c.alpha_composite(card, ((W - card.width) // 2, 200 - pad))
    d = ImageDraw.Draw(c)
    c.alpha_composite(lockup(40), (96, 70))
    d = ImageDraw.Draw(c)
    d.text((W - 96, 62), "Everything in one window.", font=font("InterDisplay-SemiBold.ttf", 46), fill=CREAM, anchor="ra")
    d.text((W - 96, 124), "Layers, smart masks, UVs, bakes, a material library, and undo for every step.",
           font=font("Inter-Regular.ttf", 21), fill=MUTED, anchor="ra")
    save(c, "app.png")


def develop():
    W, H = 1600, 900
    c = background(W, H, glows=[(1100, 470, 560, (70, 90, 70), 0.35)])
    before = Image.open(R / "develop_before.png").convert("RGBA")
    after = Image.open(R / "develop_after.png").convert("RGBA")
    box = before.getbbox()
    before, after = before.crop(box), after.crop(box)
    bw = 390
    s = int(before.height * bw / before.width)
    top = 450 - s // 2
    for i, (im, label, col) in enumerate(((before, "As captured", MUTED), (after, "Developed", GOLD))):
        x = 700 + i * 430
        shadow_under(c, x + bw / 2, top + s + 10, bw * 0.6, 40, 130)
        c.alpha_composite(im.resize((bw, s), Image.LANCZOS), (x, top))
        ImageDraw.Draw(c).text((x + bw / 2, top + s + 70), label, font=font("Inter-SemiBold.ttf", 24), fill=col, anchor="mm")
    d = ImageDraw.Draw(c)
    d.line((1110, top + 20, 1110, top + s), fill=(70, 82, 98), width=2)
    c.alpha_composite(lockup(46), (96, 88))
    d = ImageDraw.Draw(c)
    y = text_lines(d, (96, 230), [("Develop textures", CREAM), ("like photos.", GOLD)], 66)
    y = paragraph(d, (98, y + 22), "Exposure, white balance, highlights, shadows, and vibrance on an adjustment layer. "
                  "De-light takes the shadows baked into scans and generated models out.", 23, 540)
    y = paragraph(d, (98, y + 14), "The PBR Range check marks colors no real material has.", 23, 540)
    chips(d, (96, y + 26), ["Non-destructive", "Linear light", "PBR check"], dot=GOLD)
    save(c, "develop.png")


def game_ready():
    W, H = 1600, 900
    c = background(W, H, glows=[(800, 600, 760, (40, 80, 76), 0.35)])
    cards = [(MANUAL / "mesh_check.png", "Mesh Check", "Finds holes, flipped faces, open seams, and loose parts, then mends them in one click."),
             (MANUAL / "collision.png", "Collision", "Boxes, hulls, or convex pieces that hug the shape, named for Unreal or Godot."),
             (MANUAL / "lightmap.png", "Lightmap UVs", "A second UV map where every face has its own space, ready for baked lighting.")]
    x = 96
    cw = 448
    for path, title, line in cards:
        im = Image.open(path).convert("RGBA")
        # the same height for each card's picture
        ph = 390
        im = im.resize((int(im.width * ph / im.height), ph), Image.LANCZOS)
        if im.width > cw:
            off = (im.width - cw) // 2
            im = im.crop((off, 0, off + cw, ph))
        card, pad = window(im, im.width, radius=12)
        c.alpha_composite(card, (x + (cw - im.width) // 2 - pad, 232 - pad))
        d = ImageDraw.Draw(c)
        d.text((x, 660), title, font=font("InterDisplay-SemiBold.ttf", 28), fill=CREAM)
        paragraph(d, (x, 702), line, 20, cw)
        x += cw + 56
    d = ImageDraw.Draw(c)
    c.alpha_composite(lockup(40), (96, 70))
    d = ImageDraw.Draw(c)
    d.text((W - 96, 62), "From download to game-ready.", font=font("InterDisplay-SemiBold.ttf", 46), fill=CREAM, anchor="ra")
    d.text((W - 96, 124), "Reduce polygons and add levels of detail without breaking the textures.",
           font=font("Inter-Regular.ttf", 21), fill=MUTED, anchor="ra")
    d.text((W // 2, 836), "GLB  ·  glTF  ·  OBJ  ·  STL  ·  PLY  ·  Unreal  ·  Unity  ·  Godot  ·  Blender",
           font=font("Inter-Medium.ttf", 20), fill=MUTED, anchor="mm")
    save(c, "game-ready.png")


def batch():
    W, H = 1600, 900
    c = background(W, H, glows=[(560, 560, 600, (70, 64, 96), 0.35)])
    shot = Image.open(MANUAL / "batch_window.png")
    card, pad = window(shot, 600)
    c.alpha_composite(card, (96 - pad, 290 - pad))
    d = ImageDraw.Draw(c)
    c.alpha_composite(lockup(40), (96, 70))
    d = ImageDraw.Draw(c)
    y = text_lines(d, (96, 150), [[("Do it once. ", CREAM), ("Then do the whole folder.", GOLD)]], 52)
    bx = 1504
    by = 330
    w, h = bubble(c, (bx, by), "Finish every model in ~/props for Godot: under 20,000 triangles, convex collision, two levels "
                  "of detail.", 22, 640, mine=True, label="YOU")
    by += h + 36
    w, h = bubble(c, (760, by), "Done. 24 models mended, reduced, and exported as GLB, each with its collision named for "
                  "Godot. Two had holes; both are closed now.", 22, 700, label="AGENT")
    by += h + 40
    d = ImageDraw.Draw(c)
    paragraph(d, (760, by), "Recipes chain the steps; a batch runs one on hundreds of models in the background and never "
              "touches the originals. Any MCP agent can drive all 166 of Patina's tools.", 21, 740)
    save(c, "batch.png")


def x_header():
    W, H = 1500, 500
    c = background(W, H, glows=[(1300, 250, 360, (120, 84, 46), 0.55)])
    shadow_under(c, 1300, 405, 250, 36)
    place(c, CUBE, 280, (1300, 245), "mm")
    lk = lockup(84)
    c.alpha_composite(lk, (380, 128))
    d = ImageDraw.Draw(c)
    d.text((382, 262), " ".join(t for t, _ in HEADLINE), font=font("InterDisplay-Medium.ttf", 29), fill=CREAM)
    d.text((382, 310), TAGLINE, font=font("Inter-Medium.ttf", 22), fill=GOLD)
    save(c, "x-header.png")


def social_preview():
    W, H = 1280, 640
    c = background(W, H, glows=[(960, 330, 420, (120, 84, 46), 0.55)])
    shadow_under(c, 962, 560, 380, 52)
    place(c, CUBE, 440, (962, 318), "mm")
    c.alpha_composite(lockup(70), (72, 70))
    d = ImageDraw.Draw(c)
    y = text_lines(d, (72, 180), HEADLINE, 44)
    d.text((74, y + 10), TAGLINE, font=font("InterDisplay-Medium.ttf", 24), fill=GOLD)
    chips(d, (72, y + 66), CHIPS, size=17)
    save(c, "social-preview.png")


def square():
    W, H = 1080, 1080
    c = background(W, H, glows=[(540, 520, 520, (120, 84, 46), 0.55)])
    shadow_under(c, 540, 760, 460, 64)
    place(c, CUBE, 560, (540, 470), "mm")
    lk = lockup(76)
    c.alpha_composite(lk, ((W - lk.width) // 2, 64))
    d = ImageDraw.Draw(c)
    d.text((W // 2, 862), "Post-process your models", font=font("InterDisplay-SemiBold.ttf", 46), fill=CREAM, anchor="mm")
    d.text((W // 2, 918), "the way you would a photo.", font=font("InterDisplay-SemiBold.ttf", 46), fill=CREAM, anchor="mm")
    d.text((W // 2, 974), TAGLINE, font=font("InterDisplay-Medium.ttf", 26), fill=GOLD, anchor="mm")
    d.text((W // 2, 1032), "Our logo, painted in Patina.", font=font("Inter-Medium.ttf", 20), fill=MUTED, anchor="mm")
    save(c, "square.png")


if __name__ == "__main__":
    which = sys.argv[2:] or ["hero", "paint", "app", "develop", "game_ready", "batch", "x_header", "social_preview", "square"]
    for n in which:
        globals()[n]()
