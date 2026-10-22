#include "core/RetopoRings.h"

#include <Eigen/IterativeLinearSolvers>
#include <Eigen/Sparse>
#include <algorithm>
#include <cmath>
#include <functional>
#include <numeric>
#include <queue>
#include <unordered_map>
#include <unordered_set>

#include "core/RetopoJoin.h"

#include <cstdio>
#include <cstdlib>

namespace patina::retopo {

namespace {

using EdgeKey = uint64_t;
inline EdgeKey ekey(uint32_t a, uint32_t b) { return (uint64_t(std::min(a, b)) << 32) | std::max(a, b); }

// The boundary loops, each the way its triangles run along it (the triangles on its left); empty with
// simple = false where a vertex has two ways on
std::vector<std::vector<uint32_t>> loopsOf(const std::vector<glm::uvec3>& tri, bool& simple) {
  std::unordered_map<EdgeKey, int> uses;
  for (auto& t : tri)
    for (int k = 0; k < 3; ++k) uses[ekey(t[k], t[(k + 1) % 3])]++;
  std::unordered_map<uint32_t, uint32_t> next;
  simple = true;
  for (auto& t : tri)
    for (int k = 0; k < 3; ++k) {
      uint32_t a = t[k], b = t[(k + 1) % 3];
      if (uses[ekey(a, b)] != 1) continue;
      simple &= next.emplace(a, b).second;
    }
  std::vector<std::vector<uint32_t>> loops;
  if (!simple) return loops;
  std::unordered_set<uint32_t> done;
  std::vector<uint32_t> starts;
  for (auto& [a, b] : next) starts.push_back(a);
  std::sort(starts.begin(), starts.end());
  for (uint32_t s : starts) {
    if (done.count(s)) continue;
    std::vector<uint32_t> loop;
    uint32_t v = s;
    for (size_t guard = 0; guard <= next.size(); ++guard) {
      done.insert(v), loop.push_back(v);
      auto it = next.find(v);
      if (it == next.end()) {
        simple = false;
        return {};
      }
      v = it->second;
      if (v == s) break;
    }
    if (v != s) {
      simple = false;
      return {};
    }
    loops.push_back(std::move(loop));
  }
  return loops;
}

double lengthOf(const std::vector<vec3>& Q) {
  double s = 0;
  for (size_t i = 0; i < Q.size(); ++i) s += glm::length(Q[(i + 1) % Q.size()] - Q[i]);
  return s;
}

// n points evenly along a closed polyline, the first where it passes nearest to `near`
std::vector<vec3> resample(const std::vector<vec3>& Q, size_t n, const vec3& near) {
  const size_t m = Q.size();
  std::vector<double> arc(m + 1, 0);
  for (size_t i = 0; i < m; ++i) arc[i + 1] = arc[i] + glm::length(Q[(i + 1) % m] - Q[i]);
  double t0 = 0, bd = 1e30;
  for (size_t i = 0; i < m; ++i) {
    vec3 a = Q[i], b = Q[(i + 1) % m], ab = b - a;
    float l2 = glm::dot(ab, ab), t = l2 > 0 ? std::clamp(glm::dot(near - a, ab) / l2, 0.f, 1.f) : 0.f;
    vec3 p = a + t * ab;
    if (double d = glm::dot(p - near, p - near); d < bd) bd = d, t0 = arc[i] + t * (arc[i + 1] - arc[i]);
  }
  std::vector<vec3> out;
  const double L = arc[m];
  for (size_t j = 0; j < n; ++j) {
    double x = std::fmod(t0 + L * j / n, L);
    size_t i = std::min<size_t>(size_t(std::upper_bound(arc.begin(), arc.end(), x) - arc.begin()), m) - 1;
    double seg = arc[i + 1] - arc[i];
    out.push_back(glm::mix(Q[i], Q[(i + 1) % m], seg > 0 ? float((x - arc[i]) / seg) : 0.f));
  }
  return out;
}

// Of the loops a ring beside this one made, the one along Q (if any): its points exactly, turned to go
// the same way as Q
std::vector<vec3> matchFixed(const std::vector<vec3>& Q, const std::vector<std::vector<vec3>>& fixed, float L) {
  auto distTo = [&](const vec3& p) {
    float bd = 1e30f;
    for (size_t i = 0; i < Q.size(); ++i) {
      vec3 a = Q[i], b = Q[(i + 1) % Q.size()], ab = b - a;
      float l2 = glm::dot(ab, ab), t = l2 > 0 ? std::clamp(glm::dot(p - a, ab) / l2, 0.f, 1.f) : 0.f;
      bd = std::min(bd, glm::length(a + t * ab - p));
    }
    return bd;
  };
  auto nearestOn = [&](const vec3& p) {
    float bd = 1e30f;
    vec3 best = p;
    for (size_t i = 0; i < Q.size(); ++i) {
      vec3 a = Q[i], b = Q[(i + 1) % Q.size()], ab = b - a;
      float l2 = glm::dot(ab, ab), t = l2 > 0 ? std::clamp(glm::dot(p - a, ab) / l2, 0.f, 1.f) : 0.f;
      if (float d = glm::length(a + t * ab - p); d < bd) bd = d, best = a + t * ab;
    }
    return best;
  };
  for (auto& F0 : fixed) {
    if (F0.size() < 3) continue;
    // (a loop much denser or sparser than the quads, as a zone's edge that zigzags along the line, would
    // crowd or stretch the rings: joined by a row instead)
    if (double sp = lengthOf(F0) / F0.size(); sp < 0.5 * L || sp > 1.75 * L) continue;
    // (all the way along Q: its points near Q and Q's near it; Q itself zigzags along the triangles, so
    // lengths don't compare. A few points off it, where a zone's edge loop leaves the line for a quad,
    // are put onto it: a notch would fold the row along it, and joining by a row costs a 3-to-1 step
    // for each two points more on one side)
    std::vector<vec3> F = F0;
    size_t moved = 0, firstOn = F.size();
    bool far = false;
    for (size_t i = 0; i < F.size() && !far; ++i) {
      float d = distTo(F[i]);
      if (d <= 0.4f * L) {
        firstOn = std::min(firstOn, i);
        continue;
      }
      far = d > 1.0f * L || ++moved > movableLoopPoints(F.size());
      F[i] = nearestOn(F[i]);
    }
    if (far || firstOn == F.size()) continue;
    if (moved) std::rotate(F.begin(), F.begin() + (long)firstOn, F.end());  // (starting at a point not moved)
    float back = 0;
    for (size_t i = 0; i < Q.size() && back <= 0.6f * L; i += std::max<size_t>(1, Q.size() / 64)) {
      float bd = 1e30f;
      for (size_t j = 0; j < F.size(); ++j) {
        vec3 a = F[j], b = F[(j + 1) % F.size()], ab = b - a;
        float l2 = glm::dot(ab, ab), t = l2 > 0 ? std::clamp(glm::dot(Q[i] - a, ab) / l2, 0.f, 1.f) : 0.f;
        bd = std::min(bd, glm::length(a + t * ab - Q[i]));
      }
      back = std::max(back, bd);
    }
    if (back > 0.6f * L) continue;
    // the same way round as Q: where F's first two points fall along Q
    auto paramOf = [&](const vec3& p) {
      double best = 1e30, t = 0, acc = 0;
      for (size_t i = 0; i < Q.size(); ++i) {
        vec3 a = Q[i], b = Q[(i + 1) % Q.size()], ab = b - a;
        float l2 = glm::dot(ab, ab), f = l2 > 0 ? std::clamp(glm::dot(p - a, ab) / l2, 0.f, 1.f) : 0.f;
        double d = glm::length(a + f * ab - p), l = std::sqrt(l2);
        if (d < best) best = d, t = acc + f * l;
        acc += l;
      }
      return std::pair{t, acc};
    };
    auto [t0, len] = paramOf(F[0]);
    auto [t1, len1] = paramOf(F[1]);
    double dt = t1 - t0;
    dt -= len * std::round(dt / len);
    std::vector<vec3> G = F;
    if (dt < 0) std::reverse(G.begin() + 1, G.end());  // (still starting at F[0])
    // (a point that steps back or crowds the one before, where the zone's edge loop jogs across the
    // line: slid along it to halfway between its neighbors, as one of the few points moved)
    {
      auto pointAt = [&](double t) {
        t -= len * std::floor(t / len);
        double acc = 0;
        for (size_t i = 0; i < Q.size(); ++i) {
          vec3 a = Q[i], b = Q[(i + 1) % Q.size()];
          double l = glm::length(b - a);
          if (acc + l >= t && l > 0) return a + (b - a) * float((t - acc) / l);
          acc += l;
        }
        return Q[0];
      };
      const size_t n = G.size();
      std::vector<double> t(n);
      for (size_t i = 0; i < n; ++i) t[i] = paramOf(G[i]).first;
      auto fwd = [&](double a, double b) {  // (from a forwards to b, -len/2..len/2)
        double d = b - a;
        d -= len * std::floor(d / len);
        return d > 0.5 * len ? d - len : d;
      };
      for (size_t i = 1; i < n; ++i) {
        size_t nx = (i + 1) % n;
        if (fwd(t[i - 1], t[i]) > 0.15 * L && fwd(t[i], t[nx]) > 0.15 * L) continue;
        double gap = fwd(t[i - 1], t[nx]);
        if (gap <= 0.3 * L || ++moved > movableLoopPoints(n)) break;
        t[i] = t[i - 1] + 0.5 * gap, G[i] = pointAt(t[i]);
      }
    }
    // (and its points going forwards along Q all the way round, none doubling back or crowding: a zone's
    // edge that zigzags along the line would leave its quads there turned over)
    {
      bool ok = true;
      double prev = paramOf(G[0]).first, traveled = 0;
      for (size_t i = 1; i <= G.size() && ok; ++i) {
        double t = paramOf(G[i % G.size()]).first, d = t - prev;
        d -= len * std::floor(d / len);  // (forwards, 0..len)
        if (d > 0.5 * len) d -= len;
        ok &= d > 0.15 * L;
        traveled += d, prev = t;
      }
      if (!ok || std::abs(traveled - len) > 0.05 * len) continue;
    }
    return G;
  }
  return {};
}

}  // namespace

size_t movableLoopPoints(size_t n) { return std::max<size_t>(3, n / 5); }

size_t matchingLoopCount(const std::vector<vec3>& Q, const std::vector<std::vector<vec3>>& fixed, float L) {
  return matchFixed(Q, fixed, L).size();
}

std::vector<long> planRingCounts(const std::vector<RingChainLoop>& loops, const std::vector<std::array<int, 2>>& rings) {
  const int n = (int)loops.size();
  std::vector<long> out(n, 0), lo(n), hi(n);
  for (int i = 0; i < n; ++i) {
    lo[i] = std::max<long>(4, (long)std::ceil(loops[i].lengthQ / kRingMaxW));
    hi[i] = std::max<long>(lo[i], (long)std::floor(loops[i].lengthQ / kRingMinW));
    hi[i] = std::min(hi[i], lo[i] + 400);
    if (loops[i].fixedCount >= 4) lo[i] = hi[i] = loops[i].fixedCount;
  }
  std::vector<std::vector<std::pair<int, int>>> adj(n);  // (the loop beyond, through which ring)
  for (int r = 0; r < (int)rings.size(); ++r) {
    auto [a, b] = rings[r];
    if (a < 0 || b < 0 || a >= n || b >= n || a == b) continue;
    adj[a].push_back({b, r}), adj[b].push_back({a, r});
  }
  auto own = [&](int i, long c) {  // (a loop meeting another zone: steps there, a little cheaper)
    if (loops[i].fixedCount >= 4) return 0.0;
    // (a loop that takes any count: even, so a disc beside it can be a grid)
    return (loops[i].meetsZone ? 0.9 : 0.01) * std::abs(double(c) - loops[i].lengthQ) + (!loops[i].meetsZone && c % 2 ? 3.0 : 0.0);
  };
  std::vector<char> seen(n, 0);
  for (int s = 0; s < n; ++s) {
    if (seen[s] || adj[s].empty()) continue;
    // the chain this loop is on: from one end to the other (a chain only: no loop of rings, no forks)
    std::vector<int> comp;
    std::vector<int> stack{s};
    seen[s] = 1;
    bool path = true;
    size_t links = 0;
    while (!stack.empty()) {
      int v = stack.back();
      stack.pop_back();
      comp.push_back(v);
      path &= adj[v].size() <= 2;
      links += adj[v].size();
      for (auto [w, r] : adj[v])
        if (!seen[w]) seen[w] = 1, stack.push_back(w);
    }
    if (!path || links / 2 != comp.size() - 1) continue;
    int end = comp[0];
    for (int v : comp)
      if (adj[v].size() == 1) end = v;
    std::vector<int> order{end};
    for (int prev = -1, v = end; order.size() < comp.size();) {
      int nx = -1;
      for (auto [w, r] : adj[v])
        if (w != prev) nx = w;
      if (nx < 0) break;
      order.push_back(nx), prev = v, v = nx;
    }
    if (order.size() != comp.size()) continue;
    // along the chain: the least cost of each count at each loop, and where it came from
    std::vector<std::vector<double>> f(order.size());
    std::vector<std::vector<long>> from(order.size());
    for (size_t k = 0; k < order.size(); ++k) {
      int v = order[k];
      f[k].assign(hi[v] - lo[v] + 1, 1e30), from[k].assign(hi[v] - lo[v] + 1, 0);
      for (long c = lo[v]; c <= hi[v]; ++c) {
        double base = own(v, c);
        if (k == 0) {
          f[k][c - lo[v]] = base;
          continue;
        }
        int u = order[k - 1];
        for (long c2 = lo[u]; c2 <= hi[u]; ++c2) {
          double x = f[k - 1][c2 - lo[u]] + base + std::abs(double(c - c2)) + ((c - c2) % 2 ? 1.5 : 0.0);
          if (x < f[k][c - lo[v]] - 1e-9) f[k][c - lo[v]] = x, from[k][c - lo[v]] = c2;
        }
      }
    }
    size_t K = order.size() - 1;
    long c = lo[order[K]];
    for (long x = lo[order[K]]; x <= hi[order[K]]; ++x)
      if (f[K][x - lo[order[K]]] < f[K][c - lo[order[K]]] - 1e-9) c = x;
    for (size_t k = K + 1; k-- > 0;) {
      out[order[k]] = c;
      if (k > 0) c = from[k][c - lo[order[k]]];
    }
  }
  return out;
}

bool isRing(const std::vector<glm::uvec3>& tri) {
  if (tri.empty()) return false;
  bool simple;
  auto loops = loopsOf(tri, simple);
  if (!simple || loops.size() != 2) return false;
  std::unordered_set<uint32_t> verts;
  std::unordered_set<EdgeKey> edges;
  std::unordered_map<uint32_t, uint32_t> parent;
  std::function<uint32_t(uint32_t)> find = [&](uint32_t x) {
    while (parent[x] != x) x = parent[x] = parent[parent[x]];
    return x;
  };
  for (auto& t : tri)
    for (int k = 0; k < 3; ++k) {
      verts.insert(t[k]), edges.insert(ekey(t[k], t[(k + 1) % 3]));
      if (!parent.count(t[k])) parent[t[k]] = t[k];
    }
  for (auto& t : tri) parent[find(t[1])] = find(t[0]), parent[find(t[2])] = find(t[0]);
  std::unordered_set<uint32_t> roots;
  for (uint32_t v : verts) roots.insert(find(v));
  // (one piece, and V - E + F = 0: no handles)
  return roots.size() == 1 && (long)verts.size() - (long)edges.size() + (long)tri.size() == 0;
}

RingFill fillRings(const std::vector<vec3>& pos, const std::vector<glm::uvec3>& tri, float L, const std::vector<std::vector<vec3>>& fixed,
                   const std::vector<char>& loose, const std::vector<RingPlanLoop>& plan) {
  RingFill out;
  bool simple;
  auto loops = loopsOf(tri, simple);
  if (!simple || loops.size() != 2) {
    out.why = "not a ring";
    return out;
  }
  const size_t n = pos.size();
  auto polyOf = [&](const std::vector<uint32_t>& l) {
    std::vector<vec3> Q;
    for (uint32_t v : l) Q.push_back(pos[v]);
    return Q;
  };
  // A: the shorter loop (the rings run from it to B)
  if (lengthOf(polyOf(loops[0])) > lengthOf(polyOf(loops[1]))) std::swap(loops[0], loops[1]);
  const auto& A = loops[0];
  const auto& B = loops[1];
  // ---- u: 0 on A, 1 on B, harmonic between (cotangent weights, kept positive)
  std::vector<double> u(n, 0.5);
  std::vector<char> fixedV(n, 0), used(n, 0);
  for (uint32_t v : A) u[v] = 0, fixedV[v] = 1;
  for (uint32_t v : B) u[v] = 1, fixedV[v] = 1;
  std::unordered_map<EdgeKey, double> w;
  for (auto& t : tri)
    for (int k = 0; k < 3; ++k) {
      used[t[k]] = 1;
      const vec3 &o = pos[t[k]], &a = pos[t[(k + 1) % 3]], &b = pos[t[(k + 2) % 3]];
      vec3 e1 = a - o, e2 = b - o;
      double s = glm::length(glm::cross(e1, e2)), c = glm::dot(e1, e2);
      w[ekey(t[(k + 1) % 3], t[(k + 2) % 3])] += 0.5 * (s > 1e-20 ? c / s : 0.0);
    }
  double meanW = 0;
  for (auto& [e, x] : w) meanW += std::abs(x) / w.size();
  std::vector<int> idx(n, -1);
  int m = 0;
  for (size_t v = 0; v < n; ++v)
    if (used[v] && !fixedV[v]) idx[v] = m++;
  if (m > 0) {
    std::vector<Eigen::Triplet<double>> T;
    Eigen::VectorXd rhs = Eigen::VectorXd::Zero(m), x0(m);
    for (auto& [e, x] : w) {
      double we = std::max(x, 1e-3 * meanW);
      uint32_t a = uint32_t(e >> 32), b = uint32_t(e & 0xFFFFFFFF);
      for (auto [p, q] : {std::pair{a, b}, std::pair{b, a}}) {
        if (idx[p] < 0) continue;
        T.push_back({idx[p], idx[p], we});
        if (idx[q] >= 0) T.push_back({idx[p], idx[q], -we});
        else rhs[idx[p]] += we * u[q];
      }
    }
    Eigen::SparseMatrix<double> M(m, m);
    M.setFromTriplets(T.begin(), T.end());
    for (size_t v = 0; v < n; ++v)
      if (idx[v] >= 0) x0[idx[v]] = 0.5;
    Eigen::ConjugateGradient<Eigen::SparseMatrix<double>, Eigen::Lower | Eigen::Upper> cg;
    cg.setTolerance(1e-9);
    cg.setMaxIterations(4000);
    cg.compute(M);
    Eigen::VectorXd x = cg.solveWithGuess(rhs, x0);
    for (size_t v = 0; v < n; ++v)
      if (idx[v] >= 0) u[v] = std::clamp(x[idx[v]], 0.0, 1.0);
  }
  // ---- how far each vertex is from A (along the edges), for rings about a quad apart
  std::vector<std::vector<std::pair<uint32_t, float>>> adj(n);
  for (auto& [e, x] : w) {
    uint32_t a = uint32_t(e >> 32), b = uint32_t(e & 0xFFFFFFFF);
    float l = glm::length(pos[a] - pos[b]);
    adj[a].push_back({b, l}), adj[b].push_back({a, l});
  }
  std::vector<float> dA(n, 1e30f);
  using QE = std::pair<float, uint32_t>;
  std::priority_queue<QE, std::vector<QE>, std::greater<>> pq;
  for (uint32_t v : A) dA[v] = 0, pq.push({0.f, v});
  while (!pq.empty()) {
    auto [d, v] = pq.top();
    pq.pop();
    if (d > dA[v]) continue;
    for (auto [o, l] : adj[v])
      if (d + l < dA[o]) dA[o] = d + l, pq.push({d + l, o});
  }
  double D = 0;
  std::vector<double> widths;
  for (uint32_t v : B) D += dA[v] / B.size(), widths.push_back(dA[v]);
  // (only a band: about as much surface as its width along its loops; a body between two wrists is a
  // ring too, but not one to fill with rings)
  {
    double area = 0;
    for (auto& t : tri) area += 0.5 * glm::length(glm::cross(pos[t.y] - pos[t.x], pos[t.z] - pos[t.x]));
    const double band = D * 0.5 * (lengthOf(polyOf(A)) + lengthOf(polyOf(B)));
    if (area > 1.6 * band) {
      out.why = "not a band between its loops";
      return out;
    }
  }
  // (as many rows as the narrow side holds, rows there at least three quarters of a quad apart; the wide
  // side's quads are a little longer, rather than slivers at the narrow side)
  std::sort(widths.begin(), widths.end());
  const double narrow = widths.empty() ? D : widths[widths.size() / 5];
  // (a band far narrower than a quad somewhere, where its lines almost touch: its row there would be
  // slivers)
  if (!widths.empty() && widths[widths.size() / 20] < 0.35 * L) {
    out.why = "narrower than a third of a quad in places";
    return out;
  }
  const int rows = std::max(1, std::min((int)std::lround(D / L), (int)std::lround(narrow / (0.75 * L))));
  // u at a given mean distance: vertices binned by u, the mean distance in each bin (kept rising)
  const int bins = 64;
  std::vector<double> sum(bins, 0), cnt(bins, 0);
  for (size_t v = 0; v < n; ++v)
    if (used[v] && dA[v] < 1e29f) {
      int b = std::min(bins - 1, (int)(u[v] * bins));
      sum[b] += dA[v], cnt[b] += 1;
    }
  std::vector<std::pair<double, double>> curve = {{0.0, 0.0}};  // (distance, u)
  for (int b = 0; b < bins; ++b)
    if (cnt[b] > 0) curve.push_back({std::max(curve.back().first, sum[b] / cnt[b]), (b + 0.5) / bins});
  curve.push_back({std::max(curve.back().first, D), 1.0});
  auto uAt = [&](double dist) {
    for (size_t i = 1; i < curve.size(); ++i)
      if (curve[i].first >= dist) {
        double span = curve[i].first - curve[i - 1].first;
        double f = span > 0 ? (dist - curve[i - 1].first) / span : 0;
        return curve[i - 1].second + f * (curve[i].second - curve[i - 1].second);
      }
    return 1.0;
  };
  // ---- the rings: A, the level lines between, B; all with the higher u on their left
  auto levelLine = [&](double c) -> std::vector<vec3> {
    std::unordered_map<EdgeKey, uint32_t> keyIdx;
    std::vector<vec3> pts;
    std::unordered_map<uint32_t, uint32_t> next;
    auto at = [&](uint32_t a, uint32_t b) {
      EdgeKey k = ekey(a, b);
      auto it = keyIdx.find(k);
      if (it != keyIdx.end()) return it->second;
      double t = (c - u[a]) / (u[b] - u[a]);
      pts.push_back(glm::mix(pos[a], pos[b], (float)std::clamp(t, 0.0, 1.0)));
      keyIdx[k] = (uint32_t)pts.size() - 1;
      return (uint32_t)pts.size() - 1;
    };
    for (auto& t : tri) {
      int down = -1, up = -1;
      uint32_t dk = 0, uk = 0;
      for (int k = 0; k < 3; ++k) {
        uint32_t a = t[k], b = t[(k + 1) % 3];
        if (u[a] < c && c <= u[b]) up = k, uk = at(a, b);
        else if (u[a] >= c && c > u[b]) down = k, dk = at(a, b);
      }
      if (up >= 0 && down >= 0) next[dk] = uk;
    }
    // the longest loop
    std::vector<vec3> best;
    std::unordered_set<uint32_t> seen;
    for (auto& [s0, nx] : next) {
      if (seen.count(s0)) continue;
      std::vector<vec3> loop;
      uint32_t v = s0;
      bool closed = false;
      for (size_t guard = 0; guard <= next.size(); ++guard) {
        seen.insert(v), loop.push_back(pts[v]);
        auto it = next.find(v);
        if (it == next.end()) break;
        v = it->second;
        if (v == s0) {
          closed = true;
          break;
        }
      }
      if (closed && lengthOf(loop) > lengthOf(best)) best = std::move(loop);
    }
    return best;
  };
  std::vector<std::vector<vec3>> rings;
  rings.push_back(polyOf(A));
  for (int i = 1; i < rows; ++i) {
    double c = std::clamp(uAt(D * i / rows), 0.01, 0.99);
    auto Q = levelLine(c);
    if (Q.size() < 3) {
      out.why = "a ring didn't come out whole";
      return out;
    }
    rings.push_back(std::move(Q));
  }
  {
    auto Q = polyOf(B);
    std::reverse(Q.begin(), Q.end());
    rings.push_back(std::move(Q));
  }
  // ---- loops given by a ring beside this one: those points exactly (turned to go the same way)
  std::vector<vec3> givenA = matchFixed(rings.front(), fixed, L), givenB = matchFixed(rings.back(), fixed, L);
  // ---- the count round each ring: kept unless the quads would stretch too far, then changed by an
  // even number (3-to-1 steps only); between two given loops, stepped evenly from one count to the other
  // (the loops themselves zigzag along the working triangles: their lengths are taken from level lines
  // just inside them)
  std::vector<double> lens(rings.size());
  for (size_t i = 0; i < rings.size(); ++i) lens[i] = lengthOf(rings[i]);
  {
    auto Q0 = levelLine(0.03), Q1 = levelLine(0.97);
    if (Q0.size() >= 3) lens.front() = std::min(lens.front(), lengthOf(Q0));
    if (Q1.size() >= 3) lens.back() = std::min(lens.back(), lengthOf(Q1));
  }
  // ---- a band: its rings no longer than its loops; much longer ones run up and round something between
  // the lines (the ears on the back of a head), and rings across that would leave most of it out
  {
    double ends = std::max(lens.front(), lens.back()), mid = 0;
    for (size_t i = 1; i + 1 < lens.size(); ++i) mid = std::max(mid, lens[i]);
    if (mid > 1.6 * ends) {
      out.why = "not a band: its rings run much longer between its lines";
      return out;
    }
  }
  // the counts at A and B: each loop's quads between kMinW and kMaxW of a quad wide (hand-made eye loops
  // keep their count and widen outwards). Every 3-to-1 step costs two poles, inside the ring or where it
  // meets the zone beside it (which has about its loop's length / L points there): the counts with the
  // fewest steps in all are kept. A loose loop's border takes any count (no steps there); a given loop
  // has its count. Stepped evenly between by even numbers (3-to-1 steps only, an odd step at most once).
  std::vector<size_t> count(rings.size());
  const long k = (long)rings.size() - 1;
  const double kMinW = kRingMinW, kMaxW = kRingMaxW;
  auto isLoose = [&](const std::vector<uint32_t>& l) {
    if (loose.empty()) return false;
    size_t c = 0;
    for (uint32_t v : l) c += v < loose.size() && loose[v];
    return c * 2 > l.size();
  };
  const bool looseA = givenA.empty() && isLoose(A), looseB = givenB.empty() && isLoose(B);
  const double n0 = lens[0] / L, nk = lens[k] / L;
  auto range = [&](const std::vector<vec3>& given, double n) {
    if (!given.empty()) return std::pair<long, long>{(long)given.size(), (long)given.size()};
    long lo = std::max<long>(4, (long)std::ceil(n / kMaxW)), hi = std::max<long>(4, (long)std::floor(n / kMinW));
    if (hi < lo) lo = hi = std::max<long>(4, std::lround(n));
    return std::pair<long, long>{lo, hi};
  };
  auto [lo0, hi0] = range(givenA, n0);
  auto [lok, hik] = range(givenB, nk);
  long c0 = std::max<long>(4, std::lround(n0)), ck = std::max<long>(4, std::lround(nk));
  double best = 1e30;
  for (long a = lo0; a <= hi0; ++a)
    for (long b = lok; b <= hik; ++b) {
      double cost = std::abs(a - b) + ((a - b) % 2 ? 1.5 : 0.0);
      if (givenA.empty()) cost += (looseA ? 0.01 : 0.9) * std::abs(a - n0);
      if (givenB.empty()) cost += (looseB ? 0.01 : 0.9) * std::abs(b - nk);
      if (cost < best - 1e-9) best = cost, c0 = a, ck = b;
    }
  // (counts planned for the whole chain of rings beforehand, where there are)
  auto planned = [&](const std::vector<vec3>& Q) -> long {
    vec3 c(0);
    for (auto& p : Q) c += p / float(Q.size());
    const double len = lengthOf(Q);
    long got = 0;
    double bd = 1e30;
    for (auto& pl : plan) {
      double d = glm::length(pl.center - c);
      if (d < std::max(0.5 * L, 0.05 * len) && pl.length > 0.5 * len && pl.length < 2.0 * len && d < bd) bd = d, got = pl.count;
    }
    return got;
  };
  if (long p = givenA.empty() ? planned(rings.front()) : 0; p >= 4) c0 = p;
  if (long p = givenB.empty() ? planned(rings.back()) : 0; p >= 4) ck = p;
  for (long i = 0; i <= k; ++i) count[i] = (size_t)std::max<long>(4, c0 + 2 * (long)std::lround((ck - c0) * double(i) / k / 2));
  count[0] = (size_t)c0, count[k] = (size_t)ck;
  // ---- the vertices (each ring starting across from the one before) and the rows between
  std::vector<std::vector<uint32_t>> R;
  vec3 start = !givenA.empty() ? givenA[0] : rings[0][0];
  for (size_t i = 0; i < rings.size(); ++i) {
    std::vector<vec3> Q;
    if (i == 0 && !givenA.empty()) Q = givenA;
    else if (i + 1 == rings.size() && !givenB.empty()) {
      // (starting across from the ring before)
      Q = givenB;
      size_t bi = 0;
      float bd = 1e30f;
      for (size_t j = 0; j < Q.size(); ++j)
        if (float d = glm::length(Q[j] - start); d < bd) bd = d, bi = j;
      std::rotate(Q.begin(), Q.begin() + (long)bi, Q.end());
    } else Q = resample(rings[i], count[i], start);
    start = Q[0];
    std::vector<uint32_t> ids;
    for (auto& p : Q) out.pos.push_back(p), ids.push_back((uint32_t)out.pos.size() - 1);
    R.push_back(std::move(ids));
  }
  for (size_t i = 0; i + 1 < R.size(); ++i) {
    size_t q0 = out.quads.size(), t0 = out.tris.size();
    if (!stepRow(out.pos, out.quads, out.tris, R[i], R[i + 1], true)) {
      out.why = "two rings (" + std::to_string(R[i].size()) + " and " + std::to_string(R[i + 1].size()) + ") couldn't be stepped between";
      return out;
    }
    // (the row lies on the left of ring i: its faces run along ring i the way the ring goes)
    bool flip = false;
    std::unordered_set<uint64_t> fwd;
    for (size_t j = 0; j < R[i].size(); ++j) fwd.insert((uint64_t(R[i][j]) << 32) | R[i][(j + 1) % R[i].size()]);
    for (size_t q = q0; q < out.quads.size() && !flip; ++q)
      for (int k = 0; k < 4; ++k)
        if (fwd.count((uint64_t(out.quads[q][(k + 1) % 4]) << 32) | out.quads[q][k])) flip = true;
    if (flip) {
      for (size_t q = q0; q < out.quads.size(); ++q) std::reverse(out.quads[q].begin(), out.quads[q].end());
      for (size_t t = t0; t < out.tris.size(); ++t) std::swap(out.tris[t][1], out.tris[t][2]);
    }
  }
  if (getenv("PATINA_RETOPO_DEBUG")) {
    std::string cs;
    for (size_t i = 0; i < R.size(); ++i) cs += " " + std::to_string(R[i].size());
    auto cen = [](const std::vector<vec3>& Q) { vec3 c(0); for (auto& p : Q) c += p / float(Q.size()); return c; };
    vec3 ca = cen(rings.front()), cb = cen(rings.back());
    fprintf(stderr, "  ring counts:%s (given %zu %zu) A at %.3f %.3f %.3f len %.1f L, B at %.3f %.3f %.3f len %.1f L\n", cs.c_str(), givenA.size(), givenB.size(),
            ca.x, ca.y, ca.z, lens.front() / L, cb.x, cb.y, cb.z, lens.back() / L);
  }
  // ---- the rings cover about the zone's own surface (rings that cut across a bulge between the lines,
  // or fold, cover much less or more): else made as the other zones are
  {
    double zoneArea = 0, quadArea = 0;
    for (auto& t : tri) zoneArea += 0.5 * glm::length(glm::cross(pos[t.y] - pos[t.x], pos[t.z] - pos[t.x]));
    for (auto& q : out.quads)
      quadArea += 0.5 * glm::length(glm::cross(out.pos[q[2]] - out.pos[q[0]], out.pos[q[3]] - out.pos[q[1]]));
    for (auto& t : out.tris) quadArea += 0.5 * glm::length(glm::cross(out.pos[t[1]] - out.pos[t[0]], out.pos[t[2]] - out.pos[t[0]]));
    if (zoneArea > 0 && (quadArea < 0.75 * zoneArea || quadArea > 1.4 * zoneArea)) {
      out.why = "the rings cover " + std::to_string(int(100 * quadArea / zoneArea)) + "% of the zone's surface";
      out.quads.clear(), out.tris.clear(), out.pos.clear();
      return out;
    }
    // (and with about as many quads as its surface asks for: the rows are as many as the narrow side
    // holds, so a zone that bulges between its lines, as the back of a head, gets rows far apart there)
    const double asked = zoneArea / (double(L) * L);
    if (asked > 8 && double(out.quads.size() + out.tris.size()) < 0.5 * asked) {
      out.why = "the rings would be far apart (" + std::to_string(out.quads.size()) + " quads where the zone asks for about " +
                std::to_string(int(asked)) + ")";
      out.quads.clear(), out.tris.clear(), out.pos.clear();
      return out;
    }
  }
  out.rings = (int)rings.size() - 1;
  for (size_t i : {size_t(0), R.size() - 1}) {
    out.loopVerts.push_back(R[i]);
    std::vector<vec3> Q;
    for (uint32_t v : R[i]) Q.push_back(out.pos[v]);
    out.loopPos.push_back(std::move(Q));
  }
  out.ok = !out.quads.empty();
  return out;
}

bool isDisc(const std::vector<glm::uvec3>& tri) {
  if (tri.empty()) return false;
  bool simple;
  auto loops = loopsOf(tri, simple);
  if (!simple || loops.size() != 1) return false;
  std::unordered_set<uint32_t> verts;
  std::unordered_set<EdgeKey> edges;
  for (auto& t : tri)
    for (int k = 0; k < 3; ++k) verts.insert(t[k]), edges.insert(ekey(t[k], t[(k + 1) % 3]));
  return (long)verts.size() - (long)edges.size() + (long)tri.size() == 1;
}

namespace {

// A disc closed with a grid (as a grid fill): its loop of points B (an even count, the same way round as
// loop, the disc's own outline on the working triangles) split into four sides of n, m, n, m edges with
// the corners where it turns most; the disc mapped onto a square with those sides (harmonic, with
// positive weights, so it doesn't fold), and an n x m grid laid back on the surface through it. All its
// inner vertices have four edges; the four corners are the poles a disc needs.
bool gridFill(const std::vector<vec3>& pos, const std::vector<glm::uvec3>& tri, const std::vector<uint32_t>& loop,
              const std::vector<vec3>& B, RingFill& out, bool inset = false) {
  const long c = (long)B.size();
  if (c < 4 || c % 2 || loop.size() < 3) return false;
  double area = 0;
  for (auto& t : tri) area += 0.5 * glm::length(glm::cross(pos[t.y] - pos[t.x], pos[t.z] - pos[t.x]));
  const double Lb = lengthOf(B) / c;
  if (Lb <= 0) return false;
  // the sides: n + m = c / 2, n * m about the disc's area in quads of the loop's edge length
  const long S = c / 2;
  const double Aq = area / (Lb * Lb), disc = double(S) * S - 4 * Aq;
  long n = disc > 0 ? std::lround((S - std::sqrt(disc)) / 2) : S / 2;
  n = std::clamp<long>(n, 1, S / 2);
  const long m = S - n;
  // the corners: where the loop turns most (over two of its edges each way)
  const long w = c >= 8 ? 2 : 1;
  std::vector<double> turn(c);
  for (long k = 0; k < c; ++k) {
    vec3 a = B[k] - B[(k - w + c) % c], b = B[(k + w) % c] - B[k];
    float la = glm::length(a), lb = glm::length(b);
    turn[k] = la > 0 && lb > 0 ? std::acos(std::clamp(glm::dot(a, b) / (la * lb), -1.f, 1.f)) : 0.0;
  }
  long s0 = 0;
  double best = -1;
  for (long s = 0; s < c; ++s) {
    double sc = turn[s] + turn[(s + n) % c] + turn[(s + n + m) % c] + turn[(s + 2 * n + m) % c];
    if (sc > best + 1e-9) best = sc, s0 = s;
  }
  // where each of the outline's vertices falls between the points of B (t: k at B[k])
  const size_t nq = loop.size();
  std::vector<double> arc(nq + 1, 0);
  for (size_t i = 0; i < nq; ++i) arc[i + 1] = arc[i] + glm::length(pos[loop[(i + 1) % nq]] - pos[loop[i]]);
  const double total = arc[nq];
  if (total <= 0) return false;
  auto arcOf = [&](const vec3& p) {
    double bd = 1e30, at = 0;
    for (size_t i = 0; i < nq; ++i) {
      vec3 a = pos[loop[i]], b = pos[loop[(i + 1) % nq]], ab = b - a;
      float l2 = glm::dot(ab, ab), f = l2 > 0 ? std::clamp(glm::dot(p - a, ab) / l2, 0.f, 1.f) : 0.f;
      vec3 q = a + f * ab;
      if (double d = glm::dot(q - p, q - p); d < bd) bd = d, at = arc[i] + f * (arc[i + 1] - arc[i]);
    }
    return at;
  };
  std::vector<double> sb(c + 1);
  const double base = arcOf(B[0]);
  for (long k = 0; k < c; ++k) {
    double x = std::fmod(arcOf(B[k]) - base + 2 * total, total);
    if (k > 0 && x <= sb[k - 1]) return false;  // (B doesn't go round the outline in order)
    sb[k] = x;
  }
  sb[c] = total;
  auto square = [&](double t) {  // the point of the square's outline at t
    double u = std::fmod(t - s0 + 2.0 * c, double(c));
    if (u <= n) return Eigen::Vector2d(u / n, 0);
    if (u <= n + m) return Eigen::Vector2d(1, (u - n) / m);
    if (u <= 2 * n + m) return Eigen::Vector2d(1 - (u - n - m) / n, 1);
    return Eigen::Vector2d(0, 1 - (u - 2 * n - m) / m);
  };
  const size_t nv = pos.size();
  std::vector<Eigen::Vector2d> uv(nv, Eigen::Vector2d(0.5, 0.5));
  std::vector<char> fixedV(nv, 0), used(nv, 0);
  for (size_t i = 0; i < nq; ++i) {
    double x = std::fmod(arc[i] - base + 2 * total, total);
    long k = long(std::upper_bound(sb.begin(), sb.end(), x) - sb.begin()) - 1;
    k = std::clamp<long>(k, 0, c - 1);
    double f = sb[k + 1] > sb[k] ? (x - sb[k]) / (sb[k + 1] - sb[k]) : 0.0;
    uv[loop[i]] = square(k + f), fixedV[loop[i]] = 1;
  }
  // ---- the harmonic map (cotangent weights, kept positive)
  std::unordered_map<EdgeKey, double> wgt;
  for (auto& t : tri)
    for (int k = 0; k < 3; ++k) {
      used[t[k]] = 1;
      const vec3 &o = pos[t[k]], &a = pos[t[(k + 1) % 3]], &b = pos[t[(k + 2) % 3]];
      vec3 e1 = a - o, e2 = b - o;
      double sn = glm::length(glm::cross(e1, e2)), cs = glm::dot(e1, e2);
      wgt[ekey(t[(k + 1) % 3], t[(k + 2) % 3])] += 0.5 * (sn > 1e-20 ? cs / sn : 0.0);
    }
  double meanW = 0;
  for (auto& [e, x] : wgt) meanW += std::abs(x) / wgt.size();
  std::vector<int> idx(nv, -1);
  int mm = 0;
  for (size_t v = 0; v < nv; ++v)
    if (used[v] && !fixedV[v]) idx[v] = mm++;
  if (mm > 0) {
    std::vector<Eigen::Triplet<double>> T;
    Eigen::MatrixXd rhs = Eigen::MatrixXd::Zero(mm, 2);
    for (auto& [e, x] : wgt) {
      double we = std::max(x, 1e-3 * meanW);
      uint32_t a = uint32_t(e >> 32), b = uint32_t(e & 0xFFFFFFFF);
      for (auto [p, q] : {std::pair{a, b}, std::pair{b, a}}) {
        if (idx[p] < 0) continue;
        T.push_back({idx[p], idx[p], we});
        if (idx[q] >= 0) T.push_back({idx[p], idx[q], -we});
        else rhs(idx[p], 0) += we * uv[q].x(), rhs(idx[p], 1) += we * uv[q].y();
      }
    }
    Eigen::SparseMatrix<double> M(mm, mm);
    M.setFromTriplets(T.begin(), T.end());
    Eigen::ConjugateGradient<Eigen::SparseMatrix<double>, Eigen::Lower | Eigen::Upper> cg;
    cg.setTolerance(1e-10);
    cg.setMaxIterations(6000);
    cg.compute(M);
    for (int d = 0; d < 2; ++d) {
      Eigen::VectorXd x0 = Eigen::VectorXd::Constant(mm, 0.5);
      Eigen::VectorXd x = cg.solveWithGuess(rhs.col(d), x0);
      for (size_t v = 0; v < nv; ++v)
        if (idx[v] >= 0) uv[v][d] = x[idx[v]];
    }
  }
  // (the triangles all turn one way on the square, or the map folded somewhere)
  double pos2 = 0, neg2 = 0;
  for (auto& t : tri) {
    Eigen::Vector2d a = uv[t.y] - uv[t.x], b = uv[t.z] - uv[t.x];
    double ar = a.x() * b.y() - a.y() * b.x();
    (ar >= 0 ? pos2 : neg2) += std::abs(ar);
  }
  if (std::min(pos2, neg2) > 1e-3 * (pos2 + neg2)) return false;
  const bool flip = neg2 > pos2;
  // ---- the grid points: found in the triangles of the square (a grid of cells to look them up)
  const int G = std::clamp((int)std::sqrt(double(tri.size()) / 2), 1, 256);
  std::vector<std::vector<uint32_t>> cell(size_t(G) * G);
  for (uint32_t ti = 0; ti < tri.size(); ++ti) {
    double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
    for (int k = 0; k < 3; ++k) {
      const auto& p = uv[tri[ti][k]];
      x0 = std::min(x0, p.x()), y0 = std::min(y0, p.y()), x1 = std::max(x1, p.x()), y1 = std::max(y1, p.y());
    }
    int cx0 = std::clamp(int(x0 * G), 0, G - 1), cx1 = std::clamp(int(x1 * G), 0, G - 1);
    int cy0 = std::clamp(int(y0 * G), 0, G - 1), cy1 = std::clamp(int(y1 * G), 0, G - 1);
    for (int cy = cy0; cy <= cy1; ++cy)
      for (int cx = cx0; cx <= cx1; ++cx) cell[size_t(cy) * G + cx].push_back(ti);
  }
  auto locate = [&](const Eigen::Vector2d& p) -> vec3 {
    int cx = std::clamp(int(p.x() * G), 0, G - 1), cy = std::clamp(int(p.y() * G), 0, G - 1);
    double bestIn = -1e30;
    vec3 at = pos[tri[0].x];
    for (int r = 0; r < G && bestIn < -1e-9; ++r) {
      for (int dy = -r; dy <= r; ++dy)
        for (int dx = -r; dx <= r; ++dx) {
          if (std::max(std::abs(dx), std::abs(dy)) != r) continue;
          int x = cx + dx, y = cy + dy;
          if (x < 0 || y < 0 || x >= G || y >= G) continue;
          for (uint32_t ti : cell[size_t(y) * G + x]) {
            const auto &a = uv[tri[ti].x], &b = uv[tri[ti].y], &q = uv[tri[ti].z];
            double den = (b.y() - q.y()) * (a.x() - q.x()) + (q.x() - b.x()) * (a.y() - q.y());
            if (std::abs(den) < 1e-300) continue;
            double l0 = ((b.y() - q.y()) * (p.x() - q.x()) + (q.x() - b.x()) * (p.y() - q.y())) / den;
            double l1 = ((q.y() - a.y()) * (p.x() - q.x()) + (a.x() - q.x()) * (p.y() - q.y())) / den;
            double l2 = 1 - l0 - l1, in = std::min({l0, l1, l2});
            if (in > bestIn) {
              bestIn = in;
              double c0 = std::max(l0, 0.0), c1 = std::max(l1, 0.0), c2 = std::max(l2, 0.0), cs = c0 + c1 + c2;
              at = (pos[tri[ti].x] * float(c0) + pos[tri[ti].y] * float(c1) + pos[tri[ti].z] * float(c2)) / float(cs);
            }
          }
        }
      if (r >= 2 && bestIn > -0.5) break;
    }
    return at;
  };
  // ---- the grid's vertices: B round its edge, the rest inside. Inset: the grid a ring in from B, joined
  // to it by a plain row, so the four corners (the poles) sit inside and every vertex of B has four
  // edges (the loop stays clean where it is a line)
  std::vector<uint32_t> id(size_t(n + 1) * (m + 1));
  auto gid = [&](long i, long j) -> uint32_t& { return id[size_t(j) * (n + 1) + i]; };
  auto perim = [&](long u) {  // (the grid point at a place round its edge)
    if (u <= n) return std::pair<long, long>{u, 0};
    if (u <= n + m) return std::pair<long, long>{n, u - n};
    if (u <= 2 * n + m) return std::pair<long, long>{n - (u - n - m), m};
    return std::pair<long, long>{0, m - (u - 2 * n - m)};
  };
  std::vector<uint32_t> V(c);
  for (long k = 0; k < c; ++k) out.pos.push_back(B[k]), V[k] = (uint32_t)out.pos.size() - 1;
  const double dx = inset ? 0.8 / (n + 1.6) : 0.0, dy = inset ? 0.8 / (m + 1.6) : 0.0;
  auto at = [&](long i, long j) { return Eigen::Vector2d(dx + (1 - 2 * dx) * double(i) / n, dy + (1 - 2 * dy) * double(j) / m); };
  if (!inset)
    for (long k = 0; k < c; ++k) {
      auto [i, j] = perim(((k - s0) % c + c) % c);
      gid(i, j) = V[k];
    }
  for (long j = inset ? 0 : 1; j <= (inset ? m : m - 1); ++j)
    for (long i = inset ? 0 : 1; i <= (inset ? n : n - 1); ++i) out.pos.push_back(locate(at(i, j))), gid(i, j) = (uint32_t)out.pos.size() - 1;
  auto addQuad = [&](std::array<uint32_t, 4> q) {
    if (flip) std::reverse(q.begin(), q.end());
    out.quads.push_back(q);
  };
  for (long j = 0; j < m; ++j)
    for (long i = 0; i < n; ++i) addQuad({gid(i, j), gid(i + 1, j), gid(i + 1, j + 1), gid(i, j + 1)});
  if (inset)
    for (long k = 0; k < c; ++k) {
      auto [i0, j0] = perim(((k - s0) % c + c) % c);
      auto [i1, j1] = perim(((k + 1 - s0) % c + c) % c);
      addQuad({V[k], V[(k + 1) % c], gid(i1, j1), gid(i0, j0)});
    }
  out.loopVerts = {V}, out.loopPos = {B};
  out.rings = (int)std::min(n, m);
  out.ok = true;
  return true;
}

}  // namespace

RingFill fillCap(const std::vector<vec3>& pos, const std::vector<glm::uvec3>& tri, float L, const std::vector<std::vector<vec3>>& fixed,
                 const std::vector<char>& open) {
  RingFill out;
  bool simple;
  auto loops = loopsOf(tri, simple);
  if (!simple || loops.size() != 1) {
    out.why = "not a disc";
    return out;
  }
  std::vector<vec3> Q;
  for (uint32_t v : loops[0]) Q.push_back(pos[v]);
  std::vector<vec3> pts = matchFixed(Q, fixed, L);
  if (pts.empty()) {
    // (the outline zigzags along the working triangles: its length from a smoothed copy, and no more
    // than a round disc of this area would have)
    std::vector<vec3> sm = Q;
    for (int it = 0; it < 8; ++it) {
      std::vector<vec3> nx(sm.size());
      for (size_t i = 0; i < sm.size(); ++i) nx[i] = 0.5f * sm[i] + 0.25f * (sm[(i + sm.size() - 1) % sm.size()] + sm[(i + 1) % sm.size()]);
      sm = std::move(nx);
    }
    double area = 0;
    for (auto& t : tri) area += 0.5 * glm::length(glm::cross(pos[t.y] - pos[t.x], pos[t.z] - pos[t.x]));
    double len = std::min(lengthOf(sm), 1.15 * 2.0 * std::sqrt(kPi * area));
    size_t n = std::max<size_t>(4, (size_t)std::lround(len / L));
    n += n % 2;  // (an even count closes with quads only)
    pts = resample(Q, n, Q[0]);
  }
  // a grid where the count is even (else, or if the grid won't lie flat, filled as a hole is)
  // (inset where its loop is a line, not the model's open edge, and there's room for a ring and a grid)
  size_t onOpen = 0;
  for (uint32_t v : loops[0]) onOpen += v < open.size() && open[v];
  const bool inset = onOpen * 2 < loops[0].size() && pts.size() >= 6;
  if (RingFill g; gridFill(pos, tri, loops[0], pts, g, inset)) return g;
  // an odd count: a triangle where the loop turns most, then the grid on the rest
  if (pts.size() >= 7 && pts.size() % 2) {
    const size_t n = pts.size();
    size_t bk = 0;
    float bt = -1;
    for (size_t k = 0; k < n; ++k) {
      vec3 a = pts[k] - pts[(k + n - 1) % n], b = pts[(k + 1) % n] - pts[k];
      float la = glm::length(a), lb = glm::length(b);
      float t = la > 0 && lb > 0 ? std::acos(std::clamp(glm::dot(a, b) / (la * lb), -1.f, 1.f)) : 0.f;
      if (t > bt) bt = t, bk = k;
    }
    std::vector<vec3> rest;
    for (size_t k = 0; k < n; ++k)
      if (k != bk) rest.push_back(pts[k]);
    RingFill g;
    if (gridFill(pos, tri, loops[0], rest, g, inset)) {
      // (the loop as given, with the point cut off back in it; the triangle the way the loop runs)
      const uint32_t cut = (uint32_t)g.pos.size();
      g.pos.push_back(pts[bk]);
      std::vector<uint32_t> V;
      for (size_t k = 0, r = 0; k < n; ++k) V.push_back(k == bk ? cut : g.loopVerts[0][r++]);
      const uint32_t a = V[(bk + n - 1) % n], c = V[(bk + 1) % n];
      // (faces turn as the grid's: the grid's edge a -> c runs the other way in its quads)
      bool aToC = false;
      for (auto& q : g.quads)
        for (int k = 0; k < 4; ++k) aToC |= q[k] == a && q[(k + 1) % 4] == c;
      if (aToC) g.tris.push_back({c, a, cut});
      else g.tris.push_back({a, c, cut});
      g.loopVerts = {V}, g.loopPos = {pts};
      return g;
    }
  }
  std::vector<uint32_t> V;
  for (auto& p : pts) out.pos.push_back(p), V.push_back((uint32_t)out.pos.size() - 1);
  // (the triangles lie on the loop's left, so the faces run along it the same way)
  capLoop(out.pos, out.quads, out.tris, V);
  out.loopVerts = {V}, out.loopPos = {pts};
  out.ok = !out.quads.empty() || !out.tris.empty();
  return out;
}

}  // namespace patina::retopo
