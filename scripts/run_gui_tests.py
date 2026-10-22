#!/usr/bin/env python3
"""Runs the window tests (tests/gui/test_*.py) several at a time, each on a hidden display of its own (a virtual
KWin with Xwayland, on this machine's GPU), so the desktop, the open Patina and Blender are left alone. Longest
tests start first (the last run's times, in build/gui_test_times.json). Logs go to build/gui_logs/<test>.log;
what the tests leave in /tmp is removed afterward.

    python3 scripts/run_gui_tests.py                 (every test, 4 at a time)
    python3 scripts/run_gui_tests.py -j 6 pies uv_clicks
    python3 scripts/run_gui_tests.py --bin build-release/patina
"""
import argparse
import ctypes
import json
import os
import queue
import shutil
import signal
import subprocess
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TIMES = os.path.join(ROOT, "build", "gui_test_times.json")
LOGS = os.path.join(ROOT, "build", "gui_logs")


def free_display(start, taken):
    n = start
    while (os.path.exists("/tmp/.X11-unix/X%d" % n) or os.path.exists("/tmp/.X%d-lock" % n) or n in taken or
           os.path.exists(os.path.join(os.environ.get("XDG_RUNTIME_DIR", "/tmp"), "wayland-patinatest-%d" % n))):
        n += 1
    return n


class Display:
    """A hidden display: KWin (virtual, in a D-Bus session of its own) with Xwayland on it."""

    def __init__(self, n, width=1700, height=1100):
        self.n = n
        self.name = ":%d" % n
        sock = "wayland-patinatest-%d" % n
        quiet = dict(stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
        self.kwin = subprocess.Popen(["dbus-run-session", "--", "kwin_wayland", "--virtual", "--no-lockscreen",
                                      "--width", str(width), "--height", str(height), "--socket", sock], **quiet)
        runtime = os.environ.get("XDG_RUNTIME_DIR", "/tmp")
        for _ in range(100):
            if os.path.exists(os.path.join(runtime, sock)):
                break
            time.sleep(0.1)
        env = dict(os.environ, WAYLAND_DISPLAY=sock)
        self.xwl = subprocess.Popen(["Xwayland", self.name, "-geometry", "%dx%d" % (width, height)], env=env, **quiet)
        self.ready(20.0)

    def ready(self, timeout=10.0):
        """True once an X client can connect (Xwayland takes a moment after its socket appears)."""
        x11 = ctypes.CDLL("libX11.so.6")
        x11.XOpenDisplay.restype = ctypes.c_void_p
        x11.XOpenDisplay.argtypes = [ctypes.c_char_p]
        x11.XCloseDisplay.argtypes = [ctypes.c_void_p]
        t = time.time()
        while time.time() - t < timeout:
            if self.xwl.poll() is not None:
                return False
            d = x11.XOpenDisplay(self.name.encode())
            if d:
                x11.XCloseDisplay(d)
                return True
            time.sleep(0.1)
        return False

    def close(self):
        for p in (self.xwl, self.kwin):  # each in a process group of its own: KWin, its D-Bus daemon, Xwayland
            try:
                os.killpg(p.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
        for p in (self.xwl, self.kwin):
            try:
                p.wait(10)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(p.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass


def tmp_entries():
    try:
        return {n for n in os.listdir("/tmp") if n.startswith("patina_")}
    except OSError:
        return set()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("tests", nargs="*", help="test names (pies, test_pies.py ...); default: all")
    ap.add_argument("-j", "--jobs", type=int, default=4, help="tests at a time (default 4)")
    ap.add_argument("--bin", default=os.path.join(ROOT, "build", "patina"), help="the Patina to test")
    ap.add_argument("--timeout", type=int, default=900, help="seconds a test may take")
    a = ap.parse_args()
    gui = os.path.join(ROOT, "tests", "gui")
    tests = sorted(f for f in os.listdir(gui) if f.startswith("test_") and f.endswith(".py"))
    if a.tests:
        want = {t if t.startswith("test_") else "test_" + t for t in a.tests}
        want = {t if t.endswith(".py") else t + ".py" for t in want}
        tests = [t for t in tests if t in want]
    try:
        times = json.load(open(TIMES))
    except (OSError, ValueError):
        times = {}
    tests.sort(key=lambda t: -times.get(t, os.path.getsize(os.path.join(gui, t)) / 100.0))
    os.makedirs(LOGS, exist_ok=True)
    before = tmp_entries()
    jobs = max(1, min(a.jobs, len(tests)))
    displays, taken = [], set()
    t0 = time.time()
    try:
        for _ in range(jobs):
            n = free_display(20, taken)
            taken.add(n)
            displays.append(Display(n))
        free = queue.Queue()
        for d in displays:
            free.put(d)
        results, lock = {}, threading.Lock()

        def run(test):
            d = free.get()
            if not d.ready():
                print("      display %s stopped answering: starting it again" % d.name, flush=True)
                d.close()
                d.__init__(d.n)
            try:
                env = dict(os.environ, SDL_VIDEO_DRIVER="x11", PATINA_SHOTS_DISPLAY=d.name, DISPLAY=d.name,
                           PATINA_SHOTS_FREE_PORT="1")
                env.pop("WAYLAND_DISPLAY", None)
                s = time.time()
                try:
                    p = subprocess.run([sys.executable, os.path.join(gui, test), a.bin], env=env, cwd=ROOT,
                                       capture_output=True, text=True, timeout=a.timeout)
                    out, code = p.stdout + p.stderr, p.returncode
                except subprocess.TimeoutExpired as e:
                    out, code = (e.stdout or "") + "\nTIMEOUT after %d s" % a.timeout, -1
                    out = out if isinstance(out, str) else out.decode(errors="replace")
                took = time.time() - s
                open(os.path.join(LOGS, test[:-3] + ".log"), "w").write(out)
                skipped = code == 0 and "SKIP" in out
                failed = code != 0 or " FAIL " in out
                state = "SKIP" if skipped else "FAIL" if failed else "PASS"
                with lock:
                    results[test] = (state, took)
                    times[test] = round(took, 1)
                    print("%s  %-36s %6.1f s  (%s)" % (state, test, took, d.name), flush=True)
                    if failed:
                        for line in out.splitlines():
                            if "FAIL" in line or "Traceback" in line or "Error" in line or "TIMEOUT" in line:
                                print("      " + line.strip(), flush=True)
            finally:
                free.put(d)

        threads = []
        work = queue.Queue()
        for t in tests:
            work.put(t)

        def worker():
            while True:
                try:
                    t = work.get_nowait()
                except queue.Empty:
                    return
                run(t)

        for _ in range(jobs):
            th = threading.Thread(target=worker)
            th.start()
            threads.append(th)
        for th in threads:
            th.join()
    finally:
        for d in displays:
            d.close()
        for n in sorted(tmp_entries() - before):  # what the tests left in /tmp (RAM)
            p = os.path.join("/tmp", n)
            shutil.rmtree(p, ignore_errors=True) if os.path.isdir(p) else os.remove(p)
    try:
        json.dump(times, open(TIMES, "w"), indent=1, sort_keys=True)
    except OSError:
        pass
    passed = sum(1 for s, _ in results.values() if s == "PASS")
    failed = [t for t, (s, _) in results.items() if s == "FAIL"]
    skipped = [t for t, (s, _) in results.items() if s == "SKIP"]
    print("%d passed, %d failed, %d skipped in %.0f s (%d at a time)%s" % (
        passed, len(failed), len(skipped), time.time() - t0, jobs, (": " + ", ".join(failed)) if failed else ""))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
