#!/usr/bin/env python3
"""UVs Patina sends back, in a real (background) Blender (0.42). An object sent to Patina without UVs gets Patina's
UVs on every push, even when Blender has other UVs by then (an undo, a swapped mesh, another add-on): those are kept
in a UV map of their own ("UVMap before Patina") instead of the push being refused, and a second push writes over
Patina's own UVs without another copy. An object sent with UVs that were then edited in Blender still refuses
Patina's (the user's UV work wins: Update Mesh in Patina). The same in Edit Mode. (0.13) An object sent from Edit
Mode sends its UVs, second UV map and color attribute as from Object Mode.

    python3 tests/blender/test_uv_push.py        (needs blender on PATH, else skips)
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ADDON_PARENT = os.path.join(HERE, "..", "..", "blender")

SCRIPT = r'''
import json, os, sys
argv = sys.argv[sys.argv.index("--") + 1:]
sys.path.insert(0, argv[0])
tmp = argv[1]
import bpy
from patina_bridge import mesh_io, protocol

out = {}
bpy.ops.wm.read_factory_settings(use_empty=True)


def grid(name):
    bpy.ops.mesh.primitive_grid_add(x_subdivisions=6, y_subdivisions=6, size=2)
    o = bpy.context.active_object
    o.name = name
    me = o.data
    while me.uv_layers:
        me.uv_layers.remove(me.uv_layers[0])
    return o


def uvs_of(o, name=None):
    me = o.data
    layer = me.uv_layers[name] if name else me.uv_layers.active
    return [round(c, 5) for d in layer.data for c in d.uv]


def set_uvs(o, f):
    me = o.data
    layer = me.uv_layers.active or me.uv_layers.new(name="UVMap")
    for loop, d in zip(me.loops, layer.data):
        co = me.vertices[loop.vertex_index].co
        d.uv = f(co)


def ptuv_from(o, f, path):
    me = o.data
    flat = []
    for loop in me.loops:
        u, v = f(me.vertices[loop.vertex_index].co)
        flat += [u, v]
    with open(path, "wb") as fh:
        fh.write(protocol.encode_ptuv(o.name, flat))
    return [round(c, 5) for c in flat]


patina_a = lambda co: (co.x * 0.25 + 0.5, co.y * 0.25 + 0.5)
patina_b = lambda co: (co.y * 0.2 + 0.4, co.x * 0.2 + 0.4)
blender_uv = lambda co: (co.x * 0.5 + 0.5, co.y * 0.5 + 0.5)

# A: sent without UVs; Blender has UVs of its own by the first push
a = grid("NoUVs")
sent = mesh_io.export_object(a, tmp)
out["a_had_uvs"] = sent["had_uvs"]
set_uvs(a, blender_uv)
before = uvs_of(a)
want = ptuv_from(a, patina_a, os.path.join(tmp, "a1.ptuv"))
ok, msg = mesh_io.apply_ptuv(a, os.path.join(tmp, "a1.ptuv"), sent_ptmesh=sent["path"])
out["a1"] = [ok, msg]
out["a1_active"] = uvs_of(a) == want
out["a1_maps"] = [l.name for l in a.data.uv_layers]
out["a1_active_name"] = a.data.uv_layers.active.name
out["a1_kept"] = mesh_io.BACKUP_UV_NAME in a.data.uv_layers and uvs_of(a, mesh_io.BACKUP_UV_NAME) == before
# a second push over Patina's own UVs: no second copy
want2 = ptuv_from(a, patina_b, os.path.join(tmp, "a2.ptuv"))
ok, msg = mesh_io.apply_ptuv(a, os.path.join(tmp, "a2.ptuv"), sent_ptmesh=sent["path"])
out["a2"] = [ok, msg]
out["a2_active"] = uvs_of(a) == want2
out["a2_maps"] = len(a.data.uv_layers)

# B: sent with UVs, edited in Blender afterward: refused (the user's UVs win)
b = grid("WithUVs")
set_uvs(b, blender_uv)
sent_b = mesh_io.export_object(b, tmp)
out["b_had_uvs"] = sent_b["had_uvs"]
set_uvs(b, lambda co: (co.x * 0.1 + 0.1, co.y * 0.3 + 0.2))  # (edited in Blender)
ptuv_from(b, patina_a, os.path.join(tmp, "b.ptuv"))
ok, msg = mesh_io.apply_ptuv(b, os.path.join(tmp, "b.ptuv"), sent_ptmesh=sent_b["path"])
out["b"] = [ok, msg]
out["b_maps"] = len(b.data.uv_layers)

# C: as A, in Edit Mode
c = grid("EditMode")
sent_c = mesh_io.export_object(c, tmp)
set_uvs(c, blender_uv)
before_c = uvs_of(c)
want_c = ptuv_from(c, patina_a, os.path.join(tmp, "c.ptuv"))
bpy.context.view_layer.objects.active = c
for o in bpy.context.view_layer.objects:
    o.select_set(o == c)
bpy.ops.object.mode_set(mode="EDIT")
ok, msg = mesh_io.apply_ptuv(c, os.path.join(tmp, "c.ptuv"), sent_ptmesh=sent_c["path"])
bpy.ops.object.mode_set(mode="OBJECT")
out["c"] = [ok, msg]
out["c_active"] = uvs_of(c) == want_c
out["c_kept"] = mesh_io.BACKUP_UV_NAME in c.data.uv_layers and uvs_of(c, mesh_io.BACKUP_UV_NAME) == before_c

# D (add-on 0.13): sent from Edit Mode, with UVs, a second UV map and a color attribute. Blender 5.x leaves the
# mesh's attribute arrays empty in Edit Mode, so before 0.13 this went as "no UVs" and Patina unwrapped it again.
d = grid("FromEditMode")
set_uvs(d, blender_uv)
d.data.uv_layers.new(name="Lightmap")
ca = d.data.color_attributes.new("Col", "BYTE_COLOR", "CORNER")
for i, cd in enumerate(ca.data):
    cd.color_srgb = (0.9, 0.2, 0.1, 1.0) if i % 2 else (0.1, 0.3, 0.8, 1.0)
obj_mode = protocol.read_ptmesh(mesh_io.export_object(d, os.path.join(tmp, "d_object"))["path"])[1]
bpy.context.view_layer.objects.active = d
for o in bpy.context.view_layer.objects:
    o.select_set(o == d)
bpy.ops.object.mode_set(mode="EDIT")
sent_d = mesh_io.export_object(d, os.path.join(tmp, "d_edit"))
edit_mode = protocol.read_ptmesh(sent_d["path"])[1]
bpy.ops.object.mode_set(mode="OBJECT")
out["d_had_uvs"] = sent_d["had_uvs"]
for key in ("loop_uvs", "loop_uvs2", "loop_colors"):
    a, b = obj_mode.get(key), edit_mode.get(key)
    out["d_" + key] = a is not None and b is not None and max(abs(x - y) for x, y in zip(a.to_list(), b.to_list())) < 1e-6
print("UVPUSH_JSON " + json.dumps(out))
'''

failures = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        failures.append(what)


def main():
    blender = shutil.which("blender")
    if not blender:
        print("SKIP: blender not on PATH")
        return 0
    tmp = tempfile.mkdtemp(prefix="patina_uv_push_test_")
    try:
        script = os.path.join(tmp, "uv_push.py")
        open(script, "w").write(SCRIPT)
        p = subprocess.run([blender, "-b", "--factory-startup", "--python", script, "--", os.path.abspath(ADDON_PARENT), tmp],
                           capture_output=True, text=True, timeout=600)
        line = next((l for l in p.stdout.splitlines() if l.startswith("UVPUSH_JSON ")), None)
        if not line:
            print(p.stdout[-3000:], p.stderr[-3000:])
            check(False, "the Blender script ran")
            return 1
        r = json.loads(line[len("UVPUSH_JSON "):])
        check(r["a_had_uvs"] is False, "an object without UVs is sent with had_uvs false")
        check(r["a1"][0] and "kept as UV map" in r["a1"][1], "its first push applies Patina's UVs (%s)" % r["a1"][1])
        check(r["a1_active"] and r["a1_active_name"] == "UVMap", "Patina's UVs are the active map (%s)" % r["a1_active_name"])
        check(r["a1_kept"], "Blender's UVs are kept in 'UVMap before Patina' (%s)" % r["a1_maps"])
        check(r["a2"][0] and r["a2_active"] and r["a2_maps"] == 2, "a second push writes over Patina's own UVs, no new copy (%s, %d maps)" % (r["a2"][1], r["a2_maps"]))
        check(r["b_had_uvs"] is True and not r["b"][0] and "changed in Blender" in r["b"][1] and r["b_maps"] == 1,
              "UVs edited in Blender after a send with UVs still win (%s)" % r["b"][1])
        check(r["c"][0] and r["c_active"] and r["c_kept"], "the same in Edit Mode (%s)" % r["c"][1])
        check(r["d_had_uvs"] is True and r["d_loop_uvs"], "sent from Edit Mode, the UVs go (not \"no UVs\")")
        check(r["d_loop_uvs2"] and r["d_loop_colors"], "and so do the second UV map and the color attribute")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("uv push: %s" % ("all checks passed" if not failures else "%d FAILED" % len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
