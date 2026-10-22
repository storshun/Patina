#!/usr/bin/env python3
"""(1.0) Makes the PNG screenshots in a folder smaller for a package: each becomes a 256-color PNG (libimagequant,
dithered) when that keeps it close to the original (PSNR 34 dB or more) and saves at least 15%. The packages'
manual images went from 24 MB to about 8; the source tree keeps the full images.

    python3 scripts/shrink_images.py <folder> [<folder>...]
"""
import math
import os
import sys

from PIL import Image, ImageChops, ImageStat, features


def shrink(path):
    size = os.path.getsize(path)
    im = Image.open(path)
    if im.mode == "P" or size < 20000:
        return size, size
    rgba = im.convert("RGBA")
    alpha = rgba.getextrema()[3][0] < 255
    src = rgba if alpha else rgba.convert("RGB")
    method = Image.Quantize.LIBIMAGEQUANT if features.check_feature("libimagequant") else (
        Image.Quantize.FASTOCTREE if alpha else Image.Quantize.MEDIANCUT)
    q = src.quantize(colors=256, method=method, dither=Image.Dither.FLOYDSTEINBERG)
    diff = ImageChops.difference(src.convert("RGB"), q.convert("RGB"))
    mse = sum(v * v for v in ImageStat.Stat(diff).rms) / 3
    psnr = 99.0 if mse == 0 else 10 * math.log10(255 ** 2 / mse)
    tmp = path + ".shrink.png"
    q.save(tmp, optimize=True)
    small = os.path.getsize(tmp)
    if psnr >= 34 and small < size * 0.85:
        os.replace(tmp, path)
        return size, small
    os.remove(tmp)
    return size, size


def main():
    before = after = 0
    for folder in sys.argv[1:]:
        for name in sorted(os.listdir(folder)):
            if name.lower().endswith(".png"):
                b, a = shrink(os.path.join(folder, name))
                before += b
                after += a
    print("images: %.1f MB -> %.1f MB" % (before / 1e6, after / 1e6))


if __name__ == "__main__":
    main()
