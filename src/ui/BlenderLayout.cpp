// (1.2.7) The Blender layout: Patina arranged as Blender arranges its editors, over the same panels as the Full and
// Guided layouts (each drawn as a region of this screen, see beginPanel), so every button stays.
// - The menu bar has the five steps as workspace tabs (Model, UVs, Bake, Paint, Export), each with its mark (done,
//   needs a look, to do). A step brings up its tools and its tab of the Properties editor; UVs shows the UV editor
//   beside the 3D view (Blender's UV Editing), Paint the asset shelf under it (Texture Paint).
// - The tools stand at the 3D view's left; its sidebar is at its right with vertical tabs: Brush, Colors, View, Blender.
// - At the right: the Outliner (the objects, the active one's texture sets and layers) and under it the Properties
//   editor, with a column of icon tabs (the step's own panel, Object, Layer, Texture Set, Face Sets, Retopology,
//   History, Notes, Console).
// - The asset shelf lists its catalogs at the left (the material categories, smart materials, smart masks, the brush
//   categories) and their tiles beside them.
// The borders between the areas are dragged to resize them; the sizes and tabs are kept in session.json.
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/Log.h"
#include "ui/App.h"
#include "ui/Theme.h"

namespace patina {

namespace {

using GuidedStep = App::GuidedStep;
using PropsTab = App::PropsTab;
using SideTab = App::SideTab;
using ShelfKind = App::ShelfKind;
using ui::IconState;

struct StepTab {
  GuidedStep step;
  const char* id;
  const char* name;
  const char* icon;
};
const StepTab kStepTabs[] = {{GuidedStep::Model, "model", N_("Model"), "object"},
                             {GuidedStep::UVs, "uvs", N_("UVs"), "uv_edit"},
                             {GuidedStep::Bake, "bake", N_("Bake"), "channel_ao"},
                             {GuidedStep::Paint, "paint", N_("Paint"), "paint"},
                             {GuidedStep::Export, "export", N_("Export"), "render"}};

const char* stepIcon(GuidedStep s) {
  for (auto& t : kStepTabs)
    if (t.step == s) return t.icon;
  return "star";
}

ImU32 stateColor(int state) {
  return state == 1 ? IM_COL32(110, 196, 120, 255) : state == 2 ? IM_COL32(240, 172, 60, 255) : IM_COL32(128, 130, 136, 255);
}

// a border between two areas, dragged to resize: `value` (pixels at UI scale 1) follows the drag times `sign`, within
// lo..hi; true when the drag ends (to keep the sizes)
bool splitter(const char* id, ImVec2 pos, ImVec2 size, bool across, float& value, float sign, float lo, float hi, float scale) {
  ImGui::SetCursorScreenPos(pos);
  ImGui::InvisibleButton(id, ImVec2(std::max(size.x, 1.f), std::max(size.y, 1.f)));
  const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
  if (hot) ImGui::SetMouseCursor(across ? ImGuiMouseCursor_ResizeEW : ImGuiMouseCursor_ResizeNS);
  if (ImGui::IsItemActive()) {
    float d = across ? ImGui::GetIO().MouseDelta.x : ImGui::GetIO().MouseDelta.y;
    value = std::clamp(value + sign * d / scale, lo, hi);
  }
  if (hot) {
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImGui::GetWindowDrawList()->AddRectFilled(a, b, ui::themeColor(ui::theme().accent, ImGui::IsItemActive() ? 0.8f : 0.45f));
  }
  return ImGui::IsItemDeactivated();
}

// a tab of the 3D view's sidebar: its name written upward, as Blender writes them
bool sideTab(const char* id, const char* label, bool on, float w, float scale) {
  const ImVec2 ts = ImGui::CalcTextSize(label, nullptr, true);
  const bool clicked = ImGui::InvisibleButton(id, ImVec2(w, std::round(ts.x + 18.f * scale)));
  const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  const ui::Theme& th = ui::theme();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(a, b,
                    on ? ui::themeColor(th.accentDeep)
                       : ImGui::GetColorU32(ImGui::IsItemHovered() ? ImGuiCol_ButtonHovered : ImGuiCol_Button),
                    ImGui::GetStyle().FrameRounding);
  const ImVec2 c(std::round((a.x + b.x) * 0.5f), std::round((a.y + b.y) * 0.5f));
  const int v0 = dl->VtxBuffer.Size;
  dl->PushClipRectFullScreen();  // (laid out across first: the narrow tab would clip it)
  dl->AddText(ImVec2(std::round(c.x - ts.x * 0.5f), std::round(c.y - ts.y * 0.5f)),
              on ? ui::themeColor(th.onAccent) : ImGui::GetColorU32(ImGuiCol_Text), label, ImGui::FindRenderedTextEnd(label));
  dl->PopClipRect();
  for (int i = v0; i < dl->VtxBuffer.Size; ++i) {  // a quarter turn round the middle: it reads upward
    ImVec2& p = dl->VtxBuffer[i].pos;
    const ImVec2 d(p.x - c.x, p.y - c.y);
    p = ImVec2(c.x + d.y, c.y - d.x);
  }
  return clicked;
}

// a row of the Outliner: an arrow when it opens (open -1: none), an icon, the name, a note at the right (room left
// for `rightW`); 1 clicked, 2 double-clicked
int treeRow(const char* id, int depth, int open, const char* icon, const std::string& name, const std::string& note,
            bool selected, bool dim, float rightW) {
  const float fh = ImGui::GetFrameHeight(), ind = std::round(fh * 0.8f) * depth;
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const float w = ImGui::GetContentRegionAvail().x;
  int r = 0;
  if (ImGui::Selectable(id, selected, ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_AllowOverlap, ImVec2(w, fh)))
    r = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) ? 2 : 1;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImU32 text = ImGui::GetColorU32(dim ? ImGuiCol_TextDisabled : ImGuiCol_Text);
  const float th = ImGui::GetTextLineHeight();
  float x = p.x + ind + 2.f;
  if (open >= 0)
    ImGui::RenderArrow(dl, ImVec2(x, p.y + (fh - th) * 0.5f + th * 0.15f), ImGui::GetColorU32(ImGuiCol_TextDisabled),
                       open ? ImGuiDir_Down : ImGuiDir_Right, 0.7f);
  x += std::round(fh * 0.7f);
  if (icon) {
    const float isz = std::round(fh * 0.78f);
    ui::IconColors c = ui::iconColors(IconState::Normal);
    if (dim) c.alpha = 0.5f;
    ui::drawIcon(dl, icon, ImVec2(x, std::round(p.y + (fh - isz) * 0.5f)), isz, c);
    x += isz + std::round(fh * 0.25f);
  }
  const float right = p.x + w - rightW;
  float noteW = note.empty() ? 0.f : ImGui::CalcTextSize(note.c_str()).x;
  dl->PushClipRect(ImVec2(x, p.y), ImVec2(std::max(x, right - noteW - 6.f), p.y + fh), true);
  dl->AddText(ImVec2(x, std::round(p.y + (fh - th) * 0.5f)), text, name.c_str());
  dl->PopClipRect();
  if (!note.empty())
    dl->AddText(ImVec2(right - noteW - 4.f, std::round(p.y + (fh - th) * 0.5f)), ImGui::GetColorU32(ImGuiCol_TextDisabled), note.c_str());
  return r;
}

}  // namespace

// ------------------------------------------------------------------ the screen

void App::drawBlenderLayout() {
  blenderFollowTool();
  ImGuiViewport* vp = ImGui::GetMainViewport();
  const float statusH = ImGui::GetFrameHeight();
  const ImVec2 pos = vp->WorkPos, size(vp->WorkSize.x, vp->WorkSize.y - statusH);
  const float s = uiScale, gap = std::round(4.f * s), fh = ImGui::GetFrameHeight();
  ImGui::SetNextWindowPos(pos);
  ImGui::SetNextWindowSize(size);
  ImGui::SetNextWindowViewport(vp->ID);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::GetStyleColorVec4(ImGuiCol_MenuBarBg));  // (the borders between areas)
  ImGui::Begin("##Blender", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNavFocus);
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(2);
  bool keep = false;  // a border was let go: the sizes go to session.json

  // ---- the right column: the Outliner over the Properties editor
  const float rightW = std::round(std::clamp(blui.rightW * s, 240.f * s, size.x * 0.5f));
  const float rx = pos.x + size.x - rightW;
  const float outH = std::round(std::clamp(blui.outlinerH * s, 120.f * s, size.y - 200.f * s));
  keep |= splitter("##split_right", ImVec2(rx - gap, pos.y), ImVec2(gap, size.y), true, blui.rightW, -1.f, 240.f, 2000.f, s);
  keep |= splitter("##split_outliner", ImVec2(rx, pos.y + outH), ImVec2(rightW, gap), false, blui.outlinerH, 1.f, 120.f, 4000.f, s);
  ImGui::SetCursorScreenPos(pos + ImVec2(size.x - rightW, 0.f));
  ImGui::BeginChild("##outliner", ImVec2(rightW, outH), ImGuiChildFlags_AlwaysUseWindowPadding);
  {
    ImVec2 a = ImGui::GetWindowPos(), b = ImGui::GetWindowSize();
    embeddedRects.push_back({"window.Outliner", ImVec4(a.x, a.y, b.x, b.y)});
  }
  drawOutliner();
  ImGui::EndChild();
  ImGui::SetCursorScreenPos(ImVec2(rx, pos.y + outH + gap));
  ImGui::BeginChild("##properties_editor", ImVec2(rightW, size.y - outH - gap), ImGuiChildFlags_None,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  {
    ImVec2 a = ImGui::GetWindowPos(), b = ImGui::GetWindowSize();
    embeddedRects.push_back({"window.Properties Editor", ImVec4(a.x, a.y, b.x, b.y)});
  }
  drawPropertiesEditor();
  ImGui::EndChild();

  // ---- the left part: (the UV editor,) the tools, the 3D view and its sidebar, (the asset shelf)
  const float cx0 = pos.x, cw = size.x - rightW - gap;
  const bool shelf = guidedStep == GuidedStep::Paint;
  const bool uv = guidedStep == GuidedStep::UVs;
  const float shelfH = shelf ? std::round(std::clamp(blui.shelfH * s, 110.f * s, size.y * 0.65f)) : 0.f;
  const float viewH = size.y - (shelf ? shelfH + gap : 0.f);
  if (shelf) {
    keep |= splitter("##split_shelf", ImVec2(cx0, pos.y + viewH), ImVec2(cw, gap), false, blui.shelfH, -1.f, 110.f, 4000.f, s);
    ImGui::SetCursorScreenPos(ImVec2(cx0, pos.y + viewH + gap));
    ImGui::BeginChild("##asset_shelf", ImVec2(cw, shelfH), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    {
      ImVec2 a = ImGui::GetWindowPos(), b = ImGui::GetWindowSize();
      embeddedRects.push_back({"window.Asset Shelf", ImVec4(a.x, a.y, b.x, b.y)});
    }
    drawAssetShelf();
    ImGui::EndChild();
  }
  float x = cx0;
  if (uv) {
    const float uvW = std::round(std::clamp(blui.uvShare, 0.15f, 0.8f) * cw);
    embedPanel(&App::drawUVEditor, ImVec2(x, pos.y), ImVec2(uvW, viewH));
    float share = blui.uvShare * cw;  // (dragged in pixels, kept as a share of the width)
    if (splitter("##split_uv", ImVec2(x + uvW, pos.y), ImVec2(gap, viewH), true, share, 1.f, 0.15f * cw, 0.8f * cw, 1.f)) keep = true;
    blui.uvShare = std::clamp(share / std::max(cw, 1.f), 0.15f, 0.8f);
    x += uvW + gap;
  }
  const float toolsW = std::round(84.f * s);
  embedPanel(&App::drawToolbar, ImVec2(x, pos.y), ImVec2(toolsW, viewH));
  x += toolsW + gap;
  // the sidebar: its tabs at the right edge, its panel beside them while open
  const float tabW = std::round(fh * 1.05f);
  const float sideW = blui.sideOpen ? std::round(std::clamp(blui.sidebarW * s, 200.f * s, (cx0 + cw - x) * 0.6f)) : 0.f;
  const float tabX = cx0 + cw - tabW;
  ImGui::SetCursorScreenPos(ImVec2(tabX, pos.y));
  ImGui::BeginChild("##sidebar_tabs", ImVec2(tabW, viewH), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground);
  {
    static const char* kSideNames[] = {N_("Brush"), N_("Colors"), N_("View"), N_("Blender")};
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, std::round(2.f * s)));
    for (int t = 0; t < (int)SideTab::Count; ++t) {
      const bool on = blui.sideOpen && (int)blui.side == t;
      if (sideTab((std::string("##side") + kSideNames[t]).c_str(), T_(kSideNames[t]), on, tabW, s)) {
        if (on) blui.sideOpen = false;  // (the chosen tab again: the sidebar closes, as N does in Blender)
        else blui.side = (SideTab)t, blui.sideOpen = true;
        keep = true;
      }
      ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
      engine.uiRects[std::string("blender.side.") + kSideNames[t]] = {a.x, a.y, b.x - a.x, b.y - a.y};
    }
    ImGui::PopStyleVar();
  }
  ImGui::EndChild();
  const float viewW = tabX - x - (blui.sideOpen ? sideW + gap : 0.f);
  ImGui::SetCursorScreenPos(ImVec2(x, pos.y));
  panelEmbedded = true;
  panelEmbedSize = ImVec2(std::max(viewW, 16.f), viewH);
  drawViewport(false);
  panelEmbedded = false;
  if (blui.sideOpen) {
    const float sx = x + viewW + gap;
    keep |= splitter("##split_side", ImVec2(sx - gap, pos.y), ImVec2(gap, viewH), true, blui.sidebarW, -1.f, 200.f, 2000.f, s);
    ImGui::SetCursorScreenPos(ImVec2(sx, pos.y));
    ImGui::BeginChild("##view_sidebar", ImVec2(sideW, viewH), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
    {
      ImVec2 a = ImGui::GetWindowPos(), b = ImGui::GetWindowSize();
      embeddedRects.push_back({"window.Sidebar", ImVec4(a.x, a.y, b.x, b.y)});
    }
    drawViewSidebar();
    ImGui::EndChild();
  }
  ImGui::End();
  if (keep) saveSession(true);
}

// ------------------------------------------------------------------ the steps, as workspace tabs

void App::blenderStepTabs() {
  const float s = uiScale, th = ImGui::GetTextLineHeight();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ui::Theme& t = ui::theme();
  ImGui::SameLine(0, 10.f * s);
  {
    ImVec2 p = ImGui::GetCursorScreenPos();
    dl->AddLine(ImVec2(p.x - 5.f * s, p.y + 3.f * s), ImVec2(p.x - 5.f * s, p.y + ImGui::GetFrameHeight() - 3.f * s),
                ImGui::GetColorU32(ImGuiCol_Border));
  }
  for (auto& st : kStepTabs) {
    const StepState state = guidedStatus(st.step);
    const char* name = T_(st.name);
    const float dot = std::round(th * 0.36f), padX = std::round(9.f * s);
    const ImVec2 ts = ImGui::CalcTextSize(name);
    const ImVec2 sz(padX * 2.f + dot * 2.f + std::round(6.f * s) + ts.x, ImGui::GetFrameHeight());
    ImGui::PushID(st.id);
    const bool clicked = ImGui::InvisibleButton("##step", sz);
    const bool on = guidedStep == st.step, hov = ImGui::IsItemHovered();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    engine.uiRects[std::string("blender.step.") + st.id] = {a.x, a.y, b.x - a.x, b.y - a.y};
    if (on || hov)
      dl->AddRectFilled(a + ImVec2(0, 2.f * s), b - ImVec2(0, 1.f * s), ImGui::GetColorU32(on ? ImGuiCol_Button : ImGuiCol_ButtonHovered),
                        ImGui::GetStyle().FrameRounding);
    if (on) dl->AddRectFilled(ImVec2(a.x + 3.f * s, b.y - 3.f * s), ImVec2(b.x - 3.f * s, b.y - 1.f * s), ui::themeColor(t.accent), 1.f);
    const ImVec2 c(a.x + padX + dot, std::round((a.y + b.y) * 0.5f));
    if (state.state == 0) dl->AddCircle(c, dot * 0.8f, stateColor(0), 16, std::max(1.f, 1.4f * s));
    else dl->AddCircleFilled(c, dot, stateColor(state.state), 16);
    dl->AddText(ImVec2(c.x + dot + std::round(6.f * s), std::round(c.y - ts.y * 0.5f)),
                ImGui::GetColorU32(on ? ImGuiCol_Text : ImGuiCol_TextDisabled), name);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s\n%s", name, state.text.c_str());
    if (clicked) setBlenderStep(st.step);
    ImGui::PopID();
    ImGui::SameLine(0, std::round(2.f * s));
  }
}

// ------------------------------------------------------------------ the Outliner

void App::drawOutliner() {
  Document& d = engine.doc;  // (the active object)
  const float fh = ImGui::GetFrameHeight(), s = uiScale;
  const bool objects = engine.features.on("scene_objects");
  // ---- the header: the search, and a model to add
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - (objects ? iconButtonWidth(T_("Add a model"), fh) + 6.f * s : 0.f));
  ImGui::InputTextWithHint("##outliner_search", T_("Search layers"), &blui.outlinerFilter);
  if (objects) {
    ImGui::SameLine(0, 6.f * s);
    if (iconButton("objects.add", "add_object", T_("Add a model"), IconState::Normal, fh,
                   T_("Add a model file to the scene as an object of its own (a character's clothes, eyes or hair),\n"
                      "where the file puts it, with its own layers"),
                   "file.add_model"))
      openFileDialog(DialogPurpose::AddObject);
  }
  ImGui::Spacing();
  const int n = engine.objectCount(), act = engine.activeObject();
  const float rowStep = fh + ImGui::GetStyle().ItemSpacing.y;
  for (int i = 0; i < n; ++i) {
    Document& o = engine.object(i);
    const bool active = i == act;
    ImGui::PushID(i);
    // ---- the object, its eye at the right
    const ImVec2 rowPos = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    int r = treeRow("##object", 0, active ? 1 : 0, "object", o.displayName(), o.bridge.active ? T_("from Blender") : "", active,
                    o.objectHidden, fh);
    {
      ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
      engine.uiRects["objects.row." + o.displayName()] = {a.x, a.y, b.x - a.x, b.y - a.y};
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
      ImGui::SetTooltip("%s", F_("%u faces, %zu layers, %d x %d texture set%s", o.mesh.faceCount(), o.layers.size(), o.tileRes(),
                                 o.tileRes(), o.bridge.active ? ("\n" + F_("Linked to Blender's %s", o.bridge.object.c_str())).c_str() : "")
                                  .c_str());
    if (objects && ImGui::BeginPopupContextItem("##objctx")) objectMenu(i), ImGui::EndPopup();
    if (r == 2 && objects) objectRename = i, objectRenameText = o.displayName(), objectRenameOpen = true;
    else if (r == 1 && !active) selectObjectUI(i);
    if (objects) {
      ImGui::SetCursorScreenPos(ImVec2(rowPos.x + w - fh, rowPos.y));
      if (ImGui::InvisibleButton("##eye", ImVec2(fh, fh))) runCommand("set_object", {{"object", i}, {"visible", (bool)o.objectHidden}});
      ui::IconColors c = ui::iconColors(IconState::Normal);
      if (o.objectHidden) c.alpha = 0.35f;
      const float isz = std::round(fh * 0.78f);
      ui::drawIcon(ImGui::GetWindowDrawList(), "view", ImGui::GetItemRectMin() + ImVec2((fh - isz) * 0.5f, (fh - isz) * 0.5f), isz, c);
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        ImGui::SetTooltip("%s", T_("Shown in the 3D view, renders and exports, or hidden"));
    }
    ImGui::PopID();
    if (!active) continue;
    // ---- its texture sets (one per material slot or UDIM tile: they share the layers)
    const TileLayout& tl = d.mesh.tiles;
    const std::string res = F_("%d px", d.tileRes());
    if (tl.active()) {
      if (treeRow("##sets", 1, blui.setsOpen ? 1 : 0, "uv_checker", T_("Texture sets"), F_("%d, %s", tl.count(), res.c_str()),
                  false, false, 0.f))
        blui.setsOpen = !blui.setsOpen;
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
        ImGui::SetTooltip("%s", T_("One texture per material slot or UDIM tile. The layers below paint all of them;\n"
                                   "click one to see it in the UV editor (the UVs step)."));
      if (blui.setsOpen)
        for (int k = 0; k < tl.count() && k < (int)tl.tiles.size(); ++k) {
          const TileInfo& ti = tl.tiles[k];
          std::string name = !ti.name.empty() ? ti.name : (tl.mode & TileByUdim) ? udimName(ti.udim) : F_("Set %d", k + 1);
          ImGui::PushID(k);
          if (treeRow("##set", 2, -1, nullptr, name, res, engine.uvOptions.tile == k, false, 0.f)) engine.uvOptions.tile = k;
          ImGui::PopID();
        }
    } else if (treeRow("##set", 1, -1, "uv_checker", T_("Texture set"), res, blui.props == PropsTab::TextureSet, false, 0.f)) {
      blui.props = PropsTab::TextureSet;
    }
    // ---- its layers: the Layers panel's buttons and tree, a level in
    size_t count = 0;
    for (auto& l : d.layers) count += !l->isBaked() && !l->isFolder();
    if (treeRow("##layers", 1, blui.layersOpen ? 1 : 0, "add_folder", T_("Layers"), std::to_string(count), false, false, 0.f))
      blui.layersOpen = !blui.layersOpen;
    if (!blui.layersOpen) continue;
    const float ind = std::round(fh * 0.8f) * 2.f;
    ImGui::Indent(ind);
    thumbBudget = 3;  // (layer thumbnails refreshed this frame)
    layersButtons();
    const float rest = ImGui::GetContentRegionAvail().y - (n - i - 1) * rowStep - 2.f * s;
    outlinerDrawing = true;
    layersTree(std::max(rest, 6.f * rowStep));
    outlinerDrawing = false;
    ImGui::Unindent(ind);
  }
  objectPopups();
}

// ------------------------------------------------------------------ the Properties editor

void App::drawPropertiesEditor() {
  struct Tab {
    PropsTab tab;
    const char* icon;
    const char* name;
    bool on;
  };
  const Tab tabs[] = {
      {PropsTab::Step, stepIcon(guidedStep), N_("Step"), true},
      {PropsTab::Object, "object", N_("Object"), true},
      {PropsTab::Layer, "opacity", N_("Layer"), true},
      {PropsTab::TextureSet, "uv_checker", N_("Texture Set"), true},
      {PropsTab::FaceSets, "face_sets", N_("Face Sets"), engine.features.on("face_sets")},
      {PropsTab::Retopology, "quads", N_("Retopology"), engine.features.on("retopology")},
      {PropsTab::History, "view_back", N_("History"), true},
      {PropsTab::Notes, "note", N_("Notes"), true},
      {PropsTab::Console, "more", N_("Console"), true},
  };
  for (auto& t : tabs)
    if (t.tab == blui.props && !t.on) blui.props = PropsTab::Step;
  const float s = uiScale, fh = ImGui::GetFrameHeight(), bw = std::round(fh * 1.3f), pad = std::round(3.f * s);
  const ImVec2 avail = ImGui::GetContentRegionAvail();
  // ---- the icon tabs, in a column at the left
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_MenuBarBg));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad * 2.f));
  ImGui::BeginChild("##props_tabs", ImVec2(bw + 2.f * pad, avail.y), ImGuiChildFlags_AlwaysUseWindowPadding,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleVar();
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, pad));
  for (auto& t : tabs) {
    if (!t.on) continue;
    std::string tip = t.tab == PropsTab::Step ? F_("%s: %s", T_(t.name), T_(kStepTabs[(int)guidedStep].name)) : std::string(T_(t.name));
    if (iconButton((std::string("props.tab.") + t.name).c_str(), t.icon, tip.c_str(), blui.props == t.tab ? IconState::Chosen : IconState::Normal,
                   bw, nullptr, nullptr, false, bw)) {
      blui.props = t.tab;
      saveSession(true);
    }
    if (t.tab == PropsTab::Step || t.tab == PropsTab::Console) ImGui::Dummy(ImVec2(0, pad));  // (groups, as Blender's)
  }
  ImGui::PopStyleVar();
  ImGui::EndChild();
  ImGui::PopStyleColor();
  ImGui::SameLine(0, 0);
  // ---- the chosen tab's panel
  const ImVec2 cs = ImGui::GetContentRegionAvail();
  auto region = [&](const char* id, auto&& body) {
    ImGui::BeginChild(id, cs, ImGuiChildFlags_AlwaysUseWindowPadding);
    body();
    ImGui::EndChild();
  };
  switch (blui.props) {
    case PropsTab::Step: region("##props_step", [&] { stepGuidePanel(); }); break;
    case PropsTab::Object:
      region("##props_object", [&] {
        Document& d = engine.doc;
        ImGui::SeparatorText(d.displayName().c_str());
        if (d.hasMesh()) {
          ImGui::Text(T_("%zu verts  %u faces  %zu tris  %u UV islands"), d.mesh.positions.size(), d.mesh.faceCount(), d.mesh.tris.size(),
                      d.mesh.islandCount);
          ImGui::Text(T_("%d x %d texture set, %d of them"), d.tileRes(), d.tileRes(), d.mesh.tiles.count());
        }
        if (d.bridge.active) ImGui::TextDisabled(T_("Linked to Blender's %s"), d.bridge.object.c_str());
        if (engine.features.on("scene_objects")) objectPlacementUI();
      });
      break;
    case PropsTab::Layer: region("##props_layer", [&] { layerProperties(); }); break;
    case PropsTab::TextureSet: embedPanel(&App::drawTextureSet, ImVec2(-1.f, 0.f), cs); break;
    case PropsTab::FaceSets: region("##props_facesets", [&] { faceSetsPanel(); }); break;
    case PropsTab::Retopology:
      region("##props_retopo", [&] {
        retopoShown = true;  // (its numbers follow, and the live preview runs, while it is in front)
        engine.retopo().wantPreviewAt = nowSeconds();
        retopoPanel();
      });
      break;
    case PropsTab::History: embedPanel(&App::drawHistory, ImVec2(-1.f, 0.f), cs); break;
    case PropsTab::Notes: embedPanel(&App::drawNotes, ImVec2(-1.f, 0.f), cs); break;
    default: embedPanel(&App::drawConsole, ImVec2(-1.f, 0.f), cs); break;
  }
}

// ------------------------------------------------------------------ the 3D view's sidebar

void App::drawViewSidebar() {
  const ImVec2 cs = ImGui::GetContentRegionAvail();
  switch (blui.side) {
    case SideTab::Brush: embedPanel(&App::drawProperties, ImVec2(-1.f, 0.f), cs); break;
    case SideTab::Colors: embedPanel(&App::drawColors, ImVec2(-1.f, 0.f), cs); break;
    case SideTab::View:
      ImGui::BeginChild("##side_view", cs, ImGuiChildFlags_AlwaysUseWindowPadding);
      sceneUI();  // (the material under the layers, and the lighting)
      ImGui::EndChild();
      break;
    default: embedPanel(&App::drawBlender, ImVec2(-1.f, 0.f), cs); break;
  }
}

// ------------------------------------------------------------------ the asset shelf

void App::drawAssetShelf() {
  const float s = uiScale;
  const ImVec2 avail = ImGui::GetContentRegionAvail();
  // ---- the catalogs, as Blender's asset shelf lists them at its left
  const float catW = std::round(std::min(175.f * s, avail.x * 0.3f));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_MenuBarBg));
  ImGui::BeginChild("##shelf_catalogs", ImVec2(catW, avail.y), ImGuiChildFlags_AlwaysUseWindowPadding);
  ImGui::PopStyleColor();
  auto item = [&](const std::string& label, bool sel, const char* key) {
    bool r = ImGui::Selectable(label.c_str(), sel);
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    engine.uiRects[std::string("shelf.") + key] = {a.x, a.y, b.x - a.x, b.y - a.y};
    if (r) saveSession(true);
    return r;
  };
  // the three kinds of asset, a switch over their catalogs (so the brushes are never far down the list)
  const int kind = blui.shelf == ShelfKind::Materials ? 0 : blui.shelf == ShelfKind::Brushes ? 2 : 1;
  {
    static const char* kKinds[] = {N_("Materials"), N_("Smart"), N_("Brushes")};
    static const ShelfKind kFirst[] = {ShelfKind::Materials, ShelfKind::SmartMaterials, ShelfKind::Brushes};
    const float bw = std::floor((ImGui::GetContentRegionAvail().x - 2.f * s) / 3.f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(s, s));
    for (int k = 0; k < 3; ++k) {
      if (k) ImGui::SameLine();
      const bool on = kind == k;
      if (on) ImGui::PushStyleColor(ImGuiCol_Button, ui::theme().accentDeep);
      if (ImGui::Button((std::string(T_(kKinds[k])) + "##shelfkind").c_str(), ImVec2(bw, 0)) && !on) blui.shelf = kFirst[k], saveSession(true);
      if (on) ImGui::PopStyleColor();
      ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
      engine.uiRects[std::string("shelf.kind.") + kKinds[k]] = {a.x, a.y, b.x - a.x, b.y - a.y};
    }
    ImGui::PopStyleVar();
  }
  ImGui::Spacing();
  ImGui::BeginChild("##shelf_catalog_list", ImVec2(0, 0));
  if (kind == 0) {
    if (item(std::string(T_("All")) + "##mat_all", materialCategory.empty(), "materials")) materialCategory.clear();
    for (auto& c : engine.materials.categories())
      if (item(c + "##mat_" + c, materialCategory == c, ("materials:" + c).c_str())) materialCategory = c;
  } else if (kind == 1) {
    if (item(std::string(T_("Smart materials")) + "##smart", blui.shelf == ShelfKind::SmartMaterials, "smart_materials"))
      blui.shelf = ShelfKind::SmartMaterials;
    if (item(std::string(T_("Smart masks")) + "##masks", blui.shelf == ShelfKind::SmartMasks, "smart_masks"))
      blui.shelf = ShelfKind::SmartMasks;
  } else {
    if (item(std::string(T_("All")) + "##brush_all", blui.brushCategory.empty(), "brushes")) blui.brushCategory.clear();
    std::string last;
    for (auto& p : engine.brushes.presets()) {
      if (p.category == last) continue;
      last = p.category;
      if (item(p.category + "##brush_" + p.category, blui.brushCategory == p.category, ("brushes:" + p.category).c_str()))
        blui.brushCategory = p.category;
    }
  }
  ImGui::EndChild();
  ImGui::EndChild();
  ImGui::SameLine(0, std::round(4.f * s));
  // ---- the chosen catalog's tiles: the Materials or Brushes panel, the shelf choosing its tab and category
  const ImVec2 cs = ImGui::GetContentRegionAvail();
  if (blui.shelf == ShelfKind::Brushes) {
    brushCategoryOnly = true;
    brushCategoryShown = blui.brushCategory;
    embedPanel(&App::drawBrushes, ImVec2(-1.f, 0.f), cs);
    brushCategoryOnly = false;
  } else {
    materialsTabShown = (int)blui.shelf;
    embedPanel(&App::drawMaterials, ImVec2(-1.f, 0.f), cs);
    materialsTabShown = -1;
  }
}

// ------------------------------------------------------------------ open_window, and the session

bool App::blenderOpenWindow(const std::string& w) {
  if (w == "properties") blui.side = SideTab::Brush, blui.sideOpen = true;
  else if (w == "colors") blui.side = SideTab::Colors, blui.sideOpen = true;
  else if (w == "blender") blui.side = SideTab::Blender, blui.sideOpen = true;
  else if (w == "layers") blui.props = PropsTab::Layer;
  else if (w == "materials") setBlenderStep(GuidedStep::Paint), blui.shelf = ShelfKind::Materials;
  else if (w == "brushes") setBlenderStep(GuidedStep::Paint), blui.shelf = ShelfKind::Brushes;
  else if (w == "texture_set") blui.props = PropsTab::TextureSet;
  else if (w == "face_sets") blui.props = PropsTab::FaceSets;
  else if (w == "retopology") blui.props = PropsTab::Retopology;
  else if (w == "history") blui.props = PropsTab::History;
  else if (w == "notes") blui.props = PropsTab::Notes;
  else if (w == "console") blui.props = PropsTab::Console;
  else return false;
  return true;
}

void App::blenderSessionLoad(const json& j) {
  if (!j.is_object()) return;
  blui.rightW = j.value("right", blui.rightW);
  blui.outlinerH = j.value("outliner", blui.outlinerH);
  blui.shelfH = j.value("shelf", blui.shelfH);
  blui.sidebarW = j.value("sidebar", blui.sidebarW);
  blui.uvShare = std::clamp(j.value("uv_share", blui.uvShare), 0.15f, 0.8f);
  blui.props = (PropsTab)std::clamp(j.value("properties_tab", (int)blui.props), 0, (int)PropsTab::Count - 1);
  blui.side = (SideTab)std::clamp(j.value("sidebar_tab", (int)blui.side), 0, (int)SideTab::Count - 1);
  blui.sideOpen = j.value("sidebar_open", blui.sideOpen);
  blui.shelf = (ShelfKind)std::clamp(j.value("shelf_catalog", (int)blui.shelf), 0, (int)ShelfKind::Brushes);
  blui.brushCategory = j.value("brush_category", blui.brushCategory);
  blui.setsOpen = j.value("sets_open", blui.setsOpen);
  blui.layersOpen = j.value("layers_open", blui.layersOpen);
}

json App::blenderSessionSave() const {
  return json{{"right", blui.rightW},          {"outliner", blui.outlinerH},   {"shelf", blui.shelfH},
              {"sidebar", blui.sidebarW},      {"uv_share", blui.uvShare},     {"properties_tab", (int)blui.props},
              {"sidebar_tab", (int)blui.side}, {"sidebar_open", blui.sideOpen}, {"shelf_catalog", (int)blui.shelf},
              {"brush_category", blui.brushCategory}, {"sets_open", blui.setsOpen}, {"layers_open", blui.layersOpen}};
}

}  // namespace patina
