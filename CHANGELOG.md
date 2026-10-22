# Patina change log

What changed in each version, newest first. The manual (Help › Manual, or `docs/manual`) explains how to use each
feature, and [docs/ROADMAP.md](docs/ROADMAP.md) lists every feature with the version it came in.

## Unreleased

### Changed

- Patina Web's material library is baked again with every build. The 1.2.0 downloads still carry the library of
  0.26, with its British names. The newest library is a download of its own, `patina-1.2.0-web-library.7z`.
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
