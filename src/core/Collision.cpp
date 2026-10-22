#include "core/Collision.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <queue>
#include <unordered_map>
#include <unordered_set>

#include "core/Log.h"
#include "core/Mesh.h"
#include "core/MeshParts.h"

namespace patina {

namespace {

using dvec3 = glm::dvec3;
using glm::ivec3;

// Evenly spread directions (a Fibonacci sphere).
std::vector<vec3> directions(int n) {
  std::vector<vec3> d(n);
  const float ga = 2.39996323f;  // the golden angle
  for (int i = 0; i < n; ++i) {
    float z = 1.f - 2.f * (i + 0.5f) / n, r = std::sqrt(std::max(0.f, 1.f - z * z));
    d[i] = vec3(r * std::cos(ga * i), r * std::sin(ga * i), z);
  }
  return d;
}

// The points furthest out along `dirs` (each once).
std::vector<vec3> extremes(const std::vector<vec3>& pts, const std::vector<vec3>& dirs) {
  std::vector<uint32_t> best(dirs.size(), 0);
  std::vector<float> bestD(dirs.size(), -1e30f);
  for (uint32_t i = 0; i < pts.size(); ++i)
    for (size_t k = 0; k < dirs.size(); ++k) {
      float d = glm::dot(pts[i], dirs[k]);
      if (d > bestD[k]) bestD[k] = d, best[k] = i;
    }
  std::sort(best.begin(), best.end());
  best.erase(std::unique(best.begin(), best.end()), best.end());
  std::vector<vec3> out;
  out.reserve(best.size());
  for (uint32_t i : best) out.push_back(pts[i]);
  return out;
}

// Symmetric 3x3 eigenvectors (Jacobi), as the columns of the result.
glm::dmat3 eigenvectors(glm::dmat3 a) {
  glm::dmat3 v(1.0);
  for (int sweep = 0; sweep < 32; ++sweep) {
    double off = a[0][1] * a[0][1] + a[0][2] * a[0][2] + a[1][2] * a[1][2];
    if (off < 1e-20) break;
    for (int p = 0; p < 2; ++p)
      for (int q = p + 1; q < 3; ++q) {
        if (std::abs(a[p][q]) < 1e-30) continue;
        double theta = (a[q][q] - a[p][p]) / (2.0 * a[p][q]);
        double t = (theta >= 0 ? 1.0 : -1.0) / (std::abs(theta) + std::sqrt(theta * theta + 1.0));
        double c = 1.0 / std::sqrt(t * t + 1.0), s = t * c;
        glm::dmat3 j(1.0);  // (glm is column-major: j[col][row]) J(p,q) = s, J(q,p) = -s
        j[p][p] = c, j[q][q] = c, j[q][p] = s, j[p][q] = -s;
        a = glm::transpose(j) * a * j;
        v = v * j;
      }
  }
  return v;
}

// Incremental hull of a modest number of points in general position (doubles).
bool hullOf(const std::vector<vec3>& in, ConvexShape& out) {
  size_t n = in.size();
  if (n < 4) return false;
  std::vector<dvec3> p(n);
  dvec3 lo(1e300), hi(-1e300);
  for (size_t i = 0; i < n; ++i) p[i] = dvec3(in[i]), lo = glm::min(lo, p[i]), hi = glm::max(hi, p[i]);
  double size = glm::length(hi - lo);
  if (size <= 0) return false;
  double eps = 1e-9 * size;
  // a first tetrahedron from extreme points
  size_t i0 = 0, i1 = 0, i2 = 0, i3 = 0;
  for (size_t i = 1; i < n; ++i)
    if (p[i].x < p[i0].x) i0 = i;
  double best = -1;
  for (size_t i = 0; i < n; ++i)
    if (double d = glm::length(p[i] - p[i0]); d > best) best = d, i1 = i;
  best = -1;
  for (size_t i = 0; i < n; ++i)
    if (double d = glm::length(glm::cross(p[i] - p[i0], p[i1] - p[i0])); d > best) best = d, i2 = i;
  if (best <= eps * size) return false;
  dvec3 nrm = glm::normalize(glm::cross(p[i1] - p[i0], p[i2] - p[i0]));
  best = -1;
  for (size_t i = 0; i < n; ++i)
    if (double d = std::abs(glm::dot(p[i] - p[i0], nrm)); d > best) best = d, i3 = i;
  if (best <= eps * 10) return false;
  dvec3 center = (p[i0] + p[i1] + p[i2] + p[i3]) * 0.25;

  struct Face {
    uint32_t a, b, c;
    dvec3 n;
    double d;
    bool alive;
  };
  std::vector<Face> faces;
  auto addFace = [&](uint32_t a, uint32_t b, uint32_t c) {
    dvec3 nn = glm::cross(p[b] - p[a], p[c] - p[a]);
    double l = glm::length(nn);
    nn = l > 0 ? nn / l : dvec3(0);
    Face f{a, b, c, nn, glm::dot(nn, p[a]), true};
    if (glm::dot(f.n, center) - f.d > 0) {  // inwards: turn it round
      std::swap(f.b, f.c);
      f.n = -f.n;
      f.d = -f.d;
    }
    faces.push_back(f);
  };
  uint32_t t[4] = {(uint32_t)i0, (uint32_t)i1, (uint32_t)i2, (uint32_t)i3};
  addFace(t[0], t[1], t[2]);
  addFace(t[0], t[1], t[3]);
  addFace(t[0], t[2], t[3]);
  addFace(t[1], t[2], t[3]);
  // the other points, furthest first
  std::vector<uint32_t> order;
  for (uint32_t i = 0; i < n; ++i)
    if (i != i0 && i != i1 && i != i2 && i != i3) order.push_back(i);
  std::sort(order.begin(), order.end(),
            [&](uint32_t a, uint32_t b) { return glm::length(p[a] - center) > glm::length(p[b] - center); });
  std::map<std::pair<uint32_t, uint32_t>, int> edges;
  for (uint32_t pi : order) {
    std::vector<size_t> vis;
    for (size_t f = 0; f < faces.size(); ++f)
      if (faces[f].alive && glm::dot(faces[f].n, p[pi]) - faces[f].d > eps * 10) vis.push_back(f);
    if (vis.empty()) continue;
    edges.clear();
    for (size_t f : vis) {
      const Face& F = faces[f];
      edges[{F.a, F.b}]++, edges[{F.b, F.c}]++, edges[{F.c, F.a}]++;
    }
    std::vector<std::pair<uint32_t, uint32_t>> horizon;
    for (auto& [e, cnt] : edges)
      if (!edges.count({e.second, e.first})) horizon.push_back(e);
    for (size_t f : vis) faces[f].alive = false;
    for (auto& [u, v] : horizon) {
      dvec3 nn = glm::cross(p[v] - p[u], p[pi] - p[u]);
      double l = glm::length(nn);
      nn = l > 0 ? nn / l : dvec3(0);
      faces.push_back({u, v, pi, nn, glm::dot(nn, p[u]), true});
    }
    // keep the list short: drop the dead faces now and then
    if (faces.size() > 4096) faces.erase(std::remove_if(faces.begin(), faces.end(), [](const Face& f) { return !f.alive; }), faces.end());
  }
  out.verts.clear();
  out.tris.clear();
  std::unordered_map<uint32_t, uint32_t> remap;
  auto idx = [&](uint32_t i) {
    auto [it, fresh] = remap.emplace(i, (uint32_t)out.verts.size());
    if (fresh) out.verts.push_back(in[i]);
    return it->second;
  };
  for (const Face& f : faces)
    if (f.alive) out.tris.push_back({idx(f.a), idx(f.b), idx(f.c)});
  return out.tris.size() >= 4;
}

// Flat or straight point sets: a thin slab around them, so they still make a closed shape.
std::vector<vec3> thicken(const std::vector<vec3>& pts) {
  vec3 lo(1e30f), hi(-1e30f), c(0.f);
  for (auto& p : pts) lo = glm::min(lo, p), hi = glm::max(hi, p), c += p;
  c /= float(pts.size());
  float size = std::max(glm::length(hi - lo), 1e-6f);
  glm::dmat3 cov(0.0);
  for (auto& p : pts) {
    dvec3 d = dvec3(p - c);
    cov += glm::outerProduct(d, d);
  }
  glm::dmat3 ev = eigenvectors(cov);
  // the axes along which the set is thin
  std::vector<vec3> out = pts;
  float t = std::max(size * 2e-3f, 1e-4f);
  for (int k = 0; k < 3; ++k) {
    vec3 ax = glm::normalize(vec3(ev[k]));
    float lo2 = 1e30f, hi2 = -1e30f;
    for (auto& p : pts) {
      float d = glm::dot(p - c, ax);
      lo2 = std::min(lo2, d), hi2 = std::max(hi2, d);
    }
    if (hi2 - lo2 < t) {
      size_t m = out.size();
      for (size_t i = 0; i < m; ++i) out[i] -= ax * (t * 0.5f), out.push_back(out[i] + ax * t);
    }
  }
  return out;
}

void boxMesh(const vec3& c, const vec3 ax[3], const vec3& half, ConvexShape& s) {
  s.verts.clear();
  for (int i = 0; i < 8; ++i)
    s.verts.push_back(c + ax[0] * (i & 1 ? half.x : -half.x) + ax[1] * (i & 2 ? half.y : -half.y) +
                      ax[2] * (i & 4 ? half.z : -half.z));
  static const uint32_t q[6][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
  s.tris.clear();
  for (auto& f : q) s.tris.push_back({f[0], f[1], f[2]}), s.tris.push_back({f[0], f[2], f[3]});
  // outward for a right-handed frame; a left-handed one turns every face round
  if (glm::dot(glm::cross(ax[0], ax[1]), ax[2]) < 0)
    for (auto& t : s.tris) std::swap(t.y, t.z);
  s.box = true;
}

}  // namespace

float ConvexShape::volume() const {
  double v = 0;
  for (auto& t : tris) v += glm::dot(dvec3(verts[t.x]), glm::cross(dvec3(verts[t.y]), dvec3(verts[t.z])));
  return float(std::abs(v) / 6.0);
}

bool convexHull(const std::vector<vec3>& ptsIn, ConvexShape& out, int maxVerts) {
  out = ConvexShape();
  if (ptsIn.size() < 2) return false;
  std::vector<vec3> pts = ptsIn;
  maxVerts = std::clamp(maxVerts, 8, 255);
  if ((int)pts.size() > maxVerts) pts = extremes(pts, directions(maxVerts));
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (hullOf(pts, out)) {
      // a hull can still have more corners than the points asked for (it never does here), or fewer: fine
      return true;
    }
    pts = thicken(pts);  // flat or straight: give it a little thickness
  }
  // two distinct points at best: a thin box along them
  out = orientedBox(ptsIn);
  out.box = false;
  return !out.tris.empty();
}

ConvexShape orientedBox(const std::vector<vec3>& ptsIn) {
  ConvexShape s;
  if (ptsIn.empty()) return s;
  std::vector<vec3> pts = ptsIn.size() > 64 ? extremes(ptsIn, directions(64)) : ptsIn;
  vec3 c(0.f);
  for (auto& p : pts) c += p;
  c /= float(pts.size());
  glm::dmat3 cov(0.0);
  for (auto& p : pts) {
    dvec3 d = dvec3(p - c);
    cov += glm::outerProduct(d, d);
  }
  glm::dmat3 ev = eigenvectors(cov);
  float bestVol = 1e30f;
  vec3 bestAx[3], bestC, bestHalf;
  auto tryFrame = [&](const vec3 ax[3]) {
    vec3 lo(1e30f), hi(-1e30f);
    for (auto& p : ptsIn)
      for (int k = 0; k < 3; ++k) lo[k] = std::min(lo[k], glm::dot(p, ax[k])), hi[k] = std::max(hi[k], glm::dot(p, ax[k]));
    vec3 ext = glm::max(hi - lo, vec3(1e-4f));
    float vol = ext.x * ext.y * ext.z;
    if (vol < bestVol) {
      bestVol = vol;
      for (int k = 0; k < 3; ++k) bestAx[k] = ax[k];
      vec3 mid = (lo + hi) * 0.5f;
      bestC = ax[0] * mid.x + ax[1] * mid.y + ax[2] * mid.z;
      bestHalf = ext * 0.5f;
    }
  };
  vec3 axisAligned[3] = {vec3(1, 0, 0), vec3(0, 1, 0), vec3(0, 0, 1)};
  tryFrame(axisAligned);
  float upright = bestVol;
  vec3 pca[3] = {glm::normalize(vec3(ev[0])), glm::normalize(vec3(ev[1])), glm::normalize(vec3(ev[2]))};
  tryFrame(pca);
  // turned about each principal axis: catches boxes PCA alone gets wrong (near-cubes)
  for (int k = 0; k < 3; ++k)
    for (int a = 1; a < 12; ++a) {
      float ang = a * 3.14159265f / 24.f;
      glm::mat3 r = glm::mat3(glm::rotate(glm::mat4(1.f), ang, pca[k]));
      vec3 ax[3] = {r * pca[0], r * pca[1], r * pca[2]};
      tryFrame(ax);
    }
  // an upright box unless a turned one is clearly smaller: it is what people expect, and what lines up
  if (upright <= bestVol * 1.1f) {
    bestVol = 1e30f;
    tryFrame(axisAligned);
  }
  boxMesh(bestC, bestAx, bestHalf, s);
  return s;
}

CollisionResult makeCollision(const Mesh& m, const CollisionOptions& opt) {
  CollisionResult res;
  if (m.positions.empty()) return res;
  int maxVerts = std::clamp(opt.maxVerts, 8, 255);
  int maxShapes = std::clamp(opt.maxShapes, 1, 256);
  // the model's volume when it is closed (the divergence theorem over its triangles)
  {
    double v = 0;
    for (auto& t : m.tris)
      v += glm::dot(dvec3(m.positions[m.loopVert[t.x]]),
                    glm::cross(dvec3(m.positions[m.loopVert[t.y]]), dvec3(m.positions[m.loopVert[t.z]])));
    res.meshVolume = float(std::abs(v) / 6.0);
  }
  const std::string& mode = opt.mode;

  // separate parts (largest first), small ones joined to the nearest kept one when there are too many
  auto partPoints = [&]() {
    uint32_t count = 0;
    auto piece = parts::facePieces(m, &count);
    std::vector<std::vector<vec3>> pts(count);
    for (uint32_t f = 0; f < m.faceCount(); ++f)
      for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) pts[piece[f]].push_back(m.positions[m.loopVert[l]]);
    if ((int)pts.size() > maxShapes) {
      std::vector<vec3> center(pts.size());
      for (size_t i = 0; i < pts.size(); ++i) {
        vec3 lo(1e30f), hi(-1e30f);
        for (auto& p : pts[i]) lo = glm::min(lo, p), hi = glm::max(hi, p);
        center[i] = (lo + hi) * 0.5f;
      }
      for (size_t i = maxShapes; i < pts.size(); ++i) {
        size_t best = 0;
        float bd = 1e30f;
        for (int k = 0; k < maxShapes; ++k)
          if (float d = glm::length(center[i] - center[k]); d < bd) bd = d, best = k;
        pts[best].insert(pts[best].end(), pts[i].begin(), pts[i].end());
      }
      pts.resize(maxShapes);
    }
    return pts;
  };

  if (mode == "box" || mode == "hull") {
    std::vector<vec3> all(m.positions.begin(), m.positions.end());
    ConvexShape s;
    if (mode == "box") s = orientedBox(all);
    else convexHull(all, s, maxVerts);
    if (!s.tris.empty()) res.shapes.push_back(std::move(s));
  } else if (mode == "boxes" || mode == "parts") {
    for (auto& pts : partPoints()) {
      ConvexShape s;
      if (mode == "boxes") s = orientedBox(pts);
      else convexHull(pts, s, maxVerts);
      if (!s.tris.empty()) res.shapes.push_back(std::move(s));
    }
  } else {
    // ---- pieces: the model's voxels (its surface, and what the surface closes in), split into convex pieces
    vec3 lo = m.bounds.lo, hi = m.bounds.hi;
    vec3 ext = hi - lo;
    float longest = std::max({ext.x, ext.y, ext.z, 1e-6f});
    int R = std::clamp(opt.resolution, 8, 96);
    float h = longest / R;
    ivec3 dim = glm::max(ivec3(glm::ceil(ext / h)) + 2, ivec3(3));
    vec3 origin = lo - vec3(h);  // one empty voxel all round
    auto vid = [&](int x, int y, int z) { return (size_t(z) * dim.y + y) * dim.x + x; };
    size_t N = size_t(dim.x) * dim.y * dim.z;
    std::vector<uint8_t> surf(N, 0);
    // per surface voxel, the samples furthest out along 14 directions (axes and diagonals), for the hulls
    static const vec3 d14[14] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}, {1, 1, 1},
                                 {-1, -1, -1}, {1, 1, -1}, {-1, -1, 1}, {1, -1, 1}, {-1, 1, -1}, {-1, 1, 1}, {1, -1, -1}};
    std::unordered_map<size_t, std::array<vec3, 14>> ext14;
    auto mark = [&](const vec3& p) {
      ivec3 c = glm::clamp(ivec3(glm::floor((p - origin) / h)), ivec3(0), dim - 1);
      size_t i = vid(c.x, c.y, c.z);
      if (!surf[i]) {
        surf[i] = 1;
        std::array<vec3, 14> a;
        a.fill(p);
        ext14.emplace(i, a);
        return;
      }
      auto& a = ext14[i];
      for (int k = 0; k < 14; ++k)
        if (glm::dot(p, d14[k]) > glm::dot(a[k], d14[k])) a[k] = p;
    };
    for (auto& t : m.tris) {
      vec3 a = m.positions[m.loopVert[t.x]], b = m.positions[m.loopVert[t.y]], c = m.positions[m.loopVert[t.z]];
      float e = std::max({glm::length(b - a), glm::length(c - b), glm::length(a - c)});
      int n = std::clamp(int(std::ceil(e / (h * 0.5f))), 1, 256);
      for (int i = 0; i <= n; ++i)
        for (int j = 0; j <= n - i; ++j) {
          float u = float(i) / n, v = float(j) / n;
          mark(a + (b - a) * u + (c - a) * v);
        }
    }
    // inside: what cannot be reached from outside without crossing the surface
    std::vector<uint8_t> outside(N, 0);
    {
      std::vector<size_t> stack = {vid(0, 0, 0)};
      outside[stack[0]] = 1;
      while (!stack.empty()) {
        size_t i = stack.back();
        stack.pop_back();
        int x = int(i % dim.x), y = int((i / dim.x) % dim.y), z = int(i / (size_t(dim.x) * dim.y));
        const int nb[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        for (auto& o : nb) {
          int X = x + o[0], Y = y + o[1], Z = z + o[2];
          if (X < 0 || Y < 0 || Z < 0 || X >= dim.x || Y >= dim.y || Z >= dim.z) continue;
          size_t j = vid(X, Y, Z);
          if (outside[j] || surf[j]) continue;
          outside[j] = 1;
          stack.push_back(j);
        }
      }
    }
    std::vector<uint8_t> solid(N, 0);
    size_t inner = 0, solidCount = 0;
    for (size_t i = 0; i < N; ++i) {
      solid[i] = surf[i] || !outside[i];
      inner += !surf[i] && !outside[i];
      solidCount += solid[i];
    }
    if (inner == 0) res.note = "the model is open or thin, so the pieces follow its surface";
    float voxVol = h * h * h;
    if (res.meshVolume <= 0.f || inner == 0) res.meshVolume = solidCount * voxVol * 0.75f;  // (a rough guess)

    // a region's hull points: surface extremes (coarse: one point per surface voxel, for searching), and the corners
    // of its inner voxels on its border
    auto regionPoints = [&](const std::vector<size_t>& reg, const std::vector<int32_t>& owner, int32_t me,
                            bool coarse = false) {
      std::vector<vec3> pts;
      for (size_t i : reg) {
        if (surf[i]) {
          const auto& a = ext14[i];
          if (coarse) pts.push_back((a[0] + a[1] + a[2] + a[3] + a[4] + a[5]) / 6.f);
          else pts.insert(pts.end(), a.begin(), a.end());
          continue;
        }
        int x = int(i % dim.x), y = int((i / dim.x) % dim.y), z = int(i / (size_t(dim.x) * dim.y));
        bool border = false;
        const int nb[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        for (auto& o : nb) {
          int X = x + o[0], Y = y + o[1], Z = z + o[2];
          if (X < 0 || Y < 0 || Z < 0 || X >= dim.x || Y >= dim.y || Z >= dim.z || owner[vid(X, Y, Z)] != me) border = true;
        }
        if (!border) continue;
        vec3 p0 = origin + vec3(x, y, z) * h;
        for (int k = 0; k < 8; ++k) pts.push_back(p0 + vec3(k & 1, (k >> 1) & 1, (k >> 2) & 1) * h);
      }
      return pts;
    };
    std::vector<int32_t> owner(N, -1);
    // first regions: the connected lumps of solid voxels
    std::vector<std::vector<size_t>> regions;
    for (size_t s = 0; s < N; ++s) {
      if (!solid[s] || owner[s] >= 0) continue;
      int32_t id = (int32_t)regions.size();
      regions.emplace_back();
      std::vector<size_t> stack = {s};
      owner[s] = id;
      while (!stack.empty()) {
        size_t i = stack.back();
        stack.pop_back();
        regions[id].push_back(i);
        int x = int(i % dim.x), y = int((i / dim.x) % dim.y), z = int(i / (size_t(dim.x) * dim.y));
        for (int dz = -1; dz <= 1; ++dz)
          for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
              int X = x + dx, Y = y + dy, Z = z + dz;
              if (X < 0 || Y < 0 || Z < 0 || X >= dim.x || Y >= dim.y || Z >= dim.z) continue;
              size_t j = vid(X, Y, Z);
              if (!solid[j] || owner[j] >= 0) continue;
              owner[j] = id;
              stack.push_back(j);
            }
      }
    }
    // too many lumps: the small ones join the nearest big one
    if ((int)regions.size() > maxShapes) {
      std::sort(regions.begin(), regions.end(), [](auto& a, auto& b) { return a.size() > b.size(); });
      std::vector<vec3> center(regions.size());
      for (size_t r = 0; r < regions.size(); ++r) {
        vec3 c(0.f);
        for (size_t i : regions[r]) c += vec3(float(i % dim.x), float((i / dim.x) % dim.y), float(i / (size_t(dim.x) * dim.y)));
        center[r] = c / float(regions[r].size());
      }
      for (size_t r = maxShapes; r < regions.size(); ++r) {
        size_t best = 0;
        float bd = 1e30f;
        for (int k = 0; k < maxShapes; ++k)
          if (float d = glm::length(center[r] - center[k]); d < bd) bd = d, best = k;
        regions[best].insert(regions[best].end(), regions[r].begin(), regions[r].end());
      }
      regions.resize(maxShapes);
      for (size_t r = 0; r < regions.size(); ++r)
        for (size_t i : regions[r]) owner[i] = (int32_t)r;
    }
    // the separate lumps of a region (a cut can leave two)
    std::vector<uint32_t> stamp(N, 0);
    uint32_t stampNow = 0;
    auto lumps = [&](const std::vector<size_t>& reg, int32_t me) {
      std::vector<std::vector<size_t>> out;
      ++stampNow;
      for (size_t s0 : reg) {
        if (stamp[s0] == stampNow) continue;
        out.emplace_back();
        std::vector<size_t> stack = {s0};
        stamp[s0] = stampNow;
        while (!stack.empty()) {
          size_t i = stack.back();
          stack.pop_back();
          out.back().push_back(i);
          int x = int(i % dim.x), y = int((i / dim.x) % dim.y), z = int(i / (size_t(dim.x) * dim.y));
          for (int dz = -1; dz <= 1; ++dz)
            for (int dy = -1; dy <= 1; ++dy)
              for (int dx = -1; dx <= 1; ++dx) {
                int X = x + dx, Y = y + dy, Z = z + dz;
                if (X < 0 || Y < 0 || Z < 0 || X >= dim.x || Y >= dim.y || Z >= dim.z) continue;
                size_t j = vid(X, Y, Z);
                if (owner[j] != me || stamp[j] == stampNow) continue;
                stamp[j] = stampNow;
                stack.push_back(j);
              }
        }
      }
      return out;
    };
    // the empty space in a region's hulls (one hull per lump), quickly: coarse points, 128 directions (fewer
    // underestimate the hull of a ring badly); surface voxels count half (on average half of one is inside)
    auto measure = [&](const std::vector<size_t>& reg, int32_t me, float& hullVol) {
      hullVol = 0.f;
      for (auto& lump : lumps(reg, me)) {
        ConvexShape s;
        if (convexHull(regionPoints(lump, owner, me, true), s, 128)) hullVol += s.volume();
      }
      float vol = 0.f;
      for (size_t i : reg) vol += surf[i] ? 0.5f : 1.f;
      return std::max(0.f, hullVol - vol * voxVol);
    };
    struct Item {
      float err;
      int32_t id;
      bool operator<(const Item& o) const { return err < o.err; }
    };
    std::priority_queue<Item> queue;
    for (size_t r = 0; r < regions.size(); ++r) {
      float hv;
      float e = measure(regions[r], (int32_t)r, hv);
      if (hv > 0 && e / hv > opt.concavity && regions[r].size() > 8) queue.push({e, (int32_t)r});
    }
    while (!queue.empty() && (int)regions.size() < maxShapes) {
      Item it = queue.top();
      queue.pop();
      auto& reg = regions[it.id];
      ivec3 rlo(1 << 30), rhi(-1);
      for (size_t i : reg) {
        ivec3 c(int(i % dim.x), int((i / dim.x) % dim.y), int(i / (size_t(dim.x) * dim.y)));
        rlo = glm::min(rlo, c), rhi = glm::max(rhi, c);
      }
      // the cut that leaves the least empty space in the two hulls. A very hollow piece may take a cut that only
      // keeps the empty space as it was: a ring's halves still cover its hole, its quarters do not.
      float hullNow;
      measure(reg, it.id, hullNow);
      bool hollow = hullNow > 0 && it.err / hullNow > 0.2f;
      float bestCost = it.err * (hollow ? 1.02f : 0.9f);
      int bestAxis = -1, bestAt = 0;
      int32_t tmp = (int32_t)regions.size();
      for (int axis = 0; axis < 3; ++axis) {
        int span = rhi[axis] - rlo[axis];
        if (span < 2) continue;
        int tries = std::min(span, 16);
        for (int q = 1; q <= tries; ++q) {
          int at = rlo[axis] + std::max(1, (span + 1) * q / (tries + 1));
          if (at <= rlo[axis] || at > rhi[axis]) continue;
          std::vector<size_t> a, b;
          for (size_t i : reg) {
            ivec3 c(int(i % dim.x), int((i / dim.x) % dim.y), int(i / (size_t(dim.x) * dim.y)));
            (c[axis] < at ? a : b).push_back(i);
          }
          if (a.empty() || b.empty()) continue;
          for (size_t i : b) owner[i] = tmp;
          float ha, hb;
          float cost = measure(a, it.id, ha) + measure(b, tmp, hb);
          for (size_t i : b) owner[i] = it.id;
          if (cost < bestCost) bestCost = cost, bestAxis = axis, bestAt = at;
        }
      }
      if (bestAxis < 0) continue;  // no cut helps: it stays as it is
      std::vector<size_t> a, b;
      for (size_t i : reg) {
        ivec3 c(int(i % dim.x), int((i / dim.x) % dim.y), int(i / (size_t(dim.x) * dim.y)));
        (c[bestAxis] < bestAt ? a : b).push_back(i);
      }
      int32_t nb = (int32_t)regions.size();
      regions[it.id] = std::move(a);
      regions.push_back(std::move(b));
      for (size_t i : regions[nb]) owner[i] = nb;
      // each half's separate lumps become pieces of their own (while there is room)
      std::vector<int32_t> fresh = {it.id, nb};
      for (int32_t r : {it.id, nb}) {
        auto parts = lumps(regions[r], r);
        if (parts.size() < 2 || (int)(regions.size() + parts.size() - 1) > maxShapes) continue;
        regions[r] = std::move(parts[0]);
        for (size_t k = 1; k < parts.size(); ++k) {
          int32_t id = (int32_t)regions.size();
          for (size_t i : parts[k]) owner[i] = id;
          regions.push_back(std::move(parts[k]));
          fresh.push_back(id);
        }
      }
      for (int32_t r : fresh) {
        float hv;
        float e = measure(regions[r], r, hv);
        if (hv > 0 && e / hv > opt.concavity && regions[r].size() > 8) queue.push({e, r});
      }
    }
    for (size_t r = 0; r < regions.size(); ++r) {
      ConvexShape s;
      if (convexHull(regionPoints(regions[r], owner, (int32_t)r), s, maxVerts)) res.shapes.push_back(std::move(s));
    }
  }
  for (auto& s : res.shapes) res.shapesVolume += s.volume();
  return res;
}

std::vector<vec3> collisionEdges(const std::vector<ConvexShape>& shapes) {
  std::vector<vec3> out;
  for (auto& s : shapes) {
    // edges between faces that are not in one plane (no diagonals across a box side)
    std::map<std::pair<uint32_t, uint32_t>, std::vector<vec3>> edgeNormals;
    for (auto& t : s.tris) {
      vec3 n = glm::cross(s.verts[t.y] - s.verts[t.x], s.verts[t.z] - s.verts[t.x]);
      float l = glm::length(n);
      n = l > 0 ? n / l : vec3(0.f);
      uint32_t v[3] = {t.x, t.y, t.z};
      for (int k = 0; k < 3; ++k) {
        uint32_t a = v[k], b = v[(k + 1) % 3];
        edgeNormals[{std::min(a, b), std::max(a, b)}].push_back(n);
      }
    }
    for (auto& [e, ns] : edgeNormals) {
      if (ns.size() == 2 && glm::dot(ns[0], ns[1]) > 0.9995f) continue;
      out.push_back(s.verts[e.first]);
      out.push_back(s.verts[e.second]);
    }
  }
  return out;
}

}  // namespace patina
