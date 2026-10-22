#include "core/MeshCheck.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <unordered_map>

#include "core/UVIsland.h"

namespace patina {

namespace {

struct Dsu {
  std::vector<uint32_t> p;
  explicit Dsu(size_t n) : p(n) { std::iota(p.begin(), p.end(), 0u); }
  uint32_t find(uint32_t x) {
    while (p[x] != x) x = p[x] = p[p[x]];
    return x;
  }
  void unite(uint32_t a, uint32_t b) {
    a = find(a), b = find(b);
    if (a != b) p[b] = a;
  }
};

uint64_t pairKey(uint32_t a, uint32_t b) { return a < b ? (uint64_t(a) << 32) | b : (uint64_t(b) << 32) | a; }

float sizeOf(const Mesh& m) {
  if (!m.bounds.valid()) return 1.f;
  return std::max(glm::length(m.bounds.hi - m.bounds.lo), 1e-9f);
}

// Vertices within `tol` of another (a different index): groups of coincident vertices, by a spatial hash.
std::vector<uint32_t> coincident(const Mesh& m, float tol, size_t* pairs) {
  const uint32_t V = (uint32_t)m.positions.size();
  std::vector<uint32_t> to(V);
  std::iota(to.begin(), to.end(), 0u);
  if (pairs) *pairs = 0;
  if (V == 0 || tol <= 0.f) return to;
  auto cellOf = [&](const vec3& p) {
    return glm::ivec3((int)std::floor(p.x / tol), (int)std::floor(p.y / tol), (int)std::floor(p.z / tol));
  };
  auto hashCell = [](glm::ivec3 c) {
    return (uint64_t(uint32_t(c.x)) * 73856093ull) ^ (uint64_t(uint32_t(c.y)) * 19349663ull) ^ (uint64_t(uint32_t(c.z)) * 83492791ull);
  };
  std::unordered_multimap<uint64_t, uint32_t> grid;
  grid.reserve(V * 2);
  for (uint32_t v = 0; v < V; ++v) grid.emplace(hashCell(cellOf(m.positions[v])), v);
  Dsu d(V);
  float t2 = tol * tol;
  for (uint32_t v = 0; v < V; ++v) {
    glm::ivec3 c = cellOf(m.positions[v]);
    for (int dz = -1; dz <= 1; ++dz)
      for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
          auto range = grid.equal_range(hashCell(c + glm::ivec3(dx, dy, dz)));
          for (auto it = range.first; it != range.second; ++it) {
            uint32_t w = it->second;
            if (w <= v) continue;
            vec3 dd = m.positions[w] - m.positions[v];
            if (glm::dot(dd, dd) <= t2 && d.find(v) != d.find(w)) {
              d.unite(v, w);
              if (pairs) ++*pairs;
            }
          }
        }
  }
  // each group's lowest index stands for it
  std::vector<uint32_t> low(V, UINT32_MAX);
  for (uint32_t v = 0; v < V; ++v) {
    uint32_t r = d.find(v);
    low[r] = std::min(low[r], v);
  }
  for (uint32_t v = 0; v < V; ++v) to[v] = low[d.find(v)];
  return to;
}

struct Topology {
  std::vector<uint32_t> edgeUse;  // faces per edge
  std::vector<int32_t> edgeDir;   // sum of +1 (a loop runs edges[e].x -> .y) / -1
  std::vector<uint32_t> edgeFace0, edgeFace1;
  std::vector<uint32_t> loopFace;
};
Topology topologyOf(const Mesh& m) {
  Topology t;
  const size_t E = m.edges.size();
  t.edgeUse.assign(E, 0);
  t.edgeDir.assign(E, 0);
  t.edgeFace0.assign(E, UINT32_MAX);
  t.edgeFace1.assign(E, UINT32_MAX);
  t.loopFace = m.loopFace();
  for (uint32_t f = 0; f < m.faceCount(); ++f)
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) {
      uint32_t e = m.loopEdge[l];
      if (e >= E) continue;
      t.edgeUse[e]++;
      t.edgeDir[e] += m.loopVert[l] == m.edges[e].x ? 1 : -1;
      if (t.edgeFace0[e] == UINT32_MAX) t.edgeFace0[e] = f;
      else if (t.edgeFace1[e] == UINT32_MAX) t.edgeFace1[e] = f;
    }
  return t;
}

// Orientation per face within each part (connected through two-face edges): +1 or -1 so that neighbors run their
// shared edge in opposite directions; the bigger group of each part counts as +1. Returns the faces that disagree.
std::vector<int8_t> orientations(const Mesh& m, const Topology& t, std::vector<uint32_t>* comp) {
  const uint32_t F = m.faceCount();
  std::vector<int8_t> o(F, 0);
  std::vector<uint32_t> c(F, UINT32_MAX);
  // faces around each face through two-face edges
  std::vector<uint32_t> stack;
  uint32_t nComp = 0;
  for (uint32_t s = 0; s < F; ++s) {
    if (o[s]) continue;
    std::vector<uint32_t> members;
    o[s] = 1;
    c[s] = nComp;
    stack.push_back(s);
    while (!stack.empty()) {
      uint32_t f = stack.back();
      stack.pop_back();
      members.push_back(f);
      for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) {
        uint32_t e = m.loopEdge[l];
        if (e >= t.edgeUse.size() || t.edgeUse[e] != 2) continue;
        uint32_t g = t.edgeFace0[e] == f ? t.edgeFace1[e] : t.edgeFace0[e];
        if (g == UINT32_MAX || g == f || o[g]) continue;
        // the same direction along the edge: g is turned the other way from f
        bool same = std::abs(t.edgeDir[e]) == 2;
        o[g] = int8_t(same ? -o[f] : o[f]);
        c[g] = nComp;
        stack.push_back(g);
      }
    }
    int pos = 0;
    for (uint32_t f : members) pos += o[f] > 0 ? 1 : -1;
    if (pos < 0)
      for (uint32_t f : members) o[f] = int8_t(-o[f]);
    ++nComp;
  }
  if (comp) *comp = std::move(c);
  return o;
}

double signedVolume(const Mesh& m, const std::vector<uint32_t>& faces, const std::vector<int8_t>* flip) {
  double v = 0;
  for (uint32_t f : faces) {
    const vec3& a = m.positions[m.loopVert[m.faceStart[f]]];
    double s = 0;
    for (uint32_t l = m.faceStart[f] + 1; l + 1 < m.faceStart[f + 1]; ++l) {
      const vec3& b = m.positions[m.loopVert[l]];
      const vec3& c = m.positions[m.loopVert[l + 1]];
      s += glm::dot(glm::dvec3(a), glm::cross(glm::dvec3(b), glm::dvec3(c)));
    }
    v += (flip && (*flip)[f] < 0 ? -s : s) / 6.0;
  }
  return v;
}

}  // namespace

MeshCheckReport checkMesh(const Mesh& m, const MeshCheckOptions& opt) {
  MeshCheckReport r;
  MeshStats& st = r.stats;
  const uint32_t F = m.faceCount(), V = (uint32_t)m.positions.size();
  st.vertices = V, st.faces = F, st.edges = (uint32_t)m.edges.size();
  st.trianglesRendered = m.tris.size();
  st.uvIslands = m.islandCount;
  st.size = m.bounds.valid() ? m.bounds.hi - m.bounds.lo : vec3(0);
  r.faceSeverity.assign(F, 0);
  if (F == 0) return r;
  const float size = sizeOf(m);
  std::vector<uint8_t> usedSlot;
  double uvArea = 0;
  for (uint32_t f = 0; f < F; ++f) {
    uint32_t n = m.faceSize(f);
    if (n == 3) st.triangles++;
    else if (n == 4) st.quads++;
    else st.ngons++;
    st.area += m.faceArea3D(f);
    uvArea += std::abs(m.faceAreaUV(f));
    uint32_t s = f < m.faceMaterial.size() ? m.faceMaterial[f] : 0;
    if (s >= usedSlot.size()) usedSlot.resize(s + 1, 0);
    usedSlot[s] = 1;
  }
  for (uint8_t u : usedSlot) st.slots += u;
  st.uvCoverage = (float)std::min(uvArea, 1.0);
  Topology t = topologyOf(m);
  auto addProblem = [&](MeshProblem p, const std::vector<uint32_t>& faces) {
    if (p.count == 0) return;
    p.faces = faces;
    std::sort(p.faces.begin(), p.faces.end());
    p.faces.erase(std::unique(p.faces.begin(), p.faces.end()), p.faces.end());
    if (p.where.empty())
      for (size_t i = 0; i < p.faces.size() && i < 8; i += std::max<size_t>(1, p.faces.size() / 8))
        p.where.push_back(m.faceCenter(p.faces[i]));
    for (uint32_t f : p.faces) r.faceSeverity[f] = std::max<uint8_t>(r.faceSeverity[f], uint8_t(p.severity + 1));
    r.problems.push_back(std::move(p));
  };

  // ---- parts (connected through shared vertices)
  Dsu parts(V);
  std::vector<uint8_t> used(V, 0);
  for (uint32_t f = 0; f < F; ++f)
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) {
      used[m.loopVert[l]] = 1;
      parts.unite(m.loopVert[m.faceStart[f]], m.loopVert[l]);
    }
  std::unordered_map<uint32_t, uint32_t> partIndex;
  std::vector<uint32_t> facePart(F);
  for (uint32_t f = 0; f < F; ++f) {
    uint32_t root = parts.find(m.loopVert[m.faceStart[f]]);
    auto it = partIndex.emplace(root, (uint32_t)partIndex.size()).first;
    facePart[f] = it->second;
  }
  st.parts = (uint32_t)partIndex.size();

  // ---- open seams: vertices at the same place, not joined
  {
    float tol = opt.weldDistance > 0.f ? opt.weldDistance : size * 1e-5f;
    size_t pairs = 0;
    std::vector<uint32_t> to = coincident(m, tol, &pairs);
    MeshProblem p;
    p.id = "open_seams", p.name = "Open seams", p.unit = "vertices", p.fix = "weld", p.severity = 1;
    p.what = "Vertices at the same place that aren't joined: the surface has a cut there (shading seams, cracks "
             "in bakes, a part that falls apart).";
    std::vector<uint32_t> faces;
    std::vector<uint8_t> dup(V, 0);
    for (uint32_t v = 0; v < V; ++v)
      if (to[v] != v) dup[v] = 1, dup[to[v]] = 1;
    for (uint32_t v = 0; v < V; ++v) p.count += dup[v] && used[v];
    for (uint32_t l = 0; l < m.loopCount(); ++l)
      if (dup[m.loopVert[l]]) faces.push_back(t.loopFace[l]);
    addProblem(p, faces);
  }
  // ---- holes: the borders (edges with one face) in connected loops
  std::vector<uint32_t> borderEdges;
  bool allTwo = true;
  for (uint32_t e = 0; e < t.edgeUse.size(); ++e) {
    if (t.edgeUse[e] == 1) borderEdges.push_back(e);
    if (t.edgeUse[e] != 2) allTwo = false;
  }
  st.closed = allTwo;
  {
    Dsu hb(V);
    for (uint32_t e : borderEdges) hb.unite(m.edges[e].x, m.edges[e].y);
    std::unordered_map<uint32_t, std::vector<uint32_t>> loops;  // root -> border edges
    for (uint32_t e : borderEdges) loops[hb.find(m.edges[e].x)].push_back(e);
    MeshProblem p;
    p.id = "holes", p.name = "Holes", p.unit = "holes", p.fix = "fill_holes", p.severity = 1;
    p.what = "Open borders: the model isn't closed there. Fine for clothes and planes; a problem for 3D printing, "
             "thickness bakes and anything that should be solid. Join open seams first, or a hole is closed twice.";
    std::vector<uint32_t> faces;
    for (auto& [root, es] : loops) {
      p.count++;
      vec3 c(0);
      for (uint32_t e : es) {
        c += m.positions[m.edges[e].x];
        faces.push_back(t.edgeFace0[e]);
      }
      if (p.where.size() < 8) p.where.push_back(c / float(es.size()));
    }
    addProblem(p, faces);
  }
  // ---- non-manifold edges: three faces or more on one edge
  {
    MeshProblem p;
    p.id = "non_manifold_edges", p.name = "Non-manifold edges", p.unit = "edges", p.severity = 2;
    p.what = "Edges shared by three faces or more: engines, printers and bakers can't tell inside from outside there.";
    std::vector<uint32_t> faces;
    for (uint32_t f = 0; f < F; ++f)
      for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l)
        if (m.loopEdge[l] < t.edgeUse.size() && t.edgeUse[m.loopEdge[l]] > 2) faces.push_back(f);
    for (uint32_t e = 0; e < t.edgeUse.size(); ++e) p.count += t.edgeUse[e] > 2;
    addProblem(p, faces);
  }
  // ---- non-manifold vertices: fans of faces that meet at one vertex only (bowties)
  {
    const uint32_t L = m.loopCount();
    const uint32_t E = (uint32_t)t.edgeUse.size();
    Dsu corners(L);
    // the loops running along each edge (flat arrays: big scans have millions)
    std::vector<uint32_t> start(E + 1, 0), list(L);
    for (uint32_t l = 0; l < L; ++l) start[m.loopEdge[l] + 1]++;
    for (uint32_t e = 0; e < E; ++e) start[e + 1] += start[e];
    {
      std::vector<uint32_t> fill(start.begin(), start.end() - 1);
      for (uint32_t l = 0; l < L; ++l) list[fill[m.loopEdge[l]]++] = l;
    }
    // corners at the same vertex in faces that share an edge through that vertex are one fan
    for (uint32_t e = 0; e < E; ++e) {
      if (start[e + 1] - start[e] < 2) continue;
      for (uint32_t vtx : {m.edges[e].x, m.edges[e].y}) {
        auto cornerAt = [&](uint32_t l) { return m.loopVert[l] == vtx ? l : m.nextLoop(l, t.loopFace[l]); };
        uint32_t c0 = cornerAt(list[start[e]]);
        for (uint32_t i = start[e] + 1; i < start[e + 1]; ++i) corners.unite(c0, cornerAt(list[i]));
      }
    }
    std::vector<uint32_t> firstRoot(V, UINT32_MAX);
    std::vector<uint8_t> bad(V, 0);
    for (uint32_t l = 0; l < L; ++l) {
      uint32_t v = m.loopVert[l], root = corners.find(l);
      if (firstRoot[v] == UINT32_MAX) firstRoot[v] = root;
      else if (firstRoot[v] != root) bad[v] = 1;
    }
    MeshProblem p;
    p.id = "non_manifold_vertices", p.name = "Bowtie vertices", p.unit = "vertices", p.severity = 1;
    p.what = "Separate fans of faces that touch at a single vertex: smoothing, unwrapping and printing go wrong there.";
    std::vector<uint32_t> faces;
    for (uint32_t v = 0; v < V; ++v) p.count += bad[v];
    for (uint32_t l = 0; l < L; ++l)
      if (bad[m.loopVert[l]]) faces.push_back(t.loopFace[l]);
    addProblem(p, faces);
  }
  // ---- faces turned the wrong way, and closed parts inside out
  std::vector<uint32_t> comp;
  std::vector<int8_t> orient = orientations(m, t, &comp);
  {
    MeshProblem p;
    p.id = "flipped_faces", p.name = "Flipped faces", p.unit = "faces", p.fix = "fix_winding", p.severity = 2;
    p.what = "Faces turned the other way from their neighbors: they look dark or see-through in engines, and bakes "
             "and smoothing break along them.";
    std::vector<uint32_t> faces;
    for (uint32_t f = 0; f < F; ++f)
      if (orient[f] < 0) faces.push_back(f);
    p.count = faces.size();
    addProblem(p, faces);
  }
  {
    // closed parts (every edge two faces) whose volume comes out negative face inwards
    std::unordered_map<uint32_t, std::vector<uint32_t>> byComp;
    std::unordered_map<uint32_t, bool> compClosed;
    for (uint32_t f = 0; f < F; ++f) {
      byComp[comp[f]].push_back(f);
      bool cl = true;
      for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1] && cl; ++l)
        cl = m.loopEdge[l] < t.edgeUse.size() && t.edgeUse[m.loopEdge[l]] == 2;
      auto it = compClosed.emplace(comp[f], true).first;
      it->second = it->second && cl;
    }
    MeshProblem p;
    p.id = "inside_out", p.name = "Inside-out parts", p.unit = "parts", p.fix = "fix_winding", p.severity = 2;
    p.what = "Closed parts whose faces all point inwards: they render inside out and print as holes.";
    std::vector<uint32_t> faces;
    double vol = 0;
    for (auto& [c, fs] : byComp) {
      double v = signedVolume(m, fs, &orient);
      if (compClosed[c] && v < 0) {
        p.count++;
        faces.insert(faces.end(), fs.begin(), fs.end());
      }
      vol += std::abs(v);
    }
    st.volume = vol;
    addProblem(p, faces);
  }
  // ---- degenerate faces: no area, or a corner twice
  {
    double minArea = double(size) * size * 1e-12;
    MeshProblem p;
    p.id = "degenerate_faces", p.name = "Degenerate faces", p.unit = "faces", p.fix = "remove_degenerate", p.severity = 1;
    p.what = "Faces with no area (or the same corner twice): they get no texture, upset normals and tangents, and "
             "some engines refuse them.";
    std::vector<uint32_t> faces;
    for (uint32_t f = 0; f < F; ++f) {
      bool rep = false;
      for (uint32_t a = m.faceStart[f]; a < m.faceStart[f + 1] && !rep; ++a)
        for (uint32_t b = a + 1; b < m.faceStart[f + 1]; ++b) rep |= m.loopVert[a] == m.loopVert[b];
      if (rep || m.faceArea3D(f) <= minArea) faces.push_back(f);
    }
    p.count = faces.size();
    addProblem(p, faces);
  }
  // ---- duplicate faces: the same corners twice
  {
    std::unordered_map<std::string, uint32_t> seen;
    MeshProblem p;
    p.id = "duplicate_faces", p.name = "Duplicate faces", p.unit = "faces", p.fix = "remove_duplicate_faces", p.severity = 1;
    p.what = "Two faces on the same corners: they flicker (z-fighting) and double the paint.";
    std::vector<uint32_t> faces;
    std::vector<uint32_t> vs;
    for (uint32_t f = 0; f < F; ++f) {
      vs.assign(m.loopVert.begin() + m.faceStart[f], m.loopVert.begin() + m.faceStart[f + 1]);
      std::sort(vs.begin(), vs.end());
      std::string key((const char*)vs.data(), vs.size() * sizeof(uint32_t));
      auto it = seen.emplace(std::move(key), f);
      if (!it.second) faces.push_back(f), faces.push_back(it.first->second);
    }
    p.count = faces.size() / 2;
    addProblem(p, faces);
  }
  // ---- loose vertices: no face uses them
  {
    MeshProblem p;
    p.id = "loose_vertices", p.name = "Loose vertices", p.unit = "vertices", p.fix = "remove_loose", p.severity = 0;
    p.what = "Vertices no face uses: harmless here, but they bloat files and confuse some tools.";
    for (uint32_t v = 0; v < V; ++v) p.count += !used[v];
    for (uint32_t v = 0, n = 0; v < V && n < 8; ++v)
      if (!used[v]) p.where.push_back(m.positions[v]), ++n;
    addProblem(p, {});
  }
  // ---- small parts: specks and floaters
  if (st.parts > 1) {
    std::vector<AABB> box(st.parts);
    std::vector<uint32_t> nf(st.parts, 0);
    for (uint32_t f = 0; f < F; ++f) {
      nf[facePart[f]]++;
      for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) box[facePart[f]].add(m.positions[m.loopVert[l]]);
    }
    uint32_t biggest = (uint32_t)(std::max_element(nf.begin(), nf.end()) - nf.begin());
    MeshProblem p;
    p.id = "small_parts", p.name = "Specks and floaters", p.unit = "parts", p.fix = "remove_small_parts", p.severity = 1;
    p.what = "Tiny separate pieces (under 3% of the model's size): leftovers of scans and generated models, stray "
             "faces from modeling.";
    std::vector<uint8_t> small(st.parts, 0);
    for (uint32_t k = 0; k < st.parts; ++k)
      if (k != biggest && glm::length(box[k].hi - box[k].lo) < size * 0.03f && nf[k] * 100 < F) small[k] = 1, p.count++;
    std::vector<uint32_t> faces;
    for (uint32_t f = 0; f < F; ++f)
      if (small[facePart[f]]) faces.push_back(f);
    addProblem(p, faces);
  }
  // ---- UVs: faces with no UV area (they get no texture), islands on top of each other
  {
    bool anyUV = false;
    for (const vec2& uv : m.loopUV)
      if (uv.x != 0.f || uv.y != 0.f) {
        anyUV = true;
        break;
      }
    MeshProblem p;
    p.id = "no_uv_area", p.name = "Faces without UV space", p.unit = "faces", p.severity = 1;
    p.what = anyUV ? "Faces whose UVs have no area: they get no texels, so paint can't show on them. Unwrap them."
                   : "The model has no UVs. Patina makes some when it opens a model; for your own layout, unwrap.";
    std::vector<uint32_t> faces;
    for (uint32_t f = 0; f < F; ++f)
      if (m.faceHasNoUVArea(f, m.loopUV)) faces.push_back(f);
    p.count = faces.size();
    addProblem(p, faces);
    if (opt.uvOverlaps && anyUV && !m.tiles.active() && F < 400000) {
      auto groups = uv::overlappingIslands(m, 512);
      MeshProblem q;
      q.id = "uv_overlaps", q.name = "Overlapping UVs", q.unit = "islands", q.severity = 1;
      q.what = "UV islands on top of each other: paint on one shows on the other (fine for mirrored halves on "
               "purpose). The UV editor's Fix Overlaps moves them apart.";
      std::vector<uint32_t> faces2;
      for (auto& g : groups) {
        q.count++;
        faces2.insert(faces2.end(), g.begin(), g.end());
      }
      addProblem(q, faces2);
    }
  }
  // ---- scale: models far from meters (Blender's unit) are usually in the wrong units
  {
    float longest = std::max({st.size.x, st.size.y, st.size.z});
    if (longest > 0.f && (longest < 0.005f || longest > 500.f)) {
      MeshProblem p;
      p.id = "scale", p.name = longest < 0.005f ? "Very small" : "Very large", p.unit = "", p.count = 1, p.severity = 0;
      char buf[160];
      std::snprintf(buf, sizeof buf,
                    "The model is %.4g units long. In Blender's meters that is %s: often a model exported in the wrong "
                    "units (centimeters, millimeters).",
                    longest, longest < 0.005f ? "smaller than 5 mm" : "longer than 500 m");
      p.what = buf;
      p.where.push_back(m.bounds.center());
      addProblem(p, {});
    }
  }
  std::stable_sort(r.problems.begin(), r.problems.end(),
                   [](const MeshProblem& a, const MeshProblem& b) { return a.severity > b.severity; });
  return r;
}

// ------------------------------------------------------------------ fixes

Mesh fixMesh(const Mesh& src, const MeshFixOptions& o, MeshFixResult* res) {
  MeshFixResult rr;
  MeshFixResult& r = res ? *res : rr;
  r = MeshFixResult();
  const float size = sizeOf(src);
  const uint32_t F0 = src.faceCount(), V0 = (uint32_t)src.positions.size();
  // vertex remap (weld) and the faces to keep
  std::vector<uint32_t> vmap(V0);
  std::iota(vmap.begin(), vmap.end(), 0u);
  if (o.weld) {
    size_t pairs = 0;
    vmap = coincident(src, o.weldDistance > 0.f ? o.weldDistance : size * 1e-5f, &pairs);
    for (uint32_t v = 0; v < V0; ++v) r.welded += vmap[v] != v;
  }
  std::vector<uint8_t> keepFace(F0, 1);
  std::vector<uint32_t> fv;
  auto faceVerts = [&](uint32_t f) -> const std::vector<uint32_t>& {
    fv.clear();
    for (uint32_t l = src.faceStart[f]; l < src.faceStart[f + 1]; ++l) fv.push_back(vmap[src.loopVert[l]]);
    return fv;
  };
  if (o.removeDegenerate) {
    double minArea = double(size) * size * 1e-12;
    for (uint32_t f = 0; f < F0; ++f) {
      const auto& vs = faceVerts(f);
      bool rep = false;
      for (size_t a = 0; a < vs.size() && !rep; ++a)
        for (size_t b = a + 1; b < vs.size(); ++b) rep |= vs[a] == vs[b];
      if (rep || src.faceArea3D(f) <= minArea) keepFace[f] = 0, r.degenerate++;
    }
  } else if (o.weld) {
    // a weld must not leave faces with a corner twice: those faces keep their own vertices
    for (uint32_t f = 0; f < F0; ++f) {
      const auto& vs = faceVerts(f);
      bool rep = false;
      for (size_t a = 0; a < vs.size() && !rep; ++a)
        for (size_t b = a + 1; b < vs.size(); ++b) rep |= vs[a] == vs[b];
      if (rep)
        for (uint32_t l = src.faceStart[f]; l < src.faceStart[f + 1]; ++l) vmap[src.loopVert[l]] = src.loopVert[l];
    }
  }
  if (o.removeDuplicateFaces) {
    std::unordered_map<std::string, uint32_t> seen;
    std::vector<uint32_t> vs;
    for (uint32_t f = 0; f < F0; ++f) {
      if (!keepFace[f]) continue;
      vs = faceVerts(f);
      std::sort(vs.begin(), vs.end());
      if (!seen.emplace(std::string((const char*)vs.data(), vs.size() * sizeof(uint32_t)), f).second)
        keepFace[f] = 0, r.duplicates++;
    }
  }
  if (o.removeSmallParts) {
    Dsu d(V0);
    for (uint32_t f = 0; f < F0; ++f)
      if (keepFace[f])
        for (uint32_t l = src.faceStart[f]; l < src.faceStart[f + 1]; ++l)
          d.unite(vmap[src.loopVert[src.faceStart[f]]], vmap[src.loopVert[l]]);
    std::unordered_map<uint32_t, std::pair<AABB, uint32_t>> parts;
    for (uint32_t f = 0; f < F0; ++f) {
      if (!keepFace[f]) continue;
      auto& pr = parts[d.find(vmap[src.loopVert[src.faceStart[f]]])];
      pr.second++;
      for (uint32_t l = src.faceStart[f]; l < src.faceStart[f + 1]; ++l) pr.first.add(src.positions[src.loopVert[l]]);
    }
    if (parts.size() > 1) {
      uint32_t biggest = 0, most = 0, total = 0;
      for (auto& [k, pr] : parts) {
        total += pr.second;
        if (pr.second > most) most = pr.second, biggest = k;
      }
      float lim = o.smallPartSize > 0.f ? o.smallPartSize : size * 0.03f;
      std::unordered_map<uint32_t, uint8_t> drop;
      for (auto& [k, pr] : parts)
        if (k != biggest && glm::length(pr.first.hi - pr.first.lo) < lim && pr.second * 100 < total) drop[k] = 1, r.smallParts++;
      for (uint32_t f = 0; f < F0; ++f)
        if (keepFace[f] && drop.count(d.find(vmap[src.loopVert[src.faceStart[f]]]))) keepFace[f] = 0, r.smallPartFaces++;
    }
  }
  // build the new mesh: kept faces, their loops (UVs, normals), vertices in use
  Mesh m;
  m.name = src.name;
  m.materialNames = src.materialNames;
  m.autoSmoothAngleDeg = src.autoSmoothAngleDeg;
  m.hasSourceNormals = false;  // normals are made again (faces may have turned)
  std::vector<uint32_t> newIndex(V0, UINT32_MAX);
  bool removeLoose = o.removeLoose || o.weld || o.removeSmallParts || o.removeDegenerate || o.removeDuplicateFaces;
  if (!removeLoose) {
    m.positions = src.positions;
    for (uint32_t v = 0; v < V0; ++v) newIndex[v] = v;
  }
  auto vertexOf = [&](uint32_t v) {
    v = vmap[v];
    if (newIndex[v] == UINT32_MAX) newIndex[v] = (uint32_t)m.positions.size(), m.positions.push_back(src.positions[v]);
    return newIndex[v];
  };
  m.faceStart.push_back(0);
  std::vector<uint32_t> oldFace;
  for (uint32_t f = 0; f < F0; ++f) {
    if (!keepFace[f]) continue;
    for (uint32_t l = src.faceStart[f]; l < src.faceStart[f + 1]; ++l) {
      m.loopVert.push_back(vertexOf(src.loopVert[l]));
      m.loopUV.push_back(l < src.loopUV.size() ? src.loopUV[l] : vec2(0));
    }
    m.faceStart.push_back((uint32_t)m.loopVert.size());
    m.faceMaterial.push_back(f < src.faceMaterial.size() ? src.faceMaterial[f] : 0);
    m.faceFlags.push_back(f < src.faceFlags.size() ? src.faceFlags[f] : 0);
    oldFace.push_back(f);
  }
  if (o.removeLoose || removeLoose) {
    uint32_t usedOld = 0;
    std::vector<uint8_t> refd(V0, 0);
    for (uint32_t f = 0; f < F0; ++f)
      for (uint32_t l = src.faceStart[f]; l < src.faceStart[f + 1]; ++l) refd[src.loopVert[l]] = 1;
    for (uint32_t v = 0; v < V0; ++v) usedOld += refd[v];
    if (o.removeLoose) r.loose = V0 - usedOld;
  }
  // vertex groups follow their vertices
  for (const VertexGroup& g : src.vertexGroups) {
    VertexGroup ng;
    ng.name = g.name;
    std::vector<uint8_t> in(m.positions.size(), 0);
    for (uint32_t v : g.verts)
      if (v < V0) {
        uint32_t nv = newIndex[vmap[v]];
        if (nv != UINT32_MAX && !in[nv]) in[nv] = 1, ng.verts.push_back(nv);
      }
    if (!ng.verts.empty()) m.vertexGroups.push_back(std::move(ng));
  }
  // seams and sharp edges stay on the same pair of vertices (set again after every rebuild of the edges)
  std::unordered_map<uint64_t, uint8_t> flags;
  for (uint32_t e = 0; e < src.edges.size() && e < src.edgeFlags.size(); ++e)
    if (src.edgeFlags[e]) {
      uint32_t a = newIndex[vmap[src.edges[e].x]], b = newIndex[vmap[src.edges[e].y]];
      if (a != UINT32_MAX && b != UINT32_MAX && a != b) flags[pairKey(a, b)] |= src.edgeFlags[e];
    }
  auto refinalize = [&]() {
    m.edges.clear();
    m.edgeFlags.clear();
    m.finalize();
    if (flags.empty()) return;
    for (uint32_t e = 0; e < m.edges.size(); ++e) {
      auto it = flags.find(pairKey(m.edges[e].x, m.edges[e].y));
      if (it != flags.end()) m.edgeFlags[e] = it->second;
    }
  };
  // texture tiles: faces keep their tile
  if (src.tiles.active()) {
    m.tiles.mode = src.tiles.mode, m.tiles.grid = src.tiles.grid, m.tiles.tiles = src.tiles.tiles;
  }
  refinalize();
  // turn faces like their neighbors, then closed parts outward
  if (o.fixWinding) {
    Topology t = topologyOf(m);
    std::vector<uint32_t> comp;
    std::vector<int8_t> orient = orientations(m, t, &comp);
    std::unordered_map<uint32_t, std::vector<uint32_t>> byComp;
    std::unordered_map<uint32_t, bool> closed;
    for (uint32_t f = 0; f < m.faceCount(); ++f) {
      byComp[comp[f]].push_back(f);
      bool cl = true;
      for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1] && cl; ++l) cl = t.edgeUse[m.loopEdge[l]] == 2;
      auto it = closed.emplace(comp[f], true).first;
      it->second = it->second && cl;
    }
    std::vector<uint8_t> flip(m.faceCount(), 0);
    for (auto& [c, fs] : byComp) {
      bool outward = !closed[c] || signedVolume(m, fs, &orient) >= 0;
      for (uint32_t f : fs) flip[f] = (orient[f] < 0) != !outward;
    }
    bool any = false;
    for (uint32_t f = 0; f < m.faceCount(); ++f)
      if (flip[f]) {
        any = true, r.flipped++;
        // reverse the corners: keep the first, reverse the rest (UVs travel with their corners)
        std::reverse(m.loopVert.begin() + m.faceStart[f] + 1, m.loopVert.begin() + m.faceStart[f + 1]);
        std::reverse(m.loopUV.begin() + m.faceStart[f] + 1, m.loopUV.begin() + m.faceStart[f + 1]);
      }
    if (any) refinalize();
  }
  // close holes up to a size with a face over each (its corners take the border's UVs)
  if (o.fillHoles) {
    int maxEdges = o.fillHoleEdges > 0 ? o.fillHoleEdges : 64;
    Topology t = topologyOf(m);
    // border edges in the direction their face runs them; the cap runs them the other way
    std::vector<uint32_t> borderLoops;
    for (uint32_t f = 0; f < m.faceCount(); ++f)
      for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l)
        if (t.edgeUse[m.loopEdge[l]] == 1) borderLoops.push_back(l);
    // the cap walks b -> a for each border loop a -> b
    std::unordered_map<uint32_t, uint32_t> step;   // vertex -> border loop starting the cap's next corner there
    std::unordered_map<uint32_t, int> outDeg;
    for (uint32_t l : borderLoops) {
      uint32_t f = t.loopFace[l];
      uint32_t b = m.loopVert[m.nextLoop(l, f)];
      outDeg[b]++;
      step[b] = l;
    }
    std::vector<uint8_t> usedLoop(m.loopCount(), 0);
    size_t addedFaces = 0;
    for (uint32_t l0 : borderLoops) {
      if (usedLoop[l0]) continue;
      std::vector<uint32_t> ring;  // border loops in cap order
      uint32_t l = l0;
      bool ok = true;
      while (true) {
        if (usedLoop[l]) { ok = l == l0 && ring.size() >= 3; break; }
        usedLoop[l] = 1;
        ring.push_back(l);
        uint32_t a = m.loopVert[l];
        if (outDeg[a] != 1) { ok = false; break; }  // a vertex where borders cross: leave it
        auto it = step.find(a);
        if (it == step.end()) { ok = false; break; }
        l = it->second;
        if ((int)ring.size() > maxEdges) { ok = false; break; }
      }
      if (!ok || ring.size() < 3) continue;
      // cap corners: for each border loop a->b (cap order), the corner b with b's UV in that face
      for (uint32_t bl : ring) {
        uint32_t f = t.loopFace[bl];
        uint32_t nl = m.nextLoop(bl, f);
        m.loopVert.push_back(m.loopVert[nl]);
        m.loopUV.push_back(m.loopUV[nl]);
      }
      m.faceStart.push_back((uint32_t)m.loopVert.size());
      uint32_t f0 = t.loopFace[ring[0]];
      m.faceMaterial.push_back(m.faceMaterial[f0]);
      m.faceFlags.push_back(0);
      ++addedFaces;
    }
    if (addedFaces) {
      r.holesFilled = addedFaces;
      if (m.tiles.active()) m.tiles.faceTile.clear();
      refinalize();
    }
  }
  return m;
}

}  // namespace patina
