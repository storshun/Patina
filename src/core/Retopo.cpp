#include "core/Retopo.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <map>
#include <numeric>
#include <queue>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

#include "core/Log.h"
#include "core/Parallel.h"

namespace patina::retopo {
namespace {

constexpr int kOutside = -1;  // the region beyond a patch edge that touches no other patch

// ------------------------------------------------------------------ adjacency
struct Csr {
  std::vector<uint32_t> start, items;
  uint32_t size(uint32_t i) const { return start[i + 1] - start[i]; }
  const uint32_t* begin(uint32_t i) const { return items.data() + start[i]; }
};

Csr edgeFaces(const Mesh& m) {
  Csr c;
  c.start.assign(m.edges.size() + 1, 0);
  for (uint32_t l = 0; l < m.loopCount(); ++l) c.start[m.loopEdge[l] + 1]++;
  for (size_t i = 1; i < c.start.size(); ++i) c.start[i] += c.start[i - 1];
  c.items.resize(m.loopCount());
  std::vector<uint32_t> fill(c.start.begin(), c.start.end() - 1);
  for (uint32_t f = 0; f < m.faceCount(); ++f)
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) c.items[fill[m.loopEdge[l]]++] = f;
  return c;
}

std::vector<vec3> vertexNormals(const Mesh& m) {
  std::vector<vec3> n(m.positions.size(), vec3(0));
  for (auto& t : m.tris) {
    vec3 a = m.positions[m.loopVert[t.x]], b = m.positions[m.loopVert[t.y]], c = m.positions[m.loopVert[t.z]];
    vec3 fn = glm::cross(b - a, c - a);
    n[m.loopVert[t.x]] += fn, n[m.loopVert[t.y]] += fn, n[m.loopVert[t.z]] += fn;
  }
  for (auto& v : n) v = glm::length(v) > 1e-20f ? glm::normalize(v) : vec3(0, 1, 0);
  return n;
}

// ------------------------------------------------------------------ patches
struct Side {
  std::vector<std::pair<int, bool>> curves;  // (curve, forward)
};

struct Patch {
  int label = 0;
  std::vector<uint32_t> faces;
  std::vector<std::vector<uint32_t>> loops;  // boundary vertex cycles, the patch on the left
  std::vector<std::vector<int>> other;       // per loop edge (v[i] -> v[i+1]): the region beyond
  std::vector<std::vector<uint32_t>> edges;  // per loop edge: the high-poly edge
  bool ring = false;
  std::string problem;
  std::vector<int> corners;  // disk: positions in loops[0], in loop order
  std::vector<Side> sides;   // disk: corner i to corner i+1; ring: the two loops
  int rows = 0;              // ring: quads from one loop to the other
  double area = 0;
};

struct Curve {
  std::vector<uint32_t> verts;  // high-poly vertices; a closed curve repeats its first at the end
  bool closed = false;
  double target = 1;            // quads along it, from its length and the density
  double plain = 1;             // the same without the density
  int count = 0;
  std::vector<vec3> samples;    // count + 1 points (a closed curve: the last is the first)
};

std::vector<double> arcLengths(const Mesh& m, const std::vector<uint32_t>& loop) {
  std::vector<double> s(loop.size() + 1, 0.0);
  for (size_t i = 0; i < loop.size(); ++i)
    s[i + 1] = s[i] + glm::length(m.positions[loop[(i + 1) % loop.size()]] - m.positions[loop[i]]);
  return s;
}

// How sharply a boundary loop turns toward its patch at loop vertex i (radians; a convex corner
// is positive), measured w either side, in the surface's plane.
double turnAt(const Mesh& m, const std::vector<vec3>& vn, const std::vector<uint32_t>& loop, const std::vector<double>& s,
              size_t i, double w) {
  size_t n = loop.size();
  double total = s[n];
  if (total <= 0) return 0;
  auto at = [&](double arc) {
    arc = std::fmod(arc + 4 * total, total);
    size_t k = std::min<size_t>(std::upper_bound(s.begin(), s.end(), arc) - s.begin() - 1, n - 1);
    double seg = s[k + 1] - s[k], f = seg > 0 ? (arc - s[k]) / seg : 0;
    return glm::mix(m.positions[loop[k]], m.positions[loop[(k + 1) % n]], (float)f);
  };
  vec3 p = m.positions[loop[i]], a = at(s[i] - w), b = at(s[i] + w), nn = vn[loop[i]];
  vec3 d1 = p - a, d2 = b - p;
  d1 -= nn * glm::dot(d1, nn), d2 -= nn * glm::dot(d2, nn);
  if (glm::length(d1) < 1e-12f || glm::length(d2) < 1e-12f) return 0.0;
  return (double)std::atan2(glm::dot(nn, glm::cross(d1, d2)), glm::dot(d1, d2));
}

// ------------------------------------------------------------------ curves
// A polyline resampled finely (about `step` apart) and smoothed with a Gaussian of `sigma` arc length
// (the ends stay unless it is closed), with the density along it. The high-poly's edges zigzag along
// a patch boundary (up to 40% longer than the line they follow), so curves are measured and sampled
// smoothed.
void smoothPolyline(const std::vector<vec3>& raw, const std::vector<double>& rawDens, bool closed, double step, double sigma,
                    std::vector<vec3>& out, std::vector<double>& outDens) {
  std::vector<double> arc = {0.0};
  for (size_t k = 0; k + 1 < raw.size(); ++k) arc.push_back(arc.back() + glm::length(raw[k + 1] - raw[k]));
  double len = arc.back();
  int R = std::clamp(int(len / std::max(step, 1e-9)), 8, 4000);
  std::vector<vec3> fine(R + 1);
  outDens.assign(R + 1, 1.0);
  for (int k = 0, j = 0; k <= R; ++k) {
    double target = len * k / R;
    while (j + 2 < (int)raw.size() && arc[j + 1] < target) ++j;
    double seg = arc[j + 1] - arc[j], f = seg > 0 ? std::clamp((target - arc[j]) / seg, 0.0, 1.0) : 0;
    fine[k] = glm::mix(raw[j], raw[j + 1], (float)f);
    outDens[k] = rawDens[j] + (rawDens[j + 1] - rawDens[j]) * f;
  }
  double sg = sigma / std::max(len / R, 1e-12);  // in fine samples
  int win = std::max(1, int(2 * sg));
  out = fine;
  for (int k = 0; k <= R; ++k) {
    if (!closed && (k == 0 || k == R)) continue;
    vec3 acc(0);
    double ws = 0;
    for (int d = -win; d <= win; ++d) {
      int j = k + d;
      if (closed) j = ((j % R) + R) % R;
      else j = std::clamp(j, 0, R);
      double wgt = std::exp(-0.5 * d * d / (sg * sg + 1e-9));
      acc += fine[j] * (float)wgt, ws += wgt;
    }
    out[k] = acc / (float)ws;
  }
  if (closed) out[R] = out[0];
}

// ------------------------------------------------------------------ tidying painted patches
// Painting on a real model leaves specks, gaps and places where a patch touches itself at a single
// vertex. Specks smaller than about two quads join the patch they share most edges with (unpainted
// specks too, so small gaps fill); where a patch touches itself at a vertex, the smaller fan of its
// faces there goes to the patch next to it. Returns how many faces changed.
size_t tidyLabels(const Mesh& m, const Csr& ef, std::vector<int32_t>& label, double minArea) {
  const uint32_t F = m.faceCount();
  std::vector<uint32_t> vs(m.positions.size() + 1, 0), vf(m.loopCount()), vl(m.loopCount());
  for (uint32_t l = 0; l < m.loopCount(); ++l) vs[m.loopVert[l] + 1]++;
  for (size_t i = 1; i < vs.size(); ++i) vs[i] += vs[i - 1];
  {
    std::vector<uint32_t> fill(vs.begin(), vs.end() - 1);
    for (uint32_t f = 0; f < F; ++f)
      for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) vf[fill[m.loopVert[l]]] = f, vl[fill[m.loopVert[l]]++] = l;
  }
  std::vector<float> area(F);
  parallelFor(F, [&](size_t f) { area[f] = m.faceArea3D((uint32_t)f); });
  size_t changed = 0;
  for (int round = 0; round < 6; ++round) {
    size_t before = changed;
    // specks
    std::vector<char> seen(F, 0);
    std::vector<uint32_t> comp, stack;
    std::unordered_map<int32_t, int> nb;
    for (uint32_t f0 = 0; f0 < F; ++f0) {
      if (seen[f0]) continue;
      int32_t L = label[f0];
      comp.clear(), nb.clear();
      stack = {f0};
      seen[f0] = 1;
      double a = 0;
      bool small = true;
      while (!stack.empty()) {
        uint32_t f = stack.back();
        stack.pop_back();
        comp.push_back(f);
        a += area[f];
        for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) {
          uint32_t e = m.loopEdge[l];
          if (ef.size(e) != 2) continue;
          uint32_t g = ef.begin(e)[0] == f ? ef.begin(e)[1] : ef.begin(e)[0];
          if (label[g] != L) {
            nb[label[g]]++;
            continue;
          }
          if (!seen[g]) seen[g] = 1, stack.push_back(g);
        }
        if (a >= minArea) small = false;
      }
      if (!small || nb.empty()) continue;
      int32_t to = L;
      int most = 0;
      for (auto& [k, n] : nb)
        if (n > most || (n == most && k > to)) most = n, to = k;
      if (to == L) continue;
      for (uint32_t f : comp) label[f] = to;
      changed += comp.size();
    }
    // a patch touching itself at a vertex
    for (uint32_t v = 0; v + 1 < vs.size(); ++v) {
      uint32_t n = vs[v + 1] - vs[v];
      if (n < 3) continue;
      const uint32_t* fan = vf.data() + vs[v];
      bool any = false;  // a label at least twice: the quick test
      for (uint32_t i = 0; i < n && !any; ++i)
        for (uint32_t j = i + 1; j < n && !any; ++j) any = label[fan[i]] > 0 && label[fan[i]] == label[fan[j]];
      if (!any) continue;
      // the fan's faces joined across the edges at v when they have the same label
      std::vector<uint32_t> parent(n);
      std::iota(parent.begin(), parent.end(), 0u);
      std::function<uint32_t(uint32_t)> find = [&](uint32_t x) { return parent[x] == x ? x : parent[x] = find(parent[x]); };
      std::vector<std::array<uint32_t, 2>> edgesAt(n);
      for (uint32_t i = 0; i < n; ++i) {
        uint32_t f = fan[i], l = vl[vs[v] + i];
        uint32_t prev = l == m.faceStart[f] ? m.faceStart[f + 1] - 1 : l - 1;
        edgesAt[i] = {m.loopEdge[l], m.loopEdge[prev]};
      }
      for (uint32_t i = 0; i < n; ++i)
        for (uint32_t j = i + 1; j < n; ++j) {
          if (label[fan[i]] != label[fan[j]]) continue;
          for (uint32_t a : edgesAt[i])
            for (uint32_t b : edgesAt[j])
              if (a == b) parent[find(i)] = find(j);
        }
      std::map<int32_t, std::map<uint32_t, int>> parts;  // label -> root -> faces
      for (uint32_t i = 0; i < n; ++i)
        if (label[fan[i]] > 0) parts[label[fan[i]]][find(i)]++;
      for (auto& [L, roots] : parts) {
        if (roots.size() < 2) continue;
        uint32_t keep = 0;
        int most = -1;
        for (auto& [r, c] : roots)
          if (c > most) most = c, keep = r;
        for (uint32_t i = 0; i < n; ++i) {
          if (label[fan[i]] != L || find(i) == keep) continue;
          // the label of a face next to it around v
          int32_t to = 0;
          for (uint32_t j = 0; j < n; ++j) {
            if (label[fan[j]] == L) continue;
            for (uint32_t a : edgesAt[i])
              for (uint32_t b : edgesAt[j])
                if (a == b) to = label[fan[j]];
          }
          label[fan[i]] = to;
          ++changed;
        }
      }
    }
    if (changed == before) break;
  }
  return changed;
}

// ------------------------------------------------------------------ integer counts
// Least squares on the targets with the equalities wanted, rounded a few variables at a time (those
// nearest a whole number first), solving again for the rest each time.
struct Lin {
  std::vector<std::pair<int, double>> terms;
};

void solveDense(std::vector<double>& A, std::vector<double>& b, int n) {  // A n x n, row major; b -> x
  for (int c = 0; c < n; ++c) {
    int best = c;
    for (int r = c + 1; r < n; ++r)
      if (std::abs(A[(size_t)r * n + c]) > std::abs(A[(size_t)best * n + c])) best = r;
    if (best != c) {
      for (int k = 0; k < n; ++k) std::swap(A[(size_t)c * n + k], A[(size_t)best * n + k]);
      std::swap(b[c], b[best]);
    }
    double p = A[(size_t)c * n + c];
    if (std::abs(p) < 1e-10) {  // a dependent row: its multiplier is zero
      for (int k = 0; k < n; ++k) A[(size_t)c * n + k] = 0;
      A[(size_t)c * n + c] = 1;
      b[c] = 0;
      continue;
    }
    for (int r = c + 1; r < n; ++r) {
      double f = A[(size_t)r * n + c] / p;
      if (f == 0) continue;
      for (int k = c; k < n; ++k) A[(size_t)r * n + k] -= f * A[(size_t)c * n + k];
      b[r] -= f * b[c];
    }
  }
  for (int c = n - 1; c >= 0; --c) {
    double s = b[c];
    for (int k = c + 1; k < n; ++k) s -= A[(size_t)c * n + k] * b[k];
    b[c] = s / A[(size_t)c * n + c];
  }
}

std::vector<int> solveCounts(const std::vector<double>& target, const std::vector<double>& weight, const std::vector<Lin>& eqs,
                             const std::vector<int>& minVal) {
  int n = (int)target.size(), m = (int)eqs.size();
  std::vector<int> fixedVal(n, 0);
  std::vector<char> isFixed(n, 0);
  auto roundAt = [&](int v, double x) { return std::max(minVal[v], (int)std::lround(x)); };
  std::vector<char> inEq(n, 0);
  for (auto& e : eqs)
    for (auto& [v, a] : e.terms) inEq[v] = 1;
  int freeCount = 0;
  for (int v = 0; v < n; ++v) {
    if (!inEq[v] || m > 4000) isFixed[v] = 1, fixedVal[v] = roundAt(v, target[v]);
    else ++freeCount;
  }
  std::vector<double> x(target);
  while (freeCount > 0) {
    // continuous solution for the free variables: x = t - W^-1 A^T y, (A W^-1 A^T) y = A t
    std::vector<double> rhs(m, 0.0), M((size_t)m * m, 0.0);
    for (int i = 0; i < m; ++i) {
      double s = 0;
      for (auto& [v, a] : eqs[i].terms) s += a * (isFixed[v] ? fixedVal[v] : target[v]);
      rhs[i] = s;
    }
    std::vector<std::vector<std::pair<int, double>>> byVar(n);
    for (int i = 0; i < m; ++i)
      for (auto& [v, a] : eqs[i].terms)
        if (!isFixed[v]) byVar[v].push_back({i, a});
    for (int v = 0; v < n; ++v)
      for (auto& [i, a] : byVar[v])
        for (auto& [j, b] : byVar[v]) M[(size_t)i * m + j] += a * b / weight[v];
    for (int i = 0; i < m; ++i) M[(size_t)i * m + i] += 1e-9;
    solveDense(M, rhs, m);
    std::vector<std::pair<double, int>> near;
    for (int v = 0; v < n; ++v) {
      if (isFixed[v]) continue;
      double s = 0;
      for (auto& [i, a] : byVar[v]) s += a * rhs[i];
      x[v] = target[v] - s / weight[v];
      near.push_back({std::abs(x[v] - roundAt(v, x[v])), v});
    }
    std::sort(near.begin(), near.end());
    size_t take = std::max<size_t>(1, near.size() / 8);
    for (size_t k = 0; k < near.size(); ++k) {
      if (k >= take && near[k].first > 0.05) break;
      int v = near[k].second;
      isFixed[v] = 1, fixedVal[v] = roundAt(v, x[v]);
      --freeCount;
    }
  }
  return fixedVal;
}

// ------------------------------------------------------------------ quad layouts in a patch's plane
// A patch is laid out in a plane (a square, a regular polygon, or a ring between two circles) and
// filled there; the new points are then mapped onto the high-poly surface.
using Ref = int64_t;  // >= 0: a vertex made before (on a patch's boundary); < 0: new vertex -(1 + i)

struct Chain {  // points along a boundary, with their places in the plane
  std::vector<Ref> id;
  std::vector<vec2> p;
  int n() const { return (int)id.size() - 1; }
  Chain sub(int a, int b) const {
    Chain c;
    c.id.assign(id.begin() + a, id.begin() + b + 1);
    c.p.assign(p.begin() + a, p.begin() + b + 1);
    return c;
  }
  Chain reversed() const { return {std::vector<Ref>(id.rbegin(), id.rend()), std::vector<vec2>(p.rbegin(), p.rend())}; }
  vec2 at(double u) const {  // u = i / n is point i
    if (n() < 1) return p.front();
    double x = std::clamp(u, 0.0, 1.0) * n();
    int i = std::min((int)x, n() - 1);
    return glm::mix(p[i], p[i + 1], float(x - i));
  }
};

struct Layout {
  std::vector<vec2> pts;  // new points
  std::vector<std::array<Ref, 4>> quads;
  // the quads a stretch of the plane wants (its length on the surface, times the density, over the
  // edge length); without it, rows change width evenly from one end to the other
  std::function<double(vec2, vec2)> measure;
  Ref add(vec2 p) {
    pts.push_back(p);
    return -(Ref)pts.size();
  }
};

using PlaneAt = std::function<vec2(double v, double u)>;  // v: rows from the bottom (0..h), u: 0..1 across

// A Coons patch over four chains: bottom B and top T run left to right, left L and right R bottom to top.
vec2 coonsAt(const Chain& B, const Chain& R, const Chain& T, const Chain& L, double u, double s) {
  vec2 c = float(1 - s) * B.at(u) + float(s) * T.at(u) + float(1 - u) * L.at(s) + float(u) * R.at(s);
  c -= float((1 - u) * (1 - s)) * B.p.front() + float(u * (1 - s)) * B.p.back() + float((1 - u) * s) * T.p.front() +
       float(u * s) * T.p.back();
  return c;
}

// How many three-to-one steps each row takes to narrow from w0 edges to wh in h rows: spread
// evenly, or as early as the rows allow. Empty when they can't.
std::vector<int> stepsPerRow(int w0, int wh, int h) {
  int D = (w0 - wh) / 2;
  std::vector<int> m(h, 0);
  if (D <= 0) return m;
  for (int j = 0; j < h; ++j) m[j] = (int)std::floor((j + 1) * (double)D / h + 0.5) - (int)std::floor(j * (double)D / h + 0.5);
  int w = w0;
  bool fits = true;
  for (int j = 0; j < h && fits; ++j) fits = 3 * m[j] <= w, w -= 2 * m[j];
  if (fits) return m;
  int rem = D;
  w = w0;
  for (int j = 0; j < h; ++j) m[j] = std::min(rem, w / 3), rem -= m[j], w -= 2 * m[j];
  if (rem > 0) return {};
  return m;
}

// The width (in edges) of each row line from the bottom (w0) to the top (wh), h rows: near what
// each line wants, changing by steps (two edges each) only where that pays, never faster than three
// to one in a row. All widths have the parity of w0. Empty when no widths fit.
std::vector<int> chooseWidths(int w0, int wh, int h, const std::vector<double>& want, bool cyclic) {
  if (h < 1 || (w0 - wh) % 2) return {};
  int lo = cyclic ? 3 : 1;
  if (w0 < lo || wh < lo) return {};
  int hiW = std::max(w0, wh);
  for (double w : want) hiW = std::max(hiW, (int)std::ceil(w * 1.3) + 2);
  hiW = std::min(hiW, 4 * std::max(w0, wh) + 8);
  int par = w0 & 1;
  std::vector<int> vals;
  for (int w = lo; w <= hiW; ++w)
    if ((w & 1) == par) vals.push_back(w);
  int S = (int)vals.size();
  const double inf = 1e30, stepCost = 0.25;
  auto bandOk = [](int a, int b) { return 3 * std::min(a, b) >= std::max(a, b); };
  // without wants: the even spread from w0 to wh (as stepsPerRow), as wants
  std::vector<double> W = want;
  if (W.empty()) {
    W.resize(h + 1);
    for (int j = 0; j <= h; ++j) W[j] = w0 + (wh - w0) * double(j) / h;
  }
  std::vector<double> cost(S, inf), next(S);
  std::vector<std::vector<int>> from(h + 1, std::vector<int>(S, -1));
  for (int k = 0; k < S; ++k)
    if (vals[k] == w0) cost[k] = 0;
  for (int j = 1; j <= h; ++j) {
    std::fill(next.begin(), next.end(), inf);
    for (int k = 0; k < S; ++k) {
      if (cost[k] >= inf) continue;
      for (int k2 = 0; k2 < S; ++k2) {
        if (j == h && vals[k2] != wh) continue;
        if (!bandOk(vals[k], vals[k2])) continue;
        double lc = 0;
        if (j < h) {  // per quad of the line: off by more than an eighth (measured), or at all (the even spread)
          double Wj = std::max(W[j], 1.0), e = std::max(0.0, std::abs(vals[k2] - Wj) / Wj - (want.empty() ? 0.0 : 0.12));
          lc = Wj * e * e;
        }
        double c = cost[k] + lc + stepCost * std::abs(vals[k2] - vals[k]) / 2;
        if (c < next[k2]) next[k2] = c, from[j][k2] = k;
      }
    }
    cost = next;
  }
  int end = -1;
  for (int k = 0; k < S; ++k)
    if (vals[k] == wh && cost[k] < inf) end = k;
  if (end < 0) return {};
  std::vector<int> w(h + 1);
  for (int j = h, k = end; j >= 0; --j) {
    w[j] = vals[k];
    if (j) k = from[j][k];
  }
  return w;
}

// One row of quads between a wide line (w edges) and a narrow one (w2 <= w): (w - w2) / 2 steps of
// three edges to one (two three-valent and two five-valent vertices, the usual way to end edge
// loops), spread along the row (a ring's are staggered from row to row). flip: the wide line is the
// upper one.
void band(Layout& lay, const std::vector<Ref>& wide, const std::vector<Ref>& narrow, double v, bool cyclic, const PlaneAt& at,
          bool flip, int j) {
  int w = (int)wide.size() - 1, w2 = (int)narrow.size() - 1, mj = (w - w2) / 2;
  auto quad = [&](Ref a, Ref b, Ref c, Ref d) {
    if (flip) lay.quads.push_back({a, d, c, b});
    else lay.quads.push_back({a, b, c, d});
  };
  std::vector<int> start;
  double off = cyclic ? std::fmod(j * 0.618, 1.0) : 0.5;
  int prevEnd = 0;
  for (int q = 0; q < mj; ++q) {
    int s = (int)std::lround((q + off) * w / mj - 1.5);
    s = std::clamp(s, prevEnd, w - 3 * (mj - q));
    start.push_back(s);
    prevEnd = s + 3;
  }
  int i = 0, t = 0, q = 0;
  while (i < w) {
    if (q < mj && i == start[q]) {
      double uM = 0.5 * (double(i + 1) / w + double(t) / w2), uN = 0.5 * (double(i + 2) / w + double(t + 1) / w2);
      Ref M = lay.add(at(v, uM)), N = lay.add(at(v, uN));
      quad(wide[i], wide[i + 1], M, narrow[t]);
      quad(wide[i + 1], wide[i + 2], N, M);
      quad(wide[i + 2], wide[i + 3], narrow[t + 1], N);
      quad(M, N, narrow[t + 1], narrow[t]);
      i += 3, t += 1, ++q;
    } else {
      quad(wide[i], wide[i + 1], narrow[t + 1], narrow[t]);
      ++i, ++t;
    }
  }
}

// Rows of quads from a bottom line to a top line, h rows. bottom and top run left to right; left
// and right bottom to top (h + 1 points). A cyclic band (a ring) closes on itself: its lines' last
// points are their first. Each row line is as wide as the patch is there (lay.measure), so loops
// are added where the patch widens or the density rises, and end where it narrows.
bool rows(Layout& lay, const std::vector<Ref>& bottom, const std::vector<Ref>& top, const std::vector<Ref>& left,
          const std::vector<Ref>& right, int h, bool cyclic, const PlaneAt& at) {
  int w0 = (int)bottom.size() - 1, wh = (int)top.size() - 1;
  if (h < 1 || (w0 - wh) % 2) return false;
  std::vector<double> want;
  if (lay.measure && h > 1) {
    want.assign(h + 1, 0.0);
    for (int j = 1; j < h; ++j) {
      const int K = 24;
      vec2 prev = at(j, 0.0);
      for (int k = 1; k <= K; ++k) {
        vec2 p = at(j, double(k) / K);
        want[j] += lay.measure(prev, p);
        prev = p;
      }
    }
  }
  std::vector<int> w = chooseWidths(w0, wh, h, want, cyclic);
  if ((int)w.size() != h + 1) return false;
  std::vector<Ref> cur = bottom;
  for (int j = 0; j < h; ++j) {
    std::vector<Ref> nxt;
    int w2 = w[j + 1];
    if (j + 1 == h) nxt = top;
    else {
      nxt.resize(w2 + 1);
      for (int k = 0; k <= w2; ++k) {
        if (!cyclic && k == 0) nxt[k] = left[j + 1];
        else if (!cyclic && k == w2) nxt[k] = right[j + 1];
        else if (cyclic && k == w2) nxt[k] = nxt[0];
        else nxt[k] = lay.add(at(j + 1, double(k) / w2));
      }
    }
    if (w[j] >= w2) band(lay, cur, nxt, j + 0.5, cyclic, at, false, j);
    else band(lay, nxt, cur, j + 0.5, cyclic, at, true, j);
    cur = std::move(nxt);
  }
  return true;
}

void reverseQuads(Layout& lay, size_t from) {
  for (size_t q = from; q < lay.quads.size(); ++q) std::swap(lay.quads[q][1], lay.quads[q][3]);
}

bool fillQuad(Layout& lay, const Chain& B, const Chain& R, const Chain& T, const Chain& L, int depth = 0);

// Both pairs of opposite sides differ: a chain from the bottom to the top splits the patch into a
// part whose columns change (left) and one whose rows change (right).
bool splitVertical(Layout& lay, const Chain& B, const Chain& R, const Chain& T, const Chain& L, int depth) {
  int n0 = B.n(), n1 = R.n(), n2 = T.n(), n3 = L.n();
  int mn = std::min(n0, n2);
  if (mn < 2) return false;
  int D0 = std::abs(n0 - n2) / 2, D1 = std::abs(n1 - n3) / 2;
  int w0 = std::clamp((int)std::lround(mn * double(D1) / std::max(1, D0 + D1)), 1, mn - 1);
  for (int d = 0; d < mn; ++d)
    for (int sgn : {1, -1}) {
      int w = w0 + sgn * d;
      if (w < 1 || w > mn - 1 || (d == 0 && sgn < 0)) continue;
      Layout trial = lay;
      Chain C;
      C.id.push_back(B.id[w]), C.p.push_back(B.p[w]);
      for (int j = 1; j < n1; ++j) {
        double s = double(j) / n1, u = (1 - s) * w / n0 + s * w / n2;
        vec2 p = coonsAt(B, R, T, L, u, s);
        C.p.push_back(p), C.id.push_back(trial.add(p));
      }
      C.id.push_back(T.id[w]), C.p.push_back(T.p[w]);
      if (fillQuad(trial, B.sub(0, w), C, T.sub(0, w), L, depth + 1) && fillQuad(trial, B.sub(w, n0), R, T.sub(w, n2), C, depth + 1)) {
        lay = std::move(trial);
        return true;
      }
    }
  return false;
}

// A four-sided patch: bottom B (corner 0 to 1), right R (1 to 2), top T (3 to 2), left L (0 to 3).
// Equal opposite counts make a plain grid; different ones end edge loops in steps. The counts of
// opposite sides must differ by an even number.
bool fillQuad(Layout& lay, const Chain& B, const Chain& R, const Chain& T, const Chain& L, int depth) {
  int n0 = B.n(), n1 = R.n(), n2 = T.n(), n3 = L.n();
  if (n0 < 1 || n1 < 1 || n2 < 1 || n3 < 1 || (n0 + n2) % 2 || (n1 + n3) % 2) return false;
  if (n1 == n3) return rows(lay, B.id, T.id, L.id, R.id, n1, false, [&](double v, double u) { return coonsAt(B, R, T, L, u, v / n1); });
  if (n0 == n2) {  // the same, turned a quarter
    size_t q0 = lay.quads.size();
    if (!rows(lay, L.id, R.id, B.id, T.id, n0, false, [&](double v, double u) { return coonsAt(B, R, T, L, v / n0, u); })) return false;
    reverseQuads(lay, q0);
    return true;
  }
  if (depth > 0) return false;
  if (splitVertical(lay, B, R, T, L, depth)) return true;
  Layout trial = lay;  // a chain from the left to the right instead
  size_t q0 = trial.quads.size();
  if (!splitVertical(trial, L, T, R, B, depth)) return false;
  reverseQuads(trial, q0);
  lay = std::move(trial);
  return true;
}

// Whether fillQuad can fill sides of these counts (the same tests, without making anything).
struct FitCache {
  std::unordered_map<uint64_t, bool> seen;
};

bool rowsFit(int a, int b, int h) {
  if ((a - b) % 2 || h < 1 || std::min(a, b) < 1) return false;
  return (int)stepsPerRow(std::max(a, b), std::min(a, b), h).size() == h;
}

bool quadFits(int n0, int n1, int n2, int n3, FitCache* cache = nullptr, int depth = 0) {
  if (std::min({n0, n1, n2, n3}) < 1 || (n0 + n2) % 2 || (n1 + n3) % 2) return false;
  if (n1 == n3) return rowsFit(n0, n2, n1);
  if (n0 == n2) return rowsFit(n1, n3, n0);
  if (depth > 0) return false;
  uint64_t key = (uint64_t(std::min(n0, 65535)) << 48) | (uint64_t(std::min(n1, 65535)) << 32) | (uint64_t(std::min(n2, 65535)) << 16) |
                 uint64_t(std::min(n3, 65535));
  if (cache)
    if (auto it = cache->seen.find(key); it != cache->seen.end()) return it->second;
  auto split = [](int b, int r, int t, int l) {
    for (int w = 1; w <= std::min(b, t) - 1; ++w)
      if (rowsFit(r, l, w) && rowsFit(b - w, t - w, r)) return true;
    return false;
  };
  bool ok = split(n0, n1, n2, n3) || split(n3, n2, n1, n0);
  if (cache) cache->seen[key] = ok;
  return ok;
}

// Three, five or six sides (or four, when the grid can't): a pole in the middle, a spoke from a
// point on each side to it, and a four-sided part at each corner. Each side's point and each
// spoke's count are chosen so the corner parts are as near plain grids as can be.
bool fillPole(Layout& lay, const std::vector<Chain>& side, vec2 centerP) {
  int K = (int)side.size();
  if (K < 3 || K > 8) return false;
  std::vector<int> Ls(K);
  for (int k = 0; k < K; ++k) {
    Ls[k] = side[k].n();
    if (Ls[k] < 2) return false;
  }
  auto md = [&](int k) { return ((k % K) + K) % K; };
  // spokes, continuous: s[k-1] + s[k+1] = L[k], with a little pull toward a quarter of the mean side
  std::vector<double> s0(K);
  {
    double mean = 0;
    for (int L : Ls) mean += L;
    mean /= K;
    std::vector<double> A((size_t)K * K, 0.0), b(K, 0.0);
    for (int k = 0; k < K; ++k) {
      int i = md(k - 1), j = md(k + 1);
      A[(size_t)i * K + i] += 1, A[(size_t)i * K + j] += 1, A[(size_t)j * K + i] += 1, A[(size_t)j * K + j] += 1;
      b[i] += Ls[k], b[j] += Ls[k];
    }
    for (int k = 0; k < K; ++k) A[(size_t)k * K + k] += 0.05, b[k] += 0.05 * mean / 2;
    solveDense(A, b, K);
    s0 = b;
  }
  struct Cand {
    double cost;
    std::vector<int> s, a;
  };
  std::vector<Cand> cands;
  FitCache cache;
  // corner part k: bottom a[k], right s[k], top s[k-1], left the rest of side k-1
  auto partCost = [&](int k, const std::vector<int>& s, const std::vector<int>& a) {
    int km = md(k - 1), n0 = a[k], n1 = s[k], n2 = s[km], n3 = Ls[km] - a[km];
    double e1 = n0 - n2, e2 = n1 - n3;
    return e1 * e1 + e2 * e2 + (quadFits(n0, n1, n2, n3, &cache) ? 0.0 : 1e4);
  };
  auto totalCost = [&](const std::vector<int>& s, const std::vector<int>& a) {
    double c = 0;
    for (int k = 0; k < K; ++k) c += partCost(k, s, a);
    return c;
  };
  // the points on the sides for given spokes: nearest the ideal, then each the best of its values
  auto splitFor = [&](const std::vector<int>& s, std::vector<int>& a) {
    a.assign(K, 0);
    for (int k = 0; k < K; ++k) {
      int par = s[md(k - 1)] & 1;
      double want = 0.5 * (Ls[k] + s[md(k - 1)] - s[md(k + 1)]);
      int best = -1;
      double bd = 1e30;
      for (int v = 1; v <= Ls[k] - 1; ++v)
        if ((v & 1) == par && std::abs(v - want) < bd) bd = std::abs(v - want), best = v;
      if (best < 0) return 1e30;
      a[k] = best;
    }
    double cost = totalCost(s, a);
    for (int pass = 0; pass < 3 && cost > 0; ++pass) {
      bool better = false;
      for (int k = 0; k < K; ++k) {
        int kp = md(k + 1);
        double here = partCost(k, s, a) + partCost(kp, s, a);
        int keep = a[k];
        for (int v = 1 + ((s[md(k - 1)] & 1) ? 0 : 1); v <= Ls[k] - 1; v += 2) {
          if (v == keep) continue;
          a[k] = v;
          double c2 = partCost(k, s, a) + partCost(kp, s, a);
          if (c2 < here - 1e-9) here = c2, keep = v, better = true;
        }
        a[k] = keep;
      }
      cost = totalCost(s, a);
      if (!better) break;
    }
    return cost;
  };
  for (int mask = 0; mask < (1 << K); ++mask) {
    std::vector<int> par(K);
    bool ok = true;
    for (int k = 0; k < K; ++k) par[k] = (mask >> k) & 1;
    for (int k = 0; k < K && ok; ++k) ok = ((par[md(k - 1)] + par[md(k + 1)]) & 1) == (Ls[k] & 1);
    if (!ok) continue;
    std::vector<int> s(K), a;
    for (int k = 0; k < K; ++k) {
      int v = (int)std::lround(s0[k]);
      if ((v & 1) != par[k]) v += (s0[k] > v) ? 1 : -1;
      while (v < 1) v += 2;
      s[k] = v;
    }
    double cost = splitFor(s, a);
    for (int round = 0; round < 20 && cost > 0; ++round) {  // nudge the spokes while it helps
      bool better = false;
      for (int k = 0; k < K; ++k)
        for (int d : {-2, 2}) {
          if (s[k] + d < 1) continue;
          s[k] += d;
          std::vector<int> a2;
          double c2 = splitFor(s, a2);
          if (c2 < cost - 1e-9) cost = c2, a = a2, better = true;
          else s[k] -= d;
        }
      if (!better) break;
    }
    if (cost < 1e29) cands.push_back({cost, s, a});
  }
  std::sort(cands.begin(), cands.end(), [](const Cand& x, const Cand& y) { return x.cost < y.cost; });
  for (auto& cd : cands) {
    Layout trial = lay;
    Ref center = trial.add(centerP);
    std::vector<Chain> spoke(K);
    for (int k = 0; k < K; ++k) {
      vec2 m = side[k].p[cd.a[k]];
      spoke[k].id.push_back(side[k].id[cd.a[k]]), spoke[k].p.push_back(m);
      for (int j = 1; j < cd.s[k]; ++j) {
        vec2 p = glm::mix(m, centerP, float(j) / cd.s[k]);
        spoke[k].p.push_back(p), spoke[k].id.push_back(trial.add(p));
      }
      spoke[k].id.push_back(center), spoke[k].p.push_back(centerP);
    }
    bool ok = true;
    for (int k = 0; k < K && ok; ++k) {
      int km = md(k - 1);
      ok = fillQuad(trial, side[k].sub(0, cd.a[k]), spoke[k], spoke[km], side[km].sub(cd.a[km], Ls[km]).reversed());
    }
    if (ok) {
      lay = std::move(trial);
      return true;
    }
  }
  return false;
}

// A ring: rows from loop A (the outer circle, counterclockwise) to loop B (the inner one), both given
// counterclockwise with their angles (0 to 2 pi); at least h rows, more when the counts differ a lot.
bool fillRing(Layout& lay, const std::vector<Ref>& A, const std::vector<double>& angA, const std::vector<Ref>& B,
              const std::vector<double>& angB, int h) {
  int nA = (int)A.size() - 1, nB = (int)B.size() - 1;
  if (nA < 3 || nB < 3 || (nA + nB) % 2) return false;
  auto ang = [](const std::vector<double>& a, double u) {
    int n = (int)a.size() - 1;
    double x = std::clamp(u, 0.0, 1.0) * n;
    int i = std::min((int)x, n - 1);
    return a[i] + (a[i + 1] - a[i]) * (x - i);
  };
  h = std::max(h, 1);
  while ((int)stepsPerRow(std::max(nA, nB), std::min(nA, nB), h).size() != h && h < 1000) ++h;
  auto at = [&](double v, double u) {
    double s = v / h, t = (1 - s) * ang(angA, u) + s * ang(angB, u), r = 2 - s;
    return vec2(float(r * std::cos(t)), float(r * std::sin(t)));
  };
  // a tube, not a balloon: far wider between its ends than at them (a ball with two small holes)
  // is no ring to fill row by row
  if (lay.measure) {
    double mid = 0;
    const int K = 48;
    vec2 prev = at(h * 0.5, 0.0);
    for (int k = 1; k <= K; ++k) {
      vec2 p = at(h * 0.5, double(k) / K);
      mid += lay.measure(prev, p);
      prev = p;
    }
    if (mid > 3.0 * std::max(nA, nB) + 6) return false;
  }
  return rows(lay, A, B, {}, {}, h, true, at);
}

// A polygon (or square) of sides: a grid or steps for four sides, a pole otherwise.
bool fillPolygon(Layout& lay, const std::vector<Chain>& side, vec2 center) {
  if (side.size() == 4) {
    Layout trial = lay;
    if (fillQuad(trial, side[0], side[1], side[2].reversed(), side[3].reversed())) {
      lay = std::move(trial);
      return true;
    }
  }
  return fillPole(lay, side, center);
}

// The corners of a patch in the plane: on a circle, each side taking an arc in proportion to its
// edge count, so long sides get room and short ones little (a convex polygon whatever the counts).
// Without counts: a square for four sides, a regular polygon otherwise.
std::vector<vec2> polygonCorners(int K, const std::vector<int>& counts = {}) {
  if ((int)counts.size() != K) {
    if (K == 4) return {vec2(0, 0), vec2(1, 0), vec2(1, 1), vec2(0, 1)};
    std::vector<vec2> poly;
    for (int k = 0; k < K; ++k) {
      float a = -kPi / 2 - kPi / K + 2 * kPi * k / K;
      poly.push_back(vec2(std::cos(a), std::sin(a)));
    }
    return poly;
  }
  double total = 0;
  for (int c : counts) total += std::max(1, c);
  std::vector<vec2> poly;
  double a = -kPi / 2 - kPi * std::max(1, counts[0]) / total;
  for (int k = 0; k < K; ++k) {
    poly.push_back(vec2((float)std::cos(a), (float)std::sin(a)));
    a += 2 * kPi * std::max(1, counts[k]) / total;
  }
  return poly;
}

// ------------------------------------------------------------------ flattening a patch
struct Flat {
  std::vector<vec2> uv;                    // per local vertex
  std::vector<uint32_t> global;            // local -> high-poly vertex
  std::vector<glm::uvec3> tris;            // local triangles
  // point location
  vec2 lo{0}, hi{0};
  int gw = 1, gh = 1;
  std::vector<std::vector<uint32_t>> cells;
};

// Tutte embedding: boundary vertices fixed, every interior vertex at a weighted average of its
// neighbors (positive weights and a convex boundary keep every triangle the right way round).
// Solved on a ladder of coarser graphs (neighbors paired up level by level), each level's answer
// correcting the next finer one, then a few conjugate-gradient steps there: a patch of 200,000
// faces takes a fraction of a second.
struct LGraph {
  std::vector<uint32_t> start, adj;
  std::vector<double> w;
  std::vector<char> fixed;
  size_t n() const { return fixed.size(); }
};

void cgSolve(const LGraph& g, std::vector<vec2>& x, int maxIt, double tol) {
  size_t n = g.n();
  std::vector<uint32_t> idx(n, ~0u), free;
  for (uint32_t v = 0; v < n; ++v)
    if (!g.fixed[v]) idx[v] = (uint32_t)free.size(), free.push_back(v);
  if (free.empty()) return;
  size_t k = free.size();
  std::vector<double> diag(k, 0.0);
  for (size_t i = 0; i < k; ++i)
    for (uint32_t e = g.start[free[i]]; e < g.start[free[i] + 1]; ++e) diag[i] += g.w[e];
  for (int axis = 0; axis < 2; ++axis) {
    std::vector<double> X(k), b(k, 0.0), r(k), z(k), p(k), q(k);
    for (size_t i = 0; i < k; ++i) {
      uint32_t v = free[i];
      for (uint32_t e = g.start[v]; e < g.start[v + 1]; ++e)
        if (g.fixed[g.adj[e]]) b[i] += g.w[e] * x[g.adj[e]][axis];
      X[i] = x[v][axis];
    }
    auto mul = [&](const std::vector<double>& in, std::vector<double>& out) {
      for (size_t i = 0; i < k; ++i) {
        uint32_t v = free[i];
        double sum = diag[i] * in[i];
        for (uint32_t e = g.start[v]; e < g.start[v + 1]; ++e)
          if (!g.fixed[g.adj[e]]) sum -= g.w[e] * in[idx[g.adj[e]]];
        out[i] = sum;
      }
    };
    mul(X, q);
    double bnorm = 0;
    for (size_t i = 0; i < k; ++i) r[i] = b[i] - q[i], bnorm += b[i] * b[i];
    bnorm = std::sqrt(std::max(bnorm, 1e-30));
    for (size_t i = 0; i < k; ++i) z[i] = r[i] / diag[i], p[i] = z[i];
    double rz = 0;
    for (size_t i = 0; i < k; ++i) rz += r[i] * z[i];
    for (int it = 0; it < maxIt; ++it) {
      mul(p, q);
      double pq = 0;
      for (size_t i = 0; i < k; ++i) pq += p[i] * q[i];
      if (std::abs(pq) < 1e-300) break;
      double al = rz / pq, rn = 0;
      for (size_t i = 0; i < k; ++i) X[i] += al * p[i], r[i] -= al * q[i], rn += r[i] * r[i];
      if (std::sqrt(rn) < tol * bnorm) break;
      double rz2 = 0;
      for (size_t i = 0; i < k; ++i) z[i] = r[i] / diag[i], rz2 += r[i] * z[i];
      double beta = rz2 / rz;
      rz = rz2;
      for (size_t i = 0; i < k; ++i) p[i] = z[i] + beta * p[i];
    }
    for (size_t i = 0; i < k; ++i) x[free[i]][axis] = (float)X[i];
  }
}

// Pairs each vertex with its most strongly joined unpaired neighbor (free with free, fixed with
// fixed); the pairs are the next level's vertices.
LGraph coarsen(const LGraph& g, std::vector<uint32_t>& parent) {
  size_t n = g.n();
  parent.assign(n, ~0u);
  uint32_t next = 0;
  // a fixed shuffle: neighbors visited in order would pair along stripes
  std::vector<uint32_t> order(n);
  std::iota(order.begin(), order.end(), 0u);
  for (size_t i = n; i > 1; --i) std::swap(order[i - 1], order[(i * 2654435761u) % i]);
  for (uint32_t v : order) {
    if (parent[v] != ~0u) continue;
    uint32_t best = ~0u;
    double bw = -1;
    for (uint32_t e = g.start[v]; e < g.start[v + 1]; ++e) {
      uint32_t u = g.adj[e];
      if (parent[u] == ~0u && u != v && g.fixed[u] == g.fixed[v] && g.w[e] > bw) bw = g.w[e], best = u;
    }
    parent[v] = next;
    if (best != ~0u) parent[best] = next;
    ++next;
  }
  LGraph c;
  c.fixed.assign(next, 0);
  for (uint32_t v = 0; v < n; ++v) c.fixed[parent[v]] = g.fixed[v];
  std::vector<std::tuple<uint32_t, uint32_t, double>> el;
  el.reserve(g.adj.size());
  for (uint32_t v = 0; v < n; ++v)
    for (uint32_t e = g.start[v]; e < g.start[v + 1]; ++e)
      if (parent[v] != parent[g.adj[e]]) el.emplace_back(parent[v], parent[g.adj[e]], g.w[e]);
  std::sort(el.begin(), el.end());
  c.start.assign(next + 1, 0);
  for (size_t i = 0; i < el.size();) {
    auto [a, b, w] = el[i];
    size_t j = i + 1;
    while (j < el.size() && std::get<0>(el[j]) == a && std::get<1>(el[j]) == b) w += std::get<2>(el[j]), ++j;
    c.adj.push_back(b), c.w.push_back(w), c.start[a + 1]++;
    i = j;
  }
  for (size_t i = 1; i < c.start.size(); ++i) c.start[i] += c.start[i - 1];
  return c;
}

void solveTutte(Flat& fl, const std::vector<char>& fixedV, const std::vector<std::vector<uint32_t>>& nbr,
                const std::vector<std::vector<float>>& wt) {
  size_t n = fl.uv.size();
  std::vector<LGraph> lv(1);
  LGraph& g0 = lv[0];
  g0.fixed = fixedV;
  g0.start.assign(n + 1, 0);
  for (size_t v = 0; v < n; ++v) {
    g0.start[v + 1] = g0.start[v] + (uint32_t)nbr[v].size();
    for (size_t j = 0; j < nbr[v].size(); ++j) g0.adj.push_back(nbr[v][j]), g0.w.push_back(wt[v][j]);
  }
  std::vector<std::vector<uint32_t>> parents;
  while (lv.back().n() > 3000 && lv.size() < 24) {
    std::vector<uint32_t> par;
    LGraph c = coarsen(lv.back(), par);
    if (c.n() > lv.back().n() * 0.85) break;
    parents.push_back(std::move(par));
    lv.push_back(std::move(c));
  }
  // the current answer, averaged down the levels
  std::vector<std::vector<vec2>> xs(lv.size());
  xs[0] = fl.uv;
  for (size_t l = 1; l < lv.size(); ++l) {
    std::vector<vec2> sum(lv[l].n(), vec2(0));
    std::vector<int> cnt(lv[l].n(), 0);
    for (size_t v = 0; v < lv[l - 1].n(); ++v) sum[parents[l - 1][v]] += xs[l - 1][v], cnt[parents[l - 1][v]]++;
    xs[l].resize(lv[l].n());
    for (size_t v = 0; v < lv[l].n(); ++v) xs[l][v] = sum[v] / float(std::max(1, cnt[v]));
  }
  std::vector<std::vector<vec2>> before = xs;
  size_t L = lv.size() - 1;
  cgSolve(lv[L], xs[L], 4000, 1e-7);
  for (size_t l = L; l-- > 0;) {
    for (size_t v = 0; v < lv[l].n(); ++v)
      if (!lv[l].fixed[v]) xs[l][v] += xs[l + 1][parents[l][v]] - before[l + 1][parents[l][v]];
    cgSolve(lv[l], xs[l], 60, 1e-7);
  }
  fl.uv = xs[0];
}

// A flattening whose areas follow the surface's: a plain Tutte embedding shrinks the middle of a
// bulge (a cheek, a bust) to a fraction of its size, which would make the quads there large. Where
// the plane is too small for the surface, the springs are softened, and the plane solved again.
void flattenEvenly(Flat& fl, const Mesh& high, const std::vector<char>& fixedV, const std::vector<std::vector<uint32_t>>& nbr,
                   int rounds) {
  size_t n = fl.uv.size();
  std::vector<std::vector<float>> wt(n);
  for (size_t v = 0; v < n; ++v) wt[v].assign(nbr[v].size(), 1.f);
  solveTutte(fl, fixedV, nbr, wt);
  std::vector<double> a3(n, 0.0);
  double total3 = 0;
  for (auto& t : fl.tris) {
    vec3 A = high.positions[fl.global[t.x]], B = high.positions[fl.global[t.y]], C = high.positions[fl.global[t.z]];
    double a = 0.5 * glm::length(glm::cross(B - A, C - A));
    a3[t.x] += a, a3[t.y] += a, a3[t.z] += a, total3 += 3 * a;
  }
  std::vector<float> scale(n, 1.f);
  for (int round = 0; round < rounds; ++round) {
    std::vector<double> a2(n, 0.0);
    double total2 = 0;
    for (auto& t : fl.tris) {
      vec2 A = fl.uv[t.x], B = fl.uv[t.y], C = fl.uv[t.z];
      double a = 0.5 * std::abs(double(B.x - A.x) * (C.y - A.y) - double(C.x - A.x) * (B.y - A.y));
      a2[t.x] += a, a2[t.y] += a, a2[t.z] += a, total2 += 3 * a;
    }
    if (total2 <= 0 || total3 <= 0) return;
    for (size_t v = 0; v < n; ++v) {
      double r = (a3[v] / total3) / std::max(a2[v] / total2, 1e-30);  // > 1: the plane is too small here
      scale[v] = (float)std::clamp(std::pow(std::clamp(r, 0.01, 100.0), -0.35), 0.05, 20.0);
    }
    for (size_t v = 0; v < n; ++v)
      for (size_t j = 0; j < nbr[v].size(); ++j) wt[v][j] *= std::sqrt(scale[v] * scale[nbr[v][j]]);
    solveTutte(fl, fixedV, nbr, wt);
  }
}

void buildLocator(Flat& fl) {
  fl.lo = vec2(1e30f), fl.hi = vec2(-1e30f);
  for (auto& p : fl.uv) fl.lo = glm::min(fl.lo, p), fl.hi = glm::max(fl.hi, p);
  int g = std::clamp((int)std::sqrt((double)fl.tris.size() / 2.0), 1, 1024);
  fl.gw = fl.gh = g;
  fl.cells.assign((size_t)g * g, {});
  vec2 ext = glm::max(fl.hi - fl.lo, vec2(1e-12f));
  for (uint32_t t = 0; t < fl.tris.size(); ++t) {
    vec2 a = fl.uv[fl.tris[t].x], b = fl.uv[fl.tris[t].y], c = fl.uv[fl.tris[t].z];
    vec2 mn = glm::min(a, glm::min(b, c)), mx = glm::max(a, glm::max(b, c));
    int x0 = std::clamp(int((mn.x - fl.lo.x) / ext.x * g), 0, g - 1), x1 = std::clamp(int((mx.x - fl.lo.x) / ext.x * g), 0, g - 1);
    int y0 = std::clamp(int((mn.y - fl.lo.y) / ext.y * g), 0, g - 1), y1 = std::clamp(int((mx.y - fl.lo.y) / ext.y * g), 0, g - 1);
    for (int y = y0; y <= y1; ++y)
      for (int x = x0; x <= x1; ++x) fl.cells[(size_t)y * g + x].push_back(t);
  }
}

// barycentric weights of p in the triangle (may be negative outside)
vec3 bary(vec2 a, vec2 b, vec2 c, vec2 p) {
  double d = double(b.x - a.x) * (c.y - a.y) - double(c.x - a.x) * (b.y - a.y);
  if (std::abs(d) < 1e-30) return vec3(-1);
  double w1 = (double(p.x - a.x) * (c.y - a.y) - double(c.x - a.x) * (p.y - a.y)) / d;
  double w2 = (double(b.x - a.x) * (p.y - a.y) - double(p.x - a.x) * (b.y - a.y)) / d;
  return vec3(float(1 - w1 - w2), float(w1), float(w2));
}

// The point of the high-poly surface at a flattened position (and the density there).
vec3 locate(const Flat& fl, const Mesh& high, vec2 p, const std::function<float(uint32_t)>* dens = nullptr, float* densOut = nullptr) {
  vec2 ext = glm::max(fl.hi - fl.lo, vec2(1e-12f));
  int cx = std::clamp(int((p.x - fl.lo.x) / ext.x * fl.gw), 0, fl.gw - 1), cy = std::clamp(int((p.y - fl.lo.y) / ext.y * fl.gh), 0, fl.gh - 1);
  float bestOut = 1e30f;
  vec3 bestW(1, 0, 0);
  uint32_t bestT = ~0u;
  for (int ring = 0; ring <= std::max(fl.gw, fl.gh); ++ring) {
    for (int y = cy - ring; y <= cy + ring; ++y)
      for (int x = cx - ring; x <= cx + ring; ++x) {
        if (x < 0 || y < 0 || x >= fl.gw || y >= fl.gh) continue;
        if (ring && std::max(std::abs(x - cx), std::abs(y - cy)) != ring) continue;
        for (uint32_t t : fl.cells[(size_t)y * fl.gw + x]) {
          vec3 w = bary(fl.uv[fl.tris[t].x], fl.uv[fl.tris[t].y], fl.uv[fl.tris[t].z], p);
          float out = std::max(0.f, -std::min(w.x, std::min(w.y, w.z)));
          if (out < bestOut) bestOut = out, bestW = w, bestT = t;
          if (out == 0.f) goto found;
        }
      }
    if (bestOut < 1e-3f || (ring >= 2 && bestT != ~0u)) break;
  }
found:
  if (bestT == ~0u) return vec3(0);
  vec3 w = glm::max(bestW, vec3(0));
  w /= std::max(w.x + w.y + w.z, 1e-12f);
  const glm::uvec3& t = fl.tris[bestT];
  if (dens && densOut) *densOut = w.x * (*dens)(fl.global[t.x]) + w.y * (*dens)(fl.global[t.y]) + w.z * (*dens)(fl.global[t.z]);
  return w.x * high.positions[fl.global[t.x]] + w.y * high.positions[fl.global[t.y]] + w.z * high.positions[fl.global[t.z]];
}

PlaneLayout toPlane(const Layout& lay, const std::vector<vec2>& boundary, bool ok) {
  PlaneLayout out;
  out.points = boundary;
  for (auto& p : lay.pts) out.points.push_back(p);
  uint32_t nb = (uint32_t)boundary.size();
  for (auto& q : lay.quads) {
    std::array<uint32_t, 4> o;
    for (int k = 0; k < 4; ++k) o[k] = q[k] >= 0 ? (uint32_t)q[k] : nb + (uint32_t)(-q[k] - 1);
    out.quads.push_back(o);
  }
  if (!ok) out.problem = "no layout fits these counts";
  return out;
}

}  // namespace

void FaceGraph::build(const Mesh& m) {
  Csr ef = edgeFaces(m);
  faces = m.faceCount();
  start.assign(faces + 1, 0);
  for (uint32_t f = 0; f < faces; ++f)
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) start[f + 1] += ef.size(m.loopEdge[l]) - 1;
  for (uint32_t f = 0; f < faces; ++f) start[f + 1] += start[f];
  items.resize(start[faces]);
  std::vector<uint32_t> fill(start.begin(), start.end() - 1);
  for (uint32_t f = 0; f < faces; ++f)
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) {
      uint32_t e = m.loopEdge[l];
      for (uint32_t k = 0; k < ef.size(e); ++k)
        if (ef.begin(e)[k] != f) items[fill[f]++] = ef.begin(e)[k];
    }
}

std::vector<uint32_t> facesWithin(const Mesh& m, const FaceGraph& g, uint32_t face, vec3 center, float radius) {
  std::vector<uint32_t> out;
  if (face >= g.faces) return out;
  std::unordered_set<uint32_t> seen = {face};
  std::vector<uint32_t> stack = {face};
  float r2 = radius * radius;
  while (!stack.empty()) {
    uint32_t f = stack.back();
    stack.pop_back();
    out.push_back(f);
    for (uint32_t i = g.start[f]; i < g.start[f + 1]; ++i) {
      uint32_t h = g.items[i];
      if (seen.count(h)) continue;
      seen.insert(h);
      // a face is in when any corner is (so thin brushes still paint long triangles)
      bool in = false;
      for (uint32_t l = m.faceStart[h]; l < m.faceStart[h + 1] && !in; ++l) {
        vec3 d = m.positions[m.loopVert[l]] - center;
        in = glm::dot(d, d) <= r2;
      }
      if (in) stack.push_back(h);
    }
  }
  return out;
}

std::vector<int32_t> autoPatches(const Mesh& m, const std::vector<uint32_t>& faces, int count, int* cellsOut) {
  uint32_t F = m.faceCount();
  std::vector<int32_t> label(F, 0);
  if (cellsOut) *cellsOut = 0;
  if (faces.empty() || count < 1 || m.edges.empty()) return label;
  std::vector<char> in(F, 0);
  for (uint32_t f : faces) in[f] = 1;
  std::vector<vec3> c(F);
  for (uint32_t f : faces) c[f] = m.faceCenter(f);
  Csr ef = edgeFaces(m);
  auto across = [&](uint32_t f, uint32_t e) -> int64_t {  // the face over a manifold edge, if given
    if (ef.size(e) != 2) return -1;
    uint32_t g = ef.begin(e)[0] == f ? ef.begin(e)[1] : ef.begin(e)[0];
    return in[g] ? (int64_t)g : -1;
  };
  // seeds: farthest points
  std::vector<uint32_t> seeds = {faces[0]};
  {
    std::vector<float> d2(F, 1e30f);
    for (int k = 1; k < count; ++k) {
      uint32_t far = faces[0];
      float best = -1;
      vec3 s = c[seeds.back()];
      for (uint32_t f : faces) {
        d2[f] = std::min(d2[f], glm::dot(c[f] - s, c[f] - s));
        if (d2[f] > best) best = d2[f], far = f;
      }
      seeds.push_back(far);
    }
  }
  std::vector<float> dist(F);
  std::vector<int32_t> cell(F);
  auto grow = [&] {
    std::fill(dist.begin(), dist.end(), 1e30f);
    std::fill(cell.begin(), cell.end(), -1);
    using Item = std::pair<float, uint32_t>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> pq;
    for (size_t k = 0; k < seeds.size(); ++k) dist[seeds[k]] = 0, cell[seeds[k]] = (int32_t)k, pq.push({0.f, seeds[k]});
    while (!pq.empty()) {
      auto [d, f] = pq.top();
      pq.pop();
      if (d > dist[f]) continue;
      for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) {
        int64_t g = across(f, m.loopEdge[l]);
        if (g < 0) continue;
        float nd = d + glm::length(c[g] - c[f]);
        if (nd < dist[g]) dist[g] = nd, cell[g] = cell[f], pq.push({nd, (uint32_t)g});
      }
    }
  };
  for (int iter = 0; iter < 3; ++iter) {
    grow();
    // each seed to the face nearest its cell's center
    std::vector<vec3> sum(seeds.size(), vec3(0));
    std::vector<int> n(seeds.size(), 0);
    for (uint32_t f : faces)
      if (cell[f] >= 0) sum[cell[f]] += c[f], n[cell[f]]++;
    std::vector<float> bd(seeds.size(), 1e30f);
    for (uint32_t f : faces) {
      int k = cell[f];
      if (k < 0 || !n[k]) continue;
      vec3 mid = sum[k] / float(n[k]);
      float d = glm::dot(c[f] - mid, c[f] - mid);
      if (d < bd[k]) bd[k] = d, seeds[k] = f;
    }
  }
  grow();
  // vertex -> faces, for counting each cell's vertices
  std::vector<uint32_t> vs(m.positions.size() + 1, 0), vf(m.loopCount());
  for (uint32_t l = 0; l < m.loopCount(); ++l) vs[m.loopVert[l] + 1]++;
  for (size_t i = 1; i < vs.size(); ++i) vs[i] += vs[i - 1];
  {
    std::vector<uint32_t> fill(vs.begin(), vs.end() - 1);
    for (uint32_t f = 0; f < F; ++f)
      for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) vf[fill[m.loopVert[l]]++] = f;
  }
  for (int round = 0; round < 16; ++round) {
    size_t before = seeds.size();
    // pieces no seed reached: a seed each
    for (uint32_t f0 : faces) {
      if (cell[f0] >= 0) continue;
      int32_t k = (int32_t)seeds.size();
      seeds.push_back(f0);
      std::vector<uint32_t> stack = {f0};
      cell[f0] = k;
      while (!stack.empty()) {
        uint32_t f = stack.back();
        stack.pop_back();
        for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l)
          if (int64_t g = across(f, m.loopEdge[l]); g >= 0 && cell[g] < 0) cell[g] = k, stack.push_back((uint32_t)g);
      }
    }
    // each cell's shape: chi = V - E + F and its boundary loops
    size_t K = seeds.size();
    std::vector<long> V(K, 0), E(K, 0), Fc(K, 0);
    for (uint32_t f : faces) Fc[cell[f]]++;
    std::vector<int32_t> tmp;
    for (uint32_t v = 0; v + 1 < vs.size(); ++v) {
      tmp.clear();
      for (uint32_t i = vs[v]; i < vs[v + 1]; ++i)
        if (in[vf[i]]) tmp.push_back(cell[vf[i]]);
      std::sort(tmp.begin(), tmp.end());
      tmp.erase(std::unique(tmp.begin(), tmp.end()), tmp.end());
      for (int32_t k : tmp) V[k]++;
    }
    std::unordered_map<uint64_t, uint64_t> parent;  // (cell, vertex) on the cell's boundary
    auto key = [](int32_t k, uint32_t v) { return (uint64_t(uint32_t(k)) << 32) | v; };
    std::function<uint64_t(uint64_t)> find = [&](uint64_t x) {
      while (parent[x] != x) x = parent[x] = parent[parent[x]];
      return x;
    };
    for (uint32_t e = 0; e < m.edges.size(); ++e) {
      tmp.clear();
      for (uint32_t i = 0; i < ef.size(e); ++i)
        if (in[ef.begin(e)[i]]) tmp.push_back(cell[ef.begin(e)[i]]);
      std::sort(tmp.begin(), tmp.end());
      for (size_t i = 0; i < tmp.size(); ++i) {
        if (i && tmp[i] == tmp[i - 1]) continue;
        int32_t k = tmp[i];
        E[k]++;
        bool inner = i + 1 < tmp.size() && tmp[i + 1] == k && ef.size(e) == 2;
        if (inner) continue;
        uint64_t a = key(k, m.edges[e].x), b = key(k, m.edges[e].y);
        if (!parent.count(a)) parent[a] = a;
        if (!parent.count(b)) parent[b] = b;
        parent[find(a)] = find(b);
      }
    }
    std::vector<int> loops(K, 0);
    for (auto& [x, p] : parent)
      if (find(x) == x) loops[x >> 32]++;
    std::vector<float> far(K, -1);
    std::vector<uint32_t> farFace(K, 0);
    for (uint32_t f : faces)
      if (dist[f] < 1e29f && dist[f] > far[cell[f]]) far[cell[f]] = dist[f], farFace[cell[f]] = f;
    for (size_t k = 0; k < before && k < K; ++k) {
      long chi = V[k] - E[k] + Fc[k];
      bool disk = loops[k] == 1 && chi == 1, ring = loops[k] == 2 && chi == 0;
      if (!disk && !ring && Fc[k] > 1 && far[k] > 0) seeds.push_back(farFace[k]);
    }
    if (seeds.size() == before) break;
    grow();
  }
  for (uint32_t f : faces) label[f] = cell[f] >= 0 ? cell[f] + 1 : 0;
  if (cellsOut) *cellsOut = (int)seeds.size();
  return label;
}

PlaneLayout layoutPolygon(const std::vector<int>& sideCounts) {
  int K = (int)sideCounts.size();
  std::vector<vec2> poly = polygonCorners(K), boundary;
  std::vector<Chain> side(K);
  int total = 0;
  for (int c : sideCounts) total += c;
  for (int k = 0; k < K; ++k) {
    int base = (int)boundary.size();
    for (int i = 0; i < sideCounts[k]; ++i) boundary.push_back(glm::mix(poly[k], poly[(k + 1) % K], float(i) / sideCounts[k]));
    for (int i = 0; i <= sideCounts[k]; ++i) {
      side[k].id.push_back((base + i) % std::max(1, total));
      side[k].p.push_back(glm::mix(poly[k], poly[(k + 1) % K], float(i) / sideCounts[k]));
    }
  }
  vec2 center(0);
  for (auto& p : poly) center += p / float(K);
  Layout lay;
  bool ok = K >= 3 && fillPolygon(lay, side, center);
  return toPlane(lay, boundary, ok);
}

PlaneLayout layoutRing(int outer, int inner, int rowsWanted) {
  std::vector<vec2> boundary;
  std::vector<Ref> A, B;
  std::vector<double> angA, angB;
  for (int i = 0; i <= outer; ++i) {
    double t = 2 * kPi * i / outer;
    if (i < outer) boundary.push_back(vec2(2 * std::cos(t), 2 * std::sin(t)));
    A.push_back(i % outer), angA.push_back(t);
  }
  for (int i = 0; i <= inner; ++i) {
    double t = 2 * kPi * i / inner;
    if (i < inner) boundary.push_back(vec2(std::cos(t), std::sin(t)));
    B.push_back(outer + i % inner), angB.push_back(t);
  }
  Layout lay;
  bool ok = fillRing(lay, A, angA, B, angB, rowsWanted);
  return toPlane(lay, boundary, ok);
}

Result build(const Mesh& high, const Bvh& bvh, const std::vector<int32_t>& faceLabel, const std::vector<float>& vertexDensity,
             const Options& opt) {
  auto t0 = std::chrono::steady_clock::now();
  Result res;
  const uint32_t F = high.faceCount();
  const float L0 = 1.f / std::sqrt(std::max(opt.quadsPerM2, 1e-3f));
  res.edgeLength = L0;
  if (faceLabel.size() != F || high.edges.empty()) {
    res.problems.push_back("no patches: paint some first");
    return res;
  }
  std::function<float(uint32_t)> dens = [&](uint32_t v) {
    return v < vertexDensity.size() ? std::clamp(vertexDensity[v], 0.05f, 50.f) : 1.f;
  };
  Csr ef = edgeFaces(high);
  std::vector<vec3> vnormal = vertexNormals(high);
  auto mark = [&, last = t0](const char* what) mutable {
    if (!std::getenv("PATINA_RETOPO_DEBUG")) return;
    auto now = std::chrono::steady_clock::now();
    PLOG_I("retopo %s: %.2f s", what, std::chrono::duration<double>(now - last).count());
    last = now;
  };
  res.labels = faceLabel;
  res.tidied = tidyLabels(high, ef, res.labels, 2.0 * L0 * L0);
  mark("tidy");
  const std::vector<int32_t>& label = res.labels;

  // ---- patches: connected faces with the same label
  std::vector<int> patchOf(F, -1);
  std::vector<Patch> patches;
  for (uint32_t f0 = 0; f0 < F; ++f0) {
    if (label[f0] <= 0 || patchOf[f0] >= 0) continue;
    Patch P;
    P.label = label[f0];
    int pid = (int)patches.size();
    std::vector<uint32_t> stack = {f0};
    patchOf[f0] = pid;
    while (!stack.empty()) {
      uint32_t f = stack.back();
      stack.pop_back();
      P.faces.push_back(f);
      for (uint32_t l = high.faceStart[f]; l < high.faceStart[f + 1]; ++l) {
        uint32_t e = high.loopEdge[l];
        if (ef.size(e) != 2) continue;
        uint32_t g = ef.begin(e)[0] == f ? ef.begin(e)[1] : ef.begin(e)[0];
        if (patchOf[g] < 0 && label[g] == P.label) patchOf[g] = pid, stack.push_back(g);
      }
    }
    patches.push_back(std::move(P));
  }
  if (patches.empty()) {
    res.problems.push_back("no patches: paint some first");
    return res;
  }

  // ---- boundary loops of each patch
  parallelFor(patches.size(), [&](size_t pid) {
    Patch& P = patches[pid];
    struct DirEdge {
      uint32_t a, b, e;
      int other;
    };
    std::vector<DirEdge> de;
    std::unordered_map<uint32_t, std::vector<uint32_t>> out;
    std::unordered_set<uint32_t> verts, edgesUsed;
    for (uint32_t f : P.faces) {
      P.area += high.faceArea3D(f);
      for (uint32_t l = high.faceStart[f]; l < high.faceStart[f + 1]; ++l) {
        uint32_t e = high.loopEdge[l];
        verts.insert(high.loopVert[l]);
        edgesUsed.insert(e);
        int inside = 0, other = kOutside;
        for (uint32_t k = 0; k < ef.size(e); ++k) {
          uint32_t g = ef.begin(e)[k];
          if (patchOf[g] == (int)pid) ++inside;
          else other = patchOf[g];
        }
        if (inside != 1) continue;
        if (ef.size(e) != 2) other = kOutside;
        uint32_t a = high.loopVert[l], b = high.loopVert[high.nextLoop(l, f)];
        out[a].push_back((uint32_t)de.size());
        de.push_back({a, b, e, other});
      }
    }
    long chi = (long)verts.size() - (long)edgesUsed.size() + (long)P.faces.size();
    std::vector<char> used(de.size(), 0);
    bool pinch = false;
    for (uint32_t s = 0; s < de.size(); ++s) {
      if (used[s]) continue;
      std::vector<uint32_t> loop, ed;
      std::vector<int> oth;
      uint32_t cur = s;
      while (!used[cur]) {
        used[cur] = 1;
        loop.push_back(de[cur].a);
        ed.push_back(de[cur].e);
        oth.push_back(de[cur].other);
        auto& nx = out[de[cur].b];
        uint32_t next = ~0u;
        for (uint32_t c : nx)
          if (!used[c]) {
            next = c;
            break;
          }
        if (nx.size() > 1) pinch = true;
        if (next == ~0u) break;
        cur = next;
      }
      P.loops.push_back(loop);
      P.edges.push_back(ed);
      P.other.push_back(oth);
    }
    if (pinch) P.problem = "it touches itself at a point";
    else if (P.loops.size() == 1 && chi == 1) P.ring = false;
    else if (P.loops.size() == 2 && chi == 0) P.ring = true;
    else if (P.loops.empty()) P.problem = "it has no boundary (a closed surface): split it into at least two patches";
    else if (chi == 1 - (long)(P.loops.size() - 1)) P.problem = "it has a hole in it: paint over the hole, or split the patch across it";
    else P.problem = "it has " + std::to_string(P.loops.size()) + " boundary loops (a disk or a ring is needed): split it";
  });

  mark("patches and loops");
  // ---- corners. Where a patch's neighbor changes is a junction. Where three patches meet, the
  // flattest of them runs straight through and the other two have corners: a four-valent vertex,
  // as in hand-made topology. Where four or more meet, or the open edge, all have corners.
  std::unordered_set<uint32_t> breaks;  // curve ends
  struct JEntry {
    int pid, pos;
    double turn;
    bool ring;
  };
  std::unordered_map<uint32_t, std::vector<JEntry>> junction;
  std::unordered_set<uint32_t> openJunction;
  std::vector<std::vector<double>> arc0(patches.size());  // disks: arc lengths along loops[0]
  const double turnW = 1.5 * L0;
  for (size_t pid = 0; pid < patches.size(); ++pid) {
    Patch& P = patches[pid];
    if (!P.problem.empty()) continue;
    for (size_t li = 0; li < P.loops.size(); ++li) {
      auto& oth = P.other[li];
      auto& loop = P.loops[li];
      size_t n = loop.size();
      std::vector<double> s = arcLengths(high, loop);
      if (!P.ring) arc0[pid] = s;
      for (size_t i = 0; i < n; ++i) {
        int a = oth[(i + n - 1) % n], b = oth[i];
        if (a == b) continue;
        breaks.insert(loop[i]);
        if (a == kOutside || b == kOutside) openJunction.insert(loop[i]);
        double t = P.ring ? 0.0 : turnAt(high, vnormal, loop, s, i, std::min(turnW, s[n] / 8.0));
        junction[loop[i]].push_back({(int)pid, (int)i, t, P.ring});
      }
    }
  }
  // Each patch's corners, chosen from its junctions (and its sharpest bends) all at once: sharp
  // corners; sides that aren't tiny; for four sides, opposite sides of about the same length; for
  // three, five or six, a pole that fits (no side longer than the rest allow). Four sides are
  // preferred. A patch runs straight through the junctions it doesn't take, best where it is
  // nearly straight and no other patch there does the same (a T, or one patch of a Y: the vertex
  // then has four edges).
  std::vector<int> order;
  for (size_t pid = 0; pid < patches.size(); ++pid)
    if (patches[pid].problem.empty() && !patches[pid].ring) order.push_back((int)pid);
  auto junctionCount = [&](int pid) {
    auto& oth = patches[pid].other[0];
    int c = 0;
    for (size_t i = 0; i < oth.size(); ++i) c += oth[i] != oth[(i + oth.size() - 1) % oth.size()];
    return c;
  };
  std::sort(order.begin(), order.end(), [&](int x, int y) { return junctionCount(x) > junctionCount(y); });
  std::unordered_map<uint32_t, int> straightThrough;  // junction vertex -> patches running straight there
  for (int pid : order) {
    Patch& P = patches[pid];
    auto& loop = P.loops[0];
    auto& oth = P.other[0];
    size_t n = loop.size();
    const std::vector<double>& s = arc0[pid];
    double total = s[n];
    double w = std::min(turnW, total / 8.0);
    auto turn = [&](size_t i) { return turnAt(high, vnormal, loop, s, i, w); };
    // the arc length weighted by the density, for side lengths in edges
    std::vector<double> ws(n + 1, 0.0);
    for (size_t i = 0; i < n; ++i)
      ws[i + 1] = ws[i] + (s[i + 1] - s[i]) * 0.5 * (std::sqrt(dens(loop[i])) + std::sqrt(dens(loop[(i + 1) % n]))) / L0;
    struct Cand {
      int pos;
      double turn;
      bool junction, open;
      int others;  // patches already running straight there
      size_t meet; // patches meeting there
    };
    std::vector<Cand> cand;
    for (size_t i = 0; i < n; ++i)
      if (oth[i] != oth[(i + n - 1) % n]) {
        auto it = junction.find(loop[i]);
        cand.push_back({(int)i, turn(i), true, openJunction.count(loop[i]) > 0, straightThrough[loop[i]],
                        it == junction.end() ? 0 : it->second.size()});
      }
    std::vector<int> cs;
    if (cand.size() >= 3) {
      // bends sharp enough to be corners, away from the junctions
      std::vector<std::pair<double, int>> bends;
      for (size_t i = 0; i < n; ++i) {
        double t = turn(i);
        if (t < 0.9) continue;
        bool far = true;
        for (auto& c : cand) {
          double d = std::abs(s[i] - s[c.pos]);
          far &= std::min(d, total - d) > 2 * L0;
        }
        if (far) bends.push_back({t, (int)i});
      }
      std::sort(bends.rbegin(), bends.rend());
      for (auto& [t, i] : bends) {
        bool far = true;
        for (auto& c : cand) {
          double d = std::abs(s[i] - s[c.pos]);
          far &= std::min(d, total - d) > 2 * L0;
        }
        if (far && cand.size() < 12) cand.push_back({i, t, false, false, 0, 0});
      }
      std::sort(cand.begin(), cand.end(), [](const Cand& x, const Cand& y) { return x.pos < y.pos; });
      int m = (int)cand.size();
      double best = 1e30;
      int bestMask = (1 << std::min(m, 4)) - 1;
      auto sq = [](double x) { return x * x; };
      for (int mask = 1; mask < (1 << m); ++mask) {
        int K = __builtin_popcount(mask);
        if (K < 3 || K > opt.maxSides) continue;
        double score = K == 4 ? 0 : K == 3 ? 0.8 : K == 5 ? 0.6 : 1.2;
        std::vector<double> len;
        int first = -1, prev = -1;
        for (int j = 0; j < m; ++j) {
          const Cand& c = cand[j];
          if (mask >> j & 1) {
            score += 3 * sq(std::max(0.0, 0.7 - c.turn)) + (c.junction ? 0.0 : 0.6);
            if (prev >= 0) len.push_back(ws[c.pos] - ws[cand[prev].pos]);
            else first = j;
            prev = j;
          } else if (c.junction) {
            // a sharp junction left out costs, unless a chosen corner is right next to it (two
            // junctions a step apart are one corner)
            bool near = false;
            for (int k = 0; k < m && !near; ++k) {
              if (!(mask >> k & 1)) continue;
              double dd = std::abs(ws[cand[k].pos] - ws[c.pos]);
              near = std::min(dd, ws[n] - dd) < 1.5;
            }
            score += c.open ? 100.0 : (near ? 0.0 : 3 * sq(std::max(0.0, c.turn - 0.5))) + 2.0 * c.others + (c.meet >= 4 ? 1.0 : 0.0);
          }
          if (score >= best) break;
        }
        if (score >= best) continue;
        len.push_back(ws[n] - ws[cand[prev].pos] + ws[cand[first].pos]);
        double P = 0;
        for (double l : len) score += 4 * sq(std::max(0.0, 2.0 - l)), P += l;
        auto poleFit = [&](double sk) { return 10 * sq(std::max(0.0, 0.2 - sk / std::max(P / 2, 1e-9))); };
        if (K == 4) score += sq(std::log(std::max(len[0], 0.3) / std::max(len[2], 0.3))) + sq(std::log(std::max(len[1], 0.3) / std::max(len[3], 0.3)));
        else if (K == 3)
          for (int k = 0; k < 3; ++k) score += poleFit(P / 2 - len[k]);
        else if (K == 5)
          for (int k = 0; k < 5; ++k) score += poleFit(P / 2 - len[(k + 2) % 5] - len[(k + 3) % 5]);
        else if (K == 6)
          for (int g = 0; g < 2; ++g) {
            double a = len[g], b = len[g + 2], c = len[g + 4], half = 0.5 * (a + b + c);
            for (double x : {a, b, c}) score += poleFit((half - x) * P / std::max(a + b + c, 1e-9));
          }
        if (score < best) best = score, bestMask = mask;
      }
      for (int j = 0; j < m; ++j) {
        if (bestMask >> j & 1) cs.push_back(cand[j].pos);
        else if (cand[j].junction) straightThrough[loop[cand[j].pos]]++;
      }
    } else {
      for (auto& c : cand) cs.push_back(c.pos);
    }
    if (cs.size() < 3) {  // still too few (a cap, or a patch with two neighbors): four corners
      while (cs.size() < 4) {
        if (cs.empty()) {
          size_t best = 0;
          double bt = -1e9;
          for (size_t i = 0; i < n; ++i)
            if (double t = turn(i); t > bt) bt = t, best = i;
          cs.push_back((int)best);
          continue;
        }
        // the longest stretch between corners gets one, at its sharpest turn (or its middle)
        std::sort(cs.begin(), cs.end());
        size_t k = 0;
        double longest = -1;
        for (size_t j = 0; j < cs.size(); ++j) {
          double a = s[cs[j]], b = j + 1 < cs.size() ? s[cs[j + 1]] : s[cs[0]] + total;
          if (b - a > longest) longest = b - a, k = j;
        }
        double a = s[cs[k]], b = a + longest;
        int best = -1;
        double bt = 0.35;  // ~20 degrees at least, or the middle
        double midArc = std::fmod(a + longest / 2, total);
        int mid = 0;
        double md = 1e30;
        for (size_t i = 0; i < n; ++i) {
          double si = s[i] < a ? s[i] + total : s[i];
          if (si <= a || si >= b) continue;
          double f = (si - a) / longest;
          if (std::abs(s[i] - midArc) < md) md = std::abs(s[i] - midArc), mid = (int)i;
          if (f < 0.25 || f > 0.75) continue;
          if (double t = turn(i); t > bt) bt = t, best = (int)i;
        }
        int add = best >= 0 ? best : mid;
        if (std::find(cs.begin(), cs.end(), add) != cs.end()) break;
        cs.push_back(add);
      }
    }
    while ((int)cs.size() > opt.maxSides) {  // the flattest go
      size_t worst = 0;
      double wt = 1e9;
      for (size_t j = 0; j < cs.size(); ++j)
        if (double t = turn(cs[j]); t < wt) wt = t, worst = j;
      cs.erase(cs.begin() + worst);
    }
    std::sort(cs.begin(), cs.end());
    if (cs.size() < 3) {
      P.problem = "it is too small for the density";
      continue;
    }
    P.corners = cs;
    for (int c : cs) breaks.insert(loop[c]);
  }

  // ---- curves: boundary runs between breaks, shared by the patches on both sides
  std::vector<Curve> curves;
  std::unordered_map<uint32_t, int> curveOfEdge;  // high-poly edge -> curve
  // per patch loop: the curves in loop order (curve, forward), and where each starts in the loop
  std::vector<std::vector<std::vector<std::pair<int, bool>>>> loopCurves(patches.size());
  std::vector<std::vector<std::vector<int>>> loopCurveStart(patches.size());
  for (size_t pid = 0; pid < patches.size(); ++pid) {
    Patch& P = patches[pid];
    loopCurves[pid].resize(P.loops.size());
    loopCurveStart[pid].resize(P.loops.size());
    for (size_t li = 0; li < P.loops.size(); ++li) {
      auto& loop = P.loops[li];
      size_t n = loop.size();
      int s0 = -1;
      for (size_t i = 0; i < n; ++i)
        if (breaks.count(loop[i])) {
          s0 = (int)i;
          break;
        }
      if (s0 < 0) {  // no break: the loop is one closed curve, starting at its smallest vertex
        size_t mi = std::min_element(loop.begin(), loop.end()) - loop.begin();
        auto it = curveOfEdge.find(P.edges[li][mi]);
        if (it != curveOfEdge.end()) {
          Curve& c = curves[it->second];
          bool fwd = c.verts.size() > 1 && c.verts[1] == loop[(mi + 1) % n];
          loopCurves[pid][li].push_back({it->second, fwd});
        } else {
          Curve c;
          c.closed = true;
          for (size_t k = 0; k <= n; ++k) c.verts.push_back(loop[(mi + k) % n]);
          int id = (int)curves.size();
          for (size_t k = 0; k < n; ++k) curveOfEdge[P.edges[li][(mi + k) % n]] = id;
          curves.push_back(std::move(c));
          loopCurves[pid][li].push_back({id, true});
        }
        loopCurveStart[pid][li].push_back((int)mi);
        continue;
      }
      size_t i = 0;
      while (i < n) {
        size_t a = (s0 + i) % n;
        size_t len = 1;
        while (i + len < n && !breaks.count(loop[(s0 + i + len) % n])) ++len;
        auto it = curveOfEdge.find(P.edges[li][a]);
        if (it != curveOfEdge.end()) {
          Curve& c = curves[it->second];
          loopCurves[pid][li].push_back({it->second, c.verts.front() == loop[a]});
        } else {
          Curve c;
          for (size_t k = 0; k <= len; ++k) c.verts.push_back(loop[(a + k) % n]);
          int id = (int)curves.size();
          for (size_t k = 0; k < len; ++k) curveOfEdge[P.edges[li][(a + k) % n]] = id;
          curves.push_back(std::move(c));
          loopCurves[pid][li].push_back({id, true});
        }
        loopCurveStart[pid][li].push_back((int)a);
        i += len;
      }
    }
  }
  parallelFor(curves.size(), [&](size_t ci) {  // the length the quads will follow: smoothed over half a quad
    Curve& c = curves[ci];
    std::vector<vec3> raw, sm;
    std::vector<double> rawDens, smDens;
    for (uint32_t v : c.verts) raw.push_back(high.positions[v]), rawDens.push_back(std::sqrt(dens(v)));
    smoothPolyline(raw, rawDens, c.closed, 0.1 * L0, 0.5 * L0, sm, smDens);
    double t = 0, len = 0;
    for (size_t k = 0; k + 1 < sm.size(); ++k) {
      double l = glm::length(sm[k + 1] - sm[k]);
      t += l * 0.5 * (smDens[k] + smDens[k + 1]), len += l;
    }
    c.target = std::max(1.0, t / L0);
    c.plain = std::max(1.0, len / L0);
  });

  // ---- sides of each patch
  for (size_t pid = 0; pid < patches.size(); ++pid) {
    Patch& P = patches[pid];
    if (!P.problem.empty()) continue;
    if (P.ring) {
      for (int li = 0; li < 2; ++li) P.sides.push_back({loopCurves[pid][li]});
      continue;
    }
    // corner k's side: the curves from corner k to corner k+1 (curves start at breaks, corners are breaks)
    auto& lc = loopCurves[pid][0];
    auto& st = loopCurveStart[pid][0];
    size_t n = P.loops[0].size();
    for (size_t k = 0; k < P.corners.size(); ++k) {
      Side sd;
      int a = P.corners[k], b = P.corners[(k + 1) % P.corners.size()];
      size_t span = (b - a + n) % n;
      if (span == 0) span = n;
      std::vector<std::pair<size_t, std::pair<int, bool>>> in;
      for (size_t j = 0; j < lc.size(); ++j)
        if (size_t off = (st[j] - a + n) % n; off < span) in.push_back({off, lc[j]});
      std::sort(in.begin(), in.end(), [](auto& x, auto& y) { return x.first < y.first; });
      for (auto& [off, c] : in) sd.curves.push_back(c);
      P.sides.push_back(sd);
    }
  }

  // ---- edge counts. Opposite sides of four-sided patches and the two ends of rings are asked to be
  // equal, so edge loops run straight through, unless the painted density asks for a change there
  // (or the shape is lopsided, more than 1.6 to 1); then the loops end in steps. Each patch also
  // needs its counts to add up evenly (opposite sides differing by an even number, a pole's sides
  // adding up to an even number); where they don't, an edge loop is added along the shortest strip
  // of patches that puts it right, as one would by hand.
  std::vector<double> target, weight;
  std::vector<int> minVal(curves.size(), 1);
  for (size_t c = 0; c < curves.size(); ++c) {
    target.push_back(curves[c].target), weight.push_back(1.0 / std::max(1.0, curves[c].target));
    if (curves[c].closed) minVal[c] = 3;
  }
  for (auto& P : patches)  // a pole's corner parts need two edges on each side
    if (P.problem.empty() && !P.ring && P.sides.size() != 4)
      for (auto& sd : P.sides)
        if (sd.curves.size() == 1) minVal[sd.curves[0].first] = std::max(minVal[sd.curves[0].first], 2);
  auto sideTarget = [&](const Side& sd, bool plain = false) {
    double t = 0;
    for (auto& [c, f] : sd.curves) t += plain ? curves[c].plain : curves[c].target;
    return t;
  };
  struct Pref {
    Lin eq;
    double ratio;
    bool on = true;
  };
  std::vector<Pref> prefs;
  auto prefer = [&](const Side& a, const Side& b) {
    Pref p;
    for (auto& [c, f] : a.curves) p.eq.terms.push_back({c, 1.0});
    for (auto& [c, f] : b.curves) p.eq.terms.push_back({c, -1.0});
    double ta = sideTarget(a), tb = sideTarget(b), pa = sideTarget(a, true), pb = sideTarget(b, true);
    p.ratio = std::max(ta, tb) / std::max(1e-9, std::min(ta, tb));
    double shift = (ta / std::max(tb, 1e-9)) / (pa / std::max(pb, 1e-9));
    bool painted = std::max(shift, 1 / std::max(shift, 1e-9)) > 1.2;
    p.on = painted ? p.ratio <= 1.3 : p.ratio <= 1.6;
    prefs.push_back(p);
  };
  for (auto& P : patches) {
    if (!P.problem.empty()) continue;
    if (P.ring) prefer(P.sides[0], P.sides[1]);
    else if (P.sides.size() == 4) prefer(P.sides[0], P.sides[2]), prefer(P.sides[1], P.sides[3]);
  }
  std::vector<int> cnt;
  for (int round = 0; round < 16; ++round) {
    std::vector<Lin> eqs;
    for (auto& p : prefs)
      if (p.on) eqs.push_back(p.eq);
    cnt = solveCounts(target, weight, eqs, minVal);
    // a curve far from what its length and density ask for: the least equal pair still held lets go
    bool far = false;
    for (size_t c = 0; c < curves.size() && !far; ++c) far = std::abs(cnt[c] - curves[c].target) > std::max(2.5, 0.5 * curves[c].target);
    if (!far) break;
    int worst = -1;
    for (size_t k = 0; k < prefs.size(); ++k)
      if (prefs[k].on && prefs[k].ratio > 1.05 && (worst < 0 || prefs[k].ratio > prefs[worst].ratio)) worst = (int)k;
    if (worst < 0) break;
    prefs[worst].on = false;
  }
  for (size_t c = 0; c < curves.size(); ++c) cnt[c] = std::max(minVal[c], cnt[c]);
  {
    // parity: a node per sum that must be even (a four-sided patch has two: sides 0 + 2 and 1 + 3;
    // a ring one: both ends; three and five sides one: all; six sides two: alternate sides). Each
    // curve toggles one node of each patch beside it, so the odd nodes pair up (T-join): a loop
    // added along the path between two of them fixes both.
    std::vector<std::vector<int>> curveNodes(curves.size());
    int nodes = 0;
    for (auto& P : patches) {
      if (!P.problem.empty()) continue;
      int K = (int)P.sides.size(), groups = P.ring ? 1 : K == 4 || K == 6 ? 2 : 1;
      for (int g = 0; g < groups; ++g) {
        int node = nodes++;
        std::map<int, int> mult;
        for (int k = 0; k < K; ++k)
          if (P.ring || groups == 1 || k % 2 == g)
            for (auto& [c, f] : P.sides[k].curves) mult[c]++;
        for (auto& [c, m] : mult)
          if (m % 2) curveNodes[c].push_back(node);
      }
    }
    const int kFree = nodes;  // the far side of a curve on the mesh's edge or by a patch left out
    std::vector<std::vector<std::pair<int, int>>> adj(nodes + 1);  // node -> (other node, curve)
    for (size_t c = 0; c < curves.size(); ++c) {
      auto& cn = curveNodes[c];
      if (cn.empty()) continue;
      int a = cn[0], b = cn.size() > 1 ? cn[1] : kFree;
      adj[a].push_back({b, (int)c}), adj[b].push_back({a, (int)c});
    }
    std::vector<char> odd(nodes + 1, 0);
    for (size_t c = 0; c < curves.size(); ++c)
      if (cnt[c] & 1)
        for (int n : curveNodes[c]) odd[n] ^= 1;
    int fixes = 0;
    for (int start = 0; start < nodes; ++start) {
      if (!odd[start]) continue;
      // the nearest other odd node (or the free one), by the fewest curves
      std::vector<int> from(nodes + 1, -2), via(nodes + 1, -1);
      std::vector<int> queue = {start};
      from[start] = -1;
      int end = -1;
      for (size_t qi = 0; qi < queue.size() && end < 0; ++qi) {
        int u = queue[qi];
        for (auto [v, c] : adj[u]) {
          if (from[v] != -2) continue;
          from[v] = u, via[v] = c;
          if ((v != start && odd[v]) || v == kFree) {
            end = v;
            break;
          }
          queue.push_back(v);
        }
      }
      if (end < 0) break;  // can't happen: odd nodes come in pairs in each piece
      for (int v = end; v != start; v = from[v]) cnt[via[v]] += 1, ++fixes;
      odd[start] = 0;
      if (end != kFree) odd[end] = 0;
    }
    res.parityFixes = fixes;
  }
  for (size_t c = 0; c < curves.size(); ++c) curves[c].count = std::max(1, cnt[c]);

  // ---- rings: how many quads from one end to the other (the patch's area over its mean girth)
  for (auto& P : patches) {
    if (!P.problem.empty() || !P.ring) continue;
    // (the smoothed lengths: the high-poly's zigzag edges would make a ring look fatter than it is)
    double lenA = sideTarget(P.sides[0], true) * L0, lenB = sideTarget(P.sides[1], true) * L0;
    double tA = sideTarget(P.sides[0]), tB = sideTarget(P.sides[1]);
    double girth = 0.5 * (lenA + lenB), leff = (lenA + lenB) / std::max(1e-9, tA + tB);
    P.rows = std::max(1, (int)std::lround(P.area / std::max(girth, 1e-12) / std::max(leff, 1e-12)));
  }

  // ---- samples along each curve: smoothed, on the surface, spaced by the density
  parallelFor(curves.size(), [&](size_t ci) {
    Curve& c = curves[ci];
    std::vector<vec3> raw, sm;
    std::vector<double> rawDens, smDens;
    for (uint32_t v : c.verts) raw.push_back(high.positions[v]), rawDens.push_back(std::sqrt(dens(v)));
    double len = 0;
    for (size_t k = 0; k + 1 < raw.size(); ++k) len += glm::length(raw[k + 1] - raw[k]);
    smoothPolyline(raw, rawDens, c.closed, 0.1 * L0, std::max(0.5 * L0, 0.3 * len / std::max(1, c.count)), sm, smDens);
    int R = (int)sm.size() - 1;
    for (int k = 0; k <= R; ++k) {  // back onto the surface
      if (!c.closed && (k == 0 || k == R)) continue;
      RayHit h = bvh.closestPoint(high, sm[k], L0 * 2);
      if (h.hit) sm[k] = h.position;
    }
    if (c.closed) sm[R] = sm[0];
    // the samples: even steps of length times density
    std::vector<double> W(R + 1, 0.0);
    for (int k = 1; k <= R; ++k) W[k] = W[k - 1] + glm::length(sm[k] - sm[k - 1]) * 0.5 * (smDens[k] + smDens[k - 1]);
    c.samples.assign(c.count + 1, vec3(0));
    for (int k = 0, j = 0; k <= c.count; ++k) {
      double target = W[R] * k / c.count;
      while (j + 1 < R && W[j + 1] < target) ++j;
      double seg = W[j + 1] - W[j], f = seg > 0 ? std::clamp((target - W[j]) / seg, 0.0, 1.0) : 0;
      c.samples[k] = glm::mix(sm[j], sm[j + 1], (float)f);
    }
    c.samples.front() = high.positions[c.verts.front()];
    c.samples.back() = high.positions[c.verts.back()];
  });

  // ---- vertices of the low-poly
  Mesh& low = res.low;
  low.name = "Retopo";
  std::unordered_map<uint32_t, uint32_t> cornerVert;          // high-poly vertex -> low vertex
  std::map<std::pair<int, int>, uint32_t> sampleVert;          // (curve, k) -> low vertex
  std::vector<char> pinned;                                    // low vertex lies on a curve
  auto addVert = [&](vec3 p, bool pin) {
    low.positions.push_back(p);
    pinned.push_back(pin ? 1 : 0);
    return (uint32_t)low.positions.size() - 1;
  };
  auto curvePoint = [&](int c, int k) -> uint32_t {  // the low vertex of sample k of curve c
    Curve& cv = curves[c];
    int n = cv.count;
    if (cv.closed) k = ((k % n) + n) % n;
    if (k == 0 || k == n) {
      uint32_t hv = k == 0 ? cv.verts.front() : cv.verts.back();
      if (!cv.closed || k == 0) {
        auto it = cornerVert.find(hv);
        if (it != cornerVert.end()) return it->second;
        return cornerVert[hv] = addVert(cv.samples[k], true);
      }
    }
    auto key = std::make_pair(c, k);
    auto it = sampleVert.find(key);
    if (it != sampleVert.end()) return it->second;
    return sampleVert[key] = addVert(cv.samples[k], true);
  };
  // the samples of a side in order: (low vertex, 3D position), corner to corner
  auto sideSamples = [&](const Side& sd, std::vector<uint32_t>& ids, std::vector<vec3>& pos) {
    ids.clear(), pos.clear();
    for (size_t j = 0; j < sd.curves.size(); ++j) {
      auto [c, fwd] = sd.curves[j];
      int n = curves[c].count;
      for (int k = (j ? 1 : 0); k <= n; ++k) {
        int kk = fwd ? k : n - k;
        ids.push_back(curvePoint(c, kk));
        pos.push_back(curves[c].samples[kk]);
      }
    }
  };

  mark("corners, curves, counts, samples");
  // ---- each patch: flatten, lay out the quads, map them back
  struct PatchOut {
    std::vector<vec3> interior;               // new vertices (positions)
    std::vector<std::array<Ref, 4>> quads;    // >= 0: low vertex; < 0: -(1 + interior index)
    std::string problem;
  };
  std::vector<PatchOut> outs(patches.size());
  // the boundary samples are made here (they are shared), the rest in parallel
  struct Plan {
    std::vector<std::vector<uint32_t>> sideIds;
    std::vector<std::vector<vec3>> sidePos;
  };
  std::vector<Plan> plans(patches.size());
  for (size_t pid = 0; pid < patches.size(); ++pid) {
    Patch& P = patches[pid];
    if (!P.problem.empty()) continue;
    plans[pid].sideIds.resize(P.sides.size()), plans[pid].sidePos.resize(P.sides.size());
    for (size_t k = 0; k < P.sides.size(); ++k) sideSamples(P.sides[k], plans[pid].sideIds[k], plans[pid].sidePos[k]);
  }
  // the high-poly triangles of each face
  std::vector<uint32_t> triStart(F + 1, 0), triOf(high.tris.size());
  for (uint32_t t = 0; t < high.tris.size(); ++t) triStart[high.triFace[t] + 1]++;
  for (uint32_t f = 0; f < F; ++f) triStart[f + 1] += triStart[f];
  {
    std::vector<uint32_t> fill(triStart.begin(), triStart.end() - 1);
    for (uint32_t t = 0; t < high.tris.size(); ++t) triOf[fill[high.triFace[t]]++] = t;
  }
  parallelFor(patches.size(), [&](size_t pid) {
    Patch& P = patches[pid];
    PatchOut& O = outs[pid];
    if (!P.problem.empty()) return;
    Plan& pl = plans[pid];
    // local mesh of the patch
    Flat fl;
    std::unordered_map<uint32_t, uint32_t> loc;
    auto lid = [&](uint32_t v) {
      auto it = loc.find(v);
      if (it != loc.end()) return it->second;
      uint32_t i = (uint32_t)fl.global.size();
      fl.global.push_back(v);
      return loc[v] = i;
    };
    for (uint32_t f : P.faces)
      for (uint32_t k = triStart[f]; k < triStart[f + 1]; ++k) {
        const glm::uvec3& t = high.tris[triOf[k]];
        fl.tris.push_back({lid(high.loopVert[t.x]), lid(high.loopVert[t.y]), lid(high.loopVert[t.z])});
      }
    size_t nv = fl.global.size();
    fl.uv.assign(nv, vec2(0));
    std::vector<std::vector<uint32_t>> nbr(nv);
    for (auto& t : fl.tris)
      for (int k = 0; k < 3; ++k) {
        uint32_t a = t[k], b = t[(k + 1) % 3];
        if (std::find(nbr[a].begin(), nbr[a].end(), b) == nbr[a].end()) nbr[a].push_back(b), nbr[b].push_back(a);
      }
    std::vector<char> fixedV(nv, 0);
    // boundary placement by arc length along each side / loop
    auto placeLoop = [&](const std::vector<uint32_t>& loop, size_t start, const std::function<vec2(double)>& posAt) {
      size_t n = loop.size();
      std::vector<double> s(n + 1, 0.0);
      for (size_t i = 0; i < n; ++i)
        s[i + 1] = s[i] + glm::length(high.positions[loop[(start + i + 1) % n]] - high.positions[loop[(start + i) % n]]);
      for (size_t i = 0; i < n; ++i) {
        uint32_t v = loc[loop[(start + i) % n]];
        fl.uv[v] = posAt(s[i] / std::max(s[n], 1e-12));
        fixedV[v] = 1;
      }
    };
    int K = (int)P.sides.size();
    std::vector<vec2> poly;  // disk: the corners in the plane
    size_t ia = 0, ib = 0;
    if (!P.ring) {
      std::vector<int> counts;
      for (auto& ids : pl.sideIds) counts.push_back((int)ids.size() - 1);
      poly = polygonCorners(K, counts);
      auto& loop = P.loops[0];
      size_t n = loop.size();
      for (int k = 0; k < K; ++k) {  // side k: from corner k to corner k+1, by arc length
        int a = P.corners[k], b = P.corners[(k + 1) % K];
        size_t span = (b - a + n) % n;
        if (span == 0) span = n;
        std::vector<double> s(span + 1, 0.0);
        for (size_t i = 0; i < span; ++i)
          s[i + 1] = s[i] + glm::length(high.positions[loop[(a + i + 1) % n]] - high.positions[loop[(a + i) % n]]);
        for (size_t i = 0; i < span; ++i) {
          uint32_t v = loc[loop[(a + i) % n]];
          fl.uv[v] = glm::mix(poly[k], poly[(k + 1) % K], (float)(s[i] / std::max(s[span], 1e-12)));
          fixedV[v] = 1;
        }
      }
    } else {
      // a ring: the first loop on the outer circle (counterclockwise), the second on the inner one
      // (clockwise), starting where they are nearest each other
      auto& A = P.loops[0];
      auto& B = P.loops[1];
      // the first loop starts where its first side's samples start
      auto [c0, fwd0] = P.sides[0].curves[0];
      uint32_t a0 = fwd0 ? curves[c0].verts.front() : curves[c0].verts.back();
      ia = std::find(A.begin(), A.end(), a0) - A.begin();
      if (ia >= A.size()) ia = 0;
      float bd = 1e30f;
      for (size_t i = 0; i < B.size(); ++i)
        if (float d = glm::length(high.positions[B[i]] - high.positions[A[ia]]); d < bd) bd = d, ib = i;
      placeLoop(A, ia, [](double f) { return vec2(2 * std::cos(2 * kPi * f), 2 * std::sin(2 * kPi * f)); });
      placeLoop(B, ib, [](double f) { return vec2(std::cos(-2 * kPi * f), std::sin(-2 * kPi * f)); });
    }
    // interior: start from the middle of the boundary
    vec2 c(0);
    int cb = 0;
    for (size_t v = 0; v < nv; ++v)
      if (fixedV[v]) c += fl.uv[v], ++cb;
    c /= float(std::max(1, cb));
    for (size_t v = 0; v < nv; ++v)
      if (!fixedV[v]) fl.uv[v] = P.ring ? vec2(1.5f, 0) : c;
    if (P.ring)  // a warm start on the middle circle, by the angle of a boundary neighbor
      for (size_t v = 0; v < nv; ++v) {
        if (fixedV[v]) continue;
        for (uint32_t u : nbr[v])
          if (fixedV[u]) {
            fl.uv[v] = glm::normalize(fl.uv[u]) * 1.5f;
            break;
          }
      }
    auto tp0 = std::chrono::steady_clock::now();
    flattenEvenly(fl, high, fixedV, nbr, 3);
    buildLocator(fl);
    double tFlat = std::chrono::duration<double>(std::chrono::steady_clock::now() - tp0).count();

    Layout lay;
    lay.measure = [&](vec2 a, vec2 b) {
      float da = 1, db = 1;
      vec3 pa = locate(fl, high, a, &dens, &da), pb = locate(fl, high, b, &dens, &db);
      return glm::length(pb - pa) * 0.5 * (std::sqrt(da) + std::sqrt(db)) / L0;
    };
    bool ok;
    if (!P.ring) {
      std::vector<Chain> side(K);
      for (int k = 0; k < K; ++k) {
        auto& pos = pl.sidePos[k];
        std::vector<double> s(pos.size(), 0.0);
        for (size_t j = 1; j < pos.size(); ++j) s[j] = s[j - 1] + glm::length(pos[j] - pos[j - 1]);
        for (size_t j = 0; j < pos.size(); ++j) {
          side[k].id.push_back(pl.sideIds[k][j]);
          side[k].p.push_back(glm::mix(poly[k], poly[(k + 1) % K], (float)(s[j] / std::max(s.back(), 1e-12))));
        }
      }
      vec2 center(0);  // the polygon's center of area
      float A = 0;
      for (int k = 0; k < K; ++k) {
        vec2 p = poly[k], q = poly[(k + 1) % K];
        float cr = p.x * q.y - q.x * p.y;
        A += cr, center += (p + q) * cr;
      }
      center = std::abs(A) > 1e-9f ? center / (3 * A) : vec2(0);
      ok = fillPolygon(lay, side, center);
      if (!ok) O.problem = "no layout of quads fits its edge counts (move a corner or change the density)";
    } else {
      // the second loop runs the other way: the top line is it backwards, from its sample nearest
      // the first loop's start
      auto& A = pl.sideIds[0];
      auto& Apos = pl.sidePos[0];
      auto& B = pl.sideIds[1];
      auto& Bpos = pl.sidePos[1];
      int nA = (int)A.size() - 1, nB = (int)B.size() - 1;
      int s0 = 0;
      float bd = 1e30f;
      for (int k = 0; k < nB; ++k)
        if (float d = glm::length(Bpos[k] - Apos[0]); d < bd) bd = d, s0 = k;
      std::vector<Ref> bottom, top;
      std::vector<double> angA(nA + 1, 0.0), angB(nB + 1, 0.0);
      for (int k = 0; k <= nA; ++k) bottom.push_back(A[k % nA]);
      for (int k = 1; k <= nA; ++k) angA[k] = angA[k - 1] + glm::length(Apos[k] - Apos[k - 1]);
      for (int k = 0; k <= nB; ++k) top.push_back(B[((s0 - k) % nB + nB) % nB]);
      for (int k = 1; k <= nB; ++k) {
        int a = ((s0 - k + 1) % nB + nB) % nB, b = ((s0 - k) % nB + nB) % nB;
        angB[k] = angB[k - 1] + glm::length(Bpos[b] - Bpos[a]);
      }
      for (auto& x : angA) x *= 2 * kPi / std::max(angA[nA], 1e-12);
      double bTotal = std::max(angB[nB], 1e-12);
      for (auto& x : angB) x *= 2 * kPi / bTotal;
      ok = fillRing(lay, bottom, angA, top, angB, P.rows);
      if (!ok) O.problem = "it is much wider between its two ends than at them (not a tube): split it across the middle";
    }
    if (std::getenv("PATINA_RETOPO_DEBUG")) {
      std::string t;
      for (auto& ids : pl.sideIds) t += std::to_string(ids.size() - 1) + " ";
      PLOG_I("patch %d: %s%zu sides, counts %s-> %zu quads%s; %zu faces, flat %.2f s, fill %.2f s", P.label, P.ring ? "ring, " : "",
             P.sides.size(), t.c_str(), lay.quads.size(), ok ? "" : " (failed)", P.faces.size(), tFlat,
             std::chrono::duration<double>(std::chrono::steady_clock::now() - tp0).count() - tFlat);
    }
    if (!ok) return;
    O.interior.reserve(lay.pts.size());
    for (auto& p : lay.pts) O.interior.push_back(locate(fl, high, p));
    O.quads = std::move(lay.quads);
  });

  mark("flatten and fill");
  // ---- assemble
  for (size_t pid = 0; pid < patches.size(); ++pid) {
    Patch& P = patches[pid];
    PatchOut& O = outs[pid];
    PatchInfo info;
    info.label = P.label, info.ring = P.ring, info.faces = (uint32_t)P.faces.size();
    info.sides = P.ring ? 0 : (int)P.sides.size();
    if (P.problem.empty() && !O.problem.empty()) P.problem = O.problem;
    info.problem = P.problem;
    if (P.problem.empty()) {
      uint32_t base = (uint32_t)low.positions.size();
      for (auto& p : O.interior) addVert(p, false);
      if (low.faceStart.empty()) low.faceStart.push_back(0);
      for (auto& q : O.quads) {
        for (int k = 0; k < 4; ++k) low.loopVert.push_back(q[k] >= 0 ? (uint32_t)q[k] : base + (uint32_t)(-q[k] - 1));
        low.faceStart.push_back((uint32_t)low.loopVert.size());
        res.quadPatch.push_back(P.label);
      }
      info.quads = (int)O.quads.size();
      res.quads += info.quads;
    } else {
      res.problems.push_back("Patch " + std::to_string(P.label) + " was left out: " + P.problem);
    }
    res.patches.push_back(info);
  }
  if (low.faceStart.size() < 2) {
    low = Mesh();
    res.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return res;
  }

  // ---- relax the inside vertices along the surface
  {
    size_t nv = low.positions.size();
    std::vector<std::vector<uint32_t>> nb(nv);
    for (uint32_t f = 0; f + 1 < low.faceStart.size(); ++f)
      for (uint32_t l = low.faceStart[f]; l < low.faceStart[f + 1]; ++l) {
        uint32_t a = low.loopVert[l], b = low.loopVert[l + 1 < low.faceStart[f + 1] ? l + 1 : low.faceStart[f]];
        if (std::find(nb[a].begin(), nb[a].end(), b) == nb[a].end()) nb[a].push_back(b), nb[b].push_back(a);
      }
    for (int it = 0; it < opt.relaxIterations; ++it) {
      std::vector<vec3> next = low.positions;
      parallelFor(nv, [&](size_t v) {
        if (pinned[v] || nb[v].empty()) return;
        vec3 c(0);
        for (uint32_t u : nb[v]) c += low.positions[u];
        c /= float(nb[v].size());
        RayHit h = bvh.closestPoint(high, c, L0 * 4);
        next[v] = h.hit ? h.position : c;
      });
      low.positions = std::move(next);
    }
    for (size_t v = 0; v < nv; ++v)
      if (!pinned[v] && nb[v].size() != 4) ++res.poles;
    // vertices on the open edge (an edge with one quad) aren't counted
    std::map<std::pair<uint32_t, uint32_t>, int> uses;
    for (uint32_t f = 0; f + 1 < low.faceStart.size(); ++f)
      for (uint32_t l = low.faceStart[f]; l < low.faceStart[f + 1]; ++l) {
        uint32_t a = low.loopVert[l], b = low.loopVert[l + 1 < low.faceStart[f + 1] ? l + 1 : low.faceStart[f]];
        uses[{std::min(a, b), std::max(a, b)}]++;
      }
    std::vector<char> open(nv, 0);
    for (auto& [e, n] : uses)
      if (n == 1) open[e.first] = open[e.second] = 1;
    auto& Q = res.quality;
    for (size_t v = 0; v < nv; ++v) {
      if (open[v] || nb[v].empty()) continue;
      ++Q.vertices;
      size_t k = nb[v].size();
      (k == 4 ? Q.regular : k == 3 ? Q.three : k == 5 ? Q.five : Q.other)++;
    }
    Q.faces = (int)low.faceStart.size() - 1;
    for (uint32_t f = 0; f + 1 < low.faceStart.size(); ++f) Q.quads += low.faceStart[f + 1] - low.faceStart[f] == 4;
  }
  low.autoSmoothAngleDeg = 180.f;
  low.finalize();
  res.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  PLOG_I("Retopology: %d quads in %zu patches (%zu left out), %d poles, %.2f s", res.quads, patches.size(), res.problems.size(),
         res.poles, res.seconds);
  return res;
}

}  // namespace patina::retopo
