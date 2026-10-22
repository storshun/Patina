#include "core/MeshSelect.h"

#include <algorithm>
#include <cmath>
#include <deque>

namespace patina::select {

void EdgeFaces::build(const Mesh& m) {
  const size_t E = m.edges.size();
  start.assign(E + 1, 0);
  for (uint32_t l = 0; l < m.loopCount() && l < m.loopEdge.size(); ++l)
    if (m.loopEdge[l] < E) ++start[m.loopEdge[l] + 1];
  for (size_t e = 0; e < E; ++e) start[e + 1] += start[e];
  faces.assign(start[E], 0);
  std::vector<uint32_t> at(start.begin(), start.end() - 1);
  for (uint32_t f = 0; f < m.faceCount(); ++f)
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l)
      if (l < m.loopEdge.size() && m.loopEdge[l] < E) faces[at[m.loopEdge[l]]++] = f;
}

namespace {

// the face on the other side of edge e from f (none: a border, or more than two faces meet there)
int64_t across(const EdgeFaces& ef, uint32_t e, uint32_t f) {
  if (e + 1 >= ef.start.size() || ef.start[e + 1] - ef.start[e] != 2) return -1;
  uint32_t a = ef.faces[ef.start[e]], b = ef.faces[ef.start[e] + 1];
  return a == f ? int64_t(b) : b == f ? int64_t(a) : -1;
}

int localOf(const Mesh& m, uint32_t f, uint32_t e) {
  for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l)
    if (m.loopEdge[l] == e) return int(l - m.faceStart[f]);
  return -1;
}

}  // namespace

std::vector<uint32_t> faceLoop(const Mesh& m, const EdgeFaces& ef, uint32_t f, uint32_t k) {
  std::vector<uint32_t> out = {f};
  if (f >= m.faceCount() || m.faceSize(f) != 4 || !ef.builtFor(m) || m.loopEdge.size() != m.loopCount()) return out;
  std::vector<uint8_t> seen(m.faceCount(), 0);
  seen[f] = 1;
  for (uint32_t side : {k % 4, (k + 2) % 4}) {
    uint32_t cur = f, exit = side;
    for (size_t guard = 0; guard < m.faceCount(); ++guard) {
      uint32_t e = m.loopEdge[m.faceStart[cur] + exit];
      int64_t g = across(ef, e, cur);
      if (g < 0 || seen[g] || m.faceSize(uint32_t(g)) != 4) break;  // (a border, closed round, or not a quad)
      seen[g] = 1;
      out.push_back(uint32_t(g));
      int j = localOf(m, uint32_t(g), e);
      if (j < 0) break;
      cur = uint32_t(g), exit = uint32_t(j + 2) % 4;
    }
  }
  return out;
}

std::vector<uint32_t> faceRing(const Mesh& m, const EdgeFaces& ef, uint32_t f, uint32_t k) {
  return faceLoop(m, ef, f, (k + 1) % 4);
}

uint32_t nearestEdge(const Mesh& m, uint32_t f, const vec3& p) {
  uint32_t best = 0;
  float bestD = 1e30f;
  for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) {
    vec3 a = m.positions[m.loopVert[l]], b = m.positions[m.loopVert[m.nextLoop(l, f)]];
    vec3 ab = b - a;
    float t = std::clamp(glm::dot(p - a, ab) / std::max(glm::dot(ab, ab), 1e-20f), 0.f, 1.f);
    float d = glm::length(a + ab * t - p);
    if (d < bestD) bestD = d, best = l - m.faceStart[f];
  }
  return best;
}

std::vector<uint32_t> linked(const Mesh& m, const EdgeFaces& ef, const std::vector<uint32_t>& seeds, bool byVertex) {
  const uint32_t F = m.faceCount();
  std::vector<uint8_t> seen(F, 0);
  std::deque<uint32_t> q;
  for (uint32_t s : seeds)
    if (s < F && !seen[s]) seen[s] = 1, q.push_back(s);
  // vertex -> faces, for faces that only share a corner
  std::vector<uint32_t> vStart, vFaces;
  if (byVertex) {
    vStart.assign(m.positions.size() + 1, 0);
    for (uint32_t v : m.loopVert) ++vStart[v + 1];
    for (size_t i = 0; i + 1 < vStart.size(); ++i) vStart[i + 1] += vStart[i];
    vFaces.resize(m.loopVert.size());
    std::vector<uint32_t> at(vStart.begin(), vStart.end() - 1);
    for (uint32_t f = 0; f < F; ++f)
      for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) vFaces[at[m.loopVert[l]]++] = f;
  }
  std::vector<uint32_t> out;
  while (!q.empty()) {
    uint32_t f = q.front();
    q.pop_front();
    out.push_back(f);
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) {
      if (byVertex) {
        uint32_t v = m.loopVert[l];
        for (uint32_t i = vStart[v]; i < vStart[v + 1]; ++i)
          if (!seen[vFaces[i]]) seen[vFaces[i]] = 1, q.push_back(vFaces[i]);
      } else if (l < m.loopEdge.size() && ef.builtFor(m)) {
        uint32_t e = m.loopEdge[l];
        for (uint32_t i = ef.start[e]; i < ef.start[e + 1]; ++i)
          if (!seen[ef.faces[i]]) seen[ef.faces[i]] = 1, q.push_back(ef.faces[i]);
      }
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<uint32_t> flatRegion(const Mesh& m, const EdgeFaces& ef, const std::vector<uint32_t>& seeds, float angleDeg) {
  const uint32_t F = m.faceCount();
  std::vector<uint32_t> out;
  if (!ef.builtFor(m) || m.loopEdge.size() != m.loopCount()) return seeds;
  const float cosMax = std::cos(glm::radians(std::clamp(angleDeg, 0.f, 180.f)));
  std::vector<vec3> n(F);
  for (uint32_t f = 0; f < F; ++f) n[f] = m.faceNormal(f);
  std::vector<uint8_t> seen(F, 0);
  std::deque<uint32_t> q;
  for (uint32_t s : seeds)
    if (s < F && !seen[s]) seen[s] = 1, q.push_back(s);
  while (!q.empty()) {
    uint32_t f = q.front();
    q.pop_front();
    out.push_back(f);
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) {
      uint32_t e = m.loopEdge[l];
      for (uint32_t i = ef.start[e]; i < ef.start[e + 1]; ++i) {
        uint32_t g = ef.faces[i];
        if (seen[g] || glm::dot(n[f], n[g]) < cosMax) continue;
        seen[g] = 1;
        q.push_back(g);
      }
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace patina::select
