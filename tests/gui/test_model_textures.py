#!/usr/bin/env python3
"""A model's own textures (0.31) in the real window: a model painted in Patina and exported as a .glb opens again in
a fresh project with its look, as the layer "From the model" at the bottom of the stack; a .gltf whose texture is
missing says so in a message.

    python3 tests/gui/test_model_textures.py [path/to/patina] [--shot OUT_DIR]
"""
import base64
import json
import os
import shutil
import struct
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import manual_shots  # noqa: E402

BIN = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith("--") else os.path.join(ROOT, "build", "patina")
SHOT = sys.argv[sys.argv.index("--shot") + 1] if "--shot" in sys.argv else None
failures = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        failures.append(what)


def main():
    tmp = tempfile.mkdtemp(prefix="patina_model_tex_gui_")
    s = manual_shots.Session(BIN, SHOT or os.path.join(tmp, "shots"), display=":96", port=7896)
    try:
        s("open_file", path=os.path.join(ROOT, "samples", "Suzanne.glb"))
        s("apply_smart_material", id="patina_smart_worn_painted_metal")
        glb = os.path.join(tmp, "painted_suzanne.glb")
        r = s("export_model", path=glb, texture_size=1024)
        check(os.path.isfile(glb), "the painted model is exported (%s)" % r.get("format"))
        # a fresh project from the exported file
        s("open_file", path=glb)
        st = s("get_state")["mesh"]
        rep = st.get("model_textures") or {}
        check(rep.get("textures", 0) >= 3, "opening it brings its textures in (%s)" % {k: rep.get(k) for k in ("textures", "channels")})
        names = [l["name"] for l in s("list_layers")]
        check(names and names[0] == "From the model", "as the layer From the model at the bottom (%s)" % names)
        s("set_camera", view="front", frame=True)
        s("select_layer", index=0)
        time.sleep(1.0)
        if SHOT:
            s.shot("model_textures")
        # a texture that is not there: a message names it
        lost = os.path.join(tmp, "lost.gltf")
        buf = struct.pack("<9f", 0, 0, 0, 1, 0, 0, 0, 1, 0) + struct.pack("<6f", 0, 1, 1, 1, 0, 0) + struct.pack("<3H", 0, 1, 2)
        gl = {"asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
              "buffers": [{"uri": "data:application/octet-stream;base64," + base64.b64encode(buf).decode(),
                           "byteLength": len(buf)}],
              "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36}, {"buffer": 0, "byteOffset": 36, "byteLength": 24},
                              {"buffer": 0, "byteOffset": 60, "byteLength": 6}],
              "accessors": [{"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0]},
                            {"bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC2"},
                            {"bufferView": 2, "componentType": 5123, "count": 3, "type": "SCALAR"}],
              "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "TEXCOORD_0": 1}, "indices": 2, "material": 0}]}],
              "images": [{"uri": "gone_away.png"}], "textures": [{"source": 0}],
              "materials": [{"name": "Lost", "pbrMetallicRoughness": {"baseColorTexture": {"index": 0},
                                                                      "baseColorFactor": [0.9, 0.3, 0.1, 1]}}]}
        json.dump(gl, open(lost, "w"))
        s("open_file", path=lost)
        rep = s("get_state")["mesh"].get("model_textures") or {}
        check(rep.get("missing") == ["gone_away.png"], "a missing texture is named (%s)" % rep.get("missing"))
        time.sleep(1.0)
        pop = s("ui_rect", name="error.ok")
        check(bool(pop.get("found")), "and a message says so")
    finally:
        s.close()
        shutil.rmtree(tmp, ignore_errors=True)
    print("model textures: " + ("all checks passed" if not failures else "%d FAILED" % len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
