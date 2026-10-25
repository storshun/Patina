// (0.46) The Guided layout: the work in five steps along the top (Model, UVs, Bake, Paint, Export), each a screen of
// its own with what that step needs in front, over the same document, panels and commands as the Full layout (the
// docked layout of before). Every step can be chosen at any time; each shows whether it is done, needs a look or is
// still to do, and names the next one. A tool chosen from another step (its key, a menu, the palette) goes to its
// step, so no tool is ever chosen but out of sight; retopology lives in the Full layout.
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <stdexcept>

#include "core/MeshCheck.h"
#include "core/Paths.h"
#include "core/UVIsland.h"
#include "ui/App.h"

namespace fs = std::filesystem;

namespace patina {

namespace {

const ImU32 kCopper = IM_COL32(232, 133, 44, 255);
const ImU32 kDone = IM_COL32(110, 196, 120, 255);
const ImU32 kLook = IM_COL32(240, 172, 60, 255);
const ImU32 kTodo = IM_COL32(128, 130, 136, 255);
const ImVec4 kDoneText(0.55f, 0.85f, 0.58f, 1.f);
const ImVec4 kLookText(1.f, 0.75f, 0.35f, 1.f);

struct StepInfo {
  App::GuidedStep step;
  const char* id;    // guided.step.<id> in uiRects, the `guided` command's step names
  const char* name;  // on the bar
  const char* next;  // the Next button that leads here
  const char* tip;
};
const StepInfo kSteps[] = {
    {App::GuidedStep::Model, "model", N_("Model"), N_("Next: the model"),
     N_("Bring in the model: open a file or the sample, or send it from Blender. Then choose how many textures it gets "
        "and how big.")},
    {App::GuidedStep::UVs, "uvs", N_("UVs"), N_("Next: check the UVs"),
     N_("Check the UVs and fix what is wrong: overlaps, squashed faces. Unwrap the model if it has none.")},
    {App::GuidedStep::Bake, "bake", N_("Bake"), N_("Next: bake the mesh maps"),
     N_("Bake the mesh maps once: the creases, edges and thin parts that smart materials and masks follow.")},
    {App::GuidedStep::Paint, "paint", N_("Paint"), N_("Next: paint"),
     N_("Layers, materials, smart materials and brushes: texture the model.")},
    {App::GuidedStep::Export, "export", N_("Export"), N_("Next: export"),
     N_("Send the textures back to Blender, or write them for Unreal, Unity or glTF. Save the project too.")},
};

const StepInfo& info(App::GuidedStep s) { return kSteps[std::clamp((int)s, 0, (int)App::GuidedStep::Count - 1)]; }

bool paintTool(ViewportTool t) {
  return t == ViewportTool::Paint || t == ViewportTool::Erase || t == ViewportTool::Blur || t == ViewportTool::Smudge ||
         t == ViewportTool::Clone || t == ViewportTool::Fill || t == ViewportTool::Picker || t == ViewportTool::Note;
}
bool uvTool(ViewportTool t) { return t == ViewportTool::SelectFaces || t == ViewportTool::Seams || t == ViewportTool::Island; }
bool retopologyTool(ViewportTool t) {
  return t == ViewportTool::ZoneLine || t == ViewportTool::Patch || t == ViewportTool::Flow || t == ViewportTool::Density;
}

// a step's title (in its color) and what it is for
void header(const char* title, const char* what, ImVec4 color) {
  ImGui::SetWindowFontScale(1.3f);
  ImGui::TextColored(ImVec4(0.55f + color.x * 0.45f, 0.55f + color.y * 0.45f, 0.55f + color.z * 0.45f, 1.f), "%s", title);
  ImGui::SetWindowFontScale(1.f);
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::TextWrapped("%s", what);
  ImGui::PopStyleColor();
  ImGui::Spacing();
}

// a line of the step's checks: a colored mark, then the text
void checkLine(int state, const std::string& text) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  float h = ImGui::GetTextLineHeight();
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImVec2 c(p.x + h * 0.5f, p.y + h * 0.5f);
  float r = h * 0.32f;
  if (state == 1) {
    dl->AddCircleFilled(c, r, kDone, 16);
  } else if (state == 2) {
    dl->AddCircleFilled(c, r, kLook, 16);
  } else {
    dl->AddCircle(c, r, kTodo, 16, 1.5f);
  }
  ImGui::Dummy(ImVec2(h, h));
  ImGui::SameLine();
  ImGui::PushTextWrapPos(0.f);
  if (state == 2) ImGui::TextColored(kLookText, "%s", text.c_str());
  else ImGui::TextUnformatted(text.c_str());
  ImGui::PopTextWrapPos();
}

ImVec4 g_primary(0.91f, 0.52f, 0.17f, 1.f);  // the primary buttons' color: the step's (set by drawGuided)

bool wideButton(const char* label, bool primary = false) {
  if (primary) {
    const ImVec4 dark(0.13f, 0.13f, 0.15f, 1.f);
    ImGui::PushStyleColor(ImGuiCol_Button, App::mixColor(dark, g_primary, 0.62f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, App::mixColor(dark, g_primary, 0.78f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, App::mixColor(dark, g_primary, 0.9f));
  }
  bool r = ImGui::Button(label, ImVec2(-FLT_MIN, ImGui::GetFrameHeight() * 1.35f));
  if (primary) ImGui::PopStyleColor(3);
  return r;
}

void tip(const char* text) {
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s", text);
}

}  // namespace

// ------------------------------------------------------------------ panels as regions of the guided screen

bool App::beginPanel(const char* label, ImGuiWindowFlags flags) {
  if (!panelEmbedded) {
    int pushed = pushPanelColors(label);  // (the tab's colors are taken when the window begins)
    bool open = ImGui::Begin(label, nullptr, flags);
    ImGui::PopStyleColor(pushed);
    return open;
  }
  // (the English name: what the docked window is called in uiRects, and the child's ID in every language)
  const char* id = std::strstr(label, "###");
  std::string name = id ? id + 3 : label;
  flags &= ~(ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse);
  ImGui::BeginChild(name.c_str(), panelEmbedSize, ImGuiChildFlags_AlwaysUseWindowPadding, flags);
  ImVec2 p = ImGui::GetWindowPos(), s = ImGui::GetWindowSize();
  embeddedRects.push_back({"window." + name, ImVec4(p.x, p.y, s.x, s.y)});
  return true;
}

bool App::panelHovered(const char* name, bool exact) const {
  ImGuiWindow* h = ImGui::GetCurrentContext()->HoveredWindow;
  if (!h) return false;
  if (ImGuiWindow* w = ImGui::FindWindowByName(name); w && w->Active && (h == w || (!exact && h->RootWindow == w))) return true;
  // (a region of the Guided layout: a child window, named "<parent>/<name>_<id>")
  std::string tag = std::string("/") + name + "_";
  for (ImGuiWindow* x = h; x; x = exact ? nullptr : x->ParentWindow)
    if (std::strstr(x->Name, tag.c_str())) return true;
  return false;
}

void App::endPanel() {
  if (panelEmbedded) ImGui::EndChild();
  else ImGui::End();
}

void App::embedPanel(void (App::*draw)(), ImVec2 pos, ImVec2 size) {
  if (pos.x >= 0.f) ImGui::SetCursorScreenPos(pos);
  panelEmbedded = true;
  panelEmbedSize = ImVec2(std::max(size.x, 16.f), std::max(size.y, 16.f));
  (this->*draw)();
  panelEmbedded = false;
}

// ------------------------------------------------------------------ the steps' colors

ImVec4 App::stepColor(GuidedStep s) {
  auto rgb = [](int r, int g, int b) { return ImVec4(r / 255.f, g / 255.f, b / 255.f, 1.f); };
  switch (s) {
    case GuidedStep::Model: return rgb(90, 142, 216);    // blue: the model as it comes
    case GuidedStep::UVs: return rgb(44, 178, 166);      // teal: laid flat
    case GuidedStep::Bake: return rgb(154, 114, 218);    // violet: measured
    case GuidedStep::Paint: return rgb(232, 133, 44);    // copper: Patina's own
    case GuidedStep::Export: return rgb(98, 184, 98);    // green: out the door
    default: return rgb(150, 152, 158);
  }
}

App::GuidedStep App::panelStep(const char* name) {
  static const struct {
    const char* name;
    GuidedStep step;
  } kPanels[] = {{"UV Editor", GuidedStep::UVs},      {"Layers", GuidedStep::Paint},       {"Properties", GuidedStep::Paint},
                 {"Materials", GuidedStep::Paint},    {"Brushes", GuidedStep::Paint},      {"Colors", GuidedStep::Paint},
                 {"Texture Set", GuidedStep::Model},  {"Retopology", GuidedStep::Model},   {"Blender Link", GuidedStep::Export}};
  for (auto& p : kPanels)
    if (std::strcmp(p.name, name) == 0) return p.step;
  return GuidedStep::Count;
}

ImU32 App::mixColor(ImVec4 a, ImVec4 b, float t, float alpha) {
  return ImGui::ColorConvertFloat4ToU32(ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, alpha));
}

void App::applyStepTint() {
  // the screen leans to the step's color in the Guided layout, eased so a change of step is seen to happen
  ImVec4 want = guidedOn() ? stepColor(guidedStep) : ImVec4(tintNow.x, tintNow.y, tintNow.z, 0.f);
  want.w = guidedOn() ? 1.f : 0.f;
  float k = 1.f - std::exp(-ImGui::GetIO().DeltaTime * 12.f);
  if (tintNow.w <= 0.001f && want.w > 0.f) tintNow = ImVec4(want.x, want.y, want.z, tintNow.w);  // (from none: its hue at once)
  tintNow = ImVec4(tintNow.x + (want.x - tintNow.x) * k, tintNow.y + (want.y - tintNow.y) * k, tintNow.z + (want.z - tintNow.z) * k,
                   std::fabs(want.w - tintNow.w) < 0.002f ? want.w : tintNow.w + (want.w - tintNow.w) * k);
  static const struct {
    ImGuiCol col;
    float amount;
  } kTint[] = {{ImGuiCol_WindowBg, 0.075f},     {ImGuiCol_ChildBg, 0.06f},          {ImGuiCol_MenuBarBg, 0.10f},
               {ImGuiCol_TitleBg, 0.10f},      {ImGuiCol_TitleBgActive, 0.12f},    {ImGuiCol_Border, 0.40f},
               {ImGuiCol_Separator, 0.50f},    {ImGuiCol_ScrollbarBg, 0.06f},      {ImGuiCol_PopupBg, 0.05f},
               {ImGuiCol_Header, 0.20f},       {ImGuiCol_HeaderHovered, 0.24f},    {ImGuiCol_FrameBg, 0.05f},
               {ImGuiCol_Tab, 0.14f},          {ImGuiCol_TabSelected, 0.24f},      {ImGuiCol_TabHovered, 0.30f},
               {ImGuiCol_TabSelectedOverline, 1.f}, {ImGuiCol_TabDimmedSelectedOverline, 1.f}};
  ImGuiStyle& st = ImGui::GetStyle();
  for (auto& t : kTint) {
    const ImVec4& b = baseColors[t.col];
    float a = t.amount * tintNow.w;
    st.Colors[t.col] = ImVec4(b.x + (tintNow.x - b.x) * a, b.y + (tintNow.y - b.y) * a, b.z + (tintNow.z - b.z) * a, b.w);
  }
}

int App::pushPanelColors(const char* label) {
  // (the Full layout) a docked panel's tab in its step's color: a hint of it on the tab, the line over the chosen one
  const char* id = std::strstr(label, "###");
  GuidedStep s = panelStep(id ? id + 3 : label);
  if (s == GuidedStep::Count || guidedOn() || blenderOn() || !engine.features.on("guided_layout")) return 0;  // (Blender's tabs: all alike)
  ImVec4 c = stepColor(s);
  const ImVec4* b = baseColors;
  ImGui::PushStyleColor(ImGuiCol_Tab, mixColor(b[ImGuiCol_Tab], c, 0.16f));
  ImGui::PushStyleColor(ImGuiCol_TabHovered, mixColor(b[ImGuiCol_TabHovered], c, 0.32f));
  ImGui::PushStyleColor(ImGuiCol_TabSelected, mixColor(b[ImGuiCol_TabSelected], c, 0.24f));
  ImGui::PushStyleColor(ImGuiCol_TabSelectedOverline, ImGui::ColorConvertFloat4ToU32(c));
  ImGui::PushStyleColor(ImGuiCol_TabDimmed, mixColor(b[ImGuiCol_TabDimmed], c, 0.12f));
  ImGui::PushStyleColor(ImGuiCol_TabDimmedSelected, mixColor(b[ImGuiCol_TabDimmedSelected], c, 0.20f));
  ImGui::PushStyleColor(ImGuiCol_TabDimmedSelectedOverline, mixColor(b[ImGuiCol_TabDimmed], c, 0.7f));
  return 7;
}

// ------------------------------------------------------------------ the layout

bool App::guidedOn() const { return layoutMode == LayoutMode::Guided && engine.features.on("guided_layout") && !distractionFree; }

bool App::blenderOn() const { return layoutMode == LayoutMode::Blender && !distractionFree; }

void App::setLayoutMode(LayoutMode m) {
  if (m == LayoutMode::Guided && !engine.features.on("guided_layout")) m = LayoutMode::Full;
  if (m == layoutMode) return;
  layoutMode = m;
  if (m == LayoutMode::Guided) {
    // the step the work is at
    Document& d = engine.doc;
    bool started = d.hasMesh() && !d.startupScene;
    // (the step it was at, unless that step has tools and the tool chosen meanwhile is another step's)
    bool fits = guidedStep == GuidedStep::Paint ? paintTool(tool) : guidedStep == GuidedStep::UVs ? uvTool(tool) : true;
    GuidedStep s = !started ? GuidedStep::Model : fits ? guidedStep : paintTool(tool) ? GuidedStep::Paint : uvTool(tool) ? GuidedStep::UVs : guidedStep;
    if (retopologyTool(tool)) selectTool(ViewportTool::Paint), s = GuidedStep::Paint;
    guidedStep = GuidedStep::Count;  // (so the step is entered: its tool chosen)
    setGuidedStep(s);
    setStatus(T_("Guided layout: the work in five steps along the top. The Full layout is at the right of the menu bar."));
  } else if (m == LayoutMode::Blender) {
    viewOnly = false;
    setBlenderStep(guidedStep);  // (the step's tools and Properties tab)
    setStatus(T_("Blender layout: the steps are workspace tabs, the Outliner and the Properties editor at the right. Full "
                 "or Guided at the right of the menu bar."));
  } else {
    viewOnly = false;
    setStatus(T_("Full layout: every panel, docked. Guided brings back the steps."));
  }
  saveSession(true);
}

void App::setThemeUI(ui::ThemeId t) {
  if (t == ui::themeId()) return;
  ui::setTheme(t);
  pendingUiScale = userUiScale;  // (setupStyle again before the next frame, as for a new UI scale)
  saveSession(true);
}

void App::setGuidedStep(GuidedStep s) {
  if (s == GuidedStep::Count) return;
  if (s != guidedStep) {
    // a view mode the step chose (a bake, the UV checker) goes back when the step is left, if it still shows
    if (stepChangedView && engine.viewOptions.mode == stepViewMode) engine.viewOptions.mode = viewBeforeStep;
    stepChangedView = false;
  }
  guidedStep = s;
  if (s == GuidedStep::UVs && !uvTool(tool)) selectTool(ViewportTool::SelectFaces);
  if (s == GuidedStep::Paint && !paintTool(tool)) selectTool(ViewportTool::Paint);
  guidedLastTool = tool;
  toolPicked = false;
}

void App::stepView(ViewMode m) {
  if (!stepChangedView) viewBeforeStep = engine.viewOptions.mode;
  engine.viewOptions.mode = m;
  stepViewMode = m;
  stepChangedView = m != viewBeforeStep;
}

App::StepState App::guidedStatus(GuidedStep s) {
  Document& d = engine.doc;
  StepState st;
  const bool started = d.hasMesh() && !d.startupScene;
  switch (s) {
    case GuidedStep::Model:
      if (!started) st = {0, T_("Open your model, or try the sample")};
      else st = {1, d.bridge.active ? F_("%s, from Blender", d.mesh.name.c_str()) : F_("%s is open", d.mesh.name.c_str())};
      break;
    case GuidedStep::UVs: {
      if (!started) {
        st = {0, T_("No model yet")};
        break;
      }
      size_t ov = overlappingIslandCount();  // (also the faces with no UV area; both kept until the UVs change)
      if (d.mesh.islandCount == 0) st = {2, T_("The model has no UVs")};
      else if (ov) st = {2, F_(P_("%zu island overlaps another", "%zu islands overlap others", (long)ov), ov)};
      else if (!uvCollapsed.empty())
        st = {2, F_(P_("%zu face has no UV area", "%zu faces have no UV area", (long)uvCollapsed.size()), uvCollapsed.size())};
      else st = {1, F_(P_("%u UV island, no overlaps", "%u UV islands, no overlaps", (long)d.mesh.islandCount), d.mesh.islandCount)};
      break;
    }
    case GuidedStep::Bake: {
      if (!started) {
        st = {0, T_("No model yet")};
        break;
      }
      int out = d.bakesOutOfDate();
      if (out) st = {2, T_("Out of date: the UVs changed since the bake")};
      else if (d.tex.hasAO() && d.tex.hasCurvature()) st = {1, T_("Baked")};
      else if (d.tex.hasAO() || d.tex.hasCurvature()) st = {2, T_("Partly baked")};
      else st = {0, T_("Not baked yet")};
      break;
    }
    case GuidedStep::Paint: {
      int content = 0;
      for (auto& l : d.layers) {
        if (l->isBaked() || l->isFolder()) continue;
        bool has = l->type != LayerType::Paint;
        for (int c = 0; c < kChannelCount && !has; ++c) has = l->hasChannel(c);
        content += has;
      }
      if (content && started) st = {1, F_(P_("%d layer with paint or a material", "%d layers with paint or materials", content), content)};
      else st = {0, T_("Nothing painted yet")};
      break;
    }
    case GuidedStep::Export:
      if (d.hasLastExport && started) st = {1, F_("Exported to %s", d.lastExport.directory.c_str())};
      else if (d.bridge.active) st = {0, T_("Send the textures to Blender, or export them")};
      else st = {0, T_("Not exported yet")};
      break;
    default: break;
  }
  return st;
}

json App::layoutCommand(const json& a) {
  if (!engine.features.on("guided_layout") && (a.value("mode", std::string()) == "guided" || a.contains("step")))
    throw std::runtime_error("the Guided layout is switched off (features: guided_layout)");
  if (a.contains("theme")) setThemeUI(ui::themeFromId(a.at("theme").get<std::string>()));
  if (a.contains("mode")) {
    std::string mode = a.at("mode");
    setLayoutMode(mode == "guided" ? LayoutMode::Guided : mode == "blender" ? LayoutMode::Blender : LayoutMode::Full);
  }
  if (a.contains("step")) {
    std::string want = a.at("step");
    for (auto& si : kSteps) {
      if (want != si.id) continue;
      if (blenderOn()) setBlenderStep(si.step);  // (1.2.7: its workspace tab)
      else setLayoutMode(true), setGuidedStep(si.step);
    }
  }
  // (1.2.10) the Blender layout's Properties tab, and its pin
  static const char* kProps[] = {"step", "object", "layer", "texture_set", "face_sets", "retopology", "history", "notes", "console"};
  static_assert(sizeof kProps / sizeof *kProps == (size_t)PropsTab::Count);
  if (a.contains("properties_tab")) {
    std::string want = a.at("properties_tab");
    auto it = std::find(std::begin(kProps), std::end(kProps), want);
    if (it == std::end(kProps)) throw std::runtime_error("properties_tab: no tab called '" + want + "'");
    blui.props = (PropsTab)(it - std::begin(kProps));
  }
  if (a.contains("pin_properties")) blui.propsPinned = a.at("pin_properties").get<bool>(), saveSession(true);
  json steps = json::array();
  static const char* kState[] = {"todo", "done", "look"};
  for (auto& si : kSteps) {
    StepState st = guidedStatus(si.step);
    steps.push_back({{"step", si.id}, {"state", kState[std::clamp(st.state, 0, 2)]}, {"why", st.text}});
  }
  const bool guided = layoutMode == LayoutMode::Guided && engine.features.on("guided_layout");
  return json{{"layout", guided ? "guided" : layoutMode == LayoutMode::Blender ? "blender" : "full"},
              {"theme", ui::theme().id},
              {"step", info(guidedStep).id},
              {"steps", steps},
              {"properties_tab", kProps[(int)blui.props]},
              {"pin_properties", blui.propsPinned}};
}

void App::layoutSwitch() {
  // small buttons at the right end of the menu bar, the one in use lit: Guided (with its feature), Full, Blender
  const bool withGuided = engine.features.on("guided_layout");
  const char* g = L_("Guided");
  const char* f = L_("Full");
  const char* bl = L_("Blender##layout");  // (not the Blender menu's ID)
  float pad = ImGui::GetStyle().FramePadding.x * 2.f;
  float w = (withGuided ? ImGui::CalcTextSize(g, nullptr, true).x + pad + 6.f * uiScale : 0.f) + ImGui::CalcTextSize(f, nullptr, true).x +
            ImGui::CalcTextSize(bl, nullptr, true).x + pad * 2.f + 12.f * uiScale;
  ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX() + 8.f, ImGui::GetWindowWidth() - w - 8.f * uiScale));
  const ImVec4 black(0.f, 0.f, 0.f, 1.f), deep = ui::theme().accentDeep;
  auto seg = [&](const char* label, bool on, const char* key, const char* tipText) {
    ImGui::PushStyleColor(ImGuiCol_Button, on ? mixColor(black, deep, 0.75f) : IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, on ? mixColor(black, deep, 0.95f) : ImGui::GetColorU32(ImGuiCol_ButtonHovered));
    bool r = ImGui::SmallButton(label);
    ImGui::PopStyleColor(2);
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    engine.uiRects[key] = {a.x, a.y, b.x - a.x, b.y - a.y};
    tip(tipText);
    return r;
  };
  const bool guided = layoutMode == LayoutMode::Guided && withGuided;
  if (withGuided && seg(g, guided, "layout.guided",
                        T_("Guided layout: the work in five steps (Model, UVs, Bake, Paint, Export), each with what it needs in front")))
    setLayoutMode(LayoutMode::Guided);
  if (seg(f, !guided && layoutMode != LayoutMode::Blender, "layout.full",
          T_("Full layout: every panel docked, arranged as you like (View > Reset layout)")))
    setLayoutMode(LayoutMode::Full);
  if (seg(bl, layoutMode == LayoutMode::Blender, "layout.blender",
          T_("Blender layout: arranged as Blender is. The steps are workspace tabs, the tools stand beside the 3D view "
             "with its sidebar, the Outliner (objects, texture sets, layers) and the Properties editor are at the right, "
             "and the asset shelf is under the 3D view")))
    setLayoutMode(LayoutMode::Blender);
}

void App::guidedStepBar(float h) {
  // folder tabs: each step a tab in its color, a little over the one before; the chosen one in front, raised, and
  // one piece with the strip under the tabs (the page), which is in its color too
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 p0 = ImGui::GetCursorScreenPos();
  const float W = ImGui::GetContentRegionAvail().x;
  const ImVec4 barBg = ui::theme().bar;
  dl->AddRectFilled(p0, ImVec2(p0.x + W, p0.y + h), ImGui::ColorConvertFloat4ToU32(barBg));
  const float band = std::round(4.f * uiScale), slant = std::round(10.f * uiScale), r = std::round(6.f * uiScale);
  const float yb = p0.y + h - band, tabH = h - band - std::round(8.f * uiScale), lift = std::round(4.f * uiScale);
  const ImVec4 cur = stepColor(guidedStep);
  const ImU32 page = mixColor(baseColors[ImGuiCol_WindowBg], cur, 0.42f);
  dl->AddRectFilled(ImVec2(p0.x, yb), ImVec2(p0.x + W, p0.y + h), page);
  const float dt = ImGui::GetIO().DeltaTime, ease = 1.f - std::exp(-dt * 16.f);
  struct Tab {
    float x0, x1;
    StepState st;
    std::string label;
    bool hov;
  } tabs[(int)GuidedStep::Count];
  // ---- place them and take the clicks
  float x = p0.x + std::round(8.f * uiScale);
  const float badgeR = std::round(8.5f * uiScale), markW = std::round(14.f * uiScale), padX = std::round(10.f * uiScale);
  for (int k = 0; k < (int)GuidedStep::Count; ++k) {
    const StepInfo& si = kSteps[k];
    Tab& t = tabs[k];
    t.st = guidedStatus(si.step);
    t.label = T_(si.name);
    ImVec2 ts = ImGui::CalcTextSize(t.label.c_str());
    float w = slant + padX + badgeR * 2.f + std::round(7.f * uiScale) + ts.x + std::round(9.f * uiScale) + markW + padX + slant;
    t.x0 = x, t.x1 = x + w;
    ImGui::SetCursorScreenPos(ImVec2(x + slant * 0.5f, yb - tabH - lift));
    ImGui::PushID(si.id);
    if (ImGui::InvisibleButton("##step", ImVec2(w - slant, tabH + lift))) setGuidedStep(si.step);
    t.hov = ImGui::IsItemHovered();
    ImGui::PopID();
    if (t.hov) ImGui::SetTooltip("%s\n%s\n\n%s", T_(si.name), T_(si.tip), t.st.text.c_str());
    engine.uiRects[std::string("guided.step.") + si.id] = {x + slant * 0.5f, yb - tabH - lift, w - slant, tabH + lift};
    float want = guidedStep == si.step ? 1.f : t.hov ? 0.4f : 0.f;
    tabLift[k] += (want - tabLift[k]) * ease;
    if (std::fabs(want - tabLift[k]) < 0.002f) tabLift[k] = want;
    x += w - slant * 0.9f;  // (each tab a little over the one before)
  }
  // ---- draw them: the others first, then the chosen one over them
  auto drawTab = [&](int k) {
    const Tab& t = tabs[k];
    const bool chosen = guidedStep == kSteps[k].step;
    const ImVec4 c = stepColor(kSteps[k].step);
    const float yt = yb - tabH - lift * tabLift[k];
    const float x0 = t.x0, x1 = t.x1, yBottom = chosen ? yb + band : yb;
    auto path = [&] {
      dl->PathClear();
      dl->PathLineTo(ImVec2(x0, yBottom));
      dl->PathLineTo(ImVec2(x0 + slant, yt + r));
      dl->PathArcTo(ImVec2(x0 + slant + r, yt + r), r, IM_PI, IM_PI * 1.5f, 8);
      dl->PathArcTo(ImVec2(x1 - slant - r, yt + r), r, IM_PI * 1.5f, IM_PI * 2.f, 8);
      dl->PathLineTo(ImVec2(x1, yBottom));
    };
    ImU32 fill = chosen ? page : mixColor(barBg, c, t.hov ? 0.40f : 0.27f);
    // a shadow to its right (it stands over the next one), then the tab
    dl->PathClear();
    dl->PathLineTo(ImVec2(x1 - slant * 0.4f, yt + r * 0.6f));
    dl->PathLineTo(ImVec2(x1 + std::round(3.f * uiScale), yBottom));
    dl->PathLineTo(ImVec2(x1 - slant * 0.4f, yBottom));
    dl->PathFillConvex(IM_COL32(0, 0, 0, chosen ? 110 : 60));
    path();
    dl->PathFillConvex(fill);
    // light along its top edge
    dl->AddLine(ImVec2(x0 + slant + r, yt + 1.5f * uiScale), ImVec2(x1 - slant - r, yt + 1.5f * uiScale),
                mixColor(c, ImVec4(1, 1, 1, 1), 0.35f, chosen ? 0.55f : 0.25f), 1.f * uiScale);
    // its edge: the chosen tab's in its full color, the others' a thin line of theirs
    path();
    dl->PathStroke(chosen ? ImGui::ColorConvertFloat4ToU32(c) : mixColor(barBg, c, 0.75f, 0.9f), 0, chosen ? 1.8f * uiScale : 1.f * uiScale);
    // the number in a badge, the name, the state's mark
    float cx = x0 + slant + padX + badgeR, cy = (yt + yb) * 0.5f + (chosen ? 0.f : 1.f);
    dl->AddCircleFilled(ImVec2(cx, cy), badgeR, chosen ? ImGui::ColorConvertFloat4ToU32(c) : mixColor(barBg, c, 0.7f), 20);
    char num[4];
    snprintf(num, sizeof num, "%d", k + 1);
    ImVec2 ns = ImGui::CalcTextSize(num);
    dl->AddText(ImVec2(std::round(cx - ns.x * 0.5f), std::round(cy - ns.y * 0.5f)), IM_COL32(18, 18, 20, 255), num);
    ImVec2 ts = ImGui::CalcTextSize(t.label.c_str());
    float tx = cx + badgeR + std::round(7.f * uiScale);
    dl->AddText(ImVec2(tx, std::round(cy - ts.y * 0.5f)), chosen ? IM_COL32(255, 255, 255, 255) : IM_COL32(205, 207, 212, 255),
                t.label.c_str());
    ImVec2 m(tx + ts.x + std::round(9.f * uiScale) + markW * 0.5f, cy);
    const float mr = markW * 0.38f;
    if (t.st.state == 1) {
      // done: a check mark
      ImU32 cc = IM_COL32(236, 244, 236, 255);
      dl->AddLine(ImVec2(m.x - mr, m.y), ImVec2(m.x - mr * 0.25f, m.y + mr * 0.75f), cc, 2.f * uiScale);
      dl->AddLine(ImVec2(m.x - mr * 0.25f, m.y + mr * 0.75f), ImVec2(m.x + mr, m.y - mr * 0.8f), cc, 2.f * uiScale);
    } else if (t.st.state == 2) {
      // needs a look: an amber warning sign
      ImVec2 a(m.x, m.y - mr * 1.15f), b(m.x + mr * 1.15f, m.y + mr * 0.9f), cpt(m.x - mr * 1.15f, m.y + mr * 0.9f);
      dl->AddTriangleFilled(a, b, cpt, IM_COL32(246, 182, 60, 255));
      dl->AddTriangle(a, b, cpt, IM_COL32(40, 26, 6, 255), 1.f);
      dl->AddLine(ImVec2(m.x, m.y - mr * 0.45f), ImVec2(m.x, m.y + mr * 0.25f), IM_COL32(40, 26, 6, 255), 1.6f * uiScale);
      dl->AddCircleFilled(ImVec2(m.x, m.y + mr * 0.58f), 1.1f * uiScale, IM_COL32(40, 26, 6, 255), 6);
    } else {
      dl->AddCircle(m, mr * 0.8f, IM_COL32(190, 192, 198, 170), 14, 1.4f * uiScale);
    }
  };
  int chosenK = (int)guidedStep;
  for (int k = (int)GuidedStep::Count - 1; k >= 0; --k)
    if (k != chosenK) drawTab(k);  // (right to left: each tab's left edge over the one before it, like folders)
  if (chosenK >= 0 && chosenK < (int)GuidedStep::Count) drawTab(chosenK);
  // the way to everything else: search, at the right
  if (engine.features.on("command_palette")) {
    const char* key = keyLabel("app.palette");
    std::string sl = key ? F_("Search everything  (%s)", key) : std::string(T_("Search everything"));
    float sw = ImGui::CalcTextSize(sl.c_str()).x + ImGui::GetStyle().FramePadding.x * 2.f;
    float sx = std::max(x + slant + 16.f * uiScale, p0.x + W - sw - 10.f * uiScale);
    ImGui::SetCursorScreenPos(ImVec2(sx, yb - tabH * 0.5f - ImGui::GetFrameHeight() * 0.5f));
    if (ImGui::Button((sl + "###guided_search").c_str())) openCommandPalette();
    tip(T_("Every action, panel, material, brush and file: type a few letters. Everything in the Full layout is here too."));
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    engine.uiRects["guided.search"] = {a.x, a.y, b.x - a.x, b.y - a.y};
  }
  ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + h));
}

void App::guidedNextButton() {
  int k = (int)guidedStep + 1;
  if (k >= (int)GuidedStep::Count) return;
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();
  std::string label = std::string(T_(kSteps[k].next)) + "  >###guided_next";
  ImVec4 keep = g_primary;
  g_primary = stepColor(kSteps[k].step);  // (in the color of the step it leads to)
  if (wideButton(label.c_str(), true)) blenderOn() ? setBlenderStep(kSteps[k].step) : setGuidedStep(kSteps[k].step);
  g_primary = keep;
  ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  engine.uiRects["guided.next"] = {a.x, a.y, b.x - a.x, b.y - a.y};
}

// (1.2.7) the Blender layout's Properties editor: the step's own panel, as the Guided layout shows it beside the 3D
// view, with its Next button
void App::stepGuidePanel() {
  g_primary = stepColor(guidedStep);
  switch (guidedStep) {
    case GuidedStep::Model: guidedModelPanel(); break;
    case GuidedStep::UVs: guidedUVPanel(); break;
    case GuidedStep::Bake: guidedBakePanel(); break;
    case GuidedStep::Export: guidedExportPanel(); break;
    default: {
      header(T_("4  Paint"), T_(info(GuidedStep::Paint).tip), stepColor(GuidedStep::Paint));
      StepState st = guidedStatus(GuidedStep::Paint);
      checkLine(st.state, st.text);
      ImGui::Spacing();
      ImGui::PushTextWrapPos(0.f);
      ImGui::BulletText("%s", T_("The layers are in the Outliner above; the selected layer's settings in the Layer tab."));
      ImGui::BulletText("%s", T_("The brush, the colors and the lighting are in the 3D view's sidebar, at its right."));
      ImGui::BulletText("%s", T_("Materials, smart materials, smart masks and brushes are in the shelf under the 3D view: "
                                 "click one, or drag it onto the model or the layers."));
      ImGui::PopTextWrapPos();
      break;
    }
  }
  guidedNextButton();
}

// (1.2.7) the Blender layout: a tool chosen from another step (its key, a menu, the palette, a command) brings that
// step's workspace up, as a mode brings up its tools in Blender; a retopology tool, the Retopology tab
void App::blenderFollowTool() {
  if (tool == guidedLastTool && !toolPicked) return;
  toolPicked = false;
  guidedLastTool = tool;
  if (retopologyTool(tool)) {
    if (guidedStep != GuidedStep::Model) setGuidedStep(GuidedStep::Model);
    guidedLastTool = tool;
    if (!blui.propsPinned) blui.props = PropsTab::Retopology;
  } else if (paintTool(tool) && guidedStep != GuidedStep::Paint) {
    setBlenderStep(GuidedStep::Paint);
  } else if (uvTool(tool) && guidedStep != GuidedStep::UVs) {
    setBlenderStep(GuidedStep::UVs);
  }
}

void App::setBlenderStep(GuidedStep s) {
  setGuidedStep(s);  // (its tools; a view mode the last step chose goes back)
  if (!blui.propsPinned) blui.props = s == GuidedStep::Paint ? PropsTab::Layer : PropsTab::Step;  // (1.2.10: unless pinned)
}

void App::drawGuided() {
  // a tool chosen from another step (its key, a menu, the palette, a command) goes to that step
  if (tool != guidedLastTool || toolPicked) {
    toolPicked = false;
    guidedLastTool = tool;
    if (retopologyTool(tool)) {
      setLayoutMode(false);
      showRetopoWindow();
      setStatus(T_("Retopology is in the Full layout: its tools, the Retopology panel. Guided brings back the steps."));
      return;
    }
    if (paintTool(tool) && guidedStep != GuidedStep::Paint) setGuidedStep(GuidedStep::Paint);
    else if (uvTool(tool) && guidedStep != GuidedStep::UVs) setGuidedStep(GuidedStep::UVs);
  }
  viewOnly = guidedStep == GuidedStep::Model || guidedStep == GuidedStep::Bake || guidedStep == GuidedStep::Export;
  g_primary = stepColor(guidedStep);
  guidedToolGroups = guidedStep == GuidedStep::UVs ? 2 : guidedStep == GuidedStep::Paint ? (1 | 8 | 16 | 32) : 0;
  if (guidedStep == GuidedStep::Model && engine.features.on("scene_objects")) guidedToolGroups = 32;  // (1.2) the Object tool

  ImGuiViewport* vp = ImGui::GetMainViewport();
  const float statusH = ImGui::GetFrameHeight();
  ImVec2 pos = vp->WorkPos, size(vp->WorkSize.x, vp->WorkSize.y - statusH);
  ImGui::SetNextWindowPos(pos);
  ImGui::SetNextWindowSize(size);
  ImGui::SetNextWindowViewport(vp->ID);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
  ImGui::Begin("##Guided", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNavFocus);
  ImGui::PopStyleVar(2);
  const float barH = std::round(46.f * uiScale);
  guidedStepBar(barH);
  const float gap = std::round(3.f * uiScale);
  const float top = pos.y + barH + gap, H = size.y - barH - gap;
  const bool tools = guidedToolGroups != 0;
  const float toolsW = tools ? std::round(std::max(84.f * uiScale, toolbarNamesW)) : 0.f;  // (1.2.8: room for the names)
  const float rightW = std::round(std::clamp(390.f * uiScale, 280.f, size.x * 0.36f));
  const float cx0 = pos.x + (tools ? toolsW + gap : 0.f), cx1 = pos.x + size.x - rightW - gap;
  const ImVec2 right(cx1 + gap, top), rightSize(rightW, H);
  if (tools) embedPanel(&App::drawToolbar, ImVec2(pos.x, top), ImVec2(toolsW, H));
  auto viewport = [&](ImVec2 p, ImVec2 s) {
    ImGui::SetCursorScreenPos(p);
    panelEmbedded = true;
    panelEmbedSize = s;
    drawViewport(false);
    panelEmbedded = false;
  };
  auto stepPanel = [&](void (App::*fn)()) {
    ImGui::SetCursorScreenPos(right);
    ImGui::BeginChild("##guided_step", rightSize, ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders);
    ImVec2 p = ImGui::GetWindowPos(), s = ImGui::GetWindowSize();
    embeddedRects.push_back({"window.Step", ImVec4(p.x, p.y, s.x, s.y)});
    (this->*fn)();
    guidedNextButton();
    ImGui::EndChild();
  };
  switch (guidedStep) {
    case GuidedStep::UVs: {
      float w = cx1 - cx0, vw = std::round(w * 0.52f);
      viewport(ImVec2(cx0, top), ImVec2(vw, H));
      embedPanel(&App::drawUVEditor, ImVec2(cx0 + vw + gap, top), ImVec2(w - vw - gap, H));
      stepPanel(&App::guidedUVPanel);
      break;
    }
    case GuidedStep::Paint: {
      float shelfH = std::round(std::clamp(H * 0.3f, 180.f * uiScale, 330.f * uiScale));
      float w = cx1 - cx0, vh = H - shelfH - gap;
      viewport(ImVec2(cx0, top), ImVec2(w, vh));
      float mw = std::round(w * 0.68f);
      embedPanel(&App::drawMaterials, ImVec2(cx0, top + vh + gap), ImVec2(mw, shelfH));
      embedPanel(&App::drawBrushes, ImVec2(cx0 + mw + gap, top + vh + gap), ImVec2(w - mw - gap, shelfH));
      ImGui::SetCursorScreenPos(right);
      guidedPaintPanel(rightSize);
      break;
    }
    default:
      viewport(ImVec2(cx0, top), ImVec2(cx1 - cx0, H));
      stepPanel(guidedStep == GuidedStep::Model ? &App::guidedModelPanel
                : guidedStep == GuidedStep::Bake ? &App::guidedBakePanel
                                                 : &App::guidedExportPanel);
      break;
  }
  ImGui::End();
}

// ------------------------------------------------------------------ 1. Model

void App::guidedModelPanel() {
  Document& d = engine.doc;
  header(T_("1  Model"), T_("The model you texture. Open its file, or send it from Blender with the Patina add-on."), stepColor(GuidedStep::Model));
  const bool started = d.hasMesh() && !d.startupScene;
  if (!started) {
    if (wideButton(L_("Open a model or project..."), true))
      replaceProject(T_("open another file"), [this] { openFileDialog(DialogPurpose::OpenFile); });
    fs::path sample = paths::dataDir() / "samples" / "Suzanne.glb";
    std::error_code ec;
    if (fs::exists(sample, ec) && wideButton(L_("Open the sample (Suzanne)")))
      replaceProject(T_("open the sample"), [this, sample] {
        std::string err;
        if (!engine.openFile(sample.string(), &err)) showError(T_("Could not open the sample"), err);
      });
    ImGui::TextDisabled("%s", T_("Or drop a file on the window: .obj .fbx .glb .gltf .stl .ply .3mf .patina"));
    ImGui::SeparatorText(T_("From Blender"));
    ImGui::TextWrapped("%s", T_("Install the Patina Bridge add-on that comes with Patina. In Blender's 3D view, press N, open the "
                                "Patina tab and press Send to Patina. The model opens here, and its textures go back with one "
                                "click."));
    ImGui::SeparatorText(T_("A shape to try things on"));
    const char* names[] = {N_("Sphere"), N_("Cube"), N_("Plane"), N_("Torus")};
    const char* prims[] = {"sphere", "cube", "plane", "torus"};
    for (int i = 0; i < 4; ++i) {
      if (i) ImGui::SameLine();
      if (ImGui::Button(L_(names[i]))) {
        std::string p = prims[i];
        replaceProject(T_("start a new project"), [this, p] { runCommand("new_project", {{"primitive", p}}); });
      }
    }
    int shown = 0;
    for (auto& f : recentFiles) {
      if (shown >= 6) break;
      if (!fs::exists(f, ec)) continue;
      if (!shown++) ImGui::SeparatorText(T_("Recent"));
      std::string label = fs::path(f).filename().string() + "##" + f;
      if (ImGui::Selectable(label.c_str())) {
        std::string path = f;
        replaceProject(T_("open another file"), [this, path] {
          std::string err;
          if (engine.openFile(path, &err)) setStatus(F_("Opened %s", path.c_str()));
          else showError(F_("Could not open %s", fs::path(path).filename().string().c_str()), err);
        });
      }
      tip(f.c_str());
    }
    return;
  }
  // ---- the model that is open
  ImGui::Text("%s", d.mesh.name.c_str());
  std::string from = d.bridge.active          ? F_("From Blender: %s", d.bridge.object.c_str())
                     : !d.projectPath.empty() ? fs::path(d.projectPath).filename().string()
                     : !d.meshPath.empty()    ? fs::path(d.meshPath).filename().string()
                                              : std::string(T_("Made in Patina, not saved yet"));
  ImGui::TextDisabled("%s", from.c_str());
  {
    size_t slots = std::max<size_t>(1, d.mesh.materialNames.size());
    ImGui::TextDisabled(T_("%zu vertices, %u faces, %u UV islands"), d.mesh.positions.size(), d.mesh.faceCount(), d.mesh.islandCount);
    ImGui::TextDisabled(P_("%zu material slot", "%zu material slots", (long)slots), slots);
  }
  if (ImGui::Button(L_("Open another...")))
    replaceProject(T_("open another file"), [this] { openFileDialog(DialogPurpose::OpenFile); });
  if (engine.features.on("scene_objects")) {
    // (1.2) the scene: its other parts as objects of their own
    ImGui::SameLine();
    if (ImGui::Button(L_("Add a model to the scene..."))) openFileDialog(DialogPurpose::AddObject);
    {
      ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
      engine.uiRects["guided.add_model"] = {a.x, a.y, b.x - a.x, b.y - a.y};
    }
    tip(T_("A character's clothes, eyes or hair from their own files: each an object with its own layers, "
           "where its file puts it. The Object tool moves, turns and scales them."));
    if (engine.objectCount() > 1) {
      ImGui::SeparatorText(T_("Objects"));
      for (int i = 0; i < engine.objectCount(); ++i) {
        Document& o = engine.object(i);
        ImGui::PushID(i);
        std::string label = o.displayName() + "  " + F_("(%u faces)", o.mesh.faceCount());
        if (ImGui::Selectable(label.c_str(), i == engine.activeObject())) selectObjectUI(i);
        ImGui::PopID();
      }
      ImGui::TextDisabled("%s", T_("The one chosen is the one the other steps work on."));
    }
  }
  if (!d.meshPath.empty() && !d.bridge.active) {
    ImGui::SameLine();
    if (ImGui::Button(L_("Reload from its file"))) {
      json r;
      if (runCommand("reload_mesh", json::object(), &r)) setStatus(F_("Reloaded %s", d.meshPath.c_str()));
    }
    tip(T_("The model's file changed (you edited it elsewhere): load it again and keep the layers"));
  }
  // ---- what is wrong with it
  if (engine.features.on("mesh_check")) {
    ImGui::SeparatorText(T_("Check"));
    const MeshCheckReport& r = d.meshCheck();
    int problems = 0, warnings = 0;
    for (auto& p : r.problems) (p.severity >= 2 ? problems : p.severity == 1 ? warnings : problems) += p.severity >= 1;
    if (!problems && !warnings) {
      checkLine(1, T_("No problems found"));
    } else {
      int shown = 0;
      for (auto& p : r.problems) {
        if (p.severity < 1 || shown >= 4) continue;
        ++shown;
        checkLine(2, F_("%s: %zu", p.name.c_str(), p.count));
        tip(p.what.c_str());
      }
      bool on = engine.viewOptions.mode == ViewMode::MeshCheck;
      if (ImGui::Checkbox(L_("Show them on the model"), &on)) {
        if (on) stepView(ViewMode::MeshCheck);
        else engine.viewOptions.mode = stepChangedView ? viewBeforeStep : ViewMode::Material, stepChangedView = false;
      }
      if (ImGui::Button(L_("Fix everything that can be fixed"))) {
        json res;
        if (runCommand("mesh_fix", {{"all", true}}, &res)) setStatus(describeMeshFix(res));
      }
      tip(T_("Joins open seams, removes faces with no area, duplicates and specks, turns flipped faces round. "
             "Your paint moves with the faces; Mesh > Back to Before the Fix undoes it."));
      ImGui::SameLine();
      if (ImGui::Button(L_("Details..."))) showMeshCheck = true;
    }
  }
  // ---- how many textures, how big
  ImGui::SeparatorText(T_("Textures"));
  textureSetupUI();
  // ---- the rest of the mesh tools, a click away
  if (ImGui::CollapsingHeader(L_("More mesh tools"))) {
    if (engine.features.on("recipes") && ImGui::Button(L_("Size, turn, origin...")) && d.hasMesh()) showTransform = true;
    if (engine.features.on("mesh_reduce") && ImGui::Button(L_("Reduce polygons...")) && d.hasMesh()) showReduce = true;
    if (engine.features.on("lightmap_uvs") && ImGui::Button(L_("Lightmap UVs...")) && d.hasMesh()) showLightmapWindow = true;
    if (engine.features.on("collision") && ImGui::Button(L_("Collision shapes...")) && d.hasMesh()) showCollisionWindow = true;
    if (blenderOn()) {  // (1.2.7: a tab of the Properties editor there)
      if (engine.features.on("retopology") && ImGui::Button(L_("Retopology..."))) blui.props = PropsTab::Retopology;
      tip(T_("Retopology has its own tools and panel: the Retopology tab"));
    } else {
      if (engine.features.on("retopology") && ImGui::Button(L_("Retopology (Full layout)..."))) {
        setLayoutMode(false);
        showRetopoWindow();
      }
      tip(T_("Retopology has its own tools and panel, in the Full layout"));
    }
  }
}

// ------------------------------------------------------------------ 2. UVs

void App::guidedUVPanel() {
  Document& d = engine.doc;
  header(T_("2  UVs"), T_("Patina paints across the surface, so most models paint well as they are. The textures you export need "
                          "clean UVs: no overlaps, no squashed faces. Fix only what is marked."), stepColor(GuidedStep::UVs));
  if (!d.hasMesh()) {
    ImGui::TextDisabled("%s", T_("Open a model first (step 1)."));
    return;
  }
  auto run = [&](const char* cmd, json args, const char* done) {
    json r;
    if (runCommand(cmd, args, &r) && done) setStatus(done);
  };
  const bool sel = !d.selectedFaces().empty();
  size_t ov = overlappingIslandCount();
  ImGui::SeparatorText(T_("Check"));
  if (d.mesh.islandCount == 0) {
    checkLine(2, T_("No UVs: unwrap the model below"));
  } else {
    checkLine(1, F_(P_("%u UV island", "%u UV islands", (long)d.mesh.islandCount), d.mesh.islandCount));
    if (ov) {
      checkLine(2, F_(P_("%zu island overlaps another: paint shows twice there", "%zu islands overlap others: paint shows twice there",
                         (long)ov), ov));
      ImGui::SameLine();
      if (ImGui::SmallButton(L_("Fix##overlaps"))) run("uv_fix_overlaps", {{"reproject", reprojectOnUVEdit}}, T_("Overlaps fixed"));
      tip(T_("Moves the overlapping islands apart (mirrored halves that share texels on purpose stay)"));
    } else {
      checkLine(1, T_("No overlaps"));
    }
    if (!uvCollapsed.empty()) {
      checkLine(2, F_(P_("%zu face has no UV area: it gets no paint", "%zu faces have no UV area: they get no paint",
                         (long)uvCollapsed.size()), uvCollapsed.size()));
      ImGui::SameLine();
      if (ImGui::SmallButton(L_("Unwrap them"))) {
        d.faceSelection.assign(d.mesh.faceCount(), 0);
        for (uint32_t f : uvCollapsed) d.faceSelection[f] = 1;
        d.updateSelectionFlags();
        run("uv_unwrap", {{"method", "smart"}, {"selection", true}, {"reproject", reprojectOnUVEdit}}, T_("Gave those faces UVs of their own"));
      }
    } else {
      checkLine(1, T_("Every face has room in the texture"));
    }
  }
  ImGui::SeparatorText(sel ? T_("Unwrap the selected faces") : T_("Unwrap the whole model"));
  if (wideButton(L_("Unwrap automatically")))
    run("uv_unwrap", {{"method", "smart"}, {"selection", sel}, {"reproject", reprojectOnUVEdit}}, T_("Unwrapped"));
  tip(T_("Cuts the model where it bends sharply and lays each piece flat (Smart UV Project). Good for hard-surface models."));
  if (ImGui::Button(L_("Unwrap along the seams"))) run("uv_unwrap", {{"method", "lscm"}, {"selection", sel}, {"reproject", reprojectOnUVEdit}}, T_("Unwrapped along the seams"));
  tip(T_("Flattens each piece the seams cut out (LSCM): lay the seams first with the Seams tool (M). Good for characters and cloth."));
  ImGui::SameLine();
  if (ImGui::Button(L_("Pack the islands"))) run("uv_pack", {{"reproject", reprojectOnUVEdit}}, T_("Packed the islands"));
  tip(T_("Arranges the islands to fill the texture, small ones in the gaps of big ones"));
  ImGui::Checkbox(L_("Keep the paint when the UVs change"), &reprojectOnUVEdit);
  tip(T_("Paint already on the model moves to the new UVs, so nothing is lost"));
  ImGui::SeparatorText(T_("By hand"));
  auto bullet = [](const std::string& text) {
    ImGui::Bullet();
    ImGui::TextWrapped("%s", text.c_str());
  };
  auto key = [this](const char* id, const char* fallback) { return keyLabel(id) ? keyLabel(id) : fallback; };
  bullet(F_("Select (%s): click an island, Shift adds, Ctrl takes away. Visible only, on its bar: just the part you see.",
            key("tool.select", "-")));
  bullet(F_("Seams (%s): drag along the model's edges to cut a seam there.", key("tool.seams", "-")));
  bullet(F_("Island brush (%s): paint over a part, then press %s: it becomes one upright island.", key("tool.island", "-"),
            key("island.make", "Enter")));
  bullet(T_("The UV editor: drag islands; G, R and S move, rotate and scale; right-click for every UV tool."));
  ImGui::SeparatorText(T_("See"));
  bool checker = engine.viewOptions.mode == ViewMode::UVChecker;
  if (ImGui::Checkbox(L_("Checker pattern on the model"), &checker)) {
    if (checker) stepView(ViewMode::UVChecker);
    else engine.viewOptions.mode = stepChangedView ? viewBeforeStep : ViewMode::Material, stepChangedView = false;
  }
  tip(T_("Squares that stay square: the UVs are good there. Stretched or uneven squares show stretch."));
  ImGui::SameLine();
  ImGui::Checkbox(L_("Seams##guided"), &engine.viewOptions.seams);
  ImGui::SameLine();
  ImGui::Checkbox(L_("Tint islands"), &engine.viewOptions.islandTint);
  ImGui::TextDisabled("%s", T_("More: the UV menu, or right-click in the UV editor."));
}

// ------------------------------------------------------------------ 3. Bake

void App::guidedBakePanel() {
  Document& d = engine.doc;
  header(T_("3  Bake"), T_("Baking measures the model's shape once: what is tucked in, the edges and creases, the thin parts. Smart "
                           "materials and smart masks use these maps to put dirt in the creases and wear on the edges."), stepColor(GuidedStep::Bake));
  if (!d.hasMesh()) {
    ImGui::TextDisabled("%s", T_("Open a model first (step 1)."));
    return;
  }
  const int out = d.bakesOutOfDate();
  struct M {
    const char* name;
    const char* what;
    bool baked;
    ViewMode view;
  };
  const M maps[3] = {{N_("Ambient occlusion"), N_("How much light reaches each spot: dark in folds and gaps"), d.tex.hasAO(), ViewMode::AO},
                     {N_("Curvature"), N_("Edges that stick out and creases that go in: wear and dirt follow them"), d.tex.hasCurvature(),
                      ViewMode::Curvature},
                     {N_("Thickness"), N_("How thin the model is: ears, fins and cloth let light through"), d.tex.hasThickness(),
                      ViewMode::Thickness}};
  ImGui::SeparatorText(T_("Mesh maps"));
  for (int k = 0; k < 3; ++k) {
    ImGui::PushID(k);
    ImGui::Checkbox(L_(maps[k].name), &bakeWant[k]);
    tip(T_(maps[k].what));
    ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.55f);
    if (out & (1 << k)) ImGui::TextColored(kLookText, "%s", T_("out of date"));
    else if (maps[k].baked) ImGui::TextColored(kDoneText, "%s", T_("baked"));
    else ImGui::TextDisabled("%s", T_("not baked"));
    if (maps[k].baked) {
      ImGui::SameLine();
      bool showing = engine.viewOptions.mode == maps[k].view;
      if (ImGui::SmallButton(showing ? L_("Hide##map") : L_("Show##map"))) {
        if (showing) engine.viewOptions.mode = stepChangedView ? viewBeforeStep : ViewMode::Material, stepChangedView = false;
        else stepView(maps[k].view);
      }
      tip(T_("See this map on the model"));
    }
    ImGui::PopID();
  }
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
  ImGui::SliderInt(L_("Quality"), &d.aoSamples, 8, 512);
  tip(T_("Rays per texel for the ambient occlusion: more is smoother and slower (64 is good)"));
  bool any = bakeWant[0] || bakeWant[1] || bakeWant[2];
  bool all = (!bakeWant[0] || d.tex.hasAO()) && (!bakeWant[1] || d.tex.hasCurvature()) && (!bakeWant[2] || d.tex.hasThickness());
  ImGui::BeginDisabled(!any);
  if (wideButton(all && !out ? L_("Bake again") : L_("Bake"), !all || out)) {
    d.rebakeMeshMaps(bakeWant[0], bakeWant[1], bakeWant[2]);
    setStatus(T_("Baked the mesh maps"));
  }
  ImGui::EndDisabled();
  ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  engine.uiRects["guided.bake"] = {a.x, a.y, b.x - a.x, b.y - a.y};
  if (out) ImGui::TextColored(kLookText, "%s", T_("The UVs changed since the bake: bake again so the maps match them."));
  ImGui::TextDisabled("%s", T_("Each baked map is a layer at the bottom of the layer stack: paint fixes onto it there."));
  ImGui::SeparatorText(T_("From a high-poly model"));
  ImGui::TextWrapped("%s", T_("Sculpted detail from a high-poly version of this model, baked into the normal map."));
  if (ImGui::Button(L_("Bake from a high-poly model..."))) showHighPoly = true;
  if (d.tex.hasHighNormal()) {
    ImGui::SameLine();
    ImGui::TextColored(kDoneText, "%s", T_("baked"));
  }
  if (engine.features.on("bake_down")) {
    ImGui::SeparatorText(T_("Onto a low-poly model"));
    ImGui::TextWrapped("%s", T_("Painted this high-poly? Bake down moves every layer onto its low-poly version (from Blender, a "
                                "file, or Patina's retopology), which becomes the model."));
    if (ImGui::Button(L_("Bake down to a low-poly model..."))) showBakeDown = true;
    {
      ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
      engine.uiRects["guided.bake_down"] = {a.x, a.y, b.x - a.x, b.y - a.y};
    }
    if (d.bakeDownCurrent()) bakeDownReportUI(true);
  }
}

// ------------------------------------------------------------------ 4. Paint

void App::guidedPaintPanel(ImVec2 size) {
  Document& d = engine.doc;
  const float gap = std::round(3.f * uiScale);
  ImVec2 p = ImGui::GetCursorScreenPos();
  // a first hint while nothing is painted (it goes once there is a layer with paint or a material)
  float hintH = 0.f;
  if (guidedStatus(GuidedStep::Paint).state == 0 && d.hasMesh()) {
    hintH = std::round(ImGui::GetTextLineHeightWithSpacing() * 3.2f);
    ImGui::BeginChild("##paint_hint", ImVec2(size.x, hintH), ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders);
    ImGui::PushTextWrapPos(0.f);
    ImGui::TextColored(kLookText, "%s", T_("Start here"));
    ImGui::TextUnformatted(T_("Drag a smart material from the shelf below onto the model, or paint with the brush (B): a color "
                              "from Colors, or a material from the shelf."));
    ImGui::PopTextWrapPos();
    ImGui::EndChild();
    hintH += gap;
  }
  float rest = size.y - hintH;
  float layersH = std::round(rest * 0.55f);
  embedPanel(&App::drawLayers, ImVec2(p.x, p.y + hintH), ImVec2(size.x, layersH));
  ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + hintH + layersH + gap));
  ImGui::BeginChild("##paint_side", ImVec2(size.x, rest - layersH - gap), ImGuiChildFlags_None);
  if (ImGui::BeginTabBar("##paint_tabs")) {
    int k = 0;
    auto tab = [&](const char* label, const char* key, void (App::*draw)()) {
      bool open = ImGui::BeginTabItem(label, nullptr, guidedPaintTab == k++ ? ImGuiTabItemFlags_SetSelected : 0);
      ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
      engine.uiRects[key] = {a.x, a.y, b.x - a.x, b.y - a.y};
      if (open) {
        embedPanel(draw, ImVec2(-1.f, 0.f), ImGui::GetContentRegionAvail());
        ImGui::EndTabItem();
      }
    };
    tab(L_("Brush"), "guided.tab.properties", &App::drawProperties);
    tab(L_("Colors"), "guided.tab.colors", &App::drawColors);
    tab(L_("History"), "guided.tab.history", &App::drawHistory);
    // the base material under the layers and the lighting (the Texture Set panel's, in the Full layout)
    bool scene = ImGui::BeginTabItem(L_("Scene"), nullptr, guidedPaintTab == 3 ? ImGuiTabItemFlags_SetSelected : 0);
    {
      ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
      engine.uiRects["guided.tab.scene"] = {a.x, a.y, b.x - a.x, b.y - a.y};
    }
    if (scene) {
      ImGui::BeginChild("Scene", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
      sceneUI();
      ImGui::EndChild();
      ImGui::EndTabItem();
    }
    guidedPaintTab = -1;
    ImGui::EndTabBar();
  }
  ImGui::EndChild();
}

// ------------------------------------------------------------------ 5. Export

void App::guidedExportPanel() {
  Document& d = engine.doc;
  header(T_("5  Export"), T_("Send the textures back to Blender, or write them as image files for your engine. Save the project too: "
                             "the exported images are flat, the project keeps every layer."), stepColor(GuidedStep::Export));
  if (!d.hasMesh()) {
    ImGui::TextDisabled("%s", T_("Open a model first (step 1)."));
    return;
  }
  ImGui::SeparatorText(T_("Blender"));
  if (d.bridge.active) {
    if (wideButton(L_("Send the textures to Blender"), true)) {
      std::string err;
      if (engine.pushToBlender(&err)) setStatus(T_("Sent textures to Blender"));
      else showError(T_("The textures were NOT sent"), err);
    }
    ImGui::Checkbox(L_("Send them after every change"), &engine.autoPushToBlender);
    ImGui::TextDisabled(T_("Linked to %s"), d.bridge.object.c_str());
  } else {
    ImGui::TextWrapped("%s", T_("Not linked. To texture a Blender object, send it from Blender (N panel > Patina > Send to Patina): "
                                "its textures then come back with one click."));
    if (engine.features.on("model_export") && ImGui::Button(L_("Send this model to Blender as a new object"))) {
      json r;
      if (runCommand("send_model_to_blender", json::object(), &r))
        setStatus(F_("Sent the model to Blender as %s: Patina is linked to it now", r.value("object", std::string()).c_str()));
    }
    tip(T_("Blender must be running with the Patina Bridge add-on"));
  }
  ImGui::SeparatorText(T_("Texture files"));
  if (exportDir.empty()) exportDir = (paths::homeDir() / "patina_export").string();
  float w = ImGui::GetContentRegionAvail().x * 0.62f;
  ImGui::SetNextItemWidth(w);
  ImGui::Combo(L_("For"), &exportPresetIdx, Z_("Blender (Principled BSDF)\0glTF (metallicRoughness)\0Unreal (ORM, DirectX normal)\0Unity (MaskMap)\0"));
  tip(T_("Which maps are written and how they are packed: each engine wants its own (Unreal packs occlusion, roughness and "
         "metallic in one image and wants DirectX normals)"));
  ImGui::SetNextItemWidth(w);
  ImGui::Combo(L_("Size"), &exportSizeIdx, Z_("Texture set\0" "512\0" "1024\0" "2048\0" "4096\0" "8192\0" "16384\0" "32768\0"));
  ImGui::SetNextItemWidth(w);
  ImGui::InputText(L_("Folder"), &exportDir);
  ImGui::SameLine();
  if (ImGui::Button("...##guided_dir")) openFileDialog(DialogPurpose::ExportDir);
  exportPlanUI(12);
  if (wideButton(L_("Export the textures"), !d.bridge.active)) exportTexturesNow();
  {
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    engine.uiRects["guided.export"] = {a.x, a.y, b.x - a.x, b.y - a.y};
  }
  if (ImGui::Button(L_("Every export option..."))) exportWhat = 0, showExport = true;
  tip(T_("File names, layered PSD files, one layer or its mask, the baked maps"));
  if (d.hasLastExport) {
    ImGui::SameLine();
    if (ImGui::Button(L_("Export again"))) exportAgain();
    ImGui::TextDisabled(T_("Last: %s (%s)"), d.lastExport.directory.c_str(), exportPresetName(d.lastExport.preset));
  }
  ImGui::SeparatorText(T_("The project"));
  if (ImGui::Button(d.projectPath.empty() ? L_("Save the project...") : L_("Save the project"))) saveProject(false);
  ImGui::SameLine();
  if (ImGui::Button(L_("Save as..."))) saveProject(true);
  ImGui::SeparatorText(T_("More"));
  if (engine.features.on("model_export") && ImGui::Button(L_("The model with its textures (.glb, .obj...)..."))) showModelExport = true;
  if (ImGui::Button(L_("Render an image..."))) showRender = true;
}

// ------------------------------------------------------------------ unsaved work

void App::replaceProject(const std::string& what, std::function<void()> action) {
  Document& d = engine.doc;
  // (a model just opened, or the start's sphere, with nothing done to it: nothing to lose; 1.2: in any object)
  bool work = engine.sceneDirty() && (!d.projectPath.empty() || engine.sceneChanged);
  for (int i = 0; i < engine.objectCount() && !work; ++i) work = engine.object(i).dirty && engine.object(i).undo.canUndo();
  if (!work) {
    action();
    return;
  }
  pendingReplace = std::move(action);
  pendingReplaceWhat = what;
  replaceAsked = true;
}

void App::drawReplacePrompt() {
  if (replaceAsked) ImGui::OpenPopup("###replace_prompt"), replaceAsked = false;
  std::string title = std::string(T_("Unsaved changes")) + "###replace_prompt";
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  if (!ImGui::BeginPopupModal(title.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
  Document& d = engine.doc;
  std::string name = d.projectPath.empty() ? std::string(T_("untitled")) : fs::path(d.projectPath).filename().string();
  ImGui::Text(T_("Save the changes to %s before you %s?"), name.c_str(), pendingReplaceWhat.c_str());
  ImGui::TextDisabled("%s", T_("Without saving, they are lost."));
  ImGui::Spacing();
  auto rectOf = [this](const char* key) {
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    engine.uiRects[key] = {a.x, a.y, b.x - a.x, b.y - a.y};
  };
  bool save = ImGui::Button(L_("Save"), ImVec2(110 * uiScale, 0));
  rectOf("replace.save");
  if (save) {
    auto act = std::move(pendingReplace);
    pendingReplace = nullptr;
    ImGui::CloseCurrentPopup();
    if (d.projectPath.empty()) {
      afterSave = std::move(act);  // (Save As first; the action once the project is written)
      saveProject(true);
    } else {
      saveProject(false);
      if (!d.dirty && act) act();
    }
  }
  ImGui::SameLine();
  bool discard = ImGui::Button(L_("Don't save"), ImVec2(110 * uiScale, 0));
  rectOf("replace.discard");
  if (discard) {
    auto act = std::move(pendingReplace);
    pendingReplace = nullptr;
    ImGui::CloseCurrentPopup();
    if (act) act();
  }
  ImGui::SameLine();
  bool cancel = ImGui::Button(L_("Cancel"), ImVec2(110 * uiScale, 0));
  rectOf("replace.cancel");
  if (cancel || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    pendingReplace = nullptr;
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

}  // namespace patina
