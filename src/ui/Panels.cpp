// Side panels, menus and dialogs.
#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <cstring>
#include <filesystem>

#include "core/Log.h"
#include "core/Paths.h"
#include "core/UVOps.h"
#include "mcp/ClaudeSetup.h"
#include "engine/SmartMaterial.h"
#include "ui/App.h"

namespace patina {

namespace fs = std::filesystem;

static ImTextureID texId(GLuint t) { return (ImTextureID)(intptr_t)t; }

static void helpMarker(const char* text) {
  ImGui::SameLine();
  ImGui::TextDisabled("(?)");
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text);
}

static bool colorEdit(const char* label, vec3& c) { return ImGui::ColorEdit3(label, &c.x, ImGuiColorEditFlags_NoInputs); }

// ------------------------------------------------------------------ menus

// a menu of the menu bar; where it is goes in uiRects as menu.<key> (tests and the manual's screenshots)
bool menuHeader(Engine& engine, const char* key, const char* label) {
  ImGuiWindow* bar = ImGui::GetCurrentWindow();
  bool open = ImGui::BeginMenu(label);
  ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  engine.uiRects[std::string("menu.") + key] = {a.x, a.y, b.x - a.x, b.y - a.y};
  // (0.47) a line under the menus of the work, in the color of their step
  std::string k = key;
  App::GuidedStep s = k == "mesh" || k == "retopology" ? App::GuidedStep::Model
                    : k == "uv"                        ? App::GuidedStep::UVs
                    : k == "bake"                      ? App::GuidedStep::Bake
                    : k == "blender"                   ? App::GuidedStep::Export
                                                       : App::GuidedStep::Count;
  if (s != App::GuidedStep::Count && engine.features.on("guided_layout")) {
    float t = std::max(1.f, std::round(2.f * ImGui::GetFontSize() / 15.f)), pad = ImGui::GetStyle().FramePadding.x;
    bar->DrawList->AddRectFilled(ImVec2(a.x + pad, b.y - t - 1.f), ImVec2(b.x - pad, b.y - 1.f),
                                 App::mixColor(App::stepColor(s), App::stepColor(s), 0.f, 0.85f), t * 0.5f);
  }
  return open;
}

void App::drawMenuBar() {
  if (!ImGui::BeginMainMenuBar()) return;
  Document& d = engine.doc;
  if (menuHeader(engine, "file", L_("File"))) {
    if (ImGui::BeginMenu(L_("New"))) {
      auto make = [&](const char* p) {
        std::string prim = p;
        replaceProject(T_("start a new project"), [this, prim] { engine.execute("new_project", {{"primitive", prim}}); });
      };
      if (ImGui::MenuItem(L_("Sphere"))) make("sphere");
      if (ImGui::MenuItem(L_("Cube"))) make("cube");
      if (ImGui::MenuItem(L_("Plane"))) make("plane");
      if (ImGui::MenuItem(L_("Torus"))) make("torus");
      ImGui::EndMenu();
    }
    if (ImGui::MenuItem(L_("Open..."), keyLabel("file.open")))
      replaceProject(T_("open another file"), [this] { openFileDialog(DialogPurpose::OpenFile); });
    if (engine.features.on("scene_objects") &&
        ImGui::MenuItem(L_("Add a Model to the Scene..."), keyLabel("file.add_model"), false, engine.doc.hasMesh()))
      openFileDialog(DialogPurpose::AddObject);
    if (engine.features.on("scene_objects") && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
      ImGui::SetTooltip("%s", T_("A model file as another object in this project (a character's clothes, eyes, hair),\n"
                                 "where the file puts it, with its own layers and texture set"));
    if (ImGui::BeginMenu(L_("Open Recent"), !recentFiles.empty())) {
      std::string open;
      for (auto& f : recentFiles) {
        std::error_code ec;
        bool exists = fs::exists(f, ec);
        std::string label = fs::path(f).filename().string() + "##" + f;
        if (ImGui::MenuItem(label.c_str(), nullptr, false, exists)) open = f;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s%s", f.c_str(), exists ? "" : T_("\n(missing)"));
      }
      ImGui::Separator();
      if (ImGui::MenuItem(L_("Clear the list"))) recentFiles.clear(), saveSession(true);
      ImGui::EndMenu();
      if (!open.empty())
        replaceProject(T_("open another file"), [this, open] {
          std::string err;
          if (engine.openFile(open, &err)) setStatus(F_("Opened %s", open.c_str()));
          else showError(F_("Could not open %s", fs::path(open).filename().string().c_str()), err);
        });
    }
    {
      std::string autosave = (paths::cacheDir() / "autosave.patina").string();
      std::error_code ec;
      if (ImGui::MenuItem(L_("Open Autosave"), nullptr, false, fs::exists(autosave, ec)))
        replaceProject(T_("open the autosave"), [this, autosave] {
          std::string err;
          if (engine.openFile(autosave, &err)) {
            for (int i = 0; i < engine.objectCount(); ++i) engine.object(i).projectPath.clear();
            engine.doc.dirty = true;
            setStatus(T_("Recovered the autosave - use Save As to keep it"));
          } else {
            setStatus(err);
          }
        });
    }
    if (ImGui::MenuItem(L_("Save"), keyLabel("file.save"))) saveProject(false);
    if (ImGui::MenuItem(L_("Save As..."), keyLabel("file.save_as"))) saveProject(true);
    if (ImGui::MenuItem(L_("Save Incremental"), keyLabel("file.save_incremental"))) saveIncremental();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T_("Saves the next version next to the project: name_v002.patina, _v003..."));
    ImGui::Separator();
    if (ImGui::MenuItem(L_("Import Material Folder..."))) openFileDialog(DialogPurpose::ImportMaterial);
    if (engine.features.on("model_textures")) {
      if (ImGui::MenuItem(L_("Bring In the Model's Textures"), nullptr, false, d.hasMesh() && !d.meshPath.empty())) {
        json r;
        if (runCommand("import_model_textures", json::object(), &r)) noteModelTextures();
      }
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", T_("The textures and colors of each material slot in the model's file, as a paint layer at "
                                   "the bottom of the stack (opening a textured model does this by itself)"));
    }
    if (ImGui::MenuItem(L_("Export Textures..."), keyLabel("file.export"))) exportWhat = 0, showExport = true;
    if (ImGui::MenuItem(L_("Export Again"), keyLabel("file.export_again"), false, d.hasLastExport)) exportAgain();
    if (ImGui::IsItemHovered() && d.hasLastExport)
      ImGui::SetTooltip("%s (%s)", d.lastExport.directory.c_str(), exportPresetName(d.lastExport.preset));
    if (engine.features.on("model_export")) {
      if (ImGui::MenuItem(L_("Export Model..."), nullptr, false, d.hasMesh())) showModelExport = true;
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T_("The model with its textures: .glb, .gltf, .obj or .stl"));
    }
    if (engine.features.on("recipes")) {
      if (ImGui::MenuItem(L_("Batch Process..."))) showBatch = true;
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T_("Recipes (mend, reduce, texture, export) on the open model or on many files"));
    }
    if (ImGui::MenuItem(L_("Render Image..."), keyLabel("file.render"))) showRender = true;
    ImGui::Separator();
    if (ImGui::MenuItem(L_("Reload Mesh (keep layers)"), nullptr, false, !d.meshPath.empty())) {
      json r;
      if (runCommand("reload_mesh", json::object(), &r)) setStatus(F_("Reloaded %s", d.meshPath.c_str()));
    }
    ImGui::MenuItem(L_("Reload Automatically When the Mesh Changes"), nullptr, &autoReloadMesh);
    ImGui::Separator();
    if (ImGui::MenuItem(L_("Quit"), keyLabel("file.quit"))) {
      SDL_Event q;
      q.type = SDL_EVENT_QUIT;
      SDL_PushEvent(&q);
    }
    ImGui::EndMenu();
  }
  if (menuHeader(engine, "edit", L_("Edit"))) {
    std::string u = F_("Undo %s", d.undo.undoLabel().c_str()) + "###undo", r = F_("Redo %s", d.undo.redoLabel().c_str()) + "###redo";
    if (ImGui::MenuItem(u.c_str(), keyLabel("edit.undo"), false, d.undo.canUndo())) doUndo();
    if (ImGui::MenuItem(r.c_str(), keyLabel("edit.redo"), false, d.undo.canRedo())) doRedo();
    if (engine.features.on("command_palette") && ImGui::MenuItem(L_("Search Everything..."), keyLabel("app.palette")))
      openCommandPalette();
    ImGui::Separator();
    if (ImGui::MenuItem(L_("Add Paint Layer"), keyLabel("layer.new_paint"))) runCommand("add_layer", {{"type", "paint"}});
    if (ImGui::MenuItem(L_("Add Fill Layer (current material)"))) d.addFillLayer(engine.painter.material);
    if (ImGui::MenuItem(L_("Add Folder"))) runCommand("add_layer", {{"type", "folder"}});
    if (engine.features.on("auto_texture")) {
      if (ImGui::MenuItem(L_("Auto Texture from Material Slots..."), nullptr, false, d.hasMesh())) openAutoTexture();
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", T_("A library material for each Blender material slot, guessed from its name\n(Skin, Jeans, Boots_Leather...), as fill layers you can change"));
    }
    if (ImGui::MenuItem(L_("Group Selected into Folder"), keyLabel("layer.group"), false, d.selected >= 0)) groupSelectedIntoFolder();
    if (engine.features.on("macro_recorder")) {
      ImGui::Separator();
      if (ImGui::MenuItem(engine.macro.recording ? L_("Stop Recording Macro") : L_("Record Macro"), keyLabel("macro.record"))) {
        if (engine.macro.recording) engine.macro.recording = false;
        else engine.macro.start(false);
      }
      if (ImGui::MenuItem(L_("Play Macro"), keyLabel("macro.play"), false, !engine.macroSteps().empty())) playMacro(json::object());
      if (ImGui::MenuItem(L_("Macro Recorder..."))) showMacros = true;
    }
    if (engine.features.on("patina_assistant")) {
      ImGui::Separator();
      if (ImGui::MenuItem(L_("Assistant..."))) openAssistant();
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T_("Say what you want in plain words; a small model on this computer makes it a plan"));
    }
    ImGui::Separator();
    if (ImGui::MenuItem(L_("Keyboard Shortcuts..."))) showKeymap = true;
    if (ImGui::MenuItem(L_("Preferences..."))) showPreferences = true;
    ImGui::EndMenu();
  }
  if (menuHeader(engine, "view", L_("View"))) {
    ImGui::MenuItem(L_("Wireframe"), nullptr, &engine.viewOptions.wireframe);
    ImGui::MenuItem(L_("Seams"), nullptr, &engine.viewOptions.seams);
    ImGui::MenuItem(L_("Tint UV islands"), nullptr, &engine.viewOptions.islandTint);
    ImGui::MenuItem(L_("Environment background"), nullptr, &engine.viewOptions.envBackground);
    ImGui::MenuItem(L_("Use baked AO"), nullptr, &engine.viewOptions.useAO);
    if (engine.features.on("retopology") && engine.features.on("retopo_preview_toggle"))
      ImGui::MenuItem(L_("Retopology preview"), keyLabel("view.retopo_preview"), &engine.viewOptions.retopoPreview);
    if (engine.features.on("axis_gizmo")) ImGui::MenuItem(L_("Axis gizmo"), nullptr, &engine.viewOptions.axisGizmo);
    if (engine.features.on("texture_set_tools") && d.mesh.tiles.active())
      ImGui::MenuItem(L_("Isolate current texture set"), keyLabel("view.isolate_set"), &engine.viewOptions.isolateSet);
    ImGui::Separator();
    if (ImGui::MenuItem(L_("Frame mesh"), "F")) engine.frameMesh();
    static const char* kViews[][2] = {{"front", N_("View front")}, {"back", N_("View back")},     {"left", N_("View left")},
                                      {"right", N_("View right")}, {"top", N_("View top")},       {"bottom", N_("View bottom")},
                                      {"persp", N_("View persp")}};  // (the id, the menu item)
    for (auto& v : kViews) {
      float y, p;
      if (!ImGui::MenuItem(L_(v[1]))) continue;
      if (Camera::viewAngles(v[0], y, p)) snapToView(y, p);  // (0.38: remembered, Numpad 0 goes back)
      else engine.camera.setView(v[0]);
    }
    if (ImGui::MenuItem(L_("Orthographic"), "Num 5", engine.camera.ortho))
      engine.camera.ortho = !engine.camera.ortho, engine.viewMemory.orthoBySnap = false;
    if (viewSnapping() && ImGui::MenuItem(L_("Back to your view"), keyLabel("view.back"), false, engine.viewMemory.hasBack)) goBackView();
    ImGui::Separator();
    compareMenu();
    if (ImGui::MenuItem(L_("Hide panels (distraction-free)"), keyLabel("view.focus"), distractionFree))
      distractionFree = !distractionFree, distractionFreeSince = nowSeconds();
    if (ImGui::BeginMenu(L_("UI scale"))) {
      for (float sc : {0.75f, 0.85f, 1.f, 1.15f, 1.25f, 1.5f, 1.75f, 2.f}) {
        char b[16];
        snprintf(b, sizeof b, "%d%%", int(sc * 100 + 0.5f));
        if (ImGui::MenuItem(b, nullptr, std::abs(userUiScale - sc) < 0.01f)) applyUiScale(sc);
      }
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(L_("Navigation"))) {
      for (int i = 0; i < (int)NavPreset::Count; ++i)
        if (ImGui::MenuItem(L_(navPresetName((NavPreset)i)), nullptr, (int)navPreset == i)) navPreset = (NavPreset)i;
      ImGui::Separator();
      ImGui::TextDisabled("%s", T_("Ctrl+right-drag always resizes the brush; the wheel always zooms."));
      ImGui::EndMenu();
    }
    ImGui::Separator();
    if (ImGui::MenuItem(L_("Reset layout"))) requestLayoutReset = true;
    if (std::getenv("PATINA_DEV")) ImGui::MenuItem("ImGui demo", nullptr, &showDemo);  // (for Patina's developers)
    ImGui::EndMenu();
  }
  if (engine.features.on("mesh_check") && menuHeader(engine, "mesh", L_("Mesh"))) {
    if (ImGui::MenuItem(L_("Check Mesh..."), nullptr, false, d.hasMesh())) showMeshCheck = true;
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", T_("What the model is made of and what is wrong with it: holes, flipped faces, open seams, specks..."));
    if (ImGui::MenuItem(L_("Fix Everything That Can Be Fixed"), nullptr, false, d.hasMesh())) {
      json res;
      if (runCommand("mesh_fix", {{"all", true}}, &res)) setStatus(describeMeshFix(res));
    }
    if (engine.features.on("recipes")) {
      if (ImGui::MenuItem(L_("Transform..."), nullptr, false, d.hasMesh())) showTransform = true;
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T_("Units, size, turn, origin, stand it on the ground"));
    }
    if (engine.features.on("collision")) {
      if (ImGui::MenuItem(L_("Collision..."), nullptr, false, d.hasMesh())) showCollisionWindow = true;
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T_("Boxes, hulls or convex pieces for game engines"));
    }
    if (engine.features.on("lightmap_uvs")) {
      if (ImGui::MenuItem(L_("Lightmap UVs..."), nullptr, false, d.hasMesh())) showLightmapWindow = true;
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T_("A second UV map for baked lighting in game engines"));
    }
    if (engine.features.on("mesh_reduce")) {
      if (ImGui::MenuItem(L_("Reduce Polygons..."), nullptr, false, d.hasMesh())) showReduce = true;
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T_("Fewer triangles, keeping the UVs: the textures fit as they are"));
    }
    const char* back = d.lastMeshChange() == "reduction"   ? L_("Back to Before the Reduction")
                       : d.lastMeshChange() == "transform" ? L_("Back to Before the Transform")
                                                           : L_("Back to Before the Fix");
    if (ImGui::MenuItem(back, nullptr, false, d.canRevertMeshFix())) runCommand("mesh_fix_revert", json::object());
    ImGui::EndMenu();
  }
  retopoMenu();
  // (0.47) the menus of the work in the order of the Guided layout's steps: Mesh and Retopology (Model), UV, Bake,
  // Blender (Export)
  if (menuHeader(engine, "uv", L_("UV"))) {
    auto run = [&](const char* cmd, json args) {
      try {
        engine.execute(cmd, args);
      } catch (std::exception& e) {
        setStatus(e.what());
      }
    };
    // (0.47) the UV menu in groups: what most models need at the top level, the rest a submenu each, as the UV
    // editor's right-click menu has them
    bool hasSel = !d.selectedFaces().empty();
    if (engine.features.on("face_sets") && ImGui::MenuItem(L_("Face Sets..."))) openFaceSets();  // (1.2)
    if (ImGui::BeginMenu(hasSel ? L_("Unwrap the Selection###unwrap") : L_("Unwrap###unwrap"))) {
      auto unwrap = [&](const char* method) {
        run("uv_unwrap", {{"method", method}, {"selection", hasSel}, {"reproject", reprojectOnUVEdit}});
      };
      if (ImGui::MenuItem(L_("Along the Seams (LSCM)"))) unwrap("lscm");
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T_("Flattens each piece the seams cut out. Lay the seams first (the Seams tool, M)."));
      if (ImGui::MenuItem(L_("Smart UV Project"))) unwrap("smart");
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T_("Cuts where the model bends sharply and lays each piece flat: hard-surface models"));
      if (ImGui::MenuItem(L_("Box Project"))) unwrap("box");
      if (ImGui::MenuItem(L_("Planar from view"))) unwrap("planar");
      if (ImGui::MenuItem(L_("Cylinder Project"))) unwrap("cylinder");
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T_("Unrolls around the longest axis: arms, legs, tubes, barrels. Select one part first."));
      if (ImGui::MenuItem(L_("Sphere Project"))) unwrap("sphere");
      ImGui::EndMenu();
    }
    if (ImGui::MenuItem(hasSel ? L_("Relax Islands (selection)") : L_("Relax Islands")))
      run("uv_relax", {{"selection", hasSel}, {"reproject", reprojectOnUVEdit}});
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T_("Removes stretch: each island is solved again and put back in place, at the same size"));
    if (ImGui::MenuItem(L_("Pack Islands"))) run("uv_pack", {{"reproject", reprojectOnUVEdit}});
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T_("Packs the islands by their outlines, so small ones fill the gaps of big ones"));
    size_t ov = overlappingIslandCount();
    std::string ovl = (ov ? F_("Fix Overlapping UVs (%zu)", ov) : std::string(T_("Fix Overlapping UVs (none)"))) + "###fixoverlaps";
    if (ImGui::MenuItem(ovl.c_str(), nullptr, false, ov > 0)) run("uv_fix_overlaps", {{"reproject", reprojectOnUVEdit}});
    ImGui::Separator();
    if (ImGui::MenuItem(L_("Island Brush"), keyLabel("tool.island"))) selectTool(ViewportTool::Island);
    if (ImGui::MenuItem(L_("Make Island from Selection"), keyLabel("island.make"), false, hasSel)) makeIslandFromSelection();
    if (ImGui::BeginMenu(L_("Islands"))) {
      if (ImGui::MenuItem(L_("Orient Upright"), nullptr, false, hasSel)) run("uv_orient", {{"mode", "upright"}, {"selection", true}, {"reproject", reprojectOnUVEdit}});
      if (ImGui::MenuItem(L_("Orient Along Their Longest Side"), nullptr, false, hasSel)) run("uv_orient", {{"mode", "longest_axis"}, {"selection", true}, {"reproject", reprojectOnUVEdit}});
      if (ImGui::MenuItem(hasSel ? L_("Rectify Quad Islands (selection)") : L_("Rectify Quad Islands")))
        run("uv_rectify", {{"selection", hasSel}, {"reproject", reprojectOnUVEdit}});
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", T_("Straightens islands made of quads (belts, pipes, trims, planks) into a straight grid"));
      if (ImGui::MenuItem(L_("Move Selection into Free Space"), nullptr, false, hasSel)) run("uv_pack", {{"free_space", true}, {"selection", true}, {"reproject", reprojectOnUVEdit}});
      ImGui::Separator();
      if (ImGui::MenuItem(L_("Texel Density..."))) showDensity = true;
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(L_("Mirrored Halves"))) {
      if (ImGui::MenuItem(L_("Mirror UVs (X)"))) {
        json r;
        if (runCommand("uv_mirror", {{"axes", {"x"}}, {"reproject", reprojectOnUVEdit}}, &r))
          setStatus(F_("%d faces on the -X side now share the UVs of the +X side", r.value("mirrored_faces", 0)));
      }
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", T_("The -X half of a symmetric model takes the mirrored UVs of the +X half: both share one place in the texture"));
      if (ImGui::MenuItem(L_("Re-stack Mirrored Halves"), nullptr, false, uvUnstacked > 0))
        uvRunOnSelection("uv_restack", {{"reproject", reprojectOnUVEdit}}, T_("Mirrored halves are stacked on their other half again"));
      if (ImGui::MenuItem(L_("Unstack Mirrored Halves (selection)"), nullptr, false, hasSel))
        uvRunOnSelection("uv_unstack", {{"selection", true}, {"reproject", reprojectOnUVEdit}}, T_("The mirrored half has its own place now"));
      if (engine.features.on("symmetry_check")) {
        ImGui::Separator();
        if (ImGui::MenuItem(L_("Check Mesh Symmetry..."))) showSymmetry = true;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T_("How far the two halves of the model differ, with a heat map and the worst spots"));
      }
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(L_("Pins"))) {
      if (ImGui::MenuItem(L_("Pin Selected Islands"), T_("P (UV editor)"), false, hasSel)) uvPinSelection(true);
      if (ImGui::MenuItem(L_("Unpin Selected Islands"), keyLabel("uv.unpin"), false, hasSel)) uvPinSelection(false);
      if (ImGui::MenuItem(L_("Unpin Every Island"), nullptr, false, uv::pinnedFaceCount(d.mesh) > 0))
        uvRunOnSelection("uv_pin", {{"all", true}, {"pin", false}}, T_("No islands are pinned now"));
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(L_("Selection"))) {
      if (ImGui::MenuItem(L_("Grow Selection"), keyLabel("select.grow"), false, hasSel)) run("select_faces", {{"mode", "grow"}});
      if (ImGui::MenuItem(L_("Shrink Selection"), keyLabel("select.shrink"), false, hasSel)) run("select_faces", {{"mode", "shrink"}});
      if (ImGui::MenuItem(L_("Select the Whole Material Slot"), nullptr, false, hasSel && !d.mesh.faceMaterial.empty())) {
        // every face that uses a material slot of the selected faces
        std::vector<uint32_t> slots;
        for (uint32_t f : d.selectedFaces())
          if (f < d.mesh.faceMaterial.size() &&
              std::find(slots.begin(), slots.end(), d.mesh.faceMaterial[f]) == slots.end())
            slots.push_back(d.mesh.faceMaterial[f]);
        for (uint32_t sl : slots) run("select_faces", {{"mode", "material"}, {"material", sl}});
      }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T_("Adds every face that uses the same Blender material slot as the selection"));
      if (ImGui::MenuItem(L_("New Texture Set from Selection..."), nullptr, false, hasSel)) openNewTextureSet();
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T_("A texture of their own for the selected faces, such as one for the head and one for the body"));
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(L_("Seams"))) {
      if (ImGui::MenuItem(L_("Seams from Islands"))) run("uv_seams", {{"mode", "from_islands"}});
      if (ImGui::MenuItem(L_("Seams by Angle (60)"))) run("uv_seams", {{"mode", "by_angle"}, {"angle", 60}});
      if (ImGui::MenuItem(L_("Clear Seams"))) run("uv_seams", {{"mode", "clear"}});
      ImGui::EndMenu();
    }
    ImGui::Separator();
    ImGui::MenuItem(L_("Keep paint on UV edits"), nullptr, &reprojectOnUVEdit);
    ImGui::EndMenu();
  }
  if (menuHeader(engine, "bake", L_("Bake"))) {
    if (ImGui::MenuItem(L_("Ambient Occlusion"))) {
      d.tex.bakeAO(d.mesh, d.gpu, d.aoSamples);
      d.markComposite();
    }
    if (ImGui::MenuItem(L_("Curvature"))) {
      d.tex.bakeCurvature(d.mesh, d.gpu, &d.bvh);
      d.markComposite();
    }
    if (ImGui::MenuItem(L_("Thickness"))) {
      d.tex.bakeThickness(d.mesh, d.bvh, d.thicknessRays);
      d.markComposite();
    }
    if (ImGui::MenuItem(L_("All mesh maps"))) d.rebakeMeshMaps(true, true, true);
    ImGui::Separator();
    if (ImGui::MenuItem(L_("From a High-poly Mesh..."))) showHighPoly = true;
    if (ImGui::MenuItem(L_("Remove the High-poly Bake"), nullptr, false, d.tex.hasHighNormal()))
      runCommand("bake_from_high_poly", {{"clear", true}});
    if (engine.features.on("bake_down")) {
      ImGui::Separator();
      if (ImGui::MenuItem(L_("Bake Down to a Low-Poly Model..."))) showBakeDown = true;
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", T_("Painted the high-poly? Every layer moves onto its low-poly version, which becomes the model"));
      bool cur = d.bakeDownCurrent();
      if (ImGui::MenuItem(L_("Bake-Down Check"), nullptr, engine.viewOptions.mode == ViewMode::BakeCheck, cur))
        engine.viewOptions.mode = engine.viewOptions.mode == ViewMode::BakeCheck ? ViewMode::Material : ViewMode::BakeCheck;
    }
    ImGui::EndMenu();
  }
  if (menuHeader(engine, "blender", L_("Blender"))) {
    bool linked = d.bridge.active;
    if (ImGui::MenuItem(L_("Send textures to Blender"), keyLabel("blender.send"), false, linked)) {
      std::string err;
      if (engine.pushToBlender(&err)) setStatus(T_("Sent textures to Blender"));
      else setStatus(F_("Send failed: %s", err.c_str()));
    }
    ImGui::MenuItem(L_("Auto-send after changes"), nullptr, &engine.autoPushToBlender, linked);
    if (engine.features.on("model_export")) {
      ImGui::Separator();
      if (ImGui::MenuItem(L_("Send Model to Blender as a New Object"), nullptr, false, d.hasMesh())) {
        json r;
        if (runCommand("send_model_to_blender", json::object(), &r))
          setStatus(F_("Sent the model to Blender as %s: Patina is linked to it now", r.value("object", std::string()).c_str()));
      }
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", T_("The model as it is in Patina (mended, its UVs and slots) becomes a Blender object, next to\n"
                                   "the linked one, with its textures. Patina links to it from then on."));
      if (ImGui::MenuItem(L_("Replace the Linked Object's Mesh"), nullptr, false, linked)) {
        json r;
        if (runCommand("send_model_to_blender", {{"replace", true}}, &r)) setStatus(F_("Replaced the mesh of %s", d.bridge.object.c_str()));
      }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T_("Blender's object takes Patina's mesh (its modifiers stay). Undo in Blender goes back."));
    }
    ImGui::Separator();
    ImGui::TextDisabled("%s", linked ? F_("Linked: %s", d.bridge.object.c_str()).c_str() : T_("Not linked - use 'Send to Patina' in Blender"));
    ImGui::EndMenu();
  }
  if (menuHeader(engine, "help", L_("Help"))) {
    if (ImGui::MenuItem(L_("Manual (opens in your browser)"))) openLocalPage("manual");
    if (ImGui::MenuItem(L_("What's New (the change log)"))) openLocalPage("manual:changes");  // (1.2)
    if (ImGui::MenuItem(L_("Shortcuts & usage"), "F1")) showHelp = true;
    if (ImGui::MenuItem(L_("Patina Web (browser painter)"))) openLocalPage("web");
    if (ImGui::MenuItem(L_("Welcome Window"))) showWelcome = true;
    if (ImGui::MenuItem(L_("Pen Tablet Test..."))) showPenTest = true;
    ImGui::Separator();
    if (ImGui::MenuItem(L_("About Patina"))) showAbout = true;
    ImGui::EndMenu();
  }
  layoutSwitch();  // (0.46) Guided | Full
  ImGui::EndMainMenuBar();
}

void App::handleShortcuts() {
  ImGuiIO& io = ImGui::GetIO();
  if (io.WantTextInput || !keymapCapture.empty()) return;
  Document& d = engine.doc;
  if (keyPressed("app.palette") || keyPressed("app.palette_alt")) openCommandPalette();
  if (keyPressed("edit.undo", true)) doUndo();
  if (keyPressed("edit.redo", true) || keyPressed("edit.redo_alt", true)) doRedo();
  if (keyPressed("file.save")) saveProject(false);
  if (keyPressed("file.save_as")) saveProject(true);
  if (keyPressed("file.open")) replaceProject(T_("open another file"), [this] { openFileDialog(DialogPurpose::OpenFile); });
  if (keyPressed("file.add_model") && engine.features.on("scene_objects") && engine.doc.hasMesh()) openFileDialog(DialogPurpose::AddObject);
  if (keyPressed("view.local") && engine.features.on("scene_objects") && engine.objectCount() > 1) {
    engine.isolateActive = !engine.isolateActive;
    setStatus(engine.isolateActive ? F_("Only %s is shown (%s: all of them again)", engine.doc.displayName().c_str(), keyLabel("view.local"))
                                   : std::string(T_("Every object is shown")));
  }
  if (keyPressed("file.export")) exportWhat = 0, showExport = true;
  if (keyPressed("file.export_again")) exportAgain();
  if (keyPressed("file.save_incremental")) saveIncremental();
  if (keyPressed("file.render")) showRender = true;
  if (keyPressed("layer.new_paint")) d.addPaintLayer();
  if (keyPressed("layer.add_menu")) openAddLayerMenu = true;
  if (keyPressed("layer.group")) groupSelectedIntoFolder();
  if (keyPressed("layer.duplicate") && d.selected >= 0) d.duplicateLayer(d.selected);
  if (keyPressed("layer.instance") && d.selected >= 0) d.addInstance(d.selected);
  if (keyPressed("layer.solo") && d.selectedLayer()) {
    d.soloUid = d.soloUid == d.selectedLayer()->uid ? 0 : d.selectedLayer()->uid;
    d.compositeDirty = true;
  }
  if (keyPressed("file.quit")) {
    SDL_Event q;
    q.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&q);
  }
  if (keyPressed("blender.send") && d.bridge.active) {
    std::string err;
    if (engine.pushToBlender(&err)) setStatus(T_("Sent textures to Blender"));
    else showError(T_("Could not send the textures to Blender"), err);
  }
  if (keyPressed("view.help")) showHelp = !showHelp;
  if (keyPressed("view.focus")) distractionFree = !distractionFree, distractionFreeSince = nowSeconds();
  // (0.40) the view pie's items as shortcut actions too (no keys by default)
  for (const char* id : {"view.lit", "view.solid", "view.unlit", "view.path_traced", "view.wireframe", "view.seams"})
    if (keyPressed(id)) runPieItem(id);
  // Escape closes the floating windows (help, about, export) unless a text field has focus
  if (ImGui::IsKeyPressed(ImGuiKey_Escape) && !uvXform.active &&
      (showHelp || showAbout || showExport || showKeymap || showMaterialEditor || showPreferences || showDensity || showSymmetry || showAutoTexture || showMacros ||
       showRender || showHighPoly || showWelcome || showPenTest || showBakeDown) && !radialEdit) {
    showHelp = showAbout = showExport = showKeymap = showMaterialEditor = showPreferences = false;
    showDensity = showSymmetry = showAutoTexture = showMacros = showRender = showHighPoly = showWelcome = showPenTest = false;
    showBakeDown = false;
  }
  // tool keys only when the UV editor is not the hovered transform target
  bool uvHovered = false;
  uvHovered = panelHovered("UV Editor", true);
  bool layersHovered = panelHovered("Layers", false);
  if (keyPressed("layer.delete") && d.selected >= 0 && (layersHovered || !uvHovered)) d.removeLayer(d.selected);
  if (uvXform.active || uvHovered) return;
  if (keyPressed("tool.paint")) selectTool(ViewportTool::Paint);
  if (keyPressed("tool.erase")) selectTool(ViewportTool::Erase);
  if (keyPressed("tool.blur")) selectTool(ViewportTool::Blur);
  if (keyPressed("tool.clone")) selectTool(ViewportTool::Clone);
  if (keyPressed("tool.smudge")) selectTool(ViewportTool::Smudge);
  if (keyPressed("tool.fill")) selectTool(ViewportTool::Fill);
  if (keyPressed("tool.select")) selectTool(ViewportTool::SelectFaces);
  if (keyPressed("tool.seams")) selectTool(ViewportTool::Seams);
  if (keyPressed("tool.island")) selectTool(ViewportTool::Island);
  if (keyPressed("tool.note")) selectTool(ViewportTool::Note);
  if (keyPressed("tool.object") && engine.features.on("scene_objects")) {
    // (again: the next of move, turn, scale)
    if (tool == ViewportTool::Object) gizmoMode = (gizmoMode + 1) % 3;
    selectTool(ViewportTool::Object);
  }
  if (engine.features.on("retopology") && d.hasMesh()) {
    if (keyPressed("tool.zone_line")) tool = ViewportTool::ZoneLine, showRetopoWindow();
    if (keyPressed("tool.patch")) tool = ViewportTool::Patch, showRetopoWindow();
    if (keyPressed("tool.flow")) tool = ViewportTool::Flow, showRetopoWindow();
    if (keyPressed("tool.density")) tool = ViewportTool::Density, showRetopo = true;
  }
  bool faceTool = tool == ViewportTool::Island || tool == ViewportTool::SelectFaces;
  if (keyPressed("island.make") && faceTool && !d.selectedFaces().empty()) makeIslandFromSelection();
  auto sel = [&](const char* mode) {
    try {
      engine.execute("select_faces", {{"mode", mode}});
    } catch (std::exception&) {
    }
  };
  if (keyPressed("select.all") && faceTool) sel("all");
  if (keyPressed("select.none") && faceTool) sel("clear");
  if (keyPressed("select.grow", true)) sel("grow");
  if (keyPressed("select.shrink", true)) sel("shrink");
  // (0.37.1) Alt+P unpins here too, not only in the UV editor
  if (engine.features.on("viewport_pins") && d.hasMesh() && keyPressed("uv.unpin")) uvPinSelection(false, false);
  // the picker key: tap = picker tool, hold = quick pick that returns to the previous tool
  for (auto& a : keymap)
    if (!std::strcmp(a.id, "tool.picker") && a.chord) {
      ImGuiKey k = (ImGuiKey)(a.chord & ~ImGuiMod_Mask_);
      if (keyPressed("tool.picker") && !quickPickActive && tool != ViewportTool::Picker) {
        quickPickActive = true;
        quickPickUsed = false;
        quickPickReturn = tool;
        tool = ViewportTool::Picker;
      }
      if (quickPickActive && !ImGui::IsKeyDown(k)) {
        quickPickActive = false;
        if (quickPickUsed || ImGui::GetKeyData(k)->DownDurationPrev > 0.35f) tool = quickPickReturn;
      }
    }
  if (pen.takeUndo()) doUndo();
  if (keyPressed("color.swap") || pen.takeSwapColors()) {
    Painter& P = engine.painter;
    std::swap(P.material.flat.color, secondaryColor);
    if (!P.material.id.empty()) P.material.id.clear(), engine.selectedMaterialId.clear();
    setStatus(T_("Swapped primary and secondary color"));
  }
  if (keyPressed("brush.lock_topology")) {
    engine.painter.lockTopology = !engine.painter.lockTopology;
    setStatus(engine.painter.lockTopology ? T_("Topology lock on: strokes stay on the part they start on") : T_("Topology lock off"));
  }
  if (engine.features.on("macro_recorder")) {
    if (keyPressed("macro.record")) {
      if (engine.macro.recording) engine.macro.recording = false;
      else engine.macro.start(false);
      setStatus(engine.macro.recording ? std::string(T_("Recording a macro: menu actions and commands are kept"))
                                       : F_("Macro stopped: %zu steps", engine.macroSteps().size()));
    }
    if (keyPressed("macro.play") && !engine.macroSteps().empty()) playMacro(json::object());
  } else {
    engine.macro.recording = false;
  }
  if (keyPressed("brush.lock_set") && engine.features.on("texture_set_tools")) {
    engine.painter.lockSet = !engine.painter.lockSet;
    setStatus(engine.painter.lockSet ? T_("Set lock on: strokes stay inside the texture set (material slot) they start on") : T_("Set lock off"));
  }
  if (keyPressed("view.isolate_set") && engine.features.on("texture_set_tools")) {
    engine.viewOptions.isolateSet = !engine.viewOptions.isolateSet;
    setStatus(engine.viewOptions.isolateSet ? T_("Showing only the current texture set") : T_("Showing every texture set"));
  }
  if (keyPressed("view.retopo_preview") && engine.features.on("retopology") && engine.features.on("retopo_preview_toggle")) {
    engine.viewOptions.retopoPreview = !engine.viewOptions.retopoPreview;
    setStatus(engine.viewOptions.retopoPreview ? T_("Retopology preview on") : T_("Retopology preview off"));
  }
  if (keyPressed("brush.lock_island")) {
    engine.painter.lockIsland = !engine.painter.lockIsland;
    setStatus(engine.painter.lockIsland ? T_("Island lock on: strokes stay inside the UV island they start on") : T_("Island lock off"));
  }
  if (keyPressed("sym.x")) {
    engine.painter.symmetry[0] = !engine.painter.symmetry[0];
    setStatus(engine.painter.symmetry[0] ? T_("X mirror on") : T_("X mirror off"));
  }
  if (!io.KeyCtrl && !io.KeyAlt)
    for (int k = 0; k < 9; ++k)
      if (ImGui::IsKeyPressed((ImGuiKey)(ImGuiKey_1 + k)) && k < (int)ViewMode::Count) engine.viewOptions.mode = (ViewMode)k;
}

// ------------------------------------------------------------------ properties (brush + material)

void App::drawProperties() {
  beginPanel(L_("Properties"));
  Painter& P = engine.painter;
  BrushPreset& b = P.brush;
  bool brushOpen = ImGui::CollapsingHeader(L_("Brush"), ImGuiTreeNodeFlags_DefaultOpen);
  {
    ImVec2 ra = ImGui::GetItemRectMin(), rb = ImGui::GetItemRectMax();
    engine.uiRects["props.brush_header"] = {ra.x, ra.y, rb.x - ra.x, rb.y - ra.y};  // (the manual's screenshots)
  }
  if (brushOpen) {
    ImGui::Text(T_("Preset: %s"), engine.selectedBrushId.c_str());
    GLuint prev = engine.brushes.previewTexture(b);
    ImGui::Image(texId(prev), ImVec2(56, 56) * uiScale);
    ImGui::SameLine();
    ImGui::BeginGroup();
    auto shapes = engine.brushes.shapes();
    int si = (int)(std::find(shapes.begin(), shapes.end(), b.alpha.shape) - shapes.begin());
    ImGui::SetNextItemWidth(130 * uiScale);
    if (ImGui::BeginCombo(L_("Tip"), b.alpha.image.empty() ? b.alpha.shape.c_str() : T_("image"))) {
      for (int i = 0; i < (int)shapes.size(); ++i)
        if (ImGui::Selectable(shapes[i].c_str(), i == si)) b.alpha.shape = shapes[i], b.alpha.image.clear();
      ImGui::EndCombo();
    }
    ImGui::SetNextItemWidth(130 * uiScale);
    ImGui::DragFloat(L_("Seed"), &b.alpha.seed, 0.1f, 0.f, 100.f);
    if (ImGui::SmallButton(L_("Image tip..."))) openFileDialog(DialogPurpose::BrushImage);
    ImGui::EndGroup();
    ImGui::SliderFloat(L_("Size"), &b.size, 1.f, 1000.f, T_("%.0f px"), ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat(L_("Hardness"), &b.hardness, 0.f, 1.f);
    ImGui::SliderFloat(L_("Opacity"), &b.opacity, 0.f, 1.f);
    ImGui::SliderFloat(L_("Flow"), &b.flow, 0.01f, 1.f);
    ImGui::SliderFloat(L_("Spacing"), &b.spacing, 0.01f, 3.f, "%.2f", ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat(L_("Angle"), &b.angleDeg, -180.f, 180.f);
    ImGui::SliderFloat(L_("Roundness"), &b.roundness, 0.05f, 1.f);
    if (tool == ViewportTool::Blur) ImGui::SliderFloat(L_("Blur radius"), &engine.painter.blurRadius, 1.f, 32.f, T_("%.0f texels"));
    if (tool == ViewportTool::Blur || tool == ViewportTool::Smudge) {
      ImGui::Checkbox(L_("Across UV seams"), &engine.painter.seamAware);
      helpMarker(T_("Blur and smudge carry on over UV seams onto the matching side of the neighboring island"));
    }
    if (tool == ViewportTool::Clone) {
      ImGui::Checkbox(L_("Aligned clone"), &engine.painter.cloneAligned);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", T_("Aligned: the source keeps its offset across strokes.\nOff: every stroke starts sampling at the source point."));
      if (engine.painter.hasCloneSource)
        ImGui::TextDisabled(T_("Source UV %.3f, %.3f (Ctrl+click to move)"), engine.painter.cloneSourceUV.x, engine.painter.cloneSourceUV.y);
      else
        ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "%s", T_("Ctrl+click on the model or UV view to set the source"));
    }
    if (ImGui::TreeNode(L_("Dynamics"))) {
      ImGui::Checkbox(L_("Follow path"), &b.followPath);
      ImGui::SliderFloat(L_("Angle jitter"), &b.angleJitter, 0.f, 1.f);
      ImGui::SliderFloat(L_("Size jitter"), &b.sizeJitter, 0.f, 1.f);
      ImGui::SliderFloat(L_("Flow jitter"), &b.flowJitter, 0.f, 1.f);
      ImGui::SliderFloat(L_("Scatter"), &b.scatter, 0.f, 3.f);
      ImGui::SliderFloat(L_("Lazy mouse"), &b.lazyRadius, 0.f, 100.f, T_("%.0f px"));
      ImGui::Checkbox(L_("Pressure: size"), &b.pressureSize);
      ImGui::SameLine();
      ImGui::Checkbox(L_("flow"), &b.pressureFlow);
      ImGui::SameLine();
      ImGui::Checkbox(L_("opacity"), &b.pressureOpacity);
      ImGui::Checkbox(L_("Pen tilt turns the tip"), &b.tiltAngle);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", T_("The tip points the way the pen leans (pens with tilt: Wacom Pro Pen, most Huion and XP-Pen pens)"));
      if (ImGui::SmallButton(L_("Pen tablet settings..."))) showPenTest = true;
      ImGui::TreePop();
    }
    if (ImGui::TreeNode(L_("Projection"))) {
      int pj = (int)b.projection;
      if (ImGui::Combo(L_("Mode"), &pj, Z_("Screen (view)\0Surface 3D sphere\0UV space\0"))) b.projection = (BrushProjection)pj;
      ImGui::Checkbox(L_("Backface culling"), &b.backfaceCulling);
      ImGui::Checkbox(L_("Occlusion (visible surfaces only)"), &b.occlusion);
      helpMarker(T_("Off = paint through the model onto hidden surfaces behind the cursor"));
      ImGui::SliderFloat(L_("Angle falloff"), &b.angleFalloff, 0.f, 0.8f);
      helpMarker(T_("Fades paint on surfaces seen at grazing angles (0 = off)"));
      ImGui::TreePop();
    }
    static std::string newId;
    ImGui::SetNextItemWidth(140 * uiScale);
    ImGui::InputTextWithHint("##presetid", T_("preset name"), &newId);
    ImGui::SameLine();
    if (ImGui::Button(L_("Save preset")) && !newId.empty()) {
      BrushPreset p = b;
      p.id = "user_" + paths::sanitizeFileName(newId);
      p.name = newId;
      p.category = "User";
      std::string err;
      if (engine.brushes.saveUserPreset(p, &err)) setStatus(F_("Saved brush preset %s", newId.c_str()));
      else setStatus(err);
    }
  }
  drawStencilSection();
  if (ImGui::CollapsingHeader(L_("Paint material"), ImGuiTreeNodeFlags_DefaultOpen)) {
    MaterialRef& m = P.material;
    auto* e = m.id.empty() ? nullptr : engine.materials.find(m.id);
    if (e) {
      ImGui::Image(texId(engine.materials.thumbnail(*e, engine.env)), ImVec2(56, 56) * uiScale);
      ImGui::SameLine();
      ImGui::BeginGroup();
      ImGui::Text("%s", e->def.name.c_str());
      ImGui::TextDisabled("%s", e->def.category.c_str());
      if (ImGui::SmallButton(L_("Use flat color instead"))) m.id.clear();
      ImGui::EndGroup();
    } else {
      ImGui::TextDisabled("%s", T_("Flat values (pick a library material to paint textures)"));
    }
    if (!e || e->def.type == MaterialType::Flat) {
      colorEdit(L_("Base color"), m.flat.color);
      ImGui::SliderFloat(L_("Roughness"), &m.flat.roughness, 0.f, 1.f);
      ImGui::SliderFloat(L_("Metallic"), &m.flat.metallic, 0.f, 1.f);
      ImGui::SliderFloat(L_("Height"), &m.flat.height, 0.f, 1.f);
      colorEdit(L_("Emissive"), m.flat.emissive);
    }
    int mp = m.mapping;
    if (ImGui::Combo(L_("Projection##mat"), &mp, Z_("Tri-planar (object)\0UV\0Screen (planar)\0"))) m.mapping = mp;
    helpMarker(T_("Tri-planar ignores UV layout entirely; Screen projects the material from the camera like a decal"));
    ImGui::SliderFloat(L_("Scale##mat"), &m.scale, 0.05f, 20.f, "%.2f", ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat(L_("Rotation##mat"), &m.rotationDeg, -180.f, 180.f);
    if (ImGui::TreeNode(L_("Adjust"))) {
      colorEdit(L_("Tint"), m.tint);
      ImGui::SliderFloat(L_("Rough scale"), &m.roughnessScale, 0.f, 2.f);
      ImGui::SliderFloat(L_("Rough offset"), &m.roughnessOffset, -1.f, 1.f);
      ImGui::SliderFloat(L_("Metal override"), &m.metallicOverride, -1.f, 1.f, m.metallicOverride < 0 ? T_("off") : "%.2f");
      ImGui::SliderFloat(L_("Height scale"), &m.heightScale, 0.f, 4.f);
      ImGui::SliderFloat(L_("Height offset"), &m.heightOffset, -0.5f, 0.5f);
      ImGui::SliderFloat(L_("Normal strength"), &m.normalStrength, 0.f, 4.f);
      ImGui::SliderFloat(L_("Emissive strength"), &m.emissiveStrength, 0.f, 4.f);
      ImGui::SliderFloat(L_("Tri-planar blend"), &m.triplanarSharpness, 1.f, 16.f);
      {
        static const char* modes[] = {N_("Smooth cross-fade"), N_("By height (crisp)")};
        const char* shown[] = {T_(modes[0]), T_(modes[1])};
        ImGui::Combo(L_("Tri-planar seams"), &m.triplanarBlend, shown, 2);
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("%s", T_("Where the three projections meet on curved surfaces.\n"
                                     "By height: the raised parts of one projection win, so patterns don't show through\n"
                                     "each other (no ghosting). Smooth: a soft cross-fade, which can show a double pattern."));
      }
      ImGui::TreePop();
    }
  }
  if (ImGui::CollapsingHeader(L_("Paint channels"), ImGuiTreeNodeFlags_DefaultOpen)) {
    for (int c = 0; c < kChannelCount; ++c) {
      if (isExtraChannel(c) && !engine.features.on("more_channels")) continue;
      bool on = P.channelMask & (1 << c);
      if (c % 3) ImGui::SameLine();
      if (ImGui::Checkbox(L_((std::string(channelInfo((Channel)c).label) + "##pc").c_str()), &on))
        P.channelMask = on ? P.channelMask | (1 << c) : P.channelMask & ~(1 << c);
    }
    if (ImGui::SmallButton(L_("All"))) P.channelMask = kAllChannels;
    ImGui::SameLine();
    if (ImGui::SmallButton(L_("Color only"))) P.channelMask = 1;
    ImGui::SameLine();
    if (ImGui::SmallButton(L_("Height only"))) P.channelMask = 8;
    ImGui::SameLine();
    if (ImGui::SmallButton(L_("Rough+Metal"))) P.channelMask = 6;
    ImGui::SetNextItemWidth(120 * uiScale);
    ImGui::Combo(L_("Height mode"), &P.heightMode, Z_("Replace\0Add\0Max\0"));
    if (const Layer* sl = engine.doc.selectedLayer(); sl && sl->isBaked())
      ImGui::SliderFloat(L_("Map value"), &P.maskValue, 0.f, 1.f);  // (0.41) the value a baked map's fixes get
    else if (engine.doc.editMask || (sl && sl->masksOnly()))
      ImGui::SliderFloat(L_("Mask value"), &P.maskValue, 0.f, 1.f);
  }
  endPanel();
}

// ------------------------------------------------------------------ library panels

void App::drawMaterials() {
  beginPanel(L_("Materials"));
  // (0.22.5) the materials, the smart materials and the smart masks, each to click or to drag onto the
  // layer stack
  int tab = 0;
  if (ImGui::BeginTabBar("##assets")) {
    auto item = [&](const char* label, int k, const char* key, const char* tip) {
      bool open = ImGui::BeginTabItem(label);
      ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
      engine.uiRects[key] = {a.x, a.y, b.x - a.x, b.y - a.y};  // (for tests)
      if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
      if (open) tab = k, ImGui::EndTabItem();
    };
    item(L_("Materials"), 0, "materials.tab:materials", T_("Click one to paint with it, or drag it onto a fill layer or into the layer stack."));
    item(L_("Smart materials"), 1, "materials.tab:smart", T_("Ready-made layer stacks. Click one to add it, or drag it into the layer stack."));
    item(L_("Smart masks"), 2, "materials.tab:masks", T_("Masks that follow the model's shape. Click one to give it to the selected layer,\nor drag it onto a layer."));
    ImGui::EndTabBar();
  }
  if (tab == 1) drawSmartMaterialTiles();
  if (tab == 2) drawSmartMaskTiles();
  if (tab != 0) {
    endPanel();
    return;
  }
  ImGui::SetNextItemWidth(160 * uiScale);
  ImGui::InputTextWithHint("##search", T_("search"), &materialFilter);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(140 * uiScale);
  if (ImGui::BeginCombo("##cat", materialCategory.empty() ? T_("All categories") : materialCategory.c_str())) {
    if (ImGui::Selectable(L_("All categories"), materialCategory.empty())) materialCategory.clear();
    for (auto& c : engine.materials.categories())
      if (ImGui::Selectable(c.c_str(), c == materialCategory)) materialCategory = c;
    ImGui::EndCombo();
  }
  ImGui::SameLine();
  if (ImGui::Button(L_("Import folder..."))) openFileDialog(DialogPurpose::ImportMaterial);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", T_("A folder of texture maps: base color, roughness, metallic, normal, height...\n"
                               "Substance exports work too (packed ORM / MaskMap maps, several texture sets, EXR).\n"
                               "You can also drag and drop the folder onto the window."));
  ImGui::SameLine();
  if (ImGui::Button(L_("New procedural..."))) {
    editMaterial = MaterialDef();
    editMaterial.id = "user_material";
    editMaterial.name = T_("My Material");
    editMaterial.category = "User";
    editMaterial.proc.generator = 1;
    editMaterialDirty = true;
    showMaterialEditor = true;
  }
  ImGui::Separator();
  float cell = 84.f * uiScale;
  int cols = std::max(1, int(ImGui::GetContentRegionAvail().x / (cell + 8)));
  ImGui::BeginChild("##matgrid");
  int i = 0;
  int bakesThisFrame = 0;
  std::string filter = materialFilter;
  std::transform(filter.begin(), filter.end(), filter.begin(), ::tolower);
  for (auto& ep : engine.materials.entries()) {
    auto& e = *ep;
    if (e.transient) continue;
    if (!materialCategory.empty() && e.def.category != materialCategory) continue;
    if (!filter.empty()) {
      std::string n = e.def.name + " " + e.def.category + " " + e.def.id + " " + e.def.description;
      for (auto& t : e.def.tags) n += " " + t;
      std::transform(n.begin(), n.end(), n.begin(), ::tolower);
      if (n.find(filter) == std::string::npos) continue;
    }
    if (i % cols) ImGui::SameLine();
    ImGui::PushID(e.def.id.c_str());
    ImGui::BeginGroup();
    GLuint t = e.gpu.thumbnail.id();
    if (!t && bakesThisFrame < 2) {  // spread procedural bakes over frames
      t = engine.materials.thumbnail(e, engine.env);
      ++bakesThisFrame;
    }
    bool sel = engine.painter.material.id == e.def.id;
    if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.78f, 0.44f, 0.14f, 1));
    if (ImGui::ImageButton("##t", texId(t), ImVec2(cell, cell))) engine.selectMaterial(e.def.id);
    if (sel) ImGui::PopStyleColor();
    if (ImGui::IsItemVisible()) {
      ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
      engine.uiRects["materials.tile:" + e.def.id] = {a.x, a.y, b.x - a.x, b.y - a.y};  // (for tests)
    }
    // (0.22.5) dragged onto the layer stack: onto a fill layer it replaces the material, elsewhere it
    // makes a fill layer
    if (engine.features.on("drag_drop") && ImGui::BeginDragDropSource()) {
      ImGui::SetDragDropPayload("PATINA_MATERIAL", e.def.id.c_str(), e.def.id.size() + 1);
      ImGui::Image(texId(t), ImVec2(40, 40) * uiScale);
      ImGui::SameLine();
      ImGui::BeginGroup();
      ImGui::TextUnformatted(e.def.name.c_str());
      ImGui::TextDisabled("%s", dropHint.empty() ? T_("Drop on a fill layer to use it there") : dropHint.c_str());
      ImGui::EndGroup();
      dropHint.clear();
      ImGui::EndDragDropSource();
    }
    if (e.def.builtin) {
      // "P" badge: part of the shipped Patina default library (ids patina_*)
      ImVec2 a = ImGui::GetItemRectMin() + ImVec2(5, 5) * uiScale;
      ImVec2 sz = ImGui::CalcTextSize("P") + ImVec2(6, 2) * uiScale;
      ImDrawList* dl = ImGui::GetWindowDrawList();
      dl->AddRectFilled(a, a + sz, IM_COL32(200, 112, 36, 230), 3.f * uiScale);
      dl->AddText(a + ImVec2(3, 1) * uiScale, IM_COL32(255, 255, 255, 255), "P");
    }
    if (ImGui::IsItemHovered() && !ImGui::GetDragDropPayload())
      ImGui::SetTooltip(T_("%s\n%s  |  %s\n%s\nid: %s\n\nClick to paint with it. Drag it onto a fill layer to use it there."), e.def.name.c_str(),
                        e.def.category.c_str(), e.def.builtin ? T_("Patina default library") : T_("your library"), e.def.description.c_str(),
                        e.def.id.c_str());
    if (ImGui::BeginPopupContextItem()) {
      if (contextMenus()) ImGui::TextDisabled("%s", e.def.name.c_str()), ImGui::Separator();
      if (ImGui::MenuItem(L_("Paint with this"))) engine.selectMaterial(e.def.id);
      if (ImGui::MenuItem(L_("Add as fill layer"))) {
        MaterialRef r = engine.painter.material;
        r.id = e.def.id;
        engine.doc.addFillLayer(r, e.def.name);
      }
      if (contextMenus()) {
        Layer* fl = engine.doc.selectedLayer();
        if (ImGui::MenuItem(L_("Use on the selected fill layer"), nullptr, false, fl && fl->type == LayerType::Fill)) {
          MaterialRef r = fl->material;
          std::string oldName;
          if (auto* oe = engine.materials.find(r.id)) oldName = oe->def.name;
          r.id = e.def.id;
          engine.doc.replaceFillMaterial(engine.doc.selected, r, oldName, e.def.name);
        }
        if (ImGui::MenuItem(L_("Copy its id"))) ImGui::SetClipboardText(e.def.id.c_str());
      }
      if (e.def.type == MaterialType::Procedural && ImGui::MenuItem(L_("Edit / duplicate..."))) {
        editMaterial = e.def;
        if (e.def.builtin) {
          editMaterial.id = "user_" + e.def.id;
          editMaterial.name = F_("%s (custom)", e.def.name.c_str());
          editMaterial.category = "User";
        }
        editMaterialDirty = true;
        showMaterialEditor = true;
      }
      if (e.def.type == MaterialType::Image && !e.def.builtin && e.def.maps.count("normal")) {
        bool dx = e.def.normalDirectX;
        if (ImGui::MenuItem(L_("Normal map is DirectX (green down)"), nullptr, dx)) {
          pendingMaterialSave = std::make_unique<MaterialDef>(e.def);
          pendingMaterialSave->normalDirectX = !dx;
        }
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("%s", T_("Tick when bumps look like dents. Unreal and DirectX exports use this convention."));
      }
      if (!e.def.builtin && ImGui::MenuItem(L_("Delete from library"))) {
        engine.materials.removeUserMaterial(e.def.id);
        ImGui::EndPopup();
        ImGui::EndGroup();
        ImGui::PopID();
        break;
      }
      ImGui::EndPopup();
    }
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + cell);
    ImGui::TextUnformatted(e.def.name.c_str());
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    ImGui::PopID();
    ++i;
  }
  if (contextMenus() && ImGui::BeginPopupContextWindow("##matsctx", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
    materialsEmptyMenu(), ImGui::EndPopup();
  ImGui::EndChild();
  endPanel();
  if (pendingMaterialSave) {
    std::string err;
    std::string id = pendingMaterialSave->id;
    if (engine.materials.saveUserMaterial(*pendingMaterialSave, &err)) {
      if (auto* e = engine.materials.find(id)) engine.materials.rebake(*e), e->gpu.thumbnail = Texture2D();
      engine.markSceneComposite();  // (1.2) every object that fills with it
      setStatus(F_("Updated material %s", pendingMaterialSave->name.c_str()));
    } else {
      showError(T_("Could not update the material"), err);
    }
    pendingMaterialSave.reset();
  }
}

// (0.22.5) the smart materials as tiles: click to add one above the selection, or drag it into the stack
void App::drawSmartMaterialTiles() {
  Document& d = engine.doc;
  ImGui::SetNextItemWidth(160 * uiScale);
  ImGui::InputTextWithHint("##smartsearch", T_("search"), &materialFilter);
  ImGui::SameLine();
  ImGui::TextDisabled("%s", T_("Click to add one above the selected layer, or drag it to where you want it in the stack."));
  ImGui::Separator();
  std::string filter = materialFilter;
  std::transform(filter.begin(), filter.end(), filter.begin(), ::tolower);
  float cell = 84.f * uiScale;
  int cols = std::max(1, int(ImGui::GetContentRegionAvail().x / (cell + 8)));
  ImGui::BeginChild("##smartgrid");
  int i = 0, bakes = 0;
  // (the files read again now and then: one saved shows up)
  if (nowSeconds() - smartListAt > 4.0) smartList = listSmartMaterials(), smartListAt = nowSeconds();
  for (auto& m : smartList) {
    if (!filter.empty()) {
      std::string n = m.name + " " + m.description + " " + m.id;
      std::transform(n.begin(), n.end(), n.begin(), ::tolower);
      if (n.find(filter) == std::string::npos) continue;
    }
    GLuint t = 0;
    if (auto* e = m.tileMaterial.empty() ? nullptr : engine.materials.find(m.tileMaterial)) {
      t = e->gpu.thumbnail.id();
      if (!t && bakes < 2) t = engine.materials.thumbnail(*e, engine.env), ++bakes;
    }
    if (i % cols) ImGui::SameLine();
    ImGui::PushID(m.path.c_str());
    ImGui::BeginGroup();
    bool clicked = t ? ImGui::ImageButton("##t", texId(t), ImVec2(cell, cell)) : ImGui::Button("##t", ImVec2(cell, cell) + ImGui::GetStyle().FramePadding * 2);
    if (ImGui::IsItemVisible()) {
      ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
      engine.uiRects["smart.tile:" + m.id] = {a.x, a.y, b.x - a.x, b.y - a.y};  // (for tests)
    }
    {
      ImVec2 a = ImGui::GetItemRectMin() + ImVec2(5, 5) * uiScale;
      ImVec2 sz = ImGui::CalcTextSize("S") + ImVec2(6, 2) * uiScale;
      ImDrawList* dl = ImGui::GetWindowDrawList();
      dl->AddRectFilled(a, a + sz, m.builtin ? IM_COL32(200, 112, 36, 230) : IM_COL32(60, 140, 190, 230), 3.f * uiScale);
      dl->AddText(a + ImVec2(3, 1) * uiScale, IM_COL32(255, 255, 255, 255), "S");
    }
    if (contextMenus() && ImGui::BeginPopupContextItem("##smartctx")) {
      ImGui::TextDisabled("%s", m.name.c_str());
      ImGui::Separator();
      if (ImGui::MenuItem(L_("Add above the selected layer"))) clicked = true;
      if (ImGui::MenuItem(L_("Copy its id"))) ImGui::SetClipboardText(m.id.c_str());
      ImGui::Separator();
      if (ImGui::MenuItem(L_("Open the smart materials folder"))) {
#ifdef _WIN32
        std::string url = "file:///" + paths::ensureDir(paths::userDataDir() / "smart_materials").generic_string();
#else
        std::string url = "file://" + paths::ensureDir(paths::userDataDir() / "smart_materials").generic_string();
#endif
        SDL_OpenURL(url.c_str());
      }
      ImGui::EndPopup();
    }
    if (clicked) {
      std::string err;
      if (applySmartMaterial(d, m.path, &err) < 0) showError(T_("Could not apply the smart material"), err);
      else setStatus(F_("Added the smart material %s", m.name.c_str()));
    }
    if (engine.features.on("drag_drop") && ImGui::BeginDragDropSource()) {
      ImGui::SetDragDropPayload("PATINA_SMART", m.path.c_str(), m.path.size() + 1);
      if (t) ImGui::Image(texId(t), ImVec2(40, 40) * uiScale), ImGui::SameLine();
      ImGui::BeginGroup();
      ImGui::TextUnformatted(m.name.c_str());
      ImGui::TextDisabled("%s", dropHint.empty() ? T_("Drop it in the layer stack") : dropHint.c_str());
      ImGui::EndGroup();
      dropHint.clear();
      ImGui::EndDragDropSource();
    }
    if (ImGui::IsItemHovered() && !ImGui::GetDragDropPayload())
      ImGui::SetTooltip(T_("%s\n%s%s\n\nClick to add it above the selected layer, or drag it into the layer stack."), m.name.c_str(),
                        m.description.c_str(), m.hasPaint ? T_("\n(includes painted textures)") : "");
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + cell);
    ImGui::TextUnformatted(m.name.c_str());
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    ImGui::PopID();
    ++i;
  }
  ImGui::EndChild();
}

// (0.22.5) the smart masks as tiles: click to give one to the selected layer, or drag it onto a layer
void App::drawSmartMaskTiles() {
  Document& d = engine.doc;
  static const char* kWhat[MaskGenerator::Count] = {
      "",
      N_("The sharp, outward edges: worn paint, scuffs"),
      N_("Creases and dents: dirt and grime gather there"),
      N_("Where light hardly reaches: dust, dark corners"),
      N_("From the bottom up: dirt from the ground, damp"),
      N_("Random patches: breaks up wear and color"),
      N_("Surfaces facing one way (up at first): dust, snow, moss"),
      N_("One material slot from Blender: a part of the model"),
      N_("Thin parts where light shines through: ears, fingers, leaves"),
      N_("Dark to light across the model, the way you choose: fades, two-tone parts"),
      N_("Streaks running down the sides: rain marks, rust runs, leaks"),
  };
  static const char* kName[MaskGenerator::Count] = {"",
                                                    N_("Edges"),
                                                    N_("Cavity"),
                                                    N_("Occlusion"),
                                                    N_("Height gradient"),
                                                    N_("Noise"),
                                                    N_("Facing up"),
                                                    N_("Material slot"),
                                                    N_("Thin parts"),
                                                    N_("Gradient"),
                                                    N_("Drips")};
  static_assert(MaskGenerator::Count == 11, "a name and a line for every smart mask");
  int shown = engine.features.on("more_procedural") ? (int)MaskGenerator::Count : (int)MaskGenerator::Gradient;
  const Layer* sel = d.selectedLayer();
  ImGui::TextDisabled("%s", sel ? F_("Click to give the selected layer (%s) a smart mask, or drag one onto any layer.", sel->name.c_str()).c_str()
                                : T_("Drag a smart mask onto a layer."));
  ImGui::Separator();
  float w = 150.f * uiScale, h = 58.f * uiScale;
  int cols = std::max(1, int(ImGui::GetContentRegionAvail().x / (w + 8)));
  ImGui::BeginChild("##maskgrid");
  for (int t = 1, i = 0; t < shown; ++t, ++i) {
    if (i % cols) ImGui::SameLine();
    ImGui::PushID(t);
    bool give = ImGui::Button(L_(kName[t]), ImVec2(w, h));
    if (contextMenus() && ImGui::BeginPopupContextItem("##smctx")) {
      if (ImGui::MenuItem(L_("Give it to the selected layer"), nullptr, false, sel != nullptr)) give = true;
      if (ImGui::MenuItem(L_("New fill layer with this mask"))) {
        addFillLayerUI(-1);
        if (d.selectedLayer()) d.addSmartMask(d.selected, t), d.editMask = false;
        give = false;
        sel = d.selectedLayer();
      }
      ImGui::EndPopup();
    }
    if (give && sel) d.addSmartMask(d.selected, t), d.editMask = false, setStatus(F_("Smart mask on %s", sel->name.c_str()));
    {
      ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
      engine.uiRects[std::string("smartmask.tile:") + maskGeneratorName(t)] = {a.x, a.y, b.x - a.x, b.y - a.y};  // (for tests)
    }
    if (engine.features.on("drag_drop") && ImGui::BeginDragDropSource()) {
      ImGui::SetDragDropPayload("PATINA_SMARTMASK", &t, sizeof t);
      ImGui::Text(T_("Smart mask: %s"), T_(kName[t]));
      ImGui::TextDisabled("%s", dropHint.empty() ? T_("Drop it on a layer") : dropHint.c_str());
      dropHint.clear();
      ImGui::EndDragDropSource();
    }
    if (ImGui::IsItemHovered() && !ImGui::GetDragDropPayload()) ImGui::SetTooltip(T_("%s\n\nClick: on the selected layer. Drag: onto any layer."), T_(kWhat[t]));
    ImGui::PopID();
  }
  ImGui::EndChild();
}

// the material editor's live sphere and tile, through a temporary library entry
void App::editorPreview(const MaterialDef& m, float size) {
  if (editMaterialDirty) {
    MaterialDef tmp = m;
    tmp.id = "__preview__";
    tmp.type = MaterialType::Procedural;
    editPreviewEntry = engine.materials.upsertTransient(tmp);
    editMaterialDirty = false;
  }
  if (editPreviewEntry) {
    GLuint t = engine.materials.thumbnail(*editPreviewEntry, engine.env);
    ImGui::Image(texId(t), ImVec2(size, size) * uiScale);
    ImGui::SameLine();
    ImGui::Image(texId(editPreviewEntry->gpu.color.id()), ImVec2(size, size) * uiScale);
  }
}

void App::drawMaterialEditor() {
  if (!showMaterialEditor) return;
  ImGui::SetNextWindowSize(ImVec2(440, 760) * uiScale, ImGuiCond_FirstUseEver);
  if (!ImGui::Begin(L_("Material Editor"), &showMaterialEditor)) {
    ImGui::End();
    return;
  }
  MaterialDef& m = editMaterial;
  ProcParams& p = m.proc;
  bool ch = false;
  ImGui::InputText(L_("Id"), &m.id);
  ImGui::InputText(L_("Name"), &m.name);
  ImGui::InputText(L_("Category"), &m.category);
  bool labeled = engine.features.on("more_procedural");
  if (labeled) editorPreview(m, 120.f);  // at the top, where it stays in view
  auto gens = generatorNames();
  const GeneratorInfo& gi = generatorInfo(p.generator);
  if (!labeled) {
    if (ImGui::BeginCombo(L_("Generator"), gens[std::clamp(p.generator, 0, (int)gens.size() - 1)].c_str())) {
      for (int i = 0; i < (int)gens.size(); ++i)
        if (ImGui::Selectable(gens[i].c_str(), i == p.generator)) p.generator = i, ch = true;
      ImGui::EndCombo();
    }
  } else if (ImGui::BeginCombo(L_("Generator"), T_(gi.name), ImGuiComboFlags_HeightLarge)) {
    // grouped, by name
    std::vector<std::string> groups;
    for (int i = 0; i < (int)gens.size(); ++i)
      if (std::find(groups.begin(), groups.end(), generatorInfo(i).group) == groups.end()) groups.push_back(generatorInfo(i).group);
    for (auto& g : groups) {
      ImGui::SeparatorText(T_(g.c_str()));
      for (int i = 0; i < (int)gens.size(); ++i) {
        const GeneratorInfo& o = generatorInfo(i);
        if (g != o.group) continue;
        ImGui::PushID(i);
        if (ImGui::Selectable(L_(o.name), i == p.generator)) p.generator = i, ch = true;
        if (i == p.generator) ImGui::SetItemDefaultFocus();
        ImGui::SetItemTooltip("%s", o.id);
        ImGui::PopID();
      }
    }
    ImGui::EndCombo();
  }
  // what colors A, B and C are for; an unused one is hidden
  std::string colorFor[3] = {L_("Color A"), L_("Color B"), L_("Color C")};
  bool colorUsed[3] = {true, true, true};
  if (labeled) {
    std::string c = T_(gi.colors);  // (a translation keeps the '|'s)
    for (int k = 0; k < 3; ++k) {
      size_t bar = c.find('|');
      std::string part = c.substr(0, bar);
      colorUsed[k] = !part.empty();
      colorFor[k] = part + " (" + char('A' + k) + ")###color";
      c = bar == std::string::npos ? "" : c.substr(bar + 1);
    }
  }
  vec3* cols[3] = {&p.colorA, &p.colorB, &p.colorC};
  for (int k = 0; k < 3; ++k)
    if (colorUsed[k]) {
      ImGui::PushID(k);
      ch |= ImGui::ColorEdit3(colorFor[k].c_str(), &cols[k]->x);
      ImGui::PopID();
    }
  ch |= ImGui::SliderFloat2(L_("Roughness A/B"), &p.roughness.x, 0.f, 1.f);
  ch |= ImGui::SliderFloat2(L_("Metallic A/B"), &p.metallic.x, 0.f, 1.f);
  ch |= ImGui::SliderFloat(L_("Pattern scale"), &p.scale, 1.f, 32.f, "%.0f");
  ch |= ImGui::SliderFloat(L_("Detail"), &p.detail, 1.f, 8.f, "%.0f");
  ch |= ImGui::SliderFloat(L_("Seed"), &p.seed, 0.f, 100.f);
  ch |= ImGui::SliderFloat(L_("Height"), &p.heightAmp, 0.f, 1.f);
  ch |= ImGui::SliderFloat(L_("Variation"), &p.variation, 0.f, 1.f);
  ch |= ImGui::SliderFloat(L_("Normal strength"), &p.normalStrength, 0.f, 4.f);
  if (!labeled) {
    ch |= ImGui::SliderFloat4(L_("Extra"), &p.extra.x, 0.f, 4.f);
    helpMarker(T_("Generator specific: rust/chipped paint x = coverage; tiles x = mortar, y = aspect, z = brick offset;\n"
                  "scratched metal x = scratch count; two-tone x = bias, y = softness"));
  } else {
    bool any = false;
    for (int k = 0; k < 4; ++k) any |= gi.extra[k].label != nullptr;
    if (any) ImGui::SeparatorText(T_(gi.name));
    for (int k = 0; k < 4; ++k) {
      const GeneratorSetting& st = gi.extra[k];
      if (!st.label) continue;
      float& v = p.extra[k];
      ImGui::PushID(100 + k);
      if (st.options) {
        std::vector<std::string> opts;
        std::string o = T_(st.options);  // (a translation keeps the '|'s)
        for (size_t a = 0;;) {
          size_t b = o.find('|', a);
          opts.push_back(o.substr(a, b == std::string::npos ? std::string::npos : b - a));
          if (b == std::string::npos) break;
          a = b + 1;
        }
        int cur = std::clamp((int)std::floor(v + 0.5f), 0, (int)opts.size() - 1);
        if (ImGui::BeginCombo(L_(st.label), opts[cur].c_str())) {
          for (int i = 0; i < (int)opts.size(); ++i)
            if (ImGui::Selectable(opts[i].c_str(), i == cur)) v = (float)i, ch = true;
          ImGui::EndCombo();
        }
      } else {
        // 0 stands for the default where there is one: the slider says so
        char fmt[48];
        if (st.def > 0.f && v == 0.f) std::snprintf(fmt, sizeof fmt, T_("default (%g)"), st.def);
        else std::snprintf(fmt, sizeof fmt, "%%.%df", st.hi - st.lo > 3.f ? 1 : 2);
        ch |= ImGui::SliderFloat(L_(st.label), &v, st.lo, st.hi, fmt);
      }
      if (st.help) ImGui::SetItemTooltip("%s", T_(st.help));
      {
        ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        engine.uiRects["editor.extra:" + std::to_string(k)] = {a.x, a.y, b.x - a.x, b.y - a.y};  // (for tests)
      }
      ImGui::PopID();
    }
    ImGui::SeparatorText(T_("Wear and wobble"));
    ch |= ImGui::SliderFloat(L_("Grime"), &p.grime, 0.f, 1.f);
    ImGui::SetItemTooltip("%s", T_("Dirt settles in the low parts: grout, gaps, grooves, cracks"));
    if (p.grime > 0.f) ch |= ImGui::ColorEdit3(L_("Grime color"), &p.grimeColor.x);
    ch |= ImGui::SliderFloat(L_("Worn edges"), &p.wear, 0.f, 1.f);
    ImGui::SetItemTooltip("%s", T_("Raised edges wear to a lighter color and get a little smoother"));
    if (p.wear > 0.f) ch |= ImGui::ColorEdit3(L_("Edge color"), &p.wearColor.x);
    ch |= ImGui::SliderFloat(L_("Warp"), &p.warp, 0.f, 1.f);
    ImGui::SetItemTooltip("%s", T_("Wobbles the pattern: wavy stripes, uneven bricks, hand-drawn lines"));
    if (p.warp > 0.f) ch |= ImGui::SliderFloat(L_("Warp size"), &p.warpScale, 1.f, 16.f, "%.0f");
    ImGui::Separator();
  }
  if (engine.features.on("more_channels")) {
    ImGui::SeparatorText(T_("Surface"));
    if (generatorHasHoles(p.generator)) {
      ch |= ImGui::Checkbox(L_("Cut out holes"), &p.cutout);
      ImGui::SetItemTooltip("%s", T_("The pattern's holes are see-through (opacity 0): nets, lace, leaves, hair cards, grilles"));
      {
        ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        engine.uiRects["editor.cutout"] = {a.x, a.y, b.x - a.x, b.y - a.y};  // (for tests)
      }
    }
    ch |= ImGui::SliderFloat(L_("Opacity"), &p.opacity, 0.f, 1.f);
    ImGui::SetItemTooltip("%s", T_("1 is solid; lower lets what is behind show through (chiffon, glass, ghosts)"));
    ch |= ImGui::SliderFloat(L_("Sheen"), &p.sheen, 0.f, 1.f);
    ImGui::SetItemTooltip("%s", T_("A soft light at grazing angles: velvet, satin, fur, peach skin"));
    ch |= ImGui::SliderFloat(L_("Clearcoat"), &p.coat, 0.f, 1.f);
    ImGui::SetItemTooltip("%s", T_("A smooth varnish on top: car paint, lacquered wood, nail polish"));
    ch |= ImGui::SliderFloat(L_("Subsurface"), &p.subsurface, 0.f, 1.f);
    ImGui::SetItemTooltip("%s", T_("Light that goes in and comes out softer: skin, wax, leaves, marble"));
  }
  ch |= ImGui::ColorEdit3(L_("Emissive"), &p.emissive.x);
  ch |= ImGui::SliderFloat(L_("Default scale"), &m.defaultScale, 0.05f, 10.f, "%.2f", ImGuiSliderFlags_Logarithmic);
  if (ch) editMaterialDirty = true;  // (the preview at the top catches up next frame)
  if (!engine.features.on("more_procedural")) editorPreview(m, 160.f);
  if (ImGui::Button(L_("Save to library"))) {
    m.type = MaterialType::Procedural;
    std::string err;
    if (auto* e = engine.materials.saveUserMaterial(m, &err)) {
      engine.selectMaterial(e->def.id);
      setStatus(F_("Saved material %s", m.name.c_str()));
    } else {
      setStatus(err);
    }
  }
  ImGui::SameLine();
  if (ImGui::Button(L_("Paint with preview")) && editPreviewEntry) engine.selectMaterial("__preview__");
  ImGui::End();
}

void App::drawBrushes() {
  beginPanel(L_("Brushes"));
  // (1.2) brushes of your own: images or Photoshop .abr files, copied into your brushes folder
  if (ImGui::SmallButton(L_("Import brushes..."))) openFileDialog(DialogPurpose::ImportBrushes);
  ImGui::SetItemTooltip("%s", T_("Brush images (PNG, JPEG, TGA, PSD...) or Photoshop brushes (.abr): each becomes a brush.\n"
                                 "They are copied into your brushes folder. You can also drop them on this panel."));
  ImGui::SameLine();
  if (ImGui::SmallButton(L_("Open the folder"))) openFolder(paths::ensureDir(BrushLibrary::userDir()));
  ImGui::SetItemTooltip("%s", T_("Images and .abr files put in it, or in folders inside it (a group each), become brushes by themselves"));
  float cell = 64.f * uiScale;
  int cols = std::max(1, int(ImGui::GetContentRegionAvail().x / (cell + 10)));
  std::string lastCat;
  int i = 0;
  for (auto& p : engine.brushes.presets()) {
    if (p.category != lastCat) {
      ImGui::SeparatorText(p.category.c_str());
      lastCat = p.category;
      i = 0;
    }
    if (i % cols) ImGui::SameLine();
    ImGui::PushID(p.id.c_str());
    ImGui::BeginGroup();
    bool sel = engine.selectedBrushId == p.id;
    if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.78f, 0.44f, 0.14f, 1));
    if (ImGui::ImageButton("##b", texId(engine.brushes.previewTexture(p)), ImVec2(cell, cell))) engine.selectBrush(p.id);
    if (sel) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", p.name.c_str());
    if (contextMenus() && ImGui::BeginPopupContextItem("##brushctx")) {
      ImGui::TextDisabled("%s", p.name.c_str());
      ImGui::Separator();
      if (ImGui::MenuItem(L_("Paint with this brush"))) engine.selectBrush(p.id);
      if (ImGui::MenuItem(L_("Copy its id"))) ImGui::SetClipboardText(p.id.c_str());
      ImGui::Separator();
      brushesEmptyMenu();
      ImGui::EndPopup();
    }
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + cell);
    ImGui::TextUnformatted(p.name.c_str());
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    ImGui::PopID();
    ++i;
  }
  if (contextMenus() && ImGui::BeginPopupContextWindow("##brushesctx", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems))
    brushesEmptyMenu(), ImGui::EndPopup();
  endPanel();
}

// ------------------------------------------------------------------ texture set / environment

// (0.46) the texture set's resolution and how the model's faces share textures: the Texture Set panel and the Guided
// layout's Model step
void App::textureSetupUI() {
  Document& d = engine.doc;
  const int sizes[] = {512, 1024, 2048, 4096};
  int cur = 2;
  for (int i = 0; i < 4; ++i)
    if (sizes[i] == d.tileRes()) cur = i;
  if (ImGui::Combo(L_("Resolution"), &cur, "512\0001024\0002048\0004096\0")) {
    std::string why;
    if (!Document::atlasFits(sizes[cur], d.mesh.tiles.grid, &why)) {
      setStatus(why);
    } else {
      d.changeResolution(sizes[cur]);
      setStatus(d.mesh.tiles.active() ? F_("Texture set resolution: %d per tile", sizes[cur])
                                      : F_("Texture set resolution: %d", sizes[cur]));
    }
  }
  if (d.mesh.tiles.active() && ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", T_("Each texture set / UDIM tile gets this resolution"));
  // ---- texture tiles: a texture set per material slot, UDIM tiles
  {
    const TileLayout& tl = d.mesh.tiles;
    static const char* modes[] = {N_("One texture"), N_("Per material slot"), N_("UDIM tiles"), N_("Per slot and UDIM")};
    static const char* cmdModes[] = {"single", "material_slots", "udim", "material_slots_udim"};
    const char* shownModes[] = {T_(modes[0]), T_(modes[1]), T_(modes[2]), T_(modes[3])};
    int m = tl.mode & 3;
    ImGui::SetNextItemWidth(ImGui::CalcItemWidth());
    if (ImGui::Combo(L_("Texture sets"), &m, shownModes, 4) && d.hasMesh()) {
      json r;
      if (runCommand("texture_tiles", {{"mode", cmdModes[m]}}, &r))
        setStatus(m == 0   ? std::string(T_("One texture for the whole model"))
                  : m == 2 ? F_("%d UDIM tiles, %d px each", d.mesh.tiles.count(), d.tileRes())
                           : F_("%d texture sets, %d px each", d.mesh.tiles.count(), d.tileRes()));
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", T_("One texture: every face shares one texture.\n"
                                 "Per material slot: each Blender material gets its own textures and its own 0-1 UV space\n"
                                 "(body, clothes, hair...). UDIM tiles: UVs beyond 0-1 (1001, 1002...) each get a texture.\n"
                                 "One layer stack paints all of them; strokes cross from one to the next.\n"
                                 "Switching keeps the paint and clears the undo history."));
    bool hasSel = d.hasMesh() && !d.selectedFaces().empty();
    if (ImGui::Button(L_("New set from selection...")) && d.hasMesh()) openNewTextureSet();
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", hasSel ? T_("A texture of their own for the selected faces, such as one for the head and one for the body")
                                     : T_("Select the faces first (Select tool, or the UV editor), or pick the head in the window that opens"));
    if (tl.active() && engine.features.on("texture_set_tools")) {
      // 0.10: pick the current set; isolate it; keep strokes inside it
      int& cur = engine.uvOptions.tile;
      cur = std::clamp(cur, 0, tl.count() - 1);
      ImGui::SetNextItemWidth(ImGui::CalcItemWidth());
      if (ImGui::BeginCombo(L_("Current set"), tl.tiles[cur].name.c_str())) {
        for (int t = 0; t < tl.count(); ++t)
          if (ImGui::Selectable(tl.tiles[t].name.c_str(), t == cur)) cur = t;
        ImGui::EndCombo();
      }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T_("The texture set the UV editor shows, and the one Isolate keeps"));
      ImGui::Checkbox(L_("Isolate"), &engine.viewOptions.isolateSet);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", T_("Draws the other texture sets as dim ghosts in the viewport, and strokes only paint the current one"));
      ImGui::SameLine();
      ImGui::Checkbox(L_("Set lock"), &engine.painter.lockSet);
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T_("A stroke only paints the texture set it starts on"));
      ImGui::SameLine();
      if (ImGui::SmallButton(L_("Select faces"))) {
        d.faceSelection.assign(d.mesh.faceCount(), 0);
        for (uint32_t f = 0; f < d.mesh.faceCount(); ++f) d.faceSelection[f] = d.mesh.faceTile(f) == cur;
        d.updateSelectionFlags();
        setStatus(F_("Selected the faces of %s", tl.tiles[cur].name.c_str()));
      }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T_("Selects every face of the current set (fill with 'Selection' to fill it)"));
    } else if (tl.active()) {
      std::string names;
      for (int t = 0; t < tl.count() && t < 6; ++t) names += (t ? ", " : "") + tl.tiles[t].name;
      if (tl.count() > 6) names += ", ...";
      ImGui::TextDisabled("%d: %s", tl.count(), names.c_str());
    } else if (d.hasMesh()) {
      static uint64_t sugKey = 0;  // the check rasterizes the UVs: once per layout
      static uint8_t sug = 0;
      uint64_t key = uv::uvHash(d.mesh) ^ d.mesh.faceMaterial.size();
      if (key != sugKey) sugKey = key, sug = Document::detectTileMode(d.mesh);
      if (sug) {
        ImGui::TextColored(ImVec4(1, 0.72f, 0.35f, 1), "%s", (sug & TileBySlot) ? T_("Material slots overlap in UV space")
                                                                               : T_("The UVs use several UDIM tiles"));
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(T_("They share texels now. Choose '%s' above."), T_(modes[sug & 3]));
      }
    }
  }
}

// (0.46) the base material under every layer and the 3D view's lighting: the Texture Set panel and the Guided layout's
// Paint step (its Scene tab)
void App::baseMaterialUI() {
  Document& d = engine.doc;
  bool ch = false;
  ImGui::SeparatorText(T_("Base material (below all layers)"));
  ch |= colorEdit(L_("Base color"), d.tex.base.color);
  ch |= ImGui::SliderFloat(L_("Roughness"), &d.tex.base.roughness, 0.f, 1.f);
  ch |= ImGui::SliderFloat(L_("Metallic"), &d.tex.base.metallic, 0.f, 1.f);
  if (engine.features.on("more_channels")) {
    ch |= ImGui::SliderFloat(L_("Opacity"), &d.tex.base.opacity, 0.f, 1.f);
    ch |= ImGui::SliderFloat(L_("Sheen"), &d.tex.base.sheen, 0.f, 1.f);
    ch |= ImGui::SliderFloat(L_("Clearcoat"), &d.tex.base.coat, 0.f, 1.f);
    ch |= ImGui::SliderFloat(L_("Subsurface"), &d.tex.base.subsurface, 0.f, 1.f);
  }
  ch |= ImGui::SliderFloat(L_("Height->normal"), &d.tex.heightNormalStrength, 0.f, 30.f);
  ch |= ImGui::Checkbox(L_("Dilate seams (gutter fill)"), &d.tex.dilate);
  if (ch) d.markComposite();
}

void App::lightingUI() {
  ImGui::SeparatorText(T_("Lighting"));
  ImGui::SliderFloat(L_("Intensity"), &engine.env.intensity, 0.f, 4.f);
  float rot = glm::degrees(engine.env.rotation);
  if (ImGui::SliderFloat(L_("Rotation"), &rot, -180.f, 180.f)) engine.env.rotation = glm::radians(rot);
  ImGui::SliderFloat(L_("Exposure"), &engine.env.exposure, 0.1f, 4.f);
  ImGui::SliderFloat(L_("Background blur"), &engine.viewOptions.backgroundBlur, 0.f, 5.f);
  if (ImGui::Button(L_("Load HDR..."))) openFileDialog(DialogPurpose::LoadHDR);
  ImGui::SameLine();
  if (ImGui::Button(L_("Studio"))) engine.env.initStudio();
  ImGui::TextDisabled(T_("Environment: %s"), engine.env.name.c_str());
}

void App::drawTextureSet() {
  beginPanel(L_("Texture Set"));
  Document& d = engine.doc;
  textureSetupUI();
  baseMaterialUI();
  ImGui::SeparatorText(T_("Mesh maps"));
  ImGui::SliderInt(L_("AO samples"), &d.aoSamples, 8, 512);
  if (ImGui::Button(L_("Bake AO"))) {
    d.tex.bakeAO(d.mesh, d.gpu, d.aoSamples);
    d.markComposite();
  }
  ImGui::SameLine();
  if (ImGui::Button(L_("Bake curvature"))) {
    d.tex.bakeCurvature(d.mesh, d.gpu, &d.bvh);
    d.markComposite();
  }
  if (ImGui::Button(L_("Bake thickness"))) {
    d.tex.bakeThickness(d.mesh, d.bvh, d.thicknessRays);
    d.markComposite();
  }
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip(T_("How thick the model is under each texel (casts %d rays per texel).\n"
                         "Thin parts (ears, fins, cloth) for the Thin parts smart mask, subsurface and translucency."),
                      d.thicknessRays);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(90 * uiScale);
  ImGui::SliderInt(L_("rays##thick"), &d.thicknessRays, 8, 128);
  ImGui::TextDisabled(T_("AO + bent normals: %s   Curvature: %s%s   Thickness: %s"), d.tex.hasAO() ? T_("baked") : "-",
                      d.tex.hasCurvature() ? T_("baked") : "-", d.tex.curvatureHasDetail() ? T_(" (+ normal map)") : "",
                      d.tex.hasThickness() ? T_("baked") : "-");
  if (ImGui::Button(L_("Bake from high-poly..."))) showHighPoly = true;
  if (d.tex.hasHighNormal()) {
    ImGui::SameLine();
    ImGui::TextDisabled(T_("normal detail from %s"), d.highPolyPath.empty() ? T_("a high-poly mesh") : fs::path(d.highPolyPath).filename().string().c_str());
  }
  lightingUI();
  ImGui::SeparatorText(T_("Mesh"));
  if (d.hasMesh()) {
    ImGui::Text("%s", d.mesh.name.c_str());
    ImGui::Text(T_("%zu verts  %u faces  %zu tris  %u UV islands"), d.mesh.positions.size(), d.mesh.faceCount(), d.mesh.tris.size(),
                d.mesh.islandCount);
    ImGui::TextDisabled(T_("Undo memory: %.0f MB"), d.undo.bytes() / 1048576.0);
  }
  ImGui::End();
}

// ------------------------------------------------------------------ Blender

void App::drawBlender() {
  beginPanel(L_("Blender Link"));
  Document& d = engine.doc;
  ImGui::Text(T_("Command server: %s"), serverPort ? ("127.0.0.1:" + std::to_string(serverPort)).c_str() : T_("off"));
  ImGui::Text(T_("Connections: %d"), server.connectionCount());
  ImGui::Separator();
  if (d.bridge.active) {
    ImGui::Text(T_("Linked object: %s"), d.bridge.object.c_str());
    ImGui::Text(T_("Blender port: %d"), d.bridge.blenderPort);
    ImGui::TextWrapped(T_("Exchange: %s"), d.bridge.exchangeDir.c_str());
    if (d.bridge.uvsEdited) ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "%s", T_("UVs edited - they will be sent back"));
    if (d.bridge.slotsEdited)
      ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "%s", T_("Texture sets changed - Blender gets the material slots with the next send"));
    if (ImGui::Button(L_("Send textures to Blender"), ImVec2(-1, 0))) {
      std::string err;
      if (engine.pushToBlender(&err)) setStatus(T_("Sent textures to Blender"));
      else setStatus(F_("Send failed: %s", err.c_str()));
    }
    ImGui::Checkbox(L_("Auto-send after changes"), &engine.autoPushToBlender);
    if (ImGui::Button(L_("Unlink"))) d.bridge.active = false;
  } else {
    ImGui::TextWrapped(
        "%s", T_("Not linked. In Blender: install the Patina Bridge add-on (blender/patina_bridge), then press 'Send to "
                 "Patina' in the 3D view sidebar (N panel > Patina). The mesh opens here with its UVs; painted textures and "
                 "UV edits flow back with 'Send textures to Blender'."));
  }
  if (engine.features.on("blender_library")) {
    ImGui::SeparatorText(T_("Materials in Blender"));
    ImGui::TextWrapped("%s", T_("Every Patina material in Blender's Asset Browser, ready to drag onto objects."));
    ImGui::SetNextItemWidth(110 * uiScale);
    ImGui::Combo(L_("Map size"), &blenderLibSize, "512\0" "1024\0" "2048\0");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150 * uiScale);
    ImGui::Combo(L_("Mapping"), &blenderLibMapping, Z_("Box (like Patina)\0UV\0"));
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", T_("Box: projected from the sides, top and front, so no UVs are needed (tiles per meter).\n"
                                 "UV: follows the object's UV map, with Patina's normal maps."));
    if (ImGui::Button(L_("Add Patina's materials to Blender"), ImVec2(-1, 0))) {
      static const int sizes[] = {512, 1024, 2048};
      try {
        json r = engine.execute("blender_material_library", {{"size", sizes[blenderLibSize]},
                                                              {"mapping", blenderLibMapping ? "uv" : "box"}});
        blenderLibStatus = F_("Blender is building the library in %s. It shows in the Asset Browser as Patina Materials.",
                              r.value("directory", std::string("Documents")).c_str());
      } catch (std::exception& ex) {
        blenderLibStatus = ex.what();
      }
      setStatus(blenderLibStatus);
    }
    {
      ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
      engine.uiRects["blender.material_library"] = {a.x, a.y, b.x - a.x, b.y - a.y};  // (for tests)
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", T_("Blender needs the Patina Bridge add-on 0.8 or newer with its live link on.\n"
                                 "Run it again to update the library (new materials, your own materials)."));
    if (!blenderLibStatus.empty()) ImGui::TextWrapped("%s", blenderLibStatus.c_str());
  }
  ImGui::Separator();
  ImGui::SeparatorText(T_("Claude"));
  {
    // whether the Claude desktop app starts this Patina (checked now and then: cheap file read)
    static double lastCheck = -10;
    static bool has = false;
    static std::string result;
    double now = ImGui::GetTime();
    if (now - lastCheck > 5) has = mcp::claudeDesktopHasPatina(), lastCheck = now;
    if (has) ImGui::TextColored(ImVec4(0.5f, 0.85f, 0.5f, 1), "%s", T_("The Claude app can drive this window."));
    else ImGui::TextWrapped("%s", T_("Let Claude in the desktop app paint, unwrap and export here (MCP)."));
    if (ImGui::Button(has ? L_("Set up again") : L_("Connect to Claude app"))) {
      auto r = mcp::setupClaudeDesktop();
      result = r.message;
      setStatus(r.ok ? std::string(T_("Patina added to the Claude app: restart the app")) : r.message);
      lastCheck = -10;
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip(T_("Adds Patina to the MCP servers in the Claude app's claude_desktop_config.json\n"
                           "(a backup of the old file is kept). Command: %s --mcp\n"
                           "Claude Code: claude mcp add patina -- <that command> --mcp"),
                        mcp::mcpCommand().string().c_str());
    if (!result.empty()) ImGui::TextWrapped("%s", result.c_str());
  }
  ImGui::End();
}

// ------------------------------------------------------------------ console

void App::drawConsole() {
  ImGui::Begin(L_("Console"));
  ImGui::BeginChild("##log", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()), ImGuiChildFlags_None);
  static uint64_t lastCount = 0;
  for (auto& e : Log::snapshot(400)) {
    ImVec4 col = e.level == LogLevel::Error ? ImVec4(1, 0.4f, 0.4f, 1)
               : e.level == LogLevel::Warn  ? ImVec4(1, 0.8f, 0.4f, 1)
                                            : ImVec4(0.8f, 0.8f, 0.82f, 1);
    ImGui::TextColored(col, "%s", e.text.c_str());
  }
  if (Log::counter() != lastCount) {
    ImGui::SetScrollHereY(1.f);
    lastCount = Log::counter();
  }
  if (contextMenus() && ImGui::BeginPopupContextWindow("##logctx")) consoleContextMenu(), ImGui::EndPopup();
  ImGui::EndChild();
  ImGui::SetNextItemWidth(-1);
  if (ImGui::InputTextWithHint("##cmd", T_("command {json args}   e.g.  set_brush {\"size\": 50}   |   list_commands"), &consoleInput,
                               ImGuiInputTextFlags_EnterReturnsTrue)) {
    std::string line = consoleInput;
    consoleInput.clear();
    auto sp = line.find(' ');
    std::string cmd = line.substr(0, sp);
    json args = json::object();
    try {
      if (sp != std::string::npos) args = json::parse(line.substr(sp + 1));
      json r = engine.execute(cmd, args);
      std::string s = r.dump(1);
      if (s.size() > 4000) s = s.substr(0, 4000) + "...";
      PLOG_I("%s -> %s", cmd.c_str(), s.c_str());
    } catch (std::exception& e) {
      PLOG_E("%s: %s", cmd.c_str(), e.what());
    }
    ImGui::SetKeyboardFocusHere(-1);
  }
  ImGui::End();
}

// ------------------------------------------------------------------ export dialog

void App::drawExportDialog() {
  if (!showExport) return;
  ImGui::SetNextWindowSize(ImVec2(600, 660) * uiScale, ImGuiCond_FirstUseEver);  // (0.47: room for the files' list)
  if (!ImGui::Begin(L_("Export Textures"), &showExport)) {
    ImGui::End();
    return;
  }
  Document& d = engine.doc;
  if (exportDir.empty()) exportDir = (paths::homeDir() / "patina_export").string();
  ImGui::InputText(L_("Folder"), &exportDir);
  ImGui::SameLine();
  if (ImGui::Button("...")) openFileDialog(DialogPurpose::ExportDir);
  ImGui::InputTextWithHint("$mesh", d.mesh.name.c_str(), &exportBaseName);
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T_("Value of $mesh in the file names (empty = the mesh name)"));
  ImGui::Combo(L_("Preset"), &exportPresetIdx, Z_("Blender (Principled BSDF)\0glTF (metallicRoughness)\0Unreal (ORM, DirectX normal)\0Unity (MaskMap)\0"));
  ImGui::Combo(L_("Format"), &exportFormatIdx, Z_("PNG\0Layered PSD (Photoshop, Krita, GIMP)\0"));
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", T_("Layered PSD: one file per map (base color, roughness, metallic, height, normal, emission),\n"
                               "with a layer per Patina layer, its mask, opacity and blend mode. At the texture set size.\n"
                               "Texture sets and UDIM tiles get a set of files each, named like the PNG maps."));
  ImGui::BeginDisabled(exportFormatIdx == 1);
  ImGui::Combo(L_("Size"), &exportSizeIdx, Z_("Texture set\0" "512\0" "1024\0" "2048\0" "4096\0" "8192\0" "16384\0" "32768\0"));
  ImGui::EndDisabled();
  {
    const int szs[] = {0, 512, 1024, 2048, 4096, 8192, 16384, 32768};
    int want = szs[std::clamp(exportSizeIdx, 0, 7)];
    if (exportFormatIdx == 0 && want > d.tileRes())
      ImGui::TextDisabled(T_("Above the texture set (%d): rendered in tiles, materials and smart masks at full detail;\n"
                             "painted layers are scaled up. %s"), d.tileRes(),
                          want >= 16384 ? T_("Large: this takes a while and needs room on disk.") : "");
  }
  ImGui::Checkbox(L_("AO"), &exportAO);
  ImGui::SameLine();
  ImGui::BeginDisabled(!d.tex.hasThickness());
  ImGui::Checkbox(L_("Thickness"), &exportThickness);
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::BeginDisabled(!d.tex.hasBentNormals());
  ImGui::Checkbox(L_("Bent normals"), &exportBent);
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T_("Baked maps are written when they are baked (Bake menu)"));
  Layer* sel = d.selectedLayer();
  std::string selName = sel ? sel->name : std::string(T_("(none)"));
  std::string whatItems = std::string(T_("All layers")) + '\0' + F_("Only the selected layer: %s", selName.c_str()) + '\0' +
                          T_("The selected layer's mask") + '\0' + '\0';
  ImGui::Combo(L_("Export##what"), &exportWhat, whatItems.c_str());  // (not "Export": the button below)
  if (exportWhat == 1) ImGui::TextDisabled("%s", T_("Exports the maps as they look with this layer (or folder) soloed."));
  if (exportWhat == 2 && (!sel || !sel->mask)) ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "%s", T_("The selected layer has no painted mask."));
  // (1.2) a scene: the active object's maps, or every object's (each set named after its object)
  if (engine.features.on("scene_objects") && engine.objectCount() > 1 && exportWhat == 0) {
    int all = exportAllObjects ? 1 : 0;
    ImGui::RadioButton((F_("The maps of %s", d.displayName().c_str()) + "###texexp_one").c_str(), &all, 0);
    ImGui::SameLine();
    ImGui::RadioButton((F_("Every object's (%d)", engine.objectCount()) + "###texexp_all").c_str(), &all, 1);
    ImGui::SetItemTooltip("%s", T_("A set of maps for each object shown, named after it ($mesh is the object's name)"));
    exportAllObjects = all == 1;
  }
  ImGui::InputText(L_("File names"), &exportTemplate);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", T_("Tokens: $mesh  $project  $map  $size  $preset  $layer\nExample: T_$mesh_$map  or  $project/$layer_$map is not allowed (no folders)"));
  ImGui::SameLine();
  if (ImGui::SmallButton(L_("default"))) exportTemplate = "$mesh_$map";
  {
    // live preview of the first file name
    const int sz[] = {0, 512, 1024, 2048, 4096, 8192, 16384, 32768};
    std::map<std::string, std::string> vars = {
        {"mesh", paths::sanitizeFileName(exportBaseName.empty() ? d.mesh.name : exportBaseName)},
        {"project", d.projectPath.empty() ? std::string("untitled") : fs::path(d.projectPath).stem().string()},
        {"map", exportWhat == 2 ? "mask" : exportPresetIdx == 0 ? "basecolor" : exportPresetIdx == 1 ? "baseColor"
                                         : exportPresetIdx == 2 ? "BaseColor" : "BaseMap"},
        {"size", std::to_string(sz[exportSizeIdx] ? sz[exportSizeIdx] : d.tileRes())},
        {"preset", exportPresetName((ExportPreset)exportPresetIdx)},
        {"layer", exportWhat && sel ? paths::sanitizeFileName(sel->name) : std::string("all")}};
    ImGui::TextDisabled(T_("e.g. %s.%s"), paths::sanitizeFileName(expandExportName(exportTemplate, vars)).c_str(),
                        exportFormatIdx == 1 ? "psd" : "png");
  }
  ImGui::Separator();
  exportPlanUI(0);
  if (ImGui::Button(L_("Export"), ImVec2(120 * uiScale, 0)) && exportTexturesNow()) showExport = false;
  ImGui::End();
}

ExportOptions App::exportOptions() {
  Document& d = engine.doc;
  if (exportDir.empty()) exportDir = (paths::homeDir() / "patina_export").string();
  Layer* sel = d.selectedLayer();
  ExportOptions o;
  o.directory = exportDir;
  o.baseName = exportBaseName;
  o.preset = (ExportPreset)exportPresetIdx;
  const int sz[] = {0, 512, 1024, 2048, 4096, 8192, 16384, 32768};
  o.size = exportFormatIdx == 1 ? 0 : sz[std::clamp(exportSizeIdx, 0, 7)];
  o.psd = exportFormatIdx == 1;
  o.includeAO = exportAO;
  o.includeThickness = exportThickness;
  o.includeBentNormal = exportBent;
  o.nameTemplate = exportTemplate;
  if (exportWhat && sel) o.layerUid = sel->uid;
  o.maskOnly = exportWhat == 2;
  return o;
}

// (0.47) the files an export would write: name, what is in them, bits, color space (the dialog, the Export step)
void App::exportPlanUI(int maxRows) {
  Document& d = engine.doc;
  if (!d.hasMesh()) return;
  ExportOptions o = exportOptions();
  std::vector<ExportPlanFile> plan = d.exportPlan(o);
  if (plan.empty()) return;
  // with texture sets / UDIM tiles: the first one's files, and how many sets more
  const TileLayout& tl = d.mesh.tiles;
  int sets = 1;
  if (tl.active()) {
    std::vector<ExportPlanFile> first;
    for (auto& f : plan)
      if (f.key.size() > 2 && f.key.compare(f.key.size() - 2, 2, "@0") == 0) first.push_back(f);
    if (!first.empty()) sets = (int)(plan.size() / first.size()), plan.swap(first);
  }
  ImGui::TextDisabled(P_("It writes %d file:", "It writes %d files:", (long)(plan.size() * sets)), (int)(plan.size() * sets));
  if (ImGui::BeginTable("##export_plan", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerH)) {
    ImGui::TableSetupColumn(T_("File"), ImGuiTableColumnFlags_WidthStretch, 1.4f);
    ImGui::TableSetupColumn(T_("What is in it"), ImGuiTableColumnFlags_WidthStretch, 1.4f);
    ImGui::TableSetupColumn(T_("Kind"), ImGuiTableColumnFlags_WidthStretch, 0.7f);
    ImGui::TableHeadersRow();
    int row = 0;
    for (auto& f : plan) {
      if (maxRows > 0 && row++ >= maxRows) break;
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      std::string name = fs::path(f.file).filename().string();
      ImGui::TextUnformatted(name.c_str());
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", f.file.c_str());
      ImGui::TableNextColumn();
      ImGui::PushTextWrapPos(0.f);
      ImGui::TextUnformatted(f.contents.c_str());
      ImGui::PopTextWrapPos();
      ImGui::TableNextColumn();
      const char* chans = f.channels == 1 ? T_("gray") : f.channels == 4 ? "RGBA" : "RGB";
      ImGui::Text("%s %d", chans, f.bits);
      if (ImGui::IsItemHovered()) ImGui::SetTooltip(T_("%d bits a channel"), f.bits);
      ImGui::TextDisabled("%s", f.srgb ? T_("sRGB") : T_("Non-Color"));
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", f.srgb ? T_("A color map: Blender's Color Space sRGB, an engine's sRGB on")
                                       : T_("A data map: Blender's Color Space Non-Color, an engine's sRGB off"));
    }
    ImGui::EndTable();
  }
  if (maxRows > 0 && (int)plan.size() > maxRows) ImGui::TextDisabled(T_("... and %d more"), (int)plan.size() - maxRows);
  if (sets > 1) ImGui::TextDisabled(P_("The same for each texture set (%d set)", "The same for each texture set (%d sets)", (long)sets), sets);
  ImGui::TextDisabled(T_("Size: %d x %d. Files that are there are replaced."), plan[0].size, plan[0].size);
}

bool App::exportTexturesNow() {
  Document& d = engine.doc;
  Layer* sel = d.selectedLayer();
  ExportOptions o = exportOptions();
  std::map<std::string, std::string> files;
  std::string err;
  d.updateComposite(engine.materials);
  if (exportWhat && !sel) {
    showError(T_("Nothing to export"), T_("Select a layer first."));
    return false;
  }
  const bool all = exportAllObjects && !exportWhat && engine.objectCount() > 1 && engine.features.on("scene_objects");
  if (!engine.exportSceneTextures(o, all, &files, nullptr, &err)) {
    showError(T_("The textures were NOT exported"), err);
    return false;
  }
  setStatus(F_("Exported %zu file(s) to %s", files.size(), exportDir.c_str()));
  saveSession(true);
  return true;
}

// ------------------------------------------------------------------ help / status

void App::drawHelp() {
  if (!showHelp) return;
  ImGui::SetNextWindowSize(ImVec2(820, 640) * uiScale, ImGuiCond_FirstUseEver);
  if (!ImGui::Begin(L_("Patina Help"), &showHelp)) {
    ImGui::End();
    return;
  }
  ImGui::Text(T_("Keymap: %s"), keymapPresetName(keymapPreset));
  ImGui::SameLine();
  if (ImGui::SmallButton(keymapPreset == KeymapPreset::Blender ? L_("Switch to the Patina keymap") : L_("Switch to the Blender keymap")))
    applyKeymapPreset(keymapPreset == KeymapPreset::Blender ? KeymapPreset::Patina : KeymapPreset::Blender, true);
  ImGui::SameLine();
  if (ImGui::SmallButton(L_("Edit..."))) showKeymap = true;
  if (ImGui::CollapsingHeader(L_("All keyboard shortcuts"), ImGuiTreeNodeFlags_DefaultOpen)) {
    if (ImGui::BeginTable("##allkeys", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
      const char* group = "";
      for (auto& a : keymap) {
        if (!a.chord) continue;
        if (std::strcmp(group, a.group)) {
          group = a.group;
          ImGui::TableNextRow();
          ImGui::TableSetColumnIndex(0);
          ImGui::TextDisabled("%s", T_(group));
        }
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextUnformatted(T_(a.label));
        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(chordName(a.chord, true).c_str());
      }
      ImGui::EndTable();
    }
  }
  ImGui::TextDisabled("%s", T_("Every shortcut can be changed in Edit > Keyboard shortcuts."));
  if (engine.features.on("slider_typing"))
    ImGui::TextDisabled("%s", T_("Right-click any slider to type its value (Enter keeps it, Esc goes back)."));
  ImGui::SeparatorText(T_("Navigation (3D view)"));
  ImGui::BulletText("%s", T_(navPresetName(navPreset)));
  ImGui::BulletText("%s", T_("Other presets (Blender, Maya, ZBrush style) in View > Navigation; the wheel always zooms"));
  ImGui::BulletText("%s", T_("Numpad 1/3/7 front/right/top (Ctrl = opposite), Numpad 5 ortho; frame the mesh: see the table"));
  ImGui::SeparatorText(T_("Painting"));
  ImGui::BulletText("%s", T_("Tools: see the table above. Clone: Ctrl+click sets the source"));
  ImGui::BulletText("%s", T_("Ctrl + wheel: brush size, Ctrl + RMB drag: size (x) / hardness (y)"));
  ImGui::BulletText("%s", T_("Shift + click: straight line from the last stroke    Radial symmetry: tool strip"));
  ImGui::BulletText(T_("Topology lock (%s) / Island lock (%s) on the brush bar: strokes stay on the piece / UV island they start on"),
                    keyLabel("brush.lock_topology") ? keyLabel("brush.lock_topology") : "-",
                    keyLabel("brush.lock_island") ? keyLabel("brush.lock_island") : "-");
  ImGui::BulletText(T_("Stencil: hold %s and drag to move, Shift+drag to rotate, wheel to scale; %s turns it on / off"),
                    keyLabel("stencil.edit") ? keyLabel("stencil.edit") : "-",
                    keyLabel("stencil.toggle") ? keyLabel("stencil.toggle") : "-");
  ImGui::BulletText(T_("Compare view: %s on / off, %s takes a snapshot; drag the divider"),
                    keyLabel("view.compare") ? keyLabel("view.compare") : "-",
                    keyLabel("view.compare_snapshot") ? keyLabel("view.compare_snapshot") : "-");
  if (engine.features.on("view_pie") && engine.features.on("pie_menus"))
    ImGui::BulletText(T_("Pie menus over the 3D view (hold, point, let go): %s view, %s channels, %s edit modes, %s Quick Favorites"),
                      keyLabel("view.pie") ? keyLabel("view.pie") : "-", keyLabel("view.pie_channels") ? keyLabel("view.pie_channels") : "-",
                      keyLabel("tool.pie") ? keyLabel("tool.pie") : "-", keyLabel("app.favorites") ? keyLabel("app.favorites") : "-");
  ImGui::BulletText(T_("Notes (%s): click the model to pin a note, click a pin to edit it"),
                    keyLabel("tool.note") ? keyLabel("tool.note") : "-");
  if (keymapPreset == KeymapPreset::Blender)
    ImGui::BulletText("%s", T_("S samples the color under the cursor. F / Shift+F: move the mouse to set brush size / opacity, "
                               "G / Shift+G flow / hardness, Ctrl+F the angle; click to confirm"));
  else
    ImGui::BulletText("%s", T_("Hold I over the model: quick color pick, release to keep painting. Alt + right-click picks too"));
  if (ctrlPaintsSecondary) ImGui::BulletText("%s", T_("Hold Ctrl while painting to paint with the secondary color"));
  ImGui::BulletText("%s", T_("Colors panel: primary / secondary color, recent colors and swatches"));
  ImGui::BulletText("%s", T_("Fill tool: click fills, Alt + click erases"));
  ImGui::BulletText("%s", T_("Paint layers store paint; fill layers cover the mesh with a material, shaped by a mask"));
  ImGui::BulletText("%s", T_("Adjustment layers shift hue / saturation / brightness / contrast and roughness / metallic / height below them"));
  ImGui::BulletText("%s", T_("Click 'Mask' on a layer to paint its mask; smart masks use baked AO/curvature"));
  ImGui::BulletText("%s", T_("Folders: + Folder, or group the selection; drag layers onto a folder. Alt+click an eye: solo"));
  ImGui::BulletText("%s", T_("Smart... adds a ready-made smart material; right-click a layer to save one, copy/paste settings, export it"));
  ImGui::BulletText("%s", T_("Every change is in the History panel (sliders too); click a step to go back to it"));
  ImGui::BulletText("%s", T_("Materials are projected tri-planar by default: paint looks right regardless of UVs"));
  ImGui::BulletText("%s", T_("Pen tablets: pressure (size / flow / opacity in Dynamics), eraser end, tilt; Help > Pen Tablet Test"));
  ImGui::SeparatorText(T_("UV Editor"));
  ImGui::BulletText("%s", T_("Click select, Shift add, Ctrl remove, drag empty space for box select"));
  ImGui::BulletText("%s", T_("Drag a selection to move it; G/R/S move/rotate/scale (Ctrl snaps); A select all"));
  ImGui::BulletText("%s", T_("Unwrap / Pack / Seams menus; existing paint is reprojected onto edited UVs"));
  ImGui::BulletText("%s", T_("Seams tool (M): drag along the model to lay a seam, Shift+click continues it, Ctrl clears"));
  ImGui::BulletText("%s", T_("UV > Unwrap > Cylinder / Sphere Project for limbs and balls; UV > Relax Islands removes stretch in place"));
  ImGui::BulletText("%s", T_("Selection combo: Faces / Islands / Vertices; More > Weld / stitch and Align work on UV vertices"));
  ImGui::BulletText("%s", T_("Paint mode paints directly in texture space with the current brush"));
  ImGui::SeparatorText(T_("Files"));
  ImGui::BulletText("%s", T_("Drop meshes (.obj .gltf .glb .fbx .stl .3mf .ply), projects (.patina), material folders, .hdr or brush images"));
  ImGui::BulletText("%s", T_("1-9 view modes; File > Open Recent; Edit > Preferences: keymap, navigation, UI scale"));
  if (engine.features.on("command_palette"))
    ImGui::BulletText(T_("%s: command palette, type to find any action, window, material, brush, layer or file"),
                      keyLabel("app.palette") ? keyLabel("app.palette") : "Ctrl+Shift+P");
  ImGui::BulletText("%s", T_("Edit > Preferences > Features switches newer features (palette, symmetry, macros...) on or off"));
  if (engine.features.on("symmetry_check"))
    ImGui::BulletText("%s", T_("UV > Mirrored Halves > Check Mesh Symmetry: how far the halves of the model differ, with a heat map"));
  if (engine.features.on("macro_recorder"))
    ImGui::BulletText("%s", T_("Edit > Record Macro: replay what you did on another model, or save it for 'patina run'"));
  ImGui::BulletText("%s", T_("Save Incremental keeps numbered versions; Export Again repeats the last export; F12 renders an image"));
  ImGui::BulletText("%s", T_("A changed mesh file on disk can be reloaded with the layers kept (File > Reload Mesh)"));
  ImGui::SeparatorText(T_("Island brush and texel density"));
  ImGui::BulletText(T_("Island tool (L): paint a part of the model, then %s: seams, unwrap, upright, right density, free space"),
                    keymapPreset == KeymapPreset::Blender ? "U" : "Enter");
  ImGui::BulletText("%s", T_("Ctrl removes from the painted area; Ctrl+Numpad +/- grows / shrinks; A all, Alt+A none"));
  ImGui::BulletText("%s", T_("UV > Islands > Texel Density: targets (fit, 1 texel per pixel, layout, custom) and heat maps"));
  ImGui::BulletText("%s", T_("Shift + right drag in the viewport rotates the lighting"));
  ImGui::SeparatorText(T_("Automation"));
  ImGui::TextWrapped(T_("Command server on 127.0.0.1:%d (JSON lines). Blender add-on and 'patina --mcp' use it. "
                        "The Console accepts the same commands, e.g.  list_commands"), serverPort);
  ImGui::End();
}

void App::drawStatusBar() {
  ImGuiViewport* vp = ImGui::GetMainViewport();
  float h = ImGui::GetFrameHeight();
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y - h));
  ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, h));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 2));
  ImGui::Begin("##status", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
  ImGui::PopStyleVar();
  Document& d = engine.doc;
  ImGui::Text("%s%s", d.projectPath.empty() ? T_("untitled") : fs::path(d.projectPath).filename().string().c_str(),
              engine.sceneDirty() ? " *" : "");
  ImGui::SameLine(0, 20);
  if (engine.objectCount() > 1) {  // (1.2) the object painted
    ImGui::TextColored(ImVec4(0.95f, 0.66f, 0.35f, 1), T_("%s (%d of %d objects)"), d.displayName().c_str(), engine.activeObject() + 1,
                       engine.objectCount());
    ImGui::SameLine(0, 20);
  }
  if (d.mesh.tiles.active())
    ImGui::TextDisabled((d.mesh.tiles.mode & TileBySlot) ? T_("%dpx x %d sets | %zu layers | %.0f fps")
                                                         : T_("%dpx x %d tiles | %zu layers | %.0f fps"),
                        d.tileRes(), d.mesh.tiles.count(), d.layers.size(), ImGui::GetIO().Framerate);
  else
    ImGui::TextDisabled(T_("%dpx | %zu layers | %.0f fps"), d.tileRes(), d.layers.size(), ImGui::GetIO().Framerate);
  ImGui::SameLine(0, 20);
  if (d.bridge.active) ImGui::TextColored(ImVec4(0.5f, 0.8f, 1, 1), T_("Blender: %s"), d.bridge.object.c_str());
  else ImGui::TextDisabled("%s", T_("Blender: not linked"));
  if (engine.macro.recording) {
    ImGui::SameLine(0, 20);
    bool blink = std::fmod(nowSeconds(), 1.0) < 0.6;
    ImGui::TextColored(ImVec4(1, blink ? 0.3f : 0.55f, 0.3f, 1), T_("REC macro: %zu steps"), engine.macroSteps().size());
    if (ImGui::IsItemClicked()) showMacros = true;
  }
  if (!statusText.empty() && nowSeconds() - statusTime < 8.0) {
    ImGui::SameLine(0, 20);
    ImGui::TextColored(ImVec4(1, 0.8f, 0.5f, 1), "%s", statusText.c_str());
  }
  ImGui::End();
}

void App::watchBrushFolder() {
  // (1.2) images or .abr files put in the brushes folder become brushes by themselves
  static double lastLook = 0;
  if (nowSeconds() - lastLook < 3.0 || engine.painter.active()) return;
  lastLook = nowSeconds();
  if (!engine.brushes.userFolderChanged()) return;
  int change = engine.brushes.rescan();
  if (!engine.brushes.find(engine.selectedBrushId)) engine.selectedBrushId.clear();  // (the brush stays as it is)
  if (change > 0) setStatus(F_("%d new brushes from your brushes folder (the Brushes panel)", change));
  else if (change < 0) setStatus(F_("%d brushes went with their files from your brushes folder", -change));
}

void App::importBrushesUI(const std::vector<std::string>& files) {
  std::string err;
  auto ids = engine.brushes.importFiles(files, "", &err);
  if (ids.empty()) {
    showError(T_("No brush came of that"), err);
    return;
  }
  engine.selectBrush(ids[0]);
  setStatus(ids.size() == 1 ? F_("A new brush: %s (kept in your brushes folder)", engine.brushes.find(ids[0])->name.c_str())
                            : F_("%d new brushes (kept in your brushes folder)", (int)ids.size()));
}

bool App::confirmDiscard() { return !engine.sceneDirty(); }

// Opens the offline manual or Patina Web in the system browser (file:// URL).
void App::openLocalPage(const std::string& which) {
  fs::path d = paths::dataDir();
  std::vector<fs::path> cands;
  if (which.rfind("manual", 0) == 0) {
    // "manual" or "manual:<page>" (a page of it, such as manual:translating)
    std::string page = which.size() > 7 ? which.substr(7) + ".html" : std::string("index.html");
    cands = {d / "docs" / "manual" / page, d / ".." / "doc" / "patina" / "manual" / page, d / "manual" / page};
  } else
    cands = {d / "web" / "index.html", d / "web" / "dist" / "index.html"};
  for (auto& c : cands) {
    std::error_code ec;
    if (!fs::exists(c, ec)) continue;
    std::string p = fs::weakly_canonical(c, ec).generic_string();
#ifdef _WIN32
    std::string url = "file:///" + p;
#else
    std::string url = "file://" + p;
#endif
    for (size_t i = 0; i < url.size(); ++i)
      if (url[i] == ' ') url.replace(i, 1, "%20");
    if (SDL_OpenURL(url.c_str())) setStatus(F_("Opened %s in your browser", p.c_str()));
    else setStatus(F_("Could not open a browser: %s  (%s)", SDL_GetError(), p.c_str()));
    return;
  }
  setStatus(which.rfind("manual", 0) == 0 ? T_("Manual not found (expected docs/manual/index.html next to the materials)")
                                          : T_("Patina Web not found (build it with scripts/build_web.py)"));
}

void App::drawAbout() {
  if (!showAbout) return;
  ImGui::SetNextWindowSize(ImVec2(560, 0) * uiScale, ImGuiCond_Appearing);
  if (ImGui::Begin(L_("About Patina"), &showAbout, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse)) {
    ImGui::Text("Patina %s", PATINA_VERSION);
    ImGui::TextWrapped("%s", T_("Texture painter: paint materials straight onto 3D models regardless of their UVs, edit UVs, "
                                "and round-trip with Blender. Automate it from any MCP agent."));
    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped(T_("OpenGL: %s"), (const char*)glGetString(GL_RENDERER));
    ImGui::TextWrapped(T_("Data: %s"), paths::dataDir().string().c_str());
    ImGui::TextWrapped(T_("Your library: %s"), paths::userDataDir().string().c_str());
    ImGui::TextWrapped(T_("Settings: %s"), paths::configDir().string().c_str());
    ImGui::PopStyleColor();
    ImGui::TextDisabled(T_("Command server: 127.0.0.1:%d"), engine.serverPort);
    ImGui::Separator();
    ImGui::TextWrapped("%s", T_("Built with Dear ImGui, SDL3, glm, stb, cgltf, tinyobjloader, MikkTSpace, nlohmann/json, "
                                "portable-file-dialogs and llama.cpp (see the licenses in the source tree)."));
    ImGui::TextWrapped("%s", T_("The Patina Assistant's model is a fine-tune of Qwen3 1.7B by the Qwen team (Apache 2.0, "
                                "https://huggingface.co/Qwen/Qwen3-1.7B), downloaded separately."));
  }
  ImGui::End();
}

}  // namespace patina
