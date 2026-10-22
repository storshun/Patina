#include "core/SurfaceReach.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <queue>
#include <unordered_map>

namespace patina {

uint64_t FaceGraph::keyOf(const Mesh& m) {
  uint64_t k = 1469598103934665603ull ^ (uint64_t(m.positions.size()) << 32) ^ m.faceCount() ^ (uint64_t(m.loopCount()) << 17);
  size_t step = std::max<size_t>(1, m.loopVert.size() / 197);
  for (size_t i = 0; i < m.loopVert.size(); i += step) k = (k ^ m.loopVert[i]) * 1099511628211ull;
  step = std::max<size_t>(1, m.positions.size() / 97);
  for (size_t i = 0; i < m.positions.size(); i += step) {
    uint32_t b[3];
    std::memcpy(b, &m.positions[i], sizeof b);
    k = (k ^ b[0] ^ (uint64_t(b[1]) << 21) ^ (uint64_t(b[2]) << 42)) * 1099511628211ull;
  }
  return k | 1;
}

bool FaceGraph::matches(const Mesh& m) const { return key && key == keyOf(m) && weld.size() == m.positions.size(); }

void FaceGraph::build(const Mesh& m) {
  size_t V = m.positions.size();
  weld.assign(V, 0);
  float q = std::max(m.boundingRadius, 1e-6f) * 1e-6f;
  std::unordered_map<uint64_t, uint32_t> ids;
  ids.reserve(V * 2);
  std::vector<uint32_t> rep;
  for (size_t v = 0; v < V; ++v) {
    const vec3& p = m.positions[v];
    int64_t x = std::llround(p.x / q), y = std::llround(p.y / q), z = std::llround(p.z / q);
    uint64_t h = (uint64_t(x) * 73856093ull) ^ (uint64_t(y) * 19349663ull) ^ (uint64_t(z) * 83492791ull);
    auto it = ids.find(h);
    if (it != ids.end() && glm::length(m.positions[rep[it->second]] - p) <= 2 * q) {
      weld[v] = it->second;
    } else {
      ids[h] = (uint32_t)rep.size();
      weld[v] = (uint32_t)rep.size();
      rep.push_back((uint32_t)v);
    }
  }
  uint32_t n = (uint32_t)rep.size();
  vfStart.assign(n + 1, 0);
  for (uint32_t f = 0; f < m.faceCount(); ++f)
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) ++vfStart[weld[m.loopVert[l]] + 1];
  for (uint32_t i = 0; i < n; ++i) vfStart[i + 1] += vfStart[i];
  vfFaces.assign(vfStart[n], 0);
  std::vector<uint32_t> fill(vfStart.begin(), vfStart.end() - 1);
  for (uint32_t f = 0; f < m.faceCount(); ++f)
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) vfFaces[fill[weld[m.loopVert[l]]]++] = f;
  key = keyOf(m);
}

std::vector<uint32_t> facesAlongSurface(const Mesh& m, const FaceGraph& g, uint32_t face, const vec3& point,
                                        float radius, const std::function<bool(uint32_t)>& pass) {
  std::vector<uint32_t> out;
  if (face >= m.faceCount() || g.weld.size() != m.positions.size()) return out;
  std::unordered_map<uint32_t, float> dist;
  using Item = std::pair<float, uint32_t>;
  std::priority_queue<Item, std::vector<Item>, std::greater<Item>> pq;
  dist[face] = 0.f;
  pq.push({0.f, face});
  std::unordered_map<uint32_t, uint8_t> refused;
  while (!pq.empty()) {
    auto [d, f] = pq.top();
    pq.pop();
    if (d > dist[f]) continue;
    out.push_back(f);
    vec3 cf = m.faceCenter(f);
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) {
      uint32_t w = g.weld[m.loopVert[l]];
      for (uint32_t k = g.vfStart[w]; k < g.vfStart[w + 1]; ++k) {
        uint32_t h = g.vfFaces[k];
        if (h == f) continue;
        vec3 ch = m.faceCenter(h);
        // the faces round the one under the cursor: straight from the point the brush is on
        float nd = f == face ? glm::length(ch - point) : d + glm::length(ch - cf);
        if (nd > radius) continue;
        auto it = dist.find(h);
        if (it != dist.end() && it->second <= nd) continue;
        if (pass) {
          auto r = refused.find(h);
          if (r == refused.end()) r = refused.emplace(h, pass(h) ? 0 : 1).first;
          if (r->second) continue;
        }
        dist[h] = nd;
        pq.push({nd, h});
      }
    }
  }
  return out;
}

}  // namespace patina
