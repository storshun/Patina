// UV / texture editor: shows any map under the UV layout, edits UVs (select,
// move, rotate, scale, unwrap, pack, seams) and paints directly in texture space.
#include <functional>

#include <imgui.h>
#include <imgui_internal.h>

#include "core/Log.h"
#include "core/UVIsland.h"
#include "core/UVOps.h"
#include "ui/App.h"
#include "ui/Theme.h"

namespace patina {

static const char* kUVSources[] = {N_("Base Color"), N_("Roughness"), N_("Metallic"), N_("Height"), N_("Normal"), N_("Emissive"),
                                   N_("AO (baked)"), N_("Curvature (baked)"), N_("Checker"), N_("Layer: Base Color"), N_("Layer: Mask"),
                                   N_("Opacity"), N_("Sheen"), N_("Clearcoat"), N_("Subsurface")};

struct UVFrame {
  vec2 scale, offset;
  ImVec2 origin, size;
  vec2 uvOrigin{0.f};  // texture tiles: the shown tile's UV corner (UDIM), drawn as the view's 0..1
  vec2 toUV(ImVec2 m) const {
    vec2 s((m.x - origin.x) / size.x, 1.f - (m.y - origin.y) / size.y);
    return (s - offset) / scale + uvOrigin;
  }
  ImVec2 toScreen(vec2 uv) const {
    vec2 s = (uv - uvOrigin) * scale + offset;
    return ImVec2(origin.x + s.x * size.x, origin.y + (1.f - s.y) * size.y);
  }
  float uvPerPixel() const { return 1.f / (scale.y * size.y); }
};

// texture tiles: the tile the UV editor shows (-1: no tiles); faces of other tiles are not picked
static int g_uvTile = -1;
static bool faceShown(const Mesh& m, uint32_t f) { return g_uvTile < 0 || m.faceTile(f) == g_uvTile; }
static bool loopShown(const Mesh& m, uint32_t l) {
  if (g_uvTile < 0) return true;
  uint32_t f = uint32_t(std::upper_bound(m.faceStart.begin(), m.faceStart.end(), l) - m.faceStart.begin()) - 1;
  return faceShown(m, f);
}

// the UV triangles by cell (made again when the UVs change), so the face under the pointer is found
// without going through every triangle each frame (two million of them on a sculpt)
namespace {
struct UVTriGrid {
  uint64_t key = 0;
  const Mesh* mesh = nullptr;
  vec2 lo{0}, cell{1};
  int nx = 0, ny = 0;
  std::vector<uint32_t> start, tris;
};
UVTriGrid uvTriGrid;

const UVTriGrid& uvGridFor(const Mesh& m) {
  UVTriGrid& g = uvTriGrid;
  uint64_t key = uv::uvHash(m) ^ (uint64_t(m.tris.size()) << 32);
  if (g.mesh == &m && g.key == key && !g.start.empty()) return g;
  g.mesh = &m, g.key = key;
  const size_t T = m.tris.size();
  vec2 lo(1e30f), hi(-1e30f);
  for (const vec2& uv : m.loopUV) lo = glm::min(lo, uv), hi = glm::max(hi, uv);
  if (T == 0 || m.loopUV.empty()) {
    g.nx = g.ny = 1, g.start.assign(2, 0), g.tris.clear();
    return g;
  }
  vec2 size = glm::max(hi - lo, vec2(1e-6f));
  float cells = std::clamp(float(T) / 4.f, 1.f, 2048.f * 2048.f);
  float side = std::sqrt(size.x * size.y / cells);
  g.nx = std::clamp(int(std::ceil(size.x / side)), 1, 2048), g.ny = std::clamp(int(std::ceil(size.y / side)), 1, 2048);
  g.lo = lo, g.cell = size / vec2(float(g.nx), float(g.ny));
  auto range = [&](uint32_t t, int& x0, int& y0, int& x1, int& y1) {
    const auto& tr = m.tris[t];
    vec2 a = glm::min(glm::min(m.loopUV[tr.x], m.loopUV[tr.y]), m.loopUV[tr.z]);
    vec2 b = glm::max(glm::max(m.loopUV[tr.x], m.loopUV[tr.y]), m.loopUV[tr.z]);
    x0 = std::clamp(int((a.x - g.lo.x) / g.cell.x), 0, g.nx - 1), y0 = std::clamp(int((a.y - g.lo.y) / g.cell.y), 0, g.ny - 1);
    x1 = std::clamp(int((b.x - g.lo.x) / g.cell.x), 0, g.nx - 1), y1 = std::clamp(int((b.y - g.lo.y) / g.cell.y), 0, g.ny - 1);
  };
  g.start.assign(size_t(g.nx) * g.ny + 1, 0);
  for (uint32_t t = 0; t < T; ++t) {
    int x0, y0, x1, y1;
    range(t, x0, y0, x1, y1);
    for (int y = y0; y <= y1; ++y)
      for (int x = x0; x <= x1; ++x) g.start[size_t(y) * g.nx + x + 1]++;
  }
  for (size_t i = 1; i < g.start.size(); ++i) g.start[i] += g.start[i - 1];
  g.tris.resize(g.start.back());
  std::vector<uint32_t> fill(g.start.begin(), g.start.end() - 1);
  for (uint32_t t = 0; t < T; ++t) {
    int x0, y0, x1, y1;
    range(t, x0, y0, x1, y1);
    for (int y = y0; y <= y1; ++y)
      for (int x = x0; x <= x1; ++x) g.tris[fill[size_t(y) * g.nx + x]++] = t;
  }
  return g;
}
}  // namespace

// the face under a UV position; where mirrored halves are stacked, the half that is not the copy
// (1.2) the corner (local edge) of face f whose UV edge is nearest a point of the layout
static uint32_t nearestEdgeUV(const Mesh& m, uint32_t f, vec2 p) {
  uint32_t best = 0;
  float bestD = 1e30f;
  for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) {
    vec2 a = m.loopUV[l], b = m.loopUV[m.nextLoop(l, f)], ab = b - a;
    float t = std::clamp(glm::dot(p - a, ab) / std::max(glm::dot(ab, ab), 1e-20f), 0.f, 1.f);
    float dd = glm::length(a + ab * t - p);
    if (dd < bestD) bestD = dd, best = l - m.faceStart[f];
  }
  return best;
}

static int faceAtUV(const Mesh& m, vec2 p) {
  if (m.loopUV.size() != m.loopCount() || m.tris.empty()) return -1;
  const UVTriGrid& g = uvGridFor(m);
  int x = int(std::floor((p.x - g.lo.x) / g.cell.x)), y = int(std::floor((p.y - g.lo.y) / g.cell.y));
  if (x < 0 || y < 0 || x >= g.nx || y >= g.ny) return -1;
  int hit = -1, primary = -1;
  bool flags = m.faceFlags.size() == m.faceCount();
  size_t c = size_t(y) * g.nx + x;
  // (in triangle order, as before: the last one found wins)
  for (uint32_t i = g.start[c]; i < g.start[c + 1]; ++i) {
    uint32_t t = g.tris[i];
    if (pointInTriangle2(m.loopUV[m.tris[t].x], m.loopUV[m.tris[t].y], m.loopUV[m.tris[t].z], p) &&
        faceShown(m, m.triFace[t])) {
      hit = (int)m.triFace[t];
      if (!flags || !(m.faceFlags[hit] & FaceMirrorCopy)) primary = hit;
    }
  }
  return primary >= 0 ? primary : hit;
}

// nearest loop to a screen position (within maxPx), -1 if none
static int loopNear(const Mesh& m, const UVFrame& fr, ImVec2 p, float maxPx) {
  int best = -1;
  float bd = maxPx * maxPx;
  for (uint32_t l = 0; l < m.loopUV.size(); ++l) {
    if (!loopShown(m, l)) continue;
    ImVec2 s = fr.toScreen(m.loopUV[l]);
    float dx = s.x - p.x, dy = s.y - p.y, d = dx * dx + dy * dy;
    if (d < bd) bd = d, best = (int)l;
  }
  return best;
}

std::vector<uint32_t> App::uvSelectedLoops() {
  Document& d = engine.doc;
  if (uvSelectMode == UVSelectMode::Vertex) {
    std::vector<uint32_t> out;
    for (uint32_t l = 0; l < uvLoopSel.size(); ++l)
      if (uvLoopSel[l]) out.push_back(l);
    return out;
  }
  return uv::loopsOfFaces(d.mesh, d.selectedFaces());
}

// selects the loops forming one UV vertex (same mesh vertex, same UV position)
static void selectUVVertex(App& app, uint32_t loop, bool value) {
  const Mesh& m = app.engine.doc.mesh;
  uint32_t id = loop < m.loopUVVert.size() ? m.loopUVVert[loop] : 0xFFFFFFFFu;
  for (uint32_t l = 0; l < m.loopUV.size(); ++l)
    if (l == loop || (id != 0xFFFFFFFFu && m.loopUVVert[l] == id) ||
        (m.loopVert[l] == m.loopVert[loop] && glm::length(m.loopUV[l] - m.loopUV[loop]) < 1e-6f))
      app.uvLoopSel[l] = value;
}

void App::cancelUVTransform() {
  if (!uvXform.active) return;
  Document& d = engine.doc;
  d.gpu.updateUVs(d.mesh);  // restore GPU UVs
  uvXform = UVTransformState();
}

void App::commitUVTransform() {
  if (!uvXform.active) return;
  Document& d = engine.doc;
  std::vector<vec2> result = uvXform.preview;
  d.gpu.updateUVs(d.mesh);  // GPU must hold the old layout for reprojection
  const char* label = uvXform.kind == 'r' ? "Rotate UVs" : uvXform.kind == 's' ? "Scale UVs" : "Move UVs";
  d.applyUVs(result, reprojectOnUVEdit, label);
  uvXform = UVTransformState();
}

// The loops a transform moves: the selection, plus the stacked mirror half linked to it (so the
// preview shows both halves moving), minus pinned faces. `pinnedHit`: some were pinned.
static std::vector<uint32_t> transformLoops(App& app, const std::vector<uint32_t>& sel, bool* pinnedHit) {
  Document& d = app.engine.doc;
  const Mesh& m = d.mesh;
  std::vector<uint8_t> in(m.loopCount(), 0);
  for (uint32_t l : sel) in[l] = 1;
  const uv::MirrorLinks& L = d.mirrorLinks();
  if (!L.empty() && L.source.size() == in.size()) {
    for (uint32_t l = 0; l < in.size(); ++l)
      if (in[l] && L.source[l] != l) in[L.source[l]] = 1;
    for (uint32_t l = 0; l < in.size(); ++l)
      if (L.source[l] != l && in[L.source[l]]) in[l] = 1;
  }
  *pinnedHit = false;
  for (uint32_t f = 0; f < m.faceCount(); ++f)
    if (uv::isPinned(m, f))
      for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l)
        if (in[l]) in[l] = 0, *pinnedHit = true;
  std::vector<uint32_t> out;
  for (uint32_t l = 0; l < in.size(); ++l)
    if (in[l]) out.push_back(l);
  return out;
}

static void startTransform(App& app, char kind, vec2 mouseUV, bool drag) {
  Document& d = app.engine.doc;
  auto sel = app.uvSelectedLoops();
  if (sel.empty()) {
    app.setStatus(app.uvSelectMode == UVSelectMode::Vertex ? T_("Select UV vertices first") : T_("Select faces or islands first"));
    return;
  }
  bool pinned = false;
  auto loops = transformLoops(app, sel, &pinned);
  if (loops.empty()) {
    app.uvToast(T_("Pinned: Alt+P (or right-click > Pins > Unpin Selected Islands) to move them"));
    return;
  }
  if (pinned) app.uvToast(T_("Pinned islands in the selection stay where they are"));
  UVTransformState& x = app.uvXform;
  x = UVTransformState();
  x.active = true;
  x.kind = kind;
  x.dragMove = drag;
  x.startMouseUV = mouseUV;
  x.loops = std::move(loops);
  x.original = d.mesh.loopUV;
  x.preview = x.original;
  x.pivot = uv::boundsOfLoops(d.mesh, x.loops).center();
}

// ------------------------------------------------------------------ islands: pins, mirrored halves

void App::updateUVIslandInfo() {
  Document& d = engine.doc;
  Mesh& m = d.mesh;
  uint64_t key = d.undo.version() ^ (uint64_t(m.faceCount()) << 40) ^ (uint64_t(m.loopCount()) << 20) ^ m.islandCount;
  if (key == uvIslandInfoKey) return;
  uvIslandInfoKey = key;
  uvIslandInfo.clear();
  uvUnstacked = 0;
  if (!d.hasMesh() || m.loopIsland.size() != m.loopCount()) return;
  uvIslandInfo.assign(m.islandCount, UVIslandInfo());
  std::vector<int> copyFaces(m.islandCount, 0);
  bool flags = m.faceFlags.size() == m.faceCount();
  for (uint32_t f = 0; f < m.faceCount(); ++f) {
    if (!m.faceSize(f)) continue;
    uint32_t i = m.loopIsland[m.faceStart[f]];
    if (i >= m.islandCount) continue;
    UVIslandInfo& in = uvIslandInfo[i];
    ++in.faces;
    in.tile = m.faceTile(f);
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) in.box.add(m.loopUV[l]);
    if (flags && (m.faceFlags[f] & FacePinned)) in.pinned = true;
    if (flags && (m.faceFlags[f] & FaceMirrorCopy)) ++copyFaces[i];
  }
  const uv::MirrorLinks& L = d.mirrorLinks();
  for (uint32_t f = 0; f < m.faceCount() && f < L.primary.size(); ++f)
    if (L.primary[f] >= 0) {
      uint32_t i = m.loopIsland[m.faceStart[L.primary[f]]];
      if (i < m.islandCount) ++uvIslandInfo[i].copies;
    }
  for (uint32_t i = 0; i < m.islandCount; ++i) uvIslandInfo[i].isCopy = copyFaces[i] && copyFaces[i] == uvIslandInfo[i].faces;
  uvUnstacked = uv::unstackedCopies(m, L);
}

void App::uvToast(const std::string& s) {
  uvToastText = s;
  uvToastTime = nowSeconds();
  setStatus(s);
}

bool App::uvRunOnSelection(const char* cmd, json args, const char* status) {
  try {
    json r = engine.execute(cmd, args);
    if (status) setStatus(status);
    return true;
  } catch (std::exception& e) {
    uvToast(std::string(e.what()));  // pinned islands and the like: a message, not an error dialog
    return false;
  }
}

size_t App::pinnedSelectedFaces() const {
  const Document& d = engine.doc;
  const Mesh& m = d.mesh;
  size_t n = 0;
  for (uint32_t f = 0; f < d.faceSelection.size() && f < m.faceFlags.size(); ++f) n += d.faceSelection[f] && (m.faceFlags[f] & FacePinned);
  return n;
}

void App::uvPinSelection(bool pin, bool uvVertices) {
  Document& d = engine.doc;
  if (d.selectedFaces().empty() && uvVertices && uvSelectMode == UVSelectMode::Vertex) {
    // vertex mode: the faces around the selected UV vertices
    for (uint32_t f = 0; f < d.mesh.faceCount(); ++f)
      for (uint32_t l = d.mesh.faceStart[f]; l < d.mesh.faceStart[f + 1]; ++l)
        if (l < uvLoopSel.size() && uvLoopSel[l]) d.faceSelection[f] = 1;
    d.updateSelectionFlags();
  }
  if (d.selectedFaces().empty()) {
    setStatus(pin ? T_("Select the islands to pin first") : T_("Select the islands to unpin first"));
    return;
  }
  const size_t before = uv::pinnedFaceCount(d.mesh);
  if (!uvRunOnSelection("uv_pin", {{"pin", pin}, {"selection", true}})) return;
  size_t n = uv::pinnedFaceCount(d.mesh);
  if (!pin && n == before) uvToast(T_("None of the selected islands were pinned"));
  else
    uvToast(pin ? T_("Pinned: UV edits leave these islands where they are (Alt+P unpins)")
                : (n ? T_("Unpinned the selected islands") : T_("Unpinned: no islands are pinned now")));
}

void App::uvFrameSelection() {
  Document& d = engine.doc;
  auto loops = uvSelectedLoops();
  const TileLayout& tl = d.mesh.tiles;
  int tile = tl.active() ? std::clamp(engine.uvOptions.tile, 0, tl.count() - 1) : -1;
  if (loops.empty()) {
    std::vector<uint32_t> faces;  // texture tiles: the shown tile's faces
    for (uint32_t f = 0; f < d.mesh.faceCount(); ++f)
      if (tile < 0 || d.mesh.faceTile(f) == tile) faces.push_back(f);
    loops = uv::loopsOfFaces(d.mesh, faces);
  } else if (tile >= 0) {  // a selection in another tile: show that tile
    uint32_t f = uint32_t(std::upper_bound(d.mesh.faceStart.begin(), d.mesh.faceStart.end(), loops[0]) - d.mesh.faceStart.begin()) - 1;
    engine.uvOptions.tile = tile = d.mesh.faceTile(f);
  }
  Rect2 b = uv::boundsOfLoops(d.mesh, loops);
  if (!b.valid()) return;
  vec2 s = glm::max(b.size(), vec2(1e-3f));
  uvViewCenter = b.center() - (tile >= 0 ? vec2(tl.uvOrigin(tile)) : vec2(0.f));
  uvViewZoom = std::clamp(0.85f / std::max(s.x, s.y), 0.05f, 200.f);
}

// (1.2.7) The UV menu: one menu, the same in the menu bar (UV), the UV editor's More button and its right-click menu.
// They were three menus that had grown apart: the same command had another name or place in each, some only in
// one, and Mirrored halves offered different things. The right-click menu adds Move / Rotate / Scale at the top.
void App::uvOpsMenu(UVMenuFrom from) {
  Document& d = engine.doc;
  Mesh& m = d.mesh;
  const bool context = from == UVMenuFrom::Context;
  bool vertMode = uvSelectMode == UVSelectMode::Vertex;
  auto faces = d.selectedFaces();
  auto loops = uvSelectedLoops();
  bool any = !loops.empty();
  bool anyFaces = !faces.empty();
  const uv::MirrorLinks& L = d.mirrorLinks();
  size_t pinnedSel = 0, copiesSel = 0;
  for (uint32_t f : faces) {
    pinnedSel += uv::isPinned(m, f) ? 1 : 0;
    copiesSel += (f < L.primary.size() && L.primary[f] >= 0) ? 1 : 0;
  }
  bool linkedSel = false;  // the selection has a stacked mirror half
  if (!L.empty() && anyFaces) linkedSel = uv::withLinkedFaces(m, L, faces).size() > faces.size() || copiesSel;
  if (context) {
    if (vertMode) ImGui::TextDisabled(T_("%zu UV vertices selected"), loops.size());
    else if (anyFaces)
      ImGui::TextDisabled(T_("%zu faces selected%s%s"), faces.size(), pinnedSel ? T_(", pinned") : "",
                          linkedSel ? T_(", mirrored") : "");
    else
      ImGui::TextDisabled("%s", T_("Nothing selected: the commands act on every island"));
    ImGui::Separator();
  }
  // discrete edits of the selected UVs (pinned faces stay; stacked halves follow)
  auto edit = [&](const char* label, const std::function<void(Mesh&, const std::vector<uint32_t>&, vec2)>& fn) {
    bool pinned = false;
    auto tl = transformLoops(*this, loops, &pinned);
    if (tl.empty()) {
      uvToast(pinned ? T_("Pinned: Alt+P (or right-click > Pins > Unpin Selected Islands) to change them") : T_("Select something first"));
      return;
    }
    Mesh work = m;
    fn(work, tl, uv::boundsOfLoops(m, loops).center());
    d.applyUVs(work.loopUV, reprojectOnUVEdit, label);
  };
  // an island command; when it changed nothing it says so (and why), instead of a success line for no change
  auto island = [&](const char* cmd, json args, const char* done, const char* unchanged) {
    args["reproject"] = reprojectOnUVEdit;
    uint64_t v = d.undo.version();
    if (!uvRunOnSelection(cmd, args)) return;
    uvToast(d.undo.version() != v || !unchanged ? done : unchanged);
  };
  json sel = {{"selection", anyFaces}};  // nothing selected: every island
  auto with = [&](const json& extra) {
    json j = sel;
    for (auto& [k, v] : extra.items()) j[k] = v;
    return j;
  };
  if (context) {
    if (ImGui::MenuItem(L_("Move"), "G", false, any)) startTransform(*this, 'g', uvMenuUV, false);
    if (ImGui::MenuItem(L_("Rotate"), "R", false, any)) startTransform(*this, 'r', uvMenuUV, false);
    if (ImGui::MenuItem(L_("Scale"), "S", false, any)) startTransform(*this, 's', uvMenuUV, false);
    ImGui::Separator();
  }

  // ---- the layout: unwrap, relax, pack, overlaps (on the selection's islands, or all of them)
  if (ImGui::BeginMenu(anyFaces ? L_("Unwrap the Selection###unwrap") : L_("Unwrap###unwrap"))) {
    const char* names[] = {N_("Along the Seams (LSCM)"), N_("Smart UV Project"), N_("Box Project"), N_("Planar (from the 3D View)"),
                           N_("Cylinder Project"), N_("Sphere Project")};
    const char* tips[] = {N_("Flattens each piece the seams cut out. Lay the seams first (the Seams tool, M)."),
                          N_("Cuts where the model bends sharply and lays each piece flat: hard-surface models"), nullptr, nullptr,
                          N_("Unrolls around the longest axis: arms, legs, tubes, barrels. Select one part first."), nullptr};
    const char* vals[] = {"lscm", "smart", "box", "planar", "cylinder", "sphere"};
    for (int i = 0; i < 6; ++i) {
      if (ImGui::MenuItem(L_(names[i]))) island("uv_unwrap", with({{"method", vals[i]}}), T_("Unwrapped"), nullptr);
      if (tips[i] && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T_(tips[i]));
    }
    ImGui::EndMenu();
  }
  if (ImGui::MenuItem(anyFaces ? L_("Relax Islands (Selection)###relax") : L_("Relax Islands###relax")))
    island("uv_relax", sel, T_("Relaxed: the stretch is gone, the islands stay in place"),
           T_("Already relaxed: nothing moved by a quarter of a texel"));
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", T_("Solves the islands again without stretch and puts them back where they were, at the same size"));
  if (ImGui::MenuItem(anyFaces ? L_("Pack Islands (Selection)###pack") : L_("Pack Islands###pack")))
    island("uv_pack", sel,
           anyFaces ? T_("Packed around the other islands")
                    : (uv::pinnedFaceCount(m) ? T_("Packed around the pinned islands") : T_("Packed")),
           nullptr);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", anyFaces ? T_("Packs the selected islands into the space the others leave")
                                     : T_("Packs the islands by their outlines, so small ones fill the gaps of big ones"));
  size_t ov = overlappingIslandCount();
  std::string ovl = (ov ? F_("Fix Overlapping UVs (%zu)", ov) : std::string(T_("Fix Overlapping UVs (none)"))) + "###fixoverlaps";
  if (ImGui::MenuItem(ovl.c_str(), nullptr, false, ov > 0 || uvUnstacked > 0))
    island("uv_fix_overlaps", json::object(), T_("Overlaps fixed"), nullptr);
  ImGui::Separator();

  // ---- the selected islands
  if (ImGui::MenuItem(L_("Island Brush"), keyLabel("tool.island"))) selectTool(ViewportTool::Island);
  if (ImGui::MenuItem(L_("Make Island from Selection"), keyLabel("island.make"), false, anyFaces)) makeIslandFromSelection();
  if (ImGui::BeginMenu(L_("Islands"), anyFaces)) {
    if (ImGui::MenuItem(L_("Orient Upright")))
      island("uv_orient", with({{"mode", "upright"}}), T_("Oriented upright: model up is texture up"),
             T_("Already upright: nothing turned"));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T_("Turns each island so that up on the model is up in the texture"));
    if (ImGui::MenuItem(L_("Orient Along Their Longest Side")))
      island("uv_orient", with({{"mode", "longest_axis"}}), T_("Oriented along their longest side"), T_("Already along their longest side: nothing turned"));
    if (ImGui::MenuItem(L_("Straighten (Rectify Quad Islands)"))) {
      try {
        json r = engine.execute("uv_rectify", with({{"reproject", reprojectOnUVEdit}}));
        int rectified = r.value("rectified", 0), skipped = r.value("skipped", 0);
        uvToast(!rectified ? std::string(T_("Nothing straightened: the islands are not grids of quads"))
                : skipped  ? F_("Straightened %d island(s), %d are not a grid of quads", rectified, skipped)
                           : F_("Straightened %d island(s)", rectified));
      } catch (std::exception& e) {
        uvToast(e.what());
      }
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", T_("Straightens islands made of quads (belts, pipes, trims, planks) into a straight grid"));
    if (ImGui::MenuItem(L_("Move into Free Space")))
      island("uv_pack", with({{"free_space", true}}), T_("Moved into free space"), nullptr);
    if (ImGui::BeginMenu(L_("Texel Density"))) {
      const char* names[] = {N_("Automatic"), N_("Match the Layout"), N_("Fit the Texture"), N_("One Texel per Pixel")};
      const char* vals[] = {"auto", "layout", "fit", "screen"};
      for (int i = 0; i < 4; ++i)
        if (ImGui::MenuItem(L_(names[i]))) island("uv_density", with({{"set", vals[i]}}), T_("Texel density set"), nullptr);
      ImGui::Separator();
      if (ImGui::MenuItem(L_("Texel Density Window..."))) showDensity = true;
      ImGui::EndMenu();
    }
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(L_("Rotate / Flip"), any)) {
    if (ImGui::MenuItem(L_("Rotate 90 CW"))) edit("Rotate UVs", [](Mesh& w, auto& l, vec2 p) { uv::rotate(w, l, -kPi / 2, p); });
    if (ImGui::MenuItem(L_("Rotate 90 CCW"))) edit("Rotate UVs", [](Mesh& w, auto& l, vec2 p) { uv::rotate(w, l, kPi / 2, p); });
    if (ImGui::MenuItem(L_("Rotate 180"))) edit("Rotate UVs", [](Mesh& w, auto& l, vec2 p) { uv::rotate(w, l, kPi, p); });
    if (ImGui::MenuItem(L_("Flip Horizontal"))) edit("Flip UVs", [](Mesh& w, auto& l, vec2) { uv::flip(w, l, true); });
    if (ImGui::MenuItem(L_("Flip Vertical"))) edit("Flip UVs", [](Mesh& w, auto& l, vec2) { uv::flip(w, l, false); });
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(L_("Align / Weld"), any)) {
    if (ImGui::MenuItem(L_("Align Vertically (Same U)")))
      edit("Align UVs", [](Mesh& w, auto& l, vec2 p) { for (uint32_t i : l) w.loopUV[i].x = p.x; });
    if (ImGui::MenuItem(L_("Align Horizontally (Same V)")))
      edit("Align UVs", [](Mesh& w, auto& l, vec2 p) { for (uint32_t i : l) w.loopUV[i].y = p.y; });
    if (ImGui::MenuItem(L_("Weld / Stitch"))) edit("Weld UVs", [](Mesh& w, auto& l, vec2) { uv::weld(w, l); });
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", T_("Moves selected UVs of the same mesh vertex to their average.\nSelect both sides of a seam to stitch it."));
    if (ImGui::MenuItem(L_("Fit to 0-1"))) edit("Fit UVs", [](Mesh& w, auto& l, vec2) { uv::fitToUnitSquare(w, l, 0.005f); });
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(L_("Mirrored Halves"))) {
    // (1.2.7) the other half by the model's symmetry, side by side in the layout or stacked: it only knew stacked
    // halves, so one half of a face that was unwrapped in two halves had none
    if (ImGui::MenuItem(L_("Select the Other Half"), nullptr, false, anyFaces)) selectOtherHalf();
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", T_("Adds the mirror image of the selection across the model's X symmetry,\nwherever it is in the layout"));
    if (ImGui::MenuItem(anyFaces ? L_("Mirror the Selection's UVs (X)###mirroruv") : L_("Mirror UVs (X)###mirroruv"))) {
      json r;
      if (runCommand("uv_mirror", {{"axes", {"x"}}, {"selection", anyFaces}, {"reproject", reprojectOnUVEdit}}, &r)) {
        int n = r.value("mirrored_faces", 0);
        uvToast(n ? F_("%d faces on the -X side now share the UVs of the +X side", n)
                  : std::string(T_("Nothing mirrored: no faces with a mirror image on the +X side")));
      }
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", anyFaces ? T_("The -X half of the selection (or of its mirror image) takes the mirrored UVs of the +X half:\n"
                                            "both halves share one place in the texture, and paint shows on both")
                                       : T_("The -X half of a symmetric model takes the mirrored UVs of the +X half:\n"
                                            "both share one place in the texture"));
    if (ImGui::MenuItem(L_("Unstack (Make Independent)"), nullptr, false, linkedSel))
      island("uv_unstack", sel, T_("The mirrored half has its own place now: each side can be painted separately"), nullptr);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", T_("The stacked half of the selected islands stops following the other half\nand moves into free space."));
    if (ImGui::MenuItem(L_("Re-stack All Mirrored Halves"), nullptr, false, uvUnstacked > 0))
      island("uv_restack", json::object(), T_("Mirrored halves are stacked on their other half again"), nullptr);
    if (engine.features.on("symmetry_check")) {
      ImGui::Separator();
      if (ImGui::MenuItem(L_("Check Mesh Symmetry..."))) showSymmetry = true;
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T_("How far the two halves of the model differ, with a heat map and the worst spots"));
    }
    ImGui::Separator();
    ImGui::TextDisabled("%s", T_("Moving either half of a stacked pair moves both."));
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(L_("Pins"))) {
    if (ImGui::MenuItem(L_("Pin Selected Islands"), "P", false, any)) uvPinSelection(true);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", T_("Pinned islands keep their place through every UV edit;\npacking and unwrapping put other islands around them."));
    if (ImGui::MenuItem(L_("Unpin Selected Islands"), keyLabel("uv.unpin"), false, any && (pinnedSel || vertMode))) uvPinSelection(false);
    if (ImGui::MenuItem(L_("Pin Every Island"))) island("uv_pin", {{"all", true}, {"pin", true}}, T_("Every island is pinned"), nullptr);
    if (ImGui::MenuItem(L_("Unpin Every Island"), nullptr, false, uv::pinnedFaceCount(m) > 0))
      island("uv_pin", {{"all", true}, {"pin", false}}, T_("No islands are pinned now"), nullptr);
    ImGui::EndMenu();
  }
  ImGui::Separator();

  // ---- selection, texture sets, seams
  if (ImGui::BeginMenu(L_("Select"))) {
    faceSelectItems(true);
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(L_("Texture Sets"))) {
    const TileLayout& tl = m.tiles;
    bool udimOnly = (tl.mode & TileByUdim) && !(tl.mode & TileBySlot);
    if (ImGui::MenuItem(udimOnly ? L_("Move Selection to a New UDIM Tile...") : L_("New Texture Set from Selection..."), nullptr, false,
                        anyFaces))
      openNewTextureSet();
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", T_("A texture of their own for the selected faces, such as one for the head and one for the body"));
    if ((tl.mode & TileBySlot) && ImGui::BeginMenu(L_("Move Selection To"), anyFaces)) {
      for (int t = 0; t < tl.count(); ++t) {
        std::string n = m.slotName(tl.tiles[t].slot);
        if (ImGui::MenuItem(n.c_str())) {
          json r;
          if (runCommand("texture_set_from_faces", {{"selection", true}, {"name", n}, {"pack", false}}, &r))
            setStatus(F_("Moved %d faces to %s (Pack to tidy it up)", r.value("faces", 0), n.c_str()));
        }
      }
      ImGui::EndMenu();
    }
    if (!anyFaces) ImGui::TextDisabled("%s", T_("Select faces first."));
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(L_("Seams"))) {
    auto seams = [&](json a) {
      try {
        engine.execute("uv_seams", a);
      } catch (std::exception& e) {
        uvToast(e.what());
      }
    };
    // edges by how many of their faces are selected
    auto border = [&](bool mark) {
      std::vector<uint8_t> total(m.edges.size(), 0), selected(m.edges.size(), 0);
      for (uint32_t f = 0; f < m.faceCount(); ++f)
        for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) {
          uint32_t e = m.loopEdge[l];
          if (e >= total.size()) continue;
          total[e] = (uint8_t)std::min(255, total[e] + 1);
          if (d.faceSelection[f]) selected[e] = (uint8_t)std::min(255, selected[e] + 1);
        }
      auto flags = m.edgeFlags;
      for (size_t e = 0; e < flags.size(); ++e) {
        if (mark && selected[e] && selected[e] < total[e]) flags[e] |= EdgeSeam;
        if (!mark && total[e] >= 2 && selected[e] == total[e]) flags[e] &= ~EdgeSeam;
      }
      d.applySeams(flags, mark ? "Seams around the selection" : "Clear seams inside the selection");
    };
    if (ImGui::MenuItem(L_("Mark the Selection's Border as Seams"), nullptr, false, anyFaces)) border(true);
    if (ImGui::MenuItem(L_("Clear Seams Inside the Selection"), nullptr, false, anyFaces)) border(false);
    ImGui::Separator();
    if (ImGui::MenuItem(L_("Seams from Islands"))) seams({{"mode", "from_islands"}});
    if (ImGui::MenuItem(L_("Seams by Angle (60)"))) seams({{"mode", "by_angle"}, {"angle", 60}});
    if (ImGui::MenuItem(L_("Clear All Seams"))) seams({{"mode", "clear"}});
    ImGui::Separator();
    ImGui::TextDisabled("%s", T_("Unwrap (Along the Seams) afterward to use them."));
    ImGui::EndMenu();
  }
  if (engine.features.on("face_sets") && ImGui::MenuItem(L_("Face Sets..."))) openFaceSets();  // (1.2)
  if (ImGui::BeginMenu(L_("View"))) {
    if (ImGui::MenuItem(L_("Frame the Selection"), "F")) uvFrameSelection();
    if (ImGui::MenuItem(L_("Frame 0-1"), "Home")) uvViewCenter = vec2(0.5f), uvViewZoom = 0.9f;
    ImGui::EndMenu();
  }
  ImGui::Separator();
  ImGui::MenuItem(L_("Keep Paint on UV Edits"), nullptr, &reprojectOnUVEdit);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", T_("UV edits move the paint with the UVs (it costs memory: every layer is copied for undo)"));
}

// (1.2.7) The face selection's commands, the same in the UV menu and the 3D view's right-click Select (which
// adds what is under the pointer above them). In the UV editor, All and Invert keep to the texture set shown,
// and vertex mode selects UV vertices. Each change is an undo step, as in the 3D view.
void App::faceSelectItems(bool uvEditor) {
  Document& d = engine.doc;
  Mesh& m = d.mesh;
  bool vertMode = uvEditor && uvSelectMode == UVSelectMode::Vertex;
  auto faces = d.selectedFaces();
  bool anyFaces = !faces.empty();
  auto shown = [&](uint32_t f) { return !uvEditor || faceShown(m, f); };
  auto setSel = [&](const std::function<bool(uint32_t)>& want, bool add, const char* label) {
    auto before = d.faceSelection;
    d.faceSelection.resize(m.faceCount(), 0);
    if (!add) std::fill(d.faceSelection.begin(), d.faceSelection.end(), 0);
    for (uint32_t f = 0; f < m.faceCount(); ++f)
      if (want(f)) d.faceSelection[f] = 1;
    d.updateSelectionFlags();
    d.pushSelectionUndo(before, label);
    setStatus(F_("%zu faces selected", d.selectedFaces().size()));
  };
  if (ImGui::MenuItem(L_("All"), keyLabel("select.all"))) {
    if (vertMode)
      for (uint32_t l = 0; l < uvLoopSel.size(); ++l) uvLoopSel[l] = loopShown(m, l) ? 1 : 0;
    setSel(shown, false, "Select all faces");
  }
  if (ImGui::MenuItem(L_("None"), keyLabel("select.none"))) {
    std::fill(uvLoopSel.begin(), uvLoopSel.end(), 0);
    setSel([](uint32_t) { return false; }, false, "Select no faces");
  }
  if (ImGui::MenuItem(L_("Invert"))) {
    if (vertMode)
      for (uint32_t l = 0; l < uvLoopSel.size(); ++l) uvLoopSel[l] = loopShown(m, l) ? !uvLoopSel[l] : 0;
    std::vector<uint8_t> was = d.faceSelection;
    was.resize(m.faceCount(), 0);
    setSel([&](uint32_t f) { return shown(f) && !was[f]; }, false, "Invert the selection");
  }
  if (ImGui::MenuItem(L_("Grow"), keyLabel("select.grow"), false, anyFaces)) {
    auto before = d.faceSelection;
    uv::growSelection(m, d.faceSelection), d.updateSelectionFlags(), d.pushSelectionUndo(before, "Grow the selection");
  }
  if (ImGui::MenuItem(L_("Shrink"), keyLabel("select.shrink"), false, anyFaces)) {
    auto before = d.faceSelection;
    uv::shrinkSelection(m, d.faceSelection), d.updateSelectionFlags(), d.pushSelectionUndo(before, "Shrink the selection");
  }
  ImGui::Separator();
  if (ImGui::MenuItem(L_("Whole Islands"), nullptr, false, anyFaces)) {
    std::vector<uint8_t> isl(m.islandCount + 1, 0);
    for (uint32_t f : faces) isl[std::min(m.loopIsland[m.faceStart[f]], m.islandCount)] = 1;
    setSel([&](uint32_t f) { return isl[std::min(m.loopIsland[m.faceStart[f]], m.islandCount)] != 0; }, true, "Select whole islands");
  }
  if (ImGui::MenuItem(L_("The Other Half"), nullptr, false, anyFaces)) selectOtherHalf();
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", T_("Adds the mirror image of the selection across the model's X symmetry,\nwherever it is in the layout"));
  if (ImGui::MenuItem(L_("Whole Material Slot"), nullptr, false, anyFaces && !m.faceMaterial.empty())) {
    std::vector<uint8_t> slots(256, 0);
    for (uint32_t f : faces)
      if (f < m.faceMaterial.size()) slots[m.faceMaterial[f] & 255] = 1;
    setSel([&](uint32_t f) { return f < m.faceMaterial.size() && slots[m.faceMaterial[f] & 255]; }, true, "Select the material slot");
  }
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T_("Adds every face that uses the same Blender material slot as the selection"));
  if (ImGui::MenuItem(L_("Pinned Islands"), nullptr, false, uv::pinnedFaceCount(m) > 0))
    setSel([&](uint32_t f) { return uv::isPinned(m, f); }, false, "Select pinned islands");
  if (ImGui::MenuItem(L_("Overlapping Islands"), nullptr, false, overlappingIslandCount() > 0)) {
    std::vector<uint8_t> want(m.faceCount(), 0);
    for (auto& isl : uv::overlappingIslands(m))
      for (uint32_t f : isl) want[f] = 1;
    setSel([&](uint32_t f) { return want[f] != 0; }, false, "Select overlapping islands");
  }
  if (uvEditor) {
    ImGui::Separator();
    if (ImGui::MenuItem(L_("Vertices of the Selected Faces"), nullptr, false, anyFaces)) {
      std::fill(uvLoopSel.begin(), uvLoopSel.end(), 0);
      for (uint32_t l : uv::loopsOfFaces(m, faces)) uvLoopSel[l] = 1;
      uvSelectMode = UVSelectMode::Vertex;
    }
  }
}

// (1.2.7) Adds the other half of the selection: its mirror image across X (by the model's symmetry, wherever that is
// in the layout) and the halves stacked on it. A selection of whole islands takes the other islands whole, as far
// as the mirror image covers at least half of each (the halves may be triangulated a little differently).
void App::selectOtherHalf() {
  Document& d = engine.doc;
  Mesh& m = d.mesh;
  auto before = d.selectedFaces();
  if (before.empty()) return;
  std::vector<uint32_t> faceCount(m.islandCount + 1, 0), selCount(m.islandCount + 1, 0);
  auto islandOf = [&](uint32_t f) { return std::min(m.loopIsland[m.faceStart[f]], m.islandCount); };
  for (uint32_t f = 0; f < m.faceCount(); ++f) ++faceCount[islandOf(f)];
  for (uint32_t f : before) ++selCount[islandOf(f)];
  bool whole = true;
  for (uint32_t f : before) whole &= selCount[islandOf(f)] == faceCount[islandOf(f)];
  std::vector<uint8_t> was = d.faceSelection;
  json r;
  if (!runCommand("select_faces", {{"mode", "mirror"}}, &r)) return;
  if (whole) {
    std::vector<uint32_t> added(m.islandCount + 1, 0);
    for (uint32_t f = 0; f < m.faceCount(); ++f)
      if (d.faceSelection[f] && !was[f]) ++added[islandOf(f)];
    for (uint32_t f = 0; f < m.faceCount(); ++f) {
      uint32_t i = islandOf(f);
      if (i < m.islandCount && added[i] * 2 >= faceCount[i]) d.faceSelection[f] = 1;
    }
    d.updateSelectionFlags();
  }
  size_t n = d.selectedFaces().size() - before.size();
  uvToast(n ? F_("Selected the other half: %zu more faces", n)
            : std::string(T_("No other half: the selection's mirror image across X is already selected, or the model has none there")));
}

static void updateTransformPreview(App& app, vec2 mouseUV, bool snap) {
  UVTransformState& x = app.uvXform;
  Document& d = app.engine.doc;
  Mesh tmp;  // light-weight: only UVs are used by the uv:: transforms
  tmp.loopUV = x.original;
  if (x.kind == 'g') {
    vec2 delta = mouseUV - x.startMouseUV;
    if (snap) delta = glm::round(delta * 64.f) / 64.f;
    uv::translate(tmp, x.loops, delta);
  } else if (x.kind == 'r') {
    vec2 a = x.startMouseUV - x.pivot, b = mouseUV - x.pivot;
    float ang = std::atan2(b.y, b.x) - std::atan2(a.y, a.x);
    if (snap) ang = glm::radians(std::round(glm::degrees(ang) / 15.f) * 15.f);
    uv::rotate(tmp, x.loops, ang, x.pivot);
  } else {
    float s = glm::length(mouseUV - x.pivot) / std::max(glm::length(x.startMouseUV - x.pivot), 1e-5f);
    if (snap) s = std::round(s * 10.f) / 10.f;
    uv::scale(tmp, x.loops, vec2(s), x.pivot);
  }
  x.preview = tmp.loopUV;
  // show it live (texture tiles: the texture coordinates follow too)
  std::vector<vec2> atlas;
  if (d.mesh.tiles.active()) {
    atlas = d.mesh.atlasUVs(x.preview, d.mesh.tiles);
    std::swap(d.mesh.loopAtlasUV, atlas);
  }
  std::swap(d.mesh.loopUV, x.preview);
  d.gpu.updateUVs(d.mesh);
  std::swap(d.mesh.loopUV, x.preview);
  if (d.mesh.tiles.active()) std::swap(d.mesh.loopAtlasUV, atlas);
}

void App::drawUVEditor() {
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  beginPanel(L_("UV Editor"), ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleVar();
  ImGuiIO& io = ImGui::GetIO();
  Document& d = engine.doc;
  Painter& P = engine.painter;
  if (uvLoopSel.size() != d.mesh.loopUV.size()) uvLoopSel.assign(d.mesh.loopUV.size(), 0);
  bool vertMode = uvSelectMode == UVSelectMode::Vertex;

  // ---- toolbar
  ImGui::SetCursorPos(ImGui::GetCursorPos() + ImVec2(6, 4));
  const TileLayout& tl = d.mesh.tiles;
  if (tl.active()) {
    engine.uvOptions.tile = std::clamp(engine.uvOptions.tile, 0, tl.count() - 1);
    ImGui::SetNextItemWidth(110 * uiScale);
    if (ImGui::BeginCombo("##tile", tl.tiles[engine.uvOptions.tile].name.c_str())) {
      for (int t = 0; t < tl.count(); ++t)
        if (ImGui::Selectable(tl.tiles[t].name.c_str(), t == engine.uvOptions.tile)) {
          cancelUVTransform();
          engine.uvOptions.tile = t;
        }
      ImGui::Separator();
      if (ImGui::Selectable(L_("New set from selection..."))) openNewTextureSet();
      ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", (tl.mode & TileByUdim) ? T_("UDIM tile shown (each tile is its own texture)")
                                                     : T_("Texture set shown: one per material slot, each with its own 0-1 space"));
    ImGui::SameLine();
  }
  g_uvTile = tl.active() ? engine.uvOptions.tile : -1;
  const float fh = ImGui::GetFrameHeight();
  // (0.36) icons: Edit UVs / Paint, the selection mode, then the UV tools
  int mode = (int)uvMode;
  if (iconButton("uv.mode_edit", "uv_edit", T_("Edit UVs"), mode == 0 ? ui::IconState::Chosen : ui::IconState::Normal, fh,
                 T_("Select, move, rotate and scale the UVs"))) mode = 0;
  ImGui::SameLine(0, 1.f);
  if (iconButton("uv.mode_paint", "paint", T_("Paint"), mode == 1 ? ui::IconState::Chosen : ui::IconState::Normal, fh,
                 T_("Paint on the texture, flat (the 3D view's brush and material)"))) mode = 1;
  if (mode != (int)uvMode) {
    cancelUVTransform();
    uvMode = (UVMode)mode;
  }
  ImGui::SameLine();
  ImGui::SetNextItemWidth(130 * uiScale);
  const char* uvSources[IM_ARRAYSIZE(kUVSources)];
  for (int i = 0; i < IM_ARRAYSIZE(kUVSources); ++i) uvSources[i] = T_(kUVSources[i]);
  ImGui::Combo("##src", &uvTextureSource, uvSources,
               IM_ARRAYSIZE(kUVSources) - (engine.features.on("more_channels") ? 0 : 4));
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T_("What the UV editor shows under the UVs"));
  if (uvMode == UVMode::Edit) {
    ImGui::SameLine();
    // (the order of the select modes as in Blender: vertices, faces, islands)
    static const struct { UVSelectMode m; const char *id, *icon, *name, *tip; } kSel[] = {
        {UVSelectMode::Vertex, "uv.sel_vertex", "sel_vertex", N_("Vertices"), N_("Select single UV vertices")},
        {UVSelectMode::Face, "uv.sel_face", "sel_face", N_("Faces"), N_("Select faces")},
        {UVSelectMode::Island, "uv.sel_island", "sel_island", N_("Islands"), N_("Select whole UV islands")}};
    for (int i = 0; i < 3; ++i) {
      if (i) ImGui::SameLine(0, 1.f);
      if (iconButton(kSel[i].id, kSel[i].icon, T_(kSel[i].name), uvSelectMode == kSel[i].m ? ui::IconState::Chosen : ui::IconState::Normal,
                     fh, T_(kSel[i].tip)) && uvSelectMode != kSel[i].m) {
        cancelUVTransform();
        uvSelectMode = kSel[i].m;
        vertMode = uvSelectMode == UVSelectMode::Vertex;
      }
    }
    ImGui::SameLine();
    if (iconButton("uv.unwrap", "unwrap", T_("Unwrap"), ui::IconState::Normal, fh, T_("Unwrap the selected faces (or all): LSCM, Smart Project, Box...")))
      ImGui::OpenPopup("unwrap_menu");
    if (ImGui::BeginPopup("unwrap_menu")) {
      bool hasSel = !d.selectedFaces().empty();
      const char* names[] = {N_("LSCM (uses seams)"), N_("Smart Project"), N_("Box Project"), N_("Planar (from 3D view)"), N_("Cylinder"),
                             N_("Sphere")};
      const char* vals[] = {"lscm", "smart", "box", "planar", "cylinder", "sphere"};
      for (int i = 0; i < 6; ++i) {
        std::string item = (hasSel ? F_("%s (selection)", T_(names[i])) : F_("%s (all faces)", T_(names[i]))) + "###" + vals[i];
        if (ImGui::MenuItem(item.c_str())) {
          if (uvRunOnSelection("uv_unwrap", {{"method", vals[i]}, {"selection", hasSel}, {"reproject", reprojectOnUVEdit}}))
            setStatus(F_("%s: %u islands%s", T_(names[i]), d.mesh.islandCount,
                         uv::pinnedFaceCount(d.mesh) ? T_(" (pinned islands kept)") : ""));
        }
      }
      if (uv::pinnedFaceCount(d.mesh)) ImGui::TextDisabled("%s", T_("Pinned islands stay; the rest goes around them."));
      ImGui::EndPopup();
    }
    ImGui::SameLine(0, 1.f);
    if (iconButton("uv.pack", "pack", T_("Pack"), ui::IconState::Normal, fh, T_("Pack every island into 0-1 (pinned islands stay where they are)"))) {
      uvRunOnSelection("uv_pack", {{"reproject", reprojectOnUVEdit}},
                       uv::pinnedFaceCount(d.mesh) ? T_("Packed around the pinned islands") : T_("Packed"));
    }
    ImGui::SameLine(0, 1.f);
    if (iconButton("uv.seams", "seams", T_("Seams"), ui::IconState::Normal, fh, T_("Seams from the UV islands, by angle, or cleared")))
      ImGui::OpenPopup("seam_menu");
    if (ImGui::BeginPopup("seam_menu")) {
      Mesh work = d.mesh;
      if (ImGui::MenuItem(L_("Seams from UV Islands"))) {
        uv::seamsFromIslands(work, true);
        d.applySeams(work.edgeFlags, "Seams from islands");
      }
      static float angle = 60.f;
      ImGui::SetNextItemWidth(120);
      ImGui::SliderFloat("##ang", &angle, 10.f, 120.f, T_("%.0f deg"));
      ImGui::SameLine();
      if (ImGui::MenuItem(L_("Mark by Angle"))) {
        uv::seamsByAngle(work, angle, false);
        d.applySeams(work.edgeFlags, "Seams by angle");
      }
      if (ImGui::MenuItem(L_("Clear All Seams"))) {
        for (auto& f : work.edgeFlags) f &= ~EdgeSeam;
        d.applySeams(work.edgeFlags, "Clear seams");
      }
      ImGui::TextDisabled("%s", T_("Tip: use the Seams tool in the 3D view to click edges"));
      ImGui::EndPopup();
    }
    ImGui::SameLine(0, 1.f);
    if (iconButton("uv.more", "more", T_("More"), ui::IconState::Normal, fh, T_("Every UV tool, and what the UV editor shows")))
      ImGui::OpenPopup("uv_more");
    if (ImGui::BeginPopup("uv_more")) {
      uvMenuUV = uvViewCenter + (g_uvTile >= 0 ? vec2(tl.uvOrigin(g_uvTile)) : vec2(0.f));
      uvOpsMenu(UVMenuFrom::Editor);
      ImGui::Separator();
      ImGui::Checkbox(L_("Show distortion"), &engine.uvOptions.distortion);
      ImGui::Checkbox(L_("Show wireframe"), &engine.uvOptions.wireframe);
      ImGui::Checkbox(L_("Show seams"), &engine.uvOptions.seams);
      ImGui::Checkbox(L_("Show tiling"), &engine.uvOptions.tiling);
      ImGui::SliderFloat(L_("Texture dim"), &engine.uvOptions.dim, 0.2f, 1.f);
      ImGui::Checkbox(L_("Show pins and mirrored halves"), &uvShowIslandMarks);
      ImGui::EndPopup();
    }
  }

  // ---- frame & render
  ImVec2 avail = ImGui::GetContentRegionAvail();
  avail.x = std::max(avail.x, 32.f);
  avail.y = std::max(avail.y, 32.f);
  UVFrame fr;
  fr.origin = ImGui::GetCursorScreenPos();
  fr.size = avail;
  engine.uiRects["uv.image"] = {fr.origin.x, fr.origin.y, avail.x, avail.y};  // (for tests and the manual's screenshots)
  float m = std::min(avail.x, avail.y);
  fr.scale = vec2(uvViewZoom * m / avail.x, uvViewZoom * m / avail.y);
  fr.offset = vec2(0.5f) - uvViewCenter * fr.scale;
  if (g_uvTile >= 0) fr.uvOrigin = vec2(tl.uvOrigin(g_uvTile));
  engine.uvOptions.scale = fr.scale;
  engine.uvOptions.offset = fr.offset;

  TextureSet& t = d.tex;
  GLuint tex = t.finColor.id();
  int texMode = 0;
  Layer* sel = d.selectedLayer();
  switch (uvTextureSource) {
    case 1: tex = t.finRMH.id(), texMode = 1; break;
    case 2: tex = t.finRMH.id(), texMode = 2; break;
    case 3: tex = t.finRMH.id(), texMode = 3; break;
    case 4: tex = t.finNormal.id(), texMode = 5; break;
    case 5: tex = t.finEmissive.id(); break;
    case 6: tex = t.aoView().id(), texMode = 1; break;
    case 7: tex = t.curvView().id(), texMode = 1; break;
    case 8: tex = 0; break;
    case 9: tex = sel && sel->hasChannel(0) ? sel->channels[0]->id() : 0; break;
    case 10:
      if (sel && sel->isBaked()) tex = t.bakedView(sel->bakedMap).id(), texMode = 1;  // (its map, not the fixes)
      else tex = sel && sel->mask ? sel->mask->id() : 0, texMode = 7;
      break;
    case 11: case 12: case 13: case 14: tex = t.finExtra.id(), texMode = uvTextureSource - 10; break;  // r g b a
    default: break;
  }
  engine.uvOptions.texMode = texMode;
  {
    const ImVec4 &bg = ui::theme().uvBackdrop, &bd = ui::theme().uvBorder;  // (1.2.7)
    engine.uvOptions.background = vec3(bg.x, bg.y, bg.z);
    engine.uvOptions.border = vec3(bd.x, bd.y, bd.z);
  }
  // (1.2) the Face Sets view shows them on the layout too
  engine.uvOptions.faceSets = engine.viewOptions.mode == ViewMode::FaceSets && engine.features.on("face_sets");
  float fbs = io.DisplayFramebufferScale.x > 0 ? io.DisplayFramebufferScale.x : 1.f;
  // (0.22.2) while a line is drawn on the model the UV editor keeps its last picture
  const int uvW = std::max(16, int(avail.x * fbs)), uvH = std::max(16, int(avail.y * fbs));
  if (!(engine.drawingStroke && uvRT.color.id() && uvW == uvLastW && uvH == uvLastH))
    engine.renderer.renderUV(d, uvRT, uvW, uvH, engine.uvOptions, tex), uvLastW = uvW, uvLastH = uvH;
  ImGui::Image((ImTextureID)(intptr_t)uvRT.color.id(), avail, ImVec2(0, 1), ImVec2(1, 0));
  ImGui::SetCursorScreenPos(fr.origin);
  ImGui::SetNextItemAllowOverlap();  // the warning buttons drawn on top get the clicks
  ImGui::InvisibleButton("##uv_input", avail,
                         ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
  bool hovered = ImGui::IsItemHovered();
  bool active = ImGui::IsItemActive();
  vec2 mouseUV = fr.toUV(io.MousePos);
  // texture tiles: painting happens in the atlas, in the shown tile's cell
  auto toPaint = [&](vec2 uv) {
    if (g_uvTile < 0) return uv;
    // texture sets: every face wraps into its set's square (an island at u 1.2 paints at 0.2)
    if (!(tl.mode & TileByUdim)) uv = uv - glm::floor(uv);
    return tl.toAtlas(uv, g_uvTile);
  };
  P.uvTile = g_uvTile;
  const float paintScale = g_uvTile >= 0 ? tl.cellSize() : 1.f;
  ImDrawList* dl = ImGui::GetWindowDrawList();

  // ---- navigation
  if ((active && ImGui::IsMouseDown(ImGuiMouseButton_Middle)) ||
      (active && io.KeyAlt && ImGui::IsMouseDown(ImGuiMouseButton_Left))) {
    uvViewCenter -= vec2(io.MouseDelta.x / (fr.scale.x * fr.size.x), -io.MouseDelta.y / (fr.scale.y * fr.size.y));
  }
  // (1.2) Alt+click (no drag): the loop of quads across the UV edge nearest the pointer; Ctrl+Alt+click: the ring
  // along it; Shift adds (as in the 3D view)
  if (hovered && io.KeyAlt && uvMode == UVMode::Edit && d.hasMesh() && ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
      glm::length(vec2(ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.f).x, ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.f).y)) < 3.f) {
    int f = faceAtUV(d.mesh, mouseUV);
    if (f >= 0) {
      json r;
      if (runCommand("select_loop", {{"face", f}, {"edge", nearestEdgeUV(d.mesh, (uint32_t)f, mouseUV)}, {"ring", io.KeyCtrl},
                                     {"mode", io.KeyShift ? "add" : "set"}}, &r)) {
        if (!r.value("quads", true)) setStatus(T_("Loops and rings run through quads: this face isn't one"));
        else if (io.KeyCtrl) setStatus(F_("Selected a ring of %d faces", r.value("faces", 0)));
        else setStatus(F_("Selected a loop of %d faces", r.value("faces", 0)));
      }
    }
  }
  if (hovered && io.MouseWheel != 0 && !io.KeyCtrl) {
    vec2 before = mouseUV - fr.uvOrigin;  // view coordinates
    uvViewZoom = std::clamp(uvViewZoom * std::pow(1.15f, io.MouseWheel), 0.05f, 200.f);
    // keep the point under the cursor fixed
    vec2 sc(uvViewZoom * m / avail.x, uvViewZoom * m / avail.y);
    vec2 s((io.MousePos.x - fr.origin.x) / fr.size.x, 1.f - (io.MousePos.y - fr.origin.y) / fr.size.y);
    uvViewCenter = before - (s - vec2(0.5f)) / sc;
  }
  if (hovered && ImGui::IsKeyPressed(ImGuiKey_Home)) uvViewCenter = vec2(0.5f), uvViewZoom = 0.9f;

  // ---- edit mode
  static bool boxSelecting = false;
  static ImVec2 boxStart;
  bool snap = io.KeyCtrl;
  bool wasTransforming = uvXform.active;
  const float dragPx = 4.f * uiScale;
  if (uvMode == UVMode::Edit && d.hasMesh()) {
    if (uvXform.active) {
      updateTransformPreview(*this, mouseUV, snap);
      bool confirm = uvXform.dragMove ? !ImGui::IsMouseDown(ImGuiMouseButton_Left)
                                      : (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsKeyPressed(ImGuiKey_Enter));
      if (confirm) commitUVTransform();
      else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) || ImGui::IsKeyPressed(ImGuiKey_Escape)) cancelUVTransform();
      if (uvXform.active) {
        const char* names = uvXform.kind == 'g' ? T_("Move") : uvXform.kind == 'r' ? T_("Rotate") : T_("Scale");
        std::string buf = F_("%s  (click/Enter confirm, RMB/Esc cancel, Ctrl snap)", names);
        dl->AddText(ImVec2(fr.origin.x + 10, fr.origin.y + 10), IM_COL32(255, 210, 140, 255), buf.c_str());
        ImVec2 pv = fr.toScreen(uvXform.pivot);
        dl->AddCircle(pv, 5, IM_COL32(255, 210, 140, 255));
      }
    } else if (hovered) {
      // letters typed into a text field (the command palette, a layer name) are not UV shortcuts
      if (!io.KeyAlt && !io.KeyCtrl && !io.WantTextInput && !showPalette) {
        if (ImGui::IsKeyPressed(ImGuiKey_G)) startTransform(*this, 'g', mouseUV, false);
        if (ImGui::IsKeyPressed(ImGuiKey_R)) startTransform(*this, 'r', mouseUV, false);
        if (ImGui::IsKeyPressed(ImGuiKey_S)) startTransform(*this, 's', mouseUV, false);
        if (ImGui::IsKeyPressed(ImGuiKey_F)) uvFrameSelection();
        if (ImGui::IsKeyPressed(ImGuiKey_P)) uvPinSelection(true);
        if (ImGui::IsKeyPressed(ImGuiKey_A)) {
          if (vertMode) {
            bool any = std::find(uvLoopSel.begin(), uvLoopSel.end(), 1) != uvLoopSel.end();
            for (uint32_t l = 0; l < uvLoopSel.size(); ++l) uvLoopSel[l] = !any && loopShown(d.mesh, l) ? 1 : 0;
          } else {
            bool any = !d.selectedFaces().empty();
            for (uint32_t f = 0; f < d.faceSelection.size(); ++f) d.faceSelection[f] = !any && faceShown(d.mesh, f) ? 1 : 0;
            d.updateSelectionFlags();
          }
        }
      }
      if (keyPressed("uv.unpin")) uvPinSelection(false);
      if (io.KeyAlt && !io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A)) {
        std::fill(uvLoopSel.begin(), uvLoopSel.end(), 0);
        std::fill(d.faceSelection.begin(), d.faceSelection.end(), 0);
        d.updateSelectionFlags();
      }
      // left button: select at once, or wait to see whether it becomes a drag (a click on a
      // selected island only moves it once the mouse travels a few pixels)
      if (!io.KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        uvPress = UVPress();
        uvPress.pos = io.MousePos;
        uvPress.uv = mouseUV;
        uvPress.shift = io.KeyShift;
        uvPress.ctrl = io.KeyCtrl;
        bool plain = !io.KeyShift && !io.KeyCtrl;
        if (vertMode) {
          int l = loopNear(d.mesh, fr, io.MousePos, 8.f * uiScale);
          uvPress.loop = l;
          if (l >= 0 && uvLoopSel[l] && plain) {
            uvPress.down = true;  // drag moves the selection, a click selects only this vertex
          } else if (l >= 0) {
            if (plain) std::fill(uvLoopSel.begin(), uvLoopSel.end(), 0);
            // Shift+click on a selected vertex deselects it (as in Blender); Ctrl+click always does
            bool off = io.KeyCtrl || (io.KeyShift && uvLoopSel[l]);
            selectUVVertex(*this, (uint32_t)l, !off);
            uvPress.down = plain;  // and a drag right away moves it
          } else {
            boxSelecting = true;
            boxStart = io.MousePos;
          }
        } else {
          int f = faceAtUV(d.mesh, mouseUV);
          uvPress.face = f;
          bool onSelected = f >= 0 && f < (int)d.faceSelection.size() && d.faceSelection[f];
          if (onSelected && plain) {
            uvPress.down = true;
          } else if (f >= 0) {
            // Shift+click on a selected face (island) deselects it, as in Blender
            applyFaceSelectionClick((uint32_t)f, io.KeyShift, io.KeyCtrl || (io.KeyShift && onSelected));
            uvPress.down = plain;
            uvPress.face = -1;  // already selected: a release without a drag does nothing more
          } else {
            boxSelecting = true;
            boxStart = io.MousePos;
          }
        }
      }
      // right button: the context menu (on release, so a stray right-drag does nothing)
      if (!wasTransforming && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        uvPress = UVPress();
        uvPress.down = uvPress.right = true;
        uvPress.pos = io.MousePos;
        uvPress.uv = mouseUV;
      }
    }
    if (uvPress.down && !uvPress.right) {
      ImVec2 dlt = io.MousePos - uvPress.pos;
      if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        // a click on the selection: select just what is under the cursor
        if (vertMode && uvPress.loop >= 0) {
          std::fill(uvLoopSel.begin(), uvLoopSel.end(), 0);
          selectUVVertex(*this, (uint32_t)uvPress.loop, true);
        } else if (!vertMode && uvPress.face >= 0) {
          applyFaceSelectionClick((uint32_t)uvPress.face, false, false);
        }
        uvPress.down = false;
      } else if (dlt.x * dlt.x + dlt.y * dlt.y > dragPx * dragPx) {
        startTransform(*this, 'g', uvPress.uv, true);
        uvPress.down = false;
      }
    }
    if (uvPress.down && uvPress.right && !ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
      ImVec2 dlt = io.MousePos - uvPress.pos;
      uvPress.down = false;
      if (dlt.x * dlt.x + dlt.y * dlt.y <= 36.f * uiScale * uiScale) {
        // what is under the cursor joins the selection if it is not part of it
        if (vertMode) {
          int l = loopNear(d.mesh, fr, uvPress.pos, 8.f * uiScale);
          if (l >= 0 && !uvLoopSel[l]) {
            std::fill(uvLoopSel.begin(), uvLoopSel.end(), 0);
            selectUVVertex(*this, (uint32_t)l, true);
          }
        } else {
          int f = faceAtUV(d.mesh, uvPress.uv);
          if (f >= 0 && !d.faceSelection[f]) applyFaceSelectionClick((uint32_t)f, false, false);
        }
        uvMenuUV = uvPress.uv;
        ImGui::OpenPopup("uv_ctx");
      }
    }
    if (boxSelecting) {
      ImVec2 a = boxStart, b = io.MousePos;
      dl->AddRectFilled(ImMin(a, b), ImMax(a, b), ui::themeColor(ui::theme().accent, 0.157f));
      dl->AddRect(ImMin(a, b), ImMax(a, b), ui::themeColor(ui::theme().accent, 0.784f));
      if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        boxSelecting = false;
        vec2 u0 = fr.toUV(ImMin(a, b)), u1 = fr.toUV(ImMax(a, b));
        Rect2 r;
        r.add(u0);
        r.add(u1);
        if (vertMode) {
          if (!io.KeyShift && !io.KeyCtrl) std::fill(uvLoopSel.begin(), uvLoopSel.end(), 0);
          for (uint32_t l = 0; l < d.mesh.loopUV.size(); ++l) {
            vec2 u = d.mesh.loopUV[l];
            if (!loopShown(d.mesh, l)) continue;
            if (u.x >= r.lo.x && u.x <= r.hi.x && u.y >= r.lo.y && u.y <= r.hi.y) uvLoopSel[l] = io.KeyCtrl ? 0 : 1;
          }
        } else {
          if (!io.KeyShift && !io.KeyCtrl) std::fill(d.faceSelection.begin(), d.faceSelection.end(), 0);
          std::vector<uint8_t> hitIsl(d.mesh.islandCount, 0);
          for (uint32_t f = 0; f < d.mesh.faceCount(); ++f) {
            if (!faceShown(d.mesh, f)) continue;
            vec2 c(0);
            for (uint32_t l = d.mesh.faceStart[f]; l < d.mesh.faceStart[f + 1]; ++l) c += d.mesh.loopUV[l];
            c /= float(d.mesh.faceSize(f));
            if (c.x >= r.lo.x && c.x <= r.hi.x && c.y >= r.lo.y && c.y <= r.hi.y) {
              if (uvSelectMode == UVSelectMode::Island) hitIsl[d.mesh.loopIsland[d.mesh.faceStart[f]]] = 1;
              else d.faceSelection[f] = io.KeyCtrl ? 0 : 1;
            }
          }
          if (uvSelectMode == UVSelectMode::Island)
            for (uint32_t f = 0; f < d.mesh.faceCount(); ++f)
              if (hitIsl[d.mesh.loopIsland[d.mesh.faceStart[f]]]) d.faceSelection[f] = io.KeyCtrl ? 0 : 1;
          d.updateSelectionFlags();
        }
      }
    }
  }
  if (ImGui::BeginPopup("uv_ctx")) {
    uvOpsMenu(UVMenuFrom::Context);
    ImGui::EndPopup();
  }

  // ---- pins and mirrored halves on the layout
  if (uvMode == UVMode::Edit && d.hasMesh()) updateUVIslandInfo();
  int hoverIsland = -1;
  if (hovered && d.hasMesh() && !uvXform.active) {
    int f = faceAtUV(d.mesh, mouseUV);
    if (f >= 0 && d.mesh.loopIsland.size() == d.mesh.loopCount()) hoverIsland = (int)d.mesh.loopIsland[d.mesh.faceStart[f]];
  }
  if (uvMode == UVMode::Edit && uvShowIslandMarks && uvIslandInfo.size() < 4000) {
    for (size_t i = 0; i < uvIslandInfo.size(); ++i) {
      const UVIslandInfo& in = uvIslandInfo[i];
      if (!in.box.valid() || (!in.pinned && !in.copies)) continue;
      if (g_uvTile >= 0 && in.tile != g_uvTile) continue;
      ImVec2 lo = fr.toScreen(in.box.lo), hi = fr.toScreen(in.box.hi);
      ImVec2 a(std::min(lo.x, hi.x), std::min(lo.y, hi.y)), b(std::max(lo.x, hi.x), std::max(lo.y, hi.y));
      if (b.x < fr.origin.x || a.x > fr.origin.x + fr.size.x || b.y < fr.origin.y || a.y > fr.origin.y + fr.size.y) continue;
      if (in.pinned) {
        ImU32 pc = IM_COL32(110, 180, 255, 230);
        dl->AddRect(a - ImVec2(2, 2), b + ImVec2(2, 2), IM_COL32(110, 180, 255, 120), 2.f, 0, 1.f);
        // a pin: head at the top right corner, needle pointing into the island
        ImVec2 h(b.x - 1.f, a.y + 1.f);
        dl->AddLine(h, h + ImVec2(-7.f, 7.f) * uiScale, IM_COL32(20, 20, 20, 220), 3.f);
        dl->AddLine(h, h + ImVec2(-7.f, 7.f) * uiScale, pc, 1.5f);
        dl->AddCircleFilled(h, 4.5f * uiScale, IM_COL32(20, 20, 20, 220));
        dl->AddCircleFilled(h, 3.5f * uiScale, pc);
      }
      if (in.copies) {  // the other half is stacked on this island
        const char* tag = "x2";
        ImVec2 ts = ImGui::CalcTextSize(tag);
        ImVec2 p(a.x + 3.f, a.y + 2.f);
        if (b.x - a.x > ts.x + 8 && b.y - a.y > ts.y + 4) {
          dl->AddRectFilled(p - ImVec2(2, 0), p + ts + ImVec2(2, 0), IM_COL32(20, 50, 30, 200), 3.f);
          dl->AddText(p, IM_COL32(150, 235, 170, 255), tag);
        }
      }
    }
  }
  // ---- UV vertex overlay
  if (uvMode == UVMode::Edit && vertMode && d.hasMesh()) {
    const std::vector<vec2>& uvs = uvXform.active ? uvXform.preview : d.mesh.loopUV;
    float r = 3.f * uiScale;
    for (uint32_t l = 0; l < uvs.size() && l < uvLoopSel.size(); ++l)
      if (uvLoopSel[l] && loopShown(d.mesh, l)) {
        ImVec2 p = fr.toScreen(uvs[l]);
        dl->AddRectFilled(ImVec2(p.x - r, p.y - r), ImVec2(p.x + r, p.y + r), IM_COL32(255, 150, 40, 255));
      }
    if (hovered && !uvXform.active && !boxSelecting) {
      int l = loopNear(d.mesh, fr, io.MousePos, 8.f * uiScale);
      if (l >= 0) {
        ImVec2 p = fr.toScreen(d.mesh.loopUV[l]);
        dl->AddRect(ImVec2(p.x - r - 2, p.y - r - 2), ImVec2(p.x + r + 2, p.y + r + 2), IM_COL32(255, 255, 255, 230), 0, 0, 1.5f);
      }
    }
  }

  // ---- paint mode
  if (uvMode == UVMode::Paint && d.hasMesh()) {
    Painter::View view = engine.paintView(engine.camera, lastViewportSize);
    float pressure = currentPressure();
    bool clone = tool == ViewportTool::Clone;
    if (clone && hovered && !io.KeyAlt && io.KeyCtrl && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !P.active()) {
      P.setCloneSource(toPaint(mouseUV));
      cloneMarkerValid = false;  // no 3D position known for a UV-space source
      setStatus(T_("Clone source set from the UV view"));
    } else if (hovered && !io.KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !P.active()) {
      P.tool = tool == ViewportTool::Erase ? PaintTool::Erase
             : tool == ViewportTool::Blur  ? PaintTool::Blur
             : clone                       ? PaintTool::Clone
             : tool == ViewportTool::Smudge ? PaintTool::Smudge
                                           : PaintTool::Paint;
      auto toUV = [&](vec2 w) { return toPaint(fr.toUV(ImVec2(w.x, w.y))); };
      int si = strokeStartSample();
      StrokePoint start{toPaint(mouseUV), pressure};
      if (si >= 0) start = strokePointOf(pen.frameSamples()[si], toUV);
      penEraserStroke = si >= 0 && pen.frameSamples()[si].pen && pen.frameSamples()[si].eraser && pen.eraserTipErases &&
                        (P.tool == PaintTool::Paint || P.tool == PaintTool::Blur || P.tool == PaintTool::Smudge);
      engine.useToolBrush(penEraserStroke ? PaintTool::Erase : P.tool);  // (1.2.13) the tool's own brush
      if (penEraserStroke) P.tool = PaintTool::Erase;
      if (P.beginStroke(d, engine.materials, engine.brushes, view, start, true, fr.uvPerPixel() * paintScale)) {
        strokeOwner = 2;
        uvStrokeStartedThisFrame = true;
        if (si >= 0) feedStroke(P, si + 1, toUV);
      }
      else if (clone && !P.hasCloneSource)
        setStatus(T_("Clone: Ctrl+click to set a source first"));
    }
    if (clone && P.hasCloneSource) {
      // source marker: fixed point between strokes, follows the brush while cloning
      vec2 src = P.active() && P.cloneOffsetValid() ? toPaint(mouseUV) + P.cloneOffsetUV()
               : (P.cloneAligned && P.cloneOffsetValid() && hovered) ? toPaint(mouseUV) + P.cloneOffsetUV()
                                                                       : P.cloneSourceUV;
      ImVec2 sp = fr.toScreen(g_uvTile >= 0 ? tl.fromAtlas(src) : src);
      ImU32 g = IM_COL32(120, 255, 160, 230);
      dl->AddCircle(sp, 7.f, IM_COL32(0, 0, 0, 180), 24, 3.f);
      dl->AddCircle(sp, 7.f, g, 24, 1.5f);
      dl->AddLine(ImVec2(sp.x - 11, sp.y), ImVec2(sp.x + 11, sp.y), g, 1.5f);
      dl->AddLine(ImVec2(sp.x, sp.y - 11), ImVec2(sp.x, sp.y + 11), g, 1.5f);
    }
    if (hovered) {
      float r = P.brush.size * 0.5f;
      ImU32 col = clone ? IM_COL32(120, 255, 160, 220) : IM_COL32(255, 255, 255, 220);
      if (engine.features.on("tip_cursor")) {
        drawTipCursor(dl, io.MousePos, r, col, P.brush.opacity);  // (1.2.13) the tip's outline and its falloff (Viewport3D.cpp)
      } else {
        dl->AddCircle(io.MousePos, r, IM_COL32(0, 0, 0, 160), 48, 3.f);
        dl->AddCircle(io.MousePos, r, col, 48, 1.5f);
      }
      if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket)) P.brush.size = std::max(1.f, P.brush.size / 1.15f);
      if (ImGui::IsKeyPressed(ImGuiKey_RightBracket)) P.brush.size = std::min(2000.f, P.brush.size * 1.15f);
    }
  }

  if (P.active() && strokeOwner == 2 && !uvStrokeStartedThisFrame) {
    auto toUV = [&](vec2 w) { return toPaint(fr.toUV(ImVec2(w.x, w.y))); };
    int fed = feedStroke(P, 0, toUV);
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      if (!fed) P.addPoint({toPaint(mouseUV), currentPressure(), pen.lastWasPen() ? pen.tilt() : vec2(0)});
    } else {
      P.endStroke();
      strokeOwner = 0;
      penEraserStroke = false;
    }
  }
  uvStrokeStartedThisFrame = false;

  // ---- info overlay
  if (d.hasMesh()) {
    std::string selText = vertMode ? F_("%zu selected UV loops", (size_t)std::count(uvLoopSel.begin(), uvLoopSel.end(), 1))
                                   : F_("%zu selected faces", d.selectedFaces().size());
    std::string islText;
    if (hoverIsland >= 0 && hoverIsland < (int)uvIslandInfo.size()) {
      const UVIslandInfo& in = uvIslandInfo[hoverIsland];
      islText = F_("island %d: %d faces%s%s%s", hoverIsland, in.faces, in.pinned ? T_(", pinned") : "",
                   in.copies ? T_(", other half stacked on it") : "", in.isCopy ? T_(", mirrored half (follows the other)") : "");
    }
    std::string buf = hovered ? F_("%u islands  |  %s  |  u %.3f  v %.3f", d.mesh.islandCount, selText.c_str(), mouseUV.x, mouseUV.y)
                              : F_("%u islands  |  %s", d.mesh.islandCount, selText.c_str());
    ImVec2 p(fr.origin.x + 8, fr.origin.y + avail.y - ImGui::GetTextLineHeight() - 6);
    dl->AddText(p, IM_COL32(200, 200, 200, 220), buf.c_str());
    float wy = p.y;
    if (!islText.empty()) {  // the island under the cursor
      wy -= ImGui::GetTextLineHeight() + 4;
      dl->AddText(ImVec2(p.x, wy), IM_COL32(170, 210, 255, 230), islText.c_str());
    }
    if (!uvToastText.empty() && nowSeconds() - uvToastTime < 3.0) {
      ImVec2 ts = ImGui::CalcTextSize(uvToastText.c_str());
      wy -= ts.y + 10;
      ImVec2 tp(fr.origin.x + 8, wy);
      dl->AddRectFilled(tp - ImVec2(5, 3), tp + ts + ImVec2(5, 3), IM_COL32(25, 45, 80, 225), 4.f);
      dl->AddText(tp, IM_COL32(205, 225, 255, 255), uvToastText.c_str());
    }
    // warnings, each with a button that fixes it
    auto warning = [&](const char* text, const char* button, const std::function<void()>& fix) {
      ImVec2 os = ImGui::CalcTextSize(text);
      ImVec2 bs = ImGui::CalcTextSize(button, nullptr, true) + ImGui::GetStyle().FramePadding * 2.f;  // (an L_() label: its ### part hidden)
      wy -= std::max(os.y, bs.y) + 8;
      ImVec2 op(fr.origin.x + 8, wy);
      dl->AddRectFilled(op - ImVec2(4, 2), op + os + ImVec2(4, 2), IM_COL32(120, 40, 20, 200), 3.f);
      dl->AddText(op, IM_COL32(255, 210, 170, 255), text);
      ImGui::SetCursorScreenPos(ImVec2(op.x + os.x + 10, op.y - ImGui::GetStyle().FramePadding.y + 1));
      ImGui::PushID(text);
      if (ImGui::SmallButton(button)) fix();
      ImGui::PopID();
    };
    if (uvMode == UVMode::Edit && uvUnstacked > 0) {
      char wbuf[256];
      snprintf(wbuf, sizeof wbuf,
               P_("%d mirrored face moved off the other half they share texels with",
                  "%d mirrored faces moved off the other half they share texels with", uvUnstacked),
               uvUnstacked);
      warning(wbuf, L_("Re-stack"), [&] {
        uvRunOnSelection("uv_restack", {{"reproject", reprojectOnUVEdit}}, T_("Mirrored halves are stacked on their other half again"));
      });
    } else if (size_t ov = overlappingIslandCount()) {
      char ob[256];
      snprintf(ob, sizeof ob,
               P_("%zu overlapping island: paint appears twice there", "%zu overlapping islands: paint appears twice there", (long)ov), ov);
      warning(ob, L_("Fix"), [&] { uvRunOnSelection("uv_fix_overlaps", {{"reproject", reprojectOnUVEdit}}, T_("Overlaps fixed")); });
    }
    overlappingIslandCount();  // also refreshes uvCollapsed
    if (uvMode == UVMode::Edit && !uvCollapsed.empty()) {
      // faces whose UVs collapsed onto a point or a line: invisible here and never painted
      char cb[256];
      snprintf(cb, sizeof cb,
               P_("%zu face with no UV area: they get no paint", "%zu faces with no UV area: they get no paint", (long)uvCollapsed.size()),
               uvCollapsed.size());
      warning(cb, L_("Select"), [&] {
        cancelUVTransform();
        d.faceSelection.assign(d.mesh.faceCount(), 0);
        for (uint32_t f : uvCollapsed) d.faceSelection[f] = 1;
        d.updateSelectionFlags();
        uvSelectMode = UVSelectMode::Face;
        if (tl.active()) engine.uvOptions.tile = d.mesh.faceTile(uvCollapsed[0]);
        setStatus(T_("Selected the faces with no UV area: unwrap them (right-click > Unwrap selection) to give them some"));
      });
    }
    if (uvMode == UVMode::Edit && !uvXform.active && hovered) {
      const char* hint = T_("Click select | Shift add / remove | Ctrl remove | drag: box / move\n"
                            "Right-click: menu | G R S move rotate scale | P pin | F frame");
      // (one line when it fits)
      const char* wide = T_("Click select | Shift add / remove | Ctrl remove | drag: box / move | Right-click: menu | G R S move rotate scale | "
                            "P pin | F frame");
      if (ImGui::CalcTextSize(wide).x < avail.x - 20) hint = wide;
      ImVec2 hs = ImGui::CalcTextSize(hint);
      ImVec2 hp(fr.origin.x + 8, fr.origin.y + 8);
      dl->AddRectFilled(hp - ImVec2(4, 2), hp + hs + ImVec2(4, 2), IM_COL32(0, 0, 0, 140), 3.f);
      dl->AddText(hp, IM_COL32(200, 200, 200, 230), hint);
    }
  }
  endPanel();
}

}  // namespace patina
