#include "core/MeshReduce.h"

#include <meshoptimizer.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace patina {

Mesh reduceMesh(const Mesh& m, const ReduceOptions& o, ReduceResult* res) {
  ReduceResult rr;
  ReduceResult& r = res ? *res : rr;
  r = ReduceResult();
  r.trianglesBefore = m.tris.size();
  // corners with the same vertex, UV and slot are one "wedge": meshoptimizer keeps the seams between wedges
  struct Wedge {
    uint32_t v, slot;
    vec2 uv;
  };
  std::vector<Wedge> wedges;
  std::vector<float> pos, attr;
  std::unordered_map<std::string, uint32_t> seen;
  seen.reserve(m.loopCount());
  std::vector<uint32_t> loopWedge(m.loopCount());
  std::vector<uint32_t> loopFaceOf = m.loopFace();
  for (uint32_t l = 0; l < m.loopCount(); ++l) {
    uint32_t f = loopFaceOf[l];
    Wedge w{m.loopVert[l], f < m.faceMaterial.size() ? m.faceMaterial[f] : 0, m.loopUV[l]};
    char key[16];
    std::memcpy(key, &w.v, 4), std::memcpy(key + 4, &w.slot, 4), std::memcpy(key + 8, &w.uv, 8);
    auto it = seen.emplace(std::string(key, sizeof key), (uint32_t)wedges.size());
    if (it.second) {
      wedges.push_back(w);
      const vec3& p = m.positions[w.v];
      pos.insert(pos.end(), {p.x, p.y, p.z});
      vec3 n = l < m.loopNormal.size() ? m.loopNormal[l] : vec3(0, 1, 0);
      attr.insert(attr.end(), {n.x, n.y, n.z});
    }
    loopWedge[l] = it.first->second;
  }
  std::vector<uint32_t> idx;
  idx.reserve(m.tris.size() * 3);
  for (auto& t : m.tris) idx.insert(idx.end(), {loopWedge[t.x], loopWedge[t.y], loopWedge[t.z]});
  size_t target = o.targetTriangles > 0 ? o.targetTriangles : size_t(std::clamp(o.ratio, 0.f, 1.f) * float(m.tris.size()));
  target = std::max<size_t>(target, 1) * 3;
  std::vector<uint32_t> out(idx.size());
  float w[3] = {o.normalWeight, o.normalWeight, o.normalWeight};
  unsigned opts = o.keepBorders ? meshopt_SimplifyLockBorder : 0;
  float err = 0.f;
  size_t n = idx.empty() ? 0
                         : meshopt_simplifyWithAttributes(out.data(), idx.data(), idx.size(), pos.data(), wedges.size(),
                                                          sizeof(float) * 3, attr.data(), sizeof(float) * 3, w, 3,
                                                          nullptr, std::min(target, idx.size()), o.maxError, opts, &err);
  out.resize(n);
  r.error = err;
  // the new mesh: kept vertices (in their old order), triangles of wedges
  Mesh q;
  q.name = m.name;
  q.materialNames = m.materialNames;
  q.autoSmoothAngleDeg = m.autoSmoothAngleDeg;
  std::vector<uint32_t> newIndex(m.positions.size(), UINT32_MAX);
  std::vector<uint8_t> used(m.positions.size(), 0);
  for (uint32_t i : out) used[wedges[i].v] = 1;
  for (uint32_t v = 0; v < m.positions.size(); ++v)
    if (used[v]) newIndex[v] = (uint32_t)q.positions.size(), q.positions.push_back(m.positions[v]);
  q.faceStart.push_back(0);
  for (size_t t = 0; t + 2 < out.size(); t += 3) {
    const Wedge &a = wedges[out[t]], &b = wedges[out[t + 1]], &c = wedges[out[t + 2]];
    if (a.v == b.v || b.v == c.v || a.v == c.v) continue;
    for (const Wedge* x : {&a, &b, &c}) q.loopVert.push_back(newIndex[x->v]), q.loopUV.push_back(x->uv);
    q.faceStart.push_back((uint32_t)q.loopVert.size());
    q.faceMaterial.push_back(a.slot);
    q.faceFlags.push_back(0);
  }
  r.trianglesAfter = q.faceCount();
  for (const VertexGroup& g : m.vertexGroups) {
    VertexGroup ng;
    ng.name = g.name;
    for (uint32_t v : g.verts)
      if (v < newIndex.size() && newIndex[v] != UINT32_MAX) ng.verts.push_back(newIndex[v]);
    if (!ng.verts.empty()) q.vertexGroups.push_back(std::move(ng));
  }
  if (m.tiles.active()) q.tiles.mode = m.tiles.mode, q.tiles.grid = m.tiles.grid, q.tiles.tiles = m.tiles.tiles;
  q.finalize();
  // seams and sharp edges stay where both ends do
  if (!m.edgeFlags.empty()) {
    std::unordered_map<uint64_t, uint8_t> flags;
    for (uint32_t e = 0; e < m.edges.size() && e < m.edgeFlags.size(); ++e)
      if (m.edgeFlags[e]) {
        uint32_t a = newIndex[m.edges[e].x], b = newIndex[m.edges[e].y];
        if (a != UINT32_MAX && b != UINT32_MAX) flags[a < b ? (uint64_t(a) << 32) | b : (uint64_t(b) << 32) | a] = m.edgeFlags[e];
      }
    for (uint32_t e = 0; e < q.edges.size(); ++e) {
      uint32_t a = q.edges[e].x, b = q.edges[e].y;
      auto it = flags.find(a < b ? (uint64_t(a) << 32) | b : (uint64_t(b) << 32) | a);
      if (it != flags.end()) q.edgeFlags[e] = it->second;
    }
  }
  return q;
}

void transformMesh(Mesh& m, const TransformOptions& o) {
  // a rotation in Blender's axes (x, -z, y of Patina's) applied to Patina's points
  auto rotB = [&](vec3 p) {
    vec3 b(p.x, -p.z, p.y);
    float ax = glm::radians(o.rotateDeg.x), ay = glm::radians(o.rotateDeg.y), az = glm::radians(o.rotateDeg.z);
    b = vec3(b.x, b.y * std::cos(ax) - b.z * std::sin(ax), b.y * std::sin(ax) + b.z * std::cos(ax));
    b = vec3(b.x * std::cos(ay) + b.z * std::sin(ay), b.y, -b.x * std::sin(ay) + b.z * std::cos(ay));
    b = vec3(b.x * std::cos(az) - b.y * std::sin(az), b.x * std::sin(az) + b.y * std::cos(az), b.z);
    return vec3(b.x, b.z, -b.y);
  };
  bool rot = o.rotateDeg != vec3(0.f);
  for (auto& p : m.positions) p = (rot ? rotB(p) : p) * o.scale;
  if (m.hasSourceNormals && rot)
    for (auto& n : m.loopNormal) n = rotB(n);
  AABB b;
  for (auto& p : m.positions) b.add(p);
  vec3 shift(0.f);
  if (o.origin == "center") shift = -b.center();
  else if (o.origin == "base") shift = -vec3(b.center().x, b.lo.y, b.center().z);
  if (o.ground && o.origin != "base") shift.y = -b.lo.y;
  for (auto& p : m.positions) p += shift;
  m.finalize();
}

}  // namespace patina
