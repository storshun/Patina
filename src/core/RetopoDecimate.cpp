#include "core/RetopoDecimate.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <queue>
#include <thread>
#include <unordered_map>

#include "core/Parallel.h"

namespace patina::retopo {

namespace {

// a sum of squared distances to planes (Garland and Heckbert)
struct Quadric {
  double xx = 0, xy = 0, xz = 0, xw = 0, yy = 0, yz = 0, yw = 0, zz = 0, zw = 0, ww = 0;
  void addPlane(double nx, double ny, double nz, double d, double w) {
    xx += w * nx * nx, xy += w * nx * ny, xz += w * nx * nz, xw += w * nx * d;
    yy += w * ny * ny, yz += w * ny * nz, yw += w * ny * d;
    zz += w * nz * nz, zw += w * nz * d, ww += w * d * d;
  }
  Quadric operator+(const Quadric& o) const {
    Quadric q = *this;
    q.xx += o.xx, q.xy += o.xy, q.xz += o.xz, q.xw += o.xw, q.yy += o.yy, q.yz += o.yz, q.yw += o.yw, q.zz += o.zz,
        q.zw += o.zw, q.ww += o.ww;
    return q;
  }
  double eval(const vec3& p) const {
    double x = p.x, y = p.y, z = p.z;
    return xx * x * x + 2 * xy * x * y + 2 * xz * x * z + 2 * xw * x + yy * y * y + 2 * yz * y * z + 2 * yw * y + zz * z * z +
           2 * zw * z + ww;
  }
  // the point where it is least (false when that isn't well defined: a flat or straight neighborhood)
  bool minimum(vec3& out) const {
    double det = xx * (yy * zz - yz * yz) - xy * (xy * zz - yz * xz) + xz * (xy * yz - yy * xz);
    double scale = std::max({std::abs(xx), std::abs(yy), std::abs(zz), 1e-30});
    if (std::abs(det) < 1e-9 * scale * scale * scale) return false;
    double bx = -xw, by = -yw, bz = -zw;
    double dx = bx * (yy * zz - yz * yz) - xy * (by * zz - yz * bz) + xz * (by * yz - yy * bz);
    double dy = xx * (by * zz - yz * bz) - bx * (xy * zz - yz * xz) + xz * (xy * bz - by * xz);
    double dz = xx * (yy * bz - by * yz) - xy * (xy * bz - by * xz) + bx * (xy * yz - yy * xz);
    out = vec3(float(dx / det), float(dy / det), float(dz / det));
    return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z);
  }
};

struct Cand {
  double cost;
  uint32_t a, b, sa, sb;
  vec3 p;
  bool operator<(const Cand& o) const { return cost > o.cost; }  // (the cheapest on top)
};

}  // namespace

namespace {
// One mesh (or one piece of it), on one thread. frozen: vertices that must not move (a piece's border).
size_t decimateSerial(std::vector<vec3>& pos, std::vector<glm::uvec3>& tri, std::vector<float>& tgt, const std::vector<char>* frozen) {
  const size_t V = pos.size(), T = tri.size();
  if (T < 8 || tgt.size() != V) return T;
  std::vector<std::vector<uint32_t>> vt(V);
  for (uint32_t t = 0; t < T; ++t)
    for (int k = 0; k < 3; ++k) vt[tri[t][k]].push_back(t);
  std::vector<char> tAlive(T, 1), vAlive(V, 1), vBound(V, 0);
  std::vector<uint32_t> stamp(V, 0);
  std::vector<Quadric> Q(V);
  for (uint32_t t = 0; t < T; ++t) {
    const vec3 &a = pos[tri[t].x], &b = pos[tri[t].y], &c = pos[tri[t].z];
    vec3 n = glm::cross(b - a, c - a);
    double len = glm::length(n);
    if (len <= 0) continue;
    double nx = n.x / len, ny = n.y / len, nz = n.z / len, d = -(nx * a.x + ny * a.y + nz * a.z);
    for (int k = 0; k < 3; ++k) Q[tri[t][k]].addPlane(nx, ny, nz, d, 0.5 * len);
  }
  // the neighbors of a vertex (over its live triangles), sorted
  std::vector<uint32_t> na, nb;
  auto neighbors = [&](uint32_t v, std::vector<uint32_t>& out) {
    out.clear();
    for (uint32_t t : vt[v]) {
      if (!tAlive[t]) continue;
      for (int k = 0; k < 3; ++k)
        if (tri[t][k] != v) out.push_back(tri[t][k]);
    }
    std::sort(out.begin(), out.end());
  };
  // open edges: a neighbor met once; they get planes across them, heavily weighted, and stay put
  for (uint32_t v = 0; v < V; ++v) {
    neighbors(v, na);
    for (size_t i = 0; i < na.size(); ++i) {
      bool once = (i == 0 || na[i - 1] != na[i]) && (i + 1 == na.size() || na[i + 1] != na[i]);
      if (!once) continue;
      vBound[v] = 1;
      uint32_t w = na[i];
      if (w < v) continue;
      // the face beside the edge, for the plane across it
      for (uint32_t t : vt[v]) {
        const auto& f = tri[t];
        if (f.x != w && f.y != w && f.z != w) continue;
        vec3 e = pos[w] - pos[v], n = glm::cross(pos[f.y] - pos[f.x], pos[f.z] - pos[f.x]);
        vec3 m = glm::cross(e, n);
        float ml = glm::length(m);
        if (ml <= 0) break;
        m /= ml;
        double d = -glm::dot(m, pos[v]), wgt = 100.0 * glm::dot(e, e);
        Q[v].addPlane(m.x, m.y, m.z, d, wgt), Q[w].addPlane(m.x, m.y, m.z, d, wgt);
        break;
      }
    }
  }
  std::priority_queue<Cand> heap;
  auto isFrozen = [&](uint32_t v) { return frozen && (*frozen)[v]; };
  auto consider = [&](uint32_t a, uint32_t b) {
    float tt = std::min(tgt[a], tgt[b]);
    if (glm::dot(pos[a] - pos[b], pos[a] - pos[b]) >= tt * tt) return;
    if (isFrozen(a) && isFrozen(b)) return;
    if (isFrozen(a)) std::swap(a, b);  // (the one that stays put is b)
    Quadric q = Q[a] + Q[b];
    vec3 p, mid = 0.5f * (pos[a] + pos[b]);
    if (isFrozen(b)) p = pos[b];
    else if (vBound[a] && vBound[b]) p = mid;  // (an open edge: along it; checked when it comes up)
    else if (vBound[a]) p = pos[a];
    else if (vBound[b]) p = pos[b];
    else if (!q.minimum(p) || glm::dot(p - mid, p - mid) > glm::dot(pos[a] - pos[b], pos[a] - pos[b])) {
      p = mid;
      if (q.eval(pos[a]) < q.eval(p)) p = pos[a];
      if (q.eval(pos[b]) < q.eval(p)) p = pos[b];
    }
    heap.push({std::max(0.0, q.eval(p)), a, b, stamp[a], stamp[b], p});
  };
  for (uint32_t v = 0; v < V; ++v) {
    neighbors(v, na);
    na.erase(std::unique(na.begin(), na.end()), na.end());
    for (uint32_t w : na)
      if (w > v) consider(v, w);
  }
  size_t alive = T;
  while (!heap.empty()) {
    Cand c = heap.top();
    heap.pop();
    uint32_t a = c.a, b = c.b;
    if (!vAlive[a] || !vAlive[b] || stamp[a] != c.sa || stamp[b] != c.sb) continue;
    neighbors(a, na), neighbors(b, nb);
    // how many triangles hold the edge (1: an open edge, 2: inside), and the vertices both see
    int shared = 0;
    for (uint32_t t : vt[a])
      if (tAlive[t] && (tri[t].x == b || tri[t].y == b || tri[t].z == b)) ++shared;
    if (shared == 0) continue;
    na.erase(std::unique(na.begin(), na.end()), na.end());
    nb.erase(std::unique(nb.begin(), nb.end()), nb.end());
    int common = 0;
    for (size_t i = 0, j = 0; i < na.size() && j < nb.size();) {
      if (na[i] == nb[j]) ++common, ++i, ++j;
      else if (na[i] < nb[j]) ++i;
      else ++j;
    }
    if (common != shared) continue;  // (the link condition: else the mesh would pinch)
    if (vBound[a] && vBound[b] && shared != 1 && !isFrozen(b)) continue;  // (two open edges joined across)
    if (isFrozen(a)) continue;
    const vec3 p = c.p;
    // no edge much longer than the target, no triangle turned over or flattened
    const float maxLen2 = 4.f * std::min(tgt[a], tgt[b]) * std::min(tgt[a], tgt[b]);
    bool ok = true;
    for (uint32_t x : na)
      if (x != b && glm::dot(pos[x] - p, pos[x] - p) > maxLen2) ok = false;
    for (uint32_t x : nb)
      if (ok && x != a && glm::dot(pos[x] - p, pos[x] - p) > maxLen2) ok = false;
    for (uint32_t v : {a, b}) {
      for (uint32_t t : vt[v]) {
        if (!ok) break;
        if (!tAlive[t]) continue;
        const auto& f = tri[t];
        bool hasA = f.x == a || f.y == a || f.z == a, hasB = f.x == b || f.y == b || f.z == b;
        if (hasA && hasB) continue;
        vec3 P[3], N[3];
        for (int k = 0; k < 3; ++k) P[k] = pos[f[k]], N[k] = f[k] == a || f[k] == b ? p : P[k];
        vec3 n0 = glm::cross(P[1] - P[0], P[2] - P[0]), n1 = glm::cross(N[1] - N[0], N[2] - N[0]);
        float l0 = glm::length(n0), l1 = glm::length(n1);
        if (l1 <= 1e-4f * l0 || glm::dot(n0, n1) < 0.2f * l0 * l1) ok = false;
      }
    }
    if (!ok) continue;
    // collapse a into b
    pos[b] = p;
    tgt[b] = std::min(tgt[a], tgt[b]);
    Q[b] = Q[a] + Q[b];
    vBound[b] = vBound[a] | vBound[b];
    vAlive[a] = 0;
    for (uint32_t t : vt[a]) {
      if (!tAlive[t]) continue;
      auto& f = tri[t];
      if (f.x == b || f.y == b || f.z == b) {
        tAlive[t] = 0, --alive;
        continue;
      }
      for (int k = 0; k < 3; ++k)
        if (f[k] == a) f[k] = b;
      vt[b].push_back(t);
    }
    vt[a].clear(), vt[a].shrink_to_fit();
    vt[b].erase(std::remove_if(vt[b].begin(), vt[b].end(), [&](uint32_t t) { return !tAlive[t]; }), vt[b].end());
    ++stamp[b];
    neighbors(b, nb);
    nb.erase(std::unique(nb.begin(), nb.end()), nb.end());
    for (uint32_t x : nb) consider(b, x);
  }
  // compact
  std::vector<int64_t> map(V, -1);
  std::vector<vec3> np;
  std::vector<float> ng;
  std::vector<glm::uvec3> nt;
  nt.reserve(alive);
  for (uint32_t t = 0; t < T; ++t) {
    if (!tAlive[t]) continue;
    glm::uvec3 f;
    for (int k = 0; k < 3; ++k) {
      uint32_t v = tri[t][k];
      if (map[v] < 0) map[v] = (int64_t)np.size(), np.push_back(pos[v]), ng.push_back(tgt[v]);
      f[k] = (uint32_t)map[v];
    }
    nt.push_back(f);
  }
  pos = std::move(np), tri = std::move(nt), tgt = std::move(ng);
  return tri.size();
}
}  // namespace

size_t decimateToEdge(std::vector<vec3>& pos, std::vector<glm::uvec3>& tri, float target) {
  std::vector<float> tgt(pos.size(), target);
  return decimateToEdge(pos, tri, tgt);
}

size_t decimateToEdge(std::vector<vec3>& pos, std::vector<glm::uvec3>& tri, std::vector<float>& tgt) {
  const size_t T = tri.size();
  const unsigned threads = std::max(1u, std::thread::hardware_concurrency());
  if (tgt.size() != pos.size()) return T;
  if (T < 200000 || threads < 2) return decimateSerial(pos, tri, tgt, nullptr);
  // pieces of about the same size (split along the longest side, again and again), each decimated on a
  // thread of its own with its border held still; then the whole once more, which is quick by then
  std::vector<vec3> mid(T);
  for (size_t t = 0; t < T; ++t) mid[t] = (pos[tri[t].x] + pos[tri[t].y] + pos[tri[t].z]) / 3.f;
  std::vector<uint32_t> order(T);
  for (uint32_t t = 0; t < T; ++t) order[t] = t;
  int pieces = 1;
  while (pieces < (int)threads && pieces < 64) pieces *= 2;
  std::vector<std::pair<size_t, size_t>> ranges = {{0, T}};
  while ((int)ranges.size() < pieces) {
    std::vector<std::pair<size_t, size_t>> next;
    for (auto [a, b] : ranges) {
      vec3 lo(1e30f), hi(-1e30f);
      for (size_t i = a; i < b; ++i) lo = glm::min(lo, mid[order[i]]), hi = glm::max(hi, mid[order[i]]);
      vec3 ext = hi - lo;
      int axis = ext.x >= ext.y && ext.x >= ext.z ? 0 : ext.y >= ext.z ? 1 : 2;
      size_t m = (a + b) / 2;
      std::nth_element(order.begin() + a, order.begin() + m, order.begin() + b,
                       [&](uint32_t x, uint32_t y) { return mid[x][axis] < mid[y][axis]; });
      next.push_back({a, m}), next.push_back({m, b});
    }
    ranges = std::move(next);
  }
  std::vector<int> pieceOf(T);
  for (size_t k = 0; k < ranges.size(); ++k)
    for (size_t i = ranges[k].first; i < ranges[k].second; ++i) pieceOf[order[i]] = (int)k;
  // a vertex on triangles of two pieces is on a border
  std::vector<int> vPiece(pos.size(), -1);
  std::vector<char> border(pos.size(), 0);
  for (uint32_t t = 0; t < T; ++t)
    for (int k = 0; k < 3; ++k) {
      uint32_t v = tri[t][k];
      if (vPiece[v] < 0) vPiece[v] = pieceOf[t];
      else if (vPiece[v] != pieceOf[t]) border[v] = 1;
    }
  struct Piece {
    std::vector<vec3> pos;
    std::vector<float> tgt;
    std::vector<glm::uvec3> tri;
    std::vector<char> frozen;
    std::vector<uint32_t> global;  // (frozen vertices: their index in the whole)
  };
  std::vector<Piece> P(ranges.size());
  parallelFor(ranges.size(), [&](size_t k) {
    Piece& pc = P[k];
    std::unordered_map<uint32_t, uint32_t> local;
    for (size_t i = ranges[k].first; i < ranges[k].second; ++i) {
      glm::uvec3 f;
      for (int c = 0; c < 3; ++c) {
        uint32_t v = tri[order[i]][c];
        auto [it, fresh] = local.emplace(v, (uint32_t)pc.pos.size());
        if (fresh) pc.pos.push_back(pos[v]), pc.tgt.push_back(tgt[v]), pc.frozen.push_back(border[v]), pc.global.push_back(v);
        f[c] = it->second;
      }
      pc.tri.push_back(f);
    }
    // (decimateSerial compacts the vertices: keep the frozen ones' global indices by position match)
    std::vector<vec3> before = pc.pos;
    decimateSerial(pc.pos, pc.tri, pc.tgt, &pc.frozen);
    std::unordered_map<uint64_t, uint32_t> frozenAt;
    auto key = [](const vec3& p) {
      uint32_t x, y, z;
      std::memcpy(&x, &p.x, 4), std::memcpy(&y, &p.y, 4), std::memcpy(&z, &p.z, 4);
      return (uint64_t(x) * 0x9E3779B97F4A7C15ull) ^ (uint64_t(y) * 0xC2B2AE3D27D4EB4Full) ^ uint64_t(z);
    };
    for (size_t v = 0; v < before.size(); ++v)
      if (pc.frozen[v]) frozenAt[key(before[v])] = pc.global[v];
    pc.global.assign(pc.pos.size(), ~0u);
    for (size_t v = 0; v < pc.pos.size(); ++v)
      if (auto it = frozenAt.find(key(pc.pos[v])); it != frozenAt.end()) pc.global[v] = it->second;
  });
  // stitch: border vertices are shared through their global index
  std::vector<vec3> np;
  std::vector<float> ng;
  std::vector<glm::uvec3> nt;
  std::unordered_map<uint32_t, uint32_t> shared;
  for (auto& pc : P) {
    std::vector<uint32_t> map(pc.pos.size());
    for (size_t v = 0; v < pc.pos.size(); ++v) {
      if (pc.global[v] != ~0u) {
        auto [it, fresh] = shared.emplace(pc.global[v], (uint32_t)np.size());
        if (fresh) np.push_back(pc.pos[v]), ng.push_back(pc.tgt[v]);
        map[v] = it->second;
      } else {
        map[v] = (uint32_t)np.size();
        np.push_back(pc.pos[v]), ng.push_back(pc.tgt[v]);
      }
    }
    for (auto& f : pc.tri) nt.push_back({map[f.x], map[f.y], map[f.z]});
  }
  pos = std::move(np), tri = std::move(nt), tgt = std::move(ng);
  return decimateSerial(pos, tri, tgt, nullptr);
}

size_t refineToEdge(std::vector<vec3>& pos, std::vector<glm::uvec3>& tri, std::vector<float>& tgt, size_t maxTriangles) {
  if (tgt.size() != pos.size()) return tri.size();
  for (int pass = 0; pass < 12 && tri.size() < maxTriangles; ++pass) {
    // the edges to split, each once (its middle a new vertex)
    std::unordered_map<uint64_t, uint32_t> mid;
    auto key = [](uint32_t a, uint32_t b) { return (uint64_t(std::min(a, b)) << 32) | std::max(a, b); };
    for (auto& t : tri)
      for (int k = 0; k < 3; ++k) {
        uint32_t a = t[k], b = t[(k + 1) % 3];
        float lim = std::max(tgt[a], tgt[b]);
        if (glm::dot(pos[a] - pos[b], pos[a] - pos[b]) > lim * lim && !mid.count(key(a, b))) {
          mid[key(a, b)] = (uint32_t)pos.size();
          pos.push_back(0.5f * (pos[a] + pos[b]));
          tgt.push_back(std::min(tgt[a], tgt[b]));
        }
      }
    if (mid.empty()) break;
    std::vector<glm::uvec3> out;
    out.reserve(tri.size() * 2);
    for (auto& t : tri) {
      uint32_t m[3];
      int n = 0;
      for (int k = 0; k < 3; ++k) {
        auto it = mid.find(key(t[k], t[(k + 1) % 3]));
        m[k] = it == mid.end() ? ~0u : it->second;
        n += m[k] != ~0u;
      }
      if (n == 0) {
        out.push_back(t);
      } else if (n == 3) {
        out.push_back({t.x, m[0], m[2]}), out.push_back({m[0], t.y, m[1]}), out.push_back({m[2], m[1], t.z}), out.push_back({m[0], m[1], m[2]});
      } else if (n == 1) {
        int k = m[0] != ~0u ? 0 : m[1] != ~0u ? 1 : 2;  // the edge from corner k to k+1
        uint32_t a = t[k], b = t[(k + 1) % 3], c = t[(k + 2) % 3];
        out.push_back({a, m[k], c}), out.push_back({m[k], b, c});
      } else {
        int k = m[0] == ~0u ? 0 : m[1] == ~0u ? 1 : 2;  // the edge not split: corner k to k+1
        uint32_t a = t[k], b = t[(k + 1) % 3], c = t[(k + 2) % 3];
        uint32_t mbc = m[(k + 1) % 3], mca = m[(k + 2) % 3];
        out.push_back({mca, mbc, c});
        // the quad a b mbc mca, split along its shorter diagonal
        if (glm::length(pos[a] - pos[mbc]) <= glm::length(pos[b] - pos[mca])) out.push_back({a, b, mbc}), out.push_back({a, mbc, mca});
        else out.push_back({a, b, mca}), out.push_back({b, mbc, mca});
      }
    }
    tri = std::move(out);
  }
  return tri.size();
}

}  // namespace patina::retopo
