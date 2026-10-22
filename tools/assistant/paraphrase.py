"""Rewrites each canonical request the way people actually type it (terse, chatty, artist jargon,
typos, Substance Painter words) with a local instruct model behind an OpenAI-compatible server
(llama-server, vLLM). Rewrites that change the meaning in a way we can detect are dropped: another
amount class ("slightly" vs "strongly"), another colour, a part that isn't named any more, a
number that changed. Results are cached by request, so a rerun only asks for what's new.

    python3 tools/assistant/paraphrase.py --url http://localhost:8091 --per 8 --parallel 2
reads ~/patina-ml/data/canonical.jsonl, writes ~/patina-ml/data/paraphrases.jsonl
"""
import argparse
import concurrent.futures as cf
import json
import os
import random
import re
import sys
import time
import urllib.request

sys.path.insert(0, os.path.dirname(__file__))
import common  # noqa: E402
import tasks  # noqa: E402
import vocab as V  # noqa: E402

CACHE = os.path.join(common.DATA, "paraphrase-cache.jsonl")
JUDGE_CACHE = os.path.join(common.DATA, "judge-cache.jsonl")

STYLES = [
    "terse, lower case, like a quick note (\"skin creases redder, subtle\")",
    "chatty and polite, a full sentence or two",
    "a 3D artist using Substance Painter words (fill layer, generator, smart mask, AO, curvature, UV shells)",
    "with one or two typos or missing letters, typed fast",
    "a beginner who doesn't know the technical words",
    "a game artist in a hurry, imperative",
    "as a question (\"can you ...?\")",
    "British spelling, matter of fact",
]

PROMPT = """You rewrite requests that a user types into the assistant of a 3D texture painting program.
Rewrite the request below {n} different ways, the way different people would type it. Use these styles, one each:
{styles}

Rules:
- Keep the meaning exactly: the same part of the model, the same colour, the same material, the same place (creases, edges, top surfaces...), the same numbers.
- {amount_rule}
- Don't add anything the request doesn't ask for, and don't drop anything it asks for.
- Each rewrite on its own line, no numbering, no quotes, nothing else.

Request: {request}"""


def amount_rule(text):
    classes = V.amount_classes_in(text)
    if not classes:
        return "Don't add words for how much (slightly, a bit, strongly...)."
    k = sorted(classes)[0]
    return "Say how much with one of these words only: %s." % ", ".join(V.AMOUNTS[k][1])


def ask_llm(url, model, text, n, rng, temperature=0.9, timeout=120):
    styles = rng.sample(STYLES, min(n, len(STYLES)))
    while len(styles) < n:
        styles.append(rng.choice(STYLES))
    prompt = PROMPT.format(n=n, styles="\n".join("- " + s for s in styles), amount_rule=amount_rule(text), request=text)
    body = {"model": model, "messages": [{"role": "user", "content": prompt}], "temperature": temperature,
            "top_p": 0.95, "max_tokens": 900, "chat_template_kwargs": {"enable_thinking": False}}
    req = urllib.request.Request(url.rstrip("/") + "/v1/chat/completions", data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        out = json.loads(r.read())
    content = out["choices"][0]["message"]["content"]
    content = re.sub(r"<think>.*?</think>", "", content, flags=re.S)
    lines = []
    for line in content.splitlines():
        line = re.sub(r"^\s*(\d+[.)]|[-*•])\s*", "", line).strip().strip('"').strip()
        if 3 <= len(line) <= 300 and not line.lower().startswith(("request:", "here are", "rewrite")):
            lines.append(line)
    return lines


def _part_words(row):
    """Words that name the parts the plan uses (one of them must stay in a rewrite)."""
    ids = set()
    for st in row["plan"].get("steps", []):
        a = st.get("args", {})
        r = a.get("region")
        if isinstance(r, dict):
            ids.update([r.get("part")] if r.get("part") else [])
            ids.update(r.get("parts", []))
        ids.update(a.get("parts", []))
        if st["cmd"] == "select_part":
            ids.add(a.get("part"))
    groups = []
    for pid in ids:
        if not pid:
            continue
        name = tasks.part_name(pid)
        words = [w for w in re.findall(r"[a-z]+", name.lower()) if w not in ("the", "slot", "left", "right", "of")]
        if words:
            groups.append(words)
    return groups


LEAKS = ("style", "plain and simple", "game artist", "typo", "beginner", "matter of fact", "british", "substance painter",
         "in a hurry", "rewrite", "lower case", "misspell", "don't know the words", "i don't know how")
# words a rewrite may only use when the request is about them (the jargon style tended to add steps)
JARGON = ("generator", "smart mask", "uv shell", "curvature", " ao", "ambient occlusion", "noise", "mask")

JUDGE = """A user of a 3D texture painting program typed the ORIGINAL request. Below are rewrites of it.
A rewrite is good only if it asks for exactly the same change: the same part of the model, the same colour or material, the same place (creases, edges, top surfaces, near the ground...), roughly the same amount, nothing added and nothing left out. Wording, spelling, politeness and style don't matter.

ORIGINAL: {original}
{rewrites}
Answer with the numbers of the good rewrites, separated by commas, or "none". Nothing else."""


def judge(url, model, original, lines, timeout=120):
    rewrites = "\n".join("%d. %s" % (i + 1, l) for i, l in enumerate(lines))
    body = {"model": model, "messages": [{"role": "user", "content": JUDGE.format(original=original, rewrites=rewrites)}],
            "temperature": 0, "max_tokens": 60, "chat_template_kwargs": {"enable_thinking": False}}
    req = urllib.request.Request(url.rstrip("/") + "/v1/chat/completions", data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        content = json.loads(r.read())["choices"][0]["message"]["content"]
    content = re.sub(r"<think>.*?</think>", "", content, flags=re.S)
    return sorted({int(x) - 1 for x in re.findall(r"\d+", content) if 0 < int(x) <= len(lines)})


def _meaning_words():
    """Words that change what a request does: places on the model, materials, colours, parts,
    layer and mask terms."""
    words = set()
    for phrases in V.MASK_PHRASES.values():
        for ph in phrases:
            words.update(re.findall(r"[a-z]+", ph))
    for w in list(V.MATERIAL_WORDS) + list(V.SMART_WORDS) + list(V.COLOURS):
        words.update(re.findall(r"[a-z]+", w))
    for ws in V.FAMILY_WORDS.values():
        words.update(ws)
    words.update("head face body skin arm arms leg legs hand hands foot feet eye eyes hair neck torso chest back "
                 "boots boot shirt jacket pants jeans gloves hat helmet visor ear ears teeth tail wings cape "
                 "layer layers folder mask masks fill paint brush edges edge crease creases cavity cavities top "
                 "bottom ground highlight highlights shadow shadows island islands seam seams uv uvs".split())
    for w in ("the", "a", "on", "in", "of", "to", "it", "is", "and", "with", "where", "would", "towards", "near",
              "under", "that", "from", "up", "light", "doesn", "t", "s", "more", "less", "make", "put", "add"):
        words.discard(w)
    return words


MEANING_WORDS = _meaning_words()


def _stem(w):
    for suf in ("ing", "es", "s", "ed"):
        if w.endswith(suf) and len(w) > len(suf) + 2:
            return w[: -len(suf)]
    return w


def keeps_meaning(row, text):
    t = text.lower()
    if any(w in t and w not in row["request"].lower() for w in LEAKS):
        return False, "style leak"
    if any(w in t and w not in row["request"].lower() for w in JARGON):
        return False, "added jargon"
    have = {_stem(w) for w in re.findall(r"[a-z]+", row["request"].lower())}
    for st in row["plan"].get("steps", []):  # what the command itself is called is fine
        c = st["cmd"]
        if c in ("add_tint_layer", "add_material_layer", "apply_smart_material", "update_layer", "update_layer_mask",
                 "add_layer", "duplicate_layer", "remove_layer", "move_layer", "group_into_folder"):
            have |= {"fill", "layer", "tint", "folder"}
        if c.startswith("uv_") or c == "select_part":
            have |= {"uv", "island", "seam", "shell"}
        if c in ("set_brush", "set_paint_material", "set_tool"):
            have |= {"brush", "paint"}
        if st.get("args", {}).get("mask"):
            have |= {"mask"}
    for w in re.findall(r"[a-z]+", t):
        if w in MEANING_WORDS and _stem(w) not in have and w not in have:
            return False, "added meaning"
    canon = row["request"]
    a, b = V.amount_classes_in(canon), V.amount_classes_in(text)
    if a != b:
        return False, "amount"
    ca, cb = V.colour_families_in(canon), V.colour_families_in(text)
    if ca and not (ca & cb):
        return False, "colour"
    for words in _part_words(row):
        if not any(w in t or (w.endswith("s") and w[:-1] in t) for w in words):
            return False, "part"
    for num in re.findall(r"\d+", canon):
        if num not in text:
            return False, "number"
    for st in row["plan"].get("steps", []):  # the words of a text stencil stay exactly as typed
        if st["cmd"] == "set_stencil" and st.get("args", {}).get("text") and st["args"]["text"] not in text:
            return False, "stencil text"
    if re.search(r"\b(left|right)\b", canon.lower()):
        for side in ("left", "right"):
            if (side in canon.lower()) != (side in t):
                return False, "side"
    return True, ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", default="http://localhost:8091")
    ap.add_argument("--model", default="schala")
    ap.add_argument("--per", type=int, default=8)
    ap.add_argument("--parallel", type=int, default=2)
    ap.add_argument("--limit", type=int, default=0, help="only this many canonical requests (a pilot)")
    ap.add_argument("--judge", action="store_true", help="ask the model which rewrites mean the same (cached)")
    ap.add_argument("--inp", default=os.path.join(common.DATA, "canonical.jsonl"))
    ap.add_argument("--out", default=os.path.join(common.DATA, "paraphrases.jsonl"))
    a = ap.parse_args()
    rows = list(common.jsonl_read(a.inp))
    cache = {}
    if os.path.exists(CACHE):
        for c in common.jsonl_read(CACHE):
            cache[c["request"]] = c["lines"]
    todo = sorted({r["request"] for r in rows if r["request"] not in cache})
    if a.limit:
        todo = todo[:a.limit]
    print("%d canonical requests, %d cached, %d to ask" % (len({r['request'] for r in rows}), len(cache), len(todo)), flush=True)
    t0 = time.time()
    rng = random.Random(7)
    with open(CACHE, "a") as cf_out, cf.ThreadPoolExecutor(a.parallel) as pool:
        futs = {pool.submit(ask_llm, a.url, a.model, text, a.per, random.Random(rng.random())): text for text in todo}
        for i, fut in enumerate(cf.as_completed(futs)):
            text = futs[fut]
            try:
                lines = fut.result()
            except Exception as e:  # noqa: BLE001
                print("  failed: %s (%s)" % (text, e), flush=True)
                continue
            cache[text] = lines
            cf_out.write(json.dumps({"request": text, "lines": lines}) + "\n")
            cf_out.flush()
            if (i + 1) % 25 == 0:
                rate = (i + 1) / (time.time() - t0)
                print("  %d / %d  (%.1f per minute, %.0f min left)" % (i + 1, len(todo), rate * 60,
                                                                      (len(todo) - i - 1) / rate / 60), flush=True)
    # filters, then the judge (cached by request)
    kept = {}
    dropped = {}
    for r in rows:
        lines = cache.get(r["request"])
        if lines is None or r["request"] in kept:
            continue
        seen = {r["request"].lower()}
        good = []
        for line in lines:
            ok, why = keeps_meaning(r, line)
            if not ok:
                dropped[why] = dropped.get(why, 0) + 1
                continue
            if line.lower() in seen:
                continue
            seen.add(line.lower())
            good.append(line)
        kept[r["request"]] = good
    if a.judge:
        jcache = {}
        if os.path.exists(JUDGE_CACHE):
            for c in common.jsonl_read(JUDGE_CACHE):
                jcache[(c["request"], tuple(c["lines"]))] = c["good"]
        todo = [(k, v) for k, v in kept.items() if v and (k, tuple(v)) not in jcache]
        print("judging %d requests (%d cached)" % (len(todo), len(kept) - len(todo)), flush=True)
        t1 = time.time()
        with open(JUDGE_CACHE, "a") as jf, cf.ThreadPoolExecutor(a.parallel) as pool:
            futs = {pool.submit(judge, a.url, a.model, k, v): (k, v) for k, v in todo}
            for i, fut in enumerate(cf.as_completed(futs)):
                k, v = futs[fut]
                try:
                    good = fut.result()
                except Exception as e:  # noqa: BLE001
                    print("  judge failed: %s (%s)" % (k, e), flush=True)
                    continue
                jcache[(k, tuple(v))] = good
                jf.write(json.dumps({"request": k, "lines": v, "good": good}) + "\n")
                jf.flush()
                if (i + 1) % 100 == 0:
                    print("  judged %d / %d (%.0f min left)" % (i + 1, len(todo), (len(todo) - i - 1) / ((i + 1) / (time.time() - t1)) / 60), flush=True)
        for k, v in kept.items():
            g = jcache.get((k, tuple(v)))
            if g is not None:
                dropped["judge"] = dropped.get("judge", 0) + len(v) - len(g)
                kept[k] = [v[i] for i in g]
    out = []
    for r in rows:
        if r["request"] in kept:
            out.append(dict(r, requests=[r["request"]] + kept[r["request"]]))
    common.jsonl_write(a.out, out)
    n = sum(len(r["requests"]) for r in out)
    print("%d examples with %d requests (%.1f each); dropped %s" % (len(out), n, n / max(1, len(out)), dropped))


if __name__ == "__main__":
    main()
