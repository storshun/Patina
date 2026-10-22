#include "core/RetopoJoin.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <unordered_map>
#include <unordered_set>

#include "core/Log.h"

namespace patina::retopo {

namespace {

using Quad = std::array<uint32_t, 4>;
using Tri = std::array<uint32_t, 3>;
using EdgeKey = uint64_t;
inline EdgeKey ekey(uint32_t a, uint32_t b) { return (uint64_t(std::min(a, b)) << 32) | std::max(a, b); }

float segDist2(const vec3& p, const vec3& a, const vec3& b, float* tOut = nullptr) {
  vec3 ab = b - a;
  float l2 = glm::dot(ab, ab);
  float t = l2 > 0 ? std::clamp(glm::dot(p - a, ab) / l2, 0.f, 1.f) : 0.f;
  vec3 d = p - (a + t * ab);
  if (tOut) *tOut = t;
  return glm::dot(d, d);
}

// Line segments in a grid, for the nearest of them to a point (within a cell or so)
struct SegGrid {
  struct Seg {
    vec3 a, b;
    int id;
    double t0, t1;  // arc length along its chain at the ends
  };
  std::vector<Seg> segs;
  float cell = 1;
  std::unordered_map<uint64_t, std::vector<uint32_t>> cells;
  static uint64_t key(glm::ivec3 c) {
    return (uint64_t(c.x & 0x1FFFFF) << 42) | (uint64_t(c.y & 0x1FFFFF) << 21) | uint64_t(c.z & 0x1FFFFF);
  }
  void build(float c) {
    cell = std::max(c, 1e-6f);
    for (uint32_t i = 0; i < segs.size(); ++i) {
      glm::ivec3 lo = glm::ivec3(glm::floor(glm::min(segs[i].a, segs[i].b) / cell));
      glm::ivec3 hi = glm::ivec3(glm::floor(glm::max(segs[i].a, segs[i].b) / cell));
      if (glm::any(glm::greaterThan(hi - lo, glm::ivec3(32)))) continue;  // (a stray, very long segment)
      for (int z = lo.z; z <= hi.z; ++z)
        for (int y = lo.y; y <= hi.y; ++y)
          for (int x = lo.x; x <= hi.x; ++x) cells[key({x, y, z})].push_back(i);
    }
  }
  struct Hit {
    int id = -1;
    float d2 = 1e30f;
    double t = 0;
  };
  template <class Allowed>
  Hit nearest(const vec3& p, Allowed&& allowed) const {
    Hit h;
    glm::ivec3 c = glm::ivec3(glm::floor(p / cell));
    for (int dz = -1; dz <= 1; ++dz)
      for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
          auto it = cells.find(key(c + glm::ivec3(dx, dy, dz)));
          if (it == cells.end()) continue;
          for (uint32_t i : it->second) {
            const Seg& s = segs[i];
            if (!allowed(s.id)) continue;
            float t;
            if (float d = segDist2(p, s.a, s.b, &t); d < h.d2) h.d2 = d, h.id = s.id, h.t = s.t0 + t * (s.t1 - s.t0);
          }
        }
    return h;
  }
};

// The boundary loops of the faces chosen, each the way its faces run along it (the face on the left).
// Where faces meet at a corner only, the loops still come out whole (perhaps two joined at that corner).
template <class QSel, class TSel>
std::vector<std::vector<uint32_t>> loopsOf(const std::vector<Quad>& quads, const std::vector<Tri>& tris, QSel&& qsel, TSel&& tsel) {
  std::unordered_map<EdgeKey, int> uses;
  auto each = [&](auto&& fn) {
    for (size_t q = 0; q < quads.size(); ++q)
      if (qsel(q))
        for (int k = 0; k < 4; ++k) fn(quads[q][k], quads[q][(k + 1) % 4]);
    for (size_t t = 0; t < tris.size(); ++t)
      if (tsel(t))
        for (int k = 0; k < 3; ++k) fn(tris[t][k], tris[t][(k + 1) % 3]);
  };
  each([&](uint32_t a, uint32_t b) { uses[ekey(a, b)]++; });
  std::unordered_map<uint32_t, std::vector<uint32_t>> next;
  std::vector<uint32_t> starts;
  each([&](uint32_t a, uint32_t b) {
    if (uses[ekey(a, b)] != 1) return;
    auto& n = next[a];
    if (n.empty()) starts.push_back(a);
    n.push_back(b);
  });
  std::sort(starts.begin(), starts.end());
  std::vector<std::vector<uint32_t>> loops;
  for (uint32_t s : starts) {
    while (!next[s].empty()) {
      std::vector<uint32_t> loop;
      uint32_t v = s;
      for (size_t guard = 0; guard < 10000000; ++guard) {
        auto& n = next[v];
        if (n.empty()) break;
        loop.push_back(v);
        uint32_t w = n.back();
        n.pop_back();
        v = w;
        if (v == s) break;
      }
      if (loop.size() >= 3) loops.push_back(std::move(loop));
    }
  }
  return loops;
}

// Faces taken out (not looked at when a new row's facing is decided)
struct OutFlags {
  const std::vector<char>* q = nullptr;
  const std::vector<char>* t = nullptr;
  bool quad(size_t i) const { return q && i < q->size() && (*q)[i]; }
  bool tri(size_t i) const { return t && i < t->size() && (*t)[i]; }
};

// The row between two runs of vertices (one each side of a chain, the same way along it): each edge of
// the side with fewer takes 1 edge of the other (a quad), 3 (a 3-to-1 step: four quads), 2 (the one odd
// step: a triangle and a quad) or, only where one side has more than three times the other's edges, 5
// (four triangles and a quad); chosen to keep the two sides level by length.
bool buildOneRow(std::vector<vec3>& P, std::vector<Quad>& quads, std::vector<int>& quadGroup, std::vector<Tri>& tris,
              std::vector<int>& triGroup, std::vector<uint32_t> F, std::vector<uint32_t> C, bool closed, std::vector<char>& bandV,
              JoinStats& st, const OutFlags& out) {
  size_t eF = closed ? F.size() : F.size() - 1, eC = closed ? C.size() : C.size() - 1;
  if (eF < eC) std::swap(F, C), std::swap(eF, eC);
  const size_t nF = F.size(), nC = C.size();
  if (eC < 1 || nF < 2) return false;
  const size_t excess = eF - eC;
  std::vector<double> arcC(eC + 1, 0), arcF(eF + 1, 0);
  for (size_t i = 0; i < eC; ++i) arcC[i + 1] = arcC[i] + glm::length(P[C[(i + 1) % nC]] - P[C[i]]);
  for (size_t i = 0; i < eF; ++i) arcF[i + 1] = arcF[i] + glm::length(P[F[(i + 1) % nF]] - P[F[i]]);
  if (arcC[eC] <= 0 || arcF[eF] <= 0) return false;
  // where along F each vertex of C is across from: the nearest of F's vertices, going forwards
  std::vector<size_t> across(eC + 1, eF);
  across[0] = 0;
  {
    const size_t window = 3 * (eF + eC - 1) / eC + 8;
    for (size_t j = 1; j < eC; ++j) {
      size_t bi = across[j - 1];
      float bdd = 1e30f;
      for (size_t i = across[j - 1]; i <= std::min(eF, across[j - 1] + window); ++i)
        if (float d = glm::dot(P[F[i % nF]] - P[C[j]], P[F[i % nF]] - P[C[j]]); d < bdd) bdd = d, bi = i;
      across[j] = bi;
    }
  }
  std::vector<int> adv(eC, 1);
  const double avgF = arcF[eF] / eF;
  if ((eF + 1) * (eC + 1) <= 3000000) {
    // the steps that keep the row's rungs (from each vertex of C across to F) shortest overall: a
    // search over (edges of F used, edges of C used, the odd step used, the last a 3-to-1 step)
    auto rung = [&](size_t i, size_t j) { return glm::length(P[F[i % nF]] - P[C[j % nC]]); };
    const size_t W = eF + 1;
    auto at = [&](size_t i, size_t j, int odd, int p3) { return ((j * W + i) * 2 + odd) * 2 + p3; };
    std::vector<float> cost((eC + 1) * W * 4, 1e30f);
    std::vector<int8_t> how((eC + 1) * W * 4, 0);
    cost[at(0, 0, 0, 0)] = rung(0, 0);
    for (size_t j = 0; j < eC; ++j)
      for (size_t i = 0; i <= eF; ++i)
        for (int odd = 0; odd < 2; ++odd)
          for (int p3 = 0; p3 < 2; ++p3) {
            float c = cost[at(i, j, odd, p3)];
            if (c >= 1e29f) continue;
            for (int a : {1, 3, 2, 5}) {
              if (i + a > eF || (a == 2 && odd)) continue;
              // (two 3-to-1 steps side by side give a vertex six edges: kept apart unless the counts need it)
              float extra = a == 5 ? 10 * (float)avgF : a == 2 ? 0.5f * (float)avgF : a == 3 && p3 ? 2 * (float)avgF : 0.f;
              size_t k = at(i + a, j + 1, odd || a == 2, a == 3);
              float n = c + rung(i + a, j + 1) + extra;
              if (n < cost[k]) cost[k] = n, how[k] = (int8_t)(a | (odd << 4) | (p3 << 5));  // (the step, and the state before)
            }
          }
    const int oddEnd = excess % 2;
    int bestP3 = cost[at(eF, eC, oddEnd, 0)] <= cost[at(eF, eC, oddEnd, 1)] ? 0 : 1;
    if (cost[at(eF, eC, oddEnd, bestP3)] >= 1e29f) return false;
    size_t i = eF;
    int odd = oddEnd, p3 = bestP3;
    for (size_t j = eC; j-- > 0;) {
      int h = how[at(i, j + 1, odd, p3)], a = h & 15;
      if (a <= 0 || (size_t)a > i) return false;
      adv[j] = a, i -= a, odd = (h >> 4) & 1, p3 = (h >> 5) & 1;
    }
    if (i != 0) return false;
  } else {
    {
      size_t fi = 0;
      bool oddLeft = excess % 2;
      int prevA = 1;
      for (size_t j = 0; j < eC; ++j) {
        const size_t rc = eC - j - 1;  // C edges left after this one
        const double want = 0.5 * (arcC[j + 1] / arcC[eC] * arcF[eF] + arcF[across[j + 1]]);
        int bestA = -1;
        double bestErr = 1e30;
        for (int a : {1, 3, 2, 5}) {
          if (a == 2 && !oddLeft) continue;
          if (fi + a > eF) continue;
          size_t remF = eF - fi - a;
          if (remF < rc) continue;
          bool odd2 = oddLeft && a != 2;
          size_t e2 = remF - rc;
          if (e2 % 2 != (odd2 ? 1u : 0u) || e2 > 4 * rc + (odd2 ? 1u : 0u)) continue;
          // (two 3-to-1 steps side by side give a vertex six edges: kept apart unless the counts need it)
          double err = std::abs(arcF[fi + a] - want) + (a == 5 ? 10 * avgF : 0) + (a == 3 && prevA == 3 ? 2 * avgF : 0);
          if (err < bestErr) bestErr = err, bestA = a;
        }
        if (bestA < 0) return false;
        adv[j] = bestA, oddLeft = oddLeft && bestA != 2, fi += bestA, prevA = bestA;
      }
      if (fi != eF) return false;
    }
  }
  // facing the same way as the faces already along F
  bool flip = false;
  {
    uint32_t x = F[0], y = F[1];
    for (size_t i = 0; i < quads.size(); ++i)
      if (!out.quad(i))
        for (int k = 0; k < 4; ++k) flip |= quads[i][k] == x && quads[i][(k + 1) % 4] == y;
    for (size_t i = 0; i < tris.size(); ++i)
      if (!out.tri(i))
        for (int k = 0; k < 3; ++k) flip |= tris[i][k] == x && tris[i][(k + 1) % 3] == y;
  }
  const size_t q0 = quads.size(), t0 = tris.size();
  bandV.resize(P.size(), 0);
  auto addV = [&](const vec3& p) {
    P.push_back(p), bandV.push_back(1);
    return (uint32_t)(P.size() - 1);
  };
  for (uint32_t v : C) bandV[v] = 1;
  size_t fi = 0;
  for (size_t j = 0; j < eC; ++j) {
    uint32_t c0 = C[j], c1 = C[(j + 1) % nC];
    auto Fv = [&](size_t k) { return F[(fi + k) % nF]; };
    int a = adv[j];
    if (a == 1) {
      quads.push_back({Fv(0), Fv(1), c1, c0}), quadGroup.push_back(-1);
    } else if (a == 3) {
      uint32_t m0 = addV(0.5f * (P[Fv(1)] + glm::mix(P[c0], P[c1], 1.f / 3)));
      uint32_t m1 = addV(0.5f * (P[Fv(2)] + glm::mix(P[c0], P[c1], 2.f / 3)));
      quads.push_back({Fv(0), Fv(1), m0, c0}), quads.push_back({Fv(1), Fv(2), m1, m0}), quads.push_back({Fv(2), Fv(3), c1, m1}),
          quads.push_back({m0, m1, c1, c0});
      for (int k = 0; k < 4; ++k) quadGroup.push_back(-1);
      ++st.steps31;
    } else {
      // a - 1 triangles fanned from c0, then a quad
      for (int k = 0; k + 1 < a; ++k) tris.push_back({Fv(k), Fv(k + 1), c0}), triGroup.push_back(-1);
      quads.push_back({Fv(a - 1), Fv(a), c1, c0}), quadGroup.push_back(-1);
      ++st.oddSteps;
    }
    fi += a;
  }
  if (flip) {
    for (size_t q = q0; q < quads.size(); ++q) std::reverse(quads[q].begin(), quads[q].end());
    for (size_t t = t0; t < tris.size(); ++t) std::swap(tris[t][1], tris[t][2]);
  }
  return true;
}


// The row, split into rows of their own where one side has more than about twice the other's edges (so
// each steps by 3-to-1 at most): the rows between have counts in between, their vertices placed
// between the two sides.
bool buildRow(std::vector<vec3>& P, std::vector<Quad>& quads, std::vector<int>& quadGroup, std::vector<Tri>& tris, std::vector<int>& triGroup,
              std::vector<uint32_t> F, std::vector<uint32_t> C, bool closed, std::vector<char>& bandV, JoinStats& st, const OutFlags& out,
              int keep = -1) {
  // keep: which of the two (0: the first, 1: the second) lies on the line and keeps it as an edge loop
  size_t eF = closed ? F.size() : F.size() - 1, eC = closed ? C.size() : C.size() - 1;
  if (eF < eC) std::swap(F, C), std::swap(eF, eC), keep = keep < 0 ? -1 : 1 - keep;
  if (eC < 1) return false;
  const double ratio = double(eF) / eC;
  int rows = ratio > 6.5 ? 3 : ratio > 2.2 ? 2 : 1;
  // the side on the line has the fewer points: steps there would put their five-edged poles on the line,
  // so a plain row goes first and the steps after it, off the line
  const bool plainFirst = keep == 1 && eF > eC && rows < 3;
  if (plainFirst) ++rows;
  if (rows == 1) return buildOneRow(P, quads, quadGroup, tris, triGroup, F, C, closed, bandV, st, out);
  bandV.resize(P.size(), 0);
  // the points at a share of the length along each side
  auto along = [&](const std::vector<uint32_t>& V, size_t e) {
    std::vector<double> arc(e + 1, 0);
    for (size_t i = 0; i < e; ++i) arc[i + 1] = arc[i] + glm::length(P[V[(i + 1) % V.size()]] - P[V[i]]);
    return arc;
  };
  std::vector<double> aF = along(F, eF), aC = along(C, eC);
  auto at = [&](const std::vector<uint32_t>& V, const std::vector<double>& arc, double s) {
    double x = s * arc.back();
    size_t i = size_t(std::upper_bound(arc.begin(), arc.end(), x) - arc.begin());
    i = std::clamp<size_t>(i, 1, arc.size() - 1);
    double seg = arc[i] - arc[i - 1], f = seg > 0 ? (x - arc[i - 1]) / seg : 0;
    return glm::mix(P[V[(i - 1) % V.size()]], P[V[i % V.size()]], (float)f);
  };
  std::vector<uint32_t> prev = F;
  for (int r = 1; r < rows; ++r) {
    const double frac = double(r) / rows;
    size_t eM = std::max<size_t>(1, (size_t)std::lround(eC * std::pow(ratio, 1.0 - frac)));
    if (plainFirst && r == rows - 1) eM = eC;  // (the row next to the line: as many as it has)
    std::vector<uint32_t> M;
    for (size_t i = 0; i < (closed ? eM : eM + 1); ++i) {
      double sM = double(i) / eM;
      P.push_back(glm::mix(at(F, aF, sM), at(C, aC, sM), (float)frac)), bandV.push_back(1);
      M.push_back((uint32_t)(P.size() - 1));
    }
    if (!buildOneRow(P, quads, quadGroup, tris, triGroup, prev, M, closed, bandV, st, out)) return false;
    prev = std::move(M);
  }
  return buildOneRow(P, quads, quadGroup, tris, triGroup, prev, C, closed, bandV, st, out);
}

// A small hole filled with quads (and a triangle when its count is odd), cut off its outline one at a
// time where the cut is shortest. V: the hole's outline the way the new faces run.
void clipHole(std::vector<uint32_t> V, const std::vector<vec3>& P, std::vector<Quad>& quads, std::vector<int>& quadGroup,
              std::vector<Tri>& tris, std::vector<int>& triGroup) {
  auto len = [&](uint32_t a, uint32_t b) { return glm::length(P[a] - P[b]); };
  // the way the hole faces (Newell's normal of its outline): a cut whose face turns the other way is
  // taken only when there is no other
  vec3 ref(0);
  for (size_t i = 0; i < V.size(); ++i) ref += glm::cross(P[V[i]], P[V[(i + 1) % V.size()]]);
  auto facing = [&](const vec3& a, const vec3& b, const vec3& c, const vec3& d) { return glm::dot(glm::cross(c - a, d - b), ref) > 0; };
  while (V.size() > 4) {
    const size_t n = V.size();
    const size_t across = n % 2 ? 2 : 3;  // (an odd outline loses a triangle first)
    size_t bi = 0;
    float bd = 1e30f;
    for (size_t i = 0; i < n; ++i) {
      const vec3 &a = P[V[i]], &b = P[V[(i + 1) % n]], &c = P[V[(i + 2) % n]], &d = P[V[(i + across) % n]];
      bool ok = across == 2 ? glm::dot(glm::cross(b - a, c - a), ref) > 0 : facing(a, b, c, d);
      if (float dd = len(V[i], V[(i + across) % n]) * (ok ? 1.f : 10.f); dd < bd) bd = dd, bi = i;
    }
    std::rotate(V.begin(), V.begin() + (long)bi, V.end());
    if (across == 2) {
      tris.push_back({V[0], V[1], V[2]}), triGroup.push_back(-1);
      V.erase(V.begin() + 1);
    } else {
      quads.push_back({V[0], V[1], V[2], V[3]}), quadGroup.push_back(-1);
      V.erase(V.begin() + 1, V.begin() + 3);
    }
  }
  if (V.size() == 4) quads.push_back({V[0], V[1], V[2], V[3]}), quadGroup.push_back(-1);
  else if (V.size() == 3) tris.push_back({V[0], V[1], V[2]}), triGroup.push_back(-1);
}


// A hole closed with a grid (as a grid fill): its outline (an even count) split into four sides of n, m,
// n, m edges with the corners where it turns most (the sides' lengths from the hole's area), the points
// inside from the four sides (a Coons patch) put back on the surface. Not when a face would turn over.
bool gridHole(const std::vector<uint32_t>& V, std::vector<vec3>& P, std::vector<Quad>& quads, std::vector<int>& quadGroup,
              std::vector<char>& bandV, const std::function<vec3(const vec3&)>& snap) {
  const long c = (long)V.size();
  if (c < 6 || c % 2) return false;
  // (its area as a fan from its middle put on the surface: a hole over a bend, as the bridge of a nose,
  // is no slit)
  vec3 mid(0);
  double per = 0;
  for (long i = 0; i < c; ++i) mid += P[V[i]] / float(c), per += glm::length(P[V[(i + 1) % c]] - P[V[i]]);
  if (snap) mid = snap(mid);
  double area = 0;
  for (long i = 0; i < c; ++i) area += 0.5 * glm::length(glm::cross(P[V[i]] - mid, P[V[(i + 1) % c]] - mid));
  const double e = per / c;
  if (getenv("PATINA_RETOPO_DEBUG")) PLOG_I("  hole of %ld edges: roundness %.2f", c, 4 * 3.14159265 * area / (per * per));
  if (area <= 0 || e <= 0 || 4 * 3.14159265 * area / (per * per) < 0.3) return false;  // (a slit)
  const long S = c / 2;
  const double Aq = area / (e * e), disc = double(S) * S - 4 * Aq;
  long n = disc > 0 ? std::lround((S - std::sqrt(disc)) / 2) : S / 2;
  n = std::clamp<long>(n, 1, S / 2);
  const long m = S - n;
  if (getenv("PATINA_RETOPO_DEBUG")) PLOG_I("  hole of %ld edges: a %ld x %ld grid (area %.1f quads, edge %.4f)", c, n, m, Aq, e);
  const long w = c >= 8 ? 2 : 1;
  std::vector<double> turn(c);
  for (long k = 0; k < c; ++k) {
    vec3 a = P[V[k]] - P[V[(k - w + c) % c]], b = P[V[(k + w) % c]] - P[V[k]];
    float la = glm::length(a), lb = glm::length(b);
    turn[k] = la > 0 && lb > 0 ? std::acos(std::clamp(glm::dot(a, b) / (la * lb), -1.f, 1.f)) : 0.0;
  }
  long s0 = 0;
  double best = -1;
  for (long s = 0; s < c; ++s) {
    double sc = turn[s] + turn[(s + n) % c] + turn[(s + n + m) % c] + turn[(s + 2 * n + m) % c];
    if (sc > best + 1e-9) best = sc, s0 = s;
  }
  std::vector<int64_t> id(size_t(n + 1) * (m + 1), -1);
  auto gid = [&](long i, long j) -> int64_t& { return id[size_t(j) * (n + 1) + i]; };
  for (long k = 0; k < c; ++k) {
    long u = ((k - s0) % c + c) % c, i, j;
    if (u <= n) i = u, j = 0;
    else if (u <= n + m) i = n, j = u - n;
    else if (u <= 2 * n + m) i = n - (u - n - m), j = m;
    else i = 0, j = m - (u - 2 * n - m);
    gid(i, j) = V[k];
  }
  // the patch from its four sides (U), then each inner point on the surface; the grid is refused where the
  // patch folds (two quads side by side turned against each other) or a quad turns over when put on
  // the surface
  std::vector<vec3> U(size_t(n + 1) * (m + 1));
  auto u3 = [&](long i, long j) -> vec3& { return U[size_t(j) * (n + 1) + i]; };
  for (long j = 0; j <= m; ++j)
    for (long i = 0; i <= n; ++i)
      if (gid(i, j) >= 0) u3(i, j) = P[(uint32_t)gid(i, j)];
  for (long j = 1; j < m; ++j)
    for (long i = 1; i < n; ++i) {
      float u = float(i) / n, v = float(j) / m;
      u3(i, j) = (1 - v) * u3(i, 0) + v * u3(i, m) + (1 - u) * u3(0, j) + u * u3(n, j) -
                 ((1 - u) * (1 - v) * u3(0, 0) + u * (1 - v) * u3(n, 0) + u * v * u3(n, m) + (1 - u) * v * u3(0, m));
    }
  auto nU = [&](long i, long j) { return glm::cross(u3(i + 1, j + 1) - u3(i, j), u3(i, j + 1) - u3(i + 1, j)); };
  for (long j = 0; j < m; ++j)
    for (long i = 0; i < n; ++i)
      if ((i + 1 < n && glm::dot(nU(i, j), nU(i + 1, j)) <= 0) || (j + 1 < m && glm::dot(nU(i, j), nU(i, j + 1)) <= 0)) {
        if (getenv("PATINA_RETOPO_DEBUG")) PLOG_I("  hole of %ld edges: the grid folds at %ld %ld", c, i, j);
        return false;
      }
  const size_t base = P.size();
  for (long j = 1; j < m; ++j)
    for (long i = 1; i < n; ++i) {
      P.push_back(snap ? snap(u3(i, j)) : u3(i, j)), bandV.push_back(1);
      gid(i, j) = (int64_t)(P.size() - 1);
    }
  std::vector<Quad> made;
  for (long j = 0; j < m; ++j)
    for (long i = 0; i < n; ++i) {
      Quad q{(uint32_t)gid(i, j), (uint32_t)gid(i + 1, j), (uint32_t)gid(i + 1, j + 1), (uint32_t)gid(i, j + 1)};
      const vec3 &a = P[q[0]], &b = P[q[1]], &cc = P[q[2]], &d = P[q[3]];
      if (glm::dot(glm::cross(cc - a, d - b), nU(i, j)) <= 0) {
        if (getenv("PATINA_RETOPO_DEBUG")) PLOG_I("  hole of %ld edges: the grid turns over at %ld %ld", c, i, j);
        P.resize(base), bandV.resize(base);
        return false;
      }
      made.push_back(q);
    }
  for (auto& q : made) quads.push_back(q), quadGroup.push_back(-1);
  return true;
}

// A hole closed with grids: one if it takes one, else cut across its narrowest neck (with points along
// the cut so both parts have even counts) and each part closed the same way (a few cuts deep at most).
bool gridOrSplit(const std::vector<uint32_t>& V, std::vector<vec3>& P, std::vector<Quad>& quads, std::vector<int>& quadGroup,
                 std::vector<char>& bandV, const std::function<vec3(const vec3&)>& snap, int depth) {
  const size_t q0 = quads.size(), p0 = P.size();
  if (gridHole(V, P, quads, quadGroup, bandV, snap)) return true;
  const long c = (long)V.size();
  if (depth >= 4 || c < 10 || c % 2) return false;
  std::vector<double> arc(c + 1, 0);
  for (long i = 0; i < c; ++i) arc[i + 1] = arc[i] + glm::length(P[V[(i + 1) % c]] - P[V[i]]);
  const double per = arc[c], e = per / c;
  long bi = -1, bj = -1;
  double best = 0.5;  // (a neck: across no more than half the way round)
  for (long i = 0; i < c; ++i)
    for (long j = i + 3; j < c; ++j) {
      if (c - (j - i) < 3) continue;
      double round = std::min(arc[j] - arc[i], per - (arc[j] - arc[i]));
      if (round < 3 * e) continue;
      double r = glm::length(P[V[j]] - P[V[i]]) / round;
      if (r < best) best = r, bi = i, bj = j;
    }
  if (bi < 0) return false;
  // the cut's points: about an edge apart, as many as keep both parts even
  const double d = glm::length(P[V[bj]] - P[V[bi]]);
  long k = std::max<long>(0, std::lround(d / e) - 1);
  if ((k + (bj - bi)) % 2 == 0) k += (k > 0 && std::lround(d / e) - 1 < k + 0.5 ? -1 : 1);
  if (k < 0) k = 1;
  std::vector<uint32_t> cut;
  for (long t = 1; t <= k; ++t) {
    vec3 p = glm::mix(P[V[bi]], P[V[bj]], float(t) / (k + 1));
    P.push_back(snap ? snap(p) : p), bandV.push_back(1), cut.push_back((uint32_t)P.size() - 1);
  }
  // A: V[i..j] and the cut back; B: V[j..i] (round the end) and the cut forwards
  std::vector<uint32_t> A(V.begin() + bi, V.begin() + bj + 1), B;
  A.insert(A.end(), cut.rbegin(), cut.rend());
  for (long t = bj; t != bi; t = (t + 1) % c) B.push_back(V[t]);
  B.push_back(V[bi]);
  B.insert(B.end(), cut.begin(), cut.end());
  if (A.size() % 2 == 0 && B.size() % 2 == 0 && gridOrSplit(A, P, quads, quadGroup, bandV, snap, depth + 1) &&
      gridOrSplit(B, P, quads, quadGroup, bandV, snap, depth + 1))
    return true;
  quads.resize(q0), quadGroup.resize(q0), P.resize(p0), bandV.resize(p0);
  return false;
}

// A hole filled ring by ring: an inner ring about an edge in (its count from its length), joined to the
// outline with a row like those between zones, until what's left is small (or a slit), then cut up.
void fillHole(std::vector<uint32_t> V, std::vector<vec3>& P, std::vector<Quad>& quads, std::vector<int>& quadGroup, std::vector<Tri>& tris,
              std::vector<int>& triGroup, std::vector<char>& bandV, JoinStats& st, const OutFlags& out,
              const std::function<vec3(const vec3&)>& snap = {}) {
  // a grid where it can be (an odd outline loses a triangle at its sharpest corner first)
  if (V.size() >= 7 && V.size() % 2) {
    const size_t n = V.size();
    vec3 ref(0);
    for (size_t i = 0; i < n; ++i) ref += glm::cross(P[V[i]], P[V[(i + 1) % n]]);
    size_t bi = n;
    float bd = 1e30f;
    for (size_t i = 0; i < n; ++i) {
      const vec3 &a = P[V[i]], &b = P[V[(i + 1) % n]], &c = P[V[(i + 2) % n]];
      if (glm::dot(glm::cross(b - a, c - a), ref) <= 0) continue;
      if (float d = glm::length(c - a); d < bd) bd = d, bi = i;
    }
    if (bi < n) {
      std::vector<uint32_t> W = V;
      std::rotate(W.begin(), W.begin() + (long)bi, W.end());
      std::vector<uint32_t> rest(W.begin(), W.end());
      rest.erase(rest.begin() + 1);
      const size_t t0 = tris.size();
      tris.push_back({W[0], W[1], W[2]}), triGroup.push_back(-1);
      if (gridOrSplit(rest, P, quads, quadGroup, bandV, snap, 0)) {
        ++st.gridHoles;
        return;
      }
      tris.resize(t0), triGroup.resize(t0);
    }
  } else if (V.size() >= 6 && gridOrSplit(V, P, quads, quadGroup, bandV, snap, 0)) {
    ++st.gridHoles;
    return;
  }
  for (int round = 0; round < 64 && V.size() > 6; ++round) {
    const size_t n = V.size();
    double per = 0;
    vec3 c(0), nrm(0);
    for (size_t i = 0; i < n; ++i) {
      per += glm::length(P[V[(i + 1) % n]] - P[V[i]]), c += P[V[i]] / float(n);
      nrm += glm::cross(P[V[i]], P[V[(i + 1) % n]]);
    }
    const double area = 0.5 * glm::length(nrm), e = per / n;
    if (4 * 3.14159265 * area / (per * per) < 0.3) break;  // (a slit: cut up as it is)
    std::vector<vec3> in(n);
    for (size_t i = 0; i < n; ++i) {
      vec3 d = c - P[V[i]];
      float l = glm::length(d);
      in[i] = P[V[i]] + (l > 0 ? d / l : d) * std::min((float)e, 0.5f * l);
      if (snap) in[i] = snap(in[i]);  // (back on the surface: a big hole on a curve fills along it)
    }
    std::vector<double> arc(n + 1, 0);
    for (size_t i = 0; i < n; ++i) arc[i + 1] = arc[i] + glm::length(in[(i + 1) % n] - in[i]);
    size_t m = (size_t)std::lround(arc[n] / e);
    m = std::clamp<size_t>(m, std::max<size_t>(4, (n + 2) / 3), n - 2);
    std::vector<uint32_t> M;
    for (size_t k = 0; k < m; ++k) {
      double x = arc[n] * k / m;
      size_t i = std::min<size_t>(size_t(std::upper_bound(arc.begin(), arc.end(), x) - arc.begin()), n) - 1;
      double seg = arc[i + 1] - arc[i];
      P.push_back(glm::mix(in[i], in[(i + 1) % n], seg > 0 ? float((x - arc[i]) / seg) : 0.f)), bandV.push_back(1);
      M.push_back((uint32_t)(P.size() - 1));
    }
    if (!buildOneRow(P, quads, quadGroup, tris, triGroup, V, M, true, bandV, st, out)) {
      P.resize(P.size() - m), bandV.resize(P.size());
      break;
    }
    V = std::move(M);
  }
  clipHole(V, P, quads, quadGroup, tris, triGroup);
}

}  // namespace

bool joinGroups(std::vector<vec3>& P, std::vector<Quad>& quads, std::vector<int>& quadGroup, std::vector<Tri>& tris, std::vector<int>& triGroup,
                const std::vector<JoinChain>& chains, const std::vector<double>& groupL, const std::vector<std::array<vec3, 2>>& openEdges,
                std::vector<char>& bandV, JoinStats& st, const std::function<vec3(const vec3&)>& snap) {
  st = {};
  bandV.assign(P.size(), 0);
  if (chains.empty()) return true;
  const int G = (int)groupL.size();
  const float maxL = (float)*std::max_element(groupL.begin(), groupL.end());
  auto fail = [&](const std::string& why) {
    st.why = why;
    PLOG_I("Retopology (flow): join: %s", why.c_str());
    return false;
  };
  // the chains in a grid; each one's coarser side
  SegGrid cg;
  std::vector<int> coarse(chains.size());
  for (size_t ci = 0; ci < chains.size(); ++ci) {
    const auto& ch = chains[ci];
    if (ch.g < 0 || ch.h < 0 || ch.g >= G || ch.h >= G) return fail("a chain between unknown groups");
    coarse[ci] = groupL[ch.g] >= groupL[ch.h] ? ch.g : ch.h;
    double t = 0;
    for (size_t i = 0; i + 1 < ch.line.size(); ++i) {
      double l = glm::length(ch.line[i + 1] - ch.line[i]);
      cg.segs.push_back({ch.line[i], ch.line[i + 1], (int)ci, t, t + l});
      t += l;
    }
  }
  cg.build(2.f * maxL);  // (reaching two of the coarsest quads)
  st.chains = (int)chains.size();
  // (debug dumps numbered by call: a join made again doesn't write over the first one's)
  static std::atomic<int> dumpCount{0};
  const std::string dumpBase = getenv("PATINA_RETOPO_DEBUG") ? std::string(getenv("PATINA_RETOPO_DEBUG")) + "_" + std::to_string(dumpCount++) : "";

  if (getenv("PATINA_RETOPO_DEBUG"))
    for (int g = 0; g < G; ++g) {
      // how far each group's own edge is from its chains (in its quads)
      auto loops = loopsOf(
          quads, tris, [&](size_t q) { return quadGroup[q] == g; }, [&](size_t t) { return triGroup[t] == g; });
      int h[7] = {0};
      for (auto& l : loops)
        for (uint32_t v : l) {
          auto hit = cg.nearest(P[v], [&](int id) { return chains[id].g == g || chains[id].h == g; });
          float d = hit.id < 0 ? 1e9f : std::sqrt(hit.d2) / (float)groupL[g];
          h[d < 0.1f ? 0 : d < 0.25f ? 1 : d < 0.5f ? 2 : d < 1.f ? 3 : d < 1.5f ? 4 : d < 2.f ? 5 : 6]++;
        }
      PLOG_I("  group %d (quads %.4f): edge vertices within 0.1/0.25/0.5/1/1.5/2/more quads of its chains: %d %d %d %d %d %d %d", g, groupL[g], h[0],
             h[1], h[2], h[3], h[4], h[5], h[6]);
    }
  if (!dumpBase.empty() && dumpBase[0] == '/')
    if (FILE* fo = fopen((dumpBase + "_made.obj").c_str(), "w")) {
      for (auto& p : P) fprintf(fo, "v %f %f %f\n", p.x, p.y, p.z);
      for (int g = 0; g < G; ++g) {
        fprintf(fo, "o group%d\n", g);
        for (size_t q = 0; q < quads.size(); ++q)
          if (quadGroup[q] == g) fprintf(fo, "f %u %u %u %u\n", quads[q][0] + 1, quads[q][1] + 1, quads[q][2] + 1, quads[q][3] + 1);
        for (size_t t = 0; t < tris.size(); ++t)
          if (triGroup[t] == g) fprintf(fo, "f %u %u %u\n", tris[t][0] + 1, tris[t][1] + 1, tris[t][2] + 1);
      }
      fclose(fo);
    }
  // ---- 1. along each chain, the coarser side's row of faces touching its edge there out
  std::vector<char> qOut(quads.size(), 0), tOut(tris.size(), 0);
  for (int g = 0; g < G; ++g) {
    bool isCoarse = false;
    for (int c : coarse) isCoarse |= c == g;
    if (!isCoarse) continue;
    auto loops = loopsOf(
        quads, tris, [&](size_t q) { return quadGroup[q] == g; }, [&](size_t t) { return triGroup[t] == g; });
    std::unordered_set<uint32_t> edgeV;
    const float near = 0.75f * (float)groupL[g];
    for (auto& l : loops)
      for (uint32_t v : l)
        if (cg.nearest(P[v], [&](int id) { return coarse[id] == g; }).d2 < near * near) edgeV.insert(v);
    for (size_t q = 0; q < quads.size(); ++q)
      if (quadGroup[q] == g)
        for (uint32_t x : quads[q]) qOut[q] |= edgeV.count(x) > 0;
    for (size_t t = 0; t < tris.size(); ++t)
      if (triGroup[t] == g)
        for (uint32_t x : tris[t]) tOut[t] |= edgeV.count(x) > 0;
  }

  auto outCounts = [&](const char* when) {
    if (!getenv("PATINA_RETOPO_DEBUG")) return;
    std::string d;
    for (int g = 0; g < G; ++g) {
      int all = 0, out = 0;
      for (size_t q = 0; q < quads.size(); ++q)
        if (quadGroup[q] == g) ++all, out += qOut[q] != 0;
      d += " " + std::to_string(g) + ":" + std::to_string(out) + "/" + std::to_string(all);
    }
    PLOG_I("  quads out %s:%s", when, d.c_str());
  };
  outCounts("cut along the chains");
  // ---- 2. per group: bits left between the cuts (a face or two) out; where faces meet at a corner only,
  // the smaller fans out (so each boundary is a simple loop)
  for (int g = 0; g < G; ++g) {
    for (int round = 0; round < 6; ++round) {
      std::unordered_map<EdgeKey, std::vector<int64_t>> faceAt;  // edge -> faces (tris as -1 - t)
      for (size_t q = 0; q < quads.size(); ++q)
        if (!qOut[q] && quadGroup[q] == g)
          for (int k = 0; k < 4; ++k) faceAt[ekey(quads[q][k], quads[q][(k + 1) % 4])].push_back((int64_t)q);
      for (size_t t = 0; t < tris.size(); ++t)
        if (!tOut[t] && triGroup[t] == g)
          for (int k = 0; k < 3; ++k) faceAt[ekey(tris[t][k], tris[t][(k + 1) % 3])].push_back(-1 - (int64_t)t);
      auto vertsOf = [&](int64_t f) {
        std::vector<uint32_t> v;
        if (f >= 0) v.assign(quads[f].begin(), quads[f].end());
        else v.assign(tris[-1 - f].begin(), tris[-1 - f].end());
        return v;
      };
      auto drop = [&](int64_t f) {
        if (f >= 0) qOut[f] = 1;
        else tOut[-1 - f] = 1;
      };
      bool any = false;
      // small pieces
      {
        std::unordered_map<int64_t, int64_t> parent;
        std::function<int64_t(int64_t)> find = [&](int64_t x) {
          while (parent[x] != x) x = parent[x] = parent[parent[x]];
          return x;
        };
        for (auto& [e, fs] : faceAt)
          for (int64_t f : fs)
            if (!parent.count(f)) parent[f] = f;
        for (auto& [e, fs] : faceAt)
          for (size_t i = 1; i < fs.size(); ++i) parent[find(fs[i])] = find(fs[0]);
        std::unordered_map<int64_t, int> size;
        for (auto& [f, p] : parent) size[find(f)]++;
        for (auto& [f, p] : parent)
          if (size[find(f)] <= 2) drop(f), any = true;
      }
      if (any) continue;
      // corners where two fans meet
      std::unordered_map<uint32_t, int> outs;
      for (auto& [e, fs] : faceAt)
        if (fs.size() == 1) outs[uint32_t(e >> 32)]++, outs[uint32_t(e & 0xFFFFFFFF)]++;
      for (auto& [x, n] : outs) {
        if (n <= 2) continue;
        std::vector<int64_t> at;
        for (auto& [e, fs] : faceAt)
          if (uint32_t(e >> 32) == x || uint32_t(e & 0xFFFFFFFF) == x)
            for (int64_t f : fs)
              if (std::find(at.begin(), at.end(), f) == at.end()) at.push_back(f);
        std::vector<int> fan(at.size(), -1);
        int fans = 0;
        for (size_t i = 0; i < at.size(); ++i) {
          if (fan[i] >= 0) continue;
          fan[i] = fans;
          std::vector<size_t> stack = {i};
          while (!stack.empty()) {
            size_t a = stack.back();
            stack.pop_back();
            auto va = vertsOf(at[a]);
            for (size_t b = 0; b < at.size(); ++b) {
              if (fan[b] >= 0) continue;
              auto vb = vertsOf(at[b]);
              int shared = 0;
              for (uint32_t y : va)
                for (uint32_t z : vb) shared += y == z && y != x;
              if (shared) fan[b] = fans, stack.push_back(b);
            }
          }
          ++fans;
        }
        if (fans < 2) continue;
        std::vector<int> size(fans, 0);
        for (int f : fan) size[f]++;
        int keep = int(std::max_element(size.begin(), size.end()) - size.begin());
        for (size_t i = 0; i < at.size(); ++i)
          if (fan[i] != keep) drop(at[i]), any = true;
      }
      if (!any) break;
    }
  }
  outCounts("and bits left between");

  if (const char* dbg = getenv("PATINA_RETOPO_DEBUG"); dbg && *dbg == '/')
    if (FILE* fo = fopen((dumpBase + "_cut.obj").c_str(), "w")) {
      for (auto& p : P) fprintf(fo, "v %f %f %f\n", p.x, p.y, p.z);
      for (int g = 0; g < G; ++g) {
        fprintf(fo, "o group%d\n", g);
        for (size_t q = 0; q < quads.size(); ++q)
          if (!qOut[q] && quadGroup[q] == g) fprintf(fo, "f %u %u %u %u\n", quads[q][0] + 1, quads[q][1] + 1, quads[q][2] + 1, quads[q][3] + 1);
        for (size_t t = 0; t < tris.size(); ++t)
          if (!tOut[t] && triGroup[t] == g) fprintf(fo, "f %u %u %u\n", tris[t][0] + 1, tris[t][1] + 1, tris[t][2] + 1);
      }
      fclose(fo);
    }
  // ---- 3. each group's boundary loops; the stretches of them along each chain (runs)
  struct Run {
    std::vector<uint32_t> v;
    std::vector<double> t;  // where along the chain
    bool closed = false;
    bool rev = false;  // the other way from its loop (the two sides' loops go opposite ways along a chain)
  };
  std::vector<std::array<std::vector<Run>, 2>> runs(chains.size());
  std::vector<std::vector<int>> chainsOf(G);
  for (size_t ci = 0; ci < chains.size(); ++ci) chainsOf[chains[ci].g].push_back((int)ci), chainsOf[chains[ci].h].push_back((int)ci);
  for (int g = 0; g < G; ++g) {
    if (chainsOf[g].empty()) continue;
    auto loops = loopsOf(
        quads, tris, [&](size_t q) { return !qOut[q] && quadGroup[q] == g; }, [&](size_t t) { return !tOut[t] && triGroup[t] == g; });
    auto mine = [&](int id) { return chains[id].g == g || chains[id].h == g; };
    for (auto& loop : loops) {
      const size_t n = loop.size();
      std::vector<int> label(n, -1);
      for (size_t i = 0; i < n; ++i) {
        auto h = cg.nearest(P[loop[i]], mine);
        if (h.id < 0) continue;
        float thr = (coarse[h.id] == g ? 3.0f : 1.0f) * (float)groupL[g];  // (the coarse side lost a row)
        if (h.d2 < thr * thr) label[i] = h.id;
      }
      if (getenv("PATINA_RETOPO_DEBUG")) {
        vec3 c(0);
        for (uint32_t v : loop) c += P[v] / float(n);
        std::string ls;
        for (int l : label) ls += l < 0 ? "." : std::to_string(l);
        if (ls.size() > 80) ls = ls.substr(0, 80) + "...";
        PLOG_I("  group %d loop of %zu at %.3f %.3f %.3f: %s", g, n, c.x, c.y, c.z, ls.c_str());
      }
      // short gaps between stretches along the same chain are closed
      for (int pass = 0; pass < 2; ++pass)
        for (size_t i = 0; i < n; ++i) {
          if (label[i] < 0 || label[(i + 1) % n] >= 0) continue;
          for (size_t gap = 1; gap <= 3; ++gap) {
            int l = label[(i + 1 + gap) % n];
            if (l < 0) continue;
            if (l == label[i])
              for (size_t k = 1; k <= gap; ++k) label[(i + k) % n] = l;
            break;
          }
        }
      // the whole loop along one chain (a closed chain, or nearly all of one)
      std::unordered_map<int, size_t> count;
      for (int l : label)
        if (l >= 0) count[l]++;
      int whole = -1;
      for (auto& [id, c] : count)
        if (chains[id].closed && count.size() == 1 && c * 4 >= n * 3) whole = id;
      if (whole < 0 && count.size() == 1 && count.begin()->second == n) whole = count.begin()->first;
      if (whole >= 0) {
        Run r;
        r.v = loop, r.closed = true;
        runs[whole][chains[whole].g == g ? 0 : 1].push_back(std::move(r));
        continue;
      }
      // the stretches: start after a change of label
      size_t start = 0;
      for (size_t i = 0; i < n; ++i)
        if (label[i] != label[(i + n - 1) % n]) {
          start = i;
          break;
        }
      for (size_t k = 0; k < n;) {
        int l = label[(start + k) % n];
        Run r;
        while (k < n && label[(start + k) % n] == l) r.v.push_back(loop[(start + k) % n]), ++k;
        if (l < 0 || r.v.size() < 2) continue;
        runs[l][chains[l].g == g ? 0 : 1].push_back(std::move(r));
      }
    }
  }

  // ---- 4. rows along each chain, between the runs on its two sides that face each other
  auto paramOf = [&](uint32_t v, int ci) { return cg.nearest(P[v], [&](int id) { return id == ci; }).t; };
  int skipped = 0;
  const bool debug = getenv("PATINA_RETOPO_DEBUG") != nullptr;
  std::vector<double> chainLen(chains.size(), 0);
  std::vector<int> rowsMade(chains.size(), 0);
  std::vector<char> oneSided(chains.size(), 0);
  for (size_t ci = 0; ci < chains.size(); ++ci) {
    auto& RA = runs[ci][0];
    auto& RB = runs[ci][1];
    double perimeter = 0;
    for (size_t i = 0; i + 1 < chains[ci].line.size(); ++i) perimeter += glm::length(chains[ci].line[i + 1] - chains[ci].line[i]);
    chainLen[ci] = perimeter;
    if (debug) {
      auto desc = [&](const std::vector<Run>& R) {
        std::string d;
        for (auto& r : R) d += " " + std::to_string(r.v.size()) + (r.closed ? "c" : "");
        return d;
      };
      vec3 a = chains[ci].line.front(), b = chains[ci].line.back();
      PLOG_I("  chain %zu: groups %d|%d%s, %.3f long, %.3f %.3f %.3f to %.3f %.3f %.3f; runs%s |%s", ci, chains[ci].g, chains[ci].h,
             chains[ci].closed ? " closed" : "", perimeter, a.x, a.y, a.z, b.x, b.y, b.z, desc(RA).c_str(), desc(RB).c_str());
    }
    if (RA.empty() || RB.empty()) {
      ++skipped, oneSided[ci] = 1;
      continue;
    }
    if (RA.size() == 1 && RB.size() == 1 && RA[0].closed && RB[0].closed) {
      // both sides all the way round: one closed row, B the same way round (its loop turned round) and
      // starting across from A's start
      Run A = RA[0], B = RB[0];
      std::reverse(B.v.begin(), B.v.end());
      size_t a = 0;
      float bd = 1e30f;
      for (size_t i = 0; i < B.v.size(); ++i)
        if (float d = glm::dot(P[B.v[i]] - P[A.v[0]], P[B.v[i]] - P[A.v[0]]); d < bd) bd = d, a = i;
      std::rotate(B.v.begin(), B.v.begin() + (long)a, B.v.end());
      if (!buildRow(P, quads, quadGroup, tris, triGroup, A.v, B.v, true, bandV, st, {&qOut, &tOut}, coarse[ci] == chains[ci].g ? 1 : 0))
        return fail("the two sides of a border (" + std::to_string(A.v.size()) + " and " + std::to_string(B.v.size()) +
                    " vertices) can't be stepped between");
      qOut.resize(quads.size(), 0), tOut.resize(tris.size(), 0);
      ++rowsMade[ci];
      continue;
    }
    // where along the chain each run's vertices are
    for (auto* R : {&RA, &RB})
      for (auto& r : *R) {
        r.t.clear();
        for (uint32_t v : r.v) r.t.push_back(paramOf(v, (int)ci));
      }
    std::array<std::vector<Run>*, 2> sides = {&RA, &RB};
    if (chains[ci].closed && perimeter > 0) {
      // on a loop: measured from the middle of the widest gap on a side with gaps, and the runs that
      // pass that point split there
      auto span = [&](const Run& r) {  // (unwrapped: how far it goes, the way it goes)
        double s = 0;
        for (size_t i = 1; i < r.t.size(); ++i) {
          double d = r.t[i] - r.t[i - 1];
          d -= perimeter * std::round(d / perimeter);
          s += d;
        }
        return s;
      };
      double cut = 0, widest = -1;
      for (auto* R : sides) {
        std::vector<std::pair<double, double>> iv;  // (start, end) round the loop, forwards
        bool full = false;
        for (auto& r : *R) {
          if (r.closed) full = true;
          double s = span(r);
          double a0 = s >= 0 ? r.t.front() : r.t.back(), len = std::abs(s);
          iv.push_back({std::fmod(a0 + 4 * perimeter, perimeter), len});
        }
        if (full || iv.empty()) continue;
        std::sort(iv.begin(), iv.end());
        for (size_t i = 0; i < iv.size(); ++i) {
          double end = iv[i].first + iv[i].second, next = i + 1 < iv.size() ? iv[i + 1].first : iv[0].first + perimeter;
          if (next - end > widest) widest = next - end, cut = std::fmod(end + 0.5 * (next - end), perimeter);
        }
      }
      for (auto* R : sides) {
        std::vector<Run> out;
        for (auto& r : *R) {
          std::vector<double> t;
          for (double x : r.t) t.push_back(std::fmod(x - cut + 4 * perimeter, perimeter));
          std::vector<uint32_t> v = r.v;
          if (r.closed) {
            // (opened just after the cut)
            size_t lo = size_t(std::min_element(t.begin(), t.end()) - t.begin());
            std::rotate(v.begin(), v.begin() + (long)lo, v.end()), std::rotate(t.begin(), t.begin() + (long)lo, t.end());
          }
          Run cur;
          for (size_t i = 0; i < v.size(); ++i) {
            if (!cur.v.empty() && std::abs(t[i] - cur.t.back()) > 0.5 * perimeter) out.push_back(std::move(cur)), cur = Run();
            cur.v.push_back(v[i]), cur.t.push_back(t[i]);
          }
          out.push_back(std::move(cur));
        }
        *R = std::move(out);
      }
    }
    // each run the way the chain goes
    for (auto* R : sides) {
      std::vector<Run> out;
      for (auto& r : *R) {
        if (r.v.size() < 2) continue;
        if (r.t.back() < r.t.front()) std::reverse(r.v.begin(), r.v.end()), std::reverse(r.t.begin(), r.t.end()), r.rev = !r.rev;
        out.push_back(std::move(r));
      }
      *R = std::move(out);
    }
    // the pairs of runs that face each other, and each run shared out between its pairs
    const float tolA = 0.5f * (float)groupL[chains[ci].g], tolB = 0.5f * (float)groupL[chains[ci].h];
    const double minLen = 0.5 * std::min(groupL[chains[ci].g], groupL[chains[ci].h]);
    struct Pair {
      size_t a, b;
      double lo, hi;
    };
    std::vector<Pair> pairs;
    for (size_t a = 0; a < RA.size(); ++a)
      for (size_t b = 0; b < RB.size(); ++b) {
        double lo = std::max(RA[a].t.front(), RB[b].t.front()), hi = std::min(RA[a].t.back(), RB[b].t.back());
        // (facing each other: going the same way along the chain, their loops go opposite ways)
        if (hi - lo > minLen && RA[a].rev != RB[b].rev) pairs.push_back({a, b, lo, hi});
      }
    if (pairs.empty()) {
      ++skipped;
      continue;
    }
    auto piece = [&](const Run& r, const std::vector<Pair>& all, size_t me, bool sideA) {
      // the run's vertices shared out between its pairs (each to one only, by where it is along the
      // chain: the pairs split halfway across the gap, or the overlap, between them); this pair's
      // longest unbroken stretch of them
      const size_t run = sideA ? all[me].a : all[me].b;
      const float tol = sideA ? tolA : tolB;
      std::vector<size_t> mine;
      for (size_t k = 0; k < all.size(); ++k)
        if ((sideA ? all[k].a : all[k].b) == run) mine.push_back(k);
      std::sort(mine.begin(), mine.end(), [&](size_t x, size_t y) { return all[x].lo + all[x].hi < all[y].lo + all[y].hi; });
      std::vector<double> from(mine.size()), to(mine.size());
      for (size_t j = 0; j < mine.size(); ++j) {
        from[j] = j ? 0.5 * (all[mine[j - 1]].hi + all[mine[j]].lo) : all[mine[j]].lo - tol;
        to[j] = j + 1 < mine.size() ? 0.5 * (all[mine[j]].hi + all[mine[j + 1]].lo) : all[mine[j]].hi + tol;
      }
      size_t bestStart = 0, bestLen = 0;
      for (size_t i = 0; i < r.t.size();) {
        size_t owner = mine.size();
        for (size_t j = 0; j < mine.size() && owner == mine.size(); ++j)
          if (r.t[i] >= from[j] && r.t[i] < to[j]) owner = j;
        if (owner == mine.size() || mine[owner] != me) {
          ++i;
          continue;
        }
        size_t i0 = i;
        while (i < r.t.size() && r.t[i] >= from[owner] && r.t[i] < to[owner]) ++i;
        if (i - i0 > bestLen) bestLen = i - i0, bestStart = i0;
      }
      return std::vector<uint32_t>(r.v.begin() + (long)bestStart, r.v.begin() + (long)(bestStart + bestLen));
    };
    for (size_t k = 0; k < pairs.size(); ++k) {
      std::vector<uint32_t> A = piece(RA[pairs[k].a], pairs, k, true), B = piece(RB[pairs[k].b], pairs, k, false);
      if (A.size() < 2 || B.size() < 2) {
        ++skipped;
        continue;
      }
      if (!buildRow(P, quads, quadGroup, tris, triGroup, A, B, false, bandV, st, {&qOut, &tOut}, coarse[ci] == chains[ci].g ? 1 : 0))
        return fail("the two sides of a border (" + std::to_string(A.size()) + " and " + std::to_string(B.size()) +
                    " vertices) can't be stepped between");
      qOut.resize(quads.size(), 0), tOut.resize(tris.size(), 0);
      ++rowsMade[ci];
    }
  }
  // a border all the way round (or a long one) with no row at all: its two sides' edges would be
  // filled as two caps, not joined; unless one side has no quads near it at all (a zone too small for
  // any: the hole there is filled)
  // (one side with no edge of its own along it at all: that zone got no quads there, as an island too small
  // for any; the hole is the island, filled below)
  auto sideMissing = [&](size_t ci) { return oneSided[ci] != 0; };
  for (size_t ci = 0; ci < chains.size(); ++ci)
    if (!rowsMade[ci] && ((chains[ci].closed && chainLen[ci] > 4 * std::min(groupL[chains[ci].g], groupL[chains[ci].h])) ||
                          chainLen[ci] > 6 * maxL) &&
        !sideMissing(ci))
      return fail("a border between zones (" + std::to_string(chainLen[ci]) + " long) found no edge on one side to join to");
  bandV.resize(P.size(), 0);

  // ---- 5. what's left open: the model's own open edges (left), and small holes where chains meet (filled)
  SegGrid og;
  for (auto& e : openEdges) og.segs.push_back({e[0], e[1], 0, 0, 0});
  og.build(maxL);
  auto loops = loopsOf(quads, tris, [&](size_t q) { return !qOut[q]; }, [&](size_t t) { return !tOut[t]; });
  if (const char* dbg = getenv("PATINA_RETOPO_DEBUG"); dbg && *dbg == '/') {
    // the joined faces (group in the object name) and the holes, for looking at
    if (FILE* fo = fopen((dumpBase + "_joined.obj").c_str(), "w")) {
      for (auto& p : P) fprintf(fo, "v %f %f %f\n", p.x, p.y, p.z);
      for (int g = -1; g < G; ++g) {
        fprintf(fo, "o group%d\n", g);
        for (size_t q = 0; q < quads.size(); ++q)
          if (!qOut[q] && quadGroup[q] == g) fprintf(fo, "f %u %u %u %u\n", quads[q][0] + 1, quads[q][1] + 1, quads[q][2] + 1, quads[q][3] + 1);
        for (size_t t = 0; t < tris.size(); ++t)
          if (!tOut[t] && triGroup[t] == g) fprintf(fo, "f %u %u %u\n", tris[t][0] + 1, tris[t][1] + 1, tris[t][2] + 1);
      }
      fprintf(fo, "o holes\n");
      for (auto& l : loops)
        for (size_t i = 0; i < l.size(); ++i) fprintf(fo, "l %u %u\n", l[i] + 1, l[(i + 1) % l.size()] + 1);
      fclose(fo);
    }
  }
  {
    // (an outline through the same vertex twice is two holes)
    std::vector<std::vector<uint32_t>> simple;
    for (auto& l0 : loops) {
      std::vector<std::vector<uint32_t>> todo = {l0};
      while (!todo.empty()) {
        auto l = std::move(todo.back());
        todo.pop_back();
        std::unordered_map<uint32_t, size_t> seenAt;
        bool split = false;
        for (size_t j = 0; j < l.size() && !split; ++j) {
          auto [it, fresh] = seenAt.emplace(l[j], j);
          if (fresh) continue;
          size_t i = it->second;
          todo.push_back(std::vector<uint32_t>(l.begin() + (long)i, l.begin() + (long)j));
          std::vector<uint32_t> rest(l.begin(), l.begin() + (long)i);
          rest.insert(rest.end(), l.begin() + (long)j, l.end());
          todo.push_back(std::move(rest));
          split = true;
        }
        if (!split && l.size() >= 3) simple.push_back(std::move(l));
      }
    }
    loops = std::move(simple);
  }
  for (auto& loop : loops) {
    size_t nearOpen = 0, nearJoin = 0;
    double per = 0;
    for (size_t i = 0; i < loop.size(); ++i) {
      const vec3& p = P[loop[i]];
      per += glm::length(P[loop[(i + 1) % loop.size()]] - p);
      if (!og.segs.empty() && og.nearest(p, [](int) { return true; }).d2 < 0.5f * maxL * maxL) ++nearOpen;
      if (bandV[loop[i]] || cg.nearest(p, [](int) { return true; }).d2 < 2.25f * maxL * maxL) ++nearJoin;
    }
    if (nearOpen * 2 >= loop.size() || nearJoin == 0) continue;  // (the model's, or a part left out away from the zones' borders)
    if (per > 64 * maxL || loop.size() > 256)
      return fail("a hole of " + std::to_string(loop.size()) + " edges is left where zones meet");
    vec3 holeMiddle(0);
    for (uint32_t v : loop) holeMiddle += P[v] / float(loop.size());
    std::vector<uint32_t> V(loop.rbegin(), loop.rend());
    for (uint32_t v : V) bandV[v] = 1;
    // needles in the outline (in to a point and straight back out) zipped shut: the two vertices either
    // side of the point made one
    for (int guard = 0; guard < 64 && V.size() > 4; ++guard) {
      const size_t n = V.size();
      size_t at = n;
      for (size_t i = 0; i < n && at == n; ++i) {
        uint32_t a = V[(i + n - 1) % n], b = V[i], c = V[(i + 1) % n];
        float ab = glm::length(P[b] - P[a]), bc = glm::length(P[c] - P[b]), ac = glm::length(P[c] - P[a]);
        if (ac < 0.25f * std::min(ab, bc)) at = i;
      }
      if (at == n) break;
      uint32_t a = V[(at + n - 1) % n], c = V[(at + 1) % n];
      P[a] = 0.5f * (P[a] + P[c]);
      for (size_t q = 0; q < quads.size(); ++q)
        if (!qOut[q])
          for (auto& x : quads[q]) x = x == c ? a : x;
      for (size_t t = 0; t < tris.size(); ++t)
        if (!tOut[t])
          for (auto& x : tris[t]) x = x == c ? a : x;
      // (the point and the vertex after it leave the outline)
      std::vector<uint32_t> W;
      for (size_t k = 0; k < n; ++k)
        if (k != at && k != (at + 1) % n) W.push_back(V[k]);
      V = std::move(W);
      ++st.welded;
    }
    if (getenv("PATINA_RETOPO_DEBUG")) {
      vec3 c(0);
      for (uint32_t v : V) c += P[v] / float(V.size());
      PLOG_I("  hole of %zu edges (%.3f round) at %.3f %.3f %.3f filled", V.size(), per, c.x, c.y, c.z);
    }
    const int grids = st.gridHoles;
    fillHole(V, P, quads, quadGroup, tris, triGroup, bandV, st, {&qOut, &tOut}, snap);
    // (a bigger hole cut up as it is, not with a grid: marked)
    if (loop.size() > 8 && st.gridHoles == grids) st.patched.push_back(holeMiddle);
    qOut.resize(quads.size(), 0), tOut.resize(tris.size(), 0);
    ++st.holesFilled;
  }

  // ---- 6. the faces taken out gone; every edge between two faces at most, each way once
  {
    std::vector<Quad> q2;
    std::vector<int> g2;
    for (size_t q = 0; q < quads.size(); ++q)
      if (!qOut[q]) q2.push_back(quads[q]), g2.push_back(quadGroup[q]);
    quads = std::move(q2), quadGroup = std::move(g2);
    std::vector<Tri> t2;
    std::vector<int> h2;
    for (size_t t = 0; t < tris.size(); ++t)
      if (!tOut[t]) t2.push_back(tris[t]), h2.push_back(triGroup[t]);
    tris = std::move(t2), triGroup = std::move(h2);
  }
  // ---- 7. tiny edges in the rows and patches welded (a row's end squeezed into a corner leaves slivers)
  {
    const float tiny = 0.2f * (float)*std::min_element(groupL.begin(), groupL.end());
    for (int pass = 0; pass < 3; ++pass) {
      std::vector<std::vector<int64_t>> facesAt(P.size());  // (quads as q, triangles as -1 - t)
      for (size_t q = 0; q < quads.size(); ++q)
        for (uint32_t x : quads[q]) facesAt[x].push_back((int64_t)q);
      for (size_t t = 0; t < tris.size(); ++t)
        for (uint32_t x : tris[t]) facesAt[x].push_back(-1 - (int64_t)t);
      auto vertsOf = [&](int64_t f) {
        return f >= 0 ? std::vector<uint32_t>(quads[f].begin(), quads[f].end()) : std::vector<uint32_t>(tris[-1 - f].begin(), tris[-1 - f].end());
      };
      auto neighbors = [&](uint32_t v) {
        std::unordered_set<uint32_t> n;
        for (int64_t f : facesAt[v]) {
          auto vs = vertsOf(f);
          for (size_t k = 0; k < vs.size(); ++k)
            if (vs[k] == v) n.insert(vs[(k + 1) % vs.size()]), n.insert(vs[(k + vs.size() - 1) % vs.size()]);
        }
        return n;
      };
      std::vector<char> touched(P.size(), 0);
      std::vector<std::pair<uint32_t, uint32_t>> welds;  // (b into a)
      auto consider = [&](uint32_t a, uint32_t b) {
        if (a == b || touched[a] || touched[b] || (!bandV[a] && !bandV[b])) return;
        if (glm::length(P[a] - P[b]) >= tiny) return;
        // only where the two share no neighbor but the third corners of triangles on the edge
        auto na = neighbors(a), nbv = neighbors(b);
        std::unordered_set<uint32_t> allowed;
        for (int64_t f : facesAt[a])
          if (f < 0) {
            auto vs = vertsOf(f);
            if (std::find(vs.begin(), vs.end(), b) != vs.end())
              for (uint32_t x : vs)
                if (x != a && x != b) allowed.insert(x);
          }
        for (uint32_t x : na)
          if (x != b && nbv.count(x) && !allowed.count(x)) return;
        welds.push_back({bandV[a] ? b : a, bandV[a] ? a : b});  // (the zone's own vertex kept)
        touched[a] = touched[b] = 1;
        for (uint32_t x : na) touched[x] = 1;
        for (uint32_t x : nbv) touched[x] = 1;
      };
      for (size_t q = 0; q < quads.size(); ++q)
        if (quadGroup[q] < 0)
          for (int k = 0; k < 4; ++k) consider(quads[q][k], quads[q][(k + 1) % 4]);
      for (size_t t = 0; t < tris.size(); ++t)
        if (triGroup[t] < 0)
          for (int k = 0; k < 3; ++k) consider(tris[t][k], tris[t][(k + 1) % 3]);
      if (welds.empty()) break;
      std::vector<uint32_t> to(P.size());
      for (uint32_t v = 0; v < P.size(); ++v) to[v] = v;
      for (auto [keep, gone] : welds) to[gone] = keep;
      std::vector<Quad> q2;
      std::vector<int> g2;
      for (size_t q = 0; q < quads.size(); ++q) {
        std::vector<uint32_t> vs;
        for (uint32_t x : quads[q])
          if (vs.empty() || vs.back() != to[x]) vs.push_back(to[x]);
        if (vs.size() > 1 && vs.front() == vs.back()) vs.pop_back();
        if (vs.size() == 4) q2.push_back({vs[0], vs[1], vs[2], vs[3]}), g2.push_back(quadGroup[q]);
        else if (vs.size() == 3) tris.push_back({vs[0], vs[1], vs[2]}), triGroup.push_back(quadGroup[q]);
      }
      quads = std::move(q2), quadGroup = std::move(g2);
      std::vector<Tri> t2;
      std::vector<int> h2;
      for (size_t t = 0; t < tris.size(); ++t) {
        uint32_t a = to[tris[t][0]], b = to[tris[t][1]], c = to[tris[t][2]];
        if (a != b && b != c && a != c) t2.push_back({a, b, c}), h2.push_back(triGroup[t]);
      }
      tris = std::move(t2), triGroup = std::move(h2);
      st.welded += (int)welds.size();
    }
  }
  // faces the welds made one vertex twice: a quad with one corner twice is a triangle, anything less goes
  {
    std::vector<Quad> q2;
    std::vector<int> g2;
    for (size_t q = 0; q < quads.size(); ++q) {
      const Quad& Q = quads[q];
      std::vector<uint32_t> u;
      for (int k = 0; k < 4; ++k)
        if (Q[k] != Q[(k + 1) % 4]) u.push_back(Q[k]);
      if (u.size() == 4 && Q[0] != Q[2] && Q[1] != Q[3]) q2.push_back(Q), g2.push_back(quadGroup[q]);
      else if (u.size() == 3 && u[0] != u[2]) tris.push_back({u[0], u[1], u[2]}), triGroup.push_back(quadGroup[q]);
    }
    quads = std::move(q2), quadGroup = std::move(g2);
    std::vector<Tri> t2;
    std::vector<int> h2;
    for (size_t t = 0; t < tris.size(); ++t)
      if (tris[t][0] != tris[t][1] && tris[t][1] != tris[t][2] && tris[t][0] != tris[t][2]) t2.push_back(tris[t]), h2.push_back(triGroup[t]);
    tris = std::move(t2), triGroup = std::move(h2);
  }
  std::unordered_map<EdgeKey, int> uses;
  std::unordered_set<uint64_t> directed;
  bool twice = false;
  auto edge = [&](uint32_t a, uint32_t b) {
    uses[ekey(a, b)]++;
    if (!directed.insert((uint64_t(a) << 32) | b).second) {
      if (!twice && getenv("PATINA_RETOPO_DEBUG"))
        PLOG_I("  edge %u-%u used twice the same way, at %.3f %.3f %.3f", a, b, 0.5f * (P[a].x + P[b].x), 0.5f * (P[a].y + P[b].y),
               0.5f * (P[a].z + P[b].z));
      twice = true;
    }
  };
  for (auto& q : quads)
    for (int k = 0; k < 4; ++k) edge(q[k], q[(k + 1) % 4]);
  for (auto& t : tris)
    for (int k = 0; k < 3; ++k) edge(t[k], t[(k + 1) % 3]);
  size_t over = 0;
  for (auto& [e, n] : uses)
    if (n > 2) {
      ++over;
      if (getenv("PATINA_RETOPO_DEBUG")) {
        vec3 a = P[uint32_t(e >> 32)], b = P[uint32_t(e & 0xFFFFFFFF)];
        PLOG_I("  edge with %d faces: %u %u at %.3f %.3f %.3f", n, uint32_t(e >> 32), uint32_t(e & 0xFFFFFFFF), 0.5f * (a.x + b.x), 0.5f * (a.y + b.y),
               0.5f * (a.z + b.z));
        uint32_t x = uint32_t(e >> 32), y = uint32_t(e & 0xFFFFFFFF);
        for (size_t q = 0; q < quads.size(); ++q) {
          int hits = 0;
          for (uint32_t z : quads[q]) hits += z == x || z == y;
          if (hits == 2) PLOG_I("    quad %zu group %d: %u %u %u %u", q, quadGroup[q], quads[q][0], quads[q][1], quads[q][2], quads[q][3]);
        }
        for (size_t t = 0; t < tris.size(); ++t) {
          int hits = 0;
          for (uint32_t z : tris[t]) hits += z == x || z == y;
          if (hits == 2) PLOG_I("    tri %zu group %d: %u %u %u", t, triGroup[t], tris[t][0], tris[t][1], tris[t][2]);
        }
      }
    }
  // a few edges with more than two faces (or used twice the same way): the joining faces round them taken
  // out and the holes they leave closed again, rather than the whole join given up
  if ((over || twice) && over <= 6) {
    std::unordered_set<uint32_t> badV;
    for (auto& [e, n] : uses)
      if (n > 2) badV.insert(uint32_t(e >> 32)), badV.insert(uint32_t(e & 0xFFFFFFFF));
    {
      std::unordered_set<uint64_t> seenD;
      auto chk = [&](uint32_t a, uint32_t b) {
        if (!seenD.insert((uint64_t(a) << 32) | b).second) badV.insert(a), badV.insert(b);
      };
      for (auto& q : quads)
        for (int k = 0; k < 4; ++k) chk(q[k], q[(k + 1) % 4]);
      for (auto& t : tris)
        for (int k = 0; k < 3; ++k) chk(t[k], t[(k + 1) % 3]);
    }
    std::unordered_set<uint32_t> freed;
    std::vector<Quad> q2;
    std::vector<int> g2;
    for (size_t q = 0; q < quads.size(); ++q) {
      bool out = quadGroup[q] < 0 && std::any_of(quads[q].begin(), quads[q].end(), [&](uint32_t x) { return badV.count(x) > 0; });
      if (out) freed.insert(quads[q].begin(), quads[q].end());
      else q2.push_back(quads[q]), g2.push_back(quadGroup[q]);
    }
    std::vector<Tri> t2;
    std::vector<int> h2;
    for (size_t t = 0; t < tris.size(); ++t) {
      bool out = triGroup[t] < 0 && std::any_of(tris[t].begin(), tris[t].end(), [&](uint32_t x) { return badV.count(x) > 0; });
      if (out) freed.insert(tris[t].begin(), tris[t].end());
      else t2.push_back(tris[t]), h2.push_back(triGroup[t]);
    }
    if (!freed.empty()) {
      quads = std::move(q2), quadGroup = std::move(g2), tris = std::move(t2), triGroup = std::move(h2);
      auto loops = loopsOf(
          quads, tris, [](size_t) { return true; }, [](size_t) { return true; });
      int filled = 0;
      for (auto& loop : loops) {
        if (!std::any_of(loop.begin(), loop.end(), [&](uint32_t v) { return freed.count(v) > 0; }) || loop.size() > 64) continue;
        // (not the model's own open edge, as an eye socket beside the joining faces taken out)
        size_t onOpen = 0;
        for (uint32_t v : loop)
          if (!og.segs.empty() && og.nearest(P[v], [](int) { return true; }).d2 < 0.5f * maxL * maxL) ++onOpen;
        if (onOpen * 2 >= loop.size()) continue;
        std::vector<uint32_t> V(loop.rbegin(), loop.rend());
        fillHole(V, P, quads, quadGroup, tris, triGroup, bandV, st, {}, snap);
        ++filled;
      }
      // (and checked again)
      uses.clear(), directed.clear(), twice = false;
      for (auto& q : quads)
        for (int k = 0; k < 4; ++k) edge(q[k], q[(k + 1) % 4]);
      for (auto& t : tris)
        for (int k = 0; k < 3; ++k) edge(t[k], t[(k + 1) % 3]);
      over = 0;
      for (auto& [e, n] : uses) over += n > 2;
      PLOG_I("Retopology (flow): join: faces overlapping where zones meet taken out, %d holes closed again%s", filled,
             over || twice ? " (not enough)" : "");
    }
  }
  if (over || twice) return fail("the joined quads overlap (" + std::to_string(over) + " edges with more than two faces)");
  if (skipped) PLOG_I("Retopology (flow): join: %d short border stretches left to the holes where zones meet", skipped);
  return true;
}

bool stepRow(std::vector<vec3>& P, std::vector<Quad>& quads, std::vector<Tri>& tris, const std::vector<uint32_t>& F, const std::vector<uint32_t>& C,
             bool closed) {
  std::vector<int> qg(quads.size(), 0), tg(tris.size(), 0);
  std::vector<char> band(P.size(), 0);
  JoinStats st;
  return buildOneRow(P, quads, qg, tris, tg, F, C, closed, band, st, {});
}

void capLoop(std::vector<vec3>& P, std::vector<Quad>& quads, std::vector<Tri>& tris, const std::vector<uint32_t>& V) {
  std::vector<int> qg(quads.size(), 0), tg(tris.size(), 0);
  std::vector<char> band(P.size(), 0);
  JoinStats st;
  fillHole(V, P, quads, qg, tris, tg, band, st, {});
}

}  // namespace patina::retopo
