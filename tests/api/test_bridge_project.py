#!/usr/bin/env python3
"""An object Blender sends again after Patina was restarted (0.44), against real headless Patinas through the bridge
commands. Once the project it was painted in has been saved, a new Patina that gets the object opens that project and
keeps its layers, also when the mesh changed since (the paint is moved across); an object of the same name that is
another model, or Send with Keep Layers off, starts a new project. Without a project, the object starts from what
Blender's materials show (the add-on 0.13 describes them), and a Color attribute that is one flat color (Blender's
black or white default) is left out instead of turning everything black. An edited mesh that comes without its
UVs keeps Patina's UVs and islands instead of being unwrapped again.

    PATINA_BIN=build/patina python3 tests/api/test_bridge_project.py
"""
import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zlib

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


def sheet(path, name, n=8, size=1.0, offset=(0.0, 0.0, 0.0), colors=None, materials=None, uvs="grid", drop=()):
    """An n x n sheet in Blender's XY plane (Z up), written as a .ptmesh. uvs: "grid" (one island over 0..1),
    "halves" (two islands, the left and right halves apart), None (sent without UVs, as from Edit Mode before
    add-on 0.13). drop: faces left out (j * n + i)."""
    pos, fs, lv, uv = [], [], [], []
    for j in range(n + 1):
        for i in range(n + 1):
            pos += [offset[0] + size * (i / n - 0.5), offset[1] + size * (j / n - 0.5), offset[2]]
    for j in range(n):
        for i in range(n):
            if j * n + i in drop:
                continue
            q = [j * (n + 1) + i, j * (n + 1) + i + 1, (j + 1) * (n + 1) + i + 1, (j + 1) * (n + 1) + i]
            fs.append(4)
            lv += q
            for v in q:
                u, w = (v % (n + 1)) / n, (v // (n + 1)) / n
                if uvs == "halves":
                    u = u * 0.45 if i < n // 2 else 0.55 + (u - 0.5) * 0.45
                uv += [u, w] if uvs else [0.0, 0.0]
    kw = {"loop_uvs": uv, "face_material": [0] * len(fs)}
    if colors is not None:
        kw["loop_colors"] = list(colors) * len(lv)
    extra = {"had_uvs": bool(uvs), "evaluated": False, "material_names": ["Skin"]}
    if materials is not None:
        extra["materials"] = materials
    protocol.write_ptmesh(path, name, pos, fs, lv, extra_header=extra, **kw)
    return path


def png(path, w, h, rgb):
    raw = b"".join(b"\0" + bytes(rgb) * w for _ in range(h))
    chunk = lambda t, d: struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                           chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def center_color(c, out):
    """The base color in the middle of the texture set (an exported PNG), 0..255."""
    os.makedirs(out, exist_ok=True)
    r = c.request("export_textures", {"directory": out, "preset": "blender", "size": 64, "base_name": "t"})
    path = next((p for k, p in (r.get("files") or {}).items() if k.startswith("basecolor")), None)
    if path is None:
        path = next(os.path.join(out, f) for f in sorted(os.listdir(out)) if "basecolor" in f)
    from PIL import Image  # noqa: PLC0415
    im = Image.open(path).convert("RGB")
    return im.getpixel((im.width // 2, im.height // 2))


class Patina:
    def __init__(self, exe, env):
        self.port = free_port()
        self.proc = subprocess.Popen([exe, "--headless", "--port", str(self.port)], env=env,
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.c = protocol.JsonLineClient(port=self.port, connect_timeout=1.0, response_timeout=300.0)
        for _ in range(100):
            try:
                self.c.connect()
                break
            except Exception:
                time.sleep(0.1)

    def stop(self):
        self.proc.terminate()
        try:
            self.proc.wait(10)
        except subprocess.TimeoutExpired:
            self.proc.kill()


def main():
    exe = os.environ.get("PATINA_BIN") or os.path.join(HERE, "..", "..", "build", "patina")
    tmp = tempfile.mkdtemp(prefix="patina_bridge_project_test_")
    env = dict(os.environ, HOME=os.path.join(tmp, "home"), XDG_DATA_HOME=os.path.join(tmp, "data"),
               XDG_CONFIG_HOME=os.path.join(tmp, "config"), XDG_CACHE_HOME=os.path.join(tmp, "cache"), PATINA_EXE=exe)
    os.makedirs(env["HOME"], exist_ok=True)
    ex = os.path.join(tmp, "exchange")
    os.makedirs(os.path.join(ex, "Mai"), exist_ok=True)
    sent = os.path.join(ex, "Mai", "Mai.ptmesh")
    black = [0.0, 0.0, 0.0, 1.0]

    def send(p, path=sent, obj="Mai", **kw):
        a = {"path": path, "object": obj, "blender_port": free_port(), "exchange_dir": ex, "resolution": 256}
        a.update(kw)
        return p.c.request("bridge_open_mesh", a)

    project = os.path.join(tmp, "work", "Mai painted.patina")
    os.makedirs(os.path.dirname(project), exist_ok=True)
    try:
        # 1. a first Patina: the object comes in with a black Color attribute nothing shows, gets painted, saved
        p = Patina(exe, env)
        try:
            sheet(sent, "Mai", colors=black)
            r = send(p)
            layers = p.c.request("list_layers", {})
            check("model_textures" not in r and len(layers) == 1,
                  "a flat black Color attribute is left out (%d layer, %s)" % (len(layers), r.get("model_textures")))
            col = center_color(p.c, os.path.join(tmp, "out0"))
            check(min(col) > 60, "so the model isn't black (%s)" % (col,))
            p.c.request("fill", {"mode": "mesh", "material": {"id": "", "color": [0.9, 0.05, 0.05], "roughness": 0.4}})
            p.c.request("add_layer", {"type": "paint"})
            p.c.request("save_project", {"path": project})
            linked = json.load(open(os.path.join(env["XDG_CONFIG_HOME"], "patina", "blender_projects.json")))
            check(linked.get("objects", {}).get("Mai") == [project], "the save links the project to Mai (%s)" % linked)
        finally:
            p.stop()

        # 2. a new Patina (a restart): Blender sends Mai again, the project opens with its layers
        p = Patina(exe, env)
        try:
            r = send(p)
            check(r.get("project") == project and r.get("kept_layers"), "the project opens (%s)" % r.get("project"))
            n = len(p.c.request("list_layers", {}))
            check(n == 2, "with its layers (%d)" % n)
            col = center_color(p.c, os.path.join(tmp, "out1"))
            check(col[0] > 180 and col[1] < 60, "and its paint (%s)" % (col,))
            st = p.c.request("get_state", {})
            check(st["bridge"]["object"] == "Mai", "linked to Mai (%s)" % st["bridge"]["object"])
            # Blender sends again while it is open: the same project, nothing reopened
            r = send(p)
            check(r.get("kept_layers") and "project" not in r, "a second send keeps what is open")
        finally:
            p.stop()

        # 3. remodeled (more faces, the same place): the project opens and the paint is moved across
        p = Patina(exe, env)
        try:
            sheet(sent, "Mai", n=12, colors=black)
            r = send(p)
            n = len(p.c.request("list_layers", {}))
            check(r.get("project") == project and n == 2, "a remodeled Mai opens the project too (%d layers)" % n)
            col = center_color(p.c, os.path.join(tmp, "out2"))
            check(col[0] > 180 and col[1] < 60, "with the paint moved onto the new mesh (%s)" % (col,))
        finally:
            p.stop()

        # 4. another model of the same name (other faces, elsewhere, another size), Keep Layers off, the feature off: a new project
        p = Patina(exe, env)
        try:
            other = os.path.join(ex, "Mai", "other.ptmesh")
            sheet(other, "Mai", n=5, size=6.0, offset=(20.0, 0.0, 0.0))
            r = send(p, path=other)
            check("project" not in r and len(p.c.request("list_layers", {})) == 1,
                  "another model named Mai starts a new project (%s)" % r.get("project"))
            p.c.request("new_project", {"primitive": "cube"})
            sheet(sent, "Mai", colors=black)
            r = send(p, keep_layers=False)
            check("project" not in r, "Keep Layers off starts a new project")
            p.c.request("new_project", {"primitive": "cube"})
            p.c.request("features", {"id": "bridge_projects", "on": False})
            r = send(p)
            check("project" not in r, "so does the feature switched off")
            p.c.request("features", {"id": "bridge_projects", "on": True})
        finally:
            p.stop()

        # 5. no project: what Blender's materials show (an image on Base Color, a roughness value)
        p = Patina(exe, env)
        try:
            os.makedirs(os.path.join(ex, "Skin"), exist_ok=True)
            img = os.path.join(ex, "Skin", "skin_basecolor.png")
            png(img, 32, 32, (40, 200, 60))
            mats = [{"name": "Skin", "principled": True, "base_color": [0.8, 0.8, 0.8, 1.0], "roughness": 0.3,
                     "metallic": 0.0, "emission": [0.0, 0.0, 0.0], "alpha": 1.0, "coat": 0.0, "sheen": 0.0,
                     "maps": {"base_color": {"path": img, "channel": 0, "invert": False, "image": "skin"}}}]
            skin = os.path.join(ex, "Skin", "Skin.ptmesh")
            sheet(skin, "Skin", colors=black, materials=mats)
            r = send(p, path=skin, obj="Skin")
            mt = r.get("model_textures") or {}
            check(mt.get("textures") == 1 and not mt.get("vertex_colors"),
                  "Blender's material comes in as a layer, without the black Color attribute (%s)" % mt)
            col = center_color(p.c, os.path.join(tmp, "out3"))
            check(abs(col[0] - 40) < 12 and abs(col[1] - 200) < 12, "showing its image (%s)" % (col,))
            # a material that shows the Color attribute (into Base Color): one flat color counts then, alone
            mats[0]["maps"] = {}
            mats[0]["vertex_colors"] = True
            sheet(skin, "Skin", colors=[0.8, 0.1, 0.1, 1.0], materials=mats)
            p.c.request("new_project", {"primitive": "cube"})
            r = send(p, path=skin, obj="Skin")
            mt = r.get("model_textures") or {}
            col = center_color(p.c, os.path.join(tmp, "out4"))
            check(mt.get("vertex_colors") and col[0] > 200 and col[1] < 110,
                  "a flat color counts where the material shows it (%s, %s)" % (mt.get("vertex_colors"), col))
        finally:
            p.stop()
        # 6. the same object again without its UVs after an edit (a face deleted; Blender in Edit Mode before add-on
        # 0.13 sent no UVs): Patina keeps its own UVs and islands, it doesn't unwrap it again
        p = Patina(exe, env)
        try:
            os.makedirs(os.path.join(ex, "Cloth"), exist_ok=True)
            cloth = os.path.join(ex, "Cloth", "Cloth.ptmesh")
            sheet(cloth, "Cloth", uvs="halves")
            send(p, path=cloth, obj="Cloth")
            before = p.c.request("uv_info", {})["islands"]
            p.c.request("fill", {"mode": "mesh", "material": {"id": "", "color": [0.1, 0.6, 0.2], "roughness": 0.5}})
            sheet(cloth, "Cloth", uvs=None, drop=(19,))
            r = send(p, path=cloth, obj="Cloth")
            after = p.c.request("uv_info", {})["islands"]
            check(before == 2 and after == 2 and r.get("kept_uvs", 0) > 0.99 and not r.get("generated_uvs"),
                  "an edited mesh without UVs keeps Patina's UVs and islands (%s -> %s islands, %s)" % (
                      before, after, r.get("kept_uvs")))
            col = center_color(p.c, os.path.join(tmp, "out5"))
            check(col[1] > 120 and col[0] < 80, "and the paint on them (%s)" % (col,))
            # another model under that name, without UVs: nothing to keep, it is unwrapped
            sheet(cloth, "Cloth", n=5, size=4.0, offset=(9.0, 0.0, 0.0), uvs=None)
            r = send(p, path=cloth, obj="Cloth")
            check(r.get("generated_uvs") and "kept_uvs" not in r, "a different model without UVs is unwrapped (%s)" % r)
        finally:
            p.stop()
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("bridge project: %s" % ("all checks passed" if not failures else "%d FAILED" % len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
