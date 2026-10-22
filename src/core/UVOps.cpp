#include "core/UVOps.h"

#include <algorithm>
#include <chrono>
#include <array>
#include <cmath>
#include <numeric>
#include <unordered_set>

#include "core/Log.h"
#include "core/UVIsland.h"

namespace patina::uv {

namespace {

using glm::dvec2;
using glm::dvec3;

constexpr uint32_t kNone = 0xffffffffu;

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

bool finite2(const vec2& v) { return std::isfinite(v.x) && std::isfinite(v.y); }

bool uvEq(const vec2& a, const vec2& b) {
  // same tolerance as Mesh::computeUVTopology
  return std::abs(a.x - b.x) < 1e-5f && std::abs(a.y - b.y) < 1e-5f;
}

// Sorted, unique, in-range face indices; empty input = all faces.
std::vector<uint32_t> selectFaces(const Mesh& m, const std::vector<uint32_t>& faces) {
  uint32_t F = m.faceCount();
  std::vector<uint32_t> out;
  if (faces.empty()) {
    out.resize(F);
    std::iota(out.begin(), out.end(), 0u);
    return out;
  }
  out.reserve(faces.size());
  for (uint32_t f : faces)
    if (f < F) out.push_back(f);
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

bool edgesValid(const Mesh& m) {
  if (m.loopEdge.size() != m.loopCount() || m.edgeFlags.size() != m.edges.size()) return false;
  for (uint32_t e : m.loopEdge)
    if (e >= m.edges.size()) return false;
  return true;
}

// Rebuilds derived topology the algorithms rely on when it is missing/stale.
// Neither call touches UVs.
void ensureTopology(Mesh& m) {
  if (!edgesValid(m)) m.buildEdges();
  if (m.triFace.size() != m.tris.size() || (m.tris.empty() && m.faceCount() > 0)) m.triangulate();
}

bool isSeam(const Mesh& m, uint32_t e) { return (m.edgeFlags[e] & EdgeSeam) != 0; }

// edge -> loops using it (CSR). Empty when the mesh has no valid edge data.
struct EdgeLoops {
  std::vector<uint32_t> start, loops;
  explicit EdgeLoops(const Mesh& m) {
    if (!edgesValid(m)) {
      start.assign(1, 0);
      return;
    }
    uint32_t L = m.loopCount();
    start.assign(m.edges.size() + 1, 0);
    loops.resize(L);
    for (uint32_t l = 0; l < L; ++l) start[m.loopEdge[l] + 1]++;
    for (size_t i = 1; i < start.size(); ++i) start[i] += start[i - 1];
    std::vector<uint32_t> fill(start.begin(), start.end() - 1);
    for (uint32_t l = 0; l < L; ++l) loops[fill[m.loopEdge[l]]++] = l;
  }
  uint32_t edgeCount() const { return uint32_t(start.size() - 1); }
};

// Triangles (loop indices) of each face, CSR. Uses mesh.tris when consistent,
// otherwise fans every face.
struct FaceTris {
  std::vector<uint32_t> start;
  std::vector<glm::uvec3> tris;
  explicit FaceTris(const Mesh& m) {
    uint32_t F = m.faceCount(), L = m.loopCount();
    bool ok = m.triFace.size() == m.tris.size() && !(m.tris.empty() && F > 0);
    for (size_t t = 0; ok && t < m.tris.size(); ++t)
      ok = m.triFace[t] < F && m.tris[t].x < L && m.tris[t].y < L && m.tris[t].z < L;
    start.assign(F + 1, 0);
    if (ok) {
      for (uint32_t f : m.triFace) start[f + 1]++;
      for (size_t i = 1; i < start.size(); ++i) start[i] += start[i - 1];
      tris.resize(m.tris.size());
      std::vector<uint32_t> fill(start.begin(), start.end() - 1);
      for (size_t t = 0; t < m.tris.size(); ++t) tris[fill[m.triFace[t]]++] = m.tris[t];
    } else {
      for (uint32_t f = 0; f < F; ++f) {
        uint32_t s = m.faceStart[f];
        for (uint32_t l = s + 1; l + 1 < m.faceStart[f + 1]; ++l) tris.push_back({s, l, l + 1});
        start[f + 1] = (uint32_t)tris.size();
      }
    }
  }
};

// Given loops a0 (with next a1) and b0 (with next b1) on the same edge, returns
// in c0/c1 the loops of b sitting on a0's / a1's vertex. False if degenerate.
bool matchSides(const Mesh& m, uint32_t a0, uint32_t a1, uint32_t b0, uint32_t b1, uint32_t& c0, uint32_t& c1) {
  uint32_t va0 = m.loopVert[a0], va1 = m.loopVert[a1];
  if (va0 == va1) return false;
  if (m.loopVert[b0] == va1 && m.loopVert[b1] == va0) {  // opposite winding (usual)
    c0 = b1;
    c1 = b0;
    return true;
  }
  if (m.loopVert[b0] == va0 && m.loopVert[b1] == va1) {  // flipped neighbor
    c0 = b0;
    c1 = b1;
    return true;
  }
  return false;
}

// Groups `sel` by union-find roots, preserving the order of first appearance.
std::vector<std::vector<uint32_t>> groupByRoot(const std::vector<uint32_t>& sel, UnionFind& uf) {
  std::vector<std::vector<uint32_t>> out;
  std::vector<uint32_t> rootId(uf.p.size(), kNone);
  for (uint32_t f : sel) {
    uint32_t r = uf.find(f);
    if (rootId[r] == kNone) {
      rootId[r] = (uint32_t)out.size();
      out.emplace_back();
    }
    out[rootId[r]].push_back(f);
  }
  return out;
}

// Orthonormal (u, v) spanning the plane perpendicular to n with u x v = n.
// For n = +Z this gives u = +X, v = +Y; for +-Y the forward axis maps to v.
void planeAxes(vec3 n, vec3& u, vec3& v) {
  float len = glm::length(n);
  n = (len > 1e-20f && std::isfinite(len)) ? n / len : vec3(0, 0, 1);
  vec3 hint = std::abs(n.y) > 0.999f ? vec3(0, 0, n.y > 0 ? -1.f : 1.f) : vec3(0, 1, 0);
  u = glm::normalize(glm::cross(hint, n));
  v = glm::cross(n, u);
}

// Area-weighted average normal of a face set; falls back to the thinnest axis
// of the 3D bounding box when normals cancel out (closed / folded charts).
vec3 chartNormal(const Mesh& m, const std::vector<uint32_t>& faces) {
  dvec3 n(0);
  double area = 0;
  AABB box;
  for (uint32_t f : faces) {
    double a = m.faceArea3D(f);
    n += dvec3(m.faceNormal(f)) * a;
    area += a;
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) box.add(m.positions[m.loopVert[l]]);
  }
  double len = glm::length(n);
  if (len > 1e-6 * area && len > 1e-30) return vec3(n / len);
  if (!box.valid()) return vec3(0, 0, 1);
  vec3 s = box.size();
  if (s.x <= s.y && s.x <= s.z) return vec3(1, 0, 0);
  if (s.y <= s.z) return vec3(0, 1, 0);
  return vec3(0, 0, 1);
}

void projectFaces(Mesh& m, const std::vector<uint32_t>& faces, const vec3& normal) {
  vec3 u, v;
  planeAxes(normal, u, v);
  for (uint32_t f : faces)
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) {
      const vec3& p = m.positions[m.loopVert[l]];
      m.loopUV[l] = {glm::dot(p, u), glm::dot(p, v)};
    }
}

// Region growing over edge-adjacent faces (same `region` id) while the face
// normal stays within the angle limit of the seed normal. Seeds are visited by
// descending area. Appends clusters to `out` and writes the cluster index into
// faceCluster[f] (caller-owned scratch of size F, kNone for unvisited).
void clusterByNormal(const Mesh& m, const std::vector<uint32_t>& faces, float angleLimitDeg, const EdgeLoops& el,
                     const std::vector<uint32_t>& lf, const std::vector<uint32_t>& region,
                     std::vector<uint32_t>& faceCluster, std::vector<std::vector<uint32_t>>& out) {
  float cosLimit = std::cos(glm::radians(std::clamp(angleLimitDeg, 0.f, 180.f)));
  std::vector<std::pair<float, uint32_t>> order;
  order.reserve(faces.size());
  for (uint32_t f : faces) order.push_back({m.faceArea3D(f), f});
  std::stable_sort(order.begin(), order.end(), [](auto& a, auto& b) { return a.first > b.first; });
  std::vector<uint32_t> queue;
  for (auto& [area, seed] : order) {
    (void)area;
    if (faceCluster[seed] != kNone) continue;
    uint32_t id = (uint32_t)out.size();
    out.emplace_back();
    vec3 sn = m.faceNormal(seed);
    faceCluster[seed] = id;
    queue.assign(1, seed);
    for (size_t qi = 0; qi < queue.size(); ++qi) {
      uint32_t f = queue[qi];
      out[id].push_back(f);
      if (el.loops.empty()) continue;
      for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) {
        uint32_t e = m.loopEdge[l];
        for (uint32_t k = el.start[e]; k < el.start[e + 1]; ++k) {
          uint32_t g = lf[el.loops[k]];
          if (g == f || faceCluster[g] != kNone || region[g] != region[f]) continue;
          if (glm::dot(m.faceNormal(g), sn) < cosLimit) continue;
          faceCluster[g] = id;
          queue.push_back(g);
        }
      }
    }
  }
}

// ---------------------------------------------------------------- LSCM pieces

// "Chart vertices": the corners of one mesh vertex inside one chart, connected
// across non-seam edges. A vertex on a seam that does not fully separate the
// chart (a slit) therefore gets one unknown per side.
struct Corners {
  std::vector<uint32_t> loopGroup;  // per loop, kNone for loops outside the charts
  std::vector<uint8_t> boundary;    // per group
  uint32_t count = 0;
};

Corners buildCorners(const Mesh& m, const EdgeLoops& el, const std::vector<uint32_t>& lf,
                     const std::vector<uint32_t>& faceChart) {
  uint32_t L = m.loopCount();
  UnionFind uf(L);
  std::vector<uint8_t> twin(L, 0);
  for (uint32_t e = 0; e < el.edgeCount(); ++e) {
    if (isSeam(m, e)) continue;
    for (uint32_t i = el.start[e]; i < el.start[e + 1]; ++i) {
      uint32_t a0 = el.loops[i], fa = lf[a0];
      if (faceChart[fa] == kNone) continue;
      for (uint32_t j = i + 1; j < el.start[e + 1]; ++j) {
        uint32_t b0 = el.loops[j], fb = lf[b0];
        if (fb == fa || faceChart[fb] != faceChart[fa]) continue;
        uint32_t c0, c1, a1 = m.nextLoop(a0, fa);
        if (!matchSides(m, a0, a1, b0, m.nextLoop(b0, fb), c0, c1)) continue;
        uf.unite(a0, c0);
        uf.unite(a1, c1);
        twin[a0] = twin[b0] = 1;
      }
    }
  }
  Corners cs;
  cs.loopGroup.assign(L, kNone);
  std::vector<uint32_t> rootId(L, kNone);
  for (uint32_t f = 0; f < m.faceCount(); ++f) {
    if (faceChart[f] == kNone) continue;
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) {
      uint32_t r = uf.find(l);
      if (rootId[r] == kNone) rootId[r] = cs.count++;
      cs.loopGroup[l] = rootId[r];
    }
  }
  cs.boundary.assign(cs.count, 0);
  for (uint32_t f = 0; f < m.faceCount(); ++f) {
    if (faceChart[f] == kNone) continue;
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l)
      if (!twin[l]) cs.boundary[cs.loopGroup[l]] = cs.boundary[cs.loopGroup[m.nextLoop(l, f)]] = 1;
  }
  return cs;
}

// Symmetric block-CSR matrix whose 2x2 blocks are scaled rotations
// [[a, -c], [c, a]] -- exactly the structure of the LSCM normal equations
// (A^T A with complex-valued unknowns u + iv), so only (a, c) are stored.
struct BlockCsr {
  uint32_t n = 0;  // block rows
  std::vector<uint32_t> rowPtr, col;
  std::vector<double> a, c;

  // y = M x, x/y interleaved (u0, v0, u1, v1, ...)
  void mul(const std::vector<double>& x, std::vector<double>& y) const {
    for (uint32_t i = 0; i < n; ++i) {
      double yu = 0, yv = 0;
      for (uint32_t k = rowPtr[i]; k < rowPtr[i + 1]; ++k) {
        double xu = x[2 * col[k]], xv = x[2 * col[k] + 1];
        yu += a[k] * xu - c[k] * xv;
        yv += c[k] * xu + a[k] * xv;
      }
      y[2 * i] = yu;
      y[2 * i + 1] = yv;
    }
  }
  uint32_t find(uint32_t row, uint32_t column) const {
    auto b = col.begin() + rowPtr[row], e = col.begin() + rowPtr[row + 1];
    return uint32_t(std::lower_bound(b, e, column) - col.begin());
  }
};

double dot(const std::vector<double>& a, const std::vector<double>& b) {
  double s = 0;
  for (size_t i = 0; i < a.size(); ++i) s += a[i] * b[i];
  return s;
}

// Jacobi-preconditioned conjugate gradient; x holds the warm start on entry.
int solveCG(const BlockCsr& M, const std::vector<double>& b, std::vector<double>& x, int maxIt, double tol) {
  size_t N = b.size();
  std::vector<double> invD(N, 0.0), r(N), z(N), p(N), Ap(N);
  for (uint32_t i = 0; i < M.n; ++i) {
    uint32_t k = M.find(i, i);
    double d = (k < M.rowPtr[i + 1] && M.col[k] == i) ? M.a[k] : 0.0;
    invD[2 * i] = invD[2 * i + 1] = d > 1e-300 ? 1.0 / d : 0.0;  // empty rows keep the warm start
  }
  M.mul(x, Ap);
  for (size_t i = 0; i < N; ++i) {
    r[i] = (b[i] - Ap[i]) * (invD[i] != 0 ? 1.0 : 0.0);
    z[i] = invD[i] * r[i];
  }
  p = z;
  double rz = dot(r, z), bn = std::sqrt(dot(b, b));
  if (bn < 1e-300) bn = 1;
  int it = 0;
  for (; it < maxIt; ++it) {
    if (std::sqrt(dot(r, r)) <= tol * bn) break;
    M.mul(p, Ap);
    double pAp = dot(p, Ap);
    if (!(pAp > 1e-300)) break;
    double alpha = rz / pAp;
    for (size_t i = 0; i < N; ++i) {
      x[i] += alpha * p[i];
      r[i] -= alpha * Ap[i];
      z[i] = invD[i] * r[i];
    }
    double rzNew = dot(r, z);
    if (!(rz > 1e-300)) break;
    double beta = rzNew / rz;
    rz = rzNew;
    for (size_t i = 0; i < N; ++i) p[i] = z[i] + beta * p[i];
  }
  return it;
}

// LSCM for one chart (faces must all have loopGroup set). groupLocal is
// caller-owned scratch indexed by corner group (each group belongs to exactly
// one chart, so it never needs resetting). Returns CG iterations used.
int lscmChart(Mesh& m, const std::vector<uint32_t>& faces, const Corners& cs, const FaceTris& ft,
              std::vector<uint32_t>& groupLocal, const UnwrapOptions& opt) {
  // 1. chart vertices
  std::vector<dvec3> P;
  std::vector<uint32_t> bnd;
  for (uint32_t f : faces)
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) {
      uint32_t g = cs.loopGroup[l];
      if (groupLocal[g] != kNone) continue;
      groupLocal[g] = (uint32_t)P.size();
      if (cs.boundary[g]) bnd.push_back((uint32_t)P.size());
      P.push_back(dvec3(m.positions[m.loopVert[l]]));
    }
  uint32_t n = (uint32_t)P.size();
  if (n == 0) return 0;

  // 2. triangles in chart-vertex indices
  std::vector<std::array<uint32_t, 3>> T;
  for (uint32_t f : faces)
    for (uint32_t t = ft.start[f]; t < ft.start[f + 1]; ++t) {
      const glm::uvec3& tri = ft.tris[t];
      T.push_back({groupLocal[cs.loopGroup[tri.x]], groupLocal[cs.loopGroup[tri.y]], groupLocal[cs.loopGroup[tri.z]]});
    }

  // 3. pins: farthest pair on the boundary (all vertices if there is none).
  // Exact O(b^2) search for typical boundary sizes, double sweep beyond that.
  if (bnd.empty()) {
    bnd.resize(n);
    std::iota(bnd.begin(), bnd.end(), 0u);
  }
  auto farthestFrom = [&](uint32_t a) {
    uint32_t best = a;
    double bd = -1;
    for (uint32_t i : bnd) {
      double d = glm::dot(P[i] - P[a], P[i] - P[a]);
      if (d > bd) bd = d, best = i;
    }
    return best;
  };
  uint32_t pa = bnd[0], pb = bnd[0];
  if (bnd.size() <= 4096) {
    double bd = -1;
    for (size_t i = 0; i < bnd.size(); ++i)
      for (size_t j = i + 1; j < bnd.size(); ++j) {
        double d = glm::dot(P[bnd[i]] - P[bnd[j]], P[bnd[i]] - P[bnd[j]]);
        if (d > bd) bd = d, pa = bnd[i], pb = bnd[j];
      }
  } else {
    pa = farthestFrom(bnd[0]);
    pb = farthestFrom(pa);
  }

  // 4. warm start: projection onto the plane containing the pin axis that is
  // closest to the chart's average normal. Pins get their projected values.
  dvec3 N(0);
  for (auto& t : T) N += glm::cross(P[t[1]] - P[t[0]], P[t[2]] - P[t[0]]);
  dvec3 e1 = P[pb] - P[pa];
  double pinDist = glm::length(e1);
  e1 = pinDist > 1e-20 ? e1 / pinDist : dvec3(1, 0, 0);
  dvec3 nn = N - glm::dot(N, e1) * e1;
  if (glm::length(nn) < 1e-12 * (glm::length(N) + 1e-30)) {
    nn = glm::cross(e1, std::abs(e1.x) < 0.9 ? dvec3(1, 0, 0) : dvec3(0, 1, 0));
  }
  nn = glm::normalize(nn);
  dvec3 e2 = glm::cross(nn, e1);
  std::vector<dvec2> uv(n);
  for (uint32_t i = 0; i < n; ++i) uv[i] = {glm::dot(P[i] - P[pa], e1), glm::dot(P[i] - P[pa], e2)};
  if (pinDist <= 1e-20) uv[pb] = {1, 0};  // all points coincide: keep pins distinct

  int iterations = 0;
  if (n >= 3 && pa != pb) {
    // 5. per-triangle conformal coefficients in a local orthonormal frame:
    //    W_j = (edge opposite vertex j) / sqrt(2 * area), energy |sum W_j U_j|^2
    std::vector<std::array<double, 6>> W;  // wr0, wi0, wr1, wi1, wr2, wi2
    std::vector<uint32_t> validTri;
    W.reserve(T.size());
    for (uint32_t t = 0; t < T.size(); ++t) {
      dvec3 d1 = P[T[t][1]] - P[T[t][0]], d2 = P[T[t][2]] - P[T[t][0]];
      double l1 = glm::length(d1);
      dvec3 cr = glm::cross(d1, d2);
      double area2 = glm::length(cr);
      double maxE = std::max({l1, glm::length(d2), glm::length(d2 - d1)});
      if (!(area2 > 1e-12 * maxE * maxE) || !(l1 > 0)) continue;  // degenerate: no energy
      dvec3 x = d1 / l1, y = glm::cross(cr / area2, x);
      dvec2 q0(0, 0), q1(l1, 0), q2(glm::dot(d2, x), glm::dot(d2, y));
      double s = 1.0 / std::sqrt(area2);
      dvec2 w0 = (q2 - q1) * s, w1 = (q0 - q2) * s, w2 = (q1 - q0) * s;
      W.push_back({w0.x, w0.y, w1.x, w1.y, w2.x, w2.y});
      validTri.push_back(t);
    }

    // 6. unknowns: every chart vertex except the two pins
    std::vector<uint32_t> freeIdx(n, kNone);
    uint32_t nFree = 0;
    for (uint32_t i = 0; i < n; ++i)
      if (i != pa && i != pb) freeIdx[i] = nFree++;

    // 7. block sparsity pattern
    std::vector<uint64_t> keys;
    keys.reserve(validTri.size() * 9);
    for (uint32_t t : validTri)
      for (int j = 0; j < 3; ++j)
        for (int k = 0; k < 3; ++k) {
          uint32_t fj = freeIdx[T[t][j]], fk = freeIdx[T[t][k]];
          if (fj != kNone && fk != kNone) keys.push_back((uint64_t(fj) << 32) | fk);
        }
    for (uint32_t i = 0; i < nFree; ++i) keys.push_back((uint64_t(i) << 32) | i);  // diagonal always present
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    BlockCsr M;
    M.n = nFree;
    M.rowPtr.assign(nFree + 1, 0);
    M.col.resize(keys.size());
    for (size_t k = 0; k < keys.size(); ++k) {
      M.rowPtr[(keys[k] >> 32) + 1]++;
      M.col[k] = uint32_t(keys[k] & 0xffffffffu);
    }
    for (uint32_t i = 0; i < nFree; ++i) M.rowPtr[i + 1] += M.rowPtr[i];
    M.a.assign(keys.size(), 0.0);
    M.c.assign(keys.size(), 0.0);
    std::vector<double> rhs(2 * size_t(nFree), 0.0), x(2 * size_t(nFree), 0.0);

    // 8. assemble A^T A and A^T b (pinned columns moved to the right-hand side)
    for (size_t w = 0; w < validTri.size(); ++w) {
      const auto& t = T[validTri[w]];
      const auto& c = W[w];
      for (int j = 0; j < 3; ++j) {
        uint32_t fj = freeIdx[t[j]];
        if (fj == kNone) continue;
        double wrj = c[2 * j], wij = c[2 * j + 1];
        for (int k = 0; k < 3; ++k) {
          double wrk = c[2 * k], wik = c[2 * k + 1];
          double ba = wrj * wrk + wij * wik;
          double bc = wrj * wik - wij * wrk;
          uint32_t fk = freeIdx[t[k]];
          if (fk != kNone) {
            uint32_t pos = M.find(fj, fk);
            M.a[pos] += ba;
            M.c[pos] += bc;
          } else {
            const dvec2& pin = uv[t[k]];
            rhs[2 * fj] -= ba * pin.x - bc * pin.y;
            rhs[2 * fj + 1] -= bc * pin.x + ba * pin.y;
          }
        }
      }
    }
    for (uint32_t i = 0; i < n; ++i)
      if (freeIdx[i] != kNone) {
        x[2 * freeIdx[i]] = uv[i].x;
        x[2 * freeIdx[i] + 1] = uv[i].y;
      }

    // 9. solve
    iterations = solveCG(M, rhs, x, std::max(1, opt.maxIterations), std::max(1e-14, double(opt.tolerance)));
    bool ok = true;
    for (double v : x) ok = ok && std::isfinite(v);
    if (ok) {
      for (uint32_t i = 0; i < n; ++i)
        if (freeIdx[i] != kNone) uv[i] = {x[2 * freeIdx[i]], x[2 * freeIdx[i] + 1]};
    } else {
      PLOG_W("LSCM: solver diverged on a chart of %u vertices, keeping planar projection", n);
    }
  }

  // 10. write back, then flip if the chart came out mirrored (clockwise)
  for (uint32_t f : faces)
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) {
      const dvec2& p = uv[groupLocal[cs.loopGroup[l]]];
      m.loopUV[l] = vec2(p);
    }
  double signedArea = 0;
  for (uint32_t f : faces) signedArea += m.faceAreaUV(f);
  if (signedArea < 0)
    for (uint32_t f : faces)
      for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) m.loopUV[l].x = -m.loopUV[l].x;
  return iterations;
}

// ------------------------------------------------------------------ packing

// Skyline bottom-left rectangle packer of fixed width and unbounded height.
struct Skyline {
  struct Seg {
    double x, y, w;
  };
  std::vector<Seg> segs;
  double width = 1, top = 0;

  void reset(double w) {
    width = w;
    top = 0;
    segs.assign(1, {0, 0, w});
  }
  bool insert(double w, double h, double& ox, double& oy) {
    constexpr double eps = 1e-12;
    size_t best = segs.size();
    double bestY = std::numeric_limits<double>::max();
    for (size_t i = 0; i < segs.size(); ++i) {
      double x = segs[i].x;
      if (x + w > width + eps) break;
      double right = x + w, y = 0;
      for (size_t j = i; j < segs.size() && segs[j].x < right - eps; ++j) y = std::max(y, segs[j].y);
      if (y < bestY - eps) {
        bestY = y;
        best = i;
      }
    }
    if (best == segs.size()) return false;
    ox = segs[best].x;
    oy = bestY;
    double right = ox + w;
    size_t j = best;
    while (j < segs.size() && segs[j].x + segs[j].w <= right + eps) ++j;
    if (j < segs.size() && segs[j].x < right) {
      double end = segs[j].x + segs[j].w;
      segs[j].x = right;
      segs[j].w = end - right;
    }
    segs.erase(segs.begin() + best, segs.begin() + j);
    segs.insert(segs.begin() + best, {ox, oy + h, w});
    // merge neighbors at the same height
    if (best + 1 < segs.size() && std::abs(segs[best + 1].y - segs[best].y) < eps) {
      segs[best].w += segs[best + 1].w;
      segs.erase(segs.begin() + best + 1);
    }
    if (best > 0 && std::abs(segs[best - 1].y - segs[best].y) < eps) {
      segs[best - 1].w += segs[best].w;
      segs.erase(segs.begin() + best);
    }
    top = std::max(top, oy + h);
    return true;
  }
};

// Packs rectangles (w[i], h[i], in island units) scaled by a global S into the
// unit square with gaps of `margin`. Finds (close to) the largest S that fits.
// Outputs rectangle corners x/y (of the margin-inflated cell) and S.
void packRects(const std::vector<double>& w, const std::vector<double>& h, double& margin, double& S,
               std::vector<double>& x, std::vector<double>& y) {
  size_t n = w.size();
  x.assign(n, 0);
  y.assign(n, 0);
  std::vector<uint32_t> order(n);
  std::iota(order.begin(), order.end(), 0u);
  std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
    return h[a] != h[b] ? h[a] > h[b] : w[a] > w[b];
  });
  Skyline sky;
  auto fits = [&](double s) {
    sky.reset(1.0);
    for (uint32_t i : order) {
      if (!sky.insert(s * w[i] + margin, s * h[i] + margin, x[i], y[i])) return false;
      if (sky.top > 1.0 + 1e-12) return false;
    }
    return true;
  };
  // margins alone must fit (only an issue for absurd margins / island counts)
  margin = std::clamp(margin, 0.0, 0.25);
  for (int guard = 0; guard < 60 && !fits(0.0); ++guard) margin *= 0.5;

  double A = 0, B = 0, maxW = 0, maxH = 0;
  for (size_t i = 0; i < n; ++i) {
    A += w[i] * h[i];
    B += margin * (w[i] + h[i]);
    maxW = std::max(maxW, w[i]);
    maxH = std::max(maxH, h[i]);
  }
  double C = n * margin * margin;
  // upper bound: perfect packing of the inflated rectangles, and each must fit alone
  double hi = std::numeric_limits<double>::max();
  if (A > 1e-300) hi = (-B + std::sqrt(std::max(0.0, B * B - 4 * A * (C - 1)))) / (2 * A);
  else if (B > 1e-300) hi = (1 - C) / B;
  if (maxW > 1e-300) hi = std::min(hi, (1 - margin) / maxW);
  if (maxH > 1e-300) hi = std::min(hi, (1 - margin) / maxH);
  if (!(hi < std::numeric_limits<double>::max()) || !(hi > 0)) {
    S = 0;  // nothing has size; place the points
    fits(0.0);
    return;
  }
  // shrink geometrically until feasible, then bisect the last interval
  double lo = hi, fail = -1;
  for (int k = 0; k < 400 && !fits(lo); ++k) {
    fail = lo;
    lo *= 0.95;
  }
  if (fail > 0) {
    for (int k = 0; k < 10; ++k) {
      double mid = 0.5 * (lo + fail);
      if (fits(mid)) lo = mid;
      else fail = mid;
    }
  }
  S = lo;
  if (!fits(S)) {  // can only happen if the geometric search bottomed out
    S = 0;
    fits(0.0);
  }
}

struct PackIsland {
  std::vector<uint32_t> loops;
  double area3 = 0, areaUV = 0;
  double s = 1;          // density scale
  double cr = 1, sr = 0;  // rotation
  dvec2 lo{0};
  double w = 0, h = 0;
};

// Andrew's monotone chain; returns CCW hull without duplicate end point.
std::vector<dvec2> convexHull(std::vector<dvec2> p) {
  std::sort(p.begin(), p.end(), [](const dvec2& a, const dvec2& b) { return a.x != b.x ? a.x < b.x : a.y < b.y; });
  p.erase(std::unique(p.begin(), p.end()), p.end());
  if (p.size() < 3) return p;
  std::vector<dvec2> h(2 * p.size());
  size_t k = 0;
  auto cr = [](const dvec2& o, const dvec2& a, const dvec2& b) {
    return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
  };
  for (size_t i = 0; i < p.size(); ++i) {
    while (k >= 2 && cr(h[k - 2], h[k - 1], p[i]) <= 0) --k;
    h[k++] = p[i];
  }
  for (size_t i = p.size() - 1, t = k + 1; i > 0; --i) {
    while (k >= t && cr(h[k - 2], h[k - 1], p[i - 1]) <= 0) --k;
    h[k++] = p[i - 1];
  }
  h.resize(k - 1);
  return h;
}

// Rasterizes a UV triangle at cell centers of a res x res grid over [0,1]^2.
template <class Fn>
void rasterTri(vec2 a, vec2 b, vec2 c, int res, bool inclusive, Fn&& fn) {
  if (!finite2(a) || !finite2(b) || !finite2(c)) return;
  double area = cross2(b - a, c - a);
  if (std::abs(area) < 1e-14) return;
  if (area < 0) std::swap(b, c);
  double minx = std::min({a.x, b.x, c.x}) * res - 0.5, maxx = std::max({a.x, b.x, c.x}) * res - 0.5;
  double miny = std::min({a.y, b.y, c.y}) * res - 0.5, maxy = std::max({a.y, b.y, c.y}) * res - 0.5;
  if (maxx < 0 || maxy < 0 || minx > res - 1 || miny > res - 1) return;
  int x0 = std::max(0, (int)std::ceil(minx)), x1 = std::min(res - 1, (int)std::floor(maxx));
  int y0 = std::max(0, (int)std::ceil(miny)), y1 = std::min(res - 1, (int)std::floor(maxy));
  dvec2 A(a), B(b), C(c);
  auto edge = [](const dvec2& p, const dvec2& q, const dvec2& r) {
    return (q.x - p.x) * (r.y - p.y) - (q.y - p.y) * (r.x - p.x);
  };
  for (int j = y0; j <= y1; ++j)
    for (int i = x0; i <= x1; ++i) {
      dvec2 p((i + 0.5) / res, (j + 0.5) / res);
      double e0 = edge(A, B, p), e1 = edge(B, C, p), e2 = edge(C, A, p);
      bool in = inclusive ? (e0 >= 0 && e1 >= 0 && e2 >= 0) : (e0 > 0 && e1 > 0 && e2 > 0);
      if (in) fn(i, j);
    }
}

}  // namespace

// ============================================================ topology helpers

std::vector<std::vector<uint32_t>> chartsFromSeams(const Mesh& m, const std::vector<uint32_t>& faces) {
  auto sel = selectFaces(m, faces);
  uint32_t F = m.faceCount();
  std::vector<uint8_t> in(F, 0);
  for (uint32_t f : sel) in[f] = 1;
  UnionFind uf(F);
  EdgeLoops el(m);
  auto lf = m.loopFace();
  for (uint32_t e = 0; e < el.edgeCount(); ++e) {
    if (isSeam(m, e)) continue;
    uint32_t first = kNone;
    for (uint32_t k = el.start[e]; k < el.start[e + 1]; ++k) {
      uint32_t f = lf[el.loops[k]];
      if (!in[f]) continue;
      if (first == kNone) first = f;
      else uf.unite(first, f);
    }
  }
  return groupByRoot(sel, uf);
}

std::vector<std::vector<uint32_t>> facesByIsland(const Mesh& m, const std::vector<uint32_t>& faces) {
  auto sel = selectFaces(m, faces);
  std::vector<std::vector<uint32_t>> out;
  if (m.loopIsland.size() != m.loopCount()) {
    PLOG_W("facesByIsland: UV topology is stale (call computeUVTopology); treating faces as separate islands");
    for (uint32_t f : sel) out.push_back({f});
    return out;
  }
  std::vector<uint32_t> id(m.islandCount + 1, kNone);
  for (uint32_t f : sel) {
    if (m.faceSize(f) == 0) continue;
    uint32_t isl = std::min(m.loopIsland[m.faceStart[f]], m.islandCount);
    if (id[isl] == kNone) {
      id[isl] = (uint32_t)out.size();
      out.emplace_back();
    }
    out[id[isl]].push_back(f);
  }
  return out;
}

void seamsFromIslands(Mesh& m, bool clearOthers) {
  ensureTopology(m);
  if (m.loopIsland.size() != m.loopCount()) m.computeUVTopology();
  EdgeLoops el(m);
  auto lf = m.loopFace();
  for (uint32_t e = 0; e < el.edgeCount(); ++e) {
    bool seam = false;
    for (uint32_t i = el.start[e]; i < el.start[e + 1] && !seam; ++i)
      for (uint32_t j = i + 1; j < el.start[e + 1] && !seam; ++j) {
        uint32_t a0 = el.loops[i], b0 = el.loops[j];
        uint32_t a1 = m.nextLoop(a0, lf[a0]), b1 = m.nextLoop(b0, lf[b0]), c0, c1;
        if (m.loopIsland[a0] != m.loopIsland[b0]) seam = true;
        else if (matchSides(m, a0, a1, b0, b1, c0, c1))
          seam = !uvEq(m.loopUV[a0], m.loopUV[c0]) || !uvEq(m.loopUV[a1], m.loopUV[c1]);  // UV split
      }
    if (seam) m.edgeFlags[e] |= EdgeSeam;
    else if (clearOthers) m.edgeFlags[e] &= uint8_t(~EdgeSeam);
  }
}

void seamsByAngle(Mesh& m, float angleDeg, bool clearOthers) {
  ensureTopology(m);
  EdgeLoops el(m);
  auto lf = m.loopFace();
  float cosT = std::cos(glm::radians(std::clamp(angleDeg, 0.f, 180.f)));
  for (uint32_t e = 0; e < el.edgeCount(); ++e) {
    bool seam = false;
    for (uint32_t i = el.start[e]; i < el.start[e + 1] && !seam; ++i)
      for (uint32_t j = i + 1; j < el.start[e + 1] && !seam; ++j) {
        uint32_t fa = lf[el.loops[i]], fb = lf[el.loops[j]];
        if (fa != fb) seam = glm::dot(m.faceNormal(fa), m.faceNormal(fb)) < cosT;
      }
    if (seam) m.edgeFlags[e] |= EdgeSeam;
    else if (clearOthers) m.edgeFlags[e] &= uint8_t(~EdgeSeam);
  }
}

// ================================================================ unwrapping

void unwrapLSCM(Mesh& m, const std::vector<uint32_t>& faces, const UnwrapOptions& opt) {
  double t0 = nowSeconds();
  ensureTopology(m);
  if (m.loopUV.size() != m.loopCount()) m.loopUV.assign(m.loopCount(), vec2(0));
  auto sel = selectFaces(m, faces);
  if (sel.empty()) return;
  uint32_t F = m.faceCount();
  auto lf = m.loopFace();
  EdgeLoops el(m);
  FaceTris ft(m);

  std::vector<std::vector<uint32_t>> charts;
  std::vector<uint32_t> faceChart;
  Corners cs;
  std::vector<uint8_t> closed;
  auto analyze = [&]() {
    charts = chartsFromSeams(m, sel);
    faceChart.assign(F, kNone);
    for (uint32_t c = 0; c < charts.size(); ++c)
      for (uint32_t f : charts[c]) faceChart[f] = c;
    cs = buildCorners(m, el, lf, faceChart);
    closed.assign(charts.size(), 1);
    for (uint32_t c = 0; c < charts.size(); ++c)
      for (uint32_t f : charts[c])
        for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1] && closed[c]; ++l)
          if (cs.boundary[cs.loopGroup[l]]) closed[c] = 0;
  };
  analyze();

  // Closed charts (no boundary, e.g. a sphere without seams) cannot be
  // flattened: cut them into normal clusters and mark the cuts as seams.
  bool anyCut = false;
  {
    std::vector<uint32_t> faceCluster(F, kNone);
    std::vector<std::vector<uint32_t>> clusters;
    for (uint32_t c = 0; c < charts.size(); ++c) {
      if (!closed[c]) continue;
      size_t before = clusters.size();
      clusterByNormal(m, charts[c], 66.f, el, lf, faceChart, faceCluster, clusters);
      if (clusters.size() - before < 2) continue;
      uint32_t cut = 0;
      for (uint32_t f : charts[c])
        for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) {
          uint32_t e = m.loopEdge[l];
          for (uint32_t k = el.start[e]; k < el.start[e + 1]; ++k) {
            uint32_t g = lf[el.loops[k]];
            if (faceChart[g] == c && faceCluster[g] != faceCluster[f] && !isSeam(m, e)) {
              m.edgeFlags[e] |= EdgeSeam;
              ++cut;
            }
          }
        }
      PLOG_I("LSCM: closed chart of %zu faces cut into %zu pieces (%u seam edges added)", charts[c].size(),
             clusters.size() - before, cut);
      anyCut = true;
    }
  }
  if (anyCut) analyze();

  std::vector<uint32_t> groupLocal(cs.count, kNone);
  long totalIt = 0;
  for (uint32_t c = 0; c < charts.size(); ++c) {
    if (closed[c]) {  // degenerate closed chart that clustering could not split
      projectFaces(m, charts[c], chartNormal(m, charts[c]));
      continue;
    }
    totalIt += lscmChart(m, charts[c], cs, ft, groupLocal, opt);
  }
  if (opt.pack) {
    PackOptions po;
    po.margin = opt.margin;
    packCharts(m, charts, po);
  }
  PLOG_I("LSCM: %zu charts, %zu faces, %ld CG iterations, %.1f ms", charts.size(), sel.size(), totalIt,
         (nowSeconds() - t0) * 1000.0);
}

void projectPlanar(Mesh& m, const vec3& normal, const std::vector<uint32_t>& faces) {
  if (m.loopUV.size() != m.loopCount()) m.loopUV.assign(m.loopCount(), vec2(0));
  auto sel = selectFaces(m, faces);
  projectFaces(m, sel, normal);
  fitToUnitSquare(m, loopsOfFaces(m, sel));
}

void projectBox(Mesh& m, const std::vector<uint32_t>& faces, float margin) {
  ensureTopology(m);
  if (m.loopUV.size() != m.loopCount()) m.loopUV.assign(m.loopCount(), vec2(0));
  auto sel = selectFaces(m, faces);
  uint32_t F = m.faceCount();
  // direction 0..5 = +X -X +Y -Y +Z -Z
  std::vector<uint32_t> dir(F, kNone);
  static const vec3 kAxes[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
  for (uint32_t f : sel) {
    vec3 n = m.faceNormal(f), a = glm::abs(n);
    int axis = (a.x >= a.y && a.x >= a.z) ? 0 : (a.y >= a.z ? 1 : 2);
    dir[f] = uint32_t(axis * 2 + (n[axis] < 0 ? 1 : 0));
    vec3 u, v;
    planeAxes(kAxes[dir[f]], u, v);
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) {
      const vec3& p = m.positions[m.loopVert[l]];
      m.loopUV[l] = {glm::dot(p, u), glm::dot(p, v)};
    }
  }
  // charts: edge-connected faces sharing a direction
  UnionFind uf(F);
  EdgeLoops el(m);
  auto lf = m.loopFace();
  for (uint32_t e = 0; e < el.edgeCount(); ++e)
    for (uint32_t i = el.start[e]; i < el.start[e + 1]; ++i)
      for (uint32_t j = i + 1; j < el.start[e + 1]; ++j) {
        uint32_t fa = lf[el.loops[i]], fb = lf[el.loops[j]];
        if (dir[fa] != kNone && dir[fa] == dir[fb]) uf.unite(fa, fb);
      }
  PackOptions po;
  po.margin = margin;
  po.rotate = false;            // keep projection axes aligned
  po.normalizeDensity = false;  // projection is already in uniform object-space units
  packCharts(m, groupByRoot(sel, uf), po);
}

void smartProject(Mesh& m, float angleLimitDeg, const std::vector<uint32_t>& faces, float margin) {
  ensureTopology(m);
  if (m.loopUV.size() != m.loopCount()) m.loopUV.assign(m.loopCount(), vec2(0));
  auto sel = selectFaces(m, faces);
  if (sel.empty()) return;
  uint32_t F = m.faceCount();
  auto lf = m.loopFace();
  EdgeLoops el(m);
  std::vector<uint32_t> region(F, kNone), faceCluster(F, kNone);
  for (uint32_t f : sel) region[f] = 0;
  std::vector<std::vector<uint32_t>> charts;
  auto t0 = std::chrono::steady_clock::now();
  auto since = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
  clusterByNormal(m, sel, angleLimitDeg, el, lf, region, faceCluster, charts);
  const double tCluster = since();

  for (auto& chart : charts) {
    vec3 n = chartNormal(m, chart);
    // Projecting onto the average plane can fold faces that are > 90 degrees
    // from it; the seed normal is within the angle limit of every face.
    bool folds = false;
    for (uint32_t f : chart) folds = folds || glm::dot(m.faceNormal(f), n) <= 0.f;
    projectFaces(m, chart, folds ? m.faceNormal(chart.front()) : n);
  }
  // chart borders (including the border to unselected faces) become seams
  for (uint32_t f : sel)
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) {
      uint32_t e = m.loopEdge[l];
      for (uint32_t k = el.start[e]; k < el.start[e + 1]; ++k)
        if (faceCluster[lf[el.loops[k]]] != faceCluster[f]) m.edgeFlags[e] |= EdgeSeam;
    }
  const double tProject = since();
  PackOptions po;
  po.margin = margin;
  packCharts(m, charts, po);
  PLOG_D("Smart UV projection: %zu charts found %.2f s, projected %.2f s, packed %.2f s", charts.size(), tCluster, tProject - tCluster,
         since() - tProject);
}

// =================================================================== packing

void pack(Mesh& m, const PackOptions& opt, const std::vector<uint32_t>& faces) {
  if (m.loopUV.size() != m.loopCount()) return;
  if (m.loopIsland.size() != m.loopCount()) {
    ensureTopology(m);
    m.computeUVTopology();
  }
  auto islands = facesByIsland(m, {});
  if (!faces.empty()) {
    std::vector<uint8_t> want(m.islandCount + 1, 0);
    for (uint32_t f : selectFaces(m, faces))
      if (m.faceSize(f)) want[std::min(m.loopIsland[m.faceStart[f]], m.islandCount)] = 1;
    std::vector<std::vector<uint32_t>> keep;
    for (auto& isl : islands)
      if (want[std::min(m.loopIsland[m.faceStart[isl.front()]], m.islandCount)]) keep.push_back(std::move(isl));
    islands = std::move(keep);
    // only some islands: they go around the others
    if (opt.shapes && !opt.keepInPlace) {
      std::vector<uint8_t> moving(m.faceCount(), 0);
      for (auto& isl : islands)
        for (uint32_t f : isl) moving[f] = 1;
      std::vector<uint32_t> others;
      for (uint32_t f = 0; f < m.faceCount(); ++f)
        if (!moving[f]) others.push_back(f);
      if (packShapes(m, islands, opt, others)) return;
    }
  }
  packCharts(m, islands, opt);
}

void packCharts(Mesh& m, const std::vector<std::vector<uint32_t>>& charts, const PackOptions& opt) {
  if (m.loopUV.size() != m.loopCount()) return;
  if (opt.shapes && !opt.keepInPlace) {
    // outlines nest concave islands far better; for box-like islands the box packer (much faster,
    // no grid rounding) can win: both run, the layout that gives the islands more room stays
    std::vector<vec2> before = m.loopUV;
    ShapePackStats st;
    auto t0 = std::chrono::steady_clock::now();
    if (packShapes(m, charts, opt, {}, 0, &st)) {
      const double tShapes = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      std::vector<vec2> shaped = m.loopUV;
      m.loopUV = before;
      PackOptions boxes = opt;
      boxes.shapes = false;
      packCharts(m, charts, boxes);
      PLOG_D("Packing: by outline %.2f s, by box %.2f s", tShapes,
             std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() - tShapes);
      double boxFill = 0, shapeFill = 0;
      for (auto& c : charts)
        for (uint32_t f : c) {
          if (f >= m.faceCount()) continue;
          boxFill += std::abs(m.faceAreaUV(f));
          std::swap(m.loopUV, shaped);
          shapeFill += std::abs(m.faceAreaUV(f));
          std::swap(m.loopUV, shaped);
        }
      if (shapeFill >= boxFill) m.loopUV = std::move(shaped);
      return;
    }
  }
  uint32_t F = m.faceCount();
  std::vector<uint8_t> taken(F, 0);
  std::vector<PackIsland> isl;
  isl.reserve(charts.size());
  Rect2 orig;
  bool flags = m.faceFlags.size() == F;
  for (auto& chart : charts) {
    // mirror copies are not packed: they sit on the faces they copy (restacked by the caller)
    bool copies = flags && !chart.empty();
    for (uint32_t f : chart) copies &= f < F && (m.faceFlags[f] & FaceMirrorCopy) != 0;
    if (copies) continue;
    PackIsland pi;
    for (uint32_t f : chart) {
      if (f >= F || taken[f]) continue;  // faces listed twice are packed once
      taken[f] = 1;
      pi.area3 += m.faceArea3D(f);
      float a = m.faceAreaUV(f);
      if (std::isfinite(a)) pi.areaUV += std::abs(a);
      for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) {
        if (!finite2(m.loopUV[l])) m.loopUV[l] = vec2(0);
        pi.loops.push_back(l);
        orig.add(m.loopUV[l]);
      }
    }
    if (!pi.loops.empty()) isl.push_back(std::move(pi));
  }
  if (isl.empty()) return;

  // texel density: s = sqrt(area3D / areaUV); islands without a usable ratio
  // get the mean scale of the others
  if (opt.normalizeDensity) {
    double sum = 0;
    int cnt = 0;
    for (auto& p : isl)
      if (p.area3 > 1e-30 && p.areaUV > 1e-30) {
        p.s = std::sqrt(p.area3 / p.areaUV);
        sum += p.s;
        ++cnt;
      } else {
        p.s = -1;
      }
    double fallback = cnt ? sum / cnt : 1.0;
    for (auto& p : isl)
      if (p.s < 0) p.s = fallback;
  }

  std::vector<dvec2> pts;
  for (auto& p : isl) {
    pts.clear();
    for (uint32_t l : p.loops) pts.push_back(dvec2(m.loopUV[l]) * p.s);
    if (opt.rotate) {
      // minimum-area bounding rectangle: one side is collinear with a hull edge
      auto hull = convexHull(pts);
      auto boxArea = [&](double c, double s, double& w, double& h) {
        double x0 = 1e300, x1 = -1e300, y0 = 1e300, y1 = -1e300;
        for (auto& q : hull) {
          double x = c * q.x + s * q.y, y = -s * q.x + c * q.y;
          x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
        }
        w = x1 - x0, h = y1 - y0;
        return w * h;
      };
      double bw, bh, best = boxArea(1, 0, bw, bh);  // prefer no rotation on ties
      for (size_t i = 0; i < hull.size() && hull.size() >= 2; ++i) {
        dvec2 d = hull[(i + 1) % hull.size()] - hull[i];
        double len = glm::length(d);
        if (len < 1e-300) continue;
        double w, h, a = boxArea(d.x / len, d.y / len, w, h);
        if (a < best * (1 - 1e-6)) best = a, p.cr = d.x / len, p.sr = d.y / len, bw = w, bh = h;
      }
      // consistent orientation: islands lie wider than tall (better skyline rows)
      if (bh > bw * (1 + 1e-6)) {
        double c = p.cr, s = p.sr;  // compose with a +90 degree rotation
        p.cr = -s;
        p.sr = c;
      }
    }
    // rotate by -angle: x' = c x + s y, y' = -s x + c y
    double x0 = 1e300, x1 = -1e300, y0 = 1e300, y1 = -1e300;
    for (auto& q : pts) {
      double x = p.cr * q.x + p.sr * q.y, y = -p.sr * q.x + p.cr * q.y;
      x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
    }
    p.lo = {x0, y0};
    p.w = std::max(0.0, x1 - x0);
    p.h = std::max(0.0, y1 - y0);
  }

  std::vector<double> W(isl.size()), H(isl.size()), X, Y;
  for (size_t i = 0; i < isl.size(); ++i) W[i] = isl[i].w, H[i] = isl[i].h;
  double margin = std::max(0.f, opt.margin), S = 0;
  packRects(W, H, margin, S, X, Y);
  S *= 1 - 1e-6;  // keep float rounding from poking past 1.0

  // keepInPlace: the packed unit square is mapped onto the islands' former bounds
  dvec2 dstLo(0), dstScale(1);
  if (opt.keepInPlace && orig.valid()) {
    double side = std::max(orig.size().x, orig.size().y);
    if (side > 1e-12) dstLo = dvec2(orig.lo), dstScale = dvec2(side);
  }
  for (size_t i = 0; i < isl.size(); ++i) {
    const PackIsland& p = isl[i];
    dvec2 off(X[i] + margin * 0.5, Y[i] + margin * 0.5);
    for (uint32_t l : p.loops) {
      dvec2 q = dvec2(m.loopUV[l]) * p.s;
      dvec2 r(p.cr * q.x + p.sr * q.y, -p.sr * q.x + p.cr * q.y);
      m.loopUV[l] = vec2(dstLo + ((r - p.lo) * S + off) * dstScale);
    }
  }
}

// ================================================================ transforms

std::vector<uint32_t> loopsOfFaces(const Mesh& m, const std::vector<uint32_t>& faces) {
  std::vector<uint32_t> out;
  for (uint32_t f : selectFaces(m, faces))
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) out.push_back(l);
  return out;
}

Rect2 boundsOfLoops(const Mesh& m, const std::vector<uint32_t>& loops) {
  Rect2 r;
  for (uint32_t l : loops)
    if (l < m.loopUV.size() && finite2(m.loopUV[l])) r.add(m.loopUV[l]);
  return r;
}

void translate(Mesh& m, const std::vector<uint32_t>& loops, const vec2& d) {
  for (uint32_t l : loops)
    if (l < m.loopUV.size()) m.loopUV[l] += d;
}

void rotate(Mesh& m, const std::vector<uint32_t>& loops, float radians, const vec2& pivot) {
  float c = std::cos(radians), s = std::sin(radians);
  for (uint32_t l : loops) {
    if (l >= m.loopUV.size()) continue;
    vec2 d = m.loopUV[l] - pivot;
    m.loopUV[l] = pivot + vec2(c * d.x - s * d.y, s * d.x + c * d.y);
  }
}

void scale(Mesh& m, const std::vector<uint32_t>& loops, const vec2& s, const vec2& pivot) {
  for (uint32_t l : loops)
    if (l < m.loopUV.size()) m.loopUV[l] = pivot + (m.loopUV[l] - pivot) * s;
}

void flip(Mesh& m, const std::vector<uint32_t>& loops, bool horizontal) {
  Rect2 r = boundsOfLoops(m, loops);
  if (!r.valid()) return;
  vec2 c = r.center();
  for (uint32_t l : loops) {
    if (l >= m.loopUV.size()) continue;
    if (horizontal) m.loopUV[l].x = 2 * c.x - m.loopUV[l].x;
    else m.loopUV[l].y = 2 * c.y - m.loopUV[l].y;
  }
}

// Loops that share a loopUVVert already coincide (within 1e-5), so welding
// groups the given loops by mesh vertex: every corner of one vertex among
// `loops` moves to the average UV of those corners.
void weld(Mesh& m, const std::vector<uint32_t>& loops) {
  std::vector<std::pair<uint32_t, uint32_t>> byVert;  // (vertex, loop)
  byVert.reserve(loops.size());
  for (uint32_t l : loops)
    if (l < m.loopUV.size() && l < m.loopVert.size() && finite2(m.loopUV[l])) byVert.push_back({m.loopVert[l], l});
  std::sort(byVert.begin(), byVert.end());
  byVert.erase(std::unique(byVert.begin(), byVert.end()), byVert.end());
  for (size_t i = 0; i < byVert.size();) {
    size_t j = i;
    dvec2 sum(0);
    while (j < byVert.size() && byVert[j].first == byVert[i].first) sum += dvec2(m.loopUV[byVert[j++].second]);
    vec2 avg = vec2(sum / double(j - i));
    for (size_t k = i; k < j; ++k) m.loopUV[byVert[k].second] = avg;
    i = j;
  }
}

void fitToUnitSquare(Mesh& m, const std::vector<uint32_t>& loops, float margin) {
  Rect2 r = boundsOfLoops(m, loops);
  if (!r.valid()) return;
  margin = std::clamp(margin, 0.f, 0.49f);
  float side = std::max(r.size().x, r.size().y);
  float s = side > 1e-20f ? (1.f - 2.f * margin) / side : 1.f;
  vec2 c = r.center();
  for (uint32_t l : loops)
    if (l < m.loopUV.size()) m.loopUV[l] = vec2(0.5f) + (m.loopUV[l] - c) * s;
}

// =================================================================== metrics

float texelDensity(const Mesh& m, const std::vector<uint32_t>& faces) {
  // area weighted, over the faces that have UV area (unmapped faces would pull a plain mean down)
  if (m.loopUV.size() != m.loopCount()) return 0.f;
  double a3 = 0, au = 0;
  for (uint32_t f : selectFaces(m, faces)) {
    double u = std::abs(m.faceAreaUV(f));
    if (!std::isfinite(u) || u < 1e-14) continue;
    a3 += m.faceArea3D(f);
    au += u;
  }
  return a3 > 1e-30 ? float(std::sqrt(au / a3)) : 0.f;
}

std::vector<float> areaDistortion(const Mesh& m) {
  uint32_t F = m.faceCount();
  std::vector<float> out(F, 0.f);
  if (m.loopUV.size() != m.loopCount()) return out;
  std::vector<double> a3(F), au(F);
  double s3 = 0, su = 0;
  for (uint32_t f = 0; f < F; ++f) {
    a3[f] = m.faceArea3D(f);
    au[f] = std::abs(m.faceAreaUV(f));
    if (!std::isfinite(au[f])) au[f] = 0;
    s3 += a3[f];
    su += au[f];
  }
  if (s3 <= 1e-30 || su <= 1e-30) return out;
  double mean = su / s3;
  for (uint32_t f = 0; f < F; ++f) {
    if (a3[f] <= 1e-30) continue;
    double r = au[f] / a3[f] / mean;
    out[f] = float(std::clamp(std::log2(std::max(r, 1e-6)), -16.0, 16.0));
  }
  return out;
}

float coverage(const Mesh& m, int res) {
  if (m.loopUV.size() != m.loopCount()) return 0.f;
  if (m.tiles.active()) {  // texture tiles: the mean over the tiles in use
    Mesh c = m;
    double sum = 0;
    int n = 0;
    forEachTile(c, {}, [&](const std::vector<uint32_t>&) { sum += coverage(c, res), ++n; });
    return n ? float(sum / n) : 0.f;
  }
  res = std::clamp(res, 1, 4096);
  std::vector<uint8_t> grid(size_t(res) * res, 0);
  FaceTris ft(m);
  for (const auto& t : ft.tris)
    rasterTri(m.loopUV[t.x], m.loopUV[t.y], m.loopUV[t.z], res, true,
              [&](int i, int j) { grid[size_t(j) * res + i] = 1; });
  size_t covered = 0;
  for (uint8_t g : grid) covered += g;
  return float(double(covered) / double(grid.size()));
}

int overlapCount(const Mesh& m, int res) {
  if (m.loopUV.size() != m.loopCount()) return 0;
  if (m.tiles.active()) {  // texture tiles: only faces of the same tile can share texels
    Mesh c = m;
    int sum = 0;
    forEachTile(c, {}, [&](const std::vector<uint32_t>&) { sum += overlapCount(c, res); });
    return sum;
  }
  res = std::clamp(res, 1, 2048);
  FaceTris ft(m);
  // (cell, face) samples; strict inside test so faces that merely share an
  // edge never claim the same cell center
  std::vector<uint64_t> samples;
  for (uint32_t f = 0; f < m.faceCount(); ++f)
    for (uint32_t t = ft.start[f]; t < ft.start[f + 1]; ++t) {
      const auto& tri = ft.tris[t];
      rasterTri(m.loopUV[tri.x], m.loopUV[tri.y], m.loopUV[tri.z], res, false,
                [&](int i, int j) { samples.push_back((uint64_t(uint32_t(j) * res + i) << 32) | f); });
    }
  std::sort(samples.begin(), samples.end());
  samples.erase(std::unique(samples.begin(), samples.end()), samples.end());
  std::unordered_set<uint64_t> pairs;
  size_t budget = 20'000'000;  // bounded work for pathological stacks of faces
  for (size_t i = 0; i < samples.size() && budget;) {
    size_t j = i;
    while (j < samples.size() && (samples[j] >> 32) == (samples[i] >> 32)) ++j;
    for (size_t a = i; a < j && budget; ++a)
      for (size_t b = a + 1; b < j && budget; ++b, --budget) {
        uint32_t fa = uint32_t(samples[a]), fb = uint32_t(samples[b]);
        pairs.insert((uint64_t(fa) << 32) | fb);
      }
    i = j;
  }
  if (!budget) PLOG_W("overlapCount: pair budget exhausted, result is a lower bound");
  return (int)pairs.size();
}

}  // namespace patina::uv
