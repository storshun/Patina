// Retopology (0.13): patches painted on a high-poly mesh become quads at the density asked for.
#include <cmath>
#include <map>
#include <set>
#include <tuple>

#include "Test.h"
#include "core/Bvh.h"
#include "core/Mesh.h"
#include "core/Retopo.h"

using namespace patina;

namespace {

// every edge of a closed all-quad mesh has two faces, every face four corners
bool closedQuads(const Mesh& m) {
  std::map<std::pair<uint32_t, uint32_t>, int> uses;
  for (uint32_t f = 0; f < m.faceCount(); ++f) {
    if (m.faceSize(f) != 4) return false;
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) {
      uint32_t a = m.loopVert[l], b = m.loopVert[m.nextLoop(l, f)];
      uses[{std::min(a, b), std::max(a, b)}]++;
    }
  }
  for (auto& [e, n] : uses)
    if (n != 2) return false;
  return true;
}

float maxDistToSphere(const Mesh& m, float r) {
  float d = 0;
  for (auto& p : m.positions) d = std::max(d, std::abs(glm::length(p) - r));
  return d;
}

}  // namespace

TEST(retopo, sphere_as_a_cube_of_six_patches) {
  Mesh hi = makeUVSphere(96, 48, 0.5f);
  Bvh bvh;
  bvh.build(hi);
  // six patches by the dominant axis of each face's center: a cube projected onto the sphere
  std::vector<int32_t> label(hi.faceCount());
  for (uint32_t f = 0; f < hi.faceCount(); ++f) {
    vec3 c = hi.faceCenter(f), a = glm::abs(c);
    int axis = a.x >= a.y && a.x >= a.z ? 0 : a.y >= a.z ? 1 : 2;
    label[f] = 1 + axis * 2 + (c[axis] < 0 ? 1 : 0);
  }
  retopo::Options o;
  o.quadsPerM2 = 400;  // 5 cm quads on a sphere of 3.14 m²: about 1250 quads
  retopo::Result r = retopo::build(hi, bvh, label, {}, o);
  for (auto& p : r.problems) std::fprintf(stderr, "  %s\n", p.c_str());
  CHECK(r.problems.empty());
  CHECK(r.patches.size() == 6);
  for (auto& p : r.patches) {
    if (p.sides != 4) std::fprintf(stderr, "  patch %d: %d sides\n", p.label, p.sides);
    CHECK(p.sides == 4 && !p.ring);
  }
  CHECK(r.quads > 900 && r.quads < 1800);
  CHECK(closedQuads(r.low));
  CHECK(maxDistToSphere(r.low, 0.5f) < 0.01f);
  // a denser top: more quads there
  std::vector<float> dens(hi.positions.size(), 1.f);
  for (size_t v = 0; v < dens.size(); ++v)
    if (hi.positions[v].y > 0.25f) dens[v] = 4.f;
  retopo::Result d = retopo::build(hi, bvh, label, dens, o);
  CHECK(d.problems.empty());
  CHECK(closedQuads(d.low));
  int top = 0, topDense = 0;
  for (uint32_t f = 0; f < r.low.faceCount(); ++f) top += r.low.faceCenter(f).y > 0.3f;
  for (uint32_t f = 0; f < d.low.faceCount(); ++f) topDense += d.low.faceCenter(f).y > 0.3f;
  std::fprintf(stderr, "  quads on top: %d at density 1, %d at density 4\n", top, topDense);
  CHECK(topDense > top * 3 / 2);
}

TEST(retopo, tube_is_a_ring_and_caps_are_disks) {
  // a closed capsule-like shape: a sphere stretched along Y, the middle band a ring patch
  Mesh hi = makeUVSphere(96, 48, 0.5f);
  for (auto& p : hi.positions) p.y *= 3.f;
  hi.finalize();
  Bvh bvh;
  bvh.build(hi);
  std::vector<int32_t> label(hi.faceCount());
  for (uint32_t f = 0; f < hi.faceCount(); ++f) {
    float y = hi.faceCenter(f).y;
    label[f] = y > 0.9f ? 1 : y < -0.9f ? 2 : 3;
  }
  retopo::Options o;
  o.quadsPerM2 = 400;
  retopo::Result r = retopo::build(hi, bvh, label, {}, o);
  for (auto& p : r.problems) std::fprintf(stderr, "  %s\n", p.c_str());
  CHECK(r.problems.empty());
  int rings = 0, disks = 0;
  for (auto& p : r.patches) (p.ring ? rings : disks)++;
  CHECK(rings == 1 && disks == 2);
  CHECK(closedQuads(r.low));
}

TEST(retopo, problems_are_said) {
  Mesh hi = makeUVSphere(48, 24, 0.5f);
  Bvh bvh;
  bvh.build(hi);
  std::vector<int32_t> label(hi.faceCount(), 1);  // one closed patch: no boundary
  retopo::Result r = retopo::build(hi, bvh, label, {}, {});
  CHECK(r.quads == 0 && !r.problems.empty());
  CHECK(r.problems[0].find("closed surface") != std::string::npos);
}

namespace {

// A layout is a disk (or a ring) of quads, all facing the same way, whose open edges are exactly the
// boundary given, in order. Returns why not, or "" when it is.
std::string badLayout(const retopo::PlaneLayout& L, int boundary, int innerStart = -1) {
  if (!L.problem.empty()) return L.problem;
  std::map<std::pair<uint32_t, uint32_t>, int> directed;
  std::set<uint32_t> used;
  for (auto& q : L.quads) {
    std::set<uint32_t> d(q.begin(), q.end());
    if (d.size() != 4) return "a quad with a repeated corner";
    for (int k = 0; k < 4; ++k) {
      if (q[k] >= L.points.size()) return "a corner out of range";
      used.insert(q[k]);
      if (directed[{q[k], q[(k + 1) % 4]}]++) return "two quads share an edge the same way round";
    }
  }
  if (used.size() != L.points.size()) return "a point no quad uses";
  int open = 0;
  for (auto& [e, n] : directed) {
    if (directed.count({e.second, e.first})) continue;
    ++open;
    // an open edge must be a boundary edge, in order
    uint32_t a = e.first, b = e.second;
    if ((int)a >= boundary || (int)b >= boundary) return "an open edge inside";
    bool outer = innerStart < 0 || ((int)a < innerStart && (int)b < innerStart);
    int n0 = innerStart < 0 ? boundary : innerStart;
    if (outer && (int)b != ((int)a + 1) % n0) return "an open edge out of order";
    if (!outer) {
      int ni = boundary - innerStart, ia = a - innerStart, ib = b - innerStart;
      if (ia != (ib + 1) % ni) return "an inner open edge out of order";
    }
  }
  if (open != boundary) return "open edges: " + std::to_string(open) + " for " + std::to_string(boundary);
  long V = (long)L.points.size(), E = 0, Fc = (long)L.quads.size();
  for (auto& [e, n] : directed) E += directed.count({e.second, e.first}) ? 1 : 2;
  E /= 2;
  long chi = V - E + Fc;
  if (chi != (innerStart < 0 ? 1 : 0)) return "not a disk or ring (chi " + std::to_string(chi) + ")";
  return "";
}

int flipped(const retopo::PlaneLayout& L) {
  int n = 0;
  for (auto& q : L.quads) {
    float a = 0;
    for (int k = 0; k < 4; ++k) {
      vec2 p = L.points[q[k]], r = L.points[q[(k + 1) % 4]];
      a += p.x * r.y - r.x * p.y;
    }
    n += a <= 0;
  }
  return n;
}

}  // namespace

TEST(retopo, four_sided_layouts_take_any_counts_with_even_differences) {
  int tried = 0, failed = 0, inverted = 0;
  for (int n0 = 1; n0 <= 10; ++n0)
    for (int n1 = 1; n1 <= 10; ++n1)
      for (int n2 = 1; n2 <= 10; ++n2)
        for (int n3 = 1; n3 <= 10; ++n3) {
          if ((n0 + n2) % 2 || (n1 + n3) % 2) continue;
          retopo::PlaneLayout L = retopo::layoutPolygon({n0, n1, n2, n3});
          std::string bad = badLayout(L, n0 + n1 + n2 + n3);
          bool fair = std::max(n0, n2) <= 3 * std::min(n0, n2) && std::max(n1, n3) <= 3 * std::min(n1, n3) &&
                      std::min({n0, n1, n2, n3}) >= 2;
          ++tried;
          if (!bad.empty()) {
            ++failed;
            if (fair) std::fprintf(stderr, "  %d %d %d %d: %s\n", n0, n1, n2, n3, bad.c_str());
            CHECK(!fair);
            continue;
          }
          if (fair && flipped(L)) ++inverted;
          if (n0 == n2 && n1 == n3) CHECK((int)L.quads.size() == n0 * n1);  // a plain grid
        }
  std::fprintf(stderr, "  %d four-sided layouts, %d without one (all lopsided), %d fair ones with a flipped quad\n", tried,
               failed, inverted);
  CHECK(inverted == 0);
}

TEST(retopo, pole_layouts_for_three_five_and_six_sides) {
  for (int K : {3, 5, 6}) {
    int hi = K == 6 ? 6 : 9, tried = 0, failed = 0;
    std::vector<int> c(K, 2);
    while (true) {
      int total = 0;
      for (int v : c) total += v;
      if (total % 2 == 0) {
        retopo::PlaneLayout L = retopo::layoutPolygon(c);
        std::string bad = badLayout(L, total);
        ++tried;
        bool allEven = true;
        for (int v : c) allEven &= v % 2 == 0;
        if (!bad.empty()) {
          ++failed;
          if (allEven) {
            std::fprintf(stderr, "  K=%d:", K);
            for (int v : c) std::fprintf(stderr, " %d", v);
            std::fprintf(stderr, ": %s\n", bad.c_str());
          }
          CHECK(!allEven);
        }
      }
      int k = 0;
      while (k < K && ++c[k] > hi) c[k++] = 2;
      if (k == K) break;
    }
    std::fprintf(stderr, "  %d-sided: %d layouts, %d without one\n", K, tried, failed);
  }
  // the classic ones: a three-sided patch of 2s is three quads; a five-sided one five
  CHECK(retopo::layoutPolygon({2, 2, 2}).quads.size() == 3);
  CHECK(retopo::layoutPolygon({2, 2, 2, 2, 2}).quads.size() == 5);
  CHECK(retopo::layoutPolygon({4, 4, 4}).quads.size() == 12);
}

TEST(retopo, ring_layouts_join_any_counts_of_the_same_parity) {
  for (int a = 4; a <= 24; ++a)
    for (int b = 4; b <= 24; ++b) {
      if ((a + b) % 2) continue;
      for (int h = 1; h <= 4; ++h) {
        retopo::PlaneLayout L = retopo::layoutRing(a, b, h);
        std::string bad = badLayout(L, a + b, a);
        if (!bad.empty()) std::fprintf(stderr, "  ring %d %d %d: %s\n", a, b, h, bad.c_str());
        CHECK(bad.empty());
        if (a == b) CHECK((int)L.quads.size() == a * h);
      }
    }
}

TEST(retopo, a_flat_square_is_a_plain_grid_and_density_adds_loops_inside) {
  Mesh hi = makePlane(1.f, 100);  // 1 m square, 20000 triangles
  Bvh bvh;
  bvh.build(hi);
  std::vector<int32_t> label(hi.faceCount(), 1);
  retopo::Options o;
  o.quadsPerM2 = 400;  // 5 cm: a 20 x 20 grid
  retopo::Result r = retopo::build(hi, bvh, label, {}, o);
  for (auto& p : r.problems) std::fprintf(stderr, "  %s\n", p.c_str());
  std::fprintf(stderr, "  plain: %d quads, %d poles\n", r.quads, r.poles);
  CHECK(r.quads >= 18 * 18 && r.quads <= 22 * 22);
  CHECK(r.poles == 0);
  // four times the density in a band across the middle: more quads there, loops ending either side
  std::vector<float> dens(hi.positions.size(), 1.f);
  for (size_t v = 0; v < dens.size(); ++v)
    if (std::abs(hi.positions[v].z) < 0.15f) dens[v] = 4.f;
  retopo::Result d = retopo::build(hi, bvh, label, dens, o);
  int mid = 0, edge = 0;
  for (uint32_t f = 0; f < d.low.faceCount(); ++f) {
    vec3 c = d.low.faceCenter(f);
    if (std::abs(c.z) < 0.1f) ++mid;
    if (c.z > 0.3f) ++edge;
  }
  std::fprintf(stderr, "  dense band: %d quads, %d poles, %d in the band, %d near an edge\n", d.quads, d.poles, mid, edge);
  CHECK(mid > edge);
}

TEST(retopo, suggested_patches_cover_a_ball_with_about_the_quads_asked_for) {
  Mesh hi = makeUVSphere(64, 32, 0.5f);
  Bvh bvh;
  bvh.build(hi);
  std::vector<uint32_t> all(hi.faceCount());
  for (uint32_t f = 0; f < all.size(); ++f) all[f] = f;
  int cells = 0;
  std::vector<int32_t> label = retopo::autoPatches(hi, all, 6, &cells);
  CHECK(cells >= 6);
  retopo::Options o;
  o.quadsPerM2 = 800;
  retopo::Result r = retopo::build(hi, bvh, label, {}, o);
  for (auto& p : r.problems) std::fprintf(stderr, "  %s\n", p.c_str());
  double want = 4 * kPi * 0.25 * 800;
  std::fprintf(stderr, "  %d cells, %d quads (%.0f asked for), %d poles\n", cells, r.quads, want, r.poles);
  for (auto& p : r.patches) std::fprintf(stderr, "    patch %d: %d sides, %d quads\n", p.label, p.sides, p.quads);
  CHECK(r.problems.empty());
  CHECK(closedQuads(r.low));
  CHECK(r.quads > want * 0.8 && r.quads < want * 1.25);
}

#include "core/RetopoField.h"

TEST(retopo, cross_field_singularities_add_up_to_the_shape) {
  // Poincare-Hopf: the quarter turns add up to 4 x the Euler characteristic (sphere 8, torus 0)
  auto total = [](const retopo::CrossField& F) {
    int s = 0;
    for (int8_t i : F.index) s += i;
    return s;
  };
  retopo::FieldOptions o;
  o.edgeLength = 0.05f;
  Mesh sphere = makeUVSphere(64, 32, 0.5f);
  retopo::CrossField fs = retopo::computeField(sphere, {}, {}, o);
  std::fprintf(stderr, "  sphere: %d singular vertices, total %d quarter turns\n", (int)fs.singular.size(), total(fs));
  CHECK(total(fs) == 8);
  Mesh torus = makeTorus(1.f, 0.35f, 64, 24);
  retopo::CrossField ft = retopo::computeField(torus, {}, {}, o);
  std::fprintf(stderr, "  torus: %d singular vertices, total %d\n", (int)ft.singular.size(), total(ft));
  CHECK(total(ft) == 0);
  // a stroke around the sphere's middle: the field runs along it there
  retopo::Stroke st;
  for (int k = 0; k < 64; ++k) {
    float a = 2 * kPi * k / 64;
    st.points.push_back(vec3(0.5f * std::cos(a), 0.f, 0.5f * std::sin(a)));
  }
  st.loop = true;
  retopo::CrossField f2 = retopo::computeField(sphere, {st}, {}, o);
  float worst = 0;
  for (uint32_t f = 0; f < sphere.faceCount(); ++f) {
    vec3 c = sphere.faceCenter(f);
    if (std::abs(c.y) > 0.02f) continue;
    vec3 t = glm::normalize(vec3(-c.z, 0, c.x));
    float best = 0;
    for (int k = 0; k < 4; ++k) best = std::max(best, std::abs(glm::dot(f2.dir(f, k), t)));
    worst = std::max(worst, std::acos(std::min(1.f, best)));
  }
  std::fprintf(stderr, "  along the stroke: within %.1f degrees\n", worst * 180 / kPi);
  CHECK(worst < 0.1f);
  CHECK(total(f2) == 8);
}

#include "core/RetopoQuads.h"
#include "core/RetopoShell.h"

TEST(retopo, flow_quads_cover_a_ball_with_few_poles) {
  Mesh hi = makeUVSphere(96, 48, 0.5f);
  Bvh bvh;
  bvh.build(hi);
  retopo::FlowOptions o;
  o.quadsPerM2 = 400;  // 5 cm quads on 3.14 m²: about 1250
  retopo::Result r = retopo::flowQuads(hi, bvh, {}, {}, {}, o);
  for (auto& p : r.problems) std::fprintf(stderr, "  %s\n", p.c_str());
  auto& q = r.quality;
  std::fprintf(stderr, "  %d quads, %.1f%% regular, %d three, %d five, %d other\n", r.quads, q.pct(q.regular), q.three, q.five,
               q.other);
  CHECK(closedQuads(r.low));
  CHECK(r.quads > 1250 * 0.75 && r.quads < 1250 * 1.35);
  CHECK(q.pct(q.regular) > 97);
  CHECK(q.three - q.five == 8);  // a closed all-quad ball: three-edged poles = five-edged + 8
  CHECK(maxDistToSphere(r.low, 0.5f) < 0.01f);
  // a torus needs no poles at all
  Mesh torus = makeTorus(1.f, 0.35f, 128, 48);
  Bvh tb;
  tb.build(torus);
  retopo::Result t = retopo::flowQuads(torus, tb, {}, {}, {}, o);
  std::fprintf(stderr, "  torus: %d quads, %.1f%% regular\n", t.quads, t.quality.pct(t.quality.regular));
  CHECK(closedQuads(t.low));
  CHECK(t.quality.pct(t.quality.regular) > 97);
}

TEST(retopo, flow_quads_follow_an_edge_loop_and_density) {
  Mesh hi = makeUVSphere(96, 48, 0.5f);
  Bvh bvh;
  bvh.build(hi);
  retopo::FlowOptions o;
  o.quadsPerM2 = 400;
  // an edge loop drawn round the ball, tilted: quad edges run exactly along it
  retopo::Stroke st;
  vec3 n = glm::normalize(vec3(0.3f, 1.f, 0.2f)), u = glm::normalize(glm::cross(n, vec3(0, 0, 1))), w = glm::cross(n, u);
  for (int k = 0; k < 64; ++k) {
    float a = 2 * kPi * k / 64;
    st.points.push_back(0.5f * (std::cos(a) * u + std::sin(a) * w));
  }
  st.loop = true;
  st.edge = true;
  retopo::Result r = retopo::flowQuads(hi, bvh, {st}, {}, {}, o);
  CHECK(closedQuads(r.low));
  int on = 0;
  for (auto& p : r.low.positions) on += std::abs(glm::dot(p, n)) < 0.002f;
  std::fprintf(stderr, "  %d vertices on the loop (about %d expected), %.1f%% regular\n", on, (int)std::lround(kPi / 0.05f),
               r.quality.pct(r.quality.regular));
  CHECK(on >= 0.8 * kPi / 0.05f);
  CHECK(r.quality.pct(r.quality.regular) > 93);
  // the density brush, four times on top: more quads there (QuadriFlow follows a painted size only
  // partly; zones are for exact resolutions)
  std::vector<float> dens(hi.positions.size(), 1.f);
  for (size_t v = 0; v < dens.size(); ++v)
    if (hi.positions[v].y > 0.25f) dens[v] = 4.f;
  retopo::Result plain = retopo::flowQuads(hi, bvh, {}, {}, {}, o);
  retopo::Result d = retopo::flowQuads(hi, bvh, {}, {}, dens, o);
  int top = 0, topDense = 0;
  for (uint32_t f = 0; f < plain.low.faceCount(); ++f) top += plain.low.faceCenter(f).y > 0.3f;
  for (uint32_t f = 0; f < d.low.faceCount(); ++f) topDense += d.low.faceCenter(f).y > 0.3f;
  std::fprintf(stderr, "  quads on top: %d at density 1, %d at density 4\n", top, topDense);
  CHECK(closedQuads(d.low));
  CHECK(topDense > top * 13 / 10);
}

TEST(retopo, curved_lines_are_followed_without_poles_along_them) {
  // (0.20) a small circle and a wavy loop on a ball: QuadriFlow's edge loops run along both, so no
  // poles sit on them (each line point was held along the zigzag of the working triangles, and a row's
  // jump across a line was left in: the quads crossed the lines in steps, with poles there)
  Mesh hi = makeUVSphere(96, 48, 0.5f);
  Bvh bvh;
  bvh.build(hi);
  retopo::FlowOptions o;
  o.quadsPerM2 = 550;
  o.ringZones = false;  // (the band between them would be rings: this is about QuadriFlow's own loops)
  const float L = 1.f / std::sqrt(550.f);
  retopo::Stroke circle, wave;
  const float lat = 0.61f, tilt = 0.35f;
  for (int k = 0; k < 120; ++k) {
    float a = 2 * kPi * k / 120;
    vec3 p(std::cos(lat) * std::cos(a), std::sin(lat), std::cos(lat) * std::sin(a));
    p = vec3(p.x, std::cos(tilt) * p.y - std::sin(tilt) * p.z, std::sin(tilt) * p.y + std::cos(tilt) * p.z);
    circle.points.push_back(0.5f * glm::normalize(p));
    wave.points.push_back(0.5f * glm::normalize(vec3(std::cos(a), -0.35f + 0.12f * std::sin(4 * a), std::sin(a))));
  }
  circle.loop = wave.loop = true, circle.edge = wave.edge = true;
  retopo::Result r = retopo::flowQuads(hi, bvh, {circle, wave}, {}, {}, o);
  CHECK(closedQuads(r.low));
  const Mesh& m = r.low;
  std::vector<std::set<uint32_t>> nb(m.positions.size());
  for (uint32_t f = 0; f < m.faceCount(); ++f)
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) {
      uint32_t a = m.loopVert[l], b = m.loopVert[m.nextLoop(l, f)];
      nb[a].insert(b), nb[b].insert(a);
    }
  auto distTo = [](const vec3& p, const std::vector<vec3>& pts) {
    float bd = 1e30f;
    for (size_t i = 0; i < pts.size(); ++i) {
      vec3 a = pts[i], b = pts[(i + 1) % pts.size()], ab = b - a;
      float t = std::clamp(glm::dot(p - a, ab) / glm::dot(ab, ab), 0.f, 1.f);
      bd = std::min(bd, glm::length(a + t * ab - p));
    }
    return bd;
  };
  int poles = 0, onLines = 0;
  for (size_t v = 0; v < m.positions.size(); ++v) {
    if (nb[v].size() == 4) continue;
    ++poles;
    onLines += std::min(distTo(m.positions[v], circle.points), distTo(m.positions[v], wave.points)) < 0.75f * L;
  }
  std::fprintf(stderr, "  %zu quads, %d poles, %d of them on the lines; %.1f%% regular\n", m.faceCount(), poles, onLines,
               r.quality.pct(r.quality.regular));
  CHECK(onLines <= 2);
  CHECK(r.quality.pct(r.quality.regular) >= 99.0);
}

TEST(retopo, a_zone_at_four_times_the_resolution_steps_down_at_its_line) {
  Mesh hi = makeUVSphere(96, 48, 0.5f);
  Bvh bvh;
  bvh.build(hi);
  retopo::FlowOptions o;
  o.quadsPerM2 = 400;
  // a zone line round the ball at y = 0.25; the cap above it at four times the quads per m2
  retopo::Stroke st;
  const float y0 = 0.25f, r0 = std::sqrt(0.25f - y0 * y0);
  for (int k = 0; k < 96; ++k) {
    float a = 2 * kPi * k / 96;
    st.points.push_back(vec3(r0 * std::cos(a), y0, r0 * std::sin(a)));
  }
  st.loop = true;
  st.edge = true;
  std::vector<float> res(hi.faceCount(), 1.f);
  for (uint32_t f = 0; f < hi.faceCount(); ++f)
    if (hi.faceCenter(f).y > y0) res[f] = 4.f;
  retopo::Result r = retopo::flowQuads(hi, bvh, {st}, {}, {}, o, res);
  for (auto& p : r.problems) std::fprintf(stderr, "  %s\n", p.c_str());
  // quads per m2 above and below the line, away from it
  double capArea = 2 * kPi * 0.5 * (0.5 - 0.3), restArea = 2 * kPi * 0.5 * (0.5 - (-0.5)) - 2 * kPi * 0.5 * (0.5 - 0.2);
  int cap = 0, rest = 0, tris = 0;
  for (uint32_t f = 0; f < r.low.faceCount(); ++f) {
    float y = r.low.faceCenter(f).y;
    tris += r.low.faceSize(f) == 3;
    if (y > 0.3f) ++cap;
    else if (y < 0.2f) ++rest;
  }
  double dCap = cap / capArea, dRest = rest / restArea;
  auto& q = r.quality;
  std::fprintf(stderr, "  %.0f quads per m2 in the zone, %.0f outside (asked: 1600, 400); %d quads, %d triangles, %.1f%% regular\n", dCap,
               dRest, r.quads, tris, q.pct(q.regular));
  CHECK(dCap > 1600 * 0.75 && dCap < 1600 * 1.3);
  CHECK(dRest > 400 * 0.75 && dRest < 400 * 1.3);
  CHECK(tris <= 2);  // (an odd step, and now and then a filled hole)
  CHECK(q.pct(q.regular) > 90);
  // closed: every edge between two faces
  std::map<std::pair<uint32_t, uint32_t>, int> uses;
  for (uint32_t f = 0; f < r.low.faceCount(); ++f)
    for (uint32_t l = r.low.faceStart[f]; l < r.low.faceStart[f + 1]; ++l) {
      uint32_t a = r.low.loopVert[l], b = r.low.loopVert[r.low.nextLoop(l, f)];
      uses[{std::min(a, b), std::max(a, b)}]++;
    }
  int bad = 0;
  for (auto& [e, n] : uses) bad += n != 2;
  CHECK(bad == 0);
  // no face turned over (against the ball's surface), the joining rows' included
  int turned = 0;
  for (uint32_t f = 0; f < r.low.faceCount(); ++f) {
    const uint32_t s0 = r.low.faceStart[f];
    const auto& X = r.low.positions;
    vec3 c = r.low.faceCenter(f), n = r.low.faceSize(f) == 4
                                          ? glm::cross(X[r.low.loopVert[s0 + 2]] - X[r.low.loopVert[s0]], X[r.low.loopVert[s0 + 3]] - X[r.low.loopVert[s0 + 1]])
                                          : glm::cross(X[r.low.loopVert[s0 + 1]] - X[r.low.loopVert[s0]], X[r.low.loopVert[s0 + 2]] - X[r.low.loopVert[s0]]);
    turned += glm::dot(n, c) <= 0;
  }
  std::fprintf(stderr, "  %d faces turned over\n", turned);
  CHECK(turned == 0);
}

TEST(retopo, fewer_poles_never_leaves_more_poles_or_folds) {
  // (0.21) the quads' counts solved again from their own layout, kept only when better: the ball with a
  // zone at four times the resolution (rows stepping down at its line make poles in pairs)
  Mesh hi = makeUVSphere(96, 48, 0.5f);
  Bvh bvh;
  bvh.build(hi);
  retopo::Stroke st;
  const float y0 = 0.25f, r0 = std::sqrt(0.25f - y0 * y0);
  for (int k = 0; k < 96; ++k) {
    float a = 2 * kPi * k / 96;
    st.points.push_back(vec3(r0 * std::cos(a), y0, r0 * std::sin(a)));
  }
  st.loop = st.edge = true;
  std::vector<float> res(hi.faceCount(), 1.f);
  for (uint32_t f = 0; f < hi.faceCount(); ++f)
    if (hi.faceCenter(f).y > y0) res[f] = 4.f;
  retopo::FlowOptions o;
  o.quadsPerM2 = 400;
  o.cache = retopo::makeFlowCache();
  retopo::Result off = retopo::flowQuads(hi, bvh, {st}, {}, {}, o, res);
  o.requantize = true;
  retopo::Result on = retopo::flowQuads(hi, bvh, {st}, {}, {}, o, res);
  auto turned = [](const Mesh& m) {
    int n = 0;
    for (uint32_t f = 0; f < m.faceCount(); ++f) {
      const uint32_t s0 = m.faceStart[f];
      const auto& X = m.positions;
      vec3 c = m.faceCenter(f), nn = m.faceSize(f) == 4 ? glm::cross(X[m.loopVert[s0 + 2]] - X[m.loopVert[s0]], X[m.loopVert[s0 + 3]] - X[m.loopVert[s0 + 1]])
                                                       : glm::cross(X[m.loopVert[s0 + 1]] - X[m.loopVert[s0]], X[m.loopVert[s0 + 2]] - X[m.loopVert[s0]]);
      n += glm::dot(nn, c) <= 0;
    }
    return n;
  };
  std::fprintf(stderr, "  off: %d quads, %d poles, %d turned; on: %d quads, %d poles, %d turned (%s)\n", off.quads, off.poles, turned(off.low), on.quads,
               on.poles, turned(on.low), on.resolved ? "solved again" : "as made");
  CHECK(on.poles <= off.poles);
  CHECK(turned(on.low) <= turned(off.low));
  CHECK(on.quads * 10 >= off.quads * 9);
  CHECK(closedQuads(on.low));
  if (on.resolved) CHECK(on.poles < off.poles);
}

TEST(retopo, mirror_x_gives_symmetric_quads_that_meet_along_the_middle) {
  // (0.22.3) with Mirror X the quads are made on one side and mirrored: a loop drawn on the right only comes
  // out on the left too, every vertex has its mirror image, and the halves share an edge loop down the middle
  Mesh hi = makeUVSphere(96, 48, 0.5f);
  Bvh bvh;
  bvh.build(hi);
  retopo::FlowOptions o;
  o.quadsPerM2 = 550;
  o.mirrorAxes = 1;
  const float L = 1.f / std::sqrt(550.f);
  retopo::Stroke circle;
  for (int k = 0; k < 90; ++k) {
    float a = 2 * kPi * k / 90;
    circle.points.push_back(0.5f * glm::normalize(vec3(1.f, 0.45f * std::cos(a), 0.45f * std::sin(a))));
  }
  circle.loop = circle.edge = true;
  retopo::Result r = retopo::flowQuads(hi, bvh, {circle}, {}, {}, o);
  for (auto& p : r.problems) std::fprintf(stderr, "  %s\n", p.c_str());
  const Mesh& m = r.low;
  CHECK(closedQuads(m));
  auto nearest = [&](const vec3& p) {
    float bd = 1e30f;
    for (auto& q : m.positions) bd = std::min(bd, glm::length(q - p));
    return bd;
  };
  float worst = 0;
  int onMiddle = 0;
  for (auto& p : m.positions) {
    worst = std::max(worst, nearest(vec3(-p.x, p.y, p.z)));
    onMiddle += p.x == 0.f;
  }
  // (the loop on the left: as many quad vertices along its mirror image as along it)
  int along = 0, alongMirror = 0;
  for (auto& p : m.positions) {
    float d = 1e30f, dm = 1e30f;
    for (auto& c : circle.points) d = std::min(d, glm::length(p - c)), dm = std::min(dm, glm::length(p - vec3(-c.x, c.y, c.z)));
    along += d < 0.1f * L, alongMirror += dm < 0.1f * L;
  }
  std::fprintf(stderr, "  %d quads, %.1f%% regular, mirror images within %.2g, %d vertices on the middle, %d / %d along the loop and its image\n",
               r.quads, r.quality.pct(r.quality.regular), worst, onMiddle, along, alongMirror);
  CHECK(worst < 0.03f * L);  // (the images put on the surface: its triangles aren't mirror images of each other)
  CHECK(onMiddle >= int(2 * kPi * 0.5f / L * 0.8f));  // (an edge loop round the ball's middle)
  CHECK(along >= 20 && alongMirror == along);
  CHECK(r.quality.pct(r.quality.regular) > 97);
  for (auto& p : r.problems) CHECK(p.find("Mirror") == std::string::npos);
  // a lopsided ball (its right half drawn out): not mirrored, and said so
  Mesh lop = hi;
  for (auto& p : lop.positions)
    if (p.x > 0) p.x *= 1.4f;
  lop.finalize();
  Bvh lb;
  lb.build(lop);
  retopo::Result l = retopo::flowQuads(lop, lb, {}, {}, {}, o);
  bool said = false;
  for (auto& p : l.problems) said |= p.find("Mirror") != std::string::npos;
  CHECK(said);
  CHECK(closedQuads(l.low));
}

// every edge between exactly two faces (triangles allowed)
bool closedFaces(const Mesh& m) {
  std::map<std::pair<uint32_t, uint32_t>, int> uses;
  for (uint32_t f = 0; f < m.faceCount(); ++f)
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) {
      uint32_t a = m.loopVert[l], b = m.loopVert[m.nextLoop(l, f)];
      uses[{std::min(a, b), std::max(a, b)}]++;
    }
  for (auto& [e, n] : uses)
    if (n != 2) return false;
  return true;
}

// two balls (radius r, centers at x = -a and a) joined at a narrow neck round x = 0: near the neck their walls
// face along x, nearly flat against the middle plane (as a cleft's do)
Mesh makePeanut(float a, float r, int seg = 96, int rows = 80) {
  Mesh m;
  m.faceStart = {0};
  // the profile from the left tip to the right one: (x, distance from the x axis)
  std::vector<vec2> prof;
  const float neck = std::acos(-a / r);
  for (int side = -1; side <= 1; side += 2)
    for (int k = 0; k < rows; ++k) {
      int kk = side < 0 ? rows - 1 - k : k + 1;  // (left: tip to neck; right: just past the neck to the tip)
      if (side > 0 && kk == rows) break;
      float phi = neck * (1.f - float(kk) / rows);
      prof.push_back(vec2(side * (a + r * std::cos(phi)), r * std::sin(phi)));
    }
  const int nr = (int)prof.size();
  for (auto& q : prof)
    for (int i = 0; i < seg; ++i) {
      float t = 2.f * kPi * i / seg;
      m.positions.push_back(vec3(q.x, q.y * std::cos(t), q.y * std::sin(t)));
    }
  auto id = [&](int i, int j) { return uint32_t(j * seg + (i % seg)); };
  auto face = [&](std::vector<uint32_t> vs) {
    for (uint32_t v : vs) m.loopVert.push_back(v), m.loopUV.push_back(vec2(0));
    m.faceStart.push_back((uint32_t)m.loopVert.size());
  };
  for (int j = 0; j + 1 < nr; ++j)
    for (int i = 0; i < seg; ++i) face({id(i, j), id(i + 1, j), id(i + 1, j + 1), id(i, j + 1)});
  for (int end = 0; end < 2; ++end) {
    uint32_t c = (uint32_t)m.positions.size();
    m.positions.push_back(vec3(end ? a + r : -a - r, 0, 0));
    for (int i = 0; i < seg; ++i) {
      uint32_t p = id(i, end ? nr - 1 : 0), q = id(i + 1, end ? nr - 1 : 0);
      if (end) face({c, p, q});
      else face({c, q, p});
    }
  }
  m.finalize();
  return m;
}

TEST(retopo, mirror_x_presses_a_crease_down_the_middle_shut_without_doubled_faces) {
  // (0.22.4) a crease round the middle narrower than a quad (a cleft, the cleavage): its walls are pressed onto
  // the middle, and the faces lying flat there are left out, not kept and mirrored onto themselves
  Mesh hi = makePeanut(0.29f, 0.3f);
  Bvh bvh;
  bvh.build(hi);
  retopo::FlowOptions o;
  o.quadsPerM2 = 550;
  o.mirrorAxes = 1;
  retopo::Result r = retopo::flowQuads(hi, bvh, {}, {}, {}, o);
  for (auto& p : r.problems) std::fprintf(stderr, "  %s\n", p.c_str());
  const Mesh& m = r.low;
  int flat = 0;
  for (uint32_t f = 0; f < m.faceCount(); ++f) {
    bool on = true;
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) on &= m.positions[m.loopVert[l]].x == 0.f;
    flat += on;
  }
  std::fprintf(stderr, "  %d quads, %.1f%% regular, %d faces flat on the middle\n", r.quads, r.quality.pct(r.quality.regular), flat);
  CHECK(flat == 0);
  CHECK(closedFaces(m));
  for (auto& p : r.problems) CHECK(p.find("Mirror") == std::string::npos);
}

TEST(retopo, mirror_x_a_line_that_comes_near_the_middle_joins_it_without_a_pole) {
  // (0.22.4) a loop on one side that comes within half a quad of the middle (a breast's outline by the cleavage)
  // runs along the middle's loop there and leaves it across it: no star of edges where it and its image meet the
  // middle (with only its nearest points moved there: six or seven edges at a vertex)
  Mesh hi = makeUVSphere(96, 48, 0.5f);
  Bvh bvh;
  bvh.build(hi);
  retopo::FlowOptions o;
  o.quadsPerM2 = 550;
  o.mirrorAxes = 1;
  const float L = 1.f / std::sqrt(550.f);
  for (float near : {0.2f, 0.4f}) {
    // (a circle round a point on the front-right, its nearest to the middle that many quads off it)
    const vec3 center = glm::normalize(vec3(0.3f, 0.f, 1.f));
    const float ang = std::asin(center.x) - near * L / 0.5f;
    const vec3 u = glm::normalize(glm::cross(center, vec3(0, 1, 0))), v = glm::cross(u, center);
    retopo::Stroke circle;
    for (int k = 0; k < 120; ++k) {
      float a = 2 * kPi * k / 120;
      circle.points.push_back(0.5f * glm::normalize(std::cos(ang) * center + std::sin(ang) * (std::cos(a) * u + std::sin(a) * v)));
    }
    circle.loop = circle.edge = true;
    float nearest = 1e30f;
    for (auto& p : circle.points) nearest = std::min(nearest, std::fabs(p.x));
    retopo::Result r = retopo::flowQuads(hi, bvh, {circle}, {}, {}, o);
    for (auto& p : r.problems) std::fprintf(stderr, "  %s\n", p.c_str());
    std::map<uint32_t, std::set<uint32_t>> nb;
    for (uint32_t f = 0; f < r.low.faceCount(); ++f)
      for (uint32_t l = r.low.faceStart[f]; l < r.low.faceStart[f + 1]; ++l) {
        uint32_t a = r.low.loopVert[l], b = r.low.loopVert[r.low.nextLoop(l, f)];
        nb[a].insert(b), nb[b].insert(a);
      }
    size_t most = 0;
    for (auto& [x, n] : nb) most = std::max(most, n.size());
    std::fprintf(stderr, "  the line %.2f quads from the middle at its nearest; %d quads, %.1f%% regular, at most %zu edges at a vertex\n",
                 nearest / L, r.quads, r.quality.pct(r.quality.regular), most);
    CHECK(nearest < 0.5f * L);
    CHECK(closedFaces(r.low));
    CHECK(most <= 5);
    CHECK(r.quality.pct(r.quality.regular) > 97);
  }
}

TEST(retopo, small_holes_are_found_where_the_border_elsewhere_is_tangled) {
  // (0.22.4) a hole of four edges in a grid of quads is found though the grid's outer border touches itself at a
  // corner (two quads meeting only there, as strands of hair do): before, no hole was filled anywhere then
  std::vector<std::array<uint32_t, 4>> quads;
  auto id = [](int i, int j) { return uint32_t(j * 7 + i); };
  for (int j = 0; j < 6; ++j)
    for (int i = 0; i < 6; ++i)
      if (!(i == 2 && j == 3)) quads.push_back({id(i, j), id(i + 1, j), id(i + 1, j + 1), id(i, j + 1)});
  quads.push_back({id(6, 6), 100, 101, 102});  // (a quad on the grid's corner by that vertex alone)
  auto holes = retopo::smallHoles(quads);
  CHECK(holes.size() == 1);
  if (holes.size() == 1) {
    std::set<uint32_t> h(holes[0].begin(), holes[0].end()), want = {id(2, 3), id(3, 3), id(3, 4), id(2, 4)};
    CHECK(h == want);
  }
  // a hole of three edges, with triangles: a tetrahedron short of a face
  std::vector<std::array<uint32_t, 3>> tris = {{300, 301, 302}, {300, 302, 303}, {300, 303, 301}};
  auto three = retopo::smallHoles({}, &tris);
  CHECK(three.size() == 1 && three[0].size() == 3);
}

TEST(retopo, a_quad_layout_covers_a_ball_with_its_eight_poles) {
  // (0.21, in progress) the quads from a layout: a field's eight singularities, their separatrices, the
  // patches' counts solved at once; closed, about the count asked for, poles only at the singularities
  Mesh hi = makeUVSphere(96, 48, 0.5f);
  Bvh bvh;
  bvh.build(hi);
  retopo::FlowOptions o;
  o.quadsPerM2 = 400;
  o.layout = true;
  retopo::Result r = retopo::flowQuads(hi, bvh, {}, {}, {}, o);
  for (auto& p : r.problems) std::fprintf(stderr, "  %s\n", p.c_str());
  std::map<std::pair<uint32_t, uint32_t>, int> uses;
  for (uint32_t f = 0; f < r.low.faceCount(); ++f)
    for (uint32_t l = r.low.faceStart[f]; l < r.low.faceStart[f + 1]; ++l) {
      uint32_t a = r.low.loopVert[l], b = r.low.loopVert[r.low.nextLoop(l, f)];
      uses[{std::min(a, b), std::max(a, b)}]++;
    }
  int bad = 0;
  for (auto& [e, n] : uses) bad += n != 2;
  const double asked = 4 * kPi * 0.25 * 400;
  std::fprintf(stderr, "  %d quads (asked %.0f), %d poles, %.1f%% regular, %d edges not between two faces\n", r.quads, asked, r.poles,
               r.quality.pct(r.quality.regular), bad);
  CHECK(bad == 0);
  CHECK(r.quads > asked * 0.6 && r.quads < asked * 1.4);
  CHECK(r.poles <= 12);
}

TEST(retopo, the_working_shell_is_closed_and_just_outside) {
  Mesh hi = makeUVSphere(96, 48, 0.5f);
  Bvh bvh;
  bvh.build(hi);
  Mesh s = retopo::voxelShell(hi, bvh, 0.02f, 0.02f);
  std::map<std::pair<uint32_t, uint32_t>, int> uses;
  for (uint32_t f = 0; f < s.faceCount(); ++f)
    for (uint32_t l = s.faceStart[f]; l < s.faceStart[f + 1]; ++l) {
      uint32_t a = s.loopVert[l], b = s.loopVert[s.nextLoop(l, f)];
      uses[{std::min(a, b), std::max(a, b)}]++;
    }
  bool closed = !uses.empty();
  for (auto& [e, k] : uses) closed &= k == 2;
  CHECK(closed);
  float worst = maxDistToSphere(s, 0.52f);
  std::fprintf(stderr, "  shell: %u triangles, within %.4f of the offset surface\n", s.faceCount(), worst);
  CHECK(worst < 0.005f);
}

#include "core/RetopoDecimate.h"

TEST(retopo, decimation_keeps_the_shape_and_the_topology) {
  // two fine balls 4 mm apart (one mesh): far fewer triangles, still two closed balls on the same surface
  Mesh a = makeUVSphere(256, 128, 0.1f), b = makeUVSphere(256, 128, 0.1f);
  std::vector<vec3> pos;
  std::vector<glm::uvec3> tri;
  auto add = [&](const Mesh& m, vec3 off) {
    uint32_t base = (uint32_t)pos.size();
    for (auto& p : m.positions) pos.push_back(p + off);
    for (auto& t : m.tris) tri.push_back({base + m.loopVert[t.x], base + m.loopVert[t.y], base + m.loopVert[t.z]});
  };
  add(a, vec3(0)), add(b, vec3(0.204f, 0, 0));
  // (the UV sphere's poles repeat a vertex per segment: weld them)
  {
    std::map<std::tuple<int, int, int>, uint32_t> at;
    std::vector<uint32_t> remap(pos.size());
    std::vector<vec3> np;
    for (size_t v = 0; v < pos.size(); ++v) {
      auto k = std::make_tuple((int)std::lround(pos[v].x * 1e6), (int)std::lround(pos[v].y * 1e6), (int)std::lround(pos[v].z * 1e6));
      auto [it, fresh] = at.emplace(k, (uint32_t)np.size());
      if (fresh) np.push_back(pos[v]);
      remap[v] = it->second;
    }
    std::vector<glm::uvec3> nt;
    for (auto t : tri) {
      glm::uvec3 f(remap[t.x], remap[t.y], remap[t.z]);
      if (f.x != f.y && f.y != f.z && f.x != f.z) nt.push_back(f);
    }
    pos = std::move(np), tri = std::move(nt);
  }
  size_t before = tri.size();
  retopo::decimateToEdge(pos, tri, 0.015f);
  std::fprintf(stderr, "  %zu triangles -> %zu\n", before, tri.size());
  CHECK(tri.size() < before / 10);
  // closed (every edge on two triangles) and Euler characteristic 4 (two balls)
  std::map<std::pair<uint32_t, uint32_t>, int> uses;
  for (auto& t : tri)
    for (int k = 0; k < 3; ++k) uses[{std::min(t[k], t[(k + 1) % 3]), std::max(t[k], t[(k + 1) % 3])}]++;
  bool closed = true;
  for (auto& [e, n] : uses) closed &= n == 2;
  CHECK(closed);
  long chi = (long)pos.size() - (long)uses.size() + (long)tri.size();
  std::fprintf(stderr, "  Euler characteristic %ld\n", chi);
  CHECK(chi == 4);
  float worst = 0;
  for (auto& p : pos) {
    vec3 c = p.x > 0.102f ? vec3(0.204f, 0, 0) : vec3(0);
    worst = std::max(worst, std::abs(glm::length(p - c) - 0.1f));
  }
  std::fprintf(stderr, "  within %.2f mm of the balls\n", worst * 1000);
  CHECK(worst < 0.001f);
}

#include "core/RetopoMirror.h"

TEST(retopo, mirrored_strokes_join_at_the_middle) {
  auto line = [](std::vector<vec3> pts, bool loop = false) {
    retopo::Stroke s;
    s.points = std::move(pts), s.loop = loop, s.edge = true;
    return s;
  };
  auto symmetric = [](const retopo::Stroke& s) {
    // every point has its mirror image (x -> -x) among the points
    for (auto& p : s.points) {
      float best = 1e30f;
      for (auto& q : s.points) best = std::min(best, glm::length(q - vec3(-p.x, p.y, p.z)));
      if (best > 1e-5f) return false;
    }
    return true;
  };
  const vec3 c(0);
  // ending within the deadzone: one line through the middle
  auto a = retopo::mirrorStroke(line({{0.3f, 0, 0}, {0.2f, 0.1f, 0}, {0.05f, 0.1f, 0}}), 1, c, 0.1f);
  CHECK(a.size() == 1 && !a[0].loop && a[0].points.size() == 5 && symmetric(a[0]));
  // drawn across the middle: its longer side, mirrored, as one line
  auto b = retopo::mirrorStroke(line({{-0.1f, 0, 0}, {0.1f, 0.05f, 0}, {0.4f, 0.1f, 0}}), 1, c, 0.01f);
  CHECK(b.size() == 1 && symmetric(b[0]));
  float maxX = 0;
  for (auto& p : b[0].points) maxX = std::max(maxX, std::abs(p.x));
  CHECK(std::abs(maxX - 0.4f) < 1e-5f);
  // a loop across the middle: one closed symmetric loop
  std::vector<vec3> circ;
  for (int i = 0; i < 24; ++i) circ.push_back(vec3(0.1f + 0.3f * std::cos(i * 0.2618f), 0.3f * std::sin(i * 0.2618f), 0));
  auto d = retopo::mirrorStroke(line(circ, true), 1, c, 0.01f);
  CHECK(d.size() == 1 && d[0].loop && symmetric(d[0]));
  // a loop off to one side: it and its mirror image
  for (auto& p : circ) p.x += 0.5f;
  auto e = retopo::mirrorStroke(line(circ, true), 1, c, 0.01f);
  CHECK(e.size() == 2 && e[0].loop && e[1].loop && std::abs(e[1].points[0].x + e[0].points[0].x) < 1e-5f);
  // along the middle: itself
  auto f = retopo::mirrorStroke(line({{0.005f, 0, 0}, {-0.004f, 0.5f, 0}}), 1, c, 0.01f);
  CHECK(f.size() == 1 && f[0].points[0].x == 0 && f[0].points[1].x == 0);
  // away from the middle on two axes: four copies
  auto g = retopo::mirrorStroke(line({{0.3f, 0.3f, 0}, {0.4f, 0.5f, 0}}), 3, c, 0.01f);
  CHECK(g.size() == 4);
  // (0.19.4) a small loop across the middle, most of it within the deadzone: one loop that keeps its
  // width (its points were put on the middle, folding it flat, and a copy laid on top of it)
  std::vector<vec3> small;
  for (int i = 0; i < 24; ++i) small.push_back(vec3(0.003f + 0.015f * std::cos(i * 0.2618f), 0.3f + 0.015f * std::sin(i * 0.2618f), 0));
  auto h = retopo::mirrorStroke(line(small, true), 1, c, 0.018f);
  float wide = 0;
  for (auto& p : h[0].points) wide = std::max(wide, std::abs(p.x));
  CHECK(h.size() == 1 && h[0].loop && wide > 0.012f);
  std::fprintf(stderr, "  %zu %zu %zu %zu %zu %zu strokes\n", a.size(), b.size(), d.size(), e.size(), f.size(), g.size());
}

TEST(retopo, a_band_between_two_lines_is_filled_with_rings_and_a_small_cap_is_closed) {
  Mesh hi = makeUVSphere(96, 48, 0.5f);
  Bvh bvh;
  bvh.build(hi);
  retopo::FlowOptions o;
  o.quadsPerM2 = 400;
  auto ring = [](float y0) {
    retopo::Stroke st;
    const float r0 = std::sqrt(0.25f - y0 * y0);
    for (int k = 0; k < 96; ++k) {
      float a = 2 * kPi * k / 96;
      st.points.push_back(vec3(r0 * std::cos(a), y0, r0 * std::sin(a)));
    }
    st.loop = true, st.edge = true;
    return st;
  };
  // a band round the middle, and a small cap at the top inside a ring
  std::vector<retopo::Stroke> lines = {ring(0.15f), ring(-0.15f), ring(0.485f), ring(0.42f)};
  auto closed = [](const Mesh& m) {
    std::map<std::pair<uint32_t, uint32_t>, int> uses;
    for (uint32_t f = 0; f < m.faceCount(); ++f)
      for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) {
        uint32_t a = m.loopVert[l], b = m.loopVert[m.nextLoop(l, f)];
        uses[{std::min(a, b), std::max(a, b)}]++;
      }
    int bad = 0;
    for (auto& [e, n] : uses) bad += n != 2;
    return bad;
  };
  retopo::Result r = retopo::flowQuads(hi, bvh, lines, {}, {}, o);
  for (auto& p : r.problems) std::fprintf(stderr, "  %s\n", p.c_str());
  CHECK(closed(r.low) == 0);
  if (const char* dump = getenv("PATINA_TEST_DUMP"))
    if (FILE* fo = fopen(dump, "w")) {
      for (auto& p : r.low.positions) fprintf(fo, "v %f %f %f\n", p.x, p.y, p.z);
      for (uint32_t f = 0; f < r.low.faceCount(); ++f) {
        fprintf(fo, "f");
        for (uint32_t l = r.low.faceStart[f]; l < r.low.faceStart[f + 1]; ++l) fprintf(fo, " %u", r.low.loopVert[l] + 1);
        fprintf(fo, "\n");
      }
      fclose(fo);
    }
  // in the band the vertices lie on a few rings (latitudes), its edges along them or across
  std::set<int> levels;
  int along = 0, edges = 0;
  for (auto& p : r.low.positions)
    if (std::abs(p.y) < 0.14f) levels.insert((int)std::lround(p.y * 500));
  for (uint32_t f = 0; f < r.low.faceCount(); ++f)
    for (uint32_t l = r.low.faceStart[f]; l < r.low.faceStart[f + 1]; ++l) {
      vec3 a = r.low.positions[r.low.loopVert[l]], b = r.low.positions[r.low.loopVert[r.low.nextLoop(l, f)]];
      if (std::abs(a.y) > 0.14f || std::abs(b.y) > 0.14f) continue;
      vec3 d = glm::normalize(b - a), lat = glm::normalize(glm::cross(vec3(0, 1, 0), 0.5f * (a + b)));
      float c = std::abs(glm::dot(d, lat));
      ++edges, along += c > 0.985f || c < 0.17f;
    }
  std::fprintf(stderr, "  %zu ring heights in the band, %d of %d edges along or across them; %d quads, %.1f%% regular\n", levels.size(), along,
               edges, r.quads, r.quality.pct(r.quality.regular));
  CHECK(levels.size() <= 8);
  CHECK(along >= edges * 9 / 10);
  // without rings: the same lines still make a closed mesh
  o.ringZones = false;
  retopo::Result r2 = retopo::flowQuads(hi, bvh, lines, {}, {}, o);
  CHECK(closed(r2.low) == 0);
}

TEST(retopo, a_disc_inside_a_ring_is_closed_with_a_grid) {
  // (0.19) two circles round the top of a ball: rings between them, and the cap inside the inner one a
  // grid (every vertex inside it with four edges but its four corners), nothing turned over
  Mesh hi = makeUVSphere(96, 48, 0.5f);
  Bvh bvh;
  bvh.build(hi);
  retopo::FlowOptions o;
  o.quadsPerM2 = 1200;
  auto ring = [](float y0) {
    retopo::Stroke st;
    const float r0 = std::sqrt(0.25f - y0 * y0);
    for (int k = 0; k < 96; ++k) {
      float a = 2 * kPi * k / 96;
      st.points.push_back(vec3(r0 * std::cos(a), y0, r0 * std::sin(a)));
    }
    st.loop = true, st.edge = true;
    return st;
  };
  // (a narrow band, so the ring needs no steps and stays a ring)
  std::vector<retopo::Stroke> lines = {ring(0.47f), ring(0.455f)};
  retopo::Result r = retopo::flowQuads(hi, bvh, lines, {}, {}, o);
  for (auto& p : r.problems) std::fprintf(stderr, "  %s\n", p.c_str());
  const Mesh& m = r.low;
  if (const char* dump = getenv("PATINA_TEST_DUMP_DISC"))
    if (FILE* fo = fopen(dump, "w")) {
      for (auto& p : m.positions) fprintf(fo, "v %f %f %f\n", p.x, p.y, p.z);
      for (uint32_t f = 0; f < m.faceCount(); ++f) {
        fprintf(fo, "f");
        for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) fprintf(fo, " %u", m.loopVert[l] + 1);
        fprintf(fo, "\n");
      }
      fclose(fo);
    }
  std::vector<std::set<uint32_t>> nb(m.positions.size());
  int turned = 0;
  for (uint32_t f = 0; f < m.faceCount(); ++f) {
    vec3 c(0), n(0);
    const uint32_t l0 = m.faceStart[f], l1 = m.faceStart[f + 1];
    for (uint32_t l = l0; l < l1; ++l) {
      uint32_t a = m.loopVert[l], b = m.loopVert[m.nextLoop(l, f)];
      nb[a].insert(b), nb[b].insert(a);
      c += m.positions[a] / float(l1 - l0);
      n += glm::cross(m.positions[a], m.positions[b]);
    }
    if (c.y > 0.3f && glm::dot(n, c) <= 0) ++turned;
  }
  // (the grid sits a ring in from the circle: its four corners are the only poles inside it)
  int inside = 0, irregular = 0;
  for (size_t v = 0; v < m.positions.size(); ++v)
    if (m.positions[v].y > 0.4705f) ++inside, irregular += nb[v].size() != 4;
  std::fprintf(stderr, "  %d vertices inside the inner circle, %d without four edges; %d faces turned over; %.1f%% regular\n", inside, irregular,
               turned, r.quality.pct(r.quality.regular));
  CHECK(inside >= 20);
  CHECK(irregular == 4);
  CHECK(turned == 0);
  CHECK(r.quality.pct(r.quality.regular) >= 95.0);
}

TEST(retopo, a_long_part_past_a_ring_is_not_closed_with_a_grid) {
  // (0.19.3) a finger-like ellipsoid with a narrow ring near one end: the long part past the ring is a
  // disc with far more surface than its line holds (as a hand past a wrist line): no grid over it, so no
  // quads stretched along it
  Mesh hi = makeUVSphere(64, 64, 1.f);
  for (auto& p : hi.positions) p = vec3(0.05f * p.x, 0.25f * p.y, 0.05f * p.z);
  hi.finalize();
  Bvh bvh;
  bvh.build(hi);
  retopo::FlowOptions o;
  o.quadsPerM2 = 3000;
  auto ring = [](float y0) {
    retopo::Stroke st;
    const float r0 = 0.05f * std::sqrt(1 - (y0 / 0.25f) * (y0 / 0.25f));
    for (int k = 0; k < 96; ++k) {
      float a = 2 * kPi * k / 96;
      st.points.push_back(vec3(r0 * std::cos(a), y0, r0 * std::sin(a)));
    }
    st.loop = true, st.edge = true;
    return st;
  };
  retopo::Result r = retopo::flowQuads(hi, bvh, {ring(-0.15f), ring(-0.13f)}, {}, {}, o);
  for (auto& p : r.problems) std::fprintf(stderr, "  %s\n", p.c_str());
  const Mesh& m = r.low;
  const float L = 1.f / std::sqrt(3000.f);
  float longest = 0;
  for (uint32_t f = 0; f < m.faceCount(); ++f)
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l)
      longest = std::max(longest, glm::length(m.positions[m.loopVert[m.nextLoop(l, f)]] - m.positions[m.loopVert[l]]));
  std::fprintf(stderr, "  %u faces, longest edge %.2f quads, %.1f%% regular\n", m.faceCount(), longest / L, r.quality.pct(r.quality.regular));
  CHECK(m.faceCount() > 100);
  CHECK(longest < 3 * L);
}

#include "core/RetopoLoops.h"

TEST(retopo, edge_loops_run_round_a_ball_and_poles_are_found) {
  // a UV sphere's quads: the latitude loops go all the way round; the poles are its two fans
  Mesh m = makeUVSphere(16, 8, 0.5f);
  retopo::EdgeLoops L = retopo::edgeLoops(m);
  int closedRound = 0;
  for (size_t i = 0; i < L.loops.size(); ++i) closedRound += L.closed[i] && L.loops[i].size() == 16;
  std::fprintf(stderr, "  %zu loops, %d all the way round with 16 edges, %zu poles\n", L.loops.size(), closedRound, L.poles.size());
  CHECK(closedRound >= 5);  // (the latitudes between the fans' rings)
  retopo::LoopFinder f;
  f.build(m, L);
  int at = f.nearest(m, vec3(0.5f, 0.f, 0.f), 0.1f);
  CHECK(at >= 0);
  std::vector<vec3> pts;
  std::vector<vec4> cols;
  retopo::loopSegments(m, L, 0.01f, at, pts, cols);
  CHECK(pts.size() == cols.size() && pts.size() >= 6 * L.poles.size() + 2);
}

#include "core/RetopoThin.h"

namespace {
// A closed body of revolution along y: a ball of radius 0.3 with a thin finger (radius 0.03, 0.4
// long) out of its top, closed at both ends
Mesh makeBallWithFinger(int seg = 96, int rows = 240) {
  auto radius = [](float y) {  // y from -0.3 (bottom pole) to 0.7 (finger tip)
    if (y < 0.2f) return std::sqrt(std::max(0.f, 0.09f - y * y));
    if (y < 0.28f) return glm::mix(std::sqrt(0.09f - 0.04f), 0.03f, (y - 0.2f) / 0.08f);
    if (y < 0.67f) return 0.03f;
    return std::sqrt(std::max(0.f, 0.0009f - (y - 0.67f) * (y - 0.67f)));
  };
  Mesh m;
  m.positions.push_back(vec3(0, -0.3f, 0));
  for (int r = 1; r < rows; ++r) {
    float y = -0.3f + 1.0f * r / rows;
    for (int k = 0; k < seg; ++k) {
      float a = 2 * kPi * k / seg;
      m.positions.push_back(vec3(radius(y) * std::cos(a), y, radius(y) * std::sin(a)));
    }
  }
  m.positions.push_back(vec3(0, 0.7f, 0));
  const uint32_t top = (uint32_t)m.positions.size() - 1;
  auto id = [&](int r, int k) { return uint32_t(1 + (r - 1) * seg + (k % seg)); };
  m.faceStart.push_back(0);
  auto face = [&](std::initializer_list<uint32_t> vs) {
    for (uint32_t v : vs) m.loopVert.push_back(v);
    m.faceStart.push_back((uint32_t)m.loopVert.size());
  };
  for (int k = 0; k < seg; ++k) face({0, id(1, k), id(1, k + 1)});
  for (int r = 1; r + 1 < rows; ++r)
    for (int k = 0; k < seg; ++k) face({id(r, k), id(r + 1, k), id(r + 1, k + 1), id(r, k + 1)});
  for (int k = 0; k < seg; ++k) face({id(rows - 1, k), top, id(rows - 1, k + 1)});
  m.finalize();
  // (outward: the bottom pole's faces point down)
  if (m.faceNormal(0).y > 0)
    for (uint32_t f = 0; f < m.faceCount(); ++f) std::reverse(m.loopVert.begin() + m.faceStart[f], m.loopVert.begin() + m.faceStart[f + 1]);
  m.finalize();
  return m;
}
}  // namespace

TEST(retopo, a_thin_finger_gets_a_zone_of_its_own) {
  Mesh hi = makeBallWithFinger();
  Bvh bvh;
  bvh.build(hi);
  const float L = 0.05f;  // (400 quads per m²: a finger 6 cm thick is too thin for 5 cm quads)
  std::vector<float> faceL(hi.faceCount(), L);
  auto parts = retopo::findThinParts(hi, bvh, faceL);
  std::fprintf(stderr, "  %zu thin parts\n", parts.size());
  CHECK(parts.size() == 1);
  if (parts.size() != 1) return;
  const auto& p = parts[0];
  float yMin = 1e30f, yMax = -1e30f, rMax = 0;
  for (auto& l : p.loops)
    for (auto& q : l) yMin = std::min(yMin, q.y), yMax = std::max(yMax, q.y), rMax = std::max(rMax, std::hypot(q.x, q.z));
  std::fprintf(stderr, "  %zu line(s) at y %.3f..%.3f, radius up to %.3f; thickness %.3f, resolution x%g, %s, %s\n", p.loops.size(), yMin, yMax,
               rMax, p.thickness, p.resolution, p.tube ? "a tube" : "a sheet", p.clean ? "cut cleanly" : "not cut cleanly");
  CHECK(p.tube && p.clean);
  CHECK(p.loops.size() == 1);
  CHECK(yMax - yMin < 0.02f);              // a ring across the finger, not along it
  CHECK(yMin > 0.2f && yMax < 0.4f);       // at its root
  CHECK(p.resolution >= 2 && p.resolution <= 8);
  {
    // the same with its faces turned inwards (a model exported the other way round)
    Mesh in = hi;
    for (uint32_t f = 0; f < in.faceCount(); ++f) std::reverse(in.loopVert.begin() + in.faceStart[f], in.loopVert.begin() + in.faceStart[f + 1]);
    in.finalize();
    Bvh ib;
    ib.build(in);
    auto ip = retopo::findThinParts(in, ib, faceL);
    std::fprintf(stderr, "  turned inwards: %zu parts%s%s, normal of face 0 %.2f %.2f %.2f\n", ip.size(), ip.size() && ip[0].tube ? ", a tube" : "",
                 ip.size() && ip[0].clean ? ", clean" : "", in.faceNormal(0).x, in.faceNormal(0).y, in.faceNormal(0).z);
    CHECK(ip.size() == 1 && ip[0].tube && ip[0].clean);
  }
  // the zone it makes, at that resolution, covers the finger with quads
  retopo::Stroke st;
  st.points = p.loops[0], st.loop = true, st.edge = true;
  std::vector<float> res(hi.faceCount(), 1.f);
  for (uint32_t f = 0; f < hi.faceCount(); ++f)
    if (hi.faceCenter(f).y > yMax) res[f] = p.resolution;
  retopo::FlowOptions o;
  o.quadsPerM2 = 1.f / (L * L);
  retopo::Result r = retopo::flowQuads(hi, bvh, {st}, {}, {}, o, res);
  for (auto& q : r.problems) std::fprintf(stderr, "  %s\n", q.c_str());
  int onFinger = 0;
  for (uint32_t f = 0; f < r.low.faceCount(); ++f) onFinger += r.low.faceCenter(f).y > 0.35f;
  std::fprintf(stderr, "  %d faces on the finger, %d quads in all\n", onFinger, r.quads);
  CHECK(onFinger >= 30);
  bool thinMessage = false;
  for (auto& q : r.problems) thinMessage |= q.find("too thin") != std::string::npos;
  CHECK(!thinMessage);
}

TEST(retopo, a_click_on_a_limb_gives_a_line_round_it) {
  // (0.20.4) Shift+click with the Lines tool: a long ellipsoid (an arm) gets a loop right round it across
  // its length through the point clicked; a ball and a flat slab get none
  Mesh arm = makeUVSphere(64, 64, 1.f);
  for (auto& p : arm.positions) p = vec3(0.05f * p.x, 0.3f * p.y, 0.04f * p.z);  // (an oval section)
  arm.finalize();
  Bvh bvh;
  bvh.build(arm);
  std::string why;
  auto loop = retopo::ringAround(arm, bvh, vec3(0.05f, 0.06f, 0.f), &why);
  std::fprintf(stderr, "  arm: %zu points (%s)\n", loop.size(), why.c_str());
  CHECK(loop.size() >= 16);
  float ylo = 1e30f, yhi = -1e30f;
  double len = 0;
  for (size_t i = 0; i < loop.size(); ++i) {
    ylo = std::min(ylo, loop[i].y), yhi = std::max(yhi, loop[i].y);
    len += glm::length(loop[(i + 1) % loop.size()] - loop[i]);
  }
  // (across the arm: flat in y, at the point; the oval's perimeter at y = 0.06)
  const float s = std::sqrt(1 - (0.06f / 0.3f) * (0.06f / 0.3f)), a = 0.05f * s, b = 0.04f * s;
  const double oval = kPi * (3 * (a + b) - std::sqrt((3 * a + b) * (a + 3 * b)));
  std::fprintf(stderr, "  y from %.4f to %.4f; %.4f round (an oval of %.4f)\n", ylo, yhi, len, oval);
  CHECK(yhi - ylo < 0.004f);
  CHECK(std::abs(ylo - 0.06f) < 0.004f);
  CHECK(std::abs(len - oval) < 0.05 * oval);
  // a ball: no tube there
  Mesh ball = makeUVSphere(64, 32, 0.3f);
  Bvh bb;
  bb.build(ball);
  CHECK(retopo::ringAround(ball, bb, vec3(0.3f, 0.f, 0.f), &why).empty());
  std::fprintf(stderr, "  ball: %s\n", why.c_str());
}

// ---- 0.23: a change remakes only the quads near it
#include "core/RetopoLocal.h"

namespace {
// a flat grid of quads (n by n, cells h, turned by `angle` round the middle and moved by `off`), facing +z
Mesh flatGrid(int n, float h, float angle, vec2 off) {
  Mesh m;
  const float c = std::cos(angle), s = std::sin(angle);
  for (int j = 0; j <= n; ++j)
    for (int i = 0; i <= n; ++i) {
      float x = (i - n * 0.5f) * h, y = (j - n * 0.5f) * h;
      m.positions.push_back(vec3(c * x - s * y + off.x, s * x + c * y + off.y, 0.f));
    }
  m.faceStart.push_back(0);
  for (int j = 0; j < n; ++j)
    for (int i = 0; i < n; ++i) {
      uint32_t a = j * (n + 1) + i;
      for (uint32_t v : {a, a + 1, a + n + 2, a + n + 1}) m.loopVert.push_back(v);
      m.faceStart.push_back((uint32_t)m.loopVert.size());
    }
  m.finalize();
  return m;
}
}  // namespace

TEST(retopo, a_change_remakes_only_the_quads_near_it_with_a_row_between) {
  const float L = 0.02f;
  for (float angle : {0.f, 0.15f, 0.4f}) {
    Mesh old = flatGrid(40, L, 0.f, vec2(0.f)), fresh = flatGrid(44, L * 1.03f, angle, vec2(0.3f * L, 0.2f * L));
    retopo::LocalChange ch;
    ch.points.push_back(vec3(0.f));
    Mesh out;
    retopo::LocalStats st;
    bool ok = retopo::keepAwayFromChange(old, fresh, ch, L, 4 * L, {}, {}, out, st);
    std::fprintf(stderr, "  turned %.2f: %s; %d kept, %d new, a row of %d faces (%d triangles), %d of its vertices poles %s\n", angle,
                 ok ? "joined" : "not joined", st.keptFaces, st.newFaces, st.rowFaces, st.rowTriangles, st.rowPoles, st.why.c_str());
    CHECK(ok);
    if (!ok) continue;
    // every face facing +z (none turned over), every edge on at most two faces
    int turned = 0;
    std::map<std::pair<uint32_t, uint32_t>, int> uses;
    for (uint32_t f = 0; f < out.faceCount(); ++f) {
      const uint32_t s0 = out.faceStart[f], n = out.faceStart[f + 1] - s0;
      vec3 nn(0);
      for (uint32_t k = 0; k < n; ++k) {
        const vec3 &a = out.positions[out.loopVert[s0 + k]], &b = out.positions[out.loopVert[s0 + (k + 1) % n]];
        nn += glm::cross(a, b);
        uint32_t x = out.loopVert[s0 + k], y = out.loopVert[s0 + (k + 1) % n];
        uses[{std::min(x, y), std::max(x, y)}]++;
      }
      turned += nn.z <= 0;
    }
    int over = 0;
    for (auto& [e, k] : uses) over += k > 2;
    CHECK(turned == 0);
    CHECK(over == 0);
    // (an old grid and a new one at their own angle and spacing: the row has some poles, not many)
    CHECK(st.rowPoles <= 26);
    // every old quad away from the change kept exactly: its corners where they were
    std::set<std::tuple<float, float, float>> was;
    for (auto& p : old.positions) was.insert({p.x, p.y, p.z});
    int keptFar = 0, movedFar = 0;
    for (auto& p : out.positions)
      if (glm::length(p) > 7 * L) ++keptFar, movedFar += !was.count({p.x, p.y, p.z});
    CHECK(keptFar > 1000 && movedFar == 0);
  }
}

TEST(retopo, a_line_drawn_later_leaves_the_quads_away_from_it_as_they_were) {
  // (0.23) a build with the last quads given makes again only those near what changed (a second circle on the
  // other side of a ball): the rest are exactly as they were, joined to the new ones; without them, all again
  Mesh hi = makeUVSphere(96, 48, 0.5f);
  Bvh bvh;
  bvh.build(hi);
  retopo::FlowOptions o;
  o.quadsPerM2 = 600;
  o.requantize = true;
  o.cache = retopo::makeFlowCache();
  const float L = 1.f / std::sqrt(600.f);
  auto circle = [](vec3 center, float ang) {
    center = glm::normalize(center);
    const vec3 u = glm::normalize(glm::cross(center, std::abs(center.y) < 0.9f ? vec3(0, 1, 0) : vec3(1, 0, 0))), v = glm::cross(u, center);
    retopo::Stroke c;
    for (int k = 0; k < 120; ++k) {
      float a = 2 * kPi * k / 120;
      c.points.push_back(0.5f * glm::normalize(std::cos(ang) * center + std::sin(ang) * (std::cos(a) * u + std::sin(a) * v)));
    }
    c.loop = c.edge = true;
    return c;
  };
  retopo::Stroke A = circle(vec3(1, 0.2f, 0.3f), 0.5f), B = circle(vec3(-1, -0.1f, 0.4f), 0.45f);
  retopo::Result r1 = retopo::flowQuads(hi, bvh, {A}, {}, {}, o);
  CHECK(r1.builtFrom != nullptr && r1.localKept == -1);
  o.previous = std::make_shared<retopo::Result>(r1);
  retopo::Result r2 = retopo::flowQuads(hi, bvh, {A, B}, {}, {}, o);
  std::fprintf(stderr, "  first: %d quads, %d poles; with the second circle: %d quads, %d poles, %d faces kept\n", r1.quads, r1.poles, r2.quads,
               r2.poles, r2.localKept);
  CHECK(r2.localKept > 0);
  CHECK(closedFaces(r2.low));
  // the vertices further than six quads from the second circle: each exactly one of the first quads'
  std::set<std::tuple<float, float, float>> was;
  for (auto& p : r1.low.positions) was.insert({p.x, p.y, p.z});
  auto distB = [&](const vec3& p) {
    float d = 1e30f;
    for (size_t i = 0; i < B.points.size(); ++i) d = std::min(d, glm::length(p - B.points[i]));
    return d;
  };
  int far = 0, moved = 0;
  for (auto& p : r2.low.positions)
    if (distB(p) > 6 * L) ++far, moved += !was.count({p.x, p.y, p.z});
  std::fprintf(stderr, "  %d vertices far from the second circle, %d of them moved\n", far, moved);
  CHECK(far > 200 && moved == 0);
  // nothing changed: the same quads
  o.previous = std::make_shared<retopo::Result>(r2);
  retopo::Result r3 = retopo::flowQuads(hi, bvh, {A, B}, {}, {}, o);
  CHECK(r3.low.positions == r2.low.positions && r3.low.loopVert == r2.low.loopVert);
  // other settings (a coarser density): all made again
  o.quadsPerM2 = 500;
  retopo::Result r4 = retopo::flowQuads(hi, bvh, {A, B}, {}, {}, o);
  CHECK(r4.localKept == -1);
}
