#!/usr/bin/env python3
"""Curvature export (1.2.20) against a real headless Patina (`patina run`): once bake_mesh_maps has baked curvature,
export_textures writes $mesh_curvature.png (8-bit grayscale, data not sRGB, 0.5 = flat) beside the AO unless
include_curvature is false; its dry_run plan lists it; export_model writes <name>_curvature.png beside an .obj
(next to <name>_ao.png), a .gltf and a .glb (glTF has no curvature slot); nothing is written before curvature is baked.

    PATINA_BIN=build/patina python3 tests/api/test_curvature_export.py
"""
import json
import os
import struct
import subprocess
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
SAMPLE = os.path.join(HERE, "..", "..", "samples", "Suzanne.glb")
PATINA = os.environ.get("PATINA_BIN", os.path.join(HERE, "..", "..", "build", "patina"))

failures = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        failures.append(what)


def png_info(path):
    """(width, height, color type, bit depth, pixel bytes) of a non-interlaced 8-bit PNG."""
    data = open(path, "rb").read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    i, idat, ihdr = 8, b"", None
    while i < len(data):
        n = struct.unpack(">I", data[i:i + 4])[0]
        t = data[i + 4:i + 8]
        if t == b"IHDR":
            ihdr = struct.unpack(">IIBBBBB", data[i + 8:i + 8 + 13])
        elif t == b"IDAT":
            idat += data[i + 8:i + 8 + n]
        i += 12 + n
    w, h, bits, ctype = ihdr[0], ihdr[1], ihdr[2], ihdr[3]
    ch = {0: 1, 2: 3, 4: 2, 6: 4}[ctype]
    raw = zlib.decompress(idat)
    stride = w * ch
    out, prev = bytearray(), bytearray(stride)
    p = 0
    for _ in range(h):
        f = raw[p]
        line = bytearray(raw[p + 1:p + 1 + stride])
        p += 1 + stride
        for x in range(stride):
            a = line[x - ch] if x >= ch else 0
            b = prev[x]
            c = prev[x - ch] if x >= ch else 0
            if f == 1:
                line[x] = (line[x] + a) & 255
            elif f == 2:
                line[x] = (line[x] + b) & 255
            elif f == 3:
                line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        out += line
        prev = line
    return w, h, ctype, bits, ch, bytes(out)


def run(job, tmp, tag):
    path = os.path.join(tmp, tag + ".json")
    report = os.path.join(tmp, tag + "_report.json")
    json.dump(job, open(path, "w"), indent=1)
    p = subprocess.run([PATINA, "run", path, "--no-server", "--report", report, "--resolution", "256"],
                       capture_output=True, text=True, timeout=600)
    try:
        r = json.load(open(report))
    except (OSError, ValueError):
        r = {}
    if p.returncode != 0 or not r.get("ok"):
        print(p.stdout[-3000:], p.stderr[-3000:])
    return p.returncode, r


def step(r, i):
    s = r.get("steps", [])
    return s[i] if i < len(s) else {}


def main():
    tmp = tempfile.mkdtemp(prefix="patina_curv_")
    print("Curvature export in", tmp)
    tex = os.path.join(tmp, "tex")
    tex_off = os.path.join(tmp, "tex_off")
    pre = os.path.join(tmp, "pre")
    job = {"name": "curvature export", "steps": [
        {"cmd": "open_mesh", "args": {"path": SAMPLE}},
        {"cmd": "export_textures", "args": {"directory": pre, "base_name": "m"}},               # 1: nothing baked yet
        {"cmd": "bake_mesh_maps", "args": {"ao": True, "ao_samples": 32, "curvature": True}},
        {"cmd": "export_textures", "args": {"directory": tex, "base_name": "m", "dry_run": True}},  # 3
        {"cmd": "export_textures", "args": {"directory": tex, "base_name": "m"}},               # 4
        {"cmd": "export_textures", "args": {"directory": tex_off, "base_name": "m", "include_curvature": False}},
        {"cmd": "export_textures", "args": {"directory": os.path.join(tmp, "unreal"), "base_name": "m",
                                            "preset": "unreal"}},                                # 6
        {"cmd": "export_model", "args": {"path": os.path.join(tmp, "obj", "Model.obj")}},       # 7
        {"cmd": "export_model", "args": {"path": os.path.join(tmp, "gltf", "Model.gltf")}},
        {"cmd": "export_model", "args": {"path": os.path.join(tmp, "glb", "Model.glb")}},        # 9
    ]}
    code, r = run(job, tmp, "job")
    check(code == 0 and r.get("ok"), "the job ran (exit %s)" % code)

    check(not os.path.exists(os.path.join(pre, "m_curvature.png")), "no curvature file before curvature is baked")

    plan = (step(r, 3).get("result") or {}).get("plan", [])
    cp = [f for f in plan if f.get("map") == "curvature"]
    check(len(cp) == 1, "dry_run plan lists the curvature map")
    if cp:
        check(cp[0]["srgb"] is False and cp[0]["channels"] == 1 and cp[0]["bits"] == 8,
              "plan: curvature is 1-channel 8-bit data (srgb false)")
        check(cp[0]["file"].endswith("m_curvature.png"), "plan: file m_curvature.png")
        check("0.5 flat" in cp[0]["contents"], "plan: contents say 0.5 is flat")

    cf = os.path.join(tex, "m_curvature.png")
    check(os.path.isfile(cf), "export_textures wrote m_curvature.png")
    check(os.path.isfile(os.path.join(tex, "m_ao.png")), "export_textures wrote m_ao.png beside it")
    files = (step(r, 4).get("result") or {}).get("files", {})
    check("curvature" in files, "export_textures files list curvature")
    if os.path.isfile(cf):
        w, h, ctype, bits, ch, px = png_info(cf)
        check(ctype == 0 and bits == 8, "curvature PNG is 8-bit grayscale (type %d, %d bits)" % (ctype, bits))
        vals = sorted(px)
        mid = vals[len(vals) // 2]
        check(100 <= mid <= 170, "curvature median near 0.5 = flat (%d; Suzanne is mostly convex)" % mid)
        check(vals[-1] - vals[0] > 20, "curvature varies over the model (%d..%d)" % (vals[0], vals[-1]))

    check(not os.path.exists(os.path.join(tex_off, "m_curvature.png")), "include_curvature false: no curvature file")
    check(os.path.isfile(os.path.join(tex_off, "m_ao.png")), "include_curvature false still writes the AO")
    check(os.path.isfile(os.path.join(tmp, "unreal", "m_Curvature.png")), "unreal preset: m_Curvature.png")

    oc = os.path.join(tmp, "obj", "Model_curvature.png")
    check(os.path.isfile(oc) and os.path.getsize(oc) > 0, "export_model .obj: Model_curvature.png beside the OBJ")
    check(os.path.isfile(os.path.join(tmp, "obj", "Model_ao.png")), "export_model .obj: Model_ao.png beside it")
    mtl = open(os.path.join(tmp, "obj", "Model.mtl")).read() if os.path.isfile(os.path.join(tmp, "obj", "Model.mtl")) else ""
    check("curvature" not in mtl, "the .mtl does not reference curvature (no MTL slot)")
    check(os.path.isfile(os.path.join(tmp, "gltf", "Model_curvature.png")), "export_model .gltf: Model_curvature.png")
    gc = os.path.join(tmp, "glb", "Model_curvature.png")
    check(os.path.isfile(gc) and os.path.getsize(gc) > 0, "export_model .glb: Model_curvature.png beside the GLB")
    gres = step(r, 9).get("result") or {}
    check(any(os.path.abspath(v) == os.path.abspath(gc) for v in (gres.get("textures") or {}).values()),
          "export_model .glb result lists the curvature file")
    check(sorted(os.listdir(os.path.join(tmp, "glb"))) == ["Model.glb", "Model_curvature.png"],
          "the .glb folder has only the model and its curvature (%s)" % os.listdir(os.path.join(tmp, "glb")))

    if failures:
        print("%d failure(s)" % len(failures))
        sys.exit(1)
    print("all ok")


if __name__ == "__main__":
    main()
