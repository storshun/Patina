#!/usr/bin/env python3
"""The Island brush (0.43) against a real headless Patina, through island_brush: a cloth with thickness (a sheet
with a second layer 2 mm under it, joined round the rim). Seen from above, neither reach takes a face of the layer
under it (up to 0.42.0 the circle took both layers). Seen at a grazing angle, Surface takes about as many faces as
head-on (the brush's size along the surface), while Screen takes a long strip (the circle on screen). With Mirror X
the dab takes the mirror images too.

    PATINA_BIN=build/patina python3 tests/api/test_island_brush.py
"""
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


N, TH = 40, 0.002


def sheet_obj():
    """A 1 x 1 sheet in XZ round the origin, facing up, a layer under it facing down, and the rim. Faces: the top
    layer first (N*N), then the one under it (N*N), then the rim."""
    v, f = [], []

    def idx(layer, i, j):
        return layer * (N + 1) * (N + 1) + j * (N + 1) + i + 1

    for layer in (0, 1):
        for j in range(N + 1):
            for i in range(N + 1):
                v.append((-0.5 + i / N, -layer * TH, -0.5 + j / N))
    for j in range(N):
        for i in range(N):
            f.append((idx(0, i, j), idx(0, i, j + 1), idx(0, i + 1, j + 1), idx(0, i + 1, j)))
    for j in range(N):
        for i in range(N):
            f.append((idx(1, i, j), idx(1, i + 1, j), idx(1, i + 1, j + 1), idx(1, i, j + 1)))
    for k in range(N):
        f.append((idx(0, k, 0), idx(0, k + 1, 0), idx(1, k + 1, 0), idx(1, k, 0)))
        f.append((idx(0, k + 1, N), idx(0, k, N), idx(1, k, N), idx(1, k + 1, N)))
        f.append((idx(0, 0, k + 1), idx(0, 0, k), idx(1, 0, k), idx(1, 0, k + 1)))
        f.append((idx(0, N, k), idx(0, N, k + 1), idx(1, N, k + 1), idx(1, N, k)))
    lines = ["v %.6f %.6f %.6f" % p for p in v] + ["f %d %d %d %d" % q for q in f]
    return "\n".join(lines) + "\n"


def layer(face):
    return 0 if face < N * N else 1 if face < 2 * N * N else 2


def main():
    exe = os.environ.get("PATINA_BIN") or os.path.join(HERE, "..", "..", "build", "patina")
    tmp = tempfile.mkdtemp(prefix="patina_island_brush_test_")
    env = dict(os.environ, HOME=os.path.join(tmp, "home"), XDG_DATA_HOME=os.path.join(tmp, "data"),
               XDG_CONFIG_HOME=os.path.join(tmp, "config"), XDG_CACHE_HOME=os.path.join(tmp, "cache"), PATINA_EXE=exe)
    os.makedirs(env["HOME"], exist_ok=True)
    path = os.path.join(tmp, "sheet.obj")
    open(path, "w").write(sheet_obj())
    port = free_port()
    proc = subprocess.Popen([exe, "--headless", "--port", str(port)], env=env, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    try:
        c = protocol.JsonLineClient(port=port, connect_timeout=1.0, response_timeout=300.0)
        for _ in range(100):
            try:
                c.connect()
                break
            except Exception:
                time.sleep(0.1)
        c.request("open_mesh", {"path": path})
        c.request("set_viewport_size", {"width": 800, "height": 800})

        def dab(reach, point=(0.5, 0.5), size=120):
            c.request("select_faces", {"mode": "clear"})
            r = c.request("island_brush", {"point": list(point), "size": size, "reach": reach, "list": True})
            faces = r["face_list"]
            return faces, sum(1 for f in faces if layer(f) == 1)

        c.request("set_camera", {"view": "top", "frame": True})
        top = {}
        for reach in ("surface", "screen"):
            faces, under = dab(reach)
            top[reach] = len(faces)
            check(len(faces) > 30 and under == 0, "from above, %s takes %d faces, none of the layer under it (%d)"
                  % (reach, len(faces), under))
        # a grazing angle: 20 degrees above the sheet
        c.request("set_camera", {"view": "front", "frame": True})
        c.request("set_camera", {"pitch": 0.35})
        graze = {}
        for reach in ("surface", "screen"):
            faces, under = dab(reach)
            graze[reach] = len(faces)
            check(under == 0, "at a grazing angle, %s takes none of the layer under it (%d)" % (reach, under))
        ratio = graze["surface"] / max(1, top["surface"])
        check(0.6 < ratio < 1.4, "Surface takes about as much at a grazing angle as from above (%d vs %d)"
              % (graze["surface"], top["surface"]))
        check(graze["screen"] > 1.8 * graze["surface"], "Screen takes a long strip at a grazing angle (%d vs %d)"
              % (graze["screen"], graze["surface"]))
        # Mirror X: the mirror images too
        c.request("set_camera", {"view": "top", "frame": True})
        one, _ = dab("surface", point=(0.7, 0.5), size=80)
        c.request("set_symmetry", {"x": True})
        both, _ = dab("surface", point=(0.7, 0.5), size=80)
        c.request("set_symmetry", {"x": False})
        check(len(set(both)) >= 2 * len(set(one)) - 2, "with Mirror X a dab takes its mirror images (%d -> %d faces)"
              % (len(set(one)), len(set(both))))
        # one undo step a dab
        c.request("select_faces", {"mode": "clear"})
        c.request("island_brush", {"point": [0.5, 0.5], "size": 60})
        n = c.request("island_brush", {"point": [0.3, 0.5], "size": 60})["selected"]
        c.request("undo", {})
        back = c.request("select_faces", {"mode": "add", "faces": []})["selected"]
        check(0 < back < n, "Undo takes back the last dab (%d -> %d)" % (n, back))
    finally:
        proc.terminate()
        try:
            proc.wait(10)
        except subprocess.TimeoutExpired:
            proc.kill()
        shutil.rmtree(tmp, ignore_errors=True)
    print("island brush: %s" % ("all checks passed" if not failures else "%d FAILED" % len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
