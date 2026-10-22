#include "core/UVIsland.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <unordered_map>
#include <numeric>

#include "core/Log.h"
#include "core/Parallel.h"
#include "core/UVOps.h"

namespace patina::uv {

namespace {

using dvec2 = glm::dvec2;
constexpr double kPiD = 3.14159265358979323846;

void ensureTopo(Mesh& m) {
  if (m.loopEdge.size() != m.loopCount() || m.edgeFlags.size() != m.edges.size()) m.buildEdges();
  if (m.loopUV.size() != m.loopCount()) m.loopUV.assign(m.loopCount(), vec2(0));
}

// whole islands containing `faces` (empty = all) that may be laid out: not pinned, and not
// mirror copies (those follow the faces they copy)
static std::vector<std::vector<uint32_t>> movableIslands(Mesh& m, const std::vector<uint32_t>& faces) {
  if (m.loopIsland.size() != m.loopCount()) m.computeUVTopology();
  std::vector<uint8_t> want(m.islandCount + 1, faces.empty() ? 1 : 0);
  for (uint32_t f : faces)
    if (f < m.faceCount() && m.faceSize(f)) want[std::min(m.loopIsland[m.faceStart[f]], m.islandCount)] = 1;
  bool flags = m.faceFlags.size() == m.faceCount();
  std::vector<std::vector<uint32_t>> out;
  for (auto& isl : facesByIsland(m)) {
    if (isl.empty() || !want[std::min(m.loopIsland[m.faceStart[isl[0]]], m.islandCount)]) continue;
    bool pinned = false, allCopies = true;
    for (uint32_t f : isl) {
      uint8_t fl = flags ? m.faceFlags[f] : 0;
      pinned |= (fl & FacePinned) != 0;
      allCopies &= (fl & FaceMirrorCopy) != 0;
    }
    if (pinned || allCopies) continue;
    out.push_back(std::move(isl));
  }
  return out;
}

// faces adjacent to each edge
struct EdgeFaces {
  std::vector<uint32_t> start, faces;
  explicit EdgeFaces(const Mesh& m) {
    size_t E = m.edges.size();
    start.assign(E + 1, 0);
    for (uint32_t l = 0; l < m.loopCount(); ++l) start[m.loopEdge[l] + 1]++;
    for (size_t i = 1; i <= E; ++i) start[i] += start[i - 1];
    faces.resize(m.loopCount());
    std::vector<uint32_t> fill(start.begin(), start.end() - 1);
    auto lf = m.loopFace();
    for (uint32_t l = 0; l < m.loopCount(); ++l) faces[fill[m.loopEdge[l]]++] = lf[l];
  }
};

std::vector<uint32_t> uniqueFaces(const Mesh& m, const std::vector<uint32_t>& faces) {
  std::vector<uint32_t> out;
  if (faces.empty()) {
    out.resize(m.faceCount());
    std::iota(out.begin(), out.end(), 0u);
    return out;
  }
  std::vector<uint8_t> seen(m.faceCount(), 0);
  for (uint32_t f : faces)
    if (f < m.faceCount() && !seen[f]) seen[f] = 1, out.push_back(f);
  return out;
}

std::vector<uint32_t> chartLoops(const Mesh& m, const std::vector<uint32_t>& chart) {
  std::vector<uint32_t> loops;
  for (uint32_t f : chart)
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) loops.push_back(l);
  return loops;
}

dvec2 uvCenter(const Mesh& m, const std::vector<uint32_t>& loops) {
  Rect2 r;
  for (uint32_t l : loops) r.add(m.loopUV[l]);
  return r.valid() ? dvec2(r.center()) : dvec2(0.5);
}

void rotateLoops(Mesh& m, const std::vector<uint32_t>& loops, double angle, dvec2 pivot) {
  double c = std::cos(angle), s = std::sin(angle);
  for (uint32_t l : loops) {
    dvec2 d = dvec2(m.loopUV[l]) - pivot;
    m.loopUV[l] = vec2(pivot + dvec2(c * d.x - s * d.y, s * d.x + c * d.y));
  }
}

// triangles of a face (fan over the face's loops; good enough for raster tests)
template <class Fn>
void forFaceTris(const Mesh& m, uint32_t f, Fn&& fn) {
  uint32_t s = m.faceStart[f];
  for (uint32_t l = s + 1; l + 1 < m.faceStart[f + 1]; ++l) fn(s, l, l + 1);
}

// Rasterizes a UV triangle into a res x res grid (cell centers, conservative by `grow` cells).
template <class Fn>
void rasterTriCells(dvec2 a, dvec2 b, dvec2 c, int res, Fn&& fn) {
  auto ok = [](dvec2 p) { return std::isfinite(p.x) && std::isfinite(p.y); };
  if (!ok(a) || !ok(b) || !ok(c)) return;
  double area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
  if (std::abs(area) < 1e-16) return;
  if (area < 0) std::swap(b, c);
  int x0 = std::max(0, (int)std::floor(std::min({a.x, b.x, c.x}) * res));
  int x1 = std::min(res - 1, (int)std::floor(std::max({a.x, b.x, c.x}) * res));
  int y0 = std::max(0, (int)std::floor(std::min({a.y, b.y, c.y}) * res));
  int y1 = std::min(res - 1, (int)std::floor(std::max({a.y, b.y, c.y}) * res));
  auto edge = [](dvec2 p, dvec2 q, dvec2 r) { return (q.x - p.x) * (r.y - p.y) - (q.y - p.y) * (r.x - p.x); };
  // a cell is covered if its center is inside, or the triangle is smaller than a cell
  bool tiny = (x1 - x0) <= 1 && (y1 - y0) <= 1;
  for (int j = y0; j <= y1; ++j)
    for (int i = x0; i <= x1; ++i) {
      dvec2 p((i + 0.5) / res, (j + 0.5) / res);
      if (tiny || (edge(a, b, p) >= 0 && edge(b, c, p) >= 0 && edge(c, a, p) >= 0)) fn(i, j);
    }
}

struct Grid {
  int res;
  std::vector<uint8_t> cell;
  explicit Grid(int r) : res(r), cell(size_t(r) * r, 0) {}
  void addFace(const Mesh& m, uint32_t f, dvec2 off = dvec2(0)) {
    forFaceTris(m, f, [&](uint32_t a, uint32_t b, uint32_t c) {
      rasterTriCells(dvec2(m.loopUV[a]) + off, dvec2(m.loopUV[b]) + off, dvec2(m.loopUV[c]) + off, res,
                     [&](int i, int j) { cell[size_t(j) * res + i] = 1; });
    });
  }
  void dilate(int r) {
    if (r <= 0) return;
    std::vector<uint8_t> out(cell.size(), 0);
    for (int j = 0; j < res; ++j)
      for (int i = 0; i < res; ++i) {
        if (!cell[size_t(j) * res + i]) continue;
        for (int dj = -r; dj <= r; ++dj)
          for (int di = -r; di <= r; ++di) {
            int x = i + di, y = j + dj;
            if (x >= 0 && y >= 0 && x < res && y < res) out[size_t(y) * res + x] = 1;
          }
      }
    cell.swap(out);
  }
};

// summed-area table for O(1) "is this rectangle empty"
struct SAT {
  int res;
  std::vector<int> s;
  explicit SAT(const Grid& g) : res(g.res), s(size_t(g.res + 1) * (g.res + 1), 0) {
    for (int j = 0; j < res; ++j)
      for (int i = 0; i < res; ++i)
        s[size_t(j + 1) * (res + 1) + i + 1] = g.cell[size_t(j) * res + i] + s[size_t(j) * (res + 1) + i + 1] +
                                               s[size_t(j + 1) * (res + 1) + i] - s[size_t(j) * (res + 1) + i];
  }
  int sum(int x0, int y0, int x1, int y1) const {  // inclusive cell rect
    auto at = [&](int x, int y) { return s[size_t(y) * (res + 1) + x]; };
    return at(x1 + 1, y1 + 1) - at(x0, y1 + 1) - at(x1 + 1, y0) + at(x0, y0);
  }
};

}  // namespace

// ============================================================ density

namespace {
std::atomic<uint64_t> uvHashEpoch{1};
struct UVHashMemo {
  const Mesh* mesh = nullptr;
  const void* data = nullptr;
  size_t loops = 0, verts = 0;
  uint64_t epoch = 0, value = 0;
};
thread_local UVHashMemo uvMemo;

uint64_t mix64(uint64_t h, uint64_t w) {
  h ^= w * 0x9E3779B97F4A7C15ull;
  h = (h ^ (h >> 31)) * 0xBF58476D1CE4E5B9ull;
  return h ^ (h >> 29);
}

// 8 bytes at a time, in pieces hashed side by side
uint64_t hashWords(const void* p, size_t bytes) {
  const size_t words = bytes / 8;
  const size_t pieces = std::clamp<size_t>(words / (1 << 16), 1, 64);
  std::vector<uint64_t> part(pieces, 0);
  parallelFor(pieces, [&](size_t k) {
    size_t a = words * k / pieces, b = words * (k + 1) / pieces;
    uint64_t h = 0x84222325CBF29CE4ull + k;
    for (size_t i = a; i < b; ++i) {
      uint64_t w;
      std::memcpy(&w, static_cast<const char*>(p) + i * 8, 8);
      h = mix64(h, w);
    }
    part[k] = h;
  });
  uint64_t h = 1469598103934665603ull;
  for (uint64_t x : part) h = mix64(h, x);
  for (size_t i = words * 8; i < bytes; ++i) h = mix64(h, static_cast<const unsigned char*>(p)[i]);
  return h;
}
}  // namespace

std::atomic<bool> uvHashHeld{false};
void uvHashNewFrame() {
  if (!uvHashHeld.load(std::memory_order_relaxed)) uvHashEpoch.fetch_add(1, std::memory_order_relaxed);
}
void uvHashHold(bool hold) {
  // (let go: the next frame works it out again)
  if (uvHashHeld.exchange(hold) && !hold) uvHashEpoch.fetch_add(1, std::memory_order_relaxed);
}

uint64_t uvHash(const Mesh& m) {
  size_t L = m.loopUV.size(), V = m.positions.size();
  uint64_t epoch = uvHashEpoch.load(std::memory_order_relaxed);
  UVHashMemo& c = uvMemo;
  if (c.mesh == &m && c.data == m.loopUV.data() && c.loops == L && c.verts == V && c.epoch == epoch) return c.value;
  uint64_t h = mix64(mix64(1469598103934665603ull, L), V);
  if (L) h = mix64(h, hashWords(m.loopUV.data(), L * sizeof(vec2)));
  if (V) h = mix64(h, hashWords(m.positions.data(), std::min<size_t>(V, 4096) * sizeof(vec3)));
  c = {&m, m.loopUV.data(), L, V, epoch, h};
  return h;
}

float areaDensity(const Mesh& m, const std::vector<uint32_t>& faces) {
  if (m.loopUV.size() != m.loopCount()) return 0.f;
  double a3 = 0, au = 0;
  for (uint32_t f : uniqueFaces(m, faces)) {
    double u = std::abs(m.faceAreaUV(f));
    if (!std::isfinite(u) || u < 1e-14) continue;
    a3 += m.faceArea3D(f);
    au += u;
  }
  return a3 > 1e-30 ? float(std::sqrt(au / a3)) : 0.f;
}

void forEachTile(Mesh& m, const std::vector<uint32_t>& faces,
                 const std::function<void(const std::vector<uint32_t>& tileFaces)>& fn) {
  if (!m.tiles.active()) {
    fn(faces);
    return;
  }
  uint32_t F = m.faceCount();
  if (m.tiles.faceTile.size() != F) m.updateTiles();
  TileLayout saved = m.tiles;
  int n = saved.count();
  std::vector<std::vector<uint32_t>> per(n);
  if (faces.empty()) {
    for (uint32_t f = 0; f < F; ++f) per[std::clamp(saved.faceTile[f], 0, n - 1)].push_back(f);
  } else {
    for (uint32_t f : faces)
      if (f < F) per[std::clamp(saved.faceTile[f], 0, n - 1)].push_back(f);
  }
  m.tiles.mode = 0;  // the frame below is plain 0..1 while fn runs
  for (int t = 0; t < n; ++t) {
    if (per[t].empty()) continue;
    vec2 origin(saved.uvOrigin(t));
    if (m.faceFlags.size() != F) m.faceFlags.assign(F, 0);
    std::vector<vec2> keep = m.loopUV;
    std::vector<uint8_t> keepFlags = m.faceFlags;
    for (uint32_t f = 0; f < F; ++f) {
      bool mine = saved.faceTile[f] == t;
      for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l)
        m.loopUV[l] = mine ? m.loopUV[l] - origin : vec2(-1000.f);  // others: no area, far away
      // ...and out of every count: packers skip mirror copies (not moved, not obstacles, not a density
      // reference), and an unpinned copy is no pinned island either
      if (!mine) m.faceFlags[f] = uint8_t((m.faceFlags[f] & ~(FacePinned | kFaceMirrorBits)) | FaceMirrorCopy);
    }
    m.computeUVTopology();
    fn(per[t]);
    for (uint32_t f = 0; f < F; ++f) {
      bool mine = saved.faceTile[f] == t;
      for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) m.loopUV[l] = mine ? m.loopUV[l] + origin : keep[l];
      if (!mine && f < keepFlags.size() && f < m.faceFlags.size()) m.faceFlags[f] = keepFlags[f];
    }
  }
  m.tiles = saved;
  m.tiles.faceTile.clear();
  m.computeUVTopology();
}

float DensityTargets::recommended() const {
  // a layout denser than twice "fit" overlaps heavily (stacked or shared UVs): new islands at
  // that density could not get space of their own
  if (layout > 0) return fit > 0 ? std::min(layout, 2.f * fit) : layout;
  if (fit > 0 && screen > 0) return std::min(fit, screen);
  return fit > 0 ? fit : screen;
}

DensityTargets densityTargets(const Mesh& m, int resolution, const std::vector<uint8_t>* exclude, float screenHeight,
                              float fill) {
  DensityTargets t;
  t.resolution = std::max(1, resolution);
  uint32_t F = m.faceCount();
  bool flags = m.faceFlags.size() == F;
  // the surface that needs texels of its own: mirror copies share their other half's texels
  double total = 0;
  for (uint32_t f = 0; f < F; ++f)
    if (!(flags && (m.faceFlags[f] & FaceMirrorCopy))) total += m.faceArea3D(f);
  // with texture tiles the surface spreads over that many textures
  int tilesUsed = 1;
  if (m.tiles.active()) {
    std::vector<uint8_t> used(m.tiles.count(), 0);
    for (uint32_t f = 0; f < F; ++f) used[std::clamp(m.faceTile(f), 0, m.tiles.count() - 1)] = 1;
    tilesUsed = std::max(1, (int)std::count(used.begin(), used.end(), (uint8_t)1));
  }
  if (total > 1e-30) t.fit = float(std::sqrt(fill * tilesUsed / total));
  // framed view: the bounding sphere spans about 90% of the view height
  float r = std::max(m.boundingRadius, 1e-6f);
  float pxPerUnit = screenHeight * 0.9f / (2.f * r);
  t.screen = pxPerUnit / float(t.resolution);
  std::vector<uint32_t> keep;
  if (m.loopUV.size() == m.loopCount()) {
    // the layout: faces with real UV area (not excluded, not mirror copies)
    for (uint32_t f = 0; f < F; ++f) {
      if (exclude && f < exclude->size() && (*exclude)[f]) continue;
      if (flags && (m.faceFlags[f] & FaceMirrorCopy)) continue;
      if (std::abs(m.faceAreaUV(f)) < 1e-12) continue;
      keep.push_back(f);
    }
  }
  if (!keep.empty()) {
    // area-weighted median of the island densities, so neither one badly scaled island nor many
    // small ones skew it; islands mostly outside 0..1 (tiled or parked UVs) are not part of the layout
    Mesh& mm = const_cast<Mesh&>(m);
    if (mm.loopIsland.size() != mm.loopCount()) mm.computeUVTopology();
    auto isl = facesByIsland(m, keep);
    std::vector<std::pair<float, double>> d;  // density, area weight
    double wsum = 0;
    for (auto& i : isl) {
      double a = 0, inside = 0, all = 0;
      for (uint32_t f : i) {
        a += m.faceArea3D(f);
        double u = std::abs(m.faceAreaUV(f));
        vec2 c(0);
        uint32_t n = m.faceStart[f + 1] - m.faceStart[f];
        for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) c += m.loopUV[l];
        c /= float(std::max(1u, n));
        c -= vec2(m.tiles.uvOrigin(m.faceTile(f)));  // inside its own tile (UDIM)
        all += u;
        if (c.x >= 0 && c.y >= 0 && c.x <= 1 && c.y <= 1) inside += u;
      }
      if (all <= 0 || inside < all * 0.5) continue;
      float v = areaDensity(m, i);
      if (v > 0) d.push_back({v, a}), wsum += a;
    }
    std::sort(d.begin(), d.end());
    double acc = 0;
    for (auto& [v, a] : d) {
      acc += a;
      if (acc >= wsum * 0.5) {
        t.layout = v;
        break;
      }
    }
  }
  return t;
}

// ============================================================ orientation

const char* orientName(Orient o) {
  return o == Orient::Upright ? "upright" : o == Orient::LongestAxis ? "longest_axis" : "none";
}
Orient orientFromName(const std::string& s) {
  if (s == "upright" || s == "up") return Orient::Upright;
  if (s == "longest_axis" || s == "longest" || s == "axis") return Orient::LongestAxis;
  return Orient::None;
}

static void orientLongest(Mesh& m, const std::vector<uint32_t>& loops) {
  // minimum-area bounding box over the hull directions (brute force over edges of the point set's hull)
  std::vector<dvec2> pts;
  for (uint32_t l : loops) pts.push_back(dvec2(m.loopUV[l]));
  if (pts.size() < 3) return;
  double bestA = 1e300, bestAng = 0, bw = 0, bh = 0;
  // candidate angles: 0..90 degrees in 1 degree steps, refined around the best
  auto eval = [&](double ang, double& w, double& h) {
    double c = std::cos(ang), s = std::sin(ang);
    double x0 = 1e300, x1 = -1e300, y0 = 1e300, y1 = -1e300;
    for (auto& p : pts) {
      double x = c * p.x - s * p.y, y = s * p.x + c * p.y;
      x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
    }
    w = x1 - x0, h = y1 - y0;
    return w * h;
  };
  for (int k = 0; k < 90; ++k) {
    double w, h, a = eval(k * kPiD / 180.0, w, h);
    if (a < bestA * (1 - 1e-9)) bestA = a, bestAng = k * kPiD / 180.0, bw = w, bh = h;
  }
  for (int k = -10; k <= 10; ++k) {
    double ang = bestAng + k * kPiD / 1800.0, w, h, a = eval(ang, w, h);
    if (a < bestA * (1 - 1e-9)) bestA = a, bestAng = ang, bw = w, bh = h;
  }
  if (bh > bw) bestAng += kPiD / 2;
  rotateLoops(m, loops, bestAng, uvCenter(m, loops));
}

void orientChart(Mesh& m, const std::vector<uint32_t>& chart, Orient o) {
  if (o == Orient::None || chart.empty()) return;
  auto loops = chartLoops(m, chart);
  if (o == Orient::LongestAxis) {
    orientLongest(m, loops);
    return;
  }
  // least-squares fit  y(u, v) = a u + b v + c  over the chart's corners; (a, b) is
  // the UV direction in which the surface rises
  double S[3][3] = {{0}}, R[3] = {0};
  double ymin = 1e300, ymax = -1e300, uvExtent = 0;
  Rect2 bb;
  for (uint32_t l : loops) {
    dvec2 uv(m.loopUV[l]);
    double y = m.positions[m.loopVert[l]].y;
    double v[3] = {uv.x, uv.y, 1.0};
    for (int i = 0; i < 3; ++i) {
      for (int j = 0; j < 3; ++j) S[i][j] += v[i] * v[j];
      R[i] += v[i] * y;
    }
    ymin = std::min(ymin, y), ymax = std::max(ymax, y);
    bb.add(m.loopUV[l]);
  }
  uvExtent = bb.valid() ? std::max(bb.size().x, bb.size().y) : 0.0;
  // solve the 3x3 system (Cramer)
  auto det3 = [](double M[3][3]) {
    return M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) - M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0]) +
           M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0]);
  };
  double D = det3(S);
  double sol[2] = {0, 0};
  if (std::abs(D) > 1e-30) {
    for (int k = 0; k < 2; ++k) {
      double M[3][3];
      for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) M[i][j] = j == k ? R[i] : S[i][j];
      sol[k] = det3(M) / D;
    }
  }
  dvec2 g(sol[0], sol[1]);
  // horizontal surfaces (the rise across the island is tiny compared with its size) have no up
  double rise = glm::length(g) * uvExtent;
  double size3 = 0;
  {
    AABB b;
    for (uint32_t l : loops) b.add(m.positions[m.loopVert[l]]);
    size3 = glm::length(b.size());
  }
  if (rise < 0.15 * size3 || !std::isfinite(rise)) {
    orientLongest(m, loops);
    return;
  }
  double ang = kPiD / 2 - std::atan2(g.y, g.x);
  rotateLoops(m, loops, ang, uvCenter(m, loops));
}

void scaleChartToDensity(Mesh& m, const std::vector<uint32_t>& chart, float uvDensity) {
  float d = areaDensity(m, chart);
  if (d <= 0 || uvDensity <= 0) return;
  double s = uvDensity / d;
  auto loops = chartLoops(m, chart);
  dvec2 c = uvCenter(m, loops);
  for (uint32_t l : loops) m.loopUV[l] = vec2(c + (dvec2(m.loopUV[l]) - c) * s);
}

// ============================================================ placement

bool placeInFreeSpace(Mesh& m, const std::vector<std::vector<uint32_t>>& charts, float margin, bool allowRotate,
                      int gridRes) {
  uint32_t F = m.faceCount();
  std::vector<uint8_t> moving(F, 0);
  for (auto& c : charts)
    for (uint32_t f : c)
      if (f < F) moving[f] = 1;
  Grid occ(gridRes);
  // mirror copies sit exactly on the faces they copy (and follow them), so they claim no space
  bool flags = m.faceFlags.size() == F;
  for (uint32_t f = 0; f < F; ++f)
    if (!moving[f] && !(flags && (m.faceFlags[f] & FaceMirrorCopy))) occ.addFace(m, f);
  int marginCells = std::max(1, (int)std::ceil(margin * gridRes));
  occ.dilate(marginCells);
  // largest first
  std::vector<size_t> order(charts.size());
  std::iota(order.begin(), order.end(), 0);
  std::vector<double> area(charts.size(), 0);
  for (size_t i = 0; i < charts.size(); ++i)
    for (uint32_t f : charts[i]) area[i] += std::abs(m.faceAreaUV(f));
  std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return area[a] > area[b]; });
  bool all = true;
  for (size_t ci : order) {
    const auto& chart = charts[ci];
    auto loops = chartLoops(m, chart);
    bool placed = false;
    for (int rot = 0; rot < (allowRotate ? 2 : 1) && !placed; ++rot) {
      std::vector<vec2> keep;
      for (uint32_t l : loops) keep.push_back(m.loopUV[l]);
      if (rot) rotateLoops(m, loops, kPiD / 2, uvCenter(m, loops));
      Rect2 bb;
      for (uint32_t l : loops) bb.add(m.loopUV[l]);
      // footprint relative to the bounding box corner, in grid cells
      int w = (int)std::ceil(bb.size().x * gridRes) + 1, h = (int)std::ceil(bb.size().y * gridRes) + 1;
      if (w > gridRes || h > gridRes || !bb.valid()) {
        for (size_t k = 0; k < loops.size(); ++k) m.loopUV[loops[k]] = keep[k];
        continue;
      }
      Grid fp(gridRes);
      dvec2 toOrigin = -dvec2(bb.lo) + dvec2(0.5 / gridRes);
      for (uint32_t f : chart) fp.addFace(m, f, toOrigin);
      std::vector<std::pair<int, int>> cells;
      for (int j = 0; j < h && j < gridRes; ++j)
        for (int i = 0; i < w && i < gridRes; ++i)
          if (fp.cell[size_t(j) * gridRes + i]) cells.push_back({i, j});
      SAT sat(occ);
      // bottom-left first: rows from the bottom, positions left to right; a free bounding
      // box is accepted at once, otherwise the exact footprint is tested
      int bx = -1, by = -1;
      for (int y = 0; y + h <= gridRes && bx < 0; ++y)
        for (int x = 0; x + w <= gridRes; ++x) {
          if (sat.sum(x, y, x + w - 1, y + h - 1) == 0) {
            bx = x, by = y;
            break;
          }
        }
      if (bx < 0) {
        for (int y = 0; y + h <= gridRes && bx < 0; y += 2)
          for (int x = 0; x + w <= gridRes; x += 2) {
            bool free = true;
            for (auto& [i, j] : cells)
              if (occ.cell[size_t(y + j) * gridRes + x + i]) {
                free = false;
                break;
              }
            if (free) {
              bx = x, by = y;
              break;
            }
          }
      }
      if (bx < 0) {
        for (size_t k = 0; k < loops.size(); ++k) m.loopUV[loops[k]] = keep[k];
        continue;
      }
      dvec2 d = dvec2(bx, by) / double(gridRes) - dvec2(bb.lo) + dvec2(0.5 / gridRes);
      for (uint32_t l : loops) m.loopUV[l] = vec2(dvec2(m.loopUV[l]) + d);
      // claim the space (with margin)
      Grid add(gridRes);
      for (uint32_t f : chart) add.addFace(m, f);
      add.dilate(marginCells);
      for (size_t k = 0; k < occ.cell.size(); ++k) occ.cell[k] |= add.cell[k];
      placed = true;
    }
    if (!placed) all = false;
  }
  return all;
}

// ============================================================ make island

IslandResult makeIsland(Mesh& m, const std::vector<uint32_t>& faces, const IslandOptions& opt) {
  IslandResult res;
  ensureTopo(m);
  if (m.faceCount() == 0) {
    res.message = "The mesh has no faces";
    return res;
  }
  auto sel = uniqueFaces(m, faces);
  uint32_t F = m.faceCount();
  std::vector<uint8_t> in(F, 0);
  for (uint32_t f : sel) in[f] = 1;
  // density target from the layout without the region itself
  float target = opt.uvDensity;
  if (target <= 0) {
    if (m.loopIsland.size() != m.loopCount()) m.computeUVTopology();
    target = densityTargets(m, opt.resolution, &in).recommended();
  }
  if (opt.markSeams) {
    EdgeFaces ef(m);
    for (uint32_t f : sel)
      for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) {
        uint32_t e = m.loopEdge[l];
        bool border = (ef.start[e + 1] - ef.start[e]) < 2;
        for (uint32_t k = ef.start[e]; k < ef.start[e + 1]; ++k)
          if (!in[ef.faces[k]]) border = true;
        if (border) m.edgeFlags[e] |= EdgeSeam;
        else m.edgeFlags[e] &= ~EdgeSeam;
      }
  }
  UnwrapOptions uo;
  uo.pack = false;
  unwrapLSCM(m, sel, uo);
  auto charts = chartsFromSeams(m, sel);
  for (auto& c : charts) {
    orientChart(m, c, opt.orient);
    scaleChartToDensity(m, c, target);
  }
  res.charts = (int)charts.size();
  res.uvDensity = target;
  bool fits = placeInFreeSpace(m, charts, opt.margin, opt.orient != Orient::Upright);
  if (!fits && opt.orient == Orient::Upright) fits = placeInFreeSpace(m, charts, opt.margin, true);
  if (!fits) {
    if (!opt.repackIfFull) {
      res.message = "The island does not fit into the free UV space";
      return res;
    }
    m.computeUVTopology();
    repackMovable(m, opt.margin, opt.orient != Orient::Upright);
    res.repacked = true;
    res.uvDensity = areaDensity(m, sel);
    res.message = "No free space at that density: all islands were repacked at an equal density";
  }
  res.ok = true;
  PLOG_I("Island: %zu faces, %d chart(s), density %.4f uv/unit%s", sel.size(), res.charts, res.uvDensity,
         res.repacked ? " (repacked)" : "");
  return res;
}

// ============================================================ overlaps

std::vector<std::vector<uint32_t>> overlappingIslands(const Mesh& mc, int res) {
  if (mc.tiles.active()) {  // texture tiles: islands overlap only within their tile
    Mesh c = mc;
    std::vector<std::vector<uint32_t>> all;
    forEachTile(c, {}, [&](const std::vector<uint32_t>&) {
      for (auto& isl : overlappingIslands(c, res)) all.push_back(isl);
    });
    return all;
  }
  Mesh& m = const_cast<Mesh&>(mc);
  if (m.loopIsland.size() != m.loopCount()) m.computeUVTopology();
  auto islands = facesByIsland(m);
  std::vector<uint32_t> faceIsland(m.faceCount(), 0xFFFFFFFFu);
  for (uint32_t i = 0; i < islands.size(); ++i)
    for (uint32_t f : islands[i]) faceIsland[f] = i;
  // owner per cell; conflicts between different islands
  std::vector<uint32_t> owner(size_t(res) * res, 0xFFFFFFFFu);
  std::map<std::pair<uint32_t, uint32_t>, int> conflicts;
  for (uint32_t i = 0; i < islands.size(); ++i)
    for (uint32_t f : islands[i])
      forFaceTris(m, f, [&](uint32_t a, uint32_t b, uint32_t c) {
        rasterTriCells(dvec2(m.loopUV[a]), dvec2(m.loopUV[b]), dvec2(m.loopUV[c]), res, [&](int x, int y) {
          uint32_t& o = owner[size_t(y) * res + x];
          if (o == 0xFFFFFFFFu) o = i;
          else if (o != i) conflicts[{std::min(o, i), std::max(o, i)}]++;
        });
      });
  std::vector<double> area(islands.size(), 0);
  for (uint32_t i = 0; i < islands.size(); ++i)
    for (uint32_t f : islands[i]) area[i] += std::abs(m.faceAreaUV(f));
  // the halves of a symmetric island are stacked on purpose: every face of the copy island
  // lies exactly on a face of the other one
  bool flags = m.faceFlags.size() == m.faceCount();
  auto copyIsland = [&](uint32_t i) {
    if (!flags) return false;
    for (uint32_t f : islands[i])
      if (!(m.faceFlags[f] & FaceMirrorCopy)) return false;
    return true;
  };
  auto pinnedIsland = [&](uint32_t i) {
    if (!flags) return false;
    for (uint32_t f : islands[i])
      if (m.faceFlags[f] & FacePinned) return true;
    return false;
  };
  // island a lies on island b: every corner of a is on a corner of b (the two halves may be
  // triangulated differently, so faces need not match) and it is no bigger than b
  const float q = 4096.f;
  auto key = [&](int x, int y) { return (uint64_t(uint32_t(x)) << 32) | uint32_t(y); };
  std::map<uint32_t, std::unordered_map<uint64_t, std::vector<vec2>>> cells;  // per island, built on demand
  auto onTopOf = [&](uint32_t a, uint32_t b) {
    if (!copyIsland(a)) return false;
    if (area[a] > area[b] * 1.02 + 1e-12) return false;  // the copy may cover only part of b
    auto& grid = cells[b];
    if (grid.empty())
      for (uint32_t f : islands[b])
        for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) {
          vec2 c = m.loopUV[l];
          grid[key((int)std::floor(c.x * q), (int)std::floor(c.y * q))].push_back(c);
        }
    for (uint32_t f : islands[a])
      for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) {
        vec2 c = m.loopUV[l];
        int cx = (int)std::floor(c.x * q), cy = (int)std::floor(c.y * q);
        bool found = false;
        for (int dy = -1; dy <= 1 && !found; ++dy)
          for (int dx = -1; dx <= 1 && !found; ++dx) {
            auto it = grid.find(key(cx + dx, cy + dy));
            if (it == grid.end()) continue;
            for (const vec2& p : it->second)
              if (glm::length(p - c) < 2e-5f) {
                found = true;
                break;
              }
          }
        if (!found) return false;
      }
    return true;
  };
  auto stacked = [&](uint32_t a, uint32_t b) { return onTopOf(a, b) || onTopOf(b, a); };
  std::vector<uint8_t> move(islands.size(), 0);
  for (auto& [p, n] : conflicts) {
    if (n < 2) continue;  // edge contacts between neighboring islands
    uint32_t a = p.first, b = p.second;
    if (move[a] || move[b]) continue;
    if (stacked(a, b)) continue;
    bool pa = pinnedIsland(a), pb = pinnedIsland(b);
    if (pa != pb) move[pa ? b : a] = 1;  // never the pinned one
    else move[area[a] < area[b] ? a : b] = 1;
  }
  std::vector<std::vector<uint32_t>> out;
  for (uint32_t i = 0; i < islands.size(); ++i)
    if (move[i]) out.push_back(islands[i]);
  return out;
}

// ============================================================ symmetry

static vec3 flipBy(vec3 p, int mask) {
  for (int a = 0; a < 3; ++a)
    if ((mask >> a) & 1) p[a] = -p[a];
  return p;
}

// ---- mirror partners by vertices
// A face's mirror image is found from its corners: each vertex is mirrored and matched to the
// nearest vertex within a fraction of its shortest edge, and the partner is the face made of the
// matched vertices. Vertices at the same position (split at UV seams or normals) count as one.
// That is exact on symmetric meshes, even where faces are long and thin or the surface curves
// sharply, where the face center's nearest surface point can belong to a neighboring face.
namespace {
struct MirrorCache {
  const Mesh* mesh = nullptr;
  uint64_t key = 0;
  std::vector<uint32_t> weld;                // vertex -> canonical vertex
  std::vector<uint32_t> vfStart, vfFaces;    // canonical vertex -> faces
  std::vector<float> reach;                  // canonical vertex -> match tolerance
  std::vector<uint32_t> rep;                 // canonical vertex -> one of its vertices
  std::vector<float> edge;                   // canonical vertex -> shortest edge at it
  std::vector<uint32_t> adjStart, adj;       // canonical vertex -> neighboring canonical vertices (sorted)
  std::vector<uint32_t> adjQStart, adjQ;     // the same without the diagonals of quads split in two
  std::unordered_map<uint64_t, std::vector<uint32_t>> edgeFaces;  // canonical edge (a<b) -> faces
  float sideEps = 1e-4f;
  std::vector<int> vmap[8];                  // per flip mask: canonical vertex -> mirrored canonical vertex
  bool built[8] = {};
};
std::mutex g_mirrorMutex;
MirrorCache g_mirror;

uint64_t meshKey(const Mesh& m) {
  uint64_t k = 1469598103934665603ull ^ (uint64_t(m.positions.size()) << 32) ^ m.faceCount();
  size_t step = std::max<size_t>(1, m.positions.size() / 211);
  for (size_t i = 0; i < m.positions.size(); i += step) {
    uint32_t b[3];
    std::memcpy(b, &m.positions[i], sizeof b);
    k = (k ^ b[0] ^ (uint64_t(b[1]) << 21) ^ (uint64_t(b[2]) << 42)) * 1099511628211ull;
  }
  return k;
}

void buildBase(MirrorCache& c, const Mesh& m) {
  size_t V = m.positions.size();
  c.weld.assign(V, 0);
  float q = std::max(m.boundingRadius, 1e-6f) * 1e-6f;
  std::unordered_map<uint64_t, uint32_t> ids;
  ids.reserve(V * 2);
  uint32_t n = 0;
  std::vector<uint32_t> rep;
  for (size_t v = 0; v < V; ++v) {
    const vec3& p = m.positions[v];
    int64_t x = (int64_t)std::llround(p.x / q), y = (int64_t)std::llround(p.y / q), z = (int64_t)std::llround(p.z / q);
    uint64_t k = (uint64_t(x) * 73856093ull) ^ (uint64_t(y) * 19349663ull) ^ (uint64_t(z) * 83492791ull);
    auto it = ids.find(k);
    if (it != ids.end() && glm::length(m.positions[rep[it->second]] - p) <= 2 * q) {
      c.weld[v] = it->second;
    } else {
      ids[k] = n;
      rep.push_back((uint32_t)v);
      c.weld[v] = n++;
    }
  }
  // faces per canonical vertex, and how far a mirrored vertex may land from its match
  std::vector<uint32_t> cnt(n + 1, 0);
  c.reach.assign(n, 1e30f);
  for (uint32_t f = 0; f < m.faceCount(); ++f)
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) {
      ++cnt[c.weld[m.loopVert[l]] + 1];
      uint32_t ln = (l + 1 < m.faceStart[f + 1]) ? l + 1 : m.faceStart[f];
      float e = glm::length(m.positions[m.loopVert[l]] - m.positions[m.loopVert[ln]]);
      if (e > 0) {
        float& a = c.reach[c.weld[m.loopVert[l]]];
        float& b = c.reach[c.weld[m.loopVert[ln]]];
        a = std::min(a, e), b = std::min(b, e);
      }
    }
  float floorTol = std::max(m.boundingRadius, 1e-6f) * 2e-5f;
  c.edge = c.reach;
  for (auto& e : c.edge) e = e < 1e29f ? e : floorTol * 10.f;
  for (auto& r : c.reach) r = std::max(r < 1e29f ? r * 0.3f : floorTol, floorTol);
  c.sideEps = std::max(m.boundingRadius, 1e-6f) * 1e-4f;
  {  // neighbors: all, and along "quad" edges. The diagonal of a quad split into two triangles (the
     // longest edge of both, nearly flat across) is left out of the second set, because the two
     // halves of a model are often split along different diagonals.
    c.edgeFaces.clear();
    c.edgeFaces.reserve(m.loopCount());
    for (uint32_t f = 0; f < m.faceCount(); ++f)
      for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) {
        uint32_t ln = (l + 1 < m.faceStart[f + 1]) ? l + 1 : m.faceStart[f];
        uint32_t a = c.weld[m.loopVert[l]], b = c.weld[m.loopVert[ln]];
        if (a == b) continue;
        auto& fl = c.edgeFaces[(uint64_t)std::min(a, b) << 32 | std::max(a, b)];
        if (fl.empty() || fl.back() != f) fl.push_back(f);
      }
    auto P = [&](uint32_t cv) { return m.positions[rep[cv]]; };
    auto longestEdge = [&](uint32_t f, uint32_t a, uint32_t b) {
      if (m.faceSize(f) != 3) return false;
      float L = glm::length(P(a) - P(b));
      for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) {
        uint32_t ln = (l + 1 < m.faceStart[f + 1]) ? l + 1 : m.faceStart[f];
        uint32_t x = c.weld[m.loopVert[l]], y = c.weld[m.loopVert[ln]];
        if ((x == a && y == b) || (x == b && y == a)) continue;
        if (glm::length(P(x) - P(y)) > L * 1.0001f) return false;
      }
      return true;
    };
    std::vector<std::vector<uint32_t>> nb(n), nq(n);
    for (auto& [key, fl] : c.edgeFaces) {
      uint32_t a = uint32_t(key >> 32), b = uint32_t(key & 0xffffffffu);
      nb[a].push_back(b);
      nb[b].push_back(a);
      bool diag = fl.size() == 2 && glm::dot(m.faceNormal(fl[0]), m.faceNormal(fl[1])) > 0.85f &&
                  longestEdge(fl[0], a, b) && longestEdge(fl[1], a, b);
      if (!diag) nq[a].push_back(b), nq[b].push_back(a);
    }
    auto pack = [&](std::vector<std::vector<uint32_t>>& lists, std::vector<uint32_t>& start, std::vector<uint32_t>& out) {
      start.assign(n + 1, 0);
      for (uint32_t i = 0; i < n; ++i) {
        std::sort(lists[i].begin(), lists[i].end());
        lists[i].erase(std::unique(lists[i].begin(), lists[i].end()), lists[i].end());
        start[i + 1] = start[i] + (uint32_t)lists[i].size();
      }
      out.clear();
      out.reserve(start[n]);
      for (auto& l : lists) out.insert(out.end(), l.begin(), l.end());
    };
    pack(nb, c.adjStart, c.adj);
    pack(nq, c.adjQStart, c.adjQ);
  }
  c.vfStart.assign(n + 1, 0);
  for (uint32_t i = 0; i < n; ++i) c.vfStart[i + 1] = c.vfStart[i] + cnt[i + 1];
  c.vfFaces.assign(c.vfStart[n], 0);
  std::vector<uint32_t> fill(c.vfStart.begin(), c.vfStart.end() - 1);
  for (uint32_t f = 0; f < m.faceCount(); ++f)
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) {
      uint32_t cv = c.weld[m.loopVert[l]];
      // a face once per vertex (a vertex can repeat in degenerate faces)
      if (fill[cv] > c.vfStart[cv] && c.vfFaces[fill[cv] - 1] == f) continue;
      c.vfFaces[fill[cv]++] = f;
    }
  for (uint32_t i = 0; i < n; ++i) c.vfStart[i + 1] = std::max(c.vfStart[i + 1], c.vfStart[i]);
  // compact (some slots may be unused after skipping repeats): rebuild exact ranges
  std::vector<uint32_t> start2(n + 1, 0), faces2;
  faces2.reserve(c.vfFaces.size());
  for (uint32_t i = 0; i < n; ++i) {
    start2[i] = (uint32_t)faces2.size();
    for (uint32_t k = c.vfStart[i]; k < fill[i]; ++k) faces2.push_back(c.vfFaces[k]);
  }
  start2[n] = (uint32_t)faces2.size();
  c.vfStart = std::move(start2);
  c.vfFaces = std::move(faces2);
  c.rep = std::move(rep);
}
}  // namespace


// canonical vertex -> its mirror image's canonical vertex (-1: none) for a flip mask.
// Vertices on the negative side of the (first) mirror plane look for partners on the positive side,
// vertices on the plane for partners on the plane.
// 1. exact partners (within a twentieth of the match tolerance), checked against their neighbors;
// 2. grown along the mesh from there: an unmatched vertex's image is predicted from its matched
//    neighbors (its mirrored position moved by how far each neighbor's image sits from that
//    neighbor's mirrored position) and the free neighbor of those images nearest the prediction
//    wins when it is clearly nearer than the runner-up;
// 3. for parts with no exact partner anywhere: near partners (a third of the shortest edge), checked
//    and grown the same way.
// Growing from exact partners keeps a half that is out of shape by a whole edge from matching the
// neighboring row of vertices; following quad edges (not the diagonals of split quads) keeps halves
// triangulated along different diagonals from disagreeing.
static const std::vector<int>& mirrorVertexMap(MirrorCache& c, const Mesh& m, const Bvh& bvh, int mask) {
  if (c.built[mask]) return c.vmap[mask];
  uint32_t n = (uint32_t)c.rep.size();
  std::vector<int>& map = c.vmap[mask];
  map.assign(n, -1);
  std::vector<float> md(n, 1e30f);
  std::vector<int> owner(n, -1), inv(n, -1);
  int axis = (mask & 1) ? 0 : (mask & 2) ? 1 : 2;
  auto P = [&](uint32_t i) { return m.positions[c.rep[i]]; };
  auto side = [&](uint32_t v) {
    float x = P(v)[axis];
    return x < -c.sideEps ? -1 : x > c.sideEps ? 1 : 0;
  };
  auto mirrored = [&](uint32_t i) { return flipBy(P(i), mask); };
  auto image = [&](uint32_t v) { return side(v) > 0 ? inv[v] : map[v]; };
  auto adjacentQ = [&](uint32_t a, uint32_t b) {
    return a == b || std::binary_search(c.adjQ.begin() + c.adjQStart[a], c.adjQ.begin() + c.adjQStart[a + 1], b);
  };
  std::vector<uint32_t> tris;
  auto seed = [&](float frac) {
    for (uint32_t i = 0; i < n; ++i) {
      int si = side(i);
      if (si > 0 || map[i] >= 0) continue;
      vec3 p = mirrored(i);
      float tol = c.reach[i] * frac;
      tris.clear();
      bvh.querySphere(p, tol, tris);
      float best = tol;
      int bw = -1;
      for (uint32_t t : tris) {
        if (t >= m.tris.size()) continue;
        for (int k = 0; k < 3; ++k) {
          uint32_t w = c.weld[m.loopVert[m.tris[t][k]]];
          int sw = side(w);
          if (si < 0 ? sw < 0 : sw != 0) continue;
          if (owner[w] >= 0) continue;
          float d = glm::length(P(w) - p);
          if (d <= best) best = d, bw = (int)w;
        }
      }
      if (bw >= 0) map[i] = bw, md[i] = best;
    }
  };
  auto rebuildOwners = [&] {
    std::fill(owner.begin(), owner.end(), -1);
    std::fill(inv.begin(), inv.end(), -1);
    for (uint32_t v = 0; v < n; ++v) {
      if (map[v] < 0) continue;
      int& o = owner[map[v]];
      if (o < 0) o = (int)v;
      else if (md[v] < md[o]) map[o] = -1, o = (int)v;
      else map[v] = -1;
    }
    for (uint32_t w = 0; w < n; ++w)
      if (owner[w] >= 0 && side(w) > 0) inv[w] = owner[w];
  };
  // matches most of whose matched neighbors disagree go, until it settles
  auto cleanup = [&] {
    for (int pass = 0; pass < 10; ++pass) {
      std::vector<uint32_t> drop;
      for (uint32_t v = 0; v < n; ++v) {
        if (map[v] < 0 || side(v) == 0) continue;
        int agree = 0, matched = 0;
        for (uint32_t k = c.adjQStart[v]; k < c.adjQStart[v + 1]; ++k) {
          int mn = image(c.adjQ[k]);
          if (mn < 0) continue;
          ++matched;
          if (adjacentQ((uint32_t)mn, (uint32_t)map[v])) ++agree;
        }
        if (matched >= 2 && agree * 2 < matched) drop.push_back(v);
      }
      if (drop.empty()) break;
      for (uint32_t v : drop) map[v] = -1;
      rebuildOwners();
    }
  };
  // (0.43) the spacing of the vertices round each one, for the tolerances below: its second shortest edge, so
  // the rim of a cloth with thickness (an edge a millimeter long to the layer under it) doesn't count
  std::vector<float> spacing(n), meanEdge(n);
  for (uint32_t v = 0; v < n; ++v) {
    float a = 1e30f, b = 1e30f, sum = 0.f;
    uint32_t k0 = c.adjQStart[v], k1 = c.adjQStart[v + 1];
    for (uint32_t k = k0; k < k1; ++k) {
      float d = glm::length(P(c.adjQ[k]) - P(v));
      sum += d;
      if (d < a) b = a, a = d;
      else if (d < b) b = d;
    }
    spacing[v] = b < 1e29f ? b : a < 1e29f ? a : c.edge[v];
    meanEdge[v] = k1 > k0 ? sum / float(k1 - k0) : c.edge[v];
  }
  // (0.43) on a model that isn't exactly symmetric the vertices down the middle wander off the
  // plane (millimeters on a cloth-simmed dress, whole panels shifted to one side): such a vertex is its
  // own mirror image, so it may match itself, on either side, less than half its average edge from the
  // plane, when the images of its neighbors are around it (the prediction from them decides, as for any
  // other match)
  auto nearMiddle = [&](uint32_t v) { return std::abs(P(v)[axis]) < 0.5f * meanEdge[v]; };
  auto unmatched = [&](uint32_t v) {
    if (map[v] >= 0) return false;
    int s = side(v);
    return s < 0 || (s > 0 && owner[v] < 0 && nearMiddle(v));
  };
  auto grow = [&] {
    std::vector<uint32_t> work;
    for (uint32_t v = 0; v < n; ++v)
      if (unmatched(v)) work.push_back(v);
    struct Add { uint32_t v, w; double score; };
    std::vector<Add> adds;
    std::vector<int> imgs;
    std::vector<std::pair<uint32_t, int>> cands;
    std::vector<uint8_t> queued(n, 0);
    for (int pass = 0; pass < 100000 && !work.empty(); ++pass) {
      adds.clear();
      for (uint32_t v : work) {
        if (!unmatched(v)) continue;
        vec3 pv = mirrored(v), pred(0.f);
        imgs.clear();
        for (uint32_t k = c.adjQStart[v]; k < c.adjQStart[v + 1]; ++k) {
          uint32_t nbv = c.adjQ[k];
          int mn = image(nbv);
          if (mn < 0) continue;
          pred += pv + P((uint32_t)mn) - mirrored(nbv);
          imgs.push_back(mn);
        }
        if (imgs.empty()) continue;
        pred /= float(imgs.size());
        cands.clear();
        for (int mn : imgs)
          for (uint32_t q = c.adjQStart[mn]; q < c.adjQStart[mn + 1]; ++q) {
            uint32_t w = c.adjQ[q];
            bool self = w == v && nearMiddle(v);  // (a vertex off the plane on the positive side: only itself)
            if (owner[w] >= 0 || ((side(w) < 0 || side(v) > 0) && !self)) continue;
            bool found = false;
            for (auto& cw : cands)
              if (cw.first == w) ++cw.second, found = true;
            if (!found) cands.push_back({w, 1});
          }
        int bestW = -1, bestA = 0;
        float bestD = 1e30f, secondD = 1e30f;
        for (auto [w, a] : cands) {
          float d = glm::length(P(w) - pred);
          if (a > bestA || (a == bestA && d < bestD)) {
            secondD = (a == bestA) ? bestD : 1e30f;
            bestA = a, bestD = d, bestW = (int)w;
          } else if (a == bestA && d < secondD) {
            secondD = d;
          }
        }
        if (bestW < 0) continue;
        float e = std::min(spacing[v], spacing[bestW]);
        // one matched neighbor says little: the prediction must then be close and unambiguous
        float tight = bestA >= 2 ? 0.6f : 0.35f;
        if (bestD > tight * e * 2.f || secondD < bestD * 1.8f) continue;
        adds.push_back({v, (uint32_t)bestW, bestA * 1e6 - bestD});
      }
      if (adds.empty()) break;
      std::sort(adds.begin(), adds.end(), [](const Add& a, const Add& b) { return a.score > b.score; });
      // the most certain half of each pass goes in first, so the rest see better predictions
      size_t take = std::max<size_t>(1, (adds.size() + 1) / 2), done = 0;
      std::vector<uint32_t> next;
      auto queue = [&](uint32_t x) {
        if (unmatched(x) && !queued[x]) queued[x] = 1, next.push_back(x);
      };
      for (const Add& a : adds) {
        if (done >= take) {
          queue(a.v);
          continue;
        }
        if (owner[a.w] >= 0 || map[a.v] >= 0) continue;
        owner[a.w] = (int)a.v, map[a.v] = (int)a.w, md[a.v] = 0.f;
        if (side(a.w) > 0) inv[a.w] = (int)a.v;
        ++done;
        for (uint32_t k = c.adjQStart[a.v]; k < c.adjQStart[a.v + 1]; ++k) queue(c.adjQ[k]);
        for (uint32_t k = c.adjQStart[a.w]; k < c.adjQStart[a.w + 1]; ++k) {
          uint32_t y = c.adjQ[k];
          queue(y);
          for (uint32_t q = c.adjQStart[y]; q < c.adjQStart[y + 1]; ++q) queue(c.adjQ[q]);
        }
      }
      for (uint32_t x : next) queued[x] = 0;
      work = std::move(next);
    }
  };
  size_t exact = 0;
  for (float frac : {0.05f, 1.f}) {
    seed(frac);
    rebuildOwners();
    cleanup();
    if (frac < 1.f)
      for (int x : map) exact += x >= 0;
    grow();
  }
  // both ways
  for (uint32_t v = 0; v < n; ++v)
    if (side(v) < 0 && map[v] >= 0 && side((uint32_t)map[v]) > 0) map[map[v]] = (int)v;
  size_t neg = 0, matched = 0;
  for (uint32_t v = 0; v < n; ++v)
    if (side(v) <= 0) ++neg, matched += map[v] >= 0;
  if (matched > exact)
    PLOG_I("Mirror matching: %zu of %zu vertices exactly, %zu more along the mesh, %zu without a partner", exact, neg,
           matched - exact, neg - matched);
  c.built[mask] = true;
  return map;
}

// the cache for this geometry (call with g_mirrorMutex held)
static MirrorCache& mirrorCache(const Mesh& m) {
  MirrorCache& c = g_mirror;
  uint64_t k = meshKey(m);
  if (c.key != k || c.weld.size() != m.positions.size()) {  // keyed on the geometry: working copies share it
    c = MirrorCache();
    c.mesh = &m;
    c.key = k;
    buildBase(c, m);
  }
  return c;
}

// the canonical vertices of a face (sorted, unique)
static std::vector<uint32_t> canonVerts(const MirrorCache& c, const Mesh& m, uint32_t f) {
  std::vector<uint32_t> v;
  for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) v.push_back(c.weld[m.loopVert[l]]);
  std::sort(v.begin(), v.end());
  v.erase(std::unique(v.begin(), v.end()), v.end());
  return v;
}

std::vector<int> mirrorVertices(const Mesh& m, const Bvh& bvh, int flipMask) {
  std::vector<int> out(m.positions.size(), -1);
  flipMask &= 7;
  if (!flipMask || bvh.empty()) return out;
  std::lock_guard<std::mutex> lock(g_mirrorMutex);
  MirrorCache& c = mirrorCache(m);
  const std::vector<int>& map = mirrorVertexMap(c, m, bvh, flipMask);
  for (size_t v = 0; v < out.size(); ++v) {
    int mv = map[c.weld[v]];
    if (mv >= 0) out[v] = (int)c.rep[mv];
  }
  return out;
}

int mirrorFace(const Mesh& m, const Bvh& bvh, uint32_t f, int flipMask) {
  if (f >= m.faceCount() || bvh.empty()) return -1;
  flipMask &= 7;
  if (!flipMask) return (int)f;
  {
    std::lock_guard<std::mutex> lock(g_mirrorMutex);
    MirrorCache& c = mirrorCache(m);
    const std::vector<int>& map = mirrorVertexMap(c, m, bvh, flipMask);
    // the mirrored corners, as canonical vertices
    uint32_t sz = m.faceSize(f);
    std::vector<uint32_t> want;
    want.reserve(sz);
    bool all = true;
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) {
      int mv = map[c.weld[m.loopVert[l]]];
      if (mv < 0) {
        all = false;
        break;
      }
      want.push_back((uint32_t)mv);
    }
    if (all && !want.empty()) {
      std::sort(want.begin(), want.end());
      want.erase(std::unique(want.begin(), want.end()), want.end());
      uint32_t v0 = want[0];
      std::vector<uint32_t> have;
      for (uint32_t k2 = c.vfStart[v0]; k2 < c.vfStart[v0 + 1]; ++k2) {
        uint32_t g = c.vfFaces[k2];
        if (m.faceSize(g) != sz) continue;
        have.clear();
        for (uint32_t l = m.faceStart[g]; l < m.faceStart[g + 1]; ++l) have.push_back(c.weld[m.loopVert[l]]);
        std::sort(have.begin(), have.end());
        have.erase(std::unique(have.begin(), have.end()), have.end());
        if (have == want) return (int)g;
      }
    }
  }
  // no exact match (the mesh is not quite symmetric here): the face whose center is nearest the
  // mirrored center, among the hit face and its neighbors
  vec3 c = flipBy(m.faceCenter(f), flipMask);
  // tolerance: a fraction of the face's own size
  float size = 0.f;
  for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l)
    size = std::max(size, glm::length(m.positions[m.loopVert[l]] - m.faceCenter(f)));
  float tol = std::max(size * 0.35f, m.boundingRadius * 1e-5f);
  std::vector<uint32_t> tris;
  bvh.querySphere(c, tol, tris);
  int best = -1;
  float bd = tol;
  for (uint32_t tr : tris) {
    if (tr >= m.triFace.size()) continue;
    uint32_t g = m.triFace[tr];
    if (m.faceSize(g) != m.faceSize(f)) continue;
    float d = glm::length(m.faceCenter(g) - c);
    if (d <= bd) bd = d, best = (int)g;
  }
  return best;
}

std::vector<uint32_t> mirrorFaces(const Mesh& m, const Bvh& bvh, uint32_t f, int flipMask) {
  if (f >= m.faceCount() || bvh.empty()) return {};
  flipMask &= 7;
  if (!flipMask) return {f};
  {
    // the exact partner (same corners, mirrored)
    std::lock_guard<std::mutex> lock(g_mirrorMutex);
    MirrorCache& c = mirrorCache(m);
    const std::vector<int>& map = mirrorVertexMap(c, m, bvh, flipMask);
    std::vector<uint32_t> fv = canonVerts(c, m, f), W;
    bool all = true;
    for (uint32_t v : fv) {
      int mv = map[v];
      if (mv < 0) all = false;
      else W.push_back((uint32_t)mv);
    }
    std::sort(W.begin(), W.end());
    W.erase(std::unique(W.begin(), W.end()), W.end());
    if (all)
      for (uint32_t k = c.vfStart[W[0]]; k < c.vfStart[W[0] + 1]; ++k)
        if (canonVerts(c, m, c.vfFaces[k]) == W) return {c.vfFaces[k]};
    if (W.size() >= 2) {
      // the other side is triangulated differently: f is half of a quad split along one diagonal
      // and the image of the quad is split along the other. The image faces share two of f's
      // mirrored corners and have all their corners on the mirrored quad.
      {
        std::vector<uint32_t> quad = fv;
        for (size_t k = 0; k < fv.size(); ++k)
          for (size_t j = k + 1; j < fv.size(); ++j) {
            uint32_t a = fv[k], b = fv[j];
            if (std::binary_search(c.adjQ.begin() + c.adjQStart[a], c.adjQ.begin() + c.adjQStart[a + 1], b)) continue;
            auto it = c.edgeFaces.find((uint64_t)std::min(a, b) << 32 | std::max(a, b));
            if (it == c.edgeFaces.end()) continue;  // not an edge
            for (uint32_t g : it->second)
              for (uint32_t x : canonVerts(c, m, g)) quad.push_back(x);
          }
        std::vector<uint32_t> M;
        for (uint32_t v : quad)
          if (map[v] >= 0) M.push_back((uint32_t)map[v]);
        std::sort(M.begin(), M.end());
        M.erase(std::unique(M.begin(), M.end()), M.end());
        std::vector<uint32_t> out, seen;
        for (uint32_t w : W)
          for (uint32_t k = c.vfStart[w]; k < c.vfStart[w + 1]; ++k) {
            uint32_t g = c.vfFaces[k];
            if (std::find(seen.begin(), seen.end(), g) != seen.end()) continue;
            seen.push_back(g);
            int shared = 0;
            bool inside = true;
            for (uint32_t x : canonVerts(c, m, g)) {
              shared += std::binary_search(W.begin(), W.end(), x) ? 1 : 0;
              if (!std::binary_search(M.begin(), M.end(), x)) inside = false;
            }
            if (shared >= 2 && inside) out.push_back(g);
          }
        if (!out.empty()) return out;
      }
      // no quad found (a strongly bent one, or a fan): the faces there that share an edge's worth
      // of corners with the mirrored face and whose mirrored center is nearer this face than any
      // of its neighbors
      std::vector<uint32_t> near = {f};  // this face and its edge neighbors
      for (uint32_t v : fv)
        for (uint32_t k = c.vfStart[v]; k < c.vfStart[v + 1]; ++k) {
          uint32_t g = c.vfFaces[k];
          if (g == f || std::find(near.begin(), near.end(), g) != near.end()) continue;
          auto gv = canonVerts(c, m, g);
          int shared = 0;
          for (uint32_t x : gv) shared += std::binary_search(fv.begin(), fv.end(), x) ? 1 : 0;
          if (shared >= 2) near.push_back(g);
        }
      std::vector<uint32_t> out, seen;
      for (uint32_t w : W)
        for (uint32_t k = c.vfStart[w]; k < c.vfStart[w + 1]; ++k) {
          uint32_t g = c.vfFaces[k];
          if (std::find(seen.begin(), seen.end(), g) != seen.end()) continue;
          seen.push_back(g);
          auto gv = canonVerts(c, m, g);
          int shared = 0;
          for (uint32_t x : gv) shared += std::binary_search(W.begin(), W.end(), x) ? 1 : 0;
          if (shared < 2) continue;
          vec3 mc = flipBy(m.faceCenter(g), flipMask);
          float df = glm::length(m.faceCenter(f) - mc), best = 1e30f;
          for (uint32_t n : near) best = std::min(best, glm::length(m.faceCenter(n) - mc));
          if (df <= best * 1.001f + 1e-12f) out.push_back(g);
        }
      if (!out.empty()) return out;
    }
  }
  int g = mirrorFace(m, bvh, f, flipMask);
  if (g >= 0) return {(uint32_t)g};
  return {};
}

// For each corner of face f: the corner it mirrors onto (flipMask), on a face `accept` allows.
// Corners are matched by vertex, so the faces on the other side may be triangulated differently;
// where several faces share the vertex, the one nearest the mirrored face's center wins (the
// right side of a seam). False if some corner has no match.
static bool mirrorCorners(const Mesh& m, const Bvh& bvh, uint32_t f, int flipMask,
                          const std::function<bool(uint32_t)>& accept, std::vector<uint32_t>& out,
                          std::initializer_list<const std::vector<vec2>*> preferUVs = {}) {
  out.clear();
  if (f >= m.faceCount() || bvh.empty() || !(flipMask & 7)) return false;
  std::lock_guard<std::mutex> lock(g_mirrorMutex);
  MirrorCache& c = mirrorCache(m);
  const std::vector<int>& map = mirrorVertexMap(c, m, bvh, flipMask & 7);
  vec3 mc = flipBy(m.faceCenter(f), flipMask);
  for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) {
    int mv = map[c.weld[m.loopVert[l]]];
    if (mv < 0) return false;
    // several faces meet at the vertex: the one whose corner already has this corner's UV wins
    // (checked in each preferred layout in turn, so a copy stays on the half it was stacked on),
    // then the one nearest the mirrored center (the right side of a seam)
    int best = -1;
    double bd = 1e300;
    for (uint32_t k = c.vfStart[mv]; k < c.vfStart[mv + 1]; ++k) {
      uint32_t g = c.vfFaces[k];
      if (g == f || !accept(g)) continue;
      for (uint32_t q = m.faceStart[g]; q < m.faceStart[g + 1]; ++q)
        if (c.weld[m.loopVert[q]] == (uint32_t)mv) {
          int rank = (int)preferUVs.size();
          int i = 0;
          for (const std::vector<vec2>* uv : preferUVs) {
            if (uv && uv->size() == m.loopCount() && glm::length((*uv)[l] - (*uv)[q]) < 1e-6f) {
              rank = i;
              break;
            }
            ++i;
          }
          double d = rank * 1e9 + glm::length(m.faceCenter(g) - mc);
          if (d < bd) bd = d, best = (int)q;
          break;
        }
    }
    if (best < 0) return false;
    out.push_back((uint32_t)best);
  }
  return true;
}

std::vector<uint32_t> withMirroredFaces(const Mesh& m, const Bvh& bvh, const std::vector<uint32_t>& faces, int axisMask) {
  uint32_t F = m.faceCount();
  std::vector<uint8_t> in(F, 0);
  std::vector<uint32_t> todo;
  for (uint32_t f : faces)
    if (f < F && !in[f]) in[f] = 1, todo.push_back(f);
  auto addWithMirrors = [&](uint32_t f) {
    std::vector<uint32_t> q = {f};
    if (!in[f]) in[f] = 1;
    for (size_t k = 0; k < q.size(); ++k)
      for (int sub = 1; sub < 8; ++sub) {
        if ((sub & axisMask) != sub) continue;
        for (uint32_t g : mirrorFaces(m, bvh, q[k], sub))
          if (!in[g]) in[g] = 1, q.push_back(g);
      }
  };
  for (uint32_t f : todo) addWithMirrors(f);
  // where the halves are triangulated differently, a face's mirror image can straddle a selected
  // and an unselected face: the unselected one (and its own mirror image) join, so that every
  // face on the negative side has all of its mirrored corners on selected faces
  float eps = m.boundingRadius * 1e-4f;
  auto negMask = [&](uint32_t f) {
    vec3 c = m.faceCenter(f);
    int neg = 0;
    for (int a = 0; a < 3; ++a)
      if (((axisMask >> a) & 1) && c[a] < -eps) neg |= 1 << a;
    return neg;
  };
  if (!bvh.empty() && (axisMask & 7))
    for (int pass = 0; pass < 4; ++pass) {
      std::vector<uint32_t> add;
      {
        std::lock_guard<std::mutex> lock(g_mirrorMutex);
        MirrorCache& c = mirrorCache(m);
        for (uint32_t f = 0; f < F; ++f) {
          if (!in[f]) continue;
          int neg = negMask(f);
          if (!neg) continue;
          const std::vector<int>& map = mirrorVertexMap(c, m, bvh, neg);
          vec3 mc = flipBy(m.faceCenter(f), neg);
          std::vector<int> mvs;
          for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) mvs.push_back(map[c.weld[m.loopVert[l]]]);
          for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) {
            int mv = map[c.weld[m.loopVert[l]]];
            if (mv < 0) continue;  // not symmetric here: nothing to complete
            // the face holding the most of f's mirrored corners (the other half of the split
            // quad), then one sharing an edge with the selection, then the nearest
            bool covered = false;
            int best = -1;
            float bs = -1e30f;
            for (uint32_t k = c.vfStart[mv]; k < c.vfStart[mv + 1]; ++k) {
              uint32_t g = c.vfFaces[k];
              if (negMask(g)) continue;
              if (in[g]) {
                covered = true;
                break;
              }
              auto gv = canonVerts(c, m, g);
              int held = 0, touching = 0;
              for (uint32_t x : gv)
                if (std::find(mvs.begin(), mvs.end(), (int)x) != mvs.end()) ++held;
              for (uint32_t q = m.faceStart[g]; q < m.faceStart[g + 1] && !touching; ++q) {
                uint32_t qn = (q + 1 < m.faceStart[g + 1]) ? q + 1 : m.faceStart[g];
                uint32_t a = c.weld[m.loopVert[q]], b = c.weld[m.loopVert[qn]];
                auto it = c.edgeFaces.find((uint64_t)std::min(a, b) << 32 | std::max(a, b));
                if (it != c.edgeFaces.end())
                  for (uint32_t h : it->second)
                    if (h != g && in[h]) touching = 1;
              }
              float score = held * 10.f + touching * 5.f - glm::length(m.faceCenter(g) - mc) / std::max(m.boundingRadius, 1e-6f);
              if (score > bs) bs = score, best = (int)g;
            }
            if (!covered && best >= 0) add.push_back((uint32_t)best);
          }
        }
      }
      if (add.empty()) break;
      for (uint32_t g : add) addWithMirrors(g);
    }
  std::vector<uint32_t> out;
  for (uint32_t f = 0; f < F; ++f)
    if (in[f]) out.push_back(f);
  return out;
}

IslandResult makeSymmetricIsland(Mesh& m, const Bvh& bvh, const std::vector<uint32_t>& faces, int axisMask,
                                 const IslandOptions& opt, int* stackedOut) {
  ensureTopo(m);
  auto sel = uniqueFaces(m, faces);
  if (!(axisMask & 7) || bvh.empty()) return makeIsland(m, sel, opt);
  uint32_t F = m.faceCount();
  std::vector<uint8_t> in(F, 0);
  for (uint32_t f : sel) in[f] = 1;
  float eps = m.boundingRadius * 1e-4f;
  // which side of each mirror plane a face is on: faces on a negative side whose corners all
  // mirror onto corners of selected faces on the positive side are copies
  auto negMask = [&](uint32_t f) {
    vec3 c = m.faceCenter(f);
    int neg = 0;
    for (int a = 0; a < 3; ++a)
      if (((axisMask >> a) & 1) && c[a] < -eps) neg |= 1 << a;
    return neg;
  };
  auto positiveSel = [&](uint32_t g) { return in[g] && negMask(g) == 0; };
  std::vector<std::vector<uint32_t>> source(F);
  std::vector<uint32_t> primary, copies;
  for (uint32_t f : sel) {
    int neg = negMask(f);
    if (neg && mirrorCorners(m, bvh, f, neg, positiveSel, source[f])) {
      copies.push_back(f);
      continue;
    }
    primary.push_back(f);
  }
  if (copies.empty()) return makeIsland(m, sel, opt);
  // seams around the copies (the primary region gets its own in makeIsland)
  std::vector<uint8_t> isCopy(F, 0);
  for (uint32_t f : copies) isCopy[f] = 1;
  {
    EdgeFaces ef(m);
    for (uint32_t f : copies)
      for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) {
        uint32_t e = m.loopEdge[l];
        bool border = (ef.start[e + 1] - ef.start[e]) < 2;
        for (uint32_t k = ef.start[e]; k < ef.start[e + 1]; ++k)
          if (!isCopy[ef.faces[k]]) border = true;
        if (border) m.edgeFlags[e] |= EdgeSeam;
        else m.edgeFlags[e] &= ~EdgeSeam;
      }
  }
  // the copies' old UVs must not take up free space while the primary half is placed
  for (uint32_t f : copies)
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) m.loopUV[l] = vec2(0.f);
  m.computeUVTopology();
  IslandResult res = makeIsland(m, primary, opt);
  if (!res.ok) return res;
  // each copy corner takes the UV of the corner it mirrors onto
  for (uint32_t f : copies)
    for (uint32_t k = 0; k < source[f].size(); ++k) m.loopUV[m.faceStart[f] + k] = m.loopUV[source[f][k]];
  if (m.faceFlags.size() != F) m.faceFlags.assign(F, 0);
  for (uint32_t f : sel) m.faceFlags[f] &= ~kFaceMirrorBits;
  for (uint32_t f : copies) m.faceFlags[f] |= FaceMirrorCopy | uint8_t(negMask(f) << 1);
  if (stackedOut) *stackedOut = (int)copies.size();
  char buf[160];
  snprintf(buf, sizeof buf, "%zu mirrored face%s share the UVs of the other half", copies.size(),
           copies.size() == 1 ? "" : "s");
  if (res.message.empty()) res.message = buf;
  else res.message += std::string("; ") + buf;
  PLOG_I("Symmetric island: %zu faces unwrapped, %zu stacked onto them", primary.size(), copies.size());
  return res;
}

int mirrorUVs(Mesh& m, const Bvh& bvh, int axisMask, const std::vector<uint32_t>& faces) {
  ensureTopo(m);
  uint32_t F = m.faceCount();
  if (!(axisMask & 7) || bvh.empty()) return 0;
  std::vector<uint8_t> want(F, faces.empty() ? 1 : 0);
  for (uint32_t f : faces)
    if (f < F) want[f] = 1;
  float eps = m.boundingRadius * 1e-4f;
  if (m.faceFlags.size() != F) m.faceFlags.assign(F, 0);
  std::vector<uint8_t> copied(F, 0);
  auto negMask = [&](uint32_t f) {
    vec3 c = m.faceCenter(f);
    int neg = 0;
    for (int a = 0; a < 3; ++a)
      if (((axisMask >> a) & 1) && c[a] < -eps) neg |= 1 << a;
    return neg;
  };
  auto positive = [&](uint32_t g) { return negMask(g) == 0; };
  std::vector<uint32_t> src;
  auto loopFace = m.loopFace();
  int n = 0;
  for (uint32_t f = 0; f < F; ++f) {
    int neg = negMask(f);
    if (!neg || isPinned(m, f)) continue;
    if (!mirrorCorners(m, bvh, f, neg, positive, src)) continue;
    bool wanted = want[f] != 0;
    for (uint32_t s : src) wanted |= want[loopFace[s]] != 0;
    if (!wanted) continue;
    for (uint32_t k = 0; k < src.size(); ++k) m.loopUV[m.faceStart[f] + k] = m.loopUV[src[k]];
    m.faceFlags[f] = uint8_t((m.faceFlags[f] & ~kFaceMirrorBits) | FaceMirrorCopy | (neg << 1));
    copied[f] = 1;
    ++n;
  }
  if (!n) return 0;
  // seams where a copied face meets one that was not copied (the mirror plane, mostly)
  EdgeFaces ef(m);
  for (uint32_t e = 0; e < m.edges.size(); ++e) {
    bool a = false, b = false;
    for (uint32_t k = ef.start[e]; k < ef.start[e + 1]; ++k) (copied[ef.faces[k]] ? a : b) = true;
    if (a && b) m.edgeFlags[e] |= EdgeSeam;
  }
  PLOG_I("Mirrored UVs: %d faces take the UVs of their mirror image", n);
  return n;
}

// ============================================================ rectify

RectifyResult rectifyQuadIslands(Mesh& m, const std::vector<uint32_t>& faces, float margin) {
  ensureTopo(m);
  m.computeUVTopology();
  RectifyResult res;
  auto islands = movableIslands(m, faces);
  EdgeFaces ef(m);
  std::vector<std::vector<uint32_t>> moved;
  for (auto& isl : islands) {
    bool quads = !isl.empty();
    for (uint32_t f : isl) quads &= m.faceSize(f) == 4;
    if (!quads) {
      ++res.skipped;
      continue;
    }
    std::vector<uint8_t> in(m.faceCount(), 0);
    for (uint32_t f : isl) in[f] = 1;
    std::vector<vec2> before;  // to restore a failed island
    auto loops = loopsOfFaces(m, isl);
    for (uint32_t l : loops) before.push_back(m.loopUV[l]);
    float density = areaDensity(m, isl);
    Rect2 box0 = boundsOfLoops(m, loops);
    // integer grid coordinates per UV vertex (loopUVVert), quad by quad from the largest; a
    // quad whose corners disagree with the grid (a pole, a spiral) means it isn't a grid
    std::unordered_map<uint32_t, glm::ivec2> grid;
    std::vector<uint8_t> placed(m.faceCount(), 0);
    uint32_t start = isl[0];
    for (uint32_t f : isl)
      if (m.faceArea3D(f) > m.faceArea3D(start)) start = f;
    auto P = [&](uint32_t l) { return m.positions[m.loopVert[l]]; };
    {
      uint32_t s0 = m.faceStart[start];
      const glm::ivec2 c[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
      for (int k = 0; k < 4; ++k) grid[m.loopUVVert[s0 + k]] = c[k];
    }
    placed[start] = 1;
    std::vector<uint32_t> queue = {start};
    bool clean = true;
    for (size_t qi = 0; qi < queue.size() && clean; ++qi) {
      uint32_t f = queue[qi];
      uint32_t s0 = m.faceStart[f];
      for (int k = 0; k < 4 && clean; ++k) {
        uint32_t e = m.loopEdge[s0 + k];
        if (m.edgeFlags[e] & EdgeSeam) continue;
        for (uint32_t t = ef.start[e]; t < ef.start[e + 1]; ++t) {
          uint32_t g = ef.faces[t];
          if (g == f || !in[g] || placed[g]) continue;
          uint32_t gs = m.faceStart[g];
          int j = -1;
          for (int q = 0; q < 4; ++q)
            if (m.loopEdge[gs + q] == e) j = q;
          if (j < 0) continue;
          uint32_t la = gs + j, lb = gs + (j + 1) % 4, lc = gs + (j + 2) % 4, ld = gs + (j + 3) % 4;
          auto ia = grid.find(m.loopUVVert[la]), ib = grid.find(m.loopUVVert[lb]);
          if (ia == grid.end() || ib == grid.end()) continue;
          glm::ivec2 A = ia->second, B = ib->second, d = B - A;
          if (std::abs(d.x) + std::abs(d.y) != 1) {
            clean = false;
            break;
          }
          // the side of the edge away from f
          glm::ivec2 perp(-d.y, d.x);
          int fsum = 0;
          for (int q = 0; q < 4; ++q)
            if (auto it = grid.find(m.loopUVVert[s0 + q]); it != grid.end()) {
              glm::ivec2 rel = it->second - A;
              fsum += rel.x * perp.x + rel.y * perp.y;
            }
          if (fsum > 0) perp = -perp;
          for (auto [lp, v] : {std::pair{lc, B + perp}, std::pair{ld, A + perp}}) {
            uint32_t uv = m.loopUVVert[lp];
            auto it = grid.find(uv);
            if (it == grid.end()) grid[uv] = v;
            else if (it->second != v) clean = false;
          }
          placed[g] = 1;
          queue.push_back(g);
        }
      }
    }
    for (uint32_t f : isl) clean &= placed[f] != 0;
    if (!clean) {
      ++res.skipped;
      continue;
    }
    // column widths and row heights: the average 3D length of the edges between them
    glm::ivec2 lo(INT32_MAX), hi(INT32_MIN);
    for (auto& [k, v] : grid) lo = glm::min(lo, v), hi = glm::max(hi, v);
    std::vector<double> wsum(hi.x - lo.x + 1, 0), wn(hi.x - lo.x + 1, 0), hsum(hi.y - lo.y + 1, 0), hn(hi.y - lo.y + 1, 0);
    for (uint32_t f : isl) {
      uint32_t s0 = m.faceStart[f];
      for (int k = 0; k < 4; ++k) {
        uint32_t a = s0 + k, b = s0 + (k + 1) % 4;
        glm::ivec2 ga = grid[m.loopUVVert[a]] - lo, gb = grid[m.loopUVVert[b]] - lo;
        double l3 = glm::length(P(a) - P(b));
        if (ga.y == gb.y) {
          int c = std::min(ga.x, gb.x);
          wsum[c] += l3, wn[c] += 1;
        } else {
          int r0 = std::min(ga.y, gb.y);
          hsum[r0] += l3, hn[r0] += 1;
        }
      }
    }
    std::vector<double> xs(wsum.size() + 1, 0), ys(hsum.size() + 1, 0);
    for (size_t i = 0; i < wsum.size(); ++i) xs[i + 1] = xs[i] + (wn[i] > 0 ? wsum[i] / wn[i] : 0);
    for (size_t i = 0; i < hsum.size(); ++i) ys[i + 1] = ys[i] + (hn[i] > 0 ? hsum[i] / hn[i] : 0);
    std::unordered_map<uint32_t, dvec2> pos;
    for (auto& [k, v] : grid) pos[k] = dvec2(xs[v.x - lo.x], ys[v.y - lo.y]);
    for (uint32_t l : loops) m.loopUV[l] = vec2(pos[m.loopUVVert[l]]);
    // a mirrored layout would flip the texture: keep the original winding
    double area0 = 0, area1 = 0;
    {
      size_t k = 0;
      std::vector<vec2> now;
      for (uint32_t l : loops) now.push_back(m.loopUV[l]);
      for (uint32_t l : loops) m.loopUV[l] = before[k++];
      for (uint32_t f : isl) area0 += m.faceAreaUV(f);
      k = 0;
      for (uint32_t l : loops) m.loopUV[l] = now[k++];
      for (uint32_t f : isl) area1 += m.faceAreaUV(f);
    }
    if ((area0 < 0) != (area1 < 0)) flip(m, loops, true);
    scaleChartToDensity(m, isl, density > 0 ? density : 1.f);
    Rect2 box1 = boundsOfLoops(m, loops);
    translate(m, loops, box0.center() - box1.center());
    // stay inside 0..1: smaller if it no longer fits, then pushed back in
    {
      Rect2 b = boundsOfLoops(m, loops);
      vec2 sz = b.hi - b.lo;
      float room = 1.f - 2.f * margin;
      float k = std::min(1.f, room / std::max(std::max(sz.x, sz.y), 1e-9f));
      if (k < 1.f) scale(m, loops, vec2(k), b.center());
      b = boundsOfLoops(m, loops);
      vec2 d(0.f);
      if (b.lo.x < margin) d.x = margin - b.lo.x;
      else if (b.hi.x > 1.f - margin) d.x = 1.f - margin - b.hi.x;
      if (b.lo.y < margin) d.y = margin - b.lo.y;
      else if (b.hi.y > 1.f - margin) d.y = 1.f - margin - b.hi.y;
      translate(m, loops, d);
    }
    moved.push_back(isl);
    ++res.islands;
  }
  m.computeUVTopology();
  // islands that grew into others move to free space
  if (!moved.empty()) {
    auto ov = overlappingIslands(m);
    std::vector<std::vector<uint32_t>> fix;
    for (auto& o : ov)
      for (auto& mv : moved)
        if (!o.empty() && std::find(mv.begin(), mv.end(), o[0]) != mv.end()) fix.push_back(o);
    if (!fix.empty() && !placeInFreeSpace(m, fix, margin, true)) {
      repackMovable(m, margin);
      res.message = "No free space for the straightened islands: everything was repacked";
    }
    m.computeUVTopology();
  }
  PLOG_I("Rectify: %d island(s) straightened, %d left alone", res.islands, res.skipped);
  return res;
}

int fixOverlaps(Mesh& m, float margin, bool repackIfFull) {
  ensureTopo(m);
  int moved = 0;
  // repeat: moving the smaller island of each pair may leave other overlaps
  for (int pass = 0; pass < 4; ++pass) {
    auto ov = overlappingIslands(m);
    if (m.faceFlags.size() == m.faceCount())  // pinned islands stay (both of a pair pinned: left as is)
      ov.erase(std::remove_if(ov.begin(), ov.end(),
                              [&](const std::vector<uint32_t>& isl) {
                                for (uint32_t f : isl)
                                  if (m.faceFlags[f] & FacePinned) return true;
                                return false;
                              }),
               ov.end());
    if (ov.empty()) return moved;
    if (!placeInFreeSpace(m, ov, margin, true)) {
      if (!repackIfFull) return moved;
      m.computeUVTopology();
      repackMovable(m, margin);
      return -1;
    }
    moved += (int)ov.size();
    m.computeUVTopology();
  }
  return moved;
}

static std::vector<std::vector<uint32_t>> islandsOf(Mesh& m, const std::vector<uint32_t>& faces) {
  return movableIslands(m, faces);  // whole islands containing the faces, pinned ones and mirror copies left out
}

static void resolveMoved(Mesh& m, const std::vector<std::vector<uint32_t>>& moved, float margin) {
  // islands that now overlap something get a new place
  m.computeUVTopology();
  auto ov = overlappingIslands(m);
  if (ov.empty()) return;
  std::vector<uint8_t> isMoved(m.faceCount(), 0);
  for (auto& c : moved)
    for (uint32_t f : c) isMoved[f] = 1;
  // prefer moving the islands we changed
  std::vector<std::vector<uint32_t>> toPlace;
  for (auto& c : moved) toPlace.push_back(c);
  if (!placeInFreeSpace(m, toPlace, margin, true)) repackMovable(m, margin);
}

void orientIslands(Mesh& m, const std::vector<uint32_t>& faces, Orient o, float margin) {
  ensureTopo(m);
  auto isl = islandsOf(m, faces);
  for (auto& c : isl) orientChart(m, c, o);
  resolveMoved(m, isl, margin);
}

void setIslandDensity(Mesh& m, const std::vector<uint32_t>& faces, float uvDensity, float margin) {
  ensureTopo(m);
  auto isl = islandsOf(m, faces);
  for (auto& c : isl) scaleChartToDensity(m, c, uvDensity);
  resolveMoved(m, isl, margin);
}

bool packIntoFreeSpace(Mesh& m, const std::vector<uint32_t>& faces, float margin) {
  ensureTopo(m);
  auto isl = islandsOf(m, faces);
  return placeInFreeSpace(m, isl, margin, true);
}

// ============================================================ mirror links

MirrorLinks mirrorLinks(const Mesh& m, const Bvh& bvh, std::vector<uint8_t>* upgraded, const std::vector<vec2>* newUV) {
  MirrorLinks L;
  uint32_t F = m.faceCount();
  L.primary.assign(F, -1);
  L.source.resize(m.loopCount());
  std::iota(L.source.begin(), L.source.end(), 0u);
  if (m.faceFlags.size() != F || bvh.empty()) {
    for (uint32_t f = 0; f < F && f < m.faceFlags.size(); ++f)
      if (m.faceFlags[f] & FaceMirrorCopy) L.stale.push_back(f);
    return L;
  }
  float eps = m.boundingRadius * 1e-4f;
  auto loopFace = m.loopFace();
  auto notCopy = [&](uint32_t g) { return !(m.faceFlags[g] & FaceMirrorCopy); };
  std::vector<uint32_t> src, tryS;
  for (uint32_t f = 0; f < F; ++f) {
    uint8_t fl = m.faceFlags[f];
    if (!(fl & FaceMirrorCopy)) continue;
    int neg = (fl >> 1) & 7;
    int used = 0;
    src.clear();
    if (neg) {
      if (mirrorCorners(m, bvh, f, neg, notCopy, src, {newUV, &m.loopUV})) used = neg;
    } else {
      // older files: try the axes the face is on the negative side of, single axes first,
      // and prefer the one it is still stacked on
      vec3 c = m.faceCenter(f);
      int cand = 0;
      for (int a = 0; a < 3; ++a)
        if (c[a] < -eps) cand |= 1 << a;
      static const int order[7] = {1, 2, 4, 3, 5, 6, 7};
      for (int mask : order) {
        if ((mask & cand) != mask || !mirrorCorners(m, bvh, f, mask, notCopy, tryS, {newUV, &m.loopUV})) continue;
        float worst = 0;
        for (uint32_t k = 0; k < tryS.size(); ++k)
          worst = std::max(worst, glm::length(m.loopUV[m.faceStart[f] + k] - m.loopUV[tryS[k]]));
        if (!used) used = mask, src = tryS;
        if (worst < 1e-5f) {
          used = mask, src = tryS;
          break;
        }
      }
    }
    if (!used || src.size() != m.faceSize(f)) {
      L.stale.push_back(f);
      continue;
    }
    if (!neg && upgraded && upgraded->size() == F)
      (*upgraded)[f] = uint8_t(((*upgraded)[f] & ~kFaceMirrorBits) | FaceMirrorCopy | (used << 1));
    L.primary[f] = (int)loopFace[src[0]];
    ++L.copies;
    for (uint32_t k = 0; k < src.size(); ++k) L.source[m.faceStart[f] + k] = src[k];
  }
  return L;
}

int syncMirrorCopies(const Mesh& m, const MirrorLinks& L, const std::vector<vec2>& oldUV, std::vector<vec2>& uv,
                     const std::vector<uint8_t>& flags) {
  uint32_t F = m.faceCount();
  if (L.empty() || L.primary.size() != F || uv.size() != m.loopCount() || oldUV.size() != uv.size()) return 0;
  auto changed = [&](uint32_t f) {
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l)
      if (uv[l] != oldUV[l]) return true;
    return false;
  };
  // a copy that was edited on its own takes the face it copies along
  for (uint32_t f = 0; f < F; ++f) {
    int g = L.primary[f];
    if (g < 0 || !changed(f) || changed((uint32_t)g)) continue;
    if ((uint32_t)g < flags.size() && (flags[g] & FacePinned)) continue;
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) uv[L.source[l]] = uv[l];
  }
  int n = 0;
  for (uint32_t f = 0; f < F; ++f) {
    if (L.primary[f] < 0) continue;
    bool diff = false;
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l)
      if (uv[l] != uv[L.source[l]]) uv[l] = uv[L.source[l]], diff = true;
    n += diff ? 1 : 0;
  }
  return n;
}

int restackMirrorCopies(Mesh& m, const MirrorLinks& L) {
  std::vector<vec2> uv = m.loopUV;
  int n = 0;
  for (uint32_t f = 0; f < m.faceCount() && f < L.primary.size(); ++f) {
    if (L.primary[f] < 0) continue;
    bool diff = false;
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l)
      if (m.loopUV[l] != m.loopUV[L.source[l]]) m.loopUV[l] = m.loopUV[L.source[l]], diff = true;
    n += diff ? 1 : 0;
  }
  return n;
}

int unstackedCopies(const Mesh& m, const MirrorLinks& L) {
  int n = 0;
  for (uint32_t f = 0; f < m.faceCount() && f < L.primary.size(); ++f) {
    if (L.primary[f] < 0) continue;
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l)
      if (glm::length(m.loopUV[l] - m.loopUV[L.source[l]]) > 1e-6f) {
        ++n;
        break;
      }
  }
  return n;
}

std::vector<uint32_t> withLinkedFaces(const Mesh& m, const MirrorLinks& L, const std::vector<uint32_t>& faces) {
  uint32_t F = m.faceCount();
  std::vector<uint8_t> in(F, 0);
  for (uint32_t f : faces)
    if (f < F) in[f] = 1;
  if (!L.empty() && L.primary.size() == F) {
    for (uint32_t f = 0; f < F; ++f)  // the faces the selected copies copy
      if (in[f] == 1 && L.primary[f] >= 0) in[L.primary[f]] |= 2;
    for (uint32_t f = 0; f < F; ++f)  // the copies of everything selected so far
      if (L.primary[f] >= 0 && in[L.primary[f]]) in[f] |= 2;
  }
  std::vector<uint32_t> out;
  for (uint32_t f = 0; f < F; ++f)
    if (in[f]) out.push_back(f);
  return out;
}

// ============================================================ pins

size_t pinnedFaceCount(const Mesh& m) {
  size_t n = 0;
  if (m.faceFlags.size() == m.faceCount())
    for (uint8_t f : m.faceFlags) n += (f & FacePinned) ? 1 : 0;
  return n;
}

std::vector<uint32_t> collapsedFaces(const Mesh& m) {
  std::vector<uint32_t> out;
  for (uint32_t f = 0; f < m.faceCount(); ++f)  // faces that are tiny in 3D as well don't count
    if (m.faceHasNoUVArea(f, m.loopUV) && m.faceArea3D(f) > 1e-12f) out.push_back(f);
  return out;
}

static std::vector<uint32_t> filterFaces(const Mesh& m, const std::vector<uint32_t>& faces, uint8_t drop) {
  std::vector<uint32_t> out;
  bool flags = m.faceFlags.size() == m.faceCount();
  auto keep = [&](uint32_t f) { return f < m.faceCount() && !(flags && (m.faceFlags[f] & drop)); };
  if (faces.empty()) {
    for (uint32_t f = 0; f < m.faceCount(); ++f)
      if (keep(f)) out.push_back(f);
  } else {
    for (uint32_t f : faces)
      if (keep(f)) out.push_back(f);
  }
  return out;
}

std::vector<uint32_t> unpinnedFaces(const Mesh& m, const std::vector<uint32_t>& faces) {
  return filterFaces(m, faces, FacePinned);
}

std::vector<uint32_t> layoutFaces(const Mesh& m, const std::vector<uint32_t>& faces) {
  return filterFaces(m, faces, FacePinned | FaceMirrorCopy);
}

bool packAroundFixed(Mesh& m, const std::vector<uint32_t>& faces, float margin, bool keepSizes) {
  ensureTopo(m);
  m.computeUVTopology();
  auto charts = movableIslands(m, faces);
  if (charts.empty()) return false;
  uint32_t F = m.faceCount();
  std::vector<uint8_t> moving(F, 0);
  std::vector<uint32_t> movingFaces, pinned;
  for (auto& c : charts)
    for (uint32_t f : c) moving[f] = 1, movingFaces.push_back(f);
  bool flags = m.faceFlags.size() == F;
  double fixedUV = 0;
  for (uint32_t f = 0; f < F; ++f) {
    if (moving[f] || (flags && (m.faceFlags[f] & FaceMirrorCopy))) continue;
    if (flags && (m.faceFlags[f] & FacePinned)) pinned.push_back(f);
    fixedUV += std::abs(m.faceAreaUV(f));
  }
  // the pinned islands set the density when there are any
  float target = pinned.empty() ? areaDensity(m, movingFaces) : areaDensity(m, pinned);
  if (target <= 0) target = areaDensity(m, movingFaces);
  if (target <= 0) target = 1.f;
  double area3 = 0;
  for (uint32_t f : movingFaces) area3 += m.faceArea3D(f);
  double freeUV = std::max(0.05, 1.0 - fixedUV);
  double k = std::min(1.0, std::sqrt(0.75 * freeUV / std::max(area3 * target * target, 1e-30)));
  // the islands' outlines, packed into the space the others leave (at most at their density)
  {
    std::vector<uint32_t> fixedFaces;
    for (uint32_t f = 0; f < F; ++f)
      if (!moving[f]) fixedFaces.push_back(f);
    PackOptions po;
    po.margin = margin;
    po.normalizeDensity = !keepSizes;  // keepSizes: the islands keep their sizes against each other
    ShapePackStats st;
    if (packShapes(m, charts, po, fixedFaces, keepSizes ? 0.f : target, &st)) {
      PLOG_I("Packed %zu islands around the fixed ones at %.0f%% of their density", charts.size(), st.scale / target * 100.0);
      return true;
    }
  }
  (void)k;
  auto loops = loopsOfFaces(m, movingFaces);
  std::vector<vec2> keep;
  for (uint32_t l : loops) keep.push_back(m.loopUV[l]);
  for (int attempt = 0; attempt < 40; ++attempt, k *= 0.92) {
    for (size_t i = 0; i < loops.size(); ++i) m.loopUV[loops[i]] = keep[i];
    for (auto& c : charts) scaleChartToDensity(m, c, float(target * k));
    if (placeInFreeSpace(m, charts, margin, true)) {
      PLOG_I("Packed %zu islands around the fixed ones at %.0f%% of their density", charts.size(), k * 100.0);
      return true;
    }
  }
  for (size_t i = 0; i < loops.size(); ++i) m.loopUV[loops[i]] = keep[i];
  return false;
}

void repackMovable(Mesh& m, float margin, bool rotate) {
  if (pinnedFaceCount(m) && packAroundFixed(m, {}, margin)) return;
  if (pinnedFaceCount(m)) return;  // nothing better to do without moving pinned islands
  PackOptions po;
  po.margin = margin;
  po.rotate = rotate;
  po.normalizeDensity = true;
  pack(m, po);  // mirror copies are skipped, and follow afterward
}

// ============================================================ metrics

std::vector<float> faceStretch(const Mesh& m) {
  uint32_t F = m.faceCount();
  std::vector<float> out(F, 0.f);
  if (m.loopUV.size() != m.loopCount()) return out;
  for (uint32_t f = 0; f < F; ++f) {
    float worst = 0;
    forFaceTris(m, f, [&](uint32_t a, uint32_t b, uint32_t c) {
      glm::dvec3 p0(m.positions[m.loopVert[a]]), p1(m.positions[m.loopVert[b]]), p2(m.positions[m.loopVert[c]]);
      dvec2 q0(m.loopUV[a]), q1(m.loopUV[b]), q2(m.loopUV[c]);
      // local 2D frame of the 3D triangle
      glm::dvec3 e1 = p1 - p0, e2 = p2 - p0;
      double l1 = glm::length(e1);
      glm::dvec3 n = glm::cross(e1, e2);
      if (l1 < 1e-20 || glm::length(n) < 1e-20) return;
      glm::dvec3 x = e1 / l1, y = glm::normalize(glm::cross(n, x));
      dvec2 s1(l1, 0), s2(glm::dot(e2, x), glm::dot(e2, y));
      dvec2 t1 = q1 - q0, t2 = q2 - q0;
      double det = s1.x * s2.y - s2.x * s1.y;
      if (std::abs(det) < 1e-30) return;
      // Jacobian J maps 3D-local to UV: J * [s1 s2] = [t1 t2]
      double a11 = (t1.x * s2.y - t2.x * s1.y) / det, a12 = (t2.x * s1.x - t1.x * s2.x) / det;
      double a21 = (t1.y * s2.y - t2.y * s1.y) / det, a22 = (t2.y * s1.x - t1.y * s2.x) / det;
      // singular values of a 2x2 matrix
      double E = (a11 + a22) / 2, Fv = (a11 - a22) / 2, G = (a21 + a12) / 2, H = (a21 - a12) / 2;
      double Q = std::sqrt(E * E + H * H), Rr = std::sqrt(Fv * Fv + G * G);
      double smax = Q + Rr, smin = std::abs(Q - Rr);
      if (smin < 1e-12) {
        worst = 100.f;
        return;
      }
      worst = std::max(worst, float(smax / smin));
    });
    out[f] = worst;
  }
  return out;
}

std::vector<float> faceDensity(const Mesh& m) {
  uint32_t F = m.faceCount();
  std::vector<float> out(F, 0.f);
  if (m.loopUV.size() != m.loopCount()) return out;
  for (uint32_t f = 0; f < F; ++f) {
    double a3 = m.faceArea3D(f), au = std::abs(m.faceAreaUV(f));
    if (a3 > 1e-30 && std::isfinite(au) && au > 1e-16) out[f] = float(std::sqrt(au / a3));
  }
  return out;
}

// ============================================================ selection

void growSelection(const Mesh& mc, std::vector<uint8_t>& sel) {
  Mesh& m = const_cast<Mesh&>(mc);
  if (m.loopEdge.size() != m.loopCount()) m.buildEdges();
  sel.resize(m.faceCount(), 0);
  EdgeFaces ef(m);
  auto add = sel;
  for (uint32_t f = 0; f < m.faceCount(); ++f) {
    if (!sel[f]) continue;
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) {
      uint32_t e = m.loopEdge[l];
      for (uint32_t k = ef.start[e]; k < ef.start[e + 1]; ++k) add[ef.faces[k]] = 1;
    }
  }
  sel.swap(add);
}

void shrinkSelection(const Mesh& mc, std::vector<uint8_t>& sel) {
  Mesh& m = const_cast<Mesh&>(mc);
  if (m.loopEdge.size() != m.loopCount()) m.buildEdges();
  sel.resize(m.faceCount(), 0);
  EdgeFaces ef(m);
  auto out = sel;
  for (uint32_t f = 0; f < m.faceCount(); ++f) {
    if (!sel[f]) continue;
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) {
      uint32_t e = m.loopEdge[l];
      bool border = (ef.start[e + 1] - ef.start[e]) < 2;
      for (uint32_t k = ef.start[e]; k < ef.start[e + 1]; ++k)
        if (!sel[ef.faces[k]]) border = true;
      if (border) {
        out[f] = 0;
        break;
      }
    }
  }
  sel.swap(out);
}

}  // namespace patina::uv
