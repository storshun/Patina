#!/usr/bin/env bash
# Turns a training run's merged model into the GGUF files Patina loads, and prints their size
# and SHA-256 (for the manual and assistant::knownModels).
#
#   tools/assistant/train/export_gguf.sh r1 [Q4_K_M Q5_K_M Q8_0]
# needs ~/src/llama.cpp (convert_hf_to_gguf.py, build/bin/llama-quantize) and the training venv.
set -euo pipefail
RUN="$1"; shift
TYPES=("${@:-Q4_K_M}")
ML="${PATINA_ML:-$HOME/patina-ml}"
LLAMA="${LLAMA_CPP:-$HOME/src/llama.cpp}"
MERGED="$ML/runs/$RUN/merged"
OUT="$ML/gguf"
mkdir -p "$OUT"
[[ -d "$MERGED" ]] || { echo "no merged model in $MERGED (train with --merge)"; exit 1; }
source "$HOME/venvs/patina-ml/bin/activate"
F16="$OUT/patina-assistant-$RUN-f16.gguf"
python "$LLAMA/convert_hf_to_gguf.py" "$MERGED" --outtype f16 --outfile "$F16" > "$OUT/convert-$RUN.log" 2>&1
for t in "${TYPES[@]}"; do
  q="$OUT/patina-assistant-$RUN-$(echo "$t" | tr 'A-Z' 'a-z').gguf"
  "$LLAMA/build/bin/llama-quantize" "$F16" "$q" "$t" > "$OUT/quantize-$RUN-$t.log" 2>&1
done
for f in "$F16" "$OUT"/patina-assistant-"$RUN"-q*.gguf; do
  printf "%-60s %6d MB  %s\n" "$(basename "$f")" $(( $(stat -c %s "$f") / 1048576 )) "$(sha256sum "$f" | cut -d' ' -f1)"
done
