#!/usr/bin/env python3
"""(1.2) Brushes from the user's brushes folder, against a real headless Patina: each image there (and in its folders,
a group each) is a brush; a dark tip on a light ground paints with its dark part, one with transparency with its
opaque part; a file added later comes in on reload_brushes, one removed goes; import_brushes copies images and
Photoshop .abr files (versions 2 and 6, each sampled tip a brush, raw and PackBits) into the folder.

    PATINA_BIN=build/patina python3 tests/api/test_brush_folder.py
"""
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "blender", "patina_bridge"))
import protocol  # noqa: E402

failures = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        failures.append(what)


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def png(path, w, h, pixel, channels=1):
    """a PNG of w x h; pixel(x, y) gives the gray value (channels 1) or (gray, alpha) (channels 2)"""
    rows = []
    for y in range(h):
        row = bytearray([0])
        for x in range(w):
            v = pixel(x, y)
            row += bytes(v) if channels == 2 else bytes([v])
        rows.append(bytes(row))
    raw = b"".join(rows)
    chunk = lambda t, d: struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    color = 0 if channels == 1 else 4
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, color, 0, 0, 0)) +
                           chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def disc(x, y, n, r):
    return (x - n / 2 + 0.5) ** 2 + (y - n / 2 + 0.5) ** 2 < r * r


def packbits(row):
    out = bytearray()
    i = 0
    while i < len(row):
        j = i
        while j + 1 < len(row) and row[j + 1] == row[i] and j - i < 127:
            j += 1
        if j > i:
            out += bytes([257 - (j - i + 1)]) + bytes([row[i]])  # a run
            i = j + 1
        else:
            out += bytes([0, row[i]])
            i += 1
    return bytes(out)


def sample_image(n, kind):
    return [255 if (disc(x, y, n, n * 0.3) if kind == 0 else (x + y) % 8 < 4) else 0 for y in range(n) for x in range(n)]


def abr_v2(path, n=24):
    """a Photoshop 6 brush file: two sampled tips (raw, PackBits)"""
    body = b""
    for k in range(2):
        pix = sample_image(n, k)
        name = "Tip%d" % k
        d = struct.pack(">IH", 0, 25) + struct.pack(">I", len(name)) + "".join(name).encode("utf-16-be")
        d += bytes([1]) + struct.pack(">4h", 0, 0, n, n) + struct.pack(">4i", 0, 0, n, n) + struct.pack(">H", 8)
        if k == 0:
            d += bytes([0]) + bytes(pix)
        else:
            rows = [packbits(bytes(pix[y * n:(y + 1) * n])) for y in range(n)]
            d += bytes([1]) + b"".join(struct.pack(">H", len(r)) for r in rows) + b"".join(rows)
        body += struct.pack(">HI", 2, len(d)) + d
    open(path, "wb").write(struct.pack(">HH", 2, 2) + body)


def abr_v6(path, n=20, count=3):
    """a Photoshop 7+ brush file: a "samp" section with sampled tips (subversion 2: 301 bytes before the bounds)"""
    samples = b""
    for k in range(count):
        pix = sample_image(n, k % 2)
        d = bytes(301) + struct.pack(">4i", 0, 0, n, n) + struct.pack(">h", 8) + bytes([0]) + bytes(pix)
        pad = (4 - len(d) % 4) % 4
        samples += struct.pack(">I", len(d)) + d + bytes(pad)
    sec = b"8BIMsamp" + struct.pack(">I", len(samples)) + samples
    patt = b"8BIMpatt" + struct.pack(">I", 0)
    open(path, "wb").write(struct.pack(">HH", 6, 2) + sec + patt)


class Patina:
    def __init__(self, exe, env):
        self.port = free_port()
        self.proc = subprocess.Popen([exe, "--headless", "--port", str(self.port)], env=env,
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.c = protocol.JsonLineClient(port=self.port, connect_timeout=1.0, response_timeout=300.0)
        for _ in range(100):
            try:
                self.c.connect()
                break
            except Exception:
                time.sleep(0.1)

    def __call__(self, cmd, **a):
        return self.c.request(cmd, a)

    def stop(self):
        self.proc.terminate()
        try:
            self.proc.wait(10)
        except subprocess.TimeoutExpired:
            self.proc.kill()


def red_at(path, u, v):
    from PIL import Image  # noqa: PLC0415
    im = Image.open(path).convert("RGB")
    p = im.getpixel((min(im.width - 1, int(u * im.width)), min(im.height - 1, int((1 - v) * im.height))))
    return p[0] > 160 and p[1] < 90


def main():
    exe = os.environ.get("PATINA_BIN") or os.path.join(HERE, "..", "..", "build", "patina")
    tmp = tempfile.mkdtemp(prefix="patina_brush_folder_test_")
    env = dict(os.environ, HOME=os.path.join(tmp, "home"), XDG_DATA_HOME=os.path.join(tmp, "data"),
               XDG_CONFIG_HOME=os.path.join(tmp, "config"), XDG_CACHE_HOME=os.path.join(tmp, "cache"), PATINA_EXE=exe)
    os.makedirs(env["HOME"], exist_ok=True)
    folder = os.path.join(tmp, "data", "patina", "user", "brushes")
    os.makedirs(os.path.join(folder, "Grunge"), exist_ok=True)
    N = 64
    png(os.path.join(folder, "dark_disc.png"), N, N, lambda x, y: 0 if disc(x, y, N, 20) else 255)          # black on white
    png(os.path.join(folder, "Grunge", "light_disc.png"), N, N, lambda x, y: 255 if disc(x, y, N, 20) else 0)  # white on black
    png(os.path.join(folder, "clear_disc.png"), N, N, lambda x, y: (40, 255 if disc(x, y, N, 20) else 0), 2)  # transparency
    png(os.path.join(folder, ".hidden.png"), 8, 8, lambda x, y: 128)
    try:
        p = Patina(exe, env)
        try:
            brushes = {b["id"]: b for b in p("list_brushes")}
            check({"file:dark_disc.png", "file:Grunge/light_disc.png", "file:clear_disc.png"} <= set(brushes),
                  "each image in the folder is a brush (%s)" % sorted(k for k in brushes if k.startswith("file:")))
            check(brushes.get("file:Grunge/light_disc.png", {}).get("category") == "Yours: Grunge", "a folder in it is a group")
            check(next(iter(brushes)).startswith("file:"), "your brushes come first")
            check(not any(".hidden" in k for k in brushes), "hidden files are left out")
            # ---- which part paints: a dab in the middle of a plane's UVs, then the middle and a corner of the tip
            p("new_project", primitive="plane", resolution=512)
            p("add_layer", type="paint")
            for i, bid in enumerate(["file:dark_disc.png", "file:Grunge/light_disc.png", "file:clear_disc.png"]):
                p("set_brush", preset=bid)
                p("paint_stroke", space="uv", points=[[0.25 + 0.25 * i, 0.5]], brush={"size": 100, "spacing": 5},
                  material={"id": "", "color": [0.95, 0.05, 0.05]})
            files = p("export_textures", directory=os.path.join(tmp, "out"), size=512)["files"]
            for i, bid in enumerate(["dark", "light", "transparent"]):
                u = 0.25 + 0.25 * i
                check(red_at(files["basecolor"], u, 0.5) and not red_at(files["basecolor"], u + 0.075, 0.5 + 0.075),
                      "the %s tip paints with its shape, not its ground" % bid)
            # ---- added and removed while Patina runs
            png(os.path.join(folder, "later.png"), N, N, lambda x, y: 255 if disc(x, y, N, 10) else 0)
            os.remove(os.path.join(folder, "clear_disc.png"))
            r = p("reload_brushes")
            ids = {b["id"] for b in p("list_brushes")}
            check("file:later.png" in ids and "file:clear_disc.png" not in ids and r["change"] == 0,
                  "a file added comes in, one removed goes (%s)" % r)
            # ---- import: images and Photoshop brushes
            src = os.path.join(tmp, "elsewhere")
            os.makedirs(src)
            png(os.path.join(src, "stamp.png"), N, N, lambda x, y: 0 if disc(x, y, N, 25) else 255)
            abr_v2(os.path.join(src, "old.abr"))
            abr_v6(os.path.join(src, "new.abr"))
            r = p("import_brushes", paths=[os.path.join(src, f) for f in ("stamp.png", "old.abr", "new.abr")], category="Mine")
            check(r["ids"][0] == "file:Mine/stamp.png" and os.path.exists(os.path.join(folder, "Mine", "stamp.png")),
                  "an image imported is copied into the folder (%s)" % r["ids"][:1])
            old = [i for i in r["ids"] if i.startswith("file:Mine/old.abr#")]
            new = [i for i in r["ids"] if i.startswith("file:Mine/new.abr#")]
            check(len(old) == 2 and len(new) == 3, "each tip of a Photoshop brush file is a brush (%d and %d)" % (len(old), len(new)))
            st = p("get_state")["brush"]
            check(st["id"] == "file:Mine/stamp.png", "and the first one is painted with (%s)" % st["id"])
            # the tips came out as drawn: the PackBits one too (a checker of 4 px stripes)
            br = {b["id"]: b for b in p("list_brushes")}
            from PIL import Image  # noqa: PLC0415
            tip = Image.open(br["file:Mine/old.abr#2"]["image"]).convert("L")
            stripes = [tip.getpixel((x, 0)) for x in range(8)]
            check(tip.size == (24, 24) and stripes == [255, 255, 255, 255, 0, 0, 0, 0], "a PackBits tip reads right (%s)" % stripes)
            tip = Image.open(br["file:Mine/new.abr#1"]["image"]).convert("L")
            check(tip.size == (20, 20) and tip.getpixel((10, 10)) == 255 and tip.getpixel((0, 0)) == 0, "a Photoshop 7+ tip reads right")
            check(br["file:Mine/new.abr#1"]["category"] == "Yours: Mine / new", "grouped by the file")
        finally:
            p.stop()
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("brush folder: %s" % ("all checks passed" if not failures else "%d FAILED" % len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
