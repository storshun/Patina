#!/usr/bin/env python3
"""Developing the textures (0.33) against a real headless Patina: an adjustment layer at its defaults changes nothing;
exposure, temperature, tint, highlights, shadows and vibrance do what a photo editor's do (checked against the math);
De-light brightens what the ambient occlusion darkens; the PBR Range view marks colors no real material has; undo
brings the settings back.

    PATINA_BIN=build/patina python3 tests/api/test_develop.py
"""
import base64
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "blender", "patina_bridge"))
sys.path.insert(0, HERE)
import protocol  # noqa: E402
from test_model_textures import read_png, texel, srgb  # noqa: E402

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


def lin(c):
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def mean(img, ch=3):
    w, h, n, rows = img
    tot = [0.0] * ch
    for y in range(h):
        for x in range(w):
            for c in range(ch):
                tot[c] += rows[y][x * n + c]
    return [t / (w * h * 255) for t in tot]


def main():
    exe = os.path.abspath(os.environ.get("PATINA_BIN") or os.path.join(HERE, "..", "..", "build", "patina"))
    if not os.path.exists(exe):
        print("SKIP: patina binary not found (set PATINA_BIN)")
        return 0
    tmp = tempfile.mkdtemp(prefix="patina_develop_test_")
    env = dict(os.environ, HOME=os.path.join(tmp, "home"), XDG_DATA_HOME=os.path.join(tmp, "data"),
               XDG_CONFIG_HOME=os.path.join(tmp, "config"), XDG_CACHE_HOME=os.path.join(tmp, "cache"))
    os.makedirs(env["HOME"], exist_ok=True)
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

        def color():
            r = c.request("get_texture", {"channel": "basecolor", "size": 64})
            return texel(read_png(base64.b64decode(r["image_png_base64"])), 0.5, 0.5)[:3]

        def develop(fill, adjust):
            """A flat fill and an adjustment layer over it: the color that comes out."""
            c.request("new_project", {"primitive": "plane", "resolution": 256})
            c.request("add_layer", {"type": "fill", "material": {"id": "", "color": list(fill), "metallic": 0.0}})
            c.request("add_layer", {"type": "adjust", "adjust": adjust})
            return color()

        gray = (0.5, 0.5, 0.5)
        got = develop(gray, {})
        check(all(abs(g - 0.5) < 0.01 for g in got), "an adjustment layer at its defaults changes nothing (%s)" % (got,))
        got = develop(gray, {"exposure": 1.0})
        want = srgb(min(1.0, lin(0.5) * 2))
        check(all(abs(g - want) < 0.015 for g in got), "exposure +1: twice the light (%s vs %.3f)" % (got, want))
        got = develop(gray, {"exposure": -1.0})
        want = srgb(lin(0.5) / 2)
        check(all(abs(g - want) < 0.015 for g in got), "exposure -1: half the light (%s vs %.3f)" % (got, want))
        got = develop(gray, {"temperature": 1.0})
        check(got[0] > got[1] > got[2], "warmer: red up, blue down (%s)" % (got,))
        got = develop(gray, {"temperature": -1.0})
        check(got[2] > got[1] > got[0], "cooler: blue up, red down (%s)" % (got,))
        got = develop(gray, {"tint": 1.0})
        check(got[1] < got[0] - 0.03 and abs(got[0] - got[2]) < 0.01, "tint to magenta: less green (%s)" % (got,))
        dark, bright = (0.15, 0.15, 0.15), (0.92, 0.92, 0.92)
        up = develop(dark, {"shadows": 1.0})[0]
        check(up > 0.15 + 0.04, "shadows +1 lifts a dark color (%.3f)" % up)
        same = develop(bright, {"shadows": 1.0})[0]
        check(abs(same - 0.92) < 0.01, "and leaves a bright one (%.3f)" % same)
        down = develop(bright, {"highlights": -1.0})[0]
        check(down < 0.92 - 0.06, "highlights -1 darkens a bright color (%.3f)" % down)
        same = develop(dark, {"highlights": -1.0})[0]
        check(abs(same - 0.15) < 0.01, "and leaves a dark one (%.3f)" % same)
        dull = (0.5, 0.45, 0.4)
        v = develop(dull, {"vibrance": 1.0})
        check(v[0] - v[2] > 0.1 + 0.03, "vibrance +1 gives a dull color more color (%s)" % (v,))
        v = develop(dull, {"vibrance": -1.0})
        check(v[0] - v[2] < 0.1 - 0.03, "vibrance -1 takes color out (%s)" % (v,))

        # texture_check (0.34): numbers to tune by. A too-dark gray is flagged, exposure fixes it.
        c.request("new_project", {"primitive": "sphere", "resolution": 256})
        c.request("add_layer", {"type": "fill", "material": {"id": "", "color": [0.08, 0.08, 0.08], "metallic": 0.0}})
        tc = c.request("texture_check", {})
        check(tc["basecolor"]["non_metal_too_dark_pct"] > 95 and not tc["pbr_ok"] and any("exposure" in h for h in tc["hints"]),
              "texture_check flags a too-dark color and names exposure (%s)" % tc["hints"])
        r = c.request("add_layer", {"type": "adjust", "adjust": {"exposure": 1.5}})
        tc = c.request("texture_check", {})
        check(tc["pbr_ok"] and tc["basecolor"]["non_metal_too_dark_pct"] < 1 and tc["basecolor"]["luminance"]["p50"] > 30 / 255,
              "after exposure +1.5 it is in range (p50 %.3f)" % tc["basecolor"]["luminance"]["p50"])
        c.request("update_layer", {"index": r["index"], "adjust": {"temperature": 1.0}})
        tc = c.request("texture_check", {})
        check(tc["basecolor"]["cast"]["warm"] > 0.05, "temperature +1 shows as a warm cast (%.3f)" % tc["basecolor"]["cast"]["warm"])
        c.request("new_project", {"primitive": "sphere", "resolution": 256})
        c.request("add_layer", {"type": "fill", "material": {"id": "", "color": [0.5, 0.5, 0.5], "metallic": 0.5}})
        tc = c.request("texture_check", {})
        check(tc["metallic"]["half_metal_pct"] > 95 and any("half-way" in h for h in tc["hints"]), "half-metal is flagged")

        # De-light: a model with crevices, its AO baked; the occluded parts get brighter, the open ones stay
        c.request("open_file", {"path": os.path.join(HERE, "..", "..", "samples", "Suzanne.glb")})
        c.request("add_layer", {"type": "fill", "material": {"id": "", "color": [0.6, 0.6, 0.6]}})
        c.request("bake_mesh_maps", {"ao": True})
        ao = read_png(base64.b64decode(c.request("get_texture", {"channel": "ao", "size": 128})["image_png_base64"]))
        before = read_png(base64.b64decode(c.request("get_texture", {"channel": "basecolor", "size": 128})["image_png_base64"]))
        r = c.request("add_layer", {"type": "adjust", "adjust": {"delight": 1.0}})
        after = read_png(base64.b64decode(c.request("get_texture", {"channel": "basecolor", "size": 128})["image_png_base64"]))
        w, h, n, rows = ao
        dark_px = [(x, y) for y in range(h) for x in range(w) if rows[y][x * n] < 140]
        open_px = [(x, y) for y in range(h) for x in range(w) if rows[y][x * n] > 250]
        gain_dark = sum(after[3][y][x * after[2]] - before[3][y][x * before[2]] for x, y in dark_px) / max(1, len(dark_px))
        gain_open = sum(after[3][y][x * after[2]] - before[3][y][x * before[2]] for x, y in open_px) / max(1, len(open_px))
        check(len(dark_px) > 20 and gain_dark > 15, "De-light brightens the occluded parts (%d texels, +%.1f)" % (len(dark_px), gain_dark))
        check(abs(gain_open) < 2, "and leaves the open ones (%.1f)" % gain_open)

        # undo brings the settings back
        idx = r["index"]
        c.request("update_layer", {"index": idx, "adjust": {"delight": 0.0, "exposure": 0.5}})
        lay = c.request("list_layers", {})[idx]
        c.request("undo", {})
        lay2 = c.request("list_layers", {})[idx]
        a1, a2 = (lay.get("adjust") or {}), (lay2.get("adjust") or {})
        check(a1.get("exposure") == 0.5 and a2.get("exposure") == 0 and a2.get("delight") == 1,
              "undo brings the earlier settings back (%s -> %s)" % ({k: a1.get(k) for k in ("exposure", "delight")},
                                                                     {k: a2.get(k) for k in ("exposure", "delight")}))

        # the PBR Range view: a black non-metal is marked red, a mid gray is not
        def view_mid(fill, metallic=0.0):
            c.request("new_project", {"primitive": "sphere", "resolution": 256})
            c.request("add_layer", {"type": "fill", "material": {"id": "", "color": list(fill), "metallic": metallic}})
            img = c.request("render_view", {"width": 128, "height": 128, "mode": "pbr_range", "camera": {"view": "front", "frame": True}})
            return texel(read_png(base64.b64decode(img["image_png_base64"])), 0.5, 0.5)[:3]
        black = view_mid((0.02, 0.02, 0.02))
        check(black[0] > 0.5 and black[1] < 0.3, "PBR Range: a black non-metal is red (%s)" % (black,))
        fine = view_mid((0.5, 0.5, 0.5))
        check(abs(fine[0] - fine[1]) < 0.03 and abs(fine[1] - fine[2]) < 0.03, "a mid gray is fine (gray) (%s)" % (fine,))
        metal = view_mid((0.3, 0.3, 0.3), 1.0)
        check(metal[0] > 0.6 and metal[2] < 0.3, "a dark metal is orange (%s)" % (metal,))
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
        shutil.rmtree(tmp, ignore_errors=True)
    print("develop: %s" % ("all checks passed" if not failures else "%d FAILED" % len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
