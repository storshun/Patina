// Split / compare view in the 3D view: the left of the divider shows a snapshot of the
// textures (or another view mode), the right the live result. Drag the divider to wipe.
#include <imgui.h>

#include "ui/App.h"

namespace patina {

void App::compareMenu() {
  auto& cv = engine.compare;
  if (!ImGui::BeginMenu(L_("Compare view"))) return;
  if (ImGui::MenuItem(L_("Split view"), keyLabel("view.compare"), cv.enabled)) toggleCompare();
  if (ImGui::MenuItem(L_("Take a snapshot"), keyLabel("view.compare_snapshot"))) {
    engine.takeCompareSnapshot();
    cv.mode = 0;
    cv.enabled = true;
    setStatus(F_("Compare: %s on the left, the live textures on the right", cv.label.c_str()));
  }
  ImGui::Separator();
  if (ImGui::MenuItem(L_("Left: the snapshot"), nullptr, cv.mode == 0)) {
    cv.mode = 0;
    if (!cv.hasSnapshot) engine.takeCompareSnapshot();
  }
  if (engine.features.on("bake_down") &&
      ImGui::MenuItem(L_("Left: the high-poly before the bake-down"), nullptr, cv.mode == 2, engine.doc.bakeDownBefore != nullptr)) {
    cv.mode = 2;
    cv.enabled = true;
  }
  if (ImGui::BeginMenu(L_("Left: a view mode"))) {
    for (int m = 0; m < (int)ViewMode::Count; ++m)
      if (viewModeAvailable((ViewMode)m) &&
          ImGui::MenuItem(L_(viewModeLabel(m)), nullptr, cv.mode == 1 && (int)cv.view == m)) {
        cv.mode = 1;
        cv.view = (ViewMode)m;
        cv.enabled = true;
      }
    ImGui::EndMenu();
  }
  ImGui::EndMenu();
}

void App::toggleCompare() {
  auto& cv = engine.compare;
  cv.enabled = !cv.enabled;
  if (cv.enabled && cv.mode == 2 && !engine.doc.bakeDownBefore) cv.mode = 0;
  if (cv.enabled && cv.mode == 0 && !cv.hasSnapshot) engine.takeCompareSnapshot();
  setStatus(cv.enabled ? (cv.mode == 0   ? F_("Compare: %s | live", cv.label.c_str())
                          : cv.mode == 2 ? std::string(T_("Compare: the high-poly | baked down"))
                                         : std::string(T_("Compare: view mode | live")))
                       : T_("Compare view off"));
}

bool App::compareOverlay(ImVec2 origin, ImVec2 size, int pw, int ph, bool hovered) {
  auto& cv = engine.compare;
  ImGuiIO& io = ImGui::GetIO();
  if (hovered && !io.WantTextInput) {
    if (keyPressed("view.compare")) toggleCompare();
    if (keyPressed("view.compare_snapshot")) {
      engine.takeCompareSnapshot();
      cv.mode = 0;
      cv.enabled = true;
      setStatus(F_("Compare: %s | live", cv.label.c_str()));
    }
  }
  static bool dragging = false;
  if (!engine.compareReady()) {
    dragging = false;
    return false;
  }
  Layer* sel = engine.doc.selectedLayer();
  engine.renderCompare(engine.camera, compareRT, pw, ph, engine.viewOptions,
                       cv.mode == 1 && cv.view == ViewMode::LayerMask ? engine.maskViewTexture() : (sel && sel->mask ? sel->mask->id() : 0));
  ImDrawList* dl = ImGui::GetWindowDrawList();
  float sx = origin.x + cv.split * size.x;
  dl->AddImage((ImTextureID)(intptr_t)compareRT.color.id(), origin, ImVec2(sx, origin.y + size.y), ImVec2(0, 1),
               ImVec2(cv.split, 0));
  // divider with a handle
  ImVec2 mouse = io.MousePos;
  bool nearDivider = hovered && std::abs(mouse.x - sx) < 6.f * uiScale && mouse.y >= origin.y && mouse.y <= origin.y + size.y;
  if (nearDivider && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !engine.painter.active()) dragging = true;
  if (dragging) {
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) dragging = false;
    else cv.split = std::clamp((mouse.x - origin.x) / std::max(size.x, 1.f), 0.02f, 0.98f);
    sx = origin.x + cv.split * size.x;
  }
  bool hot = nearDivider || dragging;
  ImU32 lc = hot ? IM_COL32(255, 190, 90, 255) : IM_COL32(235, 235, 235, 220);
  dl->AddLine(ImVec2(sx, origin.y), ImVec2(sx, origin.y + size.y), IM_COL32(0, 0, 0, 120), 4.f);
  dl->AddLine(ImVec2(sx, origin.y), ImVec2(sx, origin.y + size.y), lc, hot ? 2.f : 1.5f);
  float cy = origin.y + size.y * 0.5f, r = 9.f * uiScale;
  dl->AddCircleFilled(ImVec2(sx, cy), r, IM_COL32(30, 30, 30, 220));
  dl->AddCircle(ImVec2(sx, cy), r, lc, 0, 1.5f);
  dl->AddTriangleFilled(ImVec2(sx - r * 0.25f, cy - r * 0.45f), ImVec2(sx - r * 0.25f, cy + r * 0.45f),
                        ImVec2(sx - r * 0.7f, cy), lc);
  dl->AddTriangleFilled(ImVec2(sx + r * 0.25f, cy - r * 0.45f), ImVec2(sx + r * 0.25f, cy + r * 0.45f),
                        ImVec2(sx + r * 0.7f, cy), lc);
  if (hot) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
  // labels at the bottom (the brush bar is at the top)
  std::string left = cv.mode == 0 ? cv.label : cv.mode == 2 ? std::string(T_("High-poly")) : std::string(T_(viewModeLabel((int)cv.view)));
  const char* right = cv.mode == 2 ? T_("Baked down") : T_("Live");
  float ly = origin.y + size.y - ImGui::GetTextLineHeight() * 2.6f;
  ImVec2 lsz = ImGui::CalcTextSize(left.c_str());
  if (sx - origin.x > lsz.x + 20) {
    dl->AddRectFilled(ImVec2(sx - lsz.x - 14, ly - 3), ImVec2(sx - 6, ly + lsz.y + 3), IM_COL32(0, 0, 0, 140), 4.f);
    dl->AddText(ImVec2(sx - lsz.x - 10, ly), IM_COL32(255, 255, 255, 230), left.c_str());
  }
  ImVec2 rsz = ImGui::CalcTextSize(right);
  if (origin.x + size.x - sx > rsz.x + 20) {
    dl->AddRectFilled(ImVec2(sx + 6, ly - 3), ImVec2(sx + rsz.x + 14, ly + rsz.y + 3), IM_COL32(0, 0, 0, 140), 4.f);
    dl->AddText(ImVec2(sx + 10, ly), IM_COL32(255, 255, 255, 230), right);
  }
  return dragging || nearDivider;
}

}  // namespace patina
