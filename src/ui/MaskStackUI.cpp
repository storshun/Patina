// Layer mask UI: the smart-mask generator widgets and the mask effect stack.
#include <imgui.h>

#include <algorithm>

#include "ui/App.h"

namespace patina {

static ImTextureID texId(GLuint t) { return (ImTextureID)(intptr_t)t; }

bool App::generatorWidgets(MaskGenerator& g, const char* label) {
  Document& d = engine.doc;
  bool changed = false;
  int gt = g.type;
  bool more = engine.features.on("more_procedural");
  if (ImGui::Combo(label, &gt,
                   more ? Z_("None\0Edges (curvature)\0Cavity\0Occlusion\0Height gradient\0Noise\0Facing direction\0Material slot\0"
                             "Thin parts (thickness)\0Gradient (any direction)\0Drips (streaks running down)\0")
                        : Z_("None\0Edges (curvature)\0Cavity\0Occlusion\0Height gradient\0Noise\0Facing direction\0Material slot\0"
                             "Thin parts (thickness)\0"))) {
    g.type = gt;
    changed = true;
    Layer probe;
    probe.generator = g;
    d.ensureBakedFor(probe);
  }
  if (g.type == MaskGenerator::Facing || g.type == MaskGenerator::Gradient) {
    static const char* names[] = {N_("Up"), N_("Down"), N_("+X"), N_("-X"), N_("+Z (front)"), N_("-Z (back)")};
    bool facing = g.type == MaskGenerator::Facing;
    static const vec3 dirs[] = {{0, 1, 0}, {0, -1, 0}, {1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, -1}};
    int cur = -1;
    for (int k = 0; k < 6; ++k)
      if (glm::length(g.direction - dirs[k]) < 1e-3f) cur = k;
    if (ImGui::BeginCombo(facing ? L_("Facing") : L_("Lighter toward"), cur >= 0 ? T_(names[cur]) : T_("Camera"))) {
      for (int k = 0; k < 6; ++k)
        if (ImGui::Selectable(L_(names[k]), cur == k)) g.direction = dirs[k], changed = true;
      if (ImGui::Selectable(L_("Toward the camera"), cur < 0)) g.direction = -engine.objectCamera().forward(), changed = true;
      ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", facing ? T_("Surfaces facing this way (object space). Up: snow, dust, moss on top.\nLevel ~0.75 = within about 60 degrees.")
                                     : T_("Dark at one end of the model, light at the other (object space).\nLevel moves the middle; Contrast sharpens it."));
  }
  if (g.type == MaskGenerator::Noise && more) {
    int nt = g.noiseType;
    if (ImGui::Combo(L_("Kind"), &nt, Z_("Clouds\0Cells\0Cracks\0Ridges\0Streaks (running down)\0"))) g.noiseType = nt, changed = true;
  }
  if (g.type == MaskGenerator::Drips && ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", T_("Streaks running down the sides from random starts: rain marks, rust runs, leaks.\nNoise scale sets their size."));
  if (g.type == MaskGenerator::MaterialSlot) {
    uint32_t slots = 1;
    for (uint32_t v : d.mesh.faceMaterial) slots = std::max(slots, v + 1);
    if (ImGui::SliderInt(L_("Material slot"), &g.slot, 0, std::max<int>(0, slots - 1))) changed = true;
    ImGui::TextDisabled(P_("%u slot on this mesh (Blender material slots; view mode Material Slots shows them)",
                           "%u slots on this mesh (Blender material slots; view mode Material Slots shows them)", (long)slots),
                        slots);
  }
  if (g.type) {
    changed |= ImGui::SliderFloat(L_("Level"), &g.level, 0.f, 1.f);
    changed |= ImGui::SliderFloat(L_("Contrast"), &g.contrast, 0.f, 1.f);
    changed |= ImGui::SliderFloat(L_("Noise"), &g.noise, 0.f, 1.f);
    changed |= ImGui::SliderFloat(L_("Noise scale"), &g.noiseScale, 0.1f, 10.f, "%.2f", ImGuiSliderFlags_Logarithmic);
    changed |= ImGui::Checkbox(L_("Invert"), &g.invert);
  }
  return changed;
}

static const char* kFxLabels[] = {N_("Paint (the painted mask)"), N_("Smart mask (generator)"), N_("Fill (a value)"), N_("Noise"),
                                   N_("Levels"), N_("Blur"), N_("Invert"), N_("Grow / shrink"), N_("Edges (an outline of the mask)"),
                                   N_("Pattern (a material as a mask)")};
static const char* kFxShort[] = {N_("Paint"), N_("Smart mask"), N_("Fill"), N_("Noise"), N_("Levels"),
                                 N_("Blur"),  N_("Invert"),     N_("Grow"), N_("Edges"), N_("Pattern")};
static const char* kFxHelp[] = {
    nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    N_("Spreads the white of the mask out (more than 0) or pulls it in (less than 0)"),
    N_("Only the edges of what the mask covers so far: a rim round chips, a line round a patch"),
    N_("Any library material's height, brightness, roughness or metal as a mask: bricks, scratches, stripes, cracks...\n"
       "Drag a material from the Materials panel onto the stack to add one.")};
// the smart masks and noise kinds as the effect summaries show them, with the names the smart masks panel shows
// (maskGeneratorName / maskNoiseTypeName are the ids, for JSON and commands)
static const char* kGenShown[] = {N_("None"),      N_("Edges"),         N_("Cavity"),     N_("Occlusion"), N_("Height gradient"), N_("Noise"),
                                  N_("Facing"), N_("Material slot"), N_("Thin parts"), N_("Gradient"),  N_("Drips")};
static_assert(sizeof(kGenShown) / sizeof(kGenShown[0]) == MaskGenerator::Count, "a name for every smart mask");
static const char* kNoiseShown[] = {N_("Clouds"), N_("Cells"), N_("Cracks"), N_("Ridges"), N_("Streaks")};

int App::maskEffectMenu(bool baked) {
  int chosen = -1;
  int n = engine.features.on("more_procedural") ? (int)MaskEffect::Count : (int)MaskEffect::Grow;
  for (int t = 0; t < n; ++t) {
    if (baked && t == MaskEffect::Paint) continue;  // (a baked map layer's fixes are painted on the model)
    if (ImGui::Selectable(L_(kFxLabels[t]))) chosen = t;
    if (kFxHelp[t]) ImGui::SetItemTooltip("%s", T_(kFxHelp[t]));
  }
  return chosen;
}

MaskEffect App::newMaskEffect(int t) {
  MaskEffect fx;
  fx.type = t;
  if (t == MaskEffect::Generator) fx.blend = MaskEffect::Multiply, fx.gen.type = MaskGenerator::Edges;
  if (t == MaskEffect::Noise) fx.blend = MaskEffect::Multiply;
  if (t == MaskEffect::Pattern) {
    fx.blend = MaskEffect::Multiply;
    if (!engine.painter.material.id.empty()) fx.pattern.id = engine.painter.material.id;
  }
  return fx;
}

// a material dropped on the mask: a Pattern effect from it (on top of the stack)
bool App::maskPatternTarget(Layer& l) {
  bool added = false;
  {
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    engine.uiRects["mask.add_effect"] = {a.x, a.y, b.x - a.x, b.y - a.y};  // (for tests)
  }
  if (!engine.features.on("more_procedural") || !engine.features.on("drag_drop")) return false;
  if (ImGui::BeginDragDropTarget()) {
    for (const char* type : {"PATINA_MATERIAL", "PATINA_FILL"})
      if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(type, ImGuiDragDropFlags_AcceptBeforeDelivery)) {
        MaterialRef m;
        if (type[7] == 'M') m.id = (const char*)p->Data;
        else if (const Layer* src = engine.doc.findLayer(*(const uint32_t*)p->Data)) m = engine.doc.contentLayer(*src)->material;
        dropHint = m.id.empty() ? T_("A flat color has no pattern") : T_("A Pattern mask effect from it");
        if (p->IsDelivery() && !m.id.empty()) {
          l.convertMaskToStack();
          MaskEffect fx = newMaskEffect(MaskEffect::Pattern);
          fx.pattern = m;
          l.maskEffects.push_back(fx);
          openFxNext = (int)l.maskEffects.size() - 1;
          engine.doc.ensureBakedFor(l);
          added = true;
        }
      }
    ImGui::EndDragDropTarget();
  }
  return added;
}

// the pattern of a Pattern effect: its material (drag one here, or pick), what it takes and how it is projected
bool App::patternWidgets(MaskEffect& e) {
  bool changed = false;
  MaterialRef& m = e.pattern;
  auto* me = m.id.empty() ? nullptr : engine.materials.find(m.id);
  ImGui::BeginGroup();
  if (me) {
    ImGui::Image(texId(engine.materials.thumbnail(*me, engine.env)), ImVec2(40, 40) * uiScale);
    ImGui::SameLine();
  }
  if (ImGui::BeginCombo(L_("Material"), me ? me->def.name.c_str() : T_("Drag one here, or pick"), ImGuiComboFlags_HeightLarge)) {
    std::string lastCat;
    for (auto& en : engine.materials.entries()) {
      if (en->transient || en->def.type == MaterialType::Flat) continue;
      if (en->def.category != lastCat) ImGui::SeparatorText(en->def.category.c_str()), lastCat = en->def.category;
      if (ImGui::Selectable(en->def.name.c_str(), en->def.id == m.id)) m.id = en->def.id, changed = true;
    }
    ImGui::EndCombo();
  }
  ImGui::EndGroup();
  {
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    engine.uiRects["mask.pattern_material"] = {a.x, a.y, b.x - a.x, b.y - a.y};  // (for tests)
  }
  if (ImGui::BeginDragDropTarget()) {
    for (const char* type : {"PATINA_MATERIAL", "PATINA_FILL"})
      if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(type, ImGuiDragDropFlags_AcceptBeforeDelivery)) {
        dropHint = T_("The pattern of this effect");
        if (p->IsDelivery()) {
          if (type[7] == 'M') m.id = (const char*)p->Data;
          else if (const Layer* src = engine.doc.findLayer(*(const uint32_t*)p->Data)) m = engine.doc.contentLayer(*src)->material;
          changed = true;
        }
      }
    ImGui::EndDragDropTarget();
  }
  int src = e.source;
  if (ImGui::Combo(L_("From its"), &src, Z_("Height\0Brightness\0Roughness\0Metal\0"))) e.source = src, changed = true;
  changed |= ImGui::SliderFloat(L_("Level"), &e.level, 0.f, 1.f);
  changed |= ImGui::SliderFloat(L_("Contrast"), &e.contrast, 0.f, 1.f);
  ImGui::SetItemTooltip("%s", T_("0 keeps the values as they are; more makes a sharper cut at Level"));
  changed |= ImGui::Checkbox(L_("Invert"), &e.invert);
  int mp = m.mapping;
  if (ImGui::Combo(L_("Projection"), &mp, Z_("Tri-planar\0UV\0Screen\0"))) m.mapping = mp, changed = true;
  changed |= ImGui::SliderFloat(L_("Scale"), &m.scale, 0.05f, 20.f, "%.2f", ImGuiSliderFlags_Logarithmic);
  changed |= ImGui::SliderFloat(L_("Rotation"), &m.rotationDeg, -180.f, 180.f);
  return changed;
}

static std::string fxSummary(const MaskEffect& e) {
  switch (e.type) {
    case MaskEffect::Generator:
      return F_("Smart mask: %s", T_(kGenShown[std::clamp(e.gen.type, 0, (int)MaskGenerator::Count - 1)]));
    case MaskEffect::Fill: return F_("Fill %.2f", e.value);
    case MaskEffect::Blur: return F_("Blur %.1f px", e.radius);
    case MaskEffect::Levels: return F_("Levels %.2f-%.2f", e.inLow, e.inHigh);
    case MaskEffect::Noise: return e.noiseType ? F_("Noise: %s", T_(kNoiseShown[std::clamp(e.noiseType, 0, 4)])) : T_("Noise");
    case MaskEffect::Grow: return e.amount >= 0.f ? F_("Grow %.1f px", std::abs(e.amount)) : F_("Shrink %.1f px", std::abs(e.amount));
    case MaskEffect::Edges: return F_("Edges %.1f px", e.radius);
    case MaskEffect::Pattern: return e.pattern.id.empty() ? T_("Pattern: none") : F_("Pattern: %s", e.pattern.id.c_str());
    default: return T_(kFxShort[std::clamp(e.type, 0, (int)MaskEffect::Count - 1)]);
  }
}

bool App::maskStackWidgets(Layer& l) {
  bool changed = false;
  Document& d = engine.doc;
  ImGui::TextDisabled("%s", l.isBaked() ? T_("Applied from the bottom up, starting from the bake with your fixes")
                                        : T_("Mask effects (applied from the bottom up; the stack starts white)"));
  int n = (int)l.maskEffects.size();
  int removeAt = -1, moveFrom = -1, moveTo = -1;
  // shown top first, like the layer stack
  for (int vi = 0; vi < n; ++vi) {
    int i = n - 1 - vi;
    MaskEffect& e = l.maskEffects[i];
    ImGui::PushID(i);
    changed |= ImGui::Checkbox("##on", &e.enabled);
    ImGui::SameLine();
    if (i == openFxNext) ImGui::SetNextItemOpen(true), openFxNext = -1;  // a new effect shows its settings
    std::string summary = fxSummary(e);
    if (e.type == MaskEffect::Pattern)
      if (auto* me = e.pattern.id.empty() ? nullptr : engine.materials.find(e.pattern.id)) summary = F_("Pattern: %s", me->def.name.c_str());
    bool open = ImGui::TreeNodeEx("##fx", ImGuiTreeNodeFlags_SpanAvailWidth, "%s", summary.c_str());
    int dupAt = -1;
    if (contextMenus() && ImGui::BeginPopupContextItem("##fxctx")) {
      if (ImGui::MenuItem(e.enabled ? L_("Turn off") : L_("Turn on"))) e.enabled = !e.enabled, changed = true;
      if (ImGui::MenuItem(L_("Move up"), nullptr, false, i < n - 1)) moveFrom = i, moveTo = i + 1;
      if (ImGui::MenuItem(L_("Move down"), nullptr, false, i > 0)) moveFrom = i, moveTo = i - 1;
      if (ImGui::MenuItem(L_("Duplicate"))) dupAt = i;
      ImGui::Separator();
      if (ImGui::MenuItem(L_("Remove"))) removeAt = i;
      ImGui::EndPopup();
    }
    const float bh = ImGui::GetTextLineHeight() + 4.f * uiScale;
    ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - 3.f * (bh + 2.f));
    ImGui::BeginDisabled(i >= n - 1);
    if (iconButton("fx.up", "move_up", T_("Move up"), ui::IconState::Normal, bh, T_("Move this effect up the stack"))) moveFrom = i, moveTo = i + 1;
    ImGui::EndDisabled();
    ImGui::SameLine(0, 2);
    ImGui::BeginDisabled(i <= 0);
    if (iconButton("fx.down", "move_down", T_("Move down"), ui::IconState::Normal, bh, T_("Move this effect down the stack"))) moveFrom = i, moveTo = i - 1;
    ImGui::EndDisabled();
    ImGui::SameLine(0, 2);
    if (iconButton("fx.remove", "delete", T_("Remove"), ui::IconState::Normal, bh, T_("Remove this effect"))) removeAt = i;
    if (dupAt >= 0) {  // (after the row: the vector may grow)
      MaskEffect copy = l.maskEffects[dupAt];
      l.maskEffects.insert(l.maskEffects.begin() + dupAt + 1, copy);
      changed = true;
      ImGui::PopID();
      if (open) ImGui::TreePop();
      break;
    }
    if (open) {
      if (!e.isFilter()) {
        int b = e.blend;
        if (ImGui::Combo(L_("Blend"), &b, Z_("Normal\0Multiply\0Add\0Subtract\0Lighten (max)\0Darken (min)\0Screen\0"))) e.blend = b, changed = true;
      }
      changed |= ImGui::SliderFloat(L_("Opacity"), &e.opacity, 0.f, 1.f);
      switch (e.type) {
        case MaskEffect::Paint:
          ImGui::TextDisabled("%s", l.isBaked() ? T_("Not used on a baked map: its fixes are painted on the model.")
                                    : l.mask    ? T_("Paint the mask with the brush (Mask on the layer row).")
                                                : T_("No painted mask yet: + Mask on the layer row, then paint."));
          break;
        case MaskEffect::Generator: changed |= generatorWidgets(e.gen, L_("Generator")); break;
        case MaskEffect::Fill: changed |= ImGui::SliderFloat(L_("Value"), &e.value, 0.f, 1.f); break;
        case MaskEffect::Noise:
          changed |= ImGui::SliderFloat(L_("Size"), &e.scale, 0.1f, 20.f, "%.2f", ImGuiSliderFlags_Logarithmic);
          changed |= ImGui::SliderFloat(L_("Contrast"), &e.contrast, 0.f, 1.f);
          changed |= ImGui::SliderFloat(L_("Seed"), &e.seed, 0.f, 100.f, "%.0f");
          if (engine.features.on("more_procedural")) {
            int nt = e.noiseType;
            if (ImGui::Combo(L_("Kind"), &nt, Z_("Clouds\0Cells\0Cracks\0Ridges\0Streaks (running down)\0"))) e.noiseType = nt, changed = true;
          }
          break;
        case MaskEffect::Levels:
          changed |= ImGui::DragFloatRange2(L_("In"), &e.inLow, &e.inHigh, 0.005f, 0.f, 1.f);
          changed |= ImGui::SliderFloat(L_("Gamma"), &e.gamma, 0.1f, 10.f, "%.2f", ImGuiSliderFlags_Logarithmic);
          changed |= ImGui::DragFloatRange2(L_("Out"), &e.outLow, &e.outHigh, 0.005f, 0.f, 1.f);
          break;
        case MaskEffect::Blur: changed |= ImGui::SliderFloat(L_("Radius"), &e.radius, 0.f, 64.f, T_("%.1f px")); break;
        case MaskEffect::Grow:
          changed |= ImGui::SliderFloat(L_("Amount"), &e.amount, -32.f, 32.f, T_("%.1f px"));
          ImGui::SetItemTooltip("%s", T_("More than 0 spreads the white out; less than 0 pulls it in"));
          break;
        case MaskEffect::Edges:
          changed |= ImGui::SliderFloat(L_("Width"), &e.radius, 0.5f, 64.f, T_("%.1f px"));
          break;
        case MaskEffect::Pattern: changed |= patternWidgets(e); break;
        default: break;
      }
      ImGui::TreePop();
    }
    ImGui::PopID();
  }
  if (moveFrom >= 0) std::swap(l.maskEffects[moveFrom], l.maskEffects[moveTo]), changed = true;
  if (removeAt >= 0) l.maskEffects.erase(l.maskEffects.begin() + removeAt), changed = true;
  if (ImGui::BeginCombo("##addfx", l.isBaked() ? T_("Add an adjustment...") : T_("Add an effect..."))) {
    int t = maskEffectMenu(l.isBaked());
    ImGui::EndCombo();
    if (t >= 0) {
      l.maskEffects.push_back(newMaskEffect(t));
      openFxNext = (int)l.maskEffects.size() - 1;
      changed = true;
    }
  }
  changed |= maskPatternTarget(l);
  ImGui::SameLine();
  if (ImGui::SmallButton(l.isBaked() ? L_("Remove them all") : L_("Remove the stack"))) {
    // back to the simple form: keep the first smart mask as the layer's generator
    for (auto& e : l.maskEffects)
      if (e.type == MaskEffect::Generator) {
        l.generator = e.gen;
        break;
      }
    l.maskEffects.clear();
    changed = true;
  }
  if (changed) {
    d.ensureBakedFor(l);
  }
  return changed;
}

}  // namespace patina
