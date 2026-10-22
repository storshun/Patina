#!/usr/bin/env python3
"""Select only what the camera sees (0.45), against a real headless Patina through select_faces visible: a skin (a
sphere, its own UV island) inside a shell open at the front and the back (a garment). From the front, the skin's
island gives only its faces seen through the opening; the shell's island only its faces turned to the camera; from
the back the other side, hardly overlapping. Without visible, the whole islands as before; with remove, only the
seen faces come off; with the feature off, visible changes nothing.

    PATINA_BIN=build/patina python3 tests/api/test_select_visible.py
"""
import math
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


def skin_and_shell(path, nu=48, nv=24):
    """An OBJ: the skin (radius 0.8, faces first) and the shell (radius 1, open where |z| > 0.7), each a UV sphere
    with its own UVs. Returns (skin faces, shell faces)."""
    lines, counts = [], []
    vbase, tbase = 0, 0
    for radius, opening in ((0.8, None), (1.0, 0.7)):
        verts, uvs, faces = [], [], []
        for j in range(nv + 1):
            th = math.pi * j / nv
            for i in range(nu + 1):
                ph = 2 * math.pi * i / nu
                verts.append((radius * math.sin(th) * math.cos(ph), radius * math.cos(th), radius * math.sin(th) * math.sin(ph)))
                uvs.append((i / nu, 1 - j / nv))
        for j in range(nv):
            for i in range(nu):
                a, b, c, d = j * (nu + 1) + i, j * (nu + 1) + i + 1, (j + 1) * (nu + 1) + i + 1, (j + 1) * (nu + 1) + i
                if opening is not None and abs(sum(verts[k][2] for k in (a, b, c, d)) / 4) > opening * radius:
                    continue
                faces.append((a, b, c, d))  # outward
        lines += ["v %.6f %.6f %.6f" % v for v in verts] + ["vt %.6f %.6f" % t for t in uvs]
        lines += ["f " + " ".join("%d/%d" % (vbase + k + 1, tbase + k + 1) for k in f) for f in faces]
        vbase += len(verts)
        tbase += len(uvs)
        counts.append(len(faces))
    open(path, "w").write("\n".join(lines) + "\n")
    return counts


def main():
    exe = os.environ.get("PATINA_BIN") or os.path.join(HERE, "..", "..", "build", "patina")
    tmp = tempfile.mkdtemp(prefix="patina_select_visible_test_")
    env = dict(os.environ, HOME=os.path.join(tmp, "home"), XDG_DATA_HOME=os.path.join(tmp, "data"),
               XDG_CONFIG_HOME=os.path.join(tmp, "config"), XDG_CACHE_HOME=os.path.join(tmp, "cache"), PATINA_EXE=exe)
    os.makedirs(env["HOME"], exist_ok=True)
    obj = os.path.join(tmp, "skin_and_shell.obj")
    nskin, nshell = skin_and_shell(obj)
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
        c.request("open_mesh", {"path": obj})
        c.request("set_viewport_size", {"width": 800, "height": 800})
        c.request("set_camera", {"view": "front", "frame": True})
        hit = c.request("pick_face", {"point": [0.5, 0.5]})
        check(hit.get("hit") and hit["face"] < nskin, "the middle of the view is the skin, through the opening (%s)" % hit.get("face"))
        skin_face = hit["face"]
        edge = {}
        for y in (0.2, 0.15, 0.1, 0.25, 0.3):
            edge = c.request("pick_face", {"point": [0.5, y]})
            if edge.get("hit") and edge["face"] >= nskin:
                break
        check(edge.get("hit") and edge["face"] >= nskin, "higher up, the shell (%s)" % edge.get("face"))
        shell_face = edge["face"]

        def sel(**kw):
            r = c.request("select_faces", dict({"mode": "set"}, **kw))
            return r["selected"], r

        n_all, _ = sel(faces=[skin_face], mode="island")
        check(n_all == nskin, "without visible, the skin's whole island (%d of %d)" % (n_all, nskin))
        c.request("select_faces", {"mode": "clear"})
        n_vis, r = sel(faces=[skin_face], mode="island", visible=True)
        # (the opening, 0.71 wide, shows the skin's cap out to about 63 degrees: some 27 percent of it)
        check(0.15 * nskin < n_vis < 0.4 * nskin, "with visible, only the skin seen through the opening (%d of %d, %d left out)"
              % (n_vis, nskin, r.get("left_out_unseen", -1)))
        c.request("select_faces", {"mode": "clear"})
        front, _ = sel(faces=[shell_face], mode="island", visible=True)
        # (a perspective camera sees a sphere's faces turned toward it, not the band round its sides)
        check(0.1 * nshell < front < 0.5 * nshell, "the shell's island: the part facing the camera (%d of %d)" % (front, nshell))
        # from the back: the other side, hardly overlapping
        c.request("set_camera", {"view": "back", "frame": True})
        c.request("select_faces", {"mode": "clear"})
        back = c.request("select_faces", {"mode": "island", "faces": [shell_face], "visible": True})["selected"]
        c.request("set_camera", {"view": "front", "frame": True})
        both = c.request("select_faces", {"mode": "island", "faces": [shell_face], "visible": True})["selected"]
        check(0.1 * nshell < back < 0.5 * nshell and both >= front + back - 0.03 * nshell,
              "from the back the other side (%d), the two hardly overlap (%d together)" % (back, both))
        # remove with visible: only what is seen comes off
        c.request("select_faces", {"mode": "island", "faces": [shell_face]})
        left = c.request("select_faces", {"mode": "remove", "faces": list(range(nskin, nskin + nshell)), "visible": True})["selected"]
        check(abs(left - (nshell - front)) <= 2, "remove with visible takes off the seen faces only (%d left)" % left)
        # the feature off: visible changes nothing
        c.request("features", {"id": "select_visible", "on": False})
        c.request("select_faces", {"mode": "clear"})
        n_off = c.request("select_faces", {"mode": "island", "faces": [skin_face], "visible": True})["selected"]
        c.request("features", {"id": "select_visible", "on": True})
        check(n_off == nskin, "with the feature off, the whole island (%d)" % n_off)
    finally:
        proc.terminate()
        try:
            proc.wait(10)
        except subprocess.TimeoutExpired:
            proc.kill()
        shutil.rmtree(tmp, ignore_errors=True)
    print("select visible: %s" % ("all checks passed" if not failures else "%d FAILED" % len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
