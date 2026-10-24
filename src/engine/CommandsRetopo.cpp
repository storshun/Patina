// Retopology (0.13): patches and density on the open (high-poly) mesh, and the quads made from them.
// See docs/RETOPO_PLAN.md and core/Retopo.h.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include "core/Log.h"
#include "core/MeshCheck.h"
#include "core/Paths.h"
#include "core/MeshIO.h"
#include "core/Retopo.h"
#include "core/RetopoField.h"
#include "core/RetopoLayout.h"
#include "core/RetopoLoops.h"
#include "core/RetopoMirror.h"
#include "core/RetopoQuads.h"
#include "core/RetopoThin.h"
#include "core/UVOps.h"
#include "engine/HighPolyBake.h"
#include "engine/Undo.h"
#include "platform/Net.h"
#include "engine/Engine.h"

namespace patina {

namespace fs = std::filesystem;

namespace {

json prop(const char* type, const char* desc) { return {{"type", type}, {"description", desc}}; }
json num(const char* type, const char* desc, double lo, double hi, json def = json()) {
  json j = {{"type", type}, {"description", desc}};
  if (std::string(type) == "integer") j["minimum"] = (int64_t)lo, j["maximum"] = (int64_t)hi;
  else j["minimum"] = lo, j["maximum"] = hi;
  if (!def.is_null()) j["default"] = def;
  return j;
}
json propNum(const char* desc, double lo, double hi) {
  return {{"type", "number"}, {"minimum", lo}, {"maximum", hi}, {"description", desc}};
}
json schema(json props = json::object(), std::vector<std::string> required = {}) {
  json s = {{"type", "object"}, {"properties", props}, {"additionalProperties", false}};
  if (!required.empty()) s["required"] = required;
  return s;
}

// faces / selection / part
std::vector<uint32_t> targetFaces(Engine& e, const json& a, bool allIfNone) {
  std::vector<uint32_t> f;
  if (a.contains("faces")) {
    for (auto& v : a["faces"]) {
      uint32_t i = v;
      if (i < e.doc.mesh.faceCount()) f.push_back(i);
    }
  } else if (a.value("selection", false)) {
    f = e.doc.selectedFaces();
  } else if (a.contains("part")) {
    f = e.partFaces(a["part"].get<std::string>());
  } else if (allIfNone) {
    f.resize(e.doc.mesh.faceCount());
    for (uint32_t i = 0; i < f.size(); ++i) f[i] = i;
  }
  return f;
}

json facesSchemaProps() {
  return {{"faces", prop("array", "Face indices")},
          {"selection", prop("boolean", "The selected faces")},
          {"part", prop("string", "A part from mesh_parts (region:head, slot:<name>, piece:<n>, group:<name>)")}};
}

// "x", "xz"...: mirror axes bits (bit 0 x)
int mirrorAxes(const json& a) {
  if (!a.contains("mirror") || !a["mirror"].is_string()) return 0;
  int axes = 0;
  for (char ch : a["mirror"].get<std::string>()) axes |= ch == 'x' || ch == 'X' ? 1 : ch == 'y' || ch == 'Y' ? 2 : ch == 'z' || ch == 'Z' ? 4 : 0;
  return axes;
}

json qualityJson(const retopo::Result& r) {
  const auto& q = r.quality;
  auto pct = [](double x) { return std::round(x * 10) / 10; };
  return {{"vertices", q.vertices}, {"regular_pct", pct(q.pct(q.regular))}, {"three_pct", pct(q.pct(q.three))},
          {"five_pct", pct(q.pct(q.five))}, {"other_pct", pct(q.pct(q.other))},
          {"quads_pct", q.faces ? pct(100.0 * q.quads / q.faces) : 0.0}};
}

json resultJson(const retopo::Result& r) {
  json patches = json::array();
  for (auto& p : r.patches)
    patches.push_back({{"label", p.label}, {"sides", p.sides}, {"ring", p.ring}, {"quads", p.quads}, {"faces", p.faces},
                       {"problem", p.problem}});
  return {{"quads", r.quads}, {"vertices", r.low.positions.size()}, {"poles", r.poles}, {"edge_length", r.edgeLength},
          {"patches", patches}, {"problems", r.problems}, {"seconds", r.seconds}, {"quality", qualityJson(r)},
          {"parity_fixes", r.parityFixes}, {"kept", r.localKept}};
}

// (1.1) The quads as they leave the retopology (a model, a file, Blender), facing out: each turned like its
// neighbors and closed parts facing out (the Mesh Check's fix), and an open one turned over when most of it faces away
// from the model it was made on. Around parts that meet inside the model (an eye in its socket) a few could come out
// turned inside out, which a normal map cannot make look right and a game engine does not draw.
Mesh outwardQuads(const Mesh& quads, const Mesh& high, Bvh& hb, int* turned = nullptr) {
  MeshFixOptions fo;
  fo.fixWinding = true;
  MeshFixResult fr;
  Mesh m = fixMesh(quads, fo, &fr);
  int n = (int)fr.flipped;
  if (hb.empty()) hb.build(high);
  long away = 0, seen = 0;
  for (uint32_t f = 0; f < m.faceCount(); f += std::max(1u, m.faceCount() / 4000)) {
    RayHit h = hb.closestPoint(high, m.faceCenter(f));
    if (!h.hit) continue;
    const auto& t = high.tris[h.tri];
    vec3 hn = high.loopNormal[t[0]] * (1.f - h.bary.x - h.bary.y) + high.loopNormal[t[1]] * h.bary.x +
              high.loopNormal[t[2]] * h.bary.y;
    ++seen, away += glm::dot(m.faceNormal(f), hn) < 0.f;
  }
  if (away * 2 > seen) {  // all of it the wrong way round
    for (uint32_t f = 0; f < m.faceCount(); ++f) {
      uint32_t a = m.faceStart[f], b = m.faceStart[f + 1];
      std::reverse(m.loopVert.begin() + a, m.loopVert.begin() + b);
      for (auto* uv : {&m.loopUV, &m.loopUV2})
        if (uv->size() == m.loopVert.size()) std::reverse(uv->begin() + a, uv->begin() + b);
      if (m.loopColor.size() == m.loopVert.size()) std::reverse(m.loopColor.begin() + a, m.loopColor.begin() + b);
    }
    m.hasSourceNormals = false;
    m.tris.clear();
    m.finalize();
    n = (int)m.faceCount() - n;
  }
  if (n) PLOG_I("Retopology: %d quads turned to face out", n);
  if (turned) *turned = n;
  return m;
}

}  // namespace

Engine::RetopoState& Engine::retopo() {
  RetopoState& s = retopoData;
  const Mesh& m = doc.mesh;
  if (s.faces != m.faceCount() || s.verts != m.positions.size() || s.meshPath != doc.meshPath) {
    s.faceLabel.assign(m.faceCount(), 0);
    s.density.assign(m.positions.size(), 1.f);
    s.nextLabel = 1;
    s.result.reset();
    s.faces = m.faceCount(), s.verts = (uint32_t)m.positions.size();
    s.meshPath = doc.meshPath;
    s.graph = {};
    s.lines.clear(), s.zoneSeeds.clear(), s.strokes.clear(), s.zones.clear();
    s.faceAdjStart.clear(), s.faceAdj.clear();
    s.flowCache = retopo::makeFlowCache();
    {
      vec3 lo(1e30f), hi(-1e30f);
      for (auto& p : m.positions) lo = glm::min(lo, p), hi = glm::max(hi, p);
      s.symCenter = m.positions.empty() ? vec3(0) : 0.5f * (lo + hi);
      s.modelSize = m.positions.empty() ? 1.f : glm::length(hi - lo);
    }
    ++s.linesVersion, ++s.densityVersion, ++s.inputsVersion;
    s.zonesVersion = ~0ull, s.segsVersion = ~0ull, s.markSegsVersion = ~0ull;
    s.resultIsPreview = false;
    ++s.version;
    loadRetopo();
    s.savedVersion = s.version;
  }
  return s;
}

namespace {
// faces across each edge that two share (compressed rows)
void buildFaceAdjacency(const Mesh& m, std::vector<uint32_t>& start, std::vector<uint32_t>& adj) {
  const size_t E = m.edges.size(), F = m.faceCount();
  std::vector<uint32_t> f0(E, ~0u), f1(E, ~0u);
  std::vector<uint8_t> n(E, 0);
  for (uint32_t f = 0; f < F; ++f)
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) {
      uint32_t e = m.loopEdge[l];
      if (n[e] == 0) f0[e] = f;
      else if (n[e] == 1) f1[e] = f;
      if (n[e] < 3) n[e]++;
    }
  start.assign(F + 1, 0);
  for (uint32_t f = 0; f < F; ++f)
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) start[f + 1] += n[m.loopEdge[l]] == 2;
  for (size_t f = 0; f < F; ++f) start[f + 1] += start[f];
  adj.resize(start[F]);
  std::vector<uint32_t> at(start.begin(), start.end() - 1);
  for (uint32_t f = 0; f < F; ++f)
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) {
      uint32_t e = m.loopEdge[l];
      if (n[e] == 2) adj[at[f]++] = f0[e] == f ? f1[e] : f0[e];
    }
}

// The faces a line on the surface goes over: from the face under each point to the face under the
// next, halving the step until the two are the same or neighbors, so a closed line always cuts the
// surface in two (however coarse or fine the mesh).
void markLineFaces(const Mesh& m, const Bvh& bvh, const std::vector<uint32_t>& start, const std::vector<uint32_t>& adj,
                   const retopo::Stroke& line, std::vector<uint8_t>& onLine) {
  auto faceAt = [&](const vec3& p, vec3& q) -> int64_t {
    RayHit h = bvh.closestPoint(m, p, 1e30f);
    if (!h.hit) return -1;
    q = h.position;
    return h.face;
  };
  auto neighbors = [&](uint32_t a, uint32_t b) {
    if (a == b) return true;
    for (uint32_t i = start[a]; i < start[a + 1]; ++i)
      if (adj[i] == b) return true;
    return false;
  };
  std::function<void(vec3, int64_t, vec3, int64_t, int)> seg = [&](vec3 pa, int64_t fa, vec3 pb, int64_t fb, int depth) {
    if (fa < 0 || fb < 0) return;
    onLine[fa] = onLine[fb] = 1;
    if (neighbors((uint32_t)fa, (uint32_t)fb) || depth > 14) return;
    vec3 qm;
    int64_t fm = faceAt(0.5f * (pa + pb), qm);
    if (fm < 0) return;
    seg(pa, fa, qm, fm, depth + 1);
    seg(qm, fm, pb, fb, depth + 1);
  };
  const auto& P = line.points;
  if (P.empty()) return;
  size_t n = P.size() + (line.loop && P.size() > 2 ? 1 : 0);
  vec3 qa;
  int64_t fa = faceAt(P[0], qa);
  if (fa >= 0) onLine[fa] = 1;
  for (size_t i = 1; i < n; ++i) {
    vec3 qb;
    int64_t fb = faceAt(P[i % P.size()], qb);
    seg(qa, fa, qb, fb, 0);
    qa = qb, fa = fb;
  }
}

// 0.14.0 kept zone lines as the mesh's edges: chained into lines, smoothed and put back on the surface
std::vector<retopo::Stroke> edgeLinesToStrokes(const Mesh& m, const Bvh& bvh, const std::vector<uint32_t>& lineEdges) {
  std::vector<retopo::Stroke> out;
  std::unordered_map<uint32_t, std::vector<uint32_t>> at;
  for (uint32_t e : lineEdges)
    if (e < m.edges.size()) at[m.edges[e].x].push_back(e), at[m.edges[e].y].push_back(e);
  std::unordered_set<uint32_t> used;
  for (uint32_t e0 : lineEdges) {
    if (e0 >= m.edges.size() || used.count(e0)) continue;
    std::vector<uint32_t> verts = {m.edges[e0].x, m.edges[e0].y};
    used.insert(e0);
    for (int dirn = 0; dirn < 2; ++dirn)
      for (size_t guard = 0; guard <= lineEdges.size(); ++guard) {
        uint32_t v = dirn ? verts.front() : verts.back();
        auto& es = at[v];
        if (es.size() != 2) break;
        uint32_t e = used.count(es[0]) ? es[1] : es[0];
        if (used.count(e)) break;
        used.insert(e);
        uint32_t w = m.edges[e].x == v ? m.edges[e].y : m.edges[e].x;
        if (dirn) verts.insert(verts.begin(), w);
        else verts.push_back(w);
      }
    retopo::Stroke st;
    st.edge = true;
    bool closed = verts.size() > 3 && verts.front() == verts.back();
    if (closed) verts.pop_back(), st.loop = true;
    for (uint32_t v : verts) st.points.push_back(m.positions[v]);
    auto& P = st.points;
    const size_t n = P.size();
    for (int it = 0; n >= 3 && it < 40; ++it) {
      float w = it % 2 ? -0.53f : 0.5f;
      std::vector<vec3> next = P;
      for (size_t i = 0; i < n; ++i) {
        if (!closed && (i == 0 || i + 1 == n)) continue;
        next[i] = P[i] + w * (0.5f * (P[(i + n - 1) % n] + P[(i + 1) % n]) - P[i]);
      }
      P = std::move(next);
    }
    if (!bvh.empty())
      for (auto& p : P)
        if (RayHit h = bvh.closestPoint(m, p, 1e30f); h.hit) p = h.position;
    out.push_back(std::move(st));
  }
  return out;
}
}  // namespace

const std::vector<int32_t>& Engine::retopoZones() {
  RetopoState& S = retopo();
  if (S.zonesVersion == S.linesVersion && S.zones.size() == S.faces) return S.zones;
  const Mesh& m = doc.mesh;
  S.zonesVersion = S.linesVersion;
  const uint32_t F = m.faceCount();
  S.zones.assign(F, 0);
  if (S.faceAdjStart.size() != F + 1) buildFaceAdjacency(m, S.faceAdjStart, S.faceAdj);
  // the faces the lines go over divide the rest; they then join the zone beside them
  std::vector<uint8_t> onLine(F, 0);
  if (!S.lines.empty()) {
    if (doc.bvh.empty()) doc.bvh.build(m);
    for (auto& l : S.lines) markLineFaces(m, doc.bvh, S.faceAdjStart, S.faceAdj, l, onLine);
  }
  int next = 0;
  std::vector<uint32_t> stack;
  for (uint32_t f0 = 0; f0 < F; ++f0) {
    if (S.zones[f0] || onLine[f0]) continue;
    int z = ++next;
    stack = {f0};
    S.zones[f0] = z;
    while (!stack.empty()) {
      uint32_t f = stack.back();
      stack.pop_back();
      for (uint32_t i = S.faceAdjStart[f]; i < S.faceAdjStart[f + 1]; ++i) {
        uint32_t g = S.faceAdj[i];
        if (S.zones[g] || onLine[g] || S.faceLabel[g] != S.faceLabel[f]) continue;
        S.zones[g] = z, stack.push_back(g);
      }
    }
  }
  // the line's own faces: the zone of a neighbor (in rounds, for lines several faces wide)
  for (int round = 0; round < 64; ++round) {
    bool left = false;
    std::vector<std::pair<uint32_t, int>> set;
    for (uint32_t f = 0; f < F; ++f) {
      if (S.zones[f]) continue;
      for (uint32_t i = S.faceAdjStart[f]; i < S.faceAdjStart[f + 1]; ++i)
        if (int z = S.zones[S.faceAdj[i]]) {
          set.push_back({f, z});
          break;
        }
      left = true;
    }
    for (auto [f, z] : set) S.zones[f] = z;
    if (!left || set.empty()) break;
  }
  for (uint32_t f = 0; f < F; ++f)
    if (!S.zones[f]) S.zones[f] = ++next;  // (a piece covered by a line: a zone of its own)
  S.zoneCount = next;
  if (S.selectedZone > S.zoneCount) S.selectedZone = 0;
  return S.zones;
}

float Engine::retopoZoneResolution(int zone) {
  RetopoState& S = retopo();
  const auto& z = retopoZones();
  float r = 1.f;
  for (auto& [f, res] : S.zoneSeeds)  // the last setting inside the zone counts
    if (f < z.size() && z[f] == zone) r = res;
  return r;
}

std::vector<float> Engine::retopoFaceResolution() {
  RetopoState& S = retopo();
  if (S.zoneSeeds.empty()) return {};
  const auto& zones = retopoZones();
  std::map<int, float> res;
  for (auto& [f, r] : S.zoneSeeds)
    if (f < zones.size()) res[zones[f]] = r;
  std::vector<float> out(zones.size(), 1.f);
  for (size_t f = 0; f < zones.size(); ++f)
    if (auto it = res.find(zones[f]); it != res.end()) out[f] = it->second;
  return out;
}

json Engine::retopoThinZones(bool apply, int quadsAround, float maxResolution) {
  RetopoState& S = retopo();
  const Mesh& m = doc.mesh;
  if (doc.bvh.empty()) doc.bvh.build(m);
  // the quads' size at each face as things are now (zones and the density brush)
  const float L0 = 1.f / std::sqrt(std::max(S.quadsPerM2, 1e-3f));
  std::vector<float> res = retopoFaceResolution(), faceL(m.faceCount(), L0);
  for (uint32_t f = 0; f < m.faceCount(); ++f) {
    float d = 0;
    uint32_t n = m.faceStart[f + 1] - m.faceStart[f];
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) d += S.density.empty() ? 1.f : S.density[m.loopVert[l]];
    d = std::max(d / std::max(n, 1u), 0.05f) * (res.empty() ? 1.f : res[f]);
    faceL[f] = L0 / std::sqrt(d);
  }
  auto all = retopo::findThinParts(m, doc.bvh, faceL, std::clamp(quadsAround, 3, 16));
  json list = json::array();
  std::vector<retopo::ThinPart> parts;
  int tooThin = 0, sheets = 0, unclear = 0;
  for (auto& p : all) {
    vec3 c = p.seedFace < m.faceCount() ? m.faceCenter(p.seedFace) : vec3(0);
    // (a part that needs more than maxResolution, as a strand of hair, is left: its zone would be huge
    // in quads, and the user can give it one by hand)
    bool take = p.tube && p.clean && p.resolution <= maxResolution + 1e-3f;
    json item = {{"lines", p.loops.size()}, {"thickness", p.thickness}, {"resolution", p.resolution}, {"area", p.area},
                 {"face", p.seedFace}, {"at", {c.x, c.y, c.z}}, {"tube", p.tube}, {"clean", p.clean}, {"given", take}};
    if (!apply) {  // (what the lines would be)
      json pts = json::array();
      for (auto& l : p.loops) {
        json one = json::array();
        for (auto& q : l) one.push_back({q.x, q.y, q.z});
        pts.push_back(one);
      }
      item["line_points"] = pts;
    }
    list.push_back(item);
    if (take) parts.push_back(p);
    else if (!p.tube) ++sheets;
    else if (!p.clean) ++unclear;
    else ++tooThin;
  }
  json out = {{"parts", list}, {"applied", false}, {"left_too_thin", tooThin}, {"left_sheets", sheets}, {"left_no_clean_cut", unclear}};
  if (!apply || parts.empty()) return out;
  // the lines, then (with the zones they make) the resolutions; one undo step for both
  auto beforeLines = S.lines;
  auto beforeSeeds = S.zoneSeeds;
  std::vector<std::pair<size_t, size_t>> range;  // each part's lines in S.lines
  for (auto& p : parts) {
    size_t first = S.lines.size();
    for (auto& l : p.loops) {
      retopo::Stroke st;
      st.points = l, st.loop = true, st.edge = true;
      S.lines.push_back(std::move(st));
    }
    range.push_back({first, S.lines.size()});
  }
  retopoLinesChanged();
  // (only where the lines did close the part off: its zone about its own size, not the whole model; the
  // lines of a part they didn't close off go again)
  std::vector<char> closedOff(parts.size(), 0);
  {
    const auto& z = retopoZones();
    std::map<int, double> zoneArea;
    for (uint32_t f = 0; f < z.size(); ++f) zoneArea[z[f]] += m.faceArea3D(f);
    for (size_t i = 0; i < parts.size(); ++i) {
      // (the zone holding most of the part)
      std::map<int, int> votes;
      for (uint32_t f : parts[i].sampleFaces)
        if (f < z.size()) votes[z[f]]++;
      int best = -1, bestN = 0;
      for (auto& [zz, n] : votes)
        if (n > bestN) bestN = n, best = zz;
      if (best >= 0)
        for (uint32_t f : parts[i].sampleFaces)
          if (f < z.size() && z[f] == best) {
            parts[i].seedFace = f;
            break;
          }
      closedOff[i] = parts[i].seedFace < z.size() && zoneArea[z[parts[i].seedFace]] <= 2.5 * parts[i].area + 1e-12;
      if (getenv("PATINA_RETOPO_DEBUG") && parts[i].seedFace < z.size()) {
        vec3 c = m.faceCenter(parts[i].seedFace);
        PLOG_I("  thin part at %.3f %.3f %.3f: area %.5f, seed face %u in zone %d of %.5f m2%s", c.x, c.y, c.z, parts[i].area, parts[i].seedFace,
               z[parts[i].seedFace], zoneArea[z[parts[i].seedFace]], closedOff[i] ? "" : " (not closed off)");
      }
    }
  }
  if (std::find(closedOff.begin(), closedOff.end(), 0) != closedOff.end()) {
    std::vector<retopo::Stroke> keep(S.lines.begin(), S.lines.begin() + (long)beforeLines.size());
    for (size_t i = 0; i < parts.size(); ++i)
      if (closedOff[i])
        for (size_t k = range[i].first; k < range[i].second; ++k) keep.push_back(S.lines[k]);
    S.lines = std::move(keep);
    retopoLinesChanged();
  }
  const auto& z = retopoZones();
  int given = 0;
  for (size_t i = 0; i < parts.size(); ++i) {
    auto& p = parts[i];
    if (!closedOff[i] || p.seedFace >= z.size()) continue;
    int zone = z[p.seedFace];
    ++given;
    S.zoneSeeds.erase(std::remove_if(S.zoneSeeds.begin(), S.zoneSeeds.end(), [&](auto& sd) { return sd.first < z.size() && z[sd.first] == zone; }),
                      S.zoneSeeds.end());
    S.zoneSeeds.push_back({p.seedFace, p.resolution});
  }
  retopoChanged();
  auto afterLines = S.lines;
  auto afterSeeds = S.zoneSeeds;
  uint32_t faces = S.faces;
  auto e = std::make_unique<LambdaEntry>();
  e->label = "Zones for thin parts";
  size_t pts = 0;
  for (auto& l : afterLines) pts += l.points.size();
  e->size = pts * sizeof(vec3) * 2 + (beforeSeeds.size() + afterSeeds.size()) * 8 + 64;
  e->onUndo = [this, beforeLines, beforeSeeds, faces](Document&) {
    auto& R = retopo();
    if (R.faces == faces) R.lines = beforeLines, R.zoneSeeds = beforeSeeds, retopoLinesChanged();
  };
  e->onRedo = [this, afterLines, afterSeeds, faces](Document&) {
    auto& R = retopo();
    if (R.faces == faces) R.lines = afterLines, R.zoneSeeds = afterSeeds, retopoLinesChanged();
  };
  doc.undo.push(std::move(e));
  out["applied"] = given > 0;
  out["lines"] = S.lines.size();
  out["zones_given"] = given;
  out["not_closed_off"] = (int)std::count(closedOff.begin(), closedOff.end(), 0);
  return out;
}

std::vector<float> Engine::retopoDensity() {
  RetopoState& S = retopo();
  if (S.zoneSeeds.empty()) return S.density;
  const Mesh& m = doc.mesh;
  const auto& zones = retopoZones();
  std::map<int, float> res;
  for (auto& [f, r] : S.zoneSeeds)
    if (f < zones.size()) res[zones[f]] = r;
  std::vector<float> sum(m.positions.size(), 0.f);
  std::vector<int> n(m.positions.size(), 0);
  for (uint32_t f = 0; f < m.faceCount(); ++f) {
    auto it = res.find(zones[f]);
    float r = it == res.end() ? 1.f : it->second;
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) sum[m.loopVert[l]] += r, n[m.loopVert[l]]++;
  }
  std::vector<float> d = S.density;
  for (size_t v = 0; v < d.size(); ++v)
    if (n[v]) d[v] *= sum[v] / n[v];
  return d;
}

namespace {
float segSegDist2(vec3 p1, vec3 q1, vec3 p2, vec3 q2) {
  vec3 d1 = q1 - p1, d2 = q2 - p2, r = p1 - p2;
  float a = glm::dot(d1, d1), e = glm::dot(d2, d2), f = glm::dot(d2, r), sc = 0, tc = 0;
  if (a <= 1e-20f && e <= 1e-20f) return glm::dot(r, r);
  if (a <= 1e-20f) {
    tc = std::clamp(f / e, 0.f, 1.f);
  } else {
    float c = glm::dot(d1, r);
    if (e <= 1e-20f) {
      sc = std::clamp(-c / a, 0.f, 1.f);
    } else {
      float b = glm::dot(d1, d2), den = a * e - b * b;
      sc = den > 1e-20f ? std::clamp((b * f - c * e) / den, 0.f, 1.f) : 0.f;
      tc = (b * sc + f) / e;
      if (tc < 0) tc = 0, sc = std::clamp(-c / a, 0.f, 1.f);
      else if (tc > 1) tc = 1, sc = std::clamp((b - c) / a, 0.f, 1.f);
    }
  }
  vec3 d = (p1 + d1 * sc) - (p2 + d2 * tc);
  return glm::dot(d, d);
}
}  // namespace

std::vector<retopo::Stroke> Engine::retopoLines(bool forBuild) {
  RetopoState& S = retopo();
  std::vector<retopo::Stroke> out = S.strokes;
  if (forBuild && S.flowLoops)
    for (auto& st : out) st.edge = true;
  if (!forBuild) {
    for (auto& l : S.lines) out.push_back(l);
    return out;
  }
  // for the quads: a line's loose end that runs on past the last line it meets (drawn over to be sure of
  // closing) is cut off there when it is shorter than a quad; a short line that meets nothing goes
  const float L = 1.f / std::sqrt(std::max(S.quadsPerM2, 1e-3f)), tol = 0.2f * L;
  struct Seg {
    vec3 a, b;
    uint32_t line, idx;
  };
  std::vector<Seg> segs;
  for (uint32_t i = 0; i < S.lines.size(); ++i) {
    const auto& P = S.lines[i].points;
    size_t n = P.size() + (S.lines[i].loop && P.size() > 2 ? 1 : 0);
    for (size_t k = 0; k + 1 < n; ++k) segs.push_back({P[k], P[(k + 1) % P.size()], i, (uint32_t)k});
  }
  const float cell = std::max(L, 1e-6f);
  auto key = [](glm::ivec3 c) { return (uint64_t(c.x & 0x1FFFFF) << 42) | (uint64_t(c.y & 0x1FFFFF) << 21) | uint64_t(c.z & 0x1FFFFF); };
  std::unordered_map<uint64_t, std::vector<uint32_t>> grid;
  for (uint32_t k = 0; k < segs.size(); ++k) {
    glm::ivec3 lo = glm::ivec3(glm::floor((glm::min(segs[k].a, segs[k].b) - tol) / cell));
    glm::ivec3 hi = glm::ivec3(glm::floor((glm::max(segs[k].a, segs[k].b) + tol) / cell));
    if (glm::any(glm::greaterThan(hi - lo, glm::ivec3(32)))) continue;
    for (int z = lo.z; z <= hi.z; ++z)
      for (int y = lo.y; y <= hi.y; ++y)
        for (int x = lo.x; x <= hi.x; ++x) grid[key({x, y, z})].push_back(k);
  }
  for (uint32_t i = 0; i < S.lines.size(); ++i) {
    retopo::Stroke st = S.lines[i];
    st.edge = true;
    auto& P = st.points;
    if (st.loop || P.size() < 2) {
      if (P.size() >= 2) out.push_back(std::move(st));
      continue;
    }
    // where along it other lines touch it (segment index), and its arc length
    std::vector<float> arc(P.size(), 0.f);
    for (size_t k = 1; k < P.size(); ++k) arc[k] = arc[k - 1] + glm::length(P[k] - P[k - 1]);
    int first = -1, last = -1;
    for (size_t k = 0; k + 1 < P.size(); ++k) {
      glm::ivec3 c = glm::ivec3(glm::floor(0.5f * (P[k] + P[k + 1]) / cell));
      bool touch = false;
      for (int dz = -1; dz <= 1 && !touch; ++dz)
        for (int dy = -1; dy <= 1 && !touch; ++dy)
          for (int dx = -1; dx <= 1 && !touch; ++dx) {
            auto it = grid.find(key(c + glm::ivec3(dx, dy, dz)));
            if (it == grid.end()) continue;
            for (uint32_t q : it->second)
              if (segs[q].line != i && segSegDist2(P[k], P[k + 1], segs[q].a, segs[q].b) < tol * tol) {
                touch = true;
                break;
              }
          }
      if (touch) {
        if (first < 0) first = (int)k;
        last = (int)k + 1;
      }
    }
    float total = arc.back();
    if (first < 0) {
      if (total >= L) out.push_back(std::move(st));
      continue;
    }
    size_t a = arc[first] < L ? (size_t)first : 0, b = total - arc[last] < L ? (size_t)last : P.size() - 1;
    if (b <= a) continue;
    st.points = std::vector<vec3>(P.begin() + a, P.begin() + b + 1);
    out.push_back(std::move(st));
  }
  return out;
}

double Engine::retopoQuadsAsked() {
  RetopoState& S = retopo();
  if (!doc.hasMesh()) return 0;
  if (S.quadsAskedInputs != S.inputsVersion) {
    S.quadsAskedInputs = S.inputsVersion;
    const Mesh& m = doc.mesh;
    auto dens = retopoDensity();
    double area = 0;
    for (uint32_t f = 0; f < m.faceCount(); ++f) {
      float dsum = 0;
      for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) dsum += l < m.loopVert.size() && m.loopVert[l] < dens.size() ? dens[m.loopVert[l]] : 1.f;
      area += m.faceArea3D(f) * dsum / float(m.faceStart[f + 1] - m.faceStart[f]);
    }
    S.quadsAsked = area * S.quadsPerM2;
  }
  return S.quadsAsked;
}

bool Engine::startRetopoBuild(int relax, bool preview) {
  RetopoState& S = retopo();
  if (!doc.hasMesh()) return false;
  // (1.2.9) a preview of a great many quads would take minutes and tens of GB (a million asked for took 114 GB):
  // only Make quads makes those
  S.previewTooBig = preview && S.method == 0 && retopoQuadsAsked() > kRetopoPreviewMaxQuads;
  if (S.previewTooBig) return false;
  // 0.16: the preview is made just as the full build is: one made from what is there now is the result
  // (not for Remake all)
  if (!preview && !S.remakeAll && S.method == 0 && S.result && S.resultIsPreview && S.resultInputs == S.inputsVersion && !S.building.valid()) {
    S.resultIsPreview = false, ++S.version, S.resultVersion = S.version;
    return true;
  }
  if (!preview && !S.remakeAll && S.method == 0 && S.building.valid() && S.buildIsPreview && S.buildInputs == S.inputsVersion) {
    S.buildIsPreview = false;  // (the preview running now becomes the full build)
    return true;
  }
  if (S.building.valid()) {
    // the full build waits for one running; a preview running is stopped for it
    if (!preview) {
      S.fullPending = true;
      if (S.buildIsPreview && S.cancel) S.cancel->store(true);
      return true;
    }
    return false;
  }
  S.fullPending = false;
  S.buildInputs = S.inputsVersion, S.buildIsPreview = preview;
  S.cancel = std::make_shared<std::atomic<bool>>(false);
  if (doc.bvh.empty()) doc.bvh.build(doc.mesh);
  retopo::Options o;
  o.quadsPerM2 = S.quadsPerM2;
  o.relaxIterations = relax;
  S.buildStarted = nowSeconds();
  // the mesh stays put while it runs (a load waits for it: Document::beforeMeshChange)
  if (S.method == 0) {
    retopo::FlowOptions fo;
    fo.quadsPerM2 = S.quadsPerM2;
    fo.ringZones = S.ringZones;
    fo.requantize = S.fewerPoles;
    fo.mirrorAxes = retopoSymmetryAxes(), fo.mirrorCenter = S.symCenter;
    fo.cancel = S.cancel.get();
    fo.cache = S.flowCache;
    // (0.23) only what changed since the last quads made again (Remake all: everything)
    if (S.result && S.result->builtFrom && S.keepAway && !S.remakeAll) fo.previous = std::make_shared<retopo::Result>(*S.result);
    S.remakeAll = false;
    S.building = std::async(std::launch::async, [this, labels = S.faceLabel, dens = S.density, res = retopoFaceResolution(),
                                                  strokes = retopoLines(), fo, keep = S.cancel] {
      return retopo::flowQuads(doc.mesh, doc.bvh, strokes, labels, dens, fo, res);
    });
  } else {
    S.building = std::async(std::launch::async, [this, labels = S.faceLabel, dens = S.density, o] {
      return retopo::build(doc.mesh, doc.bvh, labels, dens, o);
    });
  }
  return true;
}

bool Engine::retopoRemakeAll() {
  RetopoState& S = retopo();
  if (!doc.hasMesh()) return false;
  S.remakeAll = true;
  // (a build running is stopped for it)
  if (S.building.valid()) {
    S.fullPending = true;
    if (S.cancel) S.cancel->store(true);
    return true;
  }
  return startRetopoBuild(30, false);
}

void Engine::waitRetopoBuild() {
  if (retopoData.building.valid()) retopoData.building.wait();
  retopoTick();
}

void Engine::retopoChanged() {
  RetopoState& S = retopo();
  ++S.version, ++S.inputsVersion;
  S.changedAt = nowSeconds();
  // a preview made from what has just changed is no use: stopped (a new one starts in a moment)
  if (S.building.valid() && S.buildIsPreview && S.cancel) S.cancel->store(true);
}

int Engine::retopoHoverLoop(const vec3* p) {
  RetopoState& S = retopo();
  int was = S.hoverLoop;
  S.hoverLoop = -1;
  if (p && S.result && S.loops && S.loopFinder && S.loopsVersion == S.resultVersion)
    S.hoverLoop = S.loopFinder->nearest(S.result->low, *p, 0.75f / std::sqrt(std::max(S.quadsPerM2, 1e-3f)));
  (void)was;
  if (S.hoverLoop < 0 || !S.loops) return 0;
  const auto& lp = S.loops->loops[S.hoverLoop];
  return (int)(lp.size() - 1 + (S.loops->closed[S.hoverLoop] ? 1 : 0));
}

void Engine::retopoLinesChanged() {
  ++retopo().linesVersion;
  retopoChanged();
}

float Engine::retopoDeadzone() {
  RetopoState& S = retopo();
  return S.symDeadzone >= 0 ? S.symDeadzone : 0.01f * S.modelSize;
}

int Engine::retopoSymmetryAxes() const {
  return (painter.symmetry[0] ? 1 : 0) | (painter.symmetry[1] ? 2 : 0) | (painter.symmetry[2] ? 4 : 0);
}

std::vector<retopo::Stroke> Engine::retopoMirror(const retopo::Stroke& st, int axes) {
  RetopoState& S = retopo();
  if (!axes) return {st};
  auto out = retopo::mirrorStroke(st, axes, S.symCenter, retopoDeadzone());
  if (doc.bvh.empty()) doc.bvh.build(doc.mesh);
  // mirror images on the surface (a model is seldom exactly symmetric); points on a plane stay on it
  for (auto& o : out)
    for (auto& p : o.points) {
      RayHit h = doc.bvh.closestPoint(doc.mesh, p, 0.05f * S.modelSize);
      if (!h.hit) continue;
      vec3 q = h.position;
      for (int k = 0; k < 3; ++k)
        if ((axes & (1 << k)) && p[k] == S.symCenter[k]) q[k] = p[k];
      p = q;
    }
  return out;
}

std::vector<vec3> Engine::retopoRingLoop(const vec3& p, int mode, const vec3* dir, std::string* why, int* used) {
  if (!doc.hasMesh()) {
    if (why) *why = "no mesh open";
    return {};
  }
  if (doc.bvh.empty()) doc.bvh.build(doc.mesh);
  std::string whyAround;
  if (mode != 2) {
    auto loop = retopo::ringAround(doc.mesh, doc.bvh, p, &whyAround);
    if (loop.size() >= 4 || mode == 1) {
      if (why) *why = whyAround;
      if (used) *used = 1;
      return loop;
    }
  }
  // (parallel to the screen: the line of sight to p would tilt it with the camera's place, a long way down a
  // body framed whole)
  vec3 d = dir ? *dir : objectCamera().forward();
  std::string whyFacing;
  auto loop = retopo::facingLoop(doc.mesh, doc.bvh, p, d, &whyFacing);
  if (why) *why = mode == 2 || whyAround.empty() ? whyFacing : whyAround + "; " + whyFacing;
  if (used) *used = 2;
  return loop;
}

size_t Engine::retopoAddRing(const vec3& p, int mirrorAxes, std::string* why, int mode, const vec3* dir, int* used) {
  std::vector<vec3> loop = retopoRingLoop(p, mode, dir, why, used);
  if (loop.size() < 4) return 0;
  // (a section of a dense model has a point per triangle it crosses: kept to about 256 round)
  double len = 0;
  for (size_t i = 0; i < loop.size(); ++i) len += glm::length(loop[(i + 1) % loop.size()] - loop[i]);
  const double step = len / 256;
  std::vector<vec3> pts{loop[0]};
  double acc = 0;
  for (size_t i = 1; i < loop.size(); ++i) {
    acc += glm::length(loop[i] - loop[i - 1]);
    if (acc >= step) pts.push_back(loop[i]), acc = 0;
  }
  retopo::Stroke st;
  st.points = std::move(pts), st.edge = true, st.loop = true;
  auto lines = retopo().lines;
  size_t added = 0;
  for (auto& m : retopoMirror(st, mirrorAxes)) lines.push_back(std::move(m)), ++added;
  if (added) retopoSetLines(std::move(lines), "Ring line");
  return added;
}

void Engine::retopoSetConfirmed(std::vector<char> confirmed, const char* label) {
  RetopoState& S = retopo();
  std::vector<char> before(S.lines.size());
  for (size_t k = 0; k < S.lines.size(); ++k) before[k] = S.lines[k].confirmed;
  confirmed.resize(S.lines.size(), 0);
  if (confirmed == before) return;
  // (only how they are drawn and snapped to: no new zones, no new build)
  auto apply = [this](const std::vector<char>& f) {
    auto& R = retopo();
    if (f.size() != R.lines.size()) return;
    for (size_t k = 0; k < f.size(); ++k) R.lines[k].confirmed = f[k] != 0;
    ++R.version, R.changedAt = nowSeconds();
  };
  apply(confirmed);
  auto e = std::make_unique<LambdaEntry>();
  e->label = label;
  e->size = 64 + 2 * confirmed.size();
  const uint32_t faces = S.faces;
  e->onUndo = [this, apply, before, faces](Document&) {
    if (retopo().faces == faces) apply(before);
  };
  e->onRedo = [this, apply, after = std::move(confirmed), faces](Document&) {
    if (retopo().faces == faces) apply(after);
  };
  doc.undo.push(std::move(e));
}

void Engine::retopoSetLines(std::vector<retopo::Stroke> after, const char* label) {
  RetopoState& S = retopo();
  auto before = S.lines;
  auto seedsBefore = S.zoneSeeds;
  const std::vector<float> oldRes = retopoFaceResolution();  // (per face; empty: all x1)
  uint32_t faces = S.faces;
  S.lines = after;
  retopoLinesChanged();
  // a zone a new line splits off keeps the resolution it had (eyes drawn on a head at x8 are x8 too):
  // each zone with no setting of its own takes the resolution most of its faces had
  if (!oldRes.empty()) {
    const auto& z = retopoZones();
    std::set<int> seeded;
    for (auto& [f, r] : S.zoneSeeds)
      if (f < z.size()) seeded.insert(z[f]);
    std::map<int, std::map<float, std::pair<int, uint32_t>>> votes;  // zone -> resolution -> (faces, one of them)
    std::map<int, int> total;
    for (uint32_t f = 0; f < z.size() && f < oldRes.size(); ++f) {
      if (seeded.count(z[f])) continue;
      auto& v = votes[z[f]][oldRes[f]];
      if (v.first++ == 0) v.second = f;
      total[z[f]]++;
    }
    for (auto& [zone, v] : votes) {
      auto best = std::max_element(v.begin(), v.end(), [](auto& a, auto& b) { return a.second.first < b.second.first; });
      if (best->first != 1.f && best->second.first * 2 > total[zone]) S.zoneSeeds.push_back({best->second.second, best->first});
    }
  }
  auto seedsAfter = S.zoneSeeds;
  size_t pts = 0;
  for (auto& l : before) pts += l.points.size();
  for (auto& l : after) pts += l.points.size();
  auto e = std::make_unique<LambdaEntry>();
  e->label = label;
  e->size = pts * sizeof(vec3) + 64 + (seedsBefore.size() + seedsAfter.size()) * 8;
  e->onUndo = [this, before, seedsBefore, faces](Document&) {
    auto& R = retopo();
    if (R.faces == faces) R.lines = before, R.zoneSeeds = seedsBefore, retopoLinesChanged();
  };
  e->onRedo = [this, after = std::move(after), seedsAfter, faces](Document&) {
    auto& R = retopo();
    if (R.faces == faces) R.lines = after, R.zoneSeeds = seedsAfter, retopoLinesChanged();
  };
  doc.undo.push(std::move(e));
}

void Engine::retopoTick() {
  RetopoState& S = retopoData;
  if (S.building.valid() && S.building.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    retopo::Result r = S.building.get();
    if (doc.mesh.faceCount() == S.faces && !r.cancelled) {
      S.result = std::make_unique<retopo::Result>(std::move(r));
      S.resultInputs = S.buildInputs, S.resultIsPreview = S.buildIsPreview;
      ++S.version;
      S.resultVersion = S.version;
    }
  }
  // (with the Sym buttons the quads are mirrored: switching one changes them)
  if (int axes = retopoSymmetryAxes(); axes != S.symAxesSeen) {
    S.symAxesSeen = axes;
    if (doc.hasMesh()) retopoChanged();
  }
  if (!S.building.valid() && doc.hasMesh() && features.on("retopology")) {
    // (wanted within the last few seconds: the window asks each frame it shows the tools, and a frame can
    // take a second on a heavy model)
    double now = nowSeconds();
    if (S.fullPending) {
      startRetopoBuild(30, false);
    } else if (S.livePreview && !drawingStroke && now - S.wantPreviewAt < 3.0 && S.method == 0 && S.inputsVersion != S.resultInputs &&
               S.inputsVersion != S.previewTried && now - S.changedAt > 0.35 && !painter.active()) {
      S.previewTried = S.inputsVersion;
      startRetopoBuild(30, true);
    }
  }
  // the patches are kept a few seconds after the last change (not in the middle of painting)
  if (S.version != S.savedVersion && S.changedAt > 0 && nowSeconds() - S.changedAt > 3.0 && !painter.active()) saveRetopo();
}

void Engine::retopoViewOptions(ViewportOptions& o) {
  if (!features.on("retopology") || !doc.hasMesh()) {
    o.retopoView = 0, o.retopoQuads = nullptr;
    return;
  }
  RetopoState& S = retopo();
  // (the zones are only found again when the lines change, and only when shown)
  if (o.retopoView == 1) o.retopoLabels = &retopoZones();
  o.retopoDensity = &S.density;
  o.retopoKey = o.retopoView == 1 ? S.linesVersion * 2 : S.densityVersion * 2 + 1;
  if (S.segsVersion != S.version) {
    S.segsVersion = S.version;
    S.zoneLineSegs.clear(), S.strokeSegs.clear(), S.doneLineSegs.clear();
    auto add = [](std::vector<vec3>& out, const retopo::Stroke& st) {
      const auto& P = st.points;
      size_t n = P.size() + (st.loop && P.size() > 2 ? 1 : 0);
      for (size_t k = 0; k + 1 < n; ++k) out.push_back(P[k]), out.push_back(P[(k + 1) % P.size()]);
    };
    for (auto& l : S.lines) add(l.confirmed ? S.doneLineSegs : S.zoneLineSegs, l);
    for (auto& st : S.strokes) add(S.strokeSegs, st);
  }
  bool lines = o.retopoView || o.retopoShowQuads;
  o.retopoLines = lines ? &S.zoneLineSegs : nullptr;
  o.retopoLinesDone = lines ? &S.doneLineSegs : nullptr;
  o.retopoStrokes = lines ? &S.strokeSegs : nullptr;
  // where the last quads left the surface out: small red crosses
  if (S.markSegsVersion != S.resultVersion) {
    S.markSegsVersion = S.resultVersion;
    S.markSegs.clear();
    if (S.result) {
      float c = 0.15f / std::sqrt(std::max(S.quadsPerM2, 1e-3f));
      for (const vec3& p : S.result->uncovered)
        for (int k = 0; k < 3; ++k) {
          vec3 d(0);
          d[k] = c;
          S.markSegs.push_back(p - d), S.markSegs.push_back(p + d);
        }
    }
  }
  o.retopoMarks = o.retopoShowQuads && o.retopoShowMarks && !S.markSegs.empty() ? &S.markSegs : nullptr;
  o.retopoLinesKey = S.version ^ (S.resultVersion << 32);
  o.retopoSelectedZone = S.selectedZone;
  o.retopoQuads = S.result && o.retopoShowQuads && S.result->low.faceCount() ? &S.result->low : nullptr;
  o.retopoQuadsKey = S.resultVersion;
  // the poles, and the edge loop under the pointer (the loops are found when first shown for a result)
  if (o.retopoLoops && o.retopoQuads) {
    if (S.loopsVersion != S.resultVersion) {
      S.loopsVersion = S.resultVersion, S.drawnLoop = -2;
      S.loops = std::make_shared<retopo::EdgeLoops>(retopo::edgeLoops(S.result->low));
      S.loopFinder = std::make_shared<retopo::LoopFinder>();
      S.loopFinder->build(S.result->low, *S.loops);
      S.hoverLoop = -1;
    }
    if (S.drawnLoop != S.hoverLoop) {
      S.drawnLoop = S.hoverLoop, ++S.loopSegsKey;
      retopo::loopSegments(S.result->low, *S.loops, 0.2f / std::sqrt(std::max(S.quadsPerM2, 1e-3f)), S.hoverLoop, S.loopPts, S.loopCols);
    }
  }
  o.retopoLoopPts = &S.loopPts, o.retopoLoopCols = &S.loopCols, o.retopoLoopsKey = S.loopSegsKey;
  o.retopoQuadsPreview = S.resultIsPreview;
  // the high-poly's own seams and wires would hide the patches and the quads
  if (o.retopoView || o.retopoQuads) o.seams = false, o.wireframe = false;
}

namespace {
fs::path retopoFile(const std::string& meshPath, uint32_t faces) {
  uint64_t h = 1469598103934665603ull;
  std::string key = fs::absolute(meshPath).lexically_normal().string();
  for (char c : key) h = (h ^ (uint8_t)c) * 1099511628211ull;
  char name[64];
  std::snprintf(name, sizeof name, "%016llx_%u.retopo", (unsigned long long)h, faces);
  return paths::userDataDir() / "retopo" / name;
}
}  // namespace

void Engine::saveRetopo() {
  RetopoState& S = retopoData;
  S.savedVersion = S.version;
  if (S.meshPath.empty() || S.faceLabel.size() != S.faces || S.faces == 0) return;
  bool any = std::any_of(S.faceLabel.begin(), S.faceLabel.end(), [](int32_t l) { return l != 0; }) ||
             std::any_of(S.density.begin(), S.density.end(), [](float d) { return d != 1.f; }) || !S.strokes.empty() ||
             !S.lines.empty() || !S.zoneSeeds.empty() || S.symDeadzone >= 0 || !S.ringZones || !S.fewerPoles || !S.flowLoops || !S.keepAway;
  fs::path p = retopoFile(S.meshPath, S.faces);
  std::error_code ec;
  if (!any) {
    fs::remove(p, ec);
    return;
  }
  paths::ensureDir(p.parent_path());
  json h = {{"mesh", S.meshPath}, {"faces", S.faces}, {"verts", S.verts}, {"quads_per_m2", S.quadsPerM2}, {"next_label", S.nextLabel},
            {"deadzone", S.symDeadzone}, {"rings", S.ringZones}, {"fewer_poles", S.fewerPoles}, {"flow_loops", S.flowLoops},
            {"keep_away", S.keepAway}, {"format", 2}};
  std::string hs = h.dump(), data;
  uint32_t hl = (uint32_t)hs.size();
  data.append("PTRETOPO", 8);
  data.append((const char*)&hl, 4);
  data += hs;
  data.append((const char*)S.faceLabel.data(), S.faceLabel.size() * 4);
  data.append((const char*)S.density.data(), S.density.size() * 4);
  // 0.14: the strokes (count, then per stroke: flags (1 loop, 2 edge), points, x y z...)
  auto put32 = [&](uint32_t v) { data.append((const char*)&v, 4); };
  put32((uint32_t)S.strokes.size());
  for (auto& st : S.strokes) {
    put32((st.loop ? 1u : 0u) | (st.edge ? 2u : 0u)), put32((uint32_t)st.points.size());
    data.append((const char*)st.points.data(), st.points.size() * sizeof(vec3));
  }
  // 0.14.0's zone lines along the mesh's edges (none now), the zones' resolutions (face, value), and the
  // zone lines on the surface (0.14.1: as the strokes)
  put32(0);
  put32((uint32_t)S.zoneSeeds.size());
  for (auto& [f, r] : S.zoneSeeds) put32(f), data.append((const char*)&r, 4);
  put32((uint32_t)S.lines.size());
  for (auto& st : S.lines) {
    put32((st.loop ? 1u : 0u) | (st.confirmed ? 2u : 0u)), put32((uint32_t)st.points.size());
    data.append((const char*)st.points.data(), st.points.size() * sizeof(vec3));
  }
  if (!paths::writeFileAtomic(p.string(), data)) PLOG_W("Retopology: could not keep the patches in %s", p.string().c_str());
}

bool Engine::loadRetopo() {
  RetopoState& S = retopoData;
  if (S.meshPath.empty() || S.faces == 0) return false;
  fs::path p = retopoFile(S.meshPath, S.faces);
  std::error_code ec;
  if (!fs::exists(p, ec)) return false;
  std::string data;
  {
    FILE* f = std::fopen(p.string().c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    data.resize(std::max(0L, n));
    size_t got = std::fread(data.data(), 1, data.size(), f);
    std::fclose(f);
    if (got != data.size()) return false;
  }
  if (data.size() < 12 || data.compare(0, 8, "PTRETOPO") != 0) return false;
  uint32_t hl;
  std::memcpy(&hl, data.data() + 8, 4);
  if (12 + (size_t)hl > data.size()) return false;
  json h = json::parse(data.substr(12, hl), nullptr, false);
  if (h.is_discarded() || h.value("faces", 0u) != S.faces || h.value("verts", 0u) != S.verts) return false;
  size_t off = 12 + hl, need = off + (size_t)S.faces * 4 + (size_t)S.verts * 4;
  if (data.size() < need) return false;
  std::memcpy(S.faceLabel.data(), data.data() + off, (size_t)S.faces * 4);
  std::memcpy(S.density.data(), data.data() + off + (size_t)S.faces * 4, (size_t)S.verts * 4);
  S.strokes.clear();
  size_t at = need;  // strokes (0.14 files)
  auto get32 = [&](uint32_t& v) {
    if (at + 4 > data.size()) return false;
    std::memcpy(&v, data.data() + at, 4), at += 4;
    return true;
  };
  uint32_t ns = 0;
  if (get32(ns))
    for (uint32_t k = 0; k < ns && k < 100000; ++k) {
      uint32_t loop = 0, np = 0;
      if (!get32(loop) || !get32(np) || at + (size_t)np * sizeof(vec3) > data.size()) break;
      retopo::Stroke st;
      st.loop = (loop & 1) != 0, st.edge = (loop & 2) != 0;
      st.points.resize(np);
      std::memcpy(st.points.data(), data.data() + at, (size_t)np * sizeof(vec3)), at += (size_t)np * sizeof(vec3);
      S.strokes.push_back(std::move(st));
    }
  uint32_t ne = 0;
  S.lines.clear();
  if (get32(ne) && ne > 0) {
    std::vector<uint32_t> edges;
    for (uint32_t k = 0; k < ne; ++k) {
      uint32_t e;
      if (!get32(e)) break;
      edges.push_back(e);
    }
    if (doc.bvh.empty()) doc.bvh.build(doc.mesh);
    S.lines = edgeLinesToStrokes(doc.mesh, doc.bvh, edges);  // (a 0.14.0 file)
  }
  uint32_t nz = 0;
  if (get32(nz))
    for (uint32_t k = 0; k < nz; ++k) {
      uint32_t f, rb;
      if (!get32(f) || !get32(rb)) break;
      float r;
      std::memcpy(&r, &rb, 4);
      S.zoneSeeds.push_back({f, r});
    }
  uint32_t nl = 0;
  if (get32(nl))
    for (uint32_t k = 0; k < nl && k < 100000; ++k) {
      uint32_t loop = 0, np = 0;
      if (!get32(loop) || !get32(np) || at + (size_t)np * sizeof(vec3) > data.size()) break;
      retopo::Stroke st;
      st.loop = (loop & 1) != 0, st.edge = true, st.confirmed = (loop & 2) != 0;
      st.points.resize(np);
      std::memcpy(st.points.data(), data.data() + at, (size_t)np * sizeof(vec3)), at += (size_t)np * sizeof(vec3);
      S.lines.push_back(std::move(st));
    }
  ++S.linesVersion, ++S.densityVersion;
  S.quadsPerM2 = h.value("quads_per_m2", S.quadsPerM2);
  S.symDeadzone = h.value("deadzone", -1.f);
  S.ringZones = h.value("rings", true);
  S.fewerPoles = h.value("fewer_poles", true);
  S.keepAway = h.value("keep_away", true);
  // (format 2, 0.22.4: Strokes make edge loops on by default; kept before as off only because it was the default)
  S.flowLoops = h.value("format", 1) >= 2 ? h.value("flow_loops", true) : true;
  S.nextLabel = h.value("next_label", 1);
  PLOG_I("Retopology: the patches painted on this mesh before are back (%s)", p.filename().string().c_str());
  return true;
}

void Engine::registerRetopoCommands() {
  CommandRegistry& R = commands;

  R.add({"retopo_patch",
         "Retopology: makes faces of the open (high-poly) mesh a patch, to be filled with quads. Without a label the "
         "faces become a new patch; label 0 takes them out of any patch. Patches with the same label that don't "
         "touch are separate patches. Where patches meet are the low-poly's main edge loops; where three meet is a "
         "corner.",
         [] {
           json p = facesSchemaProps();
           p["label"] = num("integer", "The patch (default: a new one); 0 removes the faces from their patches", 0, 100000);
           return schema(p);
         }(),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           if (!e.doc.hasMesh()) throw CommandError("no mesh open");
           auto& S = e.retopo();
           auto faces = targetFaces(e, a, false);
           if (faces.empty()) throw CommandError("no faces given (faces, selection or part)");
           int label = a.contains("label") ? a["label"].get<int>() : S.nextLabel;
           if (label < 0) throw CommandError("a label is 0 or more");
           for (uint32_t f : faces) S.faceLabel[f] = label;
           S.nextLabel = std::max(S.nextLabel, label + 1);
           e.retopoLinesChanged();
           return json{{"label", label}, {"faces", faces.size()}};
         }});

  R.add({"retopo_paint",
         "Retopology: paints along a stroke on the open (high-poly) mesh, like the brushes: a patch (label, or a new "
         "one), erase (patch 0), or density (value multiplies the quads per m² there). Each point paints the faces "
         "within radius of it, across the surface.",
         [] {
           json p = {{"points", prop("array", "Stroke points [[x,y,z], ...] on or near the surface (model units)")},
                     {"radius", propNum("Brush radius (model units)", 0.0001, 100)},
                     {"mode", {{"type", "string"}, {"enum", {"patch", "erase", "density"}}, {"description", "What to paint (default patch)"}}},
                     {"label", num("integer", "The patch (default: a new one)", 0, 100000)},
                     {"value", propNum("Density multiplier for mode density", 0.05, 50)}};
           return schema(p, {"points", "radius"});
         }(),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           if (!e.doc.hasMesh()) throw CommandError("no mesh open");
           auto& S = e.retopo();
           const Mesh& m = e.doc.mesh;
           if (e.doc.bvh.empty()) e.doc.bvh.build(m);
           if (!S.graph.builtFor(m)) S.graph.build(m);
           std::string mode = a.value("mode", "patch");
           int label = mode == "erase" ? 0 : a.contains("label") ? a["label"].get<int>() : S.nextLabel;
           float value = a.value("value", 2.f), radius = a.at("radius").get<float>();
           size_t painted = 0;
           for (auto& pt : a.at("points")) {
             if (!pt.is_array() || pt.size() != 3) throw CommandError("points are [x,y,z]");
             vec3 p(pt[0].get<float>(), pt[1].get<float>(), pt[2].get<float>());
             RayHit h = e.doc.bvh.closestPoint(m, p, radius * 4);
             if (!h.hit) continue;
             for (uint32_t f : retopo::facesWithin(m, S.graph, h.face, h.position, radius)) {
               if (mode == "density") {
                 for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) S.density[m.loopVert[l]] = value;
               } else {
                 S.faceLabel[f] = label;
               }
               ++painted;
             }
           }
           if (mode == "patch") S.nextLabel = std::max(S.nextLabel, label + 1);
           e.retopoLinesChanged();
           json out = {{"faces", painted}};
           if (mode == "patch") out["label"] = label;
           return out;
         }});

  R.add({"retopo_auto_patches",
         "Retopology: covers the mesh (or a part, or the selection) with patches of about the same size to start from "
         "(geodesic cells around evenly spread points). Replaces the patches there.",
         [] {
           json p = facesSchemaProps();
           p["count"] = num("integer", "How many patches: more gives smaller patches that follow detail closely; fewer gives broad ones", 1, 2000, 24);
           return schema(p);
         }(),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           if (!e.doc.hasMesh()) throw CommandError("no mesh open");
           auto& S = e.retopo();
           auto faces = targetFaces(e, a, true);
           int count = std::clamp(a.value("count", 24), 1, 2000);
           int cells = 0;
           auto lab = retopo::autoPatches(e.doc.mesh, faces, count, &cells);
           for (uint32_t f : faces) S.faceLabel[f] = lab[f] > 0 ? S.nextLabel - 1 + lab[f] : 0;
           S.nextLabel += cells;
           e.retopoLinesChanged();
           return json{{"patches", cells}, {"faces", faces.size()}};
         }});

  R.add({"retopo_fill_gaps",
         "Retopology: the unpainted areas of the mesh become patches: a small area one patch, a large one several of "
         "about the size of the painted patches (cells that aren't disks are split).",
         schema(),
         [](CommandContext& c, const json&) {
           Engine& e = c.engine;
           if (!e.doc.hasMesh()) throw CommandError("no mesh open");
           auto& S = e.retopo();
           const Mesh& m = e.doc.mesh;
           if (!S.graph.builtFor(m)) S.graph.build(m);
           // the size of a patch: the painted ones' median, or 16 quads across
           std::map<int32_t, double> area;
           for (uint32_t f = 0; f < m.faceCount(); ++f)
             if (S.faceLabel[f] > 0) area[S.faceLabel[f]] += m.faceArea3D(f);
           std::vector<double> sizes;
           for (auto& [l, a] : area) sizes.push_back(a);
           double edge = 1.0 / std::sqrt(std::max(S.quadsPerM2, 1e-3f)), target = 256 * edge * edge;
           if (!sizes.empty()) {
             std::nth_element(sizes.begin(), sizes.begin() + sizes.size() / 2, sizes.end());
             target = std::max(sizes[sizes.size() / 2], 4 * edge * edge);
           }
           std::vector<char> seen(m.faceCount(), 0);
           int made = 0;
           for (uint32_t f0 = 0; f0 < m.faceCount(); ++f0) {
             if (S.faceLabel[f0] != 0 || seen[f0]) continue;
             std::vector<uint32_t> stack = {f0}, comp;
             seen[f0] = 1;
             double a = 0;
             while (!stack.empty()) {
               uint32_t f = stack.back();
               stack.pop_back();
               comp.push_back(f);
               a += m.faceArea3D(f);
               for (uint32_t i = S.graph.start[f]; i < S.graph.start[f + 1]; ++i) {
                 uint32_t g = S.graph.items[i];
                 if (!seen[g] && S.faceLabel[g] == 0) seen[g] = 1, stack.push_back(g);
               }
             }
             int count = std::clamp((int)std::lround(a / target), 1, 2000), cells = 0;
             auto lab = retopo::autoPatches(m, comp, count, &cells);
             for (uint32_t f : comp) S.faceLabel[f] = lab[f] > 0 ? S.nextLabel - 1 + lab[f] : 0;
             S.nextLabel += cells;
             made += cells;
           }
           e.retopoLinesChanged();
           return json{{"patches", made}};
         }});

  R.add({"retopo_density",
         "Retopology: the density of quads. quads_per_m2 sets it for the whole mesh (model units are meters); value "
         "multiplies it on the given faces (2: twice the quads there, 0.5: half).",
         [] {
           json p = facesSchemaProps();
           p["quads_per_m2"] = propNum("Quads per square meter for the whole mesh", 1, 10000000);
           p["value"] = propNum("A multiplier on the given faces", 0.05, 50);
           return schema(p);
         }(),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           if (!e.doc.hasMesh()) throw CommandError("no mesh open");
           auto& S = e.retopo();
           if (a.contains("quads_per_m2")) S.quadsPerM2 = a["quads_per_m2"].get<float>();
           size_t changed = 0;
           if (a.contains("value")) {
             auto faces = targetFaces(e, a, true);
             float v = a["value"].get<float>();
             const Mesh& m = e.doc.mesh;
             for (uint32_t f : faces)
               for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) S.density[m.loopVert[l]] = v, ++changed;
           }
           ++S.densityVersion, e.retopoChanged();
           return json{{"quads_per_m2", S.quadsPerM2}, {"edge_length", 1.0 / std::sqrt(S.quadsPerM2)}, {"corners_changed", changed}};
         }});

  R.add({"retopo_build",
         "Retopology: makes the quads at the density set (retopo_density) on the surface of the open mesh: along a "
         "smooth flow over the whole model, with an edge loop along each zone line (retopo_zone_line) and painted "
         "patch border, turned by the flow strokes (retopo_stroke), each zone at its own resolution (retopo_zone) and "
         "joined to its neighbors by rows of 3-to-1 steps (method flow); or filling each painted patch on its own "
         "(method patches). Returns the quad count, the share of vertices with four edges and anything left out.",
         schema({{"relax", num("integer", "Smoothing passes of the quads' inner vertices (patches method): more evens the quads out, too many pulls them off sharp features", 0, 500, 30)},
                 {"fresh", prop("boolean", "Make all the quads again (default: only those near what changed since the last ones; "
                                           "the rest kept as they were)")},
                 {"method", {{"type", "string"}, {"enum", {"flow", "patches", "layout"}},
                             {"description", "flow: quads along the model's shape, zone lines as edge loops (default); patches: each painted patch filled on its own; "
                                             "layout: (in progress) a quad layout of the lines, its patches' counts solved at once"}}}}),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           if (!e.doc.hasMesh()) throw CommandError("no mesh open");
           auto& S = e.retopo();
           retopo::Options o;
           o.quadsPerM2 = S.quadsPerM2;
           o.relaxIterations = std::clamp(a.value("relax", 30), 0, 500);
           if (e.doc.bvh.empty()) e.doc.bvh.build(e.doc.mesh);
           e.waitRetopoBuild();
           std::string method = a.value("method", std::string(S.method == 0 ? "flow" : "patches"));
           std::unique_ptr<retopo::Result> r;
           if (method == "flow" || method == "layout") {
             retopo::FlowOptions fo;
             fo.quadsPerM2 = S.quadsPerM2;
             fo.ringZones = S.ringZones;
             fo.requantize = S.fewerPoles;
             fo.mirrorAxes = e.retopoSymmetryAxes(), fo.mirrorCenter = S.symCenter;
             fo.layout = method == "layout";
             fo.cache = S.flowCache;
             if (!a.value("fresh", false) && S.keepAway && S.result && S.result->builtFrom) fo.previous = std::make_shared<retopo::Result>(*S.result);
             r = std::make_unique<retopo::Result>(
                 retopo::flowQuads(e.doc.mesh, e.doc.bvh, e.retopoLines(), S.faceLabel, S.density, fo, e.retopoFaceResolution()));
           } else {
             r = std::make_unique<retopo::Result>(retopo::build(e.doc.mesh, e.doc.bvh, S.faceLabel, S.density, o));
           }
           json out = resultJson(*r);
           S.result = std::move(r);
           S.resultInputs = S.inputsVersion, S.resultIsPreview = false;
           ++S.version;
           S.resultVersion = S.version;
           return out;
         }});

  R.add({"retopo_use_as_model",
         "Retopology: the quads (the last result) become the model you paint. They are saved as an .obj next to the "
         "high-poly (name_retopo.obj), opened with new UVs, and the high-poly is baked onto them (normals, ambient "
         "occlusion). (1.1) The layers come along (bake_down: paint and masks resampled onto the quads' UVs; a new "
         "project); keep_layers false starts them afresh.",
         schema({{"path", prop("string", "Where to save the quads (default next to the high-poly)")},
                 {"bake", prop("boolean", "Bake the high-poly onto the quads (default true)")},
                 {"keep_layers", prop("boolean", "(1.1) Bake the layers down onto the quads (default true; off when the bake-down feature is)")},
                 {"samples", num("integer", "(1.1) The bake-down's points per texel: 1, 4, 9 or 16", 1, 16, 9)},
                 {"resolution", num("integer", "(1.1) The bake-down's texture size per texture set (default the high-poly's)", 256, 8192)},
                 {"distance", num("number", "(1.1) The bake-down's distance along the normals, object units; -1: measured", -1, 1000000, -1)}}),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           auto& S = e.retopo();
           if (!S.result || S.result->low.faceCount() == 0) throw CommandError("nothing built yet (retopo_build)");
           Document& d = e.doc;
           std::string err;
           // the high-poly: its file, or a copy of the mesh when it has none
           std::string high = d.meshPath;
           std::error_code ec;
           if (high.empty() || !fs::exists(high, ec)) {
             fs::path p = paths::ensureDir(paths::userDataDir() / "retopo") / (paths::sanitizeFileName(d.mesh.name.empty() ? "model" : d.mesh.name) + "_high.ptmesh");
             if (!savePtMesh(p.string(), d.mesh, &err)) throw CommandError(err);
             high = p.string();
           }
           std::string out = a.value("path", std::string());
           if (out.empty()) {
             fs::path hp(high);
             fs::path dir = d.projectPath.empty() ? hp.parent_path() : fs::path(d.projectPath).parent_path();
             std::string stem = hp.stem().string() + "_retopo";
             fs::path p = dir / (stem + ".obj");
             for (int k = 2; fs::exists(p, ec) && k < 1000; ++k) p = dir / (stem + "_" + std::to_string(k) + ".obj");
             out = p.string();
           }
           int turned = 0;
           Mesh low = outwardQuads(S.result->low, d.mesh, d.bvh, &turned);
           low.name = fs::path(out).stem().string();
           if (!saveObj(out, low, &err)) throw CommandError(err);
           int quads = S.result->quads;
           // (1.1) the layers onto the quads: the file as it was saved (what reopening the project reads)
           if (a.value("keep_layers", true) && e.features.on("bake_down") && !d.layers.empty()) {
             Mesh saved;
             if (!loadMesh(out, saved, &err)) throw CommandError(err);
             Document::BakeDownOptions o;
             o.samples = std::clamp(a.value("samples", 9), 1, 16);
             o.resolution = a.value("resolution", 0);
             o.distance = a.value("distance", -1.f);
             o.bakeHigh = a.value("bake", true);
             o.highPath = high;
             json rep;
             if (!e.bakeDown(std::move(saved), out, o, &rep, &err)) throw CommandError(err);
             return json{{"path", out}, {"high_poly", high}, {"quads", quads}, {"turned_outward", turned}, {"bake_down", rep}};
           }
           if (!e.openMesh(out, 0, false, &err)) throw CommandError(err);
           json r = {{"path", out}, {"high_poly", high}, {"quads", quads}};
           if (a.value("bake", true)) {
             HighPolyBakeOptions o;
             json stats;
             if (!e.doc.bakeHighPoly(high, o, &stats, &err)) r["bake_error"] = err;
             else r["bake"] = stats;
           }
           return r;
         }});

  R.add({"retopo_send_to_blender",
         "Retopology: sends the quads (the last result) to Blender as a new object next to the linked one "
         "(name_retopo), in the same place.",
         schema(),
         [](CommandContext& c, const json&) {
           Engine& e = c.engine;
           auto& S = e.retopo();
           if (!S.result || S.result->low.faceCount() == 0) throw CommandError("nothing built yet (retopo_build)");
           Document& d = e.doc;
           if (!d.bridge.active) throw CommandError("not linked to Blender: use Send to Patina in Blender first");
           std::string object = d.bridge.object.empty() ? d.mesh.name : d.bridge.object;
           fs::path dir = d.bridge.exchangeDir.empty() ? paths::bridgeDir() : fs::path(d.bridge.exchangeDir);
           dir = paths::ensureDir(dir / paths::sanitizeFileName(object));
           fs::path p = dir / (paths::sanitizeFileName(object) + "_retopo.ptmesh");
           Mesh low = outwardQuads(S.result->low, d.mesh, d.bvh);
           low.name = object + "_retopo";
           std::string err;
           if (!savePtMesh(p.string(), low, &err)) throw CommandError(err);
           json args = {{"object", object}, {"path", fs::absolute(p).string()}, {"name", object + "_retopo"}};
           int port = d.bridge.blenderPort;
           net::callAsync(port, "patina_retopo_mesh", args, [port](bool ok, const json& resp) {
             if (ok) PLOG_I("Sent the quads to Blender (port %d)", port);
             else PLOG_W("Blender did not take the quads (port %d): %s", port, resp.value("error", std::string("?")).c_str());
           });
           return json{{"sent", true}, {"object", object + "_retopo"}, {"path", p.string()}, {"quads", S.result->quads}};
         }});

  R.add({"retopo_stroke",
         "Retopology: a stroke that sets which way the flow runs where it is drawn (the quads follow it), or clear them "
         "all. With edge, an edge loop runs exactly along it.",
         [] {
           json p = {{"points", prop("array", "[[x,y,z], ...] on the surface, in order")},
                     {"loop", prop("boolean", "The last point joins the first")},
                     {"edge", prop("boolean", "An edge loop runs exactly along it (default: it only sets the flow's direction)")},
                     {"mirror", prop("string", "Mirror it across the model's middle on these axes (\"x\", \"xy\"...); within the deadzone of the middle it joins its mirror image")},
                     {"clear", prop("boolean", "Remove every stroke")}};
           return schema(p);
         }(),
         [](CommandContext& c, const json& a) {
           auto& S = c.engine.retopo();
           if (a.value("clear", false)) S.strokes.clear();
           if (a.contains("points")) {
             retopo::Stroke st;
             for (auto& p : a["points"]) st.points.push_back(vec3(p[0].get<float>(), p[1].get<float>(), p[2].get<float>()));
             st.loop = a.value("loop", false);
             st.edge = a.value("edge", false);
             if (st.points.size() >= 2)
               for (auto& m : c.engine.retopoMirror(st, mirrorAxes(a))) S.strokes.push_back(std::move(m));
           }
           c.engine.retopoChanged();
           return json{{"strokes", S.strokes.size()}};
         }});

  R.add({"retopo_flow",
         "Retopology, for testing: makes the quads as retopo_build does, with settings to try (seed, working shell, "
         "field smoothing, Patina's own field). Method trace (experimental) instead traces patches along a smooth "
         "field from its singularities, replacing the painted patches, and fills them.",
         [] {
           json p = {{"curvature", propNum("Trace method: how much the surface's bends steer the flow (default 0.3)", 0, 10)},
                     {"build", prop("boolean", "Make the quads too (default true)")},
                     {"method", {{"type", "string"}, {"enum", {"quads", "trace"}}, {"description", "quads: one field over the whole model (default); trace: patches traced along it, filled one by one"}}},
                     {"seed", num("integer", "Random seed for the quads method: another seed gives another, equally valid layout", 0, 2147483647, 0)},
                     {"field_smoothing", num("integer", "Passes of smoothing the direction field sees: more ignores small detail and gives calmer flow; -1 = about a quad", -1, 200, -1)},
                     {"shell", prop("boolean", "Work on a voxel shell of the model (default: when it is big or messy)")},
                     {"lines_obj", prop("string", "Write the traced lines and singularities to this .obj (for checking)")}};
           return schema(p);
         }(),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           if (!e.doc.hasMesh()) throw CommandError("no mesh open");
           auto& S = e.retopo();
           e.waitRetopoBuild();
           const Mesh& m = e.doc.mesh;
           if (a.value("method", std::string("quads")) == "quads") {
             retopo::FlowOptions fo;
             fo.quadsPerM2 = S.quadsPerM2;
             fo.ringZones = S.ringZones;
             fo.requantize = S.fewerPoles;
             fo.mirrorAxes = e.retopoSymmetryAxes(), fo.mirrorCenter = S.symCenter;
             fo.seed = a.value("seed", 0);
             fo.shell = a.contains("shell") ? (a["shell"].get<bool>() ? 1 : 0) : -1;
             fo.fieldSmoothing = a.value("field_smoothing", -1);
             if (e.doc.bvh.empty()) e.doc.bvh.build(m);
             auto r = std::make_unique<retopo::Result>(
                 retopo::flowQuads(m, e.doc.bvh, e.retopoLines(), S.faceLabel, S.density, fo, e.retopoFaceResolution()));
             json out = {{"result", resultJson(*r)}};
             S.result = std::move(r);
             S.resultInputs = S.inputsVersion, S.resultIsPreview = false;
             ++S.version;
             S.resultVersion = S.version;
             return out;
           }
           auto t0 = std::chrono::steady_clock::now();
           retopo::FieldOptions fo;
           fo.edgeLength = 1.f / std::sqrt(std::max(S.quadsPerM2, 1e-3f));
           fo.curvature = a.value("curvature", 0.3f);
           retopo::CrossField F = retopo::computeField(m, S.strokes, S.faceLabel, fo);
           double tField = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
           retopo::LayoutOptions lo;
           lo.edgeLength = fo.edgeLength;
           retopo::FieldLayout L = retopo::traceLayout(m, F, S.strokes, S.faceLabel, lo);
           double tLayout = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() - tField;
           S.faceLabel = L.faceLabel;
           S.nextLabel = L.patches + 1;
           S.flowLines = L.lines;
           e.retopoLinesChanged();
           int plus = 0, minus = 0;
           for (uint32_t v : F.singular) (F.index[v] > 0 ? plus : minus) += 1;
           json out = {{"singularities", L.singularities}, {"three", plus}, {"five", minus}, {"patches", L.patches},
                       {"merged", L.merged}, {"lines", L.lines.size()}, {"field_seconds", tField}, {"layout_seconds", tLayout}};
           if (a.contains("lines_obj")) {
             std::string path = a["lines_obj"];
             FILE* f = std::fopen(path.c_str(), "w");
             if (!f) throw CommandError("can't write " + path);
             size_t base = 1;
             for (auto& ln : L.lines) {
               if (ln.size() < 2) continue;
               std::fprintf(f, "o line\n");
               for (auto& p : ln) std::fprintf(f, "v %.6f %.6f %.6f\n", p.x, p.y, p.z);
               std::fprintf(f, "l");
               for (size_t i = 0; i < ln.size(); ++i) std::fprintf(f, " %zu", base + i);
               std::fprintf(f, "\n");
               base += ln.size();
             }
             for (uint32_t v : F.singular) std::fprintf(f, "# singular %d %.6f %.6f %.6f\n", F.index[v], m.positions[v].x, m.positions[v].y, m.positions[v].z);
             std::fclose(f);
           }
           if (a.value("build", true)) {
             retopo::Options o;
             o.quadsPerM2 = S.quadsPerM2;
             o.relaxIterations = 30;
             if (e.doc.bvh.empty()) e.doc.bvh.build(m);
             auto r = std::make_unique<retopo::Result>(retopo::build(m, e.doc.bvh, S.faceLabel, S.density, o));
             out["result"] = resultJson(*r);
             S.result = std::move(r);
             S.resultInputs = S.inputsVersion, S.resultIsPreview = false;
             ++S.version;
             S.resultVersion = S.version;
           }
           return out;
         }});

  R.add({"retopo_zone_line",
         "Retopology: a zone line on the surface through the given points (zones are the parts of the model the lines "
         "and the painted patches split it into; each line is an edge loop, where the flow changes). With clear, the "
         "lines near the points are taken away; clear_all removes every zone line. One undo step.",
         [] {
           json p = {{"points", prop("array", "[[x,y,z], ...] on (or near) the surface, in order")},
                     {"loop", prop("boolean", "The last point joins the first")},
                     {"mirror", prop("string", "Mirror it across the model's middle on these axes (\"x\", \"xy\"...); within the deadzone of the middle it joins its mirror image")},
                     {"clear", prop("boolean", "Take away the lines near the points instead")},
                     {"clear_all", prop("boolean", "Remove every zone line")}};
           return schema(p);
         }(),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           if (!e.doc.hasMesh()) throw CommandError("no mesh open");
           auto& S = e.retopo();
           const Mesh& m = e.doc.mesh;
           if (e.doc.bvh.empty()) e.doc.bvh.build(m);
           auto lines = S.lines;
           size_t changed = 0;
           if (a.value("clear_all", false)) changed += lines.size(), lines.clear();
           if (a.contains("points")) {
             std::vector<vec3> pts;
             for (auto& pt : a["points"]) {
               vec3 p(pt[0].get<float>(), pt[1].get<float>(), pt[2].get<float>());
               RayHit h = e.doc.bvh.closestPoint(m, p, 1e30f);
               if (h.hit) pts.push_back(h.position);
             }
             if (a.value("clear", false)) {
               vec3 lo(1e30f), hi(-1e30f);
               for (auto& p : m.positions) lo = glm::min(lo, p), hi = glm::max(hi, p);
               float tol = std::max(0.5f / std::sqrt(std::max(S.quadsPerM2, 1e-3f)), 0.01f * glm::length(hi - lo));
               size_t before = lines.size();
               lines.erase(std::remove_if(lines.begin(), lines.end(),
                                          [&](const retopo::Stroke& l) {
                                            for (auto& q : l.points)
                                              for (auto& p : pts)
                                                if (glm::length(q - p) < tol) return true;
                                            return false;
                                          }),
                           lines.end());
               changed += before - lines.size();
             } else if (pts.size() >= 2) {
               retopo::Stroke st;
               st.points = std::move(pts), st.edge = true, st.loop = a.value("loop", false);
               for (auto& m : e.retopoMirror(st, mirrorAxes(a))) lines.push_back(std::move(m)), ++changed;
             }
           }
           if (changed) e.retopoSetLines(std::move(lines), a.value("clear", false) || a.value("clear_all", false) ? "Clear zone line" : "Zone line");
           e.retopoZones();
           return json{{"lines", S.lines.size()}, {"changed", changed}, {"zones", S.zoneCount}};
         }});

  R.add({"retopo_ring_line",
         "Retopology: a zone line right round the part of the model at the point (an arm, a leg, the neck, a finger), "
         "across the way the part runs there, as Shift+click with the Lines tool makes. A line round the top of an arm or "
         "a leg keeps its quads in rings. Where there is no part to go round (a head, a chest), or with mode facing: "
         "where a plane through the point facing the camera cuts the model (round a breast, the face, a knee). One "
         "undo step.",
         schema({{"point", prop("array", "[x,y,z] on (or near) the surface")},
                 {"mirror", prop("string", "Mirror it across the model's middle on these axes (\"x\", \"xy\"...)")},
                 {"mode", {{"type", "string"}, {"enum", {"auto", "around", "facing"}},
                           {"description", "auto (the default): round the part, else facing the camera"}}},
                 {"direction", prop("array", "[x,y,z] the way the facing plane faces (default: the camera's, parallel to the screen)")}},
                {"point"}),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           if (!e.doc.hasMesh()) throw CommandError("no mesh open");
           const auto& pt = a.at("point");
           if (!pt.is_array() || pt.size() != 3) throw CommandError("point must be [x,y,z]");
           std::string why, mode = a.value("mode", std::string("auto"));
           vec3 dir;
           if (a.contains("direction")) {
             const auto& dj = a["direction"];
             if (!dj.is_array() || dj.size() != 3) throw CommandError("direction must be [x,y,z]");
             dir = vec3(dj[0].get<float>(), dj[1].get<float>(), dj[2].get<float>());
           }
           int used = 0;
           size_t added = e.retopoAddRing(vec3(pt[0].get<float>(), pt[1].get<float>(), pt[2].get<float>()), mirrorAxes(a), &why,
                                          mode == "around" ? 1 : mode == "facing" ? 2 : 0, a.contains("direction") ? &dir : nullptr, &used);
           if (!added) throw CommandError("no line round the model there: " + why);
           auto& S = e.retopo();
           e.retopoZones();
           const auto& L = S.lines.back().points;
           double len = 0;
           for (size_t i = 0; i < L.size(); ++i) len += glm::length(L[(i + 1) % L.size()] - L[i]);
           return json{{"lines", S.lines.size()}, {"added", added}, {"zones", S.zoneCount}, {"length", len}, {"made", used == 2 ? "facing" : "around"}};
         }});

  R.add({"retopo_symmetry",
         "Retopology: the symmetry used with the Sym buttons (and the mirror option of retopo_zone_line and "
         "retopo_stroke): the planes go through the model's middle; points within the deadzone of a plane count as on "
         "it, so lines ending there join their mirror images.",
         schema({{"deadzone", num("number", "Model units: vertices this close to the mirror plane are put on it; negative = 1% of the model's size (the default)", -1, 1000000, -1)}}),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           if (!e.doc.hasMesh()) throw CommandError("no mesh open");
           auto& S = e.retopo();
           if (a.contains("deadzone")) S.symDeadzone = a["deadzone"].get<float>();
           return json{{"center", {S.symCenter.x, S.symCenter.y, S.symCenter.z}},
                       {"centre", {S.symCenter.x, S.symCenter.y, S.symCenter.z}},  // the old name
                       {"deadzone", e.retopoDeadzone()},
                       {"axes", e.retopoSymmetryAxes()}};
         }});

  R.add({"retopo_thin_zones",
         "Retopology: finds the parts too thin for the quads as they are now (fingers, strands of hair: fewer than about "
         "four quads would go round them; thin sheets, as locks of hair, are left) and gives each a zone: zone lines "
         "round it where it meets the rest, and a resolution for about quads_around quads round it (a power of two, up "
         "to max_resolution). One undo step. apply: false only reports them.",
         schema({{"apply", prop("boolean", "Add the lines and resolutions (default true)")},
                 {"quads_around", num("integer", "Quads round each thin part (a finger, a strap): more keeps them round, fewer is lighter", 3, 64, 6)},
                 {"max_resolution", propNum("Parts that need more than this (strands of hair) are left out (default 16)", 2, 64)}}),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           if (!e.doc.hasMesh()) throw CommandError("no mesh open");
           e.waitRetopoBuild();
           return e.retopoThinZones(a.value("apply", true), a.value("quads_around", 6), a.value("max_resolution", 16.f));
         }});

  R.add({"retopo_rings",
         "Retopology: whether a zone between two edge lines (round an eye or a mouth, along an arm) is filled with "
         "rings of quads that follow both lines (on by default); small zones inside such rings are closed with "
         "rings shrinking in. Off: those zones are made as the others are.",
         schema({{"on", prop("boolean", "Rings between two lines")}}),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           if (!e.doc.hasMesh()) throw CommandError("no mesh open");
           auto& S = e.retopo();
           if (a.contains("on") && a["on"].get<bool>() != S.ringZones) S.ringZones = a["on"].get<bool>(), e.retopoChanged();
           return json{{"on", S.ringZones}};
         }});

  R.add({"retopo_snap",
         "Retopology: whether a new zone line's ends snap onto the lines near them (on by default; confirmed lines are "
         "never snapped to). Off: the ends stay where they are drawn (tight folds: lips, fingers, the crotch). Only the "
         "Lines tool's drawing; retopo_zone_line takes the points as given.",
         schema({{"on", prop("boolean", "Snap line ends")}}),
         [](CommandContext& c, const json& a) {
           if (a.contains("on")) c.engine.retopoSnapEnds = a["on"].get<bool>();
           return json{{"on", c.engine.retopoSnapEnds}};
         }});

  R.add({"retopo_confirm_lines",
         "Retopology: confirms zone lines (or unconfirms them): confirmed lines are drawn darker and new lines don't snap "
         "to them; they still make zones and edge loops. One undo step. Without line or point: every line.",
         schema({{"confirmed", prop("boolean", "Confirm (true, the default) or unconfirm (false)")},
                 {"line", num("integer", "Only this line (its index, as retopo_info line_points lists them)", 0, 100000)},
                 {"point", {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 3}, {"maxItems", 3},
                            {"description", "Only the line nearest this point of the model"}}}}),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           if (!e.doc.hasMesh()) throw CommandError("no mesh open");
           auto& S = e.retopo();
           const bool to = a.value("confirmed", true);
           std::vector<char> flags(S.lines.size());
           for (size_t k = 0; k < S.lines.size(); ++k) flags[k] = S.lines[k].confirmed;
           int only = -1;
           if (a.contains("line")) {
             only = a["line"].get<int>();
             if (only < 0 || only >= (int)S.lines.size()) throw CommandError("no line " + std::to_string(only));
           } else if (a.contains("point")) {
             vec3 p(a["point"][0].get<float>(), a["point"][1].get<float>(), a["point"][2].get<float>());
             float bd = 1e30f;
             for (size_t k = 0; k < S.lines.size(); ++k)
               for (auto& q : S.lines[k].points)
                 if (float d = glm::length(q - p); d < bd) bd = d, only = (int)k;
             if (only < 0) throw CommandError("no lines");
           }
           for (size_t k = 0; k < flags.size(); ++k)
             if (only < 0 || (int)k == only) flags[k] = to;
           e.retopoSetConfirmed(flags, to ? "Confirm zone lines" : "Unconfirm zone lines");
           size_t n = 0;
           for (auto& l : S.lines) n += l.confirmed;
           return json{{"lines", S.lines.size()}, {"confirmed", n}};
         }});

  R.add({"retopo_flow_loops",
         "Retopology: whether each flow stroke is an edge loop as well, running exactly along it (on by default). Unlike "
         "a zone line, a stroke doesn't make a zone of its own. Off: the strokes only turn the quads their way, and the "
         "quads keep their size, so a circle of flow still has poles.",
         schema({{"on", prop("boolean", "Strokes make edge loops")}}),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           if (!e.doc.hasMesh()) throw CommandError("no mesh open");
           auto& S = e.retopo();
           if (a.contains("on") && a["on"].get<bool>() != S.flowLoops) S.flowLoops = a["on"].get<bool>(), e.retopoChanged();
           return json{{"on", S.flowLoops}};
         }});

  R.add({"retopo_keep_away",
         "Retopology: whether a change (a line, a stroke, painted density, a zone's resolution) makes only the quads "
         "near it again, the rest kept exactly as they were and joined to the new ones by a row of faces (on by "
         "default); off: every build makes all the quads again. retopo_build with fresh makes them all again once.",
         schema({{"on", prop("boolean", "Keep the quads away from changes")}}),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           if (!e.doc.hasMesh()) throw CommandError("no mesh open");
           auto& S = e.retopo();
           if (a.contains("on") && a["on"].get<bool>() != S.keepAway) S.keepAway = a["on"].get<bool>(), ++S.version, S.changedAt = nowSeconds();
           return json{{"on", S.keepAway}};
         }});

  R.add({"retopo_fewer_poles",
         "Retopology: whether the quads' counts are solved again after they are made, so that a three- and a "
         "five-edged pole close together cancel out (on by default). The new quads are kept only when they have "
         "fewer poles and no more folded faces. Off: the quads as first made.",
         schema({{"on", prop("boolean", "Fewer poles")}}),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           if (!e.doc.hasMesh()) throw CommandError("no mesh open");
           auto& S = e.retopo();
           if (a.contains("on") && a["on"].get<bool>() != S.fewerPoles) S.fewerPoles = a["on"].get<bool>(), e.retopoChanged();
           return json{{"on", S.fewerPoles}};
         }});

  R.add({"retopo_zones",
         "Retopology: the zones (parts of the model between the zone lines and painted patches' borders) with their "
         "size and resolution.",
         schema(),
         [](CommandContext& c, const json&) {
           Engine& e = c.engine;
           if (!e.doc.hasMesh()) throw CommandError("no mesh open");
           const auto& z = e.retopoZones();
           auto& S = e.retopo();
           const Mesh& m = e.doc.mesh;
           std::vector<double> area(S.zoneCount + 1, 0.0);
           std::vector<vec3> center(S.zoneCount + 1, vec3(0));
           std::vector<size_t> faces(S.zoneCount + 1, 0);
           for (uint32_t f = 0; f < m.faceCount(); ++f) {
             double a = m.faceArea3D(f);
             area[z[f]] += a, center[z[f]] += m.faceCenter(f) * float(a), faces[z[f]]++;
           }
           json out = json::array();
           for (int k = 1; k <= S.zoneCount; ++k) {
             vec3 cc = area[k] > 0 ? center[k] / float(area[k]) : vec3(0);
             out.push_back({{"zone", k}, {"faces", faces[k]}, {"area", area[k]}, {"center", {cc.x, cc.y, cc.z}}, {"centre", {cc.x, cc.y, cc.z}},  // centre: the old name
                            {"resolution", e.retopoZoneResolution(k)}});
           }
           return json{{"zones", out}};
         }});

  R.add({"retopo_zone",
         "Retopology: a zone's resolution, a multiplier on the quads per m² there (2: twice as many quads). The zone is "
         "given by its number (retopo_zones), a face, or a point on it.",
         [] {
           json p = {{"zone", num("integer", "The zone (from retopo_zones)", 0, 100000)},
                     {"face", num("integer", "A face in the zone", 0, 100000000)},
                     {"point", prop("array", "[x,y,z] on the zone")},
                     {"resolution", propNum("Multiplier on the quads per m²", 0.05, 50)}};
           return schema(p);
         }(),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           if (!e.doc.hasMesh()) throw CommandError("no mesh open");
           auto& S = e.retopo();
           const Mesh& m = e.doc.mesh;
           const auto& z = e.retopoZones();
           int64_t face = -1;
           if (a.contains("face")) face = a["face"].get<int64_t>();
           else if (a.contains("point")) {
             if (e.doc.bvh.empty()) e.doc.bvh.build(m);
             auto& p = a["point"];
             RayHit h = e.doc.bvh.closestPoint(m, vec3(p[0].get<float>(), p[1].get<float>(), p[2].get<float>()), 1e30f);
             if (h.hit) face = h.face;
           } else if (a.contains("zone")) {
             int want = a["zone"];
             for (uint32_t f = 0; f < z.size(); ++f)
               if (z[f] == want) {
                 face = f;
                 break;
               }
           }
           if (face < 0 || face >= (int64_t)m.faceCount()) throw CommandError("give a zone, a face or a point on the model");
           int zone = z[face];
           if (a.contains("resolution")) {
             float r = a["resolution"];
             // one setting per zone: earlier ones inside it go
             S.zoneSeeds.erase(std::remove_if(S.zoneSeeds.begin(), S.zoneSeeds.end(),
                                              [&](auto& sd) { return sd.first < z.size() && z[sd.first] == zone; }),
                               S.zoneSeeds.end());
             if (std::abs(r - 1.f) > 1e-4f) S.zoneSeeds.push_back({(uint32_t)face, r});
             e.retopoChanged();
           }
           return json{{"zone", zone}, {"resolution", e.retopoZoneResolution(zone)}};
         }});

  R.add({"retopo_view",
         "Retopology: what the viewport shows: the zones (in color, with their lines) or the painted density over the "
         "model (or neither), and the quads (as lines over the model, or alone). In the app's 3D view they show while "
         "preview is on (the top bar's Retopology preview button) or a retopology tool is chosen; render_view shows "
         "them either way.",
         [] {
           json p = {{"preview", prop("boolean", "The app's Retopology preview: show these in its 3D view whatever tool is chosen, "
                                                 "with a quick set of quads following each change")},
                     {"show", {{"type", "string"}, {"enum", {"zones", "density", "none", "patches"}},
                               {"description", "Over the model (patches: the same as zones)"}}},
                     {"quads", prop("boolean", "Show the quads of the last build")},
                     {"quads_only", prop("boolean", "Show the quads alone, shaded, instead of the model")},
                     {"marks", prop("boolean", "Show the red crosses over the quads: parts too thin for them (left out), "
                                               "tight spots patched where zones meet, and (with mirror) where the two sides "
                                               "of the model differ. The build's problems say which. Default true")},
                     {"loops", prop("boolean", "Show the quads' poles, and light up the edge loop under the pointer")},
                     {"lines_behind", {{"type", "string"}, {"enum", {"shown", "darkened", "hidden"}},
                                       {"description", "Zone lines and flow strokes behind the model: shown through it, darkened, or hidden"}}},
                     {"loop_at", {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 3}, {"maxItems", 3},
                                  {"description", "Light up the edge loop nearest this point of the model (with loops on)"}}}};
           return schema(p);
         }(),
         [](CommandContext& c, const json& a) {
           ViewportOptions& v = c.engine.viewOptions;
           if (a.contains("preview")) v.retopoPreview = a["preview"].get<bool>();
           if (a.contains("show")) {
             std::string s = a["show"];
             v.retopoView = s == "zones" || s == "patches" ? 1 : s == "density" ? 2 : 0;
           }
           if (a.contains("quads")) v.retopoShowQuads = a["quads"].get<bool>();
           if (a.contains("quads_only")) v.retopoQuadsOnly = a["quads_only"].get<bool>();
           if (a.contains("marks")) v.retopoShowMarks = a["marks"].get<bool>();
           if (a.contains("loops")) v.retopoLoops = a["loops"].get<bool>();
           static const char* behindNames[] = {"shown", "darkened", "hidden"};
           if (a.contains("lines_behind")) {
             std::string b = a["lines_behind"];
             v.retopoLinesBehind = b == "shown" ? 0 : b == "hidden" ? 2 : 1;
           }
           static const char* names[] = {"none", "zones", "density"};
           json out = {{"preview", v.retopoPreview}, {"show", names[std::clamp(v.retopoView, 0, 2)]}, {"quads", v.retopoShowQuads}, {"quads_only", v.retopoQuadsOnly},
                       {"marks", v.retopoShowMarks}, {"loops", v.retopoLoops}, {"lines_behind", behindNames[std::clamp(v.retopoLinesBehind, 0, 2)]}};
           auto& S = c.engine.retopo();
           if (v.retopoLoops && S.result && S.result->low.faceCount()) {
             ViewportOptions o = v;
             c.engine.retopoViewOptions(o);  // (finds the loops)
             if (S.loops) {
               int p3 = 0, p5 = 0, pm = 0;
               for (int n : S.loops->poleEdges) (n == 3 ? p3 : n == 5 ? p5 : pm)++;
               out["poles"] = {{"three", p3}, {"five", p5}, {"other", pm}};
               out["edge_loops"] = S.loops->loops.size();
             }
             if (a.contains("loop_at")) {
               vec3 p(a["loop_at"][0].get<float>(), a["loop_at"][1].get<float>(), a["loop_at"][2].get<float>());
               int edges = c.engine.retopoHoverLoop(&p);
               out["loop_edges"] = edges;
               if (S.hoverLoop >= 0) out["loop_closed"] = (bool)S.loops->closed[S.hoverLoop];
             }
           }
           return out;
         }});

  R.add({"retopo_info", "Retopology: the patches, the density, the lines and strokes, and the last result. quads_asked: about how many "
         "quads the settings ask for (zones and density counted); over 150,000 the live preview doesn't start "
         "(preview_too_big) and only retopo_build makes them.",
         schema({{"line_points", prop("boolean", "Also every zone line's points")}}),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           auto& S = e.retopo();
           std::map<int, size_t> n;
           for (int32_t l : S.faceLabel)
             if (l > 0) n[l]++;
           json labels = json::array();
           for (auto& [l, k] : n) labels.push_back({{"label", l}, {"faces", k}});
           size_t dense = 0;
           for (float d : S.density) dense += d != 1.f;
           json r = {{"quads_per_m2", S.quadsPerM2}, {"edge_length", 1.0 / std::sqrt(S.quadsPerM2)}, {"labels", labels},
                     {"density_painted_vertices", dense}, {"building", e.retopoBuilding()},
                     {"quads_asked", std::lround(e.retopoQuadsAsked())}, {"preview_too_big", S.previewTooBig}, {"strokes", S.strokes.size()},
                     {"lines", S.lines.size()}, {"keep_away", S.keepAway}};
           if (a.value("line_points", false)) {
             json L = json::array();
             for (auto& l : S.lines) {
               json pts = json::array();
               for (auto& p : l.points) pts.push_back({p.x, p.y, p.z});
               L.push_back({{"loop", l.loop}, {"confirmed", l.confirmed}, {"points", pts}});
             }
             r["line_points"] = L;
           }
           if (S.result) {
             r["result"] = resultJson(*S.result);
             r["result_is_preview"] = S.resultIsPreview;
             r["result_is_current"] = S.resultInputs == S.inputsVersion;
           }
           return r;
         }});

  R.add({"retopo_clear", "Retopology: removes the patches, the painted density and the result.",
         schema({{"keep_density", prop("boolean", "Keep the painted density")}}),
         [](CommandContext& c, const json& a) {
           auto& S = c.engine.retopo();
           std::fill(S.faceLabel.begin(), S.faceLabel.end(), 0);
           if (!a.value("keep_density", false)) std::fill(S.density.begin(), S.density.end(), 1.f);
           S.result.reset();
           S.nextLabel = 1;
           ++S.densityVersion, c.engine.retopoLinesChanged();
           return json{{"cleared", true}};
         }});

  R.add({"retopo_export", "Retopology: writes the quads (the last result) to an .obj file.",
         schema({{"path", prop("string", "The .obj file")}, {"groups", prop("boolean", "A group (g) for each patch")}}, {"path"}),
         [](CommandContext& c, const json& a) {
           auto& S = c.engine.retopo();
           if (!S.result || S.result->low.faceCount() == 0) throw CommandError("nothing built yet (retopo_build)");
           std::string path = a.at("path"), err;
           Mesh out = outwardQuads(S.result->low, c.engine.doc.mesh, c.engine.doc.bvh);
           if (a.value("groups", false)) {
             const Mesh& m = out;
             FILE* f = std::fopen(path.c_str(), "w");
             if (!f) throw CommandError("can't write " + path);
             std::fprintf(f, "# Patina retopology\no Retopo\n");
             for (auto& p : m.positions) std::fprintf(f, "v %.6f %.6f %.6f\n", p.x, p.y, p.z);
             int last = -1;
             for (uint32_t fi = 0; fi < m.faceCount(); ++fi) {
               int lab = S.result->quadPatch[fi];
               if (lab != last) std::fprintf(f, "g patch_%d\n", lab), last = lab;
               std::fprintf(f, "f");
               for (uint32_t l = m.faceStart[fi]; l < m.faceStart[fi + 1]; ++l) std::fprintf(f, " %u", m.loopVert[l] + 1);
               std::fprintf(f, "\n");
             }
             std::fclose(f);
           } else if (!saveObj(path, out, &err)) {
             throw CommandError(err);
           }
           return json{{"path", path}, {"quads", S.result->quads}};
         }});
}

}  // namespace patina
