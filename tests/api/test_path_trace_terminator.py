#!/usr/bin/env python3
"""(1.2.3) The path tracer's shadow terminator, against a real headless Patina: under a sun from the side, a coarse
smooth-shaded sphere (16 by 8 quads) looks like a fine one across the line between light and shadow. Before, light
cut off at each flat triangle's horizon and shadow rays leaving the flat triangles hit their neighbors, so the shadow
line ran in triangle-shaped steps (a nose, a cheek, under a hard light).

    PATINA_BIN=build/patina python3 tests/api/test_path_trace_terminator.py
"""
import base64
import io
import math
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


def sphere_obj(path, seg, rings):
    out = []
    for j in range(rings + 1):
        th = math.pi * j / rings
        for i in range(seg):
            ph = 2 * math.pi * i / seg
            out.append("v %f %f %f" % (math.sin(th) * math.cos(ph), math.cos(th), math.sin(th) * math.sin(ph)))
    for j in range(rings):
        for i in range(seg):
            a, b = j * seg + i + 1, j * seg + (i + 1) % seg + 1
            c, d = (j + 1) * seg + (i + 1) % seg + 1, (j + 1) * seg + i + 1
            out.append("f %d %d %d %d" % (a, d, c, b))  # (wound outward)
    open(path, "w").write("\n".join(out) + "\n")


def sun_hdr(path, w=256, h=128):
    """an equirect .hdr (Patina's way round: u 0 looks along +Z): a small sun 35 degrees up to the side (+X, turned to
    the right of the view by the environment's rotation), a dim sky, a darker ground. Up in the sky, the line between
    light and shadow crosses the sphere's faces at a slant (not along its edges)."""
    def rgbe(r, g, b):
        v = max(r, g, b)
        if v < 1e-32:
            return b"\0\0\0\0"
        m, e = math.frexp(v)
        k = m * 256.0 / v
        return bytes((int(r * k), int(g * k), int(b * k), e + 128))
    out = bytearray(b"#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y %d +X %d\n" % (h, w))
    cosSun = math.cos(math.radians(4.0))
    el = math.radians(35.0)
    sun = (math.cos(el), math.sin(el), 0.0)
    for j in range(h):
        th = math.pi * (j + 0.5) / h
        for i in range(w):
            ph = 2 * math.pi * (i + 0.5) / w
            x, y, z = math.sin(th) * math.sin(ph), math.cos(th), math.sin(th) * math.cos(ph)
            if x * sun[0] + y * sun[1] + z * sun[2] > cosSun:
                out += rgbe(150.0, 150.0, 150.0)
            elif y > 0:
                out += rgbe(0.05, 0.05, 0.05)
            else:
                out += rgbe(0.02, 0.02, 0.02)
    open(path, "wb").write(bytes(out))


def gray(png):
    """-> (w, h, values 0-1) of an 8-bit RGB(A) PNG"""
    assert png[:8] == b"\x89PNG\r\n\x1a\n"
    pos, idat = 8, b""
    while pos < len(png):
        n = struct.unpack(">I", png[pos:pos + 4])[0]
        t, d = png[pos + 4:pos + 8], png[pos + 8:pos + 8 + n]
        if t == b"IHDR":
            w, h, depth, ctype = struct.unpack(">IIBB", d[:10])
            ch = {2: 3, 6: 4}[ctype]
        elif t == b"IDAT":
            idat += d
        pos += 12 + n
    raw, stride = zlib.decompress(idat), w * ch
    rows, prev = [], bytearray(stride)
    for y in range(h):
        f, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - ch] if i >= ch else 0
            b, c = prev[i], prev[i - ch] if i >= ch else 0
            if f == 1:
                line[i] = (line[i] + a) & 255
            elif f == 2:
                line[i] = (line[i] + b) & 255
            elif f == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(line)
        prev = line
    return w, h, [[sum(r[x * ch:x * ch + 3]) / 765.0 for x in range(w)] for r in rows]


def main():
    exe = os.path.abspath(os.environ.get("PATINA_BIN") or os.path.join(HERE, "..", "..", "build", "patina"))
    if not os.path.exists(exe):
        print("SKIP: patina binary not found (set PATINA_BIN)")
        return 0
    tmp = tempfile.mkdtemp(prefix="patina_terminator_test_")
    env = dict(os.environ, HOME=os.path.join(tmp, "home"), XDG_DATA_HOME=os.path.join(tmp, "data"),
               XDG_CONFIG_HOME=os.path.join(tmp, "config"), XDG_CACHE_HOME=os.path.join(tmp, "cache"))
    os.makedirs(env["HOME"], exist_ok=True)
    coarse, fine, hdr = os.path.join(tmp, "coarse.obj"), os.path.join(tmp, "fine.obj"), os.path.join(tmp, "sun.hdr")
    sphere_obj(coarse, 16, 8)
    sphere_obj(fine, 128, 64)
    sun_hdr(hdr)
    port = free_port()
    proc = subprocess.Popen([exe, "--headless", "--port", str(port)], env=env, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    try:
        c = protocol.JsonLineClient(port=port, connect_timeout=1.0, response_timeout=300.0)
        for _ in range(100):
            try:
                c.connect()
                break
            except Exception:
                time.sleep(0.1)
        c.request("set_environment", {"hdr_path": hdr, "intensity": 1.0, "rotation": 90})  # (the sun to the right)
        c.request("set_view", {"seams": False, "wireframe": False})
        S = 160
        shots = {}
        for name, path in (("coarse", coarse), ("fine", fine)):
            c.request("open_mesh", {"path": path, "resolution": 512})
            r = c.request("render_view", {"width": S, "height": S, "mode": "path_traced", "samples": 256,
                                          "camera": {"view": "front", "target": [0, 0, 0], "distance": 3.2}})
            png = base64.b64decode(r["image_png_base64"])
            if os.environ.get("PATINA_TEST_DUMP"):  # (to look at them)
                open(os.path.join(os.environ["PATINA_TEST_DUMP"], name + ".png"), "wb").write(png)
            shots[name] = gray(png)[2]
        # (a 5 by 5 blur first: the steps are many pixels wide, the noise one pixel)
        def blur(im):
            return [[sum(im[min(S - 1, max(0, y + j))][min(S - 1, max(0, x + i))] for j in range(-2, 3) for i in range(-2, 3)) / 25.0
                     for x in range(S)] for y in range(S)]
        a, b = blur(shots["coarse"]), blur(shots["fine"])
        # the disk well inside both outlines; the shadow line runs down its middle
        cx = cy = S / 2
        rad = S * 0.24
        diffs = [abs(a[y][x] - b[y][x]) for y in range(S) for x in range(S) if (x - cx) ** 2 + (y - cy) ** 2 < rad * rad]
        lit = sum(b[int(cy)][x] for x in range(int(cx - rad), int(cx + rad))) / (2 * rad)
        mean = sum(diffs) / len(diffs)
        big = sum(d > 0.06 for d in diffs) / len(diffs)
        print("  mean difference %.4f, share off by more than 0.06: %.4f, mean light across the middle %.3f" % (mean, big, lit))
        check(lit > 0.05, "the sun lights the sphere from the side (%.3f)" % lit)
        check(mean < 0.008 and big < 0.02,
              "a coarse sphere's shadow line is as smooth as a fine one's (mean %.4f, %.1f%% far off)" % (mean, big * 100))
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
        shutil.rmtree(tmp, ignore_errors=True)
    print("path trace terminator: %s" % ("all checks passed" if not failures else "%d FAILED" % len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
