#!/usr/bin/env python3
"""The MCP server (0.34) over stdio, as Claude and other agents use it: the instructions describe the whole
workflow; every post-processing tool is listed with its settings' ranges, defaults and effects; patina_guide and
the patina://guide resources give the guides (the parameters guide is written from the schemas); the prompts fill
in their arguments; tools run (texture_check, mesh_collision) and return their numbers.

    PATINA_BIN=build/patina python3 tests/api/test_mcp.py
"""
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import queue

HERE = os.path.dirname(os.path.abspath(__file__))
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
    tmp = tempfile.mkdtemp(prefix="patina_mcp_test_")
    env = dict(os.environ, HOME=os.path.join(tmp, "home"), XDG_DATA_HOME=os.path.join(tmp, "data"),
               XDG_CONFIG_HOME=os.path.join(tmp, "config"), XDG_CACHE_HOME=os.path.join(tmp, "cache"))
    os.makedirs(env["HOME"], exist_ok=True)
    proc = subprocess.Popen([exe, "--mcp", "--port", str(free_port())], env=env, stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, bufsize=1)
    lines = queue.Queue()
    threading.Thread(target=lambda: [lines.put(l) for l in proc.stdout], daemon=True).start()
    nid = [0]

    def rpc(method, params=None, timeout=120):
        nid[0] += 1
        proc.stdin.write(json.dumps({"jsonrpc": "2.0", "id": nid[0], "method": method, "params": params or {}}) + "\n")
        proc.stdin.flush()
        while True:
            msg = json.loads(lines.get(timeout=timeout))
            if msg.get("id") == nid[0]:
                return msg

    def call(name, args=None):
        r = rpc("tools/call", {"name": name, "arguments": args or {}})["result"]
        text = "".join(c.get("text", "") for c in r["content"] if c["type"] == "text")
        return r, text

    try:
        init = rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "test"}})["result"]
        caps = init["capabilities"]
        check("tools" in caps and "resources" in caps and "prompts" in caps, "tools, resources and prompts offered (%s)" % list(caps))
        ins = init["instructions"]
        check("the way you would a photo" in ins and "patina_guide" in ins and "texture_check" in ins, "the instructions describe the whole workflow")
        proc.stdin.write(json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized"}) + "\n")

        tools = {t["name"]: t for t in rpc("tools/list")["result"]["tools"]}
        want = ["mesh_check", "mesh_fix", "mesh_fix_revert", "mesh_transform", "mesh_reduce", "mesh_collision",
                "texture_check", "import_model_textures", "export_model", "send_model_to_blender", "recipe_list",
                "recipe_save", "recipe_apply", "batch_run", "batch_status", "batch_cancel", "blender_material_library",
                "add_layer", "update_layer", "render_view", "patina_guide", "layers_at", "language_list", "language_set",
                "language_new", "language_untranslated"]
        missing = [w for w in want if w not in tools]
        check(not missing, "every post-processing tool is listed (%d tools; missing %s)" % (len(tools), missing))
        ratio = tools["mesh_reduce"]["inputSchema"]["properties"]["ratio"]
        check(ratio.get("minimum") == 0.001 and ratio.get("maximum") == 1 and ratio.get("default") == 0.5,
              "settings carry their range and default (mesh_reduce ratio %s)" % {k: ratio.get(k) for k in ("minimum", "maximum", "default")})
        adj = tools["update_layer"]["inputSchema"]["properties"]["adjust"]["properties"]
        check(all(k in adj for k in ("exposure", "temperature", "tint", "highlights", "shadows", "vibrance", "delight")) and
              adj["exposure"]["default"] == 0 and "stops" in adj["exposure"]["description"],
              "the adjustment settings are spelled out, with what they do")
        conc = tools["mesh_collision"]["inputSchema"]["properties"]["concavity"]
        check("Lower" in conc["description"] and conc["default"] == 0.05, "the description says which way to move a setting")
        modes = tools["render_view"]["inputSchema"]["properties"]["mode"].get("enum", [])
        check("pbr_range" in modes and "mesh_check" in modes, "every view mode is listed (%d)" % len(modes))
        undocumented = [(n, k) for n, t in tools.items() for k, v in t["inputSchema"].get("properties", {}).items()
                        if not v.get("description")]
        check(not undocumented, "every setting of every tool has a description (%s)" % undocumented[:8])
        unranged = [(n, k) for n, t in tools.items() for k, v in t["inputSchema"].get("properties", {}).items()
                    if v.get("type") in ("number", "integer") and not ("minimum" in v and "maximum" in v)]
        check(not unranged, "every number setting of every tool has its range (%s)" % unranged[:8])
        bad = [n for n, t in tools.items() if not isinstance(t["inputSchema"].get("properties"), dict)]
        check(not bad, "every tool's schema is a valid object schema (%s)" % bad)

        # guides
        r, text = call("patina_guide", {"topic": "develop"})
        check("delight" in text and "texture_check" in text and not r["isError"], "patina_guide develop")
        res = rpc("resources/list")["result"]["resources"]
        uris = [x["uri"] for x in res]
        check("patina://guide/workflow" in uris and "patina://guide/parameters" in uris, "the guides are resources (%s)" % uris)
        params = rpc("resources/read", {"uri": "patina://guide/parameters"})["result"]["contents"][0]["text"]
        check("## mesh_collision" in params and "`concavity` (number, 0 to 1, default 0.05)" in params,
              "the parameters guide is written from the schemas")
        bad = rpc("resources/read", {"uri": "patina://guide/nothing"})
        check("error" in bad, "an unknown resource is an error")

        # prompts
        prompts = {p["name"]: p for p in rpc("prompts/list")["result"]["prompts"]}
        check({"finish_model", "develop_textures", "game_ready", "batch_folder"} <= set(prompts), "the prompts (%s)" % list(prompts))
        got = rpc("prompts/get", {"name": "finish_model", "arguments": {"path": "/tmp/x.glb", "use": "web"}})["result"]
        text = got["messages"][0]["content"]["text"]
        check("/tmp/x.glb" in text and "web" in text and "{{" not in text, "a prompt fills in its arguments")
        check("error" in rpc("prompts/get", {"name": "finish_model", "arguments": {}}), "a missing required argument is an error")

        # the tools run and return numbers
        call("new_project", {"primitive": "torus", "resolution": 256})
        r, text = call("mesh_collision", {"mode": "pieces", "max_shapes": 8})
        out = json.loads(text)
        check(not r["isError"] and out["count"] == 8 and 0.8 < out["fit"] < 1.3, "mesh_collision through MCP (%d shapes, fit %.2f)" % (out["count"], out["fit"]))
        call("add_layer", {"type": "fill", "material": {"id": "", "color": [0.05, 0.05, 0.05]}})
        r, text = call("texture_check")
        out = json.loads(text)
        check(not r["isError"] and out["pbr_ok"] is False and out["hints"], "texture_check through MCP gives hints (%s)" % out["hints"][:1])
        r, text = call("render_view", {"width": 64, "height": 64, "mode": "pbr_range"})
        check(any(c["type"] == "image" for c in r["content"]), "render_view returns an image")

        # (0.36) the layers that paint a spot: the dark fill covers the whole torus
        here = None
        for pt in ([0.5, 0.5], [0.5, 0.3], [0.3, 0.5], [0.5, 0.7], [0.7, 0.5], [0.4, 0.4]):
            r, text = call("layers_at", {"point": pt})
            if not r["isError"]:
                here = json.loads(text)
                break
        check(here is not None and here["layers"] and here["layers"][0]["type"] == "fill" and here["layers"][0]["amount"] > 0.99
              and here["layers"][0]["shown"], "layers_at lists the fill layer at full coverage (%s)" % (here and here["layers"]))
        r, text = call("layers_at", {"point": [0.001, 0.001]})
        check(r["isError"] and "not on the model" in text, "layers_at off the model is an error")
        # a paint layer shows only where it was painted (roughness and the like keep their coverage in green)
        if here:
            pt = None
            for cand in ([0.5, 0.5], [0.5, 0.3], [0.3, 0.5], [0.5, 0.7], [0.7, 0.5], [0.4, 0.4]):
                if not call("layers_at", {"point": cand})[0]["isError"]:
                    pt = cand if pt is None else pt
            far = None
            for cand in ([0.3, 0.5], [0.7, 0.5], [0.5, 0.3], [0.5, 0.7]):
                if cand != pt and not call("layers_at", {"point": cand})[0]["isError"] and abs(cand[0] - pt[0]) + abs(cand[1] - pt[1]) > 0.3:
                    far = cand
                    break
            call("add_layer", {"type": "paint", "name": "Dab"})
            call("set_paint_material", {"id": "", "color": [0.9, 0.1, 0.1]})
            call("set_brush", {"size": 30, "hardness": 0.9, "opacity": 1, "flow": 1})
            call("paint_stroke", {"points": [pt, [pt[0] + 0.01, pt[1]]]})
            at = [l["name"] for l in json.loads(call("layers_at", {"point": pt})[1])["layers"]]
            away = [l["name"] for l in json.loads(call("layers_at", {"point": far})[1])["layers"]] if far else ["?"]
            check(at[:1] == ["Dab"] and "Dab" not in away, "layers_at: the dab where it was painted only (%s / %s)" % (at, away))

        # (0.36) languages: a new translation from the template, used, then English again
        r, text = call("language_list")
        out = json.loads(text)
        codes = [l["code"] for l in out["languages"]]
        check(not r["isError"] and out["current"] == "en" and "en" in codes and "x-test" in codes and out["template"].endswith("patina.pot"),
              "language_list (%s)" % codes)
        r, text = call("language_new", {"code": "fr", "name": "Fran\u00e7ais"})
        po = json.loads(text)["file"] if not r["isError"] else ""
        check(po.endswith("fr.po") and os.path.exists(po) and tmp in po, "language_new writes fr.po in the user's folder (%s)" % text[:200])
        if os.path.exists(po):
            with open(po, encoding="utf-8") as f:
                body = f.read()
            body = body.replace('msgid "Reload"\nmsgstr ""', 'msgid "Reload"\nmsgstr "Recharger"')
            with open(po, "w", encoding="utf-8") as f:
                f.write(body)
        r, text = call("language_set", {"code": "fr"})
        out = json.loads(text) if not r["isError"] else {}
        check(out.get("current") == "fr" and out.get("translated", 0) >= 1, "language_set fr (%s)" % text[:200])
        r, text = call("language_list")
        names = {l["code"]: l["name"] for l in json.loads(text)["languages"]}
        check(names.get("fr") == "Fran\u00e7ais", "the new language is listed by its name (%s)" % names.get("fr"))
        r, text = call("language_set", {"code": "xx"})
        check(r["isError"], "an unknown language is an error")
        r, text = call("language_set", {"code": "en"})
        check(not r["isError"] and json.loads(text)["current"] == "en", "back to English")
    finally:
        try:
            proc.stdin.close()
            proc.wait(timeout=15)
        except Exception:
            proc.kill()
        shutil.rmtree(tmp, ignore_errors=True)
    print("mcp: %s" % ("all checks passed" if not failures else "%d FAILED" % len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
