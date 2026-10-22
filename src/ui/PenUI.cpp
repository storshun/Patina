// Pen tablet test window, pen preferences and the pen commands.
#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <filesystem>

#include "core/Log.h"
#include "core/Paths.h"
#include "ui/App.h"

namespace patina {

static void curveWidget(const PenInput& pen, ImVec2 size) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImVec2 a = ImGui::GetCursorScreenPos(), b = a + size;
  dl->AddRectFilled(a, b, IM_COL32(24, 25, 28, 255), 4.f);
  dl->AddRect(a, b, IM_COL32(70, 72, 78, 255), 4.f);
  for (int g = 1; g < 4; ++g) {
    float x = a.x + size.x * g / 4.f, y = a.y + size.y * g / 4.f;
    dl->AddLine(ImVec2(x, a.y), ImVec2(x, b.y), IM_COL32(45, 46, 52, 255));
    dl->AddLine(ImVec2(a.x, y), ImVec2(b.x, y), IM_COL32(45, 46, 52, 255));
  }
  ImVec2 prev;
  for (int i = 0; i <= 64; ++i) {
    float raw = i / 64.f;
    ImVec2 p(a.x + raw * size.x, b.y - pen.map(raw) * size.y);
    if (i) dl->AddLine(prev, p, IM_COL32(230, 140, 50, 255), 2.f);
    prev = p;
  }
  if (pen.seenPen()) {
    float raw = pen.rawPressure();
    ImVec2 p(a.x + raw * size.x, b.y - pen.map(raw) * size.y);
    dl->AddCircleFilled(p, 4.f, IM_COL32(255, 255, 255, 255));
  }
  dl->AddText(ImVec2(a.x + 4, b.y - 16), IM_COL32(140, 140, 150, 255), T_("pen pressure"));
  dl->AddText(ImVec2(a.x + 4, a.y + 2), IM_COL32(140, 140, 150, 255), T_("effect"));
  ImGui::Dummy(size);
}

static bool penSettingsWidgets(PenInput& pen, float uiScale) {
  bool changed = false;
  ImGui::SetNextItemWidth(200 * uiScale);
  changed |= ImGui::SliderFloat(L_("Feel"), &pen.softness, -1.f, 1.f, pen.softness < -0.05f ? T_("firmer %.2f")
                                                                      : pen.softness > 0.05f ? T_("softer %.2f") : T_("linear"));
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", T_("Softer: light touches already paint strongly (good for light-handed artists and stiff pens).\n"
                               "Firmer: you need to press harder for full size / flow."));
  ImGui::SetNextItemWidth(200 * uiScale);
  changed |= ImGui::SliderFloat(L_("Ignore below"), &pen.minPressure, 0.f, 0.3f, "%.2f");
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", T_("Pressure under this counts as none. Raise it if the pen paints when it only rests on the tablet."));
  ImGui::SetNextItemWidth(200 * uiScale);
  changed |= ImGui::SliderFloat(L_("Full pressure at"), &pen.fullPressure, 0.3f, 1.f, "%.2f");
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", T_("Pressure that already gives the full effect. Lower it if you never reach full size."));
  if (pen.fullPressure < pen.minPressure + 0.05f) pen.fullPressure = std::min(1.f, pen.minPressure + 0.05f);
  changed |= ImGui::Checkbox(L_("The eraser end erases"), &pen.eraserTipErases);
  for (int b = 0; b < 2; ++b) {
    ImGui::SetNextItemWidth(200 * uiScale);
    int v = (int)pen.button[b];
    const char* items = Z_("Right click\0Middle click (orbit / pan)\0Swap colors\0Undo\0Nothing\0");
    if (ImGui::Combo(b ? L_("Pen button 2 (upper)") : L_("Pen button 1 (lower)"), &v, items)) {
      pen.button[b] = (PenButtonAction)v;
      changed = true;
    }
  }
  if (ImGui::SmallButton(L_("Reset to defaults"))) {
    PenInput def;
    pen.softness = def.softness;
    pen.minPressure = def.minPressure;
    pen.fullPressure = def.fullPressure;
    pen.eraserTipErases = def.eraserTipErases;
    pen.button[0] = def.button[0];
    pen.button[1] = def.button[1];
    changed = true;
  }
  return changed;
}

void App::drawPenPrefs() {
  ImGui::SeparatorText(T_("Pen tablet"));
  if (penSettingsWidgets(pen, uiScale)) saveSession(true);
  if (ImGui::Button(L_("Test the pen..."))) showPenTest = true;
  ImGui::SameLine();
  ImGui::TextDisabled("%s", pen.seenPen() ? T_("Pen seen this session") : T_("No pen seen yet"));
}

void App::drawPenTest() {
  if (!showPenTest) return;
  ImGui::SetNextWindowSize(ImVec2(820, 680) * uiScale, ImGuiCond_FirstUseEver);
  if (!ImGui::Begin(L_("Pen tablet test"), &showPenTest, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse)) {
    ImGui::End();
    return;
  }
  json dg = pen.diagnostics();
  // ---- left: live values and settings
  ImGui::BeginGroup();
  ImGui::PushItemWidth(250 * uiScale);
  ImGui::Text(T_("System: %s, %s, SDL %s"), dg["platform"].get<std::string>().c_str(),
              dg["video_driver"].get<std::string>().c_str(), dg["sdl"].get<std::string>().c_str());
  if (!pen.seenPen()) {
    ImGui::TextColored(ImVec4(1, 0.72f, 0.35f, 1), "%s", T_("No pen events yet - move the pen over this window."));
  } else {
    ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.5f, 1), T_("Pen: %s%s%s"),
                       pen.penDown() ? T_("touching") : pen.penInProximity() ? T_("hovering") : T_("away"),
                       pen.eraser() ? T_(", eraser end") : "", pen.lastWasPen() ? "" : T_(" (last input: mouse)"));
  }
  float raw = pen.rawPressure();
  char buf[64];
  snprintf(buf, sizeof buf, T_("raw %.3f"), raw);
  ImGui::ProgressBar(raw, ImVec2(250 * uiScale, 0), buf);
  ImGui::SameLine();
  ImGui::TextUnformatted(T_("Pressure"));
  snprintf(buf, sizeof buf, "%.3f", pen.map(raw));
  ImGui::ProgressBar(pen.map(raw), ImVec2(250 * uiScale, 0), buf);
  ImGui::SameLine();
  ImGui::TextUnformatted(T_("After the curve"));
  ImGui::Text(T_("Tilt %.0f / %.0f degrees   Samples %.0f per second"), pen.tilt().x, pen.tilt().y, pen.motionRateHz());
  ImGui::Text(T_("Range seen %.2f - %.2f   Max tilt %.0f   Eraser %s"), dg["pressure_seen"]["min"].get<float>(),
              dg["pressure_seen"]["max"].get<float>(), dg["tilt_seen_degrees"].get<float>(),
              dg["eraser_seen"].get<bool>() ? T_("seen") : T_("not seen"));
  // pressure over the last seconds
  {
    const auto& h = pen.pressureHistory();
    static float vals[200];
    int n = 0;
    if (!h.empty()) {
      double t1 = h.back().first;
      for (int i = 0; i < 200; ++i) {
        double t = t1 - 4.0 + 4.0 * i / 199.0;
        auto it = std::lower_bound(h.begin(), h.end(), std::make_pair(t, -1.f));
        vals[i] = it == h.end() ? h.back().second : it->second;
      }
      n = 200;
    }
    ImGui::PlotLines("##hist", n ? vals : nullptr, n, 0, T_("pressure, last 4 s"), 0.f, 1.f, ImVec2(250 * uiScale, 60 * uiScale));
  }
  ImGui::SeparatorText(T_("Settings"));
  if (penSettingsWidgets(pen, uiScale)) saveSession(true);
  curveWidget(pen, ImVec2(250, 120) * uiScale);
  ImGui::PopItemWidth();
  ImGui::EndGroup();
  ImGui::SameLine();
  // ---- right: scribble pad
  ImGui::BeginGroup();
  ImGui::TextUnformatted(T_("Draw here: line width follows the pressure after the curve"));
  ImVec2 padSize = ImGui::GetContentRegionAvail() - ImVec2(0, ImGui::GetFrameHeightWithSpacing() * 2.5f);
  padSize.x = std::max(padSize.x, 200.f);
  padSize.y = std::max(padSize.y, 200.f);
  ImVec2 a = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##pad", padSize);
  engine.uiRects["pen.pad"] = {a.x, a.y, padSize.x, padSize.y};  // (the manual's screenshots)
  bool padActive = ImGui::IsItemActive();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(a, a + padSize, IM_COL32(236, 233, 226, 255), 4.f);
  static bool drawing = false;
  if (padActive) {
    for (auto& s : pen.frameSamples()) {
      if (!s.down) continue;
      if (s.pos.x < a.x || s.pos.y < a.y || s.pos.x > a.x + padSize.x || s.pos.y > a.y + padSize.y) continue;
      float w = 1.f + 14.f * (s.pen ? s.pressure : 1.f) * uiScale;
      scribble.push_back({s.pos - vec2(a.x, a.y), w, !drawing});
      drawing = true;
    }
  } else {
    drawing = false;
  }
  if (scribble.size() > 20000) scribble.erase(scribble.begin(), scribble.begin() + 5000);
  dl->PushClipRect(a, a + padSize, true);
  for (size_t i = 1; i < scribble.size(); ++i) {
    if (scribble[i].start) continue;
    ImVec2 p0(a.x + scribble[i - 1].p.x, a.y + scribble[i - 1].p.y), p1(a.x + scribble[i].p.x, a.y + scribble[i].p.y);
    float w = 0.5f * (scribble[i - 1].w + scribble[i].w);
    dl->AddLine(p0, p1, IM_COL32(30, 30, 34, 255), w);
    dl->AddCircleFilled(p1, w * 0.5f, IM_COL32(30, 30, 34, 255));
  }
  dl->PopClipRect();
  if (ImGui::Button(L_("Clear"))) scribble.clear();
  ImGui::SameLine();
  if (ImGui::Button(L_("Copy report"))) {
    ImGui::SetClipboardText(pen.diagnostics().dump(2).c_str());
    setStatus(T_("Pen report copied to the clipboard"));
  }
  ImGui::SameLine();
  if (ImGui::Button(L_("Save report"))) {
    std::filesystem::path f = paths::ensureDir(paths::cacheDir()) / "pen_report.json";
    if (paths::writeFileAtomic(f, pen.diagnostics().dump(2) + "\n")) setStatus(F_("Pen report saved to %s", f.string().c_str()));
  }
  ImGui::SameLine();
  if (ImGui::Button(L_("Reset counters"))) pen.resetStats();
  for (auto& f : dg["findings"]) ImGui::TextWrapped("%s", T_(f.get<std::string>().c_str()));  // (marked in PenInput.cpp)
  ImGui::EndGroup();
  ImGui::End();
}

void App::registerPenCommands() {
  engine.commands.add({"pen_info",
                       "Pen tablet diagnostics: whether pen events arrive, pressure range and tilt seen, sample rate, "
                       "eraser, buttons, the pressure curve settings, and plain-language findings.",
                       {{"type", "object"}, {"properties", json::object()}},
                       [this](CommandContext&, const json&) {
                         json d = pen.diagnostics();
                         // where the 3D view is in the window (pixels), for scripted input
                         d["viewport_origin"] = {lastViewportOrigin.x, lastViewportOrigin.y};
                         d["viewport_size"] = {lastViewportSize.x, lastViewportSize.y};
                         return d;
                       }});
  engine.commands.add(
      {"set_pen_settings",
       "Pen tablet settings: pressure curve (softness -1 firm .. 1 soft, min_pressure, full_pressure), "
       "eraser_tip_erases, and what the barrel buttons do (button1 / button2: right, middle, swap_colors, undo, nothing).",
       {{"type", "object"},
        {"properties",
         {{"softness", {{"type", "number"}}},
          {"min_pressure", {{"type", "number"}}},
          {"full_pressure", {{"type", "number"}}},
          {"eraser_tip_erases", {{"type", "boolean"}}},
          {"button1", {{"type", "string"}, {"enum", {"right", "middle", "swap_colors", "undo", "nothing"}}}},
          {"button2", {{"type", "string"}, {"enum", {"right", "middle", "swap_colors", "undo", "nothing"}}}}}}},
       [this](CommandContext&, const json& a) {
         if (a.contains("softness")) pen.softness = std::clamp(a["softness"].get<float>(), -1.f, 1.f);
         if (a.contains("min_pressure")) pen.minPressure = std::clamp(a["min_pressure"].get<float>(), 0.f, 0.5f);
         if (a.contains("full_pressure")) pen.fullPressure = std::clamp(a["full_pressure"].get<float>(), 0.3f, 1.f);
         if (a.contains("eraser_tip_erases")) pen.eraserTipErases = a["eraser_tip_erases"].get<bool>();
         for (int b = 0; b < 2; ++b) {
           const char* k = b ? "button2" : "button1";
           if (!a.contains(k)) continue;
           std::string v = a[k];
           pen.button[b] = v == "right" ? PenButtonAction::RightClick
                         : v == "middle" ? PenButtonAction::MiddleClick
                         : v == "swap_colors" || v == "swap_colours" ? PenButtonAction::SwapColors  // either spelling
                         : v == "undo" ? PenButtonAction::Undo
                                       : PenButtonAction::Nothing;
         }
         saveSession(true);
         return pen.diagnostics()["settings"];
       }});
  // test harness: synthetic tablet input in the window (GUI only, not offered to MCP)
  engine.commands.add(
      {"inject_pen",
       "Feeds synthetic pen tablet input to the window, a few samples per frame (tests). samples: "
       "[{x, y (normalized viewport), pressure, down, eraser, tilt_x, tilt_y, button, mouse}].",
       {{"type", "object"}, {"properties", {{"samples", {{"type", "array"}}}, {"space", {{"type", "string"}}}}}},
       [this](CommandContext&, const json& a) {
         std::vector<PenInput::Injected> v;
         bool window = a.value("space", std::string("viewport")) == "window";
         for (auto& s : a.at("samples")) {
           PenInput::Injected i{};
           vec2 p(s.at("x").get<float>(), s.at("y").get<float>());
           i.pos = window ? p : vec2(lastViewportOrigin.x, lastViewportOrigin.y) + p * lastViewportSize;
           i.pressure = s.value("pressure", 1.f);
           i.tilt = vec2(s.value("tilt_x", 0.f), s.value("tilt_y", 0.f));
           i.down = s.value("down", true);
           i.eraser = s.value("eraser", false);
           i.button = s.value("button", 0);
           i.mouse = s.value("mouse", false);
           v.push_back(i);
         }
         pen.inject(v);
         return json{{"queued", v.size()}};
       },
       false});
  engine.commands.add({"pen_injecting", "", {{"type", "object"}, {"properties", json::object()}},
                       [this](CommandContext&, const json&) { return json{{"busy", pen.injecting()}}; }, false});
}

}  // namespace patina
