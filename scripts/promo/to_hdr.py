"""Blender's own studio lights (CC0, datafiles/studiolights/world/*.exr) as .hdr files Patina can light with.

    blender -b --factory-startup --python scripts/promo/to_hdr.py -- build-promo/hdr \
        /usr/share/blender/*/datafiles/studiolights/world/{courtyard,studio}.exr
"""
import bpy, sys, os
args = sys.argv[sys.argv.index("--") + 1:]
out = args[0]
scene = bpy.context.scene
scene.render.image_settings.file_format = "HDR"
for p in args[1:]:
    img = bpy.data.images.load(p)
    _ = img.pixels[0]  # load the pixels (images load lazily)
    dst = os.path.join(out, os.path.splitext(os.path.basename(p))[0] + ".hdr")
    img.save_render(dst, scene=scene)
    print("SAVED", dst, tuple(img.size))
