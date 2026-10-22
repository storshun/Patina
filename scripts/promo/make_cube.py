"""The logo cube for the promo: a beveled cube with four material slots (walnut, marble, gold, copper) laid out like
the icon, smart-UV-unwrapped and written as an OBJ for Patina to paint.

    blender -b --factory-startup --python scripts/promo/make_cube.py -- build-promo/models
"""
import bpy, bmesh, sys, os, math
out = sys.argv[sys.argv.index("--") + 1]
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.mesh.primitive_cube_add(size=2.0)
o = bpy.context.object
o.name = "PatinaCube"
bm = bmesh.new()
bm.from_mesh(o.data)
bmesh.ops.bevel(bm, geom=bm.edges[:] + bm.verts[:], offset=0.42, segments=10, profile=0.5, affect="EDGES")
bm.to_mesh(o.data)
bm.free()
# slots: 0 walnut, 1 marble, 2 gold, 3 copper (all plain gray: Patina paints them)
for n in ("Walnut", "Marble", "Gold", "Copper"):
    m = bpy.data.materials.new(n)
    o.data.materials.append(m)
me = o.data
for p in me.polygons:
    c, nz, nx, ny = p.center, p.normal.z, p.normal.x, p.normal.y
    # the camera looks from the front right (-Y, +X), above: front (-Y) walnut, right (+X) copper, top marble,
    # and a gold band sweeping over the top's right side and down round the top-right edge
    band = 0.15 * c.y + 0.10 < c.x  # the band edge runs diagonally over the top
    if nz > 0.35 and band:
        p.material_index = 2
    elif nz > 0.35:
        p.material_index = 1
    elif nx > 0.35:
        p.material_index = 3
    else:
        p.material_index = 0
for p in me.polygons:
    p.use_smooth = True
bpy.context.view_layer.objects.active = o
bpy.ops.object.mode_set(mode="EDIT")
bpy.ops.mesh.select_all(action="SELECT")
bpy.ops.uv.smart_project(angle_limit=math.radians(66), island_margin=0.01)
bpy.ops.object.mode_set(mode="OBJECT")
bpy.ops.wm.obj_export(filepath=os.path.join(out, "patina_cube.obj"), export_materials=True, export_uv=True,
                      export_normals=True, apply_modifiers=True, forward_axis="NEGATIVE_Z", up_axis="Y")
print("MADE", len(me.polygons))
