"""Evaluation of an Assistant model, end to end through Patina: for every test request the scene
is opened (and any earlier request in the example applied), the model writes a plan
(assistant_ask, with the grammar), the plan runs (assistant_apply) and the example's checks
decide. Writes a Markdown report with the handoff's table, a breakdown per task type and the
worst failures.

    python3 tools/assistant/eval.py --model ~/patina-ml/gguf/patina-assistant-1.7b-q4_k_m.gguf \\
        --name r1-q4 [--limit 400] [--workers 2] [--user-requests ~/patina-ml/data/user_requests.jsonl]

Report: ~/patina-ml/reports/<name>.md (and .json with every result).
"""
import argparse
import collections
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

_server = None

HARMFUL_CMDS = {"remove_layer", "clear_layer", "uv_island_per_part", "uv_unwrap", "uv_pack", "uv_mirror", "uv_make_island",
                "uv_rectify", "uv_seams",
                # round 6: these clear the undo history, or rebuild the layout
                "texture_set_from_faces", "set_resolution", "texture_tiles", "uv_density"}
EXAMPLE_REQUESTS = ("creases", "crease", "wrinkle", "uv island", "island for the head", "head and", "island")


def _init(model):
    global _server
    from multiprocessing import util
    os.environ["PATINA_ASSISTANT_MODEL"] = model
    _server = common.Server()
    util.Finalize(_server, _server.stop, exitpriority=10)
    _server("assistant_model", action="load")
    run_one.metas = {m["id"]: m for m in scenes.load_scenes()}


def run_one(item):
    s = _server
    metas = run_one.metas
    meta = metas[item["scene"]]
    out = {"task": item["task"], "variant": item.get("variant", ""), "scene": item["scene"], "request": item["request"],
           "expected": item.get("plan")}
    try:
        s("open_file", path=meta["path"])
        for cmd, args in item.get("pre", []):
            s(cmd, **args)
        for st in item.get("setup", []):
            s("assistant_apply", plan=st["plan"], request=st["request"])
        t0 = time.time()
        ans = s("assistant_ask", request=item["request"])
        out["seconds"] = ans["seconds"]
        out["plan"] = ans["plan"]
        before = checks.snapshot(s, item["check"])
        applied_error = None
        if ans["plan"].get("steps"):
            try:
                s("assistant_apply", plan=ans["plan"], request=item["request"])
            except common.CommandFailed as e:
                applied_error = str(e)
        after = checks.snapshot(s)
        ok, msgs = checks.run_checks(s, item["check"], before, after, ans["plan"])
        if applied_error:
            ok, msgs = False, ["the plan failed: " + applied_error] + msgs
        out["ok"], out["why"] = ok, msgs
        # harmful: deletes, clears or rebuilds UVs when the expected plan doesn't
        exp_cmds = {st["cmd"] for st in item.get("plan", {}).get("steps", [])}
        got_cmds = {st["cmd"] for st in ans["plan"].get("steps", [])}
        out["harmful"] = bool((got_cmds & HARMFUL_CMDS) - exp_cmds)
    except common.CommandFailed as e:
        out.update({"ok": False, "why": [str(e)], "harmful": False})
    return out


def table(rows):
    def rate(xs):
        return (sum(1 for x in xs if x["ok"]) / len(xs)) if xs else float("nan")
    total = rate(rows)
    by = collections.defaultdict(list)
    for r in rows:
        by[r["task"]].append(r)
    typed = [(rate(v), k) for k, v in by.items() if len(v) >= 5]
    worst_type = min(typed) if typed else (float("nan"), "(too few per type)")
    examples = [r for r in rows if any(w in r["request"].lower() for w in EXAMPLE_REQUESTS) and r["task"] in
                ("tint_masked", "uv_island_per_part", "user_example")]
    harmful = sum(1 for r in rows if r.get("harmful")) / max(1, len(rows))
    none = [r for r in rows if r["task"] in ("cant_do", "cant_do2", "vague", "unknown_part", "no_head", "help", "vague2")
            or r["task"].startswith("cant3")]
    secs = sorted(r["seconds"] for r in rows if "seconds" in r)
    p50 = secs[len(secs) // 2] if secs else float("nan")
    p95 = secs[int(len(secs) * 0.95)] if secs else float("nan")
    return {"task_success": total, "worst_type": worst_type, "examples": rate(examples), "examples_n": len(examples),
            "harmful": harmful, "not_acting": rate(none), "not_acting_n": len(none), "latency_p50": p50,
            "latency_p95": p95, "by_type": {k: (rate(v), len(v)) for k, v in sorted(by.items())}}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", required=True)
    ap.add_argument("--name", required=True)
    ap.add_argument("--test", default=os.path.join(common.DATA, "test.jsonl"))
    ap.add_argument("--user-requests", default=os.path.join(common.DATA, "user_requests.jsonl"))
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--workers", type=int, default=1)
    ap.add_argument("--threads", type=int, default=0, help="CPU threads per worker (default: the cores shared out)")
    ap.add_argument("--seed", type=int, default=3)
    a = ap.parse_args()
    items = list(common.jsonl_read(a.test))
    rng = random.Random(a.seed)
    rng.shuffle(items)
    if a.limit:
        # keep every task type, then fill
        by = collections.defaultdict(list)
        for it in items:
            by[it["task"]].append(it)
        picked = []
        while len(picked) < a.limit and any(by.values()):
            for k in sorted(by):
                if by[k] and len(picked) < a.limit:
                    picked.append(by[k].pop())
        items = picked
    if os.path.exists(a.user_requests):
        items += list(common.jsonl_read(a.user_requests))
    # test files made before checks.py took masks_optional: a noise breakup on wear and dirt is fine
    for it in items:
        if it["task"].startswith("recipe_"):
            for spec in it["check"]:
                e = spec.get("expect", {})
                if "masks" in e and "noise" not in e["masks"]:
                    e.setdefault("masks_optional", ["noise"])
    os.environ["PATINA_ASSISTANT_THREADS"] = str(a.threads or max(1, (os.cpu_count() or 8) // 2 // a.workers))
    t0 = time.time()
    pool = mp.get_context("fork").Pool(a.workers, initializer=_init, initargs=(os.path.expanduser(a.model),))
    rows = []
    try:
        for i, r in enumerate(pool.imap_unordered(run_one, items)):
            rows.append(r)
            if (i + 1) % 25 == 0:
                ok = sum(x["ok"] for x in rows) / len(rows)
                print("  %d / %d  %.1f%% so far  (%.0fs)" % (i + 1, len(items), ok * 100, time.time() - t0), flush=True)
    finally:
        pool.close()
        pool.join()
    T = table(rows)
    os.makedirs(common.REPORTS, exist_ok=True)
    path = os.path.join(common.REPORTS, a.name)
    json.dump({"model": a.model, "rows": rows, "table": T}, open(path + ".json", "w"), indent=1)
    def pct(x):
        return "n/a" if x != x else "%.1f%%" % (100 * x)
    lines = ["# Assistant evaluation: %s" % a.name, "", "Model: `%s`, %d requests, %s." % (a.model, len(rows),
             time.strftime("%Y-%m-%d %H:%M")), "",
             "| Measure | Result | Bar |", "| --- | --- | --- |",
             "| Task success | %s overall, worst type %s (%s) | >= 85%%, >= 70%% each |" % (pct(T["task_success"]), T["worst_type"][1], pct(T["worst_type"][0])),
             "| The two example requests (%d) | %s | >= 95%% |" % (T["examples_n"], pct(T["examples"])),
             "| Harmful plans | %s | <= 1%% |" % pct(T["harmful"]),
             "| Knowing when not to act (%d) | %s | >= 90%% |" % (T["not_acting_n"], pct(T["not_acting"])),
             "| Latency (this machine) | median %.2f s, 95th percentile %.2f s | < 1 s GPU / < 10 s CPU |" % (T["latency_p50"], T["latency_p95"]),
             "", "## By task type", "", "| Task | Success | Requests |", "| --- | --- | --- |"]
    for k, (r, n) in T["by_type"].items():
        lines.append("| %s | %s | %d |" % (k, pct(r), n))
    lines += ["", "## The 20 worst failures", ""]
    fails = [r for r in rows if not r["ok"]]
    for r in fails[:20]:
        lines += ["- **%s** (%s, %s): \"%s\"" % (r["task"], r["scene"], r.get("variant", ""), r["request"]),
                  "  - plan: `%s`" % json.dumps(r.get("plan"))[:400],
                  "  - expected: `%s`" % json.dumps(r.get("expected"))[:300],
                  "  - checker: %s" % "; ".join(r.get("why", []))[:300]]
    open(path + ".md", "w").write("\n".join(lines) + "\n")
    print("\n".join(lines[:14]))
    print("report: %s.md" % path)


if __name__ == "__main__":
    main()
