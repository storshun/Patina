#include "core/RetopoLoops.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace patina::retopo {

namespace {
using EdgeKey = uint64_t;
inline EdgeKey ekey(uint32_t a, uint32_t b) { return (uint64_t(std::min(a, b)) << 32) | std::max(a, b); }
}  // namespace

EdgeLoops edgeLoops(const Mesh& m) {
  EdgeLoops out;
  const size_t nv = m.positions.size();
  const uint32_t nf = m.faceCount();
  std::vector<std::vector<uint32_t>> nb(nv), faces(nv);
  std::unordered_map<EdgeKey, int> uses;
  for (uint32_t f = 0; f < nf; ++f)
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) {
      uint32_t a = m.loopVert[l], b = m.loopVert[l + 1 < m.faceStart[f + 1] ? l + 1 : m.faceStart[f]];
      if (uses[ekey(a, b)]++ == 0) nb[a].push_back(b), nb[b].push_back(a);
      faces[a].push_back(f);
    }
  std::vector<char> open(nv, 0);
  for (auto& [e, n] : uses)
    if (n == 1) open[uint32_t(e >> 32)] = open[uint32_t(e & 0xFFFFFFFF)] = 1;
  // whether a face has a and b next to each other
  auto faceHasEdge = [&](uint32_t f, uint32_t a, uint32_t b) {
    uint32_t s = m.faceStart[f], n = m.faceStart[f + 1] - s;
    for (uint32_t k = 0; k < n; ++k) {
      uint32_t x = m.loopVert[s + k], y = m.loopVert[s + (k + 1) % n];
      if ((x == a && y == b) || (x == b && y == a)) return true;
    }
    return false;
  };
  // on from u -> v: the edge at v across from it (none: a pole, an open edge, or not all quads round v)
  auto across = [&](uint32_t u, uint32_t v) -> int64_t {
    if (open[v] || nb[v].size() != 4 || faces[v].size() != 4) return -1;
    for (uint32_t f : faces[v])
      if (m.faceStart[f + 1] - m.faceStart[f] != 4) return -1;
    int64_t w = -1;
    for (uint32_t x : nb[v]) {
      if (x == u) continue;
      bool side = false;  // (x shares a face with u at v: a side edge, not the one across)
      for (uint32_t f : faces[v])
        if (faceHasEdge(f, u, v) && faceHasEdge(f, v, x)) side = true;
      if (!side) {
        if (w >= 0) return -1;
        w = x;
      }
    }
    return w;
  };
  std::unordered_set<EdgeKey> done;
  for (uint32_t a = 0; a < nv; ++a)
    for (uint32_t b : nb[a]) {
      if (done.count(ekey(a, b))) continue;
      // forwards from a -> b, then backwards from b -> a
      std::vector<uint32_t> fwd = {a, b}, back;
      done.insert(ekey(a, b));
      bool closed = false;
      for (size_t guard = 0; guard < nv + 1; ++guard) {
        int64_t w = across(fwd[fwd.size() - 2], fwd.back());
        if (w < 0) break;
        if ((uint32_t)w == fwd[0]) {  // (back round)
          closed = true;
          done.insert(ekey(fwd.back(), (uint32_t)w));
          break;
        }
        if (done.count(ekey(fwd.back(), (uint32_t)w))) break;
        done.insert(ekey(fwd.back(), (uint32_t)w));
        fwd.push_back((uint32_t)w);
      }
      if (!closed) {
        uint32_t u = b, v = a;
        for (size_t guard = 0; guard < nv + 1; ++guard) {
          int64_t w = across(u, v);
          if (w < 0 || done.count(ekey(v, (uint32_t)w))) break;
          done.insert(ekey(v, (uint32_t)w));
          back.push_back((uint32_t)w);
          u = v, v = (uint32_t)w;
        }
      }
      std::vector<uint32_t> loop(back.rbegin(), back.rend());
      loop.insert(loop.end(), fwd.begin(), fwd.end());
      out.loops.push_back(std::move(loop));
      out.closed.push_back(closed);
    }
  for (uint32_t v = 0; v < nv; ++v)
    if (!open[v] && !nb[v].empty() && nb[v].size() != 4) out.poles.push_back(v), out.poleEdges.push_back((int)nb[v].size());
  return out;
}

void LoopFinder::build(const Mesh& m, const EdgeLoops& L) {
  edges.clear(), loopOf.clear(), grid.clear();
  double len = 0;
  for (size_t i = 0; i < L.loops.size(); ++i) {
    const auto& lp = L.loops[i];
    size_t n = lp.size() - 1 + (L.closed[i] ? 1 : 0);
    for (size_t k = 0; k < n; ++k) {
      edges.push_back({lp[k], lp[(k + 1) % lp.size()]}), loopOf.push_back((int)i);
      len += glm::length(m.positions[lp[k]] - m.positions[lp[(k + 1) % lp.size()]]);
    }
  }
  cell = edges.empty() ? 1.f : std::max(1e-6f, float(2 * len / edges.size()));
  for (uint32_t e = 0; e < edges.size(); ++e) {
    vec3 c = 0.5f * (m.positions[edges[e][0]] + m.positions[edges[e][1]]);
    glm::ivec3 q = glm::ivec3(glm::floor(c / cell));
    grid[(uint64_t(q.x & 0x1FFFFF) << 42) | (uint64_t(q.y & 0x1FFFFF) << 21) | uint64_t(q.z & 0x1FFFFF)].push_back(e);
  }
}

int LoopFinder::nearest(const Mesh& m, const vec3& p, float maxDist) const {
  glm::ivec3 q = glm::ivec3(glm::floor(p / cell));
  float bd = maxDist * maxDist;
  int best = -1;
  for (int dz = -1; dz <= 1; ++dz)
    for (int dy = -1; dy <= 1; ++dy)
      for (int dx = -1; dx <= 1; ++dx) {
        glm::ivec3 c = q + glm::ivec3(dx, dy, dz);
        auto it = grid.find((uint64_t(c.x & 0x1FFFFF) << 42) | (uint64_t(c.y & 0x1FFFFF) << 21) | uint64_t(c.z & 0x1FFFFF));
        if (it == grid.end()) continue;
        for (uint32_t e : it->second) {
          vec3 a = m.positions[edges[e][0]], b = m.positions[edges[e][1]], ab = b - a;
          float l2 = glm::dot(ab, ab), t = l2 > 0 ? std::clamp(glm::dot(p - a, ab) / l2, 0.f, 1.f) : 0.f;
          vec3 d = a + t * ab - p;
          if (float d2 = glm::dot(d, d); d2 < bd) bd = d2, best = loopOf[e];
        }
      }
  return best;
}

void loopSegments(const Mesh& m, const EdgeLoops& L, float crossSize, int highlight, std::vector<vec3>& pts, std::vector<vec4>& cols) {
  pts.clear(), cols.clear();
  for (size_t i = 0; i < L.poles.size(); ++i) {
    vec4 c = L.poleEdges[i] == 3 ? vec4(0.2f, 0.45f, 1.f, 1.f) : L.poleEdges[i] == 5 ? vec4(1.f, 0.55f, 0.1f, 1.f) : vec4(1.f, 0.1f, 0.1f, 1.f);
    const vec3& p = m.positions[L.poles[i]];
    for (int k = 0; k < 3; ++k) {
      vec3 d(0);
      d[k] = crossSize;
      pts.push_back(p - d), pts.push_back(p + d), cols.push_back(c), cols.push_back(c);
    }
  }
  if (highlight >= 0 && highlight < (int)L.loops.size()) {
    const auto& lp = L.loops[highlight];
    size_t n = lp.size() - 1 + (L.closed[highlight] ? 1 : 0);
    const vec4 c(0.35f, 1.f, 0.3f, 1.f);
    for (size_t k = 0; k < n; ++k)
      pts.push_back(m.positions[lp[k]]), pts.push_back(m.positions[lp[(k + 1) % lp.size()]]), cols.push_back(c), cols.push_back(c);
  }
}

}  // namespace patina::retopo
