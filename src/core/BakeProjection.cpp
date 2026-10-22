#include "core/BakeProjection.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <unordered_map>

#include "core/Bvh.h"
#include "core/Log.h"
#include "core/Mesh.h"
#include "core/Parallel.h"
#include "core/Progress.h"

namespace patina {

std::vector<vec2> texelPattern(int samples) {
  int n = samples >= 16 ? 4 : samples >= 9 ? 3 : samples >= 4 ? 2 : 1;
  // a grid turned by atan(1/2): no two points share a row or a column of the texel, so edges that run along the
  // texture's axes are measured at n * n places, not n
  const float c = std::cos(0.4636476f), s = std::sin(0.4636476f);
  std::vector<vec2> p;
  for (int j = 0; j < n; ++j)
    for (int i = 0; i < n; ++i) {
      vec2 g((i + 0.5f) / n - 0.5f, (j + 0.5f) / n - 0.5f);
      vec2 r(g.x * c - g.y * s, g.x * s + g.y * c);
      p.push_back(glm::clamp(r * 0.96f + 0.5f, vec2(0.02f), vec2(0.98f)));
    }
  std::stable_sort(p.begin(), p.end(), [](vec2 a, vec2 b) { return glm::length(a - 0.5f) < glm::length(b - 0.5f); });
  return p;
}

std::vector<vec3> rayNormals(const Mesh& m) {
  // weld by place: files split their vertices at UV seams and hard edges, where the rays must not part
  float q = 1e-6f * std::max(glm::length(m.bounds.hi - m.bounds.lo), 1e-6f);
  std::unordered_map<uint64_t, uint32_t> ids;
  ids.reserve(m.positions.size());
  std::vector<uint32_t> weld(m.positions.size());
  uint32_t next = 0;
  for (size_t v = 0; v < m.positions.size(); ++v) {
    const vec3& p = m.positions[v];
    auto k = [&](float x) { return uint64_t(int64_t(std::llround(double(x) / q)) & 0x1FFFFF); };
    uint64_t key = k(p.x) | k(p.y) << 21 | k(p.z) << 42;
    auto it = ids.emplace(key, next);
    if (it.second) ++next;
    weld[v] = it.first->second;
  }
  std::vector<vec3> acc(next, vec3(0));
  for (uint32_t f = 0; f < m.faceCount(); ++f) {
    vec3 n(0);  // Newell's: twice the area, so the sum is area-weighted
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) {
      const vec3& a = m.positions[m.loopVert[l]];
      const vec3& b = m.positions[m.loopVert[m.nextLoop(l, f)]];
      n.x += (a.y - b.y) * (a.z + b.z);
      n.y += (a.z - b.z) * (a.x + b.x);
      n.z += (a.x - b.x) * (a.y + b.y);
    }
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) acc[weld[m.loopVert[l]]] += n;
  }
  std::vector<vec3> out(m.positions.size());
  for (size_t v = 0; v < out.size(); ++v) {
    vec3 n = acc[weld[v]];
    float len = glm::length(n);
    out[v] = len > 1e-30f ? n / len : vec3(0, 0, 1);
  }
  return out;
}

float cageDistance(const Mesh& low, const Mesh& high, const Bvh& hb, std::string* err) {
  float radius = std::max(low.boundingRadius, 1e-6f);
  std::vector<vec3> pts;
  size_t V = low.positions.size(), F = low.faceCount();
  size_t vs = std::max<size_t>(1, V / 40000), fs = std::max<size_t>(1, F / 40000);
  for (size_t v = 0; v < V; v += vs) pts.push_back(low.positions[v]);
  for (size_t f = 0; f < F; f += fs) pts.push_back(low.faceCenter(uint32_t(f)));
  if (pts.empty()) return -1.f;
  std::vector<float> dist(pts.size(), -1.f);
  float reach = 0.3f * radius;
  parallelFor(pts.size(), [&](size_t i) {
    RayHit h = hb.closestPoint(high, pts[i], reach);
    if (h.hit) dist[i] = glm::length(h.position - pts[i]);
  }, 32);
  size_t away = (size_t)std::count(dist.begin(), dist.end(), -1.f);
  if (away * 20 > pts.size()) {
    if (err)
      *err = "The two models are not in the same place: " + std::to_string(int(100.0 * away / pts.size() + 0.5)) +
             "% of the low-poly is far from the high-poly surface. Check that they have the same position, rotation "
             "and scale (in Blender: apply the transforms of both before exporting).";
    return -1.f;
  }
  dist.erase(std::remove(dist.begin(), dist.end(), -1.f), dist.end());
  std::sort(dist.begin(), dist.end());
  float p99 = dist.empty() ? 0.f : dist[std::min(dist.size() - 1, size_t(dist.size() * 0.99))];
  return std::clamp(1.5f * p99 + 0.002f * radius, 0.003f * radius, 0.25f * radius);
}

namespace {

struct PointHit {
  ProjectionKind kind = ProjectionKind::None;
  uint32_t tri = 0;
  vec2 bary{0};
  vec3 position{0};
  float dist = 0.f;
};

vec3 interp(const std::vector<vec3>& a, const glm::uvec3& t, vec2 b) {
  return a[t[0]] * (1.f - b.x - b.y) + a[t[1]] * b.x + a[t[2]] * b.y;
}

PointHit projectPoint(const Mesh& high, const Bvh& hb, vec3 P, vec3 Nr, float d, float eps) {
  PointHit best;
  float bestScore = 1e30f;
  // both ways along the normal; each ray starts a hair behind the point, so a surface right on it is found
  for (int dir = 0; dir < 2; ++dir) {
    vec3 D = dir == 0 ? Nr : -Nr;
    RayHit h = hb.raycast(high, P - D * eps, D, d + eps);
    if (!h.hit) continue;
    float dist = std::abs(h.t - eps);
    bool facing = glm::dot(h.normal, Nr) >= 0.f || glm::dot(h.faceNormal, Nr) >= 0.f;
    float score = dist + (facing ? 0.f : d);  // the back of a thin part: only when nothing else is there
    if (score < bestScore) {
      bestScore = score;
      best = {ProjectionKind::Ray, h.tri, h.bary, h.position, dist};
    }
  }
  if (best.kind != ProjectionKind::None) return best;
  RayHit c = hb.closestPoint(high, P, 2.f * d);
  if (c.hit) return {ProjectionKind::Near, c.tri, c.bary, c.position, glm::length(c.position - P)};
  c = hb.closestPoint(high, P);
  if (c.hit) return {ProjectionKind::Far, c.tri, c.bary, c.position, glm::length(c.position - P)};
  return best;
}

}  // namespace

bool projectOntoHigh(const Mesh& low, const Mesh& high, const Bvh& hb, const ProjectionOptions& opt, Projection& out,
                     std::string* err) {
  double t0 = nowSeconds();
  const int R = opt.resolution;
  if (R <= 0 || low.tris.empty() || low.faceCount() == 0) {
    if (err) *err = "The low-poly model has no faces";
    return false;
  }
  if (high.tris.empty() || hb.empty()) {
    if (err) *err = "The high-poly model has no faces";
    return false;
  }
  const std::vector<vec2> pat = texelPattern(opt.samples);
  const int S = (int)pat.size();
  float radius = std::max(low.boundingRadius, 1e-6f);
  float d = opt.distance;
  out.distanceMeasured = !(d > 0.f);
  if (out.distanceMeasured) {
    d = cageDistance(low, high, hb, err);
    if (d <= 0.f) return false;
  }
  const size_t N = size_t(R) * R;
  out.resolution = R, out.samples = S, out.distance = d;
  out.points = out.byRay = out.byNear = out.guessed = out.texels = 0;
  out.largestDistance = 0.f;
  out.look.assign(opt.lookups ? N * S * 2 : 0, 0xFFFF);
  out.highNormal.assign(N, vec4(0));
  out.highTangent.assign(opt.frames ? N : 0, vec4(0));
  out.hitPos.assign(opt.bakeData ? N : 0, vec4(0));
  out.hitMaterial.assign(opt.bakeData ? N : 0, 255);
  out.check.assign(N * 4, 0);

  const std::vector<vec3> rn = rayNormals(low);
  // the low's triangles by the texel rows they cross (mirror copies share the other half's texels)
  std::vector<std::vector<uint32_t>> rows(R);
  for (uint32_t t = 0; t < low.tris.size(); ++t) {
    uint32_t f = low.triFace[t];
    if (f < low.faceFlags.size() && (low.faceFlags[f] & FaceMirrorCopy)) continue;
    const auto& tri = low.tris[t];
    float y0 = 1e30f, y1 = -1e30f;
    for (int c = 0; c < 3; ++c) {
      float y = low.texUV(tri[c]).y * R;
      y0 = std::min(y0, y), y1 = std::max(y1, y);
    }
    int a = std::max(0, (int)std::floor(y0)), b = std::min(R - 1, (int)std::floor(y1));
    for (int y = a; y <= b; ++y) rows[y].push_back(t);
  }

  const float eps = 1e-4f * radius;
  std::atomic<long> points{0}, byRay{0}, byNear{0}, guessed{0}, texels{0}, flippedTexels{0};
  std::atomic<int> rowsDone{0};
  std::mutex mx;
  float largest = 0.f;
  std::atomic<bool> cancelled{false};
  constexpr uint32_t kNone = 0xFFFFFFFFu;
  parallelFor(R, [&](size_t yi) {
    if (cancelled) return;
    if (progress::cancelled()) {
      cancelled = true;
      return;
    }
    const int y = int(yi);
    thread_local std::vector<uint32_t> triOf;
    thread_local std::vector<vec2> baryOf;
    triOf.assign(size_t(R) * S, kNone);
    baryOf.resize(size_t(R) * S);
    int xLo = R, xHi = -1;
    for (uint32_t t : rows[y]) {
      const auto& tri = low.tris[t];
      vec2 a = low.texUV(tri[0]) * float(R), b = low.texUV(tri[1]) * float(R), c = low.texUV(tri[2]) * float(R);
      vec2 e1 = b - a, e2 = c - a;
      float det = e1.x * e2.y - e1.y * e2.x;
      if (std::abs(det) < 1e-12f) continue;
      float inv = 1.f / det;
      int x0 = std::max(0, (int)std::floor(std::min({a.x, b.x, c.x})));
      int x1 = std::min(R - 1, (int)std::floor(std::max({a.x, b.x, c.x})));
      xLo = std::min(xLo, x0), xHi = std::max(xHi, x1);
      constexpr float e = 1e-5f;
      for (int x = x0; x <= x1; ++x)
        for (int k = 0; k < S; ++k) {
          size_t slot = size_t(x) * S + k;
          if (triOf[slot] != kNone) continue;
          vec2 q = vec2(x + pat[k].x, y + pat[k].y) - a;
          float u = (q.x * e2.y - q.y * e2.x) * inv, v = (e1.x * q.y - e1.y * q.x) * inv;
          if (u < -e || v < -e || u + v > 1.f + e) continue;
          triOf[slot] = t;
          baryOf[slot] = {u, v};
        }
    }
    if (opt.coverPos) xLo = 0, xHi = R - 1;
    long nPts = 0, nRay = 0, nNear = 0, nFar = 0, nTex = 0, nFlip = 0;
    float rowLargest = 0.f;
    for (int x = xLo; x <= xHi; ++x) {
      size_t i = size_t(y) * R + x;
      bool any = false;
      for (int k = 0; k < S && !any; ++k) any = triOf[size_t(x) * S + k] != kNone;
      bool covered = opt.coverPos ? opt.coverPos[i * 4 + 3] >= 0.5f : any;
      if (!covered) continue;
      ++nTex;
      vec3 nAcc(0);
      int found = 0, nearPts = 0, farPts = 0, nk = 0;
      bool primaryFrame = false, primaryData = false;
      for (int k = 0; k < S; ++k) {
        size_t slot = size_t(x) * S + k;
        vec3 P, Nr;
        if (triOf[slot] != kNone) {
          const auto& tri = low.tris[triOf[slot]];
          vec2 b = baryOf[slot];
          float w0 = 1.f - b.x - b.y;
          P = low.positions[low.loopVert[tri[0]]] * w0 + low.positions[low.loopVert[tri[1]]] * b.x +
              low.positions[low.loopVert[tri[2]]] * b.y;
          Nr = rn[low.loopVert[tri[0]]] * w0 + rn[low.loopVert[tri[1]]] * b.x + rn[low.loopVert[tri[2]]] * b.y;
        } else if (!any && k == 0 && opt.coverPos && opt.coverNrm) {
          // a sliver: its texel is covered, but none of its points fell inside; its middle as the GPU saw it
          const float* p = opt.coverPos + i * 4;
          const float* n = opt.coverNrm + i * 4;
          P = vec3(p[0], p[1], p[2]), Nr = vec3(n[0], n[1], n[2]);
        } else {
          continue;
        }
        float ln = glm::length(Nr);
        if (!(ln > 1e-12f)) continue;
        Nr /= ln;
        ++nk;
        PointHit h = projectPoint(high, hb, P, Nr, d, eps);
        if (h.kind == ProjectionKind::None) continue;
        ++found;
        if (h.kind == ProjectionKind::Ray) ++nRay;
        else if (h.kind == ProjectionKind::Near) ++nNear, ++nearPts;
        else ++nFar, ++farPts;
        if (h.kind != ProjectionKind::Far) rowLargest = std::max(rowLargest, h.dist);
        const auto& ht = high.tris[h.tri];
        if (opt.lookups) {
          vec2 uv = high.texUV(ht[0]) * (1.f - h.bary.x - h.bary.y) + high.texUV(ht[1]) * h.bary.x +
                    high.texUV(ht[2]) * h.bary.y;
          size_t o = (size_t(k) * N + i) * 2;
          out.look[o] = (uint16_t)std::clamp(std::lround(uv.x * 65535.0), 0l, 65534l);
          out.look[o + 1] = (uint16_t)std::clamp(std::lround(uv.y * 65535.0), 0l, 65534l);
        }
        vec3 hn = interp(high.loopNormal, ht, h.bary);
        float hl = glm::length(hn);
        vec3 p0 = high.positions[high.loopVert[ht[0]]], p1 = high.positions[high.loopVert[ht[1]]],
             p2 = high.positions[high.loopVert[ht[2]]];
        vec3 fnrm = glm::cross(p1 - p0, p2 - p0);
        hn = hl > 1e-12f ? hn / hl : glm::normalize(fnrm + vec3(1e-30f));
        if (glm::dot(hn, Nr) < 0.f && glm::dot(fnrm, Nr) < 0.f) hn = -hn;  // a sculpt with its normals inverted
        // a guessed point moves paint (better than a hole) but says nothing of the high's shape there: its normal,
        // position and slot are left out of the bakes
        bool real = h.kind != ProjectionKind::Far;
        if (real) nAcc += hn;
        if (opt.frames && !primaryFrame) {  // the middle point (the pattern starts there), or the nearest to it found
          primaryFrame = true;
          vec3 tg(0);
          float sign = 1.f;
          if (high.loopTangent.size() == high.loopCount()) {
            float w[3] = {1.f - h.bary.x - h.bary.y, h.bary.x, h.bary.y};
            for (int c = 0; c < 3; ++c) tg += vec3(high.loopTangent[ht[c]]) * w[c];
            sign = high.loopTangent[ht[0]].w < 0.f ? -1.f : 1.f;
          }
          tg -= hn * glm::dot(hn, tg);
          float tl = glm::length(tg);
          out.highTangent[i] = vec4(tl > 1e-12f ? tg / tl : vec3(1, 0, 0), sign);
        }
        if (opt.bakeData && real && !primaryData) {
          primaryData = true;
          out.hitPos[i] = vec4(h.position, 1.f);
          uint32_t face = high.triFace[h.tri];
          out.hitMaterial[i] = face < high.faceMaterial.size() ? (uint8_t)std::min<uint32_t>(high.faceMaterial[face], 254) : 0;
        }
      }
      nPts += nk;
      float nl = glm::length(nAcc);
      uint8_t* ck = &out.check[i * 4];
      ck[0] = (uint8_t)std::lround(255.0 * nearPts / std::max(nk, 1));
      ck[1] = nk ? (uint8_t)std::lround(255.0 * (farPts + (nk - found)) / nk) : 255;
      // the low's own face (its shading normal) facing away from the high: turned inside out, or folded over a
      // hollow it bridges (a coarse shell over an eye socket). No normal of the high can look right there (it shades
      // dark): the low keeps its own, and the check shows the place
      bool flipped = false;
      if (opt.coverNrm && nl > 1e-12f) {
        const float* sn = opt.coverNrm + i * 4;
        flipped = glm::dot(vec3(sn[0], sn[1], sn[2]), nAcc) < 0.f;
      }
      if (nl > 1e-12f && !flipped) out.highNormal[i] = vec4(nAcc / nl, 1.f);
      ck[2] = flipped ? 255 : 0, ck[3] = 255;
      nFlip += flipped;
    }
    points += nPts, byRay += nRay, byNear += nNear, guessed += nFar, texels += nTex, flippedTexels += nFlip;
    if (rowLargest > 0.f) {
      std::lock_guard<std::mutex> lk(mx);
      largest = std::max(largest, rowLargest);
    }
    int done = ++rowsDone;
    if ((done & 31) == 0) progress::set(float(done) / float(R));
  }, 64);
  if (cancelled) {
    if (err) *err = "cancelled";
    return false;
  }
  out.points = points, out.byRay = byRay, out.byNear = byNear, out.guessed = guessed, out.texels = texels;
  out.flipped = flippedTexels;
  out.largestDistance = largest;
  out.seconds = nowSeconds() - t0;
  return true;
}

}  // namespace patina
