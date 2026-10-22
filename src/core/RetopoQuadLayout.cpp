#include "core/RetopoQuadLayout.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <queue>
#include <unordered_map>
#include <unordered_set>

#include <Eigen/IterativeLinearSolvers>
#include <Eigen/Sparse>
#include <libsatsuma/Extra/Highlevel.hh>
#include <libsatsuma/IO/write_bimdf.hh>
#include <libsatsuma/Solvers/EvenBiMDF.hh>
#include <libsatsuma/Problems/BiMDF.hh>

#include "core/Log.h"
#include "core/RetopoJoin.h"
#include "optimizer.hpp"
#include "parametrizer.hpp"

namespace patina::retopo {

namespace {

constexpr float kPi = 3.14159265358979f;

inline uint64_t ekey(uint32_t a, uint32_t b) { return (uint64_t(std::min(a, b)) << 32) | std::max(a, b); }

// The working triangles with their neighbors, cut as the traces go.
struct LM {
  std::vector<vec3> P;
  std::vector<std::array<uint32_t, 3>> T;
  std::vector<std::array<int64_t, 3>> N;  // across edge k (T[k] to T[k + 1]); -1: the model's edge
  std::vector<vec3> fd, fn;               // per triangle: one field direction (in its plane), its normal
  std::vector<int> zone;
  std::vector<float> L;
  std::vector<uint32_t> vt;                    // per vertex: a triangle at it
  std::unordered_map<uint64_t, uint8_t> path;  // the layout's edges and their kind
  std::vector<int> pathDeg;                    // per vertex: the layout's edges at it
  std::vector<int8_t> sing;                    // per vertex: +1 / -1 singular

  int local(uint32_t t, uint32_t v) const { return T[t][0] == v ? 0 : T[t][1] == v ? 1 : T[t][2] == v ? 2 : -1; }
  uint8_t pathOf(uint32_t a, uint32_t b) const {
    auto it = path.find(ekey(a, b));
    return it == path.end() ? 0 : it->second;
  }
  void setPath(uint32_t a, uint32_t b, uint8_t kind) {
    auto [it, fresh] = path.emplace(ekey(a, b), kind);
    if (fresh) pathDeg[a]++, pathDeg[b]++;
  }
  void repoint(int64_t t, int64_t from, int64_t to) {
    if (t < 0) return;
    for (auto& x : N[t])
      if (x == from) x = to;
  }
  uint32_t newTri(uint32_t like) {
    T.push_back({0, 0, 0}), N.push_back({-1, -1, -1});
    fd.push_back(fd[like]), fn.push_back(fn[like]), zone.push_back(zone[like]), L.push_back(L[like]);
    return (uint32_t)T.size() - 1;
  }
  uint32_t newVert(const vec3& p) {
    P.push_back(p), vt.push_back(0), pathDeg.push_back(0), sing.push_back(0);
    return (uint32_t)P.size() - 1;
  }
  // edge k of t split at X (t keeps the part at T[t][k]); the new vertex
  uint32_t splitEdge(uint32_t t, int k, const vec3& X) {
    const uint32_t a = T[t][k], b = T[t][(k + 1) % 3], c = T[t][(k + 2) % 3];
    const int64_t u = N[t][k], nbc = N[t][(k + 1) % 3], nca = N[t][(k + 2) % 3];
    const uint8_t kind = pathOf(a, b);
    const uint32_t x = newVert(X);
    const uint32_t t2 = newTri(t);
    T[t] = {a, x, c}, N[t] = {-1, (int64_t)t2, nca};
    T[t2] = {x, b, c}, N[t2] = {-1, nbc, (int64_t)t};
    repoint(nbc, t, t2);
    vt[x] = t, vt[a] = t, vt[c] = t, vt[b] = t2;
    if (u >= 0) {
      int ku = -1;
      for (int j = 0; j < 3; ++j)
        if (T[u][j] == b && T[u][(j + 1) % 3] == a) ku = j;
      if (ku >= 0) {
        const uint32_t d = T[u][(ku + 2) % 3];
        const int64_t nad = N[u][(ku + 1) % 3], ndb = N[u][(ku + 2) % 3];
        const uint32_t u2 = newTri((uint32_t)u);
        T[u] = {b, x, d}, N[u] = {(int64_t)t2, (int64_t)u2, ndb};
        T[u2] = {x, a, d}, N[u2] = {(int64_t)t, nad, u};
        repoint(nad, u, u2);
        N[t][0] = u2, N[t2][0] = u;
        vt[d] = (uint32_t)u;
      }
    }
    if (kind) {
      path.erase(ekey(a, b)), pathDeg[a]--, pathDeg[b]--;
      setPath(a, x, kind), setPath(x, b, kind);
    }
    return x;
  }
  // a vertex at X inside t (three triangles)
  uint32_t splitFace(uint32_t t, const vec3& X) {
    const uint32_t a = T[t][0], b = T[t][1], c = T[t][2];
    const int64_t nab = N[t][0], nbc = N[t][1], nca = N[t][2];
    const uint32_t s = newVert(X), t1 = newTri(t), t2 = newTri(t);
    T[t] = {a, b, s}, N[t] = {nab, (int64_t)t1, (int64_t)t2};
    T[t1] = {b, c, s}, N[t1] = {nbc, (int64_t)t2, (int64_t)t};
    T[t2] = {c, a, s}, N[t2] = {nca, (int64_t)t, (int64_t)t1};
    repoint(nbc, t, t1), repoint(nca, t, t2);
    vt[s] = t, vt[a] = t, vt[b] = t1, vt[c] = t2;
    return s;
  }
  // the triangles round v, in turn (from one open end, if the fan is open)
  void fan(uint32_t v, std::vector<uint32_t>& out) const {
    out.clear();
    const int64_t t0 = vt[v];
    int64_t start = t0;
    for (int64_t t = t0, guard = 0; guard < 512; ++guard) {
      int i = local((uint32_t)t, v);
      if (i < 0) break;
      int64_t pr = N[t][(i + 2) % 3];
      if (pr < 0) {
        start = t;
        break;
      }
      if (pr == t0) break;
      t = pr;
    }
    for (int64_t t = start, guard = 0; guard < 512; ++guard) {
      out.push_back((uint32_t)t);
      int i = local((uint32_t)t, v);
      if (i < 0) break;
      int64_t nx = N[t][i];
      if (nx < 0 || nx == start) break;
      t = nx;
    }
  }
  // the field's direction in t nearest d
  vec3 fieldNear(uint32_t t, const vec3& d) const {
    vec3 a = fd[t], b = glm::cross(fn[t], fd[t]);
    float da = glm::dot(a, d), db = glm::dot(b, d);
    return std::abs(da) >= std::abs(db) ? (da >= 0 ? a : -a) : (db >= 0 ? b : -b);
  }
  // the triangle at v whose corner holds direction d (projected into it); -1 if none
  int64_t sector(uint32_t v, const vec3& d, const std::vector<uint32_t>& F) const {
    int64_t best = -1;
    float bestScore = -1e30f;
    for (uint32_t t : F) {
      int i = local(t, v);
      if (i < 0) continue;
      const vec3& n = fn[t];
      vec3 dd = d - n * glm::dot(d, n);
      if (glm::length(dd) < 1e-20f) continue;
      // (angles from the corner's first edge, round the normal: inside when before the second edge)
      vec3 e1 = P[T[t][(i + 1) % 3]] - P[v], e2 = P[T[t][(i + 2) % 3]] - P[v];
      if (glm::length(e1) < 1e-30f || glm::length(e2) < 1e-30f) continue;
      const vec3 x = glm::normalize(e1), y = glm::cross(n, x);
      auto ang = [&](const vec3& w) {
        float a = std::atan2(glm::dot(w, y), glm::dot(w, x));
        return a < 0 ? a + 2 * kPi : a;
      };
      float a2 = ang(e2), ad = ang(dd);
      if (ad > a2 + 0.5f * (2 * kPi - a2)) ad -= 2 * kPi;  // (just before the first edge: a small negative)
      float sc = std::min(ad, a2 - ad);
      if (sc > bestScore) bestScore = sc, best = t;
    }
    return bestScore > -0.02f ? best : -1;
  }
  // (debug) neighbors that don't point back, or edges with no neighbor that have a twin
  int check(const char* when) const {
    int bad = 0, open = 0;
    std::unordered_map<uint64_t, uint32_t> half;
    for (uint32_t t = 0; t < T.size(); ++t)
      for (int k = 0; k < 3; ++k) half[(uint64_t(T[t][k]) << 32) | T[t][(k + 1) % 3]] = t;
    for (uint32_t t = 0; t < T.size(); ++t)
      for (int k = 0; k < 3; ++k) {
        int64_t u = N[t][k];
        uint32_t a = T[t][k], b = T[t][(k + 1) % 3];
        auto it = half.find((uint64_t(b) << 32) | a);
        if (u < 0) {
          if (it != half.end()) ++open;
          continue;
        }
        if (it == half.end() || it->second != (uint32_t)u) ++bad;
      }
    if (bad || open) PLOG_I("Retopology (layout): %s: %d neighbors wrong, %d missing", when, bad, open);
    return bad + open;
  }
  float angleAt(uint32_t t, int i) const {
    vec3 e1 = P[T[t][(i + 1) % 3]] - P[T[t][i]], e2 = P[T[t][(i + 2) % 3]] - P[T[t][i]];
    float l = glm::length(e1) * glm::length(e2);
    return l > 0 ? std::acos(std::clamp(glm::dot(e1, e2) / l, -1.f, 1.f)) : 0.f;
  }
};

vec3 anyPerp(const vec3& n) {
  vec3 a = std::abs(n.x) < 0.6f ? vec3(1, 0, 0) : vec3(0, 1, 0);
  return glm::normalize(a - n * glm::dot(a, n));
}

// The field from QuadriFlow's orientation step on the working triangles, held along the drawn lines and the
// open edges; per triangle a direction, and its singularity (+1 / -1).
void computeField(LM& M, const QuadLayoutInput& in, std::vector<int8_t>& triSing) {
  const size_t n0 = in.pos.size(), t0 = in.tri.size();
  qflow::Parametrizer field;
  field.V.resize(3, n0);
  for (size_t i = 0; i < n0; ++i) field.V.col(i) = Eigen::Vector3d(in.pos[i].x, in.pos[i].y, in.pos[i].z);
  field.F.resize(3, t0);
  for (size_t i = 0; i < t0; ++i) field.F.col(i) = Eigen::Vector3i(in.tri[i].x, in.tri[i].y, in.tri[i].z);
  double area = 0, edge = 0, Lsum = 0;
  for (size_t t = 0; t < t0; ++t) {
    const vec3 &a = in.pos[in.tri[t].x], &b = in.pos[in.tri[t].y], &c = in.pos[in.tri[t].z];
    double ar = 0.5 * glm::length(glm::cross(b - a, c - a));
    area += ar, Lsum += ar * in.triL[t];
    edge += glm::length(b - a) / t0;
  }
  const double Lm = area > 0 ? Lsum / area : 0.02;
  field.NormalizeMesh();
  const double ratio = Lm / std::max(edge, 1e-9);
  field.patina_normal_smoothing = in.fieldSmoothing >= 0 ? in.fieldSmoothing : std::clamp((int)std::lround(ratio * ratio * 0.5), 0, 60);
  field.flag_preserve_boundary = 1;
  field.Initialize(std::max(16, (int)std::lround(area / (Lm * Lm))));
  qflow::Hierarchy& H = field.hierarchy;
  H.clearConstraints();
  const Eigen::MatrixXd& V0 = H.mV[0];
  const Eigen::MatrixXi& F0 = H.mF;
  auto setConstraint = [&](int i, Eigen::Vector3d d) {
    Eigen::Vector3d nn = H.mN[0].col(i);
    d -= nn * nn.dot(d);
    if (d.squaredNorm() < 1e-16) return;
    H.mCO[0].col(i) = V0.col(i);
    H.mCQ[0].col(i) = d.normalized();
    H.mCQw[0][i] = H.mCOw[0][i] = 1.0;
  };
  // (the drawn line's own way near each laid edge: the laid path zigzags along the triangles)
  const double cell = Lm;
  std::unordered_map<uint64_t, std::vector<std::pair<vec3, vec3>>> grid;
  auto gkey = [&](const vec3& p, int dx, int dy, int dz) {
    int64_t x = (int64_t)std::floor(p.x / cell) + dx, y = (int64_t)std::floor(p.y / cell) + dy, z = (int64_t)std::floor(p.z / cell) + dz;
    return (uint64_t(x & 0x1FFFFF) << 42) | (uint64_t(y & 0x1FFFFF) << 21) | uint64_t(z & 0x1FFFFF);
  };
  for (size_t li = 0; li < in.lines.size(); ++li) {
    if (li < in.lineIsEdge.size() && !in.lineIsEdge[li]) continue;
    const auto& l = in.lines[li];
    for (size_t i = 0; i + 1 < l.size(); ++i) {
      vec3 a = l[i], b = l[i + 1];
      if (glm::length(b - a) <= 0) continue;
      int n = std::max(1, (int)std::ceil(glm::length(b - a) / cell));
      for (int k = 0; k <= n; ++k) grid[gkey(a + (b - a) * (float(k) / n), 0, 0, 0)].push_back({a, b});
    }
  }
  auto drawnDir = [&](const vec3& p, vec3 fallback) {
    float bd = 0.09f * float(cell * cell);
    vec3 best = fallback;
    for (int dz = -1; dz <= 1; ++dz)
      for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
          auto it = grid.find(gkey(p, dx, dy, dz));
          if (it == grid.end()) continue;
          for (auto& [a, b] : it->second) {
            vec3 e = b - a;
            if (std::abs(glm::dot(glm::normalize(e), fallback)) < 0.64f) continue;
            float t = std::clamp(glm::dot(p - a, e) / glm::dot(e, e), 0.f, 1.f);
            if (float d2 = glm::dot(a + e * t - p, a + e * t - p); d2 < bd) bd = d2, best = glm::dot(e, fallback) < 0 ? -e : e;
          }
        }
    return glm::normalize(best);
  };
  for (uint64_t k : in.lineEdges) {
    uint32_t a = uint32_t(k >> 32), b = uint32_t(k & 0xFFFFFFFF);
    if (a >= n0 || b >= n0) continue;
    vec3 e = in.pos[b] - in.pos[a];
    if (glm::dot(e, e) <= 0) continue;
    e = glm::normalize(e);
    for (uint32_t v : {a, b}) {
      vec3 d = drawnDir(in.pos[v], e);
      setConstraint((int)v, Eigen::Vector3d(d.x, d.y, d.z));
    }
  }
  for (int i = 0; i < 3 * F0.cols(); ++i) {
    if (H.mE2E[i] != -1) continue;
    int i0 = F0(i % 3, i / 3), i1 = F0((i + 1) % 3, i / 3);
    Eigen::Vector3d e = V0.col(i1) - V0.col(i0);
    if (e.squaredNorm() <= 0) continue;
    if (H.mCOw[0][i0] == 0) setConstraint(i0, e.normalized());
    if (H.mCOw[0][i1] == 0) setConstraint(i1, e.normalized());
  }
  H.propagateConstraints();
  qflow::Optimizer::optimize_orientations(H);
  const Eigen::MatrixXd& Q = H.mQ[0];
  triSing.assign(t0, 0);
  for (size_t t = 0; t < t0; ++t) {
    vec3 n = M.fn[t];
    vec3 p[3];
    for (int k = 0; k < 3; ++k) {
      uint32_t v = in.tri[t][k];
      vec3 q(Q(0, v), Q(1, v), Q(2, v));
      q -= n * glm::dot(q, n);
      p[k] = glm::length(q) > 1e-12f ? glm::normalize(q) : anyPerp(n);
    }
    auto rot = [&](vec3 x, int k) {
      for (int i = 0; i < k; ++i) x = glm::cross(n, x);
      return x;
    };
    auto bestK = [&](const vec3& from, const vec3& to) {
      int bk = 0;
      float bd = -2;
      for (int k = 0; k < 4; ++k)
        if (float d = glm::dot(rot(from, k), to); d > bd) bd = d, bk = k;
      return bk;
    };
    vec3 s = p[0] + rot(p[1], bestK(p[1], p[0])) + rot(p[2], bestK(p[2], p[0]));
    M.fd[t] = glm::length(s) > 1e-12f ? glm::normalize(s) : p[0];
    int idx = (bestK(p[0], p[1]) + bestK(p[1], p[2]) + bestK(p[2], p[0])) % 4;
    triSing[t] = idx == 3 ? 1 : idx == 1 ? -1 : 0;
  }
}

struct Trace {
  uint32_t v;
  vec3 d;
  double len = 0;
  int steps = 0;
  bool fresh = true;  // (its first step: from a singularity it leaves along d as it is)
  int64_t target = -1;  // a vertex it is to end at (joined separatrices; a path it came close to)
  int id = -1;
  std::vector<uint32_t> verts;
};

}  // namespace

QuadLayout buildQuadLayout(const QuadLayoutInput& in) {
  auto t0 = std::chrono::steady_clock::now();
  QuadLayout out;
  LM M;
  const size_t nv = in.pos.size(), nt = in.tri.size();
  M.P = in.pos;
  M.vt.assign(nv, 0), M.pathDeg.assign(nv, 0), M.sing.assign(nv, 0);
  M.T.resize(nt), M.N.assign(nt, {-1, -1, -1}), M.fd.assign(nt, vec3(0)), M.fn.resize(nt);
  M.zone = in.triZone, M.L = in.triL;
  M.zone.resize(nt, 0), M.L.resize(nt, 0.02f);
  {
    std::unordered_map<uint64_t, std::pair<uint32_t, int>> half;  // directed edge -> (triangle, k)
    half.reserve(nt * 3);
    for (uint32_t t = 0; t < nt; ++t) {
      M.T[t] = {in.tri[t].x, in.tri[t].y, in.tri[t].z};
      vec3 a = in.pos[in.tri[t].x], b = in.pos[in.tri[t].y], c = in.pos[in.tri[t].z], n = glm::cross(b - a, c - a);
      M.fn[t] = glm::length(n) > 1e-30f ? glm::normalize(n) : vec3(0, 0, 1);
      for (int k = 0; k < 3; ++k) {
        M.vt[M.T[t][k]] = t;
        half[(uint64_t(M.T[t][k]) << 32) | M.T[t][(k + 1) % 3]] = {t, k};
      }
    }
    for (uint32_t t = 0; t < nt; ++t)
      for (int k = 0; k < 3; ++k) {
        auto it = half.find((uint64_t(M.T[t][(k + 1) % 3]) << 32) | M.T[t][k]);
        if (it != half.end()) M.N[t][k] = it->second.first;
      }
  }
  // ---- the field and its singularities (a vertex put in each singular triangle)
  std::vector<int8_t> triSing;
  M.check("start");
  computeField(M, in, triSing);
  for (uint32_t t = 0; t < nt; ++t)
    for (int k = 0; k < 3; ++k) {
      uint32_t a = M.T[t][k], b = M.T[t][(k + 1) % 3];
      if (in.lineEdges.count(ekey(a, b))) M.setPath(a, b, 1);
      else if (M.N[t][k] < 0) M.setPath(a, b, 2);
    }
  // pairs of a three and a five closer than about a quad cancel (patches between them would be smaller
  // than a quad; QuadriFlow's integer step drops such pairs too)
  {
    std::vector<uint32_t> st;
    for (uint32_t t = 0; t < nt; ++t)
      if (triSing[t]) st.push_back(t);
    auto mid = [&](uint32_t t) { return (M.P[M.T[t][0]] + M.P[M.T[t][1]] + M.P[M.T[t][2]]) / 3.f; };
    std::vector<std::tuple<float, uint32_t, uint32_t>> pairs;
    for (size_t i = 0; i < st.size(); ++i)
      for (size_t j = i + 1; j < st.size(); ++j) {
        if (triSing[st[i]] + triSing[st[j]] != 0) continue;
        float d = glm::length(mid(st[i]) - mid(st[j])), Lh = std::min(M.L[st[i]], M.L[st[j]]);
        if (d < Lh) pairs.push_back({d / Lh, st[i], st[j]});
      }
    std::sort(pairs.begin(), pairs.end());
    for (auto& [d, a, b] : pairs)
      if (triSing[a] && triSing[b]) triSing[a] = triSing[b] = 0, ++out.pairsCancelled;
  }
  std::vector<std::pair<uint32_t, uint32_t>> singVerts;  // (vertex, the triangle it was put in)
  {
    // a singular triangle touching a line: the singularity put on the line (its nearest vertex there);
    // a three and a five at one vertex cancel
    std::unordered_map<uint32_t, std::pair<int, uint32_t>> onLine;
    for (uint32_t t = 0; t < nt; ++t) {
      if (!triSing[t]) continue;
      vec3 c = (M.P[M.T[t][0]] + M.P[M.T[t][1]] + M.P[M.T[t][2]]) / 3.f;
      int best = -1;
      for (int k = 0; k < 3; ++k)
        if (M.pathDeg[M.T[t][k]] > 0 && (best < 0 || glm::length(M.P[M.T[t][k]] - c) < glm::length(M.P[M.T[t][best]] - c))) best = k;
      if (best >= 0) {
        auto& e = onLine[M.T[t][best]];
        e.first += triSing[t], e.second = t;
        continue;
      }
      uint32_t v = M.splitFace(t, c);
      M.sing[v] = triSing[t];
      singVerts.push_back({v, t});
    }
    for (auto& [v, e] : onLine) {
      if (e.first == 0) continue;
      ++out.singOnLines;
      M.sing[v] = (int8_t)std::clamp(e.first, -1, 1);
      singVerts.push_back({v, e.second});
    }
  }
  M.check("singularities put in");
  std::vector<Trace> traces;
  std::vector<uint32_t> F, F2;
  // ---- separatrices: where round the singularity the field points straight out from it
  std::vector<std::vector<vec3>> sepDir(singVerts.size());
  std::unordered_map<uint32_t, int> singNo;  // singular vertex -> its number
  for (size_t si = 0; si < singVerts.size(); ++si) {
    auto [s, tOrig] = singVerts[si];
    singNo[s] = (int)si;
    const vec3 ps = M.P[s], ns = M.fn[tOrig];
    const vec3 ax = anyPerp(ns), ay = glm::cross(ns, ax);
    M.fan(s, F);
    // the triangles within three rings of s (not its own)
    std::vector<uint32_t> ring;
    {
      std::vector<uint32_t> front = {s}, seen = {s};
      for (int r = 0; r < 3; ++r) {
        std::vector<uint32_t> next;
        for (uint32_t v : front) {
          M.fan(v, F2);
          for (uint32_t g : F2) {
            if (M.local(g, s) < 0 && std::find(ring.begin(), ring.end(), g) == ring.end()) ring.push_back(g);
            for (int k = 0; k < 3; ++k)
              if (std::find(seen.begin(), seen.end(), M.T[g][k]) == seen.end()) seen.push_back(M.T[g][k]), next.push_back(M.T[g][k]);
          }
        }
        front = std::move(next);
      }
    }
    std::vector<std::pair<float, float>> smp;  // (angle round s, field angle minus it, mod a quarter turn)
    auto wrapQ = [](float a) {
      const float q = 0.5f * kPi;
      a = std::fmod(a, q);
      if (a <= -0.5f * q) a += q;
      if (a > 0.5f * q) a -= q;
      return a;
    };
    for (uint32_t g : ring) {
      vec3 c = (M.P[M.T[g][0]] + M.P[M.T[g][1]] + M.P[M.T[g][2]]) / 3.f - ps;
      c -= ns * glm::dot(c, ns);
      vec3 f = M.fd[g] - ns * glm::dot(M.fd[g], ns);
      if (glm::length(c) < 1e-20f || glm::length(f) < 1e-20f) continue;
      float th = std::atan2(glm::dot(c, ay), glm::dot(c, ax)), al = std::atan2(glm::dot(f, ay), glm::dot(f, ax));
      smp.push_back({th, wrapQ(al - th)});
    }
    std::sort(smp.begin(), smp.end());
    // smoothed round the circle (in 72 bins, the quarter-turn angles averaged as 4x angles), then the
    // directions where the field points straight out: the deepest dips of |angle| where it falls through 0,
    // as many as the singularity has
    const int want = M.sing[s] > 0 ? 3 : 5;
    const int NB = 72;
    std::vector<float> phi(NB, 0.f);
    {
      std::vector<vec2> acc(NB, vec2(0));
      for (auto& [th, ph] : smp) {
        float u = (th + kPi) / (2 * kPi) * NB;
        for (int b = 0; b < NB; ++b) {
          float db = std::abs(u - (b + 0.5f));
          db = std::min(db, NB - db);
          float w = std::exp(-db * db / (2 * 2.5f * 2.5f));
          acc[b] += w * vec2(std::cos(4 * ph), std::sin(4 * ph));
        }
      }
      for (int b = 0; b < NB; ++b) phi[b] = 0.25f * std::atan2(acc[b].y, acc[b].x);
    }
    std::vector<std::pair<float, int>> cand;  // (score, bin): falls through zero (going round), or dips
    for (int b = 0; b < NB; ++b) {
      float p0 = phi[b], p1 = phi[(b + 1) % NB], pm = phi[(b + NB - 1) % NB];
      if (p0 > 0 && p0 + wrapQ(p1 - p0) <= 0) cand.push_back({std::min(std::abs(p0), std::abs(p1)), b});
      else if (std::abs(p0) <= std::abs(p1) && std::abs(p0) <= std::abs(pm) && std::abs(p0) < 0.3f) cand.push_back({std::abs(p0) + 0.2f, b});
    }
    std::sort(cand.begin(), cand.end());
    std::vector<int> pick;
    for (auto& [sc, b] : cand) {
      if ((int)pick.size() >= want) break;
      bool close = false;
      for (int q : pick) {
        int db = std::abs(q - b);
        close |= std::min(db, NB - db) < NB * 0.6f / want;
      }
      if (!close) pick.push_back(b);
    }
    // (too few found: the rest in the widest gaps, so the singularity has its count)
    while ((int)pick.size() < want) {
      std::sort(pick.begin(), pick.end());
      int bestB = 0, bestGap = -1;
      if (pick.empty()) bestB = 0, bestGap = NB;
      for (size_t q = 0; q < pick.size(); ++q) {
        int a = pick[q], b = q + 1 < pick.size() ? pick[q + 1] : pick[0] + NB;
        if (b - a > bestGap) bestGap = b - a, bestB = (a + (b - a) / 2) % NB;
      }
      pick.push_back(bestB);
    }
    for (int b : pick) {
      float th = -kPi + (b + 0.5f) * 2 * kPi / NB;
      sepDir[si].push_back(std::cos(th) * ax + std::sin(th) * ay);
    }
    if (M.pathDeg[s] > 0) {
      // (on a line: the ways the line goes out are taken)
      std::vector<vec3> keep;
      for (auto& d : sepDir[si]) {
        bool taken = false;
        for (auto& [k2, kind] : M.path) {
          uint32_t a = uint32_t(k2 >> 32), b = uint32_t(k2 & 0xFFFFFFFF);
          if (a != s && b != s) continue;
          vec3 e = M.P[a == s ? b : a] - ps;
          if (glm::length(e) > 0 && glm::dot(glm::normalize(e), d) > std::cos(0.25f * kPi)) taken = true;
        }
        if (!taken) keep.push_back(d);
      }
      sepDir[si] = keep;
    }
    if ((int)sepDir[si].size() != want && getenv("PATINA_LAYOUT_TURNS"))
      PLOG_I("Retopology (layout): singularity %u (%d%s) with %zu separatrices found (%zu samples)", s, want, M.pathDeg[s] ? ", on a line" : "", sepDir[si].size(), smp.size());
  }
  // one step of a trace that cuts as it goes: from its vertex along the field (or straight at its target
  // when near), the edge it crosses split there. 0: on, 1: met the layout, 2: couldn't go on
  int why[4] = {0, 0, 0, 0};  // (no corner for its way, degenerate, along a layout edge already, met the layout)
  // the layout's vertices in a grid (a trace that comes close to one, alongside another path, goes to it: a
  // strip thinner than a quad along a line would fold), and which trace made each
  float cellLen = 0.02f;
  {
    std::vector<float> Ls(M.L.begin(), M.L.end());
    if (!Ls.empty()) std::nth_element(Ls.begin(), Ls.begin() + Ls.size() / 2, Ls.end()), cellLen = Ls[Ls.size() / 2];
  }
  std::unordered_map<uint64_t, std::vector<uint32_t>> nearGrid;
  std::vector<int> owner(M.P.size(), -1);
  auto gk = [&](const vec3& p3, int dx, int dy, int dz) {
    int64_t x = (int64_t)std::floor(p3.x / cellLen) + dx, y = (int64_t)std::floor(p3.y / cellLen) + dy, z = (int64_t)std::floor(p3.z / cellLen) + dz;
    return (uint64_t(x & 0x1FFFFF) << 42) | (uint64_t(y & 0x1FFFFF) << 21) | uint64_t(z & 0x1FFFFF);
  };
  for (uint32_t v = 0; v < (uint32_t)M.P.size(); ++v)
    if (M.pathDeg[v] > 0 || M.sing[v]) nearGrid[gk(M.P[v], 0, 0, 0)].push_back(v);
  int traceIds = 0;
  static const bool noSnap = getenv("PATINA_LAYOUT_SNAP") == nullptr;  // (tried: joins at a slant set off more corner repairs)
  auto cutStep = [&](Trace& tr) -> int {
    if (tr.id < 0) tr.id = traceIds++;
    const uint32_t v = tr.v;
    M.fan(v, F);
    vec3 d = tr.d;
    bool aiming = false;
    if (tr.target >= 0) {
      vec3 to = M.P[tr.target] - M.P[v];
      if (glm::length(to) < 1.5f * M.L[M.vt[v]]) d = glm::normalize(to), aiming = true;
    }
    int64_t t = M.sector(v, d, F);
    if (t < 0) return ++why[0], 2;
    if (!tr.fresh && !aiming) {
      // (the field's way nearest; where the field turns fast, near a singularity, straight on instead: a
      // trace that swapped to the way across would make a corner)
      vec3 f = M.fieldNear((uint32_t)t, d);
      if (glm::dot(f, d) > std::cos(0.2f * kPi)) {
        d = f;
        t = M.sector(v, d, F);
        if (t < 0) return ++why[0], 2;
      }
    }
    tr.fresh = false;
    const int i = M.local((uint32_t)t, v);
    const uint32_t p = M.T[t][(i + 1) % 3], q = M.T[t][(i + 2) % 3];
    const vec3& n = M.fn[t];
    vec3 dd = d - n * glm::dot(d, n);
    if (glm::length(dd) < 1e-20f) return ++why[1], 2;
    dd = glm::normalize(dd);
    const vec3 e = M.P[q] - M.P[p];
    float den = glm::dot(glm::cross(dd, e), n);
    if (std::abs(den) < 1e-30f) return ++why[1], 2;
    float w = std::clamp(glm::dot(glm::cross(dd, M.P[v] - M.P[p]), n) / den, 0.f, 1.f);
    const uint8_t crossKind = M.pathOf(p, q);
    uint32_t x;
    bool stop;
    const float snap = (int64_t)p == tr.target || (int64_t)q == tr.target ? 0.35f : 0.12f;
    if (w < snap || w > 1 - snap) {
      x = w < 0.5f ? p : q;
      stop = M.pathDeg[x] > 0 || M.sing[x] != 0;
      if (M.pathOf(v, x)) return ++why[2], 2;
    } else {
      x = M.splitEdge((uint32_t)t, (i + 1) % 3, M.P[p] + w * e);
      stop = crossKind != 0;
    }
    M.setPath(v, x, 3);
    tr.len += glm::length(M.P[x] - M.P[v]) / std::max(M.L[t], 1e-9f);
    tr.v = x, tr.d = dd, tr.verts.push_back(x);
    if (owner.size() < M.P.size()) owner.resize(M.P.size(), -1);
    if (owner[x] < 0) owner[x] = tr.id, nearGrid[gk(M.P[x], 0, 0, 0)].push_back(x);
    if (stop) return ++why[3], 1;
    if (!noSnap && tr.target < 0 && tr.len > 0.75) {
      // (another path within a third of a quad, a little ahead or beside: the trace goes to it)
      const float r = 0.33f * M.L[t];
      float bd = r * r;
      int64_t best = -1;
      for (int dz = -1; dz <= 1; ++dz)
        for (int dy = -1; dy <= 1; ++dy)
          for (int dx = -1; dx <= 1; ++dx) {
            auto it = nearGrid.find(gk(M.P[x], dx, dy, dz));
            if (it == nearGrid.end()) continue;
            for (uint32_t q2 : it->second) {
              if (q2 == x || owner[q2] == tr.id || (M.pathDeg[q2] == 0 && !M.sing[q2])) continue;
              vec3 to = M.P[q2] - M.P[x];
              if (glm::dot(to, dd) < -0.25f * r) continue;  // (not behind)
              float d2 = glm::dot(to, to);
              if (d2 >= bd) continue;
              // (alongside: the path there runs about the trace's way; one met head on is crossed as it is)
              bool along = M.sing[q2] != 0;
              M.fan(q2, F2);
              for (uint32_t ft : F2) {
                int li = M.local(ft, q2);
                for (int kk : {1, 2}) {
                  uint32_t o = M.T[ft][(li + kk) % 3];
                  if (!M.pathOf(q2, o)) continue;
                  vec3 e = M.P[o] - M.P[q2];
                  if (glm::length(e) > 0 && std::abs(glm::dot(glm::normalize(e), dd)) > 0.87f) along = true;
                }
              }
              if (along) bd = d2, best = q2;
            }
          }
      if (best >= 0) tr.target = best, ++out.snapped;
    }
    return ++tr.steps > 400000 ? 2 : 0;
  };
  // ---- separatrices joined where two singularities face each other along the field: each traced freely
  // (not cut in) until it passes a singularity close by, one of whose separatrices points back along it
  {
    float Lmed = 0;
    {
      std::vector<float> Ls(M.L.begin(), M.L.end());
      std::nth_element(Ls.begin(), Ls.begin() + Ls.size() / 2, Ls.end());
      Lmed = Ls.empty() ? 0.02f : Ls[Ls.size() / 2];
    }
    std::unordered_map<uint64_t, std::vector<int>> grid;
    auto cellKey = [&](const vec3& p, int dx, int dy, int dz) {
      int64_t x = (int64_t)std::floor(p.x / Lmed) + dx, y = (int64_t)std::floor(p.y / Lmed) + dy, z = (int64_t)std::floor(p.z / Lmed) + dz;
      return (uint64_t(x & 0x1FFFFF) << 42) | (uint64_t(y & 0x1FFFFF) << 21) | uint64_t(z & 0x1FFFFF);
    };
    for (size_t si = 0; si < singVerts.size(); ++si) grid[cellKey(M.P[singVerts[si].first], 0, 0, 0)].push_back((int)si);
    struct Join {
      float cost;
      int a, ka, b, kb;
    };
    std::vector<Join> joins;
    for (size_t si = 0; si < singVerts.size(); ++si)
      for (size_t k = 0; k < sepDir[si].size(); ++k) {
        const uint32_t s = singVerts[si].first;
        // free tracing: a point in a triangle and the edge it came in by
        M.fan(s, F);
        int64_t t = M.sector(s, sepDir[si][k], F);
        if (t < 0) continue;
        vec3 p = M.P[s], d = sepDir[si][k];
        int entry = -1;
        int fromCorner = M.local((uint32_t)t, s);
        float len = 0;
        for (int step = 0; step < 20000 && len < 60; ++step) {
          const vec3& n = M.fn[t];
          if (step > 0)
            if (vec3 f = M.fieldNear((uint32_t)t, d); glm::dot(f, d) > std::cos(0.2f * kPi)) d = f;
          vec3 dd = d - n * glm::dot(d, n);
          if (glm::length(dd) < 1e-20f) break;
          dd = glm::normalize(dd);
          int ek = -1;
          float bs = 1e30f;
          for (int kk = 0; kk < 3; ++kk) {
            if (kk == entry || (fromCorner >= 0 && kk != (fromCorner + 1) % 3)) continue;
            vec3 A = M.P[M.T[t][kk]], B = M.P[M.T[t][(kk + 1) % 3]], E = B - A;
            float den = glm::dot(glm::cross(dd, E), n);
            if (std::abs(den) < 1e-30f) continue;
            float w = glm::dot(glm::cross(dd, p - A), n) / den;
            if (w < -1e-4f || w > 1 + 1e-4f) continue;
            vec3 X = A + std::clamp(w, 0.f, 1.f) * E;
            float sd = glm::dot(X - p, dd);
            if (sd > -1e-9f && sd < bs) bs = sd, ek = kk;
          }
          if (ek < 0) break;
          vec3 A = M.P[M.T[t][ek]], B = M.P[M.T[t][(ek + 1) % 3]], E = B - A;
          float den = glm::dot(glm::cross(dd, E), n);
          float w = std::clamp(glm::dot(glm::cross(dd, p - A), n) / den, 0.f, 1.f);
          vec3 q = A + w * E;
          len += glm::length(q - p) / std::max(M.L[t], 1e-9f);
          p = q, d = dd, fromCorner = -1;
          // a singularity close by, ahead, with a separatrix pointing back along this one
          bool found = false;
          for (int dz = -1; dz <= 1 && !found; ++dz)
            for (int dy = -1; dy <= 1 && !found; ++dy)
              for (int dx = -1; dx <= 1 && !found; ++dx) {
                auto it = grid.find(cellKey(p, dx, dy, dz));
                if (it == grid.end()) continue;
                for (int sj : it->second) {
                  if (sj == (int)si) continue;
                  vec3 to = M.P[singVerts[sj].first] - p;
                  float ahead = glm::dot(to, dd), lat = glm::length(to - dd * ahead);
                  const float Lh = M.L[t];
                  if (lat > 0.5f * Lh || std::abs(ahead) > Lh) continue;
                  for (size_t kj = 0; kj < sepDir[sj].size(); ++kj)
                    if (glm::dot(sepDir[sj][kj], dd) < -0.8f) {
                      joins.push_back({len + 3 * lat / Lh, (int)si, (int)k, sj, (int)kj});
                      found = true;
                      break;
                    }
                  if (found) break;
                }
              }
          if (found) break;
          if (M.pathOf(M.T[t][ek], M.T[t][(ek + 1) % 3])) break;  // (a line or the model's edge)
          int64_t u = M.N[t][ek];
          if (u < 0) break;
          const uint32_t a = M.T[t][ek], b = M.T[t][(ek + 1) % 3];
          entry = -1;
          for (int j = 0; j < 3; ++j)
            if (M.T[u][j] == b && M.T[u][(j + 1) % 3] == a) entry = j;
          t = u;
        }
      }
    std::sort(joins.begin(), joins.end(), [](const Join& x, const Join& y) { return x.cost < y.cost; });
    std::vector<std::vector<char>> used(singVerts.size());
    for (size_t si = 0; si < singVerts.size(); ++si) used[si].assign(sepDir[si].size(), 0);
    int made = 0;
    for (auto& j : joins) {
      if (used[j.a][j.ka] || used[j.b][j.kb]) continue;
      used[j.a][j.ka] = used[j.b][j.kb] = 1;
      Trace tr;
      tr.v = singVerts[j.a].first, tr.d = sepDir[j.a][j.ka], tr.verts = {tr.v}, tr.target = singVerts[j.b].first;
      int st = 0;
      while (st == 0) st = cutStep(tr);
      if (tr.v == (uint32_t)tr.target) ++made;
      else used[j.b][j.kb] = 0;  // (it met something else first: the other one goes on its own)
      traces.push_back(std::move(tr));
    }
    out.joined = made;
    for (size_t si = 0; si < singVerts.size(); ++si)
      for (size_t k = 0; k < sepDir[si].size(); ++k)
        if (!used[si][k]) {
          Trace tr;
          tr.v = singVerts[si].first, tr.d = sepDir[si][k], tr.verts = {tr.v};
          traces.push_back(std::move(tr));
        }
  }
  const size_t firstFree = 0;
  // ---- traces from the lines' ends, corners and meetings (where the field leaves them along no line)
  {
    std::vector<uint32_t> lineVerts;
    for (uint32_t v = 0; v < (uint32_t)M.P.size(); ++v)
      if (M.pathDeg[v] > 0 && !M.sing[v]) lineVerts.push_back(v);
    std::unordered_map<uint32_t, std::vector<uint32_t>> adj;
    for (auto& [k, kind] : M.path) {
      if (kind == 3) continue;
      uint32_t a = uint32_t(k >> 32), b = uint32_t(k & 0xFFFFFFFF);
      adj[a].push_back(b), adj[b].push_back(a);
    }
    // a line's way out of v along the edge to w: to the vertex about half a quad along it
    auto along = [&](uint32_t v, uint32_t w) {
      const float reach = 0.5f * M.L[M.vt[v]];
      uint32_t prev = v, cur = w;
      for (int i = 0; i < 12 && glm::length(M.P[cur] - M.P[v]) < reach; ++i) {
        auto& nb = adj[cur];
        if (nb.size() != 2) break;
        uint32_t nx = nb[0] == prev ? nb[1] : nb[0];
        prev = cur, cur = nx;
      }
      return M.P[cur] - M.P[v];
    };
    for (uint32_t v : lineVerts) {
      auto it = adj.find(v);
      if (it == adj.end()) continue;
      auto& nb = it->second;
      M.fan(v, F);
      if (F.empty()) continue;
      vec3 n(0);
      for (uint32_t t : F) n += M.fn[t];
      n = glm::length(n) > 1e-20f ? glm::normalize(n) : M.fn[F[0]];
      auto inPlane = [&](vec3 d) {
        d -= n * glm::dot(d, n);
        return glm::length(d) > 1e-20f ? glm::normalize(d) : d;
      };
      std::vector<vec3> outs;
      for (uint32_t w : nb) outs.push_back(inPlane(along(v, w)));
      bool open = false;  // (on the model's edge)
      for (uint32_t w : nb) open |= M.pathOf(v, w) == 2;
      if (nb.size() == 2 && glm::dot(outs[0], outs[1]) < std::cos(0.75f * kPi)) continue;  // straight on
      if (nb.size() == 1 && open) continue;
      // the field's four ways at v (from a line's loose end: on along it only)
      vec3 f0 = inPlane(M.fd[F[0]]), f1 = glm::cross(n, f0);
      for (vec3 d : {f0, f1, -f0, -f1}) {
        bool taken = false;
        for (auto& o : outs) taken |= glm::dot(o, d) > std::cos(0.22f * kPi);
        if (nb.size() == 1 && glm::dot(outs[0], d) > -std::cos(0.22f * kPi)) taken = true;
        if (taken || M.sector(v, d, F) < 0) continue;
        Trace tr;
        tr.v = v, tr.d = d, tr.verts = {v}, tr.fresh = false;
        traces.push_back(tr);
      }
    }
  }
  out.traces = (int)traces.size();
  // ---- the rest a step at a time, the shortest first: the first to arrive stops the others
  {
    using QE = std::pair<double, size_t>;
    std::priority_queue<QE, std::vector<QE>, std::greater<>> pq;
    for (size_t i = firstFree; i < traces.size(); ++i)
      if (traces[i].target < 0) pq.push({0.0, i});
    size_t guard = 0;
    while (!pq.empty() && guard++ < 20000000) {
      size_t id = pq.top().second;
      pq.pop();
      int st = cutStep(traces[id]);
      if (st == 0) pq.push({traces[id].len, id});
      else if (st == 2 && traces[id].steps > 400000) ++out.tracesCut;
    }
    PLOG_I("Retopology (layout): %d separatrices joined; traces ended: %d no corner for the way, %d degenerate, %d along a layout edge, %d met the layout", out.joined,
           why[0], why[1], why[2], why[3]);
    // loose ends (a trace that couldn't go on, a line that stops short): carried on the way they go until
    // they meet the layout, so no patch has a slit in it
    for (int round = 0; round < 4; ++round) {
      std::vector<std::pair<uint32_t, uint32_t>> loose;  // (end, the vertex before it)
      for (auto& [k2, kind] : M.path) {
        uint32_t a = uint32_t(k2 >> 32), b = uint32_t(k2 & 0xFFFFFFFF);
        if (kind == 2) continue;
        for (auto [x, y] : {std::pair{a, b}, std::pair{b, a}})
          if (M.pathDeg[x] == 1 && !M.sing[x]) loose.push_back({x, y});
      }
      if (loose.empty()) break;
      int carried = 0;
      for (auto [v, u] : loose) {
        if (M.pathDeg[v] != 1) continue;
        Trace tr;
        tr.v = v, tr.d = glm::normalize(M.P[v] - M.P[u]), tr.verts = {v}, tr.fresh = false;
        int st = 0;
        while (st == 0) st = cutStep(tr);
        carried += tr.verts.size() > 1;
        traces.push_back(std::move(tr));
      }
      out.looseCarried += carried;
      if (!carried) break;
    }
  }
  M.check("traces cut in");
  for (auto& tr : traces) out.traceVerts.push_back(tr.verts);
  // the drawn lines' own way near a point (the laid lines zigzag along the working triangles)
  float Lmed2 = 0.02f;
  {
    std::vector<float> Ls(M.L.begin(), M.L.end());
    if (!Ls.empty()) std::nth_element(Ls.begin(), Ls.begin() + Ls.size() / 2, Ls.end()), Lmed2 = Ls[Ls.size() / 2];
  }
  std::unordered_map<uint64_t, std::vector<std::pair<vec3, vec3>>> drawn;
  auto dkey = [&](const vec3& p, int dx, int dy, int dz) {
    int64_t x = (int64_t)std::floor(p.x / Lmed2) + dx, y = (int64_t)std::floor(p.y / Lmed2) + dy, z = (int64_t)std::floor(p.z / Lmed2) + dz;
    return (uint64_t(x & 0x1FFFFF) << 42) | (uint64_t(y & 0x1FFFFF) << 21) | uint64_t(z & 0x1FFFFF);
  };
  for (size_t li = 0; li < in.lines.size(); ++li) {
    if (li < in.lineIsEdge.size() && !in.lineIsEdge[li]) continue;
    const auto& l = in.lines[li];
    for (size_t i = 0; i + 1 < l.size(); ++i) {
      vec3 a = l[i], b = l[i + 1];
      if (glm::length(b - a) <= 0) continue;
      int n = std::max(1, (int)std::ceil(glm::length(b - a) / Lmed2));
      for (int k = 0; k <= n; ++k) drawn[dkey(a + (b - a) * (float(k) / n), 0, 0, 0)].push_back({a, b});
    }
  }
  auto drawnTangent = [&](const vec3& p, const vec3& approx) {
    float bd = 0.36f * Lmed2 * Lmed2;
    vec3 best = approx;
    vec3 ap = glm::length(approx) > 0 ? glm::normalize(approx) : approx;
    for (int dz = -1; dz <= 1; ++dz)
      for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
          auto it = drawn.find(dkey(p, dx, dy, dz));
          if (it == drawn.end()) continue;
          for (auto& [a, b] : it->second) {
            vec3 e = b - a;
            if (std::abs(glm::dot(glm::normalize(e), ap)) < 0.5f) continue;
            float tt = std::clamp(glm::dot(p - a, e) / glm::dot(e, e), 0.f, 1.f);
            if (float d2 = glm::dot(a + e * tt - p, a + e * tt - p); d2 < bd) bd = d2, best = glm::dot(e, approx) < 0 ? -e : e;
          }
        }
    return best;
  };
  // ---- the patches; a corner inwards or a sector of a singularity with two corners gets a trace into the
  // patch, and the patches are found again (a few rounds)
  uint32_t NT = 0;
  std::vector<int> patch;
  int np = 0;
  struct Fix {
    uint32_t v;
    vec3 d;
    bool fresh;
  };
  int merges = 0;
  for (int round = 0;; ++round) {
    std::vector<Fix> fixes;
    std::vector<std::vector<std::vector<std::array<vec3, 3>>>> loopDirs;
    // the triangles between the layout's edges
    NT = (uint32_t)M.T.size();
    patch.assign(NT, -1);
    np = 0;
    for (uint32_t s = 0; s < NT; ++s) {
      if (patch[s] >= 0) continue;
      std::vector<uint32_t> stack = {s};
      patch[s] = np;
      while (!stack.empty()) {
        uint32_t t = stack.back();
        stack.pop_back();
        for (int k = 0; k < 3; ++k) {
          int64_t u = M.N[t][k];
          if (u < 0 || patch[u] >= 0 || M.pathOf(M.T[t][k], M.T[t][(k + 1) % 3])) continue;
          patch[u] = np, stack.push_back((uint32_t)u);
        }
      }
      ++np;
    }
    out.patches.assign(np, {});
    loopDirs.assign(np, {});
    auto boundary = [&](uint32_t t, int k) {
      int64_t u = M.N[t][k];
      return u < 0 || patch[u] != patch[t] || M.pathOf(M.T[t][k], M.T[t][(k + 1) % 3]) != 0;
    };
    // at each singular vertex: the sectors between the layout's edges there, the vertex's corners (3 or 5)
    // shared out between them by their angles
    std::unordered_map<uint64_t, int> sectorCorners;  // (vertex << 32 | triangle) -> corners
    for (uint32_t v = 0; v < (uint32_t)M.P.size(); ++v) {
      if (!M.sing[v]) continue;
      M.fan(v, F);
      if (F.empty()) continue;
      // (start just after a layout edge)
      size_t s0 = 0;
      for (size_t i = 0; i < F.size(); ++i) {
        uint32_t t = F[i];
        int li = M.local(t, v);
        if (M.pathOf(v, M.T[t][(li + 2) % 3])) {
          s0 = i;
          break;
        }
      }
      std::vector<std::vector<uint32_t>> sec;
      std::vector<float> ang;
      for (size_t k = 0; k < F.size(); ++k) {
        uint32_t t = F[(s0 + k) % F.size()];
        int li = M.local(t, v);
        if (sec.empty() || M.pathOf(v, M.T[t][(li + 2) % 3])) sec.emplace_back(), ang.push_back(0);
        sec.back().push_back(t), ang.back() += M.angleAt(t, li);
      }
      const int total = M.sing[v] > 0 ? 3 : 5;
      float sum = 0;
      for (float a : ang) sum += a;
      std::vector<int> c(sec.size());
      std::vector<std::pair<float, size_t>> rem;
      int given = 0;
      for (size_t k = 0; k < sec.size(); ++k) {
        float x = sum > 0 ? total * ang[k] / sum : 0;
        c[k] = (int)std::floor(x), given += c[k], rem.push_back({x - c[k], k});
      }
      std::sort(rem.rbegin(), rem.rend());
      for (size_t k = 0; k < rem.size() && given < total; ++k) c[rem[k].second]++, ++given;
      for (size_t k = 0; k < sec.size(); ++k)
        for (uint32_t t : sec[k]) sectorCorners[(uint64_t(v) << 32) | t] = c[k];
    }
    {
      std::vector<std::array<char, 3>> used(NT, {0, 0, 0});
      for (uint32_t t = 0; t < NT; ++t) {
        auto& Pt = out.patches[patch[t]];
        vec3 a = M.P[M.T[t][0]], b = M.P[M.T[t][1]], c = M.P[M.T[t][2]];
        Pt.area += 0.5 * glm::length(glm::cross(b - a, c - a));
        Pt.zone = M.zone[t];
        for (int k = 0; k < 3; ++k) {
          if (used[t][k] || !boundary(t, k)) continue;
          // a loop: from this half-edge on, turning round each vertex inside the patch to the next boundary edge
          std::vector<uint32_t> loop;
          // per loop vertex (the one after loop[i], shifted below): the triangle the loop comes in by, the
          // one it leaves by, and the field's quarter turns from the first's frame to the second's (carried
          // round the vertex inside the patch)
          std::vector<uint32_t> tIn, tOut;
          std::vector<int> carry;
          auto idxIn = [&](uint32_t tr, vec3 d) {  // d's quarter turn in tr's frame
            const vec3& n = M.fn[tr];
            d -= n * glm::dot(d, n);
            float a = std::atan2(glm::dot(d, glm::cross(n, M.fd[tr])), glm::dot(d, M.fd[tr]));
            return ((int)std::lround(a / (0.5f * kPi)) % 4 + 4) % 4;
          };
          uint32_t ct = t;
          int ck = k;
          for (int guard = 0; guard < 2000000; ++guard) {
            used[ct][ck] = 1;
            loop.push_back(M.T[ct][ck]);
            const uint32_t b2 = M.T[ct][(ck + 1) % 3];
            // round b2 inside the patch to the next boundary edge
            uint32_t rt = ct;
            int rk = (ck + 1) % 3;  // edge b2 -> next
            int g2 = 0, m = 0;
            while (!boundary(rt, rk) && g2++ < 512) {
              uint32_t u = (uint32_t)M.N[rt][rk];
              m += idxIn(u, M.fd[rt]);
              int j = M.local(u, b2);
              rt = u, rk = j;
            }
            tIn.push_back(ct), tOut.push_back(rt), carry.push_back(m);
            ct = rt, ck = rk;
            if (ct == t && ck == k) break;
            if (used[ct][ck]) break;
          }
          for (auto* v : {&tIn, &tOut})
            if (!v->empty()) std::rotate(v->rbegin(), v->rbegin() + 1, v->rend());
          if (!carry.empty()) std::rotate(carry.rbegin(), carry.rbegin() + 1, carry.rend());
          // the turn at each vertex in the field's terms (its tangents measured over about half a quad each
          // way, not past the next node: the laid lines zigzag along the working triangles)
          const size_t n = loop.size();
          auto isNode = [&](uint32_t v) { return M.pathDeg[v] != 2 || M.sing[v] != 0; };
          std::vector<int8_t> turn(n, 0);
          std::vector<float> phiAt(n, 0.f);
          std::vector<std::array<vec3, 3>> dirs(n, {vec3(0), vec3(0), vec3(0)});  // (way in, way out, normal)
          for (size_t i = 0; i < n && n >= 3; ++i) {
            const uint32_t v = loop[i];
            const float reach = 0.5f * M.L[M.vt[v]];
            size_t j = i;
            float acc = 0;
            for (size_t g = 0; g < n; ++g) {
              size_t jp = (j + n - 1) % n;
              acc += glm::length(M.P[loop[j]] - M.P[loop[jp]]);
              j = jp;
              if (acc >= reach || isNode(loop[j]) || j == i) break;
            }
            const vec3 A = M.P[loop[j]];
            j = i, acc = 0;
            for (size_t g = 0; g < n; ++g) {
              size_t jn = (j + 1) % n;
              acc += glm::length(M.P[loop[jn]] - M.P[loop[j]]);
              j = jn;
              if (acc >= reach || isNode(loop[j]) || j == i) break;
            }
            const vec3 B = M.P[loop[j]];
            if (M.sing[v]) {
              auto it = sectorCorners.find((uint64_t(v) << 32) | tIn[i]);
              turn[i] = (int8_t)(it == sectorCorners.end() ? 1 : it->second);
            if (turn[i] >= 2) {
              // (a sector of two corners: a separatrix missing there; one along the field's way into it)
              vec3 din = M.P[v] - A, dout = B - M.P[v];
              vec3 nn = M.fn[tIn[i]], inward = glm::cross(nn, glm::length(dout) > 0 ? glm::normalize(dout) : dout);
              vec3 b = (glm::length(din) > 0 ? -glm::normalize(din) : vec3(0)) + (glm::length(dout) > 0 ? glm::normalize(dout) : vec3(0));
              if (glm::length(b) < 0.3f) b = inward;
              if (glm::dot(b, inward) < 0) b = -b;
              if (glm::length(b) > 0) fixes.push_back({v, glm::normalize(b), true});
            }
              if (getenv("PATINA_LAYOUT_TURNS")) PLOG_I("    patch %d v %u singular %d: sector -> %d", patch[t], v, M.sing[v], turn[i]);
              continue;
            }
            static const bool fieldTurns = getenv("PATINA_LAYOUT_FIELDTURN") != nullptr;
            if (fieldTurns && isNode(v)) {
              // (the turn in the field's terms: each boundary edge's way as a quarter turn of the field in
              // its triangle, carried round the vertex inside the patch)
              const uint32_t vp = loop[(i + n - 1) % n], vn = loop[(i + 1) % n];
              const int kin = idxIn(tIn[i], M.P[v] - M.P[vp]), kout = idxIn(tOut[i], M.P[vn] - M.P[v]);
              int dq = ((kout - carry[i] - kin) % 4 + 4) % 4;
              if (dq == 2) {
                vec3 a1 = M.P[v] - M.P[vp], a2 = M.P[vn] - M.P[v];
                dq = glm::dot(glm::cross(a1, a2), M.fn[tIn[i]]) >= 0 ? 2 : -2;
              }
              turn[i] = (int8_t)(dq == 3 ? -1 : dq);
              phiAt[i] = turn[i] * 0.5f * kPi;
              vec3 din = M.P[v] - A, dout = B - M.P[v];
              vec3 nn = M.fn[tIn[i]];
              dirs[i] = {din, dout, nn};
              if ((turn[i] == -1 || turn[i] == -2) && glm::length(din) > 0) fixes.push_back({v, glm::normalize(din), false});
              continue;
            }
            // (the bend between the ways in and out, in the surface's plane there; along a line, its drawn way
            // at the window's ends)
            vec3 din = M.P[v] - A, dout = B - M.P[v];
            if (M.pathOf(loop[(i + n - 1) % n], v) == 1) din = drawnTangent(A, din);
            if (M.pathOf(v, loop[(i + 1) % n]) == 1) dout = drawnTangent(B, dout);
            vec3 nn = M.fn[tIn[i]] + M.fn[tOut[i]];
            nn = glm::length(nn) > 0 ? glm::normalize(nn) : M.fn[tIn[i]];
            const float phi = std::atan2(glm::dot(glm::cross(din, dout), nn), glm::dot(din, dout));
            dirs[i] = {din, dout, nn};
              const float lim = isNode(v) ? 0.25f * kPi : 0.33f * kPi;
            int d = phi > lim ? 1 : phi < -lim ? -1 : 0;
            phiAt[i] = phi;
            if (std::abs(phi) > 0.8f * kPi) d = phi > 0 ? 2 : -2;  // (a U-turn: round the tip of a slit)
            turn[i] = (int8_t)d;
            // (a corner inwards, or the tip of a slit: the way in carried on into the patch)
            if ((d == -1 || d == -2) && glm::length(din) > 0) fixes.push_back({v, glm::normalize(din), false});
            if (getenv("PATINA_LAYOUT_TURNS") && (isNode(v) || turn[i]))
              PLOG_I("    patch %d v %u node %d deg %d sing %d: phi %.0f -> %d", patch[t], v, (int)isNode(v), M.pathDeg[v], M.sing[v], phi * 180 / kPi, turn[i]);
          }
          // (a bend away from a node counts once: where it is sharpest within half a quad each way; a path
          // round a tight curve bends at each of its points)
          for (size_t i = 0; i < n && n >= 3; ++i) {
            if (!turn[i] || isNode(loop[i])) continue;
            const float reach = 0.5f * M.L[M.vt[loop[i]]];
            bool best = true;
            for (int dir : {-1, 1}) {
              size_t j = i;
              float acc = 0;
              for (size_t g = 0; g < n && best; ++g) {
                size_t jn = (j + n + dir) % n;
                acc += glm::length(M.P[loop[jn]] - M.P[loop[j]]);
                j = jn;
                if (acc > reach || isNode(loop[j]) || j == i) break;
                if (std::abs(phiAt[j]) > std::abs(phiAt[i]) || (std::abs(phiAt[j]) == std::abs(phiAt[i]) && j < i)) best = false;
              }
            }
            if (!best) {
              // (its fix too)
              for (size_t f = 0; f < fixes.size(); ++f)
                if (fixes[f].v == loop[i]) fixes.erase(fixes.begin() + (long)f), --f;
              turn[i] = 0;
            }
          }
          for (int8_t x : turn) Pt.corners += x;
          Pt.loops.push_back(std::move(loop)), Pt.turn.push_back(std::move(turn));
          loopDirs[patch[t]].push_back(std::move(dirs));
        }
      }
    }
    if (!fixes.empty() && round < 3) {
      int added = 0;
      for (auto& f : fixes) {
        Trace tr;
        tr.v = f.v, tr.d = f.d, tr.verts = {f.v}, tr.fresh = f.fresh;
        int st = 0;
        while (st == 0) st = cutStep(tr);
        added += tr.verts.size() > 1;
        traces.push_back(std::move(tr));
      }
      out.fixTraces += added;
      if (added) continue;
    }
    // two patches beside each other that would have four corners together (one has three corners, the other
    // five, say): the run of trace between them taken out, so they are one (a few rounds)
    static const bool noMerge = getenv("PATINA_LAYOUT_NOMERGE") != nullptr;
    if (noMerge || merges >= 4) break;
    struct RunAt {
      int p, l;
      size_t i0, i1;  // (loop indices of its two ends, the way the loop goes)
    };
    std::unordered_map<uint64_t, std::vector<RunAt>> runs;
    auto isNode = [&](uint32_t v) { return M.pathDeg[v] != 2 || M.sing[v] != 0; };
    for (int p = 0; p < np; ++p) {
      auto& Pt = out.patches[p];
      if (Pt.loops.size() != 1) continue;
      const auto& loop = Pt.loops[0];
      const size_t n = loop.size();
      std::vector<size_t> nodes;
      for (size_t i = 0; i < n; ++i)
        if (isNode(loop[i])) nodes.push_back(i);
      for (size_t q = 0; q < nodes.size(); ++q) {
        size_t i0 = nodes[q], i1 = nodes[(q + 1) % nodes.size()];
        if (i0 == i1) continue;
        const uint32_t u = loop[i0], w = loop[i1];
        if (M.sing[u] || M.sing[w] || M.pathDeg[u] < 3 || M.pathDeg[w] < 3) continue;
        bool trace = true;
        for (size_t i = i0; i != i1 && trace; i = (i + 1) % n) trace = M.pathOf(loop[i], loop[(i + 1) % n]) == 3;
        if (!trace) continue;
        uint64_t e1 = ekey(loop[i0], loop[(i0 + 1) % n]), e2 = ekey(loop[(i1 + n - 1) % n], loop[i1]);
        runs[std::min(e1, e2) * 1000003ull ^ std::max(e1, e2)].push_back({p, 0, i0, i1});
      }
    }
    auto bend = [&](const vec3& din, const vec3& dout, const vec3& nn, bool nodeAfter) {
      const float phi = std::atan2(glm::dot(glm::cross(din, dout), nn), glm::dot(din, dout));
      const float lim = nodeAfter ? 0.25f * kPi : 0.33f * kPi;
      return phi > lim ? 1 : phi < -lim ? -1 : 0;
    };
    std::vector<char> merged(np, 0);
    int done = 0;
    for (auto& [key, R] : runs) {
      if (R.size() != 2 || R[0].p == R[1].p || merged[R[0].p] || merged[R[1].p]) continue;
      const RunAt &a = R[0], &b = R[1];
      auto &PA = out.patches[a.p], &PB = out.patches[b.p];
      // (both four-sided already: only when one is a strip thinner than a quad, which would fold)
      static const float thinW = getenv("PATINA_LAYOUT_THIN") ? (float)atof(getenv("PATINA_LAYOUT_THIN")) : 0.4f;
      auto thin = [&](const QuadLayout::Patch& P, int p) {
        double per = 0;
        for (auto& lp : P.loops)
          for (size_t i = 0; i < lp.size(); ++i) per += glm::length(M.P[lp[(i + 1) % lp.size()]] - M.P[lp[i]]);
        float Lp = 0;
        for (uint32_t t2 = 0; t2 < NT; ++t2)
          if (patch[t2] == p) Lp = std::max(Lp, M.L[t2]);
        return per > 0 && 2 * P.area / per < thinW * Lp;
      };
      if (PA.corners == 4 && PB.corners == 4 && !thin(PA, a.p) && !thin(PB, b.p)) continue;
      const auto &la = PA.loops[0], &lb = PB.loops[0];
      if (la[a.i0] != lb[b.i1] || la[a.i1] != lb[b.i0]) continue;  // (not the same run both ways)
      const auto &da = loopDirs[a.p][0], &db = loopDirs[b.p][0];
      const uint32_t u = la[a.i0], w = la[a.i1];
      // (after: u and w lose an edge each)
      const int tu = bend(da[a.i0][0], db[b.i1][1], da[a.i0][2], M.pathDeg[u] - 1 != 2);
      const int tw = bend(db[b.i0][0], da[a.i1][1], da[a.i1][2], M.pathDeg[w] - 1 != 2);
      const int together = PA.corners - PA.turn[0][a.i0] - PA.turn[0][a.i1] + PB.corners - PB.turn[0][b.i0] - PB.turn[0][b.i1] + tu + tw;
      if (together != 4) continue;
      for (size_t i = a.i0; i != a.i1; i = (i + 1) % la.size()) {
        uint32_t x = la[i], y = la[(i + 1) % la.size()];
        if (M.path.erase(ekey(x, y))) M.pathDeg[x]--, M.pathDeg[y]--;
      }
      merged[a.p] = merged[b.p] = 1, ++done;
    }
    out.patchesMerged += done;
    ++merges;
    if (!done) break;
  }
  out.pos = M.P;
  out.tri.resize(NT);
  for (uint32_t t = 0; t < NT; ++t) out.tri[t] = glm::uvec3(M.T[t][0], M.T[t][1], M.T[t][2]);
  out.triPatch = patch, out.triZone = M.zone, out.triL = M.L, out.triDir = M.fd;
  for (auto& [k, kind] : M.path) out.pathEdges.push_back({uint32_t(k >> 32), uint32_t(k & 0xFFFFFFFF)}), out.pathKind.push_back(kind);
  for (uint32_t v = 0; v < (uint32_t)M.P.size(); ++v)
    if (M.sing[v]) out.singular.push_back(v), out.singIndex.push_back(M.sing[v]);
  out.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  int rect = 0, ring = 0, other = 0;
  for (auto& p : out.patches) {
    if (p.loops.size() == 1 && p.corners == 4) ++rect;
    else if (p.loops.size() == 2 && p.corners == 0) ++ring;
    else ++other;
  }
  int three = 0, five = 0;
  for (auto s : out.singIndex) (s > 0 ? three : five)++;
  PLOG_I("Retopology (layout): %d singularities (%d three, %d five; %d more at lines), %d traces (%d cut, %d loose ends carried on, %d from corners inwards, %d gone to a path close by; %d pairs of patches made one), %d patches: %d four-sided, %d rings, %d other; %.2f s",
         three + five, three, five, out.singOnLines, out.traces, out.tracesCut, out.looseCarried, out.fixTraces, out.snapped, out.patchesMerged, np, rect, ring, other, out.seconds);
  return out;
}

namespace {

// Points in a patch's map (its triangles laid out flat): where a point of the map falls on the surface.
struct MapLocator {
  const std::vector<vec3>* pos = nullptr;
  std::vector<glm::uvec3> tri;
  std::vector<std::array<vec2, 3>> uv;
  vec2 lo{0}, hi{0};
  float cell = 1;
  int nx = 1, ny = 1;
  std::vector<std::vector<uint32_t>> grid;
  void build() {
    lo = vec2(1e30f), hi = vec2(-1e30f);
    for (auto& t : uv)
      for (auto& q : t) lo = glm::min(lo, q), hi = glm::max(hi, q);
    float area = std::max((hi.x - lo.x) * (hi.y - lo.y), 1e-12f);
    cell = std::sqrt(area / std::max<size_t>(tri.size(), 1)) * 2;
    nx = std::clamp((int)((hi.x - lo.x) / cell) + 1, 1, 2048), ny = std::clamp((int)((hi.y - lo.y) / cell) + 1, 1, 2048);
    grid.assign(size_t(nx) * ny, {});
    for (uint32_t t = 0; t < tri.size(); ++t) {
      vec2 a = glm::min(glm::min(uv[t][0], uv[t][1]), uv[t][2]), b = glm::max(glm::max(uv[t][0], uv[t][1]), uv[t][2]);
      int x0 = std::clamp((int)((a.x - lo.x) / cell), 0, nx - 1), x1 = std::clamp((int)((b.x - lo.x) / cell), 0, nx - 1);
      int y0 = std::clamp((int)((a.y - lo.y) / cell), 0, ny - 1), y1 = std::clamp((int)((b.y - lo.y) / cell), 0, ny - 1);
      for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) grid[size_t(y) * nx + x].push_back(t);
    }
  }
  vec3 at(vec2 q) const {
    int x = std::clamp((int)((q.x - lo.x) / cell), 0, nx - 1), y = std::clamp((int)((q.y - lo.y) / cell), 0, ny - 1);
    float bestOut = 1e30f;
    vec3 best(0);
    for (int r = 0; r < 4; ++r) {
      for (int yy = y - r; yy <= y + r; ++yy)
        for (int xx = x - r; xx <= x + r; ++xx) {
          if (xx < 0 || yy < 0 || xx >= nx || yy >= ny || (std::abs(xx - x) != r && std::abs(yy - y) != r)) continue;
          for (uint32_t t : grid[size_t(yy) * nx + xx]) {
            const vec2 &a = uv[t][0], &b = uv[t][1], &c = uv[t][2];
            float den = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
            if (std::abs(den) < 1e-20f) continue;
            float l0 = ((b.y - c.y) * (q.x - c.x) + (c.x - b.x) * (q.y - c.y)) / den;
            float l1 = ((c.y - a.y) * (q.x - c.x) + (a.x - c.x) * (q.y - c.y)) / den, l2 = 1 - l0 - l1;
            float out = std::max({-l0, -l1, -l2, 0.f});
            if (out < bestOut) {
              bestOut = out;
              l0 = std::max(l0, 0.f), l1 = std::max(l1, 0.f), l2 = std::max(l2, 0.f);
              float sum = l0 + l1 + l2;
              best = (l0 * (*pos)[tri[t].x] + l1 * (*pos)[tri[t].y] + l2 * (*pos)[tri[t].z]) / std::max(sum, 1e-20f);
            }
          }
        }
      if (bestOut <= 1e-4f) break;
    }
    return best;
  }
};

}  // namespace

LayoutQuads quadsFromLayout(const QuadLayout& L, const LayoutQuadsOptions& lopt) {
  auto t0 = std::chrono::steady_clock::now();
  LayoutQuads out;
  const size_t NV = L.pos.size(), NT = L.tri.size(), NP = L.patches.size();
  // ---- the layout's graph: its edges at each vertex; nodes where it isn't a plain run
  std::unordered_map<uint64_t, uint8_t> kindOf;
  std::vector<std::vector<uint32_t>> adj(NV);
  for (size_t i = 0; i < L.pathEdges.size(); ++i) {
    uint32_t a = L.pathEdges[i][0], b = L.pathEdges[i][1];
    kindOf[ekey(a, b)] = L.pathKind[i];
    adj[a].push_back(b), adj[b].push_back(a);
  }
  std::vector<char> node(NV, 0);
  for (size_t v = 0; v < NV; ++v) node[v] = !adj[v].empty() && adj[v].size() != 2;
  for (uint32_t v : L.singular) node[v] = 1;
  std::vector<float> vL(NV, 0.f);
  for (size_t t = 0; t < NT; ++t)
    for (int k = 0; k < 3; ++k) vL[L.tri[t][k]] = std::max(vL[L.tri[t][k]], L.triL[t]);
  std::vector<std::vector<uint32_t>> trisOf(NP);
  for (uint32_t t = 0; t < NT; ++t) trisOf[L.triPatch[t]].push_back(t);
  // ---- each patch's corners (indices into its loop): four for a grid; three, five or six for the
  // midpoint pattern (a pole in the middle, a quad grid in each corner); else four chosen
  struct PInfo {
    int kind = 0;  // 0: left open, 1: a grid, 2: the midpoint pattern
    int k = 4;     // corners
    std::vector<size_t> corner;
    bool forced = false;
    float Lp = 0;
  };
  // four corners that spread round a patch's loop best: from the corners it has, with more at the middles
  // of the longest stretches between them when it has fewer
  auto chooseFour = [&](size_t p, const std::vector<size_t>& cand) {
    const auto& loop = L.patches[p].loops[0];
    const size_t n = loop.size();
    std::vector<double> arc(n + 1, 0);
    for (size_t i = 0; i < n; ++i) arc[i + 1] = arc[i] + glm::length(L.pos[loop[(i + 1) % n]] - L.pos[loop[i]]);
    const double per = arc[n];
    std::vector<size_t> pick;
    if (cand.size() >= 4) {
      double best = -1;
      const size_t C = std::min<size_t>(cand.size(), 14);
      std::vector<size_t> cs(cand.begin(), cand.begin() + (long)C);
      for (size_t a = 0; a < C; ++a)
        for (size_t b = a + 1; b < C; ++b)
          for (size_t c = b + 1; c < C; ++c)
            for (size_t d = c + 1; d < C; ++d) {
              double g = std::min({arc[cs[b]] - arc[cs[a]], arc[cs[c]] - arc[cs[b]], arc[cs[d]] - arc[cs[c]], per - arc[cs[d]] + arc[cs[a]]});
              if (g > best) best = g, pick = {cs[a], cs[b], cs[c], cs[d]};
            }
    } else {
      pick = cand;
      while (pick.size() < 4) {
        std::sort(pick.begin(), pick.end());
        double bestGap = -1, at = 0;
        if (pick.empty()) bestGap = per, at = 0;
        for (size_t q = 0; q < pick.size(); ++q) {
          double a = arc[pick[q]], b = q + 1 < pick.size() ? arc[pick[q + 1]] : arc[pick[0]] + per;
          if (b - a > bestGap) bestGap = b - a, at = std::fmod(a + 0.5 * (b - a), per);
        }
        size_t bi = 0;
        double bd = 1e30;
        for (size_t i = 0; i < n; ++i) {
          double d = std::abs(arc[i] - at);
          d = std::min(d, per - d) - (node[loop[i]] ? 0.25 * vL[loop[i]] : 0.0);  // (a node nearby, rather)
          if (d < bd && std::find(pick.begin(), pick.end(), i) == pick.end()) bd = d, bi = i;
        }
        pick.push_back(bi);
      }
    }
    std::sort(pick.begin(), pick.end());
    return pick;
  };
  std::vector<PInfo> pinfo(NP);
  for (size_t p = 0; p < NP; ++p) {
    for (uint32_t t : trisOf[p]) pinfo[p].Lp = std::max(pinfo[p].Lp, L.triL[t]);
    const auto& P = L.patches[p];
    if (P.loops.size() != 1 || P.loops[0].size() < 4) {
      ++out.whyLeft[0];
      if (getenv("PATINA_LAYOUT_PATCHES")) {
        std::string d;
        for (size_t l = 0; l < P.loops.size(); ++l) {
          int c = 0;
          for (int8_t x : P.turn[l]) c += x;
          d += std::to_string(P.loops[l].size()) + "v/" + std::to_string(c) + "c ";
        }
        vec3 c(0);
        for (uint32_t t : trisOf[p]) c += L.pos[L.tri[t].x] / float(trisOf[p].size());
        PLOG_I("  patch %zu at %.3f %.3f %.3f: loops %s(%.1f quads)", p, c.x, c.y, c.z, d.c_str(), pinfo[p].Lp > 0 ? P.area / (pinfo[p].Lp * pinfo[p].Lp) : 0.0);
      }
      continue;
    }
    const auto& loop = P.loops[0];
    const auto& turn = P.turn[0];
    const size_t n = loop.size();
    {  // (a vertex twice in the loop: a slit or a pinch, not a disc to map)
      std::vector<uint32_t> sorted(loop);
      std::sort(sorted.begin(), sorted.end());
      if (auto it = std::adjacent_find(sorted.begin(), sorted.end()); it != sorted.end()) {
        ++out.whyLeft[1];
        if (getenv("PATINA_LAYOUT_PATCHES")) {
          const uint32_t v = *it;
          PLOG_I("  patch %zu: vertex %u twice in its loop (at %.3f %.3f %.3f, %zu layout edges, singular %d)", p, v, L.pos[v].x, L.pos[v].y, L.pos[v].z,
                 adj[v].size(), (int)(std::find(L.singular.begin(), L.singular.end(), v) != L.singular.end()));
        }
        continue;
      }
    }
    // (a corner of two at one point, a thin wedge's tip where a trace meets a line at a shallow angle, is
    // one corner here)
    std::vector<size_t> cand;
    bool plain = true;
    for (size_t i = 0; i < n; ++i) {
      if (turn[i] > 0) cand.push_back(i);
      plain &= turn[i] >= 0 && turn[i] <= 2;
    }
    const int sum = (int)cand.size();
    static const bool noPatterns = getenv("PATINA_LAYOUT_NOPATTERN") != nullptr;
    PInfo& pi = pinfo[p];
    pi.kind = 1;
    // (a thin wedge: three corners, one of them a tip of two, narrower than half a quad: four corners, the
    // tip's side one edge long, so it can fold away rather than hold a squeezed pattern)
    size_t tip = n;
    int tips = 0;
    for (size_t c : cand)
      if (turn[c] == 2) tip = c, ++tips;
    double perim = 0;
    for (size_t i = 0; i < n; ++i) perim += glm::length(L.pos[loop[(i + 1) % n]] - L.pos[loop[i]]);
    const bool thinWedge = plain && cand.size() == 3 && tips == 1 && perim > 0 && 2 * P.area / perim < 0.5 * pi.Lp;
    static const bool wedgeFold = getenv("PATINA_LAYOUT_WEDGE") != nullptr;  // (tried: worse, the strips can't all fold)
    if (wedgeFold && thinWedge && std::find(cand.begin(), cand.end(), (tip + 1) % n) == cand.end()) {
      pi.corner = cand;
      pi.corner.push_back((tip + 1) % n);
      std::sort(pi.corner.begin(), pi.corner.end());
      ++out.wedges;
    } else if (plain && sum == (int)cand.size() && sum == 4) {
      pi.corner = cand;
    } else if (!noPatterns && plain && sum == (int)cand.size() && (sum == 3 || sum == 5 || sum == 6)) {
      pi.kind = 2, pi.k = sum, pi.corner = cand;
    } else {
      pi.forced = true;
      pi.corner = chooseFour(p, cand);
    }
    for (size_t c : pi.corner) node[loop[c]] = 1;
  }
  if (getenv("PATINA_LAYOUT_PATCHES"))
    for (size_t p = 0; p < NP; ++p) {
      vec3 c(0);
      for (uint32_t t : trisOf[p]) c += (L.pos[L.tri[t].x] + L.pos[L.tri[t].y] + L.pos[L.tri[t].z]) / float(3 * trisOf[p].size());
      std::string tn;
      for (auto& tl : L.patches[p].turn)
        for (int8_t x : tl)
          if (x) tn += std::to_string((int)x) + " ";
      PLOG_I("  patch %zu at %.3f %.3f %.3f: %zu loops, %.1f quads, kind %d k %d forced %d; turns %s", p, c.x, c.y, c.z, L.patches[p].loops.size(),
             pinfo[p].Lp > 0 ? L.patches[p].area / (pinfo[p].Lp * pinfo[p].Lp) : 0.0, pinfo[p].kind, pinfo[p].k, (int)pinfo[p].forced, tn.c_str());
    }
  // ---- the arcs: runs of the layout's edges between nodes (a loop with no node gets one)
  struct Arc {
    std::vector<uint32_t> v;
    double len = 0, tlen = 0;  // (along its vertices; smoothed, for the count)
    float L = 0;
    int count = 1;
  };
  std::vector<Arc> arcs;
  std::unordered_map<uint64_t, std::pair<int, bool>> arcOf;  // directed edge (a << 32 | b) -> (arc, along it)
  auto walk = [&](uint32_t a, uint32_t b) {
    Arc A;
    A.v = {a, b};
    uint32_t prev = a, cur = b;
    for (size_t guard = 0; guard < NV && !node[cur] && cur != a; ++guard) {
      uint32_t nx = adj[cur][0] == prev ? adj[cur][1] : adj[cur][0];
      prev = cur, cur = nx, A.v.push_back(cur);
    }
    const int id = (int)arcs.size();
    for (size_t i = 0; i + 1 < A.v.size(); ++i) {
      arcOf[(uint64_t(A.v[i]) << 32) | A.v[i + 1]] = {id, true};
      arcOf[(uint64_t(A.v[i + 1]) << 32) | A.v[i]] = {id, false};
      A.len += glm::length(L.pos[A.v[i + 1]] - L.pos[A.v[i]]);
      A.L += 0.5f * (vL[A.v[i]] + vL[A.v[i + 1]]) * glm::length(L.pos[A.v[i + 1]] - L.pos[A.v[i]]);
    }
    A.L = A.len > 0 ? A.L / (float)A.len : vL[a];
    {
      // (its length for the count: of the run smoothed a little; a line laid along the working triangles
      // zigzags and is longer than drawn)
      std::vector<vec3> q;
      for (uint32_t x : A.v) q.push_back(L.pos[x]);
      for (int it = 0; it < (lopt.smoothLengths ? 6 : 0) && q.size() > 2; ++it) {
        std::vector<vec3> q2 = q;
        for (size_t i = 1; i + 1 < q.size(); ++i) q2[i] = 0.5f * q[i] + 0.25f * (q[i - 1] + q[i + 1]);
        q = std::move(q2);
      }
      A.tlen = 0;
      for (size_t i = 0; i + 1 < q.size(); ++i) A.tlen += glm::length(q[i + 1] - q[i]);
    }
    arcs.push_back(std::move(A));
  };
  for (uint32_t v = 0; v < NV; ++v)
    if (node[v])
      for (uint32_t w : adj[v])
        if (!arcOf.count((uint64_t(v) << 32) | w)) walk(v, w);
  for (uint32_t v = 0; v < NV; ++v)
    for (uint32_t w : adj[v])
      if (!arcOf.count((uint64_t(v) << 32) | w)) node[v] = 1, walk(v, w);
  out.arcs = (int)arcs.size();
  if (getenv("PATINA_LAYOUT_PATCHES")) {
    double edges = 0, target = 0, raw = 0;
    for (auto& A : arcs) edges += (double)A.v.size() - 1, target += A.tlen / std::max(A.L, 1e-9f), raw += A.len / std::max(A.L, 1e-9f);
    PLOG_I("Retopology (layout): arcs: %.0f edges along them, targets %.0f (unsmoothed %.0f)", edges, target, raw);
  }
  struct SideArc {
    int arc;
    bool fwd;
  };
  std::vector<std::vector<std::vector<SideArc>>> sides(NP);
  std::vector<std::vector<int>> half(NP);  // pattern patches: a_j, the first half of side j
  for (int attempt = 0; attempt < 4; ++attempt) {
    sides.assign(NP, {}), half.assign(NP, {});
    std::vector<std::vector<std::pair<int, int>>> usesOf(arcs.size());  // (patch, side)
    // ---- each filled patch's sides as runs of arcs (each arc once per side, the way the loop goes)
    for (size_t p = 0; p < NP; ++p) {
      if (pinfo[p].kind == 0) continue;
      const auto& loop = L.patches[p].loops[0];
      const size_t n = loop.size();
      const auto& c = pinfo[p].corner;
      const int k = (int)c.size();
      sides[p].assign(k, {});
      bool ok = true;
      for (int sd = 0; sd < k && ok; ++sd) {
        size_t i = c[sd], end = c[(sd + 1) % k];
        for (size_t guard = 0; guard <= n && i != end; ++guard) {
          uint32_t a = loop[i], b = loop[(i + 1) % n];
          auto it = arcOf.find((uint64_t(a) << 32) | b);
          if (it == arcOf.end()) {
            ok = false;
            break;
          }
          auto& S = sides[p][sd];
          if (S.empty() || S.back().arc != it->second.first) S.push_back({it->second.first, it->second.second});
          i = (i + 1) % n;
        }
      }
      if (!ok) {
        pinfo[p].kind = 0, ++out.whyLeft[2];
        continue;
      }
      for (int sd = 0; sd < k; ++sd)
        for (auto& sa : sides[p][sd]) usesOf[sa.arc].push_back({(int)p, sd});
    }
    auto sideLen = [&](size_t p, int sd) {
      double l = 0;
      for (auto& sa : sides[p][sd]) l += arcs[sa.arc].tlen;
      return l;
    };
    // ---- the counts: min-deviation flow. A grid patch has a node per direction (its opposite sides
    // balance); a pattern patch a node per side (its two halves, each the far half of the side two on, add
    // up to it); arcs on the model's edge (or beside a patch left open) go to one shared node that takes any
    // even amount
    {
      using BiMDF = Satsuma::BiMDF;
      BiMDF bimdf;
      auto X = bimdf.add_node();
      bimdf.add_edge({.u = X, .v = X, .u_head = false, .v_head = false, .cost_function = Satsuma::CostFunction::Zero{}, .lower = 0, .upper = 1 << 24});
      if (attempt == 3) bimdf.add_edge({.u = X, .v = X, .u_head = true, .v_head = true, .cost_function = Satsuma::CostFunction::Zero{}, .lower = 0, .upper = 1 << 24});
      std::vector<std::vector<BiMDF::Node>> pn(NP);
      for (size_t p = 0; p < NP; ++p) {
        if (pinfo[p].kind == 1) pn[p] = {bimdf.add_node(), bimdf.add_node()};
        if (pinfo[p].kind == 2)
          for (int j = 0; j < pinfo[p].k; ++j) pn[p].push_back(bimdf.add_node());
      }
      auto end = [&](const std::pair<int, int>& u) {
        if (pinfo[u.first].kind == 1) return std::make_pair(pn[u.first][u.second % 2], u.second < 2);
        return std::make_pair(pn[u.first][u.second], true);
      };
      static const int weighting = getenv("PATINA_LAYOUT_W") ? atoi(getenv("PATINA_LAYOUT_W")) : 0;
      auto costFor = [](double target) {
        target = std::max(target, 1e-3);
        const double w = weighting == 1 ? 1.0 / std::max(target, 1.0) : weighting == 2 ? 1.0 / std::sqrt(std::max(target, 1.0)) : 1.0;
        return Satsuma::CostFunction::QuadDeviation{.target = target, .weight = w};
      };
      std::vector<BiMDF::Edge> edgeOf(arcs.size());
      std::vector<char> inFlow(arcs.size(), 0);
      static const bool min1 = getenv("PATINA_LAYOUT_MIN1") != nullptr;
      for (size_t a = 0; a < arcs.size(); ++a) {
        auto& U = usesOf[a];
        if (U.empty()) continue;
        auto [n1, h1] = end(U[0]);
        BiMDF::Node n2 = X;
        bool h2 = true;
        if (U.size() >= 2) std::tie(n2, h2) = end(U[1]);
        // (an arc may come to nothing: a strip thinner than a quad between two traces folds away, traces
        // that meet a line close together meet it at one point; PATINA_LAYOUT_MIN1: traces only)
        const uint8_t kind = kindOf[ekey(arcs[a].v[0], arcs[a].v[1])];
        if (getenv("PATINA_LAYOUT_PATCHES") && U.size() >= 2 && U[0].first == U[1].first)
          PLOG_I("  arc %zu twice beside patch %d (sides %d, %d; kind %d)", a, U[0].first, U[0].second, U[1].second, pinfo[U[0].first].kind);
        if (getenv("PATINA_LAYOUT_PATCHES") && U.size() > 2) {
          std::string d;
          for (auto& u : U) d += std::to_string(u.first) + "/" + std::to_string(u.second) + " ";
          int inner = 0;
          for (size_t i = 1; i + 1 < arcs[a].v.size(); ++i) inner += node[arcs[a].v[i]];
          PLOG_I("  arc %zu (%zu vertices, %d nodes inside) beside patch sides %s", a, arcs[a].v.size(), inner, d.c_str());
        }
        // (a run between a three- and a five-edged pole: drawn toward nothing, w (x - t)^2 + w k x^2, so they cancel)
        auto cost = costFor(lopt.keepCounts ? double(arcs[a].v.size() - 1) : arcs[a].tlen / std::max(arcs[a].L, 1e-9f));
        if (lopt.polePull > 0) {
          int8_t s0 = 0, s1 = 0;
          for (size_t q = 0; q < L.singular.size(); ++q) {
            if (L.singular[q] == arcs[a].v.front()) s0 = L.singIndex[q];
            if (L.singular[q] == arcs[a].v.back()) s1 = L.singIndex[q];
          }
          // (close pairs only: a run of two edges or less; a longer one holds the shape between them)
          if (s0 && s1 && s0 == -s1 && arcs[a].v.size() <= 3) cost.target /= 1 + lopt.polePull, cost.weight *= 1 + lopt.polePull;
        }
        int lower = min1 && kind != 3 ? 1 : 0, upper = 4 + 4 * (int)std::ceil(arcs[a].tlen / std::max(arcs[a].L, 1e-9f));
        int held = 0;
        if (!lopt.hold.empty())
          for (uint32_t v : arcs[a].v) held += v < lopt.hold.size() && lopt.hold[v];
        if ((lopt.keepLeftRuns && U.size() < 2) || 2 * held > (int)arcs[a].v.size()) {
          // (beside a patch left open: its count held, very strongly; libSatsuma takes no fixed counts)
          const int c = (int)arcs[a].v.size() - 1;
          cost = Satsuma::CostFunction::QuadDeviation{.target = (double)c, .weight = 400.0};
          lower = 0, upper = std::max(upper, c + 2);
        }
        edgeOf[a] = bimdf.add_edge({.u = n1, .v = n2, .u_head = h1, .v_head = h2, .cost_function = cost, .lower = lower, .upper = upper});
        inFlow[a] = 1;
      }
      std::vector<std::vector<BiMDF::Edge>> halfEdge(NP);
      for (size_t p = 0; p < NP; ++p) {
        if (pinfo[p].kind != 2) continue;
        const int k = pinfo[p].k;
        for (int j = 0; j < k; ++j)
          halfEdge[p].push_back(bimdf.add_edge({.u = pn[p][j], .v = pn[p][(j - 2 + 2 * k) % k], .u_head = false, .v_head = false,
                                                .cost_function = costFor(0.5 * sideLen(p, j) / std::max(pinfo[p].Lp, 1e-9f)), .lower = attempt == 0 ? 1 : 0,
                                                .upper = 4 + 4 * (int)std::ceil(sideLen(p, j) / std::max(pinfo[p].Lp, 1e-9f))}));
      }
      // (last: each patch's balance may be off, at a high cost, through the shared node: where a corner is
      // wrong a patch's opposite sides can't be equal; it is filled with steps)
      if (attempt == 3)
        for (size_t p = 0; p < NP; ++p)
          for (auto& nd : pn[p])
            for (bool h : {true, false})
              bimdf.add_edge({.u = nd, .v = X, .u_head = h, .v_head = h, .cost_function = Satsuma::CostFunction::QuadDeviation{.target = 0, .weight = 50},
                              .lower = 0, .upper = 1 << 16});
      Satsuma::BiMDFSolverConfig cfg;
      cfg.verbosity = 0, cfg.double_cover.verbosity = 0;
      // (its first guess every count rounded to even: rounded to the nearest, a layout whose counts some
      // corner forces to nothing can't be reached from it, and the solver gives up)
      if (attempt >= 2) cfg.double_cover.evening_mode = Satsuma::EveningMode::RoundToEven;
      if (const char* wf = getenv("PATINA_LAYOUT_BIMDF")) Satsuma::write_bimdf(bimdf, std::string(wf) + "_" + std::to_string(attempt) + ".bimdf");
      try {
        auto res = Satsuma::solve_bimdf(bimdf, cfg);
        out.solveCost = res.cost;
        for (size_t a = 0; a < arcs.size(); ++a)
          arcs[a].count = inFlow[a] ? std::max(0, (*res.solution)[edgeOf[a]]) : std::max(1, (int)std::lround(arcs[a].tlen / std::max(arcs[a].L, 1e-9f)));
        for (size_t p = 0; p < NP; ++p)
          for (auto& e : halfEdge[p]) half[p].push_back((*res.solution)[e]);
      } catch (const std::exception& e) {
        // (the patterns' halves of a quad or more may not add up: then they may be nothing, the pattern's
        // middle on a side)
        bool anyPattern = false;
        for (auto& pi : pinfo) anyPattern |= pi.kind == 2;
        PLOG_I("Retopology (layout): the counts couldn't be solved (%s)%s", e.what(),
               attempt == 0 && anyPattern ? "; again with patterns' halves of no quads"
               : attempt < 2              ? "; again from counts rounded to even"
               : attempt < 3              ? "; again with patches' sides allowed to differ"
                                          : "");
        if (attempt == 3) return out;
        if (attempt == 0 && !anyPattern) attempt = 1;
        continue;
      }
    }
    break;
  }
  // ---- the vertices on the arcs (shared by the patches either side); those an arc or a patch of no
  // quads brings together made one at the end
  std::vector<uint32_t> uf;
  auto find = [&](uint32_t x) {
    while (uf[x] != x) x = uf[x] = uf[uf[x]];
    return x;
  };
  auto unite = [&](uint32_t a, uint32_t b) {
    if (uf.size() < out.pos.size()) {
      size_t o = uf.size();
      uf.resize(out.pos.size());
      for (size_t i = o; i < uf.size(); ++i) uf[i] = (uint32_t)i;
    }
    a = find(a), b = find(b);
    if (a != b) uf[std::max(a, b)] = std::min(a, b);
  };
  std::vector<char> openV;  // (on the model's own open edge)
  std::vector<int64_t> vid(NV, -1);
  auto vertexAt = [&](uint32_t v) {
    if (vid[v] < 0) vid[v] = (int64_t)out.pos.size(), out.pos.push_back(L.pos[v]);
    return (uint32_t)vid[v];
  };
  std::vector<std::vector<uint32_t>> arcPts(arcs.size());  // count + 1 points along each arc, its own way
  std::vector<std::vector<double>> arcCum(arcs.size());     // and the arc's own vertices' place along it
  for (size_t a = 0; a < arcs.size(); ++a) {
    const auto& A = arcs[a];
    std::vector<double> cum(A.v.size(), 0);
    for (size_t i = 1; i < A.v.size(); ++i) cum[i] = cum[i - 1] + glm::length(L.pos[A.v[i]] - L.pos[A.v[i - 1]]);
    arcCum[a] = cum;
    auto& pts = arcPts[a];
    if (!L.srcQuads.empty() && A.count == (int)A.v.size() - 1) {
      // (a quad mesh's run, its count as it was: its own vertices)
      for (uint32_t x : A.v) pts.push_back(vertexAt(x));
      if (kindOf[ekey(A.v[0], A.v[1])] == 2) {
        openV.resize(out.pos.size(), 0);
        for (uint32_t x : pts) openV[x] = 1;
      }
      continue;
    }
    pts.push_back(vertexAt(A.v.front()));
    for (int k = 1; k < A.count; ++k) {
      double x = A.len * k / A.count;
      size_t i = std::min<size_t>(size_t(std::upper_bound(cum.begin(), cum.end(), x) - cum.begin()), cum.size() - 1);
      double seg = cum[i] - cum[i - 1];
      vec3 q = glm::mix(L.pos[A.v[i - 1]], L.pos[A.v[i]], seg > 0 ? float((x - cum[i - 1]) / seg) : 0.f);
      pts.push_back((uint32_t)out.pos.size()), out.pos.push_back(q);
    }
    pts.push_back(vertexAt(A.v.back()));
    if (A.count == 0) unite(pts.front(), pts.back());
    if (kindOf[ekey(A.v[0], A.v[1])] == 2) {
      openV.resize(out.pos.size(), 0);
      for (uint32_t x : pts) openV[x] = 1;
    }
  }
  // a side's points, corner to corner (its count + 1)
  auto sidePts = [&](size_t p, int sd) {
    std::vector<uint32_t> r;
    for (auto& sa : sides[p][sd]) {
      const auto& pts = arcPts[sa.arc];
      const int c = arcs[sa.arc].count;
      for (int k = r.empty() ? 0 : 1; k <= c; ++k) r.push_back(sa.fwd ? pts[k] : pts[c - k]);
      if (r.empty()) r.push_back(sa.fwd ? pts[0] : pts[c]);
    }
    return r;
  };
  // ---- each filled patch: its triangles mapped onto a polygon (a rectangle for a grid, a regular k-gon for
  // the pattern; the boundary by the counts, inside harmonic), the quads' inner points found in that map
  for (size_t p = 0; p < NP; ++p) {
    const PInfo& pi = pinfo[p];
    if (pi.kind == 0) {
      ++out.patchesLeft;
      continue;
    }
    const int k = pi.k;
    std::vector<int> cnt(k, 0);
    for (int sd = 0; sd < k; ++sd)
      for (auto& sa : sides[p][sd]) cnt[sd] += arcs[sa.arc].count;
    // the map's corners, and where along each side (0..its count) a point lies
    std::vector<vec2> corner(k);
    bool stepped = false;
    if (pi.kind == 1) {
      if (cnt[0] != cnt[2] && cnt[1] != cnt[3]) {
        ++out.patchesLeft, ++out.whyLeft[3];
        continue;
      }
      stepped = cnt[0] != cnt[2] || cnt[1] != cnt[3];
      const float n = (float)std::max(cnt[0], cnt[2]), m = (float)std::max(cnt[1], cnt[3]);
      corner = {vec2(0, 0), vec2(n, 0), vec2(n, m), vec2(0, m)};
      if (!stepped && (cnt[0] == 0 || cnt[1] == 0)) {
        // (no quads: the two long sides become one)
        auto zip = [&](const std::vector<uint32_t>& A, const std::vector<uint32_t>& B) {
          for (size_t i = 0; i < A.size() && i < B.size(); ++i) unite(A[i], B[B.size() - 1 - i]);
        };
        if (cnt[0] == 0) zip(sidePts(p, 1), sidePts(p, 3));
        if (cnt[1] == 0) zip(sidePts(p, 0), sidePts(p, 2));
        ++out.patchesCollapsed;
        continue;
      }
    } else {
      bool ok = true;
      for (int j = 0; j < k; ++j) ok &= half[p][j] >= 0 && half[p][j] <= cnt[j] && cnt[j] - half[p][j] == half[p][(j + 2) % k];
      if (!ok) {
        ++out.patchesLeft, ++out.whyLeft[4];
        continue;
      }
      for (int j = 0; j < k; ++j) {
        float th = 0.5f * kPi + 2 * kPi * j / k;
        corner[j] = vec2(std::cos(th), std::sin(th));
      }
    }
    auto sideUV = [&](int sd, double x) {
      return cnt[sd] > 0 ? glm::mix(corner[sd], corner[(sd + 1) % k], float(x / cnt[sd])) : corner[sd];
    };
    std::unordered_map<uint32_t, vec2> uvB;
    for (int sd = 0; sd < k; ++sd) {
      int base = 0;
      for (auto& sa : sides[p][sd]) {
        const auto& A = arcs[sa.arc];
        for (size_t i = 0; i < A.v.size(); ++i) {
          double f = A.len > 0 ? arcCum[sa.arc][i] / A.len : 0;
          if (!sa.fwd) f = 1 - f;
          uvB[A.v[i]] = sideUV(sd, base + f * A.count);
        }
        base += A.count;
      }
    }
    // harmonic map of the inner vertices (cotangent weights, kept positive)
    const auto& T = trisOf[p];
    std::unordered_map<uint32_t, int> inner;
    for (uint32_t t : T)
      for (int kk = 0; kk < 3; ++kk)
        if (!uvB.count(L.tri[t][kk]) && !inner.count(L.tri[t][kk])) {
          int id = (int)inner.size();
          inner[L.tri[t][kk]] = id;
        }
    std::unordered_map<uint32_t, vec2> uv(uvB);
    if (!inner.empty()) {
      const int ni = (int)inner.size();
      std::vector<Eigen::Triplet<double>> trip;
      Eigen::VectorXd bu = Eigen::VectorXd::Zero(ni), bv = Eigen::VectorXd::Zero(ni);
      for (uint32_t t : T)
        for (int kk = 0; kk < 3; ++kk) {
          uint32_t a = L.tri[t][kk], b = L.tri[t][(kk + 1) % 3], c = L.tri[t][(kk + 2) % 3];
          vec3 e1 = L.pos[a] - L.pos[c], e2 = L.pos[b] - L.pos[c];
          double cr = glm::length(glm::cross(e1, e2));
          double w = 0.5 * std::clamp(cr > 1e-30 ? glm::dot(e1, e2) / cr : 0.0, 1e-3, 1e3);  // (the edge a-b, its angle at c)
          for (auto [x, y] : {std::pair{a, b}, std::pair{b, a}}) {
            auto ix = inner.find(x);
            if (ix == inner.end()) continue;
            trip.push_back({ix->second, ix->second, w});
            auto iy = inner.find(y);
            if (iy != inner.end()) trip.push_back({ix->second, iy->second, -w});
            else bu[ix->second] += w * uvB[y].x, bv[ix->second] += w * uvB[y].y;
          }
        }
      Eigen::SparseMatrix<double> A(ni, ni);
      A.setFromTriplets(trip.begin(), trip.end());
      Eigen::ConjugateGradient<Eigen::SparseMatrix<double>, Eigen::Lower | Eigen::Upper> cg;
      cg.setTolerance(1e-8);
      cg.compute(A);
      Eigen::VectorXd xu = cg.solve(bu), xv = cg.solve(bv);
      for (auto& [v, id] : inner) uv[v] = vec2((float)xu[id], (float)xv[id]);
    }
    MapLocator loc;
    loc.pos = &L.pos;
    for (uint32_t t : T) loc.tri.push_back(L.tri[t]), loc.uv.push_back({uv[L.tri[t].x], uv[L.tri[t].y], uv[L.tri[t].z]});
    loc.build();
    auto newVert = [&](vec2 q) {
      out.pos.push_back(loc.at(q));
      return (uint32_t)out.pos.size() - 1;
    };
    // a grid of n x m quads on four borders (vertex runs, the way the quads go round), the inner points
    // from the bilinear map of its corners
    auto grid = [&](int n, int m, const std::vector<uint32_t>& b0, const std::vector<uint32_t>& b1, const std::vector<uint32_t>& b2,
                    const std::vector<uint32_t>& b3, vec2 A, vec2 B, vec2 C, vec2 D) {
      // b0: A to B (n), b1: B to C (m), b2: C to D (n), b3: D to A (m)
      if (n == 0 || m == 0) {
        // (no quads: its opposite borders become one)
        auto zip = [&](const std::vector<uint32_t>& X, const std::vector<uint32_t>& Y) {
          for (size_t i = 0; i < X.size() && i < Y.size(); ++i) unite(X[i], Y[Y.size() - 1 - i]);
        };
        if (m == 0) zip(b0, b2);
        if (n == 0) zip(b1, b3);
        return;
      }
      std::vector<uint32_t> g(size_t(n + 1) * (m + 1));
      auto G = [&](int i, int j) -> uint32_t& { return g[size_t(j) * (n + 1) + i]; };
      for (int i = 0; i <= n; ++i) G(i, 0) = b0[i], G(n - i, m) = b2[i];
      for (int j = 0; j <= m; ++j) G(n, j) = b1[j], G(0, m - j) = b3[j];
      for (int j = 1; j < m; ++j)
        for (int i = 1; i < n; ++i) {
          float u = float(i) / n, v = float(j) / m;
          G(i, j) = newVert((1 - u) * (1 - v) * A + u * (1 - v) * B + u * v * C + (1 - u) * v * D);
        }
      for (int j = 0; j < m; ++j)
        for (int i = 0; i < n; ++i) out.quads.push_back({G(i, j), G(i + 1, j), G(i + 1, j + 1), G(i, j + 1)});
    };
    const size_t quadsBefore = out.quads.size();
    bool unchanged = !L.srcQuads.empty() && pi.kind == 1 && !stepped;
    for (int sd = 0; sd < k && unchanged; ++sd)
      for (auto& sa : sides[p][sd]) unchanged &= arcs[sa.arc].count == (int)arcs[sa.arc].v.size() - 1;
    if (unchanged) {
      // (a quad mesh's patch whose counts are as they were: its own quads)
      std::vector<int64_t> qs;
      for (uint32_t t : trisOf[p])
        if (int64_t q = L.triSrcQuad[t]; q >= 0 && std::find(qs.begin(), qs.end(), q) == qs.end()) qs.push_back(q);
      for (int64_t q : qs) {
        const auto& f = L.srcQuads[q];
        out.quads.push_back({vertexAt(f[0]), vertexAt(f[1]), vertexAt(f[2]), vertexAt(f[3])});
      }
      out.quadKind.resize(out.quads.size(), 0);
      ++out.patchesFilled, ++out.patchesKept;
      continue;
    }
    if (pi.kind == 1 && stepped) {
      // opposite sides of different counts (a corner was wrong): rows from one side to the other, each a
      // count in between, stepped where the count changes
      std::vector<std::vector<uint32_t>> S4(4);
      for (int j = 0; j < 4; ++j) S4[j] = sidePts(p, j);
      // (the pair that matches runs across the rows: the rows go from side 3 to side 1 when those match)
      const bool across = cnt[1] == cnt[3];
      const int rows = across ? cnt[1] : cnt[0];
      std::vector<uint32_t> first = across ? S4[0] : std::vector<uint32_t>(S4[3].rbegin(), S4[3].rend());
      std::vector<uint32_t> last = across ? std::vector<uint32_t>(S4[2].rbegin(), S4[2].rend()) : S4[1];
      const int c0 = (int)first.size() - 1, c1 = (int)last.size() - 1;
      // (side runs for the rows' ends: across, side 3 bottom to top on the left and side 1 on the right)
      std::vector<uint32_t> leftEnd = across ? std::vector<uint32_t>(S4[3].rbegin(), S4[3].rend()) : S4[0];
      std::vector<uint32_t> rightEnd = across ? S4[1] : std::vector<uint32_t>(S4[2].rbegin(), S4[2].rend());
      vec3 nrm(0);
      for (uint32_t t : T) nrm += glm::cross(L.pos[L.tri[t].y] - L.pos[L.tri[t].x], L.pos[L.tri[t].z] - L.pos[L.tri[t].x]);
      std::vector<uint32_t> prev = first;
      bool ok = rows >= 1 && (int)leftEnd.size() == rows + 1 && (int)rightEnd.size() == rows + 1;
      for (int r = 1; r <= rows && ok; ++r) {
        std::vector<uint32_t> rail;
        if (r == rows) rail = last;
        else {
          const int c = (int)std::lround(c0 + double(c1 - c0) * r / rows);
          const float vv = float(r) / rows;
          rail.push_back(leftEnd[r]);
          for (int i = 1; i < c; ++i) {
            float uu = float(i) / c;
            vec2 q = across ? glm::mix(glm::mix(corner[0], corner[1], uu), glm::mix(corner[3], corner[2], uu), vv)
                            : glm::mix(glm::mix(corner[0], corner[3], uu), glm::mix(corner[1], corner[2], uu), vv);
            rail.push_back(newVert(q));
          }
          rail.push_back(rightEnd[r]);
        }
        const size_t q0 = out.quads.size(), t0 = out.tris.size();
        if (!stepRow(out.pos, out.quads, out.tris, prev, rail, false)) {
          ok = false;
          break;
        }
        // (facing the way the patch does)
        vec3 f(0);
        for (size_t q = q0; q < out.quads.size(); ++q)
          f += glm::cross(out.pos[out.quads[q][2]] - out.pos[out.quads[q][0]], out.pos[out.quads[q][3]] - out.pos[out.quads[q][1]]);
        for (size_t t = t0; t < out.tris.size(); ++t)
          f += glm::cross(out.pos[out.tris[t][1]] - out.pos[out.tris[t][0]], out.pos[out.tris[t][2]] - out.pos[out.tris[t][0]]);
        if (glm::dot(f, nrm) < 0) {
          for (size_t q = q0; q < out.quads.size(); ++q) std::reverse(out.quads[q].begin(), out.quads[q].end());
          for (size_t t = t0; t < out.tris.size(); ++t) std::swap(out.tris[t][1], out.tris[t][2]);
        }
        prev = std::move(rail);
      }
      if (!ok) {
        ++out.patchesLeft, ++out.whyLeft[3];
        continue;
      }
      ++out.patchesStepped;
    } else if (pi.kind == 1) {
      std::vector<uint32_t> s0 = sidePts(p, 0), s1 = sidePts(p, 1), s2 = sidePts(p, 2), s3 = sidePts(p, 3);
      grid(cnt[0], cnt[1], s0, s1, s2, s3, corner[0], corner[1], corner[2], corner[3]);
    } else {
      // the midpoint pattern: a spoke from the middle to each side's split point (a_j along side j), and in
      // each corner a grid of (the far half of its side before) x (the first half of its side after)
      std::vector<std::vector<uint32_t>> S(k);
      for (int j = 0; j < k; ++j) S[j] = sidePts(p, j);
      const vec2 O(0);
      const uint32_t center = newVert(O);
      std::vector<std::vector<uint32_t>> spoke(k);  // from the split point on side j to the middle: a_{j+1} long
      std::vector<vec2> mid(k);
      for (int j = 0; j < k; ++j) {
        const int a = half[p][j], r = half[p][(j + 1) % k];
        mid[j] = sideUV(j, a);
        spoke[j].push_back(S[j][a]);
        for (int t = 1; t < r; ++t) spoke[j].push_back(newVert(glm::mix(mid[j], O, float(t) / r)));
        if (r > 0) spoke[j].push_back(center);
        else unite(S[j][a], center);  // (no spoke: the middle on the side)
      }
      for (int j = 0; j < k; ++j) {
        const int j1 = (j + 1) % k;
        const int n = cnt[j] - half[p][j], m = half[p][j1];
        std::vector<uint32_t> b0(S[j].begin() + half[p][j], S[j].end()), b1(S[j1].begin(), S[j1].begin() + m + 1), b2(spoke[j1].begin(), spoke[j1].end()),
            b3(spoke[j].rbegin(), spoke[j].rend());
        grid(n, m, b0, b1, b2, b3, mid[j], corner[j1], mid[j1], O);
      }
    }
    out.quadKind.resize(out.quads.size(), 0);
    for (size_t q = quadsBefore; q < out.quads.size(); ++q) out.quadKind[q] = (int8_t)(stepped ? 4 : pi.kind == 2 ? 2 : pi.forced ? 3 : 1);
    ++out.patchesFilled;
    out.patchesForced += pi.forced;
    out.patchesPattern += pi.kind == 2;
  }
  // (the merges: each group at its middle; quads that lost a corner out)
  if (!uf.empty() || out.patchesCollapsed) {
    unite(0, 0);
    std::vector<vec3> sum(out.pos.size(), vec3(0));
    std::vector<int> num(out.pos.size(), 0);
    for (uint32_t v = 0; v < out.pos.size(); ++v) sum[find(v)] += out.pos[v], num[find(v)]++;
    std::vector<int64_t> remap(out.pos.size(), -1);
    std::vector<vec3> np;
    for (uint32_t v = 0; v < out.pos.size(); ++v)
      if (find(v) == v) remap[v] = (int64_t)np.size(), np.push_back(sum[v] / float(num[v]));
    std::vector<std::array<uint32_t, 4>> nq;
    std::vector<int8_t> nk;
    out.quadKind.resize(out.quads.size(), 0);
    for (size_t qi = 0; qi < out.quads.size(); ++qi) {
      auto q = out.quads[qi];
      for (auto& x : q) x = (uint32_t)remap[find(x)];
      bool degenerate = false;
      for (int kk = 0; kk < 4; ++kk) degenerate |= q[kk] == q[(kk + 1) % 4] || q[kk] == q[(kk + 2) % 4];
      if (degenerate) ++out.quadsDropped;
      else nq.push_back(q), nk.push_back(out.quadKind[qi]);
    }
    out.quadKind = std::move(nk);
    std::vector<std::array<uint32_t, 3>> nt;
    for (auto t : out.tris) {
      for (auto& x : t) x = (uint32_t)remap[find(x)];
      if (t[0] != t[1] && t[1] != t[2] && t[0] != t[2]) nt.push_back(t);
    }
    out.tris = std::move(nt);
    std::vector<char> ov(np.size(), 0);
    openV.resize(remap.size(), 0);
    for (uint32_t v = 0; v < remap.size(); ++v)
      if (openV[v]) ov[remap[find(v)]] = 1;
    openV = std::move(ov);
    out.pos = std::move(np), out.quads = std::move(nq);
  }
  // holes left (a patch of a triangle or two between paths, left open; a quad mesh's triangles whose
  // runs changed count): a triangle, a quad, or capped
  {
    openV.resize(out.pos.size(), 0);
    std::unordered_map<uint32_t, std::vector<uint32_t>> next;  // the holes' half-edges: from -> to (the way the hole goes round)
    std::unordered_set<uint64_t> have;
    for (auto& q : out.quads)
      for (int kk = 0; kk < 4; ++kk) have.insert((uint64_t(q[kk]) << 32) | q[(kk + 1) % 4]);
    for (auto& t : out.tris)
      for (int kk = 0; kk < 3; ++kk) have.insert((uint64_t(t[kk]) << 32) | t[(kk + 1) % 3]);
    auto addNext = [&](uint32_t a, uint32_t b) {
      if (!have.count((uint64_t(b) << 32) | a)) next[b].push_back(a);  // (the hole runs b -> a)
    };
    for (auto& q : out.quads)
      for (int kk = 0; kk < 4; ++kk) addNext(q[kk], q[(kk + 1) % 4]);
    for (auto& t : out.tris)
      for (int kk = 0; kk < 3; ++kk) addNext(t[kk], t[(kk + 1) % 3]);
    std::unordered_set<uint64_t> usedH;
    std::vector<std::pair<uint32_t, uint32_t>> starts;
    for (auto& [a, L2] : next)
      for (uint32_t b : L2) starts.push_back({a, b});
    std::sort(starts.begin(), starts.end());
    for (auto [start, nx0] : starts) {
      if (usedH.count((uint64_t(start) << 32) | nx0)) continue;
      std::vector<uint32_t> loop = {start};
      usedH.insert((uint64_t(start) << 32) | nx0);
      uint32_t cur = nx0;
      for (int g = 0; g < 40 && cur != start; ++g) {
        loop.push_back(cur);
        auto it = next.find(cur);
        if (it == next.end()) break;
        // (where two holes touch: on along the one that isn't the model's open edge)
        int64_t pick = -1;
        for (uint32_t c : it->second)
          if (!usedH.count((uint64_t(cur) << 32) | c) && (pick < 0 || (openV[(uint32_t)pick] && !openV[c]))) pick = c;
        if (pick < 0) break;
        usedH.insert((uint64_t(cur) << 32) | (uint32_t)pick);
        cur = (uint32_t)pick;
      }
      if (cur != start || loop.size() < 3 || loop.size() > (lopt.capHoles ? 32u : 4u)) continue;
      // (the model's own open edges are left: a hole most of whose points are on one; any, unless capping)
      size_t onOpen = 0;
      for (uint32_t v : loop) onOpen += openV[v] != 0;
      if (lopt.capHoles ? onOpen * 2 > loop.size() : onOpen > 0) continue;
      if (loop.size() == 4) out.quads.push_back({loop[0], loop[1], loop[2], loop[3]});
      else if (loop.size() == 3) out.tris.push_back({loop[0], loop[1], loop[2]});
      else capLoop(out.pos, out.quads, out.tris, loop);  // (rings in from its outline, a small fill in the middle)
      ++out.holesFilled;
    }
  }
  out.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  {
    double asked = 0, askedForced = 0, askedLeft = 0;
    for (size_t p = 0; p < NP; ++p) {
      const float Lp = pinfo[p].Lp;
      double a = Lp > 0 ? L.patches[p].area / (double(Lp) * Lp) : 0;
      asked += a;
      if (pinfo[p].kind == 0) askedLeft += a;
      else if (pinfo[p].forced) askedForced += a;
    }
    PLOG_I("Retopology (layout): quads asked for %.0f (%.0f in patches with corners chosen, %.0f in those left)", asked, askedForced, askedLeft);
  }
  PLOG_I("Retopology (layout): %d small holes closed, %d thin wedges given four corners, %d patches kept as they were", out.holesFilled, out.wedges, out.patchesKept);
  PLOG_I("Retopology (layout): %d patches folded away, %d quads dropped; left: %d not one loop, %d a vertex twice, %d sides not found, %d grid counts "
         "uneven, %d pattern counts uneven",
         out.patchesCollapsed, out.quadsDropped, out.whyLeft[0], out.whyLeft[1], out.whyLeft[2], out.whyLeft[3], out.whyLeft[4]);
  PLOG_I("Retopology (layout): %d arcs; %d patches filled (%d with the midpoint pattern, %d with corners chosen, %d stepped), %d left; %zu quads; %.2f s", out.arcs,
         out.patchesFilled, out.patchesPattern, out.patchesForced, out.patchesStepped, out.patchesLeft, out.quads.size(), out.seconds);
  return out;
}

QuadLayout layoutFromQuads(const std::vector<vec3>& P, const std::vector<std::array<uint32_t, 4>>& quads,
                           const std::vector<std::array<uint32_t, 3>>& tris, const std::vector<float>& vL,
                           const std::vector<std::vector<vec3>>& lines, const std::vector<char>& lineIsEdge) {
  QuadLayout out;
  const size_t NV = P.size(), NQ = quads.size();
  out.pos = P;
  // triangles: each quad two (its diagonal 0-2), then the triangles as they are
  std::vector<int64_t> faceOfTri;  // (the quad a triangle is half of; -1 a triangle of its own)
  for (size_t q = 0; q < NQ; ++q) {
    const auto& f = quads[q];
    out.tri.push_back(glm::uvec3(f[0], f[1], f[2])), out.tri.push_back(glm::uvec3(f[0], f[2], f[3]));
    faceOfTri.push_back((int64_t)q), faceOfTri.push_back((int64_t)q);
  }
  for (auto& t : tris) out.tri.push_back(glm::uvec3(t[0], t[1], t[2])), faceOfTri.push_back(-1);
  out.srcQuads = quads, out.triSrcQuad = faceOfTri;
  const size_t NT = out.tri.size();
  out.triL.resize(NT), out.triZone.assign(NT, 0);
  for (size_t t = 0; t < NT; ++t) out.triL[t] = (vL[out.tri[t].x] + vL[out.tri[t].y] + vL[out.tri[t].z]) / 3.f;
  // the mesh's edges: the faces on each (quads and triangles), each vertex's edges
  std::unordered_map<uint64_t, std::vector<int64_t>> faces;  // edge -> quads (q) and triangles (-1 - t)
  std::vector<std::vector<uint32_t>> nbr(NV);
  auto addEdge = [&](uint32_t a, uint32_t b, int64_t f) {
    auto& L2 = faces[ekey(a, b)];
    if (L2.empty()) nbr[a].push_back(b), nbr[b].push_back(a);
    L2.push_back(f);
  };
  for (size_t q = 0; q < NQ; ++q)
    for (int k = 0; k < 4; ++k) addEdge(quads[q][k], quads[q][(k + 1) % 4], (int64_t)q);
  for (size_t t = 0; t < tris.size(); ++t)
    for (int k = 0; k < 3; ++k) addEdge(tris[t][k], tris[t][(k + 1) % 3], -1 - (int64_t)t);
  std::unordered_map<uint64_t, uint8_t> path;
  std::vector<int> pathDeg(NV, 0);
  auto setPath = [&](uint32_t a, uint32_t b, uint8_t kind) {
    auto [it, fresh] = path.emplace(ekey(a, b), kind);
    if (fresh) pathDeg[a]++, pathDeg[b]++;
  };
  std::vector<char> nearTri(NV, 0), boundaryV(NV, 0);
  for (auto& t : tris)
    for (int k = 0; k < 3; ++k) nearTri[t[k]] = 1;
  for (auto& [e, F] : faces) {
    uint32_t a = uint32_t(e >> 32), b = uint32_t(e & 0xFFFFFFFF);
    bool tri = false;
    for (int64_t f : F) tri |= f < 0;
    if (F.size() == 1) setPath(a, b, 2), boundaryV[a] = boundaryV[b] = 1;
    else if (tri) setPath(a, b, 4);  // (a triangle's edge: kept, but no open edge)
  }
  // the lines' runs: edges whose two ends lie on a drawn line, along it
  {
    float cell = 0;
    for (float l : vL) cell += l / std::max<size_t>(vL.size(), 1);
    cell = std::max(cell, 1e-6f);
    std::unordered_map<uint64_t, std::vector<std::pair<vec3, vec3>>> grid;
    auto gk = [&](const vec3& p, int dx, int dy, int dz) {
      int64_t x = (int64_t)std::floor(p.x / cell) + dx, y = (int64_t)std::floor(p.y / cell) + dy, z = (int64_t)std::floor(p.z / cell) + dz;
      return (uint64_t(x & 0x1FFFFF) << 42) | (uint64_t(y & 0x1FFFFF) << 21) | uint64_t(z & 0x1FFFFF);
    };
    for (size_t li = 0; li < lines.size(); ++li) {
      if (li < lineIsEdge.size() && !lineIsEdge[li]) continue;
      for (size_t i = 0; i + 1 < lines[li].size(); ++i) {
        vec3 a = lines[li][i], b = lines[li][i + 1];
        int n = std::max(1, (int)std::ceil(glm::length(b - a) / cell));
        for (int k = 0; k <= n; ++k) grid[gk(glm::mix(a, b, float(k) / n), 0, 0, 0)].push_back({a, b});
      }
    }
    auto nearDir = [&](const vec3& p, float r, vec3& dir) {
      float bd = r * r;
      bool found = false;
      for (int dz = -1; dz <= 1; ++dz)
        for (int dy = -1; dy <= 1; ++dy)
          for (int dx = -1; dx <= 1; ++dx) {
            auto it = grid.find(gk(p, dx, dy, dz));
            if (it == grid.end()) continue;
            for (auto& [a, b] : it->second) {
              vec3 e = b - a;
              float tt = glm::dot(e, e) > 0 ? std::clamp(glm::dot(p - a, e) / glm::dot(e, e), 0.f, 1.f) : 0.f;
              vec3 c = a + e * tt;
              if (float d2 = glm::dot(c - p, c - p); d2 < bd) bd = d2, dir = glm::length(e) > 0 ? glm::normalize(e) : e, found = true;
            }
          }
      return found;
    };
    if (!grid.empty())
      for (auto& [e, F] : faces) {
        uint32_t a = uint32_t(e >> 32), b = uint32_t(e & 0xFFFFFFFF);
        if (path.count(e)) continue;
        vec3 da, db;
        const float r = 0.2f * 0.5f * (vL[a] + vL[b]);
        if (!nearDir(P[a], r, da) || !nearDir(P[b], r, db)) continue;
        vec3 ed = P[b] - P[a];
        if (glm::length(ed) <= 0) continue;
        ed = glm::normalize(ed);
        if (std::abs(glm::dot(ed, da)) > 0.85f && std::abs(glm::dot(ed, db)) > 0.85f) setPath(a, b, 1);
      }
  }
  // the poles (interior: not four edges), and where a line or the edge turns: separatrices from them, along
  // the edges, a step at a time each, straight on through four-edged vertices
  auto quadsAt = [&](uint32_t v) {
    std::vector<int64_t> r;
    for (uint32_t w : nbr[v])
      for (int64_t f : faces[ekey(v, w)])
        if (f >= 0 && std::find(r.begin(), r.end(), f) == r.end()) r.push_back(f);
    return r;
  };
  auto quadsAtCount = [&](uint32_t v) { return quadsAt(v).size(); };
  auto shareQuad = [&](uint32_t v, uint32_t a, uint32_t b) {  // edges v-a and v-b on one quad
    for (int64_t f : faces[ekey(v, a)])
      if (f >= 0) {
        const auto& q = quads[f];
        if (std::find(q.begin(), q.end(), b) != q.end()) return true;
      }
    return false;
  };
  struct T2 {
    uint32_t prev, cur;
    bool done = false;
  };
  std::vector<T2> ts;
  for (uint32_t v = 0; v < NV; ++v) {
    if (nbr[v].empty()) continue;
    // (beside a triangle, or where the model's edge turns inwards (three quads or more there): traces along
    // its free edges, so no patch has a corner inwards)
    if (nearTri[v] || (boundaryV[v] && quadsAtCount(v) >= 3)) {
      for (uint32_t w : nbr[v])
        if (!path.count(ekey(v, w))) ts.push_back({v, w});
      continue;
    }
    const bool pole = !boundaryV[v] && nbr[v].size() != 4;
    if (pole) {
      out.singular.push_back(v), out.singIndex.push_back(nbr[v].size() < 4 ? 1 : -1);
      for (uint32_t w : nbr[v])
        if (!path.count(ekey(v, w))) ts.push_back({v, w});
      continue;
    }
    // (a line or open edge that turns here, or ends: carried on straight beyond)
    if (pathDeg[v] >= 1 && !boundaryV[v]) {
      std::vector<uint32_t> pe;
      for (uint32_t w : nbr[v])
        if (path.count(ekey(v, w))) pe.push_back(w);
      bool straight = pe.size() == 2 && !shareQuad(v, pe[0], pe[1]);
      if (straight || nbr[v].size() != 4) continue;
      for (uint32_t w : pe)
        for (uint32_t x : nbr[v])  // (the edge across from each line edge, if not taken)
          if (x != w && !shareQuad(v, w, x) && !path.count(ekey(v, x))) ts.push_back({v, x});
    }
  }
  // (who reached each vertex first: a trace met by another that came later goes on, as a motorcycle graph's)
  std::vector<int> first(NV, -1);
  for (size_t i = 0; i < ts.size(); ++i) {
    setPath(ts[i].prev, ts[i].cur, 3);
    if (first[ts[i].cur] < 0 && pathDeg[ts[i].cur] == 1) first[ts[i].cur] = (int)i;
  }
  for (bool any = true; any;) {
    any = false;
    for (size_t ti = 0; ti < ts.size(); ++ti) {
      auto& t = ts[ti];
      if (t.done) continue;
      const uint32_t c = t.cur;
      // (it stops at a pole, the edge, a triangle, or a path met (one there before it))
      if ((pathDeg[c] > 1 && first[c] != (int)ti) || nbr[c].size() != 4 || boundaryV[c] || nearTri[c]) {
        t.done = true;
        continue;
      }
      uint32_t nx = c;
      for (uint32_t x : nbr[c])
        if (x != t.prev && !shareQuad(c, t.prev, x)) nx = x;
      if (nx == c) {
        t.done = true;
        continue;
      }
      if (path.count(ekey(c, nx))) {
        t.done = true;
        continue;
      }
      const bool fresh = pathDeg[nx] == 0;
      setPath(c, nx, 3);
      if (fresh) first[nx] = (int)ti;
      t.prev = c, t.cur = nx, any = true;
    }
  }
  out.traces = (int)ts.size();
  // the patches: triangles across edges that aren't paths (a quad's diagonal never is)
  std::unordered_map<uint64_t, std::vector<uint32_t>> triAt;  // edge -> triangles
  for (uint32_t t = 0; t < NT; ++t)
    for (int k = 0; k < 3; ++k) triAt[ekey(out.tri[t][k], out.tri[t][(k + 1) % 3])].push_back(t);
  std::vector<int> patch(NT, -1);
  int np = 0;
  for (uint32_t s0 = 0; s0 < NT; ++s0) {
    if (patch[s0] >= 0) continue;
    std::vector<uint32_t> st = {s0};
    patch[s0] = np;
    while (!st.empty()) {
      uint32_t t = st.back();
      st.pop_back();
      for (int k = 0; k < 3; ++k) {
        uint64_t e = ekey(out.tri[t][k], out.tri[t][(k + 1) % 3]);
        if (path.count(e)) continue;
        for (uint32_t u : triAt[e])
          if (patch[u] < 0) patch[u] = np, st.push_back(u);
      }
    }
    ++np;
  }
  out.triPatch = patch;
  out.patches.assign(np, {});
  for (uint32_t t = 0; t < NT; ++t) {
    const vec3 &a = P[out.tri[t].x], &b = P[out.tri[t].y], &c = P[out.tri[t].z];
    out.patches[patch[t]].area += 0.5 * glm::length(glm::cross(b - a, c - a));
  }
  // each patch's boundary loops (the patch on the left), and the turn at each vertex: 2 minus the patch's
  // quads there (one quad: a corner)
  std::unordered_map<uint64_t, uint32_t> half;  // directed edge a->b -> its triangle
  for (uint32_t t = 0; t < NT; ++t)
    for (int k = 0; k < 3; ++k) half[(uint64_t(out.tri[t][k]) << 32) | out.tri[t][(k + 1) % 3]] = t;
  auto isB = [&](uint32_t t, int k) {
    uint32_t a = out.tri[t][k], b = out.tri[t][(k + 1) % 3];
    if (path.count(ekey(a, b))) return true;
    auto it = half.find((uint64_t(b) << 32) | a);
    return it == half.end() || patch[it->second] != patch[t];
  };
  std::vector<std::array<char, 3>> used(NT, {0, 0, 0});
  for (uint32_t t = 0; t < NT; ++t)
    for (int k = 0; k < 3; ++k) {
      if (used[t][k] || !isB(t, k)) continue;
      std::vector<uint32_t> loop;
      std::vector<int8_t> turn;
      uint32_t ct = t;
      int ck = k;
      for (size_t guard = 0; guard < 4 * NT + 8; ++guard) {
        used[ct][ck] = 1;
        loop.push_back(out.tri[ct][ck]);
        const uint32_t b2 = out.tri[ct][(ck + 1) % 3];
        // round b2 inside the patch to the next boundary edge, counting the quads met
        std::vector<int64_t> qs;
        auto note = [&](uint32_t tt) {
          int64_t f = faceOfTri[tt];
          if (f >= 0 && std::find(qs.begin(), qs.end(), f) == qs.end()) qs.push_back(f);
        };
        note(ct);
        uint32_t rt = ct;
        int rk = (ck + 1) % 3;
        for (int g = 0; g < 64 && !isB(rt, rk); ++g) {
          uint32_t a2 = out.tri[rt][rk], b3 = out.tri[rt][(rk + 1) % 3];
          uint32_t u = half[(uint64_t(b3) << 32) | a2];
          int j = out.tri[u][0] == b2 ? 0 : out.tri[u][1] == b2 ? 1 : 2;
          rt = u, rk = j;
          note(u);
        }
        turn.push_back((int8_t)(2 - (int)qs.size()));
        ct = rt, ck = rk;
        if (ct == t && ck == k) break;
        if (used[ct][ck]) break;
      }
      if (!turn.empty()) std::rotate(turn.rbegin(), turn.rbegin() + 1, turn.rend());
      auto& Pt = out.patches[patch[t]];
      for (int8_t x : turn) Pt.corners += x;
      Pt.loops.push_back(std::move(loop)), Pt.turn.push_back(std::move(turn));
    }
  if (getenv("PATINA_LAYOUT_PATCHES"))
    for (int p = 0; p < np; ++p)
      for (size_t l = 0; l < out.patches[p].loops.size(); ++l)
        for (size_t i = 0; i < out.patches[p].loops[l].size(); ++i)
          if (out.patches[p].turn[l][i] < 0) {
            uint32_t v = out.patches[p].loops[l][i];
            int lineE = 0, traceE = 0, openE = 0;
            for (uint32_t w : nbr[v]) {
              auto it = path.find(ekey(v, w));
              if (it == path.end()) continue;
              (it->second == 1 ? lineE : it->second == 3 ? traceE : openE)++;
            }
            PLOG_I("  patch %d: a corner inwards at %u (%.3f %.3f %.3f): %zu edges, layout edges %d line %d trace %d open; boundary %d tri %d", p, v, P[v].x, P[v].y,
                   P[v].z, nbr[v].size(), pathDeg[v], lineE, traceE, openE, (int)boundaryV[v], (int)nearTri[v]);
          }
  for (auto& [e, kind] : path) out.pathEdges.push_back({uint32_t(e >> 32), uint32_t(e & 0xFFFFFFFF)}), out.pathKind.push_back(kind);
  int rect = 0;
  for (auto& p : out.patches) rect += p.loops.size() == 1 && p.corners == 4;
  PLOG_I("Retopology (layout from quads): %zu poles, %d separatrices, %d patches (%d four-sided)", out.singular.size(), out.traces, np, rect);
  return out;
}

void dumpQuadLayout(const QuadLayout& L, const std::string& prefix) {
  if (FILE* fo = std::fopen((prefix + "_patches.obj").c_str(), "w")) {
    for (auto& p : L.pos) std::fprintf(fo, "v %f %f %f\n", p.x, p.y, p.z);
    std::vector<std::vector<uint32_t>> byPatch(L.patches.size());
    for (uint32_t t = 0; t < L.tri.size(); ++t) byPatch[L.triPatch[t]].push_back(t);
    for (size_t i = 0; i < L.patches.size(); ++i) {
      auto& P = L.patches[i];
      const char* kind = P.loops.size() == 1 && P.corners == 4 ? "rect" : P.loops.size() == 2 && P.corners == 0 ? "ring" : "other";
      std::fprintf(fo, "o p%zu_%s_c%d_l%zu\n", i, kind, P.corners, P.loops.size());
      for (uint32_t t : byPatch[i]) std::fprintf(fo, "f %u %u %u\n", L.tri[t].x + 1, L.tri[t].y + 1, L.tri[t].z + 1);
    }
    std::fclose(fo);
  }
  if (FILE* fo = std::fopen((prefix + "_paths.obj").c_str(), "w")) {
    for (auto& p : L.pos) std::fprintf(fo, "v %f %f %f\n", p.x, p.y, p.z);
    for (int kind = 1; kind <= 3; ++kind) {
      std::fprintf(fo, "o %s\n", kind == 1 ? "line" : kind == 2 ? "open" : "trace");
      for (size_t i = 0; i < L.pathEdges.size(); ++i)
        if (L.pathKind[i] == kind) std::fprintf(fo, "l %u %u\n", L.pathEdges[i][0] + 1, L.pathEdges[i][1] + 1);
    }
    std::fclose(fo);
  }
  if (FILE* fo = std::fopen((prefix + "_sing.obj").c_str(), "w")) {
    size_t base = 1;
    for (size_t i = 0; i < L.singular.size(); ++i) {
      vec3 c = L.pos[L.singular[i]];
      float r = 0.004f;
      std::fprintf(fo, "o %s\n", L.singIndex[i] > 0 ? "three" : "five");
      vec3 q[4] = {c + vec3(r, r, r), c + vec3(r, -r, -r), c + vec3(-r, r, -r), c + vec3(-r, -r, r)};
      for (auto& p : q) std::fprintf(fo, "v %f %f %f\n", p.x, p.y, p.z);
      std::fprintf(fo, "f %zu %zu %zu\nf %zu %zu %zu\nf %zu %zu %zu\nf %zu %zu %zu\n", base, base + 1, base + 2, base, base + 3, base + 1, base,
                   base + 2, base + 3, base + 1, base + 3, base + 2);
      base += 4;
    }
    std::fclose(fo);
  }
}

}  // namespace patina::retopo
