#!/usr/bin/env python3
"""More procedural materials and masks (0.24) against a real headless Patina: list_generators describes all 74
generators and their settings; create_material keeps warp, grime and worn edges; the 16 new generators and
the 52 new library materials are there; mask stacks take Grow, Edges, Pattern and the new noise kinds, and a
pattern naming a material the library hasn't got is refused.

    PATINA_BIN=build/patina python3 tests/api/test_procedural.py
"""
import os
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from test_retopo_zones import check, failures, free_port, common, BIN  # noqa: E402

NEW = ["stripes", "checks", "dots", "animal_print", "cracked_earth", "flagstone", "pebbles", "strata", "crackle",
       "chainmail", "damascus", "circuit", "stained_glass", "mosaic", "sequins", "paper"]


def main():
    tmp = tempfile.mkdtemp(prefix="patina_procedural_")
    env = dict(os.environ, HOME=tmp, XDG_DATA_HOME=tmp + "/data", XDG_CONFIG_HOME=tmp + "/config", XDG_CACHE_HOME=tmp + "/cache")
    port = free_port()
    p = subprocess.Popen([BIN, "--headless", "--port", str(port)], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(150):
            try:
                c = common.Client(port, timeout=300)
                c.call("ping")
                break
            except Exception:
                time.sleep(0.2)
        else:
            raise SystemExit("patina did not start")
        gens = c.call("list_generators")
        ids = [g["id"] for g in gens]
        check(len(gens) == 74 and all(n in ids for n in NEW), "list_generators: 74, the 16 from 0.24 among them")
        st = c.call("list_generators", {"generator": "stripes"})[0]
        check(st["extra"][1]["choices"][:3] == ["Plain", "Pinstripe", "Chevron"] and st["colors"]["b"] == "Stripes",
              "a generator's styles and colors are described")
        br = c.call("list_generators", {"generator": "bricks"})[0]
        check(abs(br["extra"][0]["zero_means"] - 0.1) < 1e-6, "and what 0 stands for (joint width 0.1)")
        try:
            c.call("list_generators", {"generator": "nope"})
            check(False, "an unknown generator is refused")
        except common.CommandFailed:
            check(True, "an unknown generator is refused")
        lib = {m["id"]: m for m in c.call("list_materials")}
        check(len(lib) >= 225, "the library has the new materials (%d)" % len(lib))
        for mid in ["patina_houndstooth", "patina_lava", "patina_chainmail_steel", "patina_circuit_glow", "patina_old_parchment",
                    "patina_pool_mosaic", "patina_sequins_gold", "patina_bricks_grimy"]:
            check(mid in lib, "library: " + mid)
        made = c.call("create_material", {"name": "Grimy Flags", "type": "procedural", "generator": "flagstone",
                                          "params": {"grime": 0.5, "grime_color": "#203040", "wear": 0.3, "warp": 0.2, "warp_scale": 5}})
        mid = made.get("id") if isinstance(made, dict) else None
        got = c.call("get_material", {"id": mid or "user_grimy_flags"})
        pr = got.get("params", got.get("definition", {}).get("params", {}))
        check(abs(pr.get("grime", 0) - 0.5) < 1e-6 and abs(pr.get("wear", 0) - 0.3) < 1e-6 and abs(pr.get("warp_scale", 0) - 5) < 1e-6,
              "create_material keeps grime, wear and warp (%s)" % {k: pr.get(k) for k in ("grime", "wear", "warp", "warp_scale")})
        # masks
        c.call("new_project", {"primitive": "sphere", "resolution": 256})
        c.call("add_layer", {"type": "fill", "material": {"id": "", "color": [1, 0, 0]}})
        for fx in [{"type": "grow", "amount": -2}, {"type": "edges", "radius": 3},
                   {"type": "pattern", "material": {"id": "patina_bricks_red"}, "source": "brightness"},
                   {"type": "noise", "noise_type": "cracks"}]:
            stack = c.call("add_mask_effect", {"effect": fx})
            top = stack[-1]
            check(top["type"] == fx["type"], "add_mask_effect: " + fx["type"])
        check(stack[-2]["material"]["id"] == "patina_bricks_red" and stack[-2]["source"] == "brightness" and stack[-1]["noise_type"] == "cracks",
              "their settings come back")
        try:
            c.call("add_material_layer", {"material": "patina_rust_heavy", "mask": [{"type": "pattern", "material": "no_such"}]})
            check(False, "a pattern mask with a material the library hasn't got is refused")
        except common.CommandFailed as e:
            check("no_such" in str(e), "a pattern mask with a material the library hasn't got is refused")
        r = c.call("add_material_layer", {"material": "patina_rust_heavy", "mask": [{"type": "drips"}, {"type": "gradient", "direction": [0, 0, 1]}]})
        fx = c.call("list_layers")[-1].get("mask_effects", [])
        check([e["generator"]["type"] for e in fx] == ["drips", "gradient"], "add_material_layer: drips and gradient smart masks")
    finally:
        p.terminate()
        try:
            p.wait(20)
        except Exception:
            p.kill()
        shutil.rmtree(tmp, ignore_errors=True)
    print("procedural: %d failed" % len(failures) if failures else "procedural: all checks passed")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
