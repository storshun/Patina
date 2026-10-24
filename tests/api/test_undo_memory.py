#!/usr/bin/env python3
"""(1.2.7) Undo memory, against a real headless Patina:
- texture copies past the graphics card budget (undo_settings gpu_mb) are compressed into RAM, and undoing and
  redoing through those steps gives back exactly the same textures;
- past the RAM budget (ram_mb) the oldest steps are dropped, and the steps limit (steps) holds.
Each UV edit with "keep paint" copies every layer: on a big model one step was past the old fixed 1 GB on the
card, so only the latest UV edit could be undone.

    PATINA_BIN=build/patina python3 tests/api/test_undo_memory.py
"""
import base64
import io
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
from PIL import Image, ImageChops  # noqa: E402

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


def main():
    exe = os.path.abspath(os.environ.get("PATINA_BIN") or os.path.join(HERE, "..", "..", "build", "patina"))
    if not os.path.exists(exe):
        print("SKIP: patina binary not found (set PATINA_BIN)")
        return 0
    tmp = tempfile.mkdtemp(prefix="patina_undo_memory_test_")
    env = dict(os.environ, HOME=os.path.join(tmp, "home"), XDG_DATA_HOME=os.path.join(tmp, "data"),
               XDG_CONFIG_HOME=os.path.join(tmp, "config"), XDG_CACHE_HOME=os.path.join(tmp, "cache"))
    os.makedirs(env["HOME"], exist_ok=True)
    port = free_port()
    proc = subprocess.Popen([exe, "--headless", "--port", str(port)], env=env, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    try:
        c = protocol.JsonLineClient(port=port, connect_timeout=1.0, response_timeout=600.0)
        for _ in range(100):
            try:
                c.connect()
                break
            except Exception:
                time.sleep(0.1)
        R = c.request

        def tex(channel="basecolor"):
            r = R("get_texture", {"channel": channel, "size": 0})
            data = r.get("image_png_base64") or r.get("png_base64")
            return Image.open(io.BytesIO(base64.b64decode(data))).convert("RGBA")

        def same(a, b):
            return ImageChops.difference(a, b).getbbox() is None

        R("new_project", {"primitive": "sphere", "resolution": 2048})
        # three paint layers with noisy, different paint (it compresses like real work, not like flat color)
        for k, mat in enumerate(("Rust", "Concrete", "Leather")):
            R("add_layer", {"type": "paint", "name": "paint %d" % k})
            try:
                R("fill", {"mode": "mesh", "material": {"id": mat}})
            except protocol.RemoteError:
                R("fill", {"mode": "mesh", "material": {"color": [0.2 * k, 0.5, 0.7]}})
            R("paint_stroke", {"space": "uv", "points": [[0.1 + 0.2 * k, 0.1], [0.5, 0.5 + 0.1 * k], [0.9, 0.3]],
                               "brush": {"size": 120}, "material": {"color": [1, 0.1 * k, 0]}})
        base = R("undo_settings", {"steps": 100, "gpu_mb": 256, "ram_mb": 8192})
        print("  settings", base)
        start = len(R("history", {})["steps"])
        shots = [tex()]
        n = 5
        t0 = time.time()
        for i in range(n):
            R("uv_transform", {"faces": list(range(0, 200)), "islands": True, "translate": [0.01 * (i + 1), 0.0],
                               "reproject": True})
            shots.append(tex())
        edit_s = time.time() - t0
        u = R("undo_settings", {})
        print("  after %d UV edits (%.1f s):" % (n, edit_s), u)
        check(len(R("history", {})["steps"]) == start + n, "every UV edit is an undo step")
        check(u["used_ram_mb"] > 0, "older steps were compressed into RAM (%.0f MB)" % u["used_ram_mb"])
        check(u["used_gpu_mb"] < 256 + 200, "the card keeps about its budget: %.0f MB" % u["used_gpu_mb"])
        t0 = time.time()
        ok = True
        for i in range(n, 0, -1):
            R("undo", {})
            ok &= same(tex(), shots[i - 1])
        check(ok, "undoing every step, compressed ones too, gives back the exact textures (%.1f s)" % (time.time() - t0))
        ok = True
        for i in range(1, n + 1):
            R("redo", {})
            ok &= same(tex(), shots[i])
        check(ok, "and redoing them too")

        # the RAM budget: the oldest steps go, the newest stays
        before = len(R("history", {})["steps"])
        limit = max(1, int(u["used_ram_mb"] / 2))
        u = R("undo_settings", {"ram_mb": limit})
        after = len(R("history", {})["steps"])
        check(u["used_ram_mb"] <= limit and after < before and after >= 1,
              "a smaller RAM budget drops the oldest steps (%d -> %d steps, %.0f MB)" % (before, after, u["used_ram_mb"]))
        R("undo", {})
        check(same(tex(), shots[n - 1]), "the newest step still undoes")

        # the steps limit
        R("undo_settings", {"steps": 10, "ram_mb": 8192})
        for i in range(14):
            R("paint_stroke", {"space": "uv", "points": [[0.05 * i, 0.9], [0.05 * i + 0.02, 0.92]], "brush": {"size": 8}})
        check(len(R("history", {})["steps"]) == 10, "steps: 10 keeps 10 steps (%d)" % len(R("history", {})["steps"]))
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
        shutil.rmtree(tmp, ignore_errors=True)
    print("undo memory: %s" % ("all checks passed" if not failures else "%d FAILED" % len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
