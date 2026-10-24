// 0.4 interface: island brush actions, texel density window, render dialog, high-poly
// bake dialog, welcome screen, mesh-file watching and crash recovery.
#include <cfloat>
#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <signal.h>
#include <unistd.h>
#endif
#include <filesystem>

#include "core/Log.h"
#include "core/Paths.h"
#include "core/UVIsland.h"
#include "ui/App.h"

namespace patina {

namespace fs = std::filesystem;

bool App::runCommand(const char* cmd, const json& args, json* out) {
  try {
    json r = engine.execute(cmd, args);
    if (out) *out = r;
    return true;
  } catch (std::exception& e) {
    showError(F_("Could not run %s", cmd), e.what());
    return false;
  }
}

// ------------------------------------------------------------------ island brush

json App::islandDensityArg() const {
  switch (islandDensityMode) {
    case 1: return "layout";
    case 2: return "fit";
    case 3: return "screen";
    case 4: return islandCustomDensity;
    default: return "auto";
  }
}

void App::makeIslandFromSelection() {
  if (engine.doc.selectedFaces().empty()) {
    setStatus(T_("Paint over the model with the Island brush (L) first"));
    return;
  }
  static const char* orient[] = {"none", "upright", "longest_axis"};
  json r;
  json args = {{"selection", true}, {"orient", orient[std::clamp(islandOrient, 0, 2)]},
               {"density", islandDensityArg()}, {"reproject", reprojectOnUVEdit}, {"pin", islandPinNew}};
  // mirror symmetry on: the mirrored half shares the island's UVs
  json axes = json::array();
  const char* names[] = {"x", "y", "z"};
  for (int i = 0; i < 3; ++i)
    if (engine.painter.symmetry[i]) axes.push_back(names[i]);
  if (!axes.empty()) args["symmetry"] = axes;
  if (!runCommand("uv_make_island", args, &r)) return;
  char buf[960];
  int stacked = r.value("stacked_faces", 0);
  std::string sym;
  if (stacked) sym = F_(", mirrored half stacked on it (%d faces)", stacked);
  int pinnedLeft = r.value("pinned_faces_left_alone", 0);
  char pinned[192] = "";
  if (pinnedLeft)
    snprintf(pinned, sizeof pinned, P_("; %d pinned face left alone", "; %d pinned faces left alone", pinnedLeft), pinnedLeft);
  int made = r.value("islands_made", 1);
  snprintf(buf, sizeof buf,
           P_("Island made (%d piece) at %.0f texels per unit%s%s%s%s",
              "Island made (%d pieces) at %.0f texels per unit%s%s%s%s", made),
           made, r.value("texels_per_unit", 0.f), sym.c_str(),
           r.value("pinned", false) ? T_(", pinned (Alt+P in the UV editor unpins)") : "", pinned,
           r.value("repacked", false) ? T_(" - no free space, so the unpinned islands were repacked") : "");
  setStatus(buf);
  engine.viewOptions.seams = true;
}

size_t App::overlappingIslandCount() {
  // recomputed only when the UVs change (not every undo step: on a sculpt this is slow)
  uint64_t v = (uint64_t(engine.doc.mesh.faceCount()) << 40) ^
               (uint64_t(engine.doc.mesh.tiles.mode) << 58) ^ (uint64_t(engine.doc.mesh.tiles.count()) << 50) ^ uv::uvHash(engine.doc.mesh);
  if (v != overlapVersion) {
    overlapVersion = v;
    overlapIslands = engine.doc.hasMesh() ? uv::overlappingIslands(engine.doc.mesh, 256).size() : 0;
    uvCollapsed = engine.doc.hasMesh() ? uv::collapsedFaces(engine.doc.mesh) : std::vector<uint32_t>();
  }
  return overlapIslands;
}

// ------------------------------------------------------------------ texture sets from a selection

void App::openWindowByName(const std::string& w, const json& a) {
  // (0.46) in the Guided layout a docked panel is in a step: that step comes up (the Paint step's side tabs: the one
  // asked for); the panels it has no room for are in the Full layout
  if (blenderOn() && blenderOpenWindow(w)) return;  // (1.2.7) the Blender layout: its tab
  if (guidedOn()) {
    if (w == "layers" || w == "materials" || w == "brushes" || w == "properties" || w == "colors" || w == "history") {
      setGuidedStep(GuidedStep::Paint);
      guidedPaintTab = w == "colors" ? 1 : w == "history" ? 2 : w == "properties" ? 0 : guidedPaintTab;
      return;
    }
    if (w == "blender") return setGuidedStep(GuidedStep::Export);
    if (w == "texture_set" || w == "console" || w == "notes" || w == "retopology") setLayoutMode(false);
  }
  if (w == "export") {
    std::string what = a.value("what", std::string("all"));
    exportWhat = what == "layer" ? 1 : what == "mask" ? 2 : 0;
    static const char* presets[] = {"blender", "gltf", "unreal", "unity"};
    std::string p = a.value("preset", std::string());
    for (int i = 0; i < 4; ++i)
      if (p == presets[i]) exportPresetIdx = i;
    showExport = true;
  } else if (w == "render") showRender = true;
  else if (w == "texel_density") showDensity = true;
  else if (w == "symmetry") showSymmetry = true;
  else if (w == "mesh_check") showMeshCheck = true;
  else if (w == "export_model") showModelExport = true;
  else if (w == "face_sets") openFaceSets();  // (1.2)
  else if (w == "reduce") showReduce = true;
  else if (w == "batch") showBatch = true;
  else if (w == "transform") showTransform = true;
  else if (w == "collision") showCollisionWindow = true;
  else if (w == "lightmap") showLightmapWindow = true;
  else if (w == "high_poly") showHighPoly = true;
  else if (w == "bake_down") {
    showBakeDown = true;
    if (a.contains("path")) bakeDownInput = a["path"].get<std::string>(), bakeDownSource = 0;
  }
  else if (w == "new_texture_set") openNewTextureSet();
  else if (w == "texture_set") focusWindowNext = "Texture Set";
  else if (w == "blender") focusWindowNext = "Blender Link";
  else if (w == "preferences") {
    showPreferences = true;
    prefsScrollTo = a.value("section", std::string());
    if (a.contains("keymap")) applyKeymapPreset(a.value("keymap", std::string()) == "blender" ? KeymapPreset::Blender : KeymapPreset::Patina, true);
  }
  else if (w == "shortcuts") showKeymap = true;
  else if (w == "macros") showMacros = true;
  else if (w == "retopology") openRetopo();
  // (0.36) the panels: their tab brought to the front
  else if (w == "layers") focusWindowNext = "Layers";
  else if (w == "properties") focusWindowNext = "Properties";
  else if (w == "materials") focusWindowNext = "Materials";
  else if (w == "brushes") focusWindowNext = "Brushes";
  else if (w == "console") focusWindowNext = "Console";
  else if (w == "history") focusWindowNext = "History";
  else if (w == "notes") focusWindowNext = "Notes";
  else if (w == "colors") focusWindowNext = "Colors";
  else if (w == "about") showAbout = true;
  else if (w == "help") showHelp = true;
  else if (w == "welcome") showWelcome = true;
  else if (w == "pen_test") showPenTest = true;
  else if (w == "material_editor") {
    if (auto* e = engine.materials.find(a.value("material", std::string())); e && e->def.type == MaterialType::Procedural) {
      editMaterial = e->def;
      if (e->def.builtin) {
        editMaterial.id = "user_" + e->def.id;
        editMaterial.name = e->def.name + " (custom)";
        editMaterial.category = "User";
      }
    } else if (a.contains("generator")) {
      editMaterial = MaterialDef();
      editMaterial.id = "user_material";
      editMaterial.name = "My Material";
      editMaterial.category = "User";
      editMaterial.proc.generator = generatorFromName(a.value("generator", std::string()));
    }
    editMaterialDirty = true;
    showMaterialEditor = true;
  }
}

void App::openNewTextureSet() {
  Document& d = engine.doc;
  showNewSet = true;
  // a name to start from: Head when the selection is the head, else the next free "Set N"
  std::string name;
  auto sel = d.selectedFaces();
  if (!sel.empty()) {
    for (auto& p : engine.meshParts().parts)
      if (p.id == "region:head") {
        std::vector<uint8_t> head(d.mesh.faceCount(), 0);
        for (uint32_t f : p.faces) head[f] = 1;
        size_t in = 0;
        for (uint32_t f : sel) in += head[f];
        if (in * 10 >= sel.size() * 8) name = "Head";
      }
  }
  if (name.empty()) name = "Set " + std::to_string(d.mesh.tiles.active() ? d.mesh.tiles.count() + 1 : 2);
  snprintf(newSetName, sizeof newSetName, "%s", name.c_str());
}

void App::drawNewTextureSetWindow() {
  if (!showNewSet) return;
  Document& d = engine.doc;
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSizeConstraints(ImVec2(430 * uiScale, 0), ImVec2(430 * uiScale, FLT_MAX));
  if (!ImGui::Begin(L_("New texture set"), &showNewSet,
                    ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::End();
    return;
  }
  const TileLayout& tl = d.mesh.tiles;
  bool udimOnly = (tl.mode & TileByUdim) && !(tl.mode & TileBySlot);
  size_t nSel = d.selectedFaces().size();
  ImGui::PushTextWrapPos(0);
  if (udimOnly)
    ImGui::TextUnformatted(T_("The selected faces' islands move to the next free UDIM tile, which is a texture of its own."));
  else
    ImGui::TextUnformatted(T_("The selected faces get a texture of their own (a texture set), such as one for the head and "
                              "one for the body. In Blender they become a material slot of their own."));
  ImGui::PopTextWrapPos();
  ImGui::Spacing();
  if (nSel) ImGui::Text(T_("%zu faces selected"), nSel);
  else ImGui::TextColored(ImVec4(1, 0.72f, 0.35f, 1), "%s", T_("Nothing is selected yet"));
  // quick ways to select the usual parts
  const auto& parts = engine.meshParts();
  for (auto& p : parts.parts)
    if (p.id == "region:head") {
      ImGui::SameLine();
      if (ImGui::SmallButton(L_("Select the head"))) {
        d.faceSelection.assign(d.mesh.faceCount(), 0);
        for (uint32_t f : p.faces) d.faceSelection[f] = 1;
        d.updateSelectionFlags();
        snprintf(newSetName, sizeof newSetName, "Head");
      }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T_("Everything above the neck"));
    }
  if (!udimOnly) {
    ImGui::SetNextItemWidth(200 * uiScale);
    ImGui::InputText(L_("Name"), newSetName, sizeof newSetName);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", T_("The texture set's name (the material slot in Blender).\nThe name of an existing set moves the faces into it."));
    if (tl.mode & TileBySlot) {  // existing sets to pick
      ImGui::SameLine();
      if (ImGui::BeginCombo("##existing", "", ImGuiComboFlags_NoPreview)) {
        for (auto& t : tl.tiles) {
          std::string n = d.mesh.slotName(t.slot);
          if (ImGui::Selectable(n.c_str())) snprintf(newSetName, sizeof newSetName, "%s", n.c_str());
        }
        ImGui::EndCombo();
      }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T_("An existing texture set"));
    }
  }
  ImGui::Checkbox(L_("Pack each set to fill its space"), &newSetPack);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", T_("The new set's islands grow to fill its texture (pinned ones move too and stay pinned).\n"
                               "The set they came from is packed again unless it has pinned islands."));
  ImGui::TextDisabled("%s", T_("Paint moves with the faces. The undo history is cleared."));
  ImGui::Spacing();
  ImGui::BeginDisabled(nSel == 0 || (!udimOnly && !newSetName[0]));
  if (ImGui::Button(udimOnly ? L_("Move to a new UDIM tile") : L_("Make the texture set"), ImVec2(-1, 0))) {
    json r;
    json args = {{"selection", true}, {"pack", newSetPack}};
    if (!udimOnly) args["name"] = std::string(newSetName);
    if (runCommand("texture_set_from_faces", args, &r)) {
      showNewSet = false;
      int idx = r.value("index", 0);
      if (d.mesh.tiles.active()) engine.uvOptions.tile = std::clamp(idx, 0, d.mesh.tiles.count() - 1);
      std::string msg = udimOnly ? F_("Moved to UDIM %d", r.value("udim", 0))
                                 : F_("Texture set '%s': %d faces", r.value("set", std::string()).c_str(), r.value("faces", 0));
      if (!r["kept_layout"].empty())
        msg += F_(" (%s kept its layout: it has pinned islands)", r["kept_layout"][0].get<std::string>().c_str());
      if (d.bridge.active) msg += T_(". Send to Blender to give it the material slot");
      setStatus(msg);
    }
  }
  ImGui::EndDisabled();
  ImGui::End();
}

// ------------------------------------------------------------------ texel density

void App::drawDensityWindow() {
  if (!showDensity) return;
  ImGui::SetNextWindowSizeConstraints(ImVec2(470 * uiScale, 0), ImVec2(470 * uiScale, FLT_MAX));
  if (!ImGui::Begin(L_("Texel density"), &showDensity, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::End();
    return;
  }
  Document& d = engine.doc;
  // cached: the targets walk every island, too slow to redo each frame on big meshes
  static uint64_t cacheKey = 0;
  static uv::DensityTargets t;
  static float sel = 0.f;
  auto faces = d.selectedFaces();
  uint64_t key = uv::uvHash(d.mesh) ^ (uint64_t(d.tileRes()) << 40) ^ (uint64_t(faces.size()) << 8);
  for (size_t i = 0; i < faces.size(); i += std::max<size_t>(1, faces.size() / 64)) key = key * 31 + faces[i];
  if (key != cacheKey) {
    cacheKey = key;
    t = uv::densityTargets(d.mesh, d.tileRes());
    sel = faces.empty() ? 0.f : uv::areaDensity(d.mesh, faces);
  }
  int R = d.tileRes();
  ImGui::TextWrapped(T_("Texels per unit (1 unit = 1 m in Blender) at %d px. The heat map view mode shows the same "
                        "against the target."), R);
  if (ImGui::BeginTable("##td", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
    auto row = [&](const char* label, float v, const char* tip) {
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::TextUnformatted(label);
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
      ImGui::TableSetColumnIndex(1);
      if (v > 0) ImGui::Text(T_("%.0f px/m"), v * R);
      else ImGui::TextDisabled("-");
    };
    row(T_("Fit the texture"), t.fit, T_("The whole surface fills about 70 percent of the texture"));
    row(T_("1 texel per pixel"), t.screen, T_("The model framed in a 1080p view: one texel per screen pixel"));
    row(T_("Existing layout"), t.layout, T_("What the current islands use (median)"));
    row(T_("Selection"), sel, T_("Islands of the selected faces"));
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted(T_("Recommended"));
    ImGui::TableSetColumnIndex(1);
    ImGui::Text(T_("%.0f px/m"), t.recommended() * R);
    ImGui::EndTable();
  }
  if (t.screen > t.fit * 1.05f) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.72f, 0.35f, 1));
    int need = R;
    while (need < 8192 && t.screen * R > t.fit * need * 1.05f) need *= 2;
    ImGui::TextWrapped(T_("At %d px the texture is too small for one texel per pixel when the model fills the screen. "
                          "%d px would be enough, or accept a softer result up close."),
                       R, need);
    ImGui::PopStyleColor();
  }
  ImGui::SeparatorText(T_("Island brush target"));
  ImGui::Combo(L_("Density"), &islandDensityMode, Z_("Automatic\0Match the layout\0Fit the texture\0One texel per pixel\0Custom\0"));
  if (islandDensityMode == 4) ImGui::DragFloat(L_("px per unit"), &islandCustomDensity, 4.f, 8.f, 65536.f, "%.0f");
  ImGui::Combo(L_("Orientation"), &islandOrient, Z_("Keep\0Upright (up = up)\0Along the longest side\0"));
  ImGui::SeparatorText(T_("Selected islands"));
  ImGui::BeginDisabled(faces.empty());
  if (ImGui::Button(L_("Set to the target"))) {
    json r;
    if (runCommand("uv_density", {{"set", islandDensityArg()}, {"selection", true}, {"reproject", reprojectOnUVEdit}}, &r))
      setStatus(T_("Texel density set"));
  }
  ImGui::SameLine();
  if (ImGui::Button(L_("Orient"))) runCommand("uv_orient", {{"mode", islandOrient == 2 ? "longest_axis" : "upright"}, {"selection", true}});
  ImGui::SameLine();
  if (ImGui::Button(L_("Move into free space"))) runCommand("uv_pack", {{"free_space", true}, {"selection", true}});
  ImGui::EndDisabled();
  if (faces.empty()) ImGui::TextDisabled("%s", T_("Select faces (S) or paint with the Island brush (L) first."));
  ImGui::SeparatorText(T_("View"));
  bool dv = engine.viewOptions.mode == ViewMode::TexelDensity, sv = engine.viewOptions.mode == ViewMode::UVStretch;
  if (ImGui::Checkbox(L_("Density heat map"), &dv)) engine.viewOptions.mode = dv ? ViewMode::TexelDensity : ViewMode::Material;
  ImGui::SameLine();
  if (ImGui::Checkbox(L_("Stretch heat map"), &sv)) engine.viewOptions.mode = sv ? ViewMode::UVStretch : ViewMode::Material;
  if (dv) ImGui::TextDisabled("%s", T_("Blue: below the target   green: on target   red: above   purple: no UVs"));
  if (sv) ImGui::TextDisabled("%s", T_("Gray-blue: no stretch   yellow: 1.5x   red: 2x or more"));
  ImGui::End();
}

// ------------------------------------------------------------------ render image

void App::drawRenderDialog() {
  // (0.39) a path traced render goes on a frame's time at a time, with the dialog open or not
  if (engine.renderJob.active && engine.renderJobStep(12.0)) {
    const auto& J = engine.renderJob;
    if (!J.error.empty()) {
      if (J.error != "cancelled") showError(T_("The image was NOT rendered"), J.error);
    } else {
      setStatus(F_("Rendered %zu image(s): %s", J.written.size(), J.written.empty() ? "" : J.written[0].c_str()));
      showRender = false;
    }
  }
  if (!showRender) return;
  ImGui::SetNextWindowSizeConstraints(ImVec2(480 * uiScale, 0), ImVec2(480 * uiScale, FLT_MAX));
  if (!ImGui::Begin(L_("Render image"), &showRender, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::End();
    return;
  }
  if (renderPath.empty()) {
    std::string stem = engine.doc.projectPath.empty() ? engine.doc.mesh.name : fs::path(engine.doc.projectPath).stem().string();
    renderPath = (paths::homeDir() / "patina_renders" / (paths::sanitizeFileName(stem.empty() ? "render" : stem) + ".png")).string();
  }
  ImGui::InputText(L_("File"), &renderPath);
  ImGui::SameLine();
  if (ImGui::Button("...")) openFileDialog(DialogPurpose::RenderPath);
  ImGui::SetNextItemWidth(220 * uiScale);
  ImGui::InputInt2(L_("Size"), &renderW);
  renderW = std::clamp(renderW, 16, 8192), renderH = std::clamp(renderH, 16, 8192);
  ImGui::SameLine();
  if (ImGui::SmallButton(L_("HD"))) renderW = 1920, renderH = 1080;
  ImGui::SameLine();
  if (ImGui::SmallButton(L_("4K"))) renderW = 3840, renderH = 2160;
  ImGui::SameLine();
  if (ImGui::SmallButton(L_("Square"))) renderW = renderH = 2048;
  ImGui::Checkbox(L_("Transparent background"), &renderTransparent);
  // (0.39) path traced: real shadows, reflections and bounced light
  const bool pt = engine.features.on("path_tracing");
  if (pt) {
    ImGui::Checkbox(L_("Path traced"), &renderPathTraced);
    {
      ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
      engine.uiRects["render.path_traced"] = {a.x, a.y, b.x - a.x, b.y - a.y};  // (for tests and the manual's screenshots)
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", T_("Real shadows, reflections and light bouncing between the model's parts, lit by the\n"
                                 "environment, with a shadow on the ground. Takes longer: seconds to minutes."));
    if (renderPathTraced) {
      ImGui::SameLine();
      ImGui::SetNextItemWidth(160 * uiScale);
      ImGui::SliderInt(L_("Samples"), &renderSamples, 16, 8192, "%d", ImGuiSliderFlags_Logarithmic);
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T_("More: less grain, slower. 256 is clean for most models."));
      if (ImGui::TreeNode(L_("Path tracing settings"))) {
        pathTraceSettingsUI();
        ImGui::TreePop();
      }
    }
  }
  const bool traced = pt && renderPathTraced;
  ImGui::BeginDisabled(traced);
  ImGui::SliderInt(L_("Anti-aliasing"), &renderSS, 1, 4, traced || renderSS == 1 ? T_("off") : T_("%dx"));
  ImGui::EndDisabled();
  ImGui::SliderInt(L_("Turntable frames"), &renderFrames, 0, 360, renderFrames < 2 ? T_("single image") : T_("%d frames"));
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::TextWrapped("%s", T_("Uses the current camera, lighting and view mode, without seams or wireframe."));
  ImGui::PopStyleColor();
  bool go = false;
  if (engine.renderJob.active) {
    // the path traced render under way
    const auto& J = engine.renderJob;
    std::string what = J.frames > 1 ? F_("Frame %d of %d", J.frame + 1, J.frames) : F_("%.0f%%", J.progress * 100.f);
    ImGui::ProgressBar(J.progress, ImVec2(300 * uiScale, 0), what.c_str());
    ImGui::SameLine();
    if (ImGui::Button(L_("Cancel"))) engine.cancelRenderJob(), setStatus(T_("Render cancelled"));
  } else {
    go = ImGui::Button(L_("Render"), ImVec2(120 * uiScale, 0));
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    engine.uiRects["render.go"] = {a.x, a.y, b.x - a.x, b.y - a.y};
    go = go || (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::GetIO().WantTextInput &&
                ImGui::IsKeyPressed(ImGuiKey_Enter));
  }
  if (go) {
    RenderImageOptions o;
    o.path = renderPath;
    o.width = renderW;
    o.height = renderH;
    o.transparent = renderTransparent;
    o.supersample = renderSS;
    o.turntableFrames = renderFrames >= 2 ? renderFrames : 0;
    if (traced) o.pathSamples = std::max(1, renderSamples);
    std::vector<std::string> files;
    std::string err;
    if (traced) {
      if (!engine.startRenderJob(o, &err)) showError(T_("The image was NOT rendered"), err);
    } else if (engine.renderImage(o, &files, &err)) {
      setStatus(F_("Rendered %zu image(s): %s", files.size(), files.empty() ? "" : files[0].c_str()));
      showRender = false;
    } else {
      showError(T_("The image was NOT rendered"), err);
    }
  }
  ImGui::SameLine();
  if (ImGui::Button(L_("Open the folder"))) {
    fs::path dir = fs::path(renderPath).parent_path();
    std::error_code ec;
    fs::create_directories(dir, ec);
#ifdef _WIN32
    SDL_OpenURL(("file:///" + dir.generic_string()).c_str());
#else
    SDL_OpenURL(("file://" + dir.generic_string()).c_str());
#endif
  }
  ImGui::End();
}

// ------------------------------------------------------------------ high-poly bake

void App::drawHighPolyDialog() {
  if (!showHighPoly) return;
  ImGui::SetNextWindowSizeConstraints(ImVec2(500 * uiScale, 0), ImVec2(500 * uiScale, FLT_MAX));
  if (!ImGui::Begin(L_("Bake from a high-poly mesh"), &showHighPoly, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::End();
    return;
  }
  Document& d = engine.doc;
  if (ImGui::IsWindowAppearing() && !d.highPolyPath.empty()) {
    // show the settings of the bake this project has (also when it came from a command or the project file)
    highPolyInput = d.highPolyPath;
    if (d.highPolyOptions.distance > 0) highPolyDistance = d.highPolyOptions.distance;
    hpNormal = d.highPolyOptions.normal;
    hpAO = d.highPolyOptions.ao;
    hpIds = d.highPolyOptions.materialIds;
    hpSamples = d.highPolyOptions.aoSamples;
  }
  if (highPolyInput.empty()) highPolyInput = d.highPolyPath;
  ImGui::TextWrapped("%s", T_("Bakes the detail of a sculpt onto this model: a normal map (combined with every layer, and "
                              "exported with the textures), ambient occlusion and material slots. Both meshes must be "
                              "in the same place (export them from the same Blender scene)."));
  ImGui::InputTextWithHint(L_("High-poly"), ".glb / .gltf / .fbx / .obj", &highPolyInput);
  ImGui::SameLine();
  if (ImGui::Button("...")) openFileDialog(DialogPurpose::HighPoly);
  pointsUI(hpPoints);
  distanceUI(highPolyDistance, d.mesh.boundingRadius);
  ImGui::Checkbox(L_("Normal map"), &hpNormal);
  ImGui::SameLine();
  ImGui::Checkbox(L_("Ambient occlusion"), &hpAO);
  ImGui::SameLine();
  ImGui::Checkbox(L_("Material slots"), &hpIds);
  if (hpAO) ImGui::SliderInt(L_("AO samples"), &hpSamples, 16, 512);
  if (ImGui::Button(L_("Bake"), ImVec2(120 * uiScale, 0))) {
    json r;
    json args = {{"path", highPolyInput}, {"normal", hpNormal}, {"ao", hpAO}, {"material_ids", hpIds}, {"ao_samples", hpSamples},
                 {"samples", hpPoints}};
    if (highPolyDistance > 0) args["distance"] = highPolyDistance;
    if (runCommand("bake_from_high_poly", args, &r)) {
      setStatus(F_("Baked from the high-poly mesh: %.0f%% of the texels found it (%.1f s)",
                   r.value("hit_fraction", 0.0) * 100.0, r.value("seconds", 0.0)));
      if (!r.value("warning", std::string()).empty()) showError(T_("Check the high-poly bake"), r["warning"].get<std::string>());
    }
  }
  if (d.highPolyBaked || d.tex.hasHighNormal()) {
    ImGui::SameLine();
    if (ImGui::Button(L_("Remove the bake"))) runCommand("bake_from_high_poly", {{"clear", true}});
    ImGui::TextDisabled(T_("Baked from %s"), d.highPolyPath.empty() ? T_("(unknown)") : fs::path(d.highPolyPath).filename().string().c_str());
  }
  ImGui::End();
}

// ------------------------------------------------------------------ (1.1) the bake-down

bool App::distanceUI(float& dist, float radius) {
  bool manual = dist > 0.f, changed = false;
  if (ImGui::Checkbox(L_("Set the distance by hand"), &manual)) {
    dist = manual ? 0.02f * std::max(radius, 1e-4f) : 0.f;
    changed = true;
  }
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", T_("How far along the low-poly's normals, both ways, to look for the high-poly surface.\n"
                               "Off: measured from how far apart the two models are.\n"
                               "Raise it if parts come out flat or guessed; lower it if details of nearby parts\n"
                               "show in the wrong place."));
  if (manual) {
    ImGui::SameLine();
    ImGui::SetNextItemWidth(170 * uiScale);
    float r = std::max(radius, 1e-4f);
    changed |= ImGui::SliderFloat("##distance", &dist, 0.001f * r, 0.5f * r, "%.4f", ImGuiSliderFlags_Logarithmic);
  } else {
    ImGui::SameLine();
    ImGui::TextDisabled("%s", T_("(measured)"));
  }
  return changed;
}

bool App::pointsUI(int& points) {
  static const int kPoints[] = {1, 4, 9, 16};
  const char* names[] = {T_("1 (fastest)"), T_("4"), T_("9 (fine)"), T_("16 (finest)")};
  int idx = 0;
  for (int k = 0; k < 4; ++k)
    if (points >= kPoints[k]) idx = k;
  ImGui::SetNextItemWidth(170 * uiScale);
  bool changed = ImGui::Combo(L_("Points per texel"), &idx, names, 4);
  if (changed) points = kPoints[idx];
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", T_("Each texel is measured at this many points spread over it, and their values averaged:\n"
                               "more points give finer, smoother results along edges and in fine detail, and take longer"));
  return changed;
}

void App::bakeDownReportUI(bool buttons) {
  Document& d = engine.doc;
  if (!d.bakeDownCurrent() || !d.bakeDownReport.is_object()) return;
  const json& r = d.bakeDownReport;
  double ray = r.value("hit_fraction", 0.0), byNearest = r.value("nearest_point_fraction", 0.0), guessed = r.value("guessed_fraction", 0.0);
  ImGui::TextWrapped(T_("%.1f%% of the texture found the high-poly along the normals, %.1f%% by the nearest point, and %.2f%% "
                        "was guessed."), ray * 100.0, byNearest * 100.0, guessed * 100.0);
  ImGui::TextDisabled(T_("%d layers moved; the two surfaces were at most %.4g apart (the distance: %.4g%s)."),
                      r.value("layers_moved", 0), r.value("largest_distance", 0.0), r.value("distance", 0.0),
                      r.value("distance_measured", false) ? T_(", measured") : "");
  if (r.value("flipped_fraction", 0.0) > 0.001)
    ImGui::TextColored(ImVec4(0.98f, 0.7f, 0.25f, 1.f), "%s", T_("Parts of the low-poly face away from the high-poly (magenta in the check): turned inside out, or folded over a hollow."));
  else if (guessed > 0.002 || byNearest > 0.05)
    ImGui::TextColored(ImVec4(0.98f, 0.7f, 0.25f, 1.f), "%s", T_("Look at the check: amber was taken from the nearest point, red was guessed."));
  if (!buttons) return;
  bool checking = engine.viewOptions.mode == ViewMode::BakeCheck;
  if (ImGui::Button(checking ? L_("Hide the check") : L_("Show the check"))) {
    engine.viewOptions.mode = checking ? ViewMode::Material : ViewMode::BakeCheck;
  }
  {
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    engine.uiRects["bakedown.check"] = {a.x, a.y, b.x - a.x, b.y - a.y};
  }
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", T_("The model in gray where the rays found the high-poly; amber where only the nearest point did;\n"
                               "red where the paint had to be guessed from farther away; magenta where the low-poly\n"
                               "faces away from the high-poly (turned inside out, or folded over a hollow)"));
  ImGui::SameLine();
  auto& cv = engine.compare;
  bool comparing = cv.enabled && cv.mode == 2;
  ImGui::BeginDisabled(!d.bakeDownBefore);
  if (ImGui::Button(comparing ? L_("Stop comparing") : L_("Compare with the high-poly"))) {
    if (comparing) cv.enabled = false, cv.mode = 0;
    else cv.enabled = true, cv.mode = 2;
  }
  ImGui::EndDisabled();
  {
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    engine.uiRects["bakedown.compare"] = {a.x, a.y, b.x - a.x, b.y - a.y};
  }
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", T_("The high-poly as it was, left of the divider, and the low-poly with the layers baked down, right\n"
                               "of it: drag the divider across the model"));
}

void App::bakeDownNow() { bakeDownIn = 2; }

void App::bakeDownRun() {
  json args = {{"samples", bakeDownPoints}};
  if (bakeDownDistance > 0) args["distance"] = bakeDownDistance;
  if (bakeDownRes > 0) args["resolution"] = bakeDownRes;
  json r;
  bool ok;
  if (bakeDownSource == 1) {
    args["keep_layers"] = true;
    args["bake"] = bakeDownHigh;
    ok = runCommand("retopo_use_as_model", args, &r);
    if (ok) r = r.value("bake_down", json::object()), showRetopo = false;
  } else {
    args["path"] = bakeDownInput;
    args["bake_high_poly"] = bakeDownHigh;
    ok = runCommand("bake_down", args, &r);
  }
  if (!ok) return;
  tool = ViewportTool::Paint;
  setStatus(F_("Baked down: %d layers on the low-poly model, %.1f%% found along the normals (%.1f s). Save it as a new project.",
               r.value("layers_moved", 0), r.value("hit_fraction", 0.0) * 100.0, r.value("seconds", 0.0)));
  if (!r.value("warning", std::string()).empty()) showError(T_("Check the bake-down"), r["warning"].get<std::string>());
}

void App::drawBakeDownDialog() {
  if (bakeDownIn > 0) {  // the work takes a few seconds (more at 4K): say so in the frame shown while it runs
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const char* msg = T_("Baking the layers down...");
    ImVec2 ts = ImGui::CalcTextSize(msg), c = vp->GetCenter(), pad(18 * uiScale, 12 * uiScale);
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    fg->AddRectFilled(ImVec2(c.x - ts.x / 2 - pad.x, c.y - ts.y / 2 - pad.y), ImVec2(c.x + ts.x / 2 + pad.x, c.y + ts.y / 2 + pad.y),
                      IM_COL32(20, 20, 24, 230), 6.f * uiScale);
    fg->AddText(ImVec2(c.x - ts.x / 2, c.y - ts.y / 2), IM_COL32(240, 240, 240, 255), msg);
    if (--bakeDownIn == 0) bakeDownRun();
  }
  if (!showBakeDown) return;
  if (!engine.features.on("bake_down")) {
    showBakeDown = false;
    return;
  }
  ImGui::SetNextWindowSizeConstraints(ImVec2(540 * uiScale, 0), ImVec2(540 * uiScale, FLT_MAX));
  if (!ImGui::Begin(L_("Bake Down to a Low-Poly Model"), &showBakeDown,
                    ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::End();
    return;
  }
  Document& d = engine.doc;
  auto& S = engine.retopo();
  bool quads = S.result && S.result->low.faceCount() > 0;
  if (!quads && bakeDownSource == 1) bakeDownSource = 0;
  ImGui::TextWrapped("%s", T_("Moves every layer of this model, the painted high-poly, onto its low-poly version, which becomes the "
                              "model. Paint and masks are measured texel by texel where the low-poly's surface meets the "
                              "high-poly's, and normals are turned to fit. Fills, adjustments and smart masks stay as they are. "
                              "The high-poly's shape goes into the low-poly's normal map and ambient occlusion."));
  ImGui::TextDisabled("%s", T_("Both models must be in the same place (export them from the same Blender scene)."));
  ImGui::Spacing();
  ImGui::RadioButton(L_("A model file"), &bakeDownSource, 0);
  ImGui::SameLine();
  ImGui::BeginDisabled(!quads);
  ImGui::RadioButton(L_("The retopology's quads"), &bakeDownSource, 1);
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    ImGui::SetTooltip("%s", quads ? T_("The quads you made in Retopology (saved as an .obj next to this model)")
                                  : T_("Make quads first (Retopology)"));
  if (bakeDownSource == 0) {
    ImGui::InputTextWithHint(L_("Low-poly"), ".glb / .gltf / .fbx / .obj", &bakeDownInput);
    ImGui::SameLine();
    if (ImGui::Button("...##bakedown")) openFileDialog(DialogPurpose::BakeDownLow);
  }
  pointsUI(bakeDownPoints);
  {
    static const int kRes[] = {0, 512, 1024, 2048, 4096, 8192};
    std::string now = F_("As now (%d)", d.tileRes());
    const char* names[] = {now.c_str(), "512", "1024", "2048", "4096", "8192"};
    int idx = 0;
    for (int k = 0; k < 6; ++k)
      if (bakeDownRes == kRes[k]) idx = k;
    ImGui::SetNextItemWidth(170 * uiScale);
    if (ImGui::Combo(L_("Resolution##bakedown"), &idx, names, 6)) bakeDownRes = kRes[idx];
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T_("The low-poly model's textures, per texture set or UDIM tile"));
  }
  distanceUI(bakeDownDistance, d.mesh.boundingRadius);
  ImGui::Checkbox(L_("Bake the high-poly's shape into the normal map and AO"), &bakeDownHigh);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", T_("The sculpted detail as the low-poly's normal map (under every layer), its ambient occlusion and\n"
                               "material slots measured on the high-poly. Off: the low-poly's own bakes."));
  bool ready = d.hasMesh() && !d.layers.empty() && (bakeDownSource == 1 ? quads : !bakeDownInput.empty());
  ImGui::Spacing();
  ImGui::BeginDisabled(!ready);
  if (ImGui::Button(L_("Bake down"), ImVec2(150 * uiScale, 0)))
    replaceProject(T_("bake the layers down"), [this] { bakeDownNow(); });
  ImGui::EndDisabled();
  {
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    engine.uiRects["bakedown.go"] = {a.x, a.y, b.x - a.x, b.y - a.y};
  }
  ImGui::SameLine();
  ImGui::TextDisabled("%s", T_("The result is a new project; this one stays as it was saved."));
  if (d.bakeDownCurrent()) {
    ImGui::SeparatorText(T_("The last bake-down"));
    bakeDownReportUI(true);
  }
  ImGui::End();
}

// ------------------------------------------------------------------ welcome screen

void App::drawWelcome() {
  if (!showWelcome) return;
  ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(640, 0) * uiScale, ImGuiCond_Appearing);
  if (!ImGui::Begin(L_("Welcome to Patina"), &showWelcome, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse)) {
    ImGui::End();
    return;
  }
  ImGui::TextWrapped("%s", T_("Paint materials onto 3D models, regardless of their UVs. Open a model to start, or try the "
                              "sample."));
  ImGui::Spacing();
  float w = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
  ImGui::BeginGroup();
  ImGui::SeparatorText(T_("Start"));
  if (ImGui::Button(L_("Open a model or project..."), ImVec2(w, 0)))
    replaceProject(T_("open another file"), [this] { openFileDialog(DialogPurpose::OpenFile); }), showWelcome = false;
  fs::path sample = paths::dataDir() / "samples" / "Suzanne.glb";
  std::error_code ec;
  if (fs::exists(sample, ec) && ImGui::Button(L_("Open the sample (Suzanne)"), ImVec2(w, 0))) {
    showWelcome = false;
    replaceProject(T_("open the sample"), [this, sample] {
      std::string err;
      if (!engine.openFile(sample.string(), &err)) showError(T_("Could not open the sample"), err);
    });
  }
  if (ImGui::Button(L_("New sphere"), ImVec2(w * 0.5f - 2, 0)))
    replaceProject(T_("start a new project"), [this] { runCommand("new_project", {{"primitive", "sphere"}}); }), showWelcome = false;
  {
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    engine.uiRects["welcome.new_sphere"] = {a.x, a.y, b.x - a.x, b.y - a.y};  // (for tests)
  }
  ImGui::SameLine(0, 4);
  if (ImGui::Button(L_("New cube"), ImVec2(w * 0.5f - 2, 0)))
    replaceProject(T_("start a new project"), [this] { runCommand("new_project", {{"primitive", "cube"}}); }), showWelcome = false;
  // (0.46) the program you know: its keys and its way of turning the view (Maya: Alt with the mouse buttons)
  ImGui::SeparatorText(T_("You come from"));
  {
    int from = keymapPreset == KeymapPreset::Blender ? 0 : navPreset == NavPreset::Maya ? 1 : 2;
    const char* names[] = {N_("Blender"), N_("Maya"), N_("Something else")};
    const char* tips[] = {N_("Blender's keys and navigation: the middle button turns the view, F3 searches, Shift+A adds"),
                          N_("Maya's navigation: Alt with the left, middle and right buttons turns, moves and zooms the view"),
                          N_("Patina's keys: Alt or the middle button turns the view (as in Substance Painter)")};
    for (int k = 0; k < 3; ++k) {
      if (k) ImGui::SameLine();
      if (ImGui::RadioButton(L_(names[k]), from == k) && from != k) {
        applyKeymapPreset(k == 0 ? KeymapPreset::Blender : KeymapPreset::Patina, true);
        navPreset = k == 0 ? NavPreset::Blender : k == 1 ? NavPreset::Maya : NavPreset::Patina;
        saveSession(true);
      }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T_(tips[k]));
    }
  }
  // where the textures go: the export preset
  ImGui::SeparatorText(T_("Your textures go to"));
  {
    const char* names[] = {N_("Blender"), N_("glTF"), N_("Unreal"), N_("Unity")};
    for (int k = 0; k < 4; ++k) {
      if (k) ImGui::SameLine();
      if (ImGui::RadioButton((std::string(T_(names[k])) + "##target").c_str(), exportPresetIdx == k)) exportPresetIdx = k, saveSession(true);
    }
  }
  if (engine.features.on("guided_layout")) {
    ImGui::SeparatorText(T_("Layout"));
    const bool guided = layoutMode == LayoutMode::Guided;
    if (ImGui::RadioButton(L_("Guided: the work in five steps"), guided)) setLayoutMode(LayoutMode::Guided);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", T_("Model, UVs, Bake, Paint, Export along the top, each with what it needs in front"));
    if (ImGui::RadioButton(L_("Full: every panel"), !guided && layoutMode != LayoutMode::Blender)) setLayoutMode(LayoutMode::Full);
    if (ImGui::RadioButton(L_("Blender: the panels where Blender has them"), layoutMode == LayoutMode::Blender))
      setLayoutMode(LayoutMode::Blender);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", T_("Arranged as Blender is: the steps as workspace tabs, the Outliner and the Properties\n"
                                 "editor at the right, the asset shelf under the 3D view."));
  }
  ImGui::TextDisabled("%s", T_("Change these any time: Edit > Preferences, and Guided / Full / Blender at the right of the menu bar."));
  ImGui::EndGroup();
  ImGui::SameLine();
  ImGui::BeginGroup();
  ImGui::SeparatorText(T_("Recent"));
  int shown = 0;
  for (auto& f : recentFiles) {
    if (shown >= 8) break;
    if (!fs::exists(f, ec)) continue;
    ++shown;
    std::string label = fs::path(f).filename().string() + "##" + f;
    if (ImGui::Selectable(label.c_str(), false, 0, ImVec2(w, 0))) {
      showWelcome = false;
      std::string path = f;
      replaceProject(T_("open another file"), [this, path] {
        std::string err;
        if (!engine.openFile(path, &err)) showError(F_("Could not open %s", fs::path(path).filename().string().c_str()), err);
      });
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", f.c_str());
  }
  if (!shown) ImGui::TextDisabled("%s", T_("Nothing yet."));
  ImGui::EndGroup();
  ImGui::Separator();
  if (ImGui::Button(L_("Read the quick start"))) openLocalPage("manual");
  ImGui::SameLine();
  ImGui::Checkbox(L_("Show this window on start"), &showWelcomeOnStart);
  ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - 80 * uiScale);
  if (ImGui::Button(L_("Close"), ImVec2(80 * uiScale, 0))) showWelcome = false;
  ImGui::End();
}

// ------------------------------------------------------------------ file watching / recovery

void App::checkMeshFile() {
  Document& d = engine.doc;
  if (nowSeconds() - lastMeshCheck < 1.0) return;
  lastMeshCheck = nowSeconds();
  if (meshChangedPending || d.bridge.active || d.meshPath.empty() || !d.meshFileTime) return;
  int64_t t = Document::fileTime(d.meshPath);
  if (t == 0 || t == d.meshFileTime) return;
  // wait until the writer is done (size stable for a moment)
  static int64_t seen = 0;
  if (t != seen) {
    seen = t;
    return;
  }
  if (autoReloadMesh) {
    json r;
    if (runCommand("reload_mesh", json::object(), &r)) setStatus(T_("Reloaded the changed mesh (layers kept)"));
    d.meshFileTime = Document::fileTime(d.meshPath);
    return;
  }
  meshChangedPending = true;
}

// One marker per running instance (running-<pid>.marker), removed on a clean exit.
// A marker whose process is gone means that instance crashed.
static int currentPid() {
#ifdef _WIN32
  return int(GetCurrentProcessId());
#else
  return int(getpid());
#endif
}
static bool processAlive(int pid) {
  if (pid <= 0) return false;
#ifdef _WIN32
  HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, DWORD(pid));
  if (!h) return false;
  bool alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
  CloseHandle(h);
  return alive;
#else
  return kill(pid_t(pid), 0) == 0 || errno == EPERM;
#endif
}
static fs::path markerPath(int pid) { return paths::cacheDir() / ("running-" + std::to_string(pid) + ".marker"); }

void App::startRecoveryCheck() {
  std::error_code ec;
  fs::path dir = paths::ensureDir(paths::cacheDir());
  fs::path autosave = dir / "autosave.patina";
  bool crashed = false;
  for (auto& e : fs::directory_iterator(dir, ec)) {
    std::string n = e.path().filename().string();
    if (n.rfind("running-", 0) != 0 || e.path().extension() != ".marker") continue;
    int pid = std::atoi(n.c_str() + 8);
    if (processAlive(pid)) continue;  // another Patina window is open
    crashed = true;
    fs::remove(e.path(), ec);
  }
  fs::remove(dir / "running.marker", ec);  // 0.4 pre-release name
  if (crashed && fs::exists(autosave, ec)) {
    recoveryPending = true;
    recoveryPath = autosave.string();
  }
  paths::writeFileAtomic(markerPath(currentPid()), "patina is running\n");
}

void App::endRecoveryCheck() {
  std::error_code ec;
  fs::remove(markerPath(currentPid()), ec);  // a clean exit
}

void App::drawFilePrompts() {
  if (meshChangedPending) ImGui::OpenPopup("Mesh changed on disk");
  if (ImGui::BeginPopupModal(L_("Mesh changed on disk"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::Text(T_("%s was changed by another program."), fs::path(engine.doc.meshPath).filename().string().c_str());
    ImGui::TextDisabled("%s", T_("Updating keeps your layers: paint follows changed UVs, and is transferred\n"
                                 "through 3D when the topology changed."));
    if (ImGui::Button(L_("Update the mesh"))) {
      json r;
      if (runCommand("reload_mesh", json::object(), &r)) setStatus(T_("Mesh updated (layers kept)"));
      engine.doc.meshFileTime = Document::fileTime(engine.doc.meshPath);
      meshChangedPending = false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(L_("Not now"))) {
      engine.doc.meshFileTime = Document::fileTime(engine.doc.meshPath);
      meshChangedPending = false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(L_("Always update"))) {
      autoReloadMesh = true;
      json r;
      runCommand("reload_mesh", json::object(), &r);
      engine.doc.meshFileTime = Document::fileTime(engine.doc.meshPath);
      meshChangedPending = false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
  if (recoveryPending && !meshChangedPending) ImGui::OpenPopup("Recover your work?");
  if (ImGui::BeginPopupModal(L_("Recover your work?"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    std::error_code ec;
    auto t = fs::last_write_time(recoveryPath, ec);
    auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(t - fs::file_time_type::clock::now() +
                                                                                 std::chrono::system_clock::now());
    std::time_t tt = std::chrono::system_clock::to_time_t(sys);
    char when[64] = "";
    std::strftime(when, sizeof when, "%Y-%m-%d %H:%M", std::localtime(&tt));
    ImGui::TextUnformatted(T_("Patina did not close properly last time."));
    ImGui::Text(T_("An autosave from %s is available."), when);
    if (ImGui::Button(L_("Restore it"))) {
      std::string err;
      if (engine.openFile(recoveryPath, &err)) {
        engine.doc.projectPath.clear();
        engine.doc.dirty = true;
        setStatus(T_("Restored the autosave - use Save As to keep it"));
        showWelcome = false;
      } else {
        showError(T_("Could not restore the autosave"), err);
      }
      recoveryPending = false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(L_("Start fresh"))) {
      recoveryPending = false;
      ImGui::CloseCurrentPopup();
    }
    ImGui::TextDisabled("%s", T_("It stays available in File > Open Autosave until the next autosave."));
    ImGui::EndPopup();
  }
}

// ------------------------------------------------------------------ file actions

void App::saveIncremental() {
  Document& d = engine.doc;
  if (d.projectPath.empty()) {
    saveProject(true);
    return;
  }
  json r;
  if (runCommand("save_incremental", json::object(), &r)) {
    std::string p = r.value("path", std::string());
    setStatus(F_("Saved %s", p.c_str()));
    addRecentFile(p);
  }
}

void App::exportAgain() {
  if (!engine.doc.hasLastExport) {
    exportWhat = 0;
    showExport = true;
    setStatus(T_("Nothing exported yet in this project - choose the settings once"));
    return;
  }
  json r;
  if (runCommand("export_again", json::object(), &r))
    setStatus(F_("Exported %zu maps to %s", r["files"].size(), r.value("directory", std::string()).c_str()));
}

}  // namespace patina
