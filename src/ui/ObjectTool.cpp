// (1.2) The Object tool in the 3D view: a click picks the object to paint; the gizmo moves it (arrows, and the
// middle square in the view's plane), turns it (rings, and the outer ring about the line of sight) or scales it (the
// handles along its own axes, the middle square evenly). Ctrl snaps. One undo step in the object's history per drag.
#include <imgui.h>

#include <glm/gtx/euler_angles.hpp>

#include "ui/App.h"

namespace patina {

namespace {
const ImU32 kAxisCol[3] = {IM_COL32(240, 82, 95, 255), IM_COL32(125, 194, 66, 255), IM_COL32(74, 144, 240, 255)};
const ImU32 kHot = IM_COL32(255, 214, 90, 255);

float segDist(ImVec2 p, ImVec2 a, ImVec2 b) {
  ImVec2 ab(b.x - a.x, b.y - a.y), ap(p.x - a.x, p.y - a.y);
  float l2 = ab.x * ab.x + ab.y * ab.y;
  float t = l2 > 1e-6f ? std::clamp((ap.x * ab.x + ap.y * ab.y) / l2, 0.f, 1.f) : 0.f;
  float dx = a.x + ab.x * t - p.x, dy = a.y + ab.y * t - p.y;
  return std::sqrt(dx * dx + dy * dy);
}
}  // namespace

void App::objectToolUI(ImVec2 origin, ImVec2 size, bool hovered, vec2 local) {
  Document& d = engine.doc;
  if (!d.hasMesh()) return;
  ImGuiIO& io = ImGui::GetIO();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const Camera& cam = engine.camera;
  const float aspect = size.x / std::max(size.y, 1.f);
  const mat4 vp = cam.viewProj(aspect);
  auto toScreen = [&](const vec3& w, ImVec2& out) {
    vec4 c = vp * vec4(w, 1.f);
    if (c.w <= 1e-6f) return false;
    out = ImVec2(origin.x + (c.x / c.w * 0.5f + 0.5f) * size.x, origin.y + (0.5f - c.y / c.w * 0.5f) * size.y);
    return true;
  };
  const Document::Placement& P = d.placement;
  const vec3 center = P.location + P.pivot;
  ImVec2 pc;
  if (!toScreen(center, pc)) return;
  // the gizmo's size: about 85 pixels whatever the distance
  const float L = 85.f * uiScale;
  float depth = cam.ortho ? 1.f : glm::length(center - cam.eye());
  float worldLen = L * cam.pixelWorld(size.y) * depth;
  const mat3 R = mat3(glm::eulerAngleZYX(glm::radians(P.rotation.z), glm::radians(P.rotation.y), glm::radians(P.rotation.x)));
  // the axes: the world's for moving and turning, the object's own for scaling (it scales along them)
  vec3 axis[3];
  for (int k = 0; k < 3; ++k) axis[k] = gizmoMode == 2 ? glm::normalize(R[k]) : vec3(k == 0, k == 1, k == 2);
  ImVec2 tip[3];
  bool shown[3];
  for (int k = 0; k < 3; ++k) {
    shown[k] = toScreen(center + axis[k] * worldLen, tip[k]);
    float sl = std::hypot(tip[k].x - pc.x, tip[k].y - pc.y);
    shown[k] = shown[k] && sl > 0.18f * L;  // (an axis pointing at the camera can't be dragged along)
  }
  // the rings (turn): each axis's circle, and the outer one about the line of sight
  auto ring = [&](const vec3& n, float r, std::vector<ImVec2>& pts) {
    vec3 a = std::abs(n.y) < 0.9f ? glm::normalize(glm::cross(n, vec3(0, 1, 0))) : glm::normalize(glm::cross(n, vec3(1, 0, 0)));
    vec3 b = glm::cross(n, a);
    pts.clear();
    for (int i = 0; i <= 64; ++i) {
      float t = 2.f * kPi * float(i) / 64.f;
      ImVec2 s;
      if (toScreen(center + (a * std::cos(t) + b * std::sin(t)) * r, s)) pts.push_back(s);
    }
  };
  std::vector<ImVec2> rings[4];
  if (gizmoMode == 1) {
    for (int k = 0; k < 3; ++k) ring(axis[k], worldLen * 0.9f, rings[k]);
    ring(-cam.forward(), worldLen * 1.12f, rings[3]);
  }
  // ---- which handle is under the pointer
  ImVec2 m = io.MousePos;
  const float grab = 7.f * uiScale;
  int hot = -1;
  if (gizmoDrag < 0 && hovered) {
    float best = grab;
    if (std::abs(m.x - pc.x) < grab * 1.3f && std::abs(m.y - pc.y) < grab * 1.3f && gizmoMode != 1) hot = 3, best = 0;
    for (int k = 0; k < 3 && hot != 3; ++k) {
      if (gizmoMode == 1) {
        for (size_t i = 1; i < rings[k].size(); ++i) {
          float dd = segDist(m, rings[k][i - 1], rings[k][i]);
          if (dd < best) best = dd, hot = k;
        }
      } else if (shown[k]) {
        float dd = segDist(m, ImVec2(pc.x + (tip[k].x - pc.x) * 0.2f, pc.y + (tip[k].y - pc.y) * 0.2f), tip[k]);
        if (dd < best) best = dd, hot = k;
      }
    }
    if (gizmoMode == 1 && hot < 0)
      for (size_t i = 1; i < rings[3].size(); ++i)
        if (segDist(m, rings[3][i - 1], rings[3][i]) < best) best = segDist(m, rings[3][i - 1], rings[3][i]), hot = 3;
  }
  // ---- a drag
  const bool ctrl = io.KeyCtrl;
  if (gizmoDrag < 0 && hot >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.KeyAlt) {
    gizmoDrag = hot;
    gizmoStart = d.placement;
    gizmoMouse0 = m;
    gizmoCenter0 = pc;
  } else if (gizmoDrag < 0 && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.KeyAlt) {
    // a click on an object makes it the one painted
    Engine::ScenePick sp = engine.pickScene(local);
    if (sp.object >= 0 && sp.object != engine.activeObject()) selectObjectUI(sp.object);
  }
  if (gizmoDrag >= 0) {
    Document::Placement p = gizmoStart;
    ImVec2 dm(m.x - gizmoMouse0.x, m.y - gizmoMouse0.y);
    const int k = gizmoDrag;
    float objSize = d.mesh.bounds.valid() ? glm::length(d.mesh.bounds.size()) * std::max({P.scale.x, P.scale.y, P.scale.z}) : 1.f;
    float step = std::pow(10.f, std::floor(std::log10(std::max(objSize, 1e-6f)))) * 0.1f;  // (Ctrl: tenths of its size's order)
    if (gizmoMode == 0) {
      if (k == 3) {
        // in the view's plane
        mat4 v = cam.view();
        vec3 right(v[0][0], v[1][0], v[2][0]), up(v[0][1], v[1][1], v[2][1]);
        float s = worldLen / L;
        vec3 dw = right * dm.x * s - up * dm.y * s;
        if (ctrl) dw = glm::round(dw / step) * step;
        p.location = gizmoStart.location + dw;
      } else {
        ImVec2 ax(tip[k].x - pc.x, tip[k].y - pc.y);
        float al = std::max(std::hypot(ax.x, ax.y), 1e-3f);
        float along = (dm.x * ax.x + dm.y * ax.y) / al;  // pixels along the axis on screen
        float dw = along / al * worldLen;
        if (ctrl) dw = std::round(dw / step) * step;
        p.location = gizmoStart.location + axis[k] * dw;
      }
    } else if (gizmoMode == 1) {
      float a0 = std::atan2(gizmoMouse0.y - gizmoCenter0.y, gizmoMouse0.x - gizmoCenter0.x);
      float a1 = std::atan2(m.y - gizmoCenter0.y, m.x - gizmoCenter0.x);
      float ang = a1 - a0;  // (screen y points down: a positive angle turns clockwise on screen)
      vec3 n = k == 3 ? -cam.forward() : axis[k];
      if (glm::dot(n, cam.forward()) < 0.f) ang = -ang;  // the axis toward the viewer: clockwise on screen is negative
      float deg = glm::degrees(ang);
      if (ctrl) deg = std::round(deg / 15.f) * 15.f;
      mat4 R0 = glm::eulerAngleZYX(glm::radians(gizmoStart.rotation.z), glm::radians(gizmoStart.rotation.y),
                                   glm::radians(gizmoStart.rotation.x));
      mat4 R1 = glm::rotate(mat4(1), glm::radians(deg), n) * R0;
      float z, y, x;
      glm::extractEulerAngleZYX(R1, z, y, x);
      p.rotation = glm::degrees(vec3(x, y, z));
      gizmoAngle = deg;
    } else {
      float f;
      if (k == 3) {
        float r0 = std::max(std::hypot(gizmoMouse0.x - gizmoCenter0.x, gizmoMouse0.y - gizmoCenter0.y), 4.f);
        f = std::hypot(m.x - gizmoCenter0.x, m.y - gizmoCenter0.y) / r0;
      } else {
        ImVec2 ax(tip[k].x - pc.x, tip[k].y - pc.y);
        float al = std::max(std::hypot(ax.x, ax.y), 1e-3f);
        f = 1.f + (dm.x * ax.x + dm.y * ax.y) / (al * al);
      }
      if (ctrl) f = std::round(f * 10.f) / 10.f;
      f = std::max(f, 0.01f);
      if (k == 3) p.scale = gizmoStart.scale * f;
      else p.scale[k] = gizmoStart.scale[k] * f;
    }
    d.placement = p;  // (live while dragging)
    d.dirty = true;
    bool cancel = ImGui::IsMouseClicked(ImGuiMouseButton_Right) || ImGui::IsKeyPressed(ImGuiKey_Escape);
    if (cancel) {
      d.placement = gizmoStart;
      gizmoDrag = -1;
      setStatus(T_("Cancelled"));
    } else if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      Document::Placement now = d.placement;
      d.placement = gizmoStart;
      d.setPlacement(now, gizmoMode == 0 ? T_("Move the object") : gizmoMode == 1 ? T_("Turn the object") : T_("Scale the object"));
      gizmoDrag = -1;
    }
  }
  // ---- drawing (the handle dragged or under the pointer in yellow)
  const int lit = gizmoDrag >= 0 ? gizmoDrag : hot;
  const float th = 2.5f * uiScale;
  // the object's box, so it is clear which one it is
  if (d.mesh.bounds.valid()) {
    mat4 w = d.toWorld();
    const AABB& b = d.mesh.bounds;
    ImVec2 c[8];
    bool ok = true;
    for (int i = 0; i < 8 && ok; ++i)
      ok = toScreen(vec3(w * vec4((i & 1) ? b.hi.x : b.lo.x, (i & 2) ? b.hi.y : b.lo.y, (i & 4) ? b.hi.z : b.lo.z, 1.f)), c[i]);
    if (ok) {
      static const int e[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
      for (auto& s : e) dl->AddLine(c[s[0]], c[s[1]], IM_COL32(232, 133, 44, 150), 1.f);
    }
  }
  if (gizmoMode == 1) {
    dl->AddPolyline(rings[3].data(), (int)rings[3].size(), lit == 3 ? kHot : IM_COL32(230, 230, 230, 170), 0, th * 0.8f);
    for (int k = 0; k < 3; ++k) dl->AddPolyline(rings[k].data(), (int)rings[k].size(), lit == k ? kHot : kAxisCol[k], 0, th);
    if (gizmoDrag >= 0) {
      std::string a = F_("%.0f°", gizmoAngle);
      dl->AddText(ImVec2(m.x + 14 * uiScale, m.y - 18 * uiScale), IM_COL32(255, 255, 255, 230), a.c_str());
    }
  } else {
    for (int k = 0; k < 3; ++k) {
      if (!shown[k]) continue;
      ImU32 col = lit == k ? kHot : kAxisCol[k];
      ImVec2 a(pc.x + (tip[k].x - pc.x) * 0.2f, pc.y + (tip[k].y - pc.y) * 0.2f);
      dl->AddLine(a, tip[k], col, th);
      if (gizmoMode == 0) {
        // an arrowhead
        float dx = tip[k].x - pc.x, dy = tip[k].y - pc.y, l = std::max(std::hypot(dx, dy), 1e-3f);
        dx /= l, dy /= l;
        float h = 11.f * uiScale, w = 5.f * uiScale;
        ImVec2 t2(tip[k].x + dx * h, tip[k].y + dy * h);
        dl->AddTriangleFilled(t2, ImVec2(tip[k].x - dy * w, tip[k].y + dx * w), ImVec2(tip[k].x + dy * w, tip[k].y - dx * w), col);
      } else {
        float s = 5.f * uiScale;
        dl->AddRectFilled(ImVec2(tip[k].x - s, tip[k].y - s), ImVec2(tip[k].x + s, tip[k].y + s), col);
      }
    }
    float s = 6.f * uiScale;
    dl->AddRect(ImVec2(pc.x - s, pc.y - s), ImVec2(pc.x + s, pc.y + s), lit == 3 ? kHot : IM_COL32(240, 240, 240, 220), 1.f, 0, th * 0.8f);
  }
  engine.uiRects["gizmo.center"] = {pc.x - 6.f, pc.y - 6.f, 12.f, 12.f};
  for (int k = 0; k < 3; ++k) {
    ImVec2 q = gizmoMode == 1 && !rings[k].empty() ? rings[k][rings[k].size() / 8] : tip[k];
    engine.uiRects[std::string("gizmo.") + "xyz"[k]] = {q.x - 4.f, q.y - 4.f, 8.f, 8.f};
  }
  if (gizmoMode == 1 && !rings[3].empty()) {
    ImVec2 q = rings[3][rings[3].size() / 3];
    engine.uiRects["gizmo.view"] = {q.x - 4.f, q.y - 4.f, 8.f, 8.f};
  }
  if (hot >= 0 || gizmoDrag >= 0) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
}

}  // namespace patina
