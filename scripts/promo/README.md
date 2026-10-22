# Promo kit

The banners, posts and promo video, made from Patina's own renders and screenshots. The logo is
`docs/brand/patina_icon.webp` (the icon, cut out) and `docs/brand/patina_wordmark.png`.

Everything is written to `build-promo/` (set `PATINA_PROMO_WORK` to use another folder). Patina runs with a home of
its own, so the real library is never touched. It needs `build-release/patina` (or `PATINA_BIN`), Blender,
Pillow, numpy, ffmpeg with libx264, Xvfb (for the app screenshot) and the Inter fonts
(`/usr/share/fonts/rsms-inter-fonts`).

```sh
# 1. the logo cube, and two of Blender's studio lights as .hdr
blender -b --factory-startup --python scripts/promo/make_cube.py -- build-promo/models
blender -b --factory-startup --python scripts/promo/to_hdr.py -- build-promo/hdr \
    /usr/share/blender/*/datafiles/studiolights/world/{courtyard,studio}.exr
# 2. Patina paints the cube and renders it (a still and four turntables), 16 material spheres, a develop pair
python3 scripts/promo/render_assets.py
# 3. the app window with the cube painted in it
python3 scripts/promo/ui_shots.py
# 4. the images: hero, paint, app, develop, game-ready, batch, X header, social preview, square
python3 scripts/promo/make_banners.py
# 5. the video: 1920x1080, 30 fps, 50 s, silent
python3 scripts/promo/make_video.py
python3 scripts/promo/make_video.py --stills 2,9.8,24 build-promo/stills   # a few frames to look at
```

The cube's layers (`paint_cube.py`): a walnut fill; a marble folder limited to the top's material slot; polished
gold on the band's slot; a copper folder with verdigris in clouds. The turntable frame that matches the icon is 239
(`HERO` in `make_video.py`); the video sways the cube around it.

Check the numbers the images quote (materials, generators, smart materials, MCP tools) before a release:
`make_banners.py` and `make_video.py` write them as text.
