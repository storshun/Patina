# Patina 0.4: what makes texturing faster for an artist

How an artist uses Patina, and where it costs time today:

1. **Bring a model in.** The model comes from Blender (bridge or `.glb`). Its UVs are often missing, overlapping,
   stretched, or at inconsistent scales.
2. **Prepare the UVs.** Mark seams, unwrap, check stretching and texel density, pack. Today this means knowing
   the seams-then-unwrap workflow and judging density by eye with the checker.
3. **Bake mesh maps.** AO and curvature drive the smart masks. Game assets usually also need the detail of a
   high-poly sculpt baked into the low-poly model as a normal map. Patina could not do that.
4. **Texture.** Base materials per part of the model, then wear, dirt and hand-painted detail. Parts are often
   already separated by Blender material slots, but Patina ignored those.
5. **Review.** Change lighting, look from all sides, and show the result to someone else.
6. **Export and iterate.** Re-export after every change, re-import the mesh after edits in Blender, keep versions.

The hit list follows. **0.4** means it is built in Patina 0.4.0, **0.4.1** in 0.4.1, and so on; **later** means it is planned.

## A. UVs: the biggest hurdle for people new to texturing

| # | What | Why | Status |
|---|---|---|---|
| A1 | **Island brush**: paint over part of the model, press Enter. Patina cuts seams around the painted area, unwraps it, turns it upright, scales it to the right texel density, finds empty space in the layout for it, and keeps existing paint in place | Unwrapping becomes "paint the part you want as one piece", with no seam theory needed | **0.4** |
| A2 | **Texel density targets**: *fit the texture*, *1 texel per screen pixel* (the model framed at 1080p, i.e. under normal viewing), *match the existing layout*, or a custom px/m. Readouts and warnings when the texture is too small | "How big should this island be?" gets a number instead of guesswork | **0.4** |
| A3 | **Density and stretch view modes**: heat maps of texel density against the target, and of UV stretching | The checker shows that something is off; a heat map shows where and by how much | **0.4** |
| A4 | **Find and fix overlapping UVs**: highlight overlaps, and with one click move the overlapping islands into free space | Overlaps make paint appear in two places (the most common "bug" report) | **0.4** |
| A5 | **Orient islands**: turn islands upright (world up = texture up) or along their longest side | Wood grain, text and tiles run the right way | **0.4** |
| A6 | **Pack into free space**: place selected islands without moving the others | Small fixes no longer reshuffle a finished layout (and its paint) | **0.4** |
| A7 | **Grow / shrink selection** (Ctrl+Numpad + / -) | Refine a painted region quickly | **0.4** |
| A8 | **Rectify quad islands** (straight grids, rows and columns at their 3D lengths) and **Mirror UVs** (one half stacks on the other); UDIM tiles | Straight strips for trims, belts and planks; symmetric models use their texture once | **0.7** (UDIM: 0.8) |

## B. Baking

| # | What | Why | Status |
|---|---|---|---|
| B1 | **Bake from a high-poly mesh**: tangent-space normal map, AO and material IDs from the sculpt, with a cage distance. The baked normal map becomes the base of the stack | This is the core of a game-asset workflow | **0.4** |
| B2 | **Material ID map** from Blender material slots (low- or high-poly) | Parts are already separated in Blender, so use that | **0.4** |
| B3 | **Thickness** (Thin parts smart mask), **bent normals**, **curvature from the baked normal map**; curvature measured at a fixed scale so low-poly models get clean edges | Translucency and SSS maps, better ambient lighting in engines, wear that follows sculpted detail | **0.7** |

## C. Masking and painting

| # | What | Why | Status |
|---|---|---|---|
| C1 | **Fill by material slot** (Fill tool mode) and the **Material slot** smart mask | One click per part of the model | **0.4** |
| C2 | **Mask from selection**: a layer mask from the selected faces | Islands and selections double as masks | **0.4** |
| C3 | **Facing smart mask**: surfaces that face a direction (snow and dust on top, drips below) | More natural than the height gradient | **0.4** |
| C4 | Smudge brush, stencil / decal projection, text tool, mask effect stacks, layer thumbnails | | **0.6** |
| C5 | **Topology lock** (brush bar toggle, Alt+T): a stroke only paints the connected piece of mesh it starts on | Eyes, buttons and straps stay clean without masking | **0.4.1** |
| C6 | **Island lock** (brush bar toggle, Alt+L): a stroke only paints the UV island it starts on | Paint a panel right up to its seam | **0.4.1** |
| C7 | **Drag and drop into the layer stack**: materials onto a fill layer (it takes the material, its smart mask stays) or between layers (a new fill layer); smart materials and smart masks as tiles in the Materials panel, dragged into the stack or onto a layer; a fill layer's material and a layer's mask dragged onto other layers; Ctrl+drag copies a layer; dropped on the model, above the selection | Randall: "I can't seem to figure out how to replace a material that's a fill layer"; "all layers in smart materials with that level of simplicity" | **0.22.5** |

## D. Look development and review

| # | What | Why | Status |
|---|---|---|---|
| D1 | **Rotate the environment light** with Shift + right drag | Check how the material reacts without opening a panel | **0.4** |
| D2 | **Render image** (F12): PNG at any size, transparent background, turntable frames | Portfolio shots and sharing work in progress | **0.4** |
| D3 | Split / compare view | | **0.6** |

## E. Iterating and managing files

| # | What | Why | Status |
|---|---|---|---|
| E1 | **Save incremental** (Ctrl+Alt+S → `name_v002.patina`) | Versions without "Save As" every time | **0.4** |
| E2 | **Export again** with the last settings (Ctrl+Shift+E); export settings are stored in the project | Edit → re-export in one key | **0.4** |
| E3 | **Watch the mesh file**: when the `.glb` / `.obj` changes on disk, offer *Update mesh* (layers are kept) | Blender users who don't use the bridge | **0.4** |
| E4 | **Crash recovery**: offer the autosave on the next start | Never lose an afternoon | **0.4** |

## F. Getting started

| # | What | Why | Status |
|---|---|---|---|
| F1 | **Welcome screen**: recent files, the sample model, new primitives, choice of the Patina or Blender keymap, links to the manual | The first five minutes decide whether a newcomer stays | **0.4** |

## G. Working with other tools (0.5)

| # | What | Why | Status |
|---|---|---|---|
| G1 | **Substance exports import as they are**: packed maps (Unreal OcclusionRoughnessMetallic, ORM / ARM, glTF metallicRoughness, Unity MaskMap / MetallicSmoothness), several texture sets in one folder, EXR / 16-bit maps, the Unreal preset's DirectX normals | Most purchased and studio materials come from Substance | **0.5** |
| G2 | `.sbsar` / `.spsm` / `.spp`: a clear explanation (Adobe's engine only) instead of a failed open | Saves a support question | **0.5** |
| G3 | **HTTP API**: every command over HTTP/JSON, background **jobs** with progress and cancel, workspace file upload / download, OpenAPI description; this machine by default, the **local network only** (private addresses, token) with `--lan` | Pipelines, asset servers and render farms | **0.5** |
| G4 | **Batch mode**: `patina run job.json --set name=value`, exit codes and a JSON report; `patina call <command>` | CI, cron and shell loops | **0.5** |
| G5 | Python client (`clients/python/patina_api.py`) and a farm dispatcher example | A starting point for in-house tools | **0.5** |
| G6 | Rendering `.sbsar` through Adobe's `sbsrender` when it is installed | Only possible with Adobe's tools | later (on request) |

## H. Pen tablets (0.5.1)

| # | What | Why | Status |
|---|---|---|---|
| H1 | Every pen and mouse sample feeds the stroke (not one per frame) | Fast strokes keep their shape and pressure at 130-266 Hz tablets | **0.5.1** |
| H2 | Pen and mouse told apart by the event source; mouse strokes always at full pressure | No stray pressure after putting the pen down | **0.5.1** |
| H3 | Pressure curve: feel (soft / firm), ignore below, full pressure at | Every pen and hand is different | **0.5.1** |
| H4 | Eraser end erases; tilt turns the tip; barrel buttons mappable (right / middle click, swap colors, undo) | Use the pen like a pen | **0.5.1** |
| H5 | Pen Tablet Test window, report, `pen_info` | Check a tablet, and report what arrives | **0.5.1** |

## I. 0.6: the rest of the "later" list, and more model formats

| # | What | Why | Status |
|---|---|---|---|
| I1 | **Mask effect stacks**: paint, smart mask, fill, noise, levels, blur, invert; blend modes and opacity per effect | Most of what a node graph is used for in masks, without a graph | **0.6** |
| I2 | **Layer thumbnails** (content and mask) and **instances** (linked copies with their own mask, Alt+D) | Find layers at a glance; reuse a layer without duplicating its paint | **0.6** |
| I3 | **Smudge** (J), and **blur / smudge across UV seams** (also across the texture border of wrapped islands) | Blending without hard lines at seams | **0.6** |
| I4 | **Stencils** (mask or decal, invert, tile, hold T to place, project in one step) and a **text tool** (bundled fonts, outline) | Logos, labels, stenciled lettering | **0.6** |
| I5 | **Notes** pinned to the model, saved in the project (Note tool N, Notes panel, commands) | Reviews and hand-over | **0.6** |
| I6 | **Compare view**: snapshot or another view mode left of a draggable divider (\\) | See exactly what a change did | **0.6** |
| I7 | **FBX** (ufbx: FBX 6 and 7, binary and ASCII), **STL** and **3MF** import, in meters and Y up; meshes without UVs are unwrapped on open | Models from Maya, Max, ZBrush, game engines and 3D printing | **0.6** |
| I8 | **Symmetric islands**: with Sym X / Y / Z the Island brush paints both halves, and the mirrored half gets the same (mirrored) UVs, stacked in one place | Symmetric models use their texture space once | **0.6** |
| I9 | Rendering `.sbsar` | Needs Adobe's engine (`sbsrender`, see G6): the format is compiled and undocumented | later (G6) |

## J. 0.7: bakes, UV clean-up, layered PSD, very large exports, and a beginner's guide

| # | What | Why | Status |
|---|---|---|---|
| J1 | **Thickness bake** (CPU rays into the model) and the **Thin parts** smart mask; exported for SSS / translucency | Ears, leaves and fabric edges glow or wear differently | **0.7** |
| J2 | **Bent normals** baked with the AO, exported in tangent space (OpenGL or DirectX) | Engines use them for specular occlusion and ambient lighting | **0.7** |
| J3 | **Curvature** measured with short rays at a fixed scale (low-poly boxes get thin edges, spheres stay neutral) plus the **high-poly normal map's detail** | Edge wear on hard-surface and baked models | **0.7** |
| J4 | **Rectify quad islands** and **Mirror UVs** | See A8 | **0.7** |
| J5 | **Layered PSD export**: a file per map, Patina layers as PSD layers with masks, opacity, blend modes and groups | Hand-off to Photoshop, Krita, GIMP and Affinity | **0.7** |
| J6 | **16K and 32K export**, rendered in tiles with materials and smart masks at full detail, PNGs streamed to disk | Hero assets, film, print | **0.7** |
| J7 | **Layer Mask view shows the effective mask** (painted × smart mask, or the effect stack) | See what the layer actually uses | **0.7** |
| J8 | **Beginner's guide to masks and smart materials** in the manual | For people who have never used Substance Painter | **0.7** |
| J9 | Seamless procedural materials (whole noise periods per tile) | Rust and others showed a seam at the tile edge | **0.7** |

## K. 0.7.1: sturdier UV editing (from testing)

| # | What | Why | Status |
|---|---|---|---|
| K1 | **Pinned islands**: kept through every UV edit; pack and unwrap place the rest around them; the Island brush pins what it makes; P / Alt+P | An island that is done can't be moved by accident | **0.7.1** |
| K2 | **Linked mirror halves**: moving either half of a symmetric island moves both; drifted halves are detected and restacked; unstack to make one side independent | Moving one half used to leave the copy behind and trigger overlap warnings | **0.7.1** |
| K3 | **Right-click menu** in the UV editor with every UV operation | Everything in one place, where the mouse is | **0.7.1** |
| K4 | A click on a selected island never moves it (drags start after a few pixels); hover info, pin and mirror markers, warnings with a fix button | Fewer accidental edits, clearer state | **0.7.1** |
| K5 | `uv_pin`, `uv_restack`, `uv_unstack`, `uv_islands` commands | The same from Claude, scripts and the HTTP API | **0.7.1** |
| K7 | **Mirror matching along the mesh**: exact pairs first, then grown across the mesh by neighbors (quad edges, not split-quad diagonals) | Real character models whose halves differ slightly in shape still had mismatched faces | **0.7.3** |
| K8 | **Island brush in Patina Web**: mirroring, orientation, density, pinning, linked halves, UV right-click menu | The browser version had no UV tools besides Unique UVs | **0.7.3** |
| K10 | **No more ghosting**: tri-planar seams by height (crisp, nothing shows through), and stacked mirror halves leave the texels to the unwrapped half (desktop and web; the web brush paints both halves) | Diamond plate and other sharp patterns showed a faint second copy | **0.7.4** |
| K9 | **Connect to Claude app** button and `patina --setup-claude` | The desktop app needs Patina in its config file; editing JSON by hand is error-prone | **0.7.3** |
| K6 | **Mirror matching by corners**: symmetric selection and symmetric islands work when the two halves are triangulated differently, and with vertices split at seams | Imported character models: faces were selected but not stacked, and mirrored selections had holes | **0.7.2** |

The node graph (wishlist: "Nested effects, node graph") stays undecided until 0.8: the effect stacks cover masks, and
the open question is whether layer-level graphs add enough on top.

## L. 0.8: UV tools and a bigger library

| # | Feature | Why | Version |
|---|---|---|---|
| L1 | **Packing by shape**: islands rasterized by outline, placed where they touch their neighbors best (4 rotations), scale found by bisection; around pinned islands; the box packer runs too and the better layout stays | Bounding boxes waste the hollows of curved and L-shaped islands | **0.8** |
| L2 | **Seam brush**: drag along the model (shortest edge paths between the vertices under the cursor, welded across split vertices), Shift+click continues, Ctrl clears; `uv_seams` path mode | Clicking seams edge by edge is slow | **0.8** |
| L3 | **Cylinder** (cut where the angle around the tube's local center wraps, then LSCM) and **Sphere** unwraps | Limbs, horns, pipes, eyes | **0.8** |
| L4 | **Relax**: islands solved again (LSCM) and fitted back onto their old place, size and angle | Remove stretch without redoing the layout | **0.8** |
| L5 | 16 new generators, 74 materials (Character and Stylized categories), 11 smart materials | A wider starting point, especially for characters | **0.8** |
| L6 | Texture sets per material slot, then UDIM tiles | Characters with body / hair / clothes textures; film and game pipelines | **0.9.0**: per slot, UDIM, or both; detected on load; one atlas, one layer stack; UV tools per tile; per-tile export; a Blender material per slot and tiled UDIM images |
| L7 | Layered PSD export per texture set / UDIM tile; texture sets in Patina Web | The rest of L6 | **0.10.0**: PSD per set / tile. Texture sets in Patina Web: later |
| L8 | **More procedural materials**: 16 generators (stripes, checks, dots, animal prints, cracked earth / lava, flagstones, pebbles, rock layers, crackle, chainmail, damascus, circuit, stained glass, mosaic, sequins, paper), 52 materials (Patterns and Paper categories); grime, worn edges and warp for every generator; the editor names each generator's settings (`list_generators`) | Randall: "Are there more texture designs, patterns, and properties that we can add to the procedural generation palette?" | **0.24.0** |
| L9 | **More masks**: Pattern (any material as a mask, dropped onto the mask), Grow / shrink and Edges effects; Gradient (any direction) and Drips smart masks; noise as clouds, cells, cracks, ridges or streaks | The same request, for the masks that shape materials | **0.24.0** |
| L10 | **More material properties**: opacity (lace, nets, leaves, cut-outs), sheen (velvet, satin), clearcoat (car paint, lacquer) and subsurface (skin, wax, leaves), painted like the others, in every material, exported and sent to Blender | Offered after 0.24; Randall: "Your next planned features sound good" | **0.26.0** (Patina Web still shows them solid) |
| L11 | **More patterns**: fur, feathers, leaves, rope, damask, ice, crosshatching, lace, net and mesh, hair cards, velvet (corduroy, devoré); *Cut out holes*; 45 materials | The same | **0.26.0** |

## N. 0.10: search, checks, automation (each can be switched off)

Every feature here is listed in *Preferences › Features* and can be switched off; switched off, its menus, buttons,
shortcuts and commands go away. Defaults are on.

| # | Feature | Why | Version |
|---|---|---|---|
| N1 | **Feature switches** (`features` command, Preferences › Features, saved with the session) | Try new things without being stuck with them | **0.10.0** |
| N2 | **Command palette** (Ctrl+Shift+P / F3): actions, windows, view modes, commands, materials, brushes, layers, recent files; recent first; word-start matching | Find anything without knowing the menu | **0.10.0** |
| N3 | **Mesh symmetry check**: distance from each mirrored vertex to the surface, heat map view mode, worst spots in Blender axes, per slot / height; `mesh_symmetry`; Blender's **Select Asymmetric Vertices** | Asymmetric characters break mirrored painting, symmetric UVs, shape keys and rigs | **0.10.0** |
| N4 | **Set lock** (stroke stays on its texture set / material slot), **Isolate** (other sets ghosted, strokes kept off them), Current set, Select faces | Painting one part of a character next to another | **0.10.0** |
| N5 | **Auto texture**: a library material per material slot from its name, fill layers masked per slot | A textured first pass in one click | **0.10.0** |
| N6 | **Macro recorder**: record commands (menus, scripts, Claude), replay, save as a `patina run` job | Repeat a texturing recipe on many assets | **0.10.0** |
| N7 | **Blender: Auto Update Mesh**: re-send after edits settle (off by default; ignores Patina's own UV write-back) | No more clicking Update Mesh after every change | **0.10.0** |
| N8 | Layered PSD per texture set / UDIM tile | See L7 | **0.10.0** |
| N9 | Island brush / Select strokes are undo steps; the click confirming F / Shift+F no longer paints | Reported: a confirm click painted a selection that couldn't be undone | **0.10.1** |

## O. 0.11: the Patina Assistant

A text box where a request in plain words becomes Patina commands, worked out by a small model on the user's own
computer, previewed and kept as one undo step. Switch: *Preferences › Features › Patina Assistant*. The working plan
and its status are in [ASSISTANT_PLAN.md](ASSISTANT_PLAN.md).

| # | Feature | Why | Version |
|---|---|---|---|
| O1 | **Parts of a model**: material slots, Blender vertex groups (new `.ptmesh` chunk, add-on 0.6.0), connected pieces, and `region:head` / `region:body` found from the shape (a neck between a wider head and shoulders; looked for again under long hair); `mesh_parts`, `select_part` | Requests name parts the way people do: the jacket, the left arm, the head | **0.11.0** |
| O2 | **Tint and material layers on a part with a smart mask stack** (`add_tint_layer`, `add_material_layer`), `update_layer_mask`, `apply_smart_material` with a region | One request, one command: "slightly redder in the skin's creases" | **0.11.0** |
| O3 | **One UV island per part** (`uv_island_per_part`): cuts on the part borders, through handles and odd edges, along the back and the limbs; LSCM then ARAP; the best of several unwraps | "One island for the head and one for the body", also on messy generated meshes | **0.11.0** |
| O4 | **Undo groups**: a command or a request made of several edits is one undo step (and one macro step) | Ctrl+Z takes back the whole request | **0.11.0** |
| O5 | **The Assistant**: llama.cpp (b11540, vendored), Qwen3 1.7B fine-tuned with LoRA in five rounds on ~60k checked requests (looks, several changes at once, paint set-up, adjustments, layers by content, the brush), output held to a grammar built from the open project (it can't name a command, part or material that doesn't exist); preview, a slider per layer (and color, adjustment value), follow-ups, questions with part buttons, examples from the model's parts, local feedback file | Texture painting without knowing where everything is | **0.11.0** |
| O6 | **Training and evaluation tools** (`tools/assistant/`): scenes, ~45 task types with checkers, paraphrases from a local model, the fine-tune script, end-to-end evaluation through Patina | The next model is a rerun, not a project | **0.11.0** |
| O7 | NVIDIA 610: vertex buffers are replaced, not updated in place (fills, UV moves, texture sets and UDIM painting did nothing) | Found on an RTX 5090 | **0.11.0** |
| O9 | **Texture sets from a selection** (`texture_set_from_faces`, *UV › New Texture Set from Selection*): a part gets a material slot (texture set) of its own, paint kept, each set packed to fill its square; Blender gets the slot with the next send (add-on 0.6.1). Faces with no UV area no longer catch UV clicks or make UDIM tiles; saving, autosave (in the background), opening, UV moves and exports without long stops | A head texture and a body texture, as games use them | **0.11.1** |
| O10 | **The Assistant, round 6**: texture sets, texture space for a part (`uv_density` scale), the resolution and layout (a last step that clears the undo history, run with *Do it*), glow (emissive tints), metallic adjustments, selections, opening the right window (`open_window`: export for Unreal, render, symmetry, high-poly bake, Blender...), text stencils to paint through, questions about the project; and when Patina can't do something, one sentence why and up to three close requests it can do, as buttons (`suggest`) | It said no to less, and stopped doing something else when it couldn't | **0.12.0** |
| O8 | In-app model download; the Assistant in Patina Web; Vulkan / CUDA builds of it | Open decisions | later |

## P. 0.13: retopology

Randall: "a CozyBlanket Pro style retopology tool, borrowing from our Island Painter, allowing the user to
define mesh density per m² and selectively increasing density, retopologizing in quads along the way".
Design and status: [RETOPO_PLAN.md](RETOPO_PLAN.md).

| # | Feature | Why | Version |
|---|---|---|---|
| P1 | **Patches painted on the high-poly** (*Retopology* menu, Patch brush: a stroke on a patch grows it, on bare model starts one, Shift new, Ctrl erase, Sym mirrors, one undo step each); *Suggest patches* (geodesic cells, split where they aren't disks or rings), *Fill gaps*; kept between sessions by mesh | You decide where the edge loops go, as in CozyBlanket | **0.13.0** |
| P2 | **Quads per m² and a density brush**: edge counts follow length × density; loops are added where a patch widens or the density rises and end in three-to-one steps, inside patches too | More quads on the face and hands without them running round the whole model | **0.13.0** |
| P3 | **The fill**: corners chosen per patch (Ys become four-valent where one patch runs straight), even counts so every patch fills, grids for four sides, poles for three, five and six, rings as tubes; flattening that keeps areas; specks, gaps and single-point touches tidied; built in the background (2 to 4 s for two million triangles) | Clean quads, not a remesh | **0.13.0** |
| P4 | **Using the quads**: export .obj, send to Blender as a new object (add-on 0.7.0), or make them the model with the high-poly baked on | Straight on to painting | **0.13.0** |
| P5 | Guide strokes that steer the flow, corners placed by hand, PolyBuild-style editing of single quads, symmetry of the result | CozyBlanket's other tools | flow strokes in **0.14.0**; the rest later |

## Q. 0.14: retopology that follows the shape

Randall, after 0.13: "we need to be a lot higher quality retopology than that", with about 98% of the
vertices four-edged; "I'm expecting the user to set the locations edge flow should change ... scalable
zones that define the direction of flow and each zone has its own parameter for resolution; ... the more
input there is the more detailed the outcome becomes". Design and status: [RETOPO_PLAN.md](RETOPO_PLAN.md).

| # | Feature | Why | Version |
|---|---|---|---|
| Q1 | **Quads along the model's shape** (QuadriFlow vendored with Blender's changes, Eigen, LEMON): six fields at once (seeds × smoothing), the one with the fewest poles and no failed parts kept; pairs of three- and five-edge poles collapsed; count calibrated to the quads per m² | 95-99% four-edged vertices with nothing drawn | **0.14.0** |
| Q2 | **Zone lines** (Lines tool, seam-brush style): exact edge loops, laid into the working triangles and again into the quads; they split the model into **zones** | You set where the edge flow changes | **0.14.0** |
| Q3 | **Zone resolution** (Zone tool and slider): zones of different resolution made apart (each with a margin past its lines), joined by a row of three-to-one steps along the line (one triangle for an odd difference); falls back to a size field where a zone is narrower than its quads or three zones meet | Each zone its own density, changing at the line | **0.14.0** |
| Q4 | **Flow strokes** (Flow tool) turning the field in a band about a quad wide; the density brush kept for gentle fine tuning | Which way the quads run | **0.14.0** |
| Q5 | **Working shell** for big or messy models (over 300,000 triangles): a voxel shell just outside the surface (outside only), Taubin-smoothed, projected; the quads go back onto the model | Two-million-triangle generated meshes in a few seconds | **0.14.0** |
| Q6 | **Free lines on the surface** (Lines and Flow like a grease pencil: every pointer sample raycast; ends snap onto lines, loops close, overshoots trimmed for the build; Ctrl+click removes); zones from the faces the lines cross (adaptive walk, no gaps); lines drawn on the GPU; soft surface density brush with Sym | Randall: drawing along edges made ends impossible to meet | **0.14.1** |
| Q7 | **Big models quick**: the UV fingerprint once a frame (fast, parallel), UV hover through a grid, zones and overlays rebuilt only when their own inputs change, no tangents twice on opening | Randall: "Hi-poly models are impossibly slow to work on" | **0.14.1** |
| Q8 | **The model's own surface** for big models (quadric decimation in parallel pieces, per-vertex target from the local quad size; coarse models refined where needed): fingers apart, no shrink-wrap; parts too thin for the quads marked in red and said | Randall: hands webbed, torso and legs poor with the shell | **0.15.0** |
| Q9 | **Live preview**: a quick build a moment after each change while the tools are in use, canceled by the next change (also inside QuadriFlow's loops), drawn faintly; Make quads for full quality; working surface kept between builds; zones built in parallel | Randall: "see a ghost of quads ... in real time before committing" | **0.15.0** |
| Q10 | **Symmetry**: Mirror X/Y/Z across the model's middle for lines, flow strokes, density and zone resolutions; a middle deadzone where lines join their mirror images (like Blender's mirror clipping); quads still over the whole model | Randall's spec | **0.15.0** |
| Q11 | Junctions of three zones of different resolution, zones narrower than their quads, big steps without triangles | Randall's target layouts | **0.16.0** |
| Q12 | **Rings between two lines**: concentric loops round eyes, mouths, along limbs; small zones inside closed with rings | Randall's Suzanne target, CozyBlanket guides | **0.16.0** |
| Q13 | **Loops view** (poles, the edge loop under the pointer), builds twice as quick, the preview is the result | CozyBlanket videos | **0.17.0** |
| Q14 | **Zones for thin parts**: thin tubes (fingers) found, ringed where they meet the rest, given a resolution for six quads round | Randall's girl (fingers left out) | **0.18.0** |
| Q15 | **Grids and planned rings**: discs inside rings and holes where zones meet closed with grids (neck cuts for odd shapes); ring counts planned along each chain of rings; rings kept for untidy lines | Randall's Suzanne target (eyes, mouth) | **0.19.0** |
| Q15b | **Fewer poles**: pole pairs where zones join taken out (lines and rings untouched); a disc's grid a ring in from its line (poles inside, the line clean) | Randall's 98% four-edged target | **0.19.1** |
| Q15c | **Rings only where they pay**: a ring that would need many steps made with the zone beside it (chosen by planning the chains with and without it); plans use the loops rings will share; odd discs still grids | Randall's 98% four-edged target | **0.19.2** |
| Q15d | **Zone groups made once**: a group of zones nothing changed in keeps its quads (the head drawn on, the body and hands not made again; an undo is instant) | Edits that stay local, as in CozyBlanket's videos | **0.19.7** |
| Q15e | **Lines followed exactly**: QuadriFlow holds each line along the drawn line's own way (not the working triangles' zigzag) and no row jumps across a line; each try is judged also by how much of the lines its edges follow; rings take the edge loop of the zone beside them (a few stray points put onto the line); tries leaning off their own field (twisted limbs) passed over | Randall's 98% four-edged target; CozyBlanket: guides become loops exactly | **0.20.0**, 0.20.1 |
| Q15f | **Folds smoothed out**: faces turned over (against the surface or the faces round them) have their corners moved to the middle of their neighbors, kept piece by piece where it helps | Clean quads where zones meet | **0.20.8** |
| Q15g | **Fewer poles**: the quads' counts solved again from their own layout (one min-deviation flow, libSatsuma), short runs between a three- and a five-edged pole drawn toward nothing so the pair cancels; kept only when the finished quads have fewer poles, no more folds and about the same count | Randall's 98% four-edged target | **0.21.0** |
| Q15h | **Lines that stay out of the way**: lines behind the model darkened, hidden or shown (they used to show through: the depth offset was a share of the depth range); line-end snapping can be turned off; confirmed lines (darker, never snapped to); Make quads pinned at the window's foot | Randall: "all lines are basically visible through the mesh"; tight folds | **0.22.0** |
| Q15i | **Shift+click round what faces you**: where no part goes round (a breast, the face, a knee), the cut through the click parallel to the screen; Shift+wheel switches; noses and lips still to come (a crease-following loop) | Randall: no circumference line on breasts, noses, lips or faces | **0.22.1** (breasts, faces) |
| Q15j | **Mirrored quads, and flow strokes as loops**: with Mirror X/Y/Z the quads are made on one side and mirrored (the middle laid as an edge loop; faces across it cut; pieces not the same on both sides kept as made, or the whole model made when none is); Strokes make edge loops (flow strokes as exact loops without zones); widget ID clashes fixed and tested | Randall: "clearly a lack of symmetry between X+ and X-"; "Flow lines ... seem like they do very little"; the Show: Density error | **0.22.3** |
| Q15k | **Flow strokes as loops by default; mirrored creases**: Strokes make edge loops on by default; faces pressed flat onto the middle left out (they were doubled); a line near the middle joins it over its whole near run (no star pole); small holes filled beside hair | Randall: loops by default "Yes"; Mai with separate hair | **0.22.4** |
| Q15l | **A change remakes only the quads near it**: the last quads kept exactly beyond a few quads of what changed (lines, strokes, density, a zone's resolution), joined to the new ones by a row chosen for the fewest poles; Remake all makes everything again; Keep quads away from changes (on by default) | Randall: "the mesh is getting jumbled in zones where the preview looked very clean" | **0.23.0** |
| Q16 | Frozen zones; PolyBuild-style editing of single quads; thin strands of hair | CozyBlanket videos | later |

## R. Post-process your models the way you would a photo

Randall (2026-10-18): a mesh post-processing heaven; anything that fits that description goes on the roadmap.
Randall (2026-10-19), the wording to use: "Post-process your models the way you would a photo. Non-destructive,
layered, and scriptable."

Photographers make a picture in the camera and **develop** it in a photo editor: a library of everything they
have shot, settings that never touch the original, sliders instead of tools, presets that run on hundreds of photos at
once, before / after, and an export that knows where things go. Patina does the same for models made in Blender (and
for scanned and AI-generated models): **bring them in, check them, develop them, finish them, send them out**, one at a
time or by the folder. Blender stays the place where shapes are made; Patina is where they are made ready.

### R-a. Library: everything you have made, and Patina's materials everywhere

| # | Feature | Why | Version |
|---|---|---|---|
| R1 | **Patina's materials in Blender's Asset Browser**: one button (in Blender's Patina tab or Patina's Blender Link window) bakes every material through Patina and builds a *Patina Materials* asset library: catalogs by category, Patina's previews, box mapping like Patina (or UV mapping), your own materials too; run again to update | Randall: "Is there a way for the material library to also be translated directly to blender as part of the library in blender?" | **0.25.0** |
| R2 | **Asset catalog**: folders of models (.glb .gltf .fbx .obj .stl .3mf .patina) watched; a grid of thumbnails that turn on hover; ratings, flags, color labels, tags, collections and smart collections ("over 50k triangles", "no UVs", "not textured yet"); search; open with a double click | A photo library, for models: find any asset in seconds | later |
| R3 | **Mesh info** (a photo editor's histogram and metadata): triangles, quads and n-gons, vertices, parts, UV islands, UV coverage and overlap, texel density spread, texture sets, materials, size in meters, memory; problem counts with links | Know what you have before changing it | **0.27.0** (texel density spread and memory: later) |
| R4 | **Mesh check**: holes (open borders), non-manifold edges, flipped faces, degenerate and zero-area faces, duplicate vertices and faces, loose specks, overlapping and zero-area UVs, missing normals; shown on the model as a view mode, listed with a fix each | Problems found before they reach an engine or a printer | **0.27.0** (also bowtie vertices, inside-out parts, open seams, odd scale) |

### R-b. Develop: non-destructive post-processing with sliders

| # | Feature | Why | Version |
|---|---|---|---|
| R5 | **Develop stack**: the original mesh is kept; steps run on top of it in order, each with sliders, on / off and reset; a slider change rebuilds the result in the background (like the retopology preview); saved in the project; paint follows the mesh | A photo editor never touches the original; every change can be taken back or tuned later | later |
| R6 | **Clean up**: weld by distance, remove degenerate faces, duplicate faces and loose parts under a size, fill holes up to a size, make normals face outward | Every imported, scanned or generated model needs it | **0.27.0** as Mesh Check fixes (paint kept, *Back to before the fix*); as a develop step with R5 |
| R7 | **Shading**: smooth by angle, weighted normals for hard surfaces, sharp edges from UV seams or material borders | Hard-surface models look right in engines | later |
| R8 | **Decimate**: to a triangle count, a ratio or a screen size, keeping UV seams, borders, sharp edges, material borders and symmetry; a heat map of what moved | Game, web and AR budgets | **0.29.0**: a ratio or a count, UV seams, material and open borders kept, the textures unchanged (symmetry, screen size, heat map: later) |
| R9 | **LODs**: LOD0 to LOD3 by ratio or screen size, previewed by distance, exported with engine naming (`_LOD0`, FBX LOD groups, glTF `MSFT_lod`) | Engines want them and nobody likes making them | **0.29.0** in Export Model (glTF `_LOD` meshes and `MSFT_lod`, OBJ files; preview by distance, FBX: later) |
| R10 | **Smooth, relax and denoise** for scans and generated models (keeps volume and features), **remove floaters**, **symmetrize** (mirror a half, weld the middle), **voxel remesh** | AI-generated and photogrammetry meshes are lumpy, noisy and full of specks | later |
| R11 | **Transform**: apply scale, units, up axis; origin to the base, the center or the world origin; put it on the ground; turn it upright on its largest flat base | Models arrive at any scale and any angle | **0.30.0** (units, scale, turn, origin, ground; upright on its base: later) |
| R12 | **Retopology and bake as develop steps**: the quads (0.13-0.23) made from the original, and the original's detail baked onto them (high-poly bake, 0.4) | Detail survives every reduction | partly (the tools exist) |
| R13 | **UVs**: unwrap when there are none (per part, by angle, or the Island tools), pack to a texel density, **lightmap UVs** in a second UV map (no overlaps, padding for the lightmap size) | Engines need lightmap UVs; generated models have no UVs | partly: **lightmap UVs** in a second UV map (0.35.0); unwrap when there are none (0.1, smart project); packing to a texel density (UV tools) |
| R14 | **Texture develop** (a photo editor's basic adjustments, for the finished textures): exposure, contrast, highlights and shadows, white balance, vibrance, saturation, hue per color, sharpening, grain; looks as presets (Weathered, Clean, Stylized) | One slider to fix a whole texture set's color | partly (0.33.0): white balance, exposure, highlights and shadows, vibrance as adjustment layer settings; hue per color, sharpening and grain later |
| R36 | **Agents drive every step**: every tool's settings with range, default and effect; guides and prompts over MCP; tools that measure (mesh_check, texture_check, collision fit, reduce preview) so an agent can tune by numbers | The post-processing is as usable by Claude as by hand | **0.34.0** |
| R35 | **The model's own textures**: a textured model (glTF, OBJ with its MTL, FBX, embedded or beside it) opens looking as it did elsewhere: each material slot's color, roughness, metallic, normal, glow and opacity maps (and plain values) as a paint layer at the bottom of the stack | Game assets, scans and generated models arrive textured; everything else builds on that look | **0.31.0** (texture transforms; specular-glossiness read as roughness; vertex colors later, R16) |
| R15 | **De-light**: take the baked-in shadows and highlights out of scanned and generated textures (using AO and normals) | Scans and AI textures carry their lighting with them | partly (0.33.0): De-light from the ambient occlusion; light direction and large-scale shading later |
| R16 | **Vertex colors**: bake AO, curvature or the textures into vertex colors; start painting from vertex colors | Generated models often come with vertex colors only; some engines and printers use them | partly: vertex colors read from glTF, OBJ, FBX, PLY and Blender as the From the model layer, and PLY export with the base color as vertex colors (0.32.0); baking AO or curvature into them, and COLOR_0 in glTF export: later |
| R17 | **Texture transfer**: after decimation or retopology, project the original's textures (or vertex colors) and normals onto the new UVs | Keeps the look of a generated model on a clean mesh | later |
| R18 | **Collision meshes**: convex hull, convex pieces, box and capsule fits, exported with engine naming (`UCX_`) | Games need them | **0.34.0** (box, boxes, hull, parts, convex pieces; Unreal, Godot and plain names in glTF and OBJ; capsules later) |
| R19 | **3D print preparation**: watertight check and fix, wall thickness heat map (from the Thickness bake), hollow with drain holes, scale to a real size, split to fit the bed, STL / 3MF export | Printing is a common last step for models | partly: watertight check and fix (0.27), STL export and the 3D print recipe (0.30) |
| R20 | **Auto** (a photo editor's Auto button): clean up, normals, UVs if missing, a material per part from names (Auto texture, 0.10) and a check report, in one click | A good start in one click | later |

### R-c. Presets, sync and batches

| # | Feature | Why | Version |
|---|---|---|---|
| R21 | **Develop presets**: a develop stack, a smart material and an export preset saved together; built in: Game asset, Web / AR, 3D print, Scan clean-up, Generated model clean-up, Hero asset | Presets, as for photos: the same look on everything | **0.30.0** as recipes (steps are Patina commands; seven built in, your own) |
| R22 | **Copy, paste and sync settings** between models (choose the steps) | Sync settings, as for photos: fix one, fix them all | later |
| R23 | **Batch window**: drop a folder (or pick models in the catalog), choose a preset, run in the background with progress and a report; results beside the originals or in a folder; built on `patina run` jobs and macros | Hundreds of models without opening each one | **0.30.0** (a hidden `patina run`; the catalog later) |
| R24 | **Watch folder**: new files in a folder get a preset and are exported by themselves | Auto import, as for photos; a pipeline without scripts | later |
| R25 | **Variants** (virtual copies, as for photos): several versions of one model (LODs, colorways, clean and worn) sharing the original, each with its own develop steps and layers, exported together | Colorways and versions without copies of the file | later |

### R-d. Compare and history

| # | Feature | Why | Version |
|---|---|---|---|
| R26 | **Before / after** of the mesh (original and developed) in the Compare view, with the numbers of both | See what a step did | later |
| R27 | **Develop history and snapshots**: every change listed, named snapshots to go back to | History and snapshots, as for photos | later |

### R-e. Export and the way back

| # | Feature | Why | Version |
|---|---|---|---|
| R28 | **Export the model with its textures**: glTF / GLB, FBX, OBJ, USD; presets per engine (Unity, Unreal, Godot, Roblox, web, Blender); LODs and collision included; naming templates; triangle and texture size limits; mesh and texture compression | Today Patina exports textures; engines want the whole asset | **0.28.0**: GLB, glTF, OBJ with their textures (FBX, USD, LODs, compression: later) |
| R29 | **Publish targets**: an export that remembers where it goes (a folder, a game project's Assets folder) and sends again only what changed | Publish services, as for photos | later |
| R30 | **Back to Blender as a new version**: the developed model (LODs, textures) goes back beside the original, or replaces it, marked as an asset if wanted | The round trip ends where it started | **0.28.0** (a new object or the linked object's mesh, with UVs, slots, seams, vertex groups and textures; LODs and asset marking later) |
| R31 | **Assign Patina materials in Blender, finish in Patina**: a slot that uses a Patina library material (R1) is filled with that material, at full quality and editable, when the mesh is sent | Blender and Patina share one material library | later |
| R32 | **Send many objects at once** from Blender (a selection, a collection) with a preset; results come back as they finish | Batches from the Blender side | later |

### R-f. Show it

| # | Feature | Why | Version |
|---|---|---|---|
| R33 | **Turntable** video, GIF or frames; **contact sheets** of a batch; a one-file **web viewer** page to share | Portfolio and review | later (render image and turntable frames: 0.4) |
| R34 | **Look-dev scenes**: studio backdrops, HDRIs, a scale figure, grids; screenshots with the numbers | Judge a model in the light it will live in | later |

## S. 0.36–0.39: icons, right-click menus, other languages, the view

Randall (2026-10-19): "correct all EN-GB locale issues into EN-US, and then configure the app so it can easily be
localized by users for other languages, including clear localization documentation in the manual. The next step is
to make sure our contextual menus on right click on each screen are comprehensive and make sense. We also have a weird
modal switch where you have to open the retopology tab in order to see the retopology buttons in the toolbar ... We
should also group those buttons by function ... button icons instead of text for each function in the left and top
toolbars. We should not reinvent the iconography but borrow from very clear standards".

| # | Feature | Version |
|---|---|---|
| S1 | **Toolbar icons** (icons/*.svg, drawn by Patina's own SVG reader): the shapes of Photoshop, Krita and Blender's tools in Patina's colors; names and shortcuts on hover; names beside the icons as a preference; designed on a Claude Design canvas | **0.36.0** |
| S2 | **The tools grouped by job**: paint, UV, retopology, symmetry, notes; the retopology tools always there (no need to open the Retopology tab) | **0.36.0** |
| S3 | **Right-click menus** on the 3D view (for the spot under the pointer: the layers painted there, fill, select, a note, the clone source, the view) and on every panel's items and empty space; `layers_at` for agents | **0.36.0** |
| S4 | **Translations**: gettext .po files (Poedit, Weblate), a template made from the code, a test language, fonts for Chinese, Japanese and Korean, a manual page for translators; MCP `language_*` | **0.36.0** |
| S5 | **American English everywhere**: the last British material ids renamed (old ones still work), the manual's screenshots taken again | **0.36.0** |
| S6 | Right-to-left scripts and scripts with joined letters (Arabic, Hebrew, Hindi, Thai): need text shaping in the interface | later |
| S7 | Patina Web and the Blender add-on in other languages | later |
| S8 | **Retopology preview button** in the bar over the 3D view: the zones (or density) and the quads on the model whatever tool is chosen, its arrow choosing what shows (before, only while the Retopology tab was in front); MCP `retopo_view {preview}` | **0.37.0** |
| S9 | **Axis gizmo** in the 3D view's corner, in the app and Patina Web: X red, Y green, Z blue turning with the camera; a click looks along an axis (again: the other side), a drag orbits; `set_view {axis_gizmo}` | **0.37.0** |

Randall (2026-10-19, after 0.36): "The Retopology Preview only shows when the retopology tab is showing. This should
be a toggle in the top bar instead of requiring that retopo tab to be shown. We also need a 3d axis gizmo in the corner
of the display in both desktop and web modes." (S8, S9)

| # | Feature | Version |
|---|---|---|
| S10 | **Pin and unpin in the 3D view**: Pin and Unpin on the Select tool's bar (Unpin on the Island tool's), Alt+P over the 3D view, right-click › Pin / Unpin its UV island, and blue stripes on the pinned islands while those tools are chosen; Patina Web's Island brush the same | **0.37.1** |

Randall (2026-10-19, after 0.37): "a button to clear pinned of any selected UVs from the viewport tab. Currently the
only easy way to unpin selected UVs on the viewport is to select it from the UV menu, since the shortcut doesn't work in
the viewport mode." (S10)

| # | Feature | Version |
|---|---|---|
| S11 | **Side views jump** (numpad, gizmo; animating is a preference), **orthographic** until you orbit away (Blender's Auto Perspective), and **Numpad 0 / the gizmo's Back button** go back to the view before (again: the side view); `set_camera {view: previous}` | **0.38.0** |
| S12 | **Alt while orbiting snaps** to the nearest side view (Shift during an Alt+left orbit); an orbit stays one until its button is let go | **0.38.0** |
| S13 | **View pie on Z** (hold and point, or tap and click): wireframe, Solid, unlit, seams, Paint, Island, clear selection, retopology lines / edge loops; the new **Solid** view mode | **0.38.0** |
| S14 | **Path traced view mode and renders**: a progressive GPU path tracer (compute shader over the BVH, GGX + diffuse, clearcoat, sheen, subsurface through thin parts, opacity) lit by the environment with importance sampling and MIS (Shift + right drag turns it), a shadow-catching ground; a quick picture while things change, then passes on a frame-time budget; the Render image dialog renders it in steps; MCP `path_trace`, `render_image {path_traced}` | **0.39.0** |
| S15 | **More pies**: the view modes and the edit modes split; Z the view (lit, solid, unlit, path traced, wireframe, seams, UV checker, retopology preview, in Blender's shading-pie places), Shift+Z the channels, ` the edit modes (the tools), Q Quick Favorites (eight of the user's own: right-click a toolbar button or a pie item to add it, an empty place opens a searchable picker, Preferences › Pie menus); MCP `pie_menus` | **0.40.0** |
| S15b | **Brush keys**: one key per brush value in the Blender keymap, as F / Shift+F for size / opacity: G flow, Shift+G hardness, Ctrl+F angle (Blender's texture angle key) | **0.40.0** |
| S16 | Path tracer: a denoiser for the first samples, depth of field | later |

Randall (2026-10-19, after 0.37.1): "If so, jump. But also, one key, I'm unsure which, should return the user to their
previous perspective location. I also want the alt key whole dolling to snap to ortho axes as the user moves the mouse
around similar to blender. It would also be super duper awesome if holding the Z key allowed the user to interact with
a radial menu ... I would also love to have a fancy path race render mode in the next version, assuming a key light
that can be moved around by holding shift+alt+mouse drag if we don't already support rotating the hdri" (S11–S14)

Randall (2026-10-20, after 0.39.0): "Let's split up view modes from edit modes into different pies. Use the Q key and
tilde keys for more options. Q can be favorites. Tilde can be edit modes. We should also support a single key for
setting intensity since we have one for size and one for flow. ... We can also use shift Z for another pie if we need
it." (S15, S15b)

| # | Feature | Version |
|---|---|---|
| S17 | **Wireframe and seams hidden behind the model**: pulled toward the camera a pixel or two where each line is (a share of the depth range came to a third of the model and more), so the far side, parts behind others and layers under cloth no longer show through | **0.40.1** |

Randall (2026-10-20, after 0.40.0): "there's no way to hide backside wireframe or seams. It's probably a clipping
plane issue more than a setting, but it's really distracting and problematic." (S17)

| # | Feature | Version |
|---|---|---|
| S18 | **Baked maps as layers**: AO, curvature and thickness each a layer (Baked maps section under the stack) whose painted fixes (value + coverage over the bake: the eraser brings the bake back; blur, smudge, clone) and adjustments (the mask effects: levels, blur...) make the map smart masks, the 3D view and exports read; strength, compare with the bake, bake again keeps the fixes; the 3D view shows the map while its layer is selected; MCP `baked_maps`, `add_layer {type: baked}`, `set_brush {mask_value}` | **0.41.0** |
| S19 | Baked map layers for the bent normals, the high-poly normal and the material IDs | later |

Randall (2026-10-20, after 0.40.1): "It looks like we currently do not have the ability to modify any of the baked
textures within Patina. We should be placing those as a layer in the layer list that only affects their respective
function, like AO, curvature, and so on. Because I baked those textures for my model, and they are very bad. And I have
no way to clean them up or modify them within Patina right now." (S18)

| # | Feature | Version |
|---|---|---|
| S20 | **Type values into sliders**: right-click any slider (or drag field) to type its value, Enter / Esc; typed values clamped to the range (patched Dear ImGui); Ctrl+click as before; feature `slider_typing` | **0.42.0** |
| S21 | **Blender bridge: pushes to an object sent without UVs** always apply Patina's UVs (Blender's other UVs kept as "UVMap before Patina"), so UVs and textures come across again; add-on 0.12 | **0.42.0** |

Randall (2026-10-20, after 0.41.0): "make it so the user can click on any value that currently is represented by a
slider and type the value in ... Probably use right click to open the value setting entry and leave left click alone."
(S20) "Also, the blender bridge has not been properly pushing uvs and texture updates from patina to blender for a
little while now." (S21)

| # | Feature | Version |
|---|---|---|
| S22 | **Symmetric islands on nearly symmetric models**: vertices down the middle that wander off the mirror plane (up to half their average edge: cloth simulations, a panel shifted to one side) pair with themselves; match tolerances measured by the second shortest edge, so the rim of a cloth with thickness pairs too (desktop and Patina Web) | **0.43.0** |
| S23 | **Island brush, Screen and Surface**: Surface (default) takes the faces within the brush's size along the surface from the cursor (never a layer behind or a part only near in space; the same at any angle); Screen no longer takes faces turned away just behind the one seen (the inside of a cloth with thickness); feature `island_reach`, MCP `island_brush` | **0.43.0** |
| S24 | A symmetric island that isn't stacked: one island across the middle unwrapped to be its own mirror image (each half its own texels) | later |

Randall (2026-10-20, after 0.42.0): "I'm still seeing assymetry in the UV unwrapping by island painting when
unwrapping Mai. Also I need to be able to not select through a mesh bust still not be projection based select in
patina." (S22, S23)

| # | Feature | Version |
|---|---|---|
| S25 | **Blender: an object comes back to its project**: an object Blender sends to a Patina that doesn't have it open (after a restart) opens the saved project it was painted in, with its layers (reprojected if the mesh changed): linked when a project of it is saved or opened (`blender_projects.json`), else found among the recent files; not another model of the same name; feature `bridge_projects` | **0.44.0** |
| S26 | **Blender: start from Blender's materials**: with no project, an object starts from what its material slots show (the Principled BSDF's values and the images behind its inputs, through Separate Color, Invert, Mix, Normal Map and Bump nodes; add-on 0.13 sends them), so pushed textures come back as the layer From the model; feature `bridge_materials` | **0.44.0** |
| S27 | **No black model from a color attribute**: a color attribute from Blender that is one flat color (a black default an importer left) is left out instead of becoming the layer From the model, unless a material shows it (add-on 0.13); vertex paint comes in where a slot has no image on Base Color | **0.44.0** |

| S28 | **Islands survive a send from Edit Mode**: add-on 0.13.1 reads UVs, the second UV map and the color attribute from the edit-mode BMesh (Blender 5.x leaves the mesh's arrays empty in Edit Mode, so an object went as "no UVs" and was unwrapped again into jagged pieces); Patina keeps its own UVs on an object that comes back without UVs, carried over face by face after an edit (uv::carryUVs) | **0.44.1** |

| S29 | **Painting stays fast on big texture sets**: a stroke updates only the region its dabs reach (the stamp, the paint pass, the composite and the mip levels; screen dabs through a per-stroke screen-to-atlas cell map), and one full composite at the end; 6 texture sets in a 6144 atlas went from about 12 fps to 60 while painting | **0.44.2** |
| S30 | **Baked maps you can switch off, and re-bake when out of date**: a baked map's layer checkbox takes the map out of the 3D view and exports (smart masks keep it); each bake remembers the UVs it was made for, and the Baked maps section offers Re-bake after UV edits (MCP baked_maps out_of_date, shown, rebake) | **0.44.2** |

| S31 | **Select only what the camera sees**: the Select tool's Visible only (Islands mode): a click or drag takes just the island's faces turned to the camera, on screen and not behind other faces (Engine::visibleFaces: a few points of each face, ray-cast in parallel); Ctrl+click takes the seen faces off; MCP select_faces visible; feature `select_visible` | **0.45.0** |
| S32 | **The Guided layout**: the work in five steps (Model, UVs, Bake, Paint, Export) along the top, each a screen of its own with that step's panels and checks (done / needs a look / to do, the fix one click away), Next naming the next step; the docked layout of before is the Full layout (Guided / Full at the right of the menu bar; new installs start Guided, a Patina used before keeps Full); the steps are layouts over one project (every tool's key, menu and Search work in every step; another step's tool goes to that step); the first start asks "You come from" (Blender / Maya / other) and "Your textures go to"; MCP layout; feature `guided_layout`. Research: docs/UX_REVIEW.md | **0.46.0** |
| S33 | **Nothing lost or silent**: New, Open, Open Recent, the autosave, the sample, the welcome window and a dropped file ask before replacing unsaved work (Save / Don't save / Cancel); a brush that would paint nothing says why (no layer, hidden layer or folder, opacity 0, no channels); the view bar shows the brush only for tools that use one; a fresh Full layout has Materials in front (the Console last, History and Notes at the bottom); Search everything also on F3 (and Ctrl+K in the Blender keymap) | **0.46.0** |
| S34 | **The steps' colors**: Model blue, UVs teal, Bake violet, Paint copper, Export green. The Guided layout's step bar as folder tabs (the chosen one raised, one piece with the strip under them; the whole window tinted with its color, eased), Next in the next step's color; in the Full layout a stripe and the chosen tool's fill in the tool strip's groups, the panels' tabs (DockStyle colors pushed before Begin) and the menus underlined, the menus in the steps' order | **0.47.0** |
| S35 | **The export shows what it writes**: Document::exportPlan (the same names and maps exportTextures uses): every file, what its channels hold, bits, sRGB or Non-Color; in the Export Textures window and the Export step; export_textures dry_run | **0.47.0** |
| S36 | **The UV menu in groups** (Unwrap, Islands, Mirrored Halves, Pins, Selection, Seams); **a first start paints a color**; **tiny faces have UV area** (faceHasNoUVArea summed from the first corner: float rounding had lost areas of ~1e-8, 58 faces on Suzanne); select_faces no_uv_area | **0.47.0** |

Randall (2026-10-21, after 0.46.0): "I think all four of those things that you marked as next should be done ... I also
think we should do a better job of indicating which tools belong to which step of the process, either by highlight or
background color or border color ... would be good to give the tabs that belong to the GUI a stylized look ... maybe
actually the step tabs that you've put along the top are actually physical tabs, like in a manila folder ... clicking on
them creates a visible physical change along the top bar and maybe the entire background of the UI ... or maybe a
color tint to the background to clarify what stage they're in." (S34, S35, S36)

Randall (2026-10-21, after 0.45.0): "I think we're looking really good for a version 1.0 at this point, but I have one
more request for it ... do some deep diving into our competitors and their UI. Find out things that people don't like
about their UI that we might be replicating ... look at our user interface from a user perspective ... for someone who
has never used the application before, but is coming from Blender or coming from Maya ... I think things kind of feel a
little scattered all over the place or buried in submenus that are hard to find ... figure out the things that we need
to get in front of the user in sort of an experiential flow that makes good sense as a simplified experience. And then
we have the current layout as the kind of advanced custom experience." (S32, S33; docs/UX_REVIEW.md)

Randall (2026-10-21, after 0.44.1): "For the next version, I want to support a camera-projection facing face/UV Island
selection tool option; this would only select camera visibile faces in the UV shell, so batch selection of parts
outside an object are easier to exclude inside shapes/skins." (S31)

Randall (2026-10-21, after 0.44.1): "on V 0.44.1 disabling the baked maps doesn't change the textures in the UV
Editor or the appearance of the model in the viewport." (S30: the dark patches were a stale AO bake) "editing the
texture by erasing or painting on the Face UV set is EXTREMELY slow." (S29)

Randall (2026-10-21, after 0.44.0): "Every time I push a model from Blender to the Patina, the nice UV islands I
originally painted in Patina get all fucked up ... there are dozens of new sub-islands that were created from those
macro shapes." (S28)

Randall (2026-10-21, after 0.43.0): "the textures that I had previously pushed from Patina to Blender will not come
back to Patina in any correct manner. My mesh has changed, I think, and when I click on the mesh and I click send to
Patina, it shows up with just a flat black texture, and I can't get any other layers to show up." (S25, S26, S27)

## T. After 1.0: bake-down, selection, rendering and set dressing

Randall (2026-10-21, at 1.0): "I also want to add to the feature list more rendering and set dressing features, like
lighting, color correction, atmospheric and volumetrics, camera controls, sampling settings, denoising, and more. as
well as better face selection tools like loop selections and face sets within uvs to make painting entire batches of
faces easier. I also want a material bake down feature where if the user paints the high poly then creatures the
retopology low version, they can transfer their painting to that version with a very high degree of accuracy and
Fidelity."

### T-a. Material bake-down (high to low)

| # | Feature | Why | Version |
|---|---|---|---|
| T1 | **Bake down the layers** (**1.1.0**: Bake > Bake Down to a Low-Poly Model, `bake_down`; feature `bake_down`). Paint the high-poly, then make the low one (Patina's retopology, Blender, any file). *Bake down* moves every layer onto the low model's UVs and keeps the layer stack: paint and masks are resampled texel by texel; fill layers keep their materials; the baked maps are baked again with the high-poly's detail as a normal map. | Paint where the detail is, ship the light model, keep working in layers | **1.1.0** |
| T2 | **Accuracy**: several samples per texel spread over its footprint; rays along the low surface's normal both ways from a cage (automatic distance, or set by hand) with the closest point on the high surface as the fallback; samples filtered from the high-poly's textures; seams padded; texture sets and UDIM tiles on both sides | "a very high degree of accuracy and fidelity" | **1.1.0** |
| T3 | **A report and a check**: the share of texels that found the high surface, the largest distance, a heat map of the misses on the model; a before / after compare of the two models in the view | See where the transfer had to guess | **1.1.0** |
| T4 | **One step from retopology**: *Use the Quads as the Model* bakes down with the same accuracy; MCP `bake_down` | The retopology flow ends with the paint on the new mesh | **1.1.0** |

Shipped in **1.1.0** (2026-10-21). How it works: core/BakeProjection (the low's texels rasterized on the CPU, 1 to 16
points each on a turned grid; rays both ways along the low's welded smooth normals, the nearest front-facing hit; the
nearest point within twice the distance, then anywhere, as fallbacks; the distance measured as 1.5 times the 99th
percentile of the gap), engine/BakeDown (the POST_BAKEDOWN pass: coverage-weighted bilinear taps from the high's
dilated layer textures, normals turned from the high's tangent frame into the low's), the check map and the compare
(the high's GPU mesh and final maps). The high-poly bake uses the same projection. Tests: gpu.bake_down_moves_layers
(colors to 0.002, normals to 0.3 degrees, masks, notes, the refusal, a re-bake), gpu.bake_down_finds_the_facing_surface
(two spheres almost touching: 100% right where the nearest point gets 79% wrong), tests/gui/test_bake_down.py.
Learned: a coarse low that doesn't follow a thin part (Suzanne's ears at 260 quads per square meter) bakes strongly
tilted normals there, as any baker would; the check marks where the low faces away from the high (magenta), and the
low keeps its own normal there. Still open: an editable cage mesh, and the object selected in Blender as the low-poly
(needs the add-on to send it on request).

### T-b. Selecting faces: loops and face sets; scenes of several objects (1.2)

Randall (2026-10-21, starting 1.2): "add to 1.2 the ability to assemble parts from multiple meshes into one file in
the scene, because right now things like characters don't get their outfits or their eyes or their hair ... object
layers separate from material layers, where if you've got an object selected, kind of like Blender, you get the
material layers that are exposed for that object. This is not meant to be a full modeling program, but it is meant
to be a scene build-out program as well." Later the same day: "We also need to support auto-including brush textures
from the custom brushes folder ... That should be part of 1.2."

| # | Feature | Why | Version |
|---|---|---|---|
| T5 | **Loops and rings** (`select_loop`, `select_linked`): Alt+click by an edge selects the loop of quads across it (Blender's face loop), Ctrl+Alt+click the ring along it, Shift adds; Connected and Flat region (by the bend from face to face) grow the selection; right-click › Select from the face under the pointer (also its UV island and material slot) | Pick a band of faces in one click instead of dozens | **1.2.0** |
| T6 | **Face sets** (the Face Sets panel; feature `face_sets`): named, colored groups of faces kept in the project, shown in their colors by the Face Sets view; one click selects; hide, show alone, show all; fill, mask the selected layer; the Face set lock keeps strokes inside one; Blender's sculpt face sets come in with the bridge (add-on 0.14) | Paint or fill whole batches of faces again and again | **1.2.0** |
| T13 | **Objects in a scene** (feature `scene_objects`): a project holds several objects (a body, its clothes, eyes, hair), each with its own layers, texture set and history; the Objects list over the layers (select, show or hide, rename, copy, remove, Local view); a file with several meshes opens as an object each, File › Add a Model to the Scene adds one, texture sets matched to the first one's texel density; one project file (the first object readable by 1.1) | A character comes in whole: its outfit, eyes and hair each painted in its own layers | **1.2.0** |
| T14 | **The Object tool**: move, turn and scale objects with a gizmo (Ctrl snaps), the placement typed in, On the ground, Where it came in; a double-click on another object paints it; objects in front hide what is behind them from the brush | Build the scene out without a modeler | **1.2.0** |
| T15 | **Blender: several objects** (add-on 0.14): Send sends every selected mesh (the first starts the scene, the others come in beside it), Add to the Patina Scene; each with its matrix_world; updates to their own object; textures back to every linked object | The character in Blender is the character in Patina | **1.2.0** |
| T16 | **The scene out**: the model export with every object (glTF: a node each with its matrix; OBJ, STL, PLY moved into the scene), every object's textures named after it, the path tracer and renders with every object | One file for the engine, one picture of the whole thing | **1.2.0** |
| T17 | **Your own brushes**: every image in the brushes folder (and its folders, a group each) is a brush, each tip of a Photoshop .abr file there too; the folder looked at every few seconds; Import brushes... and a drop on the Brushes panel copy files in; tips read the right way round (dark on light, light on dark, transparency) | Brushes people already have work in Patina | **1.2.0** |
| T18 | **A change log with every build**: CHANGELOG.md in the packages, and the manual's What's new page (Help › What's New) | Patina has users now: they see what changed | **1.2.0** |
| T19 | **A color texture's alpha as opacity**: an OBJ or FBX whose cut-outs are only in the color texture's alpha (hair cards, leaves, lace) brings them in as the Opacity channel when the alpha has see-through and solid parts (`import_model_textures color_alpha`); every object's see-through parts are drawn after every opaque part, the nearest layer of cards hiding the ones behind | Hair from a game asset looks like hair, not strips | **1.2.2** |
| T20 | **G, R and S in the Object tool** (feature `object_keys`; keymap object.move / turn / scale): move in the view's plane, turn about the line of sight, scale evenly with the mouse; X / Y / Z one axis, a typed number, Ctrl snaps, click or Enter keeps it (one undo step), right-click or Esc puts it back; they come before the same keys' other uses in the tool | Scaling evenly without hitting the gizmo's small middle square; Blender users' hands already know it | **1.2.3** |
| T21 | **The materials as a list or as tiles of any size**: a list with small pictures (Materials and Smart materials tabs), a Size slider and Ctrl+wheel for the tiles, kept in the session; tiles that always fit the panel | Long names readable, more materials at a glance | **1.2.3** |
| T22 | **A smooth shadow terminator in the path tracer**: light fades at the interpolated normal's horizon (Chiang 2019 term, normal maps too), shadow and bounce rays leave from the smooth surface over the flat triangle (Hanika 2021, Cycles' geometry offset) and skip their own triangle | A coarse face under a hard light renders smooth, as in Blender | **1.2.4** |
| T23 | **Brushes stamped from the 3D view, fixed**: a UV-projected brush tests against a depth picture of the current view (it read an older stroke's, or none: strokes painted nothing), and a Screen or UV brush's mirror copies the camera can't see go on the surface | Symmetry follows the model, not the view | **1.2.5** |

Shipped in **1.2.0** (2026-10-21). How it works: each object is a Document of its own; Engine::doc is the active one
and the others wait in Engine::objects_ (they move in and out, so GpuMesh moves now). A mesh stays in its own space;
its placement (where it came in, then moved, turned and scaled about its middle) is its matrix: the renderer draws it
with it, painting and picking use the camera in its space (Camera::inSpace), the other objects go into the brush's
depth pass. The project file's header is the first object's (1.1 opens that one), the others follow under O<k>/. The
path tracer merges the objects shown into one BVH in the world, their final maps into texture arrays. Inactive
objects free their composite accumulators (TextureSet::sleep). Face sets are per face ids with a list in the
document; hidden faces carry loop flag 32, which the 3D view, the depth pass, the stamp and the lines leave out, and
the BVH's rays skip. core/MeshSelect walks quads across opposite edges for loops and rings. Brushes: the user folder
scanned with a signature checked every 3 s; .abr tips (versions 1-2, 6-10, raw and PackBits) written as PNG files in
the cache. Tests: tests/api/test_scene_objects.py, test_face_sets.py, test_brush_folder.py, tests/gui/test_scene_objects.py,
test_face_sets.py. Still open: real Photoshop .abr files to try (the test makes its own), paint that reaches across
two objects in one stroke, scene-wide ambient occlusion (one object shadowing another), the Guided layout's Face Sets.

### T-c. Rendering and set dressing

| # | Feature | Why | Version |
|---|---|---|---|
| T7 | **Lights**: sun, point, spot and area lights with color, temperature, power, size and shadows, placed with a gizmo in the view; light rigs (three-point, studio, rim); the HDR library and its rotation | Light a model like a scene, not only by the environment | 1.3 |
| T8 | **Camera**: focal length in mm, sensor size, depth of field (focus by clicking the model, f-stop, bokeh), saved views, safe frames and aspect guides, a turntable path | Frame the shot like in Blender or Maya | 1.3 |
| T9 | **Color correction**: exposure, white balance (temperature, tint), contrast, saturation, lift / gamma / gain, curves, LUTs (.cube); tone mapping AgX, Filmic, ACES, Standard; bloom, vignette, film grain | Finish the image in Patina | 1.3 |
| T10 | **Atmosphere and volumetrics**: distance and height fog, light shafts from the sun and spots through a volume in the path tracer, dust | Mood and depth for presentation renders | 1.4 |
| T11 | **Sampling and denoising**: samples, adaptive sampling with a noise threshold, a time limit, bounces by kind (diffuse, glossy, transmission), clamping; a built-in edge-aware denoiser that uses the albedo and normal (no large download) for the path-traced view and renders | Clean renders in seconds | 1.4 |
| T12 | **Set dressing**: a backdrop (cyclorama), a ground that catches shadows and reflections, plinths and a few props, a background color, gradient or image; presets (product shot, character turntable) | A presentation scene without leaving Patina | 1.4 |

## M. To do (requested)

| # | Feature | Why | Version |
|---|---|---|---|
| M1 | **One source of truth for materials**: a material's configuration lives in Patina, and changing it there updates the material in Unreal, Unity, Blender and / or Maya (live links that re-import textures and keep shader parameters in step; the Blender bridge is the starting point) | No re-exporting and re-wiring materials by hand in each tool | Blender: **0.25.0** (R1: the library, updated with one click; materials in use update with it). Others later |
| M2 | Patina Web and the desktop app kept at feature parity where the browser allows (the web stays the "lite" interface) | One tool, two ways in | 0.8.1: blur, fill, pick, mirror painting, adjustment layers, blend modes, smart masks, projects, image materials, relax, shape packing, engine export presets. Still desktop only: clone, smudge, baked AO, seam brush and unwrap methods, material editor, Blender and MCP |

## Shortcuts added in 0.4

| Action | Patina keymap | Blender keymap |
|---|---|---|
| Island brush tool | L | L |
| Make island from the painted faces | Enter | U (Blender's Unwrap) |
| Select all / none (faces) | A / Alt+A | A / Alt+A |
| Grow / shrink selection | Ctrl+Numpad + / - | Ctrl+Numpad + / - |
| Save incremental | Ctrl+Alt+S | Ctrl+Alt+S |
| Export again | Ctrl+Shift+E | Ctrl+Shift+E |
| Render image | F12 | F12 |
| Rotate environment | Shift + right drag | Shift + right drag |
| Topology lock on / off (0.4.1) | Alt+T | Alt+T |
| Island lock on / off (0.4.1) | Alt+L | Alt+L |
| Make an instance (0.6) | Alt+D | Alt+D |
| Smudge tool (0.6) | J | J |
| Note tool (0.6) | N | N |
| Stencil: hold to move / rotate / scale; on / off (0.6) | T; Shift+T | T; Shift+T |
| Compare view; snapshot (0.6) | \\; Shift+\\ | \\; Shift+\\ |

In the Blender keymap, U no longer selects the Blur tool, because Blender uses U for unwrapping.

## Plan

1. **Core UV algorithms** (`core/UVIsland.*`): island creation, orientation, density targets, free-space placement,
   overlap repair, stretch metrics. Unit tests.
2. **Engine**: commands for every feature (MCP / console / Blender), selection grow/shrink, mask from selection,
   new smart masks, material ID map, high-poly baker, render image, save incremental, export again, file watching.
   GPU tests.
3. **Interface**: Island brush tool and options, density panel in the UV editor, new view modes, environment
   rotation, render dialog, bake dialog, welcome screen, recovery and "mesh changed" prompts, keymaps.
4. **Documentation**: manual (UV editing, baking, painting, interface, both shortcut pages, gotchas, commands),
   README, this roadmap. New screenshots.
5. **Release**: 0.4.0 for Linux and Windows.
