#!/usr/bin/env python3
"""(0.44) Blender sends an object to a Patina window that was restarted: the window finds the project the object was
painted in among its recent files (projects saved before 0.44 were never linked to their object) and opens it with
its layers. In the real window (Xvfb or
PATINA_SHOTS_DISPLAY).

    python3 tests/gui/test_bridge_project.py [path/to/patina]
"""
import os
import shutil
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
sys.path.insert(0, os.path.join(ROOT, "blender", "patina_bridge"))
import manual_shots  # noqa: E402
import manual_scenes  # noqa: E402
import protocol  # noqa: E402

BIN = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "patina")
failures = []


def check(cond, what):
    print(("  ok   " if cond else "  FAIL ") + what)
    if not cond:
        failures.append(what)


def sheet(path, n=8):
    pos, fs, lv, uv = [], [], [], []
    for j in range(n + 1):
        for i in range(n + 1):
            pos += [i / n - 0.5, j / n - 0.5, 0.0]
    for j in range(n):
        for i in range(n):
            q = [j * (n + 1) + i, j * (n + 1) + i + 1, (j + 1) * (n + 1) + i + 1, (j + 1) * (n + 1) + i]
            fs.append(4)
            lv += q
            for v in q:
                uv += [(v % (n + 1)) / n, (v // (n + 1)) / n]
    protocol.write_ptmesh(path, "Mai", pos, fs, lv, loop_uvs=uv, loop_colors=[0.0, 0.0, 0.0, 1.0] * len(lv),
                          extra_header={"had_uvs": True, "evaluated": False})


def red(s):
    x0, y0, x1, y1 = manual_scenes.VIEW
    b = s.image().crop((x0, y0, x1, y1)).convert("RGB").tobytes()
    return sum(1 for r, g, bl in zip(b[0::3], b[1::3], b[2::3]) if r > 150 and g < 80 and bl < 80)


def main():
    work = tempfile.mkdtemp(prefix="patina_bridge_project_gui_")
    ex = os.path.join(work, "exchange")
    os.makedirs(os.path.join(ex, "Mai"))
    sent = os.path.join(ex, "Mai", "Mai.ptmesh")
    sheet(sent)
    project = os.path.join(work, "Mai painted.patina")
    send = {"path": sent, "object": "Mai", "blender_port": 7999, "exchange_dir": ex, "resolution": 256}
    # an earlier session: painted red and saved (then the link is dropped, as for a project saved before 0.44)
    s = manual_shots.Session(BIN, os.path.join("/tmp", "patina_bridge_project_test"), display=":97", port=7886)
    try:
        s("bridge_open_mesh", **send)
        s("fill", mode="mesh", material={"id": "", "color": [0.9, 0.05, 0.05], "roughness": 0.4})
        s("add_layer", type="paint")
        s("save_project", path=project)
        link = os.path.join(s.home, ".config", "patina", "blender_projects.json")
        check(os.path.exists(link), "saving links the project to Mai")
    finally:
        s.close()

    # a restarted window that has the project in its recent files
    s = manual_shots.Session(BIN, os.path.join("/tmp", "patina_bridge_project_test"), display=":97", port=7886,
                             session={"recent_files": [project]})
    try:
        s("set_camera", view="top", frame=True)
        before = red(s)
        r = s("bridge_open_mesh", **send)
        check(r.get("project") == project and r.get("layers") == 2,
              "Blender's Mai opens the project from the recent files (%s, %s layers)" % (r.get("project"), r.get("layers")))
        s("set_camera", view="top", frame=True)
        time.sleep(0.8)
        after = red(s)
        check(after > before + 5000, "the window shows its paint (%d -> %d red pixels)" % (before, after))
        st = s("get_state")
        check(st["bridge"]["object"] == "Mai", "and stays linked to Mai")
    finally:
        s.close()
        shutil.rmtree(work, ignore_errors=True)
        shutil.rmtree(os.path.join("/tmp", "patina_bridge_project_test"), ignore_errors=True)
    print("bridge project: %s" % ("all checks passed" if not failures else "%d FAILED" % len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
