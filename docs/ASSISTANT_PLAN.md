# Patina Assistant: working plan

The handoff (`Patina Assistant local model handoff.md`, 14 Oct 2026) is the source. This file
records what changed from it, what was added to make the Assistant easier to live with, and
where each phase stands. Plain words, British spelling, as in the manual.

## Status

| Phase | State |
| --- | --- |
| 0. Source and machine | Done. Bundle SHA-256 checked, `assistant` branch. Fixes before starting (below). |
| 1. Commands the model can aim at | Done: mesh_parts, select_part, add_tint_layer, add_material_layer, uv_island_per_part, assistant_context, plus update_layer_mask, layer_region_faces, region on apply_smart_material. Vertex groups from Blender. |
| 2. Training data | Done: 29 scenes, ~45 task types, 4,028 checked canonical examples, local paraphrases. |
| 3. Fine-tune | Done: round 7 ships with 0.12.0 (round 5 with 0.11) |
| 4. Assistant inside Patina | Plumbing done (llama.cpp b11540 vendored, worker thread, grammar, assistant_ask / apply / check / model, undo groups, the panel, palette row, feature switch). |
| 5. Evaluation | Rounds 1-7 evaluated, three hand-written tests (below); round 7 ships with 0.12.0 |

### Found while getting the baseline green

- **NVIDIA 610 driver: vertex buffer updates were ignored by the next draw.** Polygon fill,
  moving one UV island, texture-set painting and UDIM painting silently did nothing on the
  RTX 5090 (7 gpu tests failed; Mesa passed). Fixed in `GpuMesh` with a flush after each
  in-place update. This affects the released 0.10.1 on this machine too.
- The HTTP API test assumed a 2K bake takes longer than 1.5 s. On the 5090 it doesn't; the
  test now queues eight 4K bakes.
- The handoff says 12 ctest suites; 0.10.1 has 8 (`web_e2e` among them). All 8 pass.

## What the handoff left implicit, decided here

- **Paraphrase writer (Phase 2).** Schala's `schala-llm` (a 9B model on llama.cpp CUDA,
  port 8091) is already resident, so it writes the first round of paraphrases at no extra
  VRAM. The 27B model is used for a second round only when 20 GB or more is free. VRAM is
  shared with Schala, Blender and ComfyUI, so every long GPU job checks free memory first.
- **Base model (Phase 3).** Qwen3 1.7B stays the first try; newer Apache/MIT small models are
  checked when Phase 3 starts. Models with hybrid attention (Gated Delta Net) are avoided
  for now: llama.cpp's CPU and Vulkan paths are slow on that op.
- **Regions are masks, not selections.** A region becomes the layer's mask (white on the
  part's faces) under the smart mask, so the result survives later selections and can be
  repainted by hand like any other mask.

## Additions for the person using it

These cost little on top of the handoff and change how the Assistant feels in use.

1. **See it before keeping it.** *Preview* applies the plan inside its undo group, so the
   viewport shows the real result; *Keep* closes the group, *Cancel* rolls it back. The
   plain-words list stays beside it. A plan that rebuilds UVs or deletes something still
   shows the warning line, and nothing is kept until *Keep*.
2. **Adjust without asking again.** The preview shows the numbers that matter (color
   swatch, strength, mask level) as sliders taken from the plan's own arguments; moving one
   re-runs the preview. "Slightly more red" rarely lands exactly where an artist wants it.
3. **Follow-ups.** The context includes what the last request made (layer names and
   indices), so "a bit stronger", "less red", "only on the face" or "undo that" work on the
   layer just made. Phase 2 gets follow-up task types for this.
4. **Questions you answer with a click.** When the model asks ("Which part is the head?"),
   the parts it lists are buttons. Hovering one highlights its faces in the viewport;
   clicking it sends the request again with the answer.
5. **The current selection is a part.** `selection` is always in the part list, so painting
   faces with the Island brush or Select tool and asking "make these rusty" works, and so
   does fixing a region the detector missed.
6. **Things it makes are easy to find.** Layers get plain names from the request and a
   color tag; a plan that makes more than one layer puts them in a folder named after the
   request.
7. **Examples that fit the open model.** An empty panel shows a few requests built from the
   model's own part names ("Make the Jeans worn at the edges"); clicking one fills the box.
8. **Plans are commands.** *Copy as commands* puts the plan on the clipboard for the console;
   with the macro recorder running, a kept request is recorded as its steps, so it replays
   without the model.
9. **Says where it runs.** One line in the panel: it runs on this computer and sends nothing
   anywhere.
10. **Feedback that improves the next model.** A thumbs-down (and an optional "what I
    wanted") saves the request, context and plan to `userDataDir/assistant/feedback.jsonl`.
    Nothing leaves the machine; the file is what the user can choose to share. The same
    file seeds the 50 real requests that Phase 5 needs.

## Phase notes

### Phase 1
- Parts: material slots (only when there are several), Blender vertex groups (weight >= 0.5,
  a new optional `.ptmesh` chunk), connected pieces (not when they only repeat a slot or group;
  at most 8 when slots or groups name the model), and `region:head` / `region:body`.
- Head detection follows the handoff (narrowest band between bands 1.4x wider) with two
  changes found on real models: the search goes down to 55% of the height when the top 35%
  has no neck (a bust is mostly head), and when long hair hides the neck it looks again on the
  largest pieces on their own. Tested on the bust, a sphere on a cylinder, a one-piece figure,
  a cube / sphere / torus (no head), and three of the user's characters (Tripo Fubuki, a
  chibi, a long-haired figure): all three get a head.
- `uv_island_per_part` on real meshes needed more than the Island brush: each part is unwrapped
  as a small mesh of its own (zero-length edges merged, degenerate triangles dropped, quad
  diagonals never cut), with a cut graph that opens handles and non-manifold edges, cuts from
  the far tips (hands, feet) along the back, LSCM then ARAP (new, core/UVArap), and candidates
  scored by extra islands, fold-overs, stretch and size spread. On the Tripo character the body
  island's stretch (95th percentile) went from 63.8 to 1.8.
- Undo groups (handoff Phase 4.5) were built in Phase 1: each new command is one undo step.
- FBX import now welds vertices at identical positions, as glTF import does.

### Phase 2
- Checkers are data (tools/assistant/checks.py), so the same specs score the canonical plans
  and the model's plans; regions are compared by faces, so `group:Head`, `region:head` and a
  slot with the same faces are all right.
- Plans are written one way (say / ask, steps; cmd, args; argument keys alphabetical), which is
  what the grammar enforces.
- The paraphrases came from Schala's 9B model (2 of its 4 slots), with filters that drop a
  rewrite that changes the amount class, color family, part, side or a number.

### Phase 4
- The grammar is written by Patina, not llama.cpp's converter (nlohmann/json 3.11 vs 3.12, and
  live enums for the open project's parts, layer indices, materials, smart materials and brushes
  make it stricter). A plan can't name a part or material that doesn't exist.
- The panel previews by running the plan as one undo step; Cancel undoes exactly that step and
  only while it is still the last thing done.
- Not yet possible on this machine: Vulkan builds (no glslc) and the GUI tests (no Xvfb). See
  "Things for Randall" below. CUDA builds work (see "A CUDA build" below).

### Phase 3 and 5: round 1

- Qwen3 1.7B (Apache 2.0, revision 70d244cc), LoRA r32 / alpha 64 / dropout 0.05 on all
  attention and MLP projections, lr 2e-4 cosine, 2 epochs, effective batch 32, bf16, loss on the
  plan only (logits computed for the plan positions only: the full vocabulary didn't fit beside
  Schala's model). 15,254 examples, 86 minutes on the RTX 5090. Validation loss 0.064.
- Quantizing costs nothing measurable: Q8_0 92.0%, Q4_K_M 92.2% on the same 400 requests. Q4_K_M
  is 1,056 MB.
- Held-out evaluation, end to end through Patina (400 requests: unseen scenes, unseen task
  variants, and three of the user's own characters):

| Measure | Round 1 (Q4_K_M) | Bar |
| --- | --- | --- |
| Task success | 92.2% (generated scenes 90.3%, the user's characters 98.9%) | >= 85% |
| Every task type >= 70% | no: color follow-ups 0% (held out entirely, a split mistake), duplicate 25%, tool 62% | >= 70% |
| The two example requests | 75%: the 2 misses had exactly the expected plan; a UV scoring change (fixed) gave a coin-shaped part 3 islands | >= 95% |
| Harmful plans | 0.0% | <= 1% |
| Knowing when not to act | 100% | >= 90% |
| Latency, RTX 5090 (llama.cpp CUDA, same release) | median 0.16 s, 95th percentile 0.23 s | < 1 s |
| Latency, CPU (Patina's own build, 8 threads) | median 3.4 s, 95th percentile 5.6 s | < 10 s |

- Round 2 fixes the data rather than the model: every task type keeps at least one variant in
  training, noise breakup is accepted on wear and dirt, and 6,547 examples (23,653 training
  requests).

### Phase 3 and 5: round 2

- Same settings as round 1; 6,547 examples, 23,653 training requests, 112 minutes beside
  Schala's model. Validation loss 0.064.
- Both rounds on the same 500 held-out requests (round 2's test set, which includes the user's
  three characters):

| Measure | Round 1 | Round 2 | Bar |
| --- | --- | --- | --- |
| Task success | 93.2% | 96.8% | >= 85% |
| On the user's characters (148) | 93.9% | 98.6% | |
| Worst task type | color follow-ups 0% | plain tint, view mode 80% | >= 70% |
| The two example requests (10) | 100% | 100% | >= 95% |
| Harmful plans | 0.2% | 0.0% | <= 1% |
| Knowing when not to act (37) | 97.3% | 97.3% | >= 90% |
| Latency, RTX 5090 (llama.cpp CUDA) | median 0.16 s, p95 0.23 s | median 0.14 s, p95 0.24 s | < 1 s |
| Latency, CPU (Patina, 8 threads) | median 3.4 s, p95 5.6 s | median 2.9 s, p95 5.6 s | < 10 s |
| Latency, Patina's own CUDA build (RTX 5090) | | median 0.07 s, p95 0.18 s | < 1 s |

- On the user's three characters the UV example gives the right plan, but a whole body as one
  island folds in places (the arms and legs over the torso, layered hair over itself): 1-50% of
  a part's faces. Cutting the folds out locally didn't help (thousands of small islands, or no
  change), so the command now says so in plain words and points to the Island brush; the panel
  shows it under the preview. A better automatic cut for whole bodies (finding the arms and legs
  as parts) is the next step for this request.
- Round 2's file: 1,107,408,640 bytes, SHA-256
  `a772c78dcc3fe33817b6c626bdb3b9a3ae0848d4a475e048610c072a7d90482b` (still listed in
  `assistant::knownModels()`; kept as `~/patina-ml/release/patina-assistant-1.7b-r2-q4_k_m.gguf`).

### Round 3: a model that sets more up at once

What a better model should do (2026-10-15). Randall's aim: the Assistant sets up layers, masks,
materials and settings so the user drives the result with sliders, not menus. Round 2 handles one
change per request in template-like words. Round 3 targets the requests that still cost the user
the most clicks:

| Request | Before (by hand) | Round 3 |
| --- | --- | --- |
| "make the armor look battle-worn" | 3 fill layers, 3 materials, 3 smart masks, a folder | one folder, a slider per layer |
| "jacket black leather, jeans denim, mud on the boots" | three requests or 3 layers by hand | one request, one undo step |
| "I want to paint scratches on the helmet" | new layer, mask to the slot, material, brush preset, tool | ready to paint, strokes stay on the helmet |
| "make the jacket a bit darker / glossier / less saturated" | adjustment layer, mask to the slot, a slider | the same, in one sentence |
| "turn the rust down", "hide the dirt and the grime" | find the layers, change them | by what they are, not their exact names |
| "dirt everywhere except the face", "make these gold" | region and exclusions by hand | understood |
| "put it on the boots too", "rename it", "remove that" | | follow-ups after any layer |

What it takes:
- Data, not a bigger model: ten new request families in `tasks.py` (looks, multi, colored
  materials, paint set-up, adjustments, layers by content, two layers at once, except, the
  selection, more follow-ups), about 5,800 new checked examples, rewritten by Schala's 9B model.
- Patina: look words shortlist the weathering materials; selections can be set before a request in
  the data; plans for the new families leave mask levels at their defaults (shorter answers).
- Training: continue from round 2's adapter (half the time of a fresh run) on the new data plus 40%
  of the older examples, so nothing learned before is lost.
- A test that isn't made by the same templates: `hard_tests.py`, 73 requests worded by hand on
  held-out scenes and the real characters, checked by effect. Round 2 scores 50.7% on it.
- A bigger base (Qwen3 4B) would understand odd wording better but is about 2.5 times slower on
  the processor (3 s would become 7-8 s); kept for later, if the hand-written test shows the 1.7B
  model can't learn the new families.

Round 3 results (Q4_K_M, Patina's CUDA build, 2026-10-15). Trained 71 minutes from round 2's
adapter: 29,365 examples (19,769 of the new types, 9,596 older ones kept for rehearsal), lr 1.5e-4,
one epoch, validation loss 0.032.

| Test | Round 2 | Round 3 |
| --- | --- | --- |
| Hand-written requests (73 / 71) | 50.7% | 89.0% |
| ... without the 9 also worded so in the data | 46.8% | 87.5% |
| Round 2's 500 held-out requests | 96.8% | 96.0% (harmful 0.6%) |
| The new request types, held out (450) | - | 94.9% |
| Latency, GPU | median 0.14 s | median 0.10 s (0.17 s on the new types) |
| Latency, 8 CPU threads, looks and multi-part requests | - | median 6.9 s, p95 9.8 s |

Round 3's misses, fixed in round 4's data: two looks at once ("old and rusty" got one), a casual
verb ("slap some rust on") read as painting by hand, "wood for the shield, gold for the emblem"
read wrongly, and older requests that drifted toward the new types ("make it all snow" became a
snowy look, "make the shirt a little charcoal" a colored fabric): those types are rehearsed more.

Rounds 4 and 5 (2026-10-15, each continuing from the one before):
- Round 4 (54 min, 21,681 examples): the brush relative to how it is set (the context got a
  "Painting:" line), "make it look clean again", more things Patina doesn't do, "what can you
  do?", and round 3's misses (two looks at once, casual verbs, "X for the Y" lists); the older
  types that drifted were rehearsed more.
- A second hand-written test (`hard2_tests.py`, 60 requests) written after round 4, never used for
  data: round 2 50.0%, round 4 91.7%.
- Round 4 showed that the data split had kept some common variants out of training altogether
  since round 1 (an AO bake on its own, a plain tint on a slot, the soft brush, the clone and
  fill tools, the multiply blend, 23 in all), so they could be tested. Round 5 (42 min, 16,400
  examples) trains on them, with more ways to ask for brush changes, bakes and cleaning up;
  testing keeps to the held-out scenes and the two hand-written tests.
- Patina: a layer number in a plan now means the layer the model was shown ("delete 3, then 4"
  failed before: the first deletion moved the second layer); steps name library materials.

| Test | Round 2 | Round 3 | Round 4 | Round 5 (ships) |
| --- | --- | --- | --- | --- |
| Hand-written test 1 (81, 73 for rounds 2-3) | 50.7% | 89.0% | 95.1% | 93.8% |
| Hand-written test 2 (60, never used for data) | 50.0% | - | 91.7% | 96.7%* |
| Older request types, held-out scenes (400) | - | - | 96.0% (harmful 0.5%) | 96.0% (harmful 0.2%) |
| New request types, held-out scenes (400) | - | - | 92.5% | 92.2% |
| Latency, GPU (median / p95) | 0.14 / 0.24 s | 0.10 / 0.28 s | 0.11 / 0.31 s | 0.10 / 0.28 s |
| Latency, 8 CPU threads, short requests | 2.9 / 5.6 s | - | - | 3.3 / 5.2 s |
| Latency, 8 CPU threads, looks and multi-part | - | 6.9 / 9.8 s | - | 6.8 / 9.6 s |

\* round 5's extra wordings for brush changes and bakes came from the kinds of misses round 4
made on test 2, so its 96.7% is not as clean a number as round 4's 91.7%.

Round 5 ships: even with round 4 where the tests are fair, fewer harmful plans, and trained on
every variant. File `patina-assistant-1.7b-q4_k_m.gguf`, 1,107,408,640 bytes, SHA-256
`b0704b177aa143def9bba9acec57e12fa3fcc7f753f41dd20fab83b2db92b1cd`.

What it still gets wrong: a part named by the model's own name ("the whole shield" when a slot
is called Shield_Wood), three-way lists with "for" ("wood for the shield, gold for the emblem,
rust on the rim") now and then, and the closest of several skin materials ("fair skin" gave
light skin).

### Round 6 (Patina 0.12): more it can set up, and honest about the rest

Randall, 2026-10-15: the Assistant "tries to shoehorn the request into something it can do vs saying
'I can't do that, but here's some things that might be similar...'". His own history showed it:
"split the head UVs into their own texture map", "put the head on a separate uv map" and "add the
head to a new udim tile" all became "make the head one UV island" (round 5: 14.5% on a test of
such requests, 10% harmful plans).

What changed:
- Patina: a plan may `suggest` up to four requests it can do (buttons in the panel); a last step may
  clear the undo history (`texture_set_from_faces`, `set_resolution`, `texture_tiles`; the panel
  offers *Do it* instead of Preview); `open_window` (export with a preset, render, symmetry,
  high-poly bake, Blender link, preferences...); `set_stencil` for text (no file arguments);
  `add_tint_layer` glow (emissive); `uv_density` scale (more or less texture space for a part,
  pinned islands of the part move too). The system prompt asks for suggestions when Patina can't.
- Data (`tasks_r6.py`): texture sets (per slot or UDIM, "already has one" when the layout is per
  slot), texture space, resolution and layout, glow, metallic, selections, windows, text stencils,
  questions answered from the context, and 19 kinds of things Patina doesn't do (shape, objects,
  sculpting, animation, video, artwork, AI images, files, uploads, second UV channels, vertex
  colors, tiling textures, hair, engine shaders, undo...) answered in one sentence with up to three
  suggestions made from the model's own parts. 5,831 checked examples, 34,650 wordings.
- Round 5's can't-do and vague examples (no suggestions, some now done) are left out; 30% of the
  other older examples are rehearsed. Training: 28,304 examples (15,834 new), from round 5's adapter.
- A third hand-written test (`hard3_tests.py`, 69 requests, written before the data) with Randall's
  six requests on his own Mai body (`real_mai`, test only).

Results (Q4_K_M, Patina's CUDA build; round 5 on the same Patina and the same tests, the tests'
expectations updated where 0.12 now does what was refused before: exporting opens the export
window, the resolution is set):

| Test | Round 5 | Round 6 | Round 7 (ships) |
| --- | --- | --- | --- |
| Hand-written test 3 (69, round 6's abilities, Randall's requests) | 14.5% (harmful 10.1%) | 92.8% | 97.1%* (harmful 0%) |
| Hand-written test 1 (81) | 90.1% | 90.1% | 93.8% (harmful 0%) |
| Hand-written test 2 (60) | 91.7% | 95.0% | 96.7% (harmful 0%) |
| New request types, held-out scenes (600) | - | 96.3% | 96.7% |
| Older request types, held-out scenes (400) | 94.8% | 92.2% | 93.5% |
| Latency, GPU (median / p95) | 0.09 / 0.24 s | | 0.09 / 0.28 s |
| Latency, 8 CPU threads (median / p95) | | | 3.3 / 6.3 s (test 3), 3.7 / 8.4 s (test 2) |

\* Round 7 (58 min from round 6's adapter, 26,199 examples) added contrast cases for round 6's
misses on test 3 (painting a tattoo "myself" is set-up, not a refusal; two parts selected at once;
"spice it up" asks; a render opens the window while a video is refused; second UV channels; "too
shiny" is roughness, not metal) and rehearsed the older kinds round 6 had got worse at ("body only"
as a follow-up had become a texture set). So test 3's clean number is round 6's 92.8%.

Round 6 trained 64 minutes from round 5's adapter (28,304 examples; batch 4 x 8: batch 8 ran out
of GPU memory beside Schala and an open Patina). Still wrong now and then: the older misses (a
color for a slot read as the paint color, three-way "X for the Y" lists, "the whole shield"),
cleaning up that hides only some weathering layers, and "paint a dragon on her back for me" sets
you up to paint it rather than saying it won't paint it itself.

File `patina-assistant-1.7b-q4_k_m.gguf` = round 7, 1,107,408,640 bytes, SHA-256
`c0105bb18bceb516259312df7fc2a3e6ca45a8ccbadad5eefd1f6b214c89e9e0`; round 5 kept as
`patina-assistant-1.7b-r5-q4_k_m.gguf`, round 2 as `-r2-`.

### What is kept in ~/patina-ml (3.7 GB, 2026-10-15)

- `release/`: the shipped model (round 7) and rounds 5 and 2, for going back.
- `data/paraphrases-r5mix.jsonl` (every checked example of rounds 2-5 with its wordings),
  `data/r6/` (rounds 6 and 7: canonical examples and wordings), the three hand-written tests,
  `test6-old` / `test6-new` (the held-out sets above), the paraphrase cache.
- `runs/r7/adapter` and `runs/r7/data` (its train and validation split): to continue from round 7.
- Deleted, and how to get them back: the base model (`snapshot_download('Qwen/Qwen3-1.7B',
  revision='70d244cc86ccca08cf5af4e1e306ecf908b1ad5e')`, 15 seconds), older runs and adapters,
  merged models and intermediate GGUF files, the combined mixes (`mix_r6.py combine` / `combine7`)
  and built splits (`build_dataset.py`).

## A CUDA build

The CUDA 13.3 toolkit is unpacked in `~/cuda-13.3` from NVIDIA's per-component archives (no root
needed; it matches the 610 driver's CUDA 13.3). Fedora's gcc 16 is newer than nvcc 13.3 allows,
so the build passes `-allow-unsupported-compiler`; the GPU tests pass and the answers match.

    export PATH=$HOME/cuda-13.3/bin:$PATH CUDAToolkit_ROOT=$HOME/cuda-13.3
    cmake -S . -B build-cuda -G Ninja -DCMAKE_BUILD_TYPE=Release -DPATINA_ASSISTANT_CUDA=ON \
      -DCMAKE_CUDA_ARCHITECTURES=120 -DCMAKE_CUDA_COMPILER=$HOME/cuda-13.3/bin/nvcc \
      -DCMAKE_CUDA_FLAGS=-allow-unsupported-compiler
    cmake --build build-cuda -j

The release packages stay CPU-only (they must run on machines without CUDA).

## Decisions

- 2026-10-14: the model is not published anywhere for now (the manual's link spot stays marked),
  and the Assistant is not coming to Patina Web.
- The Assistant's job is setting things up (layers, masks, materials, settings) so the user
  drives the result without hunting through menus, not doing the artistic work on its own.

## Things for Randall

- The 50 requests in your own words, on your own models (Phase 5 says these are the best
  signal there is). A file `~/patina-ml/data/user_requests.txt` with one request per line and
  the model it is about is enough.
- `sudo dnf install glslc` enables a Vulkan build of the Assistant. (The GUI tests and the manual's
  screenshots run without Xvfb now: a rootful `Xwayland :5` inside `kwin_wayland --virtual`, with
  `env -u WAYLAND_DISPLAY SDL_VIDEODRIVER=x11 DISPLAY=:5 PATINA_SHOTS_DISPLAY=:5`. While the desktop
  session is locked the NVIDIA driver draws that window at 1 frame per second, so add
  `__GLX_VENDOR_LIBRARY_NAME=mesa LIBGL_ALWAYS_SOFTWARE=1` then.)
