// (0.41) Baked map layers in the window: the properties of the selected one (strength, the value the brush paints,
// its fixes and adjustments, a new bake, export), the "add a layer" items, and the 3D view following the selection.
#include <imgui.h>
#include <imgui_stdlib.h>

#include "ui/App.h"

namespace patina {

static const char* bakedMapHelp(int k) {
  switch (k) {
    case 0:
      return N_("Shadow in creases and corners: 1 is none, 0 fully shadowed. The Occlusion and Cavity smart masks, "
                "the 3D view's lighting and the exported AO map use it.");
    case 1:
      return N_("Edges are bright, creases dark, 0.5 flat. The Edges and Cavity smart masks use it.");
    default:
      return N_("Thin parts are dark, thick ones white. The Thin parts smart mask and the light shining through thin "
                "parts in the 3D view use it.");
  }
}

bool App::bakedMapMenuItems() {
  Document& d = engine.doc;
  bool any = false;
  for (int k = 0; k < kBakedMapCount; ++k) {
    if (!d.tex.hasBakedMap(k) || d.bakedLayer(k)) continue;
    any = true;
    if (ImGui::MenuItem(L_(bakedMapLabel(k)), nullptr, false, engine.features.on("baked_layers"))) d.addBakedLayer(k);
  }
  return any;
}

void App::bakeMapAgain(int k) {
  Document& d = engine.doc;
  if (!d.hasMesh()) return;
  if (k == (int)BakedMap::AO) d.tex.bakeAO(d.mesh, d.gpu, d.aoSamples);
  else if (k == (int)BakedMap::Curvature) d.tex.bakeCurvature(d.mesh, d.gpu, &d.bvh);
  else d.tex.bakeThickness(d.mesh, d.bvh, d.thicknessRays);
  d.markComposite();
  setStatus(F_("Baked the %s again", T_(bakedMapLabel(k))));
}

void App::drawBakedProperties(Layer& l) {
  Document& d = engine.doc;
  Painter& P = engine.painter;
  const int k = l.bakedMap;
  bool changed = false;
  ImGui::SeparatorText(T_(bakedMapLabel(k)));
  ImGui::TextWrapped("%s", T_(bakedMapHelp(k)));
  if (!engine.features.on("baked_layers"))
    ImGui::TextColored(ImVec4(0.95f, 0.7f, 0.3f, 1), "%s", T_("Baked maps as layers are switched off (Preferences > Features)."));
  if (ImGui::InputText(L_("Name"), &l.name)) d.pendingLabel = "Rename: " + l.name;
  bool original = !l.visible;
  if (ImGui::Checkbox(L_("Show the bake as it came out"), &original)) l.visible = !original, changed = true;
  ImGui::SetItemTooltip("%s", T_("Your fixes and adjustments off, to compare (the layer's check in the list)"));
  changed |= ImGui::SliderFloat(L_("Strength"), &l.bakedStrength, 0.f, 2.f);
  ImGui::SetItemTooltip("%s", T_("How strong the map is in the end: 0 turns it off, 1 as baked, 2 twice as strong"));
  changed |= ImGui::SliderFloat(L_("Opacity"), &l.opacity, 0.f, 1.f);
  ImGui::SetItemTooltip("%s", T_("How much of your fixes and adjustments: 0 is the bake as it came out"));

  // ---- painted fixes
  ImGui::SeparatorText(T_("Fixes"));
  ImGui::TextWrapped("%s", T_("Paint on the model to fix the map: Paint lays the value below on it, Erase brings the bake "
                              "back, Blur smooths it, Smudge and Clone move it around. The picker takes the value under "
                              "the cursor."));
  {
    float sw = ImGui::GetFrameHeight();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRectFilled(p, p + ImVec2(sw, sw), ImGui::GetColorU32(ImVec4(P.maskValue, P.maskValue, P.maskValue, 1)), 3.f);
    ImGui::GetWindowDrawList()->AddRect(p, p + ImVec2(sw, sw), IM_COL32(120, 120, 128, 255), 3.f);
    ImGui::Dummy(ImVec2(sw, sw));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(std::max(80.f, ImGui::CalcItemWidth() - sw - ImGui::GetStyle().ItemSpacing.x));
    ImGui::SliderFloat(L_("Paint value"), &P.maskValue, 0.f, 1.f, "%.3f");
    {
      ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
      engine.uiRects["baked.value"] = {a.x, a.y, b.x - a.x, b.y - a.y};  // (for tests)
    }
    float neutral = bakedMapNeutral(k);
    if (ImGui::SmallButton(L_(k == 1 ? "Flat (0.5)" : k == 0 ? "No shadow (1)" : "Thick (1)"))) P.maskValue = neutral;
    ImGui::SameLine();
    if (ImGui::SmallButton(L_("Black (0)"))) P.maskValue = 0.f;
    if (k == 1) {
      ImGui::SameLine();
      if (ImGui::SmallButton(L_("White (1)"))) P.maskValue = 1.f;
    }
  }
  ImGui::BeginDisabled(!l.mask);
  if (ImGui::Button(L_("Reset the fixes"))) d.removeMask(d.selected);
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    ImGui::SetTooltip("%s", l.mask ? T_("Takes the painted fixes away: the bake shows again (undoable)") : T_("Nothing painted yet"));

  // ---- adjustments (the mask effect stack, starting from the bake with the fixes)
  ImGui::SeparatorText(T_("Adjustments"));
  if (l.maskEffects.empty()) {
    ImGui::TextDisabled("%s", T_("Levels, blur, noise, fills... applied to the whole map, from the bottom up."));
    if (ImGui::BeginCombo("##addbakedfx", T_("Add an adjustment..."))) {
      int t = maskEffectMenu(true);
      ImGui::EndCombo();
      if (t >= 0) {
        l.maskEffects.push_back(newMaskEffect(t));
        if (t == MaskEffect::Generator || t == MaskEffect::Noise || t == MaskEffect::Pattern) l.maskEffects.back().opacity = 0.5f;
        openFxNext = (int)l.maskEffects.size() - 1;
        changed = true;
      }
    }
  } else {
    changed |= maskStackWidgets(l);
  }

  // ---- the bake
  ImGui::SeparatorText(T_("Bake"));
  if (ImGui::Button(L_("Bake again"))) bakeMapAgain(k);
  ImGui::SetItemTooltip("%s", T_("A new bake with the settings in the Texture Set panel. Your fixes and adjustments stay."));
  ImGui::SameLine();
  if (ImGui::Button(L_("Export this map..."))) exportWhat = 1, showExport = true;
  ImGui::SetItemTooltip("%s", T_("The map as it ends up, fixes and adjustments included, as a grayscale PNG"));
  if (changed) d.markComposite();
}

// The 3D view shows a baked map while its layer is selected (painting it is seen), and the view it had before
// once another layer is selected, unless the view was changed in between.
void App::followBakedSelection() {
  Document& d = engine.doc;
  const Layer* sel = d.selectedLayer();
  uint32_t uid = sel && sel->isBaked() && engine.features.on("baked_layers") ? sel->uid : 0;
  if (uid == bakedFollowUid) return;
  ViewMode& mode = engine.viewOptions.mode;
  if (bakedFollowUid) {
    // leaving a baked map layer: the view before comes back, if the map still shows (else the view stays as chosen)
    const Layer* prev = d.findLayer(bakedFollowUid);
    bool shows = prev && mode == viewModeFromName(bakedMapKey(prev->bakedMap));
    if (!uid && shows && viewBeforeBaked >= 0) mode = (ViewMode)viewBeforeBaked;
    if (!shows) viewBeforeBaked = -1;
  }
  if (uid) {
    if (viewBeforeBaked < 0) viewBeforeBaked = (int)mode;
    mode = viewModeFromName(bakedMapKey(sel->bakedMap));
  } else {
    viewBeforeBaked = -1;
  }
  bakedFollowUid = uid;
}

}  // namespace patina
