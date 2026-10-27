#!/usr/bin/env python3
"""Screenshots for the HTML manual: drives a real Patina window under Xvfb.

    python3 scripts/manual_shots.py [--bin build-release/patina] [--out docs/manual/img]

Needs Xvfb, libXtst and Pillow. Every picture in docs/manual/img comes from this
script (plus scripts/manual_web_shots.py for Patina Web), so they can be
regenerated after UI changes.
"""
import argparse
import base64
import ctypes as C
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from patina_client import Client  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def free_port():
    """A free port below the range the system hands out for connections (32768 and up on Linux), so a test's own
    connection can't take it before Patina listens on it."""
    import random
    import socket

    for _ in range(200):
        p = random.randrange(20000, 32000)
        s = socket.socket()
        try:
            s.bind(("127.0.0.1", p))
            return p
        except OSError:
            continue
        finally:
            s.close()
    raise SystemExit("no free port")


def wait_port_free(port, timeout=5.0):
    import socket

    t = time.time()
    while time.time() - t < timeout:
        s = socket.socket()
        try:
            s.bind(("127.0.0.1", port))
            return
        except OSError:
            time.sleep(0.1)
        finally:
            s.close()


class X:
    def __init__(self, display):
        self.x11 = C.CDLL("libX11.so.6")
        self.xt = C.CDLL("libXtst.so.6")
        self.x11.XOpenDisplay.restype = C.c_void_p
        self.x11.XFlush.argtypes = [C.c_void_p]
        self.xt.XTestFakeMotionEvent.argtypes = [C.c_void_p, C.c_int, C.c_int, C.c_int, C.c_ulong]
        self.xt.XTestFakeButtonEvent.argtypes = [C.c_void_p, C.c_uint, C.c_int, C.c_ulong]
        self.xt.XTestFakeKeyEvent.argtypes = [C.c_void_p, C.c_uint, C.c_int, C.c_ulong]
        self.x11.XKeysymToKeycode.argtypes = [C.c_void_p, C.c_ulong]
        self.x11.XStringToKeysym.argtypes = [C.c_char_p]
        self.x11.XStringToKeysym.restype = C.c_ulong
        self.x11.XSync.argtypes = [C.c_void_p, C.c_int]
        self.d = self.x11.XOpenDisplay(display.encode())
        if not self.d:
            raise SystemExit("cannot open display " + display)
        self.ox, self.oy = 0, 0
        self.settle = None  # (0.45) set by Session: waits for frames drawn instead of fixed times

    def pause(self, seconds, frames=3):
        """After synthetic input: until Patina has drawn `frames` more frames (the input handled), else `seconds`."""
        if self.settle is not None:
            self.x11.XSync(self.d, 0)  # (the X server has the events, so the next frames see them)
            self.settle(frames)
        else:
            time.sleep(seconds)

    def find_window(self):
        x = self.x11
        x.XDefaultRootWindow.restype = C.c_ulong
        x.XDefaultRootWindow.argtypes = [C.c_void_p]
        root = x.XDefaultRootWindow(self.d)
        r, p, n = C.c_ulong(), C.c_ulong(), C.c_uint()
        ch = C.POINTER(C.c_ulong)()
        x.XQueryTree.argtypes = [C.c_void_p, C.c_ulong, C.POINTER(C.c_ulong), C.POINTER(C.c_ulong),
                                 C.POINTER(C.POINTER(C.c_ulong)), C.POINTER(C.c_uint)]
        x.XGetGeometry.argtypes = [C.c_void_p, C.c_ulong, C.POINTER(C.c_ulong), C.POINTER(C.c_int), C.POINTER(C.c_int),
                                   C.POINTER(C.c_uint), C.POINTER(C.c_uint), C.POINTER(C.c_uint), C.POINTER(C.c_uint)]
        x.XQueryTree(self.d, root, C.byref(r), C.byref(p), C.byref(ch), C.byref(n))
        best = None
        for i in range(n.value):
            xx, yy, w, h, bw, dp = C.c_int(), C.c_int(), C.c_uint(), C.c_uint(), C.c_uint(), C.c_uint()
            x.XGetGeometry(self.d, ch[i], C.byref(r), C.byref(xx), C.byref(yy), C.byref(w), C.byref(h), C.byref(bw),
                           C.byref(dp))
            if w.value > 400 and (best is None or w.value * h.value > best[2] * best[3]):
                best = (xx.value, yy.value, w.value, h.value)
        if best:
            self.ox, self.oy = best[0], best[1]
        return best

    def move(self, x, y):
        self.xt.XTestFakeMotionEvent(self.d, -1, int(x + self.ox), int(y + self.oy), 0)
        self.x11.XFlush(self.d)

    def button(self, b, down):
        self.xt.XTestFakeButtonEvent(self.d, b, 1 if down else 0, 0)
        self.x11.XFlush(self.d)

    def key(self, name, down=None):
        kc = self.x11.XKeysymToKeycode(self.d, self.x11.XStringToKeysym(name.encode()))
        if down is None:
            self.xt.XTestFakeKeyEvent(self.d, kc, 1, 0)
            self.x11.XFlush(self.d)
            self.pause(0.06, 2)
            self.xt.XTestFakeKeyEvent(self.d, kc, 0, 0)
        else:
            self.xt.XTestFakeKeyEvent(self.d, kc, 1 if down else 0, 0)
        self.x11.XFlush(self.d)

    def click(self, x, y, b=1, hold=0.1):
        self.move(x, y)
        self.pause(0.2)
        self.button(b, True)
        if hold > 0.15:
            time.sleep(hold)  # (a press held on purpose)
        else:
            self.pause(hold, 2)
        self.button(b, False)
        self.pause(0.35)

    def drag(self, pts, b=1, dt=0.05, mods=()):
        for m in mods:
            self.key(m, True)
        self.move(*pts[0])
        self.pause(0.2)
        self.button(b, True)
        self.pause(0.15)
        for p in pts[1:]:
            self.move(*p)
            self.pause(dt, 1)
        self.pause(0.2)
        self.button(b, False)
        for m in mods:
            self.key(m, False)
        time.sleep(0.4)


class Session:
    def __init__(self, binary, out, display=":95", port=7871, welcome=False, session=None):
        if os.environ.get("PATINA_SHOTS_FREE_PORT"):  # (0.45, scripts/run_gui_tests.py: tests side by side)
            port = free_port()
        else:
            wait_port_free(port)  # (the session before may still be closing)
        self.out = out
        os.makedirs(out, exist_ok=True)
        self.home = tempfile.mkdtemp(prefix="patina_shots_home_")
        self.xvfb = None
        if not os.environ.get("PATINA_SHOTS_DISPLAY"):
            self.xvfb = subprocess.Popen(["Xvfb", display, "-screen", "0", "1600x1000x24"], stdout=subprocess.DEVNULL,
                                         stderr=subprocess.DEVNULL)
            time.sleep(1.0)
        else:
            display = os.environ["PATINA_SHOTS_DISPLAY"]
        # no welcome window in the screenshots (scenes that want it open it themselves)
        os.makedirs(os.path.join(self.home, ".config", "patina"), exist_ok=True)
        sp = os.path.join(self.home, ".config", "patina", "session.json")
        if session is not False and not os.path.exists(sp):  # (session=False: a first start, no session.json)
            with open(sp, "w") as f:
                json.dump(dict({"show_welcome": bool(welcome)}, **(session or {})), f)  # (session: preferences)
        # (1.2.12) PATINA_NO_BROWSER: the manual or the tutorial is only found, never opened in a real browser
        env = dict(os.environ, HOME=self.home, DISPLAY=display, SDL_VIDEO_DRIVER="x11", PATINA_NO_BROWSER="1")
        env.pop("WAYLAND_DISPLAY", None)  # on that X display, never on the desktop's Wayland session
        for attempt in range(3):
            self.proc = subprocess.Popen([binary, "--port", str(port)], env=env, stdout=subprocess.DEVNULL,
                                         stderr=open(os.path.join(self.home, "log.txt"), "w"))
            time.sleep(0.5)
            if self.proc.poll() is None:
                break
            # (it couldn't open the display or the port: once more, on a new port when allowed)
            print("patina exited at start (%s): again" % self.proc.returncode)
            time.sleep(1.0)
            if os.environ.get("PATINA_SHOTS_FREE_PORT"):
                port = free_port()
        for _ in range(100):
            try:
                self.c = Client(port)
                self.c("ping")
                break
            except Exception:
                time.sleep(0.2)
        else:
            code = self.proc.poll()
            try:
                tail = open(os.path.join(self.home, "log.txt")).read()[-1500:]
            except OSError:
                tail = ""
            self.proc.kill()
            raise SystemExit("patina did not start (port %d, exit %s)\n%s" % (port, code, tail))
        self.x = X(display)
        win = None
        for _ in range(50):  # the window on the display, and drawn a few times
            win = self.x.find_window()
            if win:
                break
            time.sleep(0.1)
        try:
            self.c("wait_frames", count=10)
            self.x.settle = lambda n: self.c("wait_frames", count=n)
        except Exception:  # (an older Patina: fixed times)
            time.sleep(1.5)
            win = self.x.find_window()
        print("window", win)

    def settle(self, frames=3, seconds=0.4):
        """Until the window has drawn `frames` more frames (else `seconds`)."""
        self.x.pause(seconds, frames)

    def __call__(self, cmd, **a):
        return self.c(cmd, **a)

    def image(self):
        from PIL import Image
        r = self.c("screenshot_window", max_size=1600)
        return Image.open(io.BytesIO(base64.b64decode(r["image_png_base64"]))).convert("RGB")

    def shot(self, name, crop=None, scale=None, settle=0.6):
        self.settle(max(2, int(settle * 10)), settle)
        im = self.image()
        if crop:
            im = im.crop(crop)
        if scale:
            im = im.resize((int(im.width * scale), int(im.height * scale)))
        path = os.path.join(self.out, name + ".png")
        im.save(path, optimize=True)
        print("  ", name, im.size)
        return im

    def close(self):
        self.proc.terminate()
        try:
            self.proc.wait(5)
        except Exception:
            self.proc.kill()
        if self.xvfb:
            self.xvfb.terminate()
        shutil.rmtree(self.home, ignore_errors=True)


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default=os.path.join(ROOT, "build-release", "patina"))
    ap.add_argument("--out", default=os.path.join(ROOT, "docs", "manual", "img"))
    ap.add_argument("--probe", action="store_true", help="only save a full screenshot of the default layout")
    ap.add_argument("--only03", action="store_true", help="only the scenes for the 0.3 features")
    ap.add_argument("--only031", action="store_true", help="only the scenes for 0.3.1 (Blender keymap)")
    ap.add_argument("--only04", action="store_true", help="only the scenes for the 0.4 features")
    ap.add_argument("--only041", action="store_true", help="only the scenes for 0.4.1 (stroke locks)")
    ap.add_argument("--only05", action="store_true", help="only the scenes for 0.5 (HTTP API)")
    ap.add_argument("--only051", action="store_true", help="only the scenes for 0.5.1 (pen tablets)")
    ap.add_argument("--only06", action="store_true", help="only the scenes for 0.6")
    ap.add_argument("--only07", action="store_true", help="only the scenes for 0.7 (and the masks guide)")
    ap.add_argument("--only071", action="store_true", help="only the scenes for 0.7.1 (pins, UV right-click menu)")
    ap.add_argument("--only080", action="store_true", help="only the scenes for 0.8 (seam brush, unwraps, relax, packing)")
    ap.add_argument("--only090", action="store_true", help="only the scenes for 0.9 (texture sets, UDIM)")
    ap.add_argument("--only0100", action="store_true", help="only the scenes for 0.10 (palette, symmetry, macros...)")
    ap.add_argument("--only036", action="store_true", help="only the scenes for 0.36 (right-click menu, language)")
    ap.add_argument("--only037", action="store_true", help="only the scenes for 0.37 (axis gizmo, retopology preview)")
    ap.add_argument("--only0371", action="store_true", help="only the scenes for 0.37.1 (pins in the 3D view)")
    ap.add_argument("--only038", action="store_true", help="only the scenes for 0.38 (view pie, side views, Solid)")
    ap.add_argument("--only039", action="store_true", help="only the scenes for 0.39 (path tracing)")
    ap.add_argument("--only040", action="store_true", help="only the scenes for 0.40 (the pies, the brush keys)")
    ap.add_argument("--only0410", action="store_true", help="only the scenes for 0.41 (baked maps as layers)")
    ap.add_argument("--only0430", action="store_true", help="only the scenes for 0.43 (the Island brush's Surface)")
    ap.add_argument("--only0450", action="store_true", help="only the scenes for 0.45 (the Select tool's Visible only)")
    ap.add_argument("--only0460", action="store_true", help="only the scenes for 0.46 (the Guided layout)")
    ap.add_argument("--only0470", action="store_true", help="only the scenes for 0.47 (the steps' colors, UV menu, export)")
    ap.add_argument("--only1100", action="store_true", help="only the scenes for 1.1 (the bake-down)")
    ap.add_argument("--only1200", action="store_true", help="only the scenes for 1.2 (objects, face sets, brushes)")
    ap.add_argument("--only1213", action="store_true", help="only the scenes for 1.2.13 (tool brushes, the tip cursor)")
    ap.add_argument("--only0110", action="store_true", help="only the scenes for 0.11 (the Patina Assistant; needs "
                                                           "PATINA_ASSISTANT_MODEL=<the .gguf>)")
    a = ap.parse_args()
    import manual_scenes
    if a.only036:
        s = Session(a.bin, a.out)
        try:
            manual_scenes.run_036(s)
            manual_scenes.run_036b(s)
        finally:
            s.close()
        raise SystemExit(0)
    if a.only037:
        s = Session(a.bin, a.out)
        try:
            manual_scenes.run_037(s)
        finally:
            s.close()
        raise SystemExit(0)
    if a.only0371:
        s = Session(a.bin, a.out)
        try:
            manual_scenes.run_0371(s)
        finally:
            s.close()
        raise SystemExit(0)
    if a.only038:
        s = Session(a.bin, a.out)
        try:
            manual_scenes.run_038(s)
        finally:
            s.close()
        raise SystemExit(0)
    if a.only039:
        s = Session(a.bin, a.out)
        try:
            manual_scenes.run_039(s)
        finally:
            s.close()
        raise SystemExit(0)
    if a.only040:
        s = Session(a.bin, a.out)
        try:
            manual_scenes.run_040(s)
        finally:
            s.close()
        raise SystemExit(0)
    if a.only0410:
        s = Session(a.bin, a.out)
        try:
            manual_scenes.run_0410(s)
        finally:
            s.close()
        raise SystemExit(0)
    if a.only0430:
        s = Session(a.bin, a.out)
        try:
            manual_scenes.run_0430(s)
        finally:
            s.close()
        raise SystemExit(0)
    if a.only0450:
        s = Session(a.bin, a.out)
        try:
            manual_scenes.run_0450(s)
        finally:
            s.close()
        raise SystemExit(0)
    if a.only1100:
        s = Session(a.bin, a.out)
        try:
            manual_scenes.run_1100(s)
        finally:
            s.close()
        raise SystemExit(0)
    if a.only1213:
        s = Session(a.bin, a.out)
        try:
            manual_scenes.run_1213(s)
        finally:
            s.close()
        raise SystemExit(0)
    if a.only1200:
        s = Session(a.bin, a.out)
        try:
            manual_scenes.run_1200(s)
        finally:
            s.close()
        raise SystemExit(0)
    if a.only0470:
        s = Session(a.bin, a.out, session={"layout": "guided"})
        try:
            manual_scenes.run_0470(s)
        finally:
            s.close()
        raise SystemExit(0)
    if a.only0460:
        s = Session(a.bin, a.out, session={"layout": "guided"})
        try:
            manual_scenes.run_0460(s)
        finally:
            s.close()
        raise SystemExit(0)
    if a.only0110:
        s = Session(a.bin, a.out)
        try:
            manual_scenes.run_0110(s)
        finally:
            s.close()
        raise SystemExit(0)
    if a.only0100:
        s = Session(a.bin, a.out)
        try:
            manual_scenes.run_0100(s)
        finally:
            s.close()
        raise SystemExit(0)
    if a.only090:
        s = Session(a.bin, a.out)
        try:
            manual_scenes.run_090(s)
        finally:
            s.close()
        raise SystemExit(0)
    if a.only080:
        s = Session(a.bin, a.out)
        try:
            manual_scenes.run_080(s)
        finally:
            s.close()
        raise SystemExit(0)
    if a.only071:
        s = Session(a.bin, a.out)
        try:
            manual_scenes.run_071(s)
        finally:
            s.close()
        raise SystemExit(0)
    if a.only07:
        s = Session(a.bin, a.out)
        try:
            manual_scenes.run_07(s)
        finally:
            s.close()
        raise SystemExit(0)
    if a.only06:
        s = Session(a.bin, a.out)
        try:
            manual_scenes.run_06(s)
            manual_scenes.run_07(s)
            manual_scenes.run_071(s)
        finally:
            s.close()
        raise SystemExit(0)
    if a.only051:
        s = Session(a.bin, a.out)
        try:
            manual_scenes.run_051(s)
        finally:
            s.close()
        raise SystemExit(0)
    if a.only05:
        s = Session(a.bin, a.out)
        try:
            manual_scenes.run_05(s)
        finally:
            s.close()
        raise SystemExit(0)
    if a.only041:
        s = Session(a.bin, a.out)
        try:
            manual_scenes.run_041(s)
        finally:
            s.close()
        raise SystemExit(0)
    # the welcome window needs its own fresh start
    w = Session(a.bin, a.out, welcome=True)
    try:
        manual_scenes.run_welcome(w)
    finally:
        w.close()
    if a.only04:
        s = Session(a.bin, a.out)
        try:
            manual_scenes.run_04(s)
        finally:
            s.close()
        raise SystemExit(0)
    s = Session(a.bin, a.out)
    try:
        if a.probe:
            import manual_scenes
            manual_scenes.build_suzanne(s)
            manual_scenes.probe(s)
        else:
            import manual_scenes
            if a.only031:
                manual_scenes.build_suzanne(s)
            else:
                if a.only03:
                    manual_scenes.build_suzanne(s)
                else:
                    manual_scenes.run(s)
                manual_scenes.run_03(s)
            manual_scenes.run_036(s)
            manual_scenes.run_037(s)
            manual_scenes.run_0371(s)
            manual_scenes.run_038(s)
            manual_scenes.run_039(s)
            manual_scenes.run_040(s)
            manual_scenes.run_0410(s)
            manual_scenes.run_blender_keymap(s)
            manual_scenes.run_04(s)
            manual_scenes.run_041(s)
            manual_scenes.run_05(s)
            manual_scenes.run_051(s)
    finally:
        s.close()
    if not a.probe:
        s = Session(a.bin, a.out)
        try:
            manual_scenes.run_06(s)
            manual_scenes.run_07(s)
            manual_scenes.run_071(s)
            manual_scenes.run_036b(s)
        finally:
            s.close()
