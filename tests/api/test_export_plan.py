#!/usr/bin/env python3
"""The export preview (0.47), against a real headless Patina: export_textures with dry_run lists exactly the files
the export then writes, for every preset, for layered PSD files and for a model with two texture sets; each with what
its channels hold, its bits and its color space (color maps sRGB, data maps not). Nothing is written by a dry run.

    PATINA_BIN=build/patina python3 tests/api/test_export_plan.py
"""
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

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


def two_slot_cube(path):
    """a cube whose top and bottom are material 'Lid' and the sides 'Side', each with UVs over the whole square"""
    v = [(x, y, z) for x in (-1, 1) for y in (-1, 1) for z in (-1, 1)]
    faces = {"Side": [(0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6)], "Lid": [(0, 2, 6, 4), (1, 5, 7, 3)]}
    open(os.path.splitext(path)[0] + ".mtl", "w").write("newmtl Side\nKd 0.8 0.8 0.8\nnewmtl Lid\nKd 0.8 0.8 0.8\n")
    lines = ["mtllib " + os.path.basename(os.path.splitext(path)[0]) + ".mtl"]
    lines += ["v %d %d %d" % p for p in v] + ["vt 0 0", "vt 1 0", "vt 1 1", "vt 0 1"]
    for mat, fs in faces.items():
        lines.append("usemtl " + mat)
        lines += ["f " + " ".join("%d/%d" % (i + 1, k + 1) for k, i in enumerate(f)) for f in fs]
    open(path, "w").write("\n".join(lines) + "\n")


def main():
    exe = os.environ.get("PATINA_BIN") or os.path.join(HERE, "..", "..", "build", "patina")
    tmp = tempfile.mkdtemp(prefix="patina_export_plan_test_")
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
        c.request("new_project", {"primitive": "torus", "resolution": 256})
        c.request("bake_mesh_maps", {"ao": True, "curvature": True, "thickness": False})

        def same(tag, **args):
            out = os.path.join(tmp, tag)
            plan = c.request("export_textures", dict(args, directory=out, dry_run=True))["plan"]
            wrote = os.path.isdir(out) and os.listdir(out)
            check(not wrote, "%s: a dry run writes nothing" % tag)
            files = c.request("export_textures", dict(args, directory=out))["files"]
            planned = sorted(os.path.basename(p["file"]) for p in plan)
            written = sorted(os.listdir(out))
            check(planned == written, "%s: the plan names the files written (%s)" % (tag, planned if planned != written else len(planned)))
            check(len(files) == len(plan), "%s: as many as the export answers (%d, %d)" % (tag, len(files), len(plan)))
            return {p["map"]: p for p in plan}

        p = same("blender", preset="blender")
        check(p["basecolor"]["srgb"] and not p["roughness"]["srgb"] and not p["normal"]["srgb"],
              "Blender: the base color is sRGB, roughness and normal are data")
        check("OpenGL" in p["normal"]["contents"] and p["height"]["bits"] == 16, "Blender: an OpenGL normal, a 16-bit height")
        check("ao" in p, "Blender: the baked ambient occlusion too")
        p = same("unreal", preset="unreal")
        check(p["orm"]["contents"].startswith("R occlusion, G roughness, B metallic") and "DirectX" in p["normal"]["contents"],
              "Unreal: occlusion, roughness and metallic packed in one, a DirectX normal (%s)" % p["orm"]["contents"])
        p = same("unity", preset="unity")
        check("smoothness" in p["mask"]["contents"] and p["mask"]["channels"] == 4, "Unity: the MaskMap with smoothness in alpha")
        p = same("gltf", preset="gltf", size=128)
        check(p["metallicRoughness"]["size"] == 128, "glTF: the size asked for")
        same("psd", preset="blender", format="psd")
        # two texture sets: a set of files each
        obj = os.path.join(tmp, "two.obj")
        two_slot_cube(obj)
        c.request("open_mesh", {"path": obj, "resolution": 256})
        c.request("texture_tiles", {"mode": "material_slots"})
        p = same("sets", preset="blender")
        names = sorted(os.listdir(os.path.join(tmp, "sets")))
        check(any("Lid" in n for n in names) and any("Side" in n for n in names) and len(p) == 2 * len([k for k in p if k.endswith("@0")]),
              "two texture sets: a set of files each (%d)" % len(names))
    finally:
        proc.terminate()
        try:
            proc.wait(10)
        except subprocess.TimeoutExpired:
            proc.kill()
        shutil.rmtree(tmp, ignore_errors=True)
    print("export plan: %s" % ("all checks passed" if not failures else "%d FAILED" % len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
