"""Makes the canonical examples: every task's plan runs on a fresh copy of its scene in headless
Patina (through assistant_apply, as the Assistant will run it) and is kept only when its checks
pass. Failures are logged per task type (a type that fails often has a bug).

    python3 tools/assistant/generate.py --per-scene 150 --workers 8
writes ~/patina-ml/data/canonical.jsonl and ~/patina-ml/logs/generate-failures.jsonl
"""
import argparse
import collections
import copy
import json
import multiprocessing as mp
import os
import random
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))
import checks  # noqa: E402
import common  # noqa: E402
import scenes  # noqa: E402
import tasks  # noqa: E402
import tasks_r6  # noqa: E402

_server = None
_faces_cache = {}


def _init():
    global _server
    from multiprocessing import util
    _server = common.Server()
    util.Finalize(_server, _server.stop, exitpriority=10)  # runs when the worker exits normally


def scene_with_faces(meta):
    if meta["id"] not in _faces_cache:
        _server("open_file", path=meta["path"])
        parts = _server("mesh_parts", faces=True)["parts"]
        sc = dict(meta)
        sc["part_faces"] = {p["id"]: p["face_indices"] for p in parts}
        sc["part_faces"]["all"] = list(range(meta["faces"]))
        _faces_cache[meta["id"]] = sc
    return _faces_cache[meta["id"]]


def _fill_last(obj, index):
    if isinstance(obj, dict):
        return {k: _fill_last(v, index) for k, v in obj.items()}
    if isinstance(obj, list):
        return [_fill_last(v, index) for v in obj]
    return index if obj == "@LAST" else obj


def run_example(meta, task_name, seed):
    """Samples one example of the task on the scene and runs it; returns (row or None, failure or None)."""
    s = _server
    sc = scene_with_faces(meta)
    rng = random.Random(seed)
    fn = dict((f.__name__, f) for f, _ in tasks.TASKS + tasks.TASKS_R3 + tasks.TASKS_R4 + tasks.TASKS_R5 + tasks_r6.TASKS_R6 + tasks_r6.TASKS_R7)[task_name]
    exm = fn(sc, rng)
    if exm is None:
        return None, None
    try:
        s("open_file", path=meta["path"])
        for cmd, args in exm.get("pre", []):  # the state the request starts from (a selection...)
            s(cmd, **args)
        last_index = None
        for st in exm["setup"]:
            r = s("assistant_apply", plan=st["plan"], request=st["request"])
            for res in r["results"]:
                if isinstance(res, dict) and "uid" in res:
                    last_index = res["index"]
        if last_index is not None:
            exm["plan"] = _fill_last(exm["plan"], last_index)
        ctx = s("assistant_context", request=exm["request"])
        before = checks.snapshot(s, exm["check"])
        if exm["plan"].get("steps"):
            s("assistant_apply", plan=exm["plan"], request=exm["request"])
        after = checks.snapshot(s)
        # reference measurements: what the canonical plan produced
        for spec in exm["check"]:
            if not spec.pop("ref", False):
                continue
            if spec["t"] == "uv_parts":
                for p in spec["parts"]:
                    p["faces"] = sc["part_faces"][p.pop("part")]
                    p["islands_max"] = len(s("uv_islands", faces=p["faces"], limit=100000)["islands"])
            elif spec["t"] == "state":
                spec["approx"] = checks.get_path(after["state"], spec["path"])
            elif spec["t"] == "uv_ref":
                spec["values"] = {k: after["uv"].get(k) for k in spec["fields"]}
        ok, msgs = checks.run_checks(s, exm["check"], before, after, exm["plan"])
    except common.CommandFailed as e:
        ok, msgs = False, [str(e)]
    if not ok:
        return None, {"scene": meta["id"], "task": exm["task"], "variant": exm["variant"], "request": exm["request"],
                      "plan": exm["plan"], "why": msgs}
    row = dict(exm)
    row.update({"scene": meta["id"], "held_out_scene": meta["held_out"], "seed": seed, "context": ctx["text"],
                "materials": ctx["materials"]})
    return row, None


def _job(args):
    meta, task_name, seed = args
    try:
        return run_example(meta, task_name, seed)
    except Exception as e:  # noqa: BLE001 - a broken example must not stop the run
        return None, {"scene": meta["id"], "task": task_name, "why": ["%s: %s" % (type(e).__name__, e)]}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--per-scene", type=int, default=150)
    ap.add_argument("--workers", type=int, default=8)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--only-task")
    ap.add_argument("--r3", action="store_true", help="only the round-3 tasks (tasks.TASKS_R3)")
    ap.add_argument("--r4", action="store_true", help="only the round-4 tasks (tasks.TASKS_R4)")
    ap.add_argument("--r5", action="store_true", help="only the round-5 tasks (tasks.TASKS_R5)")
    ap.add_argument("--r6", action="store_true", help="only the round-6 tasks (tasks_r6.TASKS_R6)")
    ap.add_argument("--r7", action="store_true", help="only the round-7 tasks (tasks_r6.TASKS_R7)")
    ap.add_argument("--only-scene")
    ap.add_argument("--out", default=os.path.join(common.DATA, "canonical.jsonl"))
    a = ap.parse_args()
    common.ensure_dirs()
    metas = scenes.load_scenes()
    if a.only_scene:
        metas = [m for m in metas if m["id"] == a.only_scene]
    table = tasks_r6.TASKS_R7 if a.r7 else tasks_r6.TASKS_R6 if a.r6 else tasks.TASKS_R5 if a.r5 else tasks.TASKS_R4 if a.r4 else tasks.TASKS_R3 if a.r3 else tasks.TASKS
    names = [f.__name__ for f, _ in table]
    weights = [w for _, w in table]
    jobs = []
    rng = random.Random(a.seed)
    for m in metas:
        for k in range(a.per_scene):
            t = a.only_task or rng.choices(names, weights)[0]
            jobs.append((m, t, "%d-%s-%d" % (a.seed, m["id"], k)))
    t0 = time.time()
    rows, fails = [], []
    pool = mp.get_context("fork").Pool(a.workers, initializer=_init)
    try:
        for i, (row, fail) in enumerate(pool.imap_unordered(_job, jobs, chunksize=4)):
            if row:
                rows.append(row)
            if fail:
                fails.append(fail)
            if (i + 1) % 200 == 0:
                print("  %d / %d  (%d kept, %d failed, %.0fs)" % (i + 1, len(jobs), len(rows), len(fails), time.time() - t0),
                      flush=True)
    finally:
        pool.close()  # workers exit normally and stop their servers
        pool.join()
    # identical requests on one scene are kept once
    seen, uniq = set(), []
    for r in rows:
        key = (r["scene"], r["request"], json.dumps(r["setup"]), json.dumps(r.get("pre", [])))
        if key not in seen:
            seen.add(key)
            uniq.append(r)
    common.jsonl_write(a.out, uniq)
    common.jsonl_write(os.path.join(common.LOGS, "generate-failures.jsonl"), fails)
    by = collections.Counter(r["task"] for r in uniq)
    bad = collections.Counter(f["task"] for f in fails)
    print("kept %d examples (%d duplicates dropped), %d failed, in %.0fs" % (len(uniq), len(rows) - len(uniq), len(fails),
                                                                          time.time() - t0))
    for t in sorted(set(by) | set(bad)):
        print("  %-22s kept %5d  failed %4d" % (t, by[t], bad[t]))


if __name__ == "__main__":
    main()
