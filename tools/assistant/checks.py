"""Checkers for Assistant tasks, as data. A task's "check" is a list of specs; each looks at the
project after a plan ran (and before, where it needs to) through Patina's own queries. The same
specs score the canonical plans when the data is made and the model's plans in the evaluation,
so they accept any plan with the right effect, not only the canonical one.

Spec types:
  layer         a layer that matches `expect` exists: which = "new" (any layer the plan added),
                {"uid": n} or {"name": "Dirt"}
  layer_gone    {"uid": n} is no longer in the stack
  layer_absent  {"name": s} no layer has this name
  new_opacity   {"max_range": [lo, hi]} the strongest new layer's opacity is in the range
  layer_index   {"uid": n, "index": i}
  new_layers    {"min": a, "max": b} layers added
  history       {"delta": n} undo steps added (one request = one step)
  state         {"path": "brush.preset", "eq" | "in" | "approx"+"tol" | "range"}
  baked         {"maps": ["ao", ...]}
  uv_parts      {"parts": [{"faces": [...], "islands_max": n}], "overlaps_max": k}
  uv_changed    the UVs differ from before
  selection     {"faces": [...]} the selected faces are these
  no_change     nothing in the document changed
  plan          {"kind": "ask" | "say" | "act"} what kind of answer the plan is
"""
import math


def get_path(obj, path):
    for part in path.split("."):
        if isinstance(obj, list):
            obj = obj[int(part)]
        else:
            obj = obj.get(part)
        if obj is None:
            return None
    return obj


def _density(s, faces):
    return s("uv_density", faces=faces).get("faces_density", 0.0)


def _flat(specs):
    for spec in specs or ():
        yield spec
        if spec.get("t") == "any_of":
            for opt in spec["options"]:
                yield from _flat(opt)


def _dkey(faces):
    return "%d:%d:%d" % (len(faces), sum(faces), hash(tuple(faces)) & 0xFFFFFF)


def snapshot(s, specs=()):
    st = s("get_state")
    hist = s("history")
    uv = s("uv_info")
    out = {"state": st, "history": hist.get("position", 0), "uv": uv, "density": {}}
    for spec in _flat(specs):
        if spec.get("t") == "density_gain":  # the faces' texel density against the rest's
            rest = sorted(set(range(st["mesh"]["faces"])) - set(spec["faces"]))
            out["density"][_dkey(spec["faces"])] = (_density(s, spec["faces"]), _density(s, rest) if rest else 1.0)
    return out


def plan_kind(plan):
    if not isinstance(plan, dict):
        return "invalid"
    if plan.get("steps"):
        return "act"
    if plan.get("ask"):
        return "ask"
    return "say"


def _close(a, b, tol):
    return a is not None and b is not None and abs(float(a) - float(b)) <= tol


def _colour_dist(a, b):
    return math.sqrt(sum((float(x) - float(y)) ** 2 for x, y in zip(a, b)))


def _jaccard(a, b):
    a, b = set(a), set(b)
    if not a and not b:
        return 1.0
    return len(a & b) / max(1, len(a | b))


def _smart_types(layer):
    out = []
    for fx in layer.get("mask_effects", []) or []:
        if fx.get("type") == "generator":
            g = fx.get("generator", {})
            t = g.get("type")
            if t == "material":
                continue
            if t == "facing":
                d = g.get("direction", [0, 1, 0])
                t = "facing_up" if d[1] > 0.7 else "facing_down" if d[1] < -0.7 else "facing"
            if t == "gradient_y":
                t = "gradient_down" if g.get("invert") else "gradient_up"
            out.append(t)
    if not out:
        g = layer.get("generator", {})
        if g.get("type") not in (None, "none", "material"):
            out.append(g["type"])
    return out


_LAYERS_BY_UID = {}  # the stack after the plan, for folder regions (set by run_checks)


def layer_matches(s, layer, expect, why):
    """True when the layer meets every expectation; reasons for a mismatch go to `why`."""
    e = expect
    if "type" in e and layer.get("type") != e["type"]:
        why.append("type %s" % layer.get("type"))
        return False
    mat = layer.get("material", {}) or {}
    if "material" in e:
        ok = e["material"] if isinstance(e["material"], list) else [e["material"]]
        if mat.get("id") not in ok:
            why.append("material %s" % mat.get("id"))
            return False
    if "color" in e:
        c = (mat.get("flat") or {}).get("color")
        if not c or _colour_dist(c, e["color"]["rgb"]) > e["color"].get("tol", 0.3):
            why.append("colour %s" % c)
            return False
    if "emissive" in e:
        c = (mat.get("flat") or {}).get("emissive")
        if not c or _colour_dist(c, e["emissive"]["rgb"]) > e["emissive"].get("tol", 0.3):
            why.append("emissive %s" % c)
            return False
    if "tint" in e:
        c = mat.get("tint")
        if not c or _colour_dist(c, e["tint"]["rgb"]) > e["tint"].get("tol", 0.3):
            why.append("tint %s" % c)
            return False
    if "adjust" in e:
        adj = layer.get("adjust") or {}
        for k, (lo, hi) in e["adjust"].items():
            v = adj.get(k)
            if v is None or not (lo - 1e-3 <= v <= hi + 1e-3):
                why.append("adjust %s %s" % (k, v))
                return False
    if "opacity" in e:
        lo, hi = e["opacity"]
        if not (lo - 1e-3 <= layer.get("opacity", 1.0) <= hi + 1e-3):
            why.append("opacity %.2f" % layer.get("opacity", 1.0))
            return False
    if "blend" in e and layer.get("blend") not in e["blend"]:
        why.append("blend %s" % layer.get("blend"))
        return False
    if "channel_mask" in e and layer.get("channel_mask") != e["channel_mask"]:
        why.append("channels %s" % layer.get("channel_mask"))
        return False
    if "visible" in e and layer.get("visible") != e["visible"]:
        why.append("visible %s" % layer.get("visible"))
        return False
    if "name_contains" in e and e["name_contains"].lower() not in layer.get("name", "").lower():
        why.append("name %r" % layer.get("name"))
        return False
    if "name_eq" in e and layer.get("name") != e["name_eq"]:
        why.append("name %r" % layer.get("name"))
        return False
    if "masks" in e:
        have = sorted(_smart_types(layer))
        want = sorted(e["masks"])
        # a breakup ("noise") on top of what was asked is a refinement, not another meaning
        optional = set(e.get("masks_optional", []))
        have_core = [m for m in have if m not in optional or m in want]
        if (have_core != want) if e.get("masks_exact", True) else not set(want) <= set(have):
            why.append("masks %s" % have)
            return False
    if "region" in e:
        r = s("layer_region_faces", index=layer["index"])
        # a layer in a folder shows only where the folder does (a smart material's region is on its folder)
        parent = _LAYERS_BY_UID.get(layer.get("parent") or 0)
        while r["all"] and parent is not None:
            r = s("layer_region_faces", index=parent["index"])
            parent = _LAYERS_BY_UID.get(parent.get("parent") or 0)
        if e["region"] == "all":
            if not r["all"]:
                why.append("has a region (%d faces)" % r["count"])
                return False
        else:
            if r["all"]:
                why.append("no region")
                return False
            j = _jaccard(r["face_indices"], e["region"])
            if j < e.get("min_jaccard", 0.95):
                why.append("region overlap %.2f" % j)
                return False
    if "is_folder" in e and (layer.get("type") == "folder") != e["is_folder"]:
        why.append("folder %s" % layer.get("type"))
        return False
    return True


def run_checks(s, specs, before, after, plan=None):
    """(ok, [messages]) for every spec."""
    msgs = []
    ok = True
    layers_before = {l["uid"]: l for l in before["state"]["layers"]}
    layers_after = after["state"]["layers"]
    by_uid = {l["uid"]: l for l in layers_after}
    _LAYERS_BY_UID.clear()
    _LAYERS_BY_UID.update(by_uid)
    new = [l for l in layers_after if l["uid"] not in layers_before]
    for si, spec in enumerate(specs):
        t = spec["t"]
        good = True
        why = []
        if t == "plan":
            good = plan_kind(plan) == spec["kind"]
            why.append("plan is %s" % plan_kind(plan))
        elif t == "layer":
            which = spec.get("which", "new")
            if which == "new":
                cands = new
            elif "uid" in which:
                cands = [by_uid[which["uid"]]] if which["uid"] in by_uid else []
            else:
                cands = [l for l in layers_after if l["name"] == which.get("name")]
            good = any(layer_matches(s, l, spec["expect"], why) for l in cands)
            if not cands:
                why.append("no such layer")
        elif t == "layer_any":  # a layer (new, or any) that meets one of several expectations
            cands = new if spec.get("which", "new") == "new" else layers_after
            good = any(layer_matches(s, l, e, why) for l in cands for e in spec["expects"])
            if not cands:
                why.append("no new layer")
        elif t == "layers_count":  # at least n new layers that meet the expectation
            n = sum(1 for l in new if layer_matches(s, l, spec["expect"], why))
            good = n >= spec["min"]
            why.append("%d matching new layers" % n)
        elif t == "layer_absent":
            good = not any(l["name"] == spec["name"] for l in layers_after)
            why.append("a layer %r is still there" % spec["name"])
        elif t == "new_opacity":
            ops = [l.get("opacity", 1.0) for l in new if l.get("type") != "folder"]
            lo, hi = spec["max_range"]
            good = bool(ops) and lo - 1e-3 <= max(ops) <= hi + 1e-3
            why.append("strongest new layer %s" % (max(ops) if ops else None))
        elif t == "layer_gone":
            good = spec["uid"] not in by_uid
        elif t == "layer_index":
            good = spec["uid"] in by_uid and by_uid[spec["uid"]]["index"] == spec["index"]
            why.append("index %s" % (by_uid[spec["uid"]]["index"] if spec["uid"] in by_uid else None))
        elif t == "new_layers":
            good = spec.get("min", 0) <= len(new) <= spec.get("max", 99)
            why.append("%d new" % len(new))
        elif t == "history":
            d = after["history"] - before["history"]
            good = d == spec["delta"]
            why.append("%d undo steps" % d)
        elif t == "state":
            v = get_path(after["state"], spec["path"])
            if "eq" in spec:
                good = v == spec["eq"]
            elif "in" in spec:
                good = v in spec["in"]
            elif "approx" in spec:
                good = _close(v, spec["approx"], spec.get("tol", 1e-3))
            elif "range" in spec:
                good = v is not None and spec["range"][0] - 1e-3 <= v <= spec["range"][1] + 1e-3
            elif "rgb" in spec:
                good = v is not None and _colour_dist(v, spec["rgb"]) <= spec.get("tol", 0.3)
            why.append("%s = %r" % (spec["path"], v))
        elif t == "baked":
            good = all(after["state"]["baked"].get(m) for m in spec["maps"])
            why.append(str(after["state"]["baked"]))
        elif t == "uv_parts":
            for p in spec["parts"]:
                n = s("uv_islands", faces=p["faces"], limit=100000)["islands"]
                if len(n) > p["islands_max"]:
                    good = False
                    why.append("%d islands for %d faces (at most %d)" % (len(n), len(p["faces"]), p["islands_max"]))
            ov = after["uv"].get("overlapping_islands", 0)
            if ov > spec.get("overlaps_max", 0):
                good = False
                why.append("%d overlapping islands" % ov)
        elif t == "uv_ref":
            for k in spec["fields"]:
                want, have = spec["values"][k], after["uv"].get(k)
                tol = 0 if k == "islands" else 0.03
                if have is None or abs(float(have) - float(want)) > tol + 1e-9:
                    good = False
                    why.append("%s %s (expected %s)" % (k, have, want))
        elif t == "uv_changed":
            a, b = before["uv"], after["uv"]
            good = any(a.get(k) != b.get(k) for k in ("islands", "coverage", "texel_density", "bounds", "seam_edges"))
        elif t == "selection":
            sel = [p for p in s("mesh_parts", faces=True)["parts"] if p["id"] == "selection"]
            have = sel[0]["face_indices"] if sel else []
            good = _jaccard(have, spec["faces"]) >= spec.get("min_jaccard", 0.99)
            why.append("%d selected" % len(have))
        elif t == "tile_faces":  # a texture set / UDIM tile holds these faces
            tiles = s("texture_tiles", faces=True)
            best = 0.0
            for tl in tiles.get("tiles", []):
                if "name" in spec and tl["name"].lower() != spec["name"].lower():
                    continue
                best = max(best, _jaccard(tl.get("face_indices", []), spec["faces"]))
            good = best >= spec.get("min_jaccard", 0.9) and len(tiles.get("tiles", [])) >= spec.get("min_tiles", 2)
            why.append("best overlap %.2f, %d tiles (%s)" % (best, len(tiles.get("tiles", [])), tiles.get("mode")))
        elif t == "tiles_mode":
            m = s("texture_tiles")["mode"]
            good = m == spec["mode"]
            why.append("mode %s" % m)
        elif t == "any_of":  # one of several sets of checks
            results = [run_checks(s, opt, before, after, plan) for opt in spec["options"]]
            good = any(r[0] for r in results)
            for r in results:
                why += r[1][:1]
        elif t == "density_gain":
            b = before.get("density", {}).get(_dkey(spec["faces"]))
            if b is None:
                good = False
                why.append("not measured before")
            else:
                pf, rf = _density(s, spec["faces"]), None
                rest = sorted(set(range(after["state"]["mesh"]["faces"])) - set(spec["faces"]))
                rf = _density(s, rest) if rest else 1.0
                g = (pf / max(rf, 1e-9)) / max(b[0] / max(b[1], 1e-9), 1e-9)
                good = (g >= spec["min"]) if "min" in spec else (g <= spec["max"])
                why.append("density gain %.2f" % g)
        elif t == "say_contains":
            text = (plan or {}).get("say", "") if isinstance(plan, dict) else ""
            good = any(w.lower() in text.lower() for w in spec["any"])
            why.append("said %r" % text[:80])
        elif t == "suggest":
            sug = (plan or {}).get("suggest", []) if isinstance(plan, dict) else []
            good = isinstance(sug, list) and spec.get("min", 1) <= len(sug) <= spec.get("max", 4)
            why.append("%d suggestions" % (len(sug) if isinstance(sug, list) else 0))
        elif t == "no_change":
            good = (after["history"] == before["history"] and
                    [l["uid"] for l in layers_after] == list(layers_before))
            why.append("history %d -> %d" % (before["history"], after["history"]))
        else:
            good = False
            why.append("unknown check %s" % t)
        if not good:
            ok = False
            msgs.append("%s: %s" % (t, "; ".join(why[-3:]) or "failed"))
    return ok, msgs
