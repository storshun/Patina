#!/usr/bin/env python3
"""The README's images, in docs/readme/: the logo (icon + wordmark) for light and dark pages, and the promo banners
as JPEGs small enough for a README.

    python3 scripts/promo/readme_images.py [folder with the banners]   # default: build-promo/out (make_banners.py)
"""
import os
import sys

import numpy as np
from PIL import Image

from promo_paths import BRAND, ROOT, WORK

OUT = ROOT / "docs" / "readme"
NAVY = (21, 28, 36)        # the panel the wordmark was drawn on
CREAM = (241, 217, 195)    # its letters
INK = (38, 44, 52)         # the letters on a light page
BANNERS = ["hero", "app", "paint", "develop", "game-ready"]


def wordmark():
    """The wordmark's letters as an alpha mask (from the navy panel it came on), at twice its size."""
    im = Image.open(BRAND / "patina_wordmark.png").convert("RGB")
    im = im.resize((im.width * 2, im.height * 2), Image.LANCZOS)
    a = np.asarray(im).astype(np.float32)
    alpha = np.clip((a.mean(2) - 34.0) / (205.0 - 34.0), 0, 1)
    alpha[:6, :] = alpha[-6:, :] = 0
    alpha[:, :6] = alpha[:, -6:] = 0
    return alpha


def lockup(alpha, color, height=160, gap=0.28):
    icon = Image.open(BRAND / "patina_icon.webp").convert("RGBA")
    icon = icon.crop(icon.getbbox())
    icon = icon.resize((int(icon.width * height / icon.height), height), Image.LANCZOS)
    rgba = np.dstack([np.broadcast_to(np.array(color, np.float32), alpha.shape + (3,)), alpha * 255]).astype(np.uint8)
    word = Image.fromarray(rgba, "RGBA")
    word = word.crop(word.getbbox())
    wh = int(height * 0.62)
    word = word.resize((int(word.width * wh / word.height), wh), Image.LANCZOS)
    g = int(height * gap)
    im = Image.new("RGBA", (icon.width + g + word.width, height), (0, 0, 0, 0))
    im.alpha_composite(icon, (0, 0))
    im.alpha_composite(word, (icon.width + g, (height - wh) // 2 + int(height * 0.04)))
    return im


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    alpha = wordmark()
    lockup(alpha, CREAM).save(OUT / "patina-logo-dark.png", optimize=True)
    lockup(alpha, INK).save(OUT / "patina-logo-light.png", optimize=True)
    src = sys.argv[1] if len(sys.argv) > 1 else str(WORK / "out")
    for name in BANNERS:
        path = os.path.join(src, name + ".png")
        if not os.path.exists(path):
            print("no", path)
            continue
        im = Image.open(path).convert("RGB")
        if im.width > 1600:
            im = im.resize((1600, round(im.height * 1600 / im.width)), Image.LANCZOS)
        im.save(OUT / (name + ".jpg"), quality=86, optimize=True, progressive=True)
    for f in sorted(os.listdir(OUT)):
        print(f, os.path.getsize(OUT / f) // 1024, "KiB")


if __name__ == "__main__":
    main()
