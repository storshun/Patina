"""Request-to-plan time on the GPU, with the same prompt and grammar Patina makes, through a
CUDA build of llama-server (the same llama.cpp release Patina vendors). For machines where
Patina itself is built without CUDA (no nvcc): the number the handoff asks for on the RTX 5090.

    python3 tools/assistant/gpu_latency.py --model ~/patina-ml/gguf/patina-assistant-r1-q4_k_m.gguf \\
        --server ~/llama.cpp/b11540-cuda/llama-b11540/llama-server [--n 40]
"""
import argparse
import json
import os
import random
import statistics
import subprocess
import sys
import time
import urllib.request

sys.path.insert(0, os.path.dirname(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "train"))
import common  # noqa: E402
import scenes  # noqa: E402
from train_lora import prompt_prefix  # noqa: E402


def post(url, body, timeout=60):
    req = urllib.request.Request(url, data=json.dumps(body).encode(), headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", required=True)
    ap.add_argument("--server", default=os.path.expanduser("~/llama.cpp/b11540-cuda/llama-b11540/llama-server"))
    ap.add_argument("--n", type=int, default=40)
    a = ap.parse_args()
    items = list(common.jsonl_read(os.path.join(common.DATA, "test.jsonl")))
    random.Random(1).shuffle(items)
    items = [i for i in items if not i["setup"]][:a.n]
    metas = {m["id"]: m for m in scenes.load_scenes()}
    port = common.free_port()
    # the CUDA runtime that comes with the llama.cpp release (cudart-*), next to the server's folder
    root = os.path.dirname(os.path.dirname(a.server))
    cudart = [os.path.join(root, d) for d in os.listdir(root) if d.startswith("cudart")]
    env = dict(os.environ, LD_LIBRARY_PATH=":".join([os.path.dirname(a.server)] + cudart))
    srv = subprocess.Popen([a.server, "-m", os.path.expanduser(a.model), "--port", str(port), "-ngl", "99", "-c", "4096",
                            "-np", "1", "--no-webui"], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                           preexec_fn=common._die_with_parent)
    try:
        for _ in range(240):
            try:
                urllib.request.urlopen("http://127.0.0.1:%d/health" % port, timeout=1)
                break
            except Exception:
                time.sleep(0.5)
        times, toks = [], []
        with common.Server() as s:
            for it in items:
                s("open_file", path=metas[it["scene"]]["path"])
                ctx = s("assistant_context", request=it["request"])
                g = s("assistant_grammar")["grammar"]
                text = ctx["text"].rstrip() + "\nRequest: " + it["request"]
                t0 = time.time()
                r = post("http://127.0.0.1:%d/completion" % port,
                         {"prompt": prompt_prefix(ctx["system"], text), "grammar": "" if os.environ.get("NO_GRAMMAR") else g, "temperature": 0, "n_predict": 512,
                          "cache_prompt": True})
                times.append(time.time() - t0)
                toks.append(r.get("tokens_predicted", 0))
    finally:
        srv.terminate()
        srv.wait()
    times.sort()
    print("GPU (llama-server CUDA, %d requests): median %.2f s, 95th percentile %.2f s, max %.2f s; %.0f tokens per plan"
          % (len(times), statistics.median(times), times[int(len(times) * 0.95) - 1], times[-1], statistics.mean(toks)))


if __name__ == "__main__":
    main()
