#!/usr/bin/env python3
"""The asset shelf's pictures are read on worker threads (1.2.15), in the real window, Blender layout, with a library
of the user's own (brushes and image materials made here):
- opening the Paint workspace on the shelf's Brushes, All, holds no frame (every tip's image was read in one frame:
  Randall's 610 brush images held the window for 9 seconds), and the tiles get their pictures soon after;
- the user's 4096-pixel image materials hold no frame either (each was read whole for its picture, two a frame:
  frames of over a second), and their tiles get their pictures;
- the border between the shelf's catalogs and its tiles is dragged (it was a fixed width), not below 100 pixels, and
  kept in the session.

    python3 tests/gui/test_library_loading.py [path/to/patina]
"""
import json
import os
import shutil
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import manual_shots  # noqa: E402

BIN = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "patina")
failures = []
N_BRUSHES = 160
N_MATERIALS = 6


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        failures.append(what)


def make_library(data):
    """XDG_DATA_HOME/patina/user: a brush folder of big tip images, and big image materials (as the user's own)."""
    from PIL import Image, ImageDraw
    user = os.path.join(data, "patina", "user")
    folder = os.path.join(user, "brushes", "Big Tips")
    os.makedirs(folder)
    im = Image.new("L", (1536, 1536), 255)
    d = ImageDraw.Draw(im)
    for k in range(40):  # (dark strokes on white: a Photoshop-style tip)
        x = (k * 397) % 1400
        d.ellipse([x, (k * 211) % 1300, x + 160, (k * 211) % 1300 + 240], fill=0)
    first = os.path.join(folder, "tip_000.jpg")
    im.convert("RGB").save(first, quality=90)
    for i in range(1, N_BRUSHES):
        shutil.copy(first, os.path.join(folder, "tip_%03d.jpg" % i))
    m = Image.new("RGB", (4096, 4096), (0, 0, 0))
    d = ImageDraw.Draw(m)
    for k in range(0, 4096, 512):  # (white bands: a mask)
        d.rectangle([k, 0, k + 255, 4095], fill=(255, 255, 255))
    for i in range(N_MATERIALS):
        mid = "img_big_%d" % i
        md = os.path.join(user, "materials", mid)
        os.makedirs(md)
        if i == 0:
            m.save(os.path.join(md, "mask.png"))
            src = os.path.join(md, "mask.png")
        else:
            shutil.copy(src, os.path.join(md, "mask.png"))
        json.dump({"id": mid, "name": "Big %d" % i, "category": "Imported", "type": "image",
                   "maps": {"basecolor": "mask.png"}, "default_scale": 1.0, "description": "Image", "tags": []},
                  open(os.path.join(md, "material.json"), "w"))


def main():
    data = tempfile.mkdtemp(prefix="patina_library_loading_")
    make_library(data)
    os.environ["XDG_DATA_HOME"] = data  # (Patina reads the user's library from there)
    s = manual_shots.Session(BIN, os.path.join("/tmp", "patina_library_loading_test"), display=":96", port=7907, session={
        "layout": "blender", "guided_step": 0, "materials_tile": 48.0, "materials_list": False,
        "blender_layout": {"shelf_catalog": 3, "brush_category": "", "shelf": 300}})

    def rect(name):
        r = s("ui_rect", name=name)
        return r if r.get("found") else None

    def click(r):
        t0 = time.time()
        s.x.click(int(r["x"] + r["w"] / 2), int(r["y"] + r["h"] / 2))
        return time.time() - t0

    def longest_frame(seconds):
        """The longest wait for one frame over `seconds`."""
        worst, t0 = 0.0, time.time()
        while time.time() - t0 < seconds:
            a = time.time()
            s("wait_frames", count=1)
            worst = max(worst, time.time() - a)
        return worst

    def pictured(prefix, bright=200):
        """How many of the tiles under `prefix` have a picture: a part of them white (a tile still waiting is the
        button's color and the spinner's)."""
        im = s.image()
        n = 0
        rects = s("ui_rect", prefix=prefix)["rects"]
        for r in rects.values():
            if r["y"] < 0 or r["y"] + r["h"] > im.height:
                continue
            box = im.crop((int(r["x"] + 4), int(r["y"] + 4), int(r["x"] + r["w"] - 4), int(r["y"] + r["h"] - 4)))
            px = box.convert("L").tobytes()
            n += sum(v >= bright for v in px) > len(px) * 0.03
        return n, len(rects)

    try:
        s.settle(20, 2)
        tab = rect("blender.step.paint")
        took = click(tab)
        worst = max(took, longest_frame(3.0))
        check(worst < 0.6, "the Paint workspace opens on all the brushes holding no frame (longest %.2f s)" % worst)
        n, total = 0, 0
        for _ in range(40):
            n, total = pictured("brushes.tile:")
            if total and n == total:
                break
            time.sleep(0.5)
        check(total >= 10 and n == total, "the brush tiles on screen get their pictures (%d of %d)" % (n, total))

        # the user's big image materials
        click(rect("shelf.kind.Materials"))
        a = rect("shelf.materials")
        s.x.move(int(a["x"] + 40), int(a["y"] + 40))
        for _ in range(15):
            s.x.button(5, True)
            s.x.button(5, False)
        s.settle(5, 1)
        took = click(rect("shelf.materials:Imported"))
        worst = max(took, longest_frame(3.0))
        check(worst < 0.6, "the big image materials show holding no frame (longest %.2f s)" % worst)
        for _ in range(40):
            n, total = pictured("materials.tile:img_big_", bright=120)
            if total and n == total:
                break
            time.sleep(0.5)
        check(total == N_MATERIALS and n == total, "the image materials get their pictures (%d of %d)" % (n, total))
        s.shot("library_loading")

        # the border between the catalogs and the tiles
        b0 = rect("shelf.border")
        check(b0 is not None, "the border between the catalogs and the tiles is there")
        if b0:
            tile0 = min(r["x"] for r in s("ui_rect", prefix="materials.tile:")["rects"].values())
            x0, y0 = int(b0["x"] + b0["w"] / 2), int(b0["y"] + b0["h"] / 2)
            s.x.drag([(x0, y0)] + [(x0 + 20 * k, y0) for k in range(1, 7)])
            s.settle(5, 0.5)
            b1 = rect("shelf.border")
            tile1 = min(r["x"] for r in s("ui_rect", prefix="materials.tile:")["rects"].values())
            check(abs((b1["x"] - b0["x"]) - 120) <= 4 and abs((tile1 - tile0) - 120) <= 4,
                  "the border dragged 120 pixels widens the catalogs, the tiles move over (%+.0f, %+.0f)" %
                  (b1["x"] - b0["x"], tile1 - tile0))
            x1 = int(b1["x"] + b1["w"] / 2)
            s.x.drag([(x1, y0)] + [(max(12, x1 - 60 * k), y0) for k in range(1, 8)])  # (inside the window)
            s.settle(5, 0.5)
            b2 = rect("shelf.border")
            check(abs((b2["x"] - b0["x"]) + 75) <= 4, "dragged far left it stops at 100 pixels (%+.0f)" % (b2["x"] - b0["x"]))
            x2 = int(b2["x"] + b2["w"] / 2)
            s.x.drag([(x2, y0)] + [(x2 + 25 * k, y0) for k in range(1, 5)])
            s.settle(5, 0.5)
            time.sleep(6.0)  # (the session is written every few seconds)
            sess = json.load(open(os.path.join(s.home, ".config", "patina", "session.json")))
            w = sess.get("blender_layout", {}).get("shelf_catalogs", 0)
            check(abs(w - 200) <= 4, "its width is kept in the session (%.0f)" % w)
    finally:
        s.close()
        shutil.rmtree(data, ignore_errors=True)
    print("library loading: %s" % ("all checks passed" if not failures else "%d FAILED" % len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
