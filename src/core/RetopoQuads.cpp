#include "core/RetopoQuads.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <numeric>
#include <queue>
#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "core/Log.h"
#include "core/Parallel.h"
#include "core/RetopoDecimate.h"
#include "core/RetopoJoin.h"
#include "core/RetopoLocal.h"
#include "core/RetopoMirror.h"
#include "core/RetopoQuadLayout.h"
#include "core/RetopoRings.h"
#include "core/RetopoShell.h"
#include "optimizer.hpp"
#include "parametrizer.hpp"

namespace patina::retopo {

namespace {

// ---- a manifold copy of the high-poly's triangles
struct CleanMesh {
  std::vector<vec3> pos;
  std::vector<glm::uvec3> tri;
  std::vector<uint32_t> origin;  // per vertex: the vertex of the mesh it was made from
  double area = 0;
};
}  // namespace

struct FlowCache {
  std::mutex mutex;
  uint64_t key = 0;
  CleanMesh cm;
  std::vector<float> vLocal;
  int dropped = 0;
  // each zone group's QuadriFlow quads, by all that goes into them: a group a change didn't touch (the
  // body while the head's lines are drawn) is not made again (the last few kept)
  struct Group {
    uint64_t key = 0;
    std::vector<vec3> pos;
    std::vector<std::array<uint32_t, 4>> quads;
    std::vector<std::array<uint32_t, 3>> tris;
    size_t fieldSingular = 0;
    int held = 0, smoothing = 0, seed = 0, poles = 0;
    double score = 0, missed = 0;
  };
  std::mutex groupMutex;
  std::vector<Group> groupsMade;
};
std::shared_ptr<FlowCache> makeFlowCache() { return std::make_shared<FlowCache>(); }

namespace {

CleanMesh makeManifold(const Mesh& m, double minPieceArea, int* dropped) {
  CleanMesh out;
  std::vector<glm::uvec3> tris;
  tris.reserve(m.tris.size());
  for (auto& t : m.tris) {
    uint32_t a = m.loopVert[t.x], b = m.loopVert[t.y], c = m.loopVert[t.z];
    if (a == b || b == c || a == c) continue;
    vec3 n = glm::cross(m.positions[b] - m.positions[a], m.positions[c] - m.positions[a]);
    if (glm::dot(n, n) < 1e-30f) continue;
    tris.push_back({a, b, c});
  }
  const uint32_t T = (uint32_t)tris.size();
  // an edge is kept between two triangles when exactly two use it, in opposite directions
  std::unordered_map<uint64_t, std::vector<uint32_t>> uses;  // edge -> t*3+k (k: the edge from corner k)
  uses.reserve(T * 2);
  for (uint32_t t = 0; t < T; ++t)
    for (int k = 0; k < 3; ++k) {
      uint32_t a = tris[t][k], b = tris[t][(k + 1) % 3];
      uses[(uint64_t(std::min(a, b)) << 32) | std::max(a, b)].push_back(t * 3 + k);
    }
  std::vector<uint32_t> parent(T * 3);
  std::iota(parent.begin(), parent.end(), 0u);
  auto find = [&](uint32_t x) {
    while (parent[x] != x) x = parent[x] = parent[parent[x]];
    return x;
  };
  auto unite = [&](uint32_t a, uint32_t b) { parent[find(a)] = find(b); };
  std::vector<uint32_t> tparent(T);
  std::iota(tparent.begin(), tparent.end(), 0u);
  auto tfind = [&](uint32_t x) {
    while (tparent[x] != x) x = tparent[x] = tparent[tparent[x]];
    return x;
  };
  for (auto& [key, list] : uses) {
    if (list.size() != 2) continue;
    uint32_t t1 = list[0] / 3, k1 = list[0] % 3, t2 = list[1] / 3, k2 = list[1] % 3;
    uint32_t a1 = tris[t1][k1], a2 = tris[t2][k2];
    if (a1 == a2) continue;  // the same direction: the two face opposite ways
    // corner of a in t1 with corner of a in t2 (where t2's edge ends), and the same for b
    unite(t1 * 3 + k1, t2 * 3 + (k2 + 1) % 3);
    unite(t1 * 3 + (k1 + 1) % 3, t2 * 3 + k2);
    tparent[tfind(t1)] = tfind(t2);
  }
  // pieces: tiny ones are left out (loose bits, specks), the largest always stays
  std::unordered_map<uint32_t, double> pieceArea;
  for (uint32_t t = 0; t < T; ++t) {
    const vec3 &a = m.positions[tris[t].x], &b = m.positions[tris[t].y], &c = m.positions[tris[t].z];
    pieceArea[tfind(t)] += 0.5 * glm::length(glm::cross(b - a, c - a));
  }
  uint32_t largest = 0;
  double la = -1;
  for (auto& [p, a] : pieceArea)
    if (a > la) la = a, largest = p;
  std::unordered_map<uint32_t, uint32_t> newVert;
  newVert.reserve(T * 2);
  int drop = 0;
  for (uint32_t t = 0; t < T; ++t) {
    uint32_t piece = tfind(t);
    if (piece != largest && pieceArea[piece] < minPieceArea) {
      ++drop;
      continue;
    }
    glm::uvec3 nt;
    for (int k = 0; k < 3; ++k) {
      uint32_t r = find(t * 3 + k);
      auto it = newVert.find(r);
      if (it == newVert.end()) {
        it = newVert.emplace(r, (uint32_t)out.pos.size()).first;
        out.pos.push_back(m.positions[tris[t][k]]), out.origin.push_back(tris[t][k]);
      }
      nt[k] = it->second;
    }
    out.tri.push_back(nt);
    const vec3 &a = out.pos[nt.x], &b = out.pos[nt.y], &c = out.pos[nt.z];
    out.area += 0.5 * glm::length(glm::cross(b - a, c - a));
  }
  if (dropped) *dropped = drop;
  return out;
}

// Polylines of the borders between painted patches (chains of high-poly vertices).
std::vector<std::vector<vec3>> borderLines(const Mesh& m, const std::vector<int32_t>& label) {
  std::vector<std::vector<vec3>> out;
  if (label.size() != m.faceCount()) return out;
  std::vector<std::vector<uint32_t>> ef(m.edges.size());
  for (uint32_t f = 0; f < m.faceCount(); ++f)
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) ef[m.loopEdge[l]].push_back(f);
  // border edges, chained through vertices of degree 2 into lines
  std::vector<std::vector<uint32_t>> at(m.positions.size());
  std::vector<uint32_t> border;
  for (uint32_t e = 0; e < m.edges.size(); ++e) {
    if (ef[e].size() != 2 || label[ef[e][0]] == label[ef[e][1]]) continue;
    if (label[ef[e][0]] <= 0 && label[ef[e][1]] <= 0) continue;
    border.push_back(e);
    at[m.edges[e].x].push_back(e), at[m.edges[e].y].push_back(e);
  }
  std::vector<char> used(m.edges.size(), 0);
  for (uint32_t e0 : border) {
    if (used[e0]) continue;
    // walk both ways from e0 while the vertex continues the line (two border edges there)
    std::vector<uint32_t> verts = {m.edges[e0].x, m.edges[e0].y};
    used[e0] = 1;
    for (int dirn = 0; dirn < 2; ++dirn) {
      for (int guard = 0; guard < 1000000; ++guard) {
        uint32_t v = dirn ? verts.front() : verts.back();
        if (at[v].size() != 2) break;
        uint32_t e = used[at[v][0]] ? at[v][1] : at[v][0];
        if (used[e]) break;
        used[e] = 1;
        uint32_t w = m.edges[e].x == v ? m.edges[e].y : m.edges[e].x;
        if (dirn) verts.insert(verts.begin(), w);
        else verts.push_back(w);
      }
    }
    std::vector<vec3> line;
    for (uint32_t v : verts) line.push_back(m.positions[v]);
    out.push_back(std::move(line));
  }
  return out;
}

}  // namespace

// Two local moves that take poles away, repeated while they help: a quad whose opposite corners have
// three edges each is collapsed across that diagonal (the two become one vertex with four edges, the
// other two corners lose an edge each); and the edge between two quads is turned a step when the
// poles at its ends and beside it get fewer. Never makes a vertex with more than five edges.
// Returns how many moves.
int collapsePolePairs(std::vector<vec3>& pos, std::vector<std::array<uint32_t, 4>>& quads) {
  int done = 0;
  for (int round = 0; round < 8; ++round) {
    size_t nv = pos.size();
    std::vector<std::vector<uint32_t>> nb(nv);
    std::map<std::pair<uint32_t, uint32_t>, int> eu;
    for (auto& q : quads)
      for (int k = 0; k < 4; ++k) {
        uint32_t a = q[k], b = q[(k + 1) % 4];
        if (eu[{std::min(a, b), std::max(a, b)}]++ == 0) nb[a].push_back(b), nb[b].push_back(a);
      }
    std::vector<char> open(nv, 0);
    for (auto& [e, n] : eu)
      if (n != 2) open[e.first] = open[e.second] = 1;
    auto pole = [&](int d) { return d != 4 ? 1 : 0; };
    std::vector<char> touched(nv, 0), gone(quads.size(), 0);
    std::vector<uint32_t> remap(nv);
    for (uint32_t v = 0; v < nv; ++v) remap[v] = v;
    int now = 0;
    for (size_t qi = 0; qi < quads.size(); ++qi) {
      auto& q = quads[qi];
      for (int k = 0; k < 2; ++k) {
        uint32_t a = q[k], b = q[k + 1], c = q[(k + 2) % 4], d = q[(k + 3) % 4];
        if (open[a] || open[b] || open[c] || open[d] || touched[a] || touched[b] || touched[c] || touched[d]) continue;
        int da = (int)nb[a].size(), dc = (int)nb[c].size(), db = (int)nb[b].size(), dd = (int)nb[d].size();
        int before = pole(da) + pole(dc) + pole(db) + pole(dd);
        int after = pole(da + dc - 2) + pole(db - 1) + pole(dd - 1);
        if (after >= before || db - 1 < 3 || dd - 1 < 3 || da + dc - 2 > 5) continue;
        // a and c may share only b and d (else two quads would end up on one edge)
        int shared = 0;
        for (uint32_t x : nb[a])
          if (std::find(nb[c].begin(), nb[c].end(), x) != nb[c].end()) ++shared;
        if (shared != 2) continue;
        pos[a] = 0.5f * (pos[a] + pos[c]);
        remap[c] = a;
        gone[qi] = 1;
        touched[a] = touched[b] = touched[c] = touched[d] = 1;
        for (uint32_t x : nb[a]) touched[x] = 1;
        for (uint32_t x : nb[c]) touched[x] = 1;
        ++now;
        break;
      }
    }
    // edges between two quads, turned a step when the poles at their ends and across get fewer (an
    // edge between two five-edge vertices, with three-edge ones beside it)
    std::map<std::pair<uint32_t, uint32_t>, std::vector<size_t>> quadsOf;
    for (size_t qi = 0; qi < quads.size(); ++qi)
      if (!gone[qi])
        for (int k = 0; k < 4; ++k) quadsOf[{std::min(quads[qi][k], quads[qi][(k + 1) % 4]), std::max(quads[qi][k], quads[qi][(k + 1) % 4])}].push_back(qi);
    auto normalOf = [&](const std::array<uint32_t, 4>& q) {
      return glm::cross(pos[q[2]] - pos[q[0]], pos[q[3]] - pos[q[1]]);
    };
    for (auto& [e, qs] : quadsOf) {
      if (qs.size() != 2) continue;
      uint32_t u = e.first, v = e.second;
      if (open[u] || open[v] || touched[u] || touched[v]) continue;
      // Q1 = (u, v, p, q), Q2 = (v, u, r, s): the hexagon u r s v p q
      auto rot = [&](std::array<uint32_t, 4> Q, uint32_t a, uint32_t b) {  // Q starting with a, b
        for (int k = 0; k < 4; ++k) {
          if (Q[0] == a && Q[1] == b) return Q;
          std::rotate(Q.begin(), Q.begin() + 1, Q.end());
        }
        return std::array<uint32_t, 4>{~0u, 0, 0, 0};
      };
      auto Q1 = rot(quads[qs[0]], u, v), Q2 = rot(quads[qs[1]], v, u);
      if (Q1[0] == ~0u || Q2[0] == ~0u) {
        Q1 = rot(quads[qs[1]], u, v), Q2 = rot(quads[qs[0]], v, u);
        if (Q1[0] == ~0u || Q2[0] == ~0u) continue;
        std::swap(qs[0], qs[1]);
      }
      uint32_t p = Q1[2], q = Q1[3], r = Q2[2], s = Q2[3];
      if (touched[p] || touched[q] || touched[r] || touched[s]) continue;
      int du = (int)nb[u].size(), dv = (int)nb[v].size();
      auto has = [&](uint32_t x, uint32_t y) { return eu.count({std::min(x, y), std::max(x, y)}) > 0; };
      vec3 n0 = normalOf(Q1) + normalOf(Q2);
      for (int option = 0; option < 2; ++option) {
        uint32_t x = option ? s : r, y = option ? q : p;  // the new diagonal x-y
        int dx = (int)nb[x].size(), dy = (int)nb[y].size();
        int before = pole(du) + pole(dv) + pole(dx) + pole(dy);
        int after = pole(du - 1) + pole(dv - 1) + pole(dx + 1) + pole(dy + 1);
        if (after >= before || du - 1 < 3 || dv - 1 < 3 || dx + 1 > 5 || dy + 1 > 5 || has(x, y) || x == y) continue;
        std::array<uint32_t, 4> A, B;
        if (!option) A = {r, s, v, p}, B = {p, q, u, r};
        else A = {s, v, p, q}, B = {q, u, r, s};
        if (glm::dot(normalOf(A), n0) <= 0 || glm::dot(normalOf(B), n0) <= 0) continue;
        quads[qs[0]] = A, quads[qs[1]] = B;
        for (uint32_t w : {u, v, p, q, r, s}) touched[w] = 1;
        ++now;
        break;
      }
    }
    if (!now) break;
    std::vector<std::array<uint32_t, 4>> next;
    for (size_t qi = 0; qi < quads.size(); ++qi) {
      if (gone[qi]) continue;
      auto q = quads[qi];
      for (auto& v : q) v = remap[v];
      next.push_back(q);
    }
    quads = std::move(next);
    done += now;
  }
  // unused vertices out
  std::vector<int64_t> keep(pos.size(), -1);
  std::vector<vec3> np;
  for (auto& q : quads)
    for (auto& v : q) {
      if (keep[v] < 0) keep[v] = (int64_t)np.size(), np.push_back(pos[v]);
      v = (uint32_t)keep[v];
    }
  pos = std::move(np);
  return done;
}

namespace {

// After the zones are joined: pairs of poles in the joining rows and patches taken out as in
// collapsePolePairs (a quad's two opposite poles made one, an edge turned a step), never moving or
// turning what's kept (vertices on the lines, in rings, beside triangles: protect). The vertices are
// not renumbered (one made one with another is left unused). Returns how many.
int cleanJoinPoles(std::vector<vec3>& pos, std::vector<std::array<uint32_t, 4>>& quads, std::vector<int>& quadGroup,
                   const std::vector<std::array<uint32_t, 3>>& tris, const std::vector<char>& protect) {
  int done = 0;
  const size_t nv = pos.size();
  for (int round = 0; round < 6; ++round) {
    std::vector<std::vector<uint32_t>> nb(nv);
    std::map<std::pair<uint32_t, uint32_t>, int> eu;
    auto addE = [&](uint32_t a, uint32_t b) {
      if (eu[{std::min(a, b), std::max(a, b)}]++ == 0) nb[a].push_back(b), nb[b].push_back(a);
    };
    for (auto& q : quads)
      for (int k = 0; k < 4; ++k) addE(q[k], q[(k + 1) % 4]);
    std::vector<char> fixedV(nv, 0);
    for (auto& t : tris)
      for (int k = 0; k < 3; ++k) addE(t[k], t[(k + 1) % 3]), fixedV[t[k]] = 1;
    for (auto& [e, n] : eu)
      if (n != 2) fixedV[e.first] = fixedV[e.second] = 1;
    for (size_t v = 0; v < nv && v < protect.size(); ++v) fixedV[v] |= protect[v];
    std::vector<std::vector<size_t>> quadsAt(nv);
    for (size_t qi = 0; qi < quads.size(); ++qi)
      for (uint32_t x : quads[qi]) quadsAt[x].push_back(qi);
    // (a pole on a line counts twice: the line's edge loop breaks there)
    const int lineW = 2;
    auto poleAt = [&](uint32_t v, int d) { return d != 4 ? (v < protect.size() && protect[v] ? lineW : 1) : 0; };
    auto normalOf = [&](const std::array<uint32_t, 4>& q) { return glm::cross(pos[q[2]] - pos[q[0]], pos[q[3]] - pos[q[1]]); };
    std::vector<char> touched(nv, 0), gone(quads.size(), 0);
    std::vector<uint32_t> remap(nv);
    std::iota(remap.begin(), remap.end(), 0u);
    int now = 0;
    // a quad's two opposite vertices made one (both free to move)
    for (size_t qi = 0; qi < quads.size(); ++qi) {
      const auto q = quads[qi];
      for (int k = 0; k < 2; ++k) {
        uint32_t a = q[k], b = q[k + 1], c = q[(k + 2) % 4], d = q[(k + 3) % 4];
        int da = (int)nb[a].size(), dc = (int)nb[c].size(), db = (int)nb[b].size(), dd = (int)nb[d].size();
        int before = poleAt(a, da) + poleAt(c, dc) + poleAt(b, db) + poleAt(d, dd);
        int after = poleAt(a, da + dc - 2) + poleAt(b, db - 1) + poleAt(d, dd - 1);
        if (after >= before || db - 1 < 3 || dd - 1 < 3 || da + dc - 2 > 5) continue;
        if (fixedV[a] || fixedV[c] || touched[a] || touched[b] || touched[c] || touched[d]) continue;
        int shared = 0;
        for (uint32_t x : nb[a])
          if (std::find(nb[c].begin(), nb[c].end(), x) != nb[c].end()) ++shared;
        if (shared != 2) continue;
        // (no quad round them turned over by the move)
        const vec3 m = 0.5f * (pos[a] + pos[c]);
        bool ok = true;
        for (uint32_t w : {a, c})
          for (size_t qj : quadsAt[w]) {
            if (qj == qi || !ok) continue;
            auto Q = quads[qj];
            vec3 n0 = normalOf(Q);
            for (auto& x : Q) x = x == a || x == c ? UINT32_MAX : x;
            std::array<vec3, 4> pts;
            for (int j = 0; j < 4; ++j) pts[j] = Q[j] == UINT32_MAX ? m : pos[Q[j]];
            ok &= glm::dot(glm::cross(pts[2] - pts[0], pts[3] - pts[1]), n0) > 0;
          }
        if (!ok) continue;
        pos[a] = m;
        remap[c] = a;
        gone[qi] = 1;
        touched[a] = touched[b] = touched[c] = touched[d] = 1;
        for (uint32_t x : nb[a]) touched[x] = 1;
        for (uint32_t x : nb[c]) touched[x] = 1;
        ++now;
        break;
      }
    }
    // an edge between two quads turned a step (not one along a line: both its ends kept)
    std::map<std::pair<uint32_t, uint32_t>, std::vector<size_t>> quadsOf;
    for (size_t qi = 0; qi < quads.size(); ++qi)
      if (!gone[qi])
        for (int k = 0; k < 4; ++k) quadsOf[{std::min(quads[qi][k], quads[qi][(k + 1) % 4]), std::max(quads[qi][k], quads[qi][(k + 1) % 4])}].push_back(qi);
    for (auto& [e, qs] : quadsOf) {
      if (qs.size() != 2) continue;
      uint32_t u = e.first, v = e.second;
      if ((protect.size() > std::max(u, v) && protect[u] && protect[v]) || touched[u] || touched[v]) continue;
      if (eu[{u, v}] != 2) continue;
      auto rot = [&](std::array<uint32_t, 4> Q, uint32_t a, uint32_t b) {
        for (int k = 0; k < 4; ++k) {
          if (Q[0] == a && Q[1] == b) return Q;
          std::rotate(Q.begin(), Q.begin() + 1, Q.end());
        }
        return std::array<uint32_t, 4>{~0u, 0, 0, 0};
      };
      auto Q1 = rot(quads[qs[0]], u, v), Q2 = rot(quads[qs[1]], v, u);
      if (Q1[0] == ~0u || Q2[0] == ~0u) {
        Q1 = rot(quads[qs[1]], u, v), Q2 = rot(quads[qs[0]], v, u);
        if (Q1[0] == ~0u || Q2[0] == ~0u) continue;
        std::swap(qs[0], qs[1]);
      }
      uint32_t p = Q1[2], q = Q1[3], r = Q2[2], s2 = Q2[3];
      if (touched[p] || touched[q] || touched[r] || touched[s2]) continue;
      // (a vertex beside a triangle or on an open edge keeps its edges)
      bool nearFixed = false;
      for (uint32_t w : {u, v, p, q, r, s2}) nearFixed |= (fixedV[w] && !(w < protect.size() && protect[w]));
      if (nearFixed) continue;
      int du = (int)nb[u].size(), dv = (int)nb[v].size();
      auto has = [&](uint32_t x, uint32_t y) { return eu.count({std::min(x, y), std::max(x, y)}) > 0; };
      vec3 n0 = normalOf(Q1) + normalOf(Q2);
      for (int option = 0; option < 2; ++option) {
        uint32_t x = option ? s2 : r, y = option ? q : p;
        int dx = (int)nb[x].size(), dy = (int)nb[y].size();
        // (a new edge from a vertex on a line only where that gives it four)
        if ((x < protect.size() && protect[x] && dx + 1 != 4) || (y < protect.size() && protect[y] && dy + 1 != 4)) continue;
        int before = poleAt(u, du) + poleAt(v, dv) + poleAt(x, dx) + poleAt(y, dy);
        int after = poleAt(u, du - 1) + poleAt(v, dv - 1) + poleAt(x, dx + 1) + poleAt(y, dy + 1);
        if (after >= before || du - 1 < 3 || dv - 1 < 3 || dx + 1 > 5 || dy + 1 > 5 || has(x, y) || x == y) continue;
        std::array<uint32_t, 4> A, B;
        if (!option) A = {r, s2, v, p}, B = {p, q, u, r};
        else A = {s2, v, p, q}, B = {q, u, r, s2};
        if (glm::dot(normalOf(A), n0) <= 0 || glm::dot(normalOf(B), n0) <= 0) continue;
        quads[qs[0]] = A, quads[qs[1]] = B;
        for (uint32_t w : {u, v, p, q, r, s2}) touched[w] = 1;
        ++now;
        break;
      }
    }
    if (!now) break;
    std::vector<std::array<uint32_t, 4>> next;
    std::vector<int> nextG;
    for (size_t qi = 0; qi < quads.size(); ++qi) {
      if (gone[qi]) continue;
      auto q = quads[qi];
      for (auto& v : q) v = remap[v];
      next.push_back(q), nextG.push_back(qi < quadGroup.size() ? quadGroup[qi] : -1);
    }
    quads = std::move(next), quadGroup = std::move(nextG);
    done += now;
  }
  return done;
}

using EdgeKey = uint64_t;
inline EdgeKey ekey(uint32_t a, uint32_t b) { return (uint64_t(std::min(a, b)) << 32) | std::max(a, b); }

float segDist2(const vec3& x, const vec3& a, const vec3& b, vec3* at = nullptr) {
  vec3 e = b - a;
  float len2 = glm::dot(e, e), u = len2 > 0 ? std::clamp(glm::dot(x - a, e) / len2, 0.f, 1.f) : 0.f;
  vec3 q = a + e * u;
  if (at) *at = q;
  return glm::dot(q - x, q - x);
}

// The edge lines laid into the working mesh: for each, a path along the mesh's edges from point to
// point that keeps close to the line (Dijkstra), its vertices then moved onto the line along the
// surface (unless that would fold a triangle over). Returns the path's edges.
std::unordered_set<EdgeKey> layEdgeLines(CleanMesh& m, const std::vector<std::vector<vec3>>& lines, const std::vector<char>& isEdge,
                                         float wide) {
  std::unordered_set<EdgeKey> out;
  bool any = false;
  for (size_t i = 0; i < lines.size(); ++i) any |= isEdge[i] && lines[i].size() > 1;
  if (!any) return out;
  const size_t nv = m.pos.size();
  std::vector<std::vector<uint32_t>> adj(nv), vface(nv);
  std::vector<vec3> nrm(nv, vec3(0));
  for (uint32_t t = 0; t < m.tri.size(); ++t) {
    auto T = m.tri[t];
    vec3 fn = glm::cross(m.pos[T.y] - m.pos[T.x], m.pos[T.z] - m.pos[T.x]);
    for (int k = 0; k < 3; ++k) {
      uint32_t a = T[k], b = T[(k + 1) % 3];
      if (std::find(adj[a].begin(), adj[a].end(), b) == adj[a].end()) adj[a].push_back(b), adj[b].push_back(a);
      vface[a].push_back(t), nrm[a] += fn;
    }
  }
  for (auto& n : nrm) n = glm::dot(n, n) > 0 ? glm::normalize(n) : vec3(0, 0, 1);
  const float cell = std::max(4 * wide, 1e-6f);
  auto ckey = [&](const vec3& p) {
    glm::ivec3 c = glm::ivec3(glm::floor(p / cell));
    return (uint64_t(c.x & 0x1FFFFF) << 42) | (uint64_t(c.y & 0x1FFFFF) << 21) | uint64_t(c.z & 0x1FFFFF);
  };
  std::unordered_map<uint64_t, std::vector<uint32_t>> grid;
  for (uint32_t v = 0; v < nv; ++v) grid[ckey(m.pos[v])].push_back(v);
  auto nearest = [&](const vec3& p) {
    int64_t best = -1;
    float bd = 1e30f;
    glm::ivec3 c = glm::ivec3(glm::floor(p / cell));
    for (int r = 1; r <= 4 && best < 0; ++r)
      for (int dz = -r; dz <= r; ++dz)
        for (int dy = -r; dy <= r; ++dy)
          for (int dx = -r; dx <= r; ++dx) {
            glm::ivec3 q = c + glm::ivec3(dx, dy, dz);
            auto it = grid.find((uint64_t(q.x & 0x1FFFFF) << 42) | (uint64_t(q.y & 0x1FFFFF) << 21) | uint64_t(q.z & 0x1FFFFF));
            if (it == grid.end()) continue;
            for (uint32_t v : it->second)
              if (float d = glm::dot(m.pos[v] - p, m.pos[v] - p); d < bd) bd = d, best = v;
          }
    return best;
  };
  auto folds = [&](uint32_t v, const vec3& p) {
    for (uint32_t t : vface[v]) {
      vec3 P[3], Q[3];
      for (int k = 0; k < 3; ++k) P[k] = m.pos[m.tri[t][k]], Q[k] = m.tri[t][k] == v ? p : P[k];
      vec3 n0 = glm::cross(P[1] - P[0], P[2] - P[0]), n1 = glm::cross(Q[1] - Q[0], Q[2] - Q[0]);
      if (glm::dot(n0, n1) < 0.15f * glm::dot(n0, n0)) return true;
    }
    return false;
  };
  std::vector<char> moved(nv, 0);
  for (size_t li = 0; li < lines.size(); ++li) {
    if (!isEdge[li]) continue;
    const auto& pl = lines[li];
    std::vector<uint32_t> path;
    for (size_t i = 0; i < pl.size(); ++i) {
      int64_t v = nearest(pl[i]);
      if (v < 0) continue;
      if (path.empty()) {
        path.push_back((uint32_t)v);
        continue;
      }
      if ((uint32_t)v == path.back()) continue;
      const vec3 &a = pl[i - 1], &b = pl[i];
      std::unordered_map<uint32_t, std::pair<float, uint32_t>> got = {{path.back(), {0.f, path.back()}}};
      std::priority_queue<std::pair<float, uint32_t>, std::vector<std::pair<float, uint32_t>>, std::greater<>> pq;
      pq.push({0.f, path.back()});
      bool reached = false;
      for (int pops = 0; !pq.empty() && pops < 20000; ++pops) {
        auto [d, u] = pq.top();
        pq.pop();
        if (d > got[u].first) continue;
        if (u == (uint32_t)v) {
          reached = true;
          break;
        }
        for (uint32_t w : adj[u]) {
          float len = glm::length(m.pos[w] - m.pos[u]), off2 = segDist2(0.5f * (m.pos[u] + m.pos[w]), a, b) / (wide * wide);
          float nd = d + len * (1 + 4 * off2);
          auto it = got.find(w);
          if (it == got.end() || nd < it->second.first) got[w] = {nd, u}, pq.push({nd, w});
        }
      }
      if (!reached) {
        path.push_back((uint32_t)v);
        continue;
      }
      std::vector<uint32_t> seg;
      for (uint32_t w = (uint32_t)v; w != path.back(); w = got[w].second) seg.push_back(w);
      path.insert(path.end(), seg.rbegin(), seg.rend());
    }
    for (uint32_t v : path) {
      if (moved[v]) continue;
      vec3 best = m.pos[v], q;
      float bd = 1e30f;
      for (size_t i = 0; i + 1 < pl.size(); ++i)
        if (float d = segDist2(m.pos[v], pl[i], pl[i + 1], &q); d < bd) bd = d, best = q;
      vec3 d = best - m.pos[v];
      d -= nrm[v] * glm::dot(nrm[v], d);
      if (!folds(v, m.pos[v] + d)) m.pos[v] += d;
      moved[v] = 1;
    }
    for (size_t i = 0; i + 1 < path.size(); ++i)
      if (path[i] != path[i + 1] && std::find(adj[path[i]].begin(), adj[path[i]].end(), path[i + 1]) != adj[path[i]].end())
        out.insert(ekey(path[i], path[i + 1]));
  }
  return out;
}

// The boundary loops of a set of quads, each in the direction its quads run along it (the quad on the
// left). Returns false where the boundary isn't simple (a vertex with two ways on).
bool boundaryLoops(const std::vector<std::array<uint32_t, 4>>& quads, const std::vector<char>* skip,
                   std::vector<std::vector<uint32_t>>& loops, const std::vector<std::array<uint32_t, 3>>* tris = nullptr,
                   const std::vector<char>* skipT = nullptr) {
  std::unordered_map<EdgeKey, int> uses;
  auto each = [&](auto&& fn) {
    for (size_t q = 0; q < quads.size(); ++q)
      if (!skip || !(*skip)[q])
        for (int k = 0; k < 4; ++k) fn(quads[q][k], quads[q][(k + 1) % 4]);
    if (tris)
      for (size_t t = 0; t < tris->size(); ++t)
        if (!skipT || !(*skipT)[t])
          for (int k = 0; k < 3; ++k) fn((*tris)[t][k], (*tris)[t][(k + 1) % 3]);
  };
  each([&](uint32_t a, uint32_t b) { uses[ekey(a, b)]++; });
  std::unordered_map<uint32_t, uint32_t> next;
  bool simple = true;
  each([&](uint32_t a, uint32_t b) {
    if (uses[ekey(a, b)] != 1) return;
    if (next.count(a)) simple = false;
    next[a] = b;
  });
  if (!simple) return false;
  std::unordered_set<uint32_t> done;
  for (auto& [start, nx] : next) {
    if (done.count(start)) continue;
    std::vector<uint32_t> loop;
    uint32_t v = start;
    for (size_t guard = 0; guard <= next.size(); ++guard) {
      if (done.count(v)) break;
      done.insert(v), loop.push_back(v);
      auto it = next.find(v);
      if (it == next.end()) return false;
      v = it->second;
    }
    if (v != start) return false;
    loops.push_back(std::move(loop));
  }
  return true;
}

}  // namespace

std::vector<std::vector<uint32_t>> smallHoles(const std::vector<std::array<uint32_t, 4>>& quads, const std::vector<std::array<uint32_t, 3>>* tris) {
  std::unordered_map<EdgeKey, int> uses;
  auto each = [&](auto&& fn) {
    for (auto& q : quads)
      for (int k = 0; k < 4; ++k) fn(q[k], q[(k + 1) % 4]);
    if (tris)
      for (auto& t : *tris)
        for (int k = 0; k < 3; ++k) fn(t[k], t[(k + 1) % 3]);
  };
  each([&](uint32_t a, uint32_t b) { uses[ekey(a, b)]++; });
  std::unordered_map<uint32_t, std::vector<uint32_t>> next;
  each([&](uint32_t a, uint32_t b) {
    if (uses[ekey(a, b)] == 1) next[a].push_back(b);
  });
  static const std::vector<uint32_t> none;
  auto outs = [&](uint32_t v) -> const std::vector<uint32_t>& {
    auto it = next.find(v);
    return it == next.end() ? none : it->second;
  };
  // (not a face's own border: a face on its own)
  std::set<std::vector<uint32_t>> faces;
  auto sorted = [](std::vector<uint32_t> v) {
    std::sort(v.begin(), v.end());
    return v;
  };
  for (auto& q : quads) faces.insert(sorted({q[0], q[1], q[2], q[3]}));
  if (tris)
    for (auto& t : *tris) faces.insert(sorted({t[0], t[1], t[2]}));
  std::vector<std::vector<uint32_t>> out;
  auto add = [&](std::vector<uint32_t> h) {
    if (!faces.count(sorted(h))) out.push_back(std::move(h));
  };
  for (auto& [a, bs] : next)
    for (uint32_t b : bs)
      for (uint32_t c : outs(b)) {
        if (c == a || c == b) continue;
        if (std::find(outs(c).begin(), outs(c).end(), a) != outs(c).end()) {
          if (a < b && a < c) add({a, b, c});  // (each once: from its lowest vertex)
          continue;
        }
        for (uint32_t d : outs(c))
          if (d != a && d != b && d != c && a < b && a < c && a < d && std::find(outs(d).begin(), outs(d).end(), a) != outs(d).end())
            add({a, b, c, d});
      }
  return out;
}

namespace {

// The edge lines laid into a quad mesh: along each, the path of quad edges that keeps closest to it
// (Dijkstra between points of the line a quad or so apart), its vertices moved onto the line (unless a
// quad would fold). QuadriFlow's own loops follow the lines only roughly. Returns the vertices moved.
int layLinesInQuads(std::vector<vec3>& pos, const std::vector<std::array<uint32_t, 4>>& quads, const std::vector<std::vector<vec3>>& lines,
                    const std::vector<char>& isEdge, float L) {
  const size_t nv = pos.size();
  std::vector<std::vector<uint32_t>> adj(nv), vq(nv);
  for (uint32_t qi = 0; qi < quads.size(); ++qi)
    for (int k = 0; k < 4; ++k) {
      uint32_t a = quads[qi][k], b = quads[qi][(k + 1) % 4];
      if (std::find(adj[a].begin(), adj[a].end(), b) == adj[a].end()) adj[a].push_back(b), adj[b].push_back(a);
      vq[a].push_back(qi);
    }
  const float cell = std::max(L, 1e-6f);
  auto key = [](glm::ivec3 c) { return (uint64_t(c.x & 0x1FFFFF) << 42) | (uint64_t(c.y & 0x1FFFFF) << 21) | uint64_t(c.z & 0x1FFFFF); };
  std::unordered_map<uint64_t, std::vector<uint32_t>> grid;
  for (uint32_t v = 0; v < nv; ++v)
    if (!adj[v].empty()) grid[key(glm::ivec3(glm::floor(pos[v] / cell)))].push_back(v);
  auto nearest = [&](const vec3& p, float reach) {
    glm::ivec3 c = glm::ivec3(glm::floor(p / cell));
    float bd = reach * reach;
    int64_t best = -1;
    for (int dz = -1; dz <= 1; ++dz)
      for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
          auto it = grid.find(key(c + glm::ivec3(dx, dy, dz)));
          if (it == grid.end()) continue;
          for (uint32_t v : it->second)
            if (float d = glm::dot(pos[v] - p, pos[v] - p); d < bd) bd = d, best = v;
        }
    return best;
  };
  auto quadNormal = [&](uint32_t qi, uint32_t v, const vec3& p) {
    vec3 P[4];
    for (int k = 0; k < 4; ++k) P[k] = quads[qi][k] == v ? p : pos[quads[qi][k]];
    return glm::cross(P[2] - P[0], P[3] - P[1]);
  };
  std::vector<char> moved(nv, 0);
  int count = 0;
  for (size_t li = 0; li < lines.size(); ++li) {
    if (!isEdge[li] || lines[li].size() < 2) continue;
    const auto& pl = lines[li];
    // points along the line about half a quad apart
    std::vector<vec3> sp = {pl[0]};
    float carry = 0;
    for (size_t i = 0; i + 1 < pl.size(); ++i) {
      vec3 a = pl[i], b = pl[i + 1];
      float len = glm::length(b - a), t = 0.5f * L - carry;
      while (t <= len) sp.push_back(glm::mix(a, b, t / std::max(len, 1e-12f))), t += 0.5f * L;
      carry = len - (t - 0.5f * L);
    }
    if (glm::length(sp.back() - pl.back()) > 1e-6f) sp.push_back(pl.back());
    std::vector<uint32_t> path;
    auto flush = [&] {
      for (uint32_t v : path) {
        if (moved[v]) continue;
        vec3 best = pos[v], q;
        float bd = 1e30f;
        for (size_t i = 0; i + 1 < pl.size(); ++i)
          if (float d = segDist2(pos[v], pl[i], pl[i + 1], &q); d < bd) bd = d, best = q;
        if (bd > 0.25f * L * L) continue;
        bool ok = true;
        for (uint32_t qi : vq[v]) {
          vec3 n0 = quadNormal(qi, v, pos[v]), n1 = quadNormal(qi, v, best);
          if (glm::dot(n0, n1) < 0.2f * glm::dot(n0, n0)) ok = false;
        }
        if (ok) pos[v] = best, moved[v] = 1, ++count;
      }
      path.clear();
    };
    for (size_t i = 0; i < sp.size(); ++i) {
      int64_t v = nearest(sp[i], 0.6f * L);
      if (v < 0) {
        flush();
        continue;
      }
      if (path.empty()) {
        path.push_back((uint32_t)v);
        continue;
      }
      if ((uint32_t)v == path.back()) continue;
      const vec3 &a = sp[i - 1], &b = sp[i];
      std::unordered_map<uint32_t, std::pair<float, uint32_t>> got = {{path.back(), {0.f, path.back()}}};
      std::priority_queue<std::pair<float, uint32_t>, std::vector<std::pair<float, uint32_t>>, std::greater<>> pq;
      pq.push({0.f, path.back()});
      bool reached = false;
      for (int pops = 0; !pq.empty() && pops < 4000; ++pops) {
        auto [d, u] = pq.top();
        pq.pop();
        if (d > got[u].first) continue;
        if (u == (uint32_t)v) {
          reached = true;
          break;
        }
        for (uint32_t w : adj[u]) {
          float off2 = segDist2(0.5f * (pos[u] + pos[w]), a, b) / (L * L);
          float nd = d + glm::length(pos[w] - pos[u]) * (1 + 16 * off2);
          auto it = got.find(w);
          if (it == got.end() || nd < it->second.first) got[w] = {nd, u}, pq.push({nd, w});
        }
      }
      if (!reached) {
        flush();
        path.push_back((uint32_t)v);
        continue;
      }
      std::vector<uint32_t> seg;
      for (uint32_t w = (uint32_t)v; w != path.back(); w = got[w].second) seg.push_back(w);
      path.insert(path.end(), seg.rbegin(), seg.rend());
    }
    flush();
  }
  return count;
}

}  // namespace

// The ring zones' loops (each zone of kind 1: its two border loops), their lengths in the zone's quads
// (Lz), whether each meets a zone of kind 0 (else the model's open edge, a disc or another ring), and
// the counts planned along each chain of rings (planRingCounts). zoneOf: per working triangle.
struct ChainPlan {
  std::vector<RingChainLoop> loops;
  std::vector<vec3> center;
  std::vector<double> length;              // (world units)
  std::vector<std::vector<vec3>> points;   // (each loop's vertices, along the working triangles)
  std::vector<std::array<int, 2>> rings;   // each planned ring zone's two loops
  std::vector<int> ringZone;               // (and the zone)
  std::vector<long> counts;                // per loop
  // a ring's steps in all (inside it, and where its loops meet other zones), as planned
  double cost(size_t i) const {
    auto [a, b] = rings[i];
    double c = std::abs(double(counts[a] - counts[b]));
    for (int l : {a, b})
      if (loops[l].meetsZone) c += std::abs(double(counts[l]) - loops[l].lengthQ);
    return c;
  }
};
ChainPlan planChains(const CleanMesh& cm, const std::unordered_map<uint64_t, std::array<int64_t, 2>>& edgeTris, const std::vector<int>& zoneOf,
                     const std::vector<char>& kind, const std::vector<float>& Lz) {
  ChainPlan cp;
  auto ek = [](uint32_t a, uint32_t b) { return (uint64_t(std::min(a, b)) << 32) | std::max(a, b); };
  std::vector<std::vector<uint32_t>> trisOf(kind.size());
  for (uint32_t t = 0; t < (uint32_t)zoneOf.size(); ++t)
    if (zoneOf[t] >= 0 && zoneOf[t] < (int)kind.size() && kind[zoneOf[t]] == 1) trisOf[zoneOf[t]].push_back(t);
  std::map<uint32_t, int> loopAt;  // (a loop's lowest vertex -> its index)
  for (int g = 0; g < (int)kind.size(); ++g) {
    if (kind[g] != 1 || trisOf[g].empty()) continue;
    const float Lg = Lz[g];
    std::unordered_map<uint32_t, uint32_t> next;
    std::unordered_map<uint32_t, int> other;  // (from a border vertex: the zone across its edge; -1 open)
    bool simple = true;
    for (uint32_t t : trisOf[g]) {
      for (int k = 0; k < 3 && simple; ++k) {
        uint32_t a = cm.tri[t][k], b = cm.tri[t][(k + 1) % 3];
        auto it = edgeTris.find(ek(a, b));
        if (it == edgeTris.end()) continue;
        int64_t o = it->second[0] == (int64_t)t ? it->second[1] : it->second[0];
        if (o >= 0 && zoneOf[o] == g) continue;
        simple &= next.emplace(a, b).second;
        other[a] = o < 0 ? -1 : zoneOf[o];
      }
    }
    if (!simple) continue;
    std::array<int, 2> ends{-1, -1};
    int nl = 0;
    std::unordered_set<uint32_t> done;
    for (auto& [s0, s1] : next) {
      if (done.count(s0)) continue;
      std::vector<uint32_t> L;
      uint32_t v = s0;
      while (!done.count(v) && next.count(v)) done.insert(v), L.push_back(v), v = next[v];
      if (v != s0 || nl >= 2) {
        nl = 3;
        break;
      }
      uint32_t key = *std::min_element(L.begin(), L.end());
      // (its length from a copy smoothed over about a quad: the loop zigzags along the working triangles)
      std::vector<vec3> sm;
      for (uint32_t x : L) sm.push_back(cm.pos[x]);
      double raw = 0;
      for (size_t i = 0; i < sm.size(); ++i) raw += glm::length(sm[(i + 1) % sm.size()] - sm[i]);
      const double ePer = raw / std::max<size_t>(1, sm.size());
      const int passes = (int)std::clamp(ePer > 0 ? (Lg / ePer) * (Lg / ePer) : 8.0, 8.0, 400.0);
      for (int it = 0; it < passes; ++it) {
        std::vector<vec3> nx(sm.size());
        for (size_t i = 0; i < sm.size(); ++i) nx[i] = 0.5f * sm[i] + 0.25f * (sm[(i + sm.size() - 1) % sm.size()] + sm[(i + 1) % sm.size()]);
        sm = std::move(nx);
      }
      double len = 0;
      vec3 c(0);
      for (size_t i = 0; i < sm.size(); ++i) len += glm::length(sm[(i + 1) % sm.size()] - sm[i]), c += sm[i] / float(sm.size());
      std::map<int, int> votes;
      for (uint32_t x : L) votes[other.count(x) ? other[x] : -1]++;
      int most = -1, mv = -1;
      for (auto [h, cnt] : votes)
        if (cnt > mv) mv = cnt, most = h;
      auto [it, fresh] = loopAt.emplace(key, (int)cp.loops.size());
      if (fresh) {
        cp.loops.push_back({len / Lg, most >= 0 && most < (int)kind.size() && kind[most] == 0});
        cp.center.push_back(c), cp.length.push_back(len);
        std::vector<vec3> P;
        for (uint32_t x : L) P.push_back(cm.pos[x]);
        cp.points.push_back(std::move(P));
      }
      ends[nl++] = it->second;
    }
    if (nl == 2) cp.rings.push_back(ends), cp.ringZone.push_back(g);
  }
  cp.counts = planRingCounts(cp.loops, cp.rings);
  return cp;
}

static Result flowQuadsOnce(const Mesh& high, const Bvh& bvh, const std::vector<Stroke>& strokes, const std::vector<int32_t>& faceLabel,
                            const std::vector<float>& vertexDensity, const FlowOptions& opt, const std::vector<float>& faceResolution) {
  auto t0 = std::chrono::steady_clock::now();
  Result res;
  const float L0 = 1.f / std::sqrt(std::max(opt.quadsPerM2, 1e-3f));
  res.edgeLength = L0;
  int dropped = 0;
  auto cancelled = [&] { return opt.cancel && opt.cancel->load(std::memory_order_relaxed); };
  auto since = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
  const bool hasDens = vertexDensity.size() == high.positions.size();
  const bool hasRes = faceResolution.size() == high.faceCount();
  double maxDens = 1, maxRes = 1;
  if (hasDens)
    for (float d : vertexDensity) maxDens = std::max(maxDens, (double)std::clamp(d, 0.05f, 50.f));
  if (hasRes)
    for (float r : faceResolution) maxRes = std::max(maxRes, (double)std::clamp(r, 0.05f, 50.f));
  // the smallest quads asked for
  const float Lmin = L0 / (float)std::sqrt(maxDens * maxRes);
  // the working surface: the high-poly itself when it is clean and not too detailed for the quads, else
  // a voxel shell at about the quads' detail
  // the working surface: the high-poly itself when it is light enough for the quads; else a lighter copy
  // of its own surface (decimated to about a third of the smallest quads); a voxel shell only when asked
  // for (it joins fingers and wraps hair like shrink-wrap)
  bool useShell = opt.shell == 1, decimate = false;
  {
    double area = 0;
    for (uint32_t f = 0; f < high.faceCount(); ++f) area += high.faceArea3D(f);
    double trisPerQuad = high.tris.size() / std::max(1.0, area * opt.quadsPerM2 * maxDens * maxRes);
    decimate = !useShell && opt.shell != 0 && (high.tris.size() > 200000 || trisPerQuad > 40);
  }
  float cube = std::max(Lmin, 0.5f * L0) / std::max(1.f, opt.shellCubesPerQuad);
  // (kept between builds in opt.cache: the working surface depends on the mesh, the quads per m², the
  // density brush and the zones' resolutions, not on the lines and strokes drawn)
  uint64_t cacheKey = 0;
  if (opt.cache) {
    auto mix = [](uint64_t h, uint64_t w) {
      h ^= w * 0x9E3779B97F4A7C15ull;
      h = (h ^ (h >> 31)) * 0xBF58476D1CE4E5B9ull;
      return h ^ (h >> 29);
    };
    auto hashFloats = [&](const std::vector<float>& v) {
      std::vector<uint64_t> part(64, 0);
      parallelFor(64, [&](size_t k) {
        uint64_t h = k + 1;
        for (size_t i = v.size() * k / 64; i < v.size() * (k + 1) / 64; ++i) {
          uint32_t w;
          std::memcpy(&w, &v[i], 4);
          h = mix(h, w);
        }
        part[k] = h;
      });
      uint64_t h = v.size();
      for (uint64_t x : part) h = mix(h, x);
      return h;
    };
    cacheKey = mix(mix(mix((uint64_t)(uintptr_t)high.positions.data(), high.positions.size()), high.faceCount()), uint64_t(opt.shell + 7));
    uint32_t q;
    std::memcpy(&q, &opt.quadsPerM2, 4), cacheKey = mix(cacheKey, q);
    std::memcpy(&q, &opt.shellCubesPerQuad, 4), cacheKey = mix(cacheKey, q);
    if (!high.positions.empty()) {
      const vec3& p0 = high.positions[0];
      const vec3& p1 = high.positions.back();
      float c6[6] = {p0.x, p0.y, p0.z, p1.x, p1.y, p1.z};
      for (float f : c6) std::memcpy(&q, &f, 4), cacheKey = mix(cacheKey, q);
    }
    if (hasDens) cacheKey = mix(cacheKey, hashFloats(vertexDensity));
    if (hasRes) cacheKey = mix(cacheKey, hashFloats(faceResolution));
    cacheKey |= 1;
  }
  CleanMesh cm;
  std::vector<float> vLocal;
  bool cached = false;
  if (opt.cache) {
    std::lock_guard<std::mutex> lock(opt.cache->mutex);
    if (opt.cache->key == cacheKey) {
      cm = opt.cache->cm, vLocal = opt.cache->vLocal, dropped = opt.cache->dropped;
      cached = true;
    }
  }
  if (!cached) {
    Mesh shell;
    if (useShell) {
      auto ts = std::chrono::steady_clock::now();
      shell = voxelShell(high, bvh, cube, cube);
      PLOG_I("Retopology: working shell of %u triangles (%.1f mm cubes) in %.2f s", shell.faceCount(), cube * 1000,
             std::chrono::duration<double>(std::chrono::steady_clock::now() - ts).count());
    }
    cm = makeManifold(useShell ? shell : high, 4.0 * L0 * L0, &dropped);
    // the quads' own size at each working vertex (zones of high resolution and the density brush make it
    // smaller), for decimating or refining the working surface to suit
    vLocal.assign(cm.pos.size(), L0);
    if ((hasDens || hasRes) && cm.origin.size() == cm.pos.size()) {
      std::vector<float> vRes(high.positions.size(), 1.f);
      if (hasRes)
        for (uint32_t f = 0; f < high.faceCount(); ++f)
          for (uint32_t l = high.faceStart[f]; l < high.faceStart[f + 1]; ++l)
            vRes[high.loopVert[l]] = std::max(vRes[high.loopVert[l]], std::clamp(faceResolution[f], 0.05f, 50.f));
      for (size_t v = 0; v < cm.pos.size(); ++v) {
        uint32_t o = cm.origin[v];
        float d = (hasDens ? std::clamp(vertexDensity[o], 0.05f, 50.f) : 1.f) * vRes[o];
        vLocal[v] = L0 / std::sqrt(std::max(d, 1.f));
      }
    }
    if (decimate && cm.tri.size() >= 8) {
      auto ts = std::chrono::steady_clock::now();
      size_t before = cm.tri.size();
      // a third of the quads' size there: finer in zones of high resolution and where the density brush
      // asks for more, so a fine zone doesn't make the whole surface fine
      std::vector<float> tgt(vLocal);
      for (auto& t : tgt) t /= 3.f;
      decimateToEdge(cm.pos, cm.tri, tgt);
      cm.area = 0;
      for (auto& t : cm.tri) cm.area += 0.5 * glm::length(glm::cross(cm.pos[t.y] - cm.pos[t.x], cm.pos[t.z] - cm.pos[t.x]));
      PLOG_I("Retopology: working surface of %zu triangles (from %zu, edges about %.1f mm and up) in %.2f s", cm.tri.size(), before,
             Lmin / 3.f * 1000, std::chrono::duration<double>(std::chrono::steady_clock::now() - ts).count());
      cm.origin.clear();
      vLocal.clear();
    }
    if (opt.cache) {
      std::lock_guard<std::mutex> lock(opt.cache->mutex);
      opt.cache->key = cacheKey, opt.cache->cm = cm, opt.cache->vLocal = vLocal, opt.cache->dropped = dropped;
    }
  }
  if (cm.tri.size() < 4) {
    res.problems.push_back("the model has no surface to cover");
    return res;
  }
  PLOG_I("Retopology (flow): working surface ready at %.2f s", since());
  if (cancelled()) {
    res.cancelled = true;
    return res;
  }
  // the lines: strokes, and the painted patches' borders as edge loops
  std::vector<std::vector<vec3>> lines;
  std::vector<char> lineIsEdge;  // an exact edge loop (zone lines, edge strokes) or a direction only
  for (auto& s : strokes) {
    lines.push_back(s.points);
    if (s.loop && !s.points.empty()) lines.back().push_back(s.points.front());
    lineIsEdge.push_back(s.edge);
  }
  if (opt.loopsAtBorders)
    for (auto& l : borderLines(high, faceLabel)) lines.push_back(std::move(l)), lineIsEdge.push_back(1);
  bool anyEdge = false;
  for (char e : lineIsEdge) anyEdge |= e != 0;
  // small enough triangles to lay the lines in and to hold the quads (0.4 of their size where they are; a
  // decimated surface is a third already)
  // (a coarse model is refined even with nothing drawn: QuadriFlow fails on a few long thin triangles per
  // quad, as a stretched ball's)
  const bool coarse = (double)cm.tri.size() < 8.0 * cm.area * opt.quadsPerM2 * maxRes;
  if (!decimate && !useShell && (anyEdge || maxRes > 1 || coarse) && vLocal.size() == cm.pos.size()) {
    size_t before = cm.tri.size();
    std::vector<float> tgt(vLocal);
    for (auto& t : tgt) t *= 0.4f;  // (0.6 left QuadriFlow too little to follow lines with in narrow zones)
    refineToEdge(cm.pos, cm.tri, tgt);
    if (cm.tri.size() != before) PLOG_I("Retopology: working surface refined from %zu to %zu triangles", before, cm.tri.size());
  }
  std::unordered_set<EdgeKey> laid = layEdgeLines(cm, lines, lineIsEdge, 0.25f * Lmin);
  PLOG_D("Retopology (flow): lines laid at %.2f s", since());
  const uint32_t T = (uint32_t)cm.tri.size();
  // per working triangle: the painted density and the zone's resolution there
  std::vector<float> triDens(T, 1.f), triRes(T, 1.f), triArea(T);
  parallelFor(T, [&](size_t t) {
    auto tr = cm.tri[t];
    triArea[t] = 0.5f * glm::length(glm::cross(cm.pos[tr.y] - cm.pos[tr.x], cm.pos[tr.z] - cm.pos[tr.x]));
    if (!hasDens && !hasRes) return;
    RayHit h = bvh.closestPoint(high, (cm.pos[tr.x] + cm.pos[tr.y] + cm.pos[tr.z]) / 3.f, 1e30f);
    if (!h.hit) return;
    if (hasRes) triRes[t] = std::clamp(faceResolution[h.face], 0.05f, 50.f);
    if (hasDens) {
      float d = 0;
      uint32_t f = h.face, n = high.faceStart[f + 1] - high.faceStart[f];
      for (uint32_t l = high.faceStart[f]; l < high.faceStart[f + 1]; ++l) d += vertexDensity[high.loopVert[l]];
      triDens[t] = std::clamp(d / n, 0.05f, 50.f);
    }
  });
  // regions between the lines, each at one resolution (the one over most of its area); groups of
  // regions at the same resolution are made together
  std::unordered_map<EdgeKey, std::array<int64_t, 2>> edgeTris;
  edgeTris.reserve(T * 2);
  for (uint32_t t = 0; t < T; ++t)
    for (int k = 0; k < 3; ++k) {
      auto [it, fresh] = edgeTris.emplace(ekey(cm.tri[t][k], cm.tri[t][(k + 1) % 3]), std::array<int64_t, 2>{t, -1});
      if (!fresh) it->second[1] = t;
    }
  std::vector<int> group(T, 0);
  std::vector<double> groupRes = {1.0};
  std::string smallZones;  // (said with the result)
  std::vector<vec3> smallZoneMarks;
  std::vector<char> triInRing(T, 0);  // the triangle's own region is a ring zone (not one given to it later)
  std::vector<char> groupRing = {0};  // a ring zone (between two edge loops): filled with rings
  if (maxRes > 1 || hasRes || (opt.ringZones && anyEdge)) {
    std::vector<int> region(T, -1);
    int regions = 0;
    std::vector<uint32_t> stack;
    for (uint32_t t0r = 0; t0r < T; ++t0r) {
      if (region[t0r] >= 0) continue;
      region[t0r] = regions;
      stack = {t0r};
      while (!stack.empty()) {
        uint32_t t = stack.back();
        stack.pop_back();
        for (int k = 0; k < 3; ++k) {
          EdgeKey e = ekey(cm.tri[t][k], cm.tri[t][(k + 1) % 3]);
          if (laid.count(e)) continue;
          auto& tt = edgeTris[e];
          int64_t u = tt[0] == t ? tt[1] : tt[0];
          if (u >= 0 && region[u] < 0) region[u] = regions, stack.push_back((uint32_t)u);
        }
      }
      ++regions;
    }
    // slivers between lines drawn nearly on top of each other or crossing (smaller than about two quads) go to the
    // neighbor they share the most line with
    for (int round = 0; round < 3; ++round) {
      std::vector<double> rArea(regions, 0), rRes(regions, 0);
      for (uint32_t t = 0; t < T; ++t) rArea[region[t]] += triArea[t], rRes[region[t]] += triArea[t] * triRes[t];
      std::vector<std::map<int, double>> shared(regions);
      for (EdgeKey e : laid) {
        auto it = edgeTris.find(e);
        if (it == edgeTris.end() || it->second[1] < 0) continue;
        int a = region[it->second[0]], b = region[it->second[1]];
        if (a == b) continue;
        double l = glm::length(cm.pos[uint32_t(e >> 32)] - cm.pos[uint32_t(e & 0xFFFFFFFF)]);
        shared[a][b] += l, shared[b][a] += l;
      }
      std::vector<int> to(regions);
      std::iota(to.begin(), to.end(), 0);
      bool any = false;
      for (int r = 0; r < regions; ++r) {
        if (rArea[r] <= 0 || shared[r].empty()) continue;
        double L2 = 1.0 / (opt.quadsPerM2 * std::max(rRes[r] / rArea[r], 0.05));
        if (rArea[r] >= 2.0 * L2) continue;
        int nb = r;
        double most = -1;
        for (auto& [o, l] : shared[r])
          if (l > most || (l == most && rArea[o] > rArea[nb])) most = l, nb = o;
        to[r] = nb, any = true;
      }
      if (!any) break;
      std::function<int(int)> root = [&](int r) {
        for (int guard = 0; guard < regions && to[r] != r; ++guard) r = to[r];
        return r;
      };
      std::vector<int> rootOf(regions);
      for (int r = 0; r < regions; ++r) rootOf[r] = root(r);
      for (uint32_t t = 0; t < T; ++t) region[t] = rootOf[region[t]];
    }
    // zones smaller than a few of their own quads can't hold the loops their lines ask for: said, and
    // marked (their middles)
    {
      std::vector<double> ra(regions, 0), rr(regions, 0);
      std::vector<vec3> mid(regions, vec3(0));
      for (uint32_t t = 0; t < T; ++t) {
        ra[region[t]] += triArea[t], rr[region[t]] += triArea[t] * triRes[t];
        mid[region[t]] += (cm.pos[cm.tri[t].x] + cm.pos[cm.tri[t].y] + cm.pos[cm.tri[t].z]) * float(triArea[t] / 3);
      }
      int small = 0;
      for (int r = 0; r < regions; ++r) {
        if (ra[r] <= 0) continue;
        double quads = ra[r] * opt.quadsPerM2 * (rr[r] / ra[r]);
        if (quads >= 4) continue;
        ++small, smallZoneMarks.push_back(mid[r] / float(ra[r]));
      }
      if (small)
        smallZones = small == 1 ? "1 zone is smaller than four of its quads (marked in red), too small for the loops its lines ask "
                                  "for: give it, or the zone round it, a higher resolution, or draw its lines further apart"
                                : std::to_string(small) + " zones are smaller than four of their quads (marked in red), too small for "
                                                          "the loops their lines ask for: give them, or the zones round them, a higher "
                                                          "resolution, or draw their lines further apart";
    }
    std::vector<std::map<int, double>> areaAt(regions);  // resolution (in thousandths) -> area
    for (uint32_t t = 0; t < T; ++t) areaAt[region[t]][(int)std::lround(triRes[t] * 1000)] += triArea[t];
    std::map<int, int> groupOf;
    groupRes.clear(), groupRing.clear();
    std::vector<int> regionGroup(regions);
    // the regions that are rings (one piece between two edge loops, made of laid lines or the model's
    // open edges): each a group of its own
    // (and small discs (up to some 24 quads) inside such rings: closed with rings shrinking in)
    std::vector<char> regionRing(regions, 0);
    if (opt.ringZones) {
      std::vector<std::vector<glm::uvec3>> rt(regions);
      std::vector<double> ra(regions, 0), rr(regions, 0), rb(regions, 0);
      for (uint32_t t = 0; t < T; ++t) {
        rt[region[t]].push_back(cm.tri[t]), ra[region[t]] += triArea[t], rr[region[t]] += triArea[t] * triRes[t];
        for (int k = 0; k < 3; ++k) {  // (its outline's length)
          auto& tt = edgeTris[ekey(cm.tri[t][k], cm.tri[t][(k + 1) % 3])];
          int64_t u = tt[0] == t ? tt[1] : tt[0];
          if (u < 0 || region[u] != region[t]) rb[region[t]] += glm::length(cm.pos[cm.tri[t][k]] - cm.pos[cm.tri[t][(k + 1) % 3]]);
        }
      }
      // (regions told not to be rings: those holding the points given)
      std::vector<char> noRing(regions, 0);
      std::vector<vec3> noRingsAt = opt.noRingsAt;
      if (const char* e = getenv("PATINA_NORINGS_AT")) {  // (experiment)
        std::string str(e);
        size_t at = 0;
        while (at < str.size()) {
          size_t semi = str.find(';', at);
          std::string part = str.substr(at, semi == std::string::npos ? std::string::npos : semi - at);
          float x, y, z;
          if (sscanf(part.c_str(), "%f,%f,%f", &x, &y, &z) == 3) noRingsAt.push_back(vec3(x, y, z));
          if (semi == std::string::npos) break;
          at = semi + 1;
        }
      }
      for (const vec3& q : noRingsAt) {
        float bd = 1e30f;
        int br = -1;
        for (uint32_t t = 0; t < T; ++t) {
          vec3 c = (cm.pos[cm.tri[t].x] + cm.pos[cm.tri[t].y] + cm.pos[cm.tri[t].z]) / 3.f;
          if (float d = glm::dot(c - q, c - q); d < bd) bd = d, br = region[t];
        }
        if (br >= 0) noRing[br] = 1;
      }
      std::vector<char> smallCap(regions, 0);
      for (int r = 0; r < regions; ++r) {
        if (rt[r].size() < 8 || noRing[r]) continue;
        // (in its own quads: a ring at least half a quad wide on average, a disc of two to some 24 quads)
        const double quads = ra[r] * opt.quadsPerM2 * (rr[r] / std::max(ra[r], 1e-12));
        const double L = std::sqrt(ra[r] / std::max(quads, 1e-12));
        if (ra[r] / std::max(0.5 * rb[r], 1e-12) >= 0.5 * L && isRing(rt[r])) regionRing[r] = 1;
        // (and roundish: a long sliver between two lines is no cap; its outline is measured on the
        // working triangles, which zigzag, so the bar is low)
        else if (quads >= 2 && quads < 24 && 4 * kPi * ra[r] / std::max(rb[r] * rb[r], 1e-12) > 0.25 && isDisc(rt[r])) regionRing[r] = 2, smallCap[r] = 1;
      }
      // larger discs (an eyeball, the middle of an eye's rings) closed with a grid too, where nothing but
      // one ring and the model's open edge is round them: no joining rows then (beside other zones they're
      // made with them, with no rows either)
      std::vector<char> bigDisc(regions, 0);
      auto classifyDiscs = [&] {
        for (int r = 0; r < regions; ++r)
          if (bigDisc[r]) regionRing[r] = 0, bigDisc[r] = 0;
        std::vector<int> besideRing(regions, -1);  // (the ring beside it; -2: something else too)
        std::vector<char> besidePlain(regions, 0);  // (beside a zone made by QuadriFlow at its own resolution)
        auto resOf = [&](int r) { return rr[r] / std::max(ra[r], 1e-12); };
        for (uint32_t t = 0; t < T; ++t)
          for (int k = 0; k < 3; ++k) {
            auto& tt = edgeTris[ekey(cm.tri[t][k], cm.tri[t][(k + 1) % 3])];
            int64_t u = tt[0] == t ? tt[1] : tt[0];
            if (u < 0 || region[u] == region[t]) continue;
            int& b = besideRing[region[t]];
            if (regionRing[region[u]] != 1 || (b >= 0 && b != region[u])) b = -2;
            else if (b == -1) b = region[u];
            if (regionRing[region[u]] == 0 && std::abs(resOf(region[u]) / std::max(resOf(region[t]), 1e-12) - 1) < 0.2) besidePlain[region[t]] = 1;
          }
        // (a small cap beside such a zone, as the middle of an eye whose band went with the face, goes with
        // it too: its few points joined to the face's many took steps, or left a hole)
        for (int r = 0; r < regions; ++r)
          if (smallCap[r]) regionRing[r] = besidePlain[r] ? 0 : 2;
        for (int r = 0; r < regions; ++r) {
          if (rt[r].size() < 8 || noRing[r] || regionRing[r] || besideRing[r] == -2) continue;
          const double quads = ra[r] * opt.quadsPerM2 * (rr[r] / std::max(ra[r], 1e-12));
          // (roundish, and no more than a dome: a hand past a wrist line has far more surface than its
          // outline holds, and a grid over it would lose the fingers)
          const double roundness = 4 * kPi * ra[r] / std::max(rb[r] * rb[r], 1e-12);
          if (quads >= 2 && quads < 1500 && roundness > 0.25 && roundness < 2.5 && isDisc(rt[r])) regionRing[r] = 2, bigDisc[r] = 1;
        }
      };
      classifyDiscs();
      // a ring that would need many 3-to-1 steps (inside it, or where it meets the zone beside it: the
      // band round an eye socket, much longer outside than in) is made with that zone instead, which
      // turns its flow with a few poles of its own; the rings inside it then meet that zone
      {
        std::vector<float> Lr(regions, 1.f);
        for (int r = 0; r < regions; ++r)
          Lr[r] = 1.f / (float)std::sqrt(std::max(1e-6, opt.quadsPerM2 * (rr[r] / std::max(ra[r], 1e-12))));
        // (one at a time: the one whose going saves the most steps in all, the chains planned again
        // without it, while that saves more than four steps)
        auto total = [](const ChainPlan& cp) {
          double t = 0;
          for (size_t i = 0; i < cp.rings.size(); ++i) t += std::abs(double(cp.counts[cp.rings[i][0]] - cp.counts[cp.rings[i][1]]));
          for (size_t l = 0; l < cp.loops.size(); ++l)
            if (cp.loops[l].meetsZone) t += std::abs(double(cp.counts[l]) - cp.loops[l].lengthQ);
          return t;
        };
        for (int iter = 0; iter < 16; ++iter) {
          std::vector<char> kinds(regionRing.begin(), regionRing.end());
          ChainPlan cp = planChains(cm, edgeTris, region, kinds, Lr);
          const double now = total(cp);
          int best = -1;
          double bestCost = now - 8;
          for (size_t i = 0; i < cp.rings.size(); ++i) {
            auto [a, b] = cp.rings[i];
            if (!cp.loops[a].meetsZone && !cp.loops[b].meetsZone) continue;
            std::vector<char> k2 = kinds;
            k2[cp.ringZone[i]] = 0;
            const double without = total(planChains(cm, edgeTris, region, k2, Lr));
            if (getenv("PATINA_RETOPO_DEBUG"))
              PLOG_I("  try %d ring %zu (%ld to %ld round): steps in all %.1f, %.1f without it", iter, i, cp.counts[a], cp.counts[b], now / 2, without / 2);
            if (without < bestCost) bestCost = without, best = (int)i;
          }
          if (best < 0) break;
          regionRing[cp.ringZone[best]] = 0;
          PLOG_I("Retopology (flow): a ring zone (%ld to %ld round) made with the zone beside it: %.0f fewer 3-to-1 steps",
                 cp.counts[cp.rings[best][0]], cp.counts[cp.rings[best][1]], (now - bestCost) / 2);
        }
      }
      classifyDiscs();  // (again: a disc beside a ring made with the zone beside it goes with that zone)
      if (getenv("PATINA_RETOPO_DEBUG"))
        for (int r = 0; r < regions; ++r) {
          vec3 c(0);
          for (auto& t : rt[r]) c += cm.pos[t.x] / float(rt[r].size());
          PLOG_I("  region %d: %zu triangles, %.1f quads, at %.3f %.3f %.3f, roundness %.2f: %s", r, rt[r].size(), ra[r] * opt.quadsPerM2, c.x, c.y,
                 c.z, 4 * kPi * ra[r] / std::max(rb[r] * rb[r], 1e-12),
                 regionRing[r] == 1 ? "ring" : regionRing[r] == 2 ? "disc (closed with a grid)" : isDisc(rt[r]) ? "disc" : "other");
        }
    }
    for (int r = 0; r < regions; ++r) {
      int bestRes = 1000;
      double ba = -1;
      for (auto& [rv, a] : areaAt[r])
        if (a > ba) ba = a, bestRes = rv;
      if (regionRing[r]) {
        regionGroup[r] = (int)groupRes.size(), groupRes.push_back(bestRes / 1000.0), groupRing.push_back(regionRing[r]);
        continue;
      }
      auto it = groupOf.find(bestRes);
      if (it == groupOf.end())
        it = groupOf.emplace(bestRes, (int)groupRes.size()).first, groupRes.push_back(bestRes / 1000.0), groupRing.push_back(0);
      regionGroup[r] = it->second;
    }
    for (uint32_t t = 0; t < T; ++t) group[t] = regionGroup[region[t]], triInRing[t] = regionRing[region[t]] != 0;
  }
  int groups = (int)groupRes.size();
  PLOG_D("Retopology (flow): zones found at %.2f s", since());
  // parts of a group narrower than about one and a half of its quads (a zone pinched between two
  // others, the tip of a sharp corner) go to the group beside them: there is no room for the rows that
  // join the zones there, so they would cross
  if (groups > 1) {
    std::vector<std::vector<std::pair<uint32_t, float>>> adj(cm.pos.size());
    std::vector<char> onBorder(cm.pos.size(), 0);
    for (auto& [e, tt] : edgeTris) {
      uint32_t a = uint32_t(e >> 32), b = uint32_t(e & 0xFFFFFFFF);
      float len = glm::length(cm.pos[a] - cm.pos[b]);
      adj[a].push_back({b, len}), adj[b].push_back({a, len});
      if (tt[1] >= 0 && group[tt[0]] != group[tt[1]]) onBorder[a] = onBorder[b] = 1;
    }
    // each vertex's group (a border vertex: none) and the reach that counts as thin there
    std::vector<int> vGroup(cm.pos.size(), -1);
    for (uint32_t t = 0; t < T; ++t)
      for (int k = 0; k < 3; ++k)
        if (!onBorder[cm.tri[t][k]]) vGroup[cm.tri[t][k]] = group[t];
    // (a group coarser than one beside it loses half a quad along that border to the joining row, so
    // it needs more room)
    std::vector<char> coarser(groups, 0);
    for (auto& [e, tt] : edgeTris)
      if (tt[1] >= 0 && group[tt[0]] != group[tt[1]]) {
        int a = group[tt[0]], b = group[tt[1]];
        coarser[groupRes[a] < groupRes[b] ? a : b] = 1;
      }
    auto reachOf = [&](int g) { return (coarser[g] ? 1.25f : 0.75f) / (float)std::sqrt(opt.quadsPerM2 * groupRes[g]); };
    using QE = std::pair<float, uint32_t>;
    auto dijkstra = [&](std::vector<float>& d, const std::function<bool(uint32_t, uint32_t)>& may, float limit) {
      std::priority_queue<QE, std::vector<QE>, std::greater<>> pq;
      for (uint32_t v = 0; v < d.size(); ++v)
        if (d[v] == 0) pq.push({0.f, v});
      while (!pq.empty()) {
        auto [dv, u] = pq.top();
        pq.pop();
        if (dv > d[u]) continue;
        for (auto [w, len] : adj[u])
          if (dv + len < d[w] && dv + len <= limit && may(u, w)) d[w] = dv + len, pq.push({dv + len, w});
      }
    };
    float maxReach = 0;
    for (int g = 0; g < groups; ++g) maxReach = std::max(maxReach, reachOf(g));
    // how far each vertex is from the borders; the deep ones (at least the reach in)
    std::vector<float> fromBorder(cm.pos.size(), 1e30f);
    for (uint32_t v = 0; v < cm.pos.size(); ++v)
      if (onBorder[v]) fromBorder[v] = 0;
    dijkstra(fromBorder, [](uint32_t, uint32_t) { return true; }, 2 * maxReach);
    std::vector<float> fromDeep(cm.pos.size(), 1e30f);
    for (uint32_t v = 0; v < cm.pos.size(); ++v)
      if (vGroup[v] >= 0 && fromBorder[v] >= reachOf(vGroup[v])) fromDeep[v] = 0;
    // and back out from them by the same reach, within their group: what isn't reached is thin
    dijkstra(fromDeep, [&](uint32_t u, uint32_t w) { return vGroup[w] < 0 || vGroup[w] == vGroup[u] || vGroup[u] < 0; }, 1.3f * maxReach);
    std::vector<char> thin(T, 0);
    size_t thinCount = 0;
    for (uint32_t t = 0; t < T; ++t) {
      if (groupRing[group[t]]) continue;  // (a ring is filled as it is)
      float r = 1.3f * reachOf(group[t]), best = 1e30f;
      for (int k = 0; k < 3; ++k) best = std::min(best, fromDeep[cm.tri[t][k]]);
      if (best > r) thin[t] = 1, ++thinCount;
    }
    if (thinCount) {
      // each thin triangle to the nearest other group (across the triangles)
      std::vector<float> d(T, 1e30f);
      std::vector<int> to(T, -1);
      std::priority_queue<QE, std::vector<QE>, std::greater<>> pq;
      auto mid = [&](uint32_t t) { return (cm.pos[cm.tri[t].x] + cm.pos[cm.tri[t].y] + cm.pos[cm.tri[t].z]) / 3.f; };
      for (uint32_t t = 0; t < T; ++t)
        if (!thin[t]) d[t] = 0, to[t] = group[t], pq.push({0.f, t});
      while (!pq.empty()) {
        auto [dt, t] = pq.top();
        pq.pop();
        if (dt > d[t]) continue;
        for (int k = 0; k < 3; ++k) {
          auto& tt = edgeTris[ekey(cm.tri[t][k], cm.tri[t][(k + 1) % 3])];
          int64_t u = tt[0] == t ? tt[1] : tt[0];
          if (u < 0 || !thin[u] || to[t] == group[u]) continue;
          float nd = dt + glm::length(mid((uint32_t)u) - mid(t));
          if (nd < d[u]) d[u] = nd, to[u] = to[t], pq.push({nd, (uint32_t)u});
        }
      }
      size_t moved = 0;
      for (uint32_t t = 0; t < T; ++t)
        if (thin[t] && to[t] >= 0 && to[t] != group[t]) group[t] = to[t], triRes[t] = (float)groupRes[to[t]], ++moved;
      // a group left with nothing goes
      std::vector<size_t> count(groups, 0);
      for (uint32_t t = 0; t < T; ++t) count[group[t]]++;
      std::vector<int> renum(groups, -1);
      std::vector<double> res2;
      std::vector<char> ring2;
      for (int g = 0; g < groups; ++g)
        if (count[g]) renum[g] = (int)res2.size(), res2.push_back(groupRes[g]), ring2.push_back(groupRing[g]);
      for (uint32_t t = 0; t < T; ++t) group[t] = renum[group[t]];
      groupRes = res2, groupRing = ring2, groups = (int)groupRes.size();
      PLOG_I("Retopology (flow): %zu triangles in parts of zones too thin for their quads given to the zone beside them", moved);
    }
  }
  PLOG_D("Retopology (flow): thin parts done at %.2f s", since());
  // the borders between groups, split into chains where three groups (or an open edge) meet
  std::vector<JoinChain> chains;
  std::vector<std::vector<uint32_t>> chainVerts;  // (their working vertices)
  std::vector<std::array<vec3, 2>> openEdges;
  for (auto& [e, tt] : edgeTris)
    if (tt[1] < 0) openEdges.push_back({cm.pos[uint32_t(e >> 32)], cm.pos[uint32_t(e & 0xFFFFFFFF)]});
  auto buildChains = [&] {
    chains.clear(), chainVerts.clear();
    if (groups > 1) {
      std::unordered_map<uint32_t, std::vector<uint32_t>> at;
      std::unordered_map<EdgeKey, std::pair<int, int>> pairOf;
      std::unordered_set<uint32_t> onOpen;
      for (auto& [e, tt] : edgeTris) {
        uint32_t a = uint32_t(e >> 32), b = uint32_t(e & 0xFFFFFFFF);
        if (tt[1] < 0) {
          onOpen.insert(a), onOpen.insert(b);
          continue;
        }
        int ga = group[tt[0]], gb = group[tt[1]];
        if (ga == gb) continue;
        at[a].push_back(b), at[b].push_back(a);
        pairOf[e] = {std::min(ga, gb), std::max(ga, gb)};
      }
      auto node = [&](uint32_t v) {
        auto& nb = at[v];
        if (nb.size() != 2 || onOpen.count(v)) return true;
        return pairOf[ekey(v, nb[0])] != pairOf[ekey(v, nb[1])];
      };
      std::unordered_set<EdgeKey> used;
      auto walk = [&](uint32_t v0, uint32_t v1) {
        JoinChain ch;
        auto pr = pairOf[ekey(v0, v1)];
        ch.g = pr.first, ch.h = pr.second;
        std::vector<uint32_t> vs = {v0};
        uint32_t prev = v0, v = v1;
        used.insert(ekey(v0, v1));
        for (size_t guard = 0; guard <= at.size(); ++guard) {
          vs.push_back(v);
          if (v == v0) {
            ch.closed = true;
            break;
          }
          if (node(v)) break;
          auto& nb = at[v];
          uint32_t w = nb[0] == prev ? nb[1] : nb[0];
          if (used.count(ekey(v, w))) break;
          used.insert(ekey(v, w));
          prev = v, v = w;
        }
        for (uint32_t x : vs) ch.line.push_back(cm.pos[x]);
        chains.push_back(std::move(ch)), chainVerts.push_back(vs);
      };
      std::vector<uint32_t> keys;  // (in order, for the same chains every time)
      for (auto& [v, nb] : at) keys.push_back(v);
      std::sort(keys.begin(), keys.end());
      for (uint32_t v : keys)
        if (node(v))
          for (uint32_t w : std::vector<uint32_t>(at[v]))
            if (!used.count(ekey(v, w))) walk(v, w);
      for (uint32_t v : keys)  // (then loops with no such point)
        for (uint32_t w : std::vector<uint32_t>(at[v]))
          if (!used.count(ekey(v, w))) walk(v, w);
    }
  };
  buildChains();
  // necks: where a group is narrower than it needs between two different borders (a ring of its quads
  // goes to each joining row), it goes to the group beside it there
  if (chains.size() > 1) {
    std::vector<int> moveTo(T, -1);
    size_t necks = 0;
    for (int g = 0; g < groups; ++g) {
      if (groupRing[g]) continue;
      std::vector<int> mine;
      for (size_t ci = 0; ci < chains.size(); ++ci)
        if (chains[ci].g == g || chains[ci].h == g) mine.push_back((int)ci);
      if (mine.size() < 2) continue;
      const float L = 1.f / (float)std::sqrt(opt.quadsPerM2 * groupRes[g]);
      bool coarserHere = false;
      for (int ci : mine) coarserHere |= groupRes[chains[ci].g == g ? chains[ci].h : chains[ci].g] > groupRes[g];
      const float W = (coarserHere ? 2.5f : 1.5f) * L;
      // the edges of this group's triangles
      std::unordered_map<uint32_t, std::vector<std::pair<uint32_t, float>>> adj;
      for (auto& [e, tt] : edgeTris) {
        if (group[tt[0]] != g && (tt[1] < 0 || group[tt[1]] != g)) continue;
        uint32_t a = uint32_t(e >> 32), b = uint32_t(e & 0xFFFFFFFF);
        float len = glm::length(cm.pos[a] - cm.pos[b]);
        adj[a].push_back({b, len}), adj[b].push_back({a, len});
      }
      // the nearest two borders (different chains) of each vertex, within W
      struct Best {
        int c[2] = {-1, -1};
        float d[2] = {1e30f, 1e30f};
      };
      std::unordered_map<uint32_t, Best> best;
      using QL = std::tuple<float, uint32_t, int>;
      std::priority_queue<QL, std::vector<QL>, std::greater<>> pq;
      for (int ci : mine)
        for (uint32_t v : chainVerts[ci]) pq.push({0.f, v, ci});
      while (!pq.empty()) {
        auto [d, v, c] = pq.top();
        pq.pop();
        Best& B = best[v];
        if (B.c[0] == c || B.c[1] == c) continue;  // (this chain reached it sooner)
        int slot = B.c[0] < 0 ? 0 : B.c[1] < 0 ? 1 : -1;
        if (slot < 0) continue;
        B.c[slot] = c, B.d[slot] = d;
        auto it = adj.find(v);
        if (it == adj.end()) continue;
        for (auto [w, len] : it->second)
          if (d + len < W) pq.push({d + len, w, c});
      }
      for (uint32_t t = 0; t < T; ++t) {
        if (group[t] != g) continue;
        bool neck = true;
        int nearest = -1;
        float nd = 1e30f;
        for (int k = 0; k < 3 && neck; ++k) {
          auto it = best.find(cm.tri[t][k]);
          if (it == best.end() || it->second.c[1] < 0 || it->second.d[0] + it->second.d[1] >= W) neck = false;
          else if (it->second.d[0] < nd) nd = it->second.d[0], nearest = it->second.c[0];
        }
        if (!neck || nearest < 0) continue;
        int to = chains[nearest].g == g ? chains[nearest].h : chains[nearest].g;
        moveTo[t] = to, ++necks;
      }
    }
    if (necks) {
      for (uint32_t t = 0; t < T; ++t)
        if (moveTo[t] >= 0) group[t] = moveTo[t], triRes[t] = (float)groupRes[moveTo[t]];
      std::vector<size_t> count(groups, 0);
      for (uint32_t t = 0; t < T; ++t) count[group[t]]++;
      std::vector<int> renum(groups, -1);
      std::vector<double> res2;
      std::vector<char> ring2;
      for (int g = 0; g < groups; ++g)
        if (count[g]) renum[g] = (int)res2.size(), res2.push_back(groupRes[g]), ring2.push_back(groupRing[g]);
      for (uint32_t t = 0; t < T; ++t) group[t] = renum[group[t]];
      groupRes = res2, groupRing = ring2, groups = (int)groupRes.size();
      PLOG_I("Retopology (flow): %zu triangles in necks of zones (too narrow between two borders) given to the zone beside them", necks);
      buildChains();
    }
  }

  // a ring zone that took in a thin part must still be a ring (a small disc a disc), or it is made as
  // the other zones are
  if (groups > 1) {
    std::vector<std::vector<glm::uvec3>> gt(groups);
    for (uint32_t t = 0; t < T; ++t)
      if (groupRing[group[t]]) gt[group[t]].push_back(cm.tri[t]);
    bool changed = false;
    for (int g = 0; g < groups; ++g)
      if (groupRing[g] && !(groupRing[g] == 1 ? isRing(gt[g]) : isDisc(gt[g]))) groupRing[g] = 0, changed = true;
    if (changed) PLOG_I("Retopology (flow): a ring zone changed shape (thin parts given to it): made as other zones");
  }
  PLOG_D("Retopology (flow): borders found at %.2f s", since());
  // (in progress, 0.21) the quad layout's input: the working surface, the lines and the zones' quad sizes
  auto layoutInput = [&] {
    QuadLayoutInput li;
    li.pos = cm.pos, li.tri = cm.tri, li.lines = lines, li.lineIsEdge = lineIsEdge;
    if (const char* sm = getenv("PATINA_LAYOUT_SMOOTH")) li.fieldSmoothing = atoi(sm);
    li.lineEdges.insert(laid.begin(), laid.end());
    li.triZone.resize(T), li.triL.resize(T);
    for (uint32_t t = 0; t < T; ++t)
      li.triZone[t] = group[t], li.triL[t] = 1.f / (float)std::sqrt(std::max(1e-6, opt.quadsPerM2 * triDens[t] * groupRes[group[t]]));
    return li;
  };
  struct Variant {
    std::vector<vec3> pos;
    std::vector<std::array<uint32_t, 4>> quads;
    std::vector<std::array<uint32_t, 3>> tris;  // (tiny holes QuadriFlow left, filled)
    size_t fieldSingular = 0;
    int held = 0, smoothing = 0, seed = 0, poles = 0;
    bool ok = false;
    double score = 1e30;
    double missed = 0;  // (the share of the surface far from the quads, and of quads off it)
    double lineOff = 0;  // (the length of the laid lines no edge runs along, in quads)
    double fieldOff = 0;  // (the share of its edges more than 15 degrees off its own field)
    std::vector<std::vector<uint32_t>> ringLoops;  // a ring zone's two edge loops (its vertices)
    std::vector<std::vector<vec3>> ringLoopPos;
  };
  std::atomic<int> heldTotal{0};
  // QuadriFlow on one group's triangles: the laid lines' edges sharp (an edge loop runs exactly along
  // each), the direction strokes holding the field, the painted density as a size field, the open
  // edges (the model's, and the borders with other groups) kept exactly. Several fields at once
  // (seeds and smoothing); the one with the fewest poles near the count asked for is kept. The count
  // comes out only roughly as asked, so it is measured and put right over a run or two.
  auto makeGroup = [&](const CleanMesh& m, const std::vector<float>& mDens, const std::unordered_set<EdgeKey>& sharp,
                       double quadsPerM2, bool groupPainted) -> Variant {
    double meanD = 0, area = 0;
    for (size_t t = 0; t < m.tri.size(); ++t) {
      auto tr = m.tri[t];
      double a = 0.5 * glm::length(glm::cross(m.pos[tr.y] - m.pos[tr.x], m.pos[tr.z] - m.pos[tr.x]));
      meanD += a * mDens[t], area += a;
    }
    meanD = area > 0 ? meanD / area : 1;
    const int targetFaces = std::max(16, (int)std::lround(area * quadsPerM2 * meanD));
    const float Lg = 1.f / (float)std::sqrt(std::max(1e-6, quadsPerM2 * meanD));
    double edge = 0;
    {
      size_t n = std::min<size_t>(m.tri.size(), 5000);
      for (size_t i = 0; i < n; ++i) edge += glm::length(m.pos[m.tri[i].y] - m.pos[m.tri[i].x]) / n;
    }
    double ratio0 = Lg / std::max(edge, 1e-9);
    int smooth0 = opt.fieldSmoothing >= 0 ? opt.fieldSmoothing : std::clamp((int)std::lround(ratio0 * ratio0 * 0.5), 0, 60);
    // per working vertex: the painted density (mean of its triangles')
    std::vector<float> vDens(m.pos.size(), 0.f);
    {
      std::vector<int> n(m.pos.size(), 0);
      for (size_t t = 0; t < m.tri.size(); ++t)
        for (int k = 0; k < 3; ++k) vDens[m.tri[t][k]] += mDens[t], n[m.tri[t][k]]++;
      for (size_t v = 0; v < vDens.size(); ++v) vDens[v] = n[v] ? vDens[v] / n[v] : 1.f;
    }
    const uint32_t n0 = (uint32_t)m.pos.size();
    // the working triangles as a mesh, to check the results lie on them
    Mesh work;
    work.positions = m.pos;
    work.faceStart.push_back(0);
    for (auto& t : m.tri) {
      for (int k = 0; k < 3; ++k) work.loopVert.push_back(t[k]);
      work.faceStart.push_back((uint32_t)work.loopVert.size());
    }
    work.finalize();
    Bvh workBvh;
    workBvh.build(work);
    auto run = [&](int seed, int smoothing, double g, bool pinOpen) -> Variant {
      Variant out;
      qflow::Parametrizer field;
      field.hierarchy.rng_seed = seed;
      field.V.resize(3, m.pos.size());
      for (size_t i = 0; i < m.pos.size(); ++i) field.V.col(i) = Eigen::Vector3d(m.pos[i].x, m.pos[i].y, m.pos[i].z);
      field.F.resize(3, m.tri.size());
      for (size_t i = 0; i < m.tri.size(); ++i) field.F.col(i) = Eigen::Vector3i(m.tri[i].x, m.tri[i].y, m.tri[i].z);
      field.NormalizeMesh();
      field.patina_normal_smoothing = smoothing;  // the field follows the surface smoothed over about a quad
      field.flag_preserve_boundary = 1;
      auto tq0 = std::chrono::steady_clock::now();
      auto tq = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - tq0).count(); };
      field.Initialize(std::max(16, (int)std::lround(targetFaces * g)));
      const double tInit = tq();
      qflow::Hierarchy& H = field.hierarchy;
      H.clearConstraints();
      const Eigen::MatrixXd& V0 = H.mV[0];
      const Eigen::MatrixXi& F0 = H.mF;
      auto setConstraint = [&](int i, const Eigen::Vector3d& dir) {
        H.mCO[0].col(i) = V0.col(i);
        H.mCQ[0].col(i) = dir;
        H.mCQw[0][i] = H.mCOw[0][i] = 1.0;
      };
      // the open edges: held along them (half the tries; on some shapes, as a hood, holding them sends
      // QuadriFlow's count far off, and QuadriFlow keeps to open edges roughly on its own)
      for (int i = 0; i < 3 * F0.cols() && pinOpen; ++i) {
        if (H.mE2E[i] != -1) continue;
        int i0 = F0(i % 3, i / 3), i1 = F0((i + 1) % 3, i / 3);
        Eigen::Vector3d e = V0.col(i1) - V0.col(i0);
        if (e.squaredNorm() > 0) setConstraint(i0, e.normalized()), setConstraint(i1, e.normalized());
      }
      std::vector<std::vector<int>> vadj(V0.cols());
      std::unordered_map<uint64_t, int> dedge;
      for (int f = 0; f < F0.cols(); ++f)
        for (int k = 0; k < 3; ++k) {
          int a = F0(k, f), b = F0((k + 1) % 3, f);
          dedge[(uint64_t(a) << 32) | uint32_t(b)] = 3 * f + k;
          vadj[a].push_back(b);
        }
      // the laid lines: sharp edges (QuadriFlow may have split them: the halves are followed along)
      int held = 0;
      std::vector<char> mark(V0.cols(), 0);
      // (each held along the drawn line's own way there: the laid path zigzags along the working
      // triangles, up to 30 degrees off it, and a lattice line held along a zigzag misses the line)
      const double nsc = field.normalize_scale;
      const Eigen::Vector3d noff = field.normalize_offset;
      std::unordered_map<uint64_t, std::vector<std::pair<Eigen::Vector3d, Eigen::Vector3d>>> drawnGrid;
      const double drawnCell = Lg / nsc;
      auto dkey = [&](const Eigen::Vector3d& p, int dx, int dy, int dz) {
        int64_t x = (int64_t)std::floor(p.x() / drawnCell) + dx, y = (int64_t)std::floor(p.y() / drawnCell) + dy,
                z = (int64_t)std::floor(p.z() / drawnCell) + dz;
        return (uint64_t(x & 0x1FFFFF) << 42) | (uint64_t(y & 0x1FFFFF) << 21) | uint64_t(z & 0x1FFFFF);
      };
      for (size_t li = 0; li < lines.size(); ++li) {
        if (!lineIsEdge[li]) continue;
        for (size_t i = 0; i + 1 < lines[li].size(); ++i) {
          Eigen::Vector3d a = (Eigen::Vector3d(lines[li][i].x, lines[li][i].y, lines[li][i].z) - noff) / nsc;
          Eigen::Vector3d b = (Eigen::Vector3d(lines[li][i + 1].x, lines[li][i + 1].y, lines[li][i + 1].z) - noff) / nsc;
          if ((b - a).squaredNorm() <= 0) continue;
          int n = std::max(1, (int)std::ceil((b - a).norm() / drawnCell));
          for (int k = 0; k <= n; ++k) drawnGrid[dkey(a + (b - a) * (double(k) / n), 0, 0, 0)].push_back({a, b});
        }
      }
      // (the nearest drawn segment running about the laid edge's way: where lines are close, as rings round
      // the fingers in a hand's margin, another line's segment can be nearer, and held across it the quads
      // there collapsed; the laid vertices are on their own line, so it is near)
      auto drawnDir = [&](const Eigen::Vector3d& p, Eigen::Vector3d fallback) {
        double bd = 0.04 * drawnCell * drawnCell;
        Eigen::Vector3d best = fallback;
        for (int dz = -1; dz <= 1; ++dz)
          for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
              auto it = drawnGrid.find(dkey(p, dx, dy, dz));
              if (it == drawnGrid.end()) continue;
              for (auto& [a, b] : it->second) {
                Eigen::Vector3d e = b - a;
                if (std::abs(e.normalized().dot(fallback)) < 0.64) continue;
                double t = std::clamp((p - a).dot(e) / e.squaredNorm(), 0.0, 1.0);
                if (double d2 = (a + e * t - p).squaredNorm(); d2 < bd) bd = d2, best = e.dot(fallback) < 0 ? -e : e;
              }
            }
        return best.normalized();
      };
      field.patina_line_edges.assign(field.sharp_edges.size(), 0);
      auto markSharp = [&](int a, int b) {
        for (auto [x, y] : {std::pair{a, b}, std::pair{b, a}}) {
          auto it = dedge.find((uint64_t(x) << 32) | uint32_t(y));
          if (it != dedge.end()) field.sharp_edges[it->second] = 1, field.patina_line_edges[it->second] = 1;
        }
        Eigen::Vector3d e = V0.col(b) - V0.col(a);
        if (e.squaredNorm() <= 0) return;
        for (int v : {a, b}) {
          if (!mark[v]) mark[v] = 1, ++held;
          if (H.mCOw[0][v] != 0) continue;
          Eigen::Vector3d d = drawnGrid.empty() ? Eigen::Vector3d(e.normalized()) : drawnDir(V0.col(v), e.normalized());
          // (in the surface's plane there)
          Eigen::Vector3d nn = H.mN[0].col(v), dt = d - nn * nn.dot(d);
          setConstraint(v, dt.squaredNorm() > 1e-12 ? Eigen::Vector3d(dt.normalized()) : Eigen::Vector3d(e.normalized()));
        }
      };
      for (EdgeKey ek : sharp) {
        int a = int(ek >> 32), b = int(ek & 0xFFFFFFFF);
        Eigen::Vector3d A = V0.col(a), e = V0.col(b) - A;
        double len2 = e.squaredNorm(), tc = 0;
        int cur = a;
        for (int step = 0; step < 64 && len2 > 0; ++step) {
          int nx = -1;
          double tn = 2;
          for (int w : vadj[cur]) {
            if (w == b) {
              nx = b, tn = 1;
              break;
            }
            if (w < (int)n0) continue;
            double t = (V0.col(w) - A).dot(e) / len2;
            if (t > tc + 1e-9 && t < 1 && (A + e * t - V0.col(w)).squaredNorm() < 1e-10 * len2 && t < tn) nx = w, tn = t;
          }
          if (nx < 0) break;
          markSharp(cur, nx);
          if (nx == b) break;
          cur = nx, tc = tn;
        }
      }
      // the direction strokes: the field turned along them, on a band about a quad wide; and (more
      // softly) along the edge lines on a band either side of them, so the flow between lines follows them
      static const double edgeBand = getenv("PATINA_RETOPO_BAND") ? atof(getenv("PATINA_RETOPO_BAND")) : 0.0;
      bool anyDir = edgeBand > 0;
      for (char e : lineIsEdge) anyDir |= !e;
      if (anyDir) {
        double sc = field.normalize_scale;
        Eigen::Vector3d off = field.normalize_offset;
        double cell = std::max(H.mScale, 1e-9);
        std::unordered_map<uint64_t, std::vector<int>> grid;
        auto q = [&](double x) { return (int64_t)std::floor(x / cell); };
        auto key3 = [&](int64_t x, int64_t y, int64_t z) {
          return (uint64_t(x & 0x1FFFFF) << 42) | (uint64_t(y & 0x1FFFFF) << 21) | uint64_t(z & 0x1FFFFF);
        };
        for (int i = 0; i < V0.cols(); ++i) grid[key3(q(V0(0, i)), q(V0(1, i)), q(V0(2, i)))].push_back(i);
        for (size_t li = 0; li < lines.size(); ++li) {
          const bool edgeLine = lineIsEdge[li];
          if (edgeLine && edgeBand <= 0) continue;
          const double reach = edgeLine ? edgeBand : 0.5;  // (in quads)
          const int rc = (int)std::ceil(reach);
          std::vector<Eigen::Vector3d> pl;
          for (auto& p3 : lines[li]) pl.push_back((Eigen::Vector3d(p3.x, p3.y, p3.z) - off) / sc);
          for (size_t i = 0; i + 1 < pl.size(); ++i) {
            Eigen::Vector3d a = pl[i], e = pl[i + 1] - pl[i];
            double len = e.norm();
            if (len <= 0) continue;
            Eigen::Vector3d dir = e / len;
            // the working vertices within half a quad of this piece of the stroke
            int steps = std::max(1, (int)std::ceil(len / cell));
            for (int sI = 0; sI <= steps; ++sI) {
              Eigen::Vector3d p = a + e * (double(sI) / steps);
              int64_t cx = q(p.x()), cy = q(p.y()), cz = q(p.z());
              for (int64_t dx = -rc; dx <= rc; ++dx)
                for (int64_t dy = -rc; dy <= rc; ++dy)
                  for (int64_t dz = -rc; dz <= rc; ++dz) {
                    auto it = grid.find(key3(cx + dx, cy + dy, cz + dz));
                    if (it == grid.end()) continue;
                    for (int v : it->second) {
                      if (H.mCOw[0][v] != 0) continue;  // (an edge line or open edge holds it already)
                      double d2 = (V0.col(v) - p).squaredNorm();
                      if (d2 > reach * reach * cell * cell) continue;
                      double w = edgeLine ? 0.8 * (1.0 - 0.6 * std::sqrt(d2) / (reach * cell)) : d2 < 0.0625 * cell * cell ? 1.0 : 0.7;
                      if (w < H.mCQw[0][v]) continue;
                      Eigen::Vector3d nn = H.mN[0].col(v), d = dir - nn * nn.dot(dir);
                      if (d.squaredNorm() <= 1e-12) continue;
                      H.mCQ[0].col(v) = d.normalized(), H.mCQw[0][v] = w;
                      if (!mark[v]) mark[v] = 1, ++held;
                    }
                  }
            }
          }
        }
      }
      H.propagateConstraints();
      qflow::Optimizer::optimize_orientations(H);
      const double tOrient = tq();
      if (getenv("PATINA_RETOPO_FIELDDBG")) {
        // how far the field is turned from the nearest edge line (mod 90 degrees), within 3 quads of them
        double sc = field.normalize_scale, cell = std::max(H.mScale, 1e-9);
        Eigen::Vector3d off = field.normalize_offset;
        std::vector<std::pair<Eigen::Vector3d, Eigen::Vector3d>> smp;
        for (size_t li = 0; li < lines.size(); ++li) {
          if (!lineIsEdge[li]) continue;
          for (size_t i = 0; i + 1 < lines[li].size(); ++i) {
            Eigen::Vector3d a = (Eigen::Vector3d(lines[li][i].x, lines[li][i].y, lines[li][i].z) - off) / sc;
            Eigen::Vector3d b = (Eigen::Vector3d(lines[li][i + 1].x, lines[li][i + 1].y, lines[li][i + 1].z) - off) / sc;
            int n = std::max(1, (int)std::ceil((b - a).norm() / (0.25 * cell)));
            if ((b - a).norm() <= 0) continue;
            for (int k = 0; k < n; ++k) smp.push_back({a + (b - a) * (double(k) / n), (b - a).normalized()});
          }
        }
        std::unordered_map<uint64_t, std::vector<int>> g;
        auto q = [&](double x) { return (int64_t)std::floor(x / (3 * cell)); };
        auto k3 = [&](int64_t x, int64_t y, int64_t z) { return (uint64_t(x & 0x1FFFFF) << 42) | (uint64_t(y & 0x1FFFFF) << 21) | uint64_t(z & 0x1FFFFF); };
        for (int i = 0; i < (int)smp.size(); ++i) g[k3(q(smp[i].first.x()), q(smp[i].first.y()), q(smp[i].first.z()))].push_back(i);
        double sum = 0, binSum[3] = {0, 0, 0};
        int cnt = 0, within = 0, binN[3] = {0, 0, 0};
        for (int v = 0; v < V0.cols(); ++v) {
          Eigen::Vector3d p = V0.col(v);
          double bd = 1e30;
          int bi = -1;
          for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy)
              for (int dz = -1; dz <= 1; ++dz) {
                auto it = g.find(k3(q(p.x()) + dx, q(p.y()) + dy, q(p.z()) + dz));
                if (it == g.end()) continue;
                for (int i : it->second)
                  if (double d = (smp[i].first - p).squaredNorm(); d < bd) bd = d, bi = i;
              }
          if (bi < 0 || bd > 9 * cell * cell) continue;
          Eigen::Vector3d qd = H.mQ[0].col(v), t = smp[bi].second, n = H.mN[0].col(v);
          t -= n * n.dot(t);
          if (t.norm() < 1e-9) continue;
          double c = std::abs(qd.normalized().dot(t.normalized()));
          double ang = std::acos(std::min(1.0, c)) * 180 / M_PI;
          ang = std::min(ang, 90 - ang);
          sum += ang, ++cnt, within += ang < 10;
          int bin = bd < 0.25 * cell * cell ? 0 : bd < 2.25 * cell * cell ? 1 : 2;
          binSum[bin] += ang, binN[bin]++;
        }
        PLOG_I("Retopology (field): by distance <0.5/0.5-1.5/1.5-3 quads: %.1f %.1f %.1f deg (%d %d %d vertices)", binN[0] ? binSum[0] / binN[0] : 0.0,
               binN[1] ? binSum[1] / binN[1] : 0.0, binN[2] ? binSum[2] / binN[2] : 0.0, binN[0], binN[1], binN[2]);
        PLOG_I("Retopology (field): %d vertices within 3 quads of the lines, mean %.1f deg off, %.0f%% within 10 (cell %.4f world, quad %.4f)", cnt,
               cnt ? sum / cnt : 0.0, cnt ? 100.0 * within / cnt : 0.0, cell * sc, Lg);
      }
      field.ComputeOrientationSingularities();
      qflow::Optimizer::optimize_scale(H, field.rho, field.flag_adaptive_scale);
      field.flag_adaptive_scale = 1;
      // the painted density: the quads' size at each working vertex (relative to the mean the count
      // already allows for), smoothed over about a quad so the size changes gently; QuadriFlow follows
      // it only partly, so its contrast is doubled
      if (groupPainted) {
        Eigen::MatrixXd& S0 = H.mS[0];
        std::vector<double> sz(V0.cols(), 1.0);
        for (int i = 0; i < V0.cols(); ++i) {
          double d = i < (int)n0 ? vDens[i] : meanD;
          if (i >= (int)n0)  // a vertex QuadriFlow added: its neighbors' (original) density
            for (int w : vadj[i])
              if (w < (int)n0) {
                d = vDens[w];
                break;
              }
          sz[i] = meanD / std::clamp(d, 0.05, 50.0);
        }
        double edgeLen = 0;
        for (int f = 0; f < std::min<int>(F0.cols(), 2000); ++f)
          edgeLen += (V0.col(F0(1, f)) - V0.col(F0(0, f))).norm() / std::min<int>(F0.cols(), 2000);
        int passes = std::clamp((int)std::lround(H.mScale / std::max(edgeLen, 1e-12)), 1, 30);
        for (int it = 0; it < passes; ++it) {
          std::vector<double> next = sz;
          for (int i = 0; i < V0.cols(); ++i) {
            if (vadj[i].empty()) continue;
            double s2 = 0;
            for (int j : vadj[i]) s2 += sz[j];
            next[i] = 0.5 * sz[i] + 0.5 * s2 / vadj[i].size();
          }
          sz = std::move(next);
        }
        double sa = 0, sw = 0;
        for (int i = 0; i < V0.cols(); ++i) sa += H.mA[0][i] / (sz[i] * sz[i]), sw += H.mA[0][i];
        for (auto& x : sz) x *= std::sqrt(sa / std::max(sw, 1e-30));  // (area-weighted mean of 1/size^2 = 1)
        for (int i = 0; i < V0.cols(); ++i) S0(0, i) = S0(1, i) = sz[i];
        for (size_t l = 0; l + 1 < H.mS.size(); ++l) {
          const Eigen::MatrixXi& up = H.mToUpper[l];
          for (int i = 0; i < H.mS[l + 1].cols(); ++i) {
            double sum = 0;
            int n = 0;
            for (int k = 0; k < 2; ++k)
              if (up(k, i) >= 0) sum += H.mS[l](0, up(k, i)), ++n;
            if (n) H.mS[l + 1](0, i) = H.mS[l + 1](1, i) = sum / n;
          }
        }
      }
      const double tScale = tq();
      qflow::Optimizer::optimize_positions(H, field.flag_adaptive_scale);
      field.ComputePositionSingularities();
      const double tPos = tq();
      if (!field.ComputeIndexMap()) return out;
      PLOG_D("Retopology (QuadriFlow): %zu triangles: initialize %.2f s, orientations %.2f, scale %.2f, positions %.2f, index map %.2f "
             "(constraints %.2f, max flow %.2f, subdivide %.2f, flips %.2f, extract %.2f)",
             m.tri.size(), tInit, tOrient - tInit, tScale - tOrient, tPos - tScale, tq() - tPos, field.patina_times[0], field.patina_times[1],
             field.patina_times[2], field.patina_times[3], field.patina_times[4]);
      for (auto& o : field.O_compact) {
        Eigen::Vector3d p = o * field.normalize_scale + field.normalize_offset;
        out.pos.push_back(vec3((float)p.x(), (float)p.y(), (float)p.z()));
      }
      for (auto& qd : field.F_compact) {
        bool ok = true;
        for (int k = 0; k < 4; ++k) ok &= qd[k] >= 0 && qd[k] < (int)out.pos.size();
        if (ok) out.quads.push_back({(uint32_t)qd[0], (uint32_t)qd[1], (uint32_t)qd[2], (uint32_t)qd[3]});
      }
      // how far its edges run from its own field: rows that lean off it (a limb's rows turned into a helix,
      // with no pole to show for it) leave the shape's way; the share of edges more than 15 degrees off
      {
        const double cell = std::max(H.mScale, 1e-9);
        auto key = [&](const Eigen::Vector3d& p, int dx, int dy, int dz) {
          int64_t x = (int64_t)std::floor(p.x() / cell) + dx, y = (int64_t)std::floor(p.y() / cell) + dy, z = (int64_t)std::floor(p.z() / cell) + dz;
          return (uint64_t(x & 0x1FFFFF) << 42) | (uint64_t(y & 0x1FFFFF) << 21) | uint64_t(z & 0x1FFFFF);
        };
        std::unordered_map<uint64_t, std::vector<int>> grid;
        for (int i = 0; i < V0.cols(); ++i) grid[key(V0.col(i), 0, 0, 0)].push_back(i);
        std::set<std::pair<int, int>> seen;
        size_t edges = 0, offEdges = 0;
        const auto& O = field.O_compact;
        for (auto& qd : field.F_compact)
          for (int k = 0; k < 4; ++k) {
            int a = qd[k], b = qd[(k + 1) % 4];
            if (a < 0 || b < 0 || a >= (int)O.size() || b >= (int)O.size() || !seen.insert({std::min(a, b), std::max(a, b)}).second) continue;
            Eigen::Vector3d mid = 0.5 * (O[a] + O[b]), e = O[b] - O[a];
            int bi = -1;
            double bd = 1e30;
            for (int dz = -1; dz <= 1; ++dz)
              for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                  auto it = grid.find(key(mid, dx, dy, dz));
                  if (it == grid.end()) continue;
                  for (int i : it->second)
                    if (double d = (V0.col(i) - mid).squaredNorm(); d < bd) bd = d, bi = i;
                }
            if (bi < 0) continue;
            Eigen::Vector3d nn = H.mN[0].col(bi), q = H.mQ[0].col(bi), et = e - nn * nn.dot(e);
            if (et.squaredNorm() <= 1e-18 || q.squaredNorm() <= 1e-18) continue;
            et.normalize(), q.normalize();
            double c1 = std::abs(et.dot(q)), c2 = std::abs(et.dot(nn.cross(q)));
            ++edges, offEdges += std::max(c1, c2) < 0.966;  // (cos 15 degrees)
          }
        out.fieldOff = edges ? double(offEdges) / edges : 0;
      }
      out.fieldSingular = field.singularities.size(), out.held = held, out.smoothing = smoothing, out.seed = seed;
      out.ok = !out.quads.empty();
      if (!out.ok) PLOG_D("Retopology (QuadriFlow): try gave no quads (%zu vertices, %zu faces extracted)", field.O_compact.size(), field.F_compact.size());
      return out;
    };
    struct Try {
      int seed, smoothing;
      bool pinOpen;
    };
    std::vector<Try> tries;
    int variants = std::max(1, std::min(opt.variants, (int)std::max(1u, std::thread::hardware_concurrency() / 2)));
    const int smooths[3] = {smooth0, smooth0 / 2, std::min(60, smooth0 * 2)};
    for (int k = 0; (int)tries.size() < variants; ++k) tries.push_back({opt.seed + k / 3, smooths[k % 3], true});
    Variant kept;
    double keptScore = 1e30, g = 1;
    bool pinOpenNow = true;  // (the open edges held: see run)
    int farOffPinned = 0;
    bool hasOpen = false;
    {
      std::unordered_map<EdgeKey, int> eu;
      for (auto& t : m.tri)
        for (int k = 0; k < 3; ++k) eu[ekey(t[k], t[(k + 1) % 3])]++;
      for (auto& [e, n] : eu) hasOpen |= n == 1;
    }
    for (int pass = 0; pass < std::max(1, opt.passes); ++pass) {
      if (cancelled()) break;
      std::vector<Variant> got(tries.size());
      {
        // each variant has a stop of its own: set when the build is canceled, or when it runs far longer
        // than the first to finish (QuadriFlow can get stuck on a mesh it doesn't expect)
        const size_t nv = tries.size();
        std::unique_ptr<std::atomic<bool>[]> stop(new std::atomic<bool>[nv]), done(new std::atomic<bool>[nv]);
        for (size_t i = 0; i < nv; ++i) stop[i] = false, done[i] = false;
        std::vector<std::thread> threads;
        for (size_t i = 0; i < nv; ++i)
          threads.emplace_back([&, i] {
            qflow::patina_cancel = &stop[i];
            qflow::patina_loop_count = 0;
            try {
              got[i] = run(tries[i].seed, tries[i].smoothing, g, tries[i].pinOpen && pinOpenNow);
            } catch (const std::exception& ex) {
              PLOG_D("Retopology (QuadriFlow): try %zu stopped: %s", i, ex.what());
              got[i].ok = false;
            } catch (...) {
              got[i].ok = false;
            }
            done[i] = true;
          });
        auto start = std::chrono::steady_clock::now();
        double firstDone = -1;
        for (;;) {
          bool all = true;
          for (size_t i = 0; i < nv; ++i) all &= done[i].load();
          if (all) break;
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
          double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
          for (size_t i = 0; i < nv && firstDone < 0; ++i)
            if (done[i]) firstDone = el;
          bool stopAll = cancelled() || el > 600;
          for (size_t i = 0; i < nv; ++i)
            if (!done[i] && (stopAll || (firstDone >= 0 && el > std::max(20.0, 5 * firstDone)))) stop[i] = true;
        }
        for (auto& t : threads) t.join();
      }
      int bestV = -1;
      for (size_t i = 0; i < got.size(); ++i) {
        Variant& v = got[i];
        if (!v.ok) continue;
        std::vector<int> deg(v.pos.size(), 0);
        std::map<std::pair<uint32_t, uint32_t>, int> eu;
        for (auto& qd : v.quads)
          for (int k = 0; k < 4; ++k) eu[{std::min(qd[k], qd[(k + 1) % 4]), std::max(qd[k], qd[(k + 1) % 4])}]++;
        std::vector<char> openV(v.pos.size(), 0);
        for (auto& [e, n] : eu) {
          deg[e.first]++, deg[e.second]++;
          if (n == 1) openV[e.first] = openV[e.second] = 1;
        }
        int inner = 0;
        for (size_t k = 0; k < v.pos.size(); ++k)
          if (!openV[k] && deg[k]) ++inner, v.poles += deg[k] != 4;
        double count = std::abs(double(v.quads.size()) / std::max(1, targetFaces) - 1);
        // QuadriFlow now and then fails on a part (a thin piece collapses): the share of the working
        // surface further than a quad and a half from every quad vertex
        double uncovered = 0;
        {
          const float cell = 1.5f * Lg;
          auto key = [](glm::ivec3 c) { return (uint64_t(c.x & 0x1FFFFF) << 42) | (uint64_t(c.y & 0x1FFFFF) << 21) | uint64_t(c.z & 0x1FFFFF); };
          std::unordered_set<uint64_t> near;
          for (auto& q : v.quads)
            for (uint32_t x : q) near.insert(key(glm::ivec3(glm::floor(v.pos[x] / cell))));
          size_t step = std::max<size_t>(1, m.pos.size() / 20000), n = 0, far = 0;
          for (size_t k = 0; k < m.pos.size(); k += step, ++n) {
            glm::ivec3 c = glm::ivec3(glm::floor(m.pos[k] / cell));
            bool found = false;
            for (int dz = -1; dz <= 1 && !found; ++dz)
              for (int dy = -1; dy <= 1 && !found; ++dy)
                for (int dx = -1; dx <= 1 && !found; ++dx) found = near.count(key(c + glm::ivec3(dx, dy, dz))) > 0;
            far += !found;
          }
          uncovered = n ? double(far) / n : 0;
        }
        // and quads that don't lie on it (spanning a gap, or a piece folded flat)
        size_t off = 0;
        for (auto& q : v.quads) {
          vec3 c = 0.25f * (v.pos[q[0]] + v.pos[q[1]] + v.pos[q[2]] + v.pos[q[3]]);
          RayHit h = workBvh.closestPoint(work, c, 0.35f * Lg);
          off += !h.hit;
        }
        double offShare = v.quads.empty() ? 0 : double(off) / v.quads.size();
        v.missed = uncovered + offShare;
        // and the lines it leaves: the length (in quads) of the laid lines no edge runs along; each such
        // quad's length is laid zigzag afterward, or joined with steps, and costs about a pole
        double lineOff = 0;
        if (!sharp.empty()) {
          const float cell = Lg;
          auto key = [](glm::ivec3 c) { return (uint64_t(c.x & 0x1FFFFF) << 42) | (uint64_t(c.y & 0x1FFFFF) << 21) | uint64_t(c.z & 0x1FFFFF); };
          std::unordered_map<uint64_t, std::vector<std::pair<uint32_t, uint32_t>>> eg;
          for (auto& [e, n] : eu) {
            vec3 mid = 0.5f * (v.pos[e.first] + v.pos[e.second]);
            eg[key(glm::ivec3(glm::floor(mid / cell)))].push_back(e);
          }
          for (EdgeKey ek : sharp) {
            vec3 a = m.pos[uint32_t(ek >> 32)], b = m.pos[uint32_t(ek & 0xFFFFFFFF)], p = 0.5f * (a + b), t = b - a;
            float len = glm::length(t);
            if (len <= 0) continue;
            t /= len;
            glm::ivec3 c = glm::ivec3(glm::floor(p / cell));
            bool along = false;
            for (int dz = -1; dz <= 1 && !along; ++dz)
              for (int dy = -1; dy <= 1 && !along; ++dy)
                for (int dx = -1; dx <= 1 && !along; ++dx) {
                  auto it = eg.find(key(c + glm::ivec3(dx, dy, dz)));
                  if (it == eg.end()) continue;
                  for (auto [x, y] : it->second) {
                    vec3 qa = v.pos[x], qe = v.pos[y] - qa;
                    float l2 = glm::dot(qe, qe);
                    if (l2 <= 0) continue;
                    float u = std::clamp(glm::dot(p - qa, qe) / l2, 0.f, 1.f);
                    if (glm::length(qa + u * qe - p) < 0.2f * Lg && std::abs(glm::dot(qe, t)) > 0.9f * std::sqrt(l2)) {
                      along = true;
                      break;
                    }
                  }
                }
            if (!along) lineOff += len / Lg;
          }
        }
        v.lineOff = lineOff;
        // (and its rows leaning off its own field, as a limb's helix: half the share of such edges)
        v.score = (double(v.poles) + lineOff) / std::max(1, inner) + 0.25 * count + 4 * uncovered + 4 * offShare + 0.5 * v.fieldOff;
        if (bestV < 0 || v.score < got[bestV].score) bestV = (int)i;
      }
      if (bestV < 0) {
        // (no try gave quads: on a few shapes, as a stretched ball, QuadriFlow gives far fewer than asked
        // for, down to none; asked for more, it gives some)
        if (kept.ok || pass + 1 >= std::max(1, opt.passes) || g >= 8.0) break;
        g = std::min(8.0, g * 3);
        PLOG_I("Retopology (flow): pass %d gave no quads; asked for %.0f times as many", pass + 1, g);
        continue;
      }
      double count = double(got[bestV].quads.size()) / std::max(1, targetFaces);
      // (the count is set right by all the tries' mean: one try's count varies a lot)
      double sum = 0;
      int n = 0;
      for (auto& v : got)
        if (v.ok) sum += double(v.quads.size()) / std::max(1, targetFaces), ++n;
      double mean = n ? sum / n : count;
      PLOG_I("Retopology (flow): pass %d: %zu quads (%.0f%% of the %d asked for; mean %.0f%%), %d poles, %.0f quads of lines left (score %.3f)",
             pass + 1, got[bestV].quads.size(), 100 * count, targetFaces, 100 * mean, got[bestV].poles, got[bestV].lineOff, got[bestV].score);
      if (getenv("PATINA_RETOPO_DEBUG"))
        for (auto& v : got)
          if (v.ok) PLOG_I("  try seed %d smoothing %d: %zu quads, %d poles, %.0f quads of lines left, %.1f%% of edges off the field, score %.3f", v.seed, v.smoothing, v.quads.size(), v.poles, v.lineOff, 100 * v.fieldOff, v.score);
      if (const char* td = getenv("PATINA_RETOPO_TRYDUMP"))
        for (size_t i = 0; i < got.size(); ++i)
          if (got[i].ok)
            if (FILE* fo = fopen((std::string(td) + "_" + std::to_string(m.tri.size()) + "_p" + std::to_string(pass) + "_t" + std::to_string(i) + ".obj").c_str(), "w")) {
              for (auto& p : got[i].pos) fprintf(fo, "v %f %f %f\n", p.x, p.y, p.z);
              for (auto& q : got[i].quads) fprintf(fo, "f %u %u %u %u\n", q[0] + 1, q[1] + 1, q[2] + 1, q[3] + 1);
              fclose(fo);
            }
      if (got[bestV].score < keptScore) keptScore = got[bestV].score, kept = std::move(got[bestV]);
      if (std::abs(count - 1) < 0.15) break;
      // (a count very far off with the open edges held, twice, the second time asked for more or fewer
      // to make up, as on a hood, or the best try missing much of the surface, as a torn hem: tried again
      // with them free, as asked; once far off is often only QuadriFlow's usual shortfall)
      const bool farOff = mean < 0.35 || mean > 3.0;
      farOffPinned += farOff && pinOpenNow;
      if (pinOpenNow && hasOpen && ((farOff && farOffPinned >= 2) || kept.missed > 0.05)) {
        pinOpenNow = false, g = 1;
        PLOG_I("Retopology (flow): the count came out far off; tried again with the open edges free");
        continue;
      }  // (within 15% of the count asked for)
      g = std::clamp(g / std::max(mean, 0.05), 0.3, 8.0);
    }
    if (kept.ok && getenv("PATINA_RETOPO_DEBUG")) {
      int h[6] = {0};
      for (auto& p : kept.pos) {
        float bd = 1e30f;
        for (EdgeKey ek : sharp) bd = std::min(bd, segDist2(p, m.pos[uint32_t(ek >> 32)], m.pos[uint32_t(ek & 0xFFFFFFFF)]));
        float d = std::sqrt(bd) / Lg;
        h[d < 0.05f ? 0 : d < 0.1f ? 1 : d < 0.2f ? 2 : d < 0.3f ? 3 : d < 0.4f ? 4 : 5]++;
      }
      PLOG_I("Retopology (flow): %zu sharp edges; vertices within 0.05/0.1/0.2/0.3/0.4 quads of them: %d %d %d %d %d", sharp.size(), h[0], h[1],
             h[2], h[3], h[4]);
    }
    auto dumpStage = [&](const char* tag) {
      const char* raw = getenv("PATINA_RETOPO_RAW");
      if (!raw || !kept.ok) return;
      // (named by the group's triangle count: groups are made at once)
      if (FILE* fo = fopen((std::string(raw) + "_" + std::to_string(m.tri.size()) + "_" + tag + ".obj").c_str(), "w")) {
        for (auto& p : kept.pos) fprintf(fo, "v %f %f %f\n", p.x, p.y, p.z);
        for (auto& q : kept.quads) fprintf(fo, "f %u %u %u %u\n", q[0] + 1, q[1] + 1, q[2] + 1, q[3] + 1);
        fclose(fo);
      }
    };
    dumpStage("raw");
    if (kept.ok) {
      int collapsed = collapsePolePairs(kept.pos, kept.quads);
      dumpStage("collapsed");
      // QuadriFlow now and then leaves a hole of three or four edges: a face of its own there (one larger than a
      // quad only where the surface under it is closed: none of its corners by an open edge of the surface)
      std::vector<std::pair<vec3, vec3>> openEdges;
      bool openFound = false;
      for (auto& h : smallHoles(kept.quads)) {
        float per = 0;
        for (size_t i = 0; i < h.size(); ++i) per += glm::length(kept.pos[h[(i + 1) % h.size()]] - kept.pos[h[i]]);
        if (per > 8 * Lg) continue;
        if (per > 4 * Lg) {
          if (!openFound) {
            openFound = true;
            std::unordered_map<EdgeKey, int> eu;
            for (auto& t : m.tri)
              for (int k = 0; k < 3; ++k) eu[ekey(t[k], t[(k + 1) % 3])]++;
            for (auto& [e, n] : eu)
              if (n == 1) openEdges.push_back({m.pos[uint32_t(e >> 32)], m.pos[uint32_t(e & 0xFFFFFFFF)]});
          }
          bool byOpen = false;
          for (uint32_t x : h)
            for (auto& [a, b] : openEdges) byOpen |= segDist2(kept.pos[x], a, b) < 0.25f * Lg * Lg;
          if (byOpen) continue;
        }
        if (h.size() == 3) kept.tris.push_back({h[2], h[1], h[0]});
        else kept.quads.push_back({h[3], h[2], h[1], h[0]});
      }
      // the edge lines: exact loops along them
      int laidV = layLinesInQuads(kept.pos, kept.quads, lines, lineIsEdge, Lg);
      dumpStage("laid");
      if (laidV) PLOG_I("Retopology (flow): %d vertices put on the edge lines", laidV);
      PLOG_I("Retopology (flow): kept seed %d smoothing %d (%d poles, %zu field singularities), %d pole pairs collapsed", kept.seed,
             kept.smoothing, kept.poles, kept.fieldSingular, collapsed);
      heldTotal += kept.held;
    }
    return kept;
  };

  // ---- each group's quads
  std::vector<vec3> P;
  std::vector<std::array<uint32_t, 4>> Qd;
  std::vector<std::array<uint32_t, 3>> Tr;
  std::vector<int> quadGroup, triGroup;
  bool failed = false;
  std::vector<char> bandV;  // the joining rows' vertices (smoothed more)
  JoinStats joinStats;
  // when the zones won't join with rings: those rings that meet other zones only along part of a line
  // (where three zones meet) are made as other zones and the rest kept; if that isn't it, no rings at all
  auto retryRings = [&](const char* why) -> Result {
    std::vector<char> meets(groups, 0);
    for (auto& ch : chains)
      if (!ch.closed)
        for (int g : {ch.g, ch.h}) meets[g] |= groupRing[g] != 0;
    int ringsNow = 0, meeting = 0;
    for (int g = 0; g < groups; ++g) ringsNow += groupRing[g] != 0, meeting += meets[g];
    FlowOptions o2 = opt;
    ++o2.ringRetries;
    if (meeting > 0 && meeting < ringsNow && opt.ringRetries < 2) {
      for (int g = 0; g < groups; ++g)
        if (meets[g])
          for (uint32_t t = 0; t < T; ++t)
            if (group[t] == g && triInRing[t]) {
              o2.noRingsAt.push_back((cm.pos[cm.tri[t].x] + cm.pos[cm.tri[t].y] + cm.pos[cm.tri[t].z]) / 3.f);
              break;
            }
      PLOG_I("Retopology (flow): %s; made again with %d of the %d rings as other zones", why, meeting, ringsNow);
      Result r = flowQuads(high, bvh, strokes, faceLabel, vertexDensity, o2, faceResolution);
      r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      return r;
    }
    PLOG_I("Retopology (flow): %s; made again without rings", why);
    o2.ringZones = false;
    Result r = flowQuads(high, bvh, strokes, faceLabel, vertexDensity, o2, faceResolution);
    r.problems.push_back("the rings between lines couldn't be joined to the zones round them (lines crossing, or very close), so "
                         "those zones are made as the others are");
    r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return r;
  };
  const bool anyRings = !opt.layout && std::any_of(groupRing.begin(), groupRing.end(), [](char k) { return k != 0; });
  if (opt.layout) {
    // (0.21, in progress) the quads from a quad layout of the lines: no zones made apart, no joins
    QuadLayout ql = buildQuadLayout(layoutInput());
    if (const char* lay = getenv("PATINA_LAYOUT"); lay && *lay == '/') dumpQuadLayout(ql, lay);
    LayoutQuads lq = quadsFromLayout(ql);
    if (const char* lay = getenv("PATINA_LAYOUT"); lay && *lay == '/')
      if (FILE* fo = fopen((std::string(lay) + "_raw.obj").c_str(), "w")) {
        for (auto& p : lq.pos) fprintf(fo, "v %f %f %f\n", p.x, p.y, p.z);
        for (auto& q : lq.quads) fprintf(fo, "f %u %u %u %u\n", q[0] + 1, q[1] + 1, q[2] + 1, q[3] + 1);
        for (auto& t : lq.tris) fprintf(fo, "f %u %u %u\n", t[0] + 1, t[1] + 1, t[2] + 1);
        fclose(fo);
        if (FILE* fk = fopen((std::string(lay) + "_raw_kinds.txt").c_str(), "w")) {
          for (size_t q = 0; q < lq.quads.size(); ++q) fprintf(fk, "%d\n", q < lq.quadKind.size() ? lq.quadKind[q] : 5);
          fclose(fk);
        }
      }
    P = std::move(lq.pos), Qd = std::move(lq.quads), Tr = std::move(lq.tris);
    quadGroup.assign(Qd.size(), -1), triGroup.assign(Tr.size(), -1);
    if (Qd.empty()) {
      // (no layout: the zones made as before)
      PLOG_I("Retopology (layout): no quads from the layout; made with the flow instead");
      FlowOptions o2 = opt;
      o2.layout = false;
      Result r = flowQuads(high, bvh, strokes, faceLabel, vertexDensity, o2, faceResolution);
      r.problems.push_back("the quad layout couldn't be made here, so the quads follow the flow instead");
      return r;
    }
    if (lq.patchesLeft)
      res.problems.push_back(std::to_string(lq.patchesLeft) + " patches of the layout were left open (rings and patches with more than one edge "
                             "aren't filled yet)");
  } else {
    // each group is made on its own triangles and a margin of about two of its quads beyond its lines,
    // so the lines are edge loops inside the mesh QuadriFlow sees (which it follows exactly; an open edge
    // it follows only roughly); the quads on the group's own side are kept
    std::vector<std::vector<std::pair<uint32_t, float>>> cmAdj;
    if (groups > 1) {
      cmAdj.resize(cm.pos.size());
      for (auto& [e, tt] : edgeTris) {
        uint32_t a = uint32_t(e >> 32), b = uint32_t(e & 0xFFFFFFFF);
        float len = glm::length(cm.pos[a] - cm.pos[b]);
        cmAdj[a].push_back({b, len}), cmAdj[b].push_back({a, len});
      }
    }
    // each group on a thread of its own (they don't share anything they change)
    // a ring zone: rings that follow both its loops (fixed: loops a ring beside it made already)
    std::vector<RingPlanLoop> ringPlan;  // (planned below, before the rings are made)
    // looseGroup: a zone made after the rings beside it, taking their loops as they are (a small disc, a ring
    // not made yet)
    auto makeRingOf = [&](int gi, const std::vector<std::vector<vec3>>& fixed, const std::vector<char>& looseGroup) -> Variant {
      std::vector<int64_t> map(cm.pos.size(), -1);
      std::vector<vec3> rp;
      std::vector<glm::uvec3> rt;
      for (uint32_t t = 0; t < T; ++t) {
        if (group[t] != gi) continue;
        glm::uvec3 nt;
        for (int k = 0; k < 3; ++k) {
          uint32_t v = cm.tri[t][k];
          if (map[v] < 0) map[v] = (int64_t)rp.size(), rp.push_back(cm.pos[v]);
          nt[k] = (uint32_t)map[v];
        }
        rt.push_back(nt);
      }
      // the ring's border vertices on the model's open edge or along a loose zone
      std::vector<char> loose(rp.size(), 0), openV(rp.size(), 0);
      for (uint32_t t = 0; t < T; ++t) {
        if (group[t] != gi) continue;
        for (int k = 0; k < 3; ++k) {
          uint32_t a = cm.tri[t][k], b = cm.tri[t][(k + 1) % 3];
          auto it = edgeTris.find(ekey(a, b));
          if (it == edgeTris.end()) continue;
          int64_t o = it->second[0] == (int64_t)t ? it->second[1] : it->second[0];
          if (o < 0 || (group[o] != gi && looseGroup[group[o]])) loose[map[a]] = loose[map[b]] = 1;
          if (o < 0) openV[map[a]] = openV[map[b]] = 1;
        }
      }
      const float Lr = 1.f / (float)std::sqrt(std::max(1e-6, opt.quadsPerM2 * groupRes[gi]));
      const bool cap = groupRing[gi] == 2;
      RingFill rf = cap ? fillCap(rp, rt, Lr, fixed, openV) : fillRings(rp, rt, Lr, fixed, loose, ringPlan);
      Variant v;
      if (!rf.ok) {
        PLOG_I("Retopology (flow): %s at x%.2f made as other zones (%s)", cap ? "disc" : "ring zone", groupRes[gi], rf.why.c_str());
        return v;
      }
      v.pos = std::move(rf.pos), v.quads = std::move(rf.quads), v.tris = std::move(rf.tris), v.ok = true;
      v.ringLoops = std::move(rf.loopVerts), v.ringLoopPos = std::move(rf.loopPos);
      if (cap) PLOG_I("Retopology (flow): disc at x%.2f closed with %zu quads, %zu triangles", groupRes[gi], v.quads.size(), v.tris.size());
      else PLOG_I("Retopology (flow): ring zone at x%.2f: %d rows of rings, %zu quads", groupRes[gi], rf.rings, v.quads.size());
      return v;
    };
    auto makeGroupOf = [&](int gi) -> Variant {
      CleanMesh sub;
      std::vector<int64_t> map(cm.pos.size(), -1);
      std::vector<float> subDens;
      std::vector<char> inSub(T, 0);
      bool any = false;
      for (uint32_t t = 0; t < T; ++t) inSub[t] = group[t] == gi, any |= inSub[t] != 0;
      if (!any) {  // (a group of empty regions only, every other zone a ring or a disc: nothing to make)
        Variant v;
        v.ok = true;
        return v;
      }
      if (groups > 1) {
        // the margin: within about two of this group's quads of its lines (Dijkstra along the edges)
        const float margin = 2.f / (float)std::sqrt(opt.quadsPerM2 * groupRes[gi]) + Lmin;
        std::vector<float> dist(cm.pos.size(), 1e30f);
        using QE = std::pair<float, uint32_t>;
        std::priority_queue<QE, std::vector<QE>, std::greater<>> pq;
        for (auto& [e, tt] : edgeTris) {
          if (tt[1] < 0 || (group[tt[0]] == gi) == (group[tt[1]] == gi)) continue;
          for (uint32_t v : {uint32_t(e >> 32), uint32_t(e & 0xFFFFFFFF)})
            if (dist[v] > 0) dist[v] = 0, pq.push({0.f, v});
        }
        while (!pq.empty()) {
          auto [d, u] = pq.top();
          pq.pop();
          if (d > dist[u]) continue;
          for (auto [w, len] : cmAdj[u])
            if (d + len < dist[w] && d + len <= margin) dist[w] = d + len, pq.push({d + len, w});
        }
        for (uint32_t t = 0; t < T; ++t)
          if (!inSub[t] && dist[cm.tri[t].x] <= margin && dist[cm.tri[t].y] <= margin && dist[cm.tri[t].z] <= margin) inSub[t] = 1;
      }
      std::vector<char> subMine;  // per sub-mesh triangle: in this group (not the margin)
      for (uint32_t t = 0; t < T; ++t) {
        if (!inSub[t]) continue;
        subMine.push_back(group[t] == gi);
        glm::uvec3 nt;
        for (int k = 0; k < 3; ++k) {
          uint32_t v = cm.tri[t][k];
          if (map[v] < 0) map[v] = (int64_t)sub.pos.size(), sub.pos.push_back(cm.pos[v]);
          nt[k] = (uint32_t)map[v];
        }
        sub.tri.push_back(nt), subDens.push_back(triDens[t]);
        sub.area += triArea[t];
      }
      std::unordered_set<EdgeKey> sharp;
      for (EdgeKey e : laid) {
        uint32_t a = uint32_t(e >> 32), b = uint32_t(e & 0xFFFFFFFF);
        if (map[a] < 0 || map[b] < 0) continue;
        auto et = edgeTris.find(e);
        if (et == edgeTris.end()) continue;
        const auto& tt = et->second;
        if (tt[0] < 0 || tt[1] < 0 || !inSub[tt[0]] || !inSub[tt[1]]) continue;
        sharp.insert(ekey((uint32_t)map[a], (uint32_t)map[b]));
      }
      bool groupPainted = false;
      for (float d : subDens) groupPainted |= std::abs(d - subDens[0]) > 1e-3f;
      if (groups > 1)
        PLOG_I("Retopology (flow): zones at resolution x%.2f: %zu triangles, %.3f m2", groupRes[gi], sub.tri.size(), sub.area);
      // (made before with the same input: taken again)
      uint64_t gkey = 1469598103934665603ull;
      auto mix = [&](const void* data, size_t n) {
        const unsigned char* b = (const unsigned char*)data;
        for (size_t i = 0; i < n; ++i) gkey = (gkey ^ b[i]) * 1099511628211ull;
      };
      if (opt.cache) {
        mix(sub.pos.data(), sub.pos.size() * sizeof(vec3)), mix(sub.tri.data(), sub.tri.size() * sizeof(glm::uvec3));
        mix(subDens.data(), subDens.size() * sizeof(float));
        std::vector<EdgeKey> sh(sharp.begin(), sharp.end());
        std::sort(sh.begin(), sh.end());
        mix(sh.data(), sh.size() * sizeof(EdgeKey));
        double qpm = opt.quadsPerM2 * groupRes[gi];
        mix(&qpm, sizeof qpm), mix(&groupPainted, sizeof groupPainted);
        int knobs[4] = {opt.seed, opt.variants, opt.passes, opt.fieldSmoothing};
        mix(knobs, sizeof knobs);
        // (the lines and strokes within reach of this group's surface: they are laid into its quads and turn
        // its field; only those, so a line drawn on the face leaves the body, whose box takes in the hair
        // round the face, as it was)
        const float reach = 2.f / (float)std::sqrt(std::max(1e-6, qpm));
        auto cellOf = [&](const vec3& p) {
          glm::ivec3 c = glm::ivec3(glm::floor(p / reach));
          return (uint64_t(c.x & 0x1FFFFF) << 42) | (uint64_t(c.y & 0x1FFFFF) << 21) | uint64_t(c.z & 0x1FFFFF);
        };
        std::unordered_set<uint64_t> cells;
        for (auto& p : sub.pos) cells.insert(cellOf(p));
        auto near = [&](const vec3& p) {
          for (int dz = -1; dz <= 1; ++dz)
            for (int dy = -1; dy <= 1; ++dy)
              for (int dx = -1; dx <= 1; ++dx)
                if (cells.count(cellOf(p + reach * vec3((float)dx, (float)dy, (float)dz)))) return true;
          return false;
        };
        for (size_t li = 0; li < lines.size(); ++li) {
          bool nearIt = false;
          for (size_t i = 0; i < lines[li].size() && !nearIt; ++i) {
            nearIt |= near(lines[li][i]);
            // (and along a long segment, every reach)
            if (i + 1 < lines[li].size()) {
              vec3 a = lines[li][i], e = lines[li][i + 1] - a;
              int n = (int)std::ceil(glm::length(e) / reach);
              for (int k = 1; k < n && !nearIt; ++k) nearIt |= near(a + e * (float(k) / n));
            }
          }
          if (!nearIt) continue;
          mix(&li, sizeof li), mix(lines[li].data(), lines[li].size() * sizeof(vec3)), mix(&lineIsEdge[li], 1);
        }
      }
      Variant v;
      bool reused = false;
      if (opt.cache) {
        std::lock_guard<std::mutex> lk(opt.cache->groupMutex);
        for (auto& g : opt.cache->groupsMade)
          if (g.key == gkey) {
            v.pos = g.pos, v.quads = g.quads, v.tris = g.tris, v.fieldSingular = g.fieldSingular, v.held = g.held, v.smoothing = g.smoothing;
            v.seed = g.seed, v.poles = g.poles, v.score = g.score, v.missed = g.missed, v.ok = true, reused = true;
            heldTotal += g.held;
            break;
          }
      }
      if (reused) PLOG_I("Retopology (flow): zones at x%.2f as made before (nothing there changed)", groupRes[gi]);
      else {
        v = makeGroup(sub, subDens, sharp, opt.quadsPerM2 * groupRes[gi], groupPainted);
        if (opt.cache && v.ok && !cancelled()) {
          std::lock_guard<std::mutex> lk(opt.cache->groupMutex);
          FlowCache::Group g;
          g.key = gkey, g.pos = v.pos, g.quads = v.quads, g.tris = v.tris, g.fieldSingular = v.fieldSingular, g.held = v.held;
          g.smoothing = v.smoothing, g.seed = v.seed, g.poles = v.poles, g.score = v.score, g.missed = v.missed;
          auto& list = opt.cache->groupsMade;
          list.push_back(std::move(g));
          if (list.size() > 8) list.erase(list.begin());
        }
      }
      if (groups > 1 && v.ok) {
        // only the quads (and filled holes) on this group's side: by the nearest of the triangles they
        // were made on
        const float cellS = std::max(Lmin, 1e-6f);
        auto keyS = [&](glm::ivec3 c) { return (uint64_t(c.x & 0x1FFFFF) << 42) | (uint64_t(c.y & 0x1FFFFF) << 21) | uint64_t(c.z & 0x1FFFFF); };
        std::unordered_map<uint64_t, std::vector<uint32_t>> grid;
        std::vector<vec3> mid(sub.tri.size());
        for (uint32_t t = 0; t < sub.tri.size(); ++t) {
          mid[t] = (sub.pos[sub.tri[t].x] + sub.pos[sub.tri[t].y] + sub.pos[sub.tri[t].z]) / 3.f;
          grid[keyS(glm::ivec3(glm::floor(mid[t] / cellS)))].push_back(t);
        }
        auto mine = [&](const vec3& p) {
          glm::ivec3 c = glm::ivec3(glm::floor(p / cellS));
          float bd = 1e30f;
          int best = -1;
          for (int r = 1; r <= 3 && best < 0; ++r)
            for (int dz = -r; dz <= r; ++dz)
              for (int dy = -r; dy <= r; ++dy)
                for (int dx = -r; dx <= r; ++dx) {
                  auto it = grid.find(keyS(c + glm::ivec3(dx, dy, dz)));
                  if (it == grid.end()) continue;
                  for (uint32_t t : it->second)
                    if (float d = glm::dot(mid[t] - p, mid[t] - p); d < bd) bd = d, best = (int)t;
                }
          return best >= 0 && subMine[best];
        };
        std::vector<std::array<uint32_t, 4>> q2;
        for (auto& q : v.quads)
          if (mine(0.25f * (v.pos[q[0]] + v.pos[q[1]] + v.pos[q[2]] + v.pos[q[3]]))) q2.push_back(q);
        std::vector<std::array<uint32_t, 3>> t2;
        for (auto& t : v.tris)
          if (mine((v.pos[t[0]] + v.pos[t[1]] + v.pos[t[2]]) / 3.f)) t2.push_back(t);
        v.quads = std::move(q2), v.tris = std::move(t2);
        // where QuadriFlow's loop strays off the line, the kept quads can meet at a corner only: of the
        // quads round such a vertex, the smaller fans go
        for (int round = 0; round < 6; ++round) {
          std::unordered_map<EdgeKey, int> uses;
          for (auto& q : v.quads)
            for (int k = 0; k < 4; ++k) uses[ekey(q[k], q[(k + 1) % 4])]++;
          for (auto& t : v.tris)
            for (int k = 0; k < 3; ++k) uses[ekey(t[k], t[(k + 1) % 3])]++;
          std::unordered_map<uint32_t, int> outs;
          for (auto& q : v.quads)
            for (int k = 0; k < 4; ++k)
              if (uses[ekey(q[k], q[(k + 1) % 4])] == 1) outs[q[k]]++;
          for (auto& t : v.tris)
            for (int k = 0; k < 3; ++k)
              if (uses[ekey(t[k], t[(k + 1) % 3])] == 1) outs[t[k]]++;
          std::vector<char> drop(v.quads.size(), 0);
          bool any = false;
          for (auto& [x, n] : outs) {
            if (n < 2) continue;
            // the quads round x, in fans joined across edges at x
            std::vector<size_t> at;
            for (size_t qi = 0; qi < v.quads.size(); ++qi)
              for (uint32_t y : v.quads[qi])
                if (y == x) at.push_back(qi);
            std::vector<int> fan(at.size(), -1);
            int fans = 0;
            for (size_t i = 0; i < at.size(); ++i) {
              if (fan[i] >= 0) continue;
              fan[i] = fans;
              std::vector<size_t> st = {i};
              while (!st.empty()) {
                size_t a = st.back();
                st.pop_back();
                for (size_t b = 0; b < at.size(); ++b) {
                  if (fan[b] >= 0) continue;
                  int shared = 0;
                  for (uint32_t y : v.quads[at[a]])
                    for (uint32_t z : v.quads[at[b]]) shared += y == z && y != x;
                  if (shared) fan[b] = fans, st.push_back(b);
                }
              }
              ++fans;
            }
            if (fans < 2) continue;
            std::vector<int> size(fans, 0);
            for (int f : fan) size[f]++;
            int keepFan = int(std::max_element(size.begin(), size.end()) - size.begin());
            for (size_t i = 0; i < at.size(); ++i)
              if (fan[i] != keepFan) drop[at[i]] = 1, any = true;
          }
          if (!any) break;
          std::vector<std::array<uint32_t, 4>> q3;
          for (size_t qi = 0; qi < v.quads.size(); ++qi)
            if (!drop[qi]) q3.push_back(v.quads[qi]);
          v.quads = std::move(q3);
        }
        // a quad or triangle missing by the line (its middle fell on the other side): a face of its own
        {
          const float Lg = 1.f / (float)std::sqrt(std::max(1e-6, opt.quadsPerM2 * groupRes[gi]));
          for (auto& h : smallHoles(v.quads, &v.tris)) {
            float per = 0;
            for (size_t i = 0; i < h.size(); ++i) per += glm::length(v.pos[h[(i + 1) % h.size()]] - v.pos[h[i]]);
            if (per > 4 * Lg) continue;
            // (not one lying along a border with another group: that is this group's edge, the end of
            // a thin limb, say, to be joined to the other side)
            bool along = true;
            for (uint32_t x : h) {
              float bd = 1e30f;
              for (auto& ch : chains)
                for (size_t i = 0; i + 1 < ch.line.size(); ++i) bd = std::min(bd, segDist2(v.pos[x], ch.line[i], ch.line[i + 1]));
              along &= bd < 0.5625f * Lg * Lg;
            }
            if (along) continue;
            if (h.size() == 3) v.tris.push_back({h[2], h[1], h[0]});
            else v.quads.push_back({h[3], h[2], h[1], h[0]});
          }
        }
      }
      if (const char* dbg = getenv("PATINA_RETOPO_DEBUG"); dbg && *dbg == '/' && groups > 1) {
        std::string base = std::string(dbg) + "_g" + std::to_string(gi);
        if (FILE* fo = fopen((base + "_tris.obj").c_str(), "w")) {
          for (auto& p : sub.pos) fprintf(fo, "v %f %f %f\n", p.x, p.y, p.z);
          for (auto& t : sub.tri) fprintf(fo, "f %u %u %u\n", t.x + 1, t.y + 1, t.z + 1);
          fclose(fo);
        }
        if (FILE* fo = fopen((base + "_quads.obj").c_str(), "w")) {
          for (auto& p : v.pos) fprintf(fo, "v %f %f %f\n", p.x, p.y, p.z);
          for (auto& q : v.quads) fprintf(fo, "f %u %u %u %u\n", q[0] + 1, q[1] + 1, q[2] + 1, q[3] + 1);
          fclose(fo);
        }
        if (gi == 0)
          if (FILE* fo = fopen((std::string(dbg) + "_borders.obj").c_str(), "w")) {
            size_t n = 0;
            for (auto& B : chains) {
              for (auto& p : B.line) fprintf(fo, "v %f %f %f\n", p.x, p.y, p.z);
              for (size_t i = 0; i + 1 < B.line.size(); ++i) fprintf(fo, "l %zu %zu\n", n + i + 1, n + i + 2);
              n += B.line.size();
            }
            fclose(fo);
          }
      }
      return v;
    };
    std::vector<Variant> groupOut(groups);
    {
      std::vector<std::thread> gthreads;
      for (int gi = 0; gi < groups; ++gi)
        if (!groupRing[gi])
          gthreads.emplace_back([&, gi] {
            try {
              groupOut[gi] = makeGroupOf(gi);
            } catch (...) {
              groupOut[gi].ok = false;
            }
          });
      for (auto& t : gthreads) t.join();
    }
    PLOG_D("Retopology (flow): the other zones' quads made at %.2f s", since());
    // the ring zones, one after another: a loop one beside it made already is shared (the same points)
    struct Shared {
      int g, h;  // (h's loop made with g's points)
      std::vector<vec3> pts;
    };
    std::vector<Shared> shared;
    // the counts round the rings' loops, planned along each chain of rings at once (an eye's rings keep one
    // count from the socket out; the steps go where the last meets the zone beside it)
    {
      std::vector<float> Lz(groups);
      for (int g = 0; g < groups; ++g) Lz[g] = 1.f / (float)std::sqrt(std::max(1e-6, opt.quadsPerM2 * groupRes[g]));
      std::vector<char> kinds(groupRing.begin(), groupRing.end());
      ChainPlan cp = planChains(cm, edgeTris, group, kinds, Lz);
      // (a loop along which a zone beside it, made already, has its own edge loop: the ring will take those
      // points, so their count is planned with)
      {
        std::vector<std::vector<vec3>> made;
        for (int gj = 0; gj < groups; ++gj) {
          if (groupRing[gj] || !groupOut[gj].ok) continue;
          std::vector<std::vector<uint32_t>> bl;
          if (!boundaryLoops(groupOut[gj].quads, nullptr, bl, &groupOut[gj].tris)) continue;
          for (auto& l : bl) {
            std::vector<vec3> pts;
            for (uint32_t v : l) pts.push_back(groupOut[gj].pos[v]);
            made.push_back(std::move(pts));
          }
        }
        bool any = false;
        for (size_t l = 0; l < cp.loops.size(); ++l) {
          if (!cp.loops[l].meetsZone || made.empty()) continue;
          const float Lq = (float)(cp.length[l] / std::max(cp.loops[l].lengthQ, 1e-9));
          if (size_t n = matchingLoopCount(cp.points[l], made, Lq); n >= 4) cp.loops[l].fixedCount = (long)n, any = true;
        }
        if (any) cp.counts = planRingCounts(cp.loops, cp.rings);
      }
      if (getenv("PATINA_RETOPO_DEBUG"))
        for (size_t i = 0; i < cp.counts.size(); ++i)
          PLOG_I("  ring loop %zu at %.3f %.3f %.3f: %.1f quads long, %s: %ld planned", i, cp.center[i].x, cp.center[i].y, cp.center[i].z,
                 cp.loops[i].lengthQ, cp.loops[i].meetsZone ? "meets a zone" : "any count", cp.counts[i]);
      for (size_t i = 0; i < cp.counts.size(); ++i)
        if (cp.counts[i] >= 4) ringPlan.push_back({cp.center[i], cp.length[i], cp.counts[i]});
    }
    std::vector<int> ringOrder;  // (rings, then the discs inside them)
    for (int kind : {1, 2})
      for (int gi = 0; gi < groups; ++gi)
        if (groupRing[gi] == kind) ringOrder.push_back(gi);
    for (size_t oi = 0; oi < ringOrder.size(); ++oi) {
      const int gi = ringOrder[oi];
      std::vector<std::vector<vec3>> fixed;
      std::vector<int> fixedOf;
      std::vector<std::vector<uint32_t>> fixedVerts;  // (a zone's edge loop: its vertices, which may be put onto the line)
      for (size_t oj = 0; oj < oi; ++oj)
        for (auto& L : groupOut[ringOrder[oj]].ringLoopPos) fixed.push_back(L), fixedOf.push_back(ringOrder[oj]), fixedVerts.emplace_back();
      // (and the edge loops of the other zones at about the same resolution: a ring takes those points
      // where one runs all the way along its loop, so no joining row is needed there)
      for (int gj = 0; gj < groups; ++gj) {
        if (groupRing[gj] || !groupOut[gj].ok || groupRes[gj] > 1.25 * groupRes[gi] || groupRes[gi] > 1.25 * groupRes[gj]) continue;
        std::vector<std::vector<uint32_t>> bl;
        if (!boundaryLoops(groupOut[gj].quads, nullptr, bl, &groupOut[gj].tris)) continue;
        for (auto& l : bl) {
          std::vector<vec3> pts;
          for (uint32_t v : l) pts.push_back(groupOut[gj].pos[v]);
          fixed.push_back(std::move(pts)), fixedOf.push_back(gj), fixedVerts.push_back(l);
        }
      }
      std::vector<char> looseGroup(groups, 0);
      for (size_t oj = oi + 1; oj < ringOrder.size(); ++oj) looseGroup[ringOrder[oj]] = 1;
      // (a loop taken from beside: shared; the few points of a zone's edge loop the ring put onto the line
      // moved there in that zone too, unless that turns one of its quads over: then the ring is made
      // again without that loop, and joined to it by a row)
      for (int attempt = 0; attempt < 4; ++attempt) {
        try {
          groupOut[gi] = makeRingOf(gi, fixed, looseGroup);
        } catch (...) {
          groupOut[gi].ok = false;
        }
        if (!groupOut[gi].ok) break;
        struct Move {
          size_t f;
          uint32_t v;
          vec3 to;
        };
        std::vector<Move> moves;
        std::vector<std::pair<size_t, std::vector<vec3>>> takes;  // (the fixed loop, the ring's loop)
        for (auto& L : groupOut[gi].ringLoopPos)
          for (size_t f = 0; f < fixed.size(); ++f) {
            const size_t n = L.size();
            if (n < 3 || fixed[f].size() != n) continue;
            std::unordered_map<uint64_t, size_t> at;
            auto bits = [](const vec3& p) {
              uint32_t x, y, z;
              std::memcpy(&x, &p.x, 4), std::memcpy(&y, &p.y, 4), std::memcpy(&z, &p.z, 4);
              return (uint64_t(x) * 0x9E3779B97F4A7C15ull) ^ (uint64_t(y) * 0xC2B2AE3D27D4EB4Full) ^ uint64_t(z);
            };
            for (size_t j = 0; j < n; ++j) at[bits(fixed[f][j])] = j;
            // (the turn and the start from the points found as they are)
            long shift = -1;
            int dir = 0;
            size_t same = 0;
            for (size_t k = 0; k < n; ++k) {
              auto it = at.find(bits(L[k]));
              if (it == at.end()) continue;
              ++same;
              auto it2 = at.find(bits(L[(k + 1) % n]));
              if (dir == 0 && it2 != at.end()) {
                dir = it2->second == (it->second + 1) % n ? 1 : it2->second == (it->second + n - 1) % n ? -1 : 0;
                if (dir) shift = (long)it->second - dir * (long)k;
              }
            }
            if (same == n) takes.push_back({f, L});
            if (dir == 0 || same == n || fixedVerts[f].empty() || same + movableLoopPoints(n) < n) continue;
            for (size_t k = 0; k < n; ++k) {
              size_t j = (size_t)(((shift + dir * (long)k) % (long)n + (long)n) % (long)n);
              if (fixed[f][j] != L[k]) moves.push_back({f, fixedVerts[f][j], L[k]});
            }
            takes.push_back({f, L});
          }
        // (the zone's row along a loop it gives: each face there kept facing the way it did and not a
        // sliver, the moved points where they go; a row of slivers along the line would stay, where a join
        // cuts it)
        size_t foldsAt = SIZE_MAX;
        for (auto& [f, L] : takes) {
          if (fixedVerts[f].empty() || foldsAt != SIZE_MAX) continue;
          const Variant& z = groupOut[fixedOf[f]];
          const double Lz = 1.0 / std::sqrt(std::max(1e-6, opt.quadsPerM2 * groupRes[fixedOf[f]]));
          std::unordered_map<uint32_t, vec3> to;
          for (auto& mv : moves)
            if (mv.f == f) to[mv.v] = mv.to;
          std::unordered_set<uint32_t> onLoop(fixedVerts[f].begin(), fixedVerts[f].end());
          auto at = [&](uint32_t v) {
            auto it = to.find(v);
            return it == to.end() ? z.pos[v] : it->second;
          };
          auto bad = [&](const uint32_t* fc, int k) {
            int on = 0;
            for (int i = 0; i < k; ++i) on += onLoop.count(fc[i]) > 0;
            if (on < 2) return false;
            vec3 n0(0), n1(0);
            for (int i = 0; i < k; ++i) {
              n0 += glm::cross(z.pos[fc[i]], z.pos[fc[(i + 1) % k]]);
              n1 += glm::cross(at(fc[i]), at(fc[(i + 1) % k]));
            }
            bool b = glm::dot(n0, n1) <= 0.f || 0.5 * glm::length(n1) < (k == 4 ? 0.03 : 0.015) * Lz * Lz;
            if (b && getenv("PATINA_RETOPO_DEBUG"))
              PLOG_I("  face along a loop: %d sides, %d on the loop, area %.3f quads, before %.3f, facing %.2f", k, on, 0.5 * glm::length(n1) / (Lz * Lz),
                     0.5 * glm::length(n0) / (Lz * Lz), glm::dot(glm::normalize(n0), glm::normalize(n1)));
            return b;
          };
          for (auto& q : z.quads)
            if (bad(q.data(), 4)) foldsAt = f;
          for (auto& t : z.tris)
            if (bad(t.data(), 3)) foldsAt = f;
        }
        if (getenv("PATINA_RETOPO_DEBUG")) {
          for (auto& mv : moves) {
            const Variant& z = groupOut[fixedOf[mv.f]];
            int touching = 0;
            for (auto& q : z.quads) touching += std::count(q.begin(), q.end(), mv.v) > 0;
            PLOG_I("  move of vertex %u (zone %d, %zu points): %.4f, %d quads touching", mv.v, fixedOf[mv.f], fixed[mv.f].size(), glm::length(mv.to - z.pos[mv.v]), touching);
          }
        }
        if (foldsAt != SIZE_MAX && attempt < 3) {
          PLOG_I("Retopology (flow): a zone's edge loop not taken by the ring beside it (faces along it flat or turned over)");
          fixed[foldsAt].clear();
          continue;
        }
        // (still: the ring has points of a loop it can't share, and a row joining it there would be flat;
        // made as the other zones are)
        if (foldsAt != SIZE_MAX) {
          groupOut[gi].ok = false;
          break;
        }
        for (auto& mv : moves) groupOut[fixedOf[mv.f]].pos[mv.v] = mv.to;
        for (auto& [f, L] : takes) {
          size_t moved = 0;
          for (auto& mv : moves) moved += mv.f == f;
          if (moved) PLOG_I("Retopology (flow): a ring takes a zone's edge loop, %zu of its %zu points put onto the line", moved, L.size());
          shared.push_back({fixedOf[f], gi, L});
          if (getenv("PATINA_RETOPO_DEBUG")) PLOG_I("  ring zone %d shares a loop of %zu with zone %d", gi, L.size(), fixedOf[f]);
        }
        break;
      }
      if (!groupOut[gi].ok) {
        groupRing[gi] = 0;  // (made as other zones are)
        try {
          groupOut[gi] = makeGroupOf(gi);
        } catch (...) {
          groupOut[gi].ok = false;
        }
        continue;
      }
    }
    for (int gi = 0; gi < groups && !failed; ++gi) {
      Variant& v = groupOut[gi];
      if (!v.ok) {
        failed = true;
        break;
      }
      uint32_t base = (uint32_t)P.size();
      P.insert(P.end(), v.pos.begin(), v.pos.end());
      for (auto q : v.quads) {
        for (auto& x : q) x += base;
        Qd.push_back(q), quadGroup.push_back(gi);
      }
      for (auto t : v.tris) {
        for (auto& x : t) x += base;
        Tr.push_back(t), triGroup.push_back(gi);
      }
    }
    if (cancelled()) {
      res.cancelled = true;
      return res;
    }
    PLOG_I("Retopology (flow): the zones' quads made at %.2f s", since());
    if (failed || Qd.empty()) {
      res.problems.push_back("the quads could not be made (the field didn't come out consistent)");
      return res;
    }

    // the loops two ring zones share: their vertices made one, and no joining row there
    std::vector<JoinChain> joinChains = chains;
    if (!shared.empty()) {
      std::unordered_map<uint64_t, uint32_t> at;  // (a position's bits) -> its first vertex
      auto key = [](const vec3& p) {
        uint32_t x, y, z;
        std::memcpy(&x, &p.x, 4), std::memcpy(&y, &p.y, 4), std::memcpy(&z, &p.z, 4);
        return (uint64_t(x) * 0x9E3779B97F4A7C15ull) ^ (uint64_t(y) * 0xC2B2AE3D27D4EB4Full) ^ uint64_t(z);
      };
      std::vector<uint32_t> to(P.size());
      std::iota(to.begin(), to.end(), 0u);
      std::unordered_set<uint64_t> wanted;
      for (auto& sh : shared)
        for (auto& p : sh.pts) wanted.insert(key(p));
      for (uint32_t v = 0; v < P.size(); ++v) {
        uint64_t k = key(P[v]);
        if (!wanted.count(k)) continue;
        auto [it, fresh] = at.emplace(k, v);
        if (!fresh) to[v] = it->second;
      }
      for (auto& q : Qd)
        for (auto& x : q) x = to[x];
      for (auto& t : Tr)
        for (auto& x : t) x = to[x];
      std::vector<JoinChain> kept;
      for (auto& ch : chains) {
        bool skip = false;
        for (auto& sh : shared) skip |= ch.closed && ((ch.g == sh.g && ch.h == sh.h) || (ch.g == sh.h && ch.h == sh.g));
        if (!skip) kept.push_back(ch);
        if (getenv("PATINA_RETOPO_DEBUG"))
          PLOG_I("  chain between zones %d and %d%s, %zu points: %s", ch.g, ch.h, ch.closed ? " (closed)" : "", ch.line.size(), skip ? "shared" : "joined");
      }
      PLOG_I("Retopology (flow): %zu loops shared by ring zones", shared.size());
      joinChains = std::move(kept);
    }
    // ---- where zones of different resolution meet: joined row by row (see RetopoJoin.h)
    if (!joinChains.empty()) {
      std::vector<double> groupL(groups);
      // (a ring zone keeps its edge where it meets another zone as fine, a ring before a small disc: the
      // other side's row is the one taken out)
      for (int g = 0; g < groups; ++g)
        groupL[g] = (groupRing[g] == 1 ? 0.998 : groupRing[g] == 2 ? 0.999 : 1.0) / std::sqrt(opt.quadsPerM2 * groupRes[g]);
      auto snap = [&](const vec3& p) {
        RayHit h = bvh.closestPoint(high, p, 1e30f);
        return h.hit ? h.position : p;
      };
      if (!joinGroups(P, Qd, quadGroup, Tr, triGroup, joinChains, groupL, openEdges, bandV, joinStats, snap)) failed = true;
    }
    if (failed && anyRings) return retryRings("the zones' rows didn't join");
    if (failed) {
      // fall back: everything together, the resolutions as a size field
      PLOG_I("Retopology (flow): the zones' rows didn't join; made together instead");
      FlowOptions o2 = opt;
      o2.ringZones = false;  // (made together: no zones of their own)
      std::vector<float> dens(high.positions.size(), 1.f);
      if (hasRes) {
        std::vector<float> sum(high.positions.size(), 0.f);
        std::vector<int> n(high.positions.size(), 0);
        for (uint32_t fc = 0; fc < high.faceCount(); ++fc)
          for (uint32_t l = high.faceStart[fc]; l < high.faceStart[fc + 1]; ++l) sum[high.loopVert[l]] += faceResolution[fc], n[high.loopVert[l]]++;
        for (size_t v = 0; v < dens.size(); ++v) dens[v] = (hasDens ? vertexDensity[v] : 1.f) * (n[v] ? sum[v] / n[v] : 1.f);
      } else if (hasDens) {
        dens = vertexDensity;
      }
      Result r = flowQuads(high, bvh, strokes, faceLabel, dens, o2, {});
      r.problems.push_back("zones of different resolution couldn't be joined row by row (a zone is narrower than its quads somewhere), so "
                           "their resolutions only steer the quads' size");
      r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      return r;
    }
  }
  // the ring zones' vertices: placed already, not smoothed
  std::vector<char> ringV(P.size(), 0);
  for (size_t q = 0; q < Qd.size(); ++q)
    if (quadGroup[q] >= 0 && groupRing[quadGroup[q]])
      for (uint32_t x : Qd[q]) ringV[x] = 1;
  for (size_t t = 0; t < Tr.size(); ++t)
    if (triGroup[t] >= 0 && groupRing[triGroup[t]])
      for (uint32_t x : Tr[t]) ringV[x] = 1;
  // pairs of poles in the joining rows and patches taken out (never on the lines or in the rings)
  if (!chains.empty() && !opt.layout) {
    std::vector<char> protect = ringV;
    protect.resize(P.size(), 0);
    const float near = 0.2f * Lmin, cell = std::max(Lmin, 1e-6f);
    std::unordered_map<uint64_t, std::vector<std::pair<vec3, vec3>>> segAt;
    auto cellKey = [&](const vec3& p) {
      auto q = [&](float x) { return (uint64_t)((int64_t)std::floor(x / cell) & 0x1FFFFF); };
      return (q(p.x) << 42) | (q(p.y) << 21) | q(p.z);
    };
    for (size_t li = 0; li < lines.size(); ++li) {
      if (!lineIsEdge[li]) continue;
      for (size_t i = 0; i + 1 < lines[li].size(); ++i) {
        vec3 a = lines[li][i], b = lines[li][i + 1];
        int n = std::max(1, (int)std::ceil(glm::length(b - a) / cell));
        std::unordered_set<uint64_t> cells;
        for (int k = 0; k <= n; ++k) cells.insert(cellKey(glm::mix(a, b, float(k) / n)));
        for (uint64_t c : cells) segAt[c].push_back({a, b});
      }
    }
    for (size_t v = 0; v < P.size(); ++v) {
      if (protect[v]) continue;
      for (int dx = -1; dx <= 1 && !protect[v]; ++dx)
        for (int dy = -1; dy <= 1 && !protect[v]; ++dy)
          for (int dz = -1; dz <= 1 && !protect[v]; ++dz) {
            auto it = segAt.find(cellKey(P[v] + cell * vec3(dx, dy, dz)));
            if (it == segAt.end()) continue;
            for (auto& [a, b] : it->second)
              if (segDist2(P[v], a, b) < near * near) {
                protect[v] = 1;
                break;
              }
          }
    }
    static const bool noClean = getenv("PATINA_RETOPO_NOCLEAN") != nullptr;
    if (!noClean)
      if (int n = cleanJoinPoles(P, Qd, quadGroup, Tr, protect)) PLOG_I("Retopology (flow): %d pairs of poles where zones join taken out", n);
  }
  // the vertices no longer used out
  {
    std::vector<int64_t> keep(P.size(), -1);
    std::vector<vec3> np;
    std::vector<char> nb2, nr2;
    bandV.resize(P.size(), 0);
    auto use = [&](uint32_t& v) {
      if (keep[v] < 0) keep[v] = (int64_t)np.size(), np.push_back(P[v]), nb2.push_back(bandV[v]), nr2.push_back(ringV[v]);
      v = (uint32_t)keep[v];
    };
    for (auto& q : Qd)
      for (auto& v : q) use(v);
    for (auto& t : Tr)
      for (auto& v : t) use(v);
    P = std::move(np), bandV = std::move(nb2), ringV = std::move(nr2);
  }
  if (!chains.empty())
    PLOG_I("Retopology (flow): %d zone borders joined: %d 3-to-1 steps, %d odd steps, %d holes filled where they meet (%d with a grid), %d tiny "
           "edges welded",
           joinStats.chains, joinStats.steps31, joinStats.oddSteps, joinStats.holesFilled, joinStats.gridHoles, joinStats.welded);
  if (!joinStats.patched.empty()) {
    for (auto& c : joinStats.patched) res.uncovered.push_back(c);
    res.problems.push_back(std::to_string(joinStats.patched.size()) +
                           (joinStats.patched.size() == 1 ? " place where zones meet in a tight spot was" : " places where zones meet in a tight spot were") +
                           " patched (marked in red): lines further apart there give cleaner quads");
  }

  // 0.21 (Fewer poles): the quads' counts solved again from their own layout: runs between a three- and a
  // five-edged pole drawn toward nothing, so pairs cancel
  if (const char* rq = getenv("PATINA_REQUANT"); (rq || opt.requantize) && !opt.layout && !Qd.empty()) {
    // (the quads' size wanted at each vertex: the working triangle's nearest)
    std::vector<float> cmL(cm.pos.size(), 1e30f);
    for (uint32_t t = 0; t < T; ++t) {
      float Lt = 1.f / (float)std::sqrt(std::max(1e-6, opt.quadsPerM2 * triDens[t] * groupRes[group[t]]));
      for (int k = 0; k < 3; ++k) cmL[cm.tri[t][k]] = std::min(cmL[cm.tri[t][k]], Lt);
    }
    const float cell = std::max(Lmin, 1e-6f);
    std::unordered_map<uint64_t, std::vector<uint32_t>> g;
    auto gk = [&](const vec3& p3, int dx, int dy, int dz) {
      int64_t x = (int64_t)std::floor(p3.x / cell) + dx, y = (int64_t)std::floor(p3.y / cell) + dy, z = (int64_t)std::floor(p3.z / cell) + dz;
      return (uint64_t(x & 0x1FFFFF) << 42) | (uint64_t(y & 0x1FFFFF) << 21) | uint64_t(z & 0x1FFFFF);
    };
    for (uint32_t v = 0; v < cm.pos.size(); ++v) g[gk(cm.pos[v], 0, 0, 0)].push_back(v);
    std::vector<float> vL(P.size(), (float)L0);
    for (size_t v = 0; v < P.size(); ++v) {
      float bd = 1e30f;
      for (int r = 1; r <= 3 && bd >= 1e30f; ++r)
        for (int dz = -r; dz <= r; ++dz)
          for (int dy = -r; dy <= r; ++dy)
            for (int dx = -r; dx <= r; ++dx) {
              auto it = g.find(gk(P[v], dx, dy, dz));
              if (it == g.end()) continue;
              for (uint32_t c : it->second)
                if (float d2 = glm::dot(cm.pos[c] - P[v], cm.pos[c] - P[v]); d2 < bd && cmL[c] < 1e29f) bd = d2, vL[v] = cmL[c];
            }
    }
    QuadLayout ql = layoutFromQuads(P, Qd, Tr, vL, lines, lineIsEdge);
    LayoutQuadsOptions lo;
    lo.polePull = rq && *rq ? (float)atof(rq) : 6.f;
    lo.smoothLengths = false, lo.keepLeftRuns = true, lo.capHoles = true;
    lo.keepCounts = !getenv("PATINA_REQUANT_LENGTHS");
    LayoutQuads lq = quadsFromLayout(ql, lo);
    // (kept only when it has no more faces turned against those round them, and fewer poles)
    auto measure = [](const std::vector<vec3>& X, const std::vector<std::array<uint32_t, 4>>& Q, const std::vector<std::array<uint32_t, 3>>& Tt) {
      std::vector<vec3> fn;
      std::unordered_map<uint64_t, std::vector<uint32_t>> at;
      std::unordered_map<uint32_t, std::unordered_set<uint32_t>> nbv;
      auto add = [&](const uint32_t* f, int k) {
        const uint32_t id = (uint32_t)fn.size();
        vec3 n = k == 4 ? glm::cross(X[f[2]] - X[f[0]], X[f[3]] - X[f[1]]) : glm::cross(X[f[1]] - X[f[0]], X[f[2]] - X[f[0]]);
        fn.push_back(glm::length(n) > 0 ? glm::normalize(n) : n);
        for (int i = 0; i < k; ++i) {
          at[ekey(f[i], f[(i + 1) % k])].push_back(id);
          nbv[f[i]].insert(f[(i + 1) % k]), nbv[f[(i + 1) % k]].insert(f[i]);
        }
      };
      for (auto& q : Q) add(q.data(), 4);
      for (auto& t : Tt) add(t.data(), 3);
      std::vector<float> sum(fn.size(), 0.f);
      std::vector<int> cnt(fn.size(), 0);
      std::unordered_set<uint32_t> open;
      for (auto& [e, F] : at) {
        if (F.size() == 1) open.insert(uint32_t(e >> 32)), open.insert(uint32_t(e & 0xFFFFFFFF));
        for (size_t i = 0; i < F.size(); ++i)
          for (size_t j = 0; j < F.size(); ++j)
            if (i != j) sum[F[i]] += glm::dot(fn[F[i]], fn[F[j]]), cnt[F[i]]++;
      }
      int folds = 0, poles = 0;
      for (size_t f = 0; f < fn.size(); ++f) folds += cnt[f] && sum[f] < 0;
      for (auto& [v, nb] : nbv) poles += !open.count(v) && nb.size() != 4;
      return std::pair<int, int>{folds, poles};
    };
    const auto before = measure(P, Qd, Tr), after = lq.quads.empty() ? std::pair<int, int>{1 << 30, 1 << 30} : measure(lq.pos, lq.quads, lq.tris);
    if (!lq.quads.empty() && lq.patchesLeft <= (int)Tr.size() + 2 && after.first <= before.first && after.second < before.second) {
      PLOG_I("Retopology (flow): quads solved again from their layout: %zu quads (were %zu), %d poles (were %d), %d folded (were %d)", lq.quads.size(), Qd.size(),
             after.second, before.second, after.first, before.first);
      P = std::move(lq.pos), Qd = std::move(lq.quads), Tr = std::move(lq.tris);
      bandV.assign(P.size(), 0), ringV.assign(P.size(), 0);
      res.resolved = true;
    } else if (!lq.quads.empty()) {
      PLOG_I("Retopology (flow): quads solved again from their layout not kept: %d poles (were %d), %d folded (were %d)", after.second, before.second, after.first,
             before.first);
    } else {
      PLOG_I("Retopology (flow): solving the quads again from their layout didn't work (%d patches left); kept as they were", lq.patchesLeft);
    }
  }
  // ---- the quads, back on the high-poly's surface
  Mesh& low = res.low;
  low.name = "Retopo";
  low.positions = std::move(P);
  low.faceStart.push_back(0);
  for (auto& q : Qd) {
    for (int k = 0; k < 4; ++k) low.loopVert.push_back(q[k]);
    low.faceStart.push_back((uint32_t)low.loopVert.size());
  }
  for (auto& t : Tr) {
    for (int k = 0; k < 3; ++k) low.loopVert.push_back(t[k]);
    low.faceStart.push_back((uint32_t)low.loopVert.size());
  }
  size_t nv = low.positions.size();
  bandV.resize(nv, 0);
  std::vector<std::vector<uint32_t>> nb(nv);
  std::map<std::pair<uint32_t, uint32_t>, int> uses;
  for (uint32_t fc = 0; fc + 1 < low.faceStart.size(); ++fc)
    for (uint32_t l = low.faceStart[fc]; l < low.faceStart[fc + 1]; ++l) {
      uint32_t a = low.loopVert[l], b = low.loopVert[l + 1 < low.faceStart[fc + 1] ? l + 1 : low.faceStart[fc]];
      if (std::find(nb[a].begin(), nb[a].end(), b) == nb[a].end()) nb[a].push_back(b), nb[b].push_back(a);
      uses[{std::min(a, b), std::max(a, b)}]++;
    }
  std::vector<char> open(nv, 0);
  for (auto& [e, n] : uses)
    if (n == 1) open[e.first] = open[e.second] = 1;
  const float reach = useShell ? 2.5f * cube + Lmin : 4 * L0;  // a shell stands off the surface by a cube
  // each vertex's normal from its faces, turned the way the high-poly's normals go
  std::vector<vec3> vn(nv, vec3(0));
  auto normalsOf = [&] {
    std::fill(vn.begin(), vn.end(), vec3(0));
    for (uint32_t fc = 0; fc + 1 < low.faceStart.size(); ++fc) {
      uint32_t s0 = low.faceStart[fc], n = low.faceStart[fc + 1] - s0;
      const auto& X = low.positions;
      vec3 fn = n == 4 ? glm::cross(X[low.loopVert[s0 + 2]] - X[low.loopVert[s0]], X[low.loopVert[s0 + 3]] - X[low.loopVert[s0 + 1]])
                       : glm::cross(X[low.loopVert[s0 + 1]] - X[low.loopVert[s0]], X[low.loopVert[s0 + 2]] - X[low.loopVert[s0]]);
      for (uint32_t k = 0; k < n; ++k) vn[low.loopVert[s0 + k]] += fn;
    }
    for (auto& n : vn) {
      float l = glm::length(n);
      n = l > 0 ? n / l : n;
    }
  };
  normalsOf();
  float facing = 1;  // (-1 when the quads turn the other way from the high-poly)
  {
    double agree = 0;
    for (size_t v = 0; v < nv; v += std::max<size_t>(1, nv / 2000)) {
      RayHit h = bvh.closestPoint(high, low.positions[v], reach);
      if (h.hit) agree += glm::dot(h.normal, vn[v]);
    }
    facing = agree < 0 ? -1.f : 1.f;
  }
  // the surface a vertex lies over: the nearest, unless that faces another way (another part close by,
  // as an eyeball in its socket) and one along the vertex's normal does face its way
  auto onSurface = [&](const vec3& p, size_t v, vec3& out) {
    RayHit h = bvh.closestPoint(high, p, reach);
    if (!h.hit) return false;
    out = h.position;
    vec3 n = facing * vn[v];
    if (glm::length(n) < 0.5f || glm::dot(h.normal, n) > 0.2f) return true;
    // (no further than about a quad: a ray along a stray normal would find some other part of the model)
    float bt = 1e30f;
    for (float sgn : {1.f, -1.f}) {
      RayHit r = bvh.raycast(high, p, sgn * n, std::min(reach, 0.75f * Lmin));
      if (r.hit && r.t < bt && glm::dot(r.normal, n) > 0.2f) bt = r.t, out = r.position;
    }
    return true;
  };
  parallelFor(nv, [&](size_t v) {
    vec3 q;
    if (onSurface(low.positions[v], v, q)) low.positions[v] = q;
  });
  // the vertices of the edge loops along the edge lines stay on them (they slide along them only)
  std::vector<std::array<vec3, 2>> segs;
  for (size_t i = 0; i < lines.size(); ++i)
    if (lineIsEdge[i])
      for (size_t k = 0; k + 1 < lines[i].size(); ++k) segs.push_back({lines[i][k], lines[i][k + 1]});
  std::unordered_map<uint64_t, std::vector<uint32_t>> segGrid;
  auto cellOf = [&](const vec3& p) { return glm::ivec3(glm::floor(p / L0)); };
  auto cellKey = [](glm::ivec3 c) {
    return (uint64_t(c.x & 0x1FFFFF) << 42) | (uint64_t(c.y & 0x1FFFFF) << 21) | uint64_t(c.z & 0x1FFFFF);
  };
  for (uint32_t i = 0; i < segs.size(); ++i) {
    glm::ivec3 a = cellOf(glm::min(segs[i][0], segs[i][1])), b = cellOf(glm::max(segs[i][0], segs[i][1]));
    if (glm::any(glm::greaterThan(b - a, glm::ivec3(64)))) continue;  // (a stray, very long segment)
    for (int z = a.z; z <= b.z; ++z)
      for (int y = a.y; y <= b.y; ++y)
        for (int x = a.x; x <= b.x; ++x) segGrid[cellKey({x, y, z})].push_back(i);
  }
  auto onLine = [&](const vec3& p, vec3& q) {
    glm::ivec3 c = cellOf(p);
    float bd = 1e30f;
    for (int z = -1; z <= 1; ++z)
      for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
          auto it = segGrid.find(cellKey(c + glm::ivec3(x, y, z)));
          if (it == segGrid.end()) continue;
          for (uint32_t i : it->second) {
            vec3 r;
            if (float d = segDist2(p, segs[i][0], segs[i][1], &r); d < bd) bd = d, q = r;
          }
        }
    return bd;
  };
  std::vector<char> lineV(nv, 0);
  int onLines = 0;
  if (!segs.empty())
    for (size_t v = 0; v < nv; ++v) {
      vec3 q;
      if (onLine(low.positions[v], q) < 0.04f * Lmin * Lmin) lineV[v] = 1, low.positions[v] = q, ++onLines;
    }
  auto relax = [&](int iterations, bool bandOnly) {
    for (int it = 0; it < iterations; ++it) {
      std::vector<vec3> next = low.positions;
      parallelFor(nv, [&](size_t v) {
        if (open[v] || nb[v].empty() || (bandOnly && !bandV[v]) || ringV[v]) return;
        vec3 c(0);
        for (uint32_t u : nb[v]) c += low.positions[u];
        c /= float(nb[v].size());
        vec3 p = glm::mix(low.positions[v], c, 0.5f), q;
        if (lineV[v]) {
          onLine(p, q);
          next[v] = q;
          return;
        }
        next[v] = onSurface(p, v, q) ? q : p;
      });
      low.positions = std::move(next);
      normalsOf();
    }
  };
  if (!chains.empty()) relax(10, true);
  static const int layoutRelax = getenv("PATINA_LAYOUT_RELAX") ? atoi(getenv("PATINA_LAYOUT_RELAX")) : 4;
  relax(opt.layout ? layoutRelax : opt.relaxIterations, false);
  // faces turned over (against the surface under them, or against the faces round them: squeezed where
  // rows meet, or across a crease): their corners moved to the middle of the vertices round them, a few
  // times, kept while that leaves fewer of either kind and no more of the other
  {
    const size_t F = low.faceStart.size() - 1;
    std::vector<std::vector<uint32_t>> facesAt(nv);
    for (uint32_t fc = 0; fc < F; ++fc)
      for (uint32_t l = low.faceStart[fc]; l < low.faceStart[fc + 1]; ++l) facesAt[low.loopVert[l]].push_back(fc);
    auto turned = [&] {
      std::vector<vec3> fn(F);
      std::vector<char> bad(F, 0);
      parallelFor(F, [&](size_t fc) {
        uint32_t s0 = low.faceStart[fc], n = low.faceStart[fc + 1] - s0;
        const auto& X = low.positions;
        vec3 c(0);
        for (uint32_t k = 0; k < n; ++k) c += X[low.loopVert[s0 + k]] / float(n);
        vec3 f = n == 4 ? glm::cross(X[low.loopVert[s0 + 2]] - X[low.loopVert[s0]], X[low.loopVert[s0 + 3]] - X[low.loopVert[s0 + 1]])
                        : glm::cross(X[low.loopVert[s0 + 1]] - X[low.loopVert[s0]], X[low.loopVert[s0 + 2]] - X[low.loopVert[s0]]);
        float l = glm::length(f);
        fn[fc] = l > 0 ? f / l : vec3(0);
        RayHit h = bvh.closestPoint(high, c, reach);
        if (l > 0 && h.hit && glm::dot(fn[fc], h.normal) * facing < 0.f) bad[fc] = 1;
      });
      // (against the faces sharing an edge with it, on average)
      parallelFor(F, [&](size_t fc) {
        uint32_t s0 = low.faceStart[fc], n = low.faceStart[fc + 1] - s0;
        float sum = 0;
        int cnt = 0;
        for (uint32_t k = 0; k < n; ++k) {
          uint32_t a = low.loopVert[s0 + k], b = low.loopVert[s0 + (k + 1) % n];
          for (uint32_t g : facesAt[a])
            if (g != fc && std::find(facesAt[b].begin(), facesAt[b].end(), g) != facesAt[b].end()) sum += glm::dot(fn[fc], fn[g]), ++cnt;
        }
        if (cnt && sum < 0) bad[fc] |= 2;
      });
      return bad;
    };
    auto counts = [&](const std::vector<char>& bad) {
      std::array<int, 2> c = {0, 0};
      for (char b : bad) c[0] += (b & 1) != 0, c[1] += (b & 2) != 0;
      return c;
    };
    std::vector<char> bad = turned();
    const auto before = counts(bad);
    auto folded = before;
    for (int round = 0; round < 6 && folded[0] + folded[1] > 0; ++round) {
      // the corners of the faces turned over, and (each round) a ring of vertices further round them;
      // each piece of them moved, and kept only if its faces are better for it
      std::vector<char> mark(nv, 0);
      for (size_t fc = 0; fc < F; ++fc)
        if (bad[fc])
          for (uint32_t l = low.faceStart[fc]; l < low.faceStart[fc + 1]; ++l) mark[low.loopVert[l]] = 1;
      for (int r = 0; r < round; ++r) {
        std::vector<char> m2 = mark;
        for (size_t v = 0; v < nv; ++v)
          if (mark[v])
            for (uint32_t u : nb[v]) m2[u] = 1;
        mark = std::move(m2);
      }
      std::vector<int> piece(nv, -1);
      int pieces = 0;
      for (size_t v0 = 0; v0 < nv; ++v0) {
        if (!mark[v0] || piece[v0] >= 0) continue;
        std::vector<uint32_t> stack = {(uint32_t)v0};
        piece[v0] = pieces;
        while (!stack.empty()) {
          uint32_t v = stack.back();
          stack.pop_back();
          for (uint32_t u : nb[v])
            if (mark[u] && piece[u] < 0) piece[u] = pieces, stack.push_back(u);
        }
        ++pieces;
      }
      std::vector<vec3> keepPos = low.positions;
      for (int it = 0; it < 3; ++it) {
        std::vector<vec3> next = low.positions;
        parallelFor(nv, [&](size_t v) {
          if (!mark[v] || open[v] || nb[v].empty() || ringV[v]) return;
          vec3 c(0);
          for (uint32_t u : nb[v]) c += low.positions[u];
          c /= float(nb[v].size());
          vec3 q;
          if (lineV[v]) {
            onLine(c, q);
            next[v] = q;
            return;
          }
          next[v] = onSurface(c, v, q) ? q : c;
        });
        low.positions = std::move(next);
      }
      normalsOf();
      std::vector<char> bad2 = turned();
      // (each piece's faces: those with a corner in it)
      std::vector<std::array<int, 4>> tally(pieces, {0, 0, 0, 0});
      for (size_t fc = 0; fc < F; ++fc) {
        int pc = -1;
        for (uint32_t l = low.faceStart[fc]; l < low.faceStart[fc + 1] && pc < 0; ++l) pc = piece[low.loopVert[l]];
        if (pc < 0) continue;
        tally[pc][0] += (bad[fc] & 1) != 0, tally[pc][1] += (bad[fc] & 2) != 0;
        tally[pc][2] += (bad2[fc] & 1) != 0, tally[pc][3] += (bad2[fc] & 2) != 0;
      }
      int kept = 0;
      for (size_t v = 0; v < nv; ++v) {
        if (piece[v] < 0) continue;
        auto& t = tally[piece[v]];
        if (t[2] > t[0] || t[3] > t[1] || t[2] + t[3] >= t[0] + t[1]) low.positions[v] = keepPos[v];
      }
      for (auto& t : tally) kept += !(t[2] > t[0] || t[3] > t[1] || t[2] + t[3] >= t[0] + t[1]);
      normalsOf();
      if (!kept) continue;  // (a ring further next time)
      bad = turned();
      folded = counts(bad);
    }
    if (before[0] + before[1])
      PLOG_I("Retopology (flow): faces turned over against the surface / the faces round them: %d / %d, %d / %d after moving their corners",
             before[0], before[1], folded[0], folded[1]);
  }
  // the joining rows and patches: faces turned over there (against the surface under them)
  int bandFolded = 0, bandFaces = 0;
  if (!chains.empty()) {
    for (uint32_t fc = 0; fc + 1 < low.faceStart.size(); ++fc) {
      uint32_t s0 = low.faceStart[fc], n = low.faceStart[fc + 1] - s0;
      bool band = false;
      vec3 c(0);
      for (uint32_t k = 0; k < n; ++k) band |= bandV[low.loopVert[s0 + k]] != 0, c += low.positions[low.loopVert[s0 + k]] / float(n);
      if (!band) continue;
      ++bandFaces;
      const auto& X = low.positions;
      vec3 fn = n == 4 ? glm::cross(X[low.loopVert[s0 + 2]] - X[low.loopVert[s0]], X[low.loopVert[s0 + 3]] - X[low.loopVert[s0 + 1]])
                       : glm::cross(X[low.loopVert[s0 + 1]] - X[low.loopVert[s0]], X[low.loopVert[s0 + 2]] - X[low.loopVert[s0]]);
      RayHit h = bvh.closestPoint(high, c, reach);
      if (h.hit && glm::dot(glm::normalize(fn), h.normal) * facing < -0.3f) ++bandFolded;
    }
    PLOG_I("Retopology (flow): %d of the %d faces of the joining rows and patches turned over", bandFolded, bandFaces);
    // (with rings, many of them means the rows were squeezed where rings meet: tried again as for a
    // join that fails)
    if (anyRings && bandFolded > std::max(6, bandFaces / 20)) return retryRings("too many faces of the joining rows turned over");
  }
  auto& Q = res.quality;
  for (size_t v = 0; v < nv; ++v) {
    if (open[v] || nb[v].empty()) continue;
    ++Q.vertices;
    size_t k = nb[v].size();
    (k == 4 ? Q.regular : k == 3 ? Q.three : k == 5 ? Q.five : Q.other)++;
    if (k != 4) ++res.poles;
  }
  Q.faces = (int)low.faceStart.size() - 1;
  Q.quads = (int)Qd.size();
  res.quads = Q.quads;
  low.autoSmoothAngleDeg = 180.f;
  if (Q.faces > 0) low.finalize();
  res.quadPatch.assign(Q.faces, 1);
  if (dropped) res.problems.push_back(std::to_string(dropped) + " triangles of loose bits smaller than about four quads were left out");
  // parts too thin for quads this size (fingers, strands) can come out missing: where the working
  // surface is further than a quad (its size there) from every quad, it is marked and said (half a quad
  // also caught the bottom of deep creases, round an eyeball)
  if (Q.faces > 0) {
    Bvh lowBvh;
    lowBvh.build(low);
    const size_t step = std::max<size_t>(1, cm.pos.size() / 40000);
    std::vector<size_t> samples;
    for (size_t i = 0; i < cm.pos.size(); i += step) samples.push_back(i);
    // (the quads' own size where each point is)
    std::vector<float> vL(cm.pos.size(), 0.f), vRes(cm.pos.size(), 0.f);
    for (uint32_t t = 0; t < cm.tri.size(); ++t) {
      float L = L0 / std::sqrt(std::max(triDens[t] * triRes[t], 0.05f));
      for (int k = 0; k < 3; ++k) vL[cm.tri[t][k]] = std::max(vL[cm.tri[t][k]], L), vRes[cm.tri[t][k]] = std::max(vRes[cm.tri[t][k]], triRes[t]);
    }
    std::vector<char> far(samples.size(), 0);
    parallelFor(samples.size(), [&](size_t k) {
      RayHit h = lowBvh.closestPoint(low, cm.pos[samples[k]], vL[samples[k]] + (useShell ? 2.5f * cube : 0.f));
      far[k] = !h.hit;
    });
    size_t n = 0;
    for (size_t k = 0; k < samples.size(); ++k) n += far[k];
    double share = samples.empty() ? 0 : 100.0 * n / samples.size();
    if (share >= 1.0) {  // (less is the bottom of a crease or two, not a part left out)
      size_t inFine = 0;  // (left out though in a zone at x16 or more: strands of hair, say)
      for (size_t k = 0; k < samples.size(); ++k)
        if (far[k]) {
          inFine += vRes[samples[k]] >= 16.f;
          if (res.uncovered.size() < 4000) res.uncovered.push_back(cm.pos[samples[k]]);
        }
      char buf[320];
      if (inFine * 2 > n)
        std::snprintf(buf, sizeof buf,
                      "about %.1f%% of the surface (marked in red) is too thin even for its zone's quads (x16 or more), so it "
                      "is left out: strands of hair, say. Give that zone a higher resolution by hand (Zone tool), or leave it",
                      share);
      else
        std::snprintf(buf, sizeof buf,
                      "about %.1f%% of the surface (marked in red) is too thin for quads this size, so it is left out: "
                      "give those parts (fingers, strands) a zone with a higher resolution (Zones for thin parts does it)",
                      share);
      res.problems.push_back(buf);
    }
  }
  if (!smallZones.empty()) {
    res.problems.push_back(smallZones);
    for (auto& c : smallZoneMarks)
      if (RayHit h = bvh.closestPoint(high, c, 1e30f); h.hit) res.uncovered.push_back(h.position);
  }
  res.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  PLOG_I("Retopology (flow): %d quads, %zu triangles, %d poles (%.1f%% regular), %d vertices held by %zu lines (%d quad vertices on them), %.2f s",
         res.quads, Tr.size(), res.poles, Q.pct(Q.regular), heldTotal.load(), lines.size(), onLines, res.seconds);
  return res;
}

namespace {
// faces turned against the faces round them (the mean of the normals' dots below nothing)
int foldedFaces(const Mesh& m) {
  const uint32_t F = m.faceStart.empty() ? 0 : (uint32_t)m.faceStart.size() - 1;
  std::vector<vec3> fn(F);
  std::unordered_map<uint64_t, std::vector<uint32_t>> at;
  for (uint32_t f = 0; f < F; ++f) {
    const uint32_t s0 = m.faceStart[f], n = m.faceStart[f + 1] - s0;
    const auto& X = m.positions;
    const auto* lv = &m.loopVert[s0];
    vec3 nn = n == 4 ? glm::cross(X[lv[2]] - X[lv[0]], X[lv[3]] - X[lv[1]]) : glm::cross(X[lv[1]] - X[lv[0]], X[lv[2]] - X[lv[0]]);
    fn[f] = glm::length(nn) > 0 ? glm::normalize(nn) : nn;
    for (uint32_t k = 0; k < n; ++k) at[ekey(lv[k], lv[(k + 1) % n])].push_back(f);
  }
  std::vector<std::unordered_set<uint32_t>> nbf(F);
  for (auto& [e, fs] : at)
    for (uint32_t a : fs)
      for (uint32_t b : fs)
        if (a != b) nbf[a].insert(b);
  int folded = 0;
  for (uint32_t f = 0; f < F; ++f) {
    if (nbf[f].empty()) continue;
    float d = 0;
    for (uint32_t g : nbf[f]) d += glm::dot(fn[f], fn[g]);
    folded += d < 0;
  }
  return folded;
}
}  // namespace

// ---- symmetry (0.22.3)
namespace {
// the quality counted again (the quads mirrored): each inside vertex's edges, the poles and the faces
void countQuality(Result& res) {
  const Mesh& low = res.low;
  const uint32_t F = low.faceStart.empty() ? 0 : (uint32_t)low.faceStart.size() - 1;
  const size_t nv = low.positions.size();
  std::vector<std::vector<uint32_t>> nb(nv);
  std::unordered_map<EdgeKey, int> uses;
  int quads = 0;
  for (uint32_t f = 0; f < F; ++f) {
    const uint32_t s0 = low.faceStart[f], n = low.faceStart[f + 1] - s0;
    quads += n == 4;
    for (uint32_t k = 0; k < n; ++k) {
      uint32_t a = low.loopVert[s0 + k], b = low.loopVert[s0 + (k + 1) % n];
      if (std::find(nb[a].begin(), nb[a].end(), b) == nb[a].end()) nb[a].push_back(b), nb[b].push_back(a);
      uses[ekey(a, b)]++;
    }
  }
  std::vector<char> open(nv, 0);
  for (auto& [e, n] : uses)
    if (n == 1) open[uint32_t(e >> 32)] = open[uint32_t(e & 0xFFFFFFFF)] = 1;
  auto& Q = res.quality;
  Q = {};
  res.poles = 0;
  for (size_t v = 0; v < nv; ++v) {
    if (open[v] || nb[v].empty()) continue;
    ++Q.vertices;
    size_t k = nb[v].size();
    (k == 4 ? Q.regular : k == 3 ? Q.three : k == 5 ? Q.five : Q.other)++;
    if (k != 4) ++res.poles;
  }
  Q.faces = (int)F, Q.quads = quads;
  res.quads = quads;
}

// The quads on the kept side of the middle planes (side[k]: +1 or -1) kept and mirrored onto the other, sharing
// the vertices along the middle (put exactly on the planes); the mirror images put on the model's surface (a
// model is seldom exactly symmetric). Where a face reaches across the middle (the quads bridge a crease, or a
// zone too small for them there) it is cut along it, so the halves always meet. False (the result left as it
// was) when the halves differ too much.
bool mirrorHalf(Result& r, const Mesh& high, const Bvh& bvh, int axes, const vec3& c, const float side[3], std::string* why) {
  const Mesh& low = r.low;
  const uint32_t F = low.faceStart.empty() ? 0 : (uint32_t)low.faceStart.size() - 1;
  const size_t nv = low.positions.size();
  const auto& X = low.positions;
  // per vertex: the mean length of its edges (the quads' size there)
  std::vector<float> len(nv, 0.f);
  std::vector<int> cnt(nv, 0);
  std::vector<std::vector<uint32_t>> poly(F);
  for (uint32_t f = 0; f < F; ++f) {
    const uint32_t s0 = low.faceStart[f], n = low.faceStart[f + 1] - s0;
    for (uint32_t k = 0; k < n; ++k) {
      uint32_t a = low.loopVert[s0 + k], b = low.loopVert[s0 + (k + 1) % n];
      float l = glm::length(X[a] - X[b]);
      len[a] += l, cnt[a]++, len[b] += l, cnt[b]++;
      poly[f].push_back(a);
    }
  }
  for (size_t v = 0; v < nv; ++v)
    if (cnt[v]) len[v] /= float(cnt[v]);
  // the pieces (faces joined by their edges), each mirrored only when it is symmetric: nearly all its vertices'
  // mirror images (98%) within half a quad of the model. One that isn't (hair worn to one side) is kept as made;
  // one wholly on the far side that is goes (the image of its counterpart takes its place).
  std::vector<uint32_t> comp(F);
  {
    std::iota(comp.begin(), comp.end(), 0u);
    auto find = [&](uint32_t a) {
      while (comp[a] != a) a = comp[a] = comp[comp[a]];
      return a;
    };
    std::unordered_map<EdgeKey, uint32_t> faceOf;
    for (uint32_t f = 0; f < F; ++f)
      for (size_t i = 0; i < poly[f].size(); ++i) {
        auto [it, fresh] = faceOf.emplace(ekey(poly[f][i], poly[f][(i + 1) % poly[f].size()]), f);
        if (!fresh) comp[find(f)] = find(it->second);
      }
    for (uint32_t f = 0; f < F; ++f) comp[f] = find(f);
  }
  std::vector<char> lands(nv, 0);
  parallelFor(nv, [&](size_t v) {
    bool ok = true;
    for (int k = 0; k < 3 && ok; ++k)
      if (axes & (1 << k)) ok = bvh.closestPoint(high, mirrorPoint(X[v], 1 << k, c), 0.5f * len[v]).hit;
    lands[v] = ok;
  });
  std::unordered_map<uint32_t, std::array<size_t, 3>> stat;  // piece -> vertices, their images on the model, on the near side
  {
    std::vector<int64_t> vComp(nv, -1);
    for (uint32_t f = 0; f < F; ++f)
      for (uint32_t v : poly[f]) vComp[v] = comp[f];
    for (uint32_t v = 0; v < nv; ++v) {
      if (vComp[v] < 0) continue;
      auto& st = stat[(uint32_t)vComp[v]];
      bool near = true;
      for (int k = 0; k < 3; ++k)
        if ((axes & (1 << k)) && side[k] * (X[v][k] - c[k]) <= -0.34f * len[v]) near = false;
      st[0]++, st[1] += lands[v], st[2] += near;
    }
  }
  for (auto& [cc, st] : stat)
    if (st[0] >= 50) PLOG_D("Retopology (flow): mirror: a piece of %zu vertices, %zu with their images on the model, %zu on the near side", st[0], st[1], st[2]);
  std::vector<std::vector<uint32_t>> asIs;  // (faces of pieces kept as made)
  std::vector<vec3> unlike;                  // (where a piece kept as made is furthest from its image)
  {
    std::vector<std::vector<uint32_t>> sym;
    std::unordered_map<uint32_t, char> said;
    for (uint32_t f = 0; f < F; ++f) {
      const auto& st = stat[comp[f]];
      const bool symmetric = st[1] * 50 >= st[0] * 49;
      if (symmetric) {
        if (st[2]) sym.push_back(std::move(poly[f]));
        continue;  // (wholly on the far side: its counterpart's image takes its place)
      }
      asIs.push_back(std::move(poly[f]));
      if (!said[comp[f]]) {
        said[comp[f]] = 1;
        float worst = -1;
        vec3 at(0);
        for (uint32_t v : asIs.back())
          if (!lands[v]) {
            RayHit h = bvh.closestPoint(high, mirrorPoint(X[v], axes, c), 1e30f);
            float d = h.hit ? glm::length(h.position - mirrorPoint(X[v], axes, c)) : 1e30f;
            if (d > worst) worst = d, at = X[v];
          }
        if (worst >= 0) unlike.push_back(at);
      }
    }
    poly = std::move(sym);
  }
  if (poly.empty()) {
    // (where: some of the vertices whose images are off the model, spread out)
    float worst = 0;
    std::vector<uint32_t> off;
    for (uint32_t v = 0; v < nv; ++v)
      if (!lands[v] && cnt[v]) off.push_back(v);
    for (size_t i = 0; i < off.size(); i += std::max<size_t>(1, off.size() / 60)) {
      vec3 q = mirrorPoint(X[off[i]], axes, c);
      if (RayHit h = bvh.closestPoint(high, q, 1e30f); h.hit) worst = std::max(worst, glm::length(h.position - q));
      r.uncovered.push_back(X[off[i]]);
    }
    char buf[200];
    std::snprintf(buf, sizeof buf, "the model isn't the same on both sides (marked in red: up to %.1f cm apart)", worst * 100.f);
    if (why) *why = buf;
    return false;
  }
  // vertices within a third of a quad of a plane: on it (the middle loop; where the model's own open edge
  // crosses it); then those of faces still reaching across it, up to 0.6 of a quad off
  std::vector<vec3> P(X.begin(), X.end());
  std::vector<uint8_t> planes(nv, 0);
  auto snap = [&](uint32_t v, float within) {
    for (int k = 0; k < 3; ++k)
      if ((axes & (1 << k)) && std::fabs(P[v][k] - c[k]) < within * len[v]) planes[v] |= uint8_t(1 << k), P[v][k] = c[k];
  };
  {
    std::vector<char> in(nv, 0);
    for (auto& fv : poly)
      for (uint32_t v : fv) in[v] = 1;
    for (uint32_t v = 0; v < nv; ++v)
      if (in[v]) snap(v, 0.34f);
  }
  auto sd = [&](uint32_t v, int k) { return (planes[v] & (1 << k)) ? 0.f : side[k] * (P[v][k] - c[k]); };
  auto across = [&](const std::vector<uint32_t>& fv, int k) {
    bool pos = false, neg = false;
    for (uint32_t v : fv) pos |= sd(v, k) > 0, neg |= sd(v, k) < 0;
    return pos && neg;
  };
  for (int k = 0; k < 3; ++k)
    if (axes & (1 << k))
      for (auto& fv : poly)
        if (across(fv, k))
          for (uint32_t v : fv) snap(v, 0.6f);
  // each plane in turn: the faces on its far side left out, those across it cut along it
  size_t cutFaces = 0;
  std::vector<vec3> cutAt;
  for (int k = 0; k < 3; ++k) {
    if (!(axes & (1 << k))) continue;
    std::unordered_map<EdgeKey, uint32_t> cutVert;
    std::vector<std::vector<uint32_t>> out;
    size_t flat = 0;
    for (auto& fv : poly) {
      bool pos = false, neg = false;
      for (uint32_t v : fv) pos |= sd(v, k) > 0, neg |= sd(v, k) < 0;
      // (one flat on the plane, a crease's walls pressed onto it, goes: its image would be itself turned over;
      // the faces round it meet their images along its edges)
      if (!pos && !neg) {
        ++flat;
        continue;
      }
      if (!neg) {
        out.push_back(fv);
        continue;
      }
      if (!pos) continue;
      ++cutFaces;
      std::vector<uint32_t> part;
      vec3 mid(0);
      for (size_t i = 0; i < fv.size(); ++i) {
        uint32_t a = fv[i], b = fv[(i + 1) % fv.size()];
        float da = sd(a, k), db = sd(b, k);
        if (da >= 0) part.push_back(a);
        if ((da > 0 && db < 0) || (da < 0 && db > 0)) {
          auto [it, fresh] = cutVert.emplace(ekey(a, b), (uint32_t)P.size());
          if (fresh) {
            vec3 q = glm::mix(P[a], P[b], da / (da - db));
            q[k] = c[k];
            P.push_back(q), planes.push_back(uint8_t((planes[a] & planes[b]) | (1 << k))), len.push_back(0.5f * (len[a] + len[b]));
          }
          part.push_back(it->second);
        }
        mid += P[a] / float(fv.size());
      }
      cutAt.push_back(mid);
      part.erase(std::unique(part.begin(), part.end()), part.end());
      while (part.size() > 1 && part.front() == part.back()) part.pop_back();
      if (part.size() < 3) continue;
      // (as quads and triangles: five corners a quad and a triangle, six two quads)
      if (part.size() <= 4) out.push_back(part);
      else if (part.size() == 5) out.push_back({part[0], part[1], part[2], part[3]}), out.push_back({part[3], part[4], part[0]});
      else if (part.size() == 6) out.push_back({part[0], part[1], part[2], part[3]}), out.push_back({part[3], part[4], part[5], part[0]});
      else
        for (size_t i = 1; i + 1 < part.size(); ++i) out.push_back({part[0], part[i], part[i + 1]});
    }
    poly = std::move(out);
    if (flat) PLOG_D("Retopology (flow): %zu faces flat on the middle left out", flat);
  }
  for (size_t i = 0; i < cutAt.size() && i < 40; ++i)
    PLOG_D("Retopology (flow): a face across the middle cut at %.4f %.4f %.4f", cutAt[i].x, cutAt[i].y, cutAt[i].z);
  // the kept faces, then their mirror images across each plane in turn (vertices on that plane shared)
  std::vector<int64_t> remap(P.size(), -1);
  std::vector<vec3> V;
  std::vector<uint8_t> vPlanes;
  std::vector<float> vLen;
  std::vector<std::vector<uint32_t>> faces;
  size_t middle = 0;
  for (auto& fv0 : poly) {
    std::vector<uint32_t> fv;
    for (uint32_t v : fv0) {
      if (remap[v] < 0) remap[v] = (int64_t)V.size(), V.push_back(P[v]), vPlanes.push_back(planes[v]), vLen.push_back(len[v]), middle += planes[v] != 0;
      fv.push_back((uint32_t)remap[v]);
    }
    faces.push_back(std::move(fv));
  }
  if (faces.empty()) {
    if (why) *why = "there are no quads on the side kept";
    return false;
  }
  const size_t keptVerts = V.size();
  std::vector<char> copy(keptVerts, 0);
  for (int k = 0; k < 3; ++k) {
    if (!(axes & (1 << k))) continue;
    const size_t n0 = V.size(), f0 = faces.size();
    std::vector<uint32_t> img(n0);
    for (size_t v = 0; v < n0; ++v) {
      if (vPlanes[v] & (1 << k)) {
        img[v] = (uint32_t)v;
        continue;
      }
      img[v] = (uint32_t)V.size();
      vec3 q = V[v];
      q[k] = 2.f * c[k] - q[k];
      V.push_back(q), vPlanes.push_back(vPlanes[v]), vLen.push_back(vLen[v]), copy.push_back(1);
    }
    for (size_t f = 0; f < f0; ++f) {
      std::vector<uint32_t> fv;
      for (auto it = faces[f].rbegin(); it != faces[f].rend(); ++it) fv.push_back(img[*it]);
      faces.push_back(std::move(fv));
    }
  }
  // the mirror images on the surface; too many far from it: the halves differ
  std::vector<char> far(V.size(), 0);
  parallelFor(V.size(), [&](size_t v) {
    if (!copy[v]) return;
    RayHit h = bvh.closestPoint(high, V[v], 0.5f * vLen[v]);
    if (!h.hit) {
      far[v] = 1;
      return;
    }
    vec3 q = h.position;
    for (int k = 0; k < 3; ++k)
      if (vPlanes[v] & (1 << k)) q[k] = c[k];
    V[v] = q;
  });
  size_t copies = 0, farN = 0;
  float worst = 0;
  for (size_t v = 0; v < V.size(); ++v) {
    copies += copy[v];
    if (!far[v]) continue;
    ++farN;
    RayHit h = bvh.closestPoint(high, V[v], 1e30f);
    if (h.hit) worst = std::max(worst, glm::length(h.position - V[v]));
  }
  if (farN > std::max<size_t>(4, copies / 50)) {
    char buf[200];
    std::snprintf(buf, sizeof buf, "the model's two sides differ too much (by up to %.1f cm) for the quads to be mirrored", worst * 100.f);
    if (why) *why = buf;
    return false;
  }
  // the pieces kept as made, as they are
  {
    std::vector<int64_t> at(nv, -1);
    for (auto& fv0 : asIs) {
      std::vector<uint32_t> fv;
      for (uint32_t v : fv0) {
        if (at[v] < 0) at[v] = (int64_t)V.size(), V.push_back(X[v]), vPlanes.push_back(0), vLen.push_back(len[v]), copy.push_back(0);
        fv.push_back((uint32_t)at[v]);
      }
      faces.push_back(std::move(fv));
    }
  }
  // the result: the mirrored quads
  Mesh out;
  out.name = low.name;
  out.positions = std::move(V);
  out.faceStart.push_back(0);
  for (int pass = 0; pass < 2; ++pass)  // (quads first, as built)
    for (auto& fv : faces) {
      if ((fv.size() == 4) != (pass == 0)) continue;
      for (uint32_t v : fv) out.loopVert.push_back(v);
      out.faceStart.push_back((uint32_t)out.loopVert.size());
    }
  out.autoSmoothAngleDeg = 180.f;
  PLOG_I("Retopology (flow): the quads mirrored: %zu vertices kept, %zu along the middle, %zu faces across it cut, %zu of %zu mirror images off the surface, "
         "%zu pieces kept as made (%zu faces)",
         keptVerts, middle, cutFaces, farN, copies, unlike.size(), asIs.size());
  r.low = std::move(out);
  if (r.low.faceCount() > 0) r.low.finalize();
  r.quadPatch.assign(r.low.faceCount(), 1);
  // the marks (parts left out, patched spots): the kept side's, and their mirror images
  std::vector<vec3> marks;
  for (const vec3& p : r.uncovered) {
    bool keep = true;
    for (int k = 0; k < 3; ++k)
      if ((axes & (1 << k)) && side[k] * (p[k] - c[k]) < 0) keep = false;
    if (!keep) continue;
    marks.push_back(p);
    for (int sub = 1; sub < 8; ++sub)
      if ((sub & axes) == sub) marks.push_back(mirrorPoint(p, sub, c));
  }
  for (const vec3& p : unlike) marks.push_back(p);
  r.uncovered = std::move(marks);
  if (!unlike.empty())
    r.problems.push_back("Mirror: " + std::string(unlike.size() == 1 ? "a piece of the model isn't" : std::to_string(unlike.size()) + " pieces of the model aren't") +
                         " the same on both sides (marked in red), so " + (unlike.size() == 1 ? "its" : "their") +
                         " quads are made as they are, not mirrored");
  countQuality(r);
  return true;
}
}  // namespace

// Mirror X/Y/Z: the model's middle (where each plane cuts its surface) laid as edge loops, so the quads meet it
// along an edge loop; then the quads on one side are kept and mirrored onto the other. The side kept is the one
// with more of the lines (the model's bigger side when there are none).
static Result flowQuadsMirrored(const Mesh& high, const Bvh& bvh, const std::vector<Stroke>& strokes, const std::vector<int32_t>& faceLabel,
                                const std::vector<float>& vertexDensity, const FlowOptions& opt, const std::vector<float>& faceResolution) {
  const int axes = opt.mirrorAxes & 7;
  const vec3 c = opt.mirrorCenter;
  const float L0 = 1.f / std::sqrt(std::max(opt.quadsPerM2, 1e-3f));
  const float onPlane = 1e-4f * L0;
  float side[3] = {1, 1, 1};
  for (int k = 0; k < 3; ++k) {
    if (!(axes & (1 << k))) continue;
    double pos = 0, neg = 0;
    for (auto& s : strokes)
      for (size_t i = 0; i + 1 < s.points.size(); ++i) {
        vec3 m = 0.5f * (s.points[i] + s.points[i + 1]);
        double l = glm::length(s.points[i + 1] - s.points[i]);
        (m[k] > c[k] ? pos : neg) += m[k] == c[k] ? 0 : l;
      }
    if (pos + neg <= 0) {
      for (uint32_t f = 0; f < high.faceCount(); ++f) (high.faceCenter(f)[k] > c[k] ? pos : neg) += high.faceArea3D(f);
    }
    if (neg > 1.05 * pos) side[k] = -1;
  }
  // the quads' size at a point (the zones' resolution and the density brush there)
  auto sizeAt = [&](const vec3& p) {
    RayHit h = bvh.closestPoint(high, p, 1e30f);
    if (!h.hit) return L0;
    float r = faceResolution.size() == high.faceCount() ? std::clamp(faceResolution[h.face], 0.05f, 50.f) : 1.f;
    if (vertexDensity.size() == high.positions.size()) r *= std::clamp(vertexDensity[high.loopVert[high.faceStart[h.face]]], 0.05f, 50.f);
    return L0 / std::sqrt(std::max(r, 1.f));
  };
  // (first: is any big piece of the model the same on both sides? when none is, the quads are made on the whole
  // model straight away, not made with the middle's loops and then again; its vertices sampled, each image
  // within half a quad of the model or not)
  const uint32_t nv = (uint32_t)high.positions.size(), nf = high.faceCount();
  std::vector<uint32_t> pieceOf(nv);
  std::iota(pieceOf.begin(), pieceOf.end(), 0u);
  auto find = [&](uint32_t a) {
    while (pieceOf[a] != a) a = pieceOf[a] = pieceOf[pieceOf[a]];
    return a;
  };
  std::unordered_map<uint32_t, std::array<size_t, 2>> per;  // per piece: points sampled, with their images on the model
  {
    std::vector<float> vRes(nv, 1.f);
    for (uint32_t f = 0; f < nf; ++f) {
      const uint32_t s0 = high.faceStart[f], s1 = high.faceStart[f + 1];
      const float fr = faceResolution.size() == nf ? std::clamp(faceResolution[f], 0.05f, 50.f) : 1.f;
      for (uint32_t l = s0; l < s1; ++l) {
        vRes[high.loopVert[l]] = std::max(vRes[high.loopVert[l]], fr);
        if (l > s0) pieceOf[find(high.loopVert[l])] = find(high.loopVert[s0]);
      }
    }
    const uint32_t step = std::max<uint32_t>(1, nv / 40000);
    std::vector<uint32_t> smp;
    for (uint32_t v = 0; v < nv; v += step) smp.push_back(v);
    std::vector<char> ok(smp.size(), 0);
    parallelFor(smp.size(), [&](size_t i) {
      const uint32_t v = smp[i];
      float r = vRes[v] * (vertexDensity.size() == nv ? std::clamp(vertexDensity[v], 0.05f, 50.f) : 1.f);
      const float L = L0 / std::sqrt(std::max(r, 1.f));
      bool hit = true;
      for (int k = 0; k < 3 && hit; ++k)
        if (axes & (1 << k)) hit = bvh.closestPoint(high, mirrorPoint(high.positions[v], 1 << k, c), 0.5f * L).hit;
      ok[i] = hit;
    });
    for (size_t i = 0; i < smp.size(); ++i) {
      auto& pc = per[find(smp[i])];
      pc[0]++, pc[1] += ok[i];
    }
    size_t symmetric = 0;
    for (auto& [pc, n] : per)
      if (n[1] * 50 >= n[0] * 49) symmetric += n[0];
    if (symmetric * 5 < smp.size()) {
      FlowOptions w = opt;
      w.mirrorAxes = 0;
      Result r = flowQuads(high, bvh, strokes, faceLabel, vertexDensity, w, faceResolution);
      if (r.cancelled) return r;
      float worst = 0;
      std::vector<size_t> off;
      for (size_t i = 0; i < smp.size(); ++i)
        if (!ok[i]) off.push_back(i);
      for (size_t i = 0; i < off.size(); i += std::max<size_t>(1, off.size() / 60)) {
        const vec3& p = high.positions[smp[off[i]]];
        vec3 q = mirrorPoint(p, axes, c);
        if (RayHit h = bvh.closestPoint(high, q, 1e30f); h.hit) worst = std::max(worst, glm::length(h.position - q));
        r.uncovered.push_back(p);
      }
      char buf[260];
      std::snprintf(buf, sizeof buf,
                    "Mirror: the model isn't the same on both sides (marked in red: up to %.1f cm apart), so the quads are made on the whole "
                    "model, not mirrored",
                    worst * 100.f);
      r.problems.push_back(buf);
      PLOG_I("Retopology (flow): not mirrored: %zu of %zu points of the model have their images on it", smp.size() - off.size(), smp.size());
      return r;
    }
  }
  // a point on a piece of the model that isn't the same on both sides (its quads are kept as made, so no loop is
  // laid down its middle)
  auto lopsided = [&](const vec3& p) {
    RayHit h = bvh.closestPoint(high, p, 1e30f);
    if (!h.hit) return false;
    auto it = per.find(find(high.loopVert[high.faceStart[h.face]]));
    return it != per.end() && it->second[1] * 50 < it->second[0] * 49;
  };
  // the middle: where each plane cuts the surface, as edge lines (points about a fifth of a quad apart; pieces
  // shorter than two of their quads left out: a strand of hair across the middle; and pieces mostly on parts not
  // the same on both sides: hair worn to one side)
  std::vector<Stroke> mids;
  std::vector<int> midAxis;
  std::vector<vec3> noRingsAt;
  for (int k = 0; k < 3; ++k) {
    if (!(axes & (1 << k))) continue;
    for (auto& piece : middleSection(high, k, c[k])) {
      double length = 0;
      for (size_t i = 0; i + 1 < piece.points.size(); ++i) length += glm::length(piece.points[i + 1] - piece.points[i]);
      if (piece.closed && piece.points.size() > 1) length += glm::length(piece.points.front() - piece.points.back());
      float L = L0;
      int samples = 0, off = 0;
      for (size_t i = 0; i < piece.points.size(); i += std::max<size_t>(1, piece.points.size() / 8))
        L = std::min(L, sizeAt(piece.points[i])), ++samples, off += lopsided(piece.points[i]);
      if (length < 2.0 * L) continue;
      if (2 * off > samples) {
        PLOG_D("Retopology (flow): mirror: a piece of the middle %.3f long at (%.3f %.3f %.3f) not laid (on a part not the same on both sides)",
               length, piece.points[0].x, piece.points[0].y, piece.points[0].z);
        continue;
      }
      Stroke st;
      st.edge = true, st.loop = piece.closed;
      for (const vec3& p : piece.points)
        if (st.points.empty() || glm::length(p - st.points.back()) >= 0.2f * L) st.points.push_back(p);
      if (!piece.closed && st.points.back() != piece.points.back()) st.points.push_back(piece.points.back());
      if (st.points.size() < 2) continue;
      // (the zones either side of it are made as plain zones, not rings or discs closed with a grid: the
      // middle is no border of the user's, and a ball cut in two looks like two discs)
      for (size_t i = 0; i < st.points.size(); i += 2) {
        vec3 off(0);
        off[k] = 0.3f * L;
        noRingsAt.push_back(st.points[i] + off), noRingsAt.push_back(st.points[i] - off);
      }
      mids.push_back(std::move(st)), midAxis.push_back(k);
    }
  }
  // the lines drawn: where one comes within half a quad of the middle, it is moved onto the middle's loop (the
  // nearest point of it), and so is the rest of that run of its points within a quad of the middle (the line then
  // leaves the middle across it, not at a slant that needs a pole there, and no sliver is left between them); its
  // pieces along the middle are taken out (the middle's loop runs there: a strip narrower than a quad between
  // them would have the quads cross the middle)
  std::vector<Stroke> lines;
  auto onMiddle = [&](vec3& p, int k, float reach) {
    const float L = std::min(L0, sizeAt(p));
    if (std::fabs(p[k] - c[k]) <= onPlane || std::fabs(p[k] - c[k]) >= reach * L) return;
    float bd = (reach + 0.25f) * L * (reach + 0.25f) * L;
    vec3 best = p;
    for (size_t m = 0; m < mids.size(); ++m) {
      if (midAxis[m] != k) continue;
      const auto& Q = mids[m].points;
      const size_t ns = mids[m].loop ? Q.size() : Q.size() - 1;
      for (size_t i = 0; i < ns; ++i) {
        vec3 a = Q[i], ab = Q[(i + 1) % Q.size()] - a;
        float l2 = glm::dot(ab, ab), t = l2 > 0 ? std::clamp(glm::dot(p - a, ab) / l2, 0.f, 1.f) : 0.f;
        vec3 q = a + t * ab;
        if (float d = glm::dot(q - p, q - p); d < bd) bd = d, best = q;
      }
    }
    if (best != p) best[k] = c[k], p = best;
  };
  auto toMiddle = [&](Stroke& s) {
    const size_t n = s.points.size();
    for (int k = 0; k < 3; ++k) {
      if (!(axes & (1 << k)) || n == 0) continue;
      std::vector<char> near(n), within(n);
      for (size_t i = 0; i < n; ++i) {
        const float d = std::fabs(s.points[i][k] - c[k]), L = std::min(L0, sizeAt(s.points[i]));
        near[i] = d < 0.5f * L, within[i] = d < L;
      }
      std::vector<char> snap = near;
      for (size_t j = 1; j < (s.loop ? 2 * n : n); ++j) {  // (forwards, then backwards; a loop's twice round)
        size_t i = j % n, b = (j - 1) % n;
        if (snap[b] && within[i]) snap[i] = 1;
        i = n - 1 - j % n, b = n - 1 - (j - 1) % n;
        if (snap[b] && within[i]) snap[i] = 1;
      }
      for (size_t i = 0; i < n; ++i)
        if (snap[i]) onMiddle(s.points[i], k, near[i] ? 0.5f : 1.f);
    }
  };
  auto along = [&](const vec3& a, const vec3& b) {
    for (int k = 0; k < 3; ++k)
      if ((axes & (1 << k)) && std::fabs(a[k] - c[k]) <= onPlane && std::fabs(b[k] - c[k]) <= onPlane) return true;
    return false;
  };
  for (const auto& s0 : strokes) {
    Stroke s = s0;
    if (s.edge) toMiddle(s);
    const size_t n = s.points.size();
    const size_t segs = s.loop ? n : (n ? n - 1 : 0);
    std::vector<char> drop(segs, 0);
    bool any = false;
    for (size_t i = 0; i < segs; ++i) any |= drop[i] = s.edge && along(s.points[i], s.points[(i + 1) % n]);
    if (!any) {
      lines.push_back(s);
      continue;
    }
    // the runs between the pieces taken out (a loop's from just after one of them)
    size_t start = 0;
    if (s.loop)
      while (!drop[start]) ++start;
    Stroke run = s;
    run.loop = false, run.points.clear();
    for (size_t j = 0; j < segs; ++j) {
      size_t i = s.loop ? (start + 1 + j) % segs : j;
      if (drop[i]) {
        if (run.points.size() >= 2) lines.push_back(run);
        run.points.clear();
        continue;
      }
      if (run.points.empty()) run.points.push_back(s.points[i]);
      run.points.push_back(s.points[(i + 1) % n]);
    }
    if (run.points.size() >= 2) lines.push_back(run);
  }
  const int added = (int)mids.size();
  for (auto& m : mids) lines.push_back(std::move(m));
  PLOG_I("Retopology (flow): mirrored on %s%s%s: %d pieces of the middle laid as lines, the %s side kept", axes & 1 ? "x" : "", axes & 2 ? "y" : "",
         axes & 4 ? "z" : "", added, side[0] > 0 && side[1] > 0 && side[2] > 0 ? "positive" : "negative");
  FlowOptions o = opt;
  o.mirrored = true;
  o.noRingsAt.insert(o.noRingsAt.end(), noRingsAt.begin(), noRingsAt.end());
  Result r = flowQuads(high, bvh, lines, faceLabel, vertexDensity, o, faceResolution);
  if (r.cancelled || r.low.faceCount() == 0 || r.unchanged) return r;
  std::string why;
  const size_t marksBefore = r.uncovered.size();
  if (!mirrorHalf(r, high, bvh, axes, c, side, &why)) {
    // (made again as without Mirror: the middle's loops are no use then; the marks of where the sides differ kept)
    PLOG_I("Retopology (flow): not mirrored: %s", why.c_str());
    std::vector<vec3> marks(r.uncovered.begin() + std::min(marksBefore, r.uncovered.size()), r.uncovered.end());
    FlowOptions w = opt;
    w.mirrorAxes = 0;
    Result whole = flowQuads(high, bvh, strokes, faceLabel, vertexDensity, w, faceResolution);
    if (whole.cancelled || whole.low.faceCount() == 0) return whole;
    r = std::move(whole);
    r.uncovered.insert(r.uncovered.end(), marks.begin(), marks.end());
    r.problems.push_back("Mirror: " + why + ", so the quads are made on the whole model, not mirrored");
  }
  return r;
}

// Fewer poles: the quads made with their counts solved again; when that was kept, made again without it
// (the working surface and the zones' quads come from the cache, so it is quick) and the one with fewer
// poles kept, unless it has more folded faces, fewer quads by a tenth, or leaves more of the surface out
// the whole model's quads (with Fewer poles: made twice, the better kept)
static Result flowQuadsWhole(const Mesh& high, const Bvh& bvh, const std::vector<Stroke>& strokes, const std::vector<int32_t>& faceLabel,
                             const std::vector<float>& vertexDensity, const FlowOptions& opt, const std::vector<float>& faceResolution) {
  if (!opt.requantize || opt.layout) return flowQuadsOnce(high, bvh, strokes, faceLabel, vertexDensity, opt, faceResolution);
  auto t0 = std::chrono::steady_clock::now();
  FlowOptions o = opt;
  if (!o.cache) o.cache = makeFlowCache();
  Result b = flowQuadsOnce(high, bvh, strokes, faceLabel, vertexDensity, o, faceResolution);
  if (!b.resolved || b.cancelled) return b;
  o.requantize = false;
  Result a = flowQuadsOnce(high, bvh, strokes, faceLabel, vertexDensity, o, faceResolution);
  if (a.cancelled) return a;
  const int fa = foldedFaces(a.low), fb = foldedFaces(b.low);
  const size_t ua = a.uncovered.size(), ub = b.uncovered.size();
  const bool keep = b.poles < a.poles && fb <= fa && b.quads * 10 >= a.quads * 9 && ub <= ua + std::max<size_t>(8, ua / 10);
  PLOG_I("Retopology (flow): fewer poles %s: %d poles (were %d), %d folded (were %d), %d quads (were %d), %zu points left out (were %zu)",
         keep ? "kept" : "not kept", b.poles, a.poles, fb, fa, b.quads, a.quads, ub, ua);
  Result& r = keep ? b : a;
  r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  return std::move(r);
}

// (0.23) only the quads near what changed since the last ones made again; done: false when that couldn't be
// (other settings, too much changed, the new quads not joining): the caller makes them all
static Result flowQuadsLocal(const Mesh& high, const Bvh& bvh, const std::vector<Stroke>& strokes, const std::vector<int32_t>& faceLabel,
                             const std::vector<float>& vertexDensity, const FlowOptions& opt, const std::vector<float>& faceResolution,
                             const FlowInputs& now, bool& done) {
  done = false;
  const Result& prev = *opt.previous;
  const FlowInputs& was = *prev.builtFrom;
  if (was.quadsPerM2 != now.quadsPerM2 || was.loopsAtBorders != now.loopsAtBorders || was.ringZones != now.ringZones ||
      was.requantize != now.requantize || was.layout != now.layout || was.mirrored != now.mirrored || was.mirrorAxes != now.mirrorAxes ||
      was.mirrorCenter != now.mirrorCenter || was.shell != now.shell || was.highFaces != now.highFaces || was.highVertices != now.highVertices ||
      prev.low.faceCount() == 0)
    return {};
  auto t0 = std::chrono::steady_clock::now();
  const float L0 = 1.f / std::sqrt(std::max(opt.quadsPerM2, 1e-3f));
  // what changed: the lines and strokes drawn or taken away, the painted density, zones' resolutions, patches
  LocalChange ch;
  {
    std::vector<char> usedOld(was.strokes.size(), 0);
    auto same = [](const Stroke& a, const Stroke& b) { return a.edge == b.edge && a.loop == b.loop && a.points == b.points; };
    auto add = [&](const Stroke& st) {
      std::vector<vec3> pts = st.points;
      bool closed = st.loop || (pts.size() > 3 && glm::length(pts.front() - pts.back()) < 0.5f * L0);
      if (st.loop && !pts.empty()) pts.push_back(pts.front());
      ch.lines.push_back(pts);
      if (closed) ch.loops.push_back(st.points);
    };
    for (auto& st : strokes) {
      bool found = false;
      for (size_t k = 0; k < was.strokes.size() && !found; ++k)
        if (!usedOld[k] && same(st, was.strokes[k])) usedOld[k] = 1, found = true;
      if (!found) add(st);
    }
    for (size_t k = 0; k < was.strokes.size(); ++k)
      if (!usedOld[k]) add(was.strokes[k]);
    auto diffPerVertex = [&](const std::vector<float>& a, const std::vector<float>& b) {
      for (size_t v = 0; v < high.positions.size(); ++v) {
        float x = v < a.size() ? a[v] : 1.f, y = v < b.size() ? b[v] : 1.f;
        if (std::abs(x - y) > 1e-4f) ch.points.push_back(high.positions[v]);
      }
    };
    auto center = [&](uint32_t f) {
      vec3 c(0);
      uint32_t n = high.faceStart[f + 1] - high.faceStart[f];
      for (uint32_t l = high.faceStart[f]; l < high.faceStart[f + 1]; ++l) c += high.positions[high.loopVert[l]] / float(n);
      return c;
    };
    if (was.density != now.density) diffPerVertex(was.density, now.density);
    if (was.faceResolution != now.faceResolution)
      for (uint32_t f = 0; f < high.faceCount(); ++f) {
        float x = f < was.faceResolution.size() ? was.faceResolution[f] : 1.f, y = f < now.faceResolution.size() ? now.faceResolution[f] : 1.f;
        if (std::abs(x - y) > 1e-4f) ch.points.push_back(center(f));
      }
    if (was.faceLabel != now.faceLabel)
      for (uint32_t f = 0; f < high.faceCount(); ++f) {
        int x = f < was.faceLabel.size() ? was.faceLabel[f] : 0, y = f < now.faceLabel.size() ? now.faceLabel[f] : 0;
        if (x != y) ch.points.push_back(center(f));
      }
  }
  if (ch.lines.empty() && ch.points.empty()) {
    Result r = prev;  // (nothing changed: the quads as they were)
    r.localKept = (int)r.low.faceCount(), r.unchanged = true;
    done = true;
    return r;
  }
  // (a change over much of the model: all made again)
  if (ch.points.size() > high.positions.size() / 3) return {};
  FlowOptions o = opt;
  o.previous.reset();
  Result fresh = flowQuadsWhole(high, bvh, strokes, faceLabel, vertexDensity, o, faceResolution);
  if (fresh.cancelled || fresh.low.faceCount() == 0) {
    done = true;
    return fresh;
  }
  std::vector<std::vector<vec3>> edgeLines;
  for (auto& st : strokes)
    if (st.edge) {
      edgeLines.push_back(st.points);
      if (st.loop && !st.points.empty()) edgeLines.back().push_back(st.points.front());
    }
  if (opt.loopsAtBorders)
    for (auto& l : borderLines(high, faceLabel)) edgeLines.push_back(std::move(l));
  auto snap = [&](const vec3& p) {
    RayHit h = bvh.closestPoint(high, p, 2 * L0);
    return h.hit ? h.position : p;
  };
  // (the row tried at a few distances from the change: the one with the fewest poles kept)
  Mesh joined;
  LocalStats st;
  std::vector<char> nearRow;
  float reachOf = 0;
  std::string whyNot;
  for (float k : {0.f, 1.f, 2.f, -0.75f}) {
    const float rr = std::max(1.5f, opt.localReach + k) * L0;
    Mesh m;
    LocalStats s2;
    std::vector<char> nr;
    if (!keepAwayFromChange(prev.low, fresh.low, ch, L0, rr, edgeLines, snap, m, s2, &nr)) {
      if (whyNot.empty()) whyNot = s2.why;
      continue;
    }
    if (reachOf == 0 || s2.rowPoles < st.rowPoles) joined = std::move(m), st = s2, nearRow = std::move(nr), reachOf = rr;
  }
  if (reachOf == 0) {
    PLOG_I("Retopology (flow): all the quads made again (%s)", whyNot.c_str());
    done = true;
    return fresh;
  }
  // pairs of poles along the row taken out (as where zones join); the quads further off, and the vertices on
  // lines, kept as they are
  int cleaned = 0;
  {
    std::vector<vec3> pos = joined.positions;
    std::vector<std::array<uint32_t, 4>> qs;
    std::vector<std::array<uint32_t, 3>> ts;
    for (uint32_t f = 0; f < joined.faceCount(); ++f) {
      const uint32_t s0 = joined.faceStart[f], n = joined.faceStart[f + 1] - s0;
      if (n == 4) qs.push_back({joined.loopVert[s0], joined.loopVert[s0 + 1], joined.loopVert[s0 + 2], joined.loopVert[s0 + 3]});
      else if (n == 3) ts.push_back({joined.loopVert[s0], joined.loopVert[s0 + 1], joined.loopVert[s0 + 2]});
    }
    std::vector<char> protect(pos.size(), 1);
    std::vector<std::vector<vec3>> segsOf;
    for (size_t v = 0; v < pos.size(); ++v) protect[v] = !nearRow[v];
    // (and the vertices on lines)
    for (auto& l : edgeLines)
      for (size_t i = 0; i + 1 < l.size(); ++i)
        for (size_t v = 0; v < pos.size(); ++v)
          if (nearRow[v] && !protect[v] && segDist2(pos[v], l[i], l[i + 1]) < 0.0025f * L0 * L0) protect[v] = 1;
    std::vector<int> qg(qs.size(), 0);
    cleaned = cleanJoinPoles(pos, qs, qg, ts, protect);
    if (cleaned) {
      // (the vertices no longer used out)
      std::vector<int64_t> keepV(pos.size(), -1);
      Mesh m;
      auto use = [&](uint32_t v) {
        if (keepV[v] < 0) keepV[v] = (int64_t)m.positions.size(), m.positions.push_back(pos[v]);
        return (uint32_t)keepV[v];
      };
      m.faceStart.push_back(0);
      for (auto& q : qs) {
        for (uint32_t v : q) m.loopVert.push_back(use(v));
        m.faceStart.push_back((uint32_t)m.loopVert.size());
      }
      for (auto& t : ts) {
        for (uint32_t v : t) m.loopVert.push_back(use(v));
        m.faceStart.push_back((uint32_t)m.loopVert.size());
      }
      m.autoSmoothAngleDeg = 180.f;
      m.finalize();
      joined = std::move(m);
    }
  }
  // (Fewer poles: the joined quads' counts solved again from their layout, each run's count kept as it is
  // but pairs of poles along the row drawn together to cancel)
  int resolvedPoles = -1;
  if (opt.requantize && !getenv("PATINA_LOCAL_NOREQUANT")) {
    std::vector<vec3> pos = joined.positions;
    std::vector<std::array<uint32_t, 4>> qs;
    std::vector<std::array<uint32_t, 3>> ts;
    std::vector<float> vL(pos.size(), 0.f);
    std::vector<int> vn(pos.size(), 0);
    for (uint32_t f = 0; f < joined.faceCount(); ++f) {
      const uint32_t s0 = joined.faceStart[f], n = joined.faceStart[f + 1] - s0;
      if (n == 4) qs.push_back({joined.loopVert[s0], joined.loopVert[s0 + 1], joined.loopVert[s0 + 2], joined.loopVert[s0 + 3]});
      else if (n == 3) ts.push_back({joined.loopVert[s0], joined.loopVert[s0 + 1], joined.loopVert[s0 + 2]});
      for (uint32_t k = 0; k < n; ++k) {
        uint32_t a = joined.loopVert[s0 + k], b = joined.loopVert[s0 + (k + 1) % n];
        float l = glm::length(pos[a] - pos[b]);
        vL[a] += l, vn[a]++, vL[b] += l, vn[b]++;
      }
    }
    for (size_t v = 0; v < pos.size(); ++v) vL[v] = vn[v] ? vL[v] / vn[v] : L0;
    std::vector<char> isEdge(edgeLines.size(), 1);
    QuadLayout ql = layoutFromQuads(pos, qs, ts, vL, edgeLines, isEdge);
    LayoutQuadsOptions lo;
    lo.polePull = 6.f, lo.smoothLengths = false, lo.keepLeftRuns = true, lo.capHoles = true, lo.keepCounts = true;
    // (the runs through the kept quads keep their counts: a strip of quads taken out or put in to cancel two
    // poles stays by the row)
    lo.hold.assign(pos.size(), 0);
    for (size_t v = 0; v < pos.size() && v < nearRow.size(); ++v) lo.hold[v] = !nearRow[v];
    LayoutQuads lq = quadsFromLayout(ql, lo);
    if (!lq.quads.empty() && lq.patchesLeft <= (int)ts.size() + 2) {
      Mesh m;
      m.positions = lq.pos;
      m.faceStart.push_back(0);
      for (auto& q : lq.quads) m.loopVert.insert(m.loopVert.end(), q.begin(), q.end()), m.faceStart.push_back((uint32_t)m.loopVert.size());
      for (auto& t : lq.tris) m.loopVert.insert(m.loopVert.end(), t.begin(), t.end()), m.faceStart.push_back((uint32_t)m.loopVert.size());
      m.autoSmoothAngleDeg = 180.f;
      m.finalize();
      Result a, b;
      a.low = joined, b.low = m;
      countQuality(a), countQuality(b);
      const int fa = foldedFaces(a.low), fb = foldedFaces(b.low);
      PLOG_I("Retopology (flow): the joined quads solved again: %d poles (were %d), %d folded (were %d)%s", b.poles, a.poles, fb, fa,
             b.poles < a.poles && fb <= fa ? "" : ": not kept");
      if (b.poles < a.poles && fb <= fa) {
        // (on the surface again; where the quads were kept, each vertex exactly where it was)
        const float cell = 0.5f * L0;
        auto key = [&](const vec3& p3, int dx, int dy, int dz) {
          int64_t x = (int64_t)std::floor(p3.x / cell) + dx, y = (int64_t)std::floor(p3.y / cell) + dy, z = (int64_t)std::floor(p3.z / cell) + dz;
          return (uint64_t(x & 0x1FFFFF) << 42) | (uint64_t(y & 0x1FFFFF) << 21) | uint64_t(z & 0x1FFFFF);
        };
        std::unordered_map<uint64_t, std::vector<uint32_t>> g;
        for (uint32_t v = 0; v < pos.size(); ++v)
          if (lo.hold[v]) g[key(pos[v], 0, 0, 0)].push_back(v);
        int restored = 0;
        for (auto& p : m.positions) {
          float bd = 0.35f * L0 * 0.35f * L0;
          int bi = -1;
          for (int dz = -1; dz <= 1; ++dz)
            for (int dy = -1; dy <= 1; ++dy)
              for (int dx = -1; dx <= 1; ++dx) {
                auto it = g.find(key(p, dx, dy, dz));
                if (it == g.end()) continue;
                for (uint32_t v : it->second)
                  if (float d = glm::dot(pos[v] - p, pos[v] - p); d < bd) bd = d, bi = (int)v;
              }
          if (bi >= 0) p = pos[bi], ++restored;
          else p = snap(p);
        }
        PLOG_D("Retopology (flow): %d vertices of the kept quads put back where they were", restored);
        joined = std::move(m), resolvedPoles = b.poles;
      }
    }
  }
  Result r = std::move(fresh);
  r.low = std::move(joined);
  countQuality(r);
  r.quadPatch.assign(r.low.faceCount(), 1);
  r.localKept = st.keptFaces;
  // the points left out: the new build's near the change, the last one's elsewhere
  {
    std::vector<vec3> unc, pts;
    for (auto& l : ch.lines) pts.insert(pts.end(), l.begin(), l.end());
    pts.insert(pts.end(), ch.points.begin(), ch.points.end());
    auto cellOf = [&](const vec3& p) { return glm::ivec3(glm::floor(p / reachOf)); };
    auto key = [](glm::ivec3 c) { return (uint64_t(c.x & 0x1FFFFF) << 42) | (uint64_t(c.y & 0x1FFFFF) << 21) | uint64_t(c.z & 0x1FFFFF); };
    std::unordered_map<uint64_t, std::vector<uint32_t>> grid;
    for (uint32_t i = 0; i < pts.size(); ++i) grid[key(cellOf(pts[i]))].push_back(i);
    auto isNear = [&](const vec3& p) {
      glm::ivec3 c = cellOf(p);
      for (int dz = -1; dz <= 1; ++dz)
        for (int dy = -1; dy <= 1; ++dy)
          for (int dx = -1; dx <= 1; ++dx) {
            auto it = grid.find(key(c + glm::ivec3(dx, dy, dz)));
            if (it == grid.end()) continue;
            for (uint32_t i : it->second)
              if (glm::length(pts[i] - p) < reachOf) return true;
          }
      return false;
    };
    for (auto& p : r.uncovered)
      if (isNear(p)) unc.push_back(p);
    for (auto& p : prev.uncovered)
      if (!isNear(p)) unc.push_back(p);
    r.uncovered = std::move(unc);
  }
  r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  PLOG_I("Retopology (flow): only the quads near what changed made again: %d kept, %d new, a row of %d faces (%d triangles, %d of its "
         "vertices poles) along %d seams, %d lines across, %d pole pairs taken out; %d poles in all (%.1f%% regular)",
         st.keptFaces, st.newFaces, st.rowFaces, st.rowTriangles, st.rowPoles, st.seams, st.linesAcross, cleaned, r.poles, r.quality.pct(r.quality.regular));
  done = true;
  return r;
}

Result flowQuads(const Mesh& high, const Bvh& bvh, const std::vector<Stroke>& strokes, const std::vector<int32_t>& faceLabel,
                 const std::vector<float>& vertexDensity, const FlowOptions& opt, const std::vector<float>& faceResolution) {
  if ((opt.mirrorAxes & 7) && !opt.mirrored) return flowQuadsMirrored(high, bvh, strokes, faceLabel, vertexDensity, opt, faceResolution);
  auto now = std::make_shared<FlowInputs>();
  now->strokes = strokes, now->faceLabel = faceLabel, now->density = vertexDensity, now->faceResolution = faceResolution;
  now->quadsPerM2 = opt.quadsPerM2, now->loopsAtBorders = opt.loopsAtBorders, now->ringZones = opt.ringZones;
  now->requantize = opt.requantize, now->layout = opt.layout, now->mirrored = opt.mirrored, now->mirrorAxes = opt.mirrorAxes;
  now->mirrorCenter = opt.mirrorCenter, now->shell = opt.shell, now->highFaces = high.faceCount(), now->highVertices = high.positions.size();
  Result r;
  bool done = false;
  if (opt.previous && opt.previous->builtFrom && opt.localReach > 0 && !opt.layout)
    r = flowQuadsLocal(high, bvh, strokes, faceLabel, vertexDensity, opt, faceResolution, *now, done);
  if (!done) {
    FlowOptions o = opt;
    o.previous.reset();
    r = flowQuadsWhole(high, bvh, strokes, faceLabel, vertexDensity, o, faceResolution);
  }
  if (!r.unchanged) r.builtFrom = now;
  return r;
}

}  // namespace patina::retopo
