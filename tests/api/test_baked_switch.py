#!/usr/bin/env python3
"""Baked maps (0.44.2), against a real headless Patina: a baked map's layer switched off takes the map out of the 3D
view (the ambient occlusion no longer shades the model) and out of the exports (no ao file), while smart masks keep
it; switched on again, both come back. A UV edit after a bake (the bake moved along with the paint) is reported as
out_of_date, and rebake makes it current.

    PATINA_BIN=build/patina python3 tests/api/test_baked_switch.py
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
    exe = os.environ.get("PATINA_BIN") or os.path.join(HERE, "..", "..", "build", "patina")
    tmp = tempfile.mkdtemp(prefix="patina_baked_switch_test_")
    env = dict(os.environ, HOME=os.path.join(tmp, "home"), XDG_DATA_HOME=os.path.join(tmp, "data"),
               XDG_CONFIG_HOME=os.path.join(tmp, "config"), XDG_CACHE_HOME=os.path.join(tmp, "cache"), PATINA_EXE=exe)
    os.makedirs(env["HOME"], exist_ok=True)
    port = free_port()
    proc = subprocess.Popen([exe, "--headless", "--port", str(port)], env=env, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    try:
        from PIL import Image, ImageChops  # noqa: PLC0415
        c = protocol.JsonLineClient(port=port, connect_timeout=1.0, response_timeout=300.0)
        for _ in range(100):
            try:
                c.connect()
                break
            except Exception:
                time.sleep(0.1)
        c.request("new_project", {"primitive": "torus", "resolution": 512})
        c.request("bake_mesh_maps", {"ao": True, "curvature": True, "thickness": False})

        def maps():
            return {m["map"]: m for m in c.request("baked_maps", {})["maps"]}

        def render():
            r = c.request("render_view", {"width": 320, "height": 320, "mode": "material",
                                          "camera": {"view": "top", "frame": True}})
            return Image.open(io.BytesIO(base64.b64decode(r["image_png_base64"]))).convert("L")

        def exported(name):
            out = os.path.join(tmp, name)
            os.makedirs(out, exist_ok=True)
            c.request("export_textures", {"directory": out, "preset": "blender", "size": 256, "base_name": "t"})
            return sorted(os.listdir(out))

        m = maps()
        check(m["ao"]["baked"] and m["ao"]["shown"] and not m["ao"]["out_of_date"], "AO baked, shown, current (%s)" % m["ao"])
        on = render()
        files_on = exported("on")
        ao_layer = m["ao"]["layer"]["index"]
        c.request("update_layer", {"index": ao_layer, "visible": False})
        m = maps()
        off = render()
        files_off = exported("off")
        diff = sum(1 for v in ImageChops.difference(on, off).tobytes() if v > 6)
        check(not m["ao"]["shown"], "its layer hidden switches the map off (shown false)")
        check(diff > 300, "the 3D view no longer shades with it (%d pixels change)" % diff)
        check(any("_ao" in f for f in files_on) and not any("_ao" in f for f in files_off),
              "exports leave it out (%s -> %s)" % ([f for f in files_on if "ao" in f], [f for f in files_off if "ao" in f]))
        c.request("update_layer", {"index": ao_layer, "visible": True})
        back = render()
        diff = sum(1 for v in ImageChops.difference(on, back).tobytes() if v > 6)
        check(maps()["ao"]["shown"] and diff < 50, "switched on again, it is back (%d pixels differ)" % diff)
        # a UV edit after the bake: out of date until baked again
        c.request("uv_transform", {"faces": [0], "islands": True, "rotate": 90})
        m = maps()
        check(m["ao"]["out_of_date"] and m["curvature"]["out_of_date"], "a UV edit leaves the bakes out of date")
        c.request("baked_maps", {"rebake": True})
        m = maps()
        check(not m["ao"]["out_of_date"] and not m["curvature"]["out_of_date"], "rebake makes them current")
        # saved and opened again: still current, and the next UV edit still shows
        path = os.path.join(tmp, "t.patina")
        c.request("save_project", {"path": path})
        c.request("open_file", {"path": path})
        check(not maps()["ao"]["out_of_date"], "a saved project keeps what its bakes were made for")
        c.request("uv_transform", {"faces": [0], "islands": True, "translate": [0.01, 0.0]})
        check(maps()["ao"]["out_of_date"], "and a UV edit after opening it shows again")
    finally:
        proc.terminate()
        try:
            proc.wait(10)
        except subprocess.TimeoutExpired:
            proc.kill()
        shutil.rmtree(tmp, ignore_errors=True)
    print("baked switch: %s" % ("all checks passed" if not failures else "%d FAILED" % len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
