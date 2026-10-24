// (1.2) The Face Sets panel: named, colored groups of faces (Document::faceSets). A click on a row selects its faces;
// the eye hides them; its menu shows it alone, fills it, masks the selected layer with it.
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include "ui/App.h"

namespace patina {

void App::drawFaceSetsWindow() {
  if (!engine.features.on("face_sets") || !showFaceSets || blenderOn()) return;  // (1.2.7: a Properties tab there)
  // first time: a tab beside Properties (as the Retopology window), else at the right
  const bool placed = ImGui::FindWindowByName("Face Sets") || ImGui::FindWindowSettingsByID(ImHashStr("Face Sets"));
  ImGuiDockNode* besideNode = nullptr;
  ImGuiID keepTab = 0;
  if (ImGuiWindow* beside = ImGui::FindWindowByName("Properties"); !placed && beside && beside->DockId) {
    ImGui::SetNextWindowDockID(beside->DockId, ImGuiCond_FirstUseEver);
    besideNode = ImGui::DockBuilderGetNode(beside->DockId);
    if (besideNode && besideNode->TabBar && !faceSetsFocus) keepTab = besideNode->TabBar->SelectedTabId;
  } else if (!placed) {
    ImVec2 vs = ImGui::GetMainViewport()->WorkSize, vp0 = ImGui::GetMainViewport()->WorkPos;
    ImGui::SetNextWindowPos(ImVec2(vp0.x + vs.x - 410 * uiScale, vp0.y + 60 * uiScale), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(380 * uiScale, 460 * uiScale), ImGuiCond_FirstUseEver);
  }
  if (faceSetsFocus) ImGui::SetNextWindowFocus(), faceSetsFocus = false;
  const int tabColors = pushPanelColors(L_("Face Sets"));
  const bool open = ImGui::Begin(L_("Face Sets"), &showFaceSets, ImGuiWindowFlags_NoFocusOnAppearing);
  ImGui::PopStyleColor(tabColors);
  if (keepTab && besideNode && besideNode->TabBar) besideNode->TabBar->NextSelectedTabId = keepTab;
  if (!open) {
    ImGui::End();
    return;
  }
  faceSetsPanel();
  ImGui::End();
}

void App::faceSetsPanel() {
  Document& d = engine.doc;
  const float fh = ImGui::GetFrameHeight();
  using ui::IconState;
  if (engine.objectCount() > 1) ImGui::TextDisabled(T_("The face sets of %s"), d.displayName().c_str());
  // ---- new sets
  const size_t nsel = d.selectedFaces().size();
  ImGui::BeginDisabled(nsel == 0);
  if (iconButton("facesets.new", "face_sets", T_("New from the selection"), IconState::Normal, fh,
                 T_("A face set of the selected faces (they leave the sets they were in).\n"
                    "Select them with the Select tool: Alt+click a loop, Connected, Flat region..."),
                 nullptr, true))
    runCommand("make_face_set", {{"from", "selection"}});
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button(L_("Make sets from..."))) ImGui::OpenPopup("##fsfrom");
  if (ImGui::BeginPopup("##fsfrom")) {
    ImGui::TextDisabled("%s", T_("Replaces the face sets there are"));
    if (ImGui::MenuItem(L_("The Material Slots"))) runCommand("make_face_set", {{"from", "material"}});
    if (ImGui::MenuItem(L_("The UV Islands"))) runCommand("make_face_set", {{"from", "island"}});
    if (ImGui::MenuItem(L_("The Connected Parts"))) runCommand("make_face_set", {{"from", "connected"}});
    ImGui::EndPopup();
  }
  // ---- the view, the lock, show all
  {
    bool view = engine.viewOptions.mode == ViewMode::FaceSets;
    if (ImGui::Checkbox(L_("Show their colors"), &view)) toggleViewMode(ViewMode::FaceSets);
    ImGui::SetItemTooltip("%s", T_("The Face Sets view: each set in its color on the model"));
    ImGui::SameLine();
    ImGui::Checkbox(L_("Face set lock"), &engine.painter.lockFaceSet);
    ImGui::SetItemTooltip("%s", T_("A stroke only paints the face set it starts on"));
    bool anyHidden = d.hideUnsetFaces;
    for (auto& s : d.faceSets) anyHidden |= s.hidden;
    if (anyHidden) {
      ImGui::SameLine();
      if (ImGui::SmallButton(L_("Show all"))) runCommand("use_face_set", {{"action", "show_all"}});
    }
  }
  // ---- the sets
  if (d.faceSets.empty()) {
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0.f);
    ImGui::TextDisabled("%s", T_("No face sets yet. Select faces, then New from the selection, or make sets from the "
                                 "material slots, UV islands or connected parts. A Blender model's sculpt face sets come "
                                 "in by themselves (the add-on 0.14)."));
    ImGui::PopTextWrapPos();
    return;
  }
  ImGui::BeginChild("##facesets", ImVec2(0, 0), ImGuiChildFlags_Borders);
  {
    ImVec2 a = ImGui::GetWindowPos(), b = ImGui::GetWindowSize();
    engine.uiRects["facesets.list"] = {a.x, a.y, b.x, b.y};
  }
  const std::vector<Document::FaceSet> sets = d.faceSets;  // (a click below may change them)
  for (size_t i = 0; i < sets.size(); ++i) {
    const auto& s = sets[i];
    ImGui::PushID((int)s.id);
    bool vis = !s.hidden;
    if (ImGui::Checkbox("##vis", &vis)) runCommand("edit_face_set", {{"set", s.id}, {"hidden", !vis}});
    ImGui::SetItemTooltip("%s", T_("Shown, or hidden from the 3D view and the brush (renders and exports keep it)"));
    ImGui::SameLine();
    vec3 col = s.color;
    if (ImGui::ColorEdit3("##col", &col.x, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel)) d.findFaceSet(s.id)->color = col;
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      d.findFaceSet(s.id)->color = s.color;  // (one undo step for the whole drag)
      runCommand("edit_face_set", {{"set", s.id}, {"color", {col.x, col.y, col.z}}});
    }
    ImGui::SameLine();
    std::string label = s.name + "##row";
    if (ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick,
                          ImVec2(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("000000").x, 0))) {
      if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) faceSetRename = (int)s.id, faceSetRenameText = s.name, faceSetRenameOpen = true;
      else runCommand("use_face_set", {{"set", s.id}, {"action", ImGui::GetIO().KeyShift ? "add_to_selection" : "select"}});
    }
    {
      ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
      engine.uiRects["facesets.row." + s.name] = {a.x, a.y, b.x - a.x, b.y - a.y};
    }
    ImGui::SetItemTooltip("%s", T_("Click: select its faces (Shift: add them). Double-click: rename. Right-click: more"));
    if (ImGui::BeginPopupContextItem("##fsctx")) {
      json at = {{"set", s.id}};
      auto act = [&](const char* what) {
        json a = at;
        a["action"] = what;
        runCommand("use_face_set", a);
      };
      if (ImGui::MenuItem(L_("Select Its Faces"))) act("select");
      if (ImGui::MenuItem(L_("Add Its Faces to the Selection"))) act("add_to_selection");
      if (ImGui::MenuItem(L_("Show It Alone"))) act("isolate");
      if (ImGui::MenuItem(s.hidden ? L_("Show It") : L_("Hide It"))) act(s.hidden ? "show" : "hide");
      ImGui::Separator();
      if (ImGui::MenuItem(L_("Fill It with the Paint Material"))) act("fill");
      if (ImGui::MenuItem(L_("Mask the Selected Layer with It"), nullptr, false, d.selectedLayer() != nullptr)) act("mask");
      ImGui::Separator();
      if (ImGui::MenuItem(L_("Add the Selected Faces to It"), nullptr, false, nsel > 0))
        runCommand("edit_face_set", {{"set", s.id}, {"add", "selection"}});
      if (ImGui::MenuItem(L_("Take the Selected Faces Out of It"), nullptr, false, nsel > 0))
        runCommand("edit_face_set", {{"set", s.id}, {"remove", "selection"}});
      if (ImGui::MenuItem(L_("Rename..."))) faceSetRename = (int)s.id, faceSetRenameText = s.name, faceSetRenameOpen = true;
      if (ImGui::MenuItem(L_("Delete"))) runCommand("delete_face_set", {{"set", s.id}});
      ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%zu", d.faceSetSize(s.id));
    ImGui::PopID();
  }
  ImGui::EndChild();
  // ---- rename
  if (faceSetRenameOpen) ImGui::OpenPopup("##fsrename"), faceSetRenameOpen = false;
  if (ImGui::BeginPopup("##fsrename")) {
    ImGui::TextUnformatted(T_("Name"));
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    bool done = ImGui::InputText("##name", &faceSetRenameText, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    done |= ImGui::Button(L_("Rename"));
    if (done) {
      if (!faceSetRenameText.empty()) runCommand("edit_face_set", {{"set", faceSetRename}, {"name", faceSetRenameText}});
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
  (void)fh;
}

}  // namespace patina
