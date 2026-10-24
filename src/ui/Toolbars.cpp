// (0.36) The toolbars with icons: the tools on the left, grouped by job (paint, UV, retopology, symmetry,
// notes) and all there at once; the bar over the 3D view (brush, locks, view, the tool's own settings).
// The icons are icons/*.svg (see icons/README.md); with the toolbar_icons feature off the buttons show their
// names as before.
#include <imgui.h>
#include <imgui_internal.h>

#include <cstring>

#include "ui/App.h"
#include "ui/Theme.h"

namespace patina {

using ui::IconState;

static float iconPad(float h, float uiScale) { return std::max(2.5f * uiScale, h * 0.14f); }

float App::iconButtonWidth(const char* name, float h, bool withName) const {
  const bool icons = toolbarIcons();
  const bool names = !icons || toolbarNames || withName;
  if (!names) return h;
  float pad = iconPad(h, uiScale), fx = ImGui::GetStyle().FramePadding.x;
  return (icons ? pad + (h - 2.f * pad) + 5.f * uiScale : fx) + ImGui::CalcTextSize(name, nullptr, true).x + fx;
}

bool App::iconButton(const char* id, const char* icon, const char* name, IconState state, float h, const char* tip,
                     const char* keyId, bool withName, float w) {
  const bool icons = toolbarIcons();
  const bool names = !icons || toolbarNames || withName;
  const bool disabled = (GImGui->CurrentItemFlags & ImGuiItemFlags_Disabled) != 0;
  const float pad = iconPad(h, uiScale), isz = std::round(h - 2.f * pad);
  if (w <= 0.f) w = iconButtonWidth(name, h, withName);
  int pushed = 0;
  if (state == IconState::Chosen && iconAccent.w > 0.f) {
    // (0.47) the tool strip: the chosen tool in its step's color
    const ImVec4 dark(0.1f, 0.1f, 0.11f, 1.f);
    ImGui::PushStyleColor(ImGuiCol_Button, mixColor(dark, iconAccent, 0.84f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, mixColor(dark, iconAccent, 0.92f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, mixColor(dark, iconAccent, 1.f));
    pushed = 3;
  } else if (state == IconState::Chosen) {
    const ui::Theme& th = ui::theme();
    ImGui::PushStyleColor(ImGuiCol_Button, ui::themeColor(th.accentDeep));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, mixColor(th.accentDeep, th.accent, 0.35f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, mixColor(th.accentDeep, th.accent, 0.7f));
    pushed = 3;
  } else if (state == IconState::On) {
    const ui::Theme& th = ui::theme();
    ImGui::PushStyleColor(ImGuiCol_Button, ui::themeColor(th.accentDim));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, mixColor(th.accentDim, th.accent, 0.12f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, mixColor(th.accentDim, th.accent, 0.24f));
    pushed = 3;
  }
  ImGui::PushID(id);
  bool clicked = ImGui::Button("##b", ImVec2(w, h));
  ImGui::PopStyleColor(pushed);
  ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  engine.uiRects[std::string("btn.") + id] = {a.x, a.y, b.x - a.x, b.y - a.y};  // (for tests and the manual's screenshots)
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (state == IconState::On) {
    float t = std::max(1.f, 1.5f * uiScale);
    dl->AddRect(a + ImVec2(t * 0.5f, t * 0.5f), b - ImVec2(t * 0.5f, t * 0.5f), ui::themeColor(ui::theme().accent, disabled ? 0.43f : 1.f),
                ImGui::GetStyle().FrameRounding, 0, t);
  }
  ui::IconColors col = ui::iconColors(disabled ? IconState::Disabled : state);
  if (disabled && state != IconState::Normal) col.alpha = 0.45f;
  float textX = a.x + ImGui::GetStyle().FramePadding.x;
  if (icons) {
    ImVec2 ip = names ? ImVec2(a.x + pad, a.y + pad) : ImVec2(a.x + std::round((w - isz) * 0.5f), a.y + pad);
    ui::drawIcon(dl, icon, ip, isz, col);
    textX = ip.x + isz + 5.f * uiScale;
  }
  if (names) {
    ImVec2 ts = ImGui::CalcTextSize(name, nullptr, true);
    if (!icons) textX = a.x + std::max(ImGui::GetStyle().FramePadding.x, (w - ts.x) * 0.5f);
    ImU32 tc = state == IconState::Normal ? ImGui::GetColorU32(ImGuiCol_Text) : ui::themeColor(ui::theme().onAccent, disabled ? 0.47f : 1.f);
    ImGui::PushClipRect(a, b, true);
    dl->AddText(ImVec2(textX, a.y + (h - ts.y) * 0.5f), tc, name, ImGui::FindRenderedTextEnd(name));
    ImGui::PopClipRect();
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) {
    const char* k = keyId ? keyLabel(keyId) : nullptr;
    std::string t(name, ImGui::FindRenderedTextEnd(name));
    if (k) t += std::string("  (") + k + ")";
    if (tip && *tip) t += std::string("\n") + tip;
    ImGui::SetTooltip("%s", t.c_str());
  }
  if (keyId && contextMenus() && ImGui::BeginPopupContextItem("##shortcut")) {
    shortcutMenu(keyId, name);
    ImGui::EndPopup();
  }
  ImGui::PopID();
  return clicked;
}

void App::shortcutMenu(const char* keyId, const char* name) {
  ImGui::TextDisabled("%.*s", int(ImGui::FindRenderedTextEnd(name) - name), name);
  ImGui::Separator();
  const char* k = keyLabel(keyId);
  ImGui::MenuItem(k ? F_("Shortcut: %s", k).c_str() : L_("No Shortcut"), nullptr, false, false);
  if (ImGui::MenuItem(L_("Change Shortcut..."))) showKeymap = true, keymapCapture = keyId, keymapScrollTo = true;
  for (auto& a : keymap)
    if (keyId == std::string(a.id) && a.chord != a.defaultChord && ImGui::MenuItem(L_("Reset Shortcut"))) {
      a.chord = a.defaultChord;
      saveKeymap();
    }
  // (0.40) Blender's right-click > Add to Quick Favorites (the Q pie)
  ImGui::Separator();
  favoriteMenuItems(keyId);
  if (toolbarIcons() && ImGui::MenuItem(L_("Show Names Beside Icons"), nullptr, toolbarNames)) toolbarNames = !toolbarNames, saveSession(true);
}

void App::selectTool(ViewportTool t) {
  tool = t, quickPickActive = false;
  toolPicked = true;  // (0.46: the Guided layout goes to the tool's step)
  // a retopology tool's settings are in the Retopology window: it opens (as a tab) if it was closed
  if (retopoTool()) showRetopo = true;
}

void App::drawToolbar() {
  // (1.2.8) with names beside the icons, at least as wide as they need (the names shrank to unreadable sizes)
  ImGui::SetNextWindowSizeConstraints(ImVec2(std::max(40.f, toolbarNamesW), 100), ImVec2(std::max(260.f, toolbarNamesW), 10000));
  beginPanel(L_("Tools"), ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar);
  struct T {
    ViewportTool t;
    const char* icon;
    const char* label;
    const char* tip;
    const char* key;
  };
  struct G {
    const char* name;
    std::vector<T> tools;
  };
  std::vector<G> groups = {
      {N_("Paint"),
       {{ViewportTool::Paint, "paint", N_("Paint"), N_("Paint material"), "tool.paint"},
        {ViewportTool::Erase, "erase", N_("Erase"), N_("Erase paint"), "tool.erase"},
        {ViewportTool::Blur, "blur", N_("Blur"), N_("Blur / soften paint (reaches across UV seams)"), "tool.blur"},
        {ViewportTool::Smudge, "smudge", N_("Smudge"), N_("Drag the paint under the brush along the stroke.\n"
                                                         "Flow and pressure set how far it carries."), "tool.smudge"},
        {ViewportTool::Clone, "clone", N_("Clone"), N_("Clone stamp: Ctrl+click sets the source"), "tool.clone"},
        {ViewportTool::Fill, "fill", N_("Fill"), N_("Polygon fill: face / UV island / connected / mesh.\nAlt+click erases."), "tool.fill"},
        {ViewportTool::Picker, "pick", N_("Pick"), N_("Pick color & values from the model.\nHold the key for a quick pick while painting."), "tool.picker"}}},
      {N_("UV"),
       {{ViewportTool::SelectFaces, "select", N_("Select"), N_("Select faces / islands for UV editing"), "tool.select"},
        {ViewportTool::Seams, "seams", N_("Seams"), N_("Seam brush: drag along the model to lay a seam along its edges (Ctrl clears).\n"
                                                      "A click toggles one edge; Shift+click continues from the last seam's end."), "tool.seams"},
        {ViewportTool::Island, "island", N_("Island"), N_("Island brush: paint over a part of the model, then press the Make-island\n"
                                                         "key (or the button) to unwrap it as one upright UV island at the right\n"
                                                         "texel density, placed in free space. Ctrl+drag removes faces."), "tool.island"}}}};
  if (engine.features.on("retopology"))
    groups.push_back(
        {N_("Retopo"),
         {{ViewportTool::ZoneLine, "zone_lines", N_("Lines"), N_("Retopology zone lines: draw on the model where the edge flow changes (round the\n"
                                                                "eyes, the mouth, the cranium...). Each line becomes an edge loop and splits the\n"
                                                                "model into zones. Start or end near another line (a white ring shows) to meet\n"
                                                                "it exactly; end where you began to close it. Ctrl+click takes a line away."), "tool.zone_line"},
          {ViewportTool::Patch, "zone", N_("Zone"), N_("Retopology zones: click a zone to choose it and set its resolution\n"
                                                      "(how many quads per m² it gets, compared with the rest)."), "tool.patch"},
          {ViewportTool::Flow, "flow", N_("Flow"), N_("Retopology flow: a stroke inside a zone sets which way its quads run.\n"
                                                     "Ctrl+click on a stroke takes it away."), "tool.flow"},
          {ViewportTool::Density, "density", N_("Density"), N_("Retopology density brush: fine-tunes the resolution where you paint,\n"
                                                              "within a zone. Ctrl: back to normal."), "tool.density"}}});
  const bool icons = toolbarIcons();
  bool names = !icons || toolbarNames;
  const float avail = ImGui::GetContentRegionAvail().x, sp = std::round(3.f * uiScale);
  {  // (1.2.8) the width the names need, for layouts that size the strip themselves (the Blender layout)
    float widest = ImGui::CalcTextSize(T_("Mirror X")).x;
    for (auto& g : groups)
      for (auto& t : g.tools) widest = std::max(widest, ImGui::CalcTextSize(T_(t.label)).x);
    float bhN = std::round(34.f * uiScale);
    toolbarNamesW = names ? (icons ? bhN : 0.f) + widest + ImGui::GetStyle().FramePadding.x * 2.f +
                                ImGui::GetStyle().WindowPadding.x * 2.f + 8.f * uiScale
                          : 0.f;
    // a strip still narrower than that (a dock kept its width) shows the icons alone, not names too small to read
    if (names && icons && avail + ImGui::GetStyle().WindowPadding.x * 2.f + 1.f < toolbarNamesW) names = false;
  }
  const bool namesPref = toolbarNames;
  toolbarNames = toolbarNames && names;  // (iconButton reads it; back after the strip)
  // icons alone: two columns when they fit (a narrow column gets one); with names: one row per tool
  int cols = 1;
  float bw = avail, bh = std::round(34.f * uiScale);
  if (!names) {
    cols = avail >= 2.f * 26.f * uiScale + sp ? 2 : 1;
    bw = std::floor(std::min(40.f * uiScale, (avail - sp * (cols - 1)) / cols));
    bh = bw;
  } else if (!icons) {
    bh = std::round(30.f * uiScale);
  }
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(sp, sp));
  int gi = 0;
  // (0.47) each group in the color of the step it belongs to (the Guided layout's): a stripe beside its tools, its
  // name in that color, its chosen tool filled with it
  const bool stepColors = engine.features.on("guided_layout");  // (1.2.8: the Blender layout's tabs have the colors too)
  float groupY0 = 0.f;
  ImVec4 groupColor(0, 0, 0, 0);
  auto endGroup = [&] {
    if (groupColor.w <= 0.f) return;
    float y1 = ImGui::GetItemRectMax().y, x = ImGui::GetWindowPos().x + std::round(2.f * uiScale);
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(x, groupY0), ImVec2(x + std::round(3.f * uiScale), y1),
                                              mixColor(groupColor, groupColor, 0.f, 0.85f), 1.5f * uiScale);
    groupColor.w = 0.f;
  };
  auto header = [&](const char* name, GuidedStep step) {
    endGroup();
    if (gi++) ImGui::Dummy(ImVec2(0, 2.f * uiScale));
    groupY0 = ImGui::GetCursorScreenPos().y;
    groupColor = stepColors ? stepColor(step) : ImVec4(0, 0, 0, 0);
    iconAccent = groupColor;
    // the group's name when it fits the column, else a line
    ImGui::SetWindowFontScale(0.78f);
    float tw = ImGui::CalcTextSize(name).x;
    if (tw <= avail) {
      ImVec4 c = groupColor;
      ImGui::PushStyleColor(ImGuiCol_Text, stepColors ? mixColor(ImVec4(0.6f, 0.61f, 0.64f, 1.f), c, 0.55f)
                                                      : ImGui::GetColorU32(ImGuiCol_TextDisabled));
      ImGui::TextUnformatted(name);
      ImGui::PopStyleColor();
    } else {
      ImGui::Separator();
    }
    ImGui::SetWindowFontScale(1.f);
  };
  auto place = [&](int k) {
    if (k % cols) ImGui::SameLine(0, sp);
  };
  // (0.46) the Guided layout shows only the step's tools (the bits of guidedToolGroups: 1 paint, 2 UV, 4 retopology,
  // 8 symmetry, 16 notes)
  const int shown = guidedOn() ? guidedToolGroups : ~0;
  for (size_t gk = 0; gk < groups.size(); ++gk) {
    auto& g = groups[gk];
    if (!(shown & (1 << gk))) continue;
    header(T_(g.name), gk == 0 ? GuidedStep::Paint : gk == 1 ? GuidedStep::UVs : GuidedStep::Model);
    for (size_t k = 0; k < g.tools.size(); ++k) {
      const T& t = g.tools[k];
      const char* label = T_(t.label);
      place((int)k);
      // names that don't fit a narrow column get a smaller font rather than being cut off
      float room = bw - (icons ? bh : 0.f) - ImGui::GetStyle().FramePadding.x * 2.f, tw = ImGui::CalcTextSize(label).x;
      bool shrink = names && tw > room && room > 8.f;
      if (shrink) ImGui::SetWindowFontScale(std::max(0.6f, room / tw));
      ImGui::BeginDisabled(!engine.doc.hasMesh() && (t.t == ViewportTool::ZoneLine || t.t == ViewportTool::Patch ||
                                                       t.t == ViewportTool::Flow || t.t == ViewportTool::Density));
      if (iconButton(t.key, t.icon, label, tool == t.t ? IconState::Chosen : IconState::Normal, bh, T_(t.tip), t.key, false, bw))
        selectTool(t.t);
      ImGui::EndDisabled();
      if (shrink) ImGui::SetWindowFontScale(1.f);
      ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
      engine.uiRects[std::string("tools.") + t.key] = {a.x, a.y, b.x - a.x, b.y - a.y};  // (for tests)
    }
  }
  // ---- symmetry
  if (shown & 8) {
    header(T_("Symmetry"), GuidedStep::Paint);
    Painter& P = engine.painter;
    static const char* kAxis[] = {"X", "Y", "Z"};
    static const char* kMirrorIcon[] = {"mirror_x", "mirror_y", "mirror_z"};
    static const char* kMirrorName[] = {N_("Mirror X"), N_("Mirror Y"), N_("Mirror Z")};
    for (int i = 0; i < 3; ++i) {
      place(i);
      std::string tip = F_("Mirror painting across the object %s axis (combine axes freely)", kAxis[i]);
      if (iconButton(kMirrorIcon[i], kMirrorIcon[i], T_(kMirrorName[i]), P.symmetry[i] ? IconState::On : IconState::Normal, names ? std::round(26.f * uiScale) : bh,
                     tip.c_str(), i == 0 ? "sym.x" : nullptr, false, bw))
        P.symmetry[i] = !P.symmetry[i];
    }
    {
      place(3);
      bool on = P.radialCount > 1;
      std::string lbl = (on ? F_("Radial %d", P.radialCount) : std::string(T_("Radial"))) + "###radial";
      if (iconButton("radial", "radial", lbl.c_str(), on ? IconState::On : IconState::Normal, names ? std::round(26.f * uiScale) : bh,
                     T_("Radial symmetry: repeat every dab around an axis"), nullptr, false, bw))
        ImGui::OpenPopup("RadialSym");
      if (ImGui::BeginPopup("RadialSym")) {
        ImGui::SetNextItemWidth(160 * uiScale);
        ImGui::SliderInt(L_("Copies"), &P.radialCount, 1, 32);
        ImGui::SetNextItemWidth(160 * uiScale);
        ImGui::Combo(L_("Axis"), &P.radialAxis, "X\0Y\0Z\0");
        ImGui::TextDisabled("%s", T_("1 copy = off. Works together with the X/Y/Z mirrors;\nall copies go through the object origin."));
        ImGui::EndPopup();
      }
    }
  }
  // ---- (1.2) the scene: the Object tool
  if ((shown & 32) && engine.features.on("scene_objects")) {
    header(T_("Scene"), GuidedStep::Model);
    if (iconButton("tool.object", "move", T_("Object"), tool == ViewportTool::Object ? IconState::Chosen : IconState::Normal, bh,
                   T_("Object tool: click an object to paint it; drag the gizmo to move, turn or scale it\n"
                      "(the key again: the next of the three). Ctrl snaps; Esc or a right-click while dragging cancels."),
                   "tool.object", false, bw))
      selectTool(ViewportTool::Object);
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    engine.uiRects["tools.tool.object"] = {a.x, a.y, b.x - a.x, b.y - a.y};
  }
  // ---- notes
  if (shown & 16) {
    header(T_("Notes"), GuidedStep::Paint);
    if (iconButton("tool.note", "note", T_("Note"), tool == ViewportTool::Note ? IconState::Chosen : IconState::Normal, bh,
                   T_("Notes: click on the model to pin a comment; click a pin to edit it.\nNotes are saved in the project (Notes panel)."),
                   "tool.note", false, bw))
      selectTool(ViewportTool::Note);
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    engine.uiRects["tools.tool.note"] = {a.x, a.y, b.x - a.x, b.y - a.y};
  }
  endGroup();
  iconAccent = ImVec4(0, 0, 0, 0);
  toolbarNames = namesPref;
  ImGui::PopStyleVar();
  // right-click on the empty part of the toolbar
  if (contextMenus() && ImGui::BeginPopupContextWindow("##toolsctx", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
    if (ImGui::MenuItem(L_("Show Names Beside Icons"), nullptr, toolbarNames, toolbarIcons())) toolbarNames = !toolbarNames, saveSession(true);
    if (ImGui::MenuItem(L_("Keyboard Shortcuts..."))) showKeymap = true;
    ImGui::EndPopup();
  }
  endPanel();
}

// ------------------------------------------------------------------ the bar over the 3D view

void App::viewportBar() {
  Painter& P = engine.painter;
  const bool icons = toolbarIcons(), names = !icons || toolbarNames;
  const float fh = ImGui::GetFrameHeight();
  ImGui::SetCursorPos(ImVec2(6, 4) + ImGui::GetCursorPos());
  const float barX = ImGui::GetCursorPosX();
  bool first = true, wide = false;
  // the next item, or group of items, `w` wide: beside the last, or on a new row when the viewport is narrow
  auto next = [&](float w) {
    float spacing = ImGui::GetStyle().ItemSpacing.x * (wide ? 2.5f : 1.f);
    wide = false;
    if (first) {
      first = false;
      return;
    }
    float right = ImGui::GetItemRectMax().x + spacing + w;
    if (right <= ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x - 4.f) ImGui::SameLine(0, spacing);
    else ImGui::SetCursorPosX(barX);
  };
  // a little more room before the next group, and a thin line between the groups
  auto gap = [&] {
    wide = true;
    if (first) return;
    ImVec2 r = ImGui::GetItemRectMax();
    float x = r.x + std::round(ImGui::GetStyle().ItemSpacing.x * 1.25f);
    ImGui::GetWindowDrawList()->AddLine(ImVec2(x, r.y - fh + 3.f * uiScale), ImVec2(x, r.y - 3.f * uiScale),
                                        ImGui::GetColorU32(ImGuiCol_Separator), std::max(1.f, uiScale));
  };
  // buttons side by side, one of them chosen (the names and tips: N_() tables, translated here)
  auto segmented = [&](int n, const char* const* ids, const char* const* iconNames, const char* const* nameList,
                       const char* const* tips, int chosen) {
    float total = 0;
    for (int i = 0; i < n; ++i) total += iconButtonWidth(T_(nameList[i]), fh) + (i ? 1.f : 0.f);
    next(total);
    int clicked = -1;
    for (int i = 0; i < n; ++i) {
      if (i) ImGui::SameLine(0, 1.f);
      if (iconButton(ids[i], iconNames[i], T_(nameList[i]), chosen == i ? IconState::Chosen : IconState::Normal, fh, T_(tips[i]))) clicked = i;
    }
    return clicked;
  };
  // a brush slider: its icon and the number (with names: "Size 34" as before)
  auto slider = [&](const char* icon, const char* id, const char* name, float* v, float lo, float hi, const char* numFmt,
                    const char* nameFmt, ImGuiSliderFlags fl, const char* tip) {
    {
      // (1.2.8) one field per setting: its icon inside at the left, the field filled up to the value (as Blender's
      // sliders), the name and number in the middle. The icon stood apart, beside a slider with its own grab: which
      // icon went with which slider was hard to see
      const float fw = std::round((names ? 124.f : 84.f) * uiScale);
      next(fw);
      const ImVec2 p = ImGui::GetCursorScreenPos(), q = p + ImVec2(fw, fh);
      ImDrawList* dl = ImGui::GetWindowDrawList();
      const ImGuiID sid = ImGui::GetID(id);
      const bool active = ImGui::GetActiveID() == sid, hovered = ImGui::IsMouseHoveringRect(p, q) && ImGui::IsWindowHovered();
      const float rnd = ImGui::GetStyle().FrameRounding;
      dl->AddRectFilled(p, q, ImGui::GetColorU32(active ? ImGuiCol_FrameBgActive : hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg), rnd);
      float t = (fl & ImGuiSliderFlags_Logarithmic) && lo > 0.f ? std::log(std::max(*v, lo) / lo) / std::log(hi / lo) : (*v - lo) / (hi - lo);
      t = std::clamp(t, 0.f, 1.f);
      if (t > 0.f) dl->AddRectFilled(p, ImVec2(p.x + fw * t, q.y), mixColor(ui::theme().accentDeep, ui::theme().accentDeep, 0.f, 0.55f), rnd);
      if (icons) {
        float isz = std::round(fh - 2.f * iconPad(fh, uiScale));
        ui::drawIcon(dl, icon, p + ImVec2(std::round(3.f * uiScale), (fh - isz) * 0.5f), isz, ui::iconColors(IconState::Normal));
      }
      ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
      ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0, 0, 0, 0));
      ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0, 0, 0, 0));
      ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0, 0, 0, 0));
      ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(0, 0, 0, 0));
      if (icons) ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(fh * 0.5f, ImGui::GetStyle().FramePadding.y));
      ImGui::SetNextItemWidth(fw);
      ImGui::SliderFloat(id, v, lo, hi, names ? nameFmt : numFmt, fl);
      if (icons) ImGui::PopStyleVar();
      ImGui::PopStyleColor(5);
      ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
      engine.uiRects[std::string("toolbar.slider:") + (id + 2)] = {a.x, a.y, b.x - a.x, b.y - a.y};
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s\n%s", name, tip);
      return;
    }
  };
  // (0.46) the brush's settings only for the tools that paint with a brush (not Select, Seams, Fill, Pick, Note, the
  // retopology lines and zones), and not while the view only looks
  const bool usesBrush = !viewOnly && (tool == ViewportTool::Paint || tool == ViewportTool::Erase || tool == ViewportTool::Blur ||
                                       tool == ViewportTool::Clone || tool == ViewportTool::Smudge || tool == ViewportTool::Island ||
                                       tool == ViewportTool::Density);
  if (usesBrush) {
    slider("size", "##size", T_("Size"), &P.brush.size, 1.f, 600.f, "%.0f", T_("Size %.0f"), ImGuiSliderFlags_Logarithmic,
           T_("The brush's diameter in pixels. Ctrl+wheel or [ and ] change it in the view; Ctrl+right-drag sets\n"
              "size (sideways) and hardness (up and down)."));
    slider("hardness", "##hard", T_("Hardness"), &P.brush.hardness, 0.f, 1.f, "%.2f", T_("Hard %.2f"), 0,
           T_("How much of the brush paints at full strength before its edge fades: 1 is a crisp edge, 0 all soft."));
    slider("flow_rate", "##flow", T_("Flow"), &P.brush.flow, 0.01f, 1.f, "%.2f", T_("Flow %.2f"), 0,
           T_("How much paint each dab lays down: low flow builds up where strokes go over each other."));
    slider("opacity", "##opac", T_("Opacity"), &P.brush.opacity, 0.f, 1.f, "%.2f", T_("Opacity %.2f"), 0,
           T_("The most a stroke can cover, however many times it goes over a spot."));
    slider("spacing", "##spacing", T_("Spacing"), &P.brush.spacing, 0.01f, 2.f, "%.2f", T_("Spacing %.2f"), ImGuiSliderFlags_Logarithmic,
           T_("The gap between dabs along a stroke, in brush sizes (small: a smooth line)."));
    // projection: three buttons
    if (tool == ViewportTool::Island && engine.features.on("island_reach")) {
      // (0.43) the Island brush's own: what a dab takes
      static const char* kIds[] = {"island.reach_screen", "island.reach_surface"};
      static const char* kIcon[] = {"proj_screen", "proj_surface"};
      static const char* kName[] = {N_("Screen"), N_("Surface")};
      static const char* kTip[] = {N_("The faces you see inside the circle (a surface seen edge-on gives a long strip)"),
                                   N_("The faces within the brush's size along the surface from the cursor, whatever the\n"
                                      "angle: never a layer behind (a cloth with thickness) or a part only near in space")};
      gap();
      int c = segmented(2, kIds, kIcon, kName, kTip, std::clamp(islandReach, 0, 1));
      if (c >= 0) islandReach = c;
    } else {
      static const char* kIcon[] = {"proj_screen", "proj_surface", "proj_uv"};
      static const char* kName[] = {N_("Screen"), N_("Surface 3D"), N_("UV")};
      static const char* kTip[] = {N_("Dabs projected from the view, the way Substance Painter paints"),
                                   N_("A round brush that follows the surface in 3D"), N_("Dabs placed in texture space")};
      gap();
      int c = segmented(3, kIcon, kIcon, kName, kTip, (int)P.brush.projection);
      if (c >= 0) P.brush.projection = (BrushProjection)c;
    }
  }
  bool brushTool = !viewOnly && (tool == ViewportTool::Paint || tool == ViewportTool::Erase || tool == ViewportTool::Blur ||
                                 tool == ViewportTool::Clone || tool == ViewportTool::Smudge);
  auto toggle = [&](const char* id, const char* icon, const char* name, bool& v, const char* tip, const char* keyId) {
    next(iconButtonWidth(name, fh));
    if (iconButton(id, icon, name, v ? IconState::On : IconState::Normal, fh, tip, keyId)) v = !v;
  };
  if (brushTool) {
    gap();
    toggle("lock_topology", "lock_topology", T_("Topology lock"), P.lockTopology,
           T_("A stroke only paints the connected part of the mesh it starts on.\n"
              "Parts that touch or overlap on screen but are separate pieces of mesh stay clean."),
           "brush.lock_topology");
    toggle("lock_island", "lock_island", T_("Island lock"), P.lockIsland,
           T_("A stroke only paints the UV island it starts on, and stops at its seams."), "brush.lock_island");
    if (engine.features.on("texture_set_tools") && (engine.doc.mesh.tiles.active() || engine.doc.mesh.materialNames.size() > 1))
      toggle("lock_set", "lock_set", T_("Set lock"), P.lockSet,
             T_("A stroke only paints the texture set it starts on (with one texture: its material slot).\n"
                "Works together with the topology and island locks."),
             "brush.lock_set");
    if (engine.features.on("face_sets") && !engine.doc.faceSets.empty())  // (1.2)
      toggle("lock_face_set", "face_sets", T_("Face set lock"), P.lockFaceSet,
             T_("A stroke only paints the face set it starts on (Face Sets panel).\n"
                "Works together with the other locks."),
             nullptr);
  }
  // view: the view mode, wireframe and seams
  gap();
  {
    // (1.2.8) its icon inside the field, as the sliders'
    float cw = std::round((icons ? 150.f : 130.f) * uiScale);
    next(cw);
    bool hov = false;
    const ImVec2 cp = ImGui::GetCursorScreenPos();
    ImDrawList* barDl = ImGui::GetWindowDrawList();
    if (icons) ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(fh, ImGui::GetStyle().FramePadding.y));
    ImGui::SetNextItemWidth(cw);
    int vm = (int)engine.viewOptions.mode;
    bool open = ImGui::BeginCombo("##view", T_(viewModeLabel(std::clamp(vm, 0, (int)ViewMode::Count - 1))));
    if (icons) {
      ImGui::PopStyleVar();
      float isz = std::round(fh - 2.f * iconPad(fh, uiScale));
      ui::drawIcon(barDl, "view", cp + ImVec2(std::round(3.f * uiScale), (fh - isz) * 0.5f), isz, ui::iconColors(IconState::Normal));
    }
    if (open) {
      for (int m = 0; m < (int)ViewMode::Count; ++m) {
        if (!viewModeAvailable((ViewMode)m)) continue;
        if (ImGui::Selectable(L_(viewModeLabel(m)), m == vm)) engine.viewOptions.mode = (ViewMode)m;
      }
      ImGui::EndCombo();
    }
    if (hov || ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
      ImGui::SetTooltip("%s", T_("View mode\nWhat the 3D view shows: the material, one channel, a bake, a check..."));
  }
  // (0.39) the path tracer's settings and progress, while it is the view mode
  if (engine.viewOptions.mode == ViewMode::PathTraced) {
    next(iconButtonWidth(T_("Path tracing"), fh));
    if (iconButton("path_trace", "path_trace", T_("Path tracing"), IconState::On, fh,
                   T_("The path tracer's settings: how many samples, light bounces, the ground's shadow.\n"
                      "The picture gets cleaner the longer nothing changes.")))
      ImGui::OpenPopup("##path_trace");
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImGui::SetNextWindowPos(ImVec2(a.x, b.y + 2.f), ImGuiCond_Appearing);
    if (ImGui::BeginPopup("##path_trace")) {
      pathTraceSettingsUI();
      ImGui::EndPopup();
    }
  }
  toggle("wire", "wire", T_("Wireframe"), engine.viewOptions.wireframe, T_("Draw the mesh's edges over the model"), "view.wireframe");
  toggle("seams_show", "seams_show", T_("Seams"), engine.viewOptions.seams, T_("Show the UV seams in red"), "view.seams");
  // (0.37) the retopology preview: the zones or density and the quads over the model whatever tool is chosen
  // (before, only while the Retopology window was in front); the arrow beside it chooses what shows
  if (engine.features.on("retopology") && engine.features.on("retopo_preview_toggle") && engine.doc.hasMesh()) {
    ViewportOptions& vo = engine.viewOptions;
    const char* name = T_("Retopology preview");
    const float aw = std::round(fh * 0.6f);
    next(iconButtonWidth(name, fh) + 1.f + aw);
    if (iconButton("retopo_preview", "retopo_view", name, vo.retopoPreview ? IconState::On : IconState::Normal, fh,
                   T_("Show the retopology on the model: the zones (or the density) and the quads, kept up to date\n"
                      "with a quick preview as you change them. The retopology tools show them while chosen."),
                   "view.retopo_preview"))
      vo.retopoPreview = !vo.retopoPreview;
    ImGui::SameLine(0, 1.f);
    ImGui::PushID("retopo_preview.more");
    if (vo.retopoPreview) {
      ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(88, 62, 40, 255));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(104, 74, 48, 255));
      ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(120, 84, 54, 255));
    }
    bool open = ImGui::Button("##arrow", ImVec2(aw, fh));
    if (vo.retopoPreview) ImGui::PopStyleColor(3);
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    engine.uiRects["btn.retopo_preview.more"] = {a.x, a.y, b.x - a.x, b.y - a.y};
    // (RenderArrow's box is a font size wide, its middle half its scale down)
    const float fs = ImGui::GetFontSize();
    ImGui::RenderArrow(ImGui::GetWindowDrawList(), ImVec2((a.x + b.x) * 0.5f - fs * 0.5f, (a.y + b.y) * 0.5f - fs * 0.35f),
                       ImGui::GetColorU32(ImGuiCol_Text), ImGuiDir_Down, 0.7f);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s", T_("What the retopology preview shows"));
    if (open) ImGui::OpenPopup("##retopo_show");
    ImGui::SetNextWindowPos(ImVec2(a.x, b.y + 2.f), ImGuiCond_Appearing);  // (under the arrow, not at the pointer)
    if (ImGui::BeginPopup("##retopo_show")) {
      ImGui::TextDisabled("%s", T_("Retopology preview"));
      ImGui::Separator();
      retopoShowSettings(true);
      ImGui::EndPopup();
    }
    ImGui::PopID();
  }
  // ---- the tool's own settings (none while the view only looks: the Guided layout's Model, Bake and Export)
  if (viewOnly) return;
  // (0.37.1) pinning from the 3D view: the selected (or painted) faces' islands; the pinned ones are striped blue
  const size_t pinnedSel = tool == ViewportTool::SelectFaces || tool == ViewportTool::Island ? pinnedSelectedFaces() : 0;
  auto unpinButton = [&](const char* id) {
    ImGui::BeginDisabled(pinnedSel == 0);
    next(iconButtonWidth(T_("Unpin"), fh));
    if (iconButton(id, "unpin", T_("Unpin"), IconState::Normal, fh,
                   T_("Unpin the selected islands, so UV edits can move them again.\n"
                      "Pinned islands have blue stripes on the model while this tool is chosen."),
                   "uv.unpin"))
      uvPinSelection(false, false);
    ImGui::EndDisabled();
  };
  auto facesText = [&](size_t nsel) {
    next(ImGui::CalcTextSize(F_("%zu faces, %zu pinned", size_t(88888), size_t(88888)).c_str()).x);  // (room for five digits)
    ImGui::AlignTextToFramePadding();
    if (pinnedSel) ImGui::TextDisabled(T_("%zu faces, %zu pinned"), nsel, pinnedSel);
    else ImGui::TextDisabled(T_("%zu faces"), nsel);
  };
  if (tool == ViewportTool::Fill) {
    gap();
    next(110 * uiScale);
    ImGui::SetNextItemWidth(110 * uiScale);
    int fm = (int)P.fillMode;
    if (ImGui::Combo("##fillmode", &fm, Z_("Face\0UV Island\0Connected\0Whole Mesh\0Selection\0Material slot\0"))) P.fillMode = (FillMode)fm;
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", T_("What a click fills: the face, its UV island, the connected part, the whole mesh, the selected\n"
                                 "faces, or every face of its material slot (the same Blender material)"));
  }
  if (tool == ViewportTool::Island) {
    gap();
    size_t nsel = engine.doc.selectedFaces().size();
    ImGui::BeginDisabled(nsel == 0);
    next(iconButtonWidth(T_("Make island"), fh, true));
    if (iconButton("island.make", "make_island", T_("Make island"), IconState::Normal, fh,
                   T_("Unwrap the painted faces as one upright UV island at the right texel density, placed in free space"),
                   "island.make", true))
      makeIslandFromSelection();
    ImGui::EndDisabled();
    next(iconButtonWidth(T_("Clear"), fh));
    if (iconButton("island.clear", "deselect", T_("Clear"), IconState::Normal, fh, T_("Clear the painted faces"))) {
      std::fill(engine.doc.faceSelection.begin(), engine.doc.faceSelection.end(), 0);
      engine.doc.updateSelectionFlags();
    }
    next(130 * uiScale);
    ImGui::SetNextItemWidth(130 * uiScale);
    ImGui::Combo("##islorient", &islandOrient, Z_("Keep orientation\0Upright\0Longest side\0"));
    next(150 * uiScale);
    ImGui::SetNextItemWidth(150 * uiScale);
    ImGui::Combo("##isldens", &islandDensityMode,
                 Z_("Density: automatic\0Density: layout\0Density: fit texture\0Density: 1 texel/px\0Density: custom\0"));
    next(iconButtonWidth(T_("Texel density"), fh));
    if (iconButton("island.density", "more", T_("Texel density"), IconState::Normal, fh, T_("Texel density targets"))) showDensity = true;
    next(iconButtonWidth(T_("Pin"), fh));
    if (iconButton("island.pin", "pin", T_("Pin"), islandPinNew ? IconState::On : IconState::Normal, fh,
                   T_("Pin the islands this makes, so later UV edits (moving, packing, unwrapping, fixing\n"
                      "overlaps) leave them where they are. P / Alt+P in the UV editor pin and unpin.")))
      islandPinNew = !islandPinNew;
    if (engine.features.on("viewport_pins")) unpinButton("island.unpin");
    facesText(nsel);
  }
  retopoContextBar();
  if (tool == ViewportTool::Object) {  // (1.2)
    gap();
    static const char* kIds[] = {"object.move", "object.turn", "object.scale"};
    static const char* kIcon[] = {"move", "radial", "size"};
    static const char* kName[] = {N_("Move"), N_("Turn"), N_("Scale")};
    static const char* kTip[] = {N_("Arrows: along an axis. The middle square: in the view's plane."),
                                 N_("Rings: about an axis. The outer ring: about the line of sight."),
                                 N_("Handles: along the object's own axes. The middle square: evenly.")};
    int c = segmented(3, kIds, kIcon, kName, kTip, gizmoMode);
    if (c >= 0) gizmoMode = c;
    gap();
    next(iconButtonWidth(T_("On the ground"), fh, true));
    if (iconButton("object.ground", "frame", T_("On the ground"), IconState::Normal, fh,
                   T_("Moved up or down so its lowest point is at height 0"), nullptr, true))
      runCommand("set_object", {{"on_ground", true}});
    ImGui::BeginDisabled(!engine.doc.placement.moved());
    next(iconButtonWidth(T_("Where it came in"), fh, true));
    if (iconButton("object.reset", "view_back", T_("Where it came in"), IconState::Normal, fh,
                   T_("Back to where its file or Blender put it"), nullptr, true))
      runCommand("set_object", {{"reset", true}});
    ImGui::EndDisabled();
    std::string name = engine.doc.displayName();
    next(ImGui::CalcTextSize(name.c_str()).x);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", name.c_str());
  }
  if (tool == ViewportTool::SelectFaces) {
    gap();
    static const char* kIcon[] = {"sel_face", "sel_island"};
    static const char* kName[] = {N_("Faces"), N_("Islands")};
    static const char* kTip[] = {N_("A click or drag selects faces"), N_("A click or drag selects whole UV islands")};
    static const char* kTipLoops[] = {N_("A click or drag selects faces.\nAlt+click by an edge: the loop of quads across it; Ctrl+Alt+click: the ring along it; Shift adds."),
                                      N_("A click or drag selects whole UV islands")};
    int c = segmented(2, kIcon, kIcon, kName, kTipLoops, std::min((int)uvSelectMode, 1));
    (void)kTip;
    if (c >= 0) uvSelectMode = (UVSelectMode)c;
    // (1.2) the selection grown to what links its faces
    {
      const bool any = !engine.doc.selectedFaces().empty();
      ImGui::BeginDisabled(!any);
      next(iconButtonWidth(T_("Connected"), fh, true));
      if (iconButton("select.linked", "sel_island", T_("Connected"), IconState::Normal, fh,
                     T_("The selection grown to the whole connected parts its faces are in"), nullptr, true))
        runCommand("select_linked", {{"by", "connected"}, {"mode", "add"}});
      next(iconButtonWidth(T_("Flat region"), fh, true) + 60 * uiScale);
      if (iconButton("select.flat", "sel_face", T_("Flat region"), IconState::Normal, fh,
                     T_("The selection grown over the surface where it bends less than the angle beside it\n"
                        "from face to face (a box's side, a panel, a flat part of armor)"), nullptr, true))
        runCommand("select_linked", {{"by", "flat"}, {"angle", selectFlatAngle}, {"mode", "add"}});
      ImGui::EndDisabled();
      ImGui::SameLine(0, 2);
      ImGui::SetNextItemWidth(56 * uiScale);
      ImGui::DragFloat("##flatangle", &selectFlatAngle, 0.25f, 0.5f, 90.f, "%.0f°");
      ImGui::SetItemTooltip("%s", T_("Flat region: the most the surface may bend from one face to the next"));
    }
    if (engine.features.on("select_visible")) {  // (0.45)
      next(iconButtonWidth(T_("Visible only"), fh));
      if (iconButton("select.visible", "view", T_("Visible only"), selectVisible ? IconState::On : IconState::Normal, fh,
                     T_("Islands: take only the faces the camera sees (turned toward you, on screen, not hidden\n"
                        "behind other faces). An island that wraps inside the model, or a skin under clothes,\n"
                        "gives just what you see. Turn the view to reach the rest.")))
        selectVisible = !selectVisible;
    }
    if (engine.features.on("viewport_pins")) {
      gap();
      size_t nsel = engine.doc.selectedFaces().size();
      ImGui::BeginDisabled(nsel == 0);
      next(iconButtonWidth(T_("Pin"), fh));
      if (iconButton("select.pin", "pin", T_("Pin"), IconState::Normal, fh,
                     T_("Pin the selected islands, so later UV edits (moving, packing, unwrapping, fixing\n"
                        "overlaps) leave them where they are. Pinned islands have blue stripes on the model.")))
        uvPinSelection(true, false);
      ImGui::EndDisabled();
      unpinButton("select.unpin");
      facesText(nsel);
    }
  }
}

}  // namespace patina
