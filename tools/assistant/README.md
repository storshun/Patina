# Patina Assistant: data, training and evaluation

Everything that makes the Assistant's model, as described in `docs/ASSISTANT_PLAN.md`. Python
(standard library; Pillow for pictures); the training step uses `~/venvs/patina-ml`. Data,
runs and model files live in `~/patina-ml/` (`PATINA_ML` to move it), never in the repo.

| Step | Command | Writes |
| --- | --- | --- |
| Scenes | `python3 tools/assistant/scenes.py` | `scenes/*.patina`, `*.json` |
| Examples, checked in Patina | `python3 tools/assistant/generate.py --per-scene 160 --workers 8` | `data/canonical.jsonl`, `logs/generate-failures.jsonl` |
| Paraphrases (a local model on an OpenAI-style server) | `python3 tools/assistant/paraphrase.py --url http://localhost:8091` | `data/paraphrases.jsonl` (cached) |
| Train / val / test | `python3 tools/assistant/build_dataset.py` | `data/{train,val,test}.jsonl`, `data/stats.json` |
| Fine-tune | `python tools/assistant/train/train_lora.py --run r1 --merge` | `runs/r1/` |
| GGUF + hash | `tools/assistant/train/export_gguf.sh r1 Q4_K_M` | `gguf/patina-assistant-r1-*.gguf` |
| Evaluation | `python3 tools/assistant/eval.py --model <gguf> --name r1-q4 --limit 400 --workers 4` | `reports/r1-q4.md` |
| Your own requests | `python3 tools/assistant/review_requests.py --model <gguf> --name mine` | `reports/mine.html` |
| Manual pictures | `python3 tools/assistant/manual_images.py --model <gguf>` | `docs/manual/img/assistant_*.png` |

`checks.py` holds the checkers (data, so the same specs judge the canonical plans and the
model's), `tasks.py` the task types, `vocab.py` the fixed meanings of vague words, colours and
mask words, `meshgen.py` the generated models. Headless servers are started by `common.Server`
with their own home folder and are stopped by PID (they also stop when their parent does).
