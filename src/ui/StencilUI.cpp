// Stencil and text tool: the Properties section, the overlay in the 3D view and the
// hold-key interaction (move, rotate, scale).
#include <cmath>

#include <imgui.h>
#include <imgui_stdlib.h>

#include "ui/App.h"

namespace patina {

static ImTextureID texId(GLuint t) { return (ImTextureID)(intptr_t)t; }
static void helpMarker(const char* text) {
  ImGui::SameLine();
  ImGui::TextDisabled("(?)");
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text);
}
static bool colorEdit(const char* label, vec3& c) { return ImGui::ColorEdit3(label, &c.x, ImGuiColorEditFlags_NoInputs); }

void App::drawStencilSection() {
  Stencil& st = engine.painter.stencil;
  bool open = ImGui::CollapsingHeader(L_("Stencil & text"));
  {
    ImVec2 ra = ImGui::GetItemRectMin(), rb = ImGui::GetItemRectMax();
    engine.uiRects["props.stencil_header"] = {ra.x, ra.y, rb.x - ra.x, rb.y - ra.y};  // (the manual's screenshots)
  }
  if (!open) return;
  ImGui::PushID("stencil");
  std::string err;
  if (st.valid()) {
    float h = 56 * uiScale, w = std::min(h * st.aspect(), 160 * uiScale);
    ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetCursorScreenPos(),
                                              ImGui::GetCursorScreenPos() + ImVec2(w, w / st.aspect()),
                                              IM_COL32(60, 60, 60, 255));
    ImGui::Image(texId(st.tex.id()), ImVec2(w, w / st.aspect()), ImVec2(0, 1), ImVec2(1, 0));
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::Checkbox(L_("Use stencil"), &st.enabled);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(T_("Strokes paint through the stencil (%s)"), keyLabel("stencil.toggle"));
    ImGui::TextDisabled("%d x %d", st.image.width, st.image.height);
    if (ImGui::SmallButton(L_("Fit"))) st.fit();
    ImGui::SameLine();
    if (ImGui::SmallButton(L_("Remove"))) st.clear();
    ImGui::EndGroup();
  } else {
    ImGui::TextDisabled("%s", T_("No stencil: pick an image or make one from text"));
  }
  if (ImGui::Button(L_("Image..."))) openFileDialog(DialogPurpose::StencilImage);
  ImGui::SameLine();
  ImGui::BeginDisabled(!st.valid());
  if (ImGui::Button(L_("Save image..."))) openFileDialog(DialogPurpose::StencilSave);
  ImGui::EndDisabled();

  if (st.valid()) {
    ImGui::SetNextItemWidth(130 * uiScale);
    ImGui::Combo(L_("Mode##stmode"), &st.mode, Z_("Mask (shape)\0Decal (colors)\0"));
    helpMarker(T_("Mask: paint gets through where the stencil is white and opaque; the brush material is painted.\n"
                  "Decal: the stencil's own colors are painted (other channels from the brush material)."));
    ImGui::Checkbox(L_("Invert"), &st.invert);
    ImGui::SameLine();
    ImGui::Checkbox(L_("Tile"), &st.tile);
    ImGui::SameLine();
    ImGui::Checkbox(L_("Show"), &st.showOverlay);
    ImGui::SliderFloat(L_("Overlay"), &st.overlayOpacity, 0.f, 1.f, "%.2f");
    ImGui::DragFloat2(L_("Position"), &st.center.x, 0.002f, -1.f, 2.f, "%.3f");
    ImGui::SliderFloat(L_("Scale##st"), &st.scale, 0.02f, 5.f, "%.2f", ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat(L_("Rotation##st"), &st.rotationDeg, -180.f, 180.f, T_("%.1f deg"));
    ImGui::TextDisabled(T_("Hold %s in the view: drag moves, Shift+drag rotates, wheel scales"), keyLabel("stencil.edit"));
    if (ImGui::Button(L_("Project onto the model"))) {
      PaintTool keep = engine.painter.tool;
      engine.painter.tool = PaintTool::Paint;
      if (engine.painter.projectStencil(engine.doc, engine.materials, engine.brushes,
                                        engine.paintView(engine.camera, lastViewportSize)))
        setStatus(T_("Projected the stencil onto the visible surface (one undo step)"));
      else
        setStatus(T_("Nothing to project onto: select a paint layer with channels enabled"));
      engine.painter.tool = keep;
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", T_("Paints the whole stencil onto what the camera sees, in one step.\n"
                                 "Uses the brush's occlusion, backface and angle settings."));
    ImGui::SameLine();
    if (ImGui::Button(L_("Erase through it"))) {
      PaintTool keep = engine.painter.tool;
      engine.painter.tool = PaintTool::Erase;
      engine.painter.projectStencil(engine.doc, engine.materials, engine.brushes, engine.paintView(engine.camera, lastViewportSize));
      engine.painter.tool = keep;
    }
  }

  if (ImGui::TreeNode(L_("Text"))) {
    TextStyle& ts = stencilTextStyle;
    bool changed = ImGui::InputTextMultiline("##text", &stencilText, ImVec2(-1, ImGui::GetTextLineHeight() * 3.2f));
    auto fonts = availableFonts();
    std::string cur = ts.font;
    for (auto& f : fonts)
      if (f.name == ts.font || f.path == ts.font) cur = f.name;
    ImGui::SetNextItemWidth(170 * uiScale);
    if (ImGui::BeginCombo(L_("Font"), cur.c_str(), ImGuiComboFlags_HeightLarge)) {
      for (auto& f : fonts) {
        if (ImGui::Selectable((f.name + (f.bundled ? std::string("  ") + T_("(bundled)") : std::string()) + "##" + f.path).c_str(),
                              f.name == cur)) {
          ts.font = f.bundled ? f.name : f.path;
          changed = true;
        }
        if (ImGui::IsItemHovered() && !f.bundled) ImGui::SetTooltip("%s", f.path.c_str());
      }
      ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(L_("File..."))) openFileDialog(DialogPurpose::FontFile);
    changed |= ImGui::SliderFloat(L_("Size##text"), &ts.size, 16.f, 512.f, T_("%.0f px"), ImGuiSliderFlags_Logarithmic);
    changed |= colorEdit(L_("Color##text"), ts.color);
    changed |= ImGui::SliderFloat(L_("Outline"), &ts.outline, 0.f, 32.f, T_("%.1f px"));
    if (ts.outline > 0.f) changed |= colorEdit(L_("Outline color"), ts.outlineColor);
    changed |= ImGui::Combo(L_("Align"), &ts.align, Z_("Left\0Center\0Right\0"));
    changed |= ImGui::SliderFloat(L_("Letter spacing"), &ts.letterSpacing, -20.f, 60.f, T_("%.0f px"));
    changed |= ImGui::SliderFloat(L_("Line spacing"), &ts.lineSpacing, 0.6f, 2.5f, "%.2f");
    bool make = ImGui::Button(st.text.empty() ? L_("Make text stencil") : L_("Update text stencil"));
    ImGui::SameLine();
    ImGui::Checkbox(L_("Live"), &stencilTextLive);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T_("Update the stencil while you edit the text (when it is a text stencil)"));
    if (make || (changed && stencilTextLive && !st.text.empty())) {
      if (stencilText.empty()) {
        setStatus(T_("Type some text first"));
      } else if (st.setText(stencilText, ts, &err)) {
        if (make) {
          st.enabled = true;
          if (st.mode == Stencil::Mask && ts.color != vec3(1.f)) st.mode = Stencil::Decal;
        }
      } else {
        setStatus(F_("Text: %s", err.c_str()));
      }
    }
    ImGui::TextDisabled("%s", T_("Text becomes a stencil: place it, then paint or project it"));
    ImGui::TreePop();
  }
  ImGui::PopID();
}

bool App::stencilOverlay(ImVec2 origin, ImVec2 size, bool hovered) {
  Stencil& st = engine.painter.stencil;
  ImGuiIO& io = ImGui::GetIO();
  if (hovered && !io.WantTextInput && keyPressed("stencil.toggle") && st.valid()) {
    st.enabled = !st.enabled;
    setStatus(st.enabled ? T_("Stencil on") : T_("Stencil off"));
  }
  if (!st.valid() || (!st.enabled && !st.showOverlay)) return false;
  vec2 vp(size.x, size.y);
  static bool dragging = false;
  bool held = (hovered || dragging) && !io.WantTextInput && keyHeld("stencil.edit") && !engine.painter.active();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->PushClipRect(origin, origin + size, true);
  vec2 c[4];
  st.corners(vp, c);
  ImVec2 p[4];
  for (int i = 0; i < 4; ++i) p[i] = ImVec2(origin.x + c[i].x, origin.y + c[i].y);
  if (st.enabled && st.showOverlay) {
    int a = int(std::clamp(held ? std::max(st.overlayOpacity, 0.6f) : st.overlayOpacity, 0.f, 1.f) * 255.f);
    if (st.tile) {
      // a few copies around the center so the repeat is visible
      vec2 ax = c[1] - c[0], ay = c[3] - c[0];
      for (int j = -3; j <= 3; ++j)
        for (int i = -3; i <= 3; ++i) {
          vec2 o = ax * float(i) + ay * float(j);
          ImVec2 q[4];
          for (int k = 0; k < 4; ++k) q[k] = ImVec2(p[k].x + o.x, p[k].y + o.y);
          dl->AddImageQuad(texId(st.tex.id()), q[0], q[1], q[2], q[3], ImVec2(0, 1), ImVec2(1, 1), ImVec2(1, 0),
                           ImVec2(0, 0), IM_COL32(255, 255, 255, (i || j) ? a * 2 / 3 : a));
        }
    } else {
      dl->AddImageQuad(texId(st.tex.id()), p[0], p[1], p[2], p[3], ImVec2(0, 1), ImVec2(1, 1), ImVec2(1, 0),
                       ImVec2(0, 0), IM_COL32(255, 255, 255, a));
    }
  }
  if (held || !st.enabled) {
    ImU32 col = held ? IM_COL32(255, 200, 80, 230) : IM_COL32(200, 200, 200, 90);
    dl->AddPolyline(p, 4, col, ImDrawFlags_Closed, held ? 2.f : 1.f);
  }
  dl->PopClipRect();
  if (!held) {
    dragging = false;
    return false;
  }
  // move / rotate / scale
  vec2 mouse(io.MousePos.x - origin.x, io.MousePos.y - origin.y);
  vec2 ctr = st.center * vp;
  if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && (hovered || dragging)) {
    dragging = true;
    vec2 md(io.MouseDelta.x, io.MouseDelta.y);
    if (io.KeyShift) {
      vec2 a = mouse - md - ctr, b = mouse - ctr;
      if (glm::length(a) > 4.f && glm::length(b) > 4.f) {
        float d = std::atan2(b.y, b.x) - std::atan2(a.y, a.x);
        st.rotationDeg += glm::degrees(d);
        while (st.rotationDeg > 180.f) st.rotationDeg -= 360.f;
        while (st.rotationDeg < -180.f) st.rotationDeg += 360.f;
      }
    } else if (vp.x > 0 && vp.y > 0) {
      st.center += md / vp;
    }
  } else {
    dragging = false;
  }
  if (hovered && io.MouseWheel != 0.f) st.scale = std::clamp(st.scale * std::pow(1.08f, io.MouseWheel), 0.01f, 50.f);
  std::string tip = F_("Stencil  %.0f%%  %.0f deg", st.scale * 100.f, st.rotationDeg);
  ImGui::GetForegroundDrawList()->AddText(ImVec2(io.MousePos.x + 16, io.MousePos.y + 12), IM_COL32(255, 220, 140, 255), tip.c_str());
  return true;
}

}  // namespace patina
