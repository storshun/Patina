# Patina features

Every feature, with the version it came in where that helps. The [README](../README.md) gets you started, the manual
(*Help › Manual* in the app, or [docs/manual](manual/index.html)) explains each tool, and the
[change log](../CHANGELOG.md) lists what changed in each version.

* **UV-independent painting.** Every brush dab is evaluated per texel *in 3D*
  (projected through the camera and depth-tested), so strokes cross UV seams,
  mirrored/overlapping islands and differently scaled islands without breaks.
  Materials are projected tri-planar in object space (or by UV / from the view).
* **Material painting.** The Patina default library: 270 built-in `patina_*` materials from 74 procedural generators (brushed /
  scratched metal, rust, chipped paint, concrete, wood, stone, leather, fabric,
  tiles, marble, carbon fiber, car paint, diamond plate, hammered, corrugated,
  perforated, sci-fi panels, terrazzo, sand, moss, asphalt, bark, wicker, camo,
  bricks and pavers, parquet, hex tiles, scales, metal grilles, quilted leather,
  knits, knurling, stucco, twill / satin / tartan cloth, emissive…), a **Character**
  set (skin tones from fair to deep, realistic and anime hair, silk, school plaid)
  and a **Stylized** set (toon colors, hand-painted stone, wood and metal),
  plus your own: import any
  folder of PBR maps (ambientCG / Poly Haven naming) or design procedural ones
  in the material editor.
* **Brushes.** 25 presets and 16 procedural tips (round, square, star, splatter,
  chalk noise, scratches, grunge, dots, hatch, cracks, bristle, spray, leaf, ring,
  cloud, rust) or any grayscale image; size/angle/flow jitter, scatter, follow
  path, roundness, lazy mouse, line tool, symmetry, **tablet pressure** (SDL3 pen
  API, Wayland and X11). Three projection modes: screen (Substance-style), 3D
  surface sphere, and UV space; paint-through and angle falloff options; paint,
  erase, **blur**, **clone stamp** (Ctrl+click a source on the model or in UV
  space; aligned or fixed), polygon fill (face / UV island / connected / mesh)
  and color pick tools.
* **Layers.** Paint, fill and **adjustment** layers (hue / saturation /
  brightness / contrast and roughness / metallic / height offsets of everything
  below, through a mask) and nested **folders** with their own opacity, blend
  mode and mask; 10 blend modes, per-channel enable and opacity, painted masks,
  smart masks from baked **AO / curvature** (edge wear, cavity dirt, occlusion,
  gradient, noise). **Solo** any layer (Alt+click its eye), color tags,
  copy/paste layer settings.
* **Smart materials.** Save a layer or folder and apply it to any model; 8 built
  in (worn painted metal, rusted iron, aged brass, dusty plastic, weathered wood,
  mossy stone, snow cover, edge-wear overlay).
* **Undo for everything, with a History panel**: strokes, every slider (one step
  per drag), masks, layer structure, UV edits. Click a step to jump back.
* **Island brush (new in 0.4).** Paint over the part of the model you want as
  one piece of texture and press Enter (U in the Blender keymap): Patina cuts
  seams around it, unwraps it (LSCM), turns it upright, scales it to the target
  **texel density** (fit the texture, one texel per screen pixel at 1080p, the
  existing layout, or a custom px/m) and drops it into free space without moving
  the other islands. Paint stays put; one undo step. Texel-density and UV-stretch
  **heat maps**, one-click overlap repair, orient upright / along the longest side,
  pack into free space, grow / shrink the selection. See
  [docs/ROADMAP.md](ROADMAP.md) for the full 0.4 hit list.
* **New in 1.2.0. Scenes of several objects, face sets, your own brushes.** A project holds several models, each an
  object with its own layers: a character's body, clothes, eyes and hair. The **Objects** list over the layers picks
  the one painted. A file with several meshes opens as an object each, and **File › Add a Model to the Scene** adds
  more. The **Object tool** moves, turns and scales them. From Blender, the add-on 0.14 sends every selected mesh.
  Exports, renders and the path tracer take the whole scene. The Select tool takes **loops and rings**
  (<kbd>Alt</kbd>+click, <kbd>Ctrl</kbd>+<kbd>Alt</kbd>+click), connected parts and flat regions. **Face sets** are
  named groups of faces to select, hide, fill or mask in one click, with a lock that keeps strokes inside one.
  Every image or Photoshop `.abr` file in your **brushes folder** is a brush. What changed in each version is in
  [CHANGELOG.md](../CHANGELOG.md).
* **New in 1.1.0. Bake down from the high-poly.** Paint the sculpt, where the detail is, then bake every layer down
  onto its low-poly version: *Bake › Bake Down to a Low-Poly Model* with a file from Blender or any tool, or *Use the
  quads as the model* after Patina's retopology. The low-poly becomes the model and the layer stack comes along. Paint,
  masks and fixes are measured texel by texel, at 9 points per texel, each looking along the low-poly's normal for the
  high-poly's surface. Painted normals are turned to fit the new surface, and the high-poly's shape goes into the
  normal map and ambient occlusion. The **Bake-Down Check** view shows where the transfer had to take the nearest point
  (amber) or guess (red). The **compare view** puts the high-poly as it was beside the result. On test models the
  colors come through to within 8-bit rounding, and painted normals to within a degree. Where two parts nearly touch,
  rays find the right one where the nearest point got 4 in 5 texels wrong. The high-poly bake uses the same rays: 4
  points per texel, and its distance is now measured from the two models.
* **Patina 1.0.0** (2026-10-21): the first stable release. Everything on this page, with the **Guided layout** for a
  first project (Model, UVs, Bake, Paint, Export, each step in its color) and the **Full layout** for every panel at
  once. Projects, presets, keys and layouts from the 0.x versions open as they were.
* **New in 0.47.0.** **Each step has a color**: the Guided layout's steps are folder tabs (Model blue, UVs teal, Bake
  violet, Paint copper, Export green). The step in front is raised and joined to the strip under the tabs, and the whole
  window takes on its tint. In the Full layout the same colors mark the tool strip's groups, the panels' tabs and the
  menus, which follow the steps' order (Mesh, Retopology, UV, Bake, Blender). **The export shows what it will
  write**: every file's name, what its channels hold, its bits and whether it is sRGB or Non-Color, before you press
  Export. **The UV menu is in groups** (Unwrap, Islands, Mirrored Halves, Pins, Selection, Seams). **A new Patina's
  brush paints a color**, not iron. **Tiny faces were reported as having no UV area** (58 on the sample Suzanne): the
  check lost their area in rounding, and now counts them right. MCP: `export_textures` with `dry_run`, `select_faces`
  `no_uv_area`.
* **New in 0.46.0.** **The Guided layout**: the work in five steps along the top: **Model, UVs, Bake, Paint,
  Export**. Each step is a screen with what it needs in front, and a mark that says whether it is done, needs a look
  (overlapping UVs, a stale bake...) or is still to do, with the fix one click away. New installs start in it.
  **Guided | Full** at the right of the menu bar switches to the docked layout of before (a Patina used before keeps
  it). The first start asks where you come from (Blender, Maya or something else) and where your textures go. **Unsaved
  work is never replaced without asking** (New, Open, Open Recent, a dropped file), and **a brush that would paint
  nothing says why**. Search everything also opens with F3 (and Ctrl+K with the Blender keys). The research behind it: `docs/UX_REVIEW.md`.
  MCP: `layout`.
* **New in 0.45.0.** **Select only what the camera sees**: the Select tool's new **Visible only** takes just the
  faces of a UV island that the camera sees (turned toward you, on screen, not hidden behind other faces), so the
  inside of a dress or a skin under clothes stays out of the selection. Turn the view and click again for more;
  Ctrl+click takes the seen faces off. MCP: `select_faces` with `visible`.
* **New in 0.44.2.** **Painting is fast again on big texture sets.** A model with several texture sets shares one
  large texture (6 sets: 6144 x 6144), and every brush move used to redo all of it; a stroke now updates only what it
  touches, so painting runs at full frame rate (about 12 fps before on Mai). **Baked maps can be switched off**: the
  checkbox of a baked map's layer takes it out of the 3D view and the exports. **Out-of-date bakes are shown**: after
  UV edits the Baked maps section offers Re-bake, which removes the dark patches a moved bake leaves.
* **New in 0.44.1.** **Your UV islands survive a send from Edit Mode.** Blender 5 hides a mesh's UVs from add-ons
  while it is in Edit Mode, so an object sent from there reached Patina as "no UVs" and was unwrapped again: the
  islands kept their outlines but broke into dozens of jagged pieces. The add-on (0.13.1) now reads the UVs in Edit
  Mode, and Patina keeps its own UVs on an object that comes back without any, also after faces were added or deleted.
* **New in 0.44.0.** **Blender objects come back to their project**: when Blender sends an object to a Patina that
  doesn't have it open (after a restart), Patina opens the saved project it was painted in, with all its layers, and
  moves the paint across if the mesh changed. It finds the project you last saved or opened for the object, or one in
  your recent files. Without a project, the object starts from **what its Blender materials show** (add-on 0.13), so
  pushed textures come back as a layer. And **a color attribute that is one flat color no longer turns the model
  black**.
* **New in 0.43.0.** **Symmetric islands on models that are only nearly symmetric**: the vertices down the middle of
  a cloth simulation or a sculpt wander off the mirror plane (millimeters, or a whole panel to one side), and Mirror X
  now pairs them with themselves, so the faces along the middle are stacked too. Before, the middle of such a model was
  left unstacked and its islands came out lopsided (on Mai's body the whole center line, front and back). Cloth with
  thickness pairs round its rim as well. **The Island brush never selects through the mesh**: a new **Surface** mode
  (the default) takes the faces within the brush's size along the surface, the same amount at any viewing angle,
  never the inside layer of a cloth with thickness or a part only near in space; **Screen** (the circle on screen) no
  longer takes the layer just behind the one you paint. MCP: `island_brush`.
* **New in 0.42.0.** **Right-click a slider to type its value** (Enter keeps it, Esc goes back): the quick way to
  exact values such as a brush size of 100. Typed values stay in the slider's range; a left click works as before.
  **The Blender bridge pushes again**: an object sent to Patina without UVs was refused every update ("UVs changed in
  Blender") once Blender's object had any other UVs, so neither UVs nor textures came across. Now Patina's UVs go in
  (they are the ones its textures fit) and Blender's are kept in a UV map called *UVMap before Patina*. Needs the
  Blender add-on 0.12 (`patina_bridge.zip`).
* **New in 0.41.0.** **Fix a bad bake: baked maps are layers.** The ambient occlusion, curvature and thickness each get
  a layer in a **Baked maps** section under the layer stack. Select one and the 3D view shows that map; paint on the
  model to fix it (the paint value, say 1 for no shadow), erase to bring the bake back, blur, smudge and clone it, and
  add adjustments (levels, blur, fill...) to the whole map. **Strength** scales the map (0 off, 2 double) and the
  layer's check compares with the bake as it came out. Smart masks, the 3D view and exported maps all use the fixed
  map, and baking again keeps your fixes. Agents use `baked_maps`. Older projects get the layers when opened.
* **New in 0.40.1.** **The wireframe and seams stay on the side you can see.** Lines on the far side of the model,
  or on a part behind another (an arm behind the body, a body under a layer of cloth, the inside of the eye sockets),
  no longer show through. They used to be pulled toward the camera by a share of the depth range, which came to a
  third of the model and more far from the camera. Now they are pulled a pixel or two, measured where each line is.
* **New in 0.40.0.** **Four pie menus.** The view modes and the edit modes now have pies of their own. Hold **Z**
  for the view (lit, solid, unlit, path traced, wireframe, seams, the UV checker, the retopology preview, laid out like
  Blender's shading pie), **Shift+Z** for the channels (base color, normal, roughness, metallic, height, emissive, AO,
  the layer mask), **`** for the edit modes (the tools), and **Q** for **Quick Favorites**: eight items of your own.
  Right-click a toolbar button or a pie item and choose *Add to Quick Favorites*, or choose an empty place to search
  everything a pie can hold; *Preferences › Pie menus* lists them. **Brush keys:** with the Blender keymap, **G** sets
  the flow, **Shift+G** the hardness and **Ctrl+F** the angle by moving the mouse, as F and Shift+F set the size and
  opacity (Blender's strength). Wireframe, seams, lit, solid, unlit and path traced are shortcut actions now, so you
  can give them keys. Switches in *Preferences › Features* (`pie_menus`, `brush_keys`); MCP `pie_menus`.
* **New in 0.39.0.** **Path traced view and renders.** A new *Path traced* view mode follows the light the way a
  renderer such as Cycles does: real shadows, the model reflected in itself, light bouncing between its parts, every
  painted channel (normal maps, emissive, opacity, sheen, clearcoat, subsurface), lit by the environment you already turn
  with Shift + right drag, and a shadow on an invisible ground. It runs on the graphics card and gets cleaner the longer
  nothing changes (a quick picture first while you orbit or paint; 60 fps with a million triangles here). The
  *Render image* dialog can render path traced, in steps with a progress bar, while the window keeps working. MCP:
  `path_trace`, `set_view {mode: path_traced}`, `render_view {samples}`, `render_image {path_traced, samples}`.
* **New in 0.38.0.** **Side views the Blender way.** The numpad and the axis gizmo now jump to the side, top and
  bottom views, which are orthographic until you orbit away. **Numpad 0** (or the Back button under the gizmo) takes
  you back to the view you had before, and again to the side view. Hold **Alt while orbiting** with the middle button
  and the view snaps to the nearest side view as you move (Shift during an Alt+left orbit). Hold **Z** for a **view
  pie**: wireframe, a new **Solid** view (the shape alone, light gray), unlit, seams, the Paint and Island tools, clear
  the selection, and the retopology lines or edge loops. Patina Web's gizmo jumps too. Fixed: an Alt+drag in the 3D view
  could hand the keyboard to the menu bar when Alt was let go. Switches in *Preferences › Features* (`view_snapping`,
  `view_pie`, `solid_view`); MCP `set_camera {view: previous}`, `set_view {mode: solid}`, `get_state` `window_tool`.
* **New in 0.37.1.** **Pin and unpin from the 3D view.** The Select tool's bar has **Pin** and **Unpin** for the
  selected faces' islands, with how many of them are pinned; the Island tool's bar has **Unpin** for the painted faces.
  **Alt+P** unpins over the 3D view too (before, only in the UV editor), and right-click on the model pins or unpins the
  island under the pointer. While either tool is chosen, pinned islands have **blue stripes** on the model, so you can
  see them before you select. Patina Web's Island brush has the same buttons, Alt+P and stripes. A switch in
  *Preferences › Features* (`viewport_pins`); agents use `uv_pin`.
* **New in 0.37.0.** A **Retopology preview** button in the bar over the 3D view shows the retopology zones (or
  density) and the quads on the model whatever tool is chosen, and keeps the quick preview following each change;
  before, they showed only while the Retopology tab was in front. Its arrow chooses what shows. An **axis gizmo** in
  the 3D view's corner, in the app and in Patina Web, shows the X, Y and Z axes in Blender's colors as the camera turns:
  click an axis to look along it (click it again for the other side), drag it to orbit. Both are switches in
  *Preferences › Features* and reachable from MCP (`retopo_view {preview}`, `set_view {axis_gizmo}`). Fixed: with
  the panels hidden (*Tab*), the 3D view took no strokes, navigation or clicks.
* **New in 0.36.0.** **Icons in the toolbars**: the tool strip and the bars over the 3D view, the UV editor and the
  layers show icons drawn after Photoshop's, Krita's and Blender's (brush, eraser, water drop, finger, stamp, bucket,
  eyedropper, scissors, mirror...), in Patina's copper. The tools are grouped by job: paint, UV, retopology, symmetry,
  notes. Rest the pointer on one for its name and shortcut; right-click it to change the shortcut; Preferences can put
  the names beside the icons. The **retopology tools are always in the strip**: no need to open the Retopology tab
  first. **Right-click menus** everywhere: in the 3D view for the spot under the pointer (pick its color, the layers
  painted there, fill or select its island or part, a note, the clone source, center the view) and for the tools,
  symmetry, brush and view; on the tool strip, the layers and the empty part of the list, mask effects, materials,
  smart materials and masks, brushes, colors, history, notes and the console. `layers_at` gives agents the layers
  that paint a spot. **Translations**: *Preferences > Language* shows Patina in another language from a gettext
  `.po` file (made with Poedit, Weblate, Crowdin...); `languages/patina.pot` is the template with every text of the
  interface (2,407 texts); *Make a new translation* starts one; a test language marks every translatable text; Chinese,
  Japanese and Korean use a system font (or the one the file names). The manual's new *Translating Patina* page is
  the guide, and MCP has `language_list`, `language_set`, `language_new` and `language_untranslated`. **The last
  British spellings**: the material ids `patina_flat_gray`, `patina_plastic_satin_gray`, `patina_wool_gray`,
  `patina_knit_seed_gray`, `patina_windowpane_gray`, `patina_knurled_aluminum`, `patina_scale_armor`,
  `patina_honeycomb_armor` and the smart materials `patina_smart_oxidized_copper` and `patina_smart_stylized_edges`
  (the old ids still work), and the manual's screenshots, taken again with the new interface. On Windows the
  interface uses Segoe UI (it used a small built-in font). Each of the three new features can be switched off in
  *Preferences > Features*.
* **New in 0.35.1.** American English throughout: the interface, messages, material names, the manual, the MCP
  tools and guides, and the Blender add-on (0.11.1). Settings and keys that scripts use keep working in both
  spellings: `vertex_colors` (with `vertex_colours`), `swap_colors` (or `swap_colours`), the transform origin <!-- either spelling -->
  `center` (or `centre`), and retopology's `center` (with `centre`); the auto texture style takes `stylized` or
  `stylised`. Material ids such as `patina_flat_grey` stay as they are, so projects open as before. The way Patina is
  described: post-process your models the way you would a photo; non-destructive, layered, and scriptable. A
  test (`spelling`, `scripts/american_spelling.py`) keeps the spelling American.
* **0.35.0.** **Lightmap UVs** (*Mesh › Lightmap UVs…*, `mesh_lightmap_uvs`): a second UV map for baked
  lighting in game engines: no face shares space, even detail, a gap in the lightmap's pixels; from the model's own
  islands (folded ones unwrapped again) or new charts by xatlas (the unwrapper Godot uses; Patina carries a fixed copy).
  Saved with projects, exported as glTF `TEXCOORD_1`, read back from glTF, sent to Blender as the UV map *Lightmap*
  (add-on 0.11, which also sends an object's second UV map and passes vertex colors both ways). The Game asset recipe
  makes them.
* **0.34.1.** Every setting of every tool (168 of them: painting, layers, UVs, bakes, retopology, mesh,
  export) now tells agents its range, default and what it does; the MCP test checks it stays that way.
* **0.34.0.** **Collision shapes** (*Mesh › Collision…*, `mesh_collision`): an oriented box, boxes per part, a
  convex hull, a hull per part, or the model split into convex pieces (voxels, split where a hull would hold too much
  empty space), shown in green and exported after the model with Unreal (`UCX_`/`UBX_`), Godot (`-convcolonly`) or
  plain names. The Game asset recipe makes them. **MCP for agents**: every post-processing tool's settings carry their
  range, default and effect; guides (`patina_guide`, `patina://guide/…` resources, one written from the schemas),
  prompts (finish a model, develop the textures, game ready, batch a folder), new instructions, and
  **`texture_check`**, numbers and hints for tuning the develop settings.
* **0.33.0.** **Develop the textures**: adjustment layers get a photo editor's basic develop controls: temperature and tint,
  exposure (in stops), highlights and shadows, vibrance, and **De-light**, which takes the shadows baked into scanned
  and generated textures out using the ambient occlusion (baked when needed). They work in linear light, before the
  older settings; at their defaults nothing changes. The **PBR Range** view mode marks base colors no real material
  has (too dark, too bright, dark metals, half-metal).
* **0.32.0.** **Vertex colors**: models that carry their color per vertex (generated models, scans,
  vertex-painted objects) open with it as the layer *From the model*: from glTF (`COLOR_0`, times the material's
  color), OBJ (`v x y z r g b`), FBX, the new **PLY** reader (ASCII and binary, UVs, normals, Z up as Blender reads it)
  and Blender objects sent with the add-on (0.10 sends the active color attribute). **PLY export** writes the base
  color as vertex colors, with UVs and normals.
* **0.31.0.** **The model's own textures**: opening a textured model (.glb, .gltf, .obj with its .mtl, .fbx,
  textures inside the file or beside it) brings each material slot's maps and colors in as a paint layer, *From the
  model*, at the bottom of the stack: color, roughness, metallic, normal, glow and opacity, with texture transforms.
  The model looks as it did in the program it came from, and you work on top of it. Missing textures are named; the
  texture set grows to the textures' size (up to 4K). *File › Bring In the Model's Textures* and
  `import_model_textures` do it again. Batches and recipes get it too, so textured and generated models keep their
  look through mending, reducing and exporting.
* **0.30.0.** **Recipes and batches** (*File › Batch Process…*): a recipe is a list of steps (mend, transform,
  reduce, bake, texture, export); run it on the open model or on whole folders of models in a hidden Patina, which
  leaves the files as they were and writes the results to an output folder, with progress and a result per file. Seven
  built-in recipes (Game asset, Web and AR, Clean up, 3D print, Auto texture, Worn painted metal, Centimeters to meters)
  and your own. **Transform** (*Mesh › Transform…*, `mesh_transform`): units, scale, turn, origin, stand on the ground.
  **STL export** for 3D printing. `patina run` takes jobs with `files` and `per_file` steps and `--progress`.
* **0.29.0.** **Reduce polygons** (*Mesh › Reduce Polygons…*, `mesh_reduce`): fewer triangles, keeping the UV
  layout (UV seams, material borders and every corner's UV stay, so the textures fit as they are) and open borders, with
  a preview of the count and *Back to before the reduction*. **Levels of detail** in *Export Model*: lighter copies
  that share the textures (glTF `_LOD0`, `_LOD1`… with `MSFT_lod`; OBJ `_LOD1.obj`…). Uses meshoptimizer (MIT).
* **0.28.0.** **Export the model** with its textures (*File › Export Model…*, `export_model`): GLB (all in
  one file), glTF or OBJ, a material per texture set or UDIM tile, opacity as glTF Mask or Blend, sheen and clearcoat as
  glTF extensions. **Send the model to Blender** (*Blender › Send Model to Blender as a New Object*,
  `send_model_to_blender`, add-on 0.9.0): the model as it is in Patina (mended, its UVs, slots, seams and vertex groups)
  becomes a Blender object next to the linked one, with Patina's material, and Patina links to it; or replace the
  linked object's mesh.
* **0.27.0.** The **Mesh Check** (*Mesh › Check Mesh…*): what a model is made of (faces, triangles drawn, parts,
  slots, UV islands, size in meters, area, volume, closed or not, UV space used) and what is wrong with it: holes,
  non-manifold edges, bowtie vertices, flipped faces, inside-out parts, open seams, degenerate and duplicate faces,
  specks and floaters, faces without UV space, overlapping UVs, odd scale. A heat map shows them on the model; Select,
  Look and Fix for each, and **Fix everything that can be fixed**. Fixes keep the paint, and *Back to before the fix*
  returns to the model as it was. Commands `mesh_check`, `mesh_fix`, `mesh_fix_revert`. The first of the roadmap's
  section R (post-process your models the way you would a photo).
* **0.26.0.** Four more material properties, painted like the others: **opacity** (lace, fishnet, leaves, hair
  cards, chiffon), **sheen** (velvet, satin, fur), **clearcoat** (car paint, lacquer) and **subsurface** (skin, wax, jade).
  The 3D view shows them, exports write their maps when they are used, and the Blender add-on connects them. 11 new
  generators: fur, feathers, leaves, rope, damask, ice, crosshatching, lace, net and mesh, hair cards and velvet (with
  corduroy and devoré), and *Cut out holes* for every generator with holes. 45 new library materials (270 in all); the
  skin, satin, velvet, car paint, marble and hair materials gain subsurface, sheen or clearcoat.
* **0.25.0.** Patina's materials in Blender. Click **Add Patina Materials to Blender** in Blender's Patina tab
  (add-on 0.8.0) or in Patina's Blender Link window: Patina bakes every material and Blender gets a *Patina Materials*
  library in its **Asset Browser**, in Patina's categories with Patina's previews, ready to drag onto objects. Box
  mapping projects them as Patina does (no UVs needed), or choose UV mapping; your own materials come too. Click again
  to update it. `export_material_maps` can write JPEG maps, 16-bit heights and your own materials. The roadmap gains
  section R: post-process your models the way you would a photo.
* **0.24.0.** More procedural materials. 16 new generators (stripes, checks with gingham, houndstooth and
  argyle, dots and halftone, animal prints, cracked earth and lava, flagstones, pebbles, rock layers, crackle,
  chainmail, damascus steel, circuit boards, stained glass, mosaic, sequins and glitter, paper) and 52 new library
  materials, 225 in all. Every procedural material gets **Grime** (dirt in the low parts), **Worn edges** and
  **Warp**. The material editor names each generator's own settings, with drop-downs for its styles. Masks gain
  **Pattern** (any material as a mask: drag one onto *Add a mask effect…*), **Grow / shrink**, **Edges**, the
  **Gradient** and **Drips** smart masks, and cells, cracks, ridges and streaks for noise.
* **0.23.0.** Retopology: a change makes only the quads near it again. Draw a line, a stroke or paint the
  density and the quads more than a few quads away stay exactly as they were, so a part that came out clean stays
  clean; a row of faces joins the new quads to them. **Remake all** (beside Make quads) makes everything again;
  untick **Keep quads away from changes** for the old behavior.
* **0.22.5.** Drag and drop into the layer stack. Drag a material onto a fill layer to give it that material
  (its smart mask stays), or between layers for a new fill layer there. The Materials panel has **Smart materials**
  and **Smart masks** tabs whose tiles drag into the stack or onto a layer. Drag a fill layer's thumbnail onto
  another to copy its material, or a mask onto another layer to copy the mask; Ctrl+drag copies a layer. Fixed: the
  layer list ran past the foot of a short Layers panel, and the Make independent button in an instance's settings did
  nothing (the right-click menu's worked).
* **0.22.4.** Retopology: flow strokes make edge loops by default (untick **Strokes make edge loops** for
  strokes that only turn the quads). With Mirror X, creases along the middle (the crotch, the cleavage) no longer leave
  doubled, folded faces, a line drawn close to the middle joins it cleanly instead of making a pole, and small holes
  beside hair are filled.
* **0.22.3.** Retopology with Mirror X (Y, Z) makes the quads on one side and mirrors them, meeting
  along an edge loop down the middle (a piece that isn't the same on both sides keeps its own quads). The Flow
  tool's **Strokes make edge loops** makes each flow stroke an exact loop without a zone of its own. Fixed: the
  Retopology window's Show: Density option clashed with the Density tool's button (a red Dear ImGui error), and
  the Export window's Export choice with its button; a test now checks every window for such clashes.
* **0.22.2.** While a retopology line is drawn everything else waits: the 3D view and the UV Editor
  reuse their last pictures and the live preview stops until you let go. On Windows laptops with two graphics
  cards Patina asks for the fast one. `PATINA_FRAME_STATS=<file>` logs frame timings.
* **0.22.1.** Retopology: Shift+click with Lines where there is no part to go round (a breast, the
  face) draws the cut facing you, through the point you click; Shift+wheel switches between that and
  round the part.
* **0.22.0.** Retopology lines: those behind the model are darkened (or hidden, or shown: your
  choice in the window); **Snap line ends** can be turned off for tight folds (lips, fingers, the crotch);
  **Confirm lines** (or Ctrl+Shift+click one) turns finished lines darker so new lines never snap to them.
  Make quads stays at the foot of the Retopology window.
* **0.21.0.** Retopology: **Fewer poles** (on by default). After the quads are made, the number
  of quads along each run of edges is worked out again, so a three-edged and a five-edged pole close together
  cancel out. The new quads are kept only when they have fewer poles and no more folded faces.
* **0.20.8.** Retopology: faces that come out folded (turned over against the surface or the
  faces round them) are smoothed out after the build, so fewer pinches and spikes where zones meet.
* **0.20.7.** Retopology: a ring shares the face's edge loop more often (up to a fifth of the loop's
  points may be put onto the line), so fewer three-to-one steps round eyes and mouths.
* **0.20.6.** Retopology: hold Shift with the Lines tool and rest the pointer on an arm, a leg or the
  neck to see the line right round it that a click would draw.
* **0.20.5.** The tool windows open when Patina closes (Retopology, Assistant, Texel density, Mesh
  symmetry, Auto texture, Macro recorder, Material Editor) are open again next time, in the layout you left.
  The Retopology window starts docked as a tab beside Properties (also in a layout from before); retopology's
  preview, overlays and tool buttons only run while its tab is in front or a retopology tool is chosen.
* **0.20.4.** Retopology: Shift+click with the Lines tool on an arm, a leg, the neck, the waist or a
  finger draws a line right round it there, across the way it runs (no turning the model round to draw
  the back); a line round the top of each arm and leg keeps their quads in clean rings. Mirrored with the
  Sym buttons; nothing on a part that is no tube (a head). Also as the `retopo_ring_line` command.
* **0.20.3.** Retopology: a ring that can't take the loop of the zone beside it after three
  tries is made like the other zones (a joining row there could have come out flat). The manual's lines
  for a character say why a line round the top of each arm and leg matters: without it a limb's rows can
  wind round it like a screw thread.
* **0.20.2.** Retopology: the middle of an eye or a nose tip drawn as a small loop inside a
  band that goes with the face is now made with the face too. It used to be closed on its own with a few
  points and joined to the face's many, which on the girl's face left a web of stretched quads between
  the eyes and the nose. Girl head 94.6% to 95.2% four-edged; Suzanne layouts 96.2%.
* **0.20.1.** Retopology: an arm or leg no longer comes out with its rows wound into a helix
  (stretched quads, no pole to show for it): each of QuadriFlow's tries is now judged also by how far its
  edges lean off its own flow, so the twisted ones are passed over. 0.20.0 twisted the girl's left arm
  when the hand's lines were near; fixed. A line drawn on the face no longer remakes the body's quads.
  Suzanne layouts 96.1% four-edged, 18% fewer poles than 0.19.7; the girl with ten body loops 95.1%.
* **0.20.0.** Retopology: the quads follow the lines you draw much more closely. QuadriFlow's
  edge loops now run along a curved line instead of crossing it in steps (a circle and a wavy line on a
  ball: 17 poles on the lines before, none now), and the rings round an eye or a mouth take the edge loop
  of the face beside them where it runs along their line, so far fewer three-to-one steps where they
  meet. Suzanne layouts: 95.2% to 95.8% four-edged vertices, 12% fewer poles; Randall's own lines 93.1%
  to 95.4%.
* **0.19.7.** Retopology: a zone group nothing changed in is not made again - with the head,
  the body and the hands at different resolutions, drawing on the head no longer remakes the others, and
  a build with nothing changed (an undo back to it, say) is instant. Mending a join where zones meet no
  longer closes a hole the model has itself (an eye socket).
* **0.19.6.** Retopology: a line drawn inside a zone splits it into zones that keep its
  resolution (give the head x8, then draw the eyes, and the eyes are x8 too; one undo step takes both
  back). Open models (clothing: a skirt, a hood, a sheet) are steadier: where holding the model's open
  edges sent QuadriFlow's count far off or tore a hem, the quads are made again with them free (a hood:
  100% four-edged in 8.6 s, was coarse and 48 s; a skirt with lines to its hem: the hem followed, 8.6 s,
  was torn and 31 s).
* **0.19.5.** Retopology: a crash while the live preview rebuilt (drawing and undoing lines
  quickly, found by a stress test of the window) is fixed - QuadriFlow solved with a failed factorization
  of a singular system. A stretched model for which QuadriFlow gave no quads at all now gets them.
* **0.19.4.** Retopology fixes from 340 random layouts (no crashes): lines that crossed so every
  zone was a ring or a disc made no quads at all; a zone with two lines round it but a bulge between them
  (the back of a head) was filled with rings far apart and 40% of it left out; a long stretched model with
  nothing drawn made no quads; a small loop near the middle with symmetry on folded flat. Where zones meet,
  a few overlapping faces are now mended in place rather than the whole build made again another way
  (often seconds quicker). What is too thin even for a x16 zone (strands of hair) is said as such. The
  manual has a new section, *Lines for a character*.
* **0.19.3.** Retopology fixes: with a ring along an arm (a line round the shoulder as well as the
  wrist), the hand past the wrist was closed with a grid and lost its fingers; a disc is now closed with a
  grid only when it is no more than a dome. A long, thin zone could crash the build (a division by zero in
  QuadriFlow's solver), and a few states QuadriFlow didn't expect closed Patina; both now just drop that
  try.
* **0.19.2.** **Retopology: rings only where they pay.** A ring between two lines that would need
  many three-to-one steps (the band round an eye socket, much longer outside than in) is now made with
  the zone beside it, which turns its flow with a few poles of its own; the line stays an edge loop. The
  counts round the rings are planned with the loops they will share, and a disc with an odd count is
  still closed with a grid (and one triangle). Suzanne with lines round each eye, socket, the nose and the
  mouth: 95.4% four-edged (93.2% in 0.19.1); lines drawn less tidily no longer fall back.
* **0.19.1.** **Retopology: fewer poles.** Where zones are joined, pairs of poles are now taken
  out (as QuadriFlow's own quads get), never on a line or in a ring: about 2% more four-edged vertices
  (Suzanne with lines round the eyes, nose and mouth: 93%; the anime girl: 94.5%; Suzanne with a face at
  x2 and a snout at x3: 96%). The grid closing a disc along a line now sits a ring in from it, so its
  four poles are inside and the line's own edge loop stays clean: concentric loops round a nose tip or a
  mouth with the poles in the middle, as in hand-made topology.
* **0.19.0.** **Retopology: cleaner eyes, mouths and joins.** A zone with one line round it
  inside a ring (an eyeball, the tip of a nose, the inside of the lips) is now closed with a grid, as a
  modeler's grid fill: every vertex inside has four edges. The rings round an eye keep one count from
  the socket out (the count is planned along the whole chain of rings), and the holes left where three
  zones meet are closed with grids too, cut at their narrowest neck where they need it. Lines drawn less
  tidily keep their rings far more often. On the Suzanne benchmark (two lines round each eye, the nose and
  the mouth) four-edged vertices went from 89.4% to 91.4%; two circles on a ball give 98-99%.
* **0.18.1.** Big models open about a third quicker (the two-million-triangle girl: 5.4 s, was
  8.1): tangents are made for groups of UV islands at once, and the outline packer works on every core.
  Retopology: a zone smaller than four of its quads is named in the window and marked in red (draw its
  lines further apart, or give it a higher resolution); small discs inside rings no longer get more quads
  than they have room for; zones for thin parts also work on a model whose faces point inwards.
* **0.18.0.** **Retopology: zones for thin parts.** One click finds the parts too thin for the
  quads (fingers, toes), draws a clean ring round each where it meets the rest (round the wrist) and gives
  it a resolution for about six quads round: on the anime girl at 4,000 quads per m², both hands come out
  at x16 with every finger covered, in about 3.5 seconds. Locks of hair (thin sheets) are left as they
  are. Rings round an eye or a mouth now keep the same count on every ring, as hand-made eye loops do,
  and a zone too small for any quads no longer stops the zones from joining.
* **0.17.0.** **Retopology: see the flow, twice as quick.** Tick Loops to see the quads' poles
  as small crosses and light up the edge loop under the pointer all along, as CozyBlanket does. Builds
  are about twice as quick (QuadriFlow's hole filling searched the same fillings over and over), and the
  live preview is now made exactly as Make quads makes them: what you see is what you keep, and Make
  quads keeps an up-to-date preview at once. Joins between zones put their poles beside your lines
  rather than on them, rings and small discs only form where there is room for them, and a build that
  can't join its rings tries again without them.
* **0.16.0.** **Retopology: rings between two lines, zones joined anywhere.** A zone between two
  lines (round an eye or a mouth, along an arm) is filled with rings of quads that follow both lines, and
  a small zone inside such a ring (the tip of a nose, the inside of the lips) is closed with rings
  shrinking in: draw a line round the eyeball and one round the socket and the eye gets concentric loops.
  Zones of different resolution now join where three of them meet as well, big steps (such as a hand
  at x16) stay all quads, and a part of a zone too thin or pinched for its quads goes to the zone beside
  it; spots that had to be patched are marked in red. The quads preview no longer shows the far side's
  edges through the model.
* **0.15.0.** **Retopology: the model's own surface, a live preview, symmetry.** The quads are
  made on a simplified copy of the model's own surface (not a shrink-wrapped shell), so fingers stay
  apart and the torso and legs follow the shape; parts too thin for the quads are marked in red with
  what to do. The quads now update by themselves a moment after every change (a faint live preview;
  Make quads for full quality). Mirror X/Y/Z mirrors lines, flow strokes, the density brush and zone
  resolutions across the model's middle, with a deadzone: a line that comes that close joins its mirror
  image. Zone resolution up to x32; builds run zones in parallel and keep their work between changes.
* **0.14.1.** **Big models stay quick, and retopology lines are drawn freely.** Two-million-face
  sculpts no longer slow every frame (the UV fingerprint several panels check was re-read in full each
  frame; the UV editor's hover went through every triangle). Zone lines are drawn on the surface where
  the pen goes, like a grease pencil, not along the mesh's edges: a line started or ended near another
  meets it exactly, one ended where it began closes, overshoots past a crossing are left out, and
  Ctrl+click takes a line away. Lines and flow strokes are drawn by the graphics card; the density
  brush paints the surface softly (and mirrors with Sym). Zones are only worked out again when the
  lines change.
* **0.14.0.** **Retopology that follows the shape.** *Make quads* now covers the model
  with quads that follow its shape by themselves (QuadriFlow inside Patina): most vertices have four
  edges, with poles only where the shape needs them. You steer it: **zone lines** drawn where the
  edge flow should change become exact edge loops (round the eyes, the mouth, the wrists), and split
  the model into **zones**, each with a resolution of its own; zones of different resolution are
  joined by rows of three-to-one steps along their lines. **Flow strokes** set which way the quads
  run, and the density brush fine-tunes. Big or messy models (two million triangles, scans,
  generated meshes) are wrapped in a clean working surface first: a few seconds for the sample
  character. Also: a fix for the Windows *Save As* dialog not opening.
* **0.13.0.** **Retopology.** Paint patches on a high-poly model (a sculpt, a scan, a
  generated mesh) with the Patch brush, set the density in quads per m², and Patina fills each
  patch with clean quads: grids for four-sided patches, a pole for three, five or six sides, tubes
  for rings around arms and legs, with neighboring patches sharing their edges. The Density brush
  adds quads where you paint (the face, the hands); the extra edge loops end in steps instead of
  running round the model. *Suggest patches* and *Fill gaps* cover what you haven't painted. The
  quads can be exported, sent to Blender as a new object (add-on 0.7.0), or made the model you
  paint, with the high-poly baked onto them. See the manual's Retopology page.
* **0.12.0.** **The Assistant does more, and says when it can't.** New requests:
  a texture set of its own for a part (*"split the head into its own texture map"*, *"put the
  head on a new UDIM tile"*), more texture space for a part (*"give the head more texture
  space"*), the texture resolution and layout, glowing parts (*"make the visor glow blue"*),
  more or less metallic, selections, the right window (*"export the textures for Unreal"*
  opens the export window set up for it; render, symmetry check, high-poly bake, Blender
  link...), text to paint through (*"write VALOR on the shield"*), and questions about the
  model (*"how many faces is this?"*). When something is outside Patina (changing the shape,
  animating, painting artwork for you, uploading), it says so in one sentence and offers a few
  close things it can do, as buttons, instead of doing something else. Steps that clear the
  undo history (texture sets, resolution) run with *Do it* after a warning.
* **0.11.1.** **A texture set of its own for part of a model**, such as one for the head
  and one for the body: select the faces (or press *Select the head*) and choose *UV › New
  Texture Set from Selection*. The paint moves with them, each set is packed to fill its square,
  and the Blender add-on (0.6.1) gives the object the new material slot. Fixed: clicks in the UV
  editor selected the wrong island, or nothing, when the model had a face whose UVs were
  collapsed onto one point (the editor now warns about such faces and selects them);
  Shift+click on a selected island deselects it, as in Blender. **No more long stops:** saving a
  4K project takes 1 s instead of 14 s, opening 1.3 s instead of 8, autosave runs in the
  background (it froze the window for ~14 s every five minutes; *Preferences › Saving* sets
  how often), each UV move takes 0.04 s instead of 0.9 s, and exports and sends to Blender are
  faster. Project files are about half the size.
* **0.11.0.** The **Patina Assistant** (*Window › Assistant*, or type a request into
  the command palette): say what you want in plain words, *"make the creases of the skin
  slightly redder"* or *"one UV island for the head and another for the body"*, and a small
  model on your own computer (Qwen3 1.7B fine-tuned for Patina, run by llama.cpp, a separate
  download of about 1.1 GB) turns it into a plan. It sets things up so you drive the result:
  whole looks (*"make the armor look battle-worn"* is a folder of edge wear, dirt and soot
  layers), several changes in one sentence, colored materials, a paint layer limited to a part
  with the material, brush and tool ready, adjustment layers (*"a bit darker"*, *"glossier"*),
  layers named by what they are (*"turn the rust down"*), "everywhere except", the selection,
  and the brush relative to how it is set. You see the steps in plain words, preview the
  result in the viewport, tune it with a slider per layer, and keep or cancel it as one undo
  step. Follow-ups work ("a bit stronger", "put it on the boots too", "remove that"); unclear
  requests get a question with the model's parts as buttons. Nothing is sent anywhere. New commands it builds on, also
  for scripts and Claude: `mesh_parts` (material slots, Blender vertex groups, pieces, and the
  head and body found from the shape), `add_tint_layer` / `add_material_layer` limited to a part
  and a smart mask, `update_layer_mask`, and `uv_island_per_part` (one island per part, with
  cuts along the back and limbs and an as-rigid-as-possible relax). The Blender add-on (0.6.0)
  sends vertex groups. Fixed: polygon fill, UV moves and texture-set / UDIM painting silently
  did nothing on NVIDIA 610 drivers.
* **0.10.1.** The click that confirms the brush size or strength (F / Shift+F) no longer
  paints with the Island brush or the Select tool, and every Island brush or Select stroke is
  now its own undo step.
* **0.10.0.** Search, checks and automation, each of which can be switched off in
  *Preferences › Features*. A **command palette** (Ctrl+Shift+P, F3 in the Blender keymap)
  finds any action, window, command, material, brush, layer or recent file by name. A
  **mesh symmetry check** (*UV › Mirrored Halves › Check Mesh Symmetry*, the *Symmetry* view mode) shows where
  the two halves of a model differ, and Blender's **Select Asymmetric Vertices** selects
  them there. **Set lock** and **Isolate** keep strokes on one texture set.
  **Auto texture** picks a library material for each material slot from its name. A
  **macro recorder** replays what you did on another model or saves it as a `patina run`
  job. Layered PSD export works per texture set / UDIM tile, and the Blender add-on (0.5.0)
  can **auto-update the mesh** in Patina after your edits settle.
* **0.9.0.** **Texture sets per material slot** and **UDIM tiles**. Body, clothes
  and hair each get their own textures and their own 0..1 UV space; UVs in tiles 1001,
  1002, 1011... each get a texture. Patina picks the layout when it opens a model (slots that
  overlap in UV space; islands in several tiles), and *Texture Set › Texture sets* switches it
  with the paint kept. One layer stack paints across all of them. The UV editor shows one
  tile at a time, and pack, unwrap, the Island brush and overlap fixes work per tile.
  Exports write `Hero_Body_basecolor.png` or `Hero_basecolor.1001.png`; Blender gets a
  material per slot and tiled UDIM images.
* **0.8.2.** Blender round trip fixes. Patina's material now shows on every
  material slot of the object (it used to be added as an extra slot, so a character's own
  skin material showed through Patina's new UVs; **Restore Materials** puts the old ones
  back). Textures are never applied without the UVs they were painted on. Edited UVs go
  back even after reopening a project. Armature and other deform-only modifiers no
  longer block the UV round trip. UV edits made in Blender after sending are protected.
* **0.8.1.** **Patina Web catches up**: Blur, Fill (triangle / island / part /
  whole model), Pick and Mirror X/Y/Z painting; adjustment layers, blend modes and smart
  masks (edges, cavities, facing up, gradient, noise); `.patinaweb` projects (Ctrl+S);
  your own image materials; Relax and shape packing for islands; export presets for
  glTF / Blender, Unreal and Unity at up to 4096. Same lite interface.
  **Texel density** is measured more accurately in both: the layout target is the median
  by surface area (many small islands no longer drag it), tiled UVs outside 0..1 are left
  out, stacked mirror halves no longer shrink *Fit*, overlapping layouts can't ask for more
  than the texture holds, and `uv_info` no longer counts unmapped faces.
* **0.8.0.** **Packing by shape**: islands nest into each other's gaps
  (around 60% of the texture used instead of under 40% on a typical Smart Project
  layout), around pinned islands. A **seam brush** (drag along the model, Shift+click
  to continue, Ctrl to clear), **Cylinder** and **Sphere** unwraps (bent and tapered
  limbs come out as one even strip), and **Relax** (stretch removed, islands stay put).
  The library grows to **173 materials** and **19 smart materials**: bricks, parquet,
  hex tiles, scales, grilles, quilting, knits, knurling, stucco, character skin and
  hair, satin, denim and plaid, and a hand-painted / toon set.
* **0.7.4** fixes ghosting (a faint second copy of a pattern, worst with diamond plate):
  tri-planar projections now meet in crisp seams decided by height, and the two halves of
  a symmetric island no longer write different values into the texels they share.
* **New in 0.7.3.** The **Island brush in Patina Web**, with mirror symmetry,
  orientation, texel density, pinning and a right-click menu in the UV view.
  Symmetric islands now also stack on models whose halves differ in shape (posed,
  sculpted or drifted by up to about an edge length). **Connect to Claude app**
  (Blender Link panel, or `patina --setup-claude`) adds Patina to the Claude
  desktop app's MCP servers. An application icon.
* **0.7.2** fixes Sym X with the Island brush on models whose halves are triangulated
  differently: the mirrored selection is complete and every mirrored face is stacked.
* **New in 0.7.1.** **Pin UV islands** (P / Alt+P in the UV editor; the Island brush
  pins what it makes): pinned islands survive moving, packing, unwrapping and overlap
  fixes, and the rest is packed around them. The two halves of a symmetric island stay
  linked, so moving either one moves both. The UV editor has a **right-click menu**
  with every UV operation, and a click on a selected island no longer nudges it.
* **New in 0.7.** New bakes: **thickness** (with a *Thin parts* smart mask) and
  **bent normals**, and **curvature** measured at a fixed scale (clean edges on
  low-poly models) that also picks up a high-poly normal map's detail.
  **Rectify** straightens quad islands into exact grids and **Mirror UVs** stacks
  one half of a symmetric model on the other. Export as **layered PSD** files
  (layers, masks, blend modes, groups) and at **16K / 32K** (rendered in tiles).
  The manual has a **beginner's guide to masks and smart materials**.
* **New in 0.6.** **Mask effect stacks** (paint, smart mask, fill, noise, levels,
  blur and invert, each with a blend mode and opacity); layer **thumbnails** and
  **instances** (linked copies, Alt+D); a **smudge** brush (J); blur and smudge that
  carry on **across UV seams**; **stencils** laid over the view (mask or decal, tile,
  invert, hold T to place, project in one step) and a **text tool** (bundled fonts,
  outline); **notes** pinned to the model and saved in the project (N); a **compare
  view** with a draggable divider against a snapshot (\\); **FBX**, **STL** and
  **3MF** import (STL / 3MF get UVs on open); the **Island brush follows symmetry**,
  and the mirrored half shares the island's UV space.
* **Pen tablets (improved in 0.5.1).** Every tablet sample reaches the brush (not
  one per frame), so fast strokes keep their shape and pressure; a pressure
  curve; the eraser end erases; tilt can turn the tip; barrel buttons map to
  right / middle click, swap colors or undo; *Help › Pen Tablet Test* shows
  what the tablet sends (Wacom, Huion, XP-Pen; Wayland, X11, Windows Ink).
* **Substance materials (new in 0.5).** Exported texture folders from Substance
  Designer / Painter / Sampler import as they are: packed maps (Unreal
  OcclusionRoughnessMetallic, ORM / ARM, glTF metallicRoughness, Unity MaskMap and
  MetallicSmoothness), several texture sets in one folder (one material each),
  EXR / 16-bit maps, and the Unreal preset's DirectX normals. `.sbsar` files need
  Adobe's engine; Patina says so and how to export them.
* **HTTP API and batch mode (new in 0.5).** `patina --headless --http` (this
  machine) or `--lan` (the local network only: private addresses, token) serves
  every command over HTTP/JSON, plus background **jobs** with progress and
  cancel, workspace file upload / download and an OpenAPI description, for
  pipelines and render farms. `patina run job.json --set asset=crate` runs a job
  file from scripts and CI. Python client in `clients/python/patina_api.py`, a
  farm dispatcher in `examples/api/`. See the manual page
  [HTTP API and batch mode](manual/api.html).
* **Stroke locks (new in 0.4.1).** Two brush-bar toggles: **topology lock**
  (Alt+T) keeps a stroke on the connected piece of mesh it starts on (eyes,
  buttons, straps stay clean), **island lock** (Alt+L) keeps it inside the UV
  island it starts on. Both follow symmetry.
* **High-poly baking (new in 0.4).** Bake a sculpt's normals, AO and material
  slots onto the low-poly model (CPU ray casting with a cage distance); the normal
  map is combined with every layer and exported. Blender **material slots** drive a
  Fill mode and a smart mask; a **facing-direction** smart mask puts snow on top
  and drips underneath; **mask from selection**.
* **Review and iterate (new in 0.4).** **F12** renders a PNG at any size
  (transparent background, anti-aliasing, turntable frames); Shift + right drag
  turns the lighting; **Save Incremental** (Ctrl+Alt+S, `name_v002.patina`),
  **Export Again** (Ctrl+Shift+E, settings kept in the project); Patina notices
  when the model file changes on disk and reloads it with the layers kept;
  crash recovery from the autosave; a welcome window.
* **UV editor.** Texture view under the UV layout; select faces, islands or
  individual UV vertices (in 2D, faces/islands also on the model), weld/stitch
  and align, move/rotate/scale (drag or G/R/S), LSCM unwrap respecting seams,
  smart/box/planar projection, packing with texel-density normalization, seam
  editing by clicking edges on the model. **Existing paint is reprojected onto
  the new layout**, so you can fix UVs after painting.
* **Blender bridge.** One click sends a mesh from Blender (exact loop order, so
  UVs and seams edited in Patina go back 1:1); textures come back into a
  Principled BSDF material automatically. Keep modeling after painting:
  *Update Mesh* keeps the layers, even when the topology changed (paint is
  transferred through 3D onto the new surface).
* **Quality of life** (the most-requested Substance Painter wishes, see
  [docs/WISHLIST.md](WISHLIST.md)): Colors panel with swatches and recent
  colors, hold **I** for a quick color pick, **X** swaps colors, multi-axis and
  **radial symmetry**, Alt+click erases with the Fill tool, rebindable shortcuts
  (*Edit › Keyboard shortcuts*) plus a full **Blender keymap** (*Edit › Preferences*:
  S samples, X flips colors, F / Shift+F brush size / strength, Shift+A add menu,
  Shift+D duplicate, Ctrl paints the secondary color), Blender / Maya / ZBrush navigation presets,
  UI scale, **Tab** hides all panels, *Open Recent*, brush and colors kept
  between sessions, export file-name templates (`$mesh $project $map $size
  $preset $layer`), export of a single layer or a mask, error dialogs when a
  save or export fails.
* **MCP server.** `patina --mcp` exposes ~75 tools. It attaches to a running
  window (live mode, with real window screenshots) or runs its own headless
  engine (EGL, no display needed) and can open the window on request.
* Export presets for Blender, glTF, Unreal (ORM, DirectX normal) and Unity
  (mask map); single-file `.patina` projects.
