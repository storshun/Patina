#include "core/RetopoLayout.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <unordered_map>

#include "core/Bvh.h"

namespace patina::retopo {

namespace {

// a 2D frame in a triangle's plane
struct TriFrame {
  vec3 o, x, y, n;
  vec2 to2(vec3 p) const { return vec2(glm::dot(p - o, x), glm::dot(p - o, y)); }
  vec3 to3(vec2 q) const { return o + x * q.x + y * q.y; }
  vec2 dir2(vec3 d) const { return vec2(glm::dot(d, x), glm::dot(d, y)); }
  vec3 dir3(vec2 d) const { return x * d.x + y * d.y; }
};

float cross2(vec2 a, vec2 b) { return a.x * b.y - a.y * b.x; }

struct Seg {
  vec2 a, b;  // in the triangle's frame
};

struct Tracer {
  uint32_t tri;
  vec3 p, d;
  int entry = -1, entry2 = -1;  // the edge it came in by (both edges at the vertex it started from)
  uint32_t fromVertex = ~0u;    // started at this singular vertex
  std::vector<vec3> line;
  std::vector<uint32_t> verts;  // the mesh vertex nearest each point of the line
  bool done = false;
  int steps = 0;
};

}  // namespace

FieldLayout traceLayout(const Mesh& m, const CrossField& F, const std::vector<Stroke>& strokes,
                        const std::vector<int32_t>& painted, const LayoutOptions& opt) {
  FieldLayout out;
  const uint32_t T = (uint32_t)m.tris.size(), nf = m.faceCount();
  out.faceLabel.assign(nf, 0);
  if (T == 0) return out;
  const float L0 = opt.edgeLength;
  auto tv = [&](uint32_t t, int k) { return m.loopVert[m.tris[t][k]]; };
  // ---- triangle neighbors across each edge (-1: none, the mesh's edge)
  std::vector<std::array<int64_t, 3>> nb(T, {-1, -1, -1});
  std::vector<std::array<int8_t, 3>> nbEdge(T, {-1, -1, -1});
  {
    std::unordered_map<uint64_t, std::vector<std::pair<uint32_t, int>>> byEdge;
    byEdge.reserve(T * 2);
    for (uint32_t t = 0; t < T; ++t)
      for (int k = 0; k < 3; ++k) {
        uint32_t a = tv(t, k), b = tv(t, (k + 1) % 3);
        byEdge[(uint64_t(std::min(a, b)) << 32) | std::max(a, b)].push_back({t, k});
      }
    for (auto& [key, list] : byEdge)
      if (list.size() == 2) {
        nb[list[0].first][list[0].second] = list[1].first, nbEdge[list[0].first][list[0].second] = (int8_t)list[1].second;
        nb[list[1].first][list[1].second] = list[0].first, nbEdge[list[1].first][list[1].second] = (int8_t)list[0].second;
      }
  }
  std::vector<TriFrame> fr(T);
  for (uint32_t t = 0; t < T; ++t) {
    vec3 a = m.positions[tv(t, 0)], b = m.positions[tv(t, 1)], c = m.positions[tv(t, 2)];
    vec3 n = glm::cross(b - a, c - a);
    n = glm::length(n) > 1e-30f ? glm::normalize(n) : F.normal[m.triFace[t]];
    vec3 x = glm::length(b - a) > 1e-30f ? glm::normalize(b - a) : glm::normalize(glm::cross(n, vec3(0.577f)));
    fr[t] = {a, x, glm::cross(n, x), n};
  }
  auto paintedOf = [&](uint32_t t) { return painted.size() == nf ? painted[m.triFace[t]] : 0; };
  std::vector<std::vector<Seg>> segs(T);
  std::vector<int8_t> singAt(m.positions.size(), 0);
  for (uint32_t v : F.singular) singAt[v] = F.index[v];
  out.singularities = (int)F.singular.size();

  // the field's direction in triangle t nearest d
  auto fieldDir = [&](uint32_t t, vec3 d) {
    uint32_t f = m.triFace[t];
    vec3 best(0);
    float bd = -2;
    for (int k = 0; k < 4; ++k) {
      vec3 q = F.dir(f, k);
      q -= fr[t].n * glm::dot(q, fr[t].n);
      if (glm::length(q) < 1e-20f) continue;
      q = glm::normalize(q);
      if (float dd = glm::dot(q, d); dd > bd) bd = dd, best = q;
    }
    return best;
  };
  // one step: from p in triangle t along d to where it leaves t (or meets a line, or a singularity)
  // returns false when the line ends
  auto step = [&](Tracer& tr, bool followField, vec3 aim, bool wall) -> bool {
    uint32_t t = tr.tri;
    const TriFrame& f = fr[t];
    vec2 p = f.to2(tr.p), d = f.dir2(tr.d);
    if (glm::length(d) < 1e-20f) return false;
    d = glm::normalize(d);
    vec2 P[3] = {f.to2(m.positions[tv(t, 0)]), f.to2(m.positions[tv(t, 1)]), f.to2(m.positions[tv(t, 2)])};
    float bestS = 1e30f;
    int exitK = -1;
    for (int k = 0; k < 3; ++k) {
      if (k == tr.entry || k == tr.entry2) continue;
      vec2 A = P[k], B = P[(k + 1) % 3], e = B - A;
      float den = cross2(d, e);
      if (std::abs(den) < 1e-20f) continue;
      float s = cross2(A - p, e) / den, u = cross2(A - p, d) / den;
      if (s > 1e-9f && u >= -1e-4f && u <= 1 + 1e-4f && s < bestS) bestS = s, exitK = k;
    }
    if (exitK < 0) return false;
    vec2 q = p + d * bestS;
    float endS = bestS;
    bool stop = false;
    vec3 endPoint;
    // a line already there
    for (auto& sg : segs[t]) {
      vec2 e = sg.b - sg.a;
      float den = cross2(d, e);
      if (std::abs(den) < 1e-20f) continue;
      float s = cross2(sg.a - p, e) / den, u = cross2(sg.a - p, d) / den;
      if (s > 1e-6f * (1 + bestS) && s < endS && u >= -1e-4f && u <= 1 + 1e-4f) endS = s, stop = true;
    }
    if (stop) q = p + d * endS;
    endPoint = f.to3(q);
    // a singularity close to the step (not the one it started from)
    for (int k = 0; k < 3; ++k) {
      uint32_t v = tv(t, k);
      if (!singAt[v] || v == tr.fromVertex) continue;
      vec2 V = P[k];
      float s = std::clamp(glm::dot(V - p, d), 0.f, endS);
      float dist = glm::length(p + d * s - V);
      if (dist < 0.25f * L0 && s <= endS) {
        endPoint = m.positions[v], stop = true;
        q = V;
        break;
      }
    }
    segs[t].push_back({p, q});
    tr.line.push_back(endPoint);
    tr.p = endPoint;
    {  // the nearest mesh vertex: where it crossed, the nearer end of that edge; else of the triangle
      uint32_t best = tv(t, 0);
      float bd = 1e30f;
      for (int k = 0; k < 3; ++k) {
        if (!stop && k != exitK && k != (exitK + 1) % 3) continue;
        float dd = glm::length(m.positions[tv(t, k)] - endPoint);
        if (dd < bd) bd = dd, best = tv(t, k);
      }
      tr.verts.push_back(best);
    }
    if (stop) return false;
    // into the next triangle
    int64_t nt = nb[t][exitK];
    if (nt < 0) return false;
    int ne = nbEdge[t][exitK];
    if (wall && paintedOf(t) != paintedOf((uint32_t)nt)) return false;  // a painted border
    // the direction, unfolded over the shared edge
    vec3 A = m.positions[tv(t, exitK)], B = m.positions[tv(t, (exitK + 1) % 3)], e = glm::normalize(B - A);
    vec3 dd = tr.d;
    float along = glm::dot(dd, e);
    vec3 perp = dd - e * along;
    float across = glm::length(perp);
    vec3 inward = glm::normalize(glm::cross(fr[nt].n, e));  // in the new plane, across the edge
    vec3 cnew = (m.positions[tv((uint32_t)nt, 0)] + m.positions[tv((uint32_t)nt, 1)] + m.positions[tv((uint32_t)nt, 2)]) / 3.f;
    if (glm::dot(inward, cnew - A) < 0) inward = -inward;
    vec3 d2 = e * along + inward * across;
    tr.tri = (uint32_t)nt, tr.entry = ne, tr.entry2 = -1;
    if (followField) {
      vec3 fd = fieldDir((uint32_t)nt, glm::normalize(d2));
      if (glm::length(fd) < 1e-20f) return false;
      tr.d = fd;
    } else {
      vec3 a = aim - tr.p;
      a -= fr[nt].n * glm::dot(a, fr[nt].n);
      tr.d = glm::length(a) > 1e-20f ? glm::normalize(a) : glm::normalize(d2);
    }
    return ++tr.steps < 400000;
  };

  // ---- strokes first: lines of their own (walked straight from point to point across the surface)
  std::vector<std::vector<uint32_t>> strokeVerts;
  Bvh bvh;
  if (!strokes.empty()) bvh.build(m);
  for (auto& s : strokes) {
    size_t n = s.points.size();
    if (n < 2) continue;
    std::vector<vec3> pts = s.points;
    if (s.loop) pts.push_back(pts.front());
    RayHit h = bvh.closestPoint(m, pts[0], 1e30f);
    if (!h.hit) continue;
    Tracer tr;
    tr.tri = h.tri, tr.p = h.position, tr.line = {h.position};
    {
      uint32_t best = tv(h.tri, 0);
      for (int k = 1; k < 3; ++k)
        if (glm::length(m.positions[tv(h.tri, k)] - h.position) < glm::length(m.positions[best] - h.position)) best = tv(h.tri, k);
      tr.verts.push_back(best);
    }
    for (size_t i = 1; i < pts.size(); ++i) {
      RayHit hb = bvh.closestPoint(m, pts[i], 1e30f);
      if (!hb.hit) continue;
      vec3 target = hb.position;
      for (int guard = 0; guard < 100000; ++guard) {
        if (tr.tri == hb.tri) {  // there: the last bit inside the triangle
          vec2 a = fr[tr.tri].to2(tr.p), b = fr[tr.tri].to2(target);
          if (glm::length(b - a) > 1e-12f) segs[tr.tri].push_back({a, b});
          tr.p = target, tr.line.push_back(target), tr.entry = -1;
          uint32_t best = tv(tr.tri, 0);
          for (int k = 1; k < 3; ++k)
            if (glm::length(m.positions[tv(tr.tri, k)] - target) < glm::length(m.positions[best] - target)) best = tv(tr.tri, k);
          tr.verts.push_back(best);
          break;
        }
        vec3 a = target - tr.p;
        a -= fr[tr.tri].n * glm::dot(a, fr[tr.tri].n);
        if (glm::length(a) < 1e-12f) break;
        tr.d = glm::normalize(a);
        if (!step(tr, false, target, false)) break;
        if (glm::dot(target - tr.p, a) <= 0) break;  // passed it
      }
    }
    out.lines.push_back(tr.line);
    strokeVerts.push_back(tr.verts);
  }

  // ---- separatrices: from each singular vertex, the field's directions that leave it
  std::vector<Tracer> tracers;
  {
    std::vector<std::vector<uint32_t>> vt(m.positions.size());
    for (uint32_t v : F.singular) vt[v].reserve(8);
    for (uint32_t t = 0; t < T; ++t)
      for (int k = 0; k < 3; ++k)
        if (singAt[tv(t, k)]) vt[tv(t, k)].push_back(t);
    for (uint32_t v : F.singular) {
      vec3 pv = m.positions[v];
      for (uint32_t t : vt[v]) {
        int k = 0;
        while (tv(t, k) != v) ++k;
        vec3 a = m.positions[tv(t, (k + 1) % 3)] - pv, b = m.positions[tv(t, (k + 2) % 3)] - pv;
        uint32_t f = m.triFace[t];
        for (int j = 0; j < 4; ++j) {
          vec3 d = F.dir(f, j);
          d -= fr[t].n * glm::dot(d, fr[t].n);
          if (glm::length(d) < 1e-20f) continue;
          d = glm::normalize(d);
          // inside the corner: d = la a + lb b with both positive
          vec2 a2 = fr[t].dir2(a), b2 = fr[t].dir2(b), d2 = fr[t].dir2(d);
          float den = cross2(a2, b2);
          if (std::abs(den) < 1e-20f) continue;
          float la = cross2(d2, b2) / den, lb = cross2(a2, d2) / den;
          if (la <= 0 || lb <= 0) continue;
          Tracer tr;
          tr.tri = t, tr.p = pv, tr.d = d, tr.fromVertex = v, tr.line = {pv}, tr.verts = {v};
          tr.entry = k, tr.entry2 = (k + 2) % 3;  // not out by the two edges at the vertex
          tracers.push_back(tr);
        }
      }
    }
  }
  // all of them a step at a time, so the first to arrive stops the others (a motorcycle graph)
  for (bool any = true; any;) {
    any = false;
    for (auto& tr : tracers) {
      if (tr.done) continue;
      if (!step(tr, true, vec3(0), true)) tr.done = true;
      else any = true;
    }
  }
  for (auto& tr : tracers) out.lines.push_back(tr.line);

  // ---- walls: each line as a chain of mesh edges (through the vertices nearest it), the mesh's
  // edge and painted borders; the patches are the faces between the walls
  std::unordered_map<uint64_t, uint32_t> edgeOf;
  edgeOf.reserve(m.edges.size() * 2);
  std::vector<std::vector<uint32_t>> vadj(m.positions.size());
  for (uint32_t e = 0; e < m.edges.size(); ++e) {
    uint32_t a = m.edges[e].x, b = m.edges[e].y;
    edgeOf[(uint64_t(std::min(a, b)) << 32) | std::max(a, b)] = e;
    vadj[a].push_back(b), vadj[b].push_back(a);
  }
  std::vector<char> wall(m.edges.size(), 0);
  auto wallPath = [&](uint32_t a, uint32_t b) {  // the fewest edges from a to b (a few at most)
    if (a == b) return;
    if (auto it = edgeOf.find((uint64_t(std::min(a, b)) << 32) | std::max(a, b)); it != edgeOf.end()) {
      wall[it->second] = 1;
      return;
    }
    std::unordered_map<uint32_t, uint32_t> from = {{a, a}};
    std::vector<uint32_t> q = {a};
    for (size_t i = 0; i < q.size() && !from.count(b) && i < 4000; ++i)
      for (uint32_t w : vadj[q[i]])
        if (!from.count(w)) from[w] = q[i], q.push_back(w);
    if (!from.count(b)) return;
    for (uint32_t v = b; v != a; v = from[v]) {
      uint32_t u = from[v];
      wall[edgeOf[(uint64_t(std::min(u, v)) << 32) | std::max(u, v)]] = 1;
    }
  };
  auto wallLine = [&](const std::vector<uint32_t>& vs) {
    for (size_t i = 1; i < vs.size(); ++i) wallPath(vs[i - 1], vs[i]);
  };
  for (auto& vs : strokeVerts) wallLine(vs);
  for (auto& tr : tracers) wallLine(tr.verts);
  // faces across edges that aren't walls (and only within a painted patch)
  std::vector<std::vector<uint32_t>> edgeF(m.edges.size());
  for (uint32_t f = 0; f < nf; ++f)
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) edgeF[m.loopEdge[l]].push_back(f);
  auto paintedF = [&](uint32_t f) { return painted.size() == nf ? painted[f] : 0; };
  std::vector<int32_t> region(nf, -1);
  int nreg = 0;
  for (uint32_t f0 = 0; f0 < nf; ++f0) {
    if (region[f0] >= 0) continue;
    std::vector<uint32_t> stack = {f0};
    region[f0] = nreg;
    while (!stack.empty()) {
      uint32_t f = stack.back();
      stack.pop_back();
      for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) {
        uint32_t e = m.loopEdge[l];
        if (wall[e] || edgeF[e].size() != 2) continue;
        uint32_t g = edgeF[e][0] == f ? edgeF[e][1] : edgeF[e][0];
        if (region[g] >= 0 || paintedF(g) != paintedF(f)) continue;
        region[g] = nreg, stack.push_back(g);
      }
    }
    ++nreg;
  }
  // thin or tiny regions join the neighbor they share most border with
  for (int pass = 0; pass < 4; ++pass) {
    std::vector<double> area(nreg, 0.0), perim(nreg, 0.0);
    std::vector<std::map<int, double>> shared(nreg);
    for (uint32_t f = 0; f < nf; ++f) area[region[f]] += m.faceArea3D(f);
    for (uint32_t e = 0; e < m.edges.size(); ++e) {
      double len = glm::length(m.positions[m.edges[e].y] - m.positions[m.edges[e].x]);
      if (edgeF[e].size() == 1) perim[region[edgeF[e][0]]] += len;
      if (edgeF[e].size() != 2) continue;
      int a = region[edgeF[e][0]], b = region[edgeF[e][1]];
      if (a == b) continue;
      perim[a] += len, perim[b] += len;
      if (paintedF(edgeF[e][0]) == paintedF(edgeF[e][1])) shared[a][b] += len, shared[b][a] += len;
    }
    std::vector<int> to(nreg);
    for (int r = 0; r < nreg; ++r) to[r] = r;
    int joined = 0;
    for (int r = 0; r < nreg; ++r) {
      double width = perim[r] > 0 ? 2 * area[r] / perim[r] : 1e30;
      if ((width >= 0.6 * L0 && area[r] >= 1.5 * L0 * L0) || shared[r].empty()) continue;
      int best = -1;
      double bl = -1;
      for (auto& [o, len] : shared[r])
        if (len > bl && to[o] == o) bl = len, best = o;
      if (best < 0) continue;
      to[r] = best, ++joined;
    }
    if (!joined) break;
    out.merged += joined;
    for (uint32_t f = 0; f < nf; ++f) {
      int r = region[f];
      while (to[r] != r) r = to[r];
      region[f] = r;
    }
  }
  std::vector<int32_t> label(nreg, 0);
  int next = 0;
  for (uint32_t f = 0; f < nf; ++f) {
    int r = region[f];
    if (!label[r]) label[r] = ++next;
    out.faceLabel[f] = label[r];
  }
  out.patches = next;
  return out;
}

}  // namespace patina::retopo
