// 3D viewport: camera navigation, painting, polygon fill, picking, face
// selection and seam marking directly on the model.
#include <imgui.h>
#include <imgui_internal.h>

#include <cstring>

#include "core/Log.h"
#include "core/UVIsland.h"
#include "engine/SmartMaterial.h"
#include "ui/App.h"
#include "ui/Theme.h"

namespace patina {

// (the English names: T_() / L_() them where they are shown)
static const char* kViewModeLabels[] = {N_("Material"), N_("Base Color"), N_("Roughness"), N_("Metallic"), N_("Normal"),
                                        N_("Height"), N_("Emissive"), N_("AO (baked)"), N_("Curvature (baked)"), N_("UV Checker"),
                                        N_("Layer Mask"), N_("World Normal"), N_("UV Islands"), N_("Texel Density"),
                                        N_("UV Stretch"), N_("Material Slots"), N_("Thickness (baked)"), N_("Bent Normals (baked)"), N_("Symmetry"),
                                        N_("Opacity"), N_("Sheen"), N_("Clearcoat"), N_("Subsurface"), N_("Mesh Check"), N_("PBR Range"),
                                        N_("Solid"), N_("Path traced"), N_("Bake-Down Check"), N_("Face Sets")};
static_assert(IM_ARRAYSIZE(kViewModeLabels) == (int)ViewMode::Count);
// set_tool's names for the window's tools (and get_state's window_tool)
static const std::pair<const char*, ViewportTool> kTools[] = {
    {"paint", ViewportTool::Paint},       {"erase", ViewportTool::Erase},   {"blur", ViewportTool::Blur},
    {"clone", ViewportTool::Clone},       {"smudge", ViewportTool::Smudge}, {"fill", ViewportTool::Fill},
    {"picker", ViewportTool::Picker},     {"select", ViewportTool::SelectFaces}, {"seams", ViewportTool::Seams},
    {"island", ViewportTool::Island},     {"note", ViewportTool::Note},     {"zone_lines", ViewportTool::ZoneLine},
    {"zones", ViewportTool::Patch},       {"flow", ViewportTool::Flow},     {"density", ViewportTool::Density},
    {"object", ViewportTool::Object}};

const char* viewModeLabel(int m) { return kViewModeLabels[std::clamp(m, 0, (int)ViewMode::Count - 1)]; }

bool App::viewModeAvailable(ViewMode m) const {
  if (m == ViewMode::Symmetry) return engine.features.on("symmetry_check");
  if (m >= ViewMode::Opacity && m <= ViewMode::Subsurface) return engine.features.on("more_channels");
  if (m == ViewMode::MeshCheck) return engine.features.on("mesh_check");
  if (m == ViewMode::PbrRange) return engine.features.on("texture_develop");
  if (m == ViewMode::Solid) return engine.features.on("solid_view");
  if (m == ViewMode::PathTraced) return engine.features.on("path_tracing");
  if (m == ViewMode::BakeCheck) return engine.features.on("bake_down") && engine.doc.bakeDownCurrent();
  if (m == ViewMode::FaceSets) return engine.features.on("face_sets");
  return true;
}

// nearest edge of `face` to a point (object space)
static int nearestEdge(const Mesh& m, uint32_t face, const vec3& p) {
  int best = -1;
  float bestD = 1e30f;
  for (uint32_t l = m.faceStart[face]; l < m.faceStart[face + 1]; ++l) {
    vec3 a = m.positions[m.loopVert[l]], b = m.positions[m.loopVert[m.nextLoop(l, face)]];
    vec3 ab = b - a;
    float t = std::clamp(glm::dot(p - a, ab) / std::max(glm::dot(ab, ab), 1e-20f), 0.f, 1.f);
    float d = glm::length(a + ab * t - p);
    if (d < bestD) bestD = d, best = (int)m.loopEdge[l];
  }
  return best;
}

std::vector<uint32_t> App::selectionFacesAt(uint32_t face) {
  Document& d = engine.doc;
  if (uvSelectMode != UVSelectMode::Island) return {face};  // (the face under the cursor is the one seen)
  bool visible = selectVisible && engine.features.on("select_visible");
  if (!visible) return engine.painter.facesForFill(d, face, FillMode::UVIsland);
  // the island seen from this view: worked out once per island and view (a drag asks every frame)
  float aspect = engine.viewportSize.x / std::max(engine.viewportSize.y, 1.f);
  mat4 vp = engine.camera.viewProj(aspect);
  uint64_t key = 1469598103934665603ull;
  auto mix = [&](const void* p, size_t n) {
    for (size_t i = 0; i < n; ++i) key = (key ^ static_cast<const unsigned char*>(p)[i]) * 1099511628211ull;
  };
  mix(&vp, sizeof vp);
  mix(&engine.viewportSize, sizeof engine.viewportSize);
  uint32_t island = d.mesh.loopIsland[d.mesh.faceStart[face]];
  mix(&island, sizeof island);
  uint64_t uvh = uv::uvHash(d.mesh);
  mix(&uvh, sizeof uvh);
  if (key != visibleCacheKey) {
    visibleCacheKey = key;
    visibleCache = engine.visibleFaces(engine.painter.facesForFill(d, face, FillMode::UVIsland));
    if (std::find(visibleCache.begin(), visibleCache.end(), face) == visibleCache.end()) visibleCache.push_back(face);
  }
  return visibleCache;
}

void App::applyFaceSelectionClick(uint32_t face, bool add, bool remove) {
  Document& d = engine.doc;
  d.faceSelection.resize(d.mesh.faceCount(), 0);
  std::vector<uint32_t> faces = selectionFacesAt(face);
  if (!add && !remove) std::fill(d.faceSelection.begin(), d.faceSelection.end(), 0);
  for (uint32_t f : faces) d.faceSelection[f] = remove ? 0 : 1;
  d.updateSelectionFlags();
}

void App::drawViewport(bool fullscreen) {
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  if (fullscreen) {
    // distraction-free: the viewport alone fills the window (Tab brings the panels back)
    ImGuiViewport* mv = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(mv->WorkPos);
    ImGui::SetNextWindowSize(ImVec2(mv->WorkSize.x, mv->WorkSize.y - ImGui::GetFrameHeight()));
    ImGui::Begin("##FocusViewport", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
  } else {
    beginPanel(L_("Viewport"), ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  }
  ImGui::PopStyleVar();
  ImGuiIO& io = ImGui::GetIO();
  Painter& P = engine.painter;
  Camera& cam = engine.camera;

  // follow tool changes made through commands (set_tool from MCP / console)
  if (engine.toolRequest != lastToolRequest && !P.active()) {
    lastToolRequest = engine.toolRequest;
    for (auto& [name, t] : kTools)
      if (engine.requestedTool == name) selectTool(t);
    lastPainterTool = P.tool;
  } else if (P.tool != lastPainterTool && !P.active()) {
    switch (P.tool) {
      case PaintTool::Paint: tool = ViewportTool::Paint; break;
      case PaintTool::Erase: tool = ViewportTool::Erase; break;
      case PaintTool::Blur: tool = ViewportTool::Blur; break;
      case PaintTool::Clone: tool = ViewportTool::Clone; break;
      case PaintTool::Smudge: tool = ViewportTool::Smudge; break;
      case PaintTool::Fill: tool = ViewportTool::Fill; break;
      case PaintTool::Picker: tool = ViewportTool::Picker; break;
    }
  }

  // ---- the bar over the view (Toolbars.cpp)
  viewportBar();

  // ---- render
  ImVec2 avail = ImGui::GetContentRegionAvail();
  avail.x = std::max(avail.x, 32.f);
  avail.y = std::max(avail.y, 32.f);
  ImVec2 origin = ImGui::GetCursorScreenPos();
  lastViewportOrigin = origin;
  engine.uiRects["viewport.image"] = {origin.x, origin.y, avail.x, avail.y};  // (for tests and the manual's screenshots)
  lastViewportSize = vec2(avail.x, avail.y);
  engine.viewportSize = lastViewportSize;
  float fbs = io.DisplayFramebufferScale.x > 0 ? io.DisplayFramebufferScale.x : 1.f;
  int pw = std::max(16, int(avail.x * fbs)), ph = std::max(16, int(avail.y * fbs));
  engine.updateClip();

  ImVec2 mouse = io.MousePos;
  vec2 local(mouse.x - origin.x, mouse.y - origin.y);
  bool inside = local.x >= 0 && local.y >= 0 && local.x < avail.x && local.y < avail.y;

  // hover face highlight for face-based tools
  engine.viewOptions.hoverFace = 0xFFFFFFFFu;
  bool faceTool = !viewOnly && (tool == ViewportTool::Fill || tool == ViewportTool::SelectFaces);
  RayHit hoverHit;
  // (the seams brush keeps picking while its drag holds the mouse)
  bool edgeTool = tool == ViewportTool::Seams;
  if (inside && (ImGui::IsWindowHovered() || (edgeTool && seamStroke.active &&
                                              ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))) &&
      !viewOnly && (faceTool || edgeTool || tool == ViewportTool::Patch)) {
    hoverHit = engine.pick(local);
    if (hoverHit.hit && faceTool) engine.viewOptions.hoverFace = hoverHit.face;
  }
  Layer* sel = engine.doc.selectedLayer();
  engine.checkViewSnap();  // (0.38) turned away from a side view: perspective again
  for (auto& [name, t] : kTools)
    if (t == tool) engine.windowTool = name;
  ViewportOptions vo = engine.viewOptions;
  vo.showPins = engine.features.on("viewport_pins") && (tool == ViewportTool::SelectFaces || tool == ViewportTool::Island);
  if (tool == ViewportTool::Patch || tool == ViewportTool::ZoneLine || tool == ViewportTool::Flow) vo.retopoView = 1;
  else if (tool == ViewportTool::Density) vo.retopoView = 2;
  else if (!retopoPreviewOn()) vo.retopoView = 0, vo.retopoShowQuads = false;
  engine.retopoViewOptions(vo);
  engine.collisionViewOptions(vo);
  // (0.22.2) while a line is drawn the model doesn't change: the last picture is shown again unless the view
  // or what it shows did (the line itself is drawn over it each frame)
  uint64_t viewKey = 1469598103934665603ull;
  {
    auto mixIn = [&](uint64_t w) { viewKey = (viewKey ^ w) * 1099511628211ull; };
    mat4 vpm = cam.viewProj(float(pw) / float(ph));
    for (int c = 0; c < 4; ++c)
      for (int r = 0; r < 4; ++r) {
        uint32_t b;
        std::memcpy(&b, &vpm[c][r], 4);
        mixIn(b);
      }
    mixIn(uint64_t(pw) << 32 | uint32_t(ph)), mixIn(vo.retopoKey), mixIn(vo.retopoLinesKey), mixIn(vo.retopoQuadsKey);
    mixIn(vo.retopoLoopsKey), mixIn(uint64_t(vo.mode)), mixIn(uint64_t(vo.retopoView)), mixIn(vo.retopoShowQuads | vo.retopoQuadsOnly << 1 | vo.retopoShowMarks << 2);
    mixIn(vo.hoverFace), mixIn(uint64_t(vo.retopoLinesBehind)), mixIn(uint64_t(engine.doc.undo.count()) << 32 | engine.doc.undo.position());
  }
  if (!(engine.drawingStroke && viewKey == lastViewKey && viewportRT.color.id())) {
    frameStats.gpuMark(1);
    engine.renderer.renderViewport(engine.doc, engine.env, cam, viewportRT, pw, ph, vo,
                                   engine.viewOptions.mode == ViewMode::LayerMask ? engine.maskViewTexture()
                                                                                  : (sel && sel->mask ? sel->mask->id() : 0));
    frameStats.gpuMark(2);
    lastViewKey = viewKey;
  }
  ImGui::Image((ImTextureID)(intptr_t)viewportRT.color.id(), avail, ImVec2(0, 1), ImVec2(1, 0));
  ImGui::SetCursorScreenPos(origin);
  ImGui::InvisibleButton("##viewport_input", avail,
                         ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
  bool hovered = ImGui::IsItemHovered();
  bool active = ImGui::IsItemActive();
  // (0.22.5) a material (from the library or a fill layer) or a smart material dropped on the model: added
  // above the selected layer
  if (ImGui::BeginDragDropTarget()) {
    Document& d = engine.doc;
    int target = -1;
    bool into = false;
    if (const Layer* sel = d.selectedLayer()) target = d.selected, into = sel->isFolder();
    else
      for (int k = (int)d.layers.size() - 1; k >= 0 && target < 0; --k)
        if (!d.effectiveParent(*d.layers[k])) target = k;
    for (const char* type : {"PATINA_MATERIAL", "PATINA_FILL"})
      if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(type, ImGuiDragDropFlags_AcceptBeforeDelivery)) {
        dropHint = T_("A new fill layer above the selected layer");
        if (p->IsDelivery()) {
          bool lib = type[7] == 'M';
          MaterialRef m;
          if (lib) m.id = (const char*)p->Data;
          else if (const Layer* src = d.findLayer(*(const uint32_t*)p->Data)) m = d.contentLayer(*src)->material;
          dropMaterial(m, lib, target, false, into, false);
        }
      }
    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("PATINA_SMART", ImGuiDragDropFlags_AcceptBeforeDelivery)) {
      dropHint = T_("Added above the selected layer");
      if (p->IsDelivery()) {
        std::string err;
        if (applySmartMaterial(d, (const char*)p->Data, &err) < 0) showError(T_("Could not apply the smart material"), err);
        else setStatus(F_("Added the smart material %s", d.selectedLayer()->name.c_str()));
      }
    }
    ImGui::EndDragDropTarget();
  }
  // ---- (0.38) the view pie (hold Z): over everything while it is open
  if (viewPieInput(origin, avail, hovered)) hovered = active = false;
  // ---- the axis gizmo in the corner (drawn last, over the rest)
  if (axisGizmoInput(origin, avail, hovered)) hovered = active = false;
  // ---- compare view (left of the divider), then the stencil over both
  if (compareOverlay(origin, avail, pw, ph, hovered)) hovered = active = false;
  // ---- stencil overlay; holding the stencil key hands the mouse to the stencil
  if (stencilOverlay(origin, avail, hovered)) hovered = active = false;
  // ---- note pins (with the Note tool, a click on a pin edits it)
  if (notesOverlay(origin, avail, hovered) && tool == ViewportTool::Note) hovered = active = false;

  // ---- camera navigation
  bool alt = io.KeyAlt, shift = io.KeyShift, ctrl = io.KeyCtrl;
  ImVec2 md = io.MouseDelta;
  // (0.38) Alt used with the mouse here (orbit, snap, pick) is not a tap of Alt: letting it go must not move the
  // keyboard focus to the menu bar (Dear ImGui's Alt toggle), which left the view deaf to its keys
  if (alt && (hovered || active || orbitDrag.on) &&
      (md.x != 0.f || md.y != 0.f || ImGui::IsAnyMouseDown() || io.MouseWheel != 0.f))
    ImGui::GetCurrentContext()->NavWindowingToggleLayer = false;
  if (active || hovered) {
    bool lmb = ImGui::IsMouseDown(ImGuiMouseButton_Left), mmb = ImGui::IsMouseDown(ImGuiMouseButton_Middle),
         rmb = ImGui::IsMouseDown(ImGuiMouseButton_Right);
    bool orbit = false, pan = false, dolly = false;
    switch (navPreset) {
      case NavPreset::Blender:
        orbit = mmb && !shift && !ctrl;
        pan = mmb && shift;
        dolly = mmb && ctrl;
        break;
      case NavPreset::Maya:
        orbit = lmb && alt && !shift && !ctrl;
        pan = mmb && alt;
        dolly = rmb && alt && !ctrl;
        break;
      case NavPreset::ZBrush:
        orbit = rmb && !alt && !shift && !ctrl;
        pan = rmb && alt && !shift;
        dolly = rmb && shift && !ctrl;
        break;
      default:
        // Substance / Maya style with Alt, Blender style with the middle button;
        // trackpads: Alt+LMB orbit, Alt+Shift+LMB pan, Alt+Ctrl+LMB zoom
        orbit = (lmb && alt && !shift && !ctrl) || (mmb && !shift && !ctrl && !alt);
        pan = (mmb && (shift || alt)) || (rmb && alt && shift) || (lmb && alt && shift);
        dolly = (rmb && alt && !shift) || (mmb && ctrl) || (lmb && alt && ctrl);
        break;
    }
    // (0.46) a view that only looks (the Guided layout's Model, Bake and Export steps): the left button turns it too,
    // Shift moves it, Ctrl zooms, as in a model viewer
    if (viewOnly && lmb && !alt) orbit = orbit || (!shift && !ctrl), pan = pan || shift, dolly = dolly || (ctrl && !shift);
    // Shift + right drag rotates the environment light (Alt+Shift with the ZBrush preset)
    bool envRot = rmb && shift && !ctrl && (navPreset == NavPreset::ZBrush ? alt : !alt);
    if (envRot && active && !P.active()) {
      engine.env.rotation += md.x * 0.01f;
      orbit = pan = dolly = false;
      envDrag = true;
    } else if (!rmb) {
      envDrag = false;
    }
    // (0.38) an orbit stays one until its button is let go; Alt (or Shift) meanwhile snaps it (ViewNav.cpp)
    if (orbitDragInput(active && orbit && !P.active(), md)) {
    } else if (active && orbit && !P.active()) cam.orbit(-md.x * 0.006f, md.y * 0.006f);
    else if (active && pan && !P.active()) cam.pan(vec2(md.x, md.y), lastViewportSize);
    else if (active && dolly && !P.active()) cam.zoom(std::exp(md.x * 0.005f - md.y * 0.005f));
    // (Shift+wheel with the Lines tool switches Shift+click's line between round the part and facing you)
    const bool ringWheel = shift && !ctrl && tool == ViewportTool::ZoneLine;
    if (hovered && io.MouseWheel != 0 && !ctrl && !ringWheel) cam.zoom(std::pow(0.88f, io.MouseWheel));
    if (hovered && io.MouseWheel != 0 && ctrl) P.brush.size = std::clamp(P.brush.size * std::pow(1.1f, io.MouseWheel), 1.f, 2000.f);
  }

  // ---- brush size / hardness drag (Ctrl + right mouse)
  static bool sizeDrag = false;
  static ImVec2 sizeDragOrigin;
  if (hovered && ctrl && !alt && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
    sizeDrag = true;
    sizeDragOrigin = mouse;
  }
  if (sizeDrag) {
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Right)) sizeDrag = false;
    else {
      P.brush.size = std::clamp(P.brush.size + md.x * 1.5f, 1.f, 2000.f);
      P.brush.hardness = std::clamp(P.brush.hardness - md.y * 0.005f, 0.f, 1.f);
    }
  }

  // ---- quick color pick: Alt+right-click (a click, not an Alt+right drag)
  auto quickPick = [&] {
    json out;
    if (engine.pickAt(local, true, &out)) {
      noteColorUsed(P.material.flat.color);
      engine.selectedMaterialId.clear();
      setStatus(T_("Picked color - the brush now paints these flat values"));
      return true;
    }
    return false;
  };
  // Blender's S: sample the color under the cursor right away
  if (hovered && !P.active() && !io.WantTextInput && keyPressed("color.sample")) quickPick();
  // Blender's F / Shift+F: move the mouse to set the brush size / opacity (Blender's strength), click to confirm;
  // (0.40) G / Shift+G the flow / hardness, Ctrl+F the angle (round the middle)
  const float kStrengthR = 120.f * uiScale;
  bool swallowClick = false;
  auto fractionAt = [&](float v) { return ImVec2(mouse.x - v * kStrengthR, mouse.y); };
  if (hovered && !P.active() && !radialEdit && !io.WantTextInput) {
    const bool moreKeys = engine.features.on("brush_keys");
    if (keyPressed("brush.radius")) {
      radialEdit = 1;
      radialStartValue = P.brush.size;
      radialCenter = ImVec2(mouse.x - P.brush.size * 0.5f, mouse.y);
    } else if (keyPressed("brush.strength")) {
      radialEdit = 2;
      radialStartValue = P.brush.opacity;
      radialCenter = fractionAt(P.brush.opacity);
    } else if (moreKeys && keyPressed("brush.flow")) {
      radialEdit = 3;
      radialStartValue = P.brush.flow;
      radialCenter = fractionAt(P.brush.flow);
    } else if (moreKeys && keyPressed("brush.hardness")) {
      radialEdit = 4;
      radialStartValue = P.brush.hardness;
      radialCenter = fractionAt(P.brush.hardness);
    } else if (moreKeys && keyPressed("brush.angle")) {
      // the pointer on the ring where the brush's angle points (counterclockwise from the right, as the tip turns)
      radialEdit = 5;
      radialStartValue = P.brush.angleDeg;
      float a = glm::radians(P.brush.angleDeg);
      radialCenter = ImVec2(mouse.x - std::cos(a) * kStrengthR, mouse.y + std::sin(a) * kStrengthR);
    }
  }
  if (radialEdit) {
    float dist = std::sqrt((mouse.x - radialCenter.x) * (mouse.x - radialCenter.x) +
                           (mouse.y - radialCenter.y) * (mouse.y - radialCenter.y));
    std::string txt;
    ImDrawList* odl = ImGui::GetForegroundDrawList();
    const ImU32 white = IM_COL32(255, 255, 255, 230), faint = IM_COL32(255, 255, 255, 120);
    float* value = radialEdit == 2 ? &P.brush.opacity : radialEdit == 3 ? &P.brush.flow : radialEdit == 4 ? &P.brush.hardness : nullptr;
    if (radialEdit == 1) {
      P.brush.size = std::clamp(dist * 2.f, 1.f, 2000.f);
      odl->AddCircle(radialCenter, P.brush.size * 0.5f, IM_COL32(0, 0, 0, 160), 96, 3.f);
      odl->AddCircle(radialCenter, P.brush.size * 0.5f, white, 96, 1.5f);
      txt = F_("Size %.0f px", P.brush.size);
    } else if (radialEdit == 5) {
      if (dist > 4.f * uiScale) {
        float deg = glm::degrees(std::atan2(radialCenter.y - mouse.y, mouse.x - radialCenter.x));
        if (io.KeyShift) deg = std::round(deg / 15.f) * 15.f;  // (Shift: steps of 15 degrees)
        P.brush.angleDeg = deg;
      }
      float a = glm::radians(P.brush.angleDeg);
      ImVec2 tip(radialCenter.x + std::cos(a) * kStrengthR, radialCenter.y - std::sin(a) * kStrengthR);
      odl->AddCircle(radialCenter, kStrengthR, faint, 96, 1.f);
      odl->AddLine(radialCenter, ImVec2(radialCenter.x + kStrengthR, radialCenter.y), IM_COL32(255, 255, 255, 90), 1.f);
      odl->PathArcTo(radialCenter, kStrengthR * 0.35f, 0.f, -a, 32);
      odl->PathStroke(ui::themeColor(ui::theme().accent), 0, 2.5f);
      odl->AddLine(radialCenter, tip, IM_COL32(0, 0, 0, 160), 4.f);
      odl->AddLine(radialCenter, tip, white, 2.f);
      odl->AddCircleFilled(tip, 4.f * uiScale, ui::themeColor(ui::theme().accent), 16);
      txt = F_("Angle %.0f deg", P.brush.angleDeg);
    } else {
      const float lo = radialEdit == 3 ? 0.01f : 0.f;
      *value = std::clamp(dist / kStrengthR, lo, 1.f);
      const float r = *value * kStrengthR;
      odl->AddCircle(radialCenter, kStrengthR, faint, 96, 1.f);
      if (radialEdit == 4) {
        // hardness: full strength inside the accent's disc, fading out to the ring
        const int steps = 12;
        const float band = (kStrengthR - r) / steps;
        for (int k = 0; k < steps; ++k) {
          float t = (k + 0.5f) / steps;
          odl->AddCircle(radialCenter, r + band * (k + 0.5f), ui::themeColor(ui::theme().accent, 0.353f * (1.f - t)), 96, band + 1.f);
        }
        odl->AddCircleFilled(radialCenter, r, ui::themeColor(ui::theme().accent, 0.43f), 96);
        txt = F_("Hardness %.2f", *value);
      } else {
        odl->AddCircleFilled(radialCenter, r, ui::themeColor(ui::theme().accent, 0.353f), 96);
        txt = radialEdit == 2 ? F_("Opacity %.2f", *value) : F_("Flow %.2f", *value);
      }
      odl->AddCircle(radialCenter, r, white, 96, 1.5f);
    }
    // the value on a dark plate (readable over a light model); the angle's under the middle, off its line
    ImVec2 ts = ImGui::CalcTextSize(txt.c_str());
    ImVec2 tp(std::round(radialCenter.x - ts.x * 0.5f), std::round(radialCenter.y - ts.y * 0.5f + (radialEdit == 5 ? ts.y * 1.4f : 0.f)));
    odl->AddRectFilled(tp - ImVec2(5, 2) * uiScale, tp + ts + ImVec2(5, 2) * uiScale, IM_COL32(20, 20, 22, 190), 4.f * uiScale);
    odl->AddText(tp, IM_COL32(255, 255, 255, 255), txt.c_str());
    bool confirm = ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsKeyPressed(ImGuiKey_Enter) ||
                   ImGui::IsKeyPressed(ImGuiKey_KeypadEnter);
    bool cancel = ImGui::IsMouseClicked(ImGuiMouseButton_Right) || ImGui::IsKeyPressed(ImGuiKey_Escape);
    if (cancel) {
      if (radialEdit == 1) P.brush.size = radialStartValue;
      else if (radialEdit == 5) P.brush.angleDeg = radialStartValue;
      else *value = radialStartValue;
    }
    if (confirm || cancel) radialEdit = 0;
    swallowClick = true;  // the confirming click does not paint...
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) lmbBlocked = true;  // ...nor does holding it afterward
  }
  if (viewOnly && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) lmbBlocked = true;  // (0.46) no tool acts
  if (lmbBlocked && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) lmbBlocked = false;
  if (lmbBlocked) swallowClick = true;
  if (objectModal >= 0) swallowClick = true;  // (1.2.3) G, R or S: its click keeps it, its right-click opens no menu
  if (hovered && alt && !ctrl && ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
      glm::length(vec2(ImGui::GetMouseDragDelta(ImGuiMouseButton_Right, 0.f).x, ImGui::GetMouseDragDelta(ImGuiMouseButton_Right, 0.f).y)) < 3.f)
    quickPick();
  // (0.36) a right-click without modifiers or dragging: the menu for the spot under the pointer (a drag still
  // orbits with the ZBrush navigation; Alt, Ctrl and Shift keep their right-button uses)
  if (ImGui::IsMouseClicked(ImGuiMouseButton_Right))
    viewRmbMenuOk = contextMenus() && hovered && !alt && !ctrl && !shift && !radialEdit && !swallowClick && !P.active() &&
                    !keyHeld("stencil.edit");  // (swallowClick: the right-click that cancels F / Shift+F... opens no menu)
  if (viewRmbMenuOk && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
    viewRmbMenuOk = false;
    ImVec2 dd = ImGui::GetMouseDragDelta(ImGuiMouseButton_Right, 0.f);
    if (hovered && std::abs(dd.x) + std::abs(dd.y) < 4.f) {
      viewportMenuAt = local;
      viewportMenuHit = engine.pick(local);
      layersAtMenu = viewportMenuHit.hit ? engine.layersAt(viewportMenuHit.uv) : std::vector<Engine::LayerHere>{};
      ImGui::OpenPopup("##viewctx");
    }
  }
  // ---- tools
  bool lmbClicked = hovered && !alt && !swallowClick && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
  // Alt+click (without dragging) with the Fill tool erases the faces instead
  bool altFillClick = hovered && alt && !ctrl && tool == ViewportTool::Fill && ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
                      glm::length(vec2(ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.f).x, ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.f).y)) < 3.f;
  // ---- (1.2) the scene's other objects: a double-click on one paints it from then on (the Object tool: a click)
  if (tool == ViewportTool::Object && !viewOnly) {
    objectToolUI(origin, avail, hovered && !alt && !swallowClick, local);
    lmbClicked = false;
  } else if (hovered && !viewOnly && engine.objectCount() > 1 && !engine.isolateActive && !P.active() && !alt) {
    Engine::ScenePick sp = engine.pickScene(local);
    if (sp.object >= 0 && sp.object != engine.activeObject()) {
      std::string name = engine.object(sp.object).displayName();
      if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        selectObjectUI(sp.object);
        lmbClicked = false;
      } else {
        // (the brush doesn't start on another object: it would paint nothing)
        if (lmbClicked) setStatus(F_("That is %s: double-click it to paint it", name.c_str()));
        lmbClicked = false;
        ImGui::GetWindowDrawList()->AddText(ImVec2(mouse.x + 16 * uiScale, mouse.y + 14 * uiScale), IM_COL32(255, 255, 255, 200),
                                            F_("%s - double-click to paint it", name.c_str()).c_str());
      }
    }
  }
  // (1.2) Alt+click (no drag) with the Select tool: the band of quads across the edge nearest the pointer (Blender's
  // face loop); Ctrl+Alt+click: the band along it; Shift adds to the selection
  if (hovered && alt && tool == ViewportTool::SelectFaces && !viewOnly && ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
      glm::length(vec2(ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.f).x, ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.f).y)) < 3.f &&
      engine.pick(local).hit) {
    json r;
    vec2 p = local / glm::max(lastViewportSize, vec2(1.f));
    if (runCommand("select_loop", {{"point", {p.x, p.y}}, {"ring", ctrl}, {"mode", shift ? "add" : "set"}}, &r)) {
      if (!r.value("quads", true)) setStatus(T_("Loops and rings run through quads: this face isn't one"));
      else if (ctrl) setStatus(F_("Selected a ring of %d faces", r.value("faces", 0)));
      else setStatus(F_("Selected a loop of %d faces", r.value("faces", 0)));
    }
  }
  float pressure = currentPressure();
  Painter::View view = engine.paintView(cam, lastViewportSize);  // (1.2) in the active object's space
  bool brushTool = !viewOnly && (tool == ViewportTool::Paint || tool == ViewportTool::Erase || tool == ViewportTool::Blur ||
                                 tool == ViewportTool::Clone || tool == ViewportTool::Smudge);
  auto toolFor = [&] {
    return tool == ViewportTool::Erase ? PaintTool::Erase
         : tool == ViewportTool::Blur  ? PaintTool::Blur
         : tool == ViewportTool::Clone ? PaintTool::Clone
         : tool == ViewportTool::Smudge ? PaintTool::Smudge
                                       : PaintTool::Paint;
  };
  if (tool == ViewportTool::Clone && lmbClicked && ctrl && !P.active()) {
    // Ctrl+click: pick the clone source on the surface
    RayHit h = engine.pick(local);
    if (h.hit) {
      P.setCloneSource(h.uv);
      cloneMarkerValid = true;
      cloneMarkerPos = h.position;
      cloneMarkerUV = h.uv;
      setStatus(F_("Clone source set at UV %.3f, %.3f", h.uv.x, h.uv.y));
    }
    lmbClicked = false;
  }
  auto toLocalV = [origin](vec2 w) { return vec2(w.x - origin.x, w.y - origin.y); };
  int startIdx = -1;
  if (brushTool) {
    if (!P.active()) P.tool = toolFor();
    if (lmbClicked && !P.active()) {
      P.tool = toolFor();
      // the first sample with the tip down starts the stroke (full tablet resolution)
      startIdx = strokeStartSample();
      StrokePoint start{local, pressure};
      if (startIdx >= 0) start = strokePointOf(pen.frameSamples()[startIdx], toLocalV);
      penEraserStroke = startIdx >= 0 && pen.frameSamples()[startIdx].pen && pen.frameSamples()[startIdx].eraser &&
                        pen.eraserTipErases &&
                        (P.tool == PaintTool::Paint || P.tool == PaintTool::Blur || P.tool == PaintTool::Smudge);
      if (penEraserStroke) P.tool = PaintTool::Erase;
      // Blender: Ctrl while painting uses the secondary color for this stroke
      strokeUsesSecondary = ctrlPaintsSecondary && ctrl && P.tool == PaintTool::Paint && P.material.id.empty();
      if (strokeUsesSecondary) std::swap(P.material.flat.color, secondaryColor);
      bool ok;
      if (shift && P.hasLastPoint()) {
        // straight line from the end of the previous stroke
        ok = P.beginStroke(engine.doc, engine.materials, engine.brushes, view, P.lastPoint());
        if (ok) P.addPoint(start);
      } else {
        ok = P.beginStroke(engine.doc, engine.materials, engine.brushes, view, start);
      }
      if (ok && startIdx >= 0) feedStroke(P, startIdx + 1, toLocalV);
      if (ok) strokeOwner = 1;
      if (!ok && strokeUsesSecondary) std::swap(P.material.flat.color, secondaryColor), strokeUsesSecondary = false;
      if (ok) {
        if (P.tool == PaintTool::Paint && P.material.id.empty() && !engine.doc.editMask) noteColorUsed(P.material.flat.color);
      } else if (tool == ViewportTool::Clone && !P.hasCloneSource) setStatus(T_("Clone: Ctrl+click on the model to set a source first"));
      else setStatus(T_("Nothing to paint: select a layer and enable at least one channel"));
    }
  }
  if (P.active() && strokeOwner == 1 && startIdx < 0) {
    // every pen / mouse sample since the last frame, not just the frame's mouse position
    int fed = feedStroke(P, 0, toLocalV);
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      if (!fed && (md.x != 0 || md.y != 0)) P.addPoint({local, pressure, pen.lastWasPen() ? pen.tilt() : vec2(0)});
    } else {
      P.endStroke();
      strokeOwner = 0;
      if (strokeUsesSecondary) std::swap(P.material.flat.color, secondaryColor), strokeUsesSecondary = false;
      if (penEraserStroke) P.tool = toolFor(), penEraserStroke = false;
    }
  }
  if ((lmbClicked || altFillClick) && tool == ViewportTool::Fill) {
    RayHit h = engine.pick(local);
    PaintTool keep = P.tool;
    P.tool = altFillClick ? PaintTool::Erase : PaintTool::Paint;
    if (h.hit) {
      auto faces = P.facesForFill(engine.doc, h.face, P.fillMode);
      if (P.fillFaces(engine.doc, engine.materials, faces))
        setStatus(altFillClick ? F_("Erased %zu faces", faces.size()) : F_("Filled %zu faces", faces.size()));
    } else if (P.fillMode == FillMode::Mesh || P.fillMode == FillMode::Selection) {
      auto faces = P.facesForFill(engine.doc, 0, P.fillMode);
      P.fillFaces(engine.doc, engine.materials, faces);
    }
    P.tool = keep;
  }
  if (lmbClicked && tool == ViewportTool::Picker) {
    if (quickPick() && quickPickActive) quickPickUsed = true;
  }
  if (lmbClicked && tool == ViewportTool::SelectFaces) {
    RayHit h = engine.pick(local);
    if (h.hit) applyFaceSelectionClick(h.face, shift, ctrl);
    else if (!shift && !ctrl) {
      std::fill(engine.doc.faceSelection.begin(), engine.doc.faceSelection.end(), 0);
      engine.doc.updateSelectionFlags();
    }
  }
  // Island brush and Select drags: one undo step per press, so a stray stroke is one Ctrl+Z away
  bool selTool = tool == ViewportTool::Island || tool == ViewportTool::SelectFaces;
  if (selTool && lmbClicked && !selStrokeActive) {
    engine.doc.faceSelection.resize(engine.doc.mesh.faceCount(), 0);
    selStrokeBefore = engine.doc.faceSelection;
    selStrokeActive = true;
  }
  if (selStrokeActive && (!ImGui::IsMouseDown(ImGuiMouseButton_Left) || !selTool)) {
    selStrokeActive = false;
    engine.doc.pushSelectionUndo(selStrokeBefore, tool == ViewportTool::Island ? "Island brush" : "Select faces");
    selStrokeBefore.clear();
  }
  if (tool == ViewportTool::SelectFaces && hovered && !alt && !lmbBlocked && ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
      !ImGui::IsMouseClicked(ImGuiMouseButton_Left) && hoverHit.hit) {
    // paint-select by dragging
    auto& s = engine.doc.faceSelection;
    std::vector<uint32_t> faces = selectionFacesAt(hoverHit.face);
    bool changed = false;
    for (uint32_t f : faces)
      if (f < s.size() && s[f] != (ctrl ? 0 : 1)) s[f] = ctrl ? 0 : 1, changed = true;
    if (changed) engine.doc.updateSelectionFlags();
  }
  // ---- island brush: the faces under the brush (seen in the circle, or along the surface) join (Ctrl: leave) the selection
  if (tool == ViewportTool::Island && hovered && inside && !alt && !lmbBlocked && selStrokeActive &&
      ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
      (md.x != 0 || md.y != 0 || ImGui::IsMouseClicked(ImGuiMouseButton_Left))) {
    float rpx = std::max(2.f, P.brush.size * 0.5f);
    auto& s = engine.doc.faceSelection;
    s.resize(engine.doc.mesh.faceCount(), 0);
    uint8_t want = ctrl ? 0 : 1;
    bool changed = false;
    auto reach = islandReach == 0 || !engine.features.on("island_reach") ? Engine::BrushReach::Screen : Engine::BrushReach::Surface;
    for (uint32_t f : engine.facesUnderBrush(local, rpx, reach, [&](uint32_t f) { return s[f] == want; }))
      if (f < s.size() && s[f] != want) s[f] = want, changed = true;
    if (changed) engine.doc.updateSelectionFlags();
  }
  // ---- retopology brushes (RetopoUI.cpp)
  if (retopoTool()) retopoBrush(local, hovered && !alt && !lmbBlocked, inside, lmbClicked, md.x != 0 || md.y != 0);
  else if (retopoStrokeActive) retopoEndStroke();
  // ---- seams brush: a click toggles the nearest edge; a drag lays a seam along the edges from vertex
  // to vertex under the cursor (Ctrl: clears it); Shift+click continues from the last seam's end
  if (edgeTool) {
    constexpr uint32_t kNo = uv::SeamGraph::kNoVertex;
    const Mesh& m = engine.doc.mesh;
    if (!seamGraph.matches(m)) {
      seamGraph.build(m);
      seamLastEnd = kNo;
      seamStroke = {};
    }
    uint32_t hv = hoverHit.hit ? seamGraph.nearestVertex(m, hoverHit.face, hoverHit.position) : kNo;
    auto commit = [&](const std::vector<uint32_t>& wedges, bool clear, const char* label) {
      if (wedges.empty()) return;
      auto flags = m.edgeFlags;
      seamGraph.apply(flags, wedges, !clear);
      engine.doc.applySeams(flags, label);
      engine.viewOptions.seams = true;
    };
    if (lmbClicked && hoverHit.hit) {
      if (shift && seamLastEnd != kNo && hv != kNo) {
        commit(seamGraph.path(seamLastEnd, hv), ctrl, ctrl ? "Clear seam path" : "Seam path");
        seamLastEnd = hv;
      } else {
        seamStroke = {};
        seamStroke.active = true;
        seamStroke.clear = ctrl;
        seamStroke.last = hv;
        seamStroke.startFace = hoverHit.face;
        seamStroke.startPos = hoverHit.position;
      }
    }
    if (seamStroke.active && ImGui::IsMouseDown(ImGuiMouseButton_Left) && hv != kNo && hv != seamStroke.last &&
        seamStroke.last != kNo) {
      auto p = seamGraph.path(seamStroke.last, hv);
      seamStroke.wedges.insert(seamStroke.wedges.end(), p.begin(), p.end());
      seamStroke.last = hv;
      seamStroke.moved = true;
    }
    if (seamStroke.active && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      if (seamStroke.moved) {
        commit(seamStroke.wedges, seamStroke.clear, seamStroke.clear ? "Clear seams (brush)" : "Seam brush");
        seamLastEnd = seamStroke.last;
      } else {
        int e = nearestEdge(m, seamStroke.startFace, seamStroke.startPos);
        if (e >= 0) {
          auto flags = m.edgeFlags;
          if (seamStroke.clear) flags[e] &= ~EdgeSeam;
          else flags[e] ^= EdgeSeam;
          engine.doc.applySeams(flags, "Edit seam");
          engine.viewOptions.seams = true;
          // the click's end of the edge nearest the cursor is where a Shift+click continues
          seamLastEnd = seamStroke.last;
        }
      }
      seamStroke = {};
    }
    // Shift: preview the path a click would add
    if (shift && seamLastEnd != kNo && hv != kNo && !seamStroke.active) {
      if (hv != seamPreviewTo) seamPreview = seamGraph.path(seamLastEnd, hv), seamPreviewTo = hv;
    } else {
      seamPreview.clear();
      seamPreviewTo = kNo;
    }
    // overlay: the stroke (or preview) and the vertex under the cursor
    if (inside && hovered) {
      float aspect = lastViewportSize.x / std::max(lastViewportSize.y, 1.f);
      mat4 vpm = engine.objectCamera().viewProj(aspect);  // (the seam's points are the object's)
      auto toScreen = [&](const vec3& p, ImVec2& out) {
        vec4 c = vpm * vec4(p, 1.f);
        if (c.w <= 1e-6f) return false;
        vec2 ndc = vec2(c) / c.w;
        out = ImVec2(origin.x + (ndc.x * 0.5f + 0.5f) * avail.x, origin.y + (0.5f - ndc.y * 0.5f) * avail.y);
        return true;
      };
      ImDrawList* odl = ImGui::GetWindowDrawList();
      auto drawPath = [&](const std::vector<uint32_t>& wedges, ImU32 col) {
        for (uint32_t w : wedges) {
          ImVec2 a, b;
          if (toScreen(seamGraph.pos[seamGraph.wedgeEnds[w].x], a) && toScreen(seamGraph.pos[seamGraph.wedgeEnds[w].y], b))
            odl->AddLine(a, b, col, 3.f);
        }
      };
      bool clearing = seamStroke.active ? seamStroke.clear : ctrl;
      ImU32 col = clearing ? IM_COL32(120, 200, 255, 230) : IM_COL32(255, 70, 50, 240);
      drawPath(seamStroke.wedges, col);
      drawPath(seamPreview, (col & 0x00FFFFFFu) | 0x90000000u);
      ImVec2 sp;
      if (hv != kNo && toScreen(seamGraph.pos[hv], sp)) odl->AddCircleFilled(sp, 4.f, col);
      if (seamLastEnd != kNo && shift && toScreen(seamGraph.pos[seamLastEnd], sp)) odl->AddCircle(sp, 6.f, col, 0, 2.f);
    }
  }

  // ---- overlays: brush cursor, hints
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (inside && hovered && brushTool && !alt && !radialEdit) {
    float r = std::max(1.f, P.brush.size * 0.5f * (P.brush.pressureSize ? (P.active() ? pressure : 1.f) : 1.f));
    ImU32 col = tool == ViewportTool::Erase ? IM_COL32(120, 200, 255, 220)
              : tool == ViewportTool::Blur  ? IM_COL32(190, 140, 255, 220)
              : tool == ViewportTool::Clone ? IM_COL32(120, 255, 160, 220)
              : tool == ViewportTool::Smudge ? IM_COL32(255, 200, 120, 220)
                                            : IM_COL32(255, 255, 255, 220);
    if (std::abs(P.brush.roundness - 1.f) > 0.01f || P.brush.alpha.shape == "square") {
      // show the dab footprint orientation
      float a = glm::radians(P.brush.angleDeg);
      ImVec2 c = mouse;
      ImVec2 pts[4];
      vec2 ax(std::cos(a), -std::sin(a)), ay(std::sin(a), std::cos(a));
      float rx = r, ry = r * P.brush.roundness;
      vec2 corners[4] = {{-rx, -ry}, {rx, -ry}, {rx, ry}, {-rx, ry}};
      for (int i = 0; i < 4; ++i) {
        vec2 p = ax * corners[i].x + ay * corners[i].y;
        pts[i] = ImVec2(c.x + p.x, c.y + p.y);
      }
      dl->AddPolyline(pts, 4, col, ImDrawFlags_Closed, 1.5f);
    } else {
      dl->AddCircle(mouse, r, IM_COL32(0, 0, 0, 160), 64, 3.f);
      dl->AddCircle(mouse, r, col, 64, 1.5f);
      float inner = r * std::clamp(P.brush.hardness, 0.f, 1.f);
      if (inner > 2) dl->AddCircle(mouse, inner, IM_COL32(255, 255, 255, 70), 48, 1.f);
    }
    if (P.hasLastPoint() && shift && !P.active()) {
      ImVec2 a(origin.x + P.lastPoint().pos.x, origin.y + P.lastPoint().pos.y);
      dl->AddLine(a, mouse, IM_COL32(255, 200, 120, 200), 1.5f);
    }
  }
  retopoOverlay(origin, avail);
  if (inside && hovered && tool == ViewportTool::Density && !alt && !radialEdit && !viewOnly) {
    float r = std::max(2.f, P.brush.size * 0.5f);
    ImU32 col = ctrl ? IM_COL32(255, 120, 120, 230) : tool == ViewportTool::Density ? IM_COL32(255, 170, 90, 230)
                                                                                   : IM_COL32(140, 255, 170, 230);
    dl->AddCircle(mouse, r, IM_COL32(0, 0, 0, 160), 64, 3.f);
    dl->AddCircle(mouse, r, col, 64, 1.5f);
  }
  if (inside && hovered && tool == ViewportTool::Island && !alt && !radialEdit && !viewOnly) {
    float r = std::max(2.f, P.brush.size * 0.5f);
    dl->AddCircle(mouse, r, IM_COL32(0, 0, 0, 160), 64, 3.f);
    dl->AddCircle(mouse, r, ctrl ? IM_COL32(255, 120, 120, 230) : IM_COL32(110, 220, 255, 230), 64, 1.5f);
  }
  if (!P.active()) {
    if (brushTool) P.tool = toolFor();
    else if (tool == ViewportTool::Fill) P.tool = PaintTool::Fill;
    else if (tool == ViewportTool::Picker) P.tool = PaintTool::Picker;
    lastPainterTool = P.tool;
  }
  // clone source marker
  if (tool == ViewportTool::Clone && cloneMarkerValid && P.hasCloneSource && !P.active() &&
      cloneMarkerUV == P.cloneSourceUV) {
    vec4 c = engine.objectCamera().viewProj(avail.x / avail.y) * vec4(cloneMarkerPos, 1.f);
    if (c.w > 1e-5f) {
      vec2 ndc = vec2(c) / c.w;
      ImVec2 sp(origin.x + (ndc.x * 0.5f + 0.5f) * avail.x, origin.y + (0.5f - ndc.y * 0.5f) * avail.y);
      ImU32 g = IM_COL32(120, 255, 160, 230);
      dl->AddCircle(sp, 7.f, IM_COL32(0, 0, 0, 180), 24, 3.f);
      dl->AddCircle(sp, 7.f, g, 24, 1.5f);
      dl->AddLine(ImVec2(sp.x - 11, sp.y), ImVec2(sp.x + 11, sp.y), g, 1.5f);
      dl->AddLine(ImVec2(sp.x, sp.y - 11), ImVec2(sp.x, sp.y + 11), g, 1.5f);
    }
  }
  axisGizmoDraw(origin, avail);
  viewPieDraw();
  // corner info
  {
    char buf[256];
    Layer* l = engine.doc.selectedLayer();
    const bool mask = engine.doc.editMask || (l && l->masksOnly());
    if (l && l->isBaked())  // (0.41) its fixes, and the value the brush paints
      snprintf(buf, sizeof buf, "%s %s", l->name.c_str(), F_("[fixes: paint %.2f]", P.maskValue).c_str());
    else
      snprintf(buf, sizeof buf, mask ? "%s %s  |  %s" : "%s%s  |  %s", l ? l->name.c_str() : T_("(no layer)"), mask ? T_("[mask]") : "",
               P.material.id.empty() ? T_("flat color")
               : engine.materials.find(P.material.id) ? engine.materials.find(P.material.id)->def.name.c_str()
                                                      : P.material.id.c_str());
    ImVec2 p(origin.x + 10, origin.y + avail.y - ImGui::GetTextLineHeight() - 8);
    if (!viewOnly) {
      dl->AddText(ImVec2(p.x + 1, p.y + 1), IM_COL32(0, 0, 0, 200), buf);
      dl->AddText(p, IM_COL32(230, 230, 230, 230), buf);
    }
    // (0.46) a brush that would paint nothing says why, above that line (a hidden layer, no channels...)
    if (brushTool && engine.doc.hasMesh()) {
      std::string why;
      Document& d = engine.doc;
      bool hiddenAbove = false;
      for (uint32_t up = l ? d.effectiveParent(*l) : 0; up;) {
        const Layer* q = d.findLayer(up);
        if (!q) break;
        hiddenAbove |= !q->visible;
        up = d.effectiveParent(*q);
      }
      if (!l) why = T_("No layer is selected: pick one in Layers, or add a paint layer");
      else if (l->isFolder() && !d.editMask) why = T_("A folder is selected: strokes paint its mask");
      else if (!l->visible) why = T_("The selected layer is hidden: strokes on it don't show");
      else if (hiddenAbove) why = T_("The selected layer's folder is hidden: strokes don't show");
      else if (l->opacity <= 0.001f) why = T_("The selected layer's opacity is 0: strokes don't show");
      else if (!mask && !(P.channelMask & l->channelMask)) why = T_("No channel is on: switch some on in Paint channels (Properties)");
      if (!why.empty()) {
        ImVec2 q(p.x, p.y - ImGui::GetTextLineHeight() - 6);
        ImVec2 ts = ImGui::CalcTextSize(why.c_str());
        dl->AddRectFilled(ImVec2(q.x - 4, q.y - 2), ImVec2(q.x + ts.x + 4, q.y + ts.y + 2), IM_COL32(120, 60, 10, 220), 3.f);
        dl->AddText(q, IM_COL32(255, 220, 170, 255), why.c_str());
        engine.uiRects["viewport.blocked"] = {q.x, q.y, ts.x, ts.y};
      } else {
        engine.uiRects.erase("viewport.blocked");
      }
    }
    // (0.39) how far the path tracer has got
    if (engine.viewOptions.mode == ViewMode::PathTraced && engine.doc.hasMesh()) {
      const PathTracer& t = engine.renderer.pathTracer();
      const PathTraceSettings& st = engine.viewOptions.pathTrace;
      std::string s = t.done(st) ? F_("Path traced: %d samples", t.samples()) : F_("Path traced: %d / %d samples", t.samples(), st.maxSamples);
      ImVec2 q(p.x, p.y - ImGui::GetTextLineHeight() - 4);
      dl->AddText(ImVec2(q.x + 1, q.y + 1), IM_COL32(0, 0, 0, 200), s.c_str());
      dl->AddText(q, t.done(st) ? IM_COL32(170, 220, 150, 240) : IM_COL32(232, 180, 120, 240), s.c_str());
    }
    if (fullscreen && nowSeconds() - distractionFreeSince < 4.0) {
      std::string t = F_("Panels hidden - press %s to bring them back", keyLabel("view.focus") ? keyLabel("view.focus") : "Tab");
      ImVec2 ts = ImGui::CalcTextSize(t.c_str());
      ImVec2 tp(origin.x + (avail.x - ts.x) * 0.5f, origin.y + 44 * uiScale);
      dl->AddRectFilled(tp - ImVec2(10, 6), tp + ts + ImVec2(10, 6), IM_COL32(20, 20, 22, 200), 6.f);
      dl->AddText(tp, IM_COL32(240, 240, 240, 255), t.c_str());
    }
    if (!engine.doc.hasMesh()) {
      const char* t = T_("Drop a mesh (.obj .gltf .glb .fbx .stl .3mf .ply) or a .patina project here, or File > Open");
      ImVec2 ts = ImGui::CalcTextSize(t);
      dl->AddText(ImVec2(origin.x + (avail.x - ts.x) * 0.5f, origin.y + avail.y * 0.5f), IM_COL32(220, 220, 220, 255), t);
    }
  }
  // keyboard shortcuts scoped to the viewport (or run from the command palette)
  if ((hovered && !io.WantTextInput) || !forcedAction.empty()) {
    if (keyPressed("brush.smaller", true)) P.brush.size = std::max(1.f, P.brush.size / 1.15f);
    if (keyPressed("brush.bigger", true)) P.brush.size = std::min(2000.f, P.brush.size * 1.15f);
    if (keyPressed("view.frame") || keyPressed("view.frame_alt")) engine.frameMesh();
    // the side views jump (0.38: and are remembered, orthographic with autoOrtho; Numpad 0 goes back)
    auto numpadView = [&](const char* name) {
      float y, p;
      if (Camera::viewAngles(name, y, p)) gizmo.turning = false, snapToView(y, p);
    };
    const bool numpad = objectModal < 0;  // (1.2.3) G, R or S under way: the numpad types its number
    if (numpad && ImGui::IsKeyPressed(ImGuiKey_Keypad1)) numpadView(ctrl ? "back" : "front");
    if (numpad && ImGui::IsKeyPressed(ImGuiKey_Keypad3)) numpadView(ctrl ? "left" : "right");
    if (numpad && ImGui::IsKeyPressed(ImGuiKey_Keypad7)) numpadView(ctrl ? "bottom" : "top");
    if (numpad && ImGui::IsKeyPressed(ImGuiKey_Keypad5)) cam.ortho = !cam.ortho, engine.viewMemory.orthoBySnap = false;
    if (keyPressed("view.back")) goBackView();
  }
  viewportContextMenu(origin, avail);
  endPanel();  // (the distraction-free view: its own window, ended the same way)
}

}  // namespace patina
