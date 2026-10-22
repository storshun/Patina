#!/usr/bin/env python3
"""(1.2) Selecting faces in batches and face sets, against a real headless Patina: a face loop and a ring round a
torus (across and along an edge), the connected part, a flat region; face sets made from the selection and from the
model's parts, hidden, shown alone and shown again, filling and masking a layer, renamed and recolored, undone, kept
in the project, carried through a mesh reduction; Blender's sculpt face sets coming in with the model.

    PATINA_BIN=build/patina python3 tests/api/test_face_sets.py
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

    def __call__(self, cmd, **a):
        return self.c.request(cmd, a)

    def error(self, cmd, **a):
        try:
            self.c.request(cmd, a)
        except Exception as e:  # noqa: BLE001
            return str(e)
        return None

    def stop(self):
        self.proc.terminate()
        try:
            self.proc.wait(10)
        except subprocess.TimeoutExpired:
            self.proc.kill()


def sheet_with_sets(path, n=6):
    """an n x n sheet (Blender's XY plane) whose left and right halves are two sculpt face sets"""
    pos, fs, lv, sets = [], [], [], []
    for j in range(n + 1):
        for i in range(n + 1):
            pos += [i / n - 0.5, j / n - 0.5, 0.0]
    for j in range(n):
        for i in range(n):
            fs.append(4)
            lv += [j * (n + 1) + i, j * (n + 1) + i + 1, (j + 1) * (n + 1) + i + 1, (j + 1) * (n + 1) + i]
            sets.append(3 if i < n // 2 else 7)
    uv = []
    for v in lv:
        uv += [(v % (n + 1)) / n, (v // (n + 1)) / n]
    protocol.write_ptmesh(path, "Sheet", pos, fs, lv, loop_uvs=uv, face_material=[0] * len(fs), face_sets=sets,
                          extra_header={"had_uvs": True, "evaluated": False})


def main():
    exe = os.environ.get("PATINA_BIN") or os.path.join(HERE, "..", "..", "build", "patina")
    tmp = tempfile.mkdtemp(prefix="patina_face_sets_test_")
    env = dict(os.environ, HOME=os.path.join(tmp, "home"), XDG_DATA_HOME=os.path.join(tmp, "data"),
               XDG_CONFIG_HOME=os.path.join(tmp, "config"), XDG_CACHE_HOME=os.path.join(tmp, "cache"), PATINA_EXE=exe)
    os.makedirs(env["HOME"], exist_ok=True)
    try:
        p = Patina(exe, env)
        try:
            # ---- loops and rings round a torus (64 segments round, 24 round the tube)
            p("new_project", primitive="torus", resolution=512)
            faces = p("get_state")["mesh"]["faces"]
            a = p("select_loop", face=0, edge=0)["selected"]
            b = p("select_loop", face=0, edge=1)["selected"]
            check(sorted([a, b]) == [24, 64], "a face loop and the one across it go right round (%d, %d)" % (a, b))
            r = p("select_loop", face=0, edge=0, ring=True)["selected"]
            check(r == b, "the ring along an edge is the loop across the next one (%d)" % r)
            check(p("select_loop", face=5, edge=1, mode="add")["selected"] > b, "Shift adds a loop")
            p("select_faces", mode="clear")
            check(p("select_linked", face=0)["selected"] == faces, "the connected part: the whole torus (%d)" % faces)
            check(len(p("history")["steps"]) and p("history")["steps"][-1] == "Select the connected part", "each one undo step")
            # ---- a flat region: a plane is flat all over, the torus only face by face
            check(p("select_linked", face=0, by="flat", angle=1)["selected"] < 4, "a torus has no flat region")
            p("new_project", primitive="plane", resolution=512)
            pf = p("get_state")["mesh"]["faces"]
            check(p("select_linked", face=0, by="flat", angle=5)["selected"] == pf, "a plane is one flat region (%d)" % pf)
            # ---- face sets from a selection
            p("new_project", primitive="torus", resolution=512)
            p("select_loop", face=0, edge=0)
            band = p("make_face_set", name="Band")
            check(band["name"] == "Band" and band["faces"] in (24, 64), "a face set from the selection (%s)" % band)
            p("select_loop", face=40, edge=1)
            other = p("make_face_set")
            check(other["name"].startswith("Face Set"), "a second one named by itself (%s)" % other["name"])
            sets = p("list_face_sets")
            n_band = sets["face_sets"][0]["faces"]
            check(len(sets["face_sets"]) == 2 and n_band == band["faces"] - 1,
                  "listed with their faces; the face both loops cross is in the second one only (%s)" % [s["faces"] for s in sets["face_sets"]])
            sel = p("use_face_set", set="Band", action="select")["selected"]
            check(sel == n_band, "one click selects its faces (%d)" % sel)
            # ---- hidden, shown alone, shown again
            h = p("use_face_set", set="Band", action="hide")["hidden_faces"]
            check(h == n_band, "hidden: its faces (%d)" % h)
            iso = p("use_face_set", set="Band", action="isolate")["hidden_faces"]
            check(iso == faces - n_band, "shown alone: everything else hidden, the faces in no set too (%d)" % iso)
            p("use_face_set", action="show_all")
            check(all(not s["hidden"] for s in p("list_face_sets")["face_sets"]), "and all shown again")
            # ---- fill and mask the selected layer
            p("add_layer", type="paint")
            p("set_paint_material", id="", color=[0.9, 0.1, 0.1])
            p("use_face_set", set="Band", action="fill")
            layer = p("list_layers")[-1]
            check("basecolor" in layer.get("painted_channels", []), "fill: the layer painted on it")
            p("add_layer", type="paint")
            p("use_face_set", set="Band", action="mask")
            check(p("list_layers")[-1].get("has_mask"), "mask: the selected layer masked with it")
            # ---- edited, undone
            r = p("edit_face_set", set="Band", name="Stripe", color=[0.1, 0.2, 0.9])
            check(r["name"] == "Stripe" and abs(r["color"][2] - 0.9) < 1e-6, "renamed and recolored")
            check(p.error("edit_face_set", set="Stripe", name=other["name"]) is not None, "two sets can't share a name")
            p("undo")
            check(p("list_face_sets")["face_sets"][0]["name"] == "Band", "one undo takes the change back")
            # ---- kept in the project
            p("use_face_set", set="Band", action="hide")
            project = os.path.join(tmp, "sets.patina")
            p("save_project", path=project)
            p("new_project", primitive="cube")
            check(not p("list_face_sets")["face_sets"], "a new model starts without face sets")
            p("open_file", path=project)
            back = p("list_face_sets")["face_sets"]
            check([s["name"] for s in back] == ["Band", other["name"]] and back[0]["hidden"] and back[0]["faces"] == n_band,
                  "the project keeps them, hidden as they were (%s)" % [(s["name"], s["faces"], s["hidden"]) for s in back])
            # ---- from the model's parts; through a reduction
            r = p("make_face_set", **{"from": "island"})
            check(len(r["face_sets"]) >= 1, "a set for each UV island (%d)" % len(r["face_sets"]))
            p("make_face_set", **{"from": "connected"})
            check(len(p("list_face_sets")["face_sets"]) == 1, "a set for each connected part (the torus: one)")
            p("select_loop", face=0, edge=0)
            p("make_face_set", name="Band")
            red = p("mesh_reduce", ratio=0.5)
            after = p("list_face_sets")
            check(red["triangles_after"] < red["triangles_before"] and any(s["name"] == "Band" and s["faces"] > 0 for s in after["face_sets"]),
                  "a reduction keeps the sets on the faces where they were (%s)" % [(s["name"], s["faces"]) for s in after["face_sets"]])
            p("delete_face_set", all=True)
            check(not p("list_face_sets")["face_sets"], "delete them all")
            # ---- Blender's sculpt face sets
            ex = os.path.join(tmp, "exchange", "Sheet")
            os.makedirs(ex, exist_ok=True)
            sheet_with_sets(os.path.join(ex, "Sheet.ptmesh"))
            p("bridge_open_mesh", path=os.path.join(ex, "Sheet.ptmesh"), object="Sheet", blender_port=free_port(),
              exchange_dir=os.path.dirname(ex), resolution=256)
            bl = p("list_face_sets")["face_sets"]
            check(sorted(s["name"] for s in bl) == ["Face Set 3", "Face Set 7"] and all(s["faces"] == 18 for s in bl),
                  "Blender's sculpt face sets come in with the model (%s)" % [(s["name"], s["faces"]) for s in bl])
        finally:
            p.stop()
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("face sets: %s" % ("all checks passed" if not failures else "%d FAILED" % len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
