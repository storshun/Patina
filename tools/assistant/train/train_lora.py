"""LoRA fine-tune of a small instruct model on the Assistant's data (peft + transformers).

    source ~/venvs/patina-ml/bin/activate
    python tools/assistant/train/train_lora.py --base ~/patina-ml/base/Qwen3-1.7B --run r1

Writes ~/patina-ml/runs/<run>/ (adapter checkpoints, settings.json, log) and, with --merge, the
merged model in ~/patina-ml/runs/<run>/merged/ for convert_hf_to_gguf.py.

The prompt is built here by hand, not by the tokenizer's chat template, so Patina can build
exactly the same text (src/engine/Assistant: promptFor):
  <|im_start|>system\\n{system}<|im_end|>\\n<|im_start|>user\\n{user}<|im_end|>\\n
  <|im_start|>assistant\\n<think>\\n\\n</think>\\n\\n{plan}<|im_end|>
(Qwen3 with thinking off.) Only the plan and its end marker are trained on.
"""
import argparse
import json
import math
import os
import random
import sys
import time

import torch
from torch.utils.data import Dataset

ML = os.path.expanduser(os.environ.get("PATINA_ML", "~/patina-ml"))


def prompt_prefix(system, user):
    return ("<|im_start|>system\n" + system + "<|im_end|>\n<|im_start|>user\n" + user +
            "<|im_end|>\n<|im_start|>assistant\n<think>\n\n</think>\n\n")


ANSWER_END = "<|im_end|>"


class PlanData(Dataset):
    def __init__(self, path, tok, max_len, limit=0):
        self.items = []
        skipped = 0
        with open(path) as f:
            for line in f:
                r = json.loads(line)
                m = r["messages"]
                pre = tok(prompt_prefix(m[0]["content"], m[1]["content"]), add_special_tokens=False)["input_ids"]
                ans = tok(m[2]["content"] + ANSWER_END, add_special_tokens=False)["input_ids"]
                if len(pre) + len(ans) > max_len:
                    skipped += 1
                    continue
                self.items.append((pre, ans))
                if limit and len(self.items) >= limit:
                    break
        self.skipped = skipped

    def __len__(self):
        return len(self.items)

    def __getitem__(self, i):
        pre, ans = self.items[i]
        return {"input_ids": pre + ans, "labels": [-100] * len(pre) + ans}


def collate(pad_id):
    """Left padding: every sequence ends at the same place, so the plans (at the end) line up
    and the loss needs logits only for the last few positions (see PlanTrainer)."""
    def fn(batch):
        n = max(len(b["input_ids"]) for b in batch)
        ids = torch.full((len(batch), n), pad_id, dtype=torch.long)
        lab = torch.full((len(batch), n), -100, dtype=torch.long)
        att = torch.zeros((len(batch), n), dtype=torch.long)
        for i, b in enumerate(batch):
            k = len(b["input_ids"])
            ids[i, n - k:] = torch.tensor(b["input_ids"])
            lab[i, n - k:] = torch.tensor(b["labels"])
            att[i, n - k:] = 1
        pos = (att.cumsum(1) - 1).clamp(min=0)
        return {"input_ids": ids, "labels": lab, "attention_mask": att, "position_ids": pos}
    return fn


def make_trainer_class(Trainer):
    class PlanTrainer(Trainer):
        """Logits only for the positions that are trained on (the plan at the end of each
        sequence): the full logits of a 150k-word vocabulary over ~1,000 tokens don't fit."""

        def compute_loss(self, model, inputs, return_outputs=False, num_items_in_batch=None):
            labels = inputs["labels"]
            has = (labels != -100).any(0).nonzero()
            k = int(labels.shape[1] - has.min().item() + 1) if len(has) else 2
            out = model(input_ids=inputs["input_ids"], attention_mask=inputs["attention_mask"],
                        position_ids=inputs["position_ids"], logits_to_keep=k)
            logits = out.logits[:, :-1, :].float()
            target = labels[:, -k + 1:]
            loss_sum = torch.nn.functional.cross_entropy(logits.reshape(-1, logits.shape[-1]), target.reshape(-1),
                                                         ignore_index=-100, reduction="sum")
            n = num_items_in_batch if num_items_in_batch is not None else (target != -100).sum()
            loss = loss_sum / n
            return (loss, out) if return_outputs else loss
    return PlanTrainer


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", default=os.path.join(ML, "base", "Qwen3-1.7B"))
    ap.add_argument("--run", required=True)
    ap.add_argument("--data", default=os.path.join(ML, "data"))
    ap.add_argument("--epochs", type=float, default=2.0)
    ap.add_argument("--lr", type=float, default=2e-4)
    ap.add_argument("--rank", type=int, default=32)
    ap.add_argument("--alpha", type=int, default=64)
    ap.add_argument("--dropout", type=float, default=0.05)
    ap.add_argument("--batch", type=int, default=8)
    ap.add_argument("--accum", type=int, default=4)
    ap.add_argument("--max-len", type=int, default=2048)
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--limit", type=int, default=0, help="only this many training examples (a smoke test)")
    ap.add_argument("--merge", action="store_true", help="merge the adapter into the base weights at the end")
    ap.add_argument("--no-checkpointing", action="store_true", help="faster, needs more GPU memory")
    ap.add_argument("--init-adapter", help="continue from this run's adapter (e.g. ~/patina-ml/runs/r2/adapter) "
                                         "instead of a new one; rank and alpha come from it")
    a = ap.parse_args()

    from peft import LoraConfig, PeftModel, get_peft_model
    from transformers import AutoModelForCausalLM, AutoTokenizer, Trainer, TrainingArguments, set_seed

    set_seed(a.seed)
    random.seed(a.seed)
    out = os.path.join(ML, "runs", a.run)
    os.makedirs(out, exist_ok=True)
    settings = dict(vars(a))
    settings["started"] = time.strftime("%Y-%m-%d %H:%M:%S")
    json.dump(settings, open(os.path.join(out, "settings.json"), "w"), indent=1)

    tok = AutoTokenizer.from_pretrained(a.base)
    pad = tok.pad_token_id if tok.pad_token_id is not None else tok.convert_tokens_to_ids("<|endoftext|>")
    train = PlanData(os.path.join(a.data, "train.jsonl"), tok, a.max_len, a.limit)
    val = PlanData(os.path.join(a.data, "val.jsonl"), tok, a.max_len, 400)
    print("train %d (skipped %d too long), val %d" % (len(train), train.skipped, len(val)), flush=True)

    model = AutoModelForCausalLM.from_pretrained(a.base, dtype=torch.bfloat16, attn_implementation="sdpa")
    if not a.no_checkpointing:
        model.gradient_checkpointing_enable(gradient_checkpointing_kwargs={"use_reentrant": False})
        model.enable_input_require_grads()
    if a.init_adapter:
        model = PeftModel.from_pretrained(model, os.path.expanduser(a.init_adapter), is_trainable=True)
    else:
        cfg = LoraConfig(r=a.rank, lora_alpha=a.alpha, lora_dropout=a.dropout, bias="none", task_type="CAUSAL_LM",
                         target_modules=["q_proj", "k_proj", "v_proj", "o_proj", "gate_proj", "up_proj", "down_proj"])
        model = get_peft_model(model, cfg)
    model.print_trainable_parameters()

    steps_per_epoch = math.ceil(len(train) / (a.batch * a.accum))
    args = TrainingArguments(
        output_dir=out, num_train_epochs=a.epochs, learning_rate=a.lr, lr_scheduler_type="cosine",
        warmup_steps=max(1, int(0.03 * steps_per_epoch * a.epochs)),
        per_device_train_batch_size=a.batch, per_device_eval_batch_size=a.batch, gradient_accumulation_steps=a.accum,
        bf16=True, logging_steps=10, eval_strategy="steps", eval_steps=max(50, steps_per_epoch // 4),
        save_strategy="steps", save_steps=max(50, steps_per_epoch // 2), save_total_limit=2, seed=a.seed,
        report_to=[], dataloader_num_workers=2, remove_unused_columns=False,
        weight_decay=0.0, max_grad_norm=1.0)
    trainer = make_trainer_class(Trainer)(model=model, args=args, train_dataset=train, eval_dataset=val,
                                          data_collator=collate(pad))
    t0 = time.time()
    trainer.train()
    model.save_pretrained(os.path.join(out, "adapter"))
    tok.save_pretrained(os.path.join(out, "adapter"))
    settings["minutes"] = round((time.time() - t0) / 60, 1)
    settings["log"] = trainer.state.log_history[-20:]
    json.dump(settings, open(os.path.join(out, "settings.json"), "w"), indent=1)
    if a.merge:
        merged = model.merge_and_unload()
        merged.save_pretrained(os.path.join(out, "merged"), safe_serialization=True)
        tok.save_pretrained(os.path.join(out, "merged"))
    print("done in %.1f min -> %s" % ((time.time() - t0) / 60, out), flush=True)


if __name__ == "__main__":
    sys.exit(main())
