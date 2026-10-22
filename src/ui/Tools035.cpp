// 0.35: Mesh > Lightmap UVs: a second UV map for baked lighting, with a preview of its layout.
#include <imgui.h>

#include <algorithm>

#include "ui/App.h"

namespace patina {

void App::drawLightmapWindow() {
  if (!showLightmapWindow) return;
  if (!engine.features.on("lightmap_uvs") || !engine.doc.hasMesh()) {
    showLightmapWindow = false;
    return;
  }
  ImGui::SetNextWindowSizeConstraints(ImVec2(420 * uiScale, 0), ImVec2(560 * uiScale, FLT_MAX));
  if (!ImGui::Begin(L_("Lightmap UVs"), &showLightmapWindow, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse |
                                                                 ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::End();
    return;
  }
  Document& d = engine.doc;
  ImGui::TextWrapped("%s", T_("A second UV map for baked lighting in a game engine: every face its own space, the same "
                              "detail everywhere. The paint keeps its own UVs."));
  static const int sizes[] = {128, 256, 512, 1024, 2048};
  int si = 2;
  for (int i = 0; i < 5; ++i)
    if (sizes[i] == lightmapSize) si = i;
  ImGui::SetNextItemWidth(260 * uiScale);
  if (ImGui::Combo(L_("Lightmap size"), &si, "128\0" "256\0" "512\0" "1024\0" "2048\0")) lightmapSize = sizes[si];
  ImGui::SetItemTooltip("%s", T_("The size the engine bakes the lighting at: the gaps are measured in its pixels"));
  ImGui::SetNextItemWidth(260 * uiScale);
  ImGui::SliderFloat(L_("Gap (pixels)"), &lightmapPadding, 0.f, 16.f, "%.1f");
  ImGui::SetItemTooltip("%s", T_("Room between islands so light does not bleed from one to the next: 2 to 4 is usual"));
  ImGui::SetNextItemWidth(260 * uiScale);
  ImGui::Combo(L_("Islands"), &lightmapSource, Z_("The model's own UV islands\0New ones made for lightmaps\0"));
  ImGui::SetItemTooltip("%s", T_("New islands (made by xatlas) suit models whose UVs are missing, stretched or a mess: scans "
                              "and generated models"));
  if (ImGui::Button(L_("Make"), ImVec2(160 * uiScale, 0))) {
    json r;
    if (runCommand("mesh_lightmap_uvs", {{"lightmap_size", lightmapSize}, {"padding", lightmapPadding},
                                         {"source", lightmapSource ? "auto" : "uv"}},
                   &r)) {
      int islands = r.value("islands", 0);
      float fill = r.value("fill", 0.f) * 100.f;
      lightmapStatus = r.value("overlaps", 0) ? F_("%d islands, %.0f%% of the square, some still overlap", islands, fill)
                                              : F_("%d islands, %.0f%% of the square, no overlaps", islands, fill);
    }
  }
  const auto& uv2 = d.mesh.loopUV2;
  bool has = uv2.size() == d.mesh.loopCount() && !uv2.empty();
  if (has) {
    ImGui::SameLine();
    if (ImGui::SmallButton(L_("Remove"))) runCommand("mesh_lightmap_uvs", {{"clear", true}}), lightmapStatus.clear();
  }
  if (!lightmapStatus.empty()) ImGui::TextWrapped("%s", lightmapStatus.c_str());
  if (has) {
    // the layout: each face's edges (up to 60000 faces)
    float side = 300 * uiScale;
    ImVec2 o = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(o, ImVec2(o.x + side, o.y + side), IM_COL32(28, 30, 34, 255));
    auto at = [&](vec2 u) { return ImVec2(o.x + u.x * side, o.y + (1.f - u.y) * side); };
    uint32_t F = std::min<uint32_t>(d.mesh.faceCount(), 60000);
    for (uint32_t f = 0; f < F; ++f)
      for (uint32_t l = d.mesh.faceStart[f]; l < d.mesh.faceStart[f + 1]; ++l) {
        uint32_t n = l + 1 < d.mesh.faceStart[f + 1] ? l + 1 : d.mesh.faceStart[f];
        dl->AddLine(at(uv2[l]), at(uv2[n]), IM_COL32(120, 200, 255, 160));
      }
    dl->AddRect(o, ImVec2(o.x + side, o.y + side), IM_COL32(200, 200, 200, 255));
    ImGui::Dummy(ImVec2(side, side));
  }
  ImGui::TextDisabled("%s", T_("Export Model writes them as the second UV map (glTF); Blender gets the UV map Lightmap."));
  ImGui::End();
}

}  // namespace patina
