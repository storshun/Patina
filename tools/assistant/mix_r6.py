"""Round 6's data (Patina 0.12): round 5's examples, rehearsed, plus round 6's new request types.
Round 6 answers what Patina can't do with suggestions, so round 5's can't-do and vague examples
(answered without them, and some now done: the resolution, texture sets, export) are left out;
round 6 has its own.

    python3 tools/assistant/mix_r6.py combine      paraphrases-r5mix + r6/paraphrases-r6 -> paraphrases-r6mix
    python3 tools/assistant/build_dataset.py --inp ~/patina-ml/data/paraphrases-r6mix.jsonl --max-per-example 5
    (python3 tools/assistant/mix_r6.py thin 0.5    keeps a share of the older types in train.jsonl, if wanted)
"""
import os
import random
import sys

sys.path.insert(0, os.path.dirname(__file__))
import common  # noqa: E402

DROPPED_OLD = {"cant_do", "cant_do2", "vague"}
NEW_PREFIXES = ("texset_", "density_", "resolution", "layout_", "glow", "adjust_metal", "select_", "window_", "text_stencil",
                "question_", "cant3_", "help", "vague2", "paint_art")


def is_new(task):
    return task.startswith(NEW_PREFIXES)


def combine(old_share=0.15, seed=17):
    """The older examples are sampled here (every older task keeps some), so only those get their
    contexts built."""
    rng = random.Random(seed)
    old = [r for r in common.jsonl_read(os.path.join(common.DATA, "paraphrases-r5mix.jsonl")) if r["task"] not in DROPPED_OLD]
    by = {}
    for r in old:
        by.setdefault(r["task"], []).append(r)
    kept = []
    for task, rows in sorted(by.items()):
        rng.shuffle(rows)
        kept += rows[:max(6, int(round(len(rows) * old_share)))]
    new = list(common.jsonl_read(os.path.join(common.DATA, "r6", "paraphrases-r6.jsonl")))
    assert not any(is_new(r["task"]) for r in old), "an older task looks new"
    common.jsonl_write(os.path.join(common.DATA, "paraphrases-r6mix.jsonl"), kept + new)
    print("%d older examples (of %d) + %d new" % (len(kept), len(old), len(new)))


def thin(share=0.12, seed=13):
    path = os.path.join(common.DATA, "train.jsonl")
    rows = list(common.jsonl_read(path))
    rng = random.Random(seed)
    keep = [r for r in rows if is_new(r["task"]) or rng.random() < share]
    rng.shuffle(keep)
    common.jsonl_write(path, keep)
    n_new = sum(1 for r in keep if is_new(r["task"]))
    print("train: %d (%d new types, %d older kept of %d)" % (len(keep), n_new, len(keep) - n_new,
                                                              sum(1 for r in rows if not is_new(r["task"]))))




FOCUS_OLD = ("adjust_", "followup", "clean", "auto_texture", "unknown_part", "brush_tweak", "except_part", "material_whole",
             "no_head")


def combine7(old_share=0.35, focus_share=0.8, r6_share=0.5, seed=19):
    """Round 7, from round 6's adapter: round 7's new examples, half of round 6's, and the older
    examples, most of all the kinds round 6 got worse at."""
    rng = random.Random(seed)
    old = [r for r in common.jsonl_read(os.path.join(common.DATA, "paraphrases-r5mix.jsonl")) if r["task"] not in DROPPED_OLD]
    by = {}
    for r in old:
        by.setdefault(r["task"], []).append(r)
    kept = []
    for task, rows in sorted(by.items()):
        rng.shuffle(rows)
        share = focus_share if task.startswith(FOCUS_OLD) else old_share
        kept += rows[:max(6, int(round(len(rows) * share)))]
    r6 = list(common.jsonl_read(os.path.join(common.DATA, "r6", "paraphrases-r6.jsonl")))
    rng.shuffle(r6)
    r6 = r6[:int(len(r6) * r6_share)]
    r7 = list(common.jsonl_read(os.path.join(common.DATA, "r6", "paraphrases-r7.jsonl")))
    common.jsonl_write(os.path.join(common.DATA, "paraphrases-r7mix.jsonl"), kept + r6 + r7)
    print("%d older + %d of round 6 + %d of round 7" % (len(kept), len(r6), len(r7)))


if __name__ == "__main__":
    arg = float(sys.argv[2]) if len(sys.argv) > 2 else None
    {"combine": lambda: combine(arg if arg is not None else 0.15), "thin": lambda: thin(arg if arg is not None else 0.12),
     "combine7": combine7}[sys.argv[1]]()
