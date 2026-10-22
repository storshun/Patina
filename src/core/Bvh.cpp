#include "core/Bvh.h"

#include "core/Mesh.h"

namespace patina {

void Bvh::build(const Mesh& m) {
  static uint64_t builds = 0;
  serial_ = ++builds;
  nodes_.clear();
  size_t T = m.tris.size();
  triIdx_.resize(T);
  triBox_.resize(T);
  triCenter_.resize(T);
  for (uint32_t i = 0; i < T; ++i) {
    triIdx_[i] = i;
    AABB b;
    for (int k = 0; k < 3; ++k) b.add(m.positions[m.loopVert[m.tris[i][k]]]);
    triBox_[i] = b;
    triCenter_[i] = b.center();
  }
  if (T == 0) return;
  nodes_.reserve(T * 2);
  buildRec(0, (uint32_t)T, 0);
}

uint32_t Bvh::buildRec(uint32_t begin, uint32_t end, int depth) {
  uint32_t id = (uint32_t)nodes_.size();
  nodes_.push_back({});
  AABB box, cbox;
  for (uint32_t i = begin; i < end; ++i) {
    box.add(triBox_[triIdx_[i]]);
    cbox.add(triCenter_[triIdx_[i]]);
  }
  nodes_[id].box = box;
  uint32_t n = end - begin;
  if (n <= 4 || depth > 48) {
    nodes_[id].left = begin;
    nodes_[id].count = n;
    return id;
  }
  // binned SAH along the largest centroid axis
  vec3 ext = cbox.size();
  int axis = ext.x > ext.y ? (ext.x > ext.z ? 0 : 2) : (ext.y > ext.z ? 1 : 2);
  if (ext[axis] <= 1e-12f) {
    nodes_[id].left = begin;
    nodes_[id].count = n;
    return id;
  }
  constexpr int B = 16;
  AABB bins[B];
  uint32_t counts[B] = {};
  float scale = B / ext[axis];
  auto binOf = [&](uint32_t t) {
    int b = int((triCenter_[t][axis] - cbox.lo[axis]) * scale);
    return std::clamp(b, 0, B - 1);
  };
  for (uint32_t i = begin; i < end; ++i) {
    int b = binOf(triIdx_[i]);
    bins[b].add(triBox_[triIdx_[i]]);
    counts[b]++;
  }
  auto area = [](const AABB& b) {
    if (!b.valid()) return 0.f;
    vec3 s = b.size();
    return 2.f * (s.x * s.y + s.y * s.z + s.z * s.x);
  };
  float best = std::numeric_limits<float>::max();
  int bestSplit = B / 2;
  for (int s = 1; s < B; ++s) {
    AABB l, r;
    uint32_t cl = 0, cr = 0;
    for (int i = 0; i < s; ++i) l.add(bins[i]), cl += counts[i];
    for (int i = s; i < B; ++i) r.add(bins[i]), cr += counts[i];
    if (!cl || !cr) continue;
    float cost = area(l) * cl + area(r) * cr;
    if (cost < best) best = cost, bestSplit = s;
  }
  auto mid = std::partition(triIdx_.begin() + begin, triIdx_.begin() + end,
                            [&](uint32_t t) { return binOf(t) < bestSplit; });
  uint32_t m = uint32_t(mid - triIdx_.begin());
  if (m == begin || m == end) m = begin + n / 2;
  uint32_t l = buildRec(begin, m, depth + 1);
  uint32_t r = buildRec(m, end, depth + 1);
  (void)l;
  nodes_[id].left = r;  // interior: right child index stored; left child is id+1
  nodes_[id].count = 0;
  return id;
}

static bool rayBox(const AABB& b, const vec3& o, const vec3& inv, float tMax) {
  vec3 t0 = (b.lo - o) * inv, t1 = (b.hi - o) * inv;
  vec3 tmin = glm::min(t0, t1), tmax = glm::max(t0, t1);
  float a = std::max(std::max(tmin.x, tmin.y), std::max(tmin.z, 0.f));
  float c = std::min(std::min(tmax.x, tmax.y), std::min(tmax.z, tMax));
  return a <= c;
}

RayHit Bvh::raycast(const Mesh& m, const vec3& o, const vec3& d, float tMax, const std::vector<uint8_t>* skip) const {
  if (skip && skip->size() != m.faceCount()) skip = nullptr;
  RayHit h;
  if (nodes_.empty()) return h;
  vec3 inv = 1.f / glm::vec3(std::abs(d.x) > 1e-20f ? d.x : 1e-20f, std::abs(d.y) > 1e-20f ? d.y : 1e-20f,
                             std::abs(d.z) > 1e-20f ? d.z : 1e-20f);
  uint32_t stack[128];
  int sp = 0;
  stack[sp++] = 0;
  float best = tMax;
  while (sp) {
    const Node& n = nodes_[stack[--sp]];
    if (!rayBox(n.box, o, inv, best)) continue;
    if (n.count) {
      for (uint32_t i = n.left; i < n.left + n.count; ++i) {
        uint32_t t = triIdx_[i];
        if (skip && (*skip)[m.triFace[t]]) continue;
        const vec3& p0 = m.positions[m.loopVert[m.tris[t][0]]];
        const vec3& p1 = m.positions[m.loopVert[m.tris[t][1]]];
        const vec3& p2 = m.positions[m.loopVert[m.tris[t][2]]];
        // Moller-Trumbore, two-sided
        vec3 e1 = p1 - p0, e2 = p2 - p0;
        vec3 p = glm::cross(d, e2);
        float det = glm::dot(e1, p);
        if (std::abs(det) < 1e-14f) continue;
        float id = 1.f / det;
        vec3 s = o - p0;
        float u = glm::dot(s, p) * id;
        constexpr float eps = 1e-6f;  // tolerant edges: rays through shared edges must not slip between triangles
        if (u < -eps || u > 1 + eps) continue;
        vec3 q = glm::cross(s, e1);
        float v = glm::dot(d, q) * id;
        if (v < -eps || u + v > 1 + eps) continue;
        float tt = glm::dot(e2, q) * id;
        if (tt <= 1e-6f || tt >= best) continue;
        best = tt;
        h.hit = true;
        h.t = tt;
        h.tri = t;
        h.bary = {u, v};
      }
    } else {
      uint32_t self = uint32_t(&n - nodes_.data());
      if (sp + 2 <= 128) {
        stack[sp++] = n.left;    // right child
        stack[sp++] = self + 1;  // left child (visited first)
      }
    }
  }
  if (h.hit) {
    auto tri = m.tris[h.tri];
    float w0 = 1 - h.bary.x - h.bary.y;
    h.position = o + d * h.t;
    h.normal = glm::normalize(m.loopNormal[tri[0]] * w0 + m.loopNormal[tri[1]] * h.bary.x +
                              m.loopNormal[tri[2]] * h.bary.y);
    vec3 p0 = m.positions[m.loopVert[tri[0]]], p1 = m.positions[m.loopVert[tri[1]]],
         p2 = m.positions[m.loopVert[tri[2]]];
    h.faceNormal = glm::normalize(glm::cross(p1 - p0, p2 - p0));
    h.meshUV = m.loopUV[tri[0]] * w0 + m.loopUV[tri[1]] * h.bary.x + m.loopUV[tri[2]] * h.bary.y;
    h.uv = m.texUV(tri[0]) * w0 + m.texUV(tri[1]) * h.bary.x + m.texUV(tri[2]) * h.bary.y;
    h.face = m.triFace[h.tri];
  }
  return h;
}

// Ericson, Real-Time Collision Detection 5.1.5
static vec3 closestOnTriangle(const vec3& p, const vec3& a, const vec3& b, const vec3& c, vec2& bary) {
  vec3 ab = b - a, ac = c - a, ap = p - a;
  float d1 = glm::dot(ab, ap), d2 = glm::dot(ac, ap);
  if (d1 <= 0 && d2 <= 0) return bary = {0, 0}, a;
  vec3 bp = p - b;
  float d3 = glm::dot(ab, bp), d4 = glm::dot(ac, bp);
  if (d3 >= 0 && d4 <= d3) return bary = {1, 0}, b;
  float vc = d1 * d4 - d3 * d2;
  if (vc <= 0 && d1 >= 0 && d3 <= 0) {
    float v = d1 / (d1 - d3);
    return bary = {v, 0}, a + ab * v;
  }
  vec3 cp = p - c;
  float d5 = glm::dot(ab, cp), d6 = glm::dot(ac, cp);
  if (d6 >= 0 && d5 <= d6) return bary = {0, 1}, c;
  float vb = d5 * d2 - d1 * d6;
  if (vb <= 0 && d2 >= 0 && d6 <= 0) {
    float w = d2 / (d2 - d6);
    return bary = {0, w}, a + ac * w;
  }
  float va = d3 * d6 - d5 * d4;
  if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
    float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
    return bary = {1 - w, w}, b + (c - b) * w;
  }
  float denom = 1.f / (va + vb + vc);
  float v = vb * denom, w = vc * denom;
  bary = {v, w};
  return a + ab * v + ac * w;
}

RayHit Bvh::closestPoint(const Mesh& m, const vec3& p, float maxDist) const {
  RayHit h;
  if (nodes_.empty()) return h;
  float best = maxDist * maxDist;
  uint32_t stack[128];
  int sp = 0;
  stack[sp++] = 0;
  while (sp) {
    uint32_t id = stack[--sp];
    const Node& n = nodes_[id];
    vec3 q = glm::clamp(p, n.box.lo, n.box.hi);
    if (glm::dot(q - p, q - p) > best) continue;
    if (n.count) {
      for (uint32_t i = n.left; i < n.left + n.count; ++i) {
        uint32_t t = triIdx_[i];
        vec2 bary;
        vec3 c = closestOnTriangle(p, m.positions[m.loopVert[m.tris[t][0]]], m.positions[m.loopVert[m.tris[t][1]]],
                                   m.positions[m.loopVert[m.tris[t][2]]], bary);
        float d = glm::dot(c - p, c - p);
        if (d < best) {
          best = d;
          h.hit = true;
          h.tri = t;
          h.bary = bary;
          h.position = c;
          h.t = std::sqrt(d);
        }
      }
    } else if (sp + 2 <= 128) {
      // visit the nearer child first
      uint32_t l = id + 1, r = n.left;
      vec3 ql = glm::clamp(p, nodes_[l].box.lo, nodes_[l].box.hi), qr = glm::clamp(p, nodes_[r].box.lo, nodes_[r].box.hi);
      if (glm::dot(ql - p, ql - p) < glm::dot(qr - p, qr - p)) std::swap(l, r);
      stack[sp++] = l;
      stack[sp++] = r;
    }
  }
  if (h.hit) {
    auto tri = m.tris[h.tri];
    float w0 = 1 - h.bary.x - h.bary.y;
    h.meshUV = m.loopUV[tri[0]] * w0 + m.loopUV[tri[1]] * h.bary.x + m.loopUV[tri[2]] * h.bary.y;
    h.uv = m.texUV(tri[0]) * w0 + m.texUV(tri[1]) * h.bary.x + m.texUV(tri[2]) * h.bary.y;
    h.normal = glm::normalize(m.loopNormal[tri[0]] * w0 + m.loopNormal[tri[1]] * h.bary.x + m.loopNormal[tri[2]] * h.bary.y);
    h.face = m.triFace[h.tri];
  }
  return h;
}

void Bvh::querySphere(const vec3& c, float r, std::vector<uint32_t>& out) const {
  if (nodes_.empty()) return;
  std::vector<uint32_t> stack = {0};
  while (!stack.empty()) {
    uint32_t id = stack.back();
    stack.pop_back();
    const Node& n = nodes_[id];
    vec3 q = glm::clamp(c, n.box.lo, n.box.hi);
    if (glm::dot(q - c, q - c) > r * r) continue;
    if (n.count) {
      for (uint32_t i = n.left; i < n.left + n.count; ++i) out.push_back(triIdx_[i]);
    } else {
      stack.push_back(n.left);
      stack.push_back(id + 1);
    }
  }
}

}  // namespace patina
