// Retopology (0.14): the zone line, zone, flow and density tools, the Retopology menu and window. The
// work itself is in core/RetopoQuads (and core/Retopo for painted patches) and the retopo_* commands
// (engine/CommandsRetopo.cpp).
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <filesystem>

#include "core/Log.h"
#include "core/Paths.h"
#include "core/RetopoMirror.h"
#include "core/RetopoThin.h"
#include "core/UVIsland.h"
#include "engine/Undo.h"
#include "ui/App.h"

namespace patina {

// (the Flow tool's option, in the window and the bar over the viewport)
static const char* kFlowLoopsTip =
    N_("On (the default): an edge loop runs exactly along each flow stroke, as along a zone line, but the stroke\n"
       "doesn't make a zone of its own (one resolution either side of it). Rings drawn round a spot (a nipple, an\n"
       "eye) become loops round it. Off: a stroke only turns the quads its way.");

namespace fs = std::filesystem;

void App::openRetopo() {
  showRetopoWindow();
  if (!retopoTool()) tool = ViewportTool::ZoneLine;
}

namespace {
// one undo step for the flow strokes (the whole list before and after)
void pushStrokesUndo(Engine& engine, std::vector<retopo::Stroke> before, const char* label) {
  auto& S = engine.retopo();
  auto after = S.strokes;
  uint32_t faces = S.faces;
  auto e = std::make_unique<LambdaEntry>();
  e->label = label;
  e->size = (before.size() + after.size()) * 64;
  e->onUndo = [&engine, before, faces](Document&) {
    auto& R = engine.retopo();
    if (R.faces == faces) R.strokes = before, engine.retopoChanged();
  };
  e->onRedo = [&engine, after, faces](Document&) {
    auto& R = engine.retopo();
    if (R.faces == faces) R.strokes = after, engine.retopoChanged();
  };
  engine.doc.undo.push(std::move(e));
}
}  // namespace

// The point of the zone lines nearest a viewport position (within maxPx), where the camera sees it:
// strokes start and end there exactly, so lines meet
bool App::retopoSnap(vec2 local, float maxPx, vec3& out) {
  auto& S = engine.retopo();
  if (S.lines.empty() || !engine.retopoSnapEnds) return false;
  const Mesh& m = engine.doc.mesh;
  const Camera cam = engine.objectCamera();  // (1.2) the lines are on the active object
  float aspect = lastViewportSize.x / std::max(lastViewportSize.y, 1.f);
  mat4 vpm = cam.viewProj(aspect);
  auto screen = [&](const vec3& p, vec2& sp) {
    vec4 c = vpm * vec4(p, 1.f);
    if (c.w <= 1e-6f) return false;
    sp = vec2((c.x / c.w * 0.5f + 0.5f) * lastViewportSize.x, (0.5f - c.y / c.w * 0.5f) * lastViewportSize.y);
    return true;
  };
  struct Cand {
    float d;
    vec3 p;
  };
  std::vector<Cand> cands;
  for (auto& l : S.lines) {
    if (l.confirmed) continue;  // (0.22: confirmed lines are left alone)
    const auto& P = l.points;
    size_t n = P.size() + (l.loop && P.size() > 2 ? 1 : 0);
    vec2 sa;
    bool okA = !P.empty() && screen(P[0], sa);
    for (size_t k = 0; k + 1 < n; ++k) {
      const vec3 &a = P[k], &b = P[(k + 1) % P.size()];
      vec2 sb;
      bool okB = screen(b, sb);
      if (okA && okB) {
        vec2 e = sb - sa;
        float len2 = glm::dot(e, e), t = len2 > 0 ? std::clamp(glm::dot(local - sa, e) / len2, 0.f, 1.f) : 0.f;
        // the ends of a line are where another most likely meets it: they win within a few pixels
        if (k == 0 && !l.loop && glm::length(local - sa) < maxPx) t = 0;
        if (k + 2 == n && !l.loop && glm::length(local - sb) < maxPx) t = 1;
        float d = glm::length(sa + e * t - local);
        if (d <= maxPx) cands.push_back({d, glm::mix(a, b, t)});
      }
      sa = sb, okA = okB;
    }
  }
  std::sort(cands.begin(), cands.end(), [](const Cand& x, const Cand& y) { return x.d < y.d; });
  for (size_t i = 0; i < cands.size() && i < 6; ++i) {
    vec2 sp;
    if (!screen(cands[i].p, sp)) continue;
    vec3 o, d;
    cam.ray(sp, lastViewportSize, o, d);
    RayHit h = engine.doc.bvh.raycast(m, o, d);
    float dist = glm::length(cands[i].p - o);
    if (h.hit && glm::length(h.position - cands[i].p) > 0.02f * dist) continue;  // behind the model
    out = cands[i].p;
    return true;
  }
  return false;
}

void App::retopoBrush(vec2 local, bool hovered, bool inside, bool clicked, bool moved) {
  if (!retopoTool() || !engine.doc.hasMesh()) return;
  ImGuiIO& io = ImGui::GetIO();
  bool ctrl = io.KeyCtrl;
  auto& S = engine.retopo();
  const Mesh& m = engine.doc.mesh;
  // (0.22.2) a stroke begun: a live preview being made stops (its inputs are about to change, and it would
  // take the processor from the drawing); one is made again after the stroke
  auto pausePreview = [&] {
    if (S.building.valid() && S.buildIsPreview && S.cancel) S.cancel->store(true), S.previewTried = 0;
  };
  // ---- Zone: a click chooses the zone under the cursor
  if (tool == ViewportTool::Patch) {
    if (clicked && hovered && inside) {
      RayHit c = engine.pick(local);
      if (c.hit) {
        const auto& z = engine.retopoZones();
        S.selectedZone = c.face < z.size() ? z[c.face] : 0;
        retopoZoneFace = c.face;
      } else {
        S.selectedZone = 0, retopoZoneFace = ~0u;
      }
    }
    return;
  }
  // ---- Lines and Flow: strokes on the surface, where the pen goes (like a grease pencil). A zone line
  // starts and ends exactly on a line it begins or ends near, and closes when it ends near its start.
  // Ctrl+click takes the nearest stroke away.
  if (tool == ViewportTool::ZoneLine || tool == ViewportTool::Flow) {
    const bool lines = tool == ViewportTool::ZoneLine;
    constexpr float kSnapPx = 12.f;
    auto& list = lines ? S.lines : S.strokes;
    // Shift+click with Lines: a line right round the part there (an arm, a leg, the neck), across the way
    // it runs, mirrored with the Sym buttons
    if (lines && clicked && hovered && inside && io.KeyShift && !ctrl) {
      RayHit c = engine.pick(local);
      if (c.hit) {
        std::string why;
        int used = 0;
        if (engine.retopoAddRing(c.position, engine.retopoSymmetryAxes(), &why, ringMode, nullptr, &used))
          setStatus(used == 2 ? T_("A line round the model where it faces you") : T_("A line round the part"));
        else
          setStatus(F_("No line round the model here: %s", why.c_str()));
      }
      return;
    }
    if (clicked && hovered && inside && ctrl) {
      float aspect = lastViewportSize.x / std::max(lastViewportSize.y, 1.f);
      mat4 vpm = engine.objectCamera().viewProj(aspect);
      // the stroke nearest the pointer on screen (within 14 pixels); for taking one away, a line not yet
      // confirmed wins over a confirmed one
      auto nearest = [&](int wantConfirmed) {
        int best = -1;
        float bd = 14.f;
        for (size_t k = 0; k < list.size(); ++k) {
          if (wantConfirmed >= 0 && (int)list[k].confirmed != wantConfirmed) continue;
          const auto& P = list[k].points;
          size_t n = P.size() + (list[k].loop && P.size() > 2 ? 1 : 0);
          for (size_t i = 0; i + 1 < n; ++i) {
            vec4 a = vpm * vec4(P[i], 1.f), b = vpm * vec4(P[(i + 1) % P.size()], 1.f);
            if (a.w <= 1e-6f || b.w <= 1e-6f) continue;
            vec2 sa((a.x / a.w * 0.5f + 0.5f) * lastViewportSize.x, (0.5f - a.y / a.w * 0.5f) * lastViewportSize.y);
            vec2 sb((b.x / b.w * 0.5f + 0.5f) * lastViewportSize.x, (0.5f - b.y / b.w * 0.5f) * lastViewportSize.y);
            vec2 e = sb - sa;
            float len2 = glm::dot(e, e), t = len2 > 0 ? std::clamp(glm::dot(local - sa, e) / len2, 0.f, 1.f) : 0.f;
            if (float d = glm::length(sa + e * t - local); d < bd) bd = d, best = (int)k;
          }
        }
        return best;
      };
      // with the Sym buttons, a stroke's mirror images (strokes whose points are all near the mirrored ones)
      auto withMirrors = [&](int best) {
        std::vector<char> pick(list.size(), 0);
        pick[best] = 1;
        int axes = engine.retopoSymmetryAxes();
        const float tol = 2.f * engine.retopoDeadzone() + 1e-6f;
        for (int sub = 1; sub < 8; ++sub) {
          if ((sub & axes) != sub) continue;
          std::vector<vec3> img;
          for (auto& p : list[best].points) img.push_back(retopo::mirrorPoint(p, sub, S.symCenter));
          for (size_t k = 0; k < list.size(); ++k) {
            if (pick[k] || list[k].points.empty()) continue;
            bool all = true;
            for (auto& p : list[k].points) {
              float bd = 1e30f;
              for (auto& q : img) bd = std::min(bd, glm::length(p - q));
              if (bd > tol) {
                all = false;
                break;
              }
            }
            if (all) pick[k] = 1;
          }
        }
        return pick;
      };
      // Ctrl+Shift+click with Lines (0.22): the line there confirmed, or unconfirmed (with its mirror images)
      if (lines && io.KeyShift) {
        int best = nearest(-1);
        if (best >= 0) {
          const bool to = !list[best].confirmed;
          auto pick = withMirrors(best);
          std::vector<char> flags(list.size());
          for (size_t k = 0; k < list.size(); ++k) flags[k] = pick[k] ? to : list[k].confirmed;
          engine.retopoSetConfirmed(std::move(flags), to ? "Confirm zone line" : "Unconfirm zone line");
          setStatus(to ? T_("Line confirmed: new lines won't snap to it") : T_("Line no longer confirmed"));
        }
        return;
      }
      int best = nearest(lines ? 0 : -1);
      if (best < 0 && lines) best = nearest(1);
      if (best >= 0) {
        auto after = list;
        auto drop = withMirrors(best);
        for (size_t k = after.size(); k-- > 0;)
          if (drop[k]) after.erase(after.begin() + k);
        if (lines) {
          engine.retopoSetLines(std::move(after), "Remove zone line");
        } else {
          auto before = S.strokes;
          S.strokes = std::move(after);
          pushStrokesUndo(engine, before, "Remove flow stroke");
          engine.retopoChanged();
        }
      }
      return;
    }
    // every pointer sample since the last frame, not only where the pointer is now (a slow frame on a
    // big model would make the stroke a few straight pieces)
    ImVec2 mp = ImGui::GetMousePos();
    vec2 origin(mp.x - local.x, mp.y - local.y);
    auto addAt = [&](vec2 at) {
      if (retopoHasLast && glm::length(at - retopoLast) <= 3.f) return;
      RayHit c = engine.pick(at);
      if (c.hit) retopoStrokePts.push_back(c.position), retopoStrokeScreen.push_back(at), retopoLast = at, retopoHasLast = true;
    };
    auto addSamples = [&] {
      for (const PointerSample& ps : pen.frameSamples())
        if (ps.down) addAt(ps.pos - origin);
    };
    if (clicked && hovered && inside) {
      retopoStrokePts.clear(), retopoStrokeScreen.clear();
      retopoStrokeActive = true, retopoHasLast = false;
      pausePreview();
      vec3 snapped;
      if (lines && retopoSnap(local, kSnapPx, snapped)) {
        retopoStrokePts.push_back(snapped), retopoStrokeScreen.push_back(local), retopoLast = local, retopoHasLast = true;
      } else {
        addAt(local);
      }
      addSamples();
      return;
    }
    if (!retopoStrokeActive) return;
    addSamples();
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      addAt(local);
      return;
    }
    // released
    retopoStrokeActive = false, retopoHasLast = false;
    auto pts = std::move(retopoStrokePts);
    auto scr = std::move(retopoStrokeScreen);
    retopoStrokePts.clear(), retopoStrokeScreen.clear();
    if (pts.size() < 2) return;
    float screenLen = 0;
    for (size_t i = 1; i < scr.size(); ++i) screenLen += glm::length(scr[i] - scr[i - 1]);
    retopo::Stroke st;
    st.edge = lines;
    vec3 snapped;
    if (scr.size() > 4 && screenLen > 4 * kSnapPx && glm::length(scr.back() - scr.front()) < kSnapPx) {
      // it ends where it began: a closed line (the last points, over its start, go)
      while (pts.size() > 3 && glm::length(scr.back() - scr.front()) < kSnapPx * 0.5f) pts.pop_back(), scr.pop_back();
      st.loop = true;
    } else if (lines && retopoSnap(scr.back(), kSnapPx, snapped)) {
      pts.back() = snapped;
    }
    st.points = std::move(pts);
    // with the Sym buttons: mirrored across the model's middle (joined there when it reaches it)
    auto made = engine.retopoMirror(st, engine.retopoSymmetryAxes());
    if (lines) {
      auto after = S.lines;
      for (auto& m : made) after.push_back(std::move(m));
      engine.retopoSetLines(std::move(after), "Zone line");
    } else {
      auto before = S.strokes;
      for (auto& m : made) S.strokes.push_back(std::move(m));
      pushStrokesUndo(engine, before, "Flow stroke");
      engine.retopoChanged();
    }
    return;
  }
  // ---- Density: a brush on the surface (the vertices within its reach of the point under the cursor,
  // reached over the surface, so it doesn't paint through to another part), soft at its edge
  if (clicked && !retopoStrokeActive) {
    retopoStrokeActive = true;
    retopoDensityBefore = S.density;
    pausePreview();
  }
  // (a slow frame can see the last moves and the release together: the moves are painted first; and
  // ImGui's mouse delta can miss a move, so it is measured from the last dab too)
  if (retopoStrokeActive && retopoHasLast && glm::length(local - retopoLast) > 0.5f) moved = true;
  bool released = retopoStrokeActive && !ImGui::IsMouseDown(ImGuiMouseButton_Left);
  if (!retopoStrokeActive || !hovered || !inside || !(moved || clicked)) {
    if (released) retopoEndStroke();
    return;
  }
  float rpx = std::max(2.f, engine.painter.brush.size * 0.5f);
  // dabs along the way from the last frame's position, so a quick stroke has no gaps
  std::vector<vec2> dabs;
  if (clicked || !retopoHasLast) dabs.push_back(local);
  else {
    float dist = glm::length(local - retopoLast);
    int n = std::max(1, (int)std::ceil(dist / (rpx * 0.5f)));
    for (int i = 1; i <= n; ++i) dabs.push_back(glm::mix(retopoLast, local, float(i) / n));
  }
  retopoLast = local, retopoHasLast = true;
  // vertex neighbors, once a mesh
  if (retopoVertAdjStart.size() != m.positions.size() + 1) {
    retopoVertAdjStart.assign(m.positions.size() + 1, 0);
    for (auto& e : m.edges) retopoVertAdjStart[e.x + 1]++, retopoVertAdjStart[e.y + 1]++;
    for (size_t v = 0; v < m.positions.size(); ++v) retopoVertAdjStart[v + 1] += retopoVertAdjStart[v];
    retopoVertAdj.resize(retopoVertAdjStart.back());
    std::vector<uint32_t> at(retopoVertAdjStart.begin(), retopoVertAdjStart.end() - 1);
    for (auto& e : m.edges) retopoVertAdj[at[e.x]++] = e.y, retopoVertAdj[at[e.y]++] = e.x;
    retopoStamp.assign(m.positions.size(), 0), retopoStampId = 0;
  }
  const Camera cam = engine.objectCamera();  // (1.2) the active object's units
  float pw = cam.pixelWorld(lastViewportSize.y);
  const float want = ctrl ? 1.f : retopoDensityValue;
  int axes = (engine.painter.symmetry[0] ? 1 : 0) | (engine.painter.symmetry[1] ? 2 : 0) | (engine.painter.symmetry[2] ? 4 : 0);
  bool changed = false;
  std::vector<uint32_t> stack;
  auto paint = [&](uint32_t face, const vec3& center, float R) {
    if (++retopoStampId == 0) std::fill(retopoStamp.begin(), retopoStamp.end(), 0), retopoStampId = 1;
    stack.clear();
    for (uint32_t l = m.faceStart[face]; l < m.faceStart[face + 1]; ++l) {
      uint32_t v = m.loopVert[l];
      if (retopoStamp[v] != retopoStampId) retopoStamp[v] = retopoStampId, stack.push_back(v);
    }
    while (!stack.empty()) {
      uint32_t v = stack.back();
      stack.pop_back();
      float d = glm::length(m.positions[v] - center);
      if (d > R) continue;
      float t = std::clamp((d - 0.5f * R) / (0.5f * R), 0.f, 1.f), w = 1.f - t * t * (3 - 2 * t);
      float nd = S.density[v] + (want - S.density[v]) * w;
      if (std::abs(nd - want) < 0.01f) nd = want;
      if (nd != S.density[v]) S.density[v] = nd, changed = true;
      for (uint32_t i = retopoVertAdjStart[v]; i < retopoVertAdjStart[v + 1]; ++i) {
        uint32_t u = retopoVertAdj[i];
        if (retopoStamp[u] != retopoStampId) retopoStamp[u] = retopoStampId, stack.push_back(u);
      }
    }
  };
  for (vec2 at : dabs) {
    RayHit c = engine.pick(at);
    if (!c.hit) continue;
    float R = rpx * (cam.ortho ? pw : pw * c.t);
    paint(c.face, c.position, R);
    // mirror symmetry: the same across the planes through the model's middle
    for (int sub = 1; sub < 8; ++sub) {
      if ((sub & axes) != sub) continue;
      vec3 mp = retopo::mirrorPoint(c.position, sub, S.symCenter);
      RayHit h = engine.doc.bvh.closestPoint(m, mp, R);
      if (h.hit) paint(h.face, h.position, R);
    }
  }
  if (changed) ++S.densityVersion, engine.retopoChanged();
  if (released) retopoEndStroke();
}

// the stroke being drawn (as the pen went, on screen) and where a zone line would snap
void App::retopoOverlay(ImVec2 origin, ImVec2 avail) {
  if (!engine.features.on("retopology") || !engine.doc.hasMesh() || !retopoInUse()) return;
  // with Loops on: the edge loop of the quads under the pointer, lit up all along
  {
    const auto& vo = engine.viewOptions;
    bool want = vo.retopoLoops && vo.retopoShowQuads && !retopoStrokeActive;
    ImVec2 mp = ImGui::GetMousePos();
    vec2 local(mp.x - origin.x, mp.y - origin.y);
    bool inside = local.x >= 0 && local.y >= 0 && local.x < avail.x && local.y < avail.y && ImGui::IsWindowHovered();
    // (only while the pointer is over the viewport: one lit up from a script stays until then)
    if (want && inside) {
      RayHit h = engine.pick(local);
      retopoHoverEdges = engine.retopoHoverLoop(h.hit ? &h.position : nullptr);
    } else if (!want) {
      retopoHoverEdges = 0;
    }
  }
  if (tool != ViewportTool::ZoneLine && tool != ViewportTool::Flow) return;
  const bool lines = tool == ViewportTool::ZoneLine;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  ImU32 col = lines ? IM_COL32(255, 205, 50, 255) : IM_COL32(120, 230, 255, 255);
  if (retopoStrokeScreen.size() >= 2) {
    std::vector<ImVec2> sp;
    for (auto& p : retopoStrokeScreen) sp.push_back(ImVec2(origin.x + p.x, origin.y + p.y));
    dl->AddPolyline(sp.data(), (int)sp.size(), IM_COL32(0, 0, 0, 150), 0, 5.f);
    dl->AddPolyline(sp.data(), (int)sp.size(), col, 0, 3.f);
  }
  if (!lines || !ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) return;
  ImVec2 mp = ImGui::GetMousePos();
  vec2 local(mp.x - origin.x, mp.y - origin.y);
  if (local.x < 0 || local.y < 0 || local.x > avail.x || local.y > avail.y) return;
  // Shift: the line right round the part a click would draw, once the pointer rests there (finding it
  // takes up to a fifth of a second on a big model)
  if (ImGui::GetIO().KeyShift && !ImGui::GetIO().KeyCtrl && !retopoStrokeActive) {
    // Shift+wheel: round the part, or the cut facing you (from the one shown to the other)
    if (ImGui::GetIO().MouseWheel != 0.f) {
      ringMode = ringPreviewUsed == 2 ? 1 : 2;
      ringPreviewDone = false, ringPreview.clear(), ringPreviewSince = 0;
    }
    if (glm::length(local - ringPreviewAt) > 3.f) {
      ringPreviewAt = local, ringPreviewSince = nowSeconds(), ringPreviewDone = false, ringPreview.clear();
    } else if (!ringPreviewDone && nowSeconds() - ringPreviewSince > 0.12) {
      ringPreviewDone = true;
      RayHit h = engine.pick(local);
      if (h.hit) ringPreview = engine.retopoRingLoop(h.position, ringMode, nullptr, nullptr, &ringPreviewUsed);
    }
    if (ringPreview.size() >= 3) {
      float aspect = lastViewportSize.x / std::max(lastViewportSize.y, 1.f);
      mat4 vpm = engine.objectCamera().viewProj(aspect);
      std::vector<ImVec2> sp;
      for (auto& p : ringPreview) {
        vec4 c = vpm * vec4(p, 1.f);
        if (c.w <= 1e-6f) continue;
        sp.push_back(ImVec2(origin.x + (c.x / c.w * 0.5f + 0.5f) * avail.x, origin.y + (0.5f - c.y / c.w * 0.5f) * avail.y));
      }
      if (sp.size() >= 3) {
        dl->AddPolyline(sp.data(), (int)sp.size(), IM_COL32(0, 0, 0, 110), ImDrawFlags_Closed, 4.f);
        dl->AddPolyline(sp.data(), (int)sp.size(), IM_COL32(255, 205, 50, 170), ImDrawFlags_Closed, 2.f);
        ImVec2 lo = sp[0], hi = sp[0];
        for (auto& q : sp) lo = ImMin(lo, q), hi = ImMax(hi, q);
        engine.uiRects["retopo.ring_preview"] = {lo.x, lo.y, hi.x - lo.x, hi.y - lo.y};  // (for tests)
      }
    }
    if (ringPreview.size() < 3) engine.uiRects.erase("retopo.ring_preview");
    // what a click would draw, by the pointer
    if (ringPreviewDone) {
      const char* what = ringPreview.size() < 3 ? (ringPreviewUsed == 2 ? T_("Nothing facing you here (Shift+wheel: round the part)")
                                                                         : T_("No part to go round here (Shift+wheel: facing you)"))
                         : ringPreviewUsed == 2 ? T_("Facing you (Shift+wheel: round the part)")
                                                : T_("Round the part (Shift+wheel: facing you)");
      ImVec2 at(origin.x + local.x + 16, origin.y + local.y + 14), sz = ImGui::CalcTextSize(what);
      dl->AddRectFilled(ImVec2(at.x - 4, at.y - 2), ImVec2(at.x + sz.x + 4, at.y + sz.y + 2), IM_COL32(0, 0, 0, 160), 3.f);
      dl->AddText(at, IM_COL32(255, 255, 255, 230), what);
    }
    return;
  }
  if (!ringPreview.empty()) engine.uiRects.erase("retopo.ring_preview");
  ringPreview.clear(), ringPreviewAt = vec2(-1e9f), ringMode = 0, ringPreviewUsed = 0;
  // closing a loop, or meeting a line
  if (retopoStrokeActive && retopoStrokeScreen.size() > 4 && glm::length(local - retopoStrokeScreen.front()) < 12.f) {
    vec2 s0 = retopoStrokeScreen.front();
    dl->AddCircle(ImVec2(origin.x + s0.x, origin.y + s0.y), 8.f, IM_COL32(255, 255, 255, 230), 0, 2.f);
    return;
  }
  vec3 snapped;
  if (retopoSnap(local, 12.f, snapped)) {
    float aspect = lastViewportSize.x / std::max(lastViewportSize.y, 1.f);
    vec4 c = engine.objectCamera().viewProj(aspect) * vec4(snapped, 1.f);
    if (c.w > 1e-6f)
      dl->AddCircle(ImVec2(origin.x + (c.x / c.w * 0.5f + 0.5f) * avail.x, origin.y + (0.5f - c.y / c.w * 0.5f) * avail.y), 7.f,
                    IM_COL32(255, 255, 255, 230), 0, 2.f);
  }
}

void App::retopoEndStroke() {
  retopoStrokeActive = false;
  retopoHasLast = false;
  auto& S = engine.retopo();
  // one undo step per stroke, holding only what changed
  struct Diff {
    std::vector<uint32_t> verts;
    std::vector<float> before, after;
  };
  auto d = std::make_shared<Diff>();
  if (retopoDensityBefore.size() == S.density.size())
    for (uint32_t v = 0; v < S.density.size(); ++v)
      if (retopoDensityBefore[v] != S.density[v]) d->verts.push_back(v), d->before.push_back(retopoDensityBefore[v]), d->after.push_back(S.density[v]);
  retopoDensityBefore.clear(), retopoDensityBefore.shrink_to_fit();
  if (d->verts.empty()) return;
  auto e = std::make_unique<LambdaEntry>();
  e->label = "Density brush";
  e->size = d->verts.size() * 12;
  uint32_t verts = (uint32_t)S.density.size();
  auto apply = [this, d, verts](bool after) {
    auto& R = engine.retopo();
    if (R.density.size() != verts) return;  // another mesh since
    for (size_t i = 0; i < d->verts.size(); ++i) R.density[d->verts[i]] = after ? d->after[i] : d->before[i];
    ++R.densityVersion;
    engine.retopoChanged();
  };
  e->onUndo = [apply](Document&) { apply(false); };
  e->onRedo = [apply](Document&) { apply(true); };
  engine.doc.undo.push(std::move(e));
}

bool App::retopoMakingQuads() {
  auto& S = engine.retopo();
  return engine.retopoBuilding() && (!S.buildIsPreview || S.fullPending);
}

void App::retopoBuild() {
  if (retopoMakingQuads()) return;
  // (0.37) the quads asked for show in the 3D view
  if (engine.startRetopoBuild(30)) setStatus(T_("Making quads...")), engine.viewOptions.retopoPreview = true;
}

bool App::retopoPreviewOn() const {
  return engine.features.on("retopo_preview_toggle") ? engine.viewOptions.retopoPreview : retopoShown;
}

// what the 3D view shows: the Retopology window's Show part, and the popup of the top bar's Retopology arrow
void App::retopoShowSettings(bool inBar) {
  auto& S = engine.retopo();
  ViewportOptions& vo = engine.viewOptions;
  const bool toggle = engine.features.on("retopo_preview_toggle");
  bool changed = false;
  if (toggle) {
    ImGui::Checkbox(L_("In the 3D view##retopo_preview"), &vo.retopoPreview);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", T_("Show these in the 3D view whatever tool is chosen (the Retopology preview button over the view).\n"
                                 "The retopology tools show what they set anyway."));
  }
  // (not "Density" alone: the Density tool's button has that ID)
  changed |= ImGui::RadioButton(L_("Zones##show"), &vo.retopoView, 1);
  ImGui::SameLine();
  changed |= ImGui::RadioButton(L_("Density##show"), &vo.retopoView, 2);
  ImGui::SameLine();
  changed |= ImGui::RadioButton(L_("Neither##show"), &vo.retopoView, 0);
  ImGui::TextDisabled("%s", T_("(the tools show what they set)"));
  changed |= ImGui::Checkbox(L_("The quads"), &vo.retopoShowQuads);
  ImGui::SameLine();
  ImGui::BeginDisabled(!vo.retopoShowQuads);
  changed |= ImGui::Checkbox(L_("Alone"), &vo.retopoQuadsOnly);
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T_("The quads shaded on their own, without the model"));
  ImGui::SameLine();
  changed |= ImGui::Checkbox(L_("Red marks"), &vo.retopoShowMarks);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", T_("Red crosses mark spots worth a look. The notes under the last build say what they are:\n"
                               "- parts too thin for quads this size, so they are left out (fingers, strands of hair)\n"
                               "- places where zones meet in a tight spot, patched\n"
                               "- with Mirror on, places where the two sides of the model aren't the same\n"
                               "They change nothing in the quads: switching them off only hides them."));
  ImGui::SameLine();
  changed |= ImGui::Checkbox(L_("Loops"), &vo.retopoLoops);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", T_("The poles as small crosses (blue: three edges, orange: five, red: more), and the edge loop\n"
                               "under the pointer lit up all along, so you can see where each loop goes and where it ends."));
  ImGui::EndDisabled();
  // (choosing what shows is asking to see it)
  if (changed && toggle) vo.retopoPreview = true;
  if (vo.retopoLoops && vo.retopoShowQuads && S.result && S.loops) {
    int p3 = 0, p5 = 0, pm = 0;
    for (int n : S.loops->poleEdges) (n == 3 ? p3 : n == 5 ? p5 : pm)++;
    ImGui::TextDisabled(T_("Poles: %d with three edges (blue), %d with five (orange), %d other (red)"), p3, p5, pm);
    if (retopoHoverEdges > 0)
      ImGui::TextDisabled(S.hoverLoop >= 0 && S.loops->closed[S.hoverLoop] ? T_("The loop under the pointer: %d edges, all the way round")
                                                                           : T_("The loop under the pointer: %d edges"),
                          retopoHoverEdges);
    else ImGui::TextDisabled("%s", T_("Point at the quads to light up an edge loop"));
  }
  if (inBar) {
    if (S.result && S.result->low.faceCount())
      ImGui::TextDisabled(S.resultIsPreview ? T_("%u quads (a quick preview)") : T_("%u quads"), (unsigned)S.result->low.faceCount());
    else ImGui::TextDisabled("%s", T_("No quads yet."));
    ImGui::Separator();
    ImGui::BeginDisabled(retopoMakingQuads() || !engine.doc.hasMesh());
    if (ImGui::Button(retopoMakingQuads() ? L_("Making quads...") : L_("Make quads"))) retopoBuild();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(L_("Retopology window..."))) showRetopoWindow(), ImGui::CloseCurrentPopup();
  }
}

void App::retopoMenu() {
  if (!engine.features.on("retopology")) return;
  if (!menuHeader(engine, "retopology", L_("Retopology"))) return;
  Document& d = engine.doc;
  if (ImGui::MenuItem(L_("Retopology Window..."), nullptr, showRetopo && retopoShown)) {
    if (showRetopo && retopoShown) showRetopo = false;
    else openRetopo();  // (open, or its tab brought to the front)
  }
  if (engine.features.on("retopo_preview_toggle") &&
      ImGui::MenuItem(L_("Retopology Preview"), keyLabel("view.retopo_preview"), engine.viewOptions.retopoPreview, d.hasMesh()))
    engine.viewOptions.retopoPreview = !engine.viewOptions.retopoPreview;
  ImGui::Separator();
  auto toolItem = [&](const char* label, const char* key, ViewportTool t, const char* tip) {
    if (ImGui::MenuItem(label, keyLabel(key), tool == t, d.hasMesh())) tool = t, showRetopoWindow();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
  };
  toolItem(L_("Zone Lines"), "tool.zone_line", ViewportTool::ZoneLine,
           T_("Draw where the edge flow changes. Each line becomes an edge loop and splits the model into zones."));
  toolItem(L_("Zones"), "tool.patch", ViewportTool::Patch, T_("Click a zone to set its resolution"));
  toolItem(L_("Flow Strokes"), "tool.flow", ViewportTool::Flow, T_("A stroke inside a zone sets which way its quads run"));
  toolItem(L_("Density Brush"), "tool.density", ViewportTool::Density, T_("Fine-tune the resolution where you paint"));
  ImGui::Separator();
  auto& S = engine.retopo();
  if (ImGui::MenuItem(retopoMakingQuads() ? L_("Making Quads...") : L_("Make Quads"), nullptr, false, d.hasMesh() && !retopoMakingQuads()))
    retopoBuild();
  bool built = S.result && S.result->low.faceCount() > 0;
  if (ImGui::MenuItem(L_("Export the Quads (.obj)..."), nullptr, false, built)) openFileDialog(DialogPurpose::RetopoExport);
  if (ImGui::MenuItem(L_("Send the Quads to Blender"), nullptr, false, built && d.bridge.active)) {
    json r;
    if (runCommand("retopo_send_to_blender", json::object(), &r))
      setStatus(F_("Sent the quads to Blender as %s", r.value("object", std::string()).c_str()));
  }
  if (ImGui::MenuItem(L_("Use the Quads as the Model..."), nullptr, false, built)) showRetopoWindow(), retopoAskUse = true;
  ImGui::Separator();
  if (ImGui::MenuItem(L_("Clear Zone Lines"), nullptr, false, !S.lines.empty())) engine.retopoSetLines({}, "Clear zone lines");
  if (ImGui::MenuItem(L_("Clear Flow Strokes"), nullptr, false, !S.strokes.empty())) {
    auto before = S.strokes;
    S.strokes.clear();
    pushStrokesUndo(engine, before, "Clear flow strokes");
    engine.retopoChanged();
  }
  if (ImGui::MenuItem(L_("Reset Resolutions and Density"), nullptr, false, d.hasMesh())) {
    S.zoneSeeds.clear();
    std::fill(S.density.begin(), S.density.end(), 1.f);
    ++S.densityVersion;
    engine.retopoChanged();
  }
  ImGui::EndMenu();
}

// the chosen zone's resolution, as a slider (a whole drag is one undo step)
static void zoneResolutionSlider(Engine& engine, uint32_t face, float width, std::vector<std::pair<uint32_t, float>>& before) {
  auto& S = engine.retopo();
  if (face >= engine.doc.mesh.faceCount() || S.selectedZone <= 0) return;
  float r = engine.retopoZoneResolution(S.selectedZone);
  ImGui::SetNextItemWidth(width);
  bool changed = ImGui::SliderFloat("##zoneres", &r, 0.25f, 32.f, T_("Zone resolution x%.2g"), ImGuiSliderFlags_Logarithmic);
  {
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    // (the window's ID: its name is the translated one)
    engine.uiRects[ImGui::GetCurrentWindow()->RootWindow->ID == ImHashStr("Retopology") ? "retopo.zone_resolution" : "retopo.bar.zone_resolution"] = {
        a.x, a.y, b.x - a.x, b.y - a.y};
  }
  if (ImGui::IsItemActivated()) before = S.zoneSeeds;
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", T_("How many quads per m² this zone gets, compared with the rest (2: twice as many). Its quads\n"
                               "change size at the zone's lines, in a row of three-to-one steps."));
  if (changed) {
    engine.execute("retopo_zone", {{"face", face}, {"resolution", r}});
    // with the Sym buttons, the zones the mirror images of this one's point lie in too
    int axes = engine.retopoSymmetryAxes();
    const Mesh& m = engine.doc.mesh;
    for (int sub = 1; sub < 8; ++sub) {
      if ((sub & axes) != sub) continue;
      RayHit h = engine.doc.bvh.closestPoint(m, retopo::mirrorPoint(m.faceCenter(face), sub, S.symCenter), 1e30f);
      if (h.hit && engine.retopoZones()[h.face] != engine.retopoZones()[face])
        engine.execute("retopo_zone", {{"face", h.face}, {"resolution", r}});
    }
  }
  if (ImGui::IsItemDeactivatedAfterEdit() && before != S.zoneSeeds) {
    auto after = S.zoneSeeds;
    uint32_t faces = S.faces;
    auto e = std::make_unique<LambdaEntry>();
    e->label = "Zone resolution";
    e->size = (before.size() + after.size()) * 8;
    e->onUndo = [&engine, b = before, faces](Document&) {
      auto& R = engine.retopo();
      if (R.faces == faces) R.zoneSeeds = b, engine.retopoChanged();
    };
    e->onRedo = [&engine, after, faces](Document&) {
      auto& R = engine.retopo();
      if (R.faces == faces) R.zoneSeeds = after, engine.retopoChanged();
    };
    engine.doc.undo.push(std::move(e));
  }
}

void App::retopoContextBar() {
  if (!retopoTool()) return;
  ImGui::SameLine();
  auto& S = engine.retopo();
  if (tool == ViewportTool::Density) {
    ImGui::SetNextItemWidth(150 * uiScale);
    ImGui::SliderFloat("##retopo_dens", &retopoDensityValue, 0.25f, 8.f, T_("Paints x%.2g"), ImGuiSliderFlags_Logarithmic);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", T_("How many times the quads per m² the brush paints (2: twice as many quads there, 0.5: half).\n"
                                 "Ctrl paints it back to normal (x1). Zones have a resolution of their own as well."));
  } else if (tool == ViewportTool::Patch) {
    if (S.selectedZone > 0) {
      ImGui::TextDisabled(T_("Zone %d"), S.selectedZone);
      ImGui::SameLine();
      zoneResolutionSlider(engine, retopoZoneFace, 190 * uiScale, retopoSeedsBefore);
    } else {
      ImGui::TextDisabled("%s", T_("Click a zone to set its resolution."));
    }
  } else if (tool == ViewportTool::ZoneLine) {
    ImGui::TextDisabled("%s", T_("Draw on the model where the flow changes. Start or end on a line to meet it; end at the start to close. Shift+click: a line right round an arm, a leg, the neck. Ctrl+click: take away."));
  } else if (tool == ViewportTool::Flow) {
    if (ImGui::Checkbox(L_("Strokes make edge loops##bar"), &S.flowLoops)) engine.retopoChanged();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T_(kFlowLoopsTip));
    ImGui::SameLine();
    ImGui::TextDisabled("%s", S.flowLoops ? T_("Draw where edge loops run in a zone. Ctrl+click a stroke to take it away.")
                                          : T_("Draw which way the quads run in a zone. Ctrl+click a stroke to take it away."));
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(retopoMakingQuads());
  const float fh = ImGui::GetFrameHeight();
  if (iconButton("retopo.make", "quads", retopoMakingQuads() ? T_("Making quads...") : T_("Make quads"), ui::IconState::Normal, fh,
                 T_("Make the quads now (they also follow each change as a quick preview)"), nullptr, true))
    retopoBuild();
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (iconButton("retopo.window", "more", T_("Retopology window"), ui::IconState::Normal, fh, T_("All the retopology settings"))) showRetopoWindow();
}

void App::drawRetopoWindow() {
  if (!engine.features.on("retopology")) return;
  // the result of a build finishing while the window is closed still shows (and says so)
  static uint64_t lastResult = 0;
  auto& S = engine.retopo();
  if (S.resultVersion != lastResult && S.result) {
    lastResult = S.resultVersion;
    if (!S.resultIsPreview) {  // (a preview says nothing: it comes after every change)
      const auto& r = *S.result;
      char b[200];
      std::snprintf(b, sizeof b, T_("%d quads in %.1f s: %.1f%% of the vertices have four edges"), r.quads, r.seconds,
                    r.quality.pct(r.quality.regular));
      setStatus(b);
    }
  }
  // the live preview runs while the quads show in the 3D view (a retopology tool, the top bar's Retopology
  // button) or the window is on screen (not a tab behind another: its numbers follow)
  retopoShown = false;
  if (retopoTool() || (retopoPreviewOn() && engine.viewOptions.retopoShowQuads)) S.wantPreviewAt = nowSeconds();
  if (!showRetopo || blenderOn()) return;  // (1.2.7: the Blender layout has it as a Properties tab)
  ImGui::SetNextWindowSize(ImVec2(400 * uiScale, 760 * uiScale), ImGuiCond_FirstUseEver);
  // first time: a tab beside Properties (a layout from before it was docked there), else at the right; that
  // node keeps the tab it showed unless the window was asked for
  ImGuiDockNode* besideNode = nullptr;
  ImGuiID keepTab = 0;
  const bool placed = ImGui::FindWindowByName("Retopology") || ImGui::FindWindowSettingsByID(ImHashStr("Retopology"));
  if (ImGuiWindow* beside = ImGui::FindWindowByName("Properties"); !placed && beside && beside->DockId) {
    ImGui::SetNextWindowDockID(beside->DockId, ImGuiCond_FirstUseEver);
    besideNode = ImGui::DockBuilderGetNode(beside->DockId);
    if (besideNode && besideNode->TabBar && !retopoFocus) keepTab = besideNode->TabBar->SelectedTabId;
  } else if (!placed) {
    ImVec2 vs = ImGui::GetMainViewport()->WorkSize, vp0 = ImGui::GetMainViewport()->WorkPos;
    ImGui::SetNextWindowPos(ImVec2(vp0.x + vs.x - 410 * uiScale, vp0.y + 30 * uiScale), ImGuiCond_FirstUseEver);
  }
  if (retopoFocus) ImGui::SetNextWindowFocus(), retopoFocus = false;
  // (not brought to the front by appearing: at the start it stays a tab behind Properties, so nothing of
  // retopology runs or shows until it is chosen)
  const int tabColors = pushPanelColors(L_("Retopology"));  // (0.47: its tab in the Model step's color)
  const bool open = ImGui::Begin(L_("Retopology"), &showRetopo, ImGuiWindowFlags_NoFocusOnAppearing);
  ImGui::PopStyleColor(tabColors);
  if (keepTab && besideNode && besideNode->TabBar) besideNode->TabBar->NextSelectedTabId = keepTab;
  if (!open) {
    ImGui::End();
    return;
  }
  retopoShown = !keepTab;
  if (retopoShown) S.wantPreviewAt = nowSeconds();
  retopoPanel();
  ImGui::End();
}

void App::retopoPanel() {
  // (1.2.7) the window's content: also the Blender layout's Retopology tab
  auto& S = engine.retopo();
  Document& d = engine.doc;
  if (!d.hasMesh()) {
    ImGui::TextWrapped("%s", T_("Open a model first."));
    return;
  }
  const Mesh& m = d.mesh;
  const auto& zones = engine.retopoZones();
  if (retopoStatsVersion != S.version && !retopoStrokeActive) {  // not every frame of a stroke
    retopoStatsVersion = S.version;
    retopoPatchCount = S.zoneCount;
  }
  // (0.22) everything but the Make quads row scrolls: that row stays at the foot of the window
  const float footerH = ImGui::GetFrameHeightWithSpacing() + ImGui::GetTextLineHeightWithSpacing() + 2 * ImGui::GetStyle().ItemSpacing.y + 2;
  ImGui::BeginChild("##retopo_body", ImVec2(0, -footerH));
  ImGui::TextWrapped("%s", T_("Patina makes the quads along the model's shape. Draw zone lines where the edge flow should change; "
                              "give a zone more or fewer quads; stroke the way its quads should run. The more you set, the "
                              "closer they follow you."));
  ImGui::Spacing();
  // ---- density
  ImGui::SeparatorText(T_("Resolution"));
  ImGui::SetNextItemWidth(200 * uiScale);
  if (ImGui::DragFloat(L_("Quads per m²"), &S.quadsPerM2, std::max(1.f, S.quadsPerM2 * 0.01f), 10.f, 2000000.f, "%.0f",
                       ImGuiSliderFlags_Logarithmic))
    engine.retopoChanged();
  float edge = 1.f / std::sqrt(std::max(S.quadsPerM2, 1e-3f));
  const double asked = retopoStrokeActive ? S.quadsAsked : engine.retopoQuadsAsked();  // (not every frame of a stroke)
  ImGui::TextDisabled(T_("Quads about %.2g cm across; about %.0f quads over the model"), edge * 100.f, asked);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", T_("Model units are meters. Zones and the density brush raise or lower it where they are\n"
                               "(the count includes them)."));
  if (asked > Engine::kRetopoPreviewMaxQuads)
    ImGui::TextColored(ImVec4(1.f, 0.7f, 0.3f, 1.f), "%s",
                       T_("That's a great many quads: no live preview, and Make quads may take minutes and many GB of memory.\n"
                          "Check the zones' resolutions (Zone tool) and Quads per m²."));
  // ---- tools
  ImGui::SeparatorText(T_("Zones and flow"));
  auto toolButton = [&](const char* label, ViewportTool t, const char* tip) {
    bool on = tool == t;
    if (on) ImGui::PushStyleColor(ImGuiCol_Button, ui::theme().accentDeep);
    if (ImGui::Button(label)) tool = t;
    if (on) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
  };
  toolButton(L_("Lines"), ViewportTool::ZoneLine,
             T_("Zone lines: draw on the model where the edge flow should change (round the eyes, the mouth, the\n"
                "cranium, the base of the ears). Each line becomes an edge loop; closed ones split off zones.\n"
                "A line that starts or ends near another (a white ring shows) meets it exactly; one that ends\n"
                "where it began closes. Shift+click draws a line right round the part there (an arm, a leg, the neck);\n"
                "where there is no part to go round (a breast, the face), the cut through that point facing you.\n"
                "Shift+wheel switches between the two (hold Shift and rest the pointer to see the line first).\n"
                "Ctrl+click takes a line away. Ctrl+Shift+click confirms a line (or unconfirms it): confirmed lines are\n"
                "darker and new lines don't snap to them."));
  ImGui::SameLine();
  toolButton(L_("Zone"), ViewportTool::Patch, T_("Click a zone to choose it and set its resolution"));
  ImGui::SameLine();
  toolButton(L_("Flow"), ViewportTool::Flow,
             T_("Flow strokes: draw inside a zone where its edge loops should run. Each stroke becomes an edge loop but\n"
                "doesn't make a zone. With Strokes make edge loops unticked, strokes only turn the quads their way (the\n"
                "quads keep their size, so a circle of flow round a small spot gets poles). Ctrl+click a stroke to take it away."));
  ImGui::SameLine();
  toolButton(L_("Density"), ViewportTool::Density, T_("Fine-tune the resolution where you paint (Ctrl: back to normal)"));
  if (tool == ViewportTool::Density) {
    ImGui::SetNextItemWidth(200 * uiScale);
    ImGui::SliderFloat(L_("Paints"), &retopoDensityValue, 0.25f, 8.f, "x%.2g", ImGuiSliderFlags_Logarithmic);
  }
  if (tool == ViewportTool::Flow) {
    if (ImGui::Checkbox(L_("Strokes make edge loops"), &S.flowLoops)) engine.retopoChanged();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T_(kFlowLoopsTip));
  }
  engine.retopoZones();
  ImGui::TextDisabled(T_("%zu lines, %d zones, %zu flow strokes"), S.lines.size(), S.zoneCount, S.strokes.size());
  if (S.selectedZone > 0) {
    ImGui::Text(T_("Zone %d"), S.selectedZone);
    ImGui::SameLine();
    zoneResolutionSlider(engine, retopoZoneFace, 220 * uiScale, retopoSeedsBefore);
  } else {
    ImGui::TextDisabled("%s", T_("With the Zone tool, click a zone to set its resolution."));
  }
  // (not "##zoneres": that is the slider's ID, and the list appearing mid-drag would take it over)
  if (!S.zoneSeeds.empty() && ImGui::TreeNode("##zoneslist", T_("Zones with their own resolution (%zu)"), S.zoneSeeds.size())) {
    for (size_t i = 0; i < S.zoneSeeds.size(); ++i) {
      auto [f, r] = S.zoneSeeds[i];
      int z = f < zones.size() ? zones[f] : 0;
      ImGui::PushID((int)i);
      if (ImGui::SmallButton(L_("Choose"))) S.selectedZone = z, retopoZoneFace = f, tool = ViewportTool::Patch;
      ImGui::SameLine();
      ImGui::Text(T_("Zone %d: x%.2g"), z, r);
      ImGui::PopID();
    }
    ImGui::TreePop();
  }
  if (ImGui::Button(L_("Clear lines")) && !S.lines.empty()) engine.retopoSetLines({}, "Clear zone lines");
  ImGui::SameLine();
  if (ImGui::Button(L_("Clear strokes"))) {
    auto before = S.strokes;
    S.strokes.clear();
    pushStrokesUndo(engine, before, "Clear flow strokes");
    engine.retopoChanged();
  }
  ImGui::SameLine();
  if (ImGui::Button(L_("Reset resolutions"))) {
    auto seeds = S.zoneSeeds;
    auto dens = S.density;
    uint32_t faces = S.faces;
    S.zoneSeeds.clear();
    std::fill(S.density.begin(), S.density.end(), 1.f);
    engine.retopoChanged();
    auto e = std::make_unique<LambdaEntry>();
    e->label = "Reset resolutions";
    e->size = seeds.size() * 8 + dens.size() * 4;
    Engine* eng = &engine;
    e->onUndo = [eng, seeds, dens, faces](Document&) {
      auto& R = eng->retopo();
      if (R.faces == faces) R.zoneSeeds = seeds, R.density = dens, eng->retopoChanged();
    };
    e->onRedo = [eng, faces](Document&) {
      auto& R = eng->retopo();
      if (R.faces == faces) R.zoneSeeds.clear(), std::fill(R.density.begin(), R.density.end(), 1.f), eng->retopoChanged();
    };
    engine.doc.undo.push(std::move(e));
  }
  // ---- 0.22: confirmed lines, snapping, lines behind the model
  {
    size_t done = 0;
    for (auto& l : S.lines) done += l.confirmed;
    ImGui::BeginDisabled(done == S.lines.size());
    if (ImGui::Button(L_("Confirm lines"))) engine.retopoSetConfirmed(std::vector<char>(S.lines.size(), 1), "Confirm zone lines");
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
      ImGui::SetTooltip("%s", T_("Confirms every line drawn so far: they turn darker and new lines don't snap to them (draw\n"
                                 "close to them in tight folds without being pulled on). They still make zones and edge loops.\n"
                                 "Ctrl+Shift+click a line with the Lines tool to confirm or unconfirm just that one."));
    ImGui::SameLine();
    ImGui::BeginDisabled(done == 0);
    if (ImGui::Button(L_("Unconfirm all"))) engine.retopoSetConfirmed(std::vector<char>(S.lines.size(), 0), "Unconfirm zone lines");
    ImGui::EndDisabled();
    if (done) {
      ImGui::SameLine();
      ImGui::TextDisabled(T_("%zu confirmed"), done);
    }
    ImGui::Checkbox(L_("Snap line ends"), &engine.retopoSnapEnds);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", T_("A new line that starts or ends near another line meets it exactly (a white ring shows where).\n"
                                 "Off: the ends stay where you draw them, for tight folds (lips, fingers, toes, the crotch).\n"
                                 "Confirmed lines are never snapped to. A line still closes when it ends where it began."));
    const char* behind[] = {N_("Shown"), N_("Darkened"), N_("Hidden")};
    int& lb = engine.viewOptions.retopoLinesBehind;
    ImGui::SetNextItemWidth(130 * uiScale);
    if (ImGui::BeginCombo(L_("Lines behind the model"), T_(behind[std::clamp(lb, 0, 2)]))) {
      for (int i = 0; i < 3; ++i)
        if (ImGui::Selectable(L_(behind[i]), lb == i)) lb = i;
      ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", T_("How the lines and strokes on the far side of the model are drawn: shown as if the model\n"
                                 "were glass, darkened (thin and faint), or hidden."));
  }
  bool thinClicked = ImGui::Button(L_("Zones for thin parts"));
  {
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    engine.uiRects["retopo.thin_zones"] = {a.x, a.y, b.x - a.x, b.y - a.y};
  }
  if (thinClicked) {
    json r;
    if (runCommand("retopo_thin_zones", json::object(), &r)) {
      int given = r.value("zones_given", 0), left = r.value("left_too_thin", 0) + r.value("left_no_clean_cut", 0);
      std::string msg = T_("No thin part could be given a zone");
      if (given) {
        char b[200];
        std::snprintf(b, sizeof b, P_("Gave %d thin part a zone", "Gave %d thin parts zones", given), given);
        msg = b;
      }
      if (left) msg = F_("%s; %d left (too thin, or no clean ring round them)", msg.c_str(), left);
      setStatus(msg);
    }
  }
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", T_("Finds the parts too thin for the quads (fingers, toes: fewer than about four quads would go\n"
                               "round them), draws a line round each where it meets the rest, and gives it a resolution for\n"
                               "about six quads round. Thin sheets (locks of hair) are left as they are. One undo step."));
  if (ImGui::Checkbox(L_("Rings between two lines"), &S.ringZones)) engine.retopoChanged();
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", T_("A zone between two lines (round an eye or a mouth, along an arm) is filled with rings of quads\n"
                               "that follow both lines, and a zone inside such a ring (an eyeball, the inside of the lips) is\n"
                               "closed with a grid. A band that would need many steps from one count to the other is made with\n"
                               "the zone beside it. Off: all those zones are made as the others are."));
  if (ImGui::Checkbox(L_("Fewer poles"), &S.fewerPoles)) engine.retopoChanged();
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", T_("After the quads are made, the number of quads along each run of edges is worked out again so\n"
                               "that a three-edged and a five-edged pole close together cancel out. The new quads are kept\n"
                               "only when they have fewer poles and no more folded faces. Off: the quads as first made."));
  if (ImGui::Checkbox(L_("Keep quads away from changes"), &S.keepAway)) ++S.version, S.changedAt = nowSeconds();
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", T_("On (the default): a change (a line, a stroke, the density, a zone's resolution) makes only the\n"
                               "quads within a few quads of it again. The rest stay exactly as they were, joined to the new ones\n"
                               "by a row of faces, which can leave a few more poles there. Remake all (beside Make quads) makes\n"
                               "everything again at its best. Off: every change makes all the quads again, and they can come\n"
                               "out different anywhere."));
  // ---- symmetry (the Sym buttons, mirrored here)
  ImGui::SeparatorText(T_("Symmetry"));
  {
    const char* names[3] = {"X", "Y", "Z"};
    for (int i = 0; i < 3; ++i) {
      if (i) ImGui::SameLine();
      // (the ID stays the English "Mirror X")
      std::string lbl = F_("Mirror %s", names[i]) + "###Mirror " + names[i];
      ImGui::Checkbox(lbl.c_str(), &engine.painter.symmetry[i]);
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", T_("Lines, flow strokes, the density brush and zone resolutions are mirrored across the model's\n"
                                 "middle (the same as the Sym buttons). The quads are made on one side (the one with more lines)\n"
                                 "and mirrored onto the other, meeting along an edge loop down the middle. A line that runs\n"
                                 "within half a quad of the middle gives way to that loop there."));
    float dz = engine.retopoDeadzone() * 100.f;
    ImGui::SetNextItemWidth(120 * uiScale);
    if (ImGui::DragFloat(L_("Middle deadzone (cm)"), &dz, 0.05f, 0.f, 0.25f * S.modelSize * 100.f, "%.2f"))
      S.symDeadzone = std::max(0.f, dz / 100.f), ++S.version, S.changedAt = nowSeconds();  // (kept; the quads don't change)
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", T_("A line or stroke that comes this close to the middle is taken to meet it there, and joins its mirror\n"
                                 "image (like the clipping of Blender's mirror modifier). A stroke drawn across the middle keeps\n"
                                 "its longer side, mirrored."));
  }
  // ---- quads (Make quads itself is at the foot of the window)
  ImGui::SeparatorText(T_("Quads"));
  if (S.result) {
    const auto& r = *S.result;
    const auto& q = r.quality;
    ImGui::Text(T_("%d quads, %zu vertices; %.1f s"), r.quads, r.low.positions.size(), r.seconds);
    ImGui::TextDisabled(T_("Poles: %.1f%% with three edges, %.1f%% with five, %.1f%% other; %.0f%% quads"), q.pct(q.three),
                        q.pct(q.five), q.pct(q.other), q.faces ? 100.0 * q.quads / q.faces : 0.0);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", T_("Poles go where the shape needs them, where zones of different resolution meet, and at the\n"
                                 "corners of the loops you draw. On a closed, ball-like piece, the three-edge poles\n"
                                 "outnumber the five-edge ones by eight."));
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    for (auto& p : r.problems) ImGui::TextWrapped("%s", p.c_str());
    ImGui::PopStyleColor();
  } else {
    ImGui::TextDisabled("%s", T_("No quads yet."));
  }
  // ---- what the viewport shows
  ImGui::SeparatorText(T_("Show"));
  retopoShowSettings(false);
  // ---- use the quads
  ImGui::SeparatorText(T_("Use the quads"));
  bool built = S.result && S.result->low.faceCount() > 0;
  ImGui::BeginDisabled(!built);
  if (ImGui::Button(L_("Export .obj..."))) openFileDialog(DialogPurpose::RetopoExport);
  ImGui::SameLine();
  ImGui::BeginDisabled(!d.bridge.active);
  if (ImGui::Button(L_("Send to Blender"))) {
    json r;
    if (runCommand("retopo_send_to_blender", json::object(), &r))
      setStatus(F_("Sent the quads to Blender as %s", r.value("object", std::string()).c_str()));
  }
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    ImGui::SetTooltip("%s", d.bridge.active ? T_("A new object next to the linked one, in the same place (sending again updates it)")
                                            : T_("Link a Blender object first (Send to Patina in Blender)"));
  if (ImGui::Button(L_("Use the quads as the model..."))) retopoAskUse = true;
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", T_("Opens the quads as the model you paint, with new UVs, and bakes this high-poly onto them\n"
                               "(normals, ambient occlusion); the layers come along (a bake-down). Your zones and strokes\n"
                               "stay with the high-poly for next time."));
  ImGui::EndDisabled();
  if (retopoAskUse) ImGui::OpenPopup("##retopo_use"), retopoAskUse = false;
  if (ImGui::BeginPopupModal("##retopo_use", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextUnformatted(T_("Use the quads as the model?"));
    ImGui::Spacing();
    bool down = engine.features.on("bake_down");
    if (down) {
      ImGui::Checkbox(L_("Bring the layers along (bake them down)"), &retopoKeepLayers);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", T_("Every layer moves onto the quads' UVs, measured where the two surfaces meet (Bake > Bake Down)"));
    }
    if (down && retopoKeepLayers)
      ImGui::TextWrapped("%s", T_("The quads are saved as an .obj next to the high-poly and opened with new UVs; the high-poly is "
                                  "baked onto them and every layer comes along. The result is a new project: this one stays as it "
                                  "was saved."));
    else
      ImGui::TextWrapped("%s", d.dirty ? T_("The quads are saved as an .obj next to the high-poly and opened with new UVs; the high-poly is "
                                            "baked onto them. The layers start afresh - save the project first if you want to keep this one.")
                                       : T_("The quads are saved as an .obj next to the high-poly and opened with new UVs; the high-poly is "
                                            "baked onto them. The layers start afresh."));
    if (down && retopoKeepLayers && ImGui::Button(L_("Use them"))) {
      ImGui::CloseCurrentPopup();
      replaceProject(T_("bake the layers down"), [this] {
        bakeDownSource = 1;
        bakeDownNow();
      });
    } else if (!(down && retopoKeepLayers) && ImGui::Button(L_("Use them"))) {
      json r;
      if (runCommand("retopo_use_as_model", {{"keep_layers", false}}, &r)) {
        std::string file = fs::path(r.value("path", std::string())).filename().string();
        if (r.contains("bake_error"))
          setStatus(F_("The quads are the model now (%s); the bake failed: %s", file.c_str(), r["bake_error"].get<std::string>().c_str()));
        else
          setStatus(F_("The quads are the model now (%s), with the high-poly baked onto them", file.c_str()));
        tool = ViewportTool::Paint;
        showRetopo = false;
      }
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(L_("Cancel"))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  ImGui::EndChild();
  // ---- the foot of the window: Make quads, the live preview, and how the quads came out
  ImGui::Separator();
  bool building = engine.retopoBuilding();
  bool full = retopoMakingQuads();
  ImGui::BeginDisabled(full);
  if (ImGui::Button(full ? L_("Making quads...") : L_("Make quads"), ImVec2(160 * uiScale, 0))) retopoBuild();
  {
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    engine.uiRects["retopo.make_quads"] = {a.x, a.y, b.x - a.x, b.y - a.y};
  }
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", T_("Keeps the quads (the live preview is made the same way: when it is up to date, this keeps it\n"
                               "at once; otherwise the quads are made now)"));
  ImGui::SameLine();
  ImGui::BeginDisabled(full || !S.result);
  if (ImGui::Button(L_("Remake all")) && engine.retopoRemakeAll()) setStatus(T_("Making all the quads again..."));
  {
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    engine.uiRects["retopo.remake_all"] = {a.x, a.y, b.x - a.x, b.y - a.y};
  }
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    ImGui::SetTooltip("%s", T_("Makes all the quads again, not only those near what changed (Keep quads away from changes):\n"
                               "the cleanest quads for everything as it is now, though any part may come out different"));
  ImGui::SameLine();
  if (ImGui::Checkbox(L_("Live preview"), &S.livePreview) && !S.livePreview && S.building.valid() && S.buildIsPreview && S.cancel)
    S.cancel->store(true);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", T_("The quads are made again by themselves a moment after you change something (a line, a stroke,\n"
                               "a zone's resolution, the density), and shown faintly. Make quads keeps them."));
  if (building) {
    ImGui::TextDisabled(S.buildIsPreview ? T_("Updating the preview... %.0f s") : T_("Making the quads... %.0f s"), nowSeconds() - S.buildStarted);
  } else if (S.result) {
    const auto& q = S.result->quality;
    bool current = S.resultInputs == S.inputsVersion;
    const char* fmt = S.resultIsPreview ? (current ? T_("Preview: %d quads, %.1f%% four-edged")
                                                   : T_("Preview: %d quads, %.1f%% four-edged (changed since)"))
                                        : (current ? T_("%d quads, %.1f%% four-edged") : T_("%d quads, %.1f%% four-edged (changed since)"));
    ImGui::Text(fmt, S.result->quads, q.pct(q.regular));
    if (S.result->localKept >= 0 && ImGui::IsItemHovered())
      ImGui::SetTooltip(T_("Only the quads near what changed were made again: %d faces kept as they were"), S.result->localKept);
  } else {
    ImGui::TextDisabled("%s", T_("No quads yet"));
  }
  if (!building && S.previewTooBig && S.livePreview)
    ImGui::TextDisabled(T_("No live preview for about %.0f quads: Make quads makes them"), engine.retopoQuadsAsked());
}

}  // namespace patina
