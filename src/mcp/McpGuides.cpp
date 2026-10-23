#include "mcp/McpGuides.h"

namespace patina {

namespace {

const char* kWorkflow = R"MD(# Patina for agents: the workflow

Patina post-processes 3D models the way you would a photo: non-destructive, layered, and scriptable. It takes a
model that is made (in Blender, by a scanner, by a
generator such as Meshy or Tripo, from a store) and finishes it: mended, textured, developed, lighter, with
collision, exported. Every step is a tool, and every tool returns numbers you can check. Look at the model with
`render_view` before and after each step (it returns an image).

## 1. Open
- `open_file {path}`: .glb .gltf .obj .fbx .stl .3mf .ply or a .patina project. A textured or vertex-colored model
  comes in with its own look as the paint layer *From the model* at the bottom of the stack; `get_state` →
  `mesh.model_textures` says what came in and names any texture that was not found. An OBJ or FBX color texture's
  alpha becomes the opacity channel when it cuts out (`opacity_from_color_alpha`: hair cards, leaves, lace); when it
  should not (gloss in the alpha) or should, `import_model_textures {color_alpha: "ignore" | "opacity"}` reads it again.
- Or the user sends an object from Blender (Patina Bridge add-on).
- `render_view {width: 800, height: 600, camera: {view: "front", frame: true}}`; views: front back left right top
  bottom persp; `mode` shows one channel or a check (see `set_view`).

## 2. Check and mend the mesh (guide: mesh)
1. `mesh_check`: stats (faces, parts, closed, size in Blender axes, UV coverage) and problems, each with a count,
   where it is and which fix mends it.
2. `mesh_fix {all: true}`, then `mesh_check` again to confirm. `mesh_fix_revert` goes back.
3. `mesh_transform` for units, a wrong up axis, the origin, standing it on the ground.
4. `mesh_reduce {ratio: 0.5, preview: true}` says what a reduction would leave and how far the shape would move
   (`error`); run it without `preview` to do it.
Every mesh step keeps each face corner's UV, so textures and paint stay valid.

## 3. Textures (guides: develop, painting)
- Develop the look: an adjustment layer on top, `add_layer {type: "adjust", adjust: {...}}`, tuned with
  `texture_check` (numbers and hints) and `render_view`.
- Finish or restyle it: smart materials (`list_smart_materials`, `apply_smart_material`), fill layers with smart
  masks (`bake_mesh_maps` first for edge, cavity and occlusion masks), painting (`paint_stroke`).
- `render_view {mode: "pbr_range"}` shows colors a real material would not have; `texture_check` counts them.

## 4. Game ready (guide: export)
- `mesh_collision {mode: "pieces"}` makes convex collision shapes; `fit` is their volume over the model's (1.0 to
  1.2 is close).
- `mesh_lightmap_uvs` makes the second UV map engines bake lighting into (after the last fix or reduction).
- `export_model {path: ".../name.glb", texture_size: 2048, lods: [0.5, 0.25], collision: "unreal"}`.
- Or back to Blender: `send_model_to_blender`, `send_to_blender` (textures only).

## 5. Many models (guide: batch)
Recipes are lists of tool calls (`recipe_list`, `recipe_save`, `recipe_apply`). `batch_run {recipe, folders,
output}` runs one on every model in folders, in a hidden Patina; `batch_status` follows it.

## Good to know
- `undo` / `redo` cover layer and paint changes. Mesh changes (fix, reduce, transform) clear the undo history but
  keep a copy of the project first: `mesh_fix_revert`.
- Positions and sizes in results are in Blender's axes (X right, Y back, Z up), in meters.
- `features` lists the newer features; a switched-off one refuses its tools and says so.
- `open_gui_window` (MCP only) opens the desktop window on the current project so the user can watch.
- The guide "parameters" lists every setting of every tool with its range, default and effect.
)MD";

const char* kMesh = R"MD(# Mesh: check, mend, transform, reduce, collision

## mesh_check: read this first
Returns `stats` and `problems`. Each problem has `id`, `count`, `severity` (problem, warning, note), `where` (up to 5
positions, Blender axes) and `fix` (which mesh_fix option mends it). `show: true` paints the problems on the model
(red problem, orange warning, blue note; the backs of faces magenta); `select: "<id>"` selects that problem's faces.
A model that is `closed` with no problems is watertight: needed for 3D printing and for a meaningful volume.

## mesh_fix: what each option does
- `all: true` runs every fix with its default setting. Start here for downloaded and generated models.
- `weld` joins vertices closer than `weld_distance` (0 = 1e-5 of the size). Open seams turn into shared edges; a
  hole can only be found once its border is welded. A bigger distance also joins parts that merely touch.
- `remove_small_parts` drops floaters: separate parts whose box diagonal is under `small_part_size` (0 = 3% of the
  model) and that have under 1% of the faces. Raise it to remove bigger debris (scans); lower it if real small parts
  (eyes, buttons) go.
- `fix_winding` turns faces to match their neighbors and closed parts to face out (fixes black patches).
- `fill_holes` closes holes of up to `fill_hole_edges` edges (0 = 64). For printing use 512 or more. Openings that
  belong (a cup's rim, a sleeve) close too: lower it, or skip it.
- `remove_degenerate`, `remove_duplicate_faces`, `remove_loose`: tidy-ups with no settings.
Results say how many of each were changed and the new face count; the paint is carried over in 3D.

## mesh_transform
`from_units` (cm, mm, in, ft) scales a model made in those units to meters; `scale` multiplies after it;
`rotate [x, y, z]` degrees around Blender's axes ([90, 0, 0] stands up a model lying on its back); `origin`
center or base (the middle of its bottom: for props that stand on the floor); `ground: true` puts its lowest point
at height 0. Returns the new size (Blender axes).

## mesh_reduce
- `ratio` (0.001 to 1, default 0.5): the share of triangles to keep. `triangles` asks for a count instead.
- `max_error` (default 1 = always reach the target): stop early rather than let the surface move more than this
  share of the model's size (0.005 = half a percent).
- `keep_borders` (default true): open borders stay where they are.
- `preview: true` changes nothing and returns `triangles_after` and `error`.
The result's `error` is how far the surface moved, as a share of the size: under 0.005 is invisible at normal
distance; 0.02 shows on silhouettes. Seams, material borders and UVs are kept, so textures still fit.

## mesh_lightmap_uvs
A second UV map for baked lighting (Unity, Unreal, Godot): no face shares space, even detail, `padding` texels
(default 4) between islands at `lightmap_size` (default 512). `source: "uv"` (default) repacks the model's own islands
and unwraps folded ones again; `source: "auto"` makes new charts with xatlas (for scans and generated models; slow on
dense meshes, so reduce first). Results: `islands`, `fill` (share of the square used; 0.4 to 0.7 is good),
`overlaps` (faces overlapping another: should be 0), `needed_size` when the charts did not fit (raise the size or
lower the padding). Saved with the project; exported as glTF TEXCOORD_1; Blender gets the UV map Lightmap. Mesh fixes
and reductions drop it (make it again after them); transforms keep it.

## mesh_collision
Shapes a game engine uses for physics instead of the model. Pick `mode`:
- `box`: one oriented box (crates, books, simple props). Cheapest.
- `boxes`: a box per separate part.
- `hull`: one convex hull (rocks, bottles, round props).
- `parts`: a hull per separate part (kits, furniture assembled from pieces).
- `pieces`: the model split into convex pieces where it bends in (chairs, arches, rings, characters). Closest fit.
Settings: `max_shapes` (default 16; more fit concave shapes closer but cost physics time), `max_vertices` per hull
(default 32; engines like 32 or fewer), `concavity` (pieces; default 0.05: split again while a hull holds more than
5% empty space; 0.02 closer, 0.15 rougher), `resolution` (pieces; voxels along the longest side, default 40; higher
follows thin parts, hulls stand off the surface by about a voxel).
Results: `count`, per shape `vertices` and `volume`, `fit` = shapes' volume / model volume (1.0 to 1.2 is close;
above 1.5 the shapes hold a lot of air: try pieces, more shapes or a lower concavity). The shapes show in green;
`export_model` with `collision` writes them. A mesh change drops them (they would no longer fit).
)MD";

const char* kDevelop = R"MD(# Developing the textures

An adjustment layer works on the finished look of everything below it, like the basic adjustments in a photo editor.
Use one at the top of the stack to correct a model's own textures (the *From the model* layer) or a finished paint
job. Nothing below changes; turn it off, lower its opacity, or paint its mask to limit it.

    add_layer {type: "adjust", name: "Develop", adjust: {exposure: 0.5, temperature: -0.2}}
    update_layer {index: <its index>, adjust: {shadows: 0.4}}      (only the settings given change)

## The settings, in the order they apply
1. `delight` 0..1: takes shading baked into the color out by dividing by the ambient occlusion (bake it first,
   `bake_mesh_maps {ao: true}`; it is baked when needed). 1 = fully: a texel with AO 0.5 becomes twice as bright.
   Scans and generated models need 0.5 to 1; painted models rarely need it.
2. `temperature` -1..1 (warmer +), `tint` -1..1 (more magenta +): white balance. ±1 is about ±30% between red and
   blue (temperature) or ±25% green (tint); brightness is kept. Use them to remove a color cast.
3. `exposure` -5..5 stops: +1 doubles the light, -1 halves it (linear light). The main control for too dark / too bright.
4. `highlights`, `shadows` -1..1: up to one stop on the brightest / darkest parts only, fading out toward the middle.
5. `vibrance` -1..1: colorfulness, mostly of dull colors (skin and strong colors change little).
6. `saturation` 0..3 (x), `hue` -0.5..0.5 (turns), `brightness` 0..3 (x, sRGB), `contrast` 0..3 (around mid gray).
7. `roughness`, `metallic` -1..1, `height` -0.5..0.5: added to those channels.
All at their defaults (0, or 1 for the multipliers) change nothing.

## Tuning by numbers
`texture_check` measures the finished maps:
- `basecolor.luminance` p5 / p50 / p95 (sRGB, 0..1). Most real non-metals sit between 0.12 and 0.94 (sRGB 30 to
  240); a typical median is 0.25 to 0.6. Metals should be 0.7 or more.
- `non_metal_too_dark_pct`, `non_metal_too_bright_pct`, `metal_too_dark_pct`, `metallic.half_metal_pct`: shares out of
  range (the PBR Range view shows where). `pbr_ok` is true when all are small.
- `cast.warm` (red minus blue) and `cast.green`: a neutral texture is near 0 (a red sofa is warm on purpose).
- `hints`: which setting to move.
A loop: `texture_check` → change one or two settings with `update_layer` → `texture_check` and `render_view` →
repeat. Move exposure in steps of 0.25 to 0.5 stops, temperature and tint in steps of 0.1 to 0.2.

## Example: a generated model with baked lighting, too dark and too orange
    bake_mesh_maps {ao: true}
    add_layer {type: "adjust", name: "Develop", adjust: {delight: 0.8}}
    texture_check            -> p50 0.14, warm 0.16, hint: raise exposure ... ; warm cast ...
    update_layer {index: i, adjust: {exposure: 0.6, temperature: -0.3}}
    texture_check            -> p50 0.31, warm 0.03, pbr_ok true
)MD";

const char* kExport = R"MD(# Export and Blender

## export_model {path, ...}
The extension picks the format:
- `.glb`: one file with the textures inside. Games (Godot, Unity with glTFast, Unreal), the web (three.js, Babylon),
  Sketchfab, Blender. `.gltf`: the same with a .bin and PNG files beside it. Lightmap UVs (mesh_lightmap_uvs) go in as
  TEXCOORD_1.
- `.obj`: with a .mtl and PNG maps (Blender's OBJ importer reads them); faces keep their quads and n-gons.
- `.stl`: 3D printing, millimeters, Z up, no textures (mend it first so it is closed).
- `.ply`: the base color as vertex colors, with UVs and normals (color printing, programs that use vertex colors).
Settings: `texture_size` (0 = the texture set's; 1024 for web and mobile, 2048 for most games), `textures` (default
true), `lods` (levels of detail sharing the textures, e.g. [0.5, 0.25, 0.125]), `collision`:
- `"unreal"`: shapes named `UCX_<model>_00` (hulls) and `UBX_<model>_00` (boxes): Unreal uses them as the simple
  collision of the mesh with that name.
- `"godot"`: `<model>_col00-convcolonly`: Godot 4 turns each into a convex collision shape on import.
- `"plain"`: `<model>_collision_00`, for engines without a naming rule (in Unity, add a Mesh Collider set to
  Convex on each).
The shapes made with `mesh_collision` are used; without them they are made with its current settings.

## export_textures {directory, preset}
Only the texture maps, laid out for `blender`, `gltf`, `unreal` or `unity`; `format: "psd"` writes layered PSDs.

## Blender
- `send_to_blender`: the textures (and edited UVs) to the linked Blender object.
- `send_model_to_blender`: the model as it is in Patina (mended, reduced) as a new Blender object with its textures.
- `blender_material_library`: every Patina material into Blender's Asset Browser.
- (0.44) The layers live in the project, not in Blender: `save_project` after painting a linked object. When Blender
  sends an object again to a Patina that doesn't have it open (a restart), Patina opens the project saved for it and
  keeps its layers (`get_state` shows `project` and `bridge.object`). With no project, the object starts from
  what its Blender materials show (add-on 0.13), as the layer From the model; a color attribute that is one flat
  color is left out unless a material shows it.
- (0.44.2) Baked maps: `baked_maps` reports `out_of_date` (made for other UVs: re-bake with `rebake: true`) and
  `shown`; hiding a baked layer (update_layer visible false) takes that map out of the view and exports.
- (0.44.1) An object that comes back without UVs (sent from Edit Mode with an older add-on) keeps Patina's UVs and
  islands, carried over face by face (bridge result `kept_uvs`); check `uv_info` islands after a send.
)MD";

const char* kBatch = R"MD(# Recipes and batches

A recipe is a list of tool calls that finishes a model:

    {"name": "Web lite", "description": "...", "steps": [
      {"cmd": "mesh_fix", "args": {"all": true}},
      {"cmd": "mesh_reduce", "args": {"ratio": 0.25}},
      {"cmd": "export_model", "args": {"path": "${out}/${name}.glb", "texture_size": 1024}}]}

Variables in any string: `${name}` the model's name, `${file}` its file, `${dir}` its folder, `${out}` the output
folder. Any tool can be a step (not `open_file`: a batch opens each file itself, and opening a textured model brings
its textures in as usual).

- `recipe_list`: the built-in recipes (patina_game_asset, patina_web_ar, patina_cleanup, patina_print,
  patina_auto_texture, patina_worn_metal, patina_cm_to_m) and the user's own, with their steps.
- `recipe_save {name, description, steps}` keeps one as the user's own (id user_<name>); `recipe_delete {id}`.
- `recipe_apply {id or steps, output}` runs it on the open model now.
- `batch_run {recipe or steps, files, folders, recursive, output}` runs it on many files in a hidden Patina; the
  originals are never changed. `batch_status` gives progress and a result per file (`ok`, the error if not);
  `batch_cancel` stops after the current file.
Outside Patina: `patina run job.json --progress p.jsonl` with `{"files": [...], "per_file": [steps]}`.
)MD";

const char* kPainting = R"MD(# Painting and layers, briefly

- Materials: `list_materials` (optionally one `category`), `set_paint_material {id}`; brushes: `list_brushes`,
  `set_brush`. `paint_stroke {points: [[x, y], ...]}` uses viewport coordinates 0..1 from the top left; aim with
  `set_camera` presets and `render_view`. With `set_symmetry {x: true}` the mirrored copies paint the other side
  even where the camera can't see it (1.2.5: Screen and UV brushes put those copies on the surface).
- Layers: `list_layers` (index 0 = bottom), `add_layer {type: paint | fill | adjust | folder}`, `update_layer`,
  `select_layer`, `move_layer`, `remove_layer`. A fill layer covers the model with a material; its `generator`
  (smart mask) limits it: edges, cavity, occlusion, gradient_y, noise, facing, material (a material slot), thin, drips.
- Smart materials: `list_smart_materials`, `apply_smart_material {id}` add a ready layer stack (rust, worn paint...).
- `auto_texture`: a library material per material slot, chosen from the slots' names.
- Bakes: `bake_mesh_maps` (AO, curvature, thickness) feed the smart masks; `bake_from_high_poly` bakes a sculpt's
  detail onto the model (1.1: `samples` points per texel, 4; `distance` -1 is measured from the two meshes).
- Bake-down (1.1): the user painted the high-poly and has a low-poly version (retopology). `bake_down {path: the
  low's file, samples: 9}` moves every layer onto it, and it becomes the model (a new project: tell the user to save
  it). For Patina's own quads: `retopo_use_as_model` (keep_layers true by default). The answer: `hit_fraction` (found
  along the normals; above 0.97 is good), `nearest_point_fraction`, `guessed_fraction` (above 0.01: the two are not
  in the same place there, or the low has a part the high doesn't), `flipped_fraction` (the low faces away from the
  high: turned inside out, or folded over a hollow), `largest_distance`, `warning`. Look at it with `set_view {mode:
  bake_check}` (gray found, amber nearest point, red guessed, magenta faces away) and `set_compare {enabled: true,
  mode: high_poly}` (the high as it was, left of the divider) in `render_view`. A "not in the same place" refusal
  changes nothing: the two need the same position, rotation and scale.
- Scenes (1.2): a project can hold several objects (a body, its clothes, eyes, hair), each with its own layers,
  texture set and history. `get_state` lists them (`objects`, `active_object`); every other command works on the
  active one, so `select_object {object: "Shirt"}` first, then `list_layers`, `add_layer`, paint... A file with
  several meshes opens as an object each (`open_mesh {separate: false}` keeps one); `add_object {path}` adds a file
  where it puts it. `set_object {object, location, rotation, scale, by, on_ground, reset, visible, name}` places one
  (one undo step); `pick_object {point}` tells which object is under a viewport point; `isolate_object` is the Local
  view; `export_model` writes every object shown (`objects: active` for one) and `export_textures {objects: all}` every
  object's maps named after it. From Blender, the add-on 0.14 sends all the selected meshes, each linked to its
  object.
- Faces in batches (1.2): `select_loop {point | face + edge, ring, mode}` takes the loop of quads across an edge
  (ring: along it); `select_linked {point | face, by: connected | flat | island | material, angle}` grows from a face
  or the selection. Face sets are named groups of faces kept in the project: `make_face_set {from: selection | faces
  | material | island | connected, name}`, `list_face_sets`, `edit_face_set {set, name, color, hidden, add, remove}`,
  `use_face_set {set, action: select | hide | show | isolate | show_all | fill | mask}` (fill: the paint material on
  the selected layer; mask: the selected layer's mask), `delete_face_set`; `set_brush {lock_face_set: true}` keeps
  strokes inside the set they start on; `set_view {mode: face_sets}` shows them. Hidden faces take no paint.
- Brushes (1.2): every image or Photoshop .abr tip in the user's brushes folder is a brush (`list_brushes`, ids
  `file:...`); `import_brushes {paths, category}` copies files in and selects the first; `reload_brushes` looks
  again.
- Fixing a bake (0.41): each baked map has a layer of type `baked` at the bottom of the stack. `baked_maps` lists them
  with `mean_baked` and `mean` (the map now). `baked_maps {map: ao, select: true}`, then `paint_stroke` with
  `brush: {mask_value}` paints fixes over the bake (AO: 1 no shadow, 0 full; curvature 0.5 flat): tool `erase`
  brings the bake back, `blur`, `smudge` and `clone` work on the map. `update_layer {mask_effects}` adjusts the whole
  map (levels, blur, fill...; no paint effect), `baked_maps {map, strength}` scales it (0 off, 2 double), `visible:
  false` shows the bake as it came out, `reset_fixes` / `reset_adjustments` undo the work. Smart masks, `render_view
  {mode: ao}`, `render_uv {channel: ao}` and exports all read the fixed map; baking again keeps the fixes.
- Look: `render_view`, `get_texture {channel}` (a final map as an image), `set_view {mode}`.
- Which layer makes what you see: `layers_at {point: [x, y]}` (the same viewport coordinates) lists the layers that
  paint that spot, top first, with their coverage there (0..1) and whether they are shown. Change the top one with
  coverage near 1 to change what shows; a hidden one (`shown` false) shows nothing.
- The interface's language (the windows only; commands and results stay English): `language_list`, `language_set
  {code}`, `language_new {code, name}` starts a translation (a .po file), `language_untranslated` lists the texts shown
  without one.
- What the user sees in the app's 3D view: `set_view {axis_gizmo}` shows or hides the X/Y/Z gizmo in its corner
  (never in renders); `retopo_view {preview: true}` shows the retopology zones and quads there whatever tool is
  chosen (the top bar's Retopology preview button; `render_view` shows what `retopo_view` sets either way).
- Views (0.38): `set_camera {view: front | right | top ...}` jumps to a side view and remembers the view before;
  `set_camera {view: previous}` goes back to it (again: to the side view), and the answer's `side_view` and
  `can_go_back` say where you are. (In the app the numpad and the gizmo also make side views orthographic; agents set
  `ortho` themselves.) `set_view {mode: solid}` shows the shape alone (light gray, lit, no textures) to judge the
  mesh; `get_state` gives the window's tool as `window_tool` (set_tool's names).
- Path tracing (0.39): `set_view {mode: path_traced}` shows the model path traced in the app's view (shadows,
  reflections, bounced light, the environment as the light, a shadow on an invisible ground); `path_trace` sets
  `max_samples` (512), `bounces` (6), `ground` (true), `clamp` (12, fireflies) and answers `samples` and `done`.
  `render_view {mode: path_traced, samples: 64}` returns such a picture now (more samples: less grain);
  `render_image {path: ..., path_traced: true, samples: 256}` saves one (a transparent one keeps the shadow in alpha).
  Judge the look in path traced renders: the raster modes have no shadows.
- Pie menus (0.40, the window only): `pie_menus` lists the four pies over the 3D view (Z view, Shift+Z channels,
  ` edit modes, Q Quick Favorites) with each item's `id`, `on` and `enabled`; `pie_menus {run: "<id>"}` does an item
  as if chosen there (e.g. `mode.roughness`, `view.wireframe`, `tool.island`, `tool.flow_loops`); `pie_menus
  {favorites: [8 ids, "" = empty]}` sets the user's Quick Favorites (`catalog: true` lists every id). Set favorites
  only when the user asks. The brush's flow, hardness and angle are `set_brush {flow, hardness, angle}` (G, Shift+G and
  Ctrl+F in the app with the Blender keymap).
- Pins: `uv_pin {pin: true | false, selection: true}` (or `faces`, or `all`) pins or unpins whole UV islands, so later
  UV edits leave them in place; it answers `pinned_faces` (all the pinned faces now), and `uv_info` gives the same
  count. In the app the Select tool's Pin and Unpin buttons (Alt+P over the 3D view) do this, and while the Select or
  Island tool is chosen (`set_tool {tool: select}`) pinned islands have blue stripes on the model.
- Only what the camera sees (0.45): `select_faces {mode: island, faces: [f], visible: true}` selects only the island's faces the camera sees
  (set_camera first): leaves out the inside of clothes and skins under them; `left_out_unseen` says how many.
- The window's layout (0.46): `layout` reports whether the window shows the Guided layout (the work in five steps:
  model, uvs, bake, paint, export) or the Full one, and each step's `state` (done, look: needs a look, todo) with
  `why`; a quick way to tell what a model still needs (a step at look: overlapping UVs, faces with no UV area, a bake
  older than the UVs). `layout {step: "bake"}` shows a step to the user, `layout {mode: "full"}` switches; only when the
  user asks. Headless it answers `available: false`. Replacing a project with unsaved changes in the window now asks
  the user first; commands (open_file, new_project) don't ask.
- Before an export (0.47): `export_textures {directory, preset, dry_run: true}` writes nothing and answers `plan`: every
  file it would write with `contents` (what its channels hold: "R occlusion, G roughness, B metallic"), `bits`, `size`
  and `srgb` (false: a data map, Blender's Non-Color). Tell the user what will be written when they ask, or check a
  preset's packing. Faces with no UV area (uv_info `faces_without_uv_area`): `select_faces {mode: no_uv_area}`, then
  `uv_unwrap {method: smart, selection: true}`.
- Islands by brush (0.43): `island_brush {point: [x, y], size}` is one dab of the Island brush (the same viewport
  coordinates; size in pixels, default the brush size); its faces join the selection (`remove: true` takes them out),
  with their mirror images under `set_symmetry {x: true}`. `reach: surface` (default) takes the faces within the
  brush's radius along the surface from the face under the point, so never the inner layer of a cloth with thickness
  or a part only near in space, and the same amount at any viewing angle; `reach: screen` takes the faces seen in the
  circle. It answers `faces` (taken), `changed` and `selected`; `list: true` adds `face_list`. Then `uv_make_island`
  (with `symmetry: ["x"]` the mirrored half is stacked; `uv_info` counts `mirrored_faces`). Models that are nearly
  symmetric (a cloth simulation, the middle a few millimeters off the plane) stack too since 0.43.
)MD";

}  // namespace

const std::vector<McpGuide>& mcpGuides() {
  static const std::vector<McpGuide> g = {
      {"workflow", "Workflow", "What Patina does and the order of the tools: open, mend, texture, develop, collision, export, batches.", kWorkflow},
      {"mesh", "Mesh tools", "mesh_check, mesh_fix, mesh_transform, mesh_reduce and mesh_collision: what each setting does and how to read the results.", kMesh},
      {"develop", "Developing the textures", "Adjustment layer settings (exposure, white balance, tone, vibrance, de-light) and tuning them with texture_check.", kDevelop},
      {"export", "Export and Blender", "export_model formats, texture sizes, levels of detail, collision names per engine; Blender tools.", kExport},
      {"batch", "Recipes and batches", "Recipe format and variables, built-in recipes, running a recipe on many files.", kBatch},
      {"painting", "Painting and layers", "Materials, brushes, layers, smart masks, smart materials, bakes, layers_at and the interface language in brief.", kPainting},
      {"parameters", "Every setting", "Every tool's settings with their ranges, defaults and effects (from the tools' own schemas).", ""},
  };
  return g;
}

const std::vector<McpPrompt>& mcpPrompts() {
  static const std::vector<McpPrompt> p = {
      {"finish_model", "Finish a model",
       "Open a model, check and mend it, develop its textures and export it for a use.",
       {{"path", "The model file", true}, {"use", "game, web, print or render (default game)", false}},
       "Finish the model at {{path}} for this use: {{use}} (game if empty), with Patina's tools.\n"
       "1. open_file; get_state (did its textures come in? note missing ones); render_view from front and persp.\n"
       "2. mesh_check; mesh_fix {all: true} if it has problems; mesh_check again and report what changed.\n"
       "3. If its size or orientation is wrong (stats.size, Blender axes, meters), mesh_transform.\n"
       "4. texture_check; if pbr_ok is false or the hints apply, add an adjustment layer (add_layer type adjust) and "
       "tune it with update_layer and texture_check (read the guide 'develop'); render_view to confirm.\n"
       "5. By use: game: mesh_reduce if over 50k triangles (preview first, keep error under 0.005), mesh_collision "
       "{mode: pieces}, export_model .glb with lods [0.5, 0.25] and collision 'unreal' unless told otherwise. "
       "web: mesh_reduce to about 20k triangles, export_model .glb texture_size 1024. print: mesh_fix with "
       "fill_hole_edges 512 until mesh_check says closed, mesh_transform ground true, export_model .stl. "
       "render: export_model .glb at full texture size.\n"
       "6. Report the numbers before and after (faces, problems, texture_check, fit) and where the files are."},
      {"develop_textures", "Develop the textures",
       "Correct a model's textures (too dark, a color cast, baked lighting) with an adjustment layer, by numbers.",
       {{"goal", "The look wanted, e.g. 'neutral, daylight' (default: physically plausible and neutral)", false}},
       "Develop the open model's textures toward: {{goal}} (physically plausible and neutral if empty).\n"
       "Read the guide 'develop' (patina_guide). Then: texture_check and render_view (front, persp) to see where it "
       "stands. If the color carries baked shading (dark creases on a scan or generated model), bake_mesh_maps "
       "{ao: true} and start with delight 0.6. Add one adjustment layer at the top (add_layer type adjust, name "
       "Develop). Change one or two settings at a time with update_layer; after each, texture_check and render_view. "
       "Stop when pbr_ok is true and the look matches the goal. Report the settings and the numbers before and after."},
      {"game_ready", "Make it game ready",
       "Mend, lighten and add collision and levels of detail for a game engine.",
       {{"engine", "unreal, godot or unity (default unreal)", false},
        {"triangles", "Triangle budget for the main level of detail (default: keep it unless over 50000)", false}},
       "Make the open model ready for {{engine}} (unreal if empty), triangle budget {{triangles}}.\n"
       "mesh_check and mesh_fix {all: true}. If over budget: mesh_reduce {triangles: <budget>, preview: true}, check "
       "error (under 0.005 is safe), then without preview. mesh_collision: box for box-like props, hull for round "
       "ones, pieces for anything that bends in; aim for fit under 1.2 with at most 16 shapes. For Unity and Unreal, "
       "mesh_lightmap_uvs (overlaps must be 0). export_model .glb "
       "with texture_size 2048, lods [0.5, 0.25, 0.125] and collision unreal / godot / plain (Unity). Read the guide "
       "'export' for the collision names. Report the files and the numbers."},
      {"batch_folder", "Batch a folder",
       "Run a recipe on every model in a folder.",
       {{"folder", "The folder of models", true}, {"recipe", "Recipe id (default: ask recipe_list and pick)", false},
        {"output", "Output folder (default: a patina_out folder in it)", false}},
       "Run a recipe on every model in {{folder}}. Recipe: {{recipe}} (if empty, call recipe_list and choose the one "
       "that fits, or build steps; read the guide 'batch'). Output: {{output}}. Call batch_run with folders [the "
       "folder], then batch_status every few seconds until it is done. Report how many worked and why any failed."},
  };
  return p;
}

}  // namespace patina
