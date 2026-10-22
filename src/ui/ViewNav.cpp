// (0.38) Side, top and bottom views, as in Blender: the numpad, the axis gizmo and Alt while orbiting jump to them
// (orthographic, unless Preferences say otherwise, until you turn away), and Numpad 0 goes back to the view you had
// before (and a second time to the side view again). The memory is the engine's (Engine::viewMemory), so
// set_camera's presets and view=previous share it.
#include <imgui.h>

#include <algorithm>
#include <cmath>

#include "ui/App.h"

namespace patina {

bool App::viewSnapping() const { return engine.features.on("view_snapping"); }

void App::snapToView(float yaw, float pitch, const Camera* before) {
  if (!viewSnapping()) {
    engine.camera.yaw = yaw, engine.camera.pitch = pitch;  // (as before 0.38)
    return;
  }
  engine.snapView(yaw, pitch, autoOrtho, before);
}

bool App::gizmoBackShown() const {
  return viewSnapping() && engine.viewMemory.snapped && engine.viewMemory.hasBack;
}

void App::goBackView() {
  if (!viewSnapping()) return;
  gizmo.turning = false;
  if (!engine.viewBack()) setStatus(T_("No view to go back to: go to a side, top or bottom view first (numpad, the axis gizmo)"));
}

bool App::orbitDragInput(bool orbitNow, ImVec2 md) {
  OrbitDrag& D = orbitDrag;
  if (!viewSnapping()) {
    D.on = false;
    return false;
  }
  if (D.on && !ImGui::IsMouseDown(D.button)) D.on = false;
  if (!D.on) {
    if (!orbitNow) return false;
    D.on = true;
    D.button = ImGui::IsMouseDown(ImGuiMouseButton_Middle) ? ImGuiMouseButton_Middle
               : ImGui::IsMouseDown(ImGuiMouseButton_Left) ? ImGuiMouseButton_Left
                                                            : ImGuiMouseButton_Right;
    engine.checkViewSnap();
    D.start = engine.camera;
    D.yaw = D.start.yaw, D.pitch = D.start.pitch;
    D.startSnapped = engine.viewMemory.snapped;
    D.snapping = false;
  }
  Camera& cam = engine.camera;
  const ImGuiIO& io = ImGui::GetIO();
  D.yaw += -md.x * 0.006f;
  D.pitch = std::clamp(D.pitch + md.y * 0.006f, -1.55f, 1.55f);
  // Alt snaps a middle-button orbit (Blender's key); an orbit Alt itself started (Alt+left) or the right button's
  // (ZBrush) snaps with Shift
  const bool snapKey = D.button == ImGuiMouseButton_Middle ? io.KeyAlt : io.KeyShift;
  if (snapKey) {
    // the nearest side view: yaw in quarter turns, the top or bottom past 45 degrees (their yaw turns them)
    const float q = kPi * 0.5f;
    float sy = std::round(D.yaw / q) * q;
    float sp = std::abs(D.pitch) > kPi * 0.25f ? std::copysign(1.5699f, D.pitch) : 0.f;
    if (!D.snapping || sy != cam.yaw || sp != cam.pitch) {
      // the view to go back to: where the drag began, or (from a side view) the one that view would go back to
      Camera keep = D.startSnapped && engine.viewMemory.hasBack ? engine.viewMemory.back : D.start;
      snapToView(sy, sp, &keep);
    }
    D.snapping = true;
  } else {
    if (D.snapping) D.yaw = cam.yaw, D.pitch = cam.pitch, D.snapping = false;  // carry on from the side view
    cam.yaw = D.yaw, cam.pitch = D.pitch;
  }
  return true;
}

}  // namespace patina
