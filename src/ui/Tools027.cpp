// Patina 0.27 windows: the Mesh Check (numbers, problems, fixes).
#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <filesystem>

#include "core/Paths.h"
#include "ui/App.h"

namespace patina {

void App::drawMeshCheckWindow() {
  if (!showMeshCheck) return;
  if (!engine.features.on("mesh_check")) {
    showMeshCheck = false;
    return;
  }
  ImGui::SetNextWindowSizeConstraints(ImVec2(460 * uiScale, 0), ImVec2(600 * uiScale, FLT_MAX));
  if (!ImGui::Begin(L_("Mesh check"), &showMeshCheck, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse |
                                                          ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::End();
    return;
  }
  Document& d = engine.doc;
  if (!d.hasMesh()) {
    ImGui::TextDisabled("%s", T_("Open a model first."));
    ImGui::End();
    return;
  }
  const MeshCheckReport& r = d.meshCheck();
  const MeshStats& st = r.stats;
  bool on = engine.viewOptions.mode == ViewMode::MeshCheck;
  if (ImGui::Checkbox(L_("Show the problems on the model"), &on)) engine.viewOptions.mode = on ? ViewMode::MeshCheck : ViewMode::Material;
  ImGui::TextDisabled("%s", T_("red: a problem   orange: a warning   blue: a note   magenta: the back of a face"));

  ImGui::SeparatorText(T_("The model"));
  if (ImGui::BeginTable("##meshstats", 4, ImGuiTableFlags_SizingFixedFit)) {
    auto row = [&](const char* a, const std::string& av, const char* b, const std::string& bv) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn(), ImGui::TextDisabled("%s", a);
      ImGui::TableNextColumn(), ImGui::TextUnformatted(av.c_str());
      ImGui::TableNextColumn(), ImGui::TextDisabled("%s", b);
      ImGui::TableNextColumn(), ImGui::TextUnformatted(bv.c_str());
    };
    auto n = [](double v) {
      char b[32];
      if (v >= 1e6) std::snprintf(b, sizeof b, "%.2fM", v / 1e6);
      else if (v >= 1e4) std::snprintf(b, sizeof b, "%.1fk", v / 1e3);
      else std::snprintf(b, sizeof b, "%.0f", v);
      return std::string(b);
    };
    char sz[64];
    std::snprintf(sz, sizeof sz, "%.3g x %.3g x %.3g", st.size.x, st.size.z, st.size.y);  // Blender's X, Y, Z
    row(T_("Vertices"), n(st.vertices), T_("Faces"), n(st.faces));
    row(T_("Triangles drawn"), n((double)st.trianglesRendered), T_("Parts"), n(st.parts));
    char mix[64];
    std::snprintf(mix, sizeof mix, "%s / %s / %s", n(st.triangles).c_str(), n(st.quads).c_str(), n(st.ngons).c_str());
    row(T_("Tris / quads / n-gons"), mix, T_("Material slots"), n(st.slots));
    char area[32], vol[32], cov[32];
    std::snprintf(area, sizeof area, "%.4g", st.area);
    std::snprintf(vol, sizeof vol, "%.4g", st.volume);
    std::snprintf(cov, sizeof cov, T_("%.0f%%"), st.uvCoverage * 100.f);
    row(T_("Size (m, Blender X Y Z)"), sz, T_("UV islands"), n(st.uvIslands));
    row(T_("Surface area"), area, st.closed ? T_("Volume") : T_("Volume (open)"), vol);
    row(T_("Closed"), st.closed ? T_("yes") : T_("no"), T_("UV space used"), cov);
    ImGui::EndTable();
  }

  ImGui::SeparatorText(r.problems.empty() ? T_("No problems found") : T_("Problems"));
  if (r.problems.empty()) ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.5f, 1), "%s", T_("Clean: nothing to mend."));
  MeshFixOptions one;
  for (size_t i = 0; i < r.problems.size(); ++i) {
    const MeshProblem& p = r.problems[i];
    ImGui::PushID((int)i);
    ImVec4 col = p.severity == 2 ? ImVec4(1, 0.42f, 0.38f, 1) : p.severity == 1 ? ImVec4(1, 0.7f, 0.3f, 1)
                                                                               : ImVec4(0.5f, 0.68f, 1, 1);
    if (p.unit.empty()) ImGui::TextColored(col, "%s", p.name.c_str());
    else ImGui::TextColored(col, "%s: %zu %s", p.name.c_str(), p.count, p.unit.c_str());
    ImGui::SameLine();
    if (!p.faces.empty() && ImGui::SmallButton(L_("Select"))) {
      runCommand("mesh_check", {{"select", p.id}});
      setStatus(F_("%zu faces selected: %s", p.faces.size(), p.name.c_str()));
    }
    if (!p.where.empty()) {
      ImGui::SameLine();
      if (ImGui::SmallButton(L_("Look"))) {
        float rad = std::max(glm::length(d.mesh.bounds.size()) * 0.08f, 1e-4f);
        AABB box;
        box.lo = engine.toWorld(p.where[0]) - vec3(rad), box.hi = engine.toWorld(p.where[0]) + vec3(rad);
        engine.camera.frame(box, rad);
        engine.viewOptions.mode = ViewMode::MeshCheck;
      }
    }
    if (!p.fix.empty()) {
      ImGui::SameLine();
      if (ImGui::SmallButton(L_("Fix"))) {
        json args = {{p.fix, true}};
        if (p.fix == "weld") args["weld_distance"] = meshFixWeld;
        if (p.fix == "fill_holes") args["fill_hole_edges"] = meshFixHoleEdges;
        json res;
        if (runCommand("mesh_fix", args, &res)) meshFixStatus = describeMeshFix(res);
      }
      {
        ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        engine.uiRects["meshcheck.fix:" + p.id] = {a.x, a.y, b.x - a.x, b.y - a.y};  // (for tests)
      }
    }
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 540 * uiScale);
    ImGui::TextDisabled("%s", p.what.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopID();
  }
  bool anyFix = std::any_of(r.problems.begin(), r.problems.end(), [](const MeshProblem& p) { return !p.fix.empty(); });
  if (anyFix) {
    ImGui::Spacing();
    if (ImGui::Button(L_("Fix everything that can be fixed"), ImVec2(-1, 0))) {
      json args = {{"all", true}, {"weld_distance", meshFixWeld}, {"fill_hole_edges", meshFixHoleEdges}};
      json res;
      if (runCommand("mesh_fix", args, &res)) meshFixStatus = describeMeshFix(res);
    }
    {
      ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
      engine.uiRects["meshcheck.fix_all"] = {a.x, a.y, b.x - a.x, b.y - a.y};  // (for tests)
    }
    if (ImGui::TreeNode(L_("Fix settings"))) {
      ImGui::SetNextItemWidth(160 * uiScale);
      ImGui::DragFloat(L_("Weld distance"), &meshFixWeld, std::max(glm::length(d.mesh.bounds.size()), 1e-3f) * 1e-5f, 0.f,
                       1e3f, meshFixWeld <= 0.f ? T_("automatic") : "%.6f");
      ImGui::SetItemTooltip("%s", T_("Vertices closer than this are joined. Automatic: 1/100000 of the model's size"));
      ImGui::SetNextItemWidth(160 * uiScale);
      ImGui::SliderInt(L_("Fill holes up to"), &meshFixHoleEdges, 3, 512, T_("%d edges"));
      ImGui::TreePop();
    }
    if (d.bridge.active)
      ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), T_("Linked to Blender (%s): a mended mesh no longer matches its object."),
                         d.bridge.object.c_str());
    ImGui::TextDisabled("%s", T_("Fixing keeps the paint, but clears Undo: Patina keeps a copy from before."));
  }
  if (!meshFixStatus.empty()) {
    ImGui::Separator();
    ImGui::TextWrapped("%s", meshFixStatus.c_str());
  }
  if (d.canRevertMeshFix() && d.bridge.active && engine.features.on("model_export")) {
    if (ImGui::Button(L_("Send the mended model to Blender"))) {
      json res;
      if (runCommand("send_model_to_blender", json::object(), &res))
        meshFixStatus = F_("Blender has the mended model as %s, with its textures. Patina is linked to it now.",
                           res.value("object", std::string()).c_str());
    }
    ImGui::SetItemTooltip(T_("A new object next to %s, with the mended mesh and its textures"), d.bridge.object.c_str());
    ImGui::SameLine();
  }
  if (d.canRevertMeshFix()) {
    if (ImGui::Button(d.lastMeshChange() == "reduction" ? L_("Back to before the reduction") : L_("Back to before the fix"))) {
      if (runCommand("mesh_fix_revert", json::object())) meshFixStatus = T_("Back to the mesh as it was before the fix.");
    }
    {
      ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
      engine.uiRects["meshcheck.revert"] = {a.x, a.y, b.x - a.x, b.y - a.y};  // (for tests)
    }
  }
  ImGui::End();
}

std::string App::describeMeshFix(const json& r) {
  if (!r.value("changed", false)) return T_("Nothing to mend.");
  std::string list;  // "3 vertices joined, 2 holes closed"
  auto part = [&](const std::string& what) { list += (list.empty() ? "" : ", ") + what; };
  auto count = [&](const char* key) { return r.value(key, (size_t)0); };
  if (size_t n = count("welded")) part(F_("%zu vertices joined", n));
  if (size_t n = count("degenerate_removed")) part(F_("%zu empty faces removed", n));
  if (size_t n = count("duplicates_removed")) part(F_("%zu duplicate faces removed", n));
  if (size_t n = count("loose_removed")) part(F_("%zu loose vertices removed", n));
  if (size_t n = count("small_parts_removed")) part(F_("%zu specks removed", n));
  if (size_t n = count("faces_turned")) part(F_("%zu faces turned", n));
  if (size_t n = count("holes_filled")) part(F_("%zu holes closed", n));
  return F_("Mended: %s. The paint moved to the new mesh.", list.c_str());
}

}  // namespace patina

namespace patina {

namespace fs = std::filesystem;

void App::drawModelExportWindow() {
  if (!showModelExport) return;
  if (!engine.features.on("model_export") || !engine.doc.hasMesh()) {
    showModelExport = false;
    return;
  }
  ImGui::SetNextWindowSizeConstraints(ImVec2(440 * uiScale, 0), ImVec2(600 * uiScale, FLT_MAX));
  if (!ImGui::Begin(L_("Export model"), &showModelExport, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse |
                                                              ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::End();
    return;
  }
  Document& d = engine.doc;
  if (modelExportDir.empty()) modelExportDir = (paths::homeDir() / "patina_export").string();
  if (modelExportName.empty()) modelExportName = d.mesh.name.empty() ? std::string("model") : d.mesh.name;
  ImGui::TextWrapped("%s", T_("The model with its textures, ready for an engine, a viewer or another program."));
  ImGui::SetNextItemWidth(320 * uiScale);
  ImGui::InputText(L_("Folder"), &modelExportDir);
  ImGui::SameLine();
  if (ImGui::SmallButton("...##mdir")) openFileDialog(DialogPurpose::ModelExportDir);
  ImGui::SetNextItemWidth(320 * uiScale);
  ImGui::InputText(L_("Name"), &modelExportName);
  ImGui::SetNextItemWidth(320 * uiScale);
  ImGui::Combo(L_("Format"), &modelExportFormat, Z_("GLB (one file, textures inside)\0glTF (.gltf, .bin and PNG files)\0OBJ (.obj, .mtl and PNG files)\0STL (3D printing: mm, no textures)\0PLY (the base color as vertex colors)\0"));
  static const int sizes[] = {0, 512, 1024, 2048, 4096};
  int si = 0;
  for (int i = 0; i < 5; ++i)
    if (sizes[i] == modelExportSize) si = i;
  ImGui::SetNextItemWidth(320 * uiScale);
  if (ImGui::Combo(L_("Textures"), &si, Z_("The texture set's size\0" "512\0" "1024\0" "2048\0" "4096\0"))) modelExportSize = sizes[si];
  static const char* exts[] = {".glb", ".gltf", ".obj", ".stl", ".ply"};
  fs::path out = fs::path(modelExportDir) / (paths::sanitizeFileName(modelExportName) + exts[modelExportFormat]);
  if (engine.features.on("mesh_reduce")) {
    ImGui::SetNextItemWidth(320 * uiScale);
    ImGui::SliderInt(L_("Levels of detail"), &modelExportLods, 0, 4, modelExportLods == 0 ? T_("none") : "%d");
    ImGui::SetItemTooltip("%s", T_("Lighter copies that share the textures: each has half the triangles of the one before.\n"
                                "glTF: meshes named _LOD0, _LOD1... (and MSFT_lod); OBJ: name_LOD1.obj..."));
  }
  static const char* collisionNames[] = {"none", "unreal", "godot", "plain"};
  if (engine.features.on("collision") && modelExportFormat <= 2) {
    ImGui::SetNextItemWidth(320 * uiScale);
    ImGui::Combo(L_("Collision"), &modelExportCollision,
                 Z_("None\0For Unreal (UCX_ and UBX_ names)\0For Godot (-convcolonly names)\0Plain names (_collision_)\0"));
    ImGui::SetItemTooltip("%s", T_("The shapes from Mesh > Collision after the model (made now with its settings if there are none)"));
  }
  // (1.2) a scene: every object shown, or the active one
  int shownObjects = 0;
  for (int i = 0; i < engine.objectCount(); ++i) shownObjects += !engine.object(i).objectHidden || i == engine.activeObject();
  if (engine.features.on("scene_objects") && shownObjects > 1) {
    ImGui::RadioButton((F_("Every object (%d)", shownObjects) + "###mexp_all").c_str(), &modelExportObjects, 0);
    ImGui::SameLine();
    ImGui::RadioButton((F_("Only %s", d.displayName().c_str()) + "###mexp_one").c_str(), &modelExportObjects, 1);
    ImGui::SetItemTooltip("%s", T_("Every object goes in one file, each where it is in the scene (glTF: a node each),\n"
                                   "with its own materials and textures"));
  }
  ImGui::TextDisabled("%s", out.string().c_str());
  if (ImGui::Button(L_("Export"), ImVec2(160 * uiScale, 0))) {
    json r;
    json lods = json::array();
    for (int i = 0; i < modelExportLods; ++i) lods.push_back(std::pow(0.5, i + 1));
    json args = {{"path", out.string()}, {"texture_size", modelExportSize}, {"lods", lods},
                 {"objects", modelExportObjects == 1 ? "active" : "all"}};
    if (engine.features.on("collision") && modelExportFormat <= 2 && modelExportCollision > 0)
      args["collision"] = collisionNames[std::clamp(modelExportCollision, 0, 3)];
    if (runCommand("export_model", args, &r)) {
      int m = r.contains("objects") ? (int)r["objects"].size() : r.value("materials", 1);
      if (r.contains("objects")) {
        modelExportStatus = F_("Exported %s (%d objects)", out.filename().string().c_str(), m);
      } else {
        char buf[640];
        std::snprintf(buf, sizeof buf, P_("Exported %s (%d material)", "Exported %s (%d materials)", m),
                      out.filename().string().c_str(), m);
        modelExportStatus = buf;
      }
    }
  }
  {
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    engine.uiRects["model_export.export"] = {a.x, a.y, b.x - a.x, b.y - a.y};  // (for tests)
  }
  if (!modelExportStatus.empty()) ImGui::TextWrapped("%s", modelExportStatus.c_str());
  ImGui::End();
}

}  // namespace patina

namespace patina {

void App::drawReduceWindow() {
  if (!showReduce) return;
  if (!engine.features.on("mesh_reduce") || !engine.doc.hasMesh()) {
    showReduce = false;
    return;
  }
  ImGui::SetNextWindowSizeConstraints(ImVec2(420 * uiScale, 0), ImVec2(560 * uiScale, FLT_MAX));
  if (!ImGui::Begin(L_("Reduce polygons"), &showReduce, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse |
                                                             ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::End();
    return;
  }
  Document& d = engine.doc;
  ImGui::TextWrapped("%s", T_("Fewer triangles for games, the web and AR. The UV layout stays, so the textures fit as they are; "
                              "UV seams, material borders and (if ticked) open borders don't move."));
  ImGui::SetNextItemWidth(260 * uiScale);
  ImGui::SliderFloat(L_("Keep"), &reducePercent, 1.f, 99.f, T_("%.0f%% of the triangles"));
  bool released = ImGui::IsItemDeactivatedAfterEdit();
  if (ImGui::Checkbox(L_("Keep open borders"), &reduceKeepBorders)) released = true;
  ImGui::SetItemTooltip("%s", T_("Edges with one face (clothes, planes, cut-offs) don't move"));
  if (released || reducePreviewFor < 0.f) {
    json r;
    if (runCommand("mesh_reduce", {{"ratio", reducePercent / 100.f}, {"keep_borders", reduceKeepBorders}, {"preview", true}}, &r))
      reducePreview = r, reducePreviewFor = reducePercent;
  }
  if (reducePreview.is_object())
    ImGui::Text(T_("%zu triangles -> %zu (the shape moves %.2f%% of its size)"), reducePreview.value("triangles_before", (size_t)0),
                reducePreview.value("triangles_after", (size_t)0), reducePreview.value("error", 0.f) * 100.f);
  if (ImGui::Button(L_("Reduce"), ImVec2(160 * uiScale, 0))) {
    json r;
    if (runCommand("mesh_reduce", {{"ratio", reducePercent / 100.f}, {"keep_borders", reduceKeepBorders}}, &r)) {
      reduceStatus = r.value("changed", false)
                         ? F_("Reduced to %zu triangles. The textures stayed.", r.value("triangles_after", (size_t)0))
                         : std::string(T_("Nothing to reduce."));
      reducePreviewFor = -1.f;
    }
  }
  {
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    engine.uiRects["reduce.apply"] = {a.x, a.y, b.x - a.x, b.y - a.y};  // (for tests)
  }
  if (d.canRevertMeshFix()) {
    ImGui::SameLine();
    if (ImGui::Button(d.lastMeshChange() == "reduction" ? L_("Back to before the reduction") : L_("Back to before the fix"))) {
      if (runCommand("mesh_fix_revert", json::object())) reduceStatus = T_("Back to the mesh as it was."), reducePreviewFor = -1.f;
    }
  }
  if (d.bridge.active)
    ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "%s",
                       T_("Linked to Blender: send the reduced model with Blender > Send Model to Blender."));
  ImGui::TextDisabled("%s", T_("Reducing keeps the paint, but clears Undo: Patina keeps a copy from before."));
  if (!reduceStatus.empty()) ImGui::TextWrapped("%s", reduceStatus.c_str());
  ImGui::End();
}

}  // namespace patina
