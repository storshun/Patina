#include "engine/Features.h"

#include "core/I18n.h"

namespace patina {

const std::vector<FeatureInfo>& Features::all() {
  static const std::vector<FeatureInfo> list = {
      {"command_palette", N_("Command palette"),
       N_("Ctrl+Shift+P (F3 with the Blender keymap): search every action, window, command, material, brush, layer and "
          "recent file by name, and run it with Enter."),
       true, "0.10"},
      {"symmetry_check", N_("Mesh symmetry check"),
       N_("UV > Mirrored Halves > Check Mesh Symmetry and the Symmetry view mode: how far the left and right halves of the model differ, "
          "with a heat map, the worst spots and (from Blender) the off vertices selected."),
       true, "0.10"},
      {"texture_set_tools", N_("Texture set lock and isolate"),
       N_("With texture sets: Set lock keeps a stroke on the texture set (or material slot) it starts on; Isolate dims "
          "the other sets in the viewport and keeps strokes off them."),
       true, "0.10"},
      {"auto_texture", N_("Auto texture from slot names"),
       N_("Edit > Auto Texture from Material Slots: a library material for each material slot, guessed from its name "
          "(Skin, Jeans, Boots_Leather...), as fill layers masked to each slot."),
       true, "0.10"},
      {"macro_recorder", N_("Macro recorder"),
       N_("Edit > Record Macro: keeps the commands you run (menus, windows, scripts, Claude) to replay on another model "
          "or save as a job for 'patina run'."),
       true, "0.10"},
      {"patina_assistant", N_("Patina Assistant"),
       N_("Window > Assistant: type what you want in plain words (\"make the creases of the skin a bit redder\") and a "
          "small model on this computer turns it into Patina commands you can look at before keeping them. Needs a "
          "model file; nothing is sent anywhere."),
       true, "0.11"},
      {"retopology", N_("Retopology"),
       N_("Retopology menu: clean quads over a high-poly model that follow its shape, at the density you set in quads "
          "per square meter. Zone lines become edge loops and split the model into zones with a resolution of their "
          "own; flow strokes set which way the quads run; a density brush fine-tunes. The quads can be exported, sent "
          "to Blender or made the model you paint, with the high-poly baked onto it."),
       true, "0.13"},
      {"drag_drop", N_("Drag and drop into the layer stack"),
       N_("Materials, smart materials and smart masks drag from the Materials panel onto a layer (a fill layer takes the "
          "material, a layer takes the smart mask) or between layers (a new fill layer, the smart material, there); a "
          "fill layer's material and a layer's mask drag onto other layers; Ctrl+drag copies a layer."),
       true, "0.22.5"},
      {"more_procedural", N_("More procedural materials and masks"),
       N_("The material editor names each generator's settings (dropdowns for its styles) and adds grime, worn edges "
          "and warp to every procedural material; the mask stack gains Pattern (any material as a mask), Grow / shrink "
          "and Edges; the Gradient and Drips smart masks; and cells, cracks, ridges and streaks for noise."),
       true, "0.24"},
      {"blender_library", N_("Materials in Blender"),
       N_("Blender Link window: Add Patina's materials to Blender puts the whole material library in Blender's Asset "
          "Browser (with the Patina Bridge add-on 0.8)."),
       true, "0.25"},
      {"more_channels", N_("Opacity, sheen, clearcoat and subsurface"),
       N_("Four more material properties, painted like the others: opacity (lace, nets, leaves, cut-outs), sheen "
          "(velvet), clearcoat (car paint, lacquer) and subsurface (skin, wax). Materials, brushes, layers and the base "
          "material get them; the viewport shows them; exports and Blender get their maps when they are used."),
       true, "0.26"},
      {"mesh_check", N_("Mesh check and fixes"),
       N_("Mesh > Check Mesh: what the model is made of and what is wrong with it (holes, non-manifold edges, flipped "
          "faces, open seams, specks...), a heat map in the viewport, and fixes that keep the paint."),
       true, "0.27"},
      {"model_export", N_("Export the model, send it to Blender"),
       N_("File > Export Model: the model with its textures as .glb, .gltf or .obj. Blender > Send Model to Blender: the "
          "model as it is in Patina (mended...) becomes a Blender object with its textures (add-on 0.9)."),
       true, "0.28"},
      {"mesh_reduce", N_("Reduce polygons and levels of detail"),
       N_("Mesh > Reduce Polygons: fewer triangles, keeping the UV layout (the textures fit as they are), seams, material "
          "borders and open borders; Export Model can add levels of detail that share the textures."),
       true, "0.29"},
      {"recipes", N_("Recipes and batches"),
       N_("File > Batch Process: recipes (mend, reduce, transform, texture, export...) run on the open model or on whole "
          "folders of models by a hidden Patina, leaving the files as they were. Mesh > Transform too."),
       true, "0.30"},
      {"model_textures", N_("The model's own textures and vertex colors"),
       N_("Opening a textured or vertex-colored model (.glb, .gltf, .obj, .fbx, .ply, or an object from Blender) brings "
          "in its materials: each material slot's textures and colors, and the vertex colors, become a paint layer, "
          "From the model, at the bottom of the stack. File > Bring In the Model's Textures does it again."),
       true, "0.31"},
      {"texture_develop", N_("Develop the textures"),
       N_("Adjustment layers get a photo editor's basic develop controls: temperature and tint, exposure, highlights and shadows, "
          "vibrance, and De-light, which takes the shadows baked into scanned and generated textures out. The PBR Range "
          "view shows base colors a real material would not have."),
       true, "0.33"},
      {"collision", N_("Collision shapes"),
       N_("Mesh > Collision: boxes, a convex hull, a hull per part, or the model split into convex pieces, shown in "
          "green over the model and exported with the model (glTF, OBJ) under the names Unreal or Godot look for."),
       true, "0.34"},
      {"lightmap_uvs", N_("Lightmap UVs"),
       N_("Mesh > Lightmap UVs: a second UV map for baked lighting in game engines (no overlaps, even texel density, "
          "padding for the lightmap size), saved with the project, exported in glTF and sent to Blender."),
       true, "0.35"},
      {"toolbar_icons", N_("Icons in the toolbars"),
       N_("The tools on the left and the bars over the 3D view, the UV editor and the layers show icons drawn after "
          "the ones painting and 3D programs use (brush, eraser, bucket, eyedropper, scissors...), grouped by job: "
          "paint, UV, retopology, symmetry. Resting the pointer on one shows its name and shortcut; Preferences can "
          "put the names beside the icons. Off: the names alone, as before."),
       true, "0.36"},
      {"context_menus", N_("Right-click menus"),
       N_("A right-click (without dragging) opens a menu of what can be done there: in the 3D view for the spot under "
          "the pointer (pick its color, the layers painted there, fill or select its island, a note, the view), and on "
          "the tools, brushes, colors, history, notes and the empty parts of the panels."),
       true, "0.36"},
      {"translations", N_("Translations"),
       N_("Preferences > Language: Patina in another language from a translation file (.po, the format Poedit and "
          "Weblate use) in the languages folder. Anyone can make one from the template; see the manual's Translating "
          "Patina page."),
       true, "0.36"},
      {"retopo_preview_toggle", N_("Retopology preview button"),
       N_("A Retopology button in the bar over the 3D view shows the zones (or the density) and the quads on the "
          "model, and keeps a quick set of quads following each change, whichever panel is in front. Its arrow "
          "chooses what shows. Off: they show while the Retopology window is in front, as before."),
       true, "0.37"},
      {"axis_gizmo", N_("Axis gizmo"),
       N_("The X, Y and Z axes in the corner of the 3D view, turning with the camera, the way Blender and other 3D "
          "programs show them. Click an axis to look along it (click it again for the other side), drag to orbit."),
       true, "0.37"},
      {"viewport_pins", N_("Pin and unpin in the 3D view"),
       N_("Pin and Unpin buttons in the bar over the 3D view for the Select and Island tools, and Alt+P to unpin there "
          "too (before, only in the UV editor). While those tools are chosen, the pinned islands show blue stripes on "
          "the model."),
       true, "0.37.1"},
      {"view_snapping", N_("Side views and going back"),
       N_("The numpad and the axis gizmo jump to the side, top and bottom views, which are orthographic until you turn "
          "away (a preference), and Numpad 0 (or the Back button under the gizmo) goes back to the view you had before. "
          "Alt while orbiting with the middle button (Shift with Alt+left) snaps the view to the nearest side view, as "
          "in Blender."),
       true, "0.38"},
      {"view_pie", N_("View pie"),
       N_("Hold Z over the 3D view for a pie of eight ways to show the model: wireframe, solid, lit, path traced, unlit, "
          "seams, the UV checker and the retopology preview. Point and let go, or tap Z and click. (With More pie menus "
          "off: the 0.38 pie, with the Paint and Island tools, clear the selection and the retopology lines.)"),
       true, "0.38"},
      {"solid_view", N_("Solid view"),
       N_("A view mode that shows the shape alone: light gray, lit by the environment, without textures or the normal "
          "map, as Blender's Solid shading."),
       true, "0.38"},
      {"path_tracing", N_("Path traced view and renders"),
       N_("The Path traced view mode and render images: real shadows, reflections and light bouncing between the "
          "model's parts, lit by the environment (Shift + right drag turns it), a shadow on the ground. The picture "
          "gets cleaner the longer nothing changes."),
       true, "0.39"},
      {"pie_menus", N_("More pie menus"),
       N_("Besides the view pie on Z: Shift+Z for the channels (base color, normal, roughness...), ` (the key under Esc) "
          "for the edit modes (the tools), and Q for Quick Favorites, eight items of your own. Right-click a toolbar "
          "button or a pie item to add it; Preferences > Pie menus lists them."),
       true, "0.40"},
      {"brush_keys", N_("Brush flow, hardness and angle keys"),
       N_("With the Blender keymap, G and Shift+G set the brush's flow and hardness by moving the mouse, and Ctrl+F its "
          "angle, the way F and Shift+F set its size and opacity. Click to keep the new value, right-click to go back."),
       true, "0.40"},
      {"baked_layers", N_("Baked maps as layers"),
       N_("The baked ambient occlusion, curvature and thickness each get a layer at the bottom of the layer list. "
          "Paint on it to fix the bake (the eraser brings the bake back), blur, smudge or clone it, and add levels, "
          "blur and other adjustments. Smart masks, the 3D view and exports use the fixed map."),
       true, "0.41"},
      {"slider_typing", N_("Type values into sliders"),
       N_("Right-click any slider to type its value: Enter keeps it, Esc goes back. A typed value stays in the slider's "
          "range. (Ctrl+click does the same.)"),
       true, "0.42"},
      {"island_reach", N_("Island brush along the surface"),
       N_("The Island brush's Screen and Surface buttons. Surface takes the faces within the brush's size along the "
          "surface from the cursor, never a layer behind or a part only near in space. Off: the circle on screen only. "
          "Either way, faces turned away from you are left out."),
       true, "0.43"},
      {"select_visible", N_("Select only what the camera sees"),
       N_("The Select tool's Visible only: a click or drag on a UV island takes just its faces the camera sees (turned "
          "toward you, on screen, not hidden behind other faces), so the inside of a model or a skin under clothes "
          "stays out."),
       true, "0.45"},
      {"bridge_projects", N_("Blender: reopen an object's project"),
       N_("When Blender sends an object Patina doesn't have open (after a restart), Patina opens the saved project it "
          "was painted in and keeps its layers (reprojected if the mesh changed). Off: a new project each time."),
       true, "0.44"},
      {"bridge_materials", N_("Blender: start from Blender's materials"),
       N_("An object from Blender with no project of its own starts from the textures and colors its materials show "
          "(Patina's own after a push, or any other), as a layer. Needs the Patina Bridge add-on 0.13."),
       true, "0.44"},
      {"guided_layout", N_("Guided layout"),
       N_("The work in five steps along the top (Model, UVs, Bake, Paint, Export), each a screen with what it needs in "
          "front, and the Guided / Full switch at the right of the menu bar. Off: the Full layout only."),
       true, "0.46"},
      {"bake_down", N_("Bake-down"),
       N_("Bake > Bake Down to a Low-Poly Model: the layers of the painted high-poly model moved onto its low-poly "
          "version (a file, or the retopology's quads), with the check view and the high-poly in the compare view. "
          "Off: Use the Quads as the Model starts the layers afresh."),
       true, "1.1"},
      {"scene_objects", N_("Objects in a scene"),
       N_("Several models in one project, each an object with its own layers (a body, its clothes, eyes, hair): the "
          "Objects list above the layers, File > Add a Model to the Scene, a file with several meshes opened as an "
          "object each, the Object tool to move, turn and scale them, and several objects sent from Blender. Off: one "
          "model per project, a file's meshes opened as one."),
       true, "1.2"},
      {"face_sets", N_("Face sets"),
       N_("Named, colored groups of faces kept in the project (the Face Sets panel): one click selects, hides, shows "
          "alone, fills or masks one, the Face set lock keeps strokes inside one, the Face sets view shows them, and "
          "Blender's sculpt face sets come in with the model. Off: no panel, the lock and the view are gone."),
       true, "1.2"},
  };
  return list;
}

const FeatureInfo* Features::find(const std::string& id) {
  for (auto& f : all())
    if (id == f.id) return &f;
  return nullptr;
}

bool Features::on(const std::string& id) const {
  auto it = state_.find(id);
  if (it != state_.end()) return it->second;
  const FeatureInfo* f = find(id);
  return f ? f->defaultOn : false;
}

bool Features::set(const std::string& id, bool value) {
  const FeatureInfo* f = find(id);
  if (!f) return false;
  if (value == f->defaultOn) state_.erase(id);
  else state_[id] = value;
  return true;
}

nlohmann::json Features::toJson() const {
  nlohmann::json j = nlohmann::json::object();
  for (auto& [k, v] : state_) j[k] = v;
  return j;
}

void Features::fromJson(const nlohmann::json& j) {
  state_.clear();
  if (!j.is_object()) return;
  for (auto& [k, v] : j.items())
    if (v.is_boolean()) set(k, v.get<bool>());
}

}  // namespace patina
