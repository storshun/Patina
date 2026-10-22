"""Your own requests on your own models, through the Assistant, for you to judge: the handoff's
"best signal there is". Write ~/patina-ml/data/user_requests.txt like this:

    # model: ~/Models/hero.glb
    make the skin creases slightly redder
    one UV island for the head and another for the body
    # model: ~/Models/crate.fbx
    rust on the metal edges, a bit

Every request runs on a fresh copy of the model above it: the plan the model writes, whether it
ran, and before / after pictures go into an HTML page (~/patina-ml/reports/<name>.html) with a
right / wrong choice per request; your choices save to <name>-verdicts.json in the same folder
when you press Save (the page downloads the file).

    python3 tools/assistant/review_requests.py --model ~/patina-ml/gguf/patina-assistant-r1-q4_k_m.gguf --name mine-r1
"""
import argparse
import base64
import html
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))
import common  # noqa: E402


def read_requests(path):
    model, out = None, []
    for line in open(path):
        line = line.strip()
        if not line:
            continue
        if line.startswith("#"):
            if line[1:].strip().lower().startswith("model:"):
                model = os.path.expanduser(line[1:].strip()[6:].strip())
            continue
        if model:
            out.append((model, line))
    return out


def shot(s, w=420, h=420):
    return s("render_view", width=w, height=h, camera={"view": "front", "frame": True, "yaw": 0.45, "pitch": 0.12})["image_png_base64"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", required=True)
    ap.add_argument("--name", required=True)
    ap.add_argument("--requests", default=os.path.join(common.DATA, "user_requests.txt"))
    a = ap.parse_args()
    os.environ["PATINA_ASSISTANT_MODEL"] = os.path.expanduser(a.model)
    reqs = read_requests(a.requests)
    if not reqs:
        sys.exit("no requests in %s (see the top of this file for the format)" % a.requests)
    rows = []
    with common.Server(resolution=1024) as s:
        s("assistant_model", action="load")
        for i, (model, req) in enumerate(reqs):
            s("open_file", path=model)
            before = shot(s)
            r = {"model": model, "request": req}
            try:
                ans = s("assistant_ask", request=req)
                r.update({"plan": ans["plan"], "steps": ans["steps"], "seconds": round(ans["seconds"], 2),
                          "warnings": ans["warnings"]})
                if ans["plan"].get("steps"):
                    s("assistant_apply", plan=ans["plan"], request=req)
                    r["ran"] = True
            except common.CommandFailed as e:
                r["error"] = str(e)
            r["before"], r["after"] = before, shot(s)
            rows.append(r)
            print("%2d. %s\n    %s" % (i + 1, req, r.get("plan", r.get("error"))), flush=True)
    os.makedirs(common.REPORTS, exist_ok=True)
    page = ["<!doctype html><meta charset=utf-8><title>Assistant review: %s</title>" % html.escape(a.name),
            "<style>body{font:14px sans-serif;background:#1d1f23;color:#ddd;max-width:1000px;margin:auto}"
            ".r{border-top:1px solid #444;padding:12px 0}img{width:300px;border-radius:4px;margin-right:8px}"
            "code{color:#9cf}label{margin-right:16px}</style>",
            "<h1>Assistant review: %s</h1><p>Model <code>%s</code>, %s.</p>" % (html.escape(a.name), html.escape(a.model),
                                                                            time.strftime("%Y-%m-%d %H:%M"))]
    for i, r in enumerate(rows):
        say = (r.get("plan") or {}).get("say") or (r.get("plan") or {}).get("ask") or r.get("error", "")
        steps = "".join("<li>%s</li>" % html.escape(x) for x in r.get("steps", []))
        page.append("<div class=r><b>%d. %s</b><br><small>%s</small><p>%s</p><ul>%s</ul>"
                    "<img src='data:image/png;base64,%s'><img src='data:image/png;base64,%s'><br>"
                    "<label><input type=radio name=v%d value=right> right</label>"
                    "<label><input type=radio name=v%d value=wrong> wrong</label>"
                    "<input name=n%d placeholder='what you wanted' size=60><br><small><code>%s</code></small></div>"
                    % (i + 1, html.escape(r["request"]), html.escape(r["model"]), html.escape(say), steps, r["before"],
                       r["after"], i, i, i, html.escape(json.dumps(r.get("plan")))))
    page.append("<p><button onclick='save()'>Save</button></p><script>function save(){var o=[];for(var i=0;i<%d;i++){"
                "var c=document.querySelector('input[name=v'+i+']:checked');o.push({index:i,verdict:c?c.value:null,"
                "note:document.querySelector('input[name=n'+i+']').value})}var b=new Blob([JSON.stringify(o,null,1)],"
                "{type:'application/json'});var l=document.createElement('a');l.href=URL.createObjectURL(b);"
                "l.download='%s-verdicts.json';l.click()}</script>" % (len(rows), a.name))
    path = os.path.join(common.REPORTS, a.name + ".html")
    open(path, "w").write("\n".join(page))
    json.dump([{k: v for k, v in r.items() if k not in ("before", "after")} for r in rows],
              open(os.path.join(common.REPORTS, a.name + ".json"), "w"), indent=1)
    print("review page:", path)


if __name__ == "__main__":
    main()
