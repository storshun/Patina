#include "core/RetopoThin.h"

#include <Eigen/Dense>

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "core/Parallel.h"

namespace patina::retopo {

namespace {
using EdgeKey = uint64_t;
inline EdgeKey ekey(uint32_t a, uint32_t b) { return (uint64_t(std::min(a, b)) << 32) | std::max(a, b); }

// Where the plane through c across n cuts the model within radius of c: the closed loops wholly inside
// (the ring round a wrist, and round a thumb that crosses the plane there), the one nearest c first
std::vector<std::vector<vec3>> planeSection(const Mesh& m, const Bvh& bvh, const vec3& c, const vec3& n, float radius) {
  std::vector<uint32_t> tris;
  bvh.querySphere(c, radius, tris);
  // each crossing triangle gives a segment between two of its edges; segments are chained through the
  // edges they share
  std::unordered_map<EdgeKey, std::vector<std::pair<EdgeKey, vec3>>> at;  // edge -> (other edge, this point)
  std::unordered_map<EdgeKey, vec3> pointOf;
  for (uint32_t t : tris) {
    uint32_t v[3] = {m.loopVert[m.tris[t][0]], m.loopVert[m.tris[t][1]], m.loopVert[m.tris[t][2]]};
    float d[3];
    for (int k = 0; k < 3; ++k) d[k] = glm::dot(m.positions[v[k]] - c, n);
    EdgeKey ek[2];
    int got = 0;
    for (int k = 0; k < 3 && got < 2; ++k) {
      int j = (k + 1) % 3;
      if ((d[k] < 0) == (d[j] < 0)) continue;
      float s = d[k] / (d[k] - d[j]);
      EdgeKey e = ekey(v[k], v[j]);
      pointOf[e] = glm::mix(m.positions[v[k]], m.positions[v[j]], s);
      ek[got++] = e;
    }
    if (got != 2) continue;
    at[ek[0]].push_back({ek[1], {}}), at[ek[1]].push_back({ek[0], {}});
  }
  std::vector<std::pair<float, std::vector<vec3>>> found;
  std::unordered_map<EdgeKey, char> used;
  for (auto& [e0, nb] : at) {
    if (used.count(e0)) continue;
    std::vector<vec3> loop;
    EdgeKey prev = e0, cur = e0;
    bool closed = false;
    for (size_t guard = 0; guard <= at.size(); ++guard) {
      used[cur] = 1, loop.push_back(pointOf[cur]);
      auto& list = at[cur];
      EdgeKey next = 0;
      bool found = false;
      for (auto& [o, unused] : list)
        if (o != prev && !used.count(o)) {
          next = o, found = true;
          break;
        }
      if (!found) {
        for (auto& [o, unused] : list) closed |= o == e0 && loop.size() > 2;
        break;
      }
      prev = cur, cur = next;
    }
    if (!closed || loop.size() < 4) continue;
    vec3 mid(0);
    bool inside = true;
    for (auto& p : loop) mid += p / float(loop.size()), inside &= glm::length(p - c) < radius;
    if (inside) found.push_back({glm::length(mid - c), std::move(loop)});
  }
  std::sort(found.begin(), found.end(), [](auto& a, auto& b) { return a.first < b.first; });
  std::vector<std::vector<vec3>> out;
  for (auto& f : found) out.push_back(std::move(f.second));
  return out;
}
}  // namespace

std::vector<ThinPart> findThinParts(const Mesh& m, const Bvh& bvh, const std::vector<float>& faceL, int quadsAround, double minQuads) {
  std::vector<ThinPart> out;
  const uint32_t nf = m.faceCount();
  if (nf == 0 || faceL.size() != nf) return out;
  // ---- each face's thickness: a ray into the model from just inside it (none back out: open, thick)
  vec3 lo(1e30f), hi(-1e30f);
  for (auto& p : m.positions) lo = glm::min(lo, p), hi = glm::max(hi, p);
  const float eps = 1e-5f * glm::length(hi - lo);
  // (into the model: against the normals, or along them on a model whose faces point inwards; rays into
  // a closed surface nearly always meet it again, rays out of it mostly get away)
  float inward = -1.f;
  {
    int against = 0, along = 0;
    for (uint32_t f = 0; f < nf; f += std::max<uint32_t>(1, nf / 2000)) {
      vec3 n = m.faceNormal(f);
      if (glm::dot(n, n) < 1e-20f) continue;
      vec3 c = m.faceCenter(f);
      for (float sgn : {-1.f, 1.f})
        if (bvh.raycast(m, c + sgn * n * eps, sgn * n, 1e30f).hit) (sgn < 0 ? against : along)++;
    }
    if (along > against + against / 4) inward = 1.f;
  }
  std::vector<float> thick(nf, 1e30f);
  parallelFor(nf, [&](size_t f) {
    vec3 n = m.faceNormal((uint32_t)f) * -inward;  // (outwards)
    if (glm::dot(n, n) < 1e-20f) return;
    vec3 c = m.faceCenter((uint32_t)f);
    RayHit h = bvh.raycast(m, c - n * eps, -n, 50.f * faceL[f]);
    if (h.hit) thick[f] = h.t + eps;
  });
  // the faces next to each face (across its edges), to smooth the thickness and find the parts
  std::unordered_map<EdgeKey, std::array<int64_t, 2>> edgeFaces;
  edgeFaces.reserve(size_t(nf) * 2);
  for (uint32_t f = 0; f < nf; ++f)
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) {
      uint32_t a = m.loopVert[l], b = m.loopVert[l + 1 < m.faceStart[f + 1] ? l + 1 : m.faceStart[f]];
      auto [it, fresh] = edgeFaces.emplace(ekey(a, b), std::array<int64_t, 2>{f, -1});
      if (!fresh) it->second[1] = f;
    }
  std::vector<std::vector<uint32_t>> nbr(nf);
  for (auto& [e, ff] : edgeFaces)
    if (ff[1] >= 0) nbr[ff[0]].push_back((uint32_t)ff[1]), nbr[ff[1]].push_back((uint32_t)ff[0]);
  // (a ray that slips out through a gap, or hits a fold close by, is put right by its neighbors: the
  // median of the face and those round it, twice)
  for (int pass = 0; pass < 2; ++pass) {
    std::vector<float> next(thick);
    parallelFor(nf, [&](size_t f) {
      float v[8];
      int n = 0;
      v[n++] = thick[f];
      for (uint32_t g : nbr[f])
        if (n < 8) v[n++] = thick[g];
      std::nth_element(v, v + n / 2, v + n);
      next[f] = v[n / 2];
    });
    thick = std::move(next);
  }
  // ---- thin: fewer than about four quads would go round it
  std::vector<char> thin(nf, 0);
  for (uint32_t f = 0; f < nf; ++f) thin[f] = thick[f] < 4.f / kPi * faceL[f];
  // ---- tube or sheet: from the middle of a thin face's thickness, the shortest chord across the part in
  // the face's own plane is about the thickness in a tube (a finger), far more in a sheet (a lock of hair)
  std::vector<char> tubeLike(nf, 0);
  parallelFor(nf, [&](size_t f) {
    if (!thin[f]) return;
    vec3 n = m.faceNormal((uint32_t)f) * -inward;
    if (glm::dot(n, n) < 1e-20f) return;
    vec3 c = m.faceCenter((uint32_t)f) - n * (0.5f * thick[f]);
    vec3 u = glm::normalize(glm::abs(n.x) < 0.9f ? glm::cross(n, vec3(1, 0, 0)) : glm::cross(n, vec3(0, 1, 0))), w = glm::cross(n, u);
    const float reach = 4.f * thick[f];
    float best = 1e30f;
    for (int k = 0; k < 6; ++k) {
      float a = kPi * k / 6;
      vec3 d = std::cos(a) * u + std::sin(a) * w;
      RayHit h1 = bvh.raycast(m, c, d, reach), h2 = bvh.raycast(m, c, -d, reach);
      float chord = (h1.hit ? h1.t : reach) + (h2.hit ? h2.t : reach);
      best = std::min(best, chord);
    }
    tubeLike[f] = best < 2.f * thick[f];
  });
  // ---- the parts: thin faces joined across edges
  std::vector<int> part(nf, -1);
  int parts = 0;
  std::vector<uint32_t> stack;
  for (uint32_t f0 = 0; f0 < nf; ++f0) {
    if (!thin[f0] || part[f0] >= 0) continue;
    part[f0] = parts, stack = {f0};
    while (!stack.empty()) {
      uint32_t f = stack.back();
      stack.pop_back();
      for (uint32_t g : nbr[f])
        if (thin[g] && part[g] < 0) part[g] = parts, stack.push_back(g);
    }
    ++parts;
  }
  // the thicker parts too, with their areas (a loop round a small thick island inside a thin part is no
  // edge with the rest of the model)
  std::vector<int> rest(nf, -1);
  std::vector<double> restArea;
  for (uint32_t f0 = 0; f0 < nf; ++f0) {
    if (thin[f0] || rest[f0] >= 0) continue;
    int id = (int)restArea.size();
    restArea.push_back(0);
    rest[f0] = id, stack = {f0};
    while (!stack.empty()) {
      uint32_t f = stack.back();
      stack.pop_back();
      restArea[id] += m.faceArea3D(f);
      for (uint32_t g : nbr[f])
        if (!thin[g] && rest[g] < 0) rest[g] = id, stack.push_back(g);
    }
  }
  struct Acc {
    double area = 0, thickArea = 0, lArea = 0, tubeArea = 0;
    uint32_t seed = 0;
    float seedDepth = -1;
  };
  std::vector<Acc> acc(parts);
  std::vector<std::vector<std::pair<float, float>>> thickOf(parts);  // (thickness, area) of each part's faces
  std::vector<std::vector<uint32_t>> facesOf(parts);
  for (uint32_t f = 0; f < nf; ++f) {
    if (part[f] < 0) continue;
    double a = m.faceArea3D(f);
    Acc& A = acc[part[f]];
    A.area += a, A.thickArea += a * std::min(thick[f], 1e3f * faceL[f]), A.lArea += a * faceL[f];
    if (tubeLike[f]) A.tubeArea += a;
    thickOf[part[f]].push_back({thick[f], (float)a});
    facesOf[part[f]].push_back(f);
  }
  // (the seed: a face deep inside the part, all its neighbors in it)
  for (uint32_t f = 0; f < nf; ++f) {
    if (part[f] < 0) continue;
    bool inside = true;
    for (uint32_t g : nbr[f]) inside &= part[g] == part[f];
    float depth = inside ? 1.f : 0.f;
    Acc& A = acc[part[f]];
    if (depth > A.seedDepth) A.seedDepth = depth, A.seed = f;
  }
  // ---- each part's edge with the thicker rest, as loops
  std::vector<std::vector<std::pair<uint32_t, uint32_t>>> edgesOf(parts);
  std::vector<std::unordered_map<uint32_t, int>> restAt(parts);  // per part: a boundary vertex -> the thick part beyond
  for (uint32_t f = 0; f < nf; ++f) {
    if (part[f] < 0) continue;
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) {
      uint32_t a = m.loopVert[l], b = m.loopVert[l + 1 < m.faceStart[f + 1] ? l + 1 : m.faceStart[f]];
      auto& ff = edgeFaces[ekey(a, b)];
      int64_t g = ff[0] == f ? ff[1] : ff[0];
      if (g >= 0 && part[g] != part[f]) {
        edgesOf[part[f]].push_back({a, b});  // (the way round its own face)
        if (rest[g] >= 0) restAt[part[f]][a] = rest[g];
      }
    }
  }
  for (int p = 0; p < parts; ++p) {
    const Acc& A = acc[p];
    const double L = A.area > 0 ? A.lArea / A.area : 1;
    // (its thinner quarter by area: the fingers of a hand whose palm is thin too)
    auto& tv = thickOf[p];
    std::sort(tv.begin(), tv.end());
    double d = 0, sum = 0;
    for (auto [t, a] : tv)
      if ((sum += a) >= 0.25 * A.area) {
        d = t;
        break;
      }
    if (d <= 0) continue;
    // the resolution for about quadsAround quads round it, rounded up to a power of two (parts at the same
    // resolution are made together, and joins between few resolutions are steadier)
    const double want = kPi * d / quadsAround;
    double r = std::clamp((L / want) * (L / want), 2.0, 64.0);
    float res = (float)std::min(64.0, std::exp2(std::ceil(std::log2(r) - 0.15)));
    if (A.area < minQuads * L * L) continue;  // (smaller than a few of the quads now: a strand, a speck)
    ThinPart tp;
    tp.seedFace = A.seed, tp.thickness = (float)d, tp.resolution = res, tp.area = A.area;
    for (size_t k = 0; k < facesOf[p].size(); k += std::max<size_t>(1, facesOf[p].size() / 64)) tp.sampleFaces.push_back(facesOf[p][k]);
    std::unordered_map<uint32_t, std::vector<uint32_t>> next;
    for (auto [a, b] : edgesOf[p]) next[a].push_back(b);
    std::vector<std::vector<uint32_t>> rawLoops;
    for (auto [a0, b0] : edgesOf[p]) {
      while (!next[a0].empty()) {
        std::vector<uint32_t> loop;
        uint32_t v = a0;
        for (size_t guard = 0; guard <= edgesOf[p].size(); ++guard) {
          auto& nx = next[v];
          if (nx.empty()) break;
          loop.push_back(v);
          uint32_t w = nx.back();
          nx.pop_back();
          v = w;
          if (v == a0) break;
        }
        if (v == a0 && loop.size() >= 4) rawLoops.push_back(std::move(loop));
      }
    }
    auto finish = [&](std::vector<vec3> loop) {
      // smoothed a little (still on the surface), about a point every fifth of a new quad
      for (int it = 0; it < 2; ++it) {
        std::vector<vec3> sm(loop.size());
        for (size_t i = 0; i < loop.size(); ++i)
          sm[i] = 0.5f * loop[i] + 0.25f * (loop[(i + loop.size() - 1) % loop.size()] + loop[(i + 1) % loop.size()]);
        for (auto& q : sm) {
          RayHit h = bvh.closestPoint(m, q, 2.f * (float)want);
          if (h.hit) q = h.position;
        }
        loop = std::move(sm);
      }
      std::vector<vec3> thinned;
      float since = 1e30f;
      for (size_t i = 0; i < loop.size(); ++i) {
        if (i) since += glm::length(loop[i] - loop[i - 1]);
        if (since >= 0.2f * (float)want) thinned.push_back(loop[i]), since = 0;
      }
      if (thinned.size() < 4) return;
      double len = 0;
      for (size_t i = 0; i < thinned.size(); ++i) len += glm::length(thinned[(i + 1) % thinned.size()] - thinned[i]);
      tp.loopLength += len;
      tp.loops.push_back(std::move(thinned));
    };
    int clean = 0, edges = 0;  // (edges with the rest, and of them those cut cleanly across)
    for (auto& raw : rawLoops) {
      // (round a small thick island inside the part: not its edge with the rest)
      int beyond = -1;
      for (uint32_t v : raw)
        if (auto it = restAt[p].find(v); it != restAt[p].end()) beyond = it->second;
      if (beyond >= 0 && restArea[beyond] < 2 * A.area) continue;
      std::vector<vec3> loop;
      for (uint32_t v : raw) loop.push_back(m.positions[v]);
      double len = 0;
      for (size_t i = 0; i < loop.size(); ++i) len += glm::length(loop[(i + 1) % loop.size()] - loop[i]);
      if (len < 2 * want) continue;  // (a speck)
      ++edges;
      // a clean ring instead of the wiggly edge where the thickness crosses the bar: a cut across the limb
      // (a wrist, an ankle, a finger's root), at right angles to the way into the part from this edge (to
      // the middle of the part's faces near it), moved a little toward the thick side
      vec3 mid(0);
      float spread = 0;
      for (auto& q : loop) mid += q / float(loop.size());
      for (auto& q : loop) spread = std::max(spread, glm::length(q - mid));
      vec3 inner(0);
      double wsum = 0;
      std::vector<vec3> near;
      for (uint32_t f : facesOf[p]) {
        vec3 fc = m.faceCenter(f);
        float dist = glm::length(fc - mid);
        if (dist > 2.f * spread) continue;
        double wgt = m.faceArea3D(f);
        inner += vec3(fc * (float)wgt), wsum += wgt, near.push_back(fc);
      }
      if (wsum > 0 && spread > 0) {
        inner /= (float)wsum;
        vec3 axis = inner - mid;
        if (glm::length(axis) > 1e-3f * spread) {
          vec3 nrm = -glm::normalize(axis);  // (toward the thick side)
          // (from just beyond the part's farthest reach that way: the edge wanders where the thickness is
          // near the bar, so its middle can be well inside the part)
          float reachOut = 0;
          for (auto& q : near) reachOut = std::max(reachOut, glm::dot(q - mid, nrm));
          mid += nrm * reachOut;
          // (moved back step by step until the plane meets only the limb, one clean ring; failing that, the
          // first cut's ring round the limb (its others, round a toe or a strand the plane crosses, would only
          // make islands))
          std::vector<std::vector<vec3>> chosen;
          for (float back : {0.f, 0.5f, 1.f, 1.5f, 2.f, 2.5f, 3.f}) {
            auto rings = planeSection(m, bvh, mid + nrm * (float)(back * L), nrm, 1.5f * spread + (float)L);
            if (rings.empty()) continue;
            double rl = 0;
            for (size_t i = 0; i < rings[0].size(); ++i) rl += glm::length(rings[0][(i + 1) % rings[0].size()] - rings[0][i]);
            if (rl >= 1.2 * len) continue;  // (not a ring of about the edge's own size)
            if (chosen.empty()) chosen = rings;
            if (rings.size() == 1) {
              chosen = std::move(rings);
              break;
            }
          }
          if (!chosen.empty()) loop = std::move(chosen[0]), ++clean;
        }
      }
      finish(std::move(loop));
    }
    // (only tubes, as fingers: a thin sheet, as a lock of hair, is far wider than it is thick, and its
    // quads can be as big as elsewhere)
    if (tp.loops.empty()) continue;
    tp.tube = A.tubeArea > 0.6 * A.area;
    tp.clean = clean == edges;  // (a part with an edge that couldn't be cut cleanly, as between toes, is left)
    out.push_back(std::move(tp));
  }
  std::sort(out.begin(), out.end(), [](const ThinPart& a, const ThinPart& b) { return a.area > b.area; });
  return out;
}

std::vector<vec3> facingLoop(const Mesh& m, const Bvh& bvh, const vec3& p0, const vec3& dir, std::string* why) {
  auto fail = [&](const char* w) {
    if (why) *why = w;
    return std::vector<vec3>{};
  };
  RayHit at = bvh.closestPoint(m, p0, 1e30f);
  if (!at.hit) return fail("no surface there");
  if (glm::dot(dir, dir) < 1e-20f) return fail("no direction to face");
  const vec3 p = at.position, n = glm::normalize(dir);
  vec3 lo(1e30f), hi(-1e30f);
  for (auto& q : m.positions) lo = glm::min(lo, q), hi = glm::max(hi, q);
  const float size = glm::length(hi - lo);
  // (the loops of the cut within reach, nearest first: the one through p)
  auto loops = planeSection(m, bvh, p, n, 0.6f * size);
  std::vector<vec3> best;
  float offBy = 1e30f;
  for (auto& L : loops) {
    float dd = 1e30f;
    for (size_t i = 0; i < L.size(); ++i) {
      vec3 a = L[i], b = L[(i + 1) % L.size()], ab = b - a;
      float l2 = glm::dot(ab, ab), t = l2 > 0 ? std::clamp(glm::dot(p - a, ab) / l2, 0.f, 1.f) : 0.f;
      dd = std::min(dd, glm::length(a + t * ab - p));
    }
    if (dd < offBy) offBy = dd, best = std::move(L);
  }
  if (best.empty() || offBy > 0.01f * size) return fail("the cut facing you there doesn't close round anything");
  if (why) why->clear();
  return best;
}

std::vector<vec3> ringAround(const Mesh& m, const Bvh& bvh, const vec3& p0, std::string* why) {
  auto fail = [&](const char* w) {
    if (why) *why = w;
    return std::vector<vec3>{};
  };
  RayHit at = bvh.closestPoint(m, p0, 1e30f);
  if (!at.hit) return fail("no surface there");
  const vec3 p = at.position;
  vec3 n(0);
  {
    const glm::uvec3 tr = m.tris[at.tri];
    vec3 a = m.positions[m.loopVert[tr[0]]], b = m.positions[m.loopVert[tr[1]]], e = m.positions[m.loopVert[tr[2]]];
    n = glm::cross(b - a, e - a);
  }
  if (glm::dot(n, n) < 1e-20f) return fail("no surface there");
  n = glm::normalize(n);
  vec3 lo(1e30f), hi(-1e30f);
  for (auto& q : m.positions) lo = glm::min(lo, q), hi = glm::max(hi, q);
  const float size = glm::length(hi - lo), eps = 1e-5f * size;
  // ---- through the part: a ray in from p; it leaves the part again (the face it meets faces away from
  // it), whichever way the model's normals point
  float d = -1;
  vec3 in(0);
  for (float sgn : {-1.f, 1.f}) {
    vec3 dir = sgn * n;
    RayHit h = bvh.raycast(m, p + dir * eps, dir, size);
    if (h.hit && glm::dot(h.faceNormal, dir) * glm::dot(n, dir) < 0 && (d < 0 || h.t < d)) d = h.t, in = dir;
  }
  if (d <= 0) return fail("no part to go round there (the model is open, or a single sheet)");
  const vec3 c = p + in * (0.5f * d);  // (the middle of the part)
  // ---- its axis: on a tube (round or flattened) the normals lie across the axis, so the axis is the way
  // along which they have least spread. A first guess from the normals round the middle; then the cut
  // across it, and the axis again from a band of the surface along that cut (a sphere takes in a chin or
  // misses a waist's sides), until it settles
  auto spread = [&](const std::vector<uint32_t>& tris, Eigen::Vector3d& lam, vec3& axis) {
    Eigen::Matrix3d M = Eigen::Matrix3d::Zero();
    for (uint32_t t : tris) {
      const glm::uvec3 tr = m.tris[t];
      vec3 a = m.positions[m.loopVert[tr[0]]], b = m.positions[m.loopVert[tr[1]]], e = m.positions[m.loopVert[tr[2]]];
      vec3 cr = glm::cross(b - a, e - a);
      float area = 0.5f * glm::length(cr);
      if (area <= 0) continue;
      vec3 tn = cr / (2 * area);
      Eigen::Vector3d v(tn.x, tn.y, tn.z);
      M += area * v * v.transpose();
    }
    Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> es(M);
    if (es.info() != Eigen::Success || es.eigenvalues()[2] <= 0) return false;
    lam = es.eigenvalues();
    const Eigen::Vector3d ax = es.eigenvectors().col(0);
    axis = glm::normalize(vec3((float)ax.x(), (float)ax.y(), (float)ax.z()));
    return true;
  };
  auto sectionThrough = [&](const vec3& axis, float& offBy) {
    auto loops = planeSection(m, bvh, p, axis, 3.f * d + eps);
    std::vector<vec3> best;
    offBy = 1e30f;
    for (auto& L : loops) {
      float dd = 1e30f;
      for (size_t i = 0; i < L.size(); ++i) {
        vec3 a = L[i], b = L[(i + 1) % L.size()], ab = b - a;
        float l2 = glm::dot(ab, ab), t = l2 > 0 ? std::clamp(glm::dot(p - a, ab) / l2, 0.f, 1.f) : 0.f;
        dd = std::min(dd, glm::length(a + t * ab - p));
      }
      if (dd < offBy) offBy = dd, best = L;
    }
    return best;
  };
  std::vector<uint32_t> tris;
  bvh.querySphere(c, 0.75f * d, tris);
  Eigen::Vector3d lam;
  vec3 axis;
  if (!spread(tris, lam, axis)) return fail("no part to go round there");
  const double firstRatio = lam[0] / std::max(lam[1], 1e-30);
  std::vector<vec3> best;
  float offBy = 1e30f;
  for (int iter = 0; iter < 4; ++iter) {
    best = sectionThrough(axis, offBy);
    if (best.empty() || offBy > 0.1f * d) return fail("the cut across the part there doesn't close round it");
    // (the band: the surface within a fifth of the part's thickness of the cut)
    std::vector<uint32_t> band, near;
    double len = 0;
    for (size_t i = 0; i < best.size(); ++i) len += glm::length(best[(i + 1) % best.size()] - best[i]);
    const float r = 0.2f * d, stepLen = std::max(0.5f * r, float(len / 400));
    float acc = stepLen;
    for (size_t i = 0; i < best.size(); ++i) {
      acc += glm::length(best[(i + 1) % best.size()] - best[i]);
      if (acc < stepLen) continue;
      acc = 0, near.clear();
      bvh.querySphere(best[i], r, near);
      band.insert(band.end(), near.begin(), near.end());
    }
    std::sort(band.begin(), band.end());
    band.erase(std::unique(band.begin(), band.end()), band.end());
    vec3 axis2;
    if (!spread(band, lam, axis2)) return fail("no part to go round there");
    const bool settled = std::abs(glm::dot(axis2, axis)) > 0.9994f;  // (2 degrees)
    axis = axis2;
    if (settled) break;
  }
  // (a band round a ball looks like one round a tube: the first look, round the middle, tells them apart)
  if (lam[0] > 0.35 * lam[1] || firstRatio > 0.9) return fail("the part there is no tube (flat, or round every way)");
  best = sectionThrough(axis, offBy);
  if (best.empty() || offBy > 0.1f * d) return fail("the cut across the part there doesn't close round it");
  double len = 0;
  for (size_t i = 0; i < best.size(); ++i) len += glm::length(best[(i + 1) % best.size()] - best[i]);
  if (len > 2.5 * kPi * d) return fail("the cut across the part there runs round more than it");
  if (why) why->clear();
  return best;
}

}  // namespace patina::retopo
