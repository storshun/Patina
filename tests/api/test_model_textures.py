#!/usr/bin/env python3
"""A model's own textures (0.31) against a real headless Patina: opening a textured .gltf and .obj brings each
material slot's maps and colors in as the paint layer "From the model" (color, roughness and metallic from glTF's
packed map, normal, glow, cut-out opacity), in the right places; one undo takes it away; the command does it again;
an OBJ's color texture alpha is the opacity when it cuts out (1.2.2);
missing textures are reported; plain gray models and the feature switched off bring nothing; big textures grow the
texture set.

    PATINA_BIN=build/patina python3 tests/api/test_model_textures.py
"""
import base64
import json
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


# ------------------------------------------------------------------ PNG in and out (no extra packages)

def write_png(path, w, h, pixel, gray=False):
    """pixel(x, y) -> (r, g, b, a) 0-255 (gray: (v,)), y = 0 the top row."""
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        for x in range(w):
            raw.extend(pixel(x, y))
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    data = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 0 if gray else 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + chunk(b"IEND", b""))
    open(path, "wb").write(data)


def read_png(data):
    """-> (w, h, channels, rows) with rows[y][x * channels + c], 8-bit only."""
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    pos, idat, w = 8, b"", 0
    while pos < len(data):
        n = struct.unpack(">I", data[pos:pos + 4])[0]
        t = data[pos + 4:pos + 8]
        d = data[pos + 8:pos + 8 + n]
        if t == b"IHDR":
            w, h, depth, ctype = struct.unpack(">IIBB", d[:10])
            assert depth == 8, "8-bit PNGs only"
            ch = {0: 1, 2: 3, 4: 2, 6: 4}[ctype]
        elif t == b"IDAT":
            idat += d
        pos += 12 + n
    raw = zlib.decompress(idat)
    stride = w * ch
    rows, prev = [], bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - ch] if i >= ch else 0
            b = prev[i]
            c = prev[i - ch] if i >= ch else 0
            if f == 1: line[i] = (line[i] + a) & 255
            elif f == 2: line[i] = (line[i] + b) & 255
            elif f == 3: line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(line)
        prev = line
    return w, h, ch, rows


def texel(img, u, v):
    """The map's value at UV (u, v), v up: tuple of 0-1 floats."""
    w, h, ch, rows = img
    x = min(w - 1, int(u * w))
    y = min(h - 1, int((1 - v) * h))
    return tuple(rows[y][x * ch + c] / 255 for c in range(ch))


# ------------------------------------------------------------------ the models

def srgb(c):
    return 12.92 * c if c <= 0.0031308 else 1.055 * c ** (1 / 2.4) - 0.055


def make_gltf(d, name="box.gltf", color_uri="tex_color.png", big=False):
    """Two quads: A (slot 0, textured: color + cut-out alpha, packed roughness/metallic, normal) over UV x 0-0.55,
    B (slot 1: flat green, metallic, glowing) over UV (0.65-0.95, 0.1-0.4)."""
    pos = [(-1, 0, -1), (0, 0, -1), (0, 0, 1), (-1, 0, 1), (0.2, 0, -1), (1, 0, -1), (1, 0, 1), (0.2, 0, 1)]
    nrm = [(0, 1, 0)] * 8
    # glTF UVs: v down
    uvA = [(0, 1), (0.55, 1), (0.55, 0), (0, 0)]
    uvB = [(0.65, 0.9), (0.95, 0.9), (0.95, 0.6), (0.65, 0.6)]
    uv = uvA + uvB
    idxA = [0, 2, 1, 0, 3, 2]
    idxB = [4, 6, 5, 4, 7, 6]
    b = bytearray()
    for p in pos: b += struct.pack("<3f", *p)
    for n in nrm: b += struct.pack("<3f", *n)
    for t in uv: b += struct.pack("<2f", *t)
    for i in idxA + idxB: b += struct.pack("<H", i)
    open(os.path.join(d, name.replace(".gltf", ".bin")), "wb").write(bytes(b))
    g = {
        "asset": {"version": "2.0"},
        "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
        "buffers": [{"uri": name.replace(".gltf", ".bin"), "byteLength": len(b)}],
        "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 96}, {"buffer": 0, "byteOffset": 96, "byteLength": 96},
                        {"buffer": 0, "byteOffset": 192, "byteLength": 64}, {"buffer": 0, "byteOffset": 256, "byteLength": 12},
                        {"buffer": 0, "byteOffset": 268, "byteLength": 12}],
        "accessors": [{"bufferView": 0, "componentType": 5126, "count": 8, "type": "VEC3", "min": [-1, 0, -1], "max": [1, 0, 1]},
                      {"bufferView": 1, "componentType": 5126, "count": 8, "type": "VEC3"},
                      {"bufferView": 2, "componentType": 5126, "count": 8, "type": "VEC2"},
                      {"bufferView": 3, "componentType": 5123, "count": 6, "type": "SCALAR"},
                      {"bufferView": 4, "componentType": 5123, "count": 6, "type": "SCALAR"}],
        "meshes": [{"primitives": [
            {"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}, "indices": 3, "material": 0},
            {"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}, "indices": 4, "material": 1}]}],
        "images": [{"uri": color_uri}, {"uri": "tex_mr.png"}, {"uri": "tex_normal.png"}] +
                  ([{"uri": "tex_big.png"}] if big else []),
        "textures": [{"source": 0}, {"source": 1}, {"source": 2}] + ([{"source": 3}] if big else []),
        "materials": [
            {"name": "Painted", "alphaMode": "MASK", "alphaCutoff": 0.5,
             "pbrMetallicRoughness": {"baseColorTexture": {"index": 0}, "metallicRoughnessTexture": {"index": 1},
                                      "metallicFactor": 1.0, "roughnessFactor": 1.0},
             "normalTexture": {"index": 2}},
            {"name": "Green", "pbrMetallicRoughness": {"baseColorFactor": [0.05, 0.6, 0.1, 1], "roughnessFactor": 0.3},
             "emissiveFactor": [0.0, 0.5, 0.0]} if not big else
            {"name": "Green", "pbrMetallicRoughness": {"baseColorTexture": {"index": 3}}}],
    }
    open(os.path.join(d, name), "w").write(json.dumps(g))


NORMAL = (0.3, 0.2, (1 - 0.3 ** 2 - 0.2 ** 2) ** 0.5)
NORMAL_PX = tuple(round((c * 0.5 + 0.5) * 255) for c in NORMAL)


def make_textures(d):
    S = 64
    # color: red where u < 0.5, blue beyond; alpha 0 in the top-left corner (glTF v down: u < 0.2, v_gltf < 0.2)
    write_png(os.path.join(d, "tex_color.png"), S, S,
              lambda x, y: (220, 20, 20, 0 if (x < S * 0.2 and y < S * 0.2) else 255) if x < S // 2 else (20, 20, 220, 255))
    # roughness in G: 0.2 in the top half of the image (v_gltf < 0.5), 0.9 below; metallic in B: 0
    write_png(os.path.join(d, "tex_mr.png"), S, S, lambda x, y: (0, 51 if y < S // 2 else 230, 0, 255))
    write_png(os.path.join(d, "tex_normal.png"), S, S, lambda x, y: NORMAL_PX + (255,))
    write_png(os.path.join(d, "tex_gray_mask.png"), S, S, lambda x, y: (0,) if x < S // 4 else (255,), gray=True)


def make_obj(d):
    obj = ("mtllib box.mtl\n"
           "v -1 0 -1\nv 0 0 -1\nv 0 0 1\nv -1 0 1\nv 0.2 0 -1\nv 1 0 -1\nv 1 0 1\nv 0.2 0 1\n"
           "vt 0 0\nvt 0.55 0\nvt 0.55 1\nvt 0 1\nvt 0.65 0.1\nvt 0.95 0.1\nvt 0.95 0.4\nvt 0.65 0.4\n"
           "usemtl Painted\nf 1/1 4/4 3/3 2/2\nusemtl Plain\nf 5/5 8/8 7/7 6/6\n")
    mtl = ("newmtl Painted\nKd 0.8 0.8 0.8\nNs 250\nmap_Kd textures/tex_color.png\nPr 0.4\nmap_d tex_gray_mask.png\n"
           "newmtl Plain\nKd 0.9 0.6 0.1\nPr 0.7\nPm 1\n")
    open(os.path.join(d, "box.obj"), "w").write(obj)
    open(os.path.join(d, "box.mtl"), "w").write(mtl)
    os.makedirs(os.path.join(d, "textures"), exist_ok=True)
    shutil.copy(os.path.join(d, "tex_color.png"), os.path.join(d, "textures", "tex_color.png"))
    # a plain gray model: nothing to bring in
    open(os.path.join(d, "gray.obj"), "w").write(obj.replace("box.mtl", "gray.mtl").replace("usemtl Plain", "usemtl Painted"))
    open(os.path.join(d, "gray.mtl"), "w").write("newmtl Painted\nKd 0.8 0.8 0.8\nNs 250\n")


def make_alpha_models(d):
    """(1.2.2) OBJ files whose only opacity is the color texture's alpha: a cut-out (see-through left quarter, as
    hair cards and leaves have), and an even 0.3 all over (gloss kept in the alpha, as some Unity assets do)."""
    S = 64
    write_png(os.path.join(d, "tex_cut.png"), S, S, lambda x, y: (200, 160, 90, 0 if x < S // 4 else 255))
    write_png(os.path.join(d, "tex_gloss.png"), S, S, lambda x, y: (90, 160, 200, 77))
    quad = "v -1 0 -1\nv 1 0 -1\nv 1 0 1\nv -1 0 1\nvt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\nusemtl M\nf 1/1 4/4 3/3 2/2\n"
    for name in ("cut", "gloss"):
        open(os.path.join(d, name + ".obj"), "w").write("mtllib %s.mtl\n" % name + quad)
        open(os.path.join(d, name + ".mtl"), "w").write("newmtl M\nKd 0.8 0.8 0.8\nmap_Kd tex_%s.png\n" % name)


def make_vertex_color_models(d):
    """Two separate quads, red and blue, per vertex: ASCII PLY (Z up, no UVs), binary PLY (float colors, MeshLab's
    per-face texcoord: red over u 0-0.45, blue over 0.55-1), OBJ "v x y z r g b" (UVs), and a glTF with COLOR_0 and a
    material whose color multiplies them."""
    red, blue = (230, 30, 30), (30, 30, 230)
    quads = [[(0, 0, 0), (1, 0, 0), (1, 0, 1), (0, 0, 1)], [(2, 0, 0), (3, 0, 0), (3, 0, 2), (2, 0, 2)]]  # Z up: 2 tall
    cols = [red, blue]
    lines = ["ply", "format ascii 1.0", "element vertex 8", "property float x", "property float y", "property float z",
             "property uchar red", "property uchar green", "property uchar blue", "element face 2",
             "property list uchar int vertex_indices", "end_header"]
    for q, c in zip(quads, cols):
        for v in q:
            lines.append("%g %g %g %d %d %d" % (v + c))
    lines += ["4 0 1 2 3", "4 4 5 6 7"]
    open(os.path.join(d, "vc.ply"), "w").write("\n".join(lines) + "\n")
    hdr = ("ply\nformat binary_little_endian 1.0\nelement vertex 8\nproperty float x\nproperty float y\nproperty float z\n"
           "property float red\nproperty float green\nproperty float blue\nelement face 2\n"
           "property list uchar int vertex_indices\nproperty list uchar float texcoord\nend_header\n").encode()
    b = bytearray(hdr)
    for q, c in zip(quads, cols):
        for v in q:
            b += struct.pack("<6f", *v, *(x / 255 for x in c))
    uvq = [[(0, 0), (0.45, 0), (0.45, 1), (0, 1)], [(0.55, 0), (1, 0), (1, 1), (0.55, 1)]]
    for i, uq in enumerate(uvq):
        b += struct.pack("<B4i", 4, *range(i * 4, i * 4 + 4))
        b += struct.pack("<B8f", 8, *[x for t in uq for x in t])
    open(os.path.join(d, "vc_bin.ply"), "wb").write(bytes(b))
    obj = []
    for q, c in zip(quads, cols):
        for v in q:
            obj.append("v %g %g %g %g %g %g" % (v[0], v[2], -v[1], c[0] / 255, c[1] / 255, c[2] / 255))
    for uq in uvq:
        for t in uq:
            obj.append("vt %g %g" % t)
    obj += ["f 1/1 2/2 3/3 4/4", "f 5/5 6/6 7/7 8/8"]
    open(os.path.join(d, "vc.obj"), "w").write("\n".join(obj) + "\n")
    # glTF: one quad over the whole UV square, COLOR_0 linear (1, 0.5, 0.25), material color (0.5, 1, 1)
    pos = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]
    uv = [(0, 1), (1, 1), (1, 0), (0, 0)]
    b = bytearray()
    for p in pos: b += struct.pack("<3f", *p)
    for t in uv: b += struct.pack("<2f", *t)
    for _ in pos: b += struct.pack("<4f", 1, 0.5, 0.25, 1)
    for i in (0, 1, 2, 0, 2, 3): b += struct.pack("<H", i)
    g = {"asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
         "buffers": [{"uri": "data:application/octet-stream;base64," + base64.b64encode(bytes(b)).decode(), "byteLength": len(b)}],
         "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 48}, {"buffer": 0, "byteOffset": 48, "byteLength": 32},
                         {"buffer": 0, "byteOffset": 80, "byteLength": 64}, {"buffer": 0, "byteOffset": 144, "byteLength": 12}],
         "accessors": [{"bufferView": 0, "componentType": 5126, "count": 4, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0]},
                       {"bufferView": 1, "componentType": 5126, "count": 4, "type": "VEC2"},
                       {"bufferView": 2, "componentType": 5126, "count": 4, "type": "VEC4"},
                       {"bufferView": 3, "componentType": 5123, "count": 6, "type": "SCALAR"}],
         "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "TEXCOORD_0": 1, "COLOR_0": 2}, "indices": 3, "material": 0}]}],
         "materials": [{"name": "Tint", "pbrMetallicRoughness": {"baseColorFactor": [0.5, 1, 1, 1], "metallicFactor": 0}}]}
    open(os.path.join(d, "vc.gltf"), "w").write(json.dumps(g))


def share(img, color, tol=0.12):
    """How much of the map is near this color (0-1)."""
    w, h, ch, rows = img
    n = hit = 0
    for y in range(0, h, 2):
        for x in range(0, w, 2):
            n += 1
            if all(abs(rows[y][x * ch + c] / 255 - color[c]) < tol for c in range(3)):
                hit += 1
    return hit / n


def near(a, b, tol=0.08):
    return all(abs(x - y) <= tol for x, y in zip(a, b))


def main():
    exe = os.path.abspath(os.environ.get("PATINA_BIN") or os.path.join(HERE, "..", "..", "build", "patina"))
    if not os.path.exists(exe):
        print("SKIP: patina binary not found (set PATINA_BIN)")
        return 0
    tmp = tempfile.mkdtemp(prefix="patina_model_tex_test_")
    env = dict(os.environ, HOME=os.path.join(tmp, "home"), XDG_DATA_HOME=os.path.join(tmp, "data"),
               XDG_CONFIG_HOME=os.path.join(tmp, "config"), XDG_CACHE_HOME=os.path.join(tmp, "cache"))
    os.makedirs(env["HOME"], exist_ok=True)
    m = os.path.join(tmp, "models")
    os.makedirs(m)
    make_textures(m)
    make_gltf(m)
    make_gltf(m, "lost.gltf", color_uri="not_here.png")
    write_png(os.path.join(m, "tex_big.png"), 4096, 4, lambda x, y: (200, 120, 40, 255))
    make_gltf(m, "big.gltf", big=True)
    make_obj(m)
    make_alpha_models(m)
    make_vertex_color_models(m)
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

        def tex(channel):
            r = c.request("get_texture", {"channel": channel, "size": 256})
            return read_png(base64.b64decode(r["image_png_base64"]))

        def layers():
            return [l["name"] for l in c.request("list_layers", {})]

        # --- glTF: opened with its textures
        c.request("open_file", {"path": os.path.join(m, "box.gltf")})
        r = c.request("get_state", {})["mesh"].get("model_textures") or {}
        check(r.get("slots") == 2 and r.get("textures") == 3, "box.gltf: 2 slots, 3 textures brought in (%s)" % r)
        check(set(r.get("channels", [])) >= {"basecolor", "roughness", "metallic", "normal", "emissive", "opacity"},
              "color, roughness, metallic, normal, glow and opacity channels (%s)" % r.get("channels"))
        check(layers()[0] == "From the model", "the layer 'From the model' at the bottom (%s)" % layers())
        col, rough, metal = tex("basecolor"), tex("roughness"), tex("metallic")
        A, Atop, Abot, B = (0.25, 0.5), (0.25, 0.75), (0.25, 0.25), (0.8, 0.25)
        check(near(texel(col, 0.3, 0.5)[:3], (220 / 255, 20 / 255, 20 / 255)), "slot A is red where the texture is red (%s)" % (texel(col, 0.3, 0.5),))
        check(near(texel(col, 0.53, 0.5)[:3], (20 / 255, 20 / 255, 220 / 255)), "and blue beyond the middle of the texture")
        green = (srgb(0.05), srgb(0.6), srgb(0.1))
        check(near(texel(col, *B)[:3], green), "slot B is its flat green (%s vs %s)" % (texel(col, *B), green))
        check(abs(texel(rough, *Atop)[0] - 0.2) < 0.06 and abs(texel(rough, *Abot)[0] - 0.9) < 0.06,
              "roughness from the green of the packed map, the right way up (%s, %s)" % (texel(rough, *Atop), texel(rough, *Abot)))
        check(abs(texel(rough, *B)[0] - 0.3) < 0.06, "slot B's roughness 0.3")
        check(texel(metal, *A)[0] < 0.06 and texel(metal, *B)[0] > 0.94, "metallic: A none (blue of the map), B fully")
        op = tex("opacity")
        check(texel(op, 0.05, 0.95)[0] < 0.1 and texel(op, 0.3, 0.5)[0] > 0.9,
              "cut-out opacity from the color map's alpha (%s, %s)" % (texel(op, 0.05, 0.95), texel(op, 0.3, 0.5)))
        em = tex("emissive")
        check(texel(em, *B)[1] > 0.5 and texel(em, *A)[1] < 0.05, "slot B glows green, A does not (%s)" % (texel(em, *B),))
        nm = tex("normal")
        got = texel(nm, *A)[:3]
        check(near(got, tuple(v / 255 for v in NORMAL_PX), 0.05), "the normal map comes through (%s vs %s)" % (got, NORMAL_PX))

        # --- one undo step; the command again
        c.request("undo", {})
        check("From the model" not in layers(), "undo takes the layer away")
        c.request("redo", {})
        check("From the model" in layers(), "redo brings it back")
        r2 = c.request("import_model_textures", {})
        check(r2.get("textures") == 3 and layers().count("From the model") == 2, "import_model_textures adds it again")

        # --- a texture not found: the slot keeps its color, the report names it
        c.request("open_file", {"path": os.path.join(m, "lost.gltf")})
        r = c.request("get_state", {})["mesh"].get("model_textures") or {}
        check(r.get("missing") == ["not_here.png"], "a missing texture is named (%s)" % r.get("missing"))
        check(r.get("textures") == 2 and "From the model" in layers(), "the rest still comes in")

        # --- OBJ: map_Kd in a textures folder, Pr, Pm, a gray map_d (no alpha: its gray is the opacity)
        c.request("open_file", {"path": os.path.join(m, "box.obj")})
        r = c.request("get_state", {})["mesh"].get("model_textures") or {}
        check(r.get("slots") == 2 and r.get("textures") == 2, "box.obj: 2 slots, 2 textures (%s)" % r)
        col, rough, metal = tex("basecolor"), tex("roughness"), tex("metallic")
        check(near(texel(col, 0.3, 0.5)[:3], (220 / 255, 20 / 255, 20 / 255)), "OBJ: map_Kd found in textures/, Kd ignored")
        check(near(texel(col, *B)[:3], (srgb(0.9), srgb(0.6), srgb(0.1))), "OBJ: the plain slot's Kd (%s)" % (texel(col, *B),))
        check(abs(texel(rough, *A)[0] - 0.4) < 0.06 and abs(texel(rough, *B)[0] - 0.7) < 0.06, "OBJ: Pr per slot")
        check(texel(metal, *B)[0] > 0.94 and texel(metal, *A)[0] < 0.06, "OBJ: Pm per slot")
        op = tex("opacity")
        check(texel(op, 0.05, 0.5)[0] < 0.1 and texel(op, 0.4, 0.5)[0] > 0.9, "OBJ: map_d, a gray mask")

        # --- (1.2.2) OBJ / FBX: the color texture's alpha is the opacity when it cuts out, and nothing else says so
        c.request("open_file", {"path": os.path.join(m, "cut.obj")})
        r = c.request("get_state", {})["mesh"].get("model_textures") or {}
        check("opacity" in r.get("channels", []) and r.get("opacity_from_color_alpha") is True,
              "cut.obj: the color texture's alpha brings an opacity channel (%s)" % r)
        op = tex("opacity")
        check(texel(op, 0.1, 0.5)[0] < 0.1 and texel(op, 0.6, 0.5)[0] > 0.9,
              "see-through where the alpha is 0, solid where it is 1 (%s, %s)" % (texel(op, 0.1, 0.5), texel(op, 0.6, 0.5)))
        r2 = c.request("import_model_textures", {"color_alpha": "ignore"})
        check("opacity" not in r2.get("channels", []) and r2.get("opacity_from_color_alpha") is False,
              "color_alpha ignore leaves it out (%s)" % r2.get("channels"))
        c.request("open_file", {"path": os.path.join(m, "gloss.obj")})
        r = c.request("get_state", {})["mesh"].get("model_textures") or {}
        check("opacity" not in r.get("channels", []) and r.get("opacity_from_color_alpha") is False,
              "gloss.obj: an even alpha with nothing see-through is not opacity (%s)" % r.get("channels"))
        r2 = c.request("import_model_textures", {"color_alpha": "opacity"})
        op = tex("opacity")
        check("opacity" in r2.get("channels", []) and abs(texel(op, 0.5, 0.5)[0] - 0.3) < 0.04,
              "color_alpha opacity takes it anyway (%s, %s)" % (r2.get("channels"), texel(op, 0.5, 0.5)))

        # --- plain gray: nothing; the feature off: nothing
        c.request("open_file", {"path": os.path.join(m, "gray.obj")})
        check("model_textures" not in c.request("get_state", {})["mesh"] and "From the model" not in layers(),
              "a plain gray model brings nothing in")
        c.request("features", {"id": "model_textures", "on": False})
        c.request("open_file", {"path": os.path.join(m, "box.gltf")})
        check("From the model" not in layers(), "switched off: opening brings nothing in")
        e = ""
        try:
            c.request("import_model_textures", {})
        except protocol.RemoteError as ex:
            e = str(ex)
        check("switched off" in e, "and the command says so")
        c.request("features", {"id": "model_textures", "on": True})

        # --- Patina's own export opens again looking the same (a painted Suzanne: color, roughness, normal)
        samples = os.path.join(HERE, "..", "..", "samples", "Suzanne.glb")
        c.request("open_file", {"path": samples})
        c.request("apply_smart_material", {"id": "patina_smart_worn_painted_metal"})
        first = {ch: tex(ch) for ch in ("basecolor", "roughness", "normal")}
        glb = os.path.join(m, "painted.glb")
        c.request("export_model", {"path": glb, "texture_size": 2048})
        c.request("open_file", {"path": glb})
        r = c.request("get_state", {})["mesh"].get("model_textures") or {}
        check("emissive" not in r.get("channels", []), "a black glow map brings no glow channel (%s)" % r.get("channels"))
        for ch, img in first.items():
            again = tex(ch)
            w, h, n, rows = img
            _, _, n2, rows2 = again
            diff = sum(abs(rows[y][x * n] - rows2[y][x * n2]) for y in range(0, h, 4) for x in range(0, w, 4)) / ((h // 4) * (w // 4) * 255)
            check(diff < 0.03, "round trip through a .glb: the %s map is the same (mean difference %.3f)" % (ch, diff))

        # --- vertex colors (0.32): PLY (ASCII and binary), OBJ, glTF COLOR_0
        RED, BLUE = (230 / 255, 30 / 255, 30 / 255), (30 / 255, 30 / 255, 230 / 255)
        c.request("open_file", {"path": os.path.join(m, "vc.ply")})
        st = c.request("get_state", {})["mesh"]
        r = st.get("model_textures") or {}
        check(r.get("vertex_colors") is True and r.get("vertex_colours") is True and layers()[0] == "From the model", "vc.ply: its vertex colors come in (%s)" % r)  # either spelling
        check(st["faces"] == 2 and abs(st["bounds_max"][1] - 2.0) < 1e-4 and abs(st["bounds_max"][2]) < 1e-4,
              "PLY is Z up, as Blender reads it (%s %s)" % (st["bounds_min"], st["bounds_max"]))
        col = tex("basecolor")
        check(share(col, RED) > 0.05 and share(col, BLUE) > 0.05, "a red and a blue part (%.2f %.2f)" % (share(col, RED), share(col, BLUE)))
        c.request("open_file", {"path": os.path.join(m, "vc_bin.ply")})
        col = tex("basecolor")
        check(near(texel(col, 0.2, 0.5)[:3], RED) and near(texel(col, 0.8, 0.5)[:3], BLUE),
              "binary PLY, float colors, per-face texcoords: red left, blue right (%s %s)" % (texel(col, 0.2, 0.5), texel(col, 0.8, 0.5)))
        c.request("open_file", {"path": os.path.join(m, "vc.obj")})
        col = tex("basecolor")
        check(near(texel(col, 0.2, 0.5)[:3], RED) and near(texel(col, 0.8, 0.5)[:3], BLUE), "OBJ vertex colors (sRGB)")
        c.request("open_file", {"path": os.path.join(m, "vc.gltf")})
        col = tex("basecolor")
        want = (srgb(0.5), srgb(0.5), srgb(0.25))
        check(near(texel(col, 0.5, 0.5)[:3], want, 0.04), "glTF COLOR_0 times the material color (%s vs %s)" % (texel(col, 0.5, 0.5), want))

        # --- PLY export: the base color as vertex colors, and back in again
        c.request("open_file", {"path": os.path.join(m, "vc_bin.ply")})
        out_ply = os.path.join(m, "out", "again.ply")
        r = c.request("export_model", {"path": out_ply})
        check(r.get("format") == "ply" and r.get("faces") == 2 and os.path.isfile(out_ply), "export_model writes a .ply (%s)" % r)
        c.request("open_file", {"path": out_ply})
        st = c.request("get_state", {})["mesh"]
        col = tex("basecolor")
        check((st.get("model_textures") or {}).get("vertex_colors") and st["faces"] == 2 and abs(st["bounds_max"][1] - 2.0) < 1e-4,
              "it opens again with its colors, the same way up")
        check(near(texel(col, 0.2, 0.5)[:3], RED) and near(texel(col, 0.8, 0.5)[:3], BLUE),
              "the colors in their places (%s %s)" % (texel(col, 0.2, 0.5), texel(col, 0.8, 0.5)))

        # --- a 4K texture grows the texture set (once, on open)
        c.request("open_file", {"path": os.path.join(m, "big.gltf")})
        r = c.request("get_state", {})["mesh"].get("model_textures") or {}
        check(r.get("resolution") == 4096 and r.get("resolution_before") == 2048, "a 4K texture: the texture set grows to 4K (%s)" % r)
        c.request("open_mesh", {"path": os.path.join(m, "big.gltf"), "resolution": 1024})
        r = c.request("get_state", {})["mesh"].get("model_textures") or {}
        check(r.get("resolution") == 1024, "unless a resolution was asked for (%s)" % r.get("resolution"))
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
        shutil.rmtree(tmp, ignore_errors=True)
    print("model textures: %s" % ("all checks passed" if not failures else "%d FAILED" % len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
