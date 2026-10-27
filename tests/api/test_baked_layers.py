#!/usr/bin/env python3
"""Baked map layers (0.41) against a real headless Patina: baking gives the ambient occlusion, curvature and thickness a
layer each at the bottom of the stack (one per map, also after another bake). On the AO layer the brush paints fixes
over the bake (undoable), the eraser brings the bake back, blur / smudge / clone work; mask effects (levels) adjust it,
strength scales it, hidden shows the bake as it is, a delete keeps the bake. Smart masks, exports (the AO map, the
layer alone) and projects (saved and opened again) carry the fixes; a project from before 0.41 gets the layers when
opened; with the feature off the layers change nothing.

    PATINA_BIN=build/patina python3 tests/api/test_baked_layers.py
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
sys.path.insert(0, HERE)
import protocol  # noqa: E402
from test_model_textures import read_png  # noqa: E402

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


def png_mean(path):
    w, h, n, rows = read_png(open(path, "rb").read())
    return sum(rows[y][x * n] for y in range(h) for x in range(w)) / (255.0 * w * h)


def strip_baked_layers_key(src, dst):
    """A project as 0.40 wrote it: the same file without the header's baked_layers key and without baked map layers."""
    data = open(src, "rb").read()
    # container: "PATINA01", header length (u32 LE), header JSON, then the blocks
    magic, n = data[:8], struct.unpack("<I", data[8:12])[0]
    header = json.loads(data[12:12 + n])
    header.pop("baked_layers", None)
    header["layers"] = [l for l in header["layers"] if l.get("type") != "baked"]
    hb = json.dumps(header).encode()
    open(dst, "wb").write(magic + struct.pack("<I", len(hb)) + hb + data[12 + n:])


def main():
    exe = os.path.abspath(os.environ.get("PATINA_BIN") or os.path.join(HERE, "..", "..", "build", "patina"))
    if not os.path.exists(exe):
        print("SKIP: patina binary not found (set PATINA_BIN)")
        return 0
    tmp = tempfile.mkdtemp(prefix="patina_baked_layers_test_")
    env = dict(os.environ, HOME=os.path.join(tmp, "home"), XDG_DATA_HOME=os.path.join(tmp, "data"),
               XDG_CONFIG_HOME=os.path.join(tmp, "config"), XDG_CACHE_HOME=os.path.join(tmp, "cache"), PATINA_EXE=exe)
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
        R = c.request
        R("new_project", {"primitive": "sphere", "resolution": 512})
        R("bake_mesh_maps", {"ao": True, "curvature": True, "thickness": True, "ao_samples": 16, "thickness_rays": 8})
        layers = R("list_layers")
        baked = [l for l in layers if l["type"] == "baked"]
        check(sorted(l["baked_map"] for l in baked) == ["ao", "curvature", "thickness"] and
              sorted(l["index"] for l in baked) == [0, 1, 2], "a layer per baked map, at the bottom (%s)" %
              [(l["index"], l.get("baked_map")) for l in layers])

        def maps():
            return {m["map"]: m for m in R("baked_maps")["maps"]}

        m = maps()
        ao0 = m["ao"]["mean_baked"]
        check(abs(m["ao"]["mean"] - ao0) < 1e-4 and not m["ao"]["edited"], "untouched: the map is the bake (%.4f)" % ao0)
        aoi = m["ao"]["layer"]["index"]
        # ---- fixes: paint the AO darker on the front, undo, redo, erase back to the bake
        R("baked_maps", {"map": "ao", "select": True})
        R("set_tool", {"tool": "paint"})
        R("set_brush", {"size": 260, "hardness": 1, "opacity": 1, "flow": 1, "spacing": 0.05, "mask_value": 0})
        steps0 = R("history")["position"]
        stroke = [[0.42, 0.5], [0.58, 0.5]]
        R("paint_stroke", {"points": stroke})
        m = maps()
        dark = m["ao"]["mean"]
        check(dark < ao0 - 0.02 and m["ao"]["edited"], "painting black over the AO darkens it (%.4f -> %.4f)" % (ao0, dark))
        check(abs(m["curvature"]["mean"] - m["curvature"]["mean_baked"]) < 1e-4, "and leaves the curvature alone")
        check(not any(l["painted_channels"] for l in R("list_layers")), "no material channel gets paint")
        check(R("history")["position"] == steps0 + 1, "one undo step")
        R("undo")
        check(abs(maps()["ao"]["mean"] - ao0) < 1e-3, "undo takes the fix away")
        R("redo")
        check(abs(maps()["ao"]["mean"] - dark) < 1e-3, "redo puts it back")
        # (1.2.13: the Erase tool keeps a brush of its own, Basic Hard the first time: give it the paint stroke's tip)
        R("paint_stroke", {"points": stroke, "tool": "erase",
                           "brush": {"size": 260, "hardness": 1, "opacity": 1, "flow": 1, "spacing": 0.05}})
        check(abs(maps()["ao"]["mean"] - ao0) < 2e-3, "the eraser brings the bake back (%.4f)" % maps()["ao"]["mean"])
        # ---- blur, smudge and clone paint the bake with the fixes
        R("paint_stroke", {"points": [[0.5, 0.5], [0.5, 0.501]], "brush": {"size": 120, "mask_value": 0}})
        before = maps()["ao"]["mean"]
        R("set_tool", {"tool": "blur", "blur_radius": 24})
        R("paint_stroke", {"points": [[0.45, 0.5], [0.55, 0.5]], "brush": {"size": 200}})
        R("paint_stroke", {"points": [[0.45, 0.45], [0.55, 0.55]], "brush": {"size": 200}})
        after = maps()["ao"]["mean"]
        check(abs(after - before) < 0.02 and after != before, "blur spreads the dark spot (mean %.4f -> %.4f)" % (before, after))
        R("paint_stroke", {"points": [[0.5, 0.5], [0.56, 0.52]], "tool": "smudge"})
        R("set_clone_source", {"uv": [0.25, 0.5]})
        R("paint_stroke", {"points": [[0.4, 0.4], [0.42, 0.42]], "tool": "clone"})
        check(maps()["ao"]["edited"], "smudge and clone run on it")
        R("baked_maps", {"map": "ao", "reset_fixes": True})
        m = maps()
        check(abs(m["ao"]["mean"] - ao0) < 1e-4 and not m["ao"]["layer"]["fixes"], "reset_fixes: the bake again")
        R("undo")
        check(maps()["ao"]["layer"]["fixes"], "and undo brings the fixes back")
        R("baked_maps", {"map": "ao", "reset_fixes": True})
        # ---- adjustments, strength, the eye
        # (a sphere's AO is white nearly everywhere: half gray painted on the front gives the adjustments something)
        R("set_tool", {"tool": "paint"})
        R("paint_stroke", {"points": stroke, "brush": {"size": 260, "mask_value": 0.5}})
        dark = maps()["ao"]["mean"]
        area = (ao0 - dark) / 0.5  # (the share of the texture painted)
        R("update_layer", {"index": aoi, "mask_effects": [{"type": "levels", "out_low": 0.6, "out_high": 1}]})
        lv = maps()["ao"]["mean"]
        check(abs(lv - (dark + area * 0.3)) < area * 0.05 + 1e-3,
              "Levels (out 0.6..1) lifts the half gray to 0.8 (%.4f -> %.4f)" % (dark, lv))
        R("baked_maps", {"map": "ao", "reset_adjustments": True})
        check(abs(maps()["ao"]["mean"] - dark) < 1e-3, "reset_adjustments takes it off")
        R("baked_maps", {"map": "ao", "strength": 0})
        check(maps()["ao"]["mean"] > 0.999, "strength 0: no occlusion at all (%.4f)" % maps()["ao"]["mean"])
        R("baked_maps", {"map": "ao", "strength": 2})
        s2 = maps()["ao"]["mean"]
        check(abs(s2 - (dark - area * 0.5)) < area * 0.05 + 1e-3, "strength 2: the half gray goes black (%.4f -> %.4f)" % (dark, s2))
        R("update_layer", {"index": aoi, "strength": 1})
        R("update_layer", {"index": aoi, "visible": False})
        m = maps()
        check(abs(m["ao"]["mean"] - ao0) < 1e-4 and not m["ao"]["edited"], "hidden: the map as it was baked")
        R("update_layer", {"index": aoi, "visible": True})
        R("update_layer", {"index": aoi, "opacity": 0.5})
        half = maps()["ao"]["mean"]
        check(abs(half - (ao0 + dark) / 2) < 3e-3, "opacity 0.5: halfway between the bake and the fixes (%.4f)" % half)
        R("update_layer", {"index": aoi, "opacity": 1})
        # ---- the rest of Patina reads the fixed map: a smart mask, the 3D view's AO mode, exports
        R("add_layer", {"type": "fill", "material": {"color": [1, 0, 0]},
                        "generator": {"type": "occlusion", "level": 0.5, "contrast": 0.6}})
        red = R("pick", {"point": [0.5, 0.5]})["color"]
        R("update_layer", {"index": aoi, "visible": False})
        plain = R("pick", {"point": [0.5, 0.5]})["color"]
        R("update_layer", {"index": aoi, "visible": True})
        check(red[0] - red[1] > 0.3 and abs(plain[0] - plain[1]) < 0.05,
              "an Occlusion smart mask follows the fixes (red %s, without them %s)" % (red, plain))
        px = R("render_view", {"width": 64, "height": 64, "mode": "ao"})
        w, h, n, rows = read_png(base64.b64decode(px["image_png_base64"]))
        check(rows[32][32 * n] < 200, "the 3D view's AO mode shows the fix (%d)" % rows[32][32 * n])
        out = os.path.join(tmp, "export")
        files = R("export_textures", {"directory": out, "preset": "blender", "include_ao": True})["files"]
        ao_png = files.get("ao") if isinstance(files, dict) else None
        check(ao_png and abs(png_mean(ao_png) - maps()["ao"]["mean"]) < 0.01, "the exported AO map has the fixes")
        lf = R("export_textures", {"directory": os.path.join(tmp, "one"), "layer": aoi})["files"]
        one = lf.get("ao") if isinstance(lf, dict) else None
        check(one and one.endswith("_ao.png") and abs(png_mean(one) - maps()["ao"]["mean"]) < 0.01,
              "exporting the layer alone writes its map (%s)" % one)
        # ---- one layer per map: another bake keeps the layers and their fixes
        R("bake_mesh_maps", {"ao": True, "curvature": False, "ao_samples": 16})
        layers = R("list_layers")
        check(sum(1 for l in layers if l["type"] == "baked") == 3, "baking again adds no second layer")
        check(maps()["ao"]["layer"]["fixes"] and maps()["ao"]["edited"], "and keeps the fixes")
        try:
            R("add_instance", {"index": aoi})
            check(False, "a baked map layer has no instances")
        except Exception:
            check(True, "a baked map layer has no instances")
        # ---- projects keep them
        proj = os.path.join(tmp, "p.patina")
        mean_before = maps()["ao"]["mean"]
        R("save_project", {"path": proj})
        R("new_project", {"primitive": "cube", "resolution": 256})
        R("open_file", {"path": proj})
        m = maps()
        check(m["ao"]["layer"] is not None and abs(m["ao"]["mean"] - mean_before) < 2e-3,
              "saved and opened: the fixes are there (%.4f, %.4f)" % (mean_before, m["ao"]["mean"]))
        old = os.path.join(tmp, "old.patina")
        strip_baked_layers_key(proj, old)
        R("open_file", {"path": old})
        m = maps()
        check(all(m[k]["layer"] is not None for k in ("ao", "curvature", "thickness")) and not m["ao"]["edited"],
              "a project from before 0.41 gets a layer per baked map when opened")
        check(R("history")["position"] == 0, "without an undo step")
        # ---- a delete keeps the bake; add gives the map a layer again
        aoi = m["ao"]["layer"]["index"]
        R("remove_layer", {"index": aoi})
        m = maps()
        check(m["ao"]["layer"] is None and m["ao"]["baked"], "deleting the layer keeps the bake")
        R("baked_maps", {"map": "ao", "add": True})
        check(maps()["ao"]["layer"] is not None, "baked_maps add gives it a layer again")
        # ---- the feature switch
        R("baked_maps", {"map": "ao", "strength": 0})
        R("features", {"id": "baked_layers", "on": False})
        check(abs(maps()["ao"]["mean"] - maps()["ao"]["mean_baked"]) < 1e-4, "switched off: the layers change nothing")
        R("features", {"id": "baked_layers", "on": True})
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
        shutil.rmtree(tmp, ignore_errors=True)
    print("baked layers: %s" % ("all checks passed" if not failures else "%d FAILED" % len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
