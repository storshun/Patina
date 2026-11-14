// (1.2.19) Solo in the window: Shift+H over the model shows the piece under the pointer alone (again: everything), H
// hides it, Alt+H shows everything, as Blender's sculpt hide keys; the Solo picker (the toolbar's Solo button) turns
// clicks into picks, the piece under the pointer lit: a click shows it alone, Shift+click shows it too, Ctrl+click
// hides it. A bar over the 3D view says what is hidden, with what a piece is and Show All. The pieces, the undo
// and the saving: engine/Solo.cpp (the `solo` command).
#include <imgui.h>

#include "ui/App.h"

namespace patina {

namespace {
const char* kByName[] = {N_("UV Island"), N_("Connected Part"), N_("Material Slot"), N_("Face Set")};
const char* kByPlural[] = {N_("UV islands"), N_("connected parts"), N_("material slots"), N_("face sets")};
}  // namespace

// the face under the pointer when it is over the 3D view (and on the model)
bool App::soloFaceUnderPointer(uint32_t& face, vec2& point) const {
  ImVec2 m = ImGui::GetIO().MousePos;
  vec2 local(m.x - lastViewportOrigin.x, m.y - lastViewportOrigin.y);
  if (local.x < 0 || local.y < 0 || local.x >= lastViewportSize.x || local.y >= lastViewportSize.y) return false;
  RayHit h = engine.pick(local);
  if (!h.hit) return false;
  face = h.face;
  point = local / glm::max(lastViewportSize, vec2(1.f));
  return true;
}

void App::soloAct(const char* action, uint32_t face, bool toggle) {
  Document& d = engine.doc;
  auto by = (Document::PieceBy)std::clamp(engine.soloBy, 0, 3);
  if (std::string(action) == "hide") {
    // (hiding the last shown piece would leave nothing: said, not done)
    auto piece = d.pieceFaces(face, by);
    std::vector<uint8_t> h = d.hiddenFaces;
    h.resize(d.mesh.faceCount(), 0);
    for (uint32_t f : piece) h[f] = 1;
    if (std::find(h.begin(), h.end(), uint8_t(0)) == h.end()) {
      setStatus(T_("That is the last piece shown: Alt+H shows everything"));
      return;
    }
  }
  json r;
  if (!runCommand("solo", {{"action", action}, {"face", face}, {"toggle", toggle}}, &r)) return;
  size_t hidden = r.value("hidden_faces", (size_t)0);
  if (!hidden) setStatus(T_("Everything is shown"));
  else setStatus(F_("%zu faces shown, %zu hidden (Alt+H: everything)", r.value("shown_faces", (size_t)0), hidden));
}

void App::soloKeys() {
  if (!engine.doc.hasMesh()) return;
  bool only = keyPressed("solo.only"), hide = keyPressed("solo.hide");
  if (only || hide) {
    uint32_t face;
    vec2 pt;
    if (!soloFaceUnderPointer(face, pt)) setStatus(T_("Point at a piece of the model in the 3D view, then press the key"));
    else soloAct(only ? "only" : "hide", face, true);
  }
  if (keyPressed("solo.show_all") && (engine.doc.pieceHiddenCount() || !engine.doc.hiddenFaces.empty())) {
    runCommand("solo", {{"action", "show_all"}});
    setStatus(T_("Everything is shown"));
  }
  if (keyPressed("solo.pick")) engine.soloPicking = !engine.soloPicking;
  if (engine.soloPicking && ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::GetIO().WantTextInput) engine.soloPicking = false;
}

// In the 3D view, each frame: the piece under the pointer lit and the picker's clicks (they paint nothing)
void App::soloViewport(vec2 local, bool hovered, bool& lmbClicked, bool shift, bool ctrl) {
  Document& d = engine.doc;
  auto clearHover = [&] {
    if (!d.hoverPiece.empty()) d.hoverPiece.clear(), d.updateSelectionFlags();
    soloHoverFace = 0xFFFFFFFFu;
  };
  if (!engine.soloPicking || !d.hasMesh()) {
    clearHover();
    return;
  }
  RayHit h = hovered ? engine.pick(local) : RayHit{};
  if (!h.hit) {
    clearHover();
  } else if (h.face != soloHoverFace || engine.soloBy != soloLitBy) {
    soloHoverFace = h.face;
    // (another face of the lit piece: nothing to do)
    if (engine.soloBy != soloLitBy || !std::binary_search(d.hoverPiece.begin(), d.hoverPiece.end(), h.face)) {
      auto piece = d.pieceFaces(h.face, (Document::PieceBy)std::clamp(engine.soloBy, 0, 3));
      std::sort(piece.begin(), piece.end());
      d.hoverPiece = std::move(piece);
      soloLitBy = engine.soloBy;
      d.updateSelectionFlags();
    }
  }
  if (!lmbClicked) return;
  lmbClicked = false;  // (the picker's click paints nothing)
  if (!h.hit) {
    setStatus(T_("Click a piece of the model (Esc: done picking)"));
    return;
  }
  soloAct(ctrl ? "hide" : shift ? "add" : "only", h.face, false);
}

// The bar over the top of the 3D view while the picker is on or something is hidden
void App::soloBar(ImVec2 origin, ImVec2 avail) {
  Document& d = engine.doc;
  if (!d.hasMesh()) return;
  size_t hidden = d.pieceHiddenCount();
  if (!engine.soloPicking && !hidden) {
    engine.uiRects.erase("solo.bar");
    return;
  }
  ImGui::SetNextWindowPos(ImVec2(origin.x + avail.x * 0.5f, origin.y + 8.f * uiScale), ImGuiCond_Always, ImVec2(0.5f, 0.f));
  ImGui::SetNextWindowBgAlpha(0.82f);
  ImGuiWindowFlags fl = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDocking;
  if (ImGui::Begin("##solo_bar", nullptr, fl)) {
    int by = std::clamp(engine.soloBy, 0, 3);
    if (engine.soloPicking) {
      ImGui::TextUnformatted(T_("Solo: click a piece to show it alone"));
      ImGui::TextDisabled("%s", T_("Shift+click shows it too, Ctrl+click hides it"));
    } else {
      size_t all = d.mesh.faceCount();
      ImGui::Text("%s", F_("Solo: %zu of %zu faces shown", all - std::min(all, hidden), all).c_str());
      ImGui::TextDisabled("%s", T_("The rest is hidden from the view and the brush"));
    }
    ImGui::SetNextItemWidth(150.f * uiScale);
    if (ImGui::BeginCombo("##solo_by", T_(kByName[by]))) {
      for (int i = 0; i < 4; ++i) {
        if (i == 3 && d.faceSets.empty()) continue;
        if (ImGui::Selectable(L_(kByName[i]), i == by)) engine.soloBy = i;
      }
      ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("%s", F_("What a piece is: the picker and Shift+H / H pick whole %s", T_(kByPlural[by])).c_str());
    {
      ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
      engine.uiRects["solo.by"] = {a.x, a.y, b.x - a.x, b.y - a.y};
    }
    if (hidden) {
      ImGui::SameLine();
      if (ImGui::SmallButton(L_("Invert"))) runCommand("solo", {{"action", "invert"}});
      ImGui::SetItemTooltip("%s", T_("The hidden pieces shown, the shown ones hidden"));
      ImGui::SameLine();
      if (ImGui::SmallButton(L_("Show All"))) runCommand("solo", {{"action", "show_all"}}), setStatus(T_("Everything is shown"));
      ImGui::SetItemTooltip("%s", F_("Every piece shown again (%s)", keyLabel("solo.show_all") ? keyLabel("solo.show_all") : "Alt+H").c_str());
      ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
      engine.uiRects["solo.show_all"] = {a.x, a.y, b.x - a.x, b.y - a.y};
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(engine.soloPicking ? L_("Done") : L_("Pick"))) engine.soloPicking = !engine.soloPicking;
    ImGui::SetItemTooltip("%s", engine.soloPicking ? T_("Back to painting (Esc); what is hidden stays hidden")
                                                   : T_("Pick pieces to show with clicks"));
    ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
    engine.uiRects["solo.bar"] = {wp.x, wp.y, ws.x, ws.y};
  }
  ImGui::End();
}

}  // namespace patina
