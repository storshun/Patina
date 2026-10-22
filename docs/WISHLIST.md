# What texture artists wish Substance Painter had, and where Patina stands

This list is compiled from Adobe's Substance 3D Painter feature-request forum, Steam
discussions and polycount threads (October 2026). Each item that Patina also lacked is
marked with its status: **0.3** (implemented in Patina 0.3), **later**, or
**n/a**. **n/a** covers items that don't apply to Patina, or that Patina already did
differently.

Everything marked **0.3** is built, covered by the GPU tests where it touches the
engine, and described in the manual (*Folders, solo & smart materials*, *Painting*,
*Interface*, *Texture set & export*, *Gotchas*).

Sources:
[Adobe feature requests](https://community.adobe.com/feature-requests-60) ·
[Isolate layer (most-voted QoL request)](https://community.adobe.com/feature-requests-60/feature-suggestion-isolate-layer-1185975) ·
[Hotkey support](https://community.adobe.com/feature-requests-60/subject-feedback-adding-hotkey-support-for-key-functions-in-substance-painter-1186026) ·
[Tool switching & color picking](https://community.adobe.com/feature-requests-60/workflow-and-performance-enhancements-for-substance-painter-improved-tool-switching-color-picking-1185944) ·
[Steam: personal wishlist](https://steamcommunity.com/app/744570/discussions/0/1696043263509305288/) ·
[Steam: features for painters](https://steamcommunity.com/app/273390/discussions/0/558751179487057231/) ·
[polycount master thread](https://polycount.com/discussion/128707/substance-painter-master-thread/p15)

## Layer stack

| Wish | Where it comes from | Patina |
|---|---|---|
| Isolate / solo a layer to see what it contributes | Adobe forum (most-voted QoL request, open since 2022) | **0.3**: Alt+click a layer's eye |
| Folders (groups) for layers, with masks and adjustments on folders | Adobe "Quality of life - New Folders", "folder structures in masks"; polycount "adjustment layers for folders" | **0.3**: nested folders with opacity, blend mode, mask and smart mask |
| Color tags on layers | Adobe "Add custom colors tag to layers", "Get/Set Layer color tag" | **0.3** |
| Fill layer with a black mask in one click | Adobe "Fill Layer with Black Mask option" | **0.3**: the arrow next to *+ Fill* ▸ *Fill layer with a black mask* |
| Clear a layer's paint without losing its mask | polycount | **0.3**: *Clear paint* keeps the mask |
| Copy a layer's settings to another layer | Adobe "Copy layer's parameters" | **0.3**: right-click a layer ▸ *Copy / Paste layer settings* (also `copy_layer_settings`) |
| Copy and paste layers between projects | Adobe (3 votes) | **0.3**: smart materials: save a layer or folder (optionally with its paint) and apply it in any project; 19 built-in ones |
| Undo for every change, not just strokes | Adobe "Access to the undo system" | **0.3**: every layer setting is undoable (a slider drag is one step), and so are masks and *Clear paint* |
| A history list you can jump around in | Painter has one; Patina didn't | **0.3**: History panel (click a step to jump to it) |
| Nested effects, node graph | Adobe | **0.6**: mask effect stacks (nested effects); a node graph is still open |
| Layer instancing | Painter has it | **0.6** |

## Painting

| Wish | Where it comes from | Patina |
|---|---|---|
| Quick color pick while painting, without changing tools | Adobe "Material Picker Toggle", Steam "Alt-key color picking" | **0.3**: hold <kbd>I</kbd> (or Alt+right-click) to sample, release to keep painting |
| Color palette that stays visible, recent colors, swatches | Steam "always-visible color palette" | **0.3**: Colors panel with recent colors and saved swatches |
| Foreground/background color with a swap key | Steam (Photoshop habit) | **0.3**: <kbd>X</kbd> swaps the primary and secondary paint colors (the X-mirror toggle moved to <kbd>Shift</kbd>+<kbd>X</kbd>) |
| Multi-axis and radial symmetry | Adobe "multi axis mirroring" | **0.3**: any combination of X/Y/Z mirrors plus radial symmetry (2–32 copies around an axis) |
| Polygon fill that erases and affects every channel | Adobe "Polygon Fill Eraser", "Polygon fill to erase…" | **0.3**: Alt+click with the Fill tool erases. Fills already write all channels. |
| Strokes on mirrored UV shells applied once | Steam wishlist | n/a: Patina paints each texel once per dab, whatever the UVs |
| Blur that works across UV seams | Steam wishlist | **0.6** (blur and smudge) |
| Brush settings kept between sessions | Adobe "Keep the Brush setting during a working session" | **0.3**: brush, material, tool and colors are restored on start |
| Hand-painted workflow (dedicated color tools) | Adobe (5 votes) | **0.3**: color panel, swatches, quick picker |
| Scatter / stamp brush | Adobe | Patina already has scatter and stamp presets |

## Interface and workflow

| Wish | Where it comes from | Patina |
|---|---|---|
| Customizable hotkeys | Adobe (Community Manager agreed it needs a rework) | **0.3**: *Edit ▸ Keyboard shortcuts*, saved in `keymap.json`; **0.3.1** adds a complete Blender keymap (*Edit ▸ Preferences*) |
| Navigation presets (ZBrush, Blender, Maya) | Adobe "zbrush navigation mod" (7 votes) | **0.3**: Patina, Blender, Maya and ZBrush-style presets |
| Adjustable UI scale | Adobe | **0.3**: *View ▸ UI scale* |
| Hide all panels for a distraction-free view | polycount | **0.3**: <kbd>Tab</kbd> |
| Recent files | Adobe "Recent Files Functionality" | **0.3**: *File ▸ Open Recent* |
| Tell me loudly when saving fails | Adobe "Show a Popup When Substance Painter FAILS to SAVE" | **0.3**: error dialog for failed saves and exports |
| Asterisk for unsaved projects, project name visible | Adobe, polycount | Patina already shows both |
| Comments / annotations in a project | Adobe (6 votes) | **0.6** (notes pinned to the model) |
| Linux as a first-class platform | Adobe | Patina is native on Linux |

## Export

| Wish | Where it comes from | Patina |
|---|---|---|
| File-name tokens such as `$textureSize`, `$fileName` | Adobe (several requests) | **0.3**: name templates with `$mesh $project $map $size $preset $layer` |
| Export a mask, with a choice of resolution | Adobe "choose the resolution when exporting a mask" | **0.3**: export a layer's mask, or only one layer / folder, at any export size |
| Merge texture sets at export | Adobe (44 votes) | n/a: Patina has a single texture set per project |
| UDIM tiles and tile names for engines | Adobe (36 votes) | later: islands in other tiles are moved into 0–1 today |
| Layered PSD export | polycount, Adobe "Export to Photoshop" | **0.7**: a PSD per map with layers, masks, blend modes and groups |
| 16K–32K output | Adobe | **0.7**: exports up to 32K, rendered in tiles (the texture set itself stays at 8K at most) |

## Patina 0.4

The 0.4 release came from a different angle: following an artist's working day in Patina (bring a model in, prepare
UVs, bake, texture, review, export and iterate) and removing the slow parts. The hit list, with what was built and what
is planned, is in [ROADMAP.md](ROADMAP.md). The main items are the Island brush (paint a part, get a texel-density
correct island), density and stretch heat maps, baking from a high-poly mesh, material slots, facing masks, rendered
images and turntables, save incremental, export again, mesh reloading and crash recovery.
