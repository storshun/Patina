#!/usr/bin/env python3
"""Windows kept between sessions (0.20.5): Patina starts with the Retopology window docked as a tab behind
Properties (nothing of retopology runs or shows until it is chosen); opening it brings its tab to the front;
the tool windows open when Patina closes (Retopology, Texel density...) and the layout come back next time.

    python3 tests/gui/test_window_layout.py [path/to/patina]
"""
import base64
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "tools", "assistant"))
import common  # noqa: E402

BIN = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "patina")
MODEL = os.path.join(ROOT, "samples", "Suzanne.glb")
PORT = 7889
failures = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        failures.append(what)


def main():
    home = tempfile.mkdtemp(prefix="patina_layout_test_")
    cfg = os.path.join(home, ".config", "patina")
    os.makedirs(cfg)
    with open(os.path.join(cfg, "session.json"), "w") as f:
        json.dump({"show_welcome": False}, f)
    xvfb = None
    display = os.environ.get("PATINA_SHOTS_DISPLAY")
    if not display:
        display = ":96"
        xvfb = subprocess.Popen(["Xvfb", display, "-screen", "0", "1600x1000x24"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(1.0)

    def start():
        env = dict(os.environ, HOME=home, DISPLAY=display, SDL_VIDEO_DRIVER="x11")
        env.pop("WAYLAND_DISPLAY", None)  # on that X display, never on the desktop's Wayland session
        p = subprocess.Popen([BIN, "--port", str(PORT)], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(100):
            try:
                c = common.Client(PORT, timeout=120)
                c.call("ping")
                break
            except Exception:
                time.sleep(0.2)
        else:
            p.kill()
            raise SystemExit("patina did not start")
        time.sleep(2)
        return p, c

    def stop(p, c):
        try:  # (saved first: no question about unsaved changes on the way out)
            c.call("save_project", {"path": os.path.join(home, "t.patina")})
        except Exception:
            pass
        p.terminate()
        try:
            p.wait(30)
        except Exception:
            p.kill()
            p.wait(10)

    try:
        p, c = start()
        try:
            c.call("open_file", {"path": MODEL})
            time.sleep(2)
            check(not c.call("ui_rect", {"name": "retopo.make_quads"}).get("found"),
                  "first start: the Retopology window is a tab behind another, not drawn")
            li = c.call("retopo_info")
            check(not li.get("building") and not li.get("result"), "no retopology preview runs while its tab is hidden")
            c.call("open_window", {"window": "retopology"})
            time.sleep(1.5)
            check(c.call("ui_rect", {"name": "retopo.make_quads"}).get("found"), "opening it brings its tab to the front")
            c.call("open_window", {"window": "texel_density"})
            time.sleep(1)
        finally:
            stop(p, c)
        with open(os.path.join(cfg, "session.json")) as f:
            w = json.load(f).get("windows", {})
        check(w.get("retopology") is True and w.get("texel_density") is True, "the open windows are saved (%s)" % w)
        with open(os.path.join(cfg, "layout.ini")) as f:
            check("[Window][Retopology]" in f.read(), "the layout is saved, with the Retopology window in it")
        p, c = start()
        try:
            c.call("open_file", {"path": MODEL})
            time.sleep(2)
            check(c.call("ui_rect", {"name": "retopo.make_quads"}).get("found"), "next start: the Retopology tab is still the one in front")
        finally:
            stop(p, c)
    finally:
        if xvfb:
            xvfb.terminate()
        shutil.rmtree(home, ignore_errors=True)
    print("window layout: %d failed" % len(failures) if failures else "window layout: all checks passed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
