# Patina Bridge for Blender

A live link between Blender and the Patina texture painter. You send a mesh to
Patina and paint it there. The textures, and any UVs you edited, come back as a
Principled BSDF material. They arrive automatically each time Patina pushes an
update, or when you click **Pull Textures**.

Supported versions: Blender 4.2 LTS to 4.5+ as an extension, and Blender 3.6 to 4.1
as a legacy add-on. The add-on uses only the Python standard library and the
numpy that ships with Blender.

## Install

Build the zip first:

```sh
blender/make_zip.sh          # -> blender/dist/patina_bridge.zip
```

**Blender 4.2 and newer (extension):**
1. Open *Edit > Preferences > Get Extensions*.
2. Open the drop-down menu (v) at the top right and choose **Install from Disk...**.
3. Pick `patina_bridge.zip`.
4. The add-on asks for *network* access (a localhost connection to Patina) and *files* access (the exchange folder). If *Allow Online Access* is off, the bridge still works because it only talks to `127.0.0.1`.

**Blender 3.6 to 4.1 (legacy add-on):**
open *Edit > Preferences > Add-ons > Install...*, pick the zip, and enable
**Import-Export: Patina Bridge**.

**Development:** symlink the folder instead of installing the zip:
`ln -s "$PWD/blender/patina_bridge" ~/.config/blender/4.2/scripts/addons/patina_bridge`
(or `.../extensions/user_default/patina_bridge` for 4.2+). Then use *Reload Scripts* (F3 search).

## Preferences

| Setting | Default | Notes |
|---|---|---|
| Patina Executable | `patina` | An absolute path, or a name looked up on `PATH` |
| Patina Port | 7860 | Patina's command server (`patina --port`) |
| Blender Listen Port | 7861 | Patina pushes `patina_textures_updated` here |
| Exchange Folder | `~/.cache/patina/bridge` | If empty, uses `$PATINA_BRIDGE_DIR` |
| Texture Resolution | 2048 | Size of the new texture set Patina creates |
| Start Live Link on Load | on | Starts the listener when the add-on loads |
| Use Displacement | off | Connects height through a Displacement node (midlevel 0.5) |
| Apply Modifiers When Sending | off | Sends the evaluated mesh. Deform-only modifiers (Armature, Shrinkwrap, shape keys) keep the UV round-trip; modifiers that add or remove faces **turn it off** because the topology no longer matches the original mesh |
| Auto Update Mesh | off | Re-sends a mesh that was sent to Patina after you change it, once the edits pause (0.5.0) |
| Settle Time | 1.5 s | How long the mesh must be unchanged before it is sent; sculpting waits three times as long |
| Show on Every Slot | on | Patina's material replaces the material in every slot (old ones are kept; **Restore Materials** puts them back). Off: it is added as a new slot |
| Material Library Folder | empty | Where **Add Patina Materials to Blender** puts the library; empty: `Documents/Patina Materials` (0.8.0) |

## Usage

Open the *3D Viewport* sidebar (**N**) and go to the **Patina** tab.

1. **Launch Patina** starts the executable with `--port <Patina Port>` as a separate process. The add-on then waits up to about 10 s for Patina to respond.
2. Select a mesh and click **Send to Patina**. The active object is written to `<exchange>/<object>/<object>.ptmesh` and opened in Patina. Edit-mode changes are flushed first. A mesh without UVs is sent with zero UVs and `"had_uvs": false`, and you unwrap it in Patina.
3. Paint in Patina. Whenever Patina exports, Blender:
   * writes the returned UVs and seams into the active UV map (when Patina's UVs differ from Blender's). If they can't be written, nothing is changed: the textures only fit Patina's UVs,
   * reloads the images `Patina_<object>_<key>` (basecolor and emission as sRGB, everything else as Non-Color),
   * builds or updates the material `Patina_<object>`. It contains a Principled BSDF and a tangent-space Normal Map node, plus emission, AO multiply and optional displacement when those maps exist.
4. **Pull Textures** asks Patina to export now and applies the result.
5. **Update Mesh in Patina** re-sends the mesh after modeling changes. Patina keeps the layers and reprojects the paint if the UVs changed.
6. **Live Link** starts or stops the listener that receives Patina's pushes.
7. **Auto Update Mesh** (the toggle under *Update Mesh in Patina*) does step 5 by itself: after a sent mesh changes, it is re-sent once the edits pause for the *Settle Time*. Only sent objects are watched, nothing is sent while Patina is busy, and the UVs Patina writes back don't trigger a send.
8. **Add Patina Materials to Blender** (the *Material Library* part of the tab, 0.8.0) puts every Patina material in Blender's **Asset Browser**. Patina bakes the maps (`export_material_maps`; the add-on starts a hidden `patina --headless` when Patina isn't running), a background Blender (`library_build.py`, `--factory-startup`) builds `Patina Materials.blend` from them with each material marked as an asset, in catalogs by category (`blender_assets.cats.txt`), with Patina's preview, and the folder is added to *Preferences › File Paths › Asset Libraries* as *Patina Materials*. Choose the map size (512, 1024, 2048), the mapping (*Box*: Object coordinates, box projection, bump from a 16-bit height map, scale in tiles per meter, as Patina projects; *UV*: the UV map and Patina's normal map) and whether your own materials come too. Run it again to update the library. Patina's Blender Link window has the same button (it sends `patina_material_library`).
9. **Patina → Blender models** (0.9.0): Patina's *Send Model to Blender as a New Object* (`send_model_to_blender`, message `patina_new_object`) makes a Blender object from Patina's mesh (its UVs, material slots, seams, sharp edges and vertex groups) next to the linked object with the same transform, puts Patina's material on it, and treats it as sent from then on; *Replace the Linked Object's Mesh* gives the linked object the mesh instead.
10. **Select Asymmetric Vertices** (shown once the mesh is in Patina) runs Patina's mesh symmetry check and selects the vertices whose mirror image across X misses the other side by more than 0.1% of the model's size, plus those with no other side. Use it to find what to fix with Symmetrize or snapping.

Patina paints one texture set per object, so `Patina_<object>` replaces the material in
every slot (face assignments stay). The replaced materials are kept in the file with a fake
user and listed on the object; **Restore Materials** in the panel puts them back. With
*Show on Every Slot* off, the material goes into a new slot instead (an empty slot or
Blender's default `Material` is still replaced).

With **texture sets per material slot** (Patina 0.9), each slot gets its own material,
`Patina_<object>_<set>`, named after the slot's material, which the add-on sends with the
mesh. With **UDIM tiles**, each map is one tiled image (`Patina_<object>_<map>`, source
*UDIM Tiles*, reading `<object>_<map>.<UDIM>.png`).

10. **Vertex colors** (0.10.0): *Send to Patina* sends the object's active color attribute too. A fresh object
   with colors starts in Patina from them, as the layer *From the model* (Patina 0.32 or newer; older versions ignore
   them).

11. **Lightmaps** (0.11.0): a model Patina sends (*Send Model to Blender*) brings its lightmap UVs as the UV map
   *Lightmap* (the textures stay on *UVMap*) and its vertex colors; *Send to Patina* sends an object's second UV map,
   which Patina keeps and exports.

## Troubleshooting

* **"Patina is not reachable on port 7860"**: Patina isn't running, or it is listening on another port. Check *Patina Port*, or use **Launch Patina**.
* **"Cannot listen on 7861: port is already in use"**: another Blender instance already runs the bridge. Change *Blender Listen Port* in one of them; Patina is told which port to use on every send.
* **"'patina' was not found on PATH"**: Blender started from a desktop launcher often has a shorter `PATH`. Set an absolute path in the preferences.
* **"… not updated: Patina changed its UVs, and they could not be applied"**: the reason follows in brackets. The textures are left as they were, because on the old UVs they would show up scrambled.
* **"UVs not applied: Patina sent N loops but … has M"**: the mesh changed in Blender after it was sent. Click **Update Mesh in Patina**, then pull again. UVs are not applied when the mesh was sent with modifiers that add or remove faces.
* **"UVs not applied: the UVs of … were changed in Blender after it was sent"**: Patina would overwrite your Blender UV edit. Click **Update Mesh in Patina** (the paint moves onto those UVs), then push again.
* **"UVs not applied: the face corners of … no longer match the mesh that was sent"**: the loop count is unchanged but the corners were re-ordered or re-wired (e.g. faces rebuilt, Sort Mesh Elements). Click **Update Mesh in Patina**, then pull again. UVs are written through BMesh when the mesh is in Edit Mode, so you don't have to leave it.
* **Textures look washed out or too dark**: check the image color spaces. Only basecolor and emission should be sRGB.
* All messages are printed to the system console with the prefix `[Patina]`. Start Blender from a terminal to see them.

## Development

The add-on is split into these modules:

* `protocol.py`: container format and JSON lines. It has no `bpy` imports, so it can be tested anywhere.
* `server.py`: listener, worker thread and timers.
* `mesh_io.py`, `materials.py`, `operators.py`, `ui.py`.

Run the tests:

```sh
python3 tests/blender/test_protocol.py         # container + JSON-lines client/server
python3 tests/blender/test_addon_fakebpy.py    # smoke test of the bpy modules against a fake bpy
```

The protocol is specified in `docs/BRIDGE.md`.
