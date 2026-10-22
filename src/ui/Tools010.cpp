// Patina 0.10 windows: the mesh symmetry check.
#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "ui/App.h"

namespace patina {

void App::drawSymmetryWindow() {
  if (!showSymmetry) return;
  if (!engine.features.on("symmetry_check")) {
    showSymmetry = false;
    return;
  }
  ImGui::SetNextWindowSizeConstraints(ImVec2(440 * uiScale, 0), ImVec2(560 * uiScale, FLT_MAX));
  if (!ImGui::Begin(L_("Mesh symmetry"), &showSymmetry, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse |
                                                             ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::End();
    return;
  }
  Document& d = engine.doc;
  if (!d.hasMesh()) {
    ImGui::TextDisabled("%s", T_("Open a mesh first."));
    ImGui::End();
    return;
  }
  ImGui::TextWrapped("%s", T_("Mirrors every vertex across X (Blender's X) and measures how far it lands from the surface on "
                              "the other side. It compares shapes, so it works even where the two halves have different "
                              "topology."));
  bool custom = !std::isnan(d.symmetryPlane);
  if (ImGui::Checkbox(L_("Custom mirror plane"), &custom)) d.symmetryPlane = custom ? symmetryPlaneX : NAN;
  if (custom) {
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120 * uiScale);
    // the plane is applied when the drag ends: each change measures the whole mesh again
    ImGui::DragFloat("X##symplane", &symmetryPlaneX, d.mesh.bounds.size().x * 0.001f + 1e-5f, 0, 0, "%.5f");
    if (ImGui::IsItemDeactivatedAfterEdit()) d.symmetryPlane = symmetryPlaneX;
  }
  const SymmetryReport& r = d.symmetry();
  if (!ImGui::IsItemActive()) symmetryPlaneX = r.planeX;

  bool on = engine.viewOptions.mode == ViewMode::Symmetry;
  if (ImGui::Checkbox(L_("Heat map in the viewport"), &on)) engine.viewOptions.mode = on ? ViewMode::Symmetry : ViewMode::Material;
  ImGui::TextDisabled("%s", T_("Gray-blue: symmetric   yellow: 1 tolerance   red: 10+   magenta: no other side"));

  ImGui::SeparatorText(T_("Result"));
  const char* verdict = r.unmatched * 50 > std::max(r.vertices, 1) ? T_("Not mirror-shaped")
                      : r.beyond == 0                              ? T_("Symmetric")
                      : r.maxDist < r.tolerance * 10               ? T_("Slightly asymmetric")
                                                                   : T_("Asymmetric");
  ImVec4 col = r.beyond == 0 ? ImVec4(0.45f, 0.85f, 0.5f, 1) : r.maxDist < r.tolerance * 10 ? ImVec4(0.95f, 0.8f, 0.3f, 1)
                                                                                          : ImVec4(1, 0.45f, 0.4f, 1);
  ImGui::TextColored(col, "%s", verdict);
  ImGui::Text(T_("Mirror plane x = %.5f    tolerance %.5f (0.1%% of %.4f)"), r.planeX, r.tolerance, r.size);
  ImGui::Text(T_("%d of %d vertices beyond tolerance (%.1f%%)"), r.beyond, r.vertices,
              100.f * r.beyond / std::max(r.vertices, 1));
  if (r.unmatched) ImGui::Text(T_("%d vertices have no surface on the other side"), r.unmatched);
  ImGui::Text(T_("max %.5f   p95 %.5f   median %.5f   mean %.5f"), r.maxDist, r.p95, r.median, r.meanDist);

  auto groupTable = [&](const char* id, const char* title, const std::vector<SymmetryReport::Group>& g) {
    if (g.size() < 2) return;
    if (!ImGui::BeginTable(id, 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) return;
    ImGui::TableSetupColumn(title);
    ImGui::TableSetupColumn(L_("vertices"));
    ImGui::TableSetupColumn(L_("beyond"));
    ImGui::TableSetupColumn(L_("max"));
    ImGui::TableHeadersRow();
    for (auto& x : g) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn(), ImGui::TextUnformatted(x.name.c_str());
      ImGui::TableNextColumn(), ImGui::Text("%d", x.vertices);
      ImGui::TableNextColumn(), ImGui::Text("%d", x.beyond);
      ImGui::TableNextColumn(), ImGui::Text("%.5f", x.maxDist);
    }
    ImGui::EndTable();
  };
  if (r.bySlot.size() > 1 && ImGui::CollapsingHeader(L_("Per material slot"))) groupTable("##symslot", L_("slot"), r.bySlot);
  if (ImGui::CollapsingHeader(L_("Per height"))) groupTable("##symheight", L_("band"), r.byHeight);

  if (!r.worst.empty()) {
    ImGui::SeparatorText(T_("Worst spots"));
    for (size_t i = 0; i < r.worst.size(); ++i) {
      auto& s = r.worst[i];
      ImGui::PushID((int)i);
      if (ImGui::SmallButton(L_("Look"))) {
        AABB box;
        float rad = std::max(r.size * 0.08f, s.dist * 4.f);
        box.lo = engine.toWorld(s.position) - vec3(rad), box.hi = engine.toWorld(s.position) + vec3(rad);
        engine.camera.frame(box, rad);
        engine.viewOptions.mode = ViewMode::Symmetry;
      }
      ImGui::SameLine();
      // Blender axes: (x, -z, y)
      ImGui::Text(T_("%.5f  (%.1f tol)  at %.3f, %.3f, %.3f  vertex %u"), s.dist, s.dist / std::max(r.tolerance, 1e-12f),
                  s.position.x, -s.position.z, s.position.y, s.vertex);
      ImGui::PopID();
    }
    ImGui::TextDisabled("%s", T_("Positions are in Blender's axes, so they match the N panel there."));
  }
  ImGui::End();
}

}  // namespace patina

namespace patina {

static const char* kAutoStyles[] = {"any", "realistic", "stylized"};

void App::openAutoTexture() {
  if (!engine.features.on("auto_texture") || !engine.doc.hasMesh()) return;
  json r;
  if (!runCommand("auto_texture", {{"apply", false}, {"style", kAutoStyles[autoTexStyle]}}, &r)) return;
  autoTexGuess = r;
  autoTexPick.clear();
  for (auto& s : r["slots"]) autoTexPick.push_back(s.value("material", std::string()));
  showAutoTexture = true;
}

void App::drawAutoTextureWindow() {
  if (!showAutoTexture) return;
  if (!engine.features.on("auto_texture") || !engine.doc.hasMesh()) {
    showAutoTexture = false;
    return;
  }
  ImGui::SetNextWindowSizeConstraints(ImVec2(480 * uiScale, 0), ImVec2(640 * uiScale, FLT_MAX));
  if (!ImGui::Begin(L_("Auto texture"), &showAutoTexture, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse |
                                                              ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::End();
    return;
  }
  ImGui::TextWrapped("%s", T_("A material for each material slot, guessed from the slot's name. Each becomes a fill layer "
                              "masked to that slot, in an 'Auto texture' folder at the bottom of the stack, so your paint "
                              "stays on top. Eyes, teeth, glass and decals are left for painting by hand."));
  ImGui::SetNextItemWidth(140 * uiScale);
  if (ImGui::Combo(L_("Prefer"), &autoTexStyle, Z_("Any style\0Realistic\0Stylized\0"))) openAutoTexture();
  ImGui::SameLine();
  ImGui::SetNextItemWidth(140 * uiScale);
  ImGui::Combo(L_("Projection"), &autoTexMapping, Z_("Triplanar\0UV\0"));
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T_("Triplanar: no seams, ignores the UVs. UV: follows the UV layout."));
  auto label = [&](const std::string& id) -> std::string {
    if (id.empty()) return T_("(skip)");
    auto* e = engine.materials.find(id);
    return e ? e->def.name : id;
  };
  json& slots = autoTexGuess["slots"];
  if (ImGui::BeginTable("##auto", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
    ImGui::TableSetupColumn(L_("Material slot"));
    ImGui::TableSetupColumn(L_("faces"));
    ImGui::TableSetupColumn(L_("Material"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (size_t i = 0; i < slots.size() && i < autoTexPick.size(); ++i) {
      json& s = slots[i];
      ImGui::PushID((int)i);
      ImGui::TableNextRow();
      ImGui::TableNextColumn(), ImGui::TextUnformatted(s.value("slot_name", std::string()).c_str());
      ImGui::TableNextColumn(), ImGui::Text("%d", s.value("faces", 0));
      ImGui::TableNextColumn();
      ImGui::SetNextItemWidth(260 * uiScale);
      std::string& pick = autoTexPick[i];
      if (ImGui::BeginCombo("##m", label(pick).c_str(), ImGuiComboFlags_HeightLarge)) {
        if (ImGui::Selectable(L_("(skip)"), pick.empty())) pick.clear();
        std::vector<std::string> sug;
        if (!s.value("material", std::string()).empty()) sug.push_back(s["material"]);
        for (auto& a : s["alternatives"]) sug.push_back(a);
        if (!sug.empty()) ImGui::SeparatorText(T_("Suggested"));
        for (auto& id : sug)
          if (ImGui::Selectable((label(id) + "##s" + id).c_str(), pick == id)) pick = id;
        ImGui::SeparatorText(T_("Library"));
        std::string lastCat;
        for (auto& e : engine.materials.entries()) {
          if (e->transient) continue;
          if (e->def.category != lastCat) ImGui::TextDisabled("%s", e->def.category.c_str()), lastCat = e->def.category;
          if (ImGui::Selectable((e->def.name + "##l" + e->def.id).c_str(), pick == e->def.id)) pick = e->def.id;
        }
        ImGui::EndCombo();
      }
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  int n = 0;
  for (auto& p : autoTexPick) n += !p.empty();
  ImGui::BeginDisabled(n == 0);
  char btn[160];
  std::snprintf(btn, sizeof btn, P_("Add %d fill layer", "Add %d fill layers", n), n);
  if (ImGui::Button((std::string(btn) + "###autotex_add").c_str())) {
    json ov = json::object();
    for (size_t i = 0; i < slots.size() && i < autoTexPick.size(); ++i) ov[std::to_string(slots[i].value("slot", 0))] = autoTexPick[i];
    json r;
    if (runCommand("auto_texture", {{"overrides", ov}, {"style", kAutoStyles[autoTexStyle]},
                                    {"mapping", autoTexMapping ? "uv" : "triplanar"}}, &r)) {
      setStatus(F_("Auto texture: %d fill layers in the 'Auto texture' folder", r.value("layers_added", 0)));
      showAutoTexture = false;
    }
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button(L_("Cancel"))) showAutoTexture = false;
  ImGui::End();
}

}  // namespace patina

namespace patina {

void App::playMacro(const json& args) {
  json r;
  if (!runCommand("macro_play", args, &r)) return;
  if (r.value("ok", false)) setStatus(F_("Macro played: %d steps", r.value("played", 0)));
  else
    showError(F_("The macro stopped at step %d (%s)", r.value("failed_step", 0) + 1,
                 r.value("command", std::string()).c_str()),
              r.value("error", std::string()));
}

void App::drawMacroWindow() {
  static bool wasOpen = false;
  if (!showMacros || !engine.features.on("macro_recorder")) {
    showMacros = wasOpen = false;
    return;
  }
  if (!wasOpen) {  // the saved list, fresh each time the window opens
    runCommand("macro_list", json::object(), &macroSaved);
    wasOpen = true;
  }
  ImGui::SetNextWindowSizeConstraints(ImVec2(460 * uiScale, 0), ImVec2(640 * uiScale, FLT_MAX));
  if (!ImGui::Begin(L_("Macro recorder"), &showMacros, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse |
                                                             ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::End();
    return;
  }
  auto& M = engine.macro;
  const std::vector<json> steps = engine.macroSteps();
  ImGui::TextWrapped("%s", T_("Records what you do through menus and windows (layers, fills, smart materials, UV tools, "
                              "bakes, exports) and what scripts or Claude run, so it can be replayed on another model or "
                              "saved as a job for 'patina run'. Brush strokes in the viewport are not recorded; what you "
                              "undo drops out."));
  if (M.recording) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.2f, 0.2f, 1));
    if (ImGui::Button(L_("Stop recording"))) M.recording = false;
    ImGui::PopStyleColor();
  } else if (ImGui::Button(L_("Record"))) {
    M.start(false);
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(steps.empty() || M.recording);
  if (ImGui::Button(L_("Play"))) playMacro(json::object());
  ImGui::SameLine();
  if (ImGui::Button(L_("Clear"))) M.clear();
  ImGui::EndDisabled();
  ImGui::SeparatorText(F_("Steps (%zu)", steps.size()).c_str());
  if (steps.empty()) ImGui::TextDisabled("%s", M.recording ? T_("Waiting for the first action...") : T_("Nothing recorded yet."));
  int drop = -1;
  if (!steps.empty() &&
      ImGui::BeginChild("##steps", ImVec2(0, std::min(220.f, 24.f * (float)steps.size() + 8.f) * uiScale), ImGuiChildFlags_None)) {
    for (size_t i = 0; i < steps.size(); ++i) {
      ImGui::PushID((int)i);
      if (ImGui::SmallButton("x")) drop = (int)i;
      ImGui::SameLine();
      std::string args = steps[i].value("args", json::object()).dump();
      if (args.size() > 70) args = args.substr(0, 67) + "...";
      ImGui::Text("%zu. %s", i + 1, steps[i].value("cmd", std::string()).c_str());
      ImGui::SameLine();
      ImGui::TextDisabled("%s", args.c_str());
      ImGui::PopID();
    }
  }
  if (!steps.empty()) ImGui::EndChild();
  if (drop >= 0) M.erase(engine.doc.undo, (size_t)drop);
  ImGui::SetNextItemWidth(200 * uiScale);
  ImGui::InputText("##mname", &macroName);
  ImGui::SameLine();
  ImGui::BeginDisabled(steps.empty() || macroName.empty());
  if (ImGui::Button(L_("Save"))) {
    json r;
    if (runCommand("macro_save", {{"name", macroName}}, &r)) {
      setStatus(F_("Saved %s", r.value("path", std::string()).c_str()));
      runCommand("macro_list", json::object(), &macroSaved);
    }
  }
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    ImGui::SetTooltip("%s", T_("Saved as a job file: patina run <file> replays it without a window"));
  if (macroSaved.is_array() && !macroSaved.empty()) {
    ImGui::SeparatorText(T_("Saved macros"));
    for (auto& m : macroSaved) {
      std::string name = m.value("name", std::string()), path = m.value("path", std::string());
      ImGui::PushID(path.c_str());
      if (ImGui::SmallButton(L_("Play"))) playMacro({{"path", path}});
      ImGui::SameLine();
      ImGui::Text("%s", name.c_str());
      ImGui::SameLine();
      ImGui::TextDisabled(T_("%d steps"), m.value("steps", 0));
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", path.c_str());
      ImGui::PopID();
    }
  }
  ImGui::End();
}

}  // namespace patina
