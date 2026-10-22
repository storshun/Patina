"""Assembles the training data: every request (canonical and paraphrased) with the project
description Patina gives for it, in chat format, split so the test set measures generalisation:
whole scenes and whole task variants are held out, and the rest is split by canonical example so
no paraphrase of a test request is trained on.

    python3 tools/assistant/build_dataset.py
reads ~/patina-ml/data/paraphrases.jsonl; writes train.jsonl, val.jsonl, test.jsonl (with
check specs, for the evaluation) and stats.json in ~/patina-ml/data/
"""
import argparse
import collections
import hashlib
import json
import os
import random
import sys

sys.path.insert(0, os.path.dirname(__file__))
import common  # noqa: E402


def frac(key, salt):
    """A stable number in 0..1 for a key (the same split every run)."""
    return int(hashlib.sha1((salt + key).encode()).hexdigest()[:8], 16) / 0xFFFFFFFF


def _sorted(v):
    if isinstance(v, dict):
        return {k: _sorted(v[k]) for k in sorted(v)}
    if isinstance(v, list):
        return [_sorted(x) for x in v]
    return v


def canonical_plan(plan):
    """The one way a plan is written, which Patina's grammar follows: say or ask, then steps, then suggest;
    each step cmd then args; argument keys (and keys inside them) in alphabetical order."""
    out = {}
    for k in ("say", "ask"):
        if k in plan:
            out[k] = plan[k]
    if plan.get("steps"):
        out["steps"] = [{"cmd": st["cmd"], "args": _sorted(st.get("args", {}))} for st in plan["steps"]]
    if plan.get("suggest"):
        out["suggest"] = list(plan["suggest"])
    return out


def plan_text(plan):
    return json.dumps(canonical_plan(plan), ensure_ascii=False, separators=(",", ":"))


def user_text(context, request):
    return context.rstrip() + "\nRequest: " + request


def plan_materials(plan):
    out = set()
    for st in plan.get("steps", []):
        a = st.get("args", {})
        if st["cmd"] in ("add_material_layer",) and isinstance(a.get("material"), str):
            out.add(a["material"])
        if st["cmd"] == "set_paint_material" and a.get("id"):
            out.add(a["id"])
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--inp", default=os.path.join(common.DATA, "paraphrases.jsonl"))
    ap.add_argument("--max-per-example", type=int, default=7)
    ap.add_argument("--held-variant-share", type=float, default=0.08)
    ap.add_argument("--val-share", type=float, default=0.03)
    a = ap.parse_args()
    rows = list(common.jsonl_read(a.inp))
    rng = random.Random(5)
    # contexts: one scene state per (scene, setup); a context per request text (the material
    # shortlist depends on the words used)
    groups = collections.defaultdict(list)
    for i, r in enumerate(rows):
        groups[(r["scene"], json.dumps(r["setup"], sort_keys=True), json.dumps(r.get("pre", [])))].append(i)
    metas = {m["id"]: m for m in __import__("scenes").load_scenes()}
    # Held-out variants: a share of each task's variants, but never all of a task's variants, so
    # every task type is trained on and the test asks for a variant it wasn't shown.
    variants = collections.defaultdict(set)
    for r in rows:
        variants[r["task"]].add(r["variant"])
    held_variants = set()
    for task, vs in variants.items():
        ranked = sorted(vs, key=lambda v: frac(task + "/" + v, "variant"))
        k = sum(1 for v in ranked if frac(task + "/" + v, "variant") < a.held_variant_share)
        for v in ranked[:min(k, len(ranked) - 1)]:
            held_variants.add(task + "/" + v)
    system = None
    out = {"train": [], "val": [], "test": []}
    stats = collections.Counter()
    with common.Server() as s:
        for (scene, setup_key, pre_key), idxs in groups.items():
            s("open_file", path=metas[scene]["path"])
            for cmd, args in json.loads(pre_key):
                s(cmd, **args)
            for st in json.loads(setup_key):
                s("assistant_apply", plan=st["plan"], request=st["request"])
            for i in idxs:
                r = rows[i]
                held_variant = (r["task"] + "/" + r["variant"]) in held_variants
                if r["held_out_scene"] or held_variant:
                    split = "test"
                elif frac(r["scene"] + r["request"] + setup_key, "val") < a.val_share:
                    split = "val"
                else:
                    split = "train"
                variants = list(r["requests"])
                canonical, rest = variants[0], variants[1:]
                rng.shuffle(rest)
                # test: the paraphrases (how people type), plus the canonical sentence
                variants = [canonical] + rest[:a.max_per_example - 1]
                need = plan_materials(r["plan"])
                for text in variants:
                    ctx = s("assistant_context", request=text)
                    system = system or ctx["system"]
                    if need - set(ctx["materials"]):
                        stats["dropped: material not listed"] += 1
                        continue
                    item = {"messages": [{"role": "system", "content": ctx["system"]},
                                         {"role": "user", "content": user_text(ctx["text"], text)},
                                         {"role": "assistant", "content": plan_text(r["plan"])}],
                            "task": r["task"], "family": r["family"], "variant": r["variant"], "scene": r["scene"],
                            "canonical": text == canonical}
                    if split == "test":
                        item.update({"request": text, "plan": r["plan"], "check": r["check"], "setup": r["setup"],
                                     "pre": r.get("pre", [])})
                    out[split].append(item)
                    stats[split] += 1
    for k, items in out.items():
        rng.shuffle(items)
        common.jsonl_write(os.path.join(common.DATA, k + ".jsonl"), items)
    fam = collections.Counter(i["family"] for i in out["train"])
    info = {"counts": dict(stats), "train_by_family": dict(fam),
            "test_tasks": dict(collections.Counter(i["task"] for i in out["test"])),
            "held_out_variants": sorted({i["task"] + "/" + i["variant"] for i in out["test"]
                                         if not metas[i["scene"]]["held_out"]}),
            "system_prompt": system}
    json.dump(info, open(os.path.join(common.DATA, "stats.json"), "w"), indent=1)
    print(json.dumps(info["counts"]), "\ntrain by family:", dict(fam))


if __name__ == "__main__":
    main()
