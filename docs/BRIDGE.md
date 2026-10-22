# Patina ⇄ Blender bridge protocol

Both sides talk **newline-delimited JSON over TCP on 127.0.0.1**. Bulk data goes
through files in a shared exchange folder.

| | Default | Override |
|---|---|---|
| Patina command server | `127.0.0.1:7860` | `PATINA_PORT` env / `--port` |
| Blender add-on listener | `127.0.0.1:7861` | add-on preference |
| Exchange folder | `~/.cache/patina/bridge/` | add-on preference / `PATINA_BRIDGE_DIR` |

## Message format

Request (one line):

```json
{"id": 12, "cmd": "bridge_open_mesh", "args": {...}}
```

Response (one line, same `id`):

```json
{"id": 12, "ok": true, "result": {...}}
{"id": 12, "ok": false, "error": "message"}
```

`id` may be any integer; a request without `id` still gets a response with `"id": null`.

## Files

### `.ptmesh` (Blender → Patina)

```
8 bytes   magic "PTMESH01"
uint32 LE length N of the JSON header
N bytes   UTF-8 JSON header
...       raw little-endian blocks, in the order of header["blocks"]
```

Header:

```json
{
  "version": 1, "name": "Suzanne", "up": "Z",
  "vertex_count": V, "face_count": F, "loop_count": L, "edge_count": E,
  "blocks": [
    {"name": "positions",    "type": "f32", "count": V*3},
    {"name": "face_sizes",   "type": "u32", "count": F},     // polygon.loop_total, in polygon order
    {"name": "loop_verts",   "type": "u32", "count": L},     // loop.vertex_index
    {"name": "loop_uvs",     "type": "f32", "count": L*2},   // active UV layer (optional)
    {"name": "loop_normals", "type": "f32", "count": L*3},   // corner/split normals (optional)
    {"name": "edges",        "type": "u32", "count": E*2},   // edge.vertices (optional)
    {"name": "edge_seams",   "type": "u8",  "count": E},     // edge.use_seam (optional)
    {"name": "edge_sharp",   "type": "u8",  "count": E},     // edge.use_edge_sharp (optional)
    {"name": "face_material","type": "u32", "count": F}      // polygon.material_index (optional)
  ]
}
```

Types: `f32`, `u32`, `i32` (4 bytes), `u8` (1 byte). Positions are in the object's
local space, Blender's Z-up axes. Polygons must be in Blender's polygon order and
loops in Blender's loop order so the returned UVs map back 1:1.

`"materials"` (add-on 0.13, optional): what each material slot shows, in slot order
(`null` for an empty slot). Patina starts an object it has no project for from it, as the
layer *From the model*. Values are linear; a linked input shows its image alone, as in Blender.

```json
{"name": "Skin", "principled": true,
 "base_color": [r, g, b, a], "roughness": 0.5, "metallic": 0.0, "emission": [r, g, b],  // emission × strength
 "alpha": 1.0, "coat": 0.0, "sheen": 0.0,
 "alpha_mode": "blend" | "clip", "alpha_cutoff": 0.5,          // only when see-through
 "vertex_colors": true,                                        // a Color Attribute into Base Color
 "maps": {"base_color": {"path": "/…/skin.png", "channel": 0, "invert": false, "image": "skin"},
          "roughness": {…, "channel": 1},                      // through Separate Color: its channel
          "normal": {…, "strength": 1.0},                       // through a tangent-space Normal Map node
          "bump": {…}, "metallic": {…}, "emission": {…}, "alpha": {…}, "coat": {…}, "sheen": {…}},
 "notes": ["Skin: packed images left out"]}
```

The add-on follows reroutes, Separate Color (the channel), Invert, Mix (its A color, so
Patina's AO multiply leads to the base color) and Normal Map / Bump nodes. Packed, generated
and UDIM images are left out and named in `notes`. A slot without a Principled BSDF sends
`"principled": false` and its viewport color.

### `.ptuv` (Patina → Blender)

Same container, magic `"PTUV0001"`, header `{"version":1,"name":..,"loop_count":L,"edge_count":E}`,
blocks `loop_uvs` (f32 L*2) and `edge_seams` (u8 E).

### Textures (Patina → Blender)

PNG files written into `<exchange>/<object name>/`:

| Key | File | Color space | Bits |
|---|---|---|---|
| basecolor | `<name>_basecolor.png` | sRGB | 8 |
| roughness | `<name>_roughness.png` | Non-Color | 8 |
| metallic | `<name>_metallic.png` | Non-Color | 8 |
| normal | `<name>_normal.png` | Non-Color (OpenGL, +Y) | 8 |
| height | `<name>_height.png` | Non-Color (0.5 = neutral) | 16 |
| emission | `<name>_emission.png` | sRGB | 8 |
| ao | `<name>_ao.png` (only if baked) | Non-Color | 8 |

## Commands Blender sends to Patina

| cmd | args | result |
|---|---|---|
| `ping` | – | `{"app":"patina","version":"0.1.0"}` |
| `bridge_open_mesh` | `{"path": "/…/Suzanne.ptmesh", "object": "Suzanne", "blender_port": 7861, "exchange_dir": "/…/bridge", "resolution": 2048, "keep_layers": true}` | `{"faces":F,"loops":L,"islands":I}` — loads the mesh. With `keep_layers` and the same object, the layers are kept: if the topology matches, paint is reprojected from the old UVs to the new UVs; if the topology changed (modeling edits), paint is transferred through 3D (closest point on the old surface). Otherwise a new texture set is started. (0.44) An object Patina doesn't have open (or has only just started: never saved, no undo step) opens the saved project it was painted in, when there is one: remembered in `<config>/blender_projects.json` each time a project linked to the object is saved or opened, else a recent file linked to it (the window's). It must be the same model (the same faces, or about the same place and size); the result then has `"project": path, "layers": n`. Not with `keep_layers: false`, or feature `bridge_projects` off. (0.44.1) A mesh without UVs (`had_uvs: false`) for an object Patina has keeps Patina's UVs, carried over by where the faces are (`"kept_uvs": share of faces matched`), when at least half the faces match; else it is unwrapped (`"generated_uvs": true`). Add-on 0.13.1 reads the UVs, second UV map and color attribute from the edit-mode BMesh: Blender 5.x leaves the mesh's attribute arrays empty in Edit Mode. |
| `bridge_update_mesh` | same as open, always keeps layers (reprojects for UV changes, transfers through 3D for topology changes) | same |
| `bridge_export` | `{"object":"Suzanne"}` | `{"textures":{key: path}, "uv_path": "/…/Suzanne.ptuv" or null}` — Patina writes textures (and UVs if they were edited) and returns where. |

## Commands Patina sends to Blender (port 7861)

| cmd | args |
|---|---|
| `ping` | – |
| `patina_textures_updated` | `{"object": "Suzanne", "textures": {"basecolor": "/…/Suzanne_basecolor.png", …}, "uv_path": "/…/Suzanne.ptuv" or null}` |
| `patina_material_library` | `{"size": 1024, "mapping": "box" or "uv", "which": "all" or "builtin"}`: the add-on (0.8+) starts building Blender's Patina material library (it calls `export_material_maps` back) and answers `{"started": true, "directory": "…"}` at once |
| `patina_new_object` | `{"path": "/…/Ball_patina.ptmesh", "name": "Ball_patina", "source": "Ball" or "", "replace": false, "textures": {…}, "texture_sets": […], "tile_mode": {…}}`: the add-on (0.9+) makes the object from the `.ptmesh` (Blender axes, UVs, material slots by name, seams, sharp edges, vertex groups) next to `source` with its transform and collections (or replaces `source`'s mesh), records it as sent (that `.ptmesh` is its sent mesh), and applies the textures as for `patina_textures_updated` |

On `patina_textures_updated` the add-on (on Blender's main thread, via a timer):

1. Finds the object by name.
2. If `uv_path` is given: reads `.ptuv`, checks the loop count, writes `loop_uvs`
   into the active UV layer (`uv_layer.data.foreach_set("uv", …)`) and
   `edge_seams` into `edge.use_seam`.
3. Loads or reloads each texture (`bpy.data.images`, named `Patina_<object>_<key>`,
   `image.reload()` when it already exists), sets color space.
4. Creates or updates a material `Patina_<object>` with a Principled BSDF:
   base color, roughness, metallic, normal (Normal Map node, tangent space),
   emission color (strength 1 if an emission map exists) and optionally height
   → Displacement node (midlevel 0.5) when the "Use displacement" preference is on.
   Assigns it to the object if it has no Patina material yet.
