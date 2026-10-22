#include <cmath>

#include "Test.h"
#include "core/Log.h"
#include "core/Mesh.h"
#include "core/SurfaceReach.h"
#include "core/UVOps.h"

using namespace patina;

namespace {

bool allFinite(const Mesh& m) {
  for (auto& t : m.loopUV)
    if (!std::isfinite(t.x) || !std::isfinite(t.y)) return false;
  return true;
}

bool allInUnit(const Mesh& m, float eps = 1e-5f) {
  for (auto& t : m.loopUV)
    if (t.x < -eps || t.y < -eps || t.x > 1 + eps || t.y > 1 + eps) return false;
  return true;
}

// Mean |angle_uv - angle_3d| over all triangle corners (radians).
double meanAngleError(const Mesh& m) {
  double sum = 0;
  int n = 0;
  for (auto& t : m.tris) {
    uint32_t c[3] = {t.x, t.y, t.z};
    for (int i = 0; i < 3; ++i) {
      uint32_t a = c[i], b = c[(i + 1) % 3], d = c[(i + 2) % 3];
      vec3 p = m.positions[m.loopVert[a]];
      vec3 e1 = m.positions[m.loopVert[b]] - p, e2 = m.positions[m.loopVert[d]] - p;
      vec2 q = m.loopUV[a], f1 = m.loopUV[b] - q, f2 = m.loopUV[d] - q;
      float l3 = glm::length(e1) * glm::length(e2), l2 = glm::length(f1) * glm::length(f2);
      if (l3 < 1e-12f || l2 < 1e-12f) continue;
      double a3 = std::acos(std::clamp(glm::dot(e1, e2) / l3, -1.f, 1.f));
      double a2 = std::acos(std::clamp(glm::dot(f1, f2) / l2, -1.f, 1.f));
      sum += std::abs(a3 - a2);
      ++n;
    }
  }
  return n ? sum / n : 1e9;
}

int flippedTris(const Mesh& m) {
  int n = 0;
  for (auto& t : m.tris)
    if (cross2(m.loopUV[t.y] - m.loopUV[t.x], m.loopUV[t.z] - m.loopUV[t.x]) < 0) ++n;
  return n;
}

void scramble(Mesh& m, uint64_t seed) {
  Rng rng(seed);
  for (auto& t : m.loopUV) t = {rng.uniform(), rng.uniform()};
}

int seamCount(const Mesh& m) {
  int n = 0;
  for (auto f : m.edgeFlags) n += (f & EdgeSeam) ? 1 : 0;
  return n;
}

// Mesh of `n` disconnected quads with random 3D sizes and random overlapping UVs.
Mesh makeRandomQuads(int n, uint64_t seed) {
  Mesh m;
  Rng rng(seed);
  m.faceStart = {0};
  for (int i = 0; i < n; ++i) {
    float w = 0.05f + rng.uniform() * 2.f, h = 0.05f + rng.uniform() * 0.5f;
    float z = float(i);
    uint32_t b = (uint32_t)m.positions.size();
    m.positions.insert(m.positions.end(), {{0, 0, z}, {w, 0, z}, {w, h, z}, {0, h, z}});
    float uw = 0.05f + rng.uniform() * 0.4f, uh = 0.05f + rng.uniform() * 0.4f;
    vec2 o(rng.uniform(), rng.uniform());
    m.loopVert.insert(m.loopVert.end(), {b, b + 1, b + 2, b + 3});
    m.loopUV.insert(m.loopUV.end(), {o, o + vec2(uw, 0), o + vec2(uw, uh), o + vec2(0, uh)});
    m.faceStart.push_back((uint32_t)m.loopVert.size());
  }
  m.finalize();
  return m;
}

}  // namespace

TEST(uv, lscm_plane_conformal) {
  Mesh m = makePlane(2.f, 8);
  scramble(m, 7);
  uv::unwrapLSCM(m);
  CHECK(allFinite(m));
  CHECK(allInUnit(m));
  double err = meanAngleError(m);
  std::fprintf(stderr, "  plane angle error %.2e rad\n", err);
  CHECK(err < 0.02);
  CHECK(flippedTris(m) == 0);
  m.computeUVTopology();
  CHECK(m.islandCount == 1u);
}

TEST(uv, lscm_bent_plane) {
  // A developable surface (bent sheet): LSCM should flatten it almost exactly.
  Mesh m = makePlane(2.f, 24);
  for (auto& p : m.positions) p.y = 0.4f * std::sin(p.x * 2.f);
  m.finalize();
  scramble(m, 3);
  uv::unwrapLSCM(m);
  CHECK(allFinite(m));
  double err = meanAngleError(m);
  std::fprintf(stderr, "  bent plane angle error %.2e rad\n", err);
  CHECK(err < 0.02);
  CHECK(flippedTris(m) == 0);
}

TEST(uv, lscm_sphere_with_seam) {
  Mesh m = makeUVSphere();
  CHECK(seamCount(m) > 0);
  CHECK(uv::chartsFromSeams(m).size() == 1u);  // a meridian seam does not disconnect
  scramble(m, 11);
  uv::unwrapLSCM(m);
  CHECK(allFinite(m));
  CHECK(allInUnit(m));
  int flipped = flippedTris(m);
  std::fprintf(stderr, "  sphere flipped %d / %zu, angle error %.3f\n", flipped, m.tris.size(), meanAngleError(m));
  CHECK(flipped < int(m.tris.size() / 100));
}

TEST(uv, lscm_closed_sphere) {
  Mesh m = makeUVSphere(32, 16);
  for (auto& f : m.edgeFlags) f = 0;  // no seams: closed chart must be cut
  uv::unwrapLSCM(m);
  CHECK(allFinite(m));
  CHECK(allInUnit(m));
  CHECK(seamCount(m) > 0);
  CHECK(uv::chartsFromSeams(m).size() > 1u);
  CHECK(flippedTris(m) < int(m.tris.size() / 100));
  m.computeUVTopology();
  CHECK(uv::overlapCount(m, 512) < int(m.faceCount() / 50));
}

TEST(uv, lscm_cube_cut_faces_only) {
  // Unwrapping a subset only touches those faces' loops.
  Mesh m = makeCube();
  uv::seamsFromIslands(m);
  auto before = m.loopUV;
  uv::UnwrapOptions opt;
  opt.pack = false;
  uv::unwrapLSCM(m, {0, 1});
  bool othersKept = true;
  for (uint32_t l = m.faceStart[2]; l < m.loopCount(); ++l) othersKept = othersKept && m.loopUV[l] == before[l];
  CHECK(othersKept);
  CHECK(allFinite(m));
}

TEST(uv, lscm_large_chart_performance) {
  Mesh m = makePlane(2.f, 160);  // 51200 triangles
  for (auto& p : m.positions) p.y = 0.3f * std::sin(p.x * 3.f) * std::cos(p.z * 2.f);
  m.finalize();
  scramble(m, 5);
  double t0 = nowSeconds();
  uv::unwrapLSCM(m);
  double dt = nowSeconds() - t0;
  std::fprintf(stderr, "  %zu tris in %.2f s, angle error %.4f, flipped %d\n", m.tris.size(), dt, meanAngleError(m),
               flippedTris(m));
  CHECK(allFinite(m));
  CHECK(allInUnit(m));
  CHECK(dt < 10.0);
  CHECK(flippedTris(m) == 0);
  CHECK(meanAngleError(m) < 0.05);
}

TEST(uv, pack_random_rect_islands) {
  Mesh m = makeRandomQuads(200, 42);
  CHECK(m.islandCount == 200u);
  CHECK(uv::overlapCount(m) > 0);  // the random layout overlaps
  uv::pack(m);
  m.computeUVTopology();
  CHECK(m.islandCount == 200u);
  CHECK(allFinite(m));
  CHECK(allInUnit(m));
  CHECK(uv::overlapCount(m, 1024) == 0);
  float cov = uv::coverage(m, 512);
  std::fprintf(stderr, "  200 islands coverage %.3f\n", cov);
  CHECK(cov > 0.5f);
  // uniform texel density after normalization
  float d0 = uv::texelDensity(m, {0}), d1 = uv::texelDensity(m, {137});
  CHECK_NEAR(d0, d1, 1e-3 * d0);
}

TEST(uv, pack_performance_1000) {
  Mesh m = makeRandomQuads(1000, 9), b = m;
  double t0 = nowSeconds();
  uv::PackOptions boxes;
  boxes.shapes = false;
  uv::pack(b, boxes);
  double db = (nowSeconds() - t0) * 1000.0;
  t0 = nowSeconds();
  uv::pack(m);
  double dt = (nowSeconds() - t0) * 1000.0;
  std::fprintf(stderr, "  packed 1000 islands: boxes %.1f ms (coverage %.3f), shapes %.1f ms (coverage %.3f)\n", db,
               uv::coverage(b, 512), dt, uv::coverage(m, 512));
  CHECK(db < 250.0);   // target < 100 ms; slack for loaded CI machines
  CHECK(dt < 2000.0);  // the shape packer does far more work; still interactive
  CHECK(allInUnit(m));
  CHECK(uv::overlapCount(m, 1024) == 0);
}

TEST(uv, pack_subset_and_margin) {
  Mesh m = makeRandomQuads(20, 1);
  auto before = m.loopUV;
  uv::PackOptions po;
  po.margin = 0.02f;
  uv::pack(m, po, {0, 1, 2});
  bool othersKept = true;
  for (uint32_t l = m.faceStart[3]; l < m.loopCount(); ++l) othersKept = othersKept && m.loopUV[l] == before[l];
  CHECK(othersKept);
  // packed islands keep at least the margin between each other
  for (uint32_t a = 0; a < 3; ++a)
    for (uint32_t b = a + 1; b < 3; ++b) {
      Rect2 ra = uv::boundsOfLoops(m, uv::loopsOfFaces(m, {a})), rb = uv::boundsOfLoops(m, uv::loopsOfFaces(m, {b}));
      float gx = std::max(ra.lo.x - rb.hi.x, rb.lo.x - ra.hi.x), gy = std::max(ra.lo.y - rb.hi.y, rb.lo.y - ra.hi.y);
      CHECK(std::max(gx, gy) >= 0.02f * 0.99f);
    }
}

TEST(uv, smart_project_cube) {
  Mesh m = makeCube();
  for (auto& f : m.edgeFlags) f = 0;
  scramble(m, 2);
  uv::smartProject(m);
  m.computeUVTopology();
  CHECK(m.islandCount == 6u);
  CHECK(allInUnit(m));
  CHECK(uv::coverage(m) > 0.4f);
  CHECK(uv::overlapCount(m) == 0);
  CHECK(seamCount(m) == 12);
  CHECK(flippedTris(m) == 0);
}

TEST(uv, smart_project_sphere) {
  Mesh m = makeUVSphere(32, 16);
  uv::smartProject(m, 45.f);
  m.computeUVTopology();
  CHECK(allFinite(m));
  CHECK(allInUnit(m));
  CHECK(m.islandCount > 4u);
  CHECK(flippedTris(m) == 0);
  CHECK(uv::overlapCount(m, 512) == 0);
}

TEST(uv, project_box_cube) {
  Mesh m = makeCube(2.f);
  uv::projectBox(m);
  m.computeUVTopology();
  CHECK(m.islandCount == 6u);
  CHECK(allInUnit(m));
  CHECK(flippedTris(m) == 0);
  CHECK(uv::overlapCount(m) == 0);
}

TEST(uv, project_planar_plane) {
  Mesh m = makePlane(2.f, 4);
  scramble(m, 4);
  uv::projectPlanar(m, vec3(0, 1, 0));
  Rect2 r = uv::boundsOfLoops(m, uv::loopsOfFaces(m, {}));
  CHECK_NEAR(r.lo.x, 0, 1e-5);
  CHECK_NEAR(r.hi.y, 1, 1e-5);
  CHECK(meanAngleError(m) < 1e-3);
  CHECK(flippedTris(m) == 0);  // looking down the normal keeps CCW winding
}

TEST(uv, seams_from_islands_cube) {
  Mesh m = makeCube();
  CHECK(m.islandCount == 6u);
  uv::seamsFromIslands(m);
  int seams = seamCount(m);
  CHECK(seams > 0);
  CHECK(seams == 12);  // every cube face is its own island in the cross layout
  // every edge between different islands is a seam
  auto lf = m.loopFace();
  bool ok = true;
  for (uint32_t a = 0; a < m.loopCount(); ++a)
    for (uint32_t b = a + 1; b < m.loopCount(); ++b)
      if (m.loopEdge[a] == m.loopEdge[b] && m.loopIsland[a] != m.loopIsland[b])
        ok = ok && (m.edgeFlags[m.loopEdge[a]] & EdgeSeam);
  CHECK(ok);
  CHECK(uv::chartsFromSeams(m).size() == 6u);
  CHECK(uv::facesByIsland(m).size() == 6u);

  Mesh p = makePlane(2.f, 3);
  uv::seamsFromIslands(p);
  CHECK(seamCount(p) == 0);
}

TEST(uv, seams_by_angle) {
  Mesh m = makeCube();
  uv::seamsByAngle(m, 30.f, true);
  CHECK(seamCount(m) == 12);
  Mesh s = makeUVSphere(32, 16);
  uv::seamsByAngle(s, 30.f, true);
  CHECK(seamCount(s) == 0);
}

TEST(uv, transforms) {
  Mesh m = makePlane(2.f, 3);
  scramble(m, 8);
  auto orig = m.loopUV;
  auto loops = uv::loopsOfFaces(m, {});
  CHECK(loops.size() == m.loopCount());
  vec2 pivot(0.3f, 0.7f);
  uv::rotate(m, loops, kPi / 2, pivot);
  // 90 degrees CCW: (1,0) offset becomes (0,1)
  vec2 d0 = orig[0] - pivot;
  CHECK_NEAR(m.loopUV[0].x, pivot.x - d0.y, 1e-5);
  CHECK_NEAR(m.loopUV[0].y, pivot.y + d0.x, 1e-5);
  uv::rotate(m, loops, -kPi / 2, pivot);
  float maxErr = 0;
  for (size_t i = 0; i < orig.size(); ++i) maxErr = std::max(maxErr, glm::length(m.loopUV[i] - orig[i]));
  CHECK(maxErr < 1e-5f);

  uv::scale(m, loops, vec2(2.f, 0.5f), pivot);
  uv::scale(m, loops, vec2(0.5f, 2.f), pivot);
  uv::translate(m, loops, vec2(0.25f, -1.f));
  uv::translate(m, loops, vec2(-0.25f, 1.f));
  uv::flip(m, loops, true);
  uv::flip(m, loops, true);
  uv::flip(m, loops, false);
  uv::flip(m, loops, false);
  maxErr = 0;
  for (size_t i = 0; i < orig.size(); ++i) maxErr = std::max(maxErr, glm::length(m.loopUV[i] - orig[i]));
  CHECK(maxErr < 1e-5f);

  uv::fitToUnitSquare(m, loops, 0.1f);
  Rect2 r = uv::boundsOfLoops(m, loops);
  CHECK(r.lo.x >= 0.1f - 1e-5f && r.lo.y >= 0.1f - 1e-5f && r.hi.x <= 0.9f + 1e-5f && r.hi.y <= 0.9f + 1e-5f);
  CHECK(std::abs(std::max(r.size().x, r.size().y) - 0.8f) < 1e-4f);

  // weld: all corners of each vertex collapse to one UV -> a single island
  uv::weld(m, loops);
  m.computeUVTopology();
  CHECK(m.islandCount == 1u);
  CHECK(m.uvVertCount == m.positions.size());
}

TEST(uv, metrics) {
  Mesh m = makePlane(2.f, 4);  // 2x2 plane, UVs fill 0..1
  CHECK_NEAR(uv::texelDensity(m), 0.5, 1e-4);
  CHECK_NEAR(uv::coverage(m, 128), 1.0, 1e-3);
  CHECK(uv::overlapCount(m) == 0);
  auto dist = uv::areaDistortion(m);
  CHECK(dist.size() == m.faceCount());
  float maxAbs = 0;
  for (float d : dist) maxAbs = std::max(maxAbs, std::abs(d));
  CHECK(maxAbs < 1e-3f);
  // stack face 1 on face 0 -> exactly one overlapping pair
  for (uint32_t i = 0; i < 4; ++i) m.loopUV[m.faceStart[1] + i] = m.loopUV[m.faceStart[0] + i];
  CHECK(uv::overlapCount(m) == 1);
  dist = uv::areaDistortion(m);
  CHECK(std::isfinite(dist[0]));
}

TEST(uv, degenerate_input) {
  Mesh m;
  m.positions = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 0}, {2, 0, 0}, {2, 0, 0}, {2, 0, 0},
                 {5, 5, 5}, {6, 5, 5}, {5, 6, 5}, {3, 3, 3}, {4, 3, 3}, {5, 3, 3}};
  m.faceStart = {0};
  auto add = [&](std::initializer_list<uint32_t> vs) {
    for (uint32_t v : vs) m.loopVert.push_back(v);
    m.faceStart.push_back((uint32_t)m.loopVert.size());
  };
  add({0, 1, 2});     // normal triangle
  add({3, 1, 2});     // duplicated vertex position of 0
  add({4, 5, 6});     // zero area (all coincident)
  add({7, 8, 9});     // isolated triangle
  add({10, 11, 12});  // collinear
  add({0, 0, 1});     // repeated vertex index
  m.finalize();
  for (int pass = 0; pass < 4; ++pass) {
    for (auto& t : m.loopUV) t = vec2(0);
    if (pass == 0) uv::unwrapLSCM(m);
    if (pass == 1) uv::smartProject(m);
    if (pass == 2) uv::projectBox(m);
    if (pass == 3) uv::projectPlanar(m, vec3(0));
    CHECK(allFinite(m));
    CHECK(allInUnit(m));
    m.computeUVTopology();
    uv::pack(m);
    CHECK(allFinite(m));
  }
  CHECK(std::isfinite(uv::texelDensity(m)));
  for (float d : uv::areaDistortion(m)) CHECK(std::isfinite(d));
  // empty mesh
  Mesh e;
  e.faceStart = {0};
  e.finalize();
  uv::unwrapLSCM(e);
  uv::smartProject(e);
  uv::pack(e);
  CHECK(uv::coverage(e) == 0.f);
  CHECK(uv::overlapCount(e) == 0);
}

// ---------------------------------------------------------------- 0.4 island tools
#include "core/UVIsland.h"

namespace {
double corrVY(const Mesh& m) {
  // correlation of V with object-space Y over all loops
  double n = m.loopCount(), sv = 0, sy = 0, svv = 0, syy = 0, svy = 0;
  for (uint32_t l = 0; l < m.loopCount(); ++l) {
    double v = m.loopUV[l].y, y = m.positions[m.loopVert[l]].y;
    sv += v, sy += y, svv += v * v, syy += y * y, svy += v * y;
  }
  double cov = svy / n - sv / n * sy / n, vv = svv / n - sv * sv / n / n, yy = syy / n - sy * sy / n / n;
  return cov / std::sqrt(std::max(vv * yy, 1e-30));
}
}  // namespace

TEST(uv, island_from_faces_keeps_layout) {
  Mesh m = makeCube(2.f);
  uv::smartProject(m, 66.f);
  m.computeUVTopology();
  std::vector<vec2> before = m.loopUV;
  float layout = uv::densityTargets(m, 1024).layout;
  CHECK(layout > 0);
  // two adjacent faces become one island
  std::vector<uint32_t> faces = {0, 1};
  // make sure 0 and 1 share an edge
  bool share = false;
  for (uint32_t a = m.faceStart[0]; a < m.faceStart[1]; ++a)
    for (uint32_t b = m.faceStart[1]; b < m.faceStart[2]; ++b) share |= m.loopEdge[a] == m.loopEdge[b];
  if (!share) faces = {0, 2};
  uv::IslandOptions o;
  o.resolution = 1024;
  auto r = uv::makeIsland(m, faces, o);
  m.computeUVTopology();
  CHECK(r.ok);
  CHECK(!r.repacked);
  CHECK(r.charts == 1);
  CHECK(m.loopIsland[m.faceStart[faces[0]]] == m.loopIsland[m.faceStart[faces[1]]]);
  CHECK(std::abs(uv::areaDensity(m, faces) / layout - 1.f) < 0.03f);
  CHECK(allInUnit(m));
  CHECK(uv::overlapCount(m) == 0);
  // other faces did not move
  for (uint32_t f = 0; f < m.faceCount(); ++f) {
    if (f == faces[0] || f == faces[1]) continue;
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) CHECK(glm::length(m.loopUV[l] - before[l]) < 1e-6f);
  }
}

TEST(uv, island_upright_on_a_wall) {
  Mesh m = makePlane(2.f, 6);
  for (auto& p : m.positions) p = {p.x, -p.z, 0.f};  // stand the plane up (XY), keep the winding
  m.finalize();
  for (auto& t : m.loopUV) t = vec2(0);              // no UVs yet
  m.computeUVTopology();
  uv::IslandOptions o;
  o.resolution = 1024;
  auto r = uv::makeIsland(m, {}, o);
  m.computeUVTopology();
  CHECK(r.ok);
  CHECK(corrVY(m) > 0.99);  // up on the wall = up in the texture
  CHECK(allInUnit(m));
  // no layout: the density is the smaller of "fit" and "screen"
  auto t = uv::densityTargets(m, 1024);
  CHECK(std::abs(uv::areaDensity(m) - std::min(t.fit, t.screen)) / uv::areaDensity(m) < 0.05f);
}

TEST(uv, density_targets) {
  Mesh m = makePlane(2.f, 1);  // 4 square units
  auto t = uv::densityTargets(m, 2048);
  CHECK(std::abs(t.fit - std::sqrt(0.7f / 4.f)) < 1e-4f);
  CHECK(t.layout > 0);  // the default plane UVs cover 0..1
  CHECK(std::abs(t.layout - 0.5f) < 1e-3f);
  CHECK(t.screen > 0);
}

// square quads in the XZ plane, each its own island: side `s` in the model, `us` in UV (density us / s)
static void addSquare(Mesh& m, vec3 o, float s, vec2 uo, float us) {
  if (m.faceStart.empty()) m.faceStart = {0};
  uint32_t b = (uint32_t)m.positions.size();
  vec3 p[4] = {o, o + vec3(s, 0, 0), o + vec3(s, 0, -s), o + vec3(0, 0, -s)};
  vec2 q[4] = {uo, uo + vec2(us, 0), uo + vec2(us, us), uo + vec2(0, us)};
  for (int k = 0; k < 4; ++k) {
    m.positions.push_back(p[k]);
    m.loopVert.push_back(b + k);
    m.loopUV.push_back(q[k]);
  }
  m.faceStart.push_back((uint32_t)m.loopVert.size());
}

TEST(uv, density_layout_is_area_weighted) {
  // one big island (area 4, density 0.25) and 40 small ones (area 0.4 in all, density 0.5)
  Mesh m;
  addSquare(m, {0, 0, 0}, 2.f, {0.02f, 0.02f}, 0.5f);
  for (int i = 0; i < 40; ++i)
    addSquare(m, {3 + (i % 8) * 0.2f, 0, -(i / 8) * 0.2f}, 0.1f, {0.56f + (i % 8) * 0.055f, 0.02f + (i / 8) * 0.055f}, 0.05f);
  m.finalize();
  m.computeUVTopology();
  auto t = uv::densityTargets(m, 1024);
  CHECK(std::abs(t.layout - 0.25f) < 1e-3f);
  CHECK(std::abs(t.fit - std::sqrt(0.7f / 4.4f)) < 1e-4f);
  CHECK(std::abs(t.recommended() - 0.25f) < 1e-3f);
  // mirror copies share their other half's texels, so they don't count toward fit
  m.faceFlags.assign(m.faceCount(), 0);
  for (uint32_t f = 1; f < m.faceCount(); ++f) m.faceFlags[f] = FaceMirrorCopy;
  t = uv::densityTargets(m, 1024);
  CHECK(std::abs(t.fit - std::sqrt(0.7f / 4.f)) < 1e-4f);
  // the reported texel density is area weighted too
  CHECK(std::abs(uv::texelDensity(m) - std::sqrt((0.25f + 40 * 0.0025f) / 4.4f)) < 1e-4f);
}

TEST(uv, density_layout_ignores_tiled_and_caps_stacked) {
  Mesh m;
  addSquare(m, {0, 0, 0}, 2.f, {1, 1}, 4.f);       // tiled: UVs 1..5
  addSquare(m, {3, 0, 0}, 1.f, {0.1f, 0.1f}, 0.25f);
  m.finalize();
  m.computeUVTopology();
  CHECK(std::abs(uv::densityTargets(m, 1024).layout - 0.25f) < 1e-3f);
  // ten unit squares stacked on the same 0..1 square: density 1, but they could never all have it
  Mesh s;
  for (int i = 0; i < 10; ++i) addSquare(s, {i * 1.5f, 0, 0}, 1.f, {0, 0}, 1.f);
  s.finalize();
  s.computeUVTopology();
  auto t = uv::densityTargets(s, 1024);
  CHECK(std::abs(t.layout - 1.f) < 1e-3f);
  CHECK(std::abs(t.recommended() - 2.f * std::sqrt(0.07f)) < 1e-3f);
}

TEST(uv, density_texel_density_skips_unmapped_faces) {
  Mesh m = makePlane(2.f, 4);  // density 0.5 everywhere
  for (uint32_t f = 0; f < 8; ++f)
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) m.loopUV[l] = vec2(0.9f);
  CHECK(std::abs(uv::texelDensity(m) - 0.5f) < 1e-3f);
}

TEST(uv, density_pack_equalizes) {
  // islands at densities 0.1 .. 0.6 come out of a pack at one density
  Mesh m;
  for (int i = 0; i < 6; ++i) addSquare(m, {i * 1.5f, 0, 0}, 1.f, {0.15f * i, 0.f}, 0.1f * (i + 1));
  m.finalize();
  m.computeUVTopology();
  uv::PackOptions po;
  uv::pack(m, po);
  m.computeUVTopology();
  float lo = 1e9f, hi = 0;
  for (auto& isl : uv::facesByIsland(m)) {
    float d = uv::areaDensity(m, isl);
    lo = std::min(lo, d), hi = std::max(hi, d);
  }
  CHECK(lo > 0 && hi / lo < 1.01f);
  CHECK(allInUnit(m));
}

TEST(uv, fix_overlaps_moves_islands_apart) {
  Mesh m = makeCube(2.f);
  uv::smartProject(m, 66.f);
  m.computeUVTopology();
  CHECK(uv::overlapCount(m) == 0);
  // put island of face 3 on top of face 0's island
  auto isl = uv::facesByIsland(m);
  CHECK(isl.size() >= 2);
  auto loopsA = uv::loopsOfFaces(m, isl[0]), loopsB = uv::loopsOfFaces(m, isl[1]);
  vec2 d = uv::boundsOfLoops(m, loopsA).center() - uv::boundsOfLoops(m, loopsB).center();
  uv::translate(m, loopsB, d);
  m.computeUVTopology();
  CHECK(uv::overlapCount(m) > 0);
  int moved = uv::fixOverlaps(m);
  m.computeUVTopology();
  CHECK(moved != 0);
  CHECK(uv::overlapCount(m) == 0);
  CHECK(allInUnit(m));
}

TEST(uv, orient_longest_axis_and_stretch) {
  Mesh m = makePlane(2.f, 4);
  // make the UVs a rotated, stretched strip
  for (auto& t : m.loopUV) t = vec2(0.5f) + glm::mat2(0.7f, 0.7f, -0.2f, 0.2f) * (t - vec2(0.5f));
  auto st = uv::faceStretch(m);
  CHECK(st[0] > 2.f);
  for (auto& t : m.loopUV) t = vec2(0.25f) + (t - vec2(0.25f)) * 0.f;  // reset
  m = makePlane(2.f, 4);
  for (auto& t : m.loopUV) t = vec2(t.x * 0.8f, t.y * 0.2f);  // wide strip
  uv::rotate(m, uv::loopsOfFaces(m, {}), 0.6f, vec2(0.4f, 0.1f));
  m.computeUVTopology();
  uv::orientIslands(m, {}, uv::Orient::LongestAxis);
  Rect2 b = uv::boundsOfLoops(m, uv::loopsOfFaces(m, {}));
  CHECK(b.size().x > b.size().y * 3.5f);  // lies along U again
  auto st2 = uv::faceStretch(m);
  CHECK(std::abs(st2[0] - 4.f) < 0.05f);  // 0.8 / 0.2
}

TEST(uv, grow_and_shrink_selection) {
  Mesh m = makePlane(2.f, 5);
  std::vector<uint8_t> sel(m.faceCount(), 0);
  sel[12] = 1;  // center of 5x5
  uv::growSelection(m, sel);
  int n = 0;
  for (auto s : sel) n += s;
  CHECK(n == 5);  // the face and its 4 edge neighbors
  uv::growSelection(m, sel);
  uv::shrinkSelection(m, sel);
  n = 0;
  for (auto s : sel) n += s;
  CHECK(n == 5);
}

// ---------------------------------------------------------------- 0.7: rectify quad islands
#include "core/UVIsland.h"

// a belt: n quads along a half circle, width w, uneven lengths
static Mesh curvedStrip(int n = 12) {
  Mesh m;
  m.name = "Belt";
  m.faceStart = {0};
  for (int i = 0; i <= n; ++i) {
    float a = 3.14159f * float(i) / n + 0.1f * std::sin(i * 1.7f);
    for (int s = 0; s < 2; ++s) m.positions.push_back({std::cos(a) * 2.f, s * 0.4f, std::sin(a) * 2.f});
  }
  for (int i = 0; i < n; ++i) {
    uint32_t a = i * 2, b = (i + 1) * 2, c = b + 1, d = a + 1;
    m.loopVert.insert(m.loopVert.end(), {a, b, c, d});
    m.faceStart.push_back((uint32_t)m.loopVert.size());
  }
  m.loopUV.assign(m.loopVert.size(), vec2(0));
  m.finalize();
  return m;
}

TEST(uv, rectify_straightens_a_curved_strip) {
  Mesh m = curvedStrip();
  // a curved layout first (planar projection from above bends the strip into an arc)
  uv::projectPlanar(m, vec3(0, 1, 0));
  uv::fitToUnitSquare(m, uv::loopsOfFaces(m, {}), 0.05f);
  m.computeUVTopology();
  CHECK(m.islandCount == 1);
  auto r = uv::rectifyQuadIslands(m);
  CHECK(r.islands == 1 && r.skipped == 0);
  m.computeUVTopology();
  CHECK(m.islandCount == 1);
  // every quad edge runs along U or V; quads keep their 3D proportions
  float worstAxis = 0, worstRatio = 0;
  double s = -1;
  for (uint32_t f = 0; f < m.faceCount(); ++f) {
    uint32_t b = m.faceStart[f];
    for (int k = 0; k < 4; ++k) {
      vec2 d = m.loopUV[b + (k + 1) % 4] - m.loopUV[b + k];
      worstAxis = std::max(worstAxis, std::min(std::abs(d.x), std::abs(d.y)) / std::max(glm::length(d), 1e-9f));
      float l3 = glm::length(m.positions[m.loopVert[b + (k + 1) % 4]] - m.positions[m.loopVert[b + k]]);
      double ratio = glm::length(d) / l3;
      if (s < 0) s = ratio;
      worstRatio = std::max(worstRatio, float(std::abs(ratio / s - 1.0)));
    }
  }
  if (worstAxis > 1e-3f || worstRatio > 0.1f) std::fprintf(stderr, "  axis %.4f ratio %.3f\n", worstAxis, worstRatio);
  CHECK(worstAxis < 1e-3f);
  CHECK(worstRatio < 0.1f);
  CHECK(uv::overlapCount(m) == 0);
  // triangles are left alone
  Mesh sph = makeUVSphere(16, 8);
  auto r2 = uv::rectifyQuadIslands(sph);
  CHECK(r2.islands == 0 && r2.skipped >= 1);
}

TEST(uv, rectify_whole_torus_after_lscm) {
  Mesh t = makeTorus(1.f, 0.3f, 24, 8);  // seams along one ring and one tube loop: one rectangle
  uv::unwrapLSCM(t);
  t.computeUVTopology();
  CHECK(t.islandCount == 1);
  auto r = uv::rectifyQuadIslands(t);
  CHECK(r.islands == 1);
  float worstAxis = 0;
  for (uint32_t f = 0; f < t.faceCount(); ++f) {
    uint32_t b = t.faceStart[f];
    for (int k = 0; k < 4; ++k) {
      vec2 d = t.loopUV[b + (k + 1) % 4] - t.loopUV[b + k];
      worstAxis = std::max(worstAxis, std::min(std::abs(d.x), std::abs(d.y)) / std::max(glm::length(d), 1e-9f));
    }
  }
  CHECK(worstAxis < 1e-3f);
  CHECK(allInUnit(t));
}

TEST(uv, rectify_cylinder_with_seam) {
  Mesh t = makeTorus(1.f, 0.3f, 24, 8);  // quads; its UVs are a grid already, cut it up first
  uv::smartProject(t, 66.f);
  t.computeUVTopology();
  uint32_t before = t.islandCount;
  auto r = uv::rectifyQuadIslands(t);
  t.computeUVTopology();
  CHECK(r.islands + r.skipped == (int)before);
  CHECK(r.islands >= 1);
  CHECK(allInUnit(t));
  CHECK(uv::overlapCount(t) == 0);
}

// A symmetric island whose unwrapped half is bigger than its mirror copy (faces on the mirror
// plane have no partner): the stacked halves are not an overlap; copies follow edits of either
// half; drifted copies are restacked.
TEST(uv, mirror_links_follow_and_restack) {
  Mesh m = makePlane(2.f, 5);
  Bvh bvh;
  bvh.build(m);
  std::vector<uint32_t> all;
  for (uint32_t f = 0; f < m.faceCount(); ++f) all.push_back(f);
  int stacked = 0;
  auto r = uv::makeSymmetricIsland(m, bvh, all, 1, {}, &stacked);
  CHECK(r.ok);
  CHECK(stacked == 10);  // 5 rows x 2 columns on the -X side; the middle column has no partner
  m.computeUVTopology();
  CHECK(uv::overlappingIslands(m).empty());
  auto L = uv::mirrorLinks(m, bvh);
  CHECK(L.copies == 10);
  CHECK(L.stale.empty());
  CHECK(uv::unstackedCopies(m, L) == 0);
  uint32_t copy = 0;
  while (L.primary[copy] < 0) ++copy;
  uint32_t prim = (uint32_t)L.primary[copy];
  CHECK(m.faceFlags[copy] & FaceMirrorX);
  // editing only the copy moves the half it copies
  std::vector<vec2> uv = m.loopUV;
  for (uint32_t l = m.faceStart[copy]; l < m.faceStart[copy + 1]; ++l) uv[l] += vec2(0.1f, 0.f);
  uv::syncMirrorCopies(m, L, m.loopUV, uv, m.faceFlags);
  CHECK(glm::length(uv[m.faceStart[prim]] - m.loopUV[m.faceStart[prim]] - vec2(0.1f, 0.f)) < 1e-6f);
  // ...unless that half is pinned: then the copy goes back
  std::vector<uint8_t> flags = m.faceFlags;
  flags[prim] |= FacePinned;
  uv = m.loopUV;
  for (uint32_t l = m.faceStart[copy]; l < m.faceStart[copy + 1]; ++l) uv[l] += vec2(0.1f, 0.f);
  uv::syncMirrorCopies(m, L, m.loopUV, uv, flags);
  CHECK(uv == m.loopUV);
  // drifted copies are an overlap until restacked
  for (uint32_t f = 0; f < m.faceCount(); ++f)
    if (L.primary[f] >= 0)
      for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) m.loopUV[l] += vec2(0.03f, 0.f);
  m.computeUVTopology();
  CHECK(!uv::overlappingIslands(m).empty());
  CHECK(uv::unstackedCopies(m, L) == 10);
  CHECK(uv::restackMirrorCopies(m, L) == 10);
  m.computeUVTopology();
  CHECK(uv::overlappingIslands(m).empty());
  // older files (no axis bits) find their partners again
  for (auto& fl : m.faceFlags)
    if (fl & FaceMirrorCopy) fl = FaceMirrorCopy;
  auto L2 = uv::mirrorLinks(m, bvh);
  CHECK(L2.copies == 10);
  CHECK(L2.primary == L.primary);
}

// Pinned islands keep their place when everything else is repacked around them.
TEST(uv, pack_around_pinned_islands) {
  Mesh m = makeUVSphere(16, 8);
  uv::smartProject(m, 66.f);
  m.computeUVTopology();
  CHECK(m.islandCount > 3);
  // pin the island of face 0
  m.faceFlags.assign(m.faceCount(), 0);
  uint32_t isl = m.loopIsland[m.faceStart[0]];
  std::vector<vec2> keep = m.loopUV;
  for (uint32_t f = 0; f < m.faceCount(); ++f)
    if (m.loopIsland[m.faceStart[f]] == isl) m.faceFlags[f] |= FacePinned;
  // move the pinned island into the middle, where a plain pack would put others
  for (uint32_t f = 0; f < m.faceCount(); ++f)
    if (m.faceFlags[f] & FacePinned)
      for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) m.loopUV[l] = (m.loopUV[l] - vec2(0.5f)) * 0.5f + vec2(0.5f);
  keep = m.loopUV;
  uv::repackMovable(m);
  m.computeUVTopology();
  float moved = 0;
  for (uint32_t f = 0; f < m.faceCount(); ++f)
    if (m.faceFlags[f] & FacePinned)
      for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) moved = std::max(moved, glm::length(m.loopUV[l] - keep[l]));
  CHECK(moved == 0.f);
  CHECK(uv::overlappingIslands(m).empty());
  Rect2 b = uv::boundsOfLoops(m, uv::loopsOfFaces(m, {}));
  CHECK(b.lo.x >= -1e-4f && b.lo.y >= -1e-4f && b.hi.x <= 1.0001f && b.hi.y <= 1.0001f);
  // fixing overlaps moves the other island, never the pinned one
  for (uint32_t f = 0; f < m.faceCount(); ++f)
    if (!(m.faceFlags[f] & FacePinned) && m.loopIsland[m.faceStart[f]] != isl) {
      uint32_t other = m.loopIsland[m.faceStart[f]];
      Rect2 pb = uv::boundsOfLoops(m, uv::loopsOfFaces(m, {0}));
      std::vector<uint32_t> of;
      for (uint32_t g = 0; g < m.faceCount(); ++g)
        if (m.loopIsland[m.faceStart[g]] == other) of.push_back(g);
      auto loops = uv::loopsOfFaces(m, of);
      uv::translate(m, loops, pb.center() - uv::boundsOfLoops(m, loops).center());
      break;
    }
  m.computeUVTopology();
  CHECK(!uv::overlappingIslands(m).empty());
  uv::fixOverlaps(m);
  m.computeUVTopology();
  CHECK(uv::overlappingIslands(m).empty());
  moved = 0;
  for (uint32_t f = 0; f < m.faceCount(); ++f)
    if (m.faceFlags[f] & FacePinned)
      for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) moved = std::max(moved, glm::length(m.loopUV[l] - keep[l]));
  CHECK(moved == 0.f);
}

// Mirror partners on meshes where the face center's nearest surface point is unreliable: long
// thin faces on a tight curve, vertices split per face (as imported files often are), and a
// little noise. Every face must find the face made of its mirrored corners.
TEST(uv, mirror_faces_by_vertices) {
  Mesh base = makeTorus(1.f, 0.12f, 240, 6);  // thin, tightly curved quads
  // split every face's vertices (each face gets its own copies) and add a little noise
  Mesh m;
  m.name = "split";
  uint32_t seed = 7;
  auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return (float((seed >> 8) & 0xFFFF) / 65535.f - 0.5f); };
  m.faceStart.push_back(0);
  for (uint32_t f = 0; f < base.faceCount(); ++f) {
    for (uint32_t l = base.faceStart[f]; l < base.faceStart[f + 1]; ++l) {
      vec3 p = base.positions[base.loopVert[l]];
      m.positions.push_back(p);
      m.loopVert.push_back((uint32_t)m.positions.size() - 1);
      m.loopUV.push_back(base.loopUV[l]);
    }
    m.faceStart.push_back((uint32_t)m.loopVert.size());
  }
  // noise per original vertex (the same on all its split copies), mirrored halves independent
  std::vector<vec3> noise(base.positions.size());
  for (auto& n : noise) n = vec3(rnd(), rnd(), rnd()) * 2e-5f;
  for (uint32_t f = 0, k = 0; f < base.faceCount(); ++f)
    for (uint32_t l = base.faceStart[f]; l < base.faceStart[f + 1]; ++l, ++k) m.positions[k] += noise[base.loopVert[l]];
  m.finalize();
  Bvh bvh;
  bvh.build(m);
  // ground truth: the face whose corners are this face's corners mirrored in X
  auto truth = [&](uint32_t f) {
    int best = -1;
    float bd = 1e30f;
    for (uint32_t g = 0; g < m.faceCount(); ++g) {
      if (m.faceSize(g) != m.faceSize(f)) continue;
      float worst = 0;
      for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) {
        vec3 p = m.positions[m.loopVert[l]];
        p.x = -p.x;
        float nd = 1e30f;
        for (uint32_t k = m.faceStart[g]; k < m.faceStart[g + 1]; ++k) nd = std::min(nd, glm::length(m.positions[m.loopVert[k]] - p));
        worst = std::max(worst, nd);
      }
      if (worst < bd) bd = worst, best = (int)g;
    }
    return best;
  };
  int wrong = 0, checked = 0;
  for (uint32_t f = 0; f < m.faceCount(); f += 7) {
    ++checked;
    if (uv::mirrorFace(m, bvh, f, 1) != truth(f)) ++wrong;
  }
  if (wrong) std::fprintf(stderr, "  %d of %d mirror partners wrong\n", wrong, checked);
  CHECK(wrong == 0);
}

// A symmetric surface whose two halves are triangulated differently (the quads on both sides are
// split along the same world diagonal, so a triangle's mirror image is not a triangle of the
// mesh) - common in imported game models. Mirrored selection must still cover the mirrored
// region, and a symmetric island must stack all of it (no stray faces, no holes).
TEST(uv, symmetric_island_with_mismatched_triangulation) {
  const int N = 10;
  Mesh m;
  m.faceStart.push_back(0);
  auto idx = [&](int i, int j) { return (uint32_t)(j * (N + 1) + i); };
  for (int j = 0; j <= N; ++j)
    for (int i = 0; i <= N; ++i) {
      float x = -1.f + 2.f * i / N, z = -1.f + 2.f * j / N;
      m.positions.push_back(vec3(x, 0.3f * std::cos(x * 2.f) + 0.1f * z * z, z));  // curved, symmetric in X
    }
  auto tri = [&](uint32_t a, uint32_t b, uint32_t c) {
    for (uint32_t v : {a, b, c}) m.loopVert.push_back(v), m.loopUV.push_back(vec2(0));
    m.faceStart.push_back((uint32_t)m.loopVert.size());
  };
  auto quad = [&](uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    for (uint32_t v : {a, b, c, d}) m.loopVert.push_back(v), m.loopUV.push_back(vec2(0));
    m.faceStart.push_back((uint32_t)m.loopVert.size());
  };
  for (int j = 0; j < N; ++j)
    for (int i = 0; i < N; ++i) {
      uint32_t a = idx(i, j), b = idx(i + 1, j), c = idx(i + 1, j + 1), d = idx(i, j + 1);
      if ((i + j) % 3 == 0) {
        // the same world diagonal on both sides: not mirror symmetric
        tri(a, b, c);
        tri(a, c, d);
      } else {
        quad(a, b, c, d);
      }
    }
  m.finalize();
  Bvh bvh;
  bvh.build(m);
  // a patch on the +X side, and what its mirror should be
  std::vector<uint32_t> patch;
  for (uint32_t f = 0; f < m.faceCount(); ++f) {
    vec3 c = m.faceCenter(f);
    if (c.x > 0.25f && c.x < 0.9f && c.z > -0.6f && c.z < 0.5f) patch.push_back(f);
  }
  auto both = uv::withMirroredFaces(m, bvh, patch, 1);
  int expectNeg = 0, gotNeg = 0;
  for (uint32_t f = 0; f < m.faceCount(); ++f) {
    vec3 c = m.faceCenter(f);
    if (c.x < -0.25f && c.x > -0.9f && c.z > -0.6f && c.z < 0.5f) ++expectNeg;
  }
  for (uint32_t f : both) gotNeg += m.faceCenter(f).x < 0 ? 1 : 0;
  // the mirror of the patch, plus the other halves of triangle pairs cut by its border
  if (gotNeg < expectNeg || gotNeg > expectNeg + 4)
    std::fprintf(stderr, "  mirrored selection: %d faces, expected %d\n", gotNeg, expectNeg);
  CHECK(gotNeg >= expectNeg && gotNeg <= expectNeg + 4);
  expectNeg = gotNeg;
  // the symmetric island: every -X face is stacked, none is unwrapped on its own
  int stacked = 0;
  auto r = uv::makeSymmetricIsland(m, bvh, both, 1, {}, &stacked);
  CHECK(r.ok);
  if (stacked != expectNeg) std::fprintf(stderr, "  stacked %d of %d\n", stacked, expectNeg);
  if (stacked != expectNeg)
    for (uint32_t f : both)
      if (m.faceCenter(f).x < 0 && !(m.faceFlags[f] & FaceMirrorCopy)) {
        vec3 c = m.faceCenter(f);
        std::fprintf(stderr, "    not stacked: face %u size %u center %.2f %.2f, mirror ->", f, m.faceSize(f), c.x, c.z);
        for (uint32_t g : uv::mirrorFaces(m, bvh, f, 1)) {
          vec3 d = m.faceCenter(g);
          bool inPatch = std::find(both.begin(), both.end(), g) != both.end();
          std::fprintf(stderr, " %u(%.2f %.2f%s)", g, d.x, d.z, inPatch ? " sel" : "");
        }
        std::fprintf(stderr, "\n");
      }
  CHECK(stacked == expectNeg);
  m.computeUVTopology();
  // one island for the unwrapped half, one for the stacked half: no stray pieces
  std::vector<uint8_t> selIsl(m.islandCount, 0);
  for (uint32_t f : both) selIsl[m.loopIsland[m.faceStart[f]]] = 1;
  CHECK(std::count(selIsl.begin(), selIsl.end(), 1) == 2);
  CHECK(uv::overlappingIslands(m).empty());
  auto L = uv::mirrorLinks(m, bvh);
  CHECK((int)L.copies == expectNeg);
  CHECK(uv::unstackedCopies(m, L) == 0);
}

// Halves that differ in shape (one side warped by more than half an edge, as on a posed or
// sculpted model) but are built the same way: mirror partners follow the mesh's connections.
TEST(uv, mirror_faces_on_an_asymmetric_shape) {
  const int N = 12;
  Mesh m;
  m.faceStart.push_back(0);
  auto idx = [&](int i, int j) { return (uint32_t)(j * (N + 1) + i); };
  for (int j = 0; j <= N; ++j)
    for (int i = 0; i <= N; ++i) {
      float x = -1.f + 2.f * i / N, z = -1.f + 2.f * j / N, y = 0.25f * std::cos(x * 2.f);
      if (x > 0) {  // the +X half is warped: up to 0.7 of an edge length, smoothly from the middle
        float k = std::min(1.f, x * 4.f);
        x += 0.12f * std::sin(z * 2.5f) * k;
        y += 0.1f * std::sin(z * 3.f + 1.f) * k;
        z += 0.06f * std::cos(x * 4.f) * k;
      }
      m.positions.push_back(vec3(x, y, z));
    }
  for (int j = 0; j < N; ++j)
    for (int i = 0; i < N; ++i) {
      for (uint32_t v : {idx(i, j), idx(i + 1, j), idx(i + 1, j + 1), idx(i, j + 1)}) m.loopVert.push_back(v), m.loopUV.push_back(vec2(0));
      m.faceStart.push_back((uint32_t)m.loopVert.size());
    }
  m.finalize();
  Bvh bvh;
  bvh.build(m);
  int wrong = 0, total = 0;
  for (int j = 0; j < N; ++j)
    for (int i = 0; i < N / 2; ++i) {  // -X half: face (i, j) mirrors onto (N-1-i, j)
      uint32_t f = uint32_t(j * N + i), g = uint32_t(j * N + (N - 1 - i));
      auto mf = uv::mirrorFaces(m, bvh, f, 1);
      ++total;
      if (mf.size() != 1 || mf[0] != g) ++wrong;
    }
  if (wrong) std::fprintf(stderr, "  %d of %d mirror partners wrong\n", wrong, total);
  CHECK(wrong == 0);
  // and a symmetric island stacks the whole mirrored half
  std::vector<uint32_t> all;
  for (uint32_t f = 0; f < m.faceCount(); ++f) all.push_back(f);
  int stacked = 0;
  auto r = uv::makeSymmetricIsland(m, bvh, all, 1, {}, &stacked);
  CHECK(r.ok);
  CHECK(stacked == N * N / 2);
}

// halves triangulated along different diagonals AND out of shape by a whole edge length (plus noise):
// every mirrored corner must be stacked on the corner at the true mirror vertex
TEST(uv, symmetric_island_on_a_warped_mismatched_mesh) {
  const int N = 16;
  const float e = 2.f / N;
  Mesh m;
  m.faceStart.push_back(0);
  auto idx = [&](int i, int j) { return (uint32_t)(j * (N + 1) + i); };
  uint32_t seed = 7;
  auto rnd = [&] { seed = seed * 1103515245u + 12345u; return float((seed >> 8) & 0xffff) / 65535.f - 0.5f; };
  for (int j = 0; j <= N; ++j)
    for (int i = 0; i <= N; ++i) {
      float x = -1.f + 2.f * i / N, z = -1.f + 2.f * j / N, y = 0.25f * std::cos(x * 2.f);
      if (x > 0) {
        float k = std::min(1.f, x * 4.f);
        x += 1.0f * e * std::sin(z * 2.5f) * k + 0.3f * e * rnd() * k;
        y += 0.6f * e * std::sin(z * 3.f + 1.f) * k + 0.3f * e * rnd() * k;
        z += 0.3f * e * rnd() * k;
      }
      m.positions.push_back(vec3(x, y, z));
    }
  for (int j = 0; j < N; ++j)
    for (int i = 0; i < N; ++i) {
      uint32_t a = idx(i, j), b = idx(i + 1, j), c = idx(i + 1, j + 1), d = idx(i, j + 1);
      std::vector<std::array<uint32_t, 3>> tri;
      if ((i + j) % 3 == 0) tri = {{a, b, c}, {a, c, d}};
      else tri = {{a, b, d}, {b, c, d}};
      for (auto& t : tri) {
        for (uint32_t v : t) m.loopVert.push_back(v), m.loopUV.push_back(vec2(0));
        m.faceStart.push_back((uint32_t)m.loopVert.size());
      }
    }
  m.finalize();
  Bvh bvh;
  bvh.build(m);
  std::vector<uint32_t> all;
  for (uint32_t f = 0; f < m.faceCount(); ++f) all.push_back(f);
  int stacked = 0;
  auto r = uv::makeSymmetricIsland(m, bvh, all, 1, {}, &stacked);
  CHECK(r.ok);
  CHECK(stacked > (int)m.faceCount() / 2 - 4);
  // UV per vertex on the primary half (one chart: every corner at a vertex agrees)
  std::vector<vec2> vuv(m.positions.size(), vec2(-1));
  for (uint32_t f = 0; f < m.faceCount(); ++f)
    if (!(m.faceFlags[f] & FaceMirrorCopy))
      for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) vuv[m.loopVert[l]] = m.loopUV[l];
  int wrong = 0, corners = 0;
  for (uint32_t f = 0; f < m.faceCount(); ++f) {
    if (!(m.faceFlags[f] & FaceMirrorCopy)) continue;
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) {
      uint32_t v = m.loopVert[l];
      int i = int(v % (N + 1)), j = int(v / (N + 1));
      ++corners;
      if (glm::length(m.loopUV[l] - vuv[idx(N - i, j)]) > 1e-5f) ++wrong;
    }
  }
  if (wrong) std::fprintf(stderr, "  %d of %d mirrored corners on the wrong vertex\n", wrong, corners);
  CHECK(corners > 0 && wrong == 0);
  // the links found later agree with the stacking
  auto L = uv::mirrorLinks(m, bvh);
  CHECK(L.copies == stacked);
  CHECK(uv::unstackedCopies(m, L) == 0);
}

// (0.43) a cloth that is symmetric but not on the plane: the vertices down the middle wander off it (a fifth
// of an edge), the lower rows are shifted sideways (the panel is symmetric about x = 0.15 edge), and the cloth
// has thickness (a second layer under it, joined round the rim by edges a twelfth as long). Up to 0.42.0 only
// vertices within 1e-4 of the model's size counted as on the middle, so the faces along it were never stacked
// (Randall's Mai body and dress: the whole center line, front and back)
TEST(uv, symmetric_island_with_a_wandering_middle_and_thickness) {
  const int N = 16;
  const float e = 2.f / N, th = e / 12.f;
  Mesh m;
  m.faceStart.push_back(0);
  auto idx = [&](int layer, int i, int j) { return (uint32_t)(layer * (N + 1) * (N + 1) + j * (N + 1) + i); };
  for (int layer = 0; layer < 2; ++layer)
    for (int j = 0; j <= N; ++j)
      for (int i = 0; i <= N; ++i) {
        float x = -1.f + 2.f * i / N, z = -1.f + 2.f * j / N, y = 0.25f * std::cos(x * 2.f) - layer * th;
        if (i == N / 2) x += 0.2f * e * std::sin(z * 3.f + 0.5f);  // the middle wanders
        if (j > N / 2) x += 0.15f * e;                             // the lower rows: off to one side
        m.positions.push_back(vec3(x, y, z));
      }
  auto quad = [&](uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    for (uint32_t v : {a, b, c, d}) m.loopVert.push_back(v), m.loopUV.push_back(vec2(0));
    m.faceStart.push_back((uint32_t)m.loopVert.size());
  };
  for (int j = 0; j < N; ++j)
    for (int i = 0; i < N; ++i) {
      quad(idx(0, i, j), idx(0, i, j + 1), idx(0, i + 1, j + 1), idx(0, i + 1, j));  // top, facing up
      quad(idx(1, i, j), idx(1, i + 1, j), idx(1, i + 1, j + 1), idx(1, i, j + 1));  // under it, facing down
    }
  for (int k = 0; k < N; ++k) {  // the rim
    quad(idx(0, k, 0), idx(0, k + 1, 0), idx(1, k + 1, 0), idx(1, k, 0));
    quad(idx(0, k + 1, N), idx(0, k, N), idx(1, k, N), idx(1, k + 1, N));
    quad(idx(0, 0, k + 1), idx(0, 0, k), idx(1, 0, k), idx(1, 0, k + 1));
    quad(idx(0, N, k), idx(0, N, k + 1), idx(1, N, k + 1), idx(1, N, k));
  }
  m.finalize();
  Bvh bvh;
  bvh.build(m);
  // every vertex finds its mirror image: across the middle, or itself on it
  auto mv = uv::mirrorVertices(m, bvh, 1);
  int wrongV = 0;
  for (int layer = 0; layer < 2; ++layer)
    for (int j = 0; j <= N; ++j)
      for (int i = 0; i <= N; ++i)
        if (mv[idx(layer, i, j)] != (int)idx(layer, N - i, j)) ++wrongV;
  if (wrongV) std::fprintf(stderr, "  %d of %d vertices without their mirror image\n", wrongV, 2 * (N + 1) * (N + 1));
  CHECK(wrongV == 0);
  std::vector<uint32_t> all;
  for (uint32_t f = 0; f < m.faceCount(); ++f) all.push_back(f);
  int negative = 0;
  for (uint32_t f = 0; f < m.faceCount(); ++f) negative += m.faceCenter(f).x < -m.boundingRadius * 1e-4f;
  int stacked = 0;
  auto r = uv::makeSymmetricIsland(m, bvh, all, 1, {}, &stacked);
  CHECK(r.ok);
  if (stacked != negative) std::fprintf(stderr, "  %d of %d faces on the -X side stacked\n", stacked, negative);
  CHECK(stacked == negative);
  // and each copy corner sits on the corner of its mirror image
  std::vector<vec2> vuv(m.positions.size(), vec2(-1));
  for (uint32_t f = 0; f < m.faceCount(); ++f)
    if (!(m.faceFlags[f] & FaceMirrorCopy))
      for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) vuv[m.loopVert[l]] = m.loopUV[l];
  int wrong = 0;
  for (uint32_t f = 0; f < m.faceCount(); ++f) {
    if (!(m.faceFlags[f] & FaceMirrorCopy)) continue;
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) {
      uint32_t v = m.loopVert[l], per = (N + 1) * (N + 1), layer = v / per, i = v % per % (N + 1), j = v % per / (N + 1);
      if (glm::length(m.loopUV[l] - vuv[idx(layer, N - i, j)]) > 1e-5f) ++wrong;
    }
  }
  CHECK(wrong == 0);
}

// (0.43) the Island brush's Surface reach on a cloth with thickness: from the top layer it spreads as far as its
// radius along the surface and never onto the layer under it, which is only reached round the rim; faces the
// filter refuses stop it
TEST(uv, faces_along_the_surface_stay_on_their_layer) {
  const int N = 20;
  const float e = 1.f / N, th = e / 10.f;
  Mesh m;
  m.faceStart.push_back(0);
  auto idx = [&](int layer, int i, int j) { return (uint32_t)(layer * (N + 1) * (N + 1) + j * (N + 1) + i); };
  for (int layer = 0; layer < 2; ++layer)
    for (int j = 0; j <= N; ++j)
      for (int i = 0; i <= N; ++i) m.positions.push_back(vec3(float(i) * e, -layer * th, float(j) * e));
  auto quad = [&](uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    for (uint32_t v : {a, b, c, d}) m.loopVert.push_back(v), m.loopUV.push_back(vec2(0));
    m.faceStart.push_back((uint32_t)m.loopVert.size());
  };
  for (int j = 0; j < N; ++j)
    for (int i = 0; i < N; ++i) {
      quad(idx(0, i, j), idx(0, i, j + 1), idx(0, i + 1, j + 1), idx(0, i + 1, j));
      quad(idx(1, i, j), idx(1, i + 1, j), idx(1, i + 1, j + 1), idx(1, i, j + 1));
    }
  for (int k = 0; k < N; ++k) {
    quad(idx(0, k, 0), idx(0, k + 1, 0), idx(1, k + 1, 0), idx(1, k, 0));
    quad(idx(0, k + 1, N), idx(0, k, N), idx(1, k, N), idx(1, k + 1, N));
    quad(idx(0, 0, k + 1), idx(0, 0, k), idx(1, 0, k), idx(1, 0, k + 1));
    quad(idx(0, N, k), idx(0, N, k + 1), idx(1, N, k + 1), idx(1, N, k));
  }
  m.finalize();
  FaceGraph g;
  g.build(m);
  CHECK(g.matches(m));
  auto layerOf = [&](uint32_t f) { return m.faceCenter(f).y < -th * 0.5f ? 1 : m.faceCenter(f).y > -th * 0.1f ? 0 : 2; };
  // the middle of the top layer: a disc of the radius (centers within 4 edges: about pi 4^2 = 50 faces)
  uint32_t mid = uint32_t(2 * ((N / 2) * N + N / 2));
  vec3 p = m.faceCenter(mid);
  auto a = facesAlongSurface(m, g, mid, p, 4.f * e);
  int top = 0, other = 0, far = 0;
  for (uint32_t f : a) {
    (layerOf(f) == 0 ? top : other)++;
    if (glm::length(m.faceCenter(f) - p) > 4.f * e * 1.001f) ++far;
  }
  std::fprintf(stderr, "  middle: %d faces on the top layer, %d elsewhere\n", top, other);
  CHECK(other == 0 && far == 0 && top >= 40 && top <= 52);
  // by the rim: round it onto the layer under, unless the faces turned away (down) are refused
  uint32_t edge = uint32_t(2 * ((N / 2) * N + 0));
  p = m.faceCenter(edge);
  auto b = facesAlongSurface(m, g, edge, p, 3.f * e);
  int under = 0;
  for (uint32_t f : b) under += layerOf(f) == 1;
  CHECK(under > 0);
  auto c = facesAlongSurface(m, g, edge, p, 3.f * e, [&](uint32_t f) { return m.faceNormal(f).y > -0.5f; });
  under = 0;
  int rim = 0;
  for (uint32_t f : c) under += layerOf(f) == 1, rim += layerOf(f) == 2;
  CHECK(under == 0 && rim > 0);
}

// islands with concave outlines (L shapes and arcs): packing their outlines nests them into each
// other's gaps, which bounding boxes can't; and pinned islands are packed around
namespace {
Mesh makeConcaveIslands(int n, uint64_t seed) {
  Mesh m;
  Rng rng(seed);
  m.faceStart = {0};
  // a grid of shared vertices per island; quads reference them, so each island is connected
  auto island = [&](int cols, int rows, auto&& keep, auto&& place) {
    uint32_t base = (uint32_t)m.positions.size();
    std::vector<vec2> uv;
    for (int j = 0; j <= rows; ++j)
      for (int i = 0; i <= cols; ++i) {
        auto [p3, p2] = place(i, j);
        m.positions.push_back(p3);
        uv.push_back(p2);
      }
    auto id = [&](int i, int j) { return base + uint32_t(j * (cols + 1) + i); };
    for (int j = 0; j < rows; ++j)
      for (int i = 0; i < cols; ++i) {
        if (!keep(i, j)) continue;
        for (uint32_t v : {id(i, j), id(i + 1, j), id(i + 1, j + 1), id(i, j + 1)}) {
          m.loopVert.push_back(v);
          m.loopUV.push_back(uv[v - base]);
        }
        m.faceStart.push_back((uint32_t)m.loopVert.size());
      }
  };
  for (int i = 0; i < n; ++i) {
    float z = float(i) * 3.f, s = 0.3f + rng.uniform();
    vec2 o(rng.uniform() * 0.5f, rng.uniform() * 0.5f);
    if (i % 2 == 0) {  // an L: three of four squares
      island(2, 2, [](int x, int y) { return !(x == 1 && y == 1); },
             [&](int x, int y) { return std::pair{vec3(x * s, y * s, z), o + vec2(x, y) * 0.1f}; });
    } else {  // a half ring of 8 quads
      island(8, 1, [](int, int) { return true; }, [&](int k, int r) {
        float a = kPi * k / 8, rad = (r ? 1.6f : 1.f) * s;
        vec2 p(std::cos(a) * rad, std::sin(a) * rad);
        return std::pair{vec3(p, z), o + p * 0.08f};
      });
    }
  }
  m.finalize();
  return m;
}
}  // namespace

TEST(uv, pack_shapes_nests_concave_islands) {
  Mesh a = makeConcaveIslands(60, 5), b = a;
  uv::PackOptions boxes;
  boxes.shapes = false;
  uv::pack(a, boxes);
  double t0 = nowSeconds();
  uv::pack(b);  // shapes (default)
  double dt = (nowSeconds() - t0) * 1000.0;
  a.computeUVTopology();
  b.computeUVTopology();
  float ca = uv::coverage(a, 512), cb = uv::coverage(b, 512);
  std::fprintf(stderr, "  60 concave islands: boxes %.3f, shapes %.3f (%.0f ms)\n", ca, cb, dt);
  if (const char* dump = std::getenv("PATINA_DUMP_PACK")) {
    for (const Mesh* mm : {&a, &b}) {
      FILE* fo = std::fopen((std::string(dump) + (mm == &a ? "_boxes.txt" : "_shapes.txt")).c_str(), "w");
      for (uint32_t f = 0; f < mm->faceCount(); ++f) {
        for (uint32_t l = mm->faceStart[f]; l < mm->faceStart[f + 1]; ++l) std::fprintf(fo, "%f %f ", mm->loopUV[l].x, mm->loopUV[l].y);
        std::fprintf(fo, "\n");
      }
      std::fclose(fo);
    }
  }
  CHECK(b.islandCount == a.islandCount);
  CHECK(allInUnit(b));
  CHECK(uv::overlapCount(b, 1024) == 0);
  CHECK(cb > ca * 1.15f);  // clearly tighter
  float d0 = uv::texelDensity(b, {0}), d1 = uv::texelDensity(b, {b.faceCount() - 1});
  CHECK_NEAR(d0, d1, 0.02 * d0);  // equal texel density kept
  // pin the first island: it stays put, the rest goes around it without overlapping
  Mesh c = b;
  c.faceFlags.assign(c.faceCount(), 0);
  for (uint32_t f = 0; f < 3; ++f) c.faceFlags[f] = FacePinned;  // the first L
  std::vector<vec2> pinnedUV(c.loopUV.begin(), c.loopUV.begin() + 12);
  CHECK(uv::packAroundFixed(c, {}, 0.005f));
  c.computeUVTopology();
  CHECK(std::equal(pinnedUV.begin(), pinnedUV.end(), c.loopUV.begin()));
  CHECK(uv::overlapCount(c, 1024) == 0);
  CHECK(allInUnit(c));
}

namespace {
// an open cylinder (with caps) along Y, quads, vertices shared; split at the seam like an export
Mesh makeCappedCylinder(int seg = 32, int rows = 8, float r = 0.5f, float h = 2.f) {
  Mesh m;
  m.faceStart = {0};
  auto id = [&](int i, int j) { return uint32_t(j * seg + (i % seg)); };
  for (int j = 0; j <= rows; ++j)
    for (int i = 0; i < seg; ++i) {
      float a = 2.f * kPi * i / seg;
      m.positions.push_back(vec3(std::cos(a) * r, -h / 2 + h * j / rows, std::sin(a) * r));
    }
  auto face = [&](std::vector<uint32_t> vs) {
    for (uint32_t v : vs) m.loopVert.push_back(v), m.loopUV.push_back(vec2(0));
    m.faceStart.push_back((uint32_t)m.loopVert.size());
  };
  for (int j = 0; j < rows; ++j)
    for (int i = 0; i < seg; ++i) face({id(i, j), id(i, j + 1), id(i + 1, j + 1), id(i + 1, j)});
  // caps: fans around a center vertex
  for (int end = 0; end < 2; ++end) {
    uint32_t c = (uint32_t)m.positions.size();
    m.positions.push_back(vec3(0, end ? h / 2 : -h / 2, 0));
    for (int i = 0; i < seg; ++i) {
      uint32_t a = id(i, end ? rows : 0), b = id(i + 1, end ? rows : 0);
      if (end) face({c, b, a});
      else face({c, a, b});
    }
  }
  m.finalize();
  return m;
}
}  // namespace

TEST(uv, seam_path_follows_edges_across_split_vertices) {
  // every face with vertices of its own (like a flat-shaded export): the path must still run
  // across faces, because vertices at one position count as one
  Mesh src = makeUVSphere(16, 8), m;
  m.faceStart = {0};
  for (uint32_t f = 0; f < src.faceCount(); ++f) {
    for (uint32_t l = src.faceStart[f]; l < src.faceStart[f + 1]; ++l) {
      m.loopVert.push_back((uint32_t)m.positions.size());
      m.positions.push_back(src.positions[src.loopVert[l]]);
      m.loopUV.push_back(src.loopUV[l]);
    }
    m.faceStart.push_back((uint32_t)m.loopVert.size());
  }
  m.finalize();
  uv::SeamGraph g;
  g.build(m);
  CHECK(g.matches(m));
  CHECK(g.pos.size() * 3 < m.positions.size());  // the duplicates are welded
  // from the south pole to the north pole: a meridian of 8 edges
  uint32_t south = 0, north = 0;
  for (uint32_t v = 0; v < g.pos.size(); ++v) {
    if (g.pos[v].y < g.pos[south].y) south = v;
    if (g.pos[v].y > g.pos[north].y) north = v;
  }
  auto p = g.path(south, north);
  std::fprintf(stderr, "  pole to pole: %zu edges\n", p.size());
  CHECK(p.size() == 8u);
  // marking it sets seams on mesh edges, including duplicated ones
  std::vector<uint8_t> flags(m.edgeFlags.size(), 0);
  g.apply(flags, p, true);
  int marked = 0;
  for (auto f : flags) marked += (f & EdgeSeam) ? 1 : 0;
  CHECK(marked >= 8);
  g.apply(flags, p, false);
  marked = 0;
  for (auto f : flags) marked += (f & EdgeSeam) ? 1 : 0;
  CHECK(marked == 0);
  // a different mesh is noticed
  Mesh other = makeUVSphere(12, 6);
  CHECK(!g.matches(other));
}

TEST(uv, cylinder_and_sphere_unwraps) {
  {
    Mesh m = makeCappedCylinder();
    uv::projectCylinder(m);
    m.computeUVTopology();
    std::fprintf(stderr, "  cylinder: %u islands, angle error %.3f, flipped %d, seams %d\n", m.islandCount,
                 meanAngleError(m), flippedTris(m), seamCount(m));
    CHECK(m.islandCount == 3u);  // the side (cut once) and the two caps
    CHECK(allInUnit(m));
    CHECK(flippedTris(m) == 0);
    CHECK(meanAngleError(m) < 0.02);
    CHECK(uv::overlapCount(m, 512) == 0);
  }
  {
    // an arm: tapered and bent so far that a straight axis leaves the tube near its end
    Mesh m = makeCappedCylinder(32, 12, 0.35f, 3.f);
    for (auto& p : m.positions) {
      float t = (p.y + 1.5f) / 3.f;
      float r = 1.f - 0.35f * t;
      p.x = p.x * r + 0.5f * t * t;
      p.z *= r;
    }
    m.finalize();
    uv::projectCylinder(m);
    m.computeUVTopology();
    std::fprintf(stderr, "  bent tube: %u islands, angle error %.3f, flipped %d, overlaps %d\n", m.islandCount,
                 meanAngleError(m), flippedTris(m), uv::overlapCount(m, 512));
    CHECK(m.islandCount == 3u);
    CHECK(flippedTris(m) == 0);
    CHECK(uv::overlapCount(m, 512) == 0);
    CHECK(meanAngleError(m) < 0.05);
  }
  {
    Mesh m = makeUVSphere(32, 16);
    for (auto& uv : m.loopUV) uv = vec2(0);
    uv::projectSphere(m);
    m.computeUVTopology();
    std::fprintf(stderr, "  sphere: %u islands, flipped %d\n", m.islandCount, flippedTris(m));
    CHECK(m.islandCount == 1u);
    CHECK(allInUnit(m));
    CHECK(flippedTris(m) == 0);
  }
}

TEST(uv, relax_removes_stretch_and_keeps_the_island_in_place) {
  Mesh m = makePlane(2.f, 10);
  // a sheared, squashed layout in a corner of the texture
  for (auto& uv : m.loopUV) uv = vec2(0.1f + uv.x * 0.3f + uv.y * 0.15f, 0.2f + uv.y * 0.12f);
  m.computeUVTopology();
  double before = meanAngleError(m);
  Rect2 b0 = uv::boundsOfLoops(m, uv::loopsOfFaces(m, {}));
  double a0 = 0;
  for (uint32_t f = 0; f < m.faceCount(); ++f) a0 += m.faceAreaUV(f);
  int n = uv::relaxIslands(m, {0});
  m.computeUVTopology();
  double after = meanAngleError(m);
  Rect2 b1 = uv::boundsOfLoops(m, uv::loopsOfFaces(m, {}));
  double a1 = 0;
  for (uint32_t f = 0; f < m.faceCount(); ++f) a1 += m.faceAreaUV(f);
  std::fprintf(stderr, "  relax: angle error %.3f -> %.4f, area %.4f -> %.4f\n", before, after, a0, a1);
  CHECK(n == 1);
  CHECK(after < before * 0.1);
  CHECK(std::abs(a1 - a0) < 1e-4 * std::abs(a0) + 1e-9);   // same size
  CHECK(glm::length(b1.center() - b0.center()) < 0.05f);  // same place
  CHECK(flippedTris(m) == 0);
  CHECK(m.islandCount == 1u);
}

// 0.11.1: a face whose UVs collapsed onto one point "contained" every point, so it caught every
// click in the UV editor (Randall's character: island clicks selected one stray face instead)
TEST(uv, collapsed_uv_faces_contain_nothing) {
  vec2 p(0.4f, 0.3f), c(1.18f, -0.69f);
  CHECK(!pointInTriangle2(c, c, c, p));                                   // all three corners on one point
  CHECK(!pointInTriangle2(vec2(0), vec2(0.5f, 0.5f), vec2(1, 1), vec2(0.25f, 0.25f)));  // on one line
  CHECK(pointInTriangle2(vec2(0), vec2(1, 0), vec2(0, 1), p));            // counter-clockwise
  CHECK(pointInTriangle2(vec2(0), vec2(0, 1), vec2(1, 0), p));            // clockwise (mirrored islands)
  CHECK(!pointInTriangle2(vec2(0), vec2(1, 0), vec2(0, 1), vec2(0.8f, 0.8f)));
  Mesh m = makePlane(2.f, 2);  // 4 quads
  for (uint32_t l = m.faceStart[3]; l < m.faceStart[4]; ++l) m.loopUV[l] = c;
  m.computeUVTopology();
  auto bad = uv::collapsedFaces(m);
  CHECK(bad.size() == 1 && bad[0] == 3);
}

TEST(uv, tangents_made_in_parallel_match_those_made_in_one_go) {
  // a big ball, cut into many charts: its tangents by groups of islands on threads, and in one go
  Mesh m = makeUVSphere(420, 300, 1.f);
  uv::smartProject(m, 30.f);
  m.computeUVTopology();
  CHECK(m.tris.size() > 200000 && m.islandCount > 8);
  m.computeTangents();
  std::vector<vec4> parallel = m.loopTangent;
  setenv("PATINA_SERIAL_TANGENTS", "1", 1);
  m.computeTangents();
  unsetenv("PATINA_SERIAL_TANGENTS");
  float worst = 0;
  for (size_t i = 0; i < parallel.size(); ++i) worst = std::max(worst, glm::length(parallel[i] - m.loopTangent[i]));
  std::fprintf(stderr, "  %zu triangles, %u islands: largest difference %g\n", m.tris.size(), m.islandCount, worst);
  CHECK(worst < 1e-5f);
}

// (0.44) A mesh that comes back without its UVs after an edit (Blender in Edit Mode): Patina's UVs carried over
// by where the faces are. A sheet in two islands (a seam down the middle), then the same sheet with a face
// deleted and the faces in another order: every face keeps its UVs, the islands stay two. A vertex moved: the
// faces around it don't match, and still land in their island.
TEST(uv, uvs_carried_over_an_edit) {
  const int N = 10;
  auto sheet = [&](bool edited) {
    Mesh m;
    m.faceStart.push_back(0);
    for (int j = 0; j <= N; ++j)
      for (int i = 0; i <= N; ++i) m.positions.push_back(vec3(float(i), 0.f, float(j)));
    std::vector<std::array<uint32_t, 4>> quads;
    for (int j = 0; j < N; ++j)
      for (int i = 0; i < N; ++i) {
        uint32_t a = j * (N + 1) + i;
        quads.push_back({a, a + N + 1, a + N + 2, a + 1});
      }
    if (edited) {
      quads.erase(quads.begin() + 2 * N + 3);  // a hole in the left half
      std::reverse(quads.begin(), quads.end());  // other face order
    }
    for (auto& q : quads) {
      for (uint32_t v : q) {
        m.loopVert.push_back(v);
        vec3 p = m.positions[v];
        // two islands: the left half and the right half, apart in UV space
        bool right = false;
        for (uint32_t w : q) right |= m.positions[w].x > N / 2 + 0.01f;
        m.loopUV.push_back(right ? vec2(0.55f + 0.04f * (p.x - N / 2), 0.1f * p.z) : vec2(0.04f * p.x, 0.1f * p.z));
      }
      m.faceStart.push_back((uint32_t)m.loopVert.size());
    }
    m.finalize();
    return m;
  };
  Mesh before = sheet(false);
  CHECK(before.islandCount == 2u);
  Mesh after = sheet(true);
  std::vector<vec2> want = after.loopUV;
  after.loopUV.assign(after.loopCount(), vec2(0.f));
  float share = uv::carryUVs(before, after);
  after.computeUVTopology();
  float worst = 0.f;
  for (size_t l = 0; l < want.size(); ++l) worst = std::max(worst, glm::length(after.loopUV[l] - want[l]));
  std::fprintf(stderr, "  carried: %.3f of the faces matched, %u islands, worst UV off by %g\n", share,
               after.islandCount, worst);
  CHECK(share > 0.999f && after.islandCount == 2u && worst < 1e-6f);
  // a face that changed (one corner moved off the old grid): it still lands in its island
  Mesh moved = sheet(false);
  moved.positions[3 * (N + 1) + 2] += vec3(0.3f, 0.f, 0.2f);
  moved.loopUV.assign(moved.loopCount(), vec2(0.f));
  share = uv::carryUVs(before, moved);
  moved.computeUVTopology();
  std::fprintf(stderr, "  one vertex moved: %.3f matched, %u islands\n", share, moved.islandCount);
  CHECK(share > 0.95f && share < 1.f && moved.islandCount == 2u);
}

// (0.47) A tiny face in a dense part of the layout (the sample's Suzanne has 58: a UV area of about 1e-8, far from
// the origin) has UVs; only a face whose UVs lie on a line or a point has none. The area used to be summed on the
// corners' own coordinates, whose rounding lost it.
TEST(uv, tiny_faces_far_from_the_origin_have_uv_area) {
  Mesh m;
  m.positions = {vec3(0, 0, 0), vec3(1, 0, 0), vec3(0, 0, 1), vec3(2, 0, 0), vec3(3, 0, 0), vec3(2, 0, 1)};
  m.faceStart = {0, 3, 6};
  m.loopVert = {0, 1, 2, 3, 4, 5};
  const vec2 o(0.420512f, 0.442190f);
  m.loopUV = {o, o + vec2(-1.23e-4f, 2.45e-4f), o + vec2(-9e-6f, 1.9e-4f),            // tiny, but a triangle
              vec2(0.3f, 0.3f), vec2(0.4f, 0.4f), vec2(0.35f, 0.35f)};                // on a line: no area
  m.finalize();
  CHECK(!m.faceHasNoUVArea(0, m.loopUV));
  CHECK(m.faceHasNoUVArea(1, m.loopUV));
  auto c = uv::collapsedFaces(m);
  CHECK(c.size() == 1 && c[0] == 1);
}
