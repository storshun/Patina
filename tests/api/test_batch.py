#!/usr/bin/env python3
"""Recipes and batches (0.30) against a real headless Patina: the built-in recipes are listed; a recipe of your own is
saved, applied to the open model and deleted; mesh_transform scales, places and goes back; a batch runs a recipe on a
folder of models in a hidden Patina, leaves a broken file behind and carries on, and writes the results.

    PATINA_BIN=build/patina python3 tests/api/test_batch.py
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


CUBE = ("v -1 -1 -1\nv 1 -1 -1\nv 1 1 -1\nv -1 1 -1\nv -1 -1 1\nv 1 -1 1\nv 1 1 1\nv -1 1 1\n"
        "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n"
        "f 1/1 4/4 3/3 2/2\nf 5/1 6/2 7/3 8/4\nf 1/1 2/2 6/3 5/4\nf 2/1 3/2 7/3 6/4\nf 3/1 4/2 8/3 7/4\nf 4/1 1/2 5/3 8/4\n")


def main():
    exe = os.path.abspath(os.environ.get("PATINA_BIN") or os.path.join(HERE, "..", "..", "build", "patina"))
    if not os.path.exists(exe):
        print("SKIP: patina binary not found (set PATINA_BIN)")
        return 0
    tmp = tempfile.mkdtemp(prefix="patina_batch_test_")
    env = dict(os.environ, HOME=os.path.join(tmp, "home"), XDG_DATA_HOME=os.path.join(tmp, "data"),
               XDG_CONFIG_HOME=os.path.join(tmp, "config"), XDG_CACHE_HOME=os.path.join(tmp, "cache"), PATINA_EXE=exe)
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

        def err_of(cmd, args):
            try:
                c.request(cmd, args)
            except protocol.RemoteError as e:
                return str(e)
            return ""

        recipes = c.request("recipe_list")
        ids = [r["id"] for r in recipes]
        check(all(i in ids for i in ("patina_game_asset", "patina_web_ar", "patina_print", "patina_cleanup")),
              "the built-in recipes are there (%d)" % len(ids))
        # models: two cubes (one with a hole and in centimeters) and a broken file
        models = os.path.join(tmp, "models")
        sub = os.path.join(models, "more")
        os.makedirs(sub)
        open(os.path.join(models, "box.obj"), "w").write(CUBE)
        open(os.path.join(sub, "holey.obj"), "w").write(CUBE.replace("f 4/1 1/2 5/3 8/4\n", ""))
        open(os.path.join(models, "broken.obj"), "w").write("this is not a model\n")
        # mesh_transform on the open model, and back
        c.request("open_file", {"path": os.path.join(models, "box.obj")})
        t = c.request("mesh_transform", {"from_units": "cm", "origin": "base"})
        check(t["changed"] and abs(t["size"][0] - 0.02) < 1e-5, "mesh_transform: centimeters to meters (%s)" % t["size"])
        st = c.request("mesh_check")["stats"]
        check(abs(st["size"][2] - 0.02) < 1e-5, "the model is 2 cm tall now")
        c.request("mesh_fix_revert")
        check(abs(c.request("mesh_check")["stats"]["size"][0] - 2.0) < 1e-5, "and back")
        # a recipe of your own, applied to the open model
        out1 = os.path.join(tmp, "applied")
        r = c.request("recipe_save", {"name": "Test recipe", "description": "a test",
                                      "steps": [{"cmd": "mesh_reduce", "args": {"ratio": 0.9}},
                                                {"cmd": "export_model", "args": {"path": "${out}/${name}_done.obj", "textures": False}}]})
        check(r["id"] == "user_test_recipe", "recipe_save gives it an id (%s)" % r["id"])
        a = c.request("recipe_apply", {"id": r["id"], "output": out1})
        check(len(a["steps"]) == 2 and os.path.isfile(os.path.join(out1, "box_done.obj")), "recipe_apply runs it on the open model")
        e = err_of("recipe_save", {"name": "Bad", "steps": [{"cmd": "no_such_command"}]})
        check("cmd Patina knows" in e, "a step Patina doesn't know is refused")
        e = err_of("recipe_delete", {"id": "patina_game_asset"})
        check("built-in" in e, "built-in recipes can't be deleted")
        c.request("recipe_delete", {"id": r["id"]})
        check("user_test_recipe" not in [x["id"] for x in c.request("recipe_list")], "your recipe is deleted")
        # a batch over the folder and its subfolder
        out2 = os.path.join(tmp, "batch_out")
        b = c.request("batch_run", {"recipe": "patina_cleanup", "folders": [models], "recursive": True, "output": out2})
        check(len(b["files"]) == 3, "the batch finds the three files (%s)" % [os.path.basename(f) for f in b["files"]])
        s = {}
        for _ in range(600):
            s = c.request("batch_status")
            if not s.get("running"):
                break
            time.sleep(0.25)
        check(not s.get("running") and s.get("done") == 3, "the batch finishes (%s of %s)" % (s.get("done"), s.get("files")))
        res = {os.path.basename(x["file"]): x for x in s.get("results", [])}
        check(res.get("box.obj", {}).get("ok") and res.get("holey.obj", {}).get("ok"), "the two models are done")
        check(res.get("broken.obj", {}).get("ok") is False and s.get("failed") == 1, "the broken file failed, and the rest went on")
        for nm in ("box.glb", "holey.glb"):
            p = os.path.join(out2, nm)
            check(os.path.isfile(p) and open(p, "rb").read(4) == b"glTF", "%s written" % nm)
        holey = [x for x in res.get("holey.obj", {}).get("steps", []) if x.get("cmd") == "mesh_fix"]
        check(holey and holey[0].get("ok"), "the holey one was mended")
        check(open(os.path.join(sub, "holey.obj")).read().count("\nf ") == 5, "the file itself is unchanged")
        e = err_of("batch_run", {"recipe": "patina_cleanup", "folders": [os.path.join(tmp, "empty")]})
        check("no model files" in e, "a batch with no models says so")
    finally:
        proc.terminate()
        try:
            proc.wait(10)
        except subprocess.TimeoutExpired:
            proc.kill()
        shutil.rmtree(tmp, ignore_errors=True)
    print("batch: " + ("all checks passed" if not failures else "%d FAILED" % len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
