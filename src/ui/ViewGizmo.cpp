// (0.37) The axis gizmo in the 3D view's top right corner, after Blender's navigation gizmo: the X, Y and Z
// axes turning with the camera (X red, Y green, Z blue, as on the mirror icons; the negative ends as plain
// dots). A click on an end looks along that axis (on the one already facing you: from the other side): 0.38 jumps
// there (a short turn with Preferences' animateViews) and remembers the view before, which the Back button under
// the gizmo (Numpad 0) goes back to; a drag orbits; the wheel zooms. Patina Web has the same one (web/src/gizmo.js).
#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>

#include "core/Log.h"
#include "ui/App.h"

namespace patina {

namespace {

struct End {
  int id;      // 0..5: +X -X +Y -Y +Z -Z
  ImVec2 at;   // on screen
  float depth;  // toward the viewer: + in front
};

// the camera angles of the view along each end (Camera::setView), and their names
struct ViewOf {
  float yaw, pitch;
  const char* name;
  const char* key;
};
const ViewOf kViews[6] = {{kPi * 0.5f, 0.f, N_("Right view"), "Numpad 3"}, {-kPi * 0.5f, 0.f, N_("Left view"), "Ctrl+Numpad 3"},
                          {0.f, 1.5699f, N_("Top view"), "Numpad 7"},      {0.f, -1.5699f, N_("Bottom view"), "Ctrl+Numpad 7"},
                          {0.f, 0.f, N_("Front view"), "Numpad 1"},        {kPi, 0.f, N_("Back view"), "Ctrl+Numpad 1"}};
const ImU32 kAxisColor[3] = {IM_COL32(240, 82, 95, 255), IM_COL32(125, 194, 66, 255), IM_COL32(74, 144, 240, 255)};
const char* kAxisName[6] = {"X", "-X", "Y", "-Y", "Z", "-Z"};

struct Layout {
  ImVec2 center{0, 0};
  float radius = 0, bubble = 0, reach = 0;
  End ends[6];
  bool fits = false;
};

Layout layoutOf(const Camera& cam, ImVec2 origin, ImVec2 size, float uiScale) {
  Layout L;
  L.radius = std::round(44.f * uiScale);
  L.bubble = std::round(9.f * uiScale);
  L.reach = L.radius - L.bubble - 2.f * uiScale;
  const float margin = std::round(10.f * uiScale);
  L.fits = size.x >= 4.f * L.radius && size.y >= 3.f * L.radius;
  L.center = ImVec2(origin.x + size.x - margin - L.radius, origin.y + margin + L.radius);
  const glm::mat3 rot(cam.view());
  for (int i = 0; i < 6; ++i) {
    vec3 axis(0.f);
    axis[i / 2] = i % 2 ? -1.f : 1.f;
    vec3 v = rot * axis;  // in view space: x right, y up, z toward the viewer
    L.ends[i] = {i, ImVec2(L.center.x + v.x * L.reach, L.center.y - v.y * L.reach), v.z};
  }
  return L;
}

float dist2(ImVec2 a, ImVec2 b) { return (a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y); }

// the end under the pointer: the one nearest the viewer when they overlap
int endAt(const Layout& L, ImVec2 p) {
  int best = -1;
  for (const End& e : L.ends)
    if (dist2(e.at, p) <= (L.bubble + 1.f) * (L.bubble + 1.f) && (best < 0 || e.depth > L.ends[best].depth)) best = e.id;
  return best;
}

// the end looking along which the camera is now (within half a degree), or -1
ImVec2 backButtonCenter(const Layout& L, float uiScale) {
  return ImVec2(L.center.x, L.center.y + L.radius + std::round(16.f * uiScale));
}

int facingEnd(const Camera& cam) {
  vec3 toEye = -cam.forward();
  for (int i = 0; i < 6; ++i) {
    vec3 axis(0.f);
    axis[i / 2] = i % 2 ? -1.f : 1.f;
    if (glm::dot(toEye, axis) > 0.99996f) return i;
  }
  return -1;
}

}  // namespace

bool App::axisGizmoInput(ImVec2 origin, ImVec2 size, bool hovered) {
  AxisGizmo& G = gizmo;
  Camera& cam = engine.camera;
  ImGuiIO& io = ImGui::GetIO();
  // the turn to a view, if one is under way (it stops when anything else moves the camera)
  if (G.turning) {
    if (std::abs(cam.yaw - G.setYaw) > 1e-5f || std::abs(cam.pitch - G.setPitch) > 1e-5f) {
      G.turning = false;
    } else {
      float t = std::clamp(float((nowSeconds() - G.turnStart) / 0.24), 0.f, 1.f);
      float s = t * t * (3.f - 2.f * t);
      cam.yaw = G.fromYaw + (G.toYaw - G.fromYaw) * s;
      cam.pitch = G.fromPitch + (G.toPitch - G.fromPitch) * s;
      G.setYaw = cam.yaw, G.setPitch = cam.pitch;
      if (t >= 1.f) G.turning = false, snapToView(G.toYaw, G.toPitch, &G.before);
    }
  }
  const int wasHot = G.hot;
  G.over = false;
  G.hot = -1;
  if (!engine.features.on("axis_gizmo") || !engine.viewOptions.axisGizmo) {
    G.pressed = false;
    return false;
  }
  Layout L = layoutOf(cam, origin, size, uiScale);
  if (!L.fits) {
    G.pressed = false;
    return false;
  }
  ImVec2 mouse = io.MousePos;
  bool inside = dist2(mouse, L.center) <= L.radius * L.radius;
  engine.uiRects["gizmo.view"] = {L.center.x - L.radius, L.center.y - L.radius, 2 * L.radius, 2 * L.radius};  // (for tests)
  for (const End& e : L.ends)
    engine.uiRects[std::string("gizmo.") + kAxisName[e.id]] = {e.at.x - L.bubble, e.at.y - L.bubble, 2 * L.bubble, 2 * L.bubble};
  Painter& P = engine.painter;
  // (0.38) the Back button under it, while a side view has a view to go back to
  G.backHot = false;
  if (gizmoBackShown()) {
    ImVec2 bc = backButtonCenter(L, uiScale);
    const float br = std::round(11.f * uiScale);
    engine.uiRects["gizmo.back"] = {bc.x - br, bc.y - br, 2 * br, 2 * br};
    if (!G.pressed && hovered && dist2(mouse, bc) <= br * br && !P.active() && !ImGui::IsMouseDown(ImGuiMouseButton_Middle) &&
        !ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
      G.backHot = true;
      if (G.backHot != G.wasBackHot) G.hotSince = nowSeconds();
      G.wasBackHot = true;
      if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) goBackView();
      return true;
    }
  }
  G.wasBackHot = false;
  if (!G.pressed && hovered && inside && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.KeyAlt && !P.active())
    G.pressed = true, G.dragged = false, G.pressAt = mouse;
  if (G.pressed) {
    G.over = true;
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      if (!G.dragged && dist2(mouse, G.pressAt) > 9.f) G.dragged = true;
      if (G.dragged) {
        cam.orbit(-io.MouseDelta.x * 0.006f, io.MouseDelta.y * 0.006f);
        G.turning = false;
      } else {
        G.hot = endAt(L, mouse);
      }
    } else {
      int e = G.dragged ? -1 : endAt(L, mouse);
      G.pressed = false;
      if (e >= 0) {
        // on the end already facing you: the other side
        if (facingEnd(cam) == e) e ^= 1;
        const ViewOf& v = kViews[e];
        G.fromYaw = cam.yaw, G.fromPitch = cam.pitch;
        // the shorter way round
        G.toYaw = v.yaw + std::round((cam.yaw - v.yaw) / (2.f * kPi)) * 2.f * kPi;
        G.toPitch = v.pitch;
        if (animateViews) {
          G.turnStart = nowSeconds();
          G.turning = true;
          G.setYaw = cam.yaw, G.setPitch = cam.pitch;
          engine.checkViewSnap();
          // (the view to go back to: this one, or from a side view the one that would go back to)
          G.before = engine.viewMemory.snapped && engine.viewMemory.hasBack ? engine.viewMemory.back : cam;
        } else {
          snapToView(G.toYaw, G.toPitch);
        }
      }
    }
    return true;
  }
  // over the gizmo with the buttons free: it takes the pointer (no brush circle, no strokes from here)
  const bool busy = P.active() || ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsMouseDown(ImGuiMouseButton_Middle) ||
                    ImGui::IsMouseDown(ImGuiMouseButton_Right);
  if (!(hovered && inside && !busy)) return false;
  G.over = true;
  G.hot = endAt(L, mouse);
  if (G.hot != wasHot) G.hotSince = nowSeconds();
  if (io.MouseWheel != 0.f && !io.KeyCtrl) cam.zoom(std::pow(0.88f, io.MouseWheel));
  return true;
}

void App::axisGizmoDraw(ImVec2 origin, ImVec2 size) {
  if (!engine.features.on("axis_gizmo") || !engine.viewOptions.axisGizmo) return;
  const AxisGizmo& G = gizmo;
  Layout L = layoutOf(engine.camera, origin, size, uiScale);
  if (!L.fits) return;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (G.over) dl->AddCircleFilled(L.center, L.radius, IM_COL32(255, 255, 255, G.pressed && G.dragged ? 40 : 26), 48);
  // back to front
  End order[6];
  std::copy(std::begin(L.ends), std::end(L.ends), order);
  std::sort(order, order + 6, [](const End& a, const End& b) { return a.depth < b.depth; });
  const float line = std::max(1.5f, 2.f * uiScale);
  ImFont* font = ImGui::GetFont();
  const float fs = std::round(ImGui::GetFontSize() * 0.92f);
  for (const End& e : order) {
    const int axis = e.id / 2;
    const bool positive = e.id % 2 == 0;
    // (the ends pointing away a little dimmer)
    const float shade = 0.72f + 0.28f * std::clamp(e.depth * 0.5f + 0.5f, 0.f, 1.f);
    ImVec4 c = ImGui::ColorConvertU32ToFloat4(kAxisColor[axis]);
    c.x *= shade, c.y *= shade, c.z *= shade;
    const ImU32 col = ImGui::ColorConvertFloat4ToU32(c);
    const bool hot = G.hot == e.id;
    if (positive) {
      // the axis itself, from the middle to the end's dot
      ImVec2 d(e.at.x - L.center.x, e.at.y - L.center.y);
      float len = std::sqrt(d.x * d.x + d.y * d.y);
      if (len > L.bubble) {
        ImVec2 tip(e.at.x - d.x / len * L.bubble, e.at.y - d.y / len * L.bubble);
        dl->AddLine(L.center, tip, IM_COL32(0, 0, 0, 70), line + 2.f);
        dl->AddLine(L.center, tip, col, line);
      }
      dl->AddCircleFilled(e.at, L.bubble + 1.f, IM_COL32(0, 0, 0, 90), 24);
      dl->AddCircleFilled(e.at, L.bubble, col, 24);
      const char* t = kAxisName[e.id];
      ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0.f, t);
      dl->AddText(font, fs, ImVec2(std::round(e.at.x - ts.x * 0.5f), std::round(e.at.y - ts.y * 0.5f)), IM_COL32(22, 22, 24, 255), t);
    } else {
      // the negative end: a darker dot with a ring, its name when pointed at
      dl->AddCircleFilled(e.at, L.bubble + 1.f, IM_COL32(0, 0, 0, 80), 24);
      c.w = 0.42f;
      dl->AddCircleFilled(e.at, L.bubble, ImGui::ColorConvertFloat4ToU32(c), 24);
      dl->AddCircle(e.at, L.bubble - 0.5f, col, 24, std::max(1.f, 1.25f * uiScale));
      if (hot) {
        const char* t = kAxisName[e.id];
        ImVec2 ts = font->CalcTextSizeA(fs * 0.9f, FLT_MAX, 0.f, t);
        dl->AddText(font, fs * 0.9f, ImVec2(std::round(e.at.x - ts.x * 0.5f), std::round(e.at.y - ts.y * 0.5f)), IM_COL32(255, 255, 255, 255), t);
      }
    }
    if (hot) dl->AddCircle(e.at, L.bubble + 0.5f, IM_COL32(255, 255, 255, 230), 24, std::max(1.f, 1.5f * uiScale));
  }
  // (0.38) the Back button
  if (gizmoBackShown()) {
    ImVec2 bc = backButtonCenter(L, uiScale);
    const float br = std::round(11.f * uiScale);
    dl->AddCircleFilled(bc, br, G.backHot ? IM_COL32(255, 255, 255, 60) : IM_COL32(20, 20, 22, 150), 24);
    dl->AddCircle(bc, br - 0.5f, IM_COL32(255, 255, 255, G.backHot ? 160 : 70), 24, std::max(1.f, uiScale));
    const float isz = std::round(br * 1.25f);
    ui::drawIcon(dl, "view_back", ImVec2(std::round(bc.x - isz * 0.5f), std::round(bc.y - isz * 0.5f)), isz,
                 ui::iconColors(ui::IconState::Normal));
    if (G.backHot && nowSeconds() - G.hotSince > 0.45) {
      const char* k = keyLabel("view.back");
      ImGui::SetTooltip("%s%s%s%s\n%s", T_("Back to your view"), k ? "  (" : "", k ? k : "", k ? ")" : "",
                        T_("The view before this side view (again: back here)"));
    }
  }
  // the end under the pointer: where a click goes, after a moment
  if (G.hot >= 0 && !G.dragged && nowSeconds() - G.hotSince > 0.45) {
    int e = facingEnd(engine.camera) == G.hot ? G.hot ^ 1 : G.hot;
    ImGui::SetTooltip("%s  (%s)\n%s", T_(kViews[e].name), kViews[e].key, T_("Click an axis to look along it, drag to orbit"));
  }
}

}  // namespace patina
