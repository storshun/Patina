#!/usr/bin/env python3
"""(1.2.7) The UV editor's Mirrored halves and no-change UV edits, against a real headless Patina:
- select_faces mode mirror adds the other half of the selection by the model's symmetry, also when the two halves
  sit side by side in the layout. "Select the other half" only knew halves stacked on each other, so on a face
  unwrapped in two halves it was grayed out;
- an island command that moves nothing (Orient upright on islands that already are) says changed: false and makes
  no undo step. The editor said "Oriented upright" for a change no one could see.

    PATINA_BIN=build/patina python3 tests/api/test_uv_halves.py
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


def main():
    exe = os.path.abspath(os.environ.get("PATINA_BIN") or os.path.join(HERE, "..", "..", "build", "patina"))
    if not os.path.exists(exe):
        print("SKIP: patina binary not found (set PATINA_BIN)")
        return 0
    tmp = tempfile.mkdtemp(prefix="patina_uv_halves_test_")
    env = dict(os.environ, HOME=os.path.join(tmp, "home"), XDG_DATA_HOME=os.path.join(tmp, "data"),
               XDG_CONFIG_HOME=os.path.join(tmp, "config"), XDG_CACHE_HOME=os.path.join(tmp, "cache"))
    os.makedirs(env["HOME"], exist_ok=True)
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
        R = c.request
        R("new_project", {"primitive": "cube", "resolution": 512})
        R("uv_unwrap", {"method": "smart", "reproject": False})

        def island_of(f):
            return R("uv_islands", {"faces": [f]})["islands"][0]["island"]

        # the cube's +X and -X sides: each one's other half is the opposite side, in another island
        pairs = {}
        for f in range(6):
            R("select_faces", {"mode": "set", "faces": [f]})
            R("select_faces", {"mode": "mirror"})
            sel = R("uv_islands", {"selection": True})["islands"]
            pairs[f] = sorted(i["first_face"] for i in sel)
        sides = [f for f in range(6) if len(pairs[f]) == 2]
        check(len(sides) == 2, "the two X sides find their other half (%s)" % pairs)
        if len(sides) == 2:
            a, b = sides
            check(island_of(a) != island_of(b), "and the halves sit side by side, not stacked (islands %d, %d)"
                  % (island_of(a), island_of(b)))
        check(sum(len(v) == 1 for v in pairs.values()) == 4, "the sides the X mirror maps onto themselves add nothing")
        # stacked halves still count: Mirror UVs, then the other half of a stacked copy
        if len(sides) == 2:
            R("uv_mirror", {"axes": ["x"], "reproject": False})
            R("select_faces", {"mode": "set", "faces": [sides[0]]})
            n = R("select_faces", {"mode": "mirror"})["selected"]
            check(n == 2, "after Mirror UVs the stacked halves select each other (%d faces)" % n)

        # an edit that moves nothing: no undo step, changed false
        R("select_faces", {"mode": "all"})
        R("uv_orient", {"mode": "upright", "selection": True, "reproject": False})
        steps = len(R("history", {})["steps"])
        r = R("uv_orient", {"mode": "upright", "selection": True, "reproject": False})
        check(r.get("changed") is False, "Orient upright a second time changes nothing (%s)" % r)
        check(len(R("history", {})["steps"]) == steps, "and makes no undo step")
        R("uv_orient", {"mode": "longest_axis", "selection": True, "reproject": False})
        R("uv_relax", {"selection": True, "reproject": False})
        r = R("uv_relax", {"selection": True, "reproject": False})
        check(r.get("changed") is False, "Relax a second time changes nothing (%s)" % r)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
        shutil.rmtree(tmp, ignore_errors=True)
    print("uv halves: %s" % ("all checks passed" if not failures else "%d FAILED" % len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
