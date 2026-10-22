# llama.cpp in Patina

The Patina Assistant runs its model with llama.cpp, vendored here so the build downloads nothing.

- Upstream: https://github.com/ggml-org/llama.cpp, release tag **b11540** (commit 1e6f04a).
- Kept: `include/`, `src/` (the llama library), `cmake/`, and `ggml/` with the CPU, CUDA and
  Vulkan backends only. Everything else (tools, examples, tests, common/, other backends) is left
  out. The JSON-schema-to-grammar converter is not used: Patina writes its own grammar
  (`src/engine/Assistant.cpp`, buildGrammar).
- Licence: MIT (`LICENSE`).
- Upgrading: copy the same folders from a newer tag, update this file, and run the Assistant
  evaluation again (`tools/assistant/eval.py`) before shipping it.
- One local change: the top-level `CMakeLists.txt` adds `vendor/` only when it exists (it is not
  kept; only common/ and the tools use it).
- `src/models/clef.cpp`: `#include <algorithm>` added (GCC 16 needs it).
