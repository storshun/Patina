// (0.44) UVs carried over to the same model after an edit, for a mesh that comes back without its UVs (Blender in
// Edit Mode, a UV map removed): faces are matched by where their corners are, so the islands stay as they were
// instead of being unwrapped again.
#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "core/UVOps.h"

namespace patina::uv {

namespace {

struct KeyHash {
  size_t operator()(const glm::ivec3& k) const {
    return (size_t(uint32_t(k.x)) * 73856093u) ^ (size_t(uint32_t(k.y)) * 19349663u) ^ (size_t(uint32_t(k.z)) * 83492791u);
  }
};

// The UV the old face `f` gives a point near it: its own corner's UV at that place, else the point projected into
// the triangle of the face it is closest to, with that triangle's UVs extended (so the face stays in f's island).
vec2 uvFromFace(const Mesh& m, uint32_t f, const vec3& p) {
  uint32_t s = m.faceStart[f], e = m.faceStart[f + 1];
  float best = 1e30f;
  vec2 out = m.loopUV[s];
  for (uint32_t i = s + 1; i + 1 < e; ++i) {
    vec3 a = m.positions[m.loopVert[s]], b = m.positions[m.loopVert[i]], c = m.positions[m.loopVert[i + 1]];
    vec3 ab = b - a, ac = c - a, n = glm::cross(ab, ac);
    float nn = glm::dot(n, n);
    if (nn < 1e-30f) continue;
    vec3 ap = p - a;
    float dist = std::abs(glm::dot(ap, n)) / std::sqrt(nn);
    // barycentrics in the triangle's plane (unclamped: a point beyond the face extends its UVs)
    float v = glm::dot(glm::cross(ap, ac), n) / nn, w = glm::dot(glm::cross(ab, ap), n) / nn, u = 1.f - v - w;
    float outside = std::max({0.f, -u, -v, -w});
    float score = dist + outside * std::sqrt(std::sqrt(nn));
    if (score < best) {
      best = score;
      out = u * m.loopUV[s] + v * m.loopUV[i] + w * m.loopUV[i + 1];
    }
  }
  return out;
}

vec3 centroid(const Mesh& m, uint32_t f) {
  vec3 c(0.f);
  for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) c += m.positions[m.loopVert[l]];
  return c / float(m.faceStart[f + 1] - m.faceStart[f]);
}

}  // namespace

float carryUVs(const Mesh& from, Mesh& to, float tolerance) {
  to.loopUV.assign(to.loopCount(), vec2(0.f));
  if (from.faceCount() == 0 || to.faceCount() == 0 || from.loopUV.size() != from.loopCount()) return 0.f;
  vec3 lo(1e30f), hi(-1e30f);
  for (auto& p : from.positions) lo = glm::min(lo, p), hi = glm::max(hi, p);
  float size = std::max(glm::length(hi - lo), 1e-6f);
  float tol = tolerance > 0.f ? tolerance : 1e-6f * size;
  float cell = 2.f * tol;
  auto keyOf = [&](const vec3& p) { return glm::ivec3(glm::floor(p / cell)); };

  // old positions, welded: the same place is one id
  std::unordered_map<glm::ivec3, std::vector<uint32_t>, KeyHash> grid;  // cell -> ids
  std::vector<vec3> idPos;
  std::vector<int> oldId(from.positions.size(), -1);
  auto find = [&](const vec3& p) -> int {
    glm::ivec3 k = keyOf(p);
    for (int dz = -1; dz <= 1; ++dz)
      for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
          auto it = grid.find(k + glm::ivec3(dx, dy, dz));
          if (it == grid.end()) continue;
          for (uint32_t id : it->second)
            if (glm::length(idPos[id] - p) <= tol) return (int)id;
        }
    return -1;
  };
  for (size_t v = 0; v < from.positions.size(); ++v) {
    int id = find(from.positions[v]);
    if (id < 0) {
      id = (int)idPos.size();
      idPos.push_back(from.positions[v]);
      grid[keyOf(from.positions[v])].push_back((uint32_t)id);
    }
    oldId[v] = id;
  }
  // old faces by the places of their corners, and the faces at each place
  auto faceKey = [](std::vector<int> ids) {
    std::sort(ids.begin(), ids.end());
    uint64_t h = 1469598103934665603ull;
    for (int i : ids) h = (h ^ uint64_t(uint32_t(i))) * 1099511628211ull;
    return h;
  };
  std::unordered_map<uint64_t, std::vector<uint32_t>> byKey;
  std::vector<std::vector<uint32_t>> cornersAt(idPos.size());  // place -> old corners there
  std::vector<uint32_t> faceOf(from.loopCount());
  for (uint32_t f = 0; f < from.faceCount(); ++f) {
    std::vector<int> ids;
    for (uint32_t l = from.faceStart[f]; l < from.faceStart[f + 1]; ++l) {
      ids.push_back(oldId[from.loopVert[l]]);
      cornersAt[oldId[from.loopVert[l]]].push_back(l);
      faceOf[l] = f;
    }
    byKey[faceKey(ids)].push_back(f);
  }
  bool haveIslands = from.loopIsland.size() == from.loopCount();
  std::vector<char> used(from.faceCount(), 0);
  auto slotOf = [](const Mesh& m, uint32_t f) { return f < m.faceMaterial.size() ? m.faceMaterial[f] : 0u; };

  uint32_t matched = 0;
  for (uint32_t g = 0; g < to.faceCount(); ++g) {
    uint32_t s = to.faceStart[g], e = to.faceStart[g + 1];
    std::vector<int> ids;
    bool all = true;
    for (uint32_t l = s; l < e; ++l) {
      int id = find(to.positions[to.loopVert[l]]);
      ids.push_back(id);
      all &= id >= 0;
    }
    // the old face with its corners at the same places (an unused one first, then one in the same slot)
    int f = -1;
    if (all) {
      auto it = byKey.find(faceKey(ids));
      if (it != byKey.end()) {
        std::vector<int> want = ids;
        std::sort(want.begin(), want.end());
        int score = -1;
        for (uint32_t c : it->second) {
          std::vector<int> have;
          for (uint32_t l = from.faceStart[c]; l < from.faceStart[c + 1]; ++l) have.push_back(oldId[from.loopVert[l]]);
          std::sort(have.begin(), have.end());
          if (have != want) continue;
          int sc = (used[c] ? 0 : 2) + (slotOf(from, c) == slotOf(to, g) ? 1 : 0);
          if (sc > score) score = sc, f = (int)c;
        }
      }
    }
    if (f >= 0) {
      used[f] = 1;
      ++matched;
      for (uint32_t l = s; l < e; ++l) {
        int id = ids[l - s];
        for (uint32_t k = from.faceStart[f]; k < from.faceStart[f + 1]; ++k)
          if (oldId[from.loopVert[k]] == id) {
            to.loopUV[l] = from.loopUV[k];
            break;
          }
      }
      continue;
    }
    // no such face (new or changed, such as a face next to merged vertices): the nearest old face that shares a
    // place with it, else the nearest of all, picks the island; each corner at an old place takes that island's UV
    // there (so the face joins its neighbors), the others that face's UVs extended (one island, never a corner
    // from each neighbor)
    vec3 c = centroid(to, g);
    float best = 1e30f;
    for (int id : ids) {
      if (id < 0) continue;
      for (uint32_t k : cornersAt[id]) {
        float d = glm::length(centroid(from, faceOf[k]) - c);
        if (d < best) best = d, f = (int)faceOf[k];
      }
    }
    if (f < 0)
      for (uint32_t cand = 0; cand < from.faceCount(); ++cand) {
        float d = glm::length(centroid(from, cand) - c);
        if (d < best) best = d, f = (int)cand;
      }
    uint32_t island = haveIslands ? from.loopIsland[from.faceStart[f]] : 0;
    for (uint32_t l = s; l < e; ++l) {
      vec3 p = to.positions[to.loopVert[l]];
      int id = ids[l - s];
      bool done = false;
      for (uint32_t k = from.faceStart[f]; k < from.faceStart[f + 1] && id >= 0 && !done; ++k)
        if (oldId[from.loopVert[k]] == id) to.loopUV[l] = from.loopUV[k], done = true;
      if (!done && id >= 0 && haveIslands)
        for (uint32_t k : cornersAt[id])
          if (from.loopIsland[k] == island) {
            to.loopUV[l] = from.loopUV[k], done = true;
            break;
          }
      if (!done) to.loopUV[l] = uvFromFace(from, (uint32_t)f, p);
    }
  }
  return float(matched) / float(to.faceCount());
}

}  // namespace patina::uv
