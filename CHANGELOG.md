# Patina change log

What changed in each version, newest first. The manual (Help › Manual, or `docs/manual`) explains how to use each
feature, and [docs/ROADMAP.md](docs/ROADMAP.md) lists every feature with the version it came in.

## 1.2.8 (2026-10-24)

### Changed

- **The bar over the 3D view is easier to read.** Each brush setting is one field with its icon inside it, filled up
  to its value, as Blender's sliders are; thin lines part the groups (the brush, its projection, the locks, the view).
  The icons stood apart from their sliders, so which icon went with which slider was hard to see.
- **Fields are narrower.** A slider or drop-down is at most about 13 characters wide, so its label sits beside it in
  wide panels (they took 65% of a panel's width).
- **The Blender layout's workspace tabs have their steps' colors again**, as the Guided layout's steps, and so do the
  tool groups in its tool strip.
- **Material Edit** is a tab of the Blender layout's sidebar, beside Brush, Colors, View and Blender. The Material
  Editor's window opened behind the layout there.
- With names beside the icons, the tool strip of the Blender and Guided layouts is as wide as the names need. A
  docked strip that is narrower shows the icons alone instead of names too small to read.
- The Outliner says the layers are the object's, for all of its texture sets (each layer paints every set).

### Fixed

- **The layer buttons ran off the panel** with names beside the icons. They go on to a new row now.
- **Retopology: a change remade the whole model.** "Only the quads near a change" made new quads on the whole model,
  then kept them only near the change, so every edit cost a full build (19 s on a head, up to a minute and more on a
  body). The new quads are now made only on the part of the model round the change: on a 190,000-face character, a
  zone line drawn after the first build updates in about 1 second instead of 21. Builds also ask for the number of
  quads that worked last time, so most need one pass instead of two.

## 1.2.7 (2026-10-24)

### New

- **A Blender layout.** Besides Guided and Full, the switch at the right of the menu bar (and *Edit › Preferences ›
  Display*, the Welcome window, the command palette) has *Blender*: Patina arranged as Blender arranges its editors,
  with every panel and button of the Full layout.
  - The five steps are workspace tabs in the menu bar, each with its mark (done, needs a look, to do). A step brings
    up its tools and its own panel; UVs puts the UV editor beside the 3D view, as Blender's UV Editing workspace, and
    Paint the asset shelf under it, as Texture Paint. A tool of another step brings that step up.
  - The tools stand at the 3D view's left. Its sidebar is at its right, with vertical tabs as Blender's N panel:
    Brush, Colors, View (the base material and the lighting) and Blender. The chosen tab again closes it.
  - At the right, the Outliner: the objects, and under the active one its texture sets (a click shows one in the UV
    editor) and its layers, with the layer buttons, folders, drag and drop and a search. (The layers paint every
    texture set of their object, so they sit beside the sets, not in each one.)
  - Under it, the Properties editor, with a column of icon tabs: the step's own panel (the Guided layout's, with its
    Next button), Object, Layer (the selected layer's settings), Texture Set, Face Sets, Retopology, History, Notes and
    Console.
  - The asset shelf lists its catalogs at its left: the material categories, the smart materials and masks, the
    brush categories.
  - The borders between the areas are dragged to resize them, and the sizes and tabs are kept. The Full layout's own
    arrangement is left as it was. Claude uses `layout` with `mode: "blender"`; `step` then picks the workspace tab.
- **A Vivid Dark theme.** *Edit › Preferences › Display › Theme*: Patina's own gray and copper, or Vivid Dark, the navy
  and cyan of KDE Plasma's Vivid-Dark-Global-6, with rounder corners. It matches the "Vivid Dark" Blender theme, so
  Patina and Blender look alike side by side. The chosen tool, icons, pie menus, the brush size circle, selections and
  drop marks follow the theme, and so does the UV editor round the texture. Claude uses `layout` with `theme`.

### Changed

- **One UV menu, the same everywhere.** UV in the menu bar, the UV editor's More button and its right-click menu
  had grown apart: the same command had another name or place in each, and some were only in one. Now all three are
  the same menu, in the same order, with the same names (the right-click menu adds Move, Rotate and Scale at the
  top). With faces selected, the commands work on their islands; with nothing selected, on every island.
- **The same menu everywhere, for the other menus too.** The menu bar's View and the 3D view's right-click View
  are now one menu (they named the same commands differently, "Frame mesh" and "Frame the model", and each had
  options the other didn't). The new-layer list is the same in Edit › New Layer, the Layers panel's right-click
  menus and the Add menu: five lists had grown apart, some with the adjustment layer, the white mask, baked maps or
  smart materials and some without. The 3D view's right-click menu had two Select menus; now it has one, with the
  same items as the UV editor's Select. Menu items are in Title Case everywhere, as in the menu bar and Blender.
- **Mirrored Halves › Mirror the Selection's UVs (X)**: with faces selected, Mirror UVs stacks only their halves.

- **Undo keeps much more, in less memory.** Older undo steps are now compressed into RAM, while the latest ones stay
  on the graphics card for instant undo. *Edit › Preferences › Undo* sets the number of steps, the RAM undo may use
  (default: a quarter of your computer's memory, 1 to 8 GB), and how much stays on the graphics card. Past the RAM
  limit, the oldest steps are dropped. Undo is never written to disk, to spare your SSD. Claude uses
  `undo_settings`.

### Fixed

- **On a big model, only the latest UV edit could be undone.** With "Keep paint" on, a UV edit copies every layer:
  4.9 GB for a character with seven texture sets. Each object's history could hold 1 GB on the graphics card, so every
  UV edit pushed out the one before. Now that copy is compressed to about 131 MB, and the steps before it stay. The
  limit also covered each object separately, so a scene of eight objects could hold 8 GB of graphics memory; it now
  covers them all together.
- **"Select the Other Half" was grayed out for a face unwrapped in two halves.** It only knew halves stacked on each
  other. Now it adds the mirror image of the selection by the model's symmetry, wherever it is in the layout (whole
  islands when you selected whole islands). It is also in Select › The Other Half. Claude uses `select_faces` with
  `mode: "mirror"`.
- **UV commands that changed nothing said they had done it.** Orient Upright on an island that was already upright
  said "Oriented upright", and Relax on a relaxed island said "Relaxed". Now they say "Already upright: nothing
  turned" or "Already relaxed", and make no undo step (with "Keep paint" on, each step copied every layer).
  `uv_orient` and `uv_relax` return `changed`.

- `patina --mcp` ignored the stop signal (SIGTERM, or Ctrl+C) while it waited for its client. Only closing its input
  or a forced kill ended it. Now the stop signal ends it too, with its own hidden Patina and when it drives a window. It also ends while its hidden Patina is
  still starting (which can wait a long time when the graphics driver is busy).

### Blender add-on 0.14.1

- **"Send to Blender" was refused after the mesh changed in Blender, even when Patina hadn't changed a UV.** The
  add-on said "Patina changed its UVs ... Patina sent 60930 loops but ... has 60916" and kept the old textures. Now,
  when Patina sends back the UVs it was given, the textures load: they fit every face you didn't change. A warning
  says the mesh changed, and that sending it to Patina again paints the changed faces too (the paint carries over).
  When Patina did change the UVs, the textures still wait, as before.

## 1.2.6 (2026-10-23)

### Fixed

- **Blender got another model's UVs, and textures sent from Patina never arrived.** When Claude Desktop (or another
  MCP client) started `patina --mcp` before any Patina window was open, that hidden Patina took port 7860. A Patina
  window opened afterward moved to 7861, which is the port Blender's add-on listens on. So Blender's requests went to
  the hidden Patina, which answered with its startup sphere: "Patina sent 8064 loops but ... has 60993". And the
  window's "Send to Blender" reached itself: "unknown command 'patina_textures_updated'". Now:
  - the hidden Patina of `patina --mcp` leaves the port free, and its tools switch to a Patina window as soon as one
    is open;
  - a window that finds its port busy never takes 7861;
  - a Patina asked for an object it has no link to says so, instead of sending its own object's textures and UVs.
- The manual's tip for two windows suggested `--port 7861`, Blender's port. It says `--port 7862` now.

## 1.2.5 (2026-10-23)

### Fixed

- **A brush set to UV projection could stop painting.** After a few strokes, or after the camera moved, strokes
  in the 3D view put nothing on the layer, and the status line said "Nothing to paint". A fill or a Screen stroke
  made it work again until the next camera move. Such a brush checks which spots the camera sees, but Patina drew
  the picture of the view it checks against only for Screen brushes, so it used an old one. It draws it for these
  brushes too now.
- **Symmetry needed the camera to see both sides.** With a Screen or UV brush, a mirrored copy the camera couldn't
  see (facing away, or off screen) was left out, or it landed on whatever the view showed at that spot, like an
  eyelid's copy on the nose. Those copies now go straight onto the model's surface, so the mirror follows the model,
  not the view.
- "Nothing to paint" no longer shows after a stroke that did paint with a library material.

## 1.2.4 (2026-10-22)

### Fixed

- **The path tracer showed the triangles of smooth models where light grazes them.** Under a hard light, the line
  between light and shadow on a nose, a cheek or a shoulder ran in triangle-shaped steps, though the Lit view and
  Blender's viewport were smooth. This is the "shadow terminator" problem. Light was cut off at each flat triangle's
  own horizon, and shadow rays leaving the flat triangles hit their neighbors. Now light fades out at the smooth
  surface's horizon, and rays leave from the smooth surface the normals describe, as Blender's Cycles does. A normal
  map's light fades out smoothly too. Renders under soft light don't change.

## 1.2.3 (2026-10-22)

### New

- **G, R and S in the Object tool**, as in Blender's Object Mode. With the pointer over the 3D view, G moves the
  object, R turns it about the line of sight and S scales it evenly. There's no handle to hit. X, Y or Z keeps to
  one axis, a typed number sets the amount (S 0.5 halves it), and Ctrl snaps. A click or Enter keeps the change as
  one undo step, and a right-click or Esc puts it back. The keys are in *Edit › Keyboard Shortcuts* for both keymaps.
  In the Object tool they come first: S doesn't pick the Select tool there.
- **A list view for materials**: a small picture, the name, and the category and library of each, in the
  Materials and Smart materials tabs.
- **A size slider for the material tiles** (Ctrl+mouse wheel over them does it too). Patina remembers the view and
  the size.

### Fixed

- **Adding a model could replace the one you had.** After you opened a project, Patina still took it for the sphere
  it starts with. *Add a Model to the Scene* then put the new model in place of a one-object project, and a model
  dropped on the window opened instead of asking. Blender's *Add to the Patina Scene* replaced the scene too. Now
  only the untouched starting sphere gives way.
- **Material tiles ran off the side of the panel.** The panel didn't count the space around each tile or the
  scrollbar, so at some widths the last column was cut off. A long name could also widen its tile. The tiles fit now,
  and long names end with "...". The same fix applies to the smart materials, smart masks and brushes.

## 1.2.2 (2026-10-22)

### Fixed

- **Hair cards, leaves and lace from OBJ and FBX files came in solid.** Many game assets keep their cut-outs only in
  the color texture's alpha, and their file says nothing about opacity. Patina now makes that alpha the Opacity
  channel of the layer *From the model* when it has see-through and solid parts, and the status line says so. An
  alpha that is even all over is left out, because some assets keep gloss there. For a model you already brought in,
  select it and use *File › Bring In the Model's Textures* again.
- **See-through parts drew in the wrong order.** Back layers of hair cards could land on top of front ones, and an
  object that was not the one you paint lost its see-through parts in front of the one you do. Every object's opaque
  parts are drawn first now, then the see-through parts, and the nearest layer of cards hides the ones behind it.

### Changed

- The layer's *Opacity* channel box and its *Channel opacity › Opacity* slider explain themselves: the see-through
  channel and its strength, not the slider that says how strongly the whole layer shows.
- `import_model_textures` takes `color_alpha`: `"opacity"` always uses the color texture's alpha, `"ignore"` never.

## 1.2.1 (2026-10-22)

### Fixed

- **Saving could close Patina.** A paint layer with clearcoat (painted with car paint, lacquer or another material
  that has it) made every save and every autosave stop the app.
- **Paint in the opacity, sheen and subsurface channels was lost on saving.** Since 0.26, a save wrote those channels
  of paint layers as empty images, so they were gone when the project was opened again ("could not restore channel
  sheen"). They are saved now. Paint saved that way before 1.2.1 can't come back; fill layers keep theirs, because
  they come from their material.
- A save that fails says why instead of closing Patina, and the autosave tries again later.

### Changed

- Patina Web's material library is baked again with every build. 1.2.0's downloads had the library of 0.26, with its
  British names; 1.2.1's has the current one.
- The Linux download's `INSTALL.txt` names what the build needs: its glibc version and SDL 3. `install.sh` checks for
  SDL 3, and says so when the system's glibc is too old for the build.
- The install notes and the manual no longer name version 0.2.0 in their file names.
- A new README: download, install, your first texture, how-tos, and this change log from 1.0.0 on. The list of every
  feature moved to `docs/FEATURES.md`.

## 1.2.0 (2026-10-21)

### New

- **Objects in a scene.** A project can hold several models, each an object with its own layers, texture set and
  undo history: a body, its clothes, its eyes, its hair. The **Objects** list over the layers shows them. Click one
  to paint it, and the layers below, the UV editor and the bakes are that object's.
- **File › Add a Model to the Scene** (Ctrl+Shift+I) adds a model file as an object of its own, where the file
  puts it. Its texture set gets as many texels per meter as the object already open, so small parts like eyes get
  smaller textures.
- **Files with several meshes** (glTF, FBX, OBJ) open as an object each. Files with more than 24 meshes (kits of
  many small parts) still open as one. The MCP command `open_mesh` takes `separate` to choose.
- **The Object tool** (W) moves, turns and scales objects with a gizmo in the 3D view. Arrows move along an axis,
  rings turn, handles scale, and Ctrl snaps. Press W again for the next of the three. Each drag is one undo step in
  that object's history. A click on another object picks it. *On the ground* and *Where it came in* are on the
  tool's bar.
- **Double-click another object** in the 3D view to paint it. A brush stroke that starts on another object does
  nothing and says which object it is. Objects in front of the one you paint hide it from the brush.
- **Local view** (/) shows only the object you paint. Each object can also be hidden from its row in the list.
- **The object's placement** (location, rotation, scale) can be typed in under the Objects list. Right-click a
  row to rename, copy, remove, frame or hide an object.
- **Blender: several objects at once.** With add-on 0.14, *Send to Patina* sends every selected mesh. The active
  one starts the scene and the others come in beside it. *Add to the Patina Scene* adds them to what is open.
  Objects come in where Blender has them (their `matrix_world`). Updates go to their own object, and *Send
  Textures to Blender* sends every linked object's textures.
- **Exports of the whole scene.** *Export Model* writes every object shown into one file. glTF gets a node per
  object, where it is. OBJ, STL and PLY get the objects' vertices moved into the scene. *Export Textures* can
  write every object's maps, each set named after its object.
- **The path tracer renders every object**, with each one's own textures, in the Path traced view and in renders.
- **Select faces in batches** with the Select tool, in the 3D view and the UV editor:
  - **Alt+click** by an edge selects the loop of quads across it (Blender's face loop).
  - **Ctrl+Alt+click** selects the ring along it. Shift adds to the selection.
  - **Connected** grows the selection to the whole connected parts.
  - **Flat region** grows it over the surface where it bends less than an angle you set.
  - Right-click › **Select** offers all of these, plus the UV island and the material slot, from the face under
    the pointer.
- **Face sets**: named, colored groups of faces, kept in the project, like Blender's sculpt face sets. The **Face
  Sets** panel (UV › Face Sets) has them:
  - **New from the selection**, or a set for each material slot, UV island or connected part.
  - **Click** a set to select its faces. The **eye** hides them from the 3D view, the brush and picking (renders and
    exports keep them).
  - Right-click a set to **show it alone**, **fill** it with the paint material, or **mask** the selected layer
    with it.
  - The **Face set lock** keeps a stroke inside the set it starts on. The **Face Sets** view shows each set in its
    color, on the model and on the UV layout.
  - Face sets follow their faces through a bake-down, a reduction and a remodeled mesh. Blender's sculpt face sets
    come in with the model (add-on 0.14).
- **Your own brushes.** Every image in your brushes folder becomes a brush: PNG, JPEG, TGA, BMP, GIF, PSD or EXR.
  Each folder inside it is a group in the Brushes panel, at the top. Photoshop brush files (`.abr`) there give a brush for each
  of their tips. Patina looks at the folder every few seconds, so a file you add shows up by itself.
  - **Import brushes...** on the Brushes panel, or a drop on it, copies files into the folder.
  - Tips are read the right way round: a dark shape on a light ground paints with the shape, as Photoshop, GIMP and
    Krita tips are made. So do a light shape on a dark ground and an image with transparency.
- **MCP:** `list_objects`, `add_object`, `select_object`, `set_object`, `remove_object`, `duplicate_object`,
  `isolate_object`, `frame_object`, `pick_object`, `select_loop`, `select_linked`, `list_face_sets`,
  `make_face_set`, `edit_face_set`, `delete_face_set`, `use_face_set`, `reload_brushes`, `import_brushes`.
  `export_model` and `export_textures` take `objects`, `send_to_blender` takes `objects`, and `set_brush` takes
  `lock_face_set`. New feature switches: `scene_objects` and `face_sets`.

### Changed

- A project with several objects is saved in one `.patina` file. Patina 1.1 and older open its first object only.
- A model dropped on the window while one is open asks whether to add it to the scene or open it instead. Drop it
  on the Objects list to add it right away.
- Objects you are not painting free their compositing memory until they are painted or exported again.
- The status bar names the object you paint when there are several.
- An image tip drawn small is smoother: its mip levels are made now.
- An image dropped on the Brushes panel becomes a brush of its own, kept in your brushes folder. It used to change
  only the current brush's tip.
- Help › What's New opens this list in the manual.

### Blender add-on 0.14.0

- *Send to Patina* sends every selected mesh, and the new *Add to the Patina Scene* adds them beside what Patina
  has.
- Each object's placement goes along. The automatic mesh sync only updates objects Patina's scene has.
- The sculpt face sets go along, for Patina's face sets.

## 1.1.0 (2026-10-21)

### New

- **Bake down from the high-poly.** Paint the sculpt, then bake every layer down onto its low-poly version, with
  *Bake › Bake Down to a Low-Poly Model* or *Use the Quads as the Model* after Patina's retopology. The low-poly
  becomes the model and the whole layer stack comes along.
  - Paint, masks and fixes are measured at up to 16 points per texel, each looking along the low-poly's normal for
    the high-poly's surface.
  - Painted normals are turned to fit the new surface. The high-poly's shape goes into the normal map and the
    ambient occlusion.
- **The Bake-Down Check view** marks where the transfer had to take the nearest point (amber) or guess (red), and
  where the low-poly faces away from the high-poly (magenta).
- **The compare view** can put the high-poly as it was beside the result.

### Changed

- *Bake from a high-poly mesh* uses the same rays: 4 points per texel, with the distance measured from the two
  models.
- Retopology quads face outward when they are used as the model, sent to Blender or exported.

## 1.0.0 (2026-10-21)

The first stable release. It has the **Guided layout** for a first project (Model, UVs, Bake, Paint and Export, each
step in its own color) and the **Full layout** with every panel at once. Projects, presets, keys and layouts from the
0.x versions open as they were.

## Before 1.0

- **0.47.0:** each step of the Guided layout has its color; the export shows the files it writes.
- **0.46.0:** the Guided layout: the work in five steps.
- **0.45.0:** Select only what the camera sees (*Visible only*).
- **0.44.0 to 0.44.2:** Blender objects reopen the project they were painted in; UVs survive a send from Edit
  Mode; fast painting on big texture sets.
- **0.43.0:** symmetric islands on nearly symmetric models; the Island brush's Screen and Surface reach.
- **0.42.0:** right-click a slider to type its value.
- **0.41.0:** baked maps (ambient occlusion, curvature, thickness) are layers you can fix.
- **0.40.0 and 0.40.1:** four pie menus; the wireframe and seams stay on the side you can see.
- **0.39.0:** the path traced view and renders.
- **0.38.0:** side views the Blender way (numpad, axis gizmo).
- **0.37.0 and 0.37.1:** the retopology preview button; pin and unpin from the 3D view.
- **0.36.0:** icons in the toolbars, right-click menus, the interface in other languages.
- **0.35.1:** American English throughout.

The versions before these are in [docs/ROADMAP.md](docs/ROADMAP.md), each feature with the version it came in.
