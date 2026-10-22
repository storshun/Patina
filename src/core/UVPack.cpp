// Shape-aware UV packing.
//
// The islands' real outlines are rasterized (conservatively: every cell a triangle touches) into a
// grid over 0..1, and each island, largest first, goes to the lowest place where its outline fits,
// trying four rotations. Small islands drop into the gaps and concave parts of big ones, which a
// bounding-box packer leaves empty. The overall scale (the texel density when densities are
// equalized) is found by bisection: the largest one at which every island fits.
//
// A footprint is stored as runs of cells per row; the grid keeps a prefix sum per row, so testing
// a run is O(1), and a failed test jumps straight past the blocking cell.
#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <thread>

#include "core/Log.h"
#include "core/Parallel.h"
#include "core/UVOps.h"

namespace patina::uv {

using dvec2 = glm::dvec2;

namespace {

struct Run {
  int a, b;  // inclusive cell columns
};

struct Footprint {
  int w = 0, h = 0;                   // cells
  std::vector<std::vector<Run>> rows;  // per row, merged runs
  std::vector<int> order;             // rows to test first (widest first: fails fastest)
  int wideRow = 0, wideRun = 1;       // the row with the longest single run, and its length
};

// conservative cell runs of triangles (cell units, any position) added to `rows` (row 0 = y0)
void addTriRuns(dvec2 p0, dvec2 p1, dvec2 p2, int y0, int nrows, std::vector<std::vector<Run>>& rows,
                int xmin = INT32_MIN, int xmax = INT32_MAX) {
  const dvec2 v[3] = {p0, p1, p2};
  double ylo = std::min({p0.y, p1.y, p2.y}), yhi = std::max({p0.y, p1.y, p2.y});
  if (!std::isfinite(ylo) || !std::isfinite(yhi)) return;
  int j0 = std::max(y0, (int)std::floor(ylo)), j1 = std::min(y0 + nrows - 1, (int)std::ceil(yhi) - 1);
  if (std::ceil(yhi) - 1 < std::floor(ylo)) j1 = std::min(j1, (int)std::floor(ylo));  // flat triangle
  for (int j = j0; j <= j1; ++j) {
    // x extent of the triangle inside the strip [j, j + 1]
    double lo = 1e300, hi = -1e300;
    for (int k = 0; k < 3; ++k) {
      const dvec2 &a = v[k], &b = v[(k + 1) % 3];
      if (a.y >= j && a.y <= j + 1) lo = std::min(lo, a.x), hi = std::max(hi, a.x);
      for (double yy : {double(j), double(j + 1)}) {
        if ((a.y - yy) * (b.y - yy) < 0) {
          double t = (yy - a.y) / (b.y - a.y);
          double x = a.x + t * (b.x - a.x);
          lo = std::min(lo, x), hi = std::max(hi, x);
        }
      }
    }
    if (lo > hi) continue;
    int a = (int)std::floor(lo), b = std::max(a, (int)std::ceil(hi) - 1);
    a = std::max(a, xmin), b = std::min(b, xmax);
    if (a > b) continue;
    rows[j - y0].push_back({a, b});
  }
}

void mergeRuns(std::vector<Run>& r) {
  if (r.size() < 2) return;
  std::sort(r.begin(), r.end(), [](const Run& x, const Run& y) { return x.a < y.a; });
  std::vector<Run> out;
  for (const Run& x : r) {
    if (!out.empty() && x.a <= out.back().b + 1) out.back().b = std::max(out.back().b, x.b);
    else out.push_back(x);
  }
  r.swap(out);
}

struct Occupancy {
  int R;
  std::vector<uint8_t> cell;
  std::vector<int> pre;      // per row: R + 1 prefix sums
  std::vector<int> longest;  // per row: the longest run of free cells
  explicit Occupancy(int r) : R(r), cell(size_t(r) * r, 0), pre(size_t(r) * (r + 1), 0), longest(r, r) {}
  void rebuild(int y) {
    int* p = &pre[size_t(y) * (R + 1)];
    const uint8_t* c = &cell[size_t(y) * R];
    p[0] = 0;
    int run = 0, best = 0;
    for (int x = 0; x < R; ++x) {
      p[x + 1] = p[x] + c[x];
      run = c[x] ? 0 : run + 1;
      best = std::max(best, run);
    }
    longest[y] = best;
  }
  void rebuildAll() {
    for (int y = 0; y < R; ++y) rebuild(y);
  }
  bool free(int y, int a, int b) const {
    const int* p = &pre[size_t(y) * (R + 1)];
    return p[b + 1] == p[a];
  }
  // the rightmost occupied cell in [a, b] (which has one)
  int lastOccupied(int y, int a, int b) const {
    const int* p = &pre[size_t(y) * (R + 1)];
    int target = p[b + 1], lo = a + 1, hi = b + 1;  // smallest t with p[t] == target
    while (lo < hi) {
      int mid = (lo + hi) / 2;
      if (p[mid] == target) hi = mid;
      else lo = mid + 1;
    }
    return lo - 1;
  }
  void mark(int y, int a, int b) {
    if (y < 0 || y >= R) return;
    a = std::max(a, 0), b = std::min(b, R - 1);
    for (int x = a; x <= b; ++x) cell[size_t(y) * R + x] = 1;
  }
  double fill() const {
    size_t n = 0;
    for (uint8_t c : cell) n += c;
    return double(n) / double(cell.size());
  }
};

// an island to pack: its corners in the normalized frame (density 1 when equalizing), and its
// triangles as indices into them
struct Shape {
  std::vector<uint32_t> loops;
  std::vector<dvec2> pts;
  std::vector<glm::uvec3> tris;
  double area = 0;
  // per rotation (0, 90, 180, 270 degrees): bounding box low corner and size at scale 1
  dvec2 lo[4] = {}, size[4] = {};
};

dvec2 rot90(dvec2 p, int r) {
  switch (r & 3) {
    case 1: return {-p.y, p.x};
    case 2: return {-p.x, -p.y};
    case 3: return {p.y, -p.x};
    default: return p;
  }
}

std::vector<dvec2> hull(std::vector<dvec2> p) {
  std::sort(p.begin(), p.end(), [](const dvec2& a, const dvec2& b) { return a.x != b.x ? a.x < b.x : a.y < b.y; });
  p.erase(std::unique(p.begin(), p.end()), p.end());
  if (p.size() < 3) return p;
  std::vector<dvec2> h(2 * p.size());
  size_t k = 0;
  auto cr = [](const dvec2& o, const dvec2& a, const dvec2& b) { return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); };
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

// rotates the points so their minimum-area bounding box is axis aligned
void alignMinBox(std::vector<dvec2>& pts) {
  auto hl = hull(pts);
  if (hl.size() < 3) return;
  double bestA = 1e300, bc = 1, bs = 0;
  auto area = [&](double c, double s) {
    double x0 = 1e300, x1 = -1e300, y0 = 1e300, y1 = -1e300;
    for (auto& q : hl) {
      double x = c * q.x + s * q.y, y = -s * q.x + c * q.y;
      x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
    }
    return (x1 - x0) * (y1 - y0);
  };
  bestA = area(1, 0);
  for (size_t i = 0; i < hl.size(); ++i) {
    dvec2 d = hl[(i + 1) % hl.size()] - hl[i];
    double len = glm::length(d);
    if (len < 1e-300) continue;
    double a = area(d.x / len, d.y / len);
    if (a < bestA * (1 - 1e-6)) bestA = a, bc = d.x / len, bs = d.y / len;
  }
  for (auto& q : pts) q = {bc * q.x + bs * q.y, -bs * q.x + bc * q.y};
}

Footprint footprint(const Shape& s, int rot, double k, int R) {
  Footprint f;
  double scale = k * R;
  dvec2 lo = s.lo[rot] * scale;
  f.w = std::max(1, (int)std::ceil(s.size[rot].x * scale - 1e-9));
  f.h = std::max(1, (int)std::ceil(s.size[rot].y * scale - 1e-9));
  f.rows.assign(f.h, {});
  for (const auto& t : s.tris) {
    dvec2 a = rot90(s.pts[t.x], rot) * scale - lo, b = rot90(s.pts[t.y], rot) * scale - lo,
          c = rot90(s.pts[t.z], rot) * scale - lo;
    addTriRuns(a, b, c, 0, f.h, f.rows, 0, f.w - 1);
  }
  for (auto& r : f.rows) mergeRuns(r);
  f.order.resize(f.h);
  std::iota(f.order.begin(), f.order.end(), 0);
  auto width = [&](int j) {
    int w = 0;
    for (const Run& r : f.rows[j]) w += r.b - r.a + 1;
    return w;
  };
  std::stable_sort(f.order.begin(), f.order.end(), [&](int x, int y) { return width(x) > width(y); });
  for (int j = 0; j < f.h; ++j)
    for (const Run& r : f.rows[j])
      if (r.b - r.a + 1 > f.wideRun) f.wideRun = r.b - r.a + 1, f.wideRow = j;
  return f;
}

// whether turning the footprint by 180 degrees gives the same cells (rectangles, most quads)
bool halfTurnSymmetric(const Footprint& a, const Footprint& b) {
  if (a.w != b.w || a.h != b.h) return false;
  for (int j = 0; j < a.h; ++j) {
    const auto& ra = a.rows[j];
    const auto& rb = b.rows[a.h - 1 - j];
    if (ra.size() != rb.size()) return false;
    for (size_t i = 0; i < ra.size(); ++i) {
      const Run& x = ra[i];
      const Run& y = rb[rb.size() - 1 - i];
      if (x.a != a.w - 1 - y.b || x.b != a.w - 1 - y.a) return false;
    }
  }
  return true;
}

// how much of the footprint's outline at (x, y) touches something (neighbors, their margins, the
// border), as a share of the outline: tight spots score high
double contact(const Occupancy& occ, const Footprint& f, int x, int y) {
  int R = occ.R, touch = 0, total = 0;
  auto occupied = [&](int yy, int a, int b) {  // occupied cells in [a, b] of row yy (outside = occupied)
    if (yy < 0 || yy >= R) return b - a + 1;
    int n = 0;
    if (a < 0) n += -a, a = 0;
    if (b >= R) n += b - R + 1, b = R - 1;
    if (a <= b) n += occ.pre[size_t(yy) * (R + 1) + b + 1] - occ.pre[size_t(yy) * (R + 1) + a];
    return n;
  };
  for (int j = 0; j < f.h; ++j)
    for (const Run& r : f.rows[j]) {
      int a = x + r.a, b = x + r.b, yy = y + j;
      touch += occupied(yy, a - 1, a - 1) + occupied(yy, b + 1, b + 1);
      touch += occupied(yy - 1, a, b) + occupied(yy + 1, a, b);
      total += 2 + 2 * (r.b - r.a + 1);
    }
  return total ? double(touch) / total : 0.0;
}

// Candidate places for the footprint: the leftmost fit in each of the lowest rows where it fits
// (up to `maxCandidates` rows, within a band above the first). Returns false if none.
bool findPlaces(const Occupancy& occ, const Footprint& f, int maxCandidates, std::vector<glm::ivec2>& out) {
  out.clear();
  int R = occ.R;
  if (f.w > R || f.h > R) return false;
  int firstY = -1;
  for (int y = 0; y + f.h <= R; ++y) {
    if (firstY >= 0 && (y - firstY > f.h * 2 + 4 || (int)out.size() >= maxCandidates)) break;
    if (occ.longest[y + f.wideRow] < f.wideRun) continue;  // that row has no room for the widest run
    int x = 0;
    while (x + f.w <= R) {
      int next = -1;
      for (int j : f.order) {
        for (const Run& r : f.rows[j]) {
          if (!occ.free(y + j, x + r.a, x + r.b)) {
            next = occ.lastOccupied(y + j, x + r.a, x + r.b) - r.a + 1;
            break;
          }
        }
        if (next >= 0) break;
      }
      if (next < 0) {
        out.push_back({x, y});
        if (firstY < 0) firstY = y;
        break;
      }
      x = std::max(x + 1, next);
    }
  }
  return !out.empty();
}

// marks the footprint at (x, y), grown by mc cells on every side (the margin)
void claim(Occupancy& occ, const Footprint& f, int x, int y, int mc) {
  for (int j = 0; j < f.h; ++j)
    for (const Run& r : f.rows[j])
      for (int yy = y + j - mc; yy <= y + j + mc; ++yy) occ.mark(yy, x + r.a - mc, x + r.b + mc);
  for (int yy = std::max(0, y - mc); yy <= std::min(occ.R - 1, y + f.h - 1 + mc); ++yy) occ.rebuild(yy);
}

}  // namespace

bool packShapes(Mesh& m, const std::vector<std::vector<uint32_t>>& charts, const PackOptions& opt,
                const std::vector<uint32_t>& obstacles, double maxScale, ShapePackStats* stats) {
  if (m.loopUV.size() != m.loopCount()) return false;
  uint32_t F = m.faceCount();
  const int R = std::clamp(opt.resolution, 64, 4096);
  bool flags = m.faceFlags.size() == F;
  // ---- the islands, in a normalized frame
  std::vector<uint8_t> taken(F, 0);
  std::vector<Shape> shapes;
  for (const auto& chart : charts) {
    bool copies = flags && !chart.empty();
    for (uint32_t f : chart) copies &= f < F && (m.faceFlags[f] & FaceMirrorCopy) != 0;
    if (copies) continue;  // mirror copies sit on the faces they copy
    Shape s;
    double a3 = 0, au = 0;
    std::vector<uint32_t> faces;
    for (uint32_t f : chart) {
      if (f >= F || taken[f]) continue;
      taken[f] = 1;
      faces.push_back(f);
      a3 += m.faceArea3D(f);
      float u = m.faceAreaUV(f);
      if (std::isfinite(u)) au += std::abs(u);
    }
    if (faces.empty()) continue;
    double sc = 1;
    if (opt.normalizeDensity) sc = (a3 > 1e-30 && au > 1e-30) ? std::sqrt(a3 / au) : -1;
    for (uint32_t f : faces) {
      uint32_t base = (uint32_t)s.pts.size();
      for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) {
        vec2 uv = m.loopUV[l];
        if (!std::isfinite(uv.x) || !std::isfinite(uv.y)) uv = vec2(0);
        s.loops.push_back(l);
        s.pts.push_back(dvec2(uv));
      }
      uint32_t n = m.faceStart[f + 1] - m.faceStart[f];
      for (uint32_t k = 1; k + 1 < n; ++k) s.tris.push_back({base, base + k, base + k + 1});
    }
    s.area = sc;  // (temporarily: the scale)
    shapes.push_back(std::move(s));
  }
  if (shapes.empty()) return false;
  {  // islands without a usable density ratio get the mean scale of the others
    double sum = 0;
    int n = 0;
    for (auto& s : shapes)
      if (s.area > 0) sum += s.area, ++n;
    double fallback = n ? sum / n : 1.0;
    parallelFor(shapes.size(), [&](size_t si) {
      Shape& s = shapes[si];
      double sc = s.area > 0 ? s.area : fallback;
      for (auto& p : s.pts) p *= sc;
      if (opt.rotate) alignMinBox(s.pts);
      double a = 0;
      for (const auto& t : s.tris) {
        dvec2 u = s.pts[t.y] - s.pts[t.x], v = s.pts[t.z] - s.pts[t.x];
        a += std::abs(u.x * v.y - u.y * v.x) * 0.5;
      }
      s.area = a;
      for (int r = 0; r < 4; ++r) {
        dvec2 lo(1e300), hi(-1e300);
        for (auto& p : s.pts) {
          dvec2 q = rot90(p, r);
          lo = glm::min(lo, q), hi = glm::max(hi, q);
        }
        s.lo[r] = lo, s.size[r] = hi - lo;
      }
    });
  }
  std::vector<size_t> order(shapes.size());
  std::iota(order.begin(), order.end(), 0);
  std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
    double ma = std::max(shapes[a].size[0].x, shapes[a].size[0].y), mb = std::max(shapes[b].size[0].x, shapes[b].size[0].y);
    return shapes[a].area * 0.5 + ma * ma * 0.5 > shapes[b].area * 0.5 + mb * mb * 0.5;
  });
  // ---- what stays: obstacles (their own UVs), the border, and the margin
  const int mc = std::max(1, (int)std::ceil(std::max(0.f, opt.margin) * R));
  const int border = std::max(1, mc / 2);
  Occupancy base(R);
  {
    std::vector<std::vector<Run>> rows(R);
    for (uint32_t f : obstacles) {
      if (f >= F || taken[f]) continue;
      if (flags && (m.faceFlags[f] & FaceMirrorCopy)) continue;
      uint32_t s0 = m.faceStart[f], n = m.faceStart[f + 1] - s0;
      for (uint32_t k = 1; k + 1 < n; ++k)
        addTriRuns(dvec2(m.loopUV[s0]) * double(R), dvec2(m.loopUV[s0 + k]) * double(R), dvec2(m.loopUV[s0 + k + 1]) * double(R),
                   0, R, rows, -mc, R - 1 + mc);
    }
    for (int y = 0; y < R; ++y)
      for (Run r : rows[y])
        for (int yy = y - mc; yy <= y + mc; ++yy) base.mark(yy, r.a - mc, r.b + mc);
    for (int y = 0; y < R; ++y) {
      bool edge = y < border || y >= R - border;
      for (int x = 0; x < R; ++x)
        if (edge || x < border || x >= R - border) base.cell[size_t(y) * R + x] = 1;
    }
    base.rebuildAll();
  }
  // ---- packing at a scale
  struct Place {
    int rot = 0, x = 0, y = 0;
  };
  std::vector<Place> best;
  double bestK = 0;
  const int rots = opt.rotate ? 4 : 1;
  auto tryPack = [&](double k, std::vector<Place>& place) {
    place.assign(shapes.size(), Place{});
    std::vector<glm::ivec2> cand;
    Occupancy occ = base;
    // (every island's outlines at this scale first, all at once: they don't depend on the placing)
    std::vector<std::array<Footprint, 4>> allFps(shapes.size());
    parallelFor(shapes.size() * rots, [&](size_t j) { allFps[j / rots][j % rots] = footprint(shapes[j / rots], int(j % rots), k, R); });
    for (size_t i : order) {
      int bx = -1, by = -1, br = 0;
      double bestScore = -1e300;
      Footprint bf;
      auto& fps = allFps[i];
      for (int r = 0; r < rots; ++r) {
        if (r >= 2 && halfTurnSymmetric(fps[r - 2], fps[r])) continue;  // nothing new to try
        const Footprint& fp = fps[r];
        if (!findPlaces(occ, fp, opt.candidates, cand)) continue;
        for (auto c : cand) {
          // snug (touching the neighbors) and low
          double score = contact(occ, fp, c.x, c.y) - 1.5 * double(c.y + fp.h) / R - 0.25 * double(c.x) / R;
          if (score > bestScore) bestScore = score, bx = c.x, by = c.y, br = r, bf = fp;
        }
      }
      if (bx < 0) return false;
      claim(occ, bf, bx, by, mc);
      place[i] = {br, bx, by};
    }
    return true;
  };
  double total = 0;
  for (auto& s : shapes) total += s.area;
  double freeArea = std::max(0.02, 1.0 - base.fill());
  double hi = total > 1e-30 ? std::sqrt(freeArea / total) : 1.0;
  for (auto& s : shapes) {
    double side = std::max(s.size[0].x, s.size[0].y);
    double usable = 1.0 - 2.0 * double(border + 1) / R;
    if (side > 1e-30) hi = std::min(hi, usable / side);
  }
  if (maxScale > 0) hi = std::min(hi, maxScale);
  // (several scales tried at once, each packing on a thread of its own: the largest that fits, found
  // as closely as one by one, in a few rounds)
  const int lanes = std::clamp((int)std::thread::hardware_concurrency(), 1, 8);
  auto tryMany = [&](const std::vector<double>& ks, std::vector<char>& ok, std::vector<std::vector<Place>>& places) {
    ok.assign(ks.size(), 0), places.assign(ks.size(), {});
    std::vector<std::thread> th;
    for (size_t i = 1; i < ks.size(); ++i) th.emplace_back([&, i] { ok[i] = tryPack(ks[i], places[i]); });
    if (!ks.empty()) ok[0] = tryPack(ks[0], places[0]);
    for (auto& t : th) t.join();
  };
  double lo = 0;
  std::vector<char> ok;
  std::vector<std::vector<Place>> places;
  if (tryPack(hi, best)) {
    bestK = hi;
  } else {
    // downwards from hi by 0.85 a step, lanes at a time, until one fits
    double k = hi;
    for (int g = 0; g < 30 && bestK <= 0; g += lanes) {
      std::vector<double> ks;
      for (int j = 1; j <= lanes && g + j <= 30; ++j) ks.push_back(k * std::pow(0.85, j));
      tryMany(ks, ok, places);
      for (size_t i = 0; i < ks.size(); ++i)
        if (ok[i]) {
          lo = ks[i], bestK = ks[i], best = std::move(places[i]), hi = i ? ks[i - 1] : k;
          break;
        }
      if (bestK <= 0) k = ks.back();
    }
    if (bestK <= 0) return false;
    // then between the last that didn't fit and the first that did
    for (int round = 0; round < 3 && hi > lo * 1.01; ++round) {
      std::vector<double> ks;
      for (int j = 1; j <= lanes; ++j) ks.push_back(lo * std::pow(hi / lo, double(j) / (lanes + 1)));
      tryMany(ks, ok, places);
      for (size_t i = 0; i < ks.size(); ++i)
        if (ok[i] && ks[i] > lo) lo = ks[i], bestK = ks[i], best = places[i];
      // (packing isn't quite monotonic: a failure below the best success is passed over)
      for (size_t i = 0; i < ks.size(); ++i)
        if (!ok[i] && ks[i] > lo && ks[i] < hi) hi = ks[i];
    }
  }
  // ---- apply
  for (size_t i = 0; i < shapes.size(); ++i) {
    const Shape& s = shapes[i];
    const Place& p = best[i];
    dvec2 lo2 = s.lo[p.rot] * bestK, off = dvec2(p.x, p.y) / double(R);
    for (size_t k = 0; k < s.loops.size(); ++k) m.loopUV[s.loops[k]] = vec2(rot90(s.pts[k], p.rot) * bestK - lo2 + off);
  }
  if (stats) {
    stats->islands = (int)shapes.size();
    stats->scale = bestK;
    stats->fill = total * bestK * bestK;
  }
  PLOG_I("Packed %zu island shapes: %.0f%% of the texture used", shapes.size(), total * bestK * bestK * 100.0);
  return true;
}

}  // namespace patina::uv
