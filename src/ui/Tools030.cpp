// Patina 0.30 windows: recipes and batches (File > Batch Process), and Mesh > Transform.
#include <imgui.h>
#include <imgui_stdlib.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <filesystem>

#include "core/Paths.h"
#include "engine/Recipes.h"
#include "ui/App.h"

namespace patina {

namespace fs = std::filesystem;

namespace {
// "mesh_fix {all}" style one-liners for a recipe's steps
std::string stepLabel(const json& st) {
  std::string cmd = st.value("cmd", std::string("?"));
  static const std::pair<const char*, const char*> names[] = {
      {"mesh_fix", N_("Mend the mesh")},         {"mesh_reduce", N_("Reduce polygons")},
      {"mesh_transform", N_("Transform")},       {"bake_mesh_maps", N_("Bake mesh maps")},
      {"auto_texture", N_("Auto texture")},      {"apply_smart_material", N_("Smart material")},
      {"add_layer", N_("Add a layer")},          {"export_model", N_("Export the model")},
      {"export_textures", N_("Export textures")}, {"render_image", N_("Render an image")},
      {"save_project", N_("Save the project")},  {"uv_unwrap", N_("Unwrap")},
      {"set_resolution", N_("Texture size")},    {"uv_pack", N_("Pack UVs")},
      {"mesh_collision", N_("Collision shapes")}, {"mesh_lightmap_uvs", N_("Lightmap UVs")},
      {"import_model_textures", N_("The model's textures")}};
  std::string label = cmd;
  for (auto& [c, n] : names)
    if (cmd == c) label = T_(n);
  json args = st.value("args", json::object());
  std::string detail;
  for (auto it = args.begin(); it != args.end(); ++it) {
    std::string v = it.value().is_string() ? it.value().get<std::string>() : it.value().dump();
    if (v.size() > 40) v = "..." + v.substr(v.size() - 37);
    detail += (detail.empty() ? "" : ", ") + it.key() + " " + v;
  }
  return detail.empty() ? label : label + " (" + detail + ")";
}
}  // namespace

void App::drawBatchWindow() {
  if (!showBatch) return;
  if (!engine.features.on("recipes")) {
    showBatch = false;
    return;
  }
  ImGui::SetNextWindowSizeConstraints(ImVec2(560 * uiScale, 0), ImVec2(760 * uiScale, FLT_MAX));
  if (!ImGui::Begin(L_("Batch process"), &showBatch, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse |
                                                         ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::End();
    return;
  }
  std::vector<Recipe> recipes = listRecipes();
  auto cur = std::find_if(recipes.begin(), recipes.end(), [&](const Recipe& r) { return r.id == batchRecipe; });
  if (cur == recipes.end() && !recipes.empty()) cur = recipes.begin(), batchRecipe = cur->id;
  ImGui::TextWrapped("%s", T_("A recipe is a list of steps: mend, reduce, texture, export... Run it on the open model, or on "
                              "many files at once. A batch runs in a hidden Patina, so you can keep working, and the files "
                              "themselves are never changed: the results go to the output folder."));
  ImGui::SeparatorText(T_("Recipe"));
  ImGui::SetNextItemWidth(360 * uiScale);
  if (ImGui::BeginCombo("##recipe", cur != recipes.end() ? cur->name.c_str() : T_("(none)"))) {
    for (auto& r : recipes)
      if (ImGui::Selectable(((r.builtin ? r.name : F_("%s  (yours)", r.name.c_str())) + "##" + r.id).c_str(), r.id == batchRecipe)) {
        batchRecipe = r.id;
        batchEditing = false;
      }
    ImGui::EndCombo();
  }
  if (cur != recipes.end()) {
    ImGui::SameLine();
    if (ImGui::SmallButton(batchEditing ? L_("Close the editor") : L_("Edit / copy"))) {
      batchEditing = !batchEditing;
      batchEditName = cur->builtin ? F_("%s (mine)", cur->name.c_str()) : cur->name;
      batchEditDesc = cur->description;
      batchEditSteps = cur->steps.dump(1);
    }
    if (!cur->builtin) {
      ImGui::SameLine();
      if (ImGui::SmallButton(L_("Delete"))) {
        runCommand("recipe_delete", {{"id", cur->id}});
        batchRecipe.clear();
      }
    }
    if (!cur->description.empty()) ImGui::TextDisabled("%s", cur->description.c_str());
    for (size_t i = 0; i < cur->steps.size(); ++i) ImGui::BulletText("%s", stepLabel(cur->steps[i]).c_str());
    if (batchEditing) {
      ImGui::SeparatorText(T_("Your version"));
      ImGui::SetNextItemWidth(360 * uiScale);
      ImGui::InputText(L_("Name"), &batchEditName);
      ImGui::SetNextItemWidth(360 * uiScale);
      ImGui::InputText(L_("What it is for"), &batchEditDesc);
      ImGui::TextDisabled("%s", T_("Steps: Patina commands (see the manual's Commands page). ${name} ${file} ${dir} ${out}"));
      ImGui::InputTextMultiline("##steps", &batchEditSteps, ImVec2(640 * uiScale, 220 * uiScale));
      if (ImGui::Button(L_("Save as my recipe"))) {
        json steps = json::parse(batchEditSteps, nullptr, false);
        json r;
        if (steps.is_discarded() || !steps.is_array()) batchMessage = T_("The steps are not a JSON list: [{\"cmd\": ..., \"args\": {...}}]");
        else if (runCommand("recipe_save", {{"name", batchEditName}, {"description", batchEditDesc}, {"steps", steps}}, &r))
          batchRecipe = r.value("id", batchRecipe), batchEditing = false, batchMessage = F_("Saved %s", batchEditName.c_str());
      }
    }
    ImGui::Spacing();
    if (ImGui::Button(L_("Apply to the open model"), ImVec2(220 * uiScale, 0))) {
      json r;
      if (runCommand("recipe_apply", {{"id", cur->id}}, &r))
        batchMessage = F_("Done: %zu steps on the open model.", r["steps"].size());
    }
    ImGui::SetItemTooltip("%s", T_("Runs the steps here, on the model you have open (exports go beside its file)"));
  }

  ImGui::SeparatorText(T_("Files"));
  if (ImGui::Button(L_("Add files..."))) openFileDialog(DialogPurpose::BatchFiles);
  ImGui::SameLine();
  if (ImGui::Button(L_("Add a folder..."))) openFileDialog(DialogPurpose::BatchFolder);
  ImGui::SameLine();
  ImGui::Checkbox(L_("Its subfolders too"), &batchRecursive);
  if (!batchPaths.empty()) {
    ImGui::SameLine();
    if (ImGui::SmallButton(L_("Clear"))) batchPaths.clear();
  }
  for (size_t i = 0; i < batchPaths.size(); ++i) {
    ImGui::PushID((int)i);
    if (ImGui::SmallButton("x")) {
      batchPaths.erase(batchPaths.begin() + i);
      ImGui::PopID();
      break;
    }
    ImGui::SameLine();
    std::error_code ec;
    std::string shown = fs::is_directory(batchPaths[i], ec) ? F_("%s  (folder)", batchPaths[i].c_str()) : batchPaths[i];
    ImGui::TextUnformatted(shown.c_str());
    ImGui::PopID();
  }
  if (batchPaths.empty()) ImGui::TextDisabled("%s", T_("Add model files or folders (.glb .gltf .obj .fbx .stl .3mf .ply .patina)"));
  ImGui::SetNextItemWidth(420 * uiScale);
  ImGui::InputTextWithHint("##out", T_("Output folder (default: patina_out beside the first file)"), &batchOutput);
  ImGui::SameLine();
  if (ImGui::Button("...##out")) openFileDialog(DialogPurpose::BatchOutput);

  if (!batchState.is_object()) batchState = json::object();
  bool running = batchState.value("running", false);
  double now = ImGui::GetTime();
  if (engine.batch && now - batchPolled > 0.5) {
    batchPolled = now;
    try {
      batchState = engine.execute("batch_status", json::object());
    } catch (std::exception&) {
    }
  }
  ImGui::BeginDisabled(running || batchPaths.empty() || cur == recipes.end());
  if (ImGui::Button(L_("Run the batch"), ImVec2(220 * uiScale, 0))) {
    json args = {{"recipe", batchRecipe}, {"recursive", batchRecursive}};
    json files = json::array(), folders = json::array();
    for (auto& p : batchPaths) {
      std::error_code ec;
      (fs::is_directory(p, ec) ? folders : files).push_back(p);
    }
    args["files"] = files, args["folders"] = folders;
    if (!batchOutput.empty()) args["output"] = batchOutput;
    json r;
    if (runCommand("batch_run", args, &r)) {
      batchMessage = F_("Started: %zu files, results in %s", r["files"].size(), r.value("output", std::string()).c_str());
      batchPolled = 0;
      batchState = json::object({{"running", true}});
    }
  }
  {
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    engine.uiRects["batch.run"] = {a.x, a.y, b.x - a.x, b.y - a.y};  // (for tests)
  }
  ImGui::EndDisabled();
  if (running) {
    ImGui::SameLine();
    if (ImGui::Button(L_("Stop after this file"))) runCommand("batch_cancel", json::object());
  }
  if (batchState.is_object() && batchState.contains("files")) {
    size_t done = batchState.value("done", (size_t)0), total = std::max<size_t>(1, batchState.value("files", (size_t)1));
    std::string label = running ? F_("%zu of %zu files", done, total) : F_("%zu of %zu files - finished", done, total);
    ImGui::ProgressBar(float(done) / float(total), ImVec2(-1, 0), label.c_str());
    size_t failed = batchState.value("failed", (size_t)0);
    if (!running && failed) ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), T_("%zu files failed (see below)"), failed);
    if (!running && ImGui::Button(L_("Open the output folder"))) {
      std::error_code ec;
      std::string p = fs::weakly_canonical(batchState.value("output", std::string()), ec).generic_string();
#ifdef _WIN32
      std::string url = "file:///" + p;
#else
      std::string url = "file://" + p;
#endif
      for (size_t i = 0; i < url.size(); ++i)
        if (url[i] == ' ') url.replace(i, 1, "%20");
      if (!SDL_OpenURL(url.c_str())) setStatus(F_("Could not open %s", p.c_str()));
    }
    if (ImGui::BeginTable("##batchres", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY,
                          ImVec2(0, std::min(220.f, 26.f + 22.f * (float)batchState["results"].size()) * uiScale))) {
      ImGui::TableSetupColumn(L_("File"));
      ImGui::TableSetupColumn("");
      ImGui::TableSetupColumn(L_("Seconds"));
      ImGui::TableHeadersRow();
      for (auto& f : batchState["results"]) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(), ImGui::TextUnformatted(fs::path(f.value("file", std::string())).filename().string().c_str());
        ImGui::TableNextColumn();
        if (f.value("ok", false)) ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.5f, 1), "%s", T_("done"));
        else {
          ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), "%s", T_("failed"));
          if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", f.value("error", std::string()).c_str());
        }
        ImGui::TableNextColumn(), ImGui::Text("%.1f", f.value("seconds", 0.0));
      }
      ImGui::EndTable();
    }
  }
  if (!batchMessage.empty()) ImGui::TextWrapped("%s", batchMessage.c_str());
  ImGui::End();
}

void App::drawTransformWindow() {
  if (!showTransform) return;
  if (!engine.features.on("recipes") || !engine.doc.hasMesh()) {
    showTransform = false;
    return;
  }
  ImGui::SetNextWindowSizeConstraints(ImVec2(400 * uiScale, 0), ImVec2(560 * uiScale, FLT_MAX));
  if (!ImGui::Begin(L_("Transform"), &showTransform, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse |
                                                         ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::End();
    return;
  }
  Document& d = engine.doc;
  vec3 sz = d.mesh.bounds.valid() ? d.mesh.bounds.hi - d.mesh.bounds.lo : vec3(0);
  ImGui::Text(T_("Now %.4g x %.4g x %.4g (Blender X Y Z)"), sz.x, sz.z, sz.y);
  ImGui::SetNextItemWidth(200 * uiScale);
  ImGui::Combo(L_("It was made in"), &transformUnits, Z_("meters (as Blender)\0centimeters\0millimeters\0inches\0feet\0"));
  ImGui::SetNextItemWidth(200 * uiScale);
  ImGui::DragFloat(L_("Then scale by"), &transformScale, 0.01f, 0.0001f, 10000.f, "%.4g");
  ImGui::SetNextItemWidth(200 * uiScale);
  ImGui::DragFloat3(L_("Turn (degrees, X Y Z)"), &transformRotate.x, 1.f, -360.f, 360.f, "%.0f");
  ImGui::SetNextItemWidth(200 * uiScale);
  ImGui::Combo(L_("Origin"), &transformOrigin, Z_("Keep\0Center of its box\0Center of its bottom\0"));
  ImGui::Checkbox(L_("Stand it on the ground (lowest point at 0)"), &transformGround);
  if (ImGui::Button(L_("Apply"), ImVec2(160 * uiScale, 0))) {
    static const char* units[] = {"m", "cm", "mm", "in", "ft"};
    static const char* origins[] = {"keep", "center", "base"};
    json r;
    if (runCommand("mesh_transform", {{"from_units", units[transformUnits]}, {"scale", transformScale},
                                      {"rotate", {transformRotate.x, transformRotate.y, transformRotate.z}},
                                      {"origin", origins[transformOrigin]}, {"ground", transformGround}},
                   &r))
      setStatus(r.value("changed", false) ? T_("Transformed the model") : T_("Nothing to change"));
  }
  if (d.canRevertMeshFix() && d.lastMeshChange() == "transform") {
    ImGui::SameLine();
    if (ImGui::Button(L_("Back to before"))) runCommand("mesh_fix_revert", json::object());
  }
  ImGui::TextDisabled("%s", T_("The paint stays; fill layers projected in 3D follow the new size."));
  ImGui::End();
}

}  // namespace patina
