// Seam paths, cylinder / sphere unwraps and relaxing islands in place.
#include <algorithm>
#include <cmath>
#include <queue>
#include <unordered_map>

#include "core/Log.h"
#include "core/UVOps.h"

namespace patina::uv {

using dvec2 = glm::dvec2;
using dvec3 = glm::dvec3;
constexpr double kPiD = 3.14159265358979323846;

// ============================================================ seam paths

void SeamGraph::build(const Mesh& m) {
  size_t V = m.positions.size();
  weld.assign(V, 0);
  pos.clear();
  float q = std::max(m.boundingRadius, 1e-6f) * 1e-6f;
  std::unordered_map<uint64_t, uint32_t> ids;
  ids.reserve(V * 2);
  for (size_t v = 0; v < V; ++v) {
    const vec3& p = m.positions[v];
    int64_t x = std::llround(p.x / q), y = std::llround(p.y / q), z = std::llround(p.z / q);
    uint64_t k = (uint64_t(x) * 73856093ull) ^ (uint64_t(y) * 19349663ull) ^ (uint64_t(z) * 83492791ull);
    auto it = ids.find(k);
    if (it != ids.end() && glm::length(pos[it->second] - p) <= 2 * q) {
      weld[v] = it->second;
    } else {
      ids[k] = (uint32_t)pos.size();
      weld[v] = (uint32_t)pos.size();
      pos.push_back(p);
    }
  }
  // welded edges: (a < b) -> id; each lists the mesh edges on it
  std::unordered_map<uint64_t, uint32_t> eid;
  wedgeEnds.clear();
  wedgeMesh.clear();
  std::vector<std::vector<std::pair<uint32_t, uint32_t>>> nb(pos.size());  // (neighbor, welded edge)
  for (uint32_t e = 0; e < m.edges.size(); ++e) {
    uint32_t a = weld[m.edges[e].x], b = weld[m.edges[e].y];
    if (a == b) continue;
    if (a > b) std::swap(a, b);
    uint64_t k = (uint64_t)a << 32 | b;
    auto it = eid.find(k);
    uint32_t id;
    if (it == eid.end()) {
      id = (uint32_t)wedgeEnds.size();
      eid[k] = id;
      wedgeEnds.push_back({a, b});
      wedgeMesh.push_back({});
      nb[a].push_back({b, id});
      nb[b].push_back({a, id});
    } else {
      id = it->second;
    }
    wedgeMesh[id].push_back(e);
  }
  adjStart.assign(pos.size() + 1, 0);
  for (size_t i = 0; i < pos.size(); ++i) adjStart[i + 1] = adjStart[i] + (uint32_t)nb[i].size();
  adj.clear();
  adj.reserve(adjStart.back());
  for (auto& l : nb) adj.insert(adj.end(), l.begin(), l.end());
  meshVerts = V;
  meshEdges = m.edges.size();
}

bool SeamGraph::matches(const Mesh& m) const {
  if (meshVerts != m.positions.size() || meshEdges != m.edges.size() || pos.empty() || m.positions.empty()) return false;
  // a few samples: an edited or different mesh of the same size is caught
  for (size_t i = 0; i < m.positions.size(); i += std::max<size_t>(1, m.positions.size() / 17))
    if (pos[weld[i]] != m.positions[i] && glm::length(pos[weld[i]] - m.positions[i]) > 1e-6f * std::max(m.boundingRadius, 1e-6f))
      return false;
  return true;
}

uint32_t SeamGraph::nearestVertex(const Mesh& m, uint32_t face, const vec3& p) const {
  uint32_t best = kNoVertex;
  float bd = 1e30f;
  for (uint32_t l = m.faceStart[face]; l < m.faceStart[face + 1]; ++l) {
    uint32_t v = weld[m.loopVert[l]];
    float d = glm::length(pos[v] - p);
    if (d < bd) bd = d, best = v;
  }
  return best;
}

std::vector<uint32_t> SeamGraph::path(uint32_t a, uint32_t b) const {
  std::vector<uint32_t> out;
  if (a >= pos.size() || b >= pos.size() || a == b) return out;
  // A* over edge lengths (straight-line distance to the goal as the estimate)
  std::unordered_map<uint32_t, float> dist;
  std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> from;  // vertex -> (previous, welded edge)
  using Item = std::pair<float, uint32_t>;
  std::priority_queue<Item, std::vector<Item>, std::greater<Item>> open;
  dist[a] = 0;
  open.push({glm::length(pos[a] - pos[b]), a});
  size_t guard = 0;
  while (!open.empty() && ++guard < 4000000) {
    auto [f, v] = open.top();
    open.pop();
    if (v == b) break;
    float dv = dist[v];
    if (f > dv + glm::length(pos[v] - pos[b]) + 1e-12f) continue;  // stale
    for (uint32_t k = adjStart[v]; k < adjStart[v + 1]; ++k) {
      auto [w, e] = adj[k];
      float nd = dv + glm::length(pos[v] - pos[w]);
      auto it = dist.find(w);
      if (it != dist.end() && it->second <= nd) continue;
      dist[w] = nd;
      from[w] = {v, e};
      open.push({nd + glm::length(pos[w] - pos[b]), w});
    }
  }
  if (!from.count(b)) return out;
  for (uint32_t v = b; v != a;) {
    auto [p, e] = from[v];
    out.push_back(e);
    v = p;
  }
  std::reverse(out.begin(), out.end());
  return out;
}

void SeamGraph::apply(std::vector<uint8_t>& edgeFlags, const std::vector<uint32_t>& wedges, bool mark) const {
  for (uint32_t w : wedges)
    if (w < wedgeMesh.size())
      for (uint32_t e : wedgeMesh[w])
        if (e < edgeFlags.size()) {
          if (mark) edgeFlags[e] |= EdgeSeam;
          else edgeFlags[e] &= uint8_t(~EdgeSeam);
        }
}

// ============================================================ cylinder / sphere

namespace {

std::vector<uint32_t> facesOrAll(const Mesh& m, const std::vector<uint32_t>& faces) {
  if (!faces.empty()) {
    std::vector<uint32_t> out;
    std::vector<uint8_t> seen(m.faceCount(), 0);
    for (uint32_t f : faces)
      if (f < m.faceCount() && !seen[f]) seen[f] = 1, out.push_back(f);
    return out;
  }
  std::vector<uint32_t> out(m.faceCount());
  for (uint32_t f = 0; f < m.faceCount(); ++f) out[f] = f;
  return out;
}

// the axis of most spread (power iteration on the covariance), or a fixed one
dvec3 pickAxis(const Mesh& m, const std::vector<uint32_t>& faces, int axis, dvec3& center) {
  dvec3 c(0);
  double n = 0;
  for (uint32_t f : faces)
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) c += dvec3(m.positions[m.loopVert[l]]), n += 1;
  center = n > 0 ? c / n : dvec3(0);
  if (axis >= 0 && axis < 3) {
    dvec3 a(0);
    a[axis] = 1;
    return a;
  }
  glm::dmat3 cov(0);
  for (uint32_t f : faces)
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) {
      dvec3 d = dvec3(m.positions[m.loopVert[l]]) - center;
      cov += glm::outerProduct(d, d);
    }
  dvec3 v(0.3, 1.0, 0.2);
  for (int it = 0; it < 64; ++it) {
    dvec3 w = cov * v;
    double l = glm::length(w);
    if (l < 1e-300) return dvec3(0, 1, 0);
    v = w / l;
  }
  // snap to a world axis when nearly aligned (models are usually built along them)
  for (int k = 0; k < 3; ++k)
    if (std::abs(v[k]) > 0.985) {
      dvec3 a(0);
      a[k] = v[k] > 0 ? 1 : -1;
      return a;
    }
  return v;
}

void basisOf(const dvec3& axis, dvec3& u, dvec3& v) {
  dvec3 t = std::abs(axis.y) < 0.9 ? dvec3(0, 1, 0) : dvec3(1, 0, 0);
  u = glm::normalize(glm::cross(t, axis));
  v = glm::cross(axis, u);
}

// angles of a face's corners, unwrapped so the face doesn't straddle the -pi/pi cut
void unwrapAngles(std::vector<double>& a) {
  if (a.empty()) return;
  double ref = a[0];
  for (double& x : a) {
    while (x - ref > kPiD) x -= 2 * kPiD;
    while (ref - x > kPiD) x += 2 * kPiD;
  }
  // keep the face's mean angle in -pi..pi
  double mean = 0;
  for (double x : a) mean += x;
  mean /= a.size();
  double shift = mean > kPiD ? -2 * kPiD : mean < -kPiD ? 2 * kPiD : 0;
  for (double& x : a) x += shift;
}

void finishProjection(Mesh& m, const std::vector<uint32_t>& faces, float margin) {
  // seams where the projection cut the surface (and around the faces), then the pieces packed
  m.computeUVTopology();
  seamsFromIslands(m, false);
  PackOptions po;
  po.margin = margin;
  po.rotate = false;
  po.normalizeDensity = false;  // the projections are in model units already
  pack(m, po, faces);
}

}  // namespace

// Cylinder: the faces are cut open along one line of edges and flattened with LSCM, so bent and
// tapered tubes (arms, legs, horns, pipes) come out as one even strip. The cut is where the angle
// around the tube wraps; the angle is measured around the tube's own center at each height (slices
// along the axis), so it keeps going round the tube where the tube bends away from a straight axis.
// Faces that face along the axis (the ends) become their own islands.
void projectCylinder(Mesh& m, const std::vector<uint32_t>& facesIn, int axisIn, float margin) {
  if (m.loopUV.size() != m.loopCount()) m.loopUV.assign(m.loopCount(), vec2(0));
  auto faces = facesOrAll(m, facesIn);
  if (faces.empty()) return;
  uint32_t F = m.faceCount();
  dvec3 c;
  dvec3 axis = pickAxis(m, faces, axisIn, c), bu, bv;
  basisOf(axis, bu, bv);
  std::vector<uint8_t> in(F, 0), cap(F, 0);
  for (uint32_t f : faces) in[f] = 1, cap[f] = std::abs(glm::dot(dvec3(m.faceNormal(f)), axis)) > 0.75 ? 1 : 0;
  // the tube's center per slice along the axis (from the side faces' corners)
  double h0 = 1e300, h1 = -1e300;
  size_t nCorners = 0;
  for (uint32_t f : faces)
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) {
      double h = glm::dot(dvec3(m.positions[m.loopVert[l]]) - c, axis);
      h0 = std::min(h0, h), h1 = std::max(h1, h), ++nCorners;
    }
  int bins = std::clamp((int)std::sqrt(double(nCorners)) / 3, 2, 64);
  std::vector<dvec3> sum(bins, dvec3(0));
  std::vector<double> cnt(bins, 0);
  auto binOf = [&](double h) { return h1 > h0 ? std::clamp((h - h0) / (h1 - h0) * bins - 0.5, 0.0, bins - 1.0) : 0.0; };
  for (uint32_t f : faces) {
    if (cap[f]) continue;
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) {
      dvec3 p(m.positions[m.loopVert[l]]);
      int k = (int)std::lround(binOf(glm::dot(p - c, axis)));
      sum[k] += p, cnt[k] += 1;
    }
  }
  std::vector<dvec3> center(bins, c);
  {  // empty slices take their neighbors' center
    int last = -1;
    for (int k = 0; k < bins; ++k)
      if (cnt[k] > 0) center[k] = sum[k] / cnt[k], last = k;
      else if (last >= 0) center[k] = center[last];
    for (int k = bins - 1; k >= 0; --k)
      if (cnt[k] <= 0 && (k + 1 < bins)) center[k] = (last >= 0 && k > last) ? center[k] : center[k + 1];
  }
  auto angleOf = [&](const dvec3& p) {
    double b = binOf(glm::dot(p - c, axis));
    int k0 = (int)std::floor(b), k1 = std::min(k0 + 1, bins - 1);
    dvec3 cc = glm::mix(center[k0], center[k1], b - k0);
    dvec3 d = p - cc;
    return std::atan2(glm::dot(d, bv), glm::dot(d, bu));
  };
  // each side face's mean angle (its corners unwrapped around the first)
  std::vector<double> faceAng(F, 0);
  std::vector<double> ang;
  for (uint32_t f : faces) {
    if (cap[f]) continue;
    ang.clear();
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) ang.push_back(angleOf(dvec3(m.positions[m.loopVert[l]])));
    unwrapAngles(ang);
    double mean = 0;
    for (double x : ang) mean += x;
    faceAng[f] = mean / ang.size();
  }
  // seams: old seams inside the faces go; new ones between the ends and the side, and where the
  // side's angle wraps (between faces just below +pi and faces just above -pi)
  std::vector<std::vector<uint32_t>> edgeFaces(m.edges.size());
  for (uint32_t f = 0; f < F; ++f)
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l)
      if (m.loopEdge[l] < edgeFaces.size()) edgeFaces[m.loopEdge[l]].push_back(f);
  if (m.edgeFlags.size() != m.edges.size()) m.edgeFlags.assign(m.edges.size(), 0);
  for (uint32_t e = 0; e < edgeFaces.size(); ++e) {
    const auto& ef = edgeFaces[e];
    bool inside = !ef.empty();
    for (uint32_t f : ef) inside &= in[f] != 0;
    bool touches = false;
    for (uint32_t f : ef) touches |= in[f] != 0;
    if (!touches) continue;
    bool seam = !inside;  // the border of the faces
    if (inside && ef.size() >= 2) {
      for (size_t i = 0; i < ef.size() && !seam; ++i)
        for (size_t j = i + 1; j < ef.size() && !seam; ++j) {
          uint32_t a = ef[i], b = ef[j];
          if (cap[a] != cap[b]) seam = true;
          else if (!cap[a] && std::abs(faceAng[a] - faceAng[b]) > kPiD) seam = true;
        }
    }
    if (seam) m.edgeFlags[e] |= EdgeSeam;
    else m.edgeFlags[e] &= uint8_t(~EdgeSeam);
  }
  UnwrapOptions o;
  o.pack = false;
  o.margin = margin;
  unwrapLSCM(m, faces, o);
  m.computeUVTopology();
  PackOptions po;
  po.margin = margin;
  pack(m, po, faces);
}

void projectSphere(Mesh& m, const std::vector<uint32_t>& facesIn, int axisIn, float margin) {
  if (m.loopUV.size() != m.loopCount()) m.loopUV.assign(m.loopCount(), vec2(0));
  auto faces = facesOrAll(m, facesIn);
  if (faces.empty()) return;
  dvec3 c;
  dvec3 axis = pickAxis(m, faces, axisIn < 0 ? 1 : axisIn, c), bu, bv;  // default: Y is up
  basisOf(axis, bu, bv);
  double rsum = 0, n = 0;
  for (uint32_t f : faces)
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l)
      rsum += glm::length(dvec3(m.positions[m.loopVert[l]]) - c), n += 1;
  double r = n > 0 ? std::max(rsum / n, 1e-9) : 1.0;
  std::vector<double> ang;
  for (uint32_t f : faces) {
    ang.clear();
    dvec3 fc = dvec3(m.faceCenter(f)) - c;
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) {
      dvec3 d = dvec3(m.positions[m.loopVert[l]]) - c;
      double ru = glm::dot(d, bu), rv = glm::dot(d, bv);
      // a corner on the pole has no longitude of its own: it takes the face center's
      if (ru * ru + rv * rv < 1e-12 * glm::dot(d, d)) ru = glm::dot(fc, bu), rv = glm::dot(fc, bv);
      ang.push_back(std::atan2(rv, ru));
    }
    unwrapAngles(ang);
    uint32_t k = 0;
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l, ++k) {
      dvec3 d = dvec3(m.positions[m.loopVert[l]]) - c;
      double len = std::max(glm::length(d), 1e-12);
      double lat = std::asin(std::clamp(glm::dot(d, axis) / len, -1.0, 1.0));
      m.loopUV[l] = vec2(float(ang[k] * r), float(lat * r));
    }
  }
  finishProjection(m, faces, margin);
}

// ============================================================ relax

int relaxIslands(Mesh& m, const std::vector<uint32_t>& faces, float strength) {
  if (m.loopUV.size() != m.loopCount() || faces.empty()) return 0;
  if (m.loopIsland.size() != m.loopCount()) m.computeUVTopology();
  strength = std::clamp(strength, 0.f, 1.f);
  // whole islands of the given faces
  std::vector<uint8_t> want(m.islandCount + 1, 0);
  for (uint32_t f : faces)
    if (f < m.faceCount() && m.faceSize(f)) want[std::min(m.loopIsland[m.faceStart[f]], m.islandCount)] = 1;
  std::vector<uint32_t> all;
  for (uint32_t f = 0; f < m.faceCount(); ++f)
    if (m.faceSize(f) && want[std::min(m.loopIsland[m.faceStart[f]], m.islandCount)]) all.push_back(f);
  auto islands = facesByIsland(m, all);
  int done = 0;
  for (auto& isl : islands) {
    std::vector<uint32_t> loops;
    for (uint32_t f : isl)
      for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) loops.push_back(l);
    std::vector<vec2> before(loops.size());
    double areaBefore = 0;
    for (size_t i = 0; i < loops.size(); ++i) before[i] = m.loopUV[loops[i]];
    for (uint32_t f : isl) areaBefore += m.faceAreaUV(f);
    UnwrapOptions o;
    o.pack = false;
    unwrapLSCM(m, isl, o);
    // the conformal layout, turned, scaled (to the island's old area) and moved onto the old one
    double areaAfter = 0;
    for (uint32_t f : isl) areaAfter += m.faceAreaUV(f);
    if (!(std::abs(areaAfter) > 1e-30) || !std::isfinite(areaAfter)) {
      for (size_t i = 0; i < loops.size(); ++i) m.loopUV[loops[i]] = before[i];
      continue;
    }
    bool mirrored = (areaAfter < 0) != (areaBefore < 0);
    dvec2 cb(0), ca(0);
    for (size_t i = 0; i < loops.size(); ++i) {
      dvec2 q(m.loopUV[loops[i]]);
      if (mirrored) q.x = -q.x;
      cb += dvec2(before[i]), ca += q;
    }
    cb /= double(loops.size()), ca /= double(loops.size());
    double sa = 0, sb = 0;
    for (size_t i = 0; i < loops.size(); ++i) {
      dvec2 q(m.loopUV[loops[i]]);
      if (mirrored) q.x = -q.x;
      q -= ca;
      dvec2 p = dvec2(before[i]) - cb;
      sa += q.x * p.x + q.y * p.y;
      sb += q.x * p.y - q.y * p.x;
    }
    double ang = std::atan2(sb, sa), co = std::cos(ang), si = std::sin(ang);
    double s = std::sqrt(std::abs(areaBefore) / std::abs(areaAfter));
    for (size_t i = 0; i < loops.size(); ++i) {
      dvec2 q(m.loopUV[loops[i]]);
      if (mirrored) q.x = -q.x;
      q -= ca;
      dvec2 r = cb + s * dvec2(co * q.x - si * q.y, si * q.x + co * q.y);
      m.loopUV[loops[i]] = vec2(glm::mix(dvec2(before[i]), r, double(strength)));
    }
    ++done;
  }
  return done;
}

}  // namespace patina::uv
