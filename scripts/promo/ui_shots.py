"""The app screenshot for the promo: the logo cube painted in a real Patina window (on Xvfb, as the manual's shots,
or on the X display PATINA_SHOTS_DISPLAY names).

    python3 scripts/promo/ui_shots.py
"""
import os
import sys
import time

from promo_paths import EXE, ROOT, WORK

sys.path.insert(0, str(ROOT / "scripts"))
import manual_shots  # noqa: E402
import paint_cube  # noqa: E402

# the window goes to that X display (Xvfb, or PATINA_SHOTS_DISPLAY), never to the desktop's Wayland session
os.environ.pop("WAYLAND_DISPLAY", None)
os.environ["SDL_VIDEO_DRIVER"] = "x11"
s = manual_shots.Session(str(EXE), str(WORK / "shots"), display=":96", port=7892)
try:
    paint_cube.paint(s, 2048)
    s("set_environment", hdr_path=str(WORK / "hdr" / "courtyard.hdr"), intensity=1.0)
    s("set_view", mode="material", seams=False)
    s("select_layer", index=paint_cube.idx(s, "Gold"))
    s("set_camera", yaw=0.8, pitch=0.5, frame=True)
    time.sleep(2.0)
    s.shot("ui_main")
finally:
    s.close()
