#include "core/Mesh.h"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <thread>

#include <mikktspace.h>

#include <numeric>
#include <unordered_map>

#include "core/Log.h"

namespace patina {

namespace {

struct UnionFind {
  std::vector<uint32_t> p;
  explicit UnionFind(size_t n) : p(n) { std::iota(p.begin(), p.end(), 0u); }
  uint32_t find(uint32_t x) {
    while (p[x] != x) {
      p[x] = p[p[x]];
      x = p[x];
    }
    return x;
  }
  void unite(uint32_t a, uint32_t b) {
    a = find(a);
    b = find(b);
    if (a != b) p[std::max(a, b)] = std::min(a, b);
  }
};

uint64_t edgeKey(uint32_t a, uint32_t b) {
  if (a > b) std::swap(a, b);
  return (uint64_t(a) << 32) | b;
}

bool uvEq(const vec2& a, const vec2& b) {
  return std::abs(a.x - b.x) < 1e-5f && std::abs(a.y - b.y) < 1e-5f;
}

}  // namespace

vec3 Mesh::faceNormal(uint32_t f) const {
  // Newell's method (robust for ngons)
  vec3 n(0);
  for (uint32_t l = faceStart[f]; l < faceStart[f + 1]; ++l) {
    const vec3& a = positions[loopVert[l]];
    const vec3& b = positions[loopVert[nextLoop(l, f)]];
    n.x += (a.y - b.y) * (a.z + b.z);
    n.y += (a.z - b.z) * (a.x + b.x);
    n.z += (a.x - b.x) * (a.y + b.y);
  }
  float len = glm::length(n);
  return len > 1e-20f ? n / len : vec3(0, 1, 0);
}

vec3 Mesh::faceCenter(uint32_t f) const {
  vec3 c(0);
  for (uint32_t l = faceStart[f]; l < faceStart[f + 1]; ++l) c += positions[loopVert[l]];
  return c / float(faceSize(f));
}

float Mesh::faceArea3D(uint32_t f) const {
  vec3 n(0);
  const vec3& o = positions[loopVert[faceStart[f]]];
  for (uint32_t l = faceStart[f] + 1; l + 1 < faceStart[f + 1]; ++l)
    n += glm::cross(positions[loopVert[l]] - o, positions[loopVert[l + 1]] - o);
  return glm::length(n) * 0.5f;
}

float Mesh::faceAreaUV(uint32_t f) const {
  float a = 0;
  for (uint32_t l = faceStart[f]; l < faceStart[f + 1]; ++l)
    a += cross2(loopUV[l], loopUV[nextLoop(l, f)]);
  return a * 0.5f;
}

std::vector<uint32_t> Mesh::loopFace() const {
  std::vector<uint32_t> lf(loopCount());
  for (uint32_t f = 0; f < faceCount(); ++f)
    for (uint32_t l = faceStart[f]; l < faceStart[f + 1]; ++l) lf[l] = f;
  return lf;
}

void Mesh::finalize() {
  if (loopUV.size() != loopVert.size()) loopUV.assign(loopVert.size(), vec2(0));
  if (faceMaterial.size() != faceCount()) faceMaterial.assign(faceCount(), 0);
  if (faceFlags.size() != faceCount()) faceFlags.assign(faceCount(), 0);
  if (!hasSourceNormals || loopNormal.size() != loopVert.size()) hasSourceNormals = false;
  buildEdges();
  triangulate();
  computeBounds();
  if (!hasSourceNormals) computeNormals();
  // (without UVs there are no tangents to find; a model without them gets UVs, and tangents, on
  // opening: on a two-million-face sculpt this saves a second or two)
  bool anyUV = false;
  for (const vec2& uv : loopUV)
    if (uv.x != 0.f || uv.y != 0.f) {
      anyUV = true;
      break;
    }
  if (anyUV) computeTangents();
  else loopTangent.assign(loopVert.size(), vec4(1, 0, 0, 1));
  computeUVTopology();
}

void Mesh::buildEdges() {
  std::unordered_map<uint64_t, uint32_t> map;
  map.reserve(edges.size() * 2 + loopVert.size());
  bool keep = !edges.empty();
  if (edgeFlags.size() != edges.size()) edgeFlags.assign(edges.size(), 0);
  for (uint32_t e = 0; e < edges.size(); ++e) map[edgeKey(edges[e].x, edges[e].y)] = e;
  loopEdge.assign(loopVert.size(), 0);
  size_t added = 0;
  for (uint32_t f = 0; f < faceCount(); ++f) {
    for (uint32_t l = faceStart[f]; l < faceStart[f + 1]; ++l) {
      uint32_t a = loopVert[l], b = loopVert[nextLoop(l, f)];
      auto key = edgeKey(a, b);
      auto it = map.find(key);
      if (it == map.end()) {
        uint32_t id = (uint32_t)edges.size();
        edges.push_back({std::min(a, b), std::max(a, b)});
        edgeFlags.push_back(0);
        map.emplace(key, id);
        loopEdge[l] = id;
        ++added;
      } else {
        loopEdge[l] = it->second;
      }
    }
  }
  if (keep && added) PLOG_W("Mesh '%s': %zu face edges were missing from the edge list", name.c_str(), added);
}

// Ear clipping of one polygon projected onto its best-fit plane.
static void triangulatePolygon(const Mesh& m, uint32_t f, std::vector<glm::uvec3>& out) {
  uint32_t n = m.faceSize(f);
  uint32_t s = m.faceStart[f];
  if (n < 3) return;
  if (n == 3) {
    out.push_back({s, s + 1, s + 2});
    return;
  }
  if (n == 4) {
    // split along the shorter diagonal, unless that produces a fold (concave quad)
    vec3 p[4];
    for (int i = 0; i < 4; ++i) p[i] = m.positions[m.loopVert[s + i]];
    vec3 nrm = m.faceNormal(f);
    auto convexAt = [&](int i) {
      vec3 a = p[(i + 3) % 4], b = p[i], c = p[(i + 1) % 4];
      return glm::dot(glm::cross(b - a, c - b), nrm) > 0;
    };
    bool d02 = glm::length(p[0] - p[2]) <= glm::length(p[1] - p[3]);
    if (d02 && (!convexAt(0) || !convexAt(2))) d02 = false;
    else if (!d02 && (!convexAt(1) || !convexAt(3))) d02 = true;
    if (d02) {
      out.push_back({s, s + 1, s + 2});
      out.push_back({s, s + 2, s + 3});
    } else {
      out.push_back({s, s + 1, s + 3});
      out.push_back({s + 1, s + 2, s + 3});
    }
    return;
  }
  vec3 nrm = m.faceNormal(f);
  vec3 ax = std::abs(nrm.x) > 0.9f ? vec3(0, 1, 0) : vec3(1, 0, 0);
  vec3 u = glm::normalize(glm::cross(ax, nrm));
  vec3 v = glm::cross(nrm, u);
  std::vector<vec2> pts(n);
  std::vector<uint32_t> idx(n);
  for (uint32_t i = 0; i < n; ++i) {
    vec3 p = m.positions[m.loopVert[s + i]];
    pts[i] = {glm::dot(p, u), glm::dot(p, v)};
    idx[i] = i;
  }
  auto inTri = [](vec2 p, vec2 a, vec2 b, vec2 c) {
    float d1 = cross2(b - a, p - a), d2 = cross2(c - b, p - b), d3 = cross2(a - c, p - c);
    return d1 >= 0 && d2 >= 0 && d3 >= 0;
  };
  int guard = 0;
  while (idx.size() > 3 && guard++ < 10000) {
    bool clipped = false;
    size_t k = idx.size();
    for (size_t i = 0; i < k; ++i) {
      uint32_t ia = idx[(i + k - 1) % k], ib = idx[i], ic = idx[(i + 1) % k];
      vec2 a = pts[ia], b = pts[ib], c = pts[ic];
      if (cross2(b - a, c - b) <= 1e-12f) continue;  // reflex
      bool empty = true;
      for (size_t j = 0; j < k && empty; ++j) {
        uint32_t q = idx[j];
        if (q == ia || q == ib || q == ic) continue;
        if (inTri(pts[q], a, b, c)) empty = false;
      }
      if (!empty) continue;
      out.push_back({s + ia, s + ib, s + ic});
      idx.erase(idx.begin() + i);
      clipped = true;
      break;
    }
    if (!clipped) {  // degenerate: fall back to a fan for the rest
      for (size_t i = 1; i + 1 < idx.size(); ++i) out.push_back({s + idx[0], s + idx[i], s + idx[i + 1]});
      return;
    }
  }
  if (idx.size() == 3) out.push_back({s + idx[0], s + idx[1], s + idx[2]});
}

void Mesh::triangulate() {
  tris.clear();
  triFace.clear();
  tris.reserve(loopVert.size());
  for (uint32_t f = 0; f < faceCount(); ++f) {
    size_t before = tris.size();
    triangulatePolygon(*this, f, tris);
    triFace.insert(triFace.end(), tris.size() - before, f);
  }
}

void Mesh::computeBounds() {
  bounds = AABB();
  for (auto& p : positions) bounds.add(p);
  boundingRadius = 0.f;
  vec3 c = bounds.center();
  for (auto& p : positions) boundingRadius = std::max(boundingRadius, glm::length(p - c));
}

void Mesh::computeNormals() {
  uint32_t F = faceCount();
  std::vector<vec3> fn(F);
  for (uint32_t f = 0; f < F; ++f) fn[f] = faceNormal(f);
  auto lf = loopFace();
  // vertex -> loops
  std::vector<uint32_t> vStart(positions.size() + 1, 0), vLoops(loopVert.size());
  for (uint32_t v : loopVert) vStart[v + 1]++;
  for (size_t i = 1; i < vStart.size(); ++i) vStart[i] += vStart[i - 1];
  std::vector<uint32_t> fill(vStart.begin(), vStart.end() - 1);
  for (uint32_t l = 0; l < loopVert.size(); ++l) vLoops[fill[loopVert[l]]++] = l;
  // corner angle weights
  std::vector<float> cornerAngle(loopVert.size());
  for (uint32_t f = 0; f < F; ++f) {
    uint32_t s = faceStart[f], n = faceSize(f);
    for (uint32_t i = 0; i < n; ++i) {
      vec3 p = positions[loopVert[s + i]];
      vec3 a = positions[loopVert[s + (i + n - 1) % n]] - p;
      vec3 b = positions[loopVert[s + (i + 1) % n]] - p;
      float la = glm::length(a), lb = glm::length(b);
      cornerAngle[s + i] = (la > 0 && lb > 0) ? std::acos(std::clamp(glm::dot(a, b) / (la * lb), -1.f, 1.f)) : 0.f;
    }
  }
  float cosT = std::cos(glm::radians(autoSmoothAngleDeg));
  loopNormal.assign(loopVert.size(), vec3(0));
  for (uint32_t l = 0; l < loopVert.size(); ++l) {
    uint32_t v = loopVert[l];
    vec3 nf = fn[lf[l]];
    vec3 acc(0);
    for (uint32_t k = vStart[v]; k < vStart[v + 1]; ++k) {
      uint32_t m = vLoops[k];
      vec3 ng = fn[lf[m]];
      if (m == l || glm::dot(nf, ng) >= cosT) acc += ng * cornerAngle[m];
    }
    float len = glm::length(acc);
    loopNormal[l] = len > 1e-20f ? acc / len : nf;
  }
}

namespace {
struct MikkCtx {
  Mesh* m;
  const std::vector<uint32_t>* tris = nullptr;  // some of the triangles (all: none)
  uint32_t tri(int f) const { return tris ? (*tris)[f] : (uint32_t)f; }
};
int mkNumFaces(const SMikkTSpaceContext* c) {
  auto* x = (MikkCtx*)c->m_pUserData;
  return (int)(x->tris ? x->tris->size() : x->m->tris.size());
}
int mkNumVerts(const SMikkTSpaceContext*, const int) { return 3; }
void mkPos(const SMikkTSpaceContext* c, float out[], const int f, const int v) {
  auto* x = (MikkCtx*)c->m_pUserData;
  vec3 p = x->m->positions[x->m->loopVert[x->m->tris[x->tri(f)][v]]];
  out[0] = p.x, out[1] = p.y, out[2] = p.z;
}
void mkNrm(const SMikkTSpaceContext* c, float out[], const int f, const int v) {
  auto* x = (MikkCtx*)c->m_pUserData;
  vec3 n = x->m->loopNormal[x->m->tris[x->tri(f)][v]];
  out[0] = n.x, out[1] = n.y, out[2] = n.z;
}
void mkUV(const SMikkTSpaceContext* c, float out[], const int f, const int v) {
  auto* x = (MikkCtx*)c->m_pUserData;
  vec2 t = x->m->loopUV[x->m->tris[x->tri(f)][v]];
  out[0] = t.x, out[1] = t.y;
}
void mkSet(const SMikkTSpaceContext* c, const float t[], const float sign, const int f, const int v) {
  auto* x = (MikkCtx*)c->m_pUserData;
  x->m->loopTangent[x->m->tris[x->tri(f)][v]] = vec4(t[0], t[1], t[2], sign);
}
}  // namespace

void Mesh::computeTangents() {
  loopTangent.assign(loopVert.size(), vec4(1, 0, 0, 1));
  if (tris.empty()) return;
  SMikkTSpaceInterface iface{};
  iface.m_getNumFaces = mkNumFaces;
  iface.m_getNumVerticesOfFace = mkNumVerts;
  iface.m_getPosition = mkPos;
  iface.m_getNormal = mkNrm;
  iface.m_getTexCoord = mkUV;
  iface.m_setTSpaceBasic = mkSet;
  // a big mesh with many UV islands: groups of islands on threads of their own (MikkTSpace only joins
  // corners with the same UV, so islands never share a tangent: the same result, several times quicker)
  const unsigned threads = std::max(1u, std::thread::hardware_concurrency());
  if (tris.size() > 200000 && threads > 1 && islandCount > 1 && loopIsland.size() == loopVert.size() && triFace.size() == tris.size() &&
      !getenv("PATINA_SERIAL_TANGENTS")) {
    std::vector<size_t> islandTris(islandCount + 1, 0);
    for (auto& t : tris) islandTris[std::min(loopIsland[t[0]], islandCount)]++;
    const size_t groups = std::min<size_t>(threads, 32);
    std::vector<std::vector<uint32_t>> part(groups);
    std::vector<size_t> load(groups, 0), groupOf(islandCount + 1, 0);
    // (the biggest islands first, each to the lightest group)
    std::vector<uint32_t> order(islandCount + 1);
    std::iota(order.begin(), order.end(), 0u);
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return islandTris[a] > islandTris[b]; });
    for (uint32_t i : order) {
      size_t g = size_t(std::min_element(load.begin(), load.end()) - load.begin());
      groupOf[i] = g, load[g] += islandTris[i];
    }
    for (uint32_t t = 0; t < tris.size(); ++t) part[groupOf[std::min(loopIsland[tris[t][0]], islandCount)]].push_back(t);
    std::vector<char> ok(groups, 1);
    std::vector<std::thread> th;
    for (size_t g = 0; g < groups; ++g)
      if (!part[g].empty())
        th.emplace_back([&, g] {
          MikkCtx ctx{this, &part[g]};
          SMikkTSpaceContext c{&iface, &ctx};
          ok[g] = genTangSpaceDefault(&c);
        });
    for (auto& t : th) t.join();
    if (std::find(ok.begin(), ok.end(), 0) != ok.end()) PLOG_W("MikkTSpace tangent generation failed");
    return;
  }
  MikkCtx ctx{this};
  SMikkTSpaceContext c{&iface, &ctx};
  if (!genTangSpaceDefault(&c)) PLOG_W("MikkTSpace tangent generation failed");
}

// ------------------------------------------------------------------ texture tiles

std::string udimName(int udim) { return std::to_string(udim); }

int udimOf(vec2 uv) {
  int u = std::clamp((int)std::floor(uv.x), 0, 9), v = std::clamp((int)std::floor(uv.y), 0, 99);
  return 1001 + u + 10 * v;
}

glm::ivec2 udimCorner(int udim) {
  int k = std::max(0, udim - 1001);
  return {k % 10, k / 10};
}

glm::ivec2 TileLayout::uvOrigin(int t) const {
  if (!(mode & TileByUdim) || t < 0 || t >= (int)tiles.size()) return {0, 0};
  return udimCorner(tiles[t].udim);
}

vec2 TileLayout::toAtlas(vec2 uv, int t) const { return cellOrigin(t) + (uv - vec2(uvOrigin(t))) * cellSize(); }

int TileLayout::tileAtAtlas(vec2 a) const {
  int cx = std::clamp((int)std::floor(a.x * grid), 0, grid - 1), cy = std::clamp((int)std::floor(a.y * grid), 0, grid - 1);
  return cy * grid + cx;
}

vec2 TileLayout::fromAtlas(vec2 a, int* tile) const {
  int t = tileAtAtlas(a);
  if (tile) *tile = t;
  return (a - cellOrigin(t)) * float(grid) + vec2(uvOrigin(t));
}

std::string Mesh::slotName(uint32_t slot) const {
  if (slot < materialNames.size() && !materialNames[slot].empty()) return materialNames[slot];
  return "Slot " + std::to_string(slot + 1);
}

static vec2 faceCentroidUV(const Mesh& m, const std::vector<vec2>& uvs, uint32_t f) {
  vec2 c(0);
  for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) c += uvs[l];
  return c / float(std::max(1u, m.faceSize(f)));
}

bool Mesh::faceHasNoUVArea(uint32_t f, const std::vector<vec2>& uvs) const {
  if (faceSize(f) < 3 || uvs.size() != loopCount()) return false;
  float a = 0;
  vec2 lo(1e30f), hi(-1e30f);
  // (0.47) from the first corner: on the corners' own coordinates the products are ~0.2 and a small face's area
  // (1e-8 in a dense part of the layout) was lost in their rounding, so tiny faces with good UVs counted as having none
  const vec2 o = uvs[faceStart[f]];
  for (uint32_t l = faceStart[f]; l < faceStart[f + 1]; ++l) {
    uint32_t n = nextLoop(l, f);
    vec2 p = uvs[l] - o, q = uvs[n] - o;
    a += p.x * q.y - q.x * p.y;
    lo = glm::min(lo, uvs[l]), hi = glm::max(hi, uvs[l]);
  }
  a = std::abs(a) * 0.5f;
  float len = std::max(hi.x - lo.x, hi.y - lo.y);
  return a < 1e-12f || a < 1e-6f * len * len;
}

std::vector<vec2> Mesh::atlasUVs(const std::vector<vec2>& uvs, const TileLayout& t) const {
  if (!t.mode || uvs.size() != loopCount()) return uvs;
  std::vector<vec2> out(uvs.size());
  for (uint32_t f = 0; f < faceCount(); ++f) {
    int tile = f < t.faceTile.size() ? t.faceTile[f] : 0;
    vec2 cen = faceCentroidUV(*this, uvs, f);
    bool lost = tile < 0 || tile >= (int)t.tiles.size();
    if ((t.mode & TileByUdim) && !lost) {
      // outside the UDIM range (u < 0, u >= 10, v < 0) or not the face's own tile (more than 64): no texels
      vec2 fl = glm::floor(cen);
      lost = fl.x < 0 || fl.x > 9 || fl.y < 0 || udimOf(cen) != t.tiles[tile].udim;
    }
    if (lost) {
      for (uint32_t l = faceStart[f]; l < faceStart[f + 1]; ++l) out[l] = vec2(-10.f);
      continue;
    }
    // UDIM: the tile's corner; texture sets: each face wraps into its set's cell. Corners past the
    // tile's edge (a face straddling a tile border) are held in the cell, never in a neighbor's.
    vec2 o = (t.mode & TileByUdim) ? vec2(t.uvOrigin(tile)) : glm::floor(cen);
    vec2 c = t.cellOrigin(tile);
    for (uint32_t l = faceStart[f]; l < faceStart[f + 1]; ++l)
      out[l] = c + glm::clamp(uvs[l] - o, vec2(0.f), vec2(1.f)) * t.cellSize();
  }
  return out;
}

bool Mesh::updateTiles(const std::vector<vec2>* uvsIn) {
  const std::vector<vec2>& uvs = uvsIn ? *uvsIn : loopUV;
  uint32_t F = faceCount();
  if (!tiles.mode) {
    bool changed = tiles.grid != 1 || !tiles.tiles.empty();
    tiles.grid = 1;
    tiles.tiles.clear();
    tiles.faceTile.assign(F, 0);
    loopAtlasUV.clear();
    return changed;
  }
  if (uvs.size() != loopCount()) {  // no UVs (yet): one tile, texture coordinate = UV
    if (tiles.tiles.empty()) tiles.tiles.push_back({0, 1001, (tiles.mode & TileBySlot) ? slotName(0) : udimName(1001)});
    tiles.faceTile.assign(F, 0);
    loopAtlasUV.clear();
    return false;
  }
  // UDIM tiles: a face with no UV area gets no texels, so it never makes a tile of its own (a
  // stray face collapsed at u 1.2 made a whole second tile); it joins the most used tile
  std::vector<uint8_t> noArea;
  int commonUdim = 1001;
  if (tiles.mode & TileByUdim) {
    noArea.assign(F, 0);
    std::map<int, int> count;
    for (uint32_t f = 0; f < F; ++f) {
      noArea[f] = faceHasNoUVArea(f, uvs) ? 1 : 0;
      if (!noArea[f]) count[udimOf(faceCentroidUV(*this, uvs, f))]++;
    }
    int best = 0;
    for (auto& [u, n] : count)
      if (n > best) best = n, commonUdim = u;
  }
  auto keyOf = [&](uint32_t f) {
    TileInfo k;
    if (tiles.mode & TileBySlot) k.slot = f < faceMaterial.size() ? faceMaterial[f] : 0;
    if (tiles.mode & TileByUdim) k.udim = noArea[f] ? commonUdim : udimOf(faceCentroidUV(*this, uvs, f));
    return k;
  };
  auto name = [&](const TileInfo& k) {
    if ((tiles.mode & TileBySlot) && (tiles.mode & TileByUdim)) return slotName(k.slot) + " " + udimName(k.udim);
    return (tiles.mode & TileBySlot) ? slotName(k.slot) : udimName(k.udim);
  };
  std::vector<TileInfo> before = tiles.tiles;
  int gridBefore = tiles.grid;
  if (tiles.tiles.empty()) {  // a fresh layout: tiles in slot, then UDIM order
    std::vector<TileInfo> keys;
    for (uint32_t f = 0; f < F; ++f) {
      TileInfo k = keyOf(f);
      if (std::find(keys.begin(), keys.end(), k) == keys.end()) keys.push_back(k);
    }
    std::sort(keys.begin(), keys.end(), [](const TileInfo& a, const TileInfo& b) {
      return a.slot != b.slot ? a.slot < b.slot : a.udim < b.udim;
    });
    tiles.tiles = keys;
    tiles.grid = 1;
  }
  tiles.faceTile.assign(F, 0);
  bool full = false;
  for (uint32_t f = 0; f < F; ++f) {
    TileInfo k = keyOf(f);
    auto it = std::find(tiles.tiles.begin(), tiles.tiles.end(), k);
    int idx;
    if (it != tiles.tiles.end()) {
      idx = int(it - tiles.tiles.begin());
    } else if ((int)tiles.tiles.size() < kMaxTiles) {
      idx = (int)tiles.tiles.size();
      tiles.tiles.push_back(k);
    } else {
      idx = 0, full = true;
    }
    tiles.faceTile[f] = idx;
  }
  if (tiles.tiles.size() > (size_t)kMaxTiles) tiles.tiles.resize(kMaxTiles);
  if (full) PLOG_W("More than %d texture tiles: the rest share the first tile", kMaxTiles);
  for (auto& t : tiles.tiles)
    if (t.name.empty()) t.name = name(t);
  int need = 1;
  while (need * need < (int)tiles.tiles.size()) ++need;
  tiles.grid = std::max(tiles.grid, need);
  loopAtlasUV = atlasUVs(uvs, tiles);
  return tiles.grid != gridBefore || !(tiles.tiles.size() == before.size() &&
                                       std::equal(before.begin(), before.end(), tiles.tiles.begin()));
}

void Mesh::computeUVTopology() {
  updateTiles();
  uint32_t L = loopCount();
  UnionFind islands(L), uvv(L);
  for (uint32_t f = 0; f < faceCount(); ++f)
    for (uint32_t l = faceStart[f] + 1; l < faceStart[f + 1]; ++l) islands.unite(faceStart[f], l);

  // edge -> loops using it
  std::vector<uint32_t> eStart(edges.size() + 1, 0), eLoops(L);
  for (uint32_t l = 0; l < L; ++l) eStart[loopEdge[l] + 1]++;
  for (size_t i = 1; i < eStart.size(); ++i) eStart[i] += eStart[i - 1];
  std::vector<uint32_t> fill(eStart.begin(), eStart.end() - 1);
  for (uint32_t l = 0; l < L; ++l) eLoops[fill[loopEdge[l]]++] = l;
  auto lf = loopFace();

  for (uint32_t e = 0; e < edges.size(); ++e) {
    for (uint32_t i = eStart[e]; i < eStart[e + 1]; ++i) {
      for (uint32_t j = i + 1; j < eStart[e + 1]; ++j) {
        uint32_t a0 = eLoops[i], a1 = nextLoop(a0, lf[a0]);
        uint32_t b0 = eLoops[j], b1 = nextLoop(b0, lf[b0]);
        if (loopVert[a0] != loopVert[b0]) std::swap(b0, b1);  // opposite winding (usual)
        if (loopVert[a0] != loopVert[b0] || loopVert[a1] != loopVert[b1]) continue;
        if (uvEq(loopUV[a0], loopUV[b0]) && uvEq(loopUV[a1], loopUV[b1]) && faceTile(lf[a0]) == faceTile(lf[b0])) {
          islands.unite(a0, b0);
          uvv.unite(a0, b0);
          uvv.unite(a1, b1);
        }
      }
    }
  }
  std::unordered_map<uint32_t, uint32_t> remap;
  loopIsland.resize(L);
  for (uint32_t l = 0; l < L; ++l) {
    auto r = islands.find(l);
    auto it = remap.emplace(r, (uint32_t)remap.size()).first;
    loopIsland[l] = it->second;
  }
  islandCount = (uint32_t)remap.size();
  remap.clear();
  loopUVVert.resize(L);
  for (uint32_t l = 0; l < L; ++l) {
    auto r = uvv.find(l);
    auto it = remap.emplace(r, (uint32_t)remap.size()).first;
    loopUVVert[l] = it->second;
  }
  uvVertCount = (uint32_t)remap.size();
}

bool Mesh::validate(std::string* err) const {
  auto fail = [&](const char* m) {
    if (err) *err = m;
    return false;
  };
  if (faceStart.empty() || faceStart.front() != 0) return fail("faceStart must start with 0");
  if (faceStart.back() != loopVert.size()) return fail("faceStart must end with loop count");
  for (size_t f = 0; f + 1 < faceStart.size(); ++f)
    if (faceStart[f + 1] < faceStart[f] + 3) return fail("face with fewer than 3 corners");
  for (uint32_t v : loopVert)
    if (v >= positions.size()) return fail("loop vertex index out of range");
  if (!loopUV.empty() && loopUV.size() != loopVert.size()) return fail("uv count mismatch");
  for (auto& e : edges)
    if (e.x >= positions.size() || e.y >= positions.size()) return fail("edge index out of range");
  return true;
}

// ------------------------------------------------------------------ primitives

static void addQuad(Mesh& m, uint32_t a, uint32_t b, uint32_t c, uint32_t d, vec2 ua, vec2 ub, vec2 uc, vec2 ud) {
  m.loopVert.insert(m.loopVert.end(), {a, b, c, d});
  m.loopUV.insert(m.loopUV.end(), {ua, ub, uc, ud});
  m.faceStart.push_back((uint32_t)m.loopVert.size());
}

Mesh makeCube(float size) {
  Mesh m;
  m.name = "Cube";
  float h = size * 0.5f;
  m.positions = {{-h, -h, -h}, {h, -h, -h}, {h, h, -h}, {-h, h, -h},
                 {-h, -h, h},  {h, -h, h},  {h, h, h},  {-h, h, h}};
  m.faceStart = {0};
  // Cross layout: 4 columns x 3 rows of 1/4 x 1/3 cells
  auto cell = [](int cx, int cy, int corner) {
    vec2 o(cx * 0.25f, cy / 3.f);
    vec2 s(0.25f, 1.f / 3.f);
    const vec2 c[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    return o + c[corner] * s * 0.98f + s * 0.01f;
  };
  addQuad(m, 4, 5, 6, 7, cell(1, 1, 0), cell(1, 1, 1), cell(1, 1, 2), cell(1, 1, 3));  // +Z front
  addQuad(m, 5, 1, 2, 6, cell(2, 1, 0), cell(2, 1, 1), cell(2, 1, 2), cell(2, 1, 3));  // +X right
  addQuad(m, 1, 0, 3, 2, cell(3, 1, 0), cell(3, 1, 1), cell(3, 1, 2), cell(3, 1, 3));  // -Z back
  addQuad(m, 0, 4, 7, 3, cell(0, 1, 0), cell(0, 1, 1), cell(0, 1, 2), cell(0, 1, 3));  // -X left
  addQuad(m, 7, 6, 2, 3, cell(1, 2, 0), cell(1, 2, 1), cell(1, 2, 2), cell(1, 2, 3));  // +Y top
  addQuad(m, 0, 1, 5, 4, cell(1, 0, 0), cell(1, 0, 1), cell(1, 0, 2), cell(1, 0, 3));  // -Y bottom
  m.autoSmoothAngleDeg = 30.f;
  m.finalize();
  return m;
}

Mesh makeUVSphere(int seg, int rings, float r) {
  Mesh m;
  m.name = "Sphere";
  // vertices: poles + (rings-1) rings of seg verts
  m.positions.push_back({0, r, 0});
  for (int i = 1; i < rings; ++i) {
    float th = kPi * i / rings;
    for (int j = 0; j < seg; ++j) {
      float ph = 2 * kPi * j / seg;
      m.positions.push_back({r * std::sin(th) * std::cos(ph), r * std::cos(th), -r * std::sin(th) * std::sin(ph)});
    }
  }
  m.positions.push_back({0, -r, 0});
  uint32_t south = (uint32_t)m.positions.size() - 1;
  auto vid = [&](int ring, int j) { return uint32_t(1 + (ring - 1) * seg + (j % seg)); };
  m.faceStart = {0};
  for (int i = 0; i < rings; ++i) {
    for (int j = 0; j < seg; ++j) {
      float u0 = float(j) / seg, u1 = float(j + 1) / seg;
      float v0 = 1.f - float(i) / rings, v1 = 1.f - float(i + 1) / rings;
      if (i == 0) {
        m.loopVert.insert(m.loopVert.end(), {0u, vid(1, j), vid(1, j + 1)});
        m.loopUV.insert(m.loopUV.end(), {vec2((u0 + u1) / 2, v0), vec2(u0, v1), vec2(u1, v1)});
      } else if (i == rings - 1) {
        m.loopVert.insert(m.loopVert.end(), {vid(i, j), south, vid(i, j + 1)});
        m.loopUV.insert(m.loopUV.end(), {vec2(u0, v0), vec2((u0 + u1) / 2, v1), vec2(u1, v0)});
      } else {
        m.loopVert.insert(m.loopVert.end(), {vid(i, j), vid(i + 1, j), vid(i + 1, j + 1), vid(i, j + 1)});
        m.loopUV.insert(m.loopUV.end(), {vec2(u0, v0), vec2(u0, v1), vec2(u1, v1), vec2(u1, v0)});
      }
      m.faceStart.push_back((uint32_t)m.loopVert.size());
    }
  }
  m.autoSmoothAngleDeg = 180.f;
  m.finalize();
  // mark the UV seam (meridian at j=0)
  for (uint32_t e = 0; e < m.edges.size(); ++e) {
    vec3 a = m.positions[m.edges[e].x], b = m.positions[m.edges[e].y];
    if (std::abs(a.z) < 1e-6f && std::abs(b.z) < 1e-6f && a.x >= -1e-6f && b.x >= -1e-6f) m.edgeFlags[e] |= EdgeSeam;
  }
  return m;
}

Mesh makePlane(float size, int subdiv) {
  Mesh m;
  m.name = "Plane";
  int n = std::max(1, subdiv);
  for (int y = 0; y <= n; ++y)
    for (int x = 0; x <= n; ++x)
      m.positions.push_back({(float(x) / n - 0.5f) * size, 0, (0.5f - float(y) / n) * size});
  m.faceStart = {0};
  for (int y = 0; y < n; ++y)
    for (int x = 0; x < n; ++x) {
      uint32_t a = y * (n + 1) + x, b = a + 1, c = a + n + 2, d = a + n + 1;
      addQuad(m, a, b, c, d, {float(x) / n, float(y) / n}, {float(x + 1) / n, float(y) / n},
              {float(x + 1) / n, float(y + 1) / n}, {float(x) / n, float(y + 1) / n});
    }
  m.finalize();
  return m;
}

Mesh makeTorus(float R, float r, int su, int sv) {
  Mesh m;
  m.name = "Torus";
  for (int i = 0; i < su; ++i)
    for (int j = 0; j < sv; ++j) {
      float a = 2 * kPi * i / su, b = 2 * kPi * j / sv;
      m.positions.push_back({(R + r * std::cos(b)) * std::cos(a), r * std::sin(b), -(R + r * std::cos(b)) * std::sin(a)});
    }
  auto vid = [&](int i, int j) { return uint32_t((i % su) * sv + (j % sv)); };
  m.faceStart = {0};
  for (int i = 0; i < su; ++i)
    for (int j = 0; j < sv; ++j) {
      // UVs: U around the ring (x3 for squarer texels), V around the tube
      float u0 = float(i) / su, u1 = float(i + 1) / su, v0 = float(j) / sv, v1 = float(j + 1) / sv;
      addQuad(m, vid(i, j), vid(i + 1, j), vid(i + 1, j + 1), vid(i, j + 1), {u0, v0}, {u1, v0}, {u1, v1}, {u0, v1});
    }
  m.autoSmoothAngleDeg = 180.f;
  m.finalize();
  // seams along the u = 0 ring and the v = 0 loop
  for (uint32_t e = 0; e < m.edges.size(); ++e) {
    uint32_t a = m.edges[e].x, b = m.edges[e].y;
    int ia = int(a) / sv, ja = int(a) % sv, ib = int(b) / sv, jb = int(b) % sv;
    if ((ia == 0 && ib == 0) || (ja == 0 && jb == 0)) m.edgeFlags[e] |= EdgeSeam;
  }
  return m;
}

}  // namespace patina
