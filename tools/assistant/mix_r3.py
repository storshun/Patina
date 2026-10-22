"""Round 3's data: round 2's examples plus the new request types, built into one train / val /
test split, with the older request types thinned out in training (the model starts from round 2's
adapter and has learned them; a share keeps them from being forgotten).

    python3 tools/assistant/mix_r3.py combine      paraphrases-r2 + paraphrases-r3all -> paraphrases-r3mix
    python3 tools/assistant/build_dataset.py --inp ~/patina-ml/data/paraphrases-r3mix.jsonl
    python3 tools/assistant/mix_r3.py thin         thins the older types in train.jsonl
"""
import json
import os
import random
import sys

sys.path.insert(0, os.path.dirname(__file__))
import common  # noqa: E402

NEW_PREFIXES = ("look_", "multi", "material_colour", "paint_setup", "layer_ref", "except_part", "selection_", "followup2_",
                "adjust_", "brush_tweak", "clean_", "cant_do2", "look2", "recipe_casual", "multi_for")


def is_new(task):
    return task.startswith(NEW_PREFIXES)


def short_masks(plan):
    """A mask at its default level and contrast is written as its type alone: shorter answers."""
    for st in plan.get("steps", []):
        a = st.get("args", {})
        if isinstance(a.get("mask"), list):
            a["mask"] = [{"type": m["type"]} if isinstance(m, dict) else m for m in a["mask"]]
    return plan


def combine():
    old = list(common.jsonl_read(os.path.join(common.DATA, "paraphrases-r2.jsonl")))
    new = list(common.jsonl_read(os.path.join(common.DATA, "paraphrases-r3all.jsonl")))
    for r in new:
        r["plan"] = short_masks(r["plan"])
    common.jsonl_write(os.path.join(common.DATA, "paraphrases-r3mix.jsonl"), old + new)
    print("%d older examples + %d new" % (len(old), len(new)))


def thin(share=0.4, seed=11):
    path = os.path.join(common.DATA, "train.jsonl")
    rows = list(common.jsonl_read(path))
    rng = random.Random(seed)
    keep = [r for r in rows if is_new(r["task"]) or rng.random() < share]
    rng.shuffle(keep)
    common.jsonl_write(path, keep)
    n_new = sum(1 for r in keep if is_new(r["task"]))
    print("train: %d (%d new types, %d older kept of %d)" % (len(keep), n_new, len(keep) - n_new,
                                                              sum(1 for r in rows if not is_new(r["task"]))))


if __name__ == "__main__":
    {"combine": combine, "thin": thin}[sys.argv[1]]()
