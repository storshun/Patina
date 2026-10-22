"""Merges a LoRA checkpoint into the base weights (on the CPU, so it doesn't compete with a
training run for the GPU), for export_gguf.sh.

    python tools/assistant/train/merge_adapter.py --adapter ~/patina-ml/runs/r1/checkpoint-476 --run r1-e1
writes ~/patina-ml/runs/<run>/merged/
"""
import argparse
import os

import torch

ML = os.path.expanduser(os.environ.get("PATINA_ML", "~/patina-ml"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", default=os.path.join(ML, "base", "Qwen3-1.7B"))
    ap.add_argument("--adapter", required=True)
    ap.add_argument("--run", required=True)
    a = ap.parse_args()
    from peft import PeftModel
    from transformers import AutoModelForCausalLM, AutoTokenizer
    model = AutoModelForCausalLM.from_pretrained(a.base, dtype=torch.bfloat16, device_map="cpu")
    model = PeftModel.from_pretrained(model, os.path.expanduser(a.adapter)).merge_and_unload()
    out = os.path.join(ML, "runs", a.run, "merged")
    model.save_pretrained(out, safe_serialization=True)
    AutoTokenizer.from_pretrained(a.base).save_pretrained(out)
    print("merged ->", out)


if __name__ == "__main__":
    main()
