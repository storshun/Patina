// (1.2) The Face Sets panel: named, colored groups of faces (Document::faceSets). (1.2.10) As Blender's vertex groups:
// a click on a row makes it the active set, the buttons under the list add or remove the selection, select or deselect
// its faces; the eye hides them, the trash deletes the set; its menu shows only these faces, fills, masks.
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
  // (1.2.10) as Blender's vertex groups: a list with one set active (its row lit), Create and the trash in the rows;
  // under the list, buttons for the active set: Add Selection, Remove Selection, Select, Deselect
  Document& d = engine.doc;
  const float fh = ImGui::GetFrameHeight();
  using ui::IconState;
  if (engine.objectCount() > 1) ImGui::TextDisabled(T_("The face sets of %s"), d.displayName().c_str());
  const size_t nsel = d.selectedFaces().size();
  const char* selectHow = T_("Select faces with the Select tool: Alt+click a loop, Connected, Flat region...");
  // ---- create
  ImGui::BeginDisabled(nsel == 0);
  if (iconButton("facesets.new", "face_sets", T_("Create from Selection"), IconState::Normal, fh,
                 F_("A new face set of the selected faces. They leave the sets they were in; those sets stay, with "
                    "their names and colors.\n%s", selectHow).c_str(),
                 nullptr, true)) {
    json out;
    if (runCommand("make_face_set", {{"from", "selection"}}, &out)) faceSetActive = out.value("id", 0u);
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button(L_("Create Sets From..."))) ImGui::OpenPopup("##fsfrom");
  {
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    engine.uiRects["facesets.create_from"] = {a.x, a.y, b.x - a.x, b.y - a.y};
  }
  if (ImGui::BeginPopup("##fsfrom")) {
    ImGui::TextDisabled("%s", T_("Replaces the face sets there are"));
    if (ImGui::MenuItem(L_("Material Slots"))) runCommand("make_face_set", {{"from", "material"}});
    if (ImGui::MenuItem(L_("UV Islands"))) runCommand("make_face_set", {{"from", "island"}});
    if (ImGui::MenuItem(L_("Connected Parts"))) runCommand("make_face_set", {{"from", "connected"}});
    ImGui::EndPopup();
  }
  // ---- the view, the lock, show all
  bool anyHidden = d.hideUnsetFaces;
  for (auto& s : d.faceSets) anyHidden |= s.hidden;
  {
    bool view = engine.viewOptions.mode == ViewMode::FaceSets;
    if (ImGui::Checkbox(L_("Show by Color"), &view)) toggleViewMode(ViewMode::FaceSets);
    ImGui::SetItemTooltip("%s", T_("The Face Sets view: each set in its color on the model"));
    ImGui::SameLine();
    ImGui::Checkbox(L_("Lock Face Sets"), &engine.painter.lockFaceSet);
    ImGui::SetItemTooltip("%s", T_("A stroke only paints the face set it starts on"));
    ImGui::SameLine();
    ImGui::BeginDisabled(!anyHidden);
    if (ImGui::SmallButton(L_("Show All"))) runCommand("use_face_set", {{"action", "show_all"}});
    ImGui::SetItemTooltip("%s", anyHidden ? T_("Shows every face set and the faces in none") : T_("Nothing is hidden"));
    {
      ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
      engine.uiRects["facesets.show_all"] = {a.x, a.y, b.x - a.x, b.y - a.y};
    }
    ImGui::EndDisabled();
  }
  // ---- the sets
  if (d.faceSets.empty()) {
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0.f);
    ImGui::TextDisabled("%s", T_("No face sets yet. Select faces, then Create from Selection, or create sets from the "
                                 "material slots, UV islands or connected parts. A Blender model's sculpt face sets come "
                                 "in by themselves (the add-on 0.14)."));
    ImGui::PopTextWrapPos();
    return;
  }
  if (!d.findFaceSet(faceSetActive)) faceSetActive = d.faceSets.front().id;
  const float style = ImGui::GetStyle().ItemSpacing.y, framePad = ImGui::GetStyle().FramePadding.y;
  const float below = 2.f * fh + 2.f * style + framePad * 2.f + 4.f * uiScale;  // (the buttons under the list)
  ImGui::BeginChild("##facesets", ImVec2(0, std::max(4.f * fh, ImGui::GetContentRegionAvail().y - below)), ImGuiChildFlags_Borders);
  {
    ImVec2 a = ImGui::GetWindowPos(), b = ImGui::GetWindowSize();
    engine.uiRects["facesets.list"] = {a.x, a.y, b.x, b.y};
  }
  const std::vector<Document::FaceSet> sets = d.faceSets;  // (a click below may change them)
  const float trashW = fh, countW = ImGui::CalcTextSize("000000").x;
  for (size_t i = 0; i < sets.size(); ++i) {
    const auto& s = sets[i];
    const bool active = s.id == faceSetActive;
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
    const float rowW = ImGui::GetContentRegionAvail().x - countW - trashW - 2.f * ImGui::GetStyle().ItemSpacing.x;
    if (ImGui::Selectable(label.c_str(), active, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(rowW, 0))) {
      faceSetActive = s.id;
      if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) faceSetRename = (int)s.id, faceSetRenameText = s.name, faceSetRenameOpen = true;
    }
    {
      ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
      engine.uiRects["facesets.row." + s.name] = {a.x, a.y, b.x - a.x, b.y - a.y};
    }
    ImGui::SetItemTooltip("%s", T_("Click: make it the active set (the buttons under the list work on it). "
                                   "Double-click: rename. Right-click: more"));
    if (ImGui::BeginPopupContextItem("##fsctx")) {
      faceSetActive = s.id;
      json at = {{"set", s.id}};
      auto act = [&](const char* what) {
        json a = at;
        a["action"] = what;
        runCommand("use_face_set", a);
      };
      if (ImGui::MenuItem(L_("Select"))) act("select");
      if (ImGui::MenuItem(L_("Add to Selection"))) act("add_to_selection");
      if (ImGui::MenuItem(L_("Deselect"))) act("deselect");
      ImGui::Separator();
      if (ImGui::MenuItem(L_("Show Only These Faces"))) act("isolate");
      if (ImGui::MenuItem(s.hidden ? L_("Show") : L_("Hide"))) act(s.hidden ? "show" : "hide");
      if (ImGui::MenuItem(L_("Show All"), nullptr, false, anyHidden)) runCommand("use_face_set", {{"action", "show_all"}});
      ImGui::Separator();
      if (ImGui::MenuItem(L_("Fill with the Paint Material"))) act("fill");
      if (ImGui::MenuItem(L_("Mask the Selected Layer"), nullptr, false, d.selectedLayer() != nullptr)) act("mask");
      ImGui::Separator();
      if (ImGui::MenuItem(L_("Add Selection to Set"), nullptr, false, nsel > 0))
        runCommand("edit_face_set", {{"set", s.id}, {"add", "selection"}});
      if (ImGui::MenuItem(L_("Remove Selection from Set"), nullptr, false, nsel > 0))
        runCommand("edit_face_set", {{"set", s.id}, {"remove", "selection"}});
      if (ImGui::MenuItem(L_("Rename..."))) faceSetRename = (int)s.id, faceSetRenameText = s.name, faceSetRenameOpen = true;
      if (ImGui::MenuItem(L_("Delete Set"))) runCommand("delete_face_set", {{"set", s.id}});
      ImGui::EndPopup();
    }
    ImGui::SameLine();
    {
      // the count, right-aligned before the trash
      std::string n = std::to_string(d.faceSetSize(s.id));
      ImGui::SetCursorPosX(ImGui::GetCursorPosX() + countW - ImGui::CalcTextSize(n.c_str()).x);
      ImGui::TextDisabled("%s", n.c_str());
      ImGui::SetItemTooltip("%s", T_("Its faces"));
    }
    ImGui::SameLine();
    if (iconButton(("facesets.delete." + s.name).c_str(), "delete", T_("Delete Set"), IconState::Normal, trashW,
                   T_("Deletes this face set. Its faces stay on the model, in no set (Ctrl+Z brings the set back).")))
      runCommand("delete_face_set", {{"set", s.id}});
    ImGui::PopID();
  }
  ImGui::EndChild();
  // ---- the active set's buttons, as Blender's Assign, Remove, Select, Deselect
  if (const Document::FaceSet* s = d.findFaceSet(faceSetActive)) {
    const uint32_t id = s->id;
    auto button = [&](const char* label, const char* key, const char* tipText, bool on) {
      ImGui::BeginDisabled(!on);
      bool r = ImGui::Button(label);
      ImGui::EndDisabled();
      ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
      engine.uiRects[std::string("facesets.") + key] = {a.x, a.y, b.x - a.x, b.y - a.y};
      ImGui::SetItemTooltip("%s", tipText);
      return r;
    };
    const std::string nm = s->name;
    if (button(L_("Add Selection"), "assign",
               F_("Adds the selected faces to %s (they leave the sets they were in). %s", nm.c_str(), selectHow).c_str(), nsel > 0))
      runCommand("edit_face_set", {{"set", id}, {"add", "selection"}});
    ImGui::SameLine();
    if (button(L_("Remove Selection"), "remove", F_("Takes the selected faces out of %s (into no set)", nm.c_str()).c_str(), nsel > 0))
      runCommand("edit_face_set", {{"set", id}, {"remove", "selection"}});
    if (button(L_("Select"), "select", F_("Adds the faces of %s to the selection", nm.c_str()).c_str(), true))
      runCommand("use_face_set", {{"set", id}, {"action", "add_to_selection"}});
    ImGui::SameLine();
    if (button(L_("Deselect"), "deselect", F_("Takes the faces of %s out of the selection", nm.c_str()).c_str(), nsel > 0))
      runCommand("use_face_set", {{"set", id}, {"action", "deselect"}});
    ImGui::SameLine();
    if (button(L_("Deselect All"), "deselect_all", T_("Clears the selection"), nsel > 0)) runCommand("select_faces", {{"mode", "clear"}});
    ImGui::SameLine();
    ImGui::TextDisabled(T_("%zu selected"), nsel);
  }
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
}

}  // namespace patina
