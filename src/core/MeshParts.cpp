#include "core/MeshParts.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <map>
#include <numeric>
#include <queue>
#include <set>
#include <unordered_map>

namespace patina::parts {

namespace {

// Area of the convex hull of 2D points (monotone chain).
float hullArea(std::vector<vec2>& p) {
  if (p.size() < 3) return 0.f;
  std::sort(p.begin(), p.end(), [](const vec2& a, const vec2& b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
  auto cross = [](const vec2& o, const vec2& a, const vec2& b) { return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x); };
  std::vector<vec2> h(p.size() * 2);
  size_t k = 0;
  for (size_t i = 0; i < p.size(); ++i) {
    while (k >= 2 && cross(h[k - 2], h[k - 1], p[i]) <= 0) --k;
    h[k++] = p[i];
  }
  for (size_t i = p.size() - 1, t = k + 1; i-- > 0;) {
    while (k >= t && cross(h[k - 2], h[k - 1], p[i]) <= 0) --k;
    h[k++] = p[i];
  }
  h.resize(k > 0 ? k - 1 : 0);
  float a = 0;
  for (size_t i = 0; i < h.size(); ++i) {
    const vec2& u = h[i];
    const vec2& v = h[(i + 1) % h.size()];
    a += u.x * v.y - v.x * u.y;
  }
  return std::abs(a) * 0.5f;
}

// Welded vertex id per vertex (positions closer than a tiny fraction of the model size are one).
std::vector<uint32_t> weldVertices(const Mesh& m) {
  float q = std::max(m.boundingRadius, 1e-6f) * 1e-5f;
  std::unordered_map<uint64_t, uint32_t> seen;
  std::vector<uint32_t> id(m.positions.size());
  for (size_t i = 0; i < m.positions.size(); ++i) {
    const vec3& p = m.positions[i];
    auto c = [&](float v) { return uint64_t(int64_t(std::llround(v / q)) & 0x1FFFFF); };
    uint64_t key = c(p.x) | (c(p.y) << 21) | (c(p.z) << 42);
    auto [it, fresh] = seen.emplace(key, (uint32_t)i);
    id[i] = it->second;
  }
  return id;
}

struct UnionFind {
  std::vector<uint32_t> p;
  explicit UnionFind(size_t n) : p(n) { std::iota(p.begin(), p.end(), 0u); }
  uint32_t find(uint32_t x) {
    while (p[x] != x) x = p[x] = p[p[x]];
    return x;
  }
  void unite(uint32_t a, uint32_t b) { p[find(a)] = find(b); }
};

std::vector<std::string> tokens(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  auto flush = [&] {
    if (!cur.empty()) out.push_back(cur);
    cur.clear();
  };
  for (size_t i = 0; i < s.size(); ++i) {
    unsigned char c = (unsigned char)s[i];
    if (std::isalpha(c)) {
      // camelCase boundary: "BodySkin" -> body, skin
      if (std::isupper(c) && !cur.empty() && std::islower((unsigned char)s[i - 1])) flush();
      cur += (char)std::tolower(c);
    } else {
      flush();
    }
  }
  flush();
  return out;
}

const char* vertical(float t) {
  return t > 0.8f ? "at the top" : t > 0.6f ? "in the upper part" : t > 0.4f ? "in the middle" : t > 0.2f ? "in the lower part"
                                                                                                         : "at the bottom";
}

}  // namespace

std::vector<float> sliceWidths(const Mesh& m, int bands, const std::vector<uint8_t>* faceMask) {
  std::vector<float> w(bands, 0.f);
  if (!m.faceCount() || !m.bounds.valid()) return w;
  float y0 = m.bounds.lo.y, H = m.bounds.hi.y - m.bounds.lo.y;
  if (H <= 0) return w;
  std::vector<std::vector<vec2>> pts(bands);
  for (size_t ti = 0; ti < m.tris.size(); ++ti) {
    if (faceMask && !(*faceMask)[m.triFace[ti]]) continue;
    const auto& t = m.tris[ti];
    vec3 p[3] = {m.positions[m.loopVert[t.x]], m.positions[m.loopVert[t.y]], m.positions[m.loopVert[t.z]]};
    float lo = std::min({p[0].y, p[1].y, p[2].y}), hi = std::max({p[0].y, p[1].y, p[2].y});
    int b0 = std::max(0, int(std::floor((lo - y0) / H * bands - 0.5f)));
    int b1 = std::min(bands - 1, int(std::ceil((hi - y0) / H * bands - 0.5f)));
    for (int b = b0; b <= b1; ++b) {
      float c = y0 + (b + 0.5f) / bands * H;
      if (c < lo || c > hi) continue;
      for (int e = 0; e < 3; ++e) {
        const vec3 &a = p[e], &q = p[(e + 1) % 3];
        if ((a.y - c) * (q.y - c) > 0 || a.y == q.y) continue;
        float s = (c - a.y) / (q.y - a.y);
        vec3 x = a + (q - a) * s;
        pts[b].push_back({x.x, x.z});
      }
    }
  }
  for (int b = 0; b < bands; ++b) w[b] = std::sqrt(hullArea(pts[b]));
  return w;
}

namespace {
NeckResult findNeckIn(const Mesh& m, const std::vector<uint8_t>* faceMask) {
  NeckResult r;
  const int N = 100;
  auto w = sliceWidths(m, N, faceMask);
  float H = m.bounds.hi.y - m.bounds.lo.y;
  // The neck is in the top 35% of the height (a standing figure); a bust is mostly head, so
  // when nothing is found there the search goes on down to 55%. The shoulders can be up to a
  // quarter of the height below the neck.
  const float kRatio = 1.4f;
  int best = -1;
  float bestW = 1e30f, bestA = 0, bestB = 0;
  auto search = [&](int from, int to) {
    for (int i = from; i < to; ++i) {
      if (w[i] <= 0) continue;
      float above = 0, below = 0;
      for (int j = i + 1; j < N; ++j) above = std::max(above, w[j]);
      for (int j = std::max(0, i - N / 4); j < i; ++j) below = std::max(below, w[j]);
      if (above >= kRatio * w[i] && below >= kRatio * w[i] && w[i] < bestW) {
        best = i;
        bestW = w[i];
        bestA = above / w[i];
        bestB = below / w[i];
      }
    }
  };
  search(int(N * 0.65f), N - 2);
  if (best < 0) search(int(N * 0.45f), int(N * 0.65f));
  if (best < 0) {
    r.why = "no narrow neck between a wider head and wider shoulders";
    return r;
  }
  r.found = true;
  r.y = m.bounds.lo.y + (best + 0.5f) / N * H;
  r.width = bestW;
  r.ratioAbove = bestA;
  r.ratioBelow = bestB;
  return r;
}
}  // namespace

NeckResult findNeck(const Mesh& m) {
  if (!m.faceCount()) {
    NeckResult r;
    r.why = "there is no mesh";
    return r;
  }
  NeckResult r = findNeckIn(m, nullptr);
  if (r.found) return r;
  // hair or a cape over the shoulders: the largest pieces on their own
  uint32_t pieces = 0;
  auto piece = facePieces(m, &pieces);
  if (pieces < 2) return r;
  std::vector<uint32_t> count(pieces, 0);
  for (uint32_t p : piece) count[p]++;
  for (uint32_t p = 0; p < std::min<uint32_t>(pieces, 3); ++p) {
    if (count[p] * 5 < m.faceCount()) break;  // too small to be the body
    std::vector<uint8_t> mask(m.faceCount(), 0);
    for (uint32_t f = 0; f < m.faceCount(); ++f) mask[f] = piece[f] == p;
    NeckResult q = findNeckIn(m, &mask);
    if (q.found) {
      q.piece = (int)p;
      return q;
    }
  }
  return r;
}

std::vector<uint32_t> facePieces(const Mesh& m, uint32_t* count) {
  uint32_t F = m.faceCount();
  auto weld = weldVertices(m);
  UnionFind uf(m.positions.size());
  for (uint32_t f = 0; f < F; ++f)
    for (uint32_t l = m.faceStart[f] + 1; l < m.faceStart[f + 1]; ++l) uf.unite(weld[m.loopVert[l]], weld[m.loopVert[m.faceStart[f]]]);
  std::unordered_map<uint32_t, uint32_t> rootIndex;
  std::vector<uint32_t> raw(F);
  std::vector<uint32_t> sizes;
  for (uint32_t f = 0; f < F; ++f) {
    uint32_t root = uf.find(weld[m.loopVert[m.faceStart[f]]]);
    auto [it, fresh] = rootIndex.emplace(root, (uint32_t)sizes.size());
    if (fresh) sizes.push_back(0);
    raw[f] = it->second;
    sizes[it->second]++;
  }
  std::vector<uint32_t> order(sizes.size());
  std::iota(order.begin(), order.end(), 0u);
  std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return sizes[a] > sizes[b]; });
  std::vector<uint32_t> rank(sizes.size());
  for (uint32_t i = 0; i < order.size(); ++i) rank[order[i]] = i;
  for (auto& p : raw) p = rank[p];
  if (count) *count = (uint32_t)sizes.size();
  return raw;
}

std::vector<uint8_t> headFaces(const Mesh& m, const NeckResult& neck) {
  uint32_t F = m.faceCount();
  std::vector<uint8_t> head(F, 0);
  if (!neck.found) return head;
  uint32_t pieces = 0;
  auto piece = facePieces(m, &pieces);
  std::vector<uint32_t> total(pieces, 0), above(pieces, 0);
  std::vector<vec3> center(F);
  for (uint32_t f = 0; f < F; ++f) {
    center[f] = m.faceCenter(f);
    total[piece[f]]++;
    if (center[f].y > neck.y) above[piece[f]]++;
  }
  float H = m.bounds.hi.y - m.bounds.lo.y;
  float near = H * 0.03f;
  // Pieces that cross the cut: their faces above it, in the connected parts that reach the cut
  // (a hand raised above the head is connected only through the arm, below the cut).
  auto weld = weldVertices(m);
  UnionFind uf(m.positions.size());
  for (uint32_t f = 0; f < F; ++f) {
    if (center[f].y <= neck.y) continue;
    for (uint32_t l = m.faceStart[f] + 1; l < m.faceStart[f + 1]; ++l) uf.unite(weld[m.loopVert[l]], weld[m.loopVert[m.faceStart[f]]]);
  }
  std::vector<uint8_t> reachesCut(m.positions.size(), 0);
  for (uint32_t f = 0; f < F; ++f)
    if (center[f].y > neck.y && center[f].y < neck.y + near) reachesCut[uf.find(weld[m.loopVert[m.faceStart[f]]])] = 1;
  AABB foot;  // footprint of the head in X and Z
  for (uint32_t f = 0; f < F; ++f) {
    if (center[f].y <= neck.y) continue;
    bool crossing = above[piece[f]] * 10 < total[piece[f]] * 9;
    // a neck found on one piece (the body under long hair) cuts only that piece
    if (crossing && neck.piece >= 0 && piece[f] != (uint32_t)neck.piece) continue;
    if (crossing && !reachesCut[uf.find(weld[m.loopVert[m.faceStart[f]]])]) continue;
    if (crossing) {
      head[f] = 1;
      foot.add(center[f]);
    }
  }
  // Pieces entirely above the cut (eyes, teeth, hair, a hat): inside the footprint, or the
  // whole head when nothing crosses the cut (a head that is its own piece).
  bool anyCrossing = foot.valid();
  if (anyCrossing) {
    vec3 grow = (foot.hi - foot.lo) * 0.15f;
    foot.lo -= grow;
    foot.hi += grow;
  }
  for (uint32_t p = 0; p < pieces; ++p) {
    // whole pieces above the cut; when the neck was found on one piece, other pieces that hang
    // across the cut (long hair) go with the head when most of them is above it
    bool other = neck.piece >= 0 && p != (uint32_t)neck.piece;
    if (other ? above[p] * 2 < total[p] : above[p] * 10 < total[p] * 9) continue;
    AABB b;
    for (uint32_t f = 0; f < F; ++f)
      if (piece[f] == p) b.add(center[f]);
    vec3 c = b.center();
    bool inside = !anyCrossing || (c.x >= foot.lo.x && c.x <= foot.hi.x && c.z >= foot.lo.z && c.z <= foot.hi.z);
    if (!inside) continue;
    for (uint32_t f = 0; f < F; ++f)
      if (piece[f] == p) head[f] = 1;
  }
  return head;
}

std::vector<uint32_t> groupFaces(const Mesh& m, const VertexGroup& g) {
  std::vector<uint8_t> in(m.positions.size(), 0);
  for (uint32_t v : g.verts)
    if (v < in.size()) in[v] = 1;
  std::vector<uint32_t> all, most;
  for (uint32_t f = 0; f < m.faceCount(); ++f) {
    uint32_t n = 0, k = m.faceSize(f);
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) n += in[m.loopVert[l]];
    if (n == k) all.push_back(f);
    if (n * 2 >= k && n) most.push_back(f);
  }
  return all.empty() ? most : all;
}

int islandSeams(Mesh& m, const std::vector<uint32_t>& faces, bool backCut) {
  // Works on mesh edges, as the unwrapper does: a vertex split in the file (an OBJ seam column,
  // a pole of duplicated vertices) is already an open border there.
  uint32_t F = m.faceCount();
  size_t E = m.edges.size();
  if (m.edgeFlags.size() != E) m.edgeFlags.assign(E, 0);
  std::vector<uint8_t> in(F, 0);
  for (uint32_t f : faces)
    if (f < F) in[f] = 1;
  std::vector<uint32_t> edgeUse(E, 0), edgeIn(E, 0);
  for (uint32_t f = 0; f < F; ++f)
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) {
      edgeUse[m.loopEdge[l]]++;
      edgeIn[m.loopEdge[l]] += in[f];
    }
  // seams on the border, none inside
  std::vector<uint8_t> border(E, 0);
  for (size_t e = 0; e < E; ++e) {
    if (!edgeIn[e]) continue;
    border[e] = edgeIn[e] < 2 || edgeUse[e] != edgeIn[e];
    if (border[e]) m.edgeFlags[e] |= EdgeSeam;
    else m.edgeFlags[e] &= ~EdgeSeam;
  }
  // pieces of the region: faces joined across inside edges
  UnionFind charts(F);
  {
    std::vector<int64_t> firstFace(E, -1);
    for (uint32_t f = 0; f < F; ++f) {
      if (!in[f]) continue;
      for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) {
        uint32_t e = m.loopEdge[l];
        if (border[e]) continue;
        if (firstFace[e] < 0) firstFace[e] = f;
        else charts.unite(f, (uint32_t)firstFace[e]);
      }
    }
  }
  std::map<uint32_t, std::vector<uint32_t>> pieceEdges;  // chart -> its edges
  {
    std::vector<uint8_t> seen(E, 0);
    for (uint32_t f = 0; f < F; ++f) {
      if (!in[f]) continue;
      for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) {
        uint32_t e = m.loopEdge[l];
        if (!seen[e]) seen[e] = 1, pieceEdges[charts.find(f)].push_back(e);
      }
    }
  }
  int added = 0;
  for (auto& [root, list] : pieceEdges) {
    // border loops of the piece
    UnionFind lu(m.positions.size());
    for (uint32_t e : list)
      if (border[e]) lu.unite(m.edges[e].x, m.edges[e].y);
    std::set<uint32_t> loops;
    for (uint32_t e : list)
      if (border[e]) loops.insert(lu.find(m.edges[e].x));
    if (!backCut && loops.size() == 1) continue;  // already a disk
    // shortest path from one end of the piece to the other over its inside edges, kept to the
    // back: an edge costs its length, more the further toward the front (+Z) it is. The ends are
    // along the piece's longest side (top and bottom, unless it is clearly longer sideways: an arm
    // held out)
    std::unordered_map<uint32_t, std::vector<std::pair<uint32_t, uint32_t>>> adj;  // vertex -> (other, edge)
    AABB box;
    for (uint32_t e : list) box.add(m.positions[m.edges[e].x]), box.add(m.positions[m.edges[e].y]);
    vec3 ext = box.hi - box.lo;
    int axis = 1;
    if (ext.x > 1.3f * ext.y && ext.x >= ext.z) axis = 0;
    else if (ext.z > 1.3f * ext.y && ext.z > ext.x) axis = 2;
    float zlo = 1e30f, zhi = -1e30f, ytop = -1e30f, ybot = 1e30f;
    uint32_t top = 0, bottom = 0;
    for (uint32_t e : list) {
      for (uint32_t v : {m.edges[e].x, m.edges[e].y}) {
        const vec3& p = m.positions[v];
        zlo = std::min(zlo, p.z);
        zhi = std::max(zhi, p.z);
        float h = p[axis];
        if (h > ytop + 1e-6f || (std::abs(h - ytop) <= 1e-6f && p.z < m.positions[top].z)) ytop = h, top = v;
        if (h < ybot - 1e-6f || (std::abs(h - ybot) <= 1e-6f && p.z < m.positions[bottom].z)) ybot = h, bottom = v;
      }
      if (m.edgeFlags[e] & kEdgeDiagonal) continue;  // can't be cut in the real mesh
      adj[m.edges[e].x].push_back({m.edges[e].y, e});
      adj[m.edges[e].y].push_back({m.edges[e].x, e});
    }
    if (top == bottom) continue;
    float zr = std::max(zhi - zlo, 1e-6f);
    std::unordered_map<uint32_t, float> dist;
    std::unordered_map<uint32_t, uint32_t> via;  // vertex -> edge it was reached by
    using QE = std::pair<float, uint32_t>;
    std::priority_queue<QE, std::vector<QE>, std::greater<QE>> q;
    dist[top] = 0;
    q.push({0.f, top});
    while (!q.empty()) {
      auto [d, v] = q.top();
      q.pop();
      if (d > dist[v]) continue;
      if (v == bottom) break;
      for (auto [o, e] : adj[v]) {
        const vec3 &p = m.positions[v], &r = m.positions[o];
        float front = ((p.z + r.z) * 0.5f - zlo) / zr;
        float nd = d + glm::length(p - r) * (1.f + 8.f * front * front) + 1e-6f;
        auto it = dist.find(o);
        if (it == dist.end() || nd < it->second) {
          dist[o] = nd;
          via[o] = e;
          q.push({nd, o});
        }
      }
    }
    if (!via.count(bottom)) continue;
    for (uint32_t v = bottom; v != top;) {
      uint32_t e = via[v];
      if (!(m.edgeFlags[e] & EdgeSeam)) {
        m.edgeFlags[e] |= EdgeSeam;
        ++added;
      }
      v = m.edges[e].x == v ? m.edges[e].y : m.edges[e].x;
    }
  }
  return added;
}

int diskCuts(Mesh& m, const std::vector<uint32_t>& faces) {
  uint32_t F = m.faceCount();
  size_t E = m.edges.size();
  if (m.edgeFlags.size() != E) m.edgeFlags.assign(E, 0);
  std::vector<uint8_t> in(F, 0);
  for (uint32_t f : faces)
    if (f < F) in[f] = 1;
  // faces on each edge (inside the region)
  std::vector<std::vector<uint32_t>> edgeFaces(E);
  for (uint32_t f = 0; f < F; ++f) {
    if (!in[f]) continue;
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) edgeFaces[m.loopEdge[l]].push_back(f);
  }
  float zlo = 1e30f, zhi = -1e30f;
  for (size_t e = 0; e < E; ++e)
    if (!edgeFaces[e].empty())
      for (uint32_t v : {m.edges[e].x, m.edges[e].y}) zlo = std::min(zlo, m.positions[v].z), zhi = std::max(zhi, m.positions[v].z);
  float zr = std::max(zhi - zlo, 1e-6f);
  // what cutting an edge costs: its length, more at the front
  auto cutCost = [&](size_t e) {
    if (m.edgeFlags[e] & kEdgeDiagonal) return 1e30f;  // never left as a cut: crossed first
    const vec3 &a = m.positions[m.edges[e].x], &b = m.positions[m.edges[e].y];
    float front = ((a.z + b.z) * 0.5f - zlo) / zr;
    return glm::length(a - b) * (1.f + 8.f * front * front);
  };
  // fixed cuts: seams, borders and edges with more than two faces
  std::vector<uint8_t> cut(E, 0), fixed(E, 0);
  for (size_t e = 0; e < E; ++e) {
    if (edgeFaces[e].empty()) continue;
    bool f = (m.edgeFlags[e] & EdgeSeam) || edgeFaces[e].size() != 2;
    fixed[e] = cut[e] = f ? 1 : 0;
    if (!f) cut[e] = 1;  // until the face tree crosses it
  }
  // a maximum spanning tree of the faces (Prim): the dearest edges to cut are crossed first,
  // the cheap ones (short, at the back) are left as cuts
  std::vector<uint8_t> seen(F, 0);
  using QE = std::pair<float, uint32_t>;  // (cost, edge)
  std::priority_queue<QE> q;
  auto visit = [&](uint32_t f) {
    seen[f] = 1;
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) {
      uint32_t e = m.loopEdge[l];
      if (!fixed[e] && cut[e]) q.push({cutCost(e), e});
    }
  };
  for (uint32_t f0 = 0; f0 < F; ++f0) {
    if (!in[f0] || seen[f0]) continue;
    visit(f0);
    while (!q.empty()) {
      auto [c, e] = q.top();
      q.pop();
      uint32_t a = edgeFaces[e][0], b = edgeFaces[e][1];
      if (seen[a] && seen[b]) continue;
      cut[e] = 0;  // crossed: the two faces stay joined
      visit(seen[a] ? b : a);
    }
  }
  // prune: a cut that ends in a vertex no other cut reaches is not needed
  std::unordered_map<uint32_t, int> degree;
  for (size_t e = 0; e < E; ++e)
    if (cut[e]) degree[m.edges[e].x]++, degree[m.edges[e].y]++;
  std::unordered_map<uint32_t, std::vector<uint32_t>> vertEdges;
  for (size_t e = 0; e < E; ++e)
    if (cut[e] && !fixed[e]) vertEdges[m.edges[e].x].push_back((uint32_t)e), vertEdges[m.edges[e].y].push_back((uint32_t)e);
  std::vector<uint32_t> stack;
  for (auto& [v, d] : degree)
    if (d == 1) stack.push_back(v);
  while (!stack.empty()) {
    uint32_t v = stack.back();
    stack.pop_back();
    if (degree[v] != 1) continue;
    for (uint32_t e : vertEdges[v]) {
      if (!cut[e] || fixed[e]) continue;
      cut[e] = 0;
      uint32_t o = m.edges[e].x == v ? m.edges[e].y : m.edges[e].x;
      degree[v]--;
      if (--degree[o] == 1) stack.push_back(o);
      break;
    }
  }
  int added = 0;
  for (size_t e = 0; e < E; ++e)
    if (cut[e] && !fixed[e]) {
      m.edgeFlags[e] |= EdgeSeam;
      ++added;
    }
  return added;
}

size_t pieceCount(const Mesh& m, const std::vector<uint32_t>& faces) {
  uint32_t F = m.faceCount();
  std::vector<uint8_t> in(F, 0);
  for (uint32_t f : faces)
    if (f < F) in[f] = 1;
  UnionFind uf(F);
  std::vector<int64_t> first(m.edges.size(), -1);
  for (uint32_t f = 0; f < F; ++f) {
    if (!in[f]) continue;
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) {
      uint32_t e = m.loopEdge[l];
      if (first[e] < 0) first[e] = f;
      else uf.unite(f, (uint32_t)first[e]);
    }
  }
  std::set<uint32_t> roots;
  for (uint32_t f = 0; f < F; ++f)
    if (in[f]) roots.insert(uf.find(f));
  return roots.size();
}

int tipCuts(Mesh& m, const std::vector<uint32_t>& faces, float minLength) {
  uint32_t F = m.faceCount();
  size_t E = m.edges.size();
  if (m.edgeFlags.size() != E) m.edgeFlags.assign(E, 0);
  std::vector<uint8_t> in(F, 0);
  for (uint32_t f : faces)
    if (f < F) in[f] = 1;
  std::vector<uint32_t> use(E, 0);
  std::vector<uint8_t> partEdge(E, 0);
  for (uint32_t f = 0; f < F; ++f) {
    if (!in[f]) continue;
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) {
      use[m.loopEdge[l]]++;
      partEdge[m.loopEdge[l]] = 1;
    }
  }
  // sources: vertices on the border or on a seam already there
  std::vector<uint8_t> onCut(m.positions.size(), 0);
  std::unordered_map<uint32_t, std::vector<std::pair<uint32_t, uint32_t>>> adj;
  float zlo = 1e30f, zhi = -1e30f;
  for (size_t e = 0; e < E; ++e) {
    if (!partEdge[e]) continue;
    uint32_t a = m.edges[e].x, b = m.edges[e].y;
    if (!(m.edgeFlags[e] & kEdgeDiagonal)) {  // a diagonal can't be cut in the real mesh
      adj[a].push_back({b, (uint32_t)e});
      adj[b].push_back({a, (uint32_t)e});
    }
    if (use[e] < 2 || (m.edgeFlags[e] & EdgeSeam)) onCut[a] = onCut[b] = 1;
    zlo = std::min({zlo, m.positions[a].z, m.positions[b].z});
    zhi = std::max({zhi, m.positions[a].z, m.positions[b].z});
  }
  float zr = std::max(zhi - zlo, 1e-6f);
  using QE = std::pair<float, uint32_t>;
  // shortest paths from the sources; `bias` makes edges toward the front (+Z) dearer
  auto paths = [&](float bias, std::unordered_map<uint32_t, float>& dist, std::unordered_map<uint32_t, uint32_t>& via) {
    std::priority_queue<QE, std::vector<QE>, std::greater<QE>> q;
    for (auto& [v, n] : adj)
      if (onCut[v]) dist[v] = 0, q.push({0.f, v});
    while (!q.empty()) {
      auto [d, v] = q.top();
      q.pop();
      if (d > dist[v]) continue;
      for (auto [o, e] : adj[v]) {
        const vec3 &p = m.positions[v], &r = m.positions[o];
        float front = ((p.z + r.z) * 0.5f - zlo) / zr;
        float nd = d + glm::length(p - r) * (1.f + bias * front * front) + 1e-7f;
        auto it = dist.find(o);
        if (it == dist.end() || nd < it->second) {
          dist[o] = nd;
          via[o] = e;
          q.push({nd, o});
        }
      }
    }
  };
  bool anySource = false;
  for (auto& [v, n] : adj) anySource |= onCut[v] != 0;
  if (!anySource) return 0;  // closed and uncut: islandSeams' back cut comes first
  std::unordered_map<uint32_t, float> dist, bdist;
  std::unordered_map<uint32_t, uint32_t> unused, via;
  paths(0.f, dist, unused);  // true distances find the tips
  paths(8.f, bdist, via);    // the cuts themselves run along the back
  float far = 0;
  for (auto& [v, d] : dist) far = std::max(far, d);
  if (far <= 0) return 0;
  // Tips: maxima of the distance that stand out by at least minLength (their persistence: how far
  // the distance falls before the region around them joins a higher one). A hand or foot stands
  // out by the length of the limb; a bump on the chest or a finger by much less.
  std::vector<std::pair<float, uint32_t>> order;
  for (auto& [v, d] : dist) order.push_back({d, v});
  std::sort(order.begin(), order.end(), std::greater<>());
  std::unordered_map<uint32_t, uint32_t> parent;
  std::unordered_map<uint32_t, uint32_t> peak;  // root -> its highest vertex
  std::function<uint32_t(uint32_t)> find = [&](uint32_t x) {
    while (parent[x] != x) x = parent[x] = parent[parent[x]];
    return x;
  };
  std::vector<std::pair<float, uint32_t>> tips;  // (persistence, vertex)
  for (auto [d, v] : order) {
    parent[v] = v;
    peak[v] = v;
    for (auto [o, e] : adj[v]) {
      if (!parent.count(o)) continue;  // not added yet (lower)
      uint32_t a = find(v), b = find(o);
      if (a == b) continue;
      // the lower peak dies here
      uint32_t pa = peak[a], pb = peak[b];
      uint32_t lo = dist[pa] < dist[pb] ? pa : pb, hi = lo == pa ? pb : pa;
      if (lo != v) tips.push_back({dist[lo] - d, lo});
      uint32_t keepRoot = lo == pa ? b : a, dropRoot = keepRoot == a ? b : a;
      parent[dropRoot] = keepRoot;
      peak[keepRoot] = hi;
    }
  }
  if (!order.empty()) tips.push_back({far, peak[find(order.front().second)]});  // the farthest point of all
  std::sort(tips.begin(), tips.end(), std::greater<>());
  int added = 0;
  for (auto [pers, tip] : tips) {
    if (pers < minLength * far) break;
    // a tip on the front (a nose, a chin) would put a seam across the face: hands and feet are
    // never at the very front
    if ((m.positions[tip].z - zlo) / zr > 0.7f) continue;
    // the path back to the nearest cut, along the back
    uint32_t v = tip;
    std::vector<uint32_t> path;
    while (!onCut[v] && via.count(v)) {
      uint32_t e = via[v];
      path.push_back(e);
      v = m.edges[e].x == v ? m.edges[e].y : m.edges[e].x;
    }
    for (uint32_t e : path) {
      if (!(m.edgeFlags[e] & EdgeSeam)) ++added;
      m.edgeFlags[e] |= EdgeSeam;
      onCut[m.edges[e].x] = onCut[m.edges[e].y] = 1;
    }
  }
  return added;
}

PartMesh partMesh(const Mesh& m, const std::vector<uint32_t>& faces) {
  PartMesh out;
  Mesh& c = out.mesh;
  c.name = m.name;
  float tiny = std::max(m.boundingRadius, 1e-6f) * 1e-6f;
  UnionFind uf(m.positions.size());
  for (uint32_t f : faces) {
    if (f >= m.faceCount()) continue;
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) {
      uint32_t a = m.loopVert[l], b = m.loopVert[m.nextLoop(l, f)];
      if (a != b && glm::length(m.positions[a] - m.positions[b]) < tiny) uf.unite(a, b);
    }
  }
  std::unordered_map<uint32_t, uint32_t> local;  // original (merged) vertex -> part vertex
  auto vert = [&](uint32_t v) {
    uint32_t r = uf.find(v);
    auto [it, fresh] = local.emplace(r, (uint32_t)c.positions.size());
    if (fresh) c.positions.push_back(m.positions[r]);
    return it->second;
  };
  c.faceStart.push_back(0);
  std::map<std::pair<uint32_t, uint32_t>, uint8_t> seam;  // seams of the original, by merged end points
  for (size_t e = 0; e < m.edges.size(); ++e)
    if (m.edgeFlags.size() == m.edges.size() && (m.edgeFlags[e] & EdgeSeam)) {
      uint32_t a = uf.find(m.edges[e].x), b = uf.find(m.edges[e].y);
      seam[{std::min(a, b), std::max(a, b)}] = 1;
    }
  std::set<std::pair<uint32_t, uint32_t>> diagonals, sides;
  auto key = [](uint32_t a, uint32_t b) { return std::make_pair(std::min(a, b), std::max(a, b)); };
  for (uint32_t f : faces) {
    if (f >= m.faceCount()) continue;
    // fan triangles of the face, as Mesh::triangulate makes them for convex faces
    uint32_t s0 = m.faceStart[f], n = m.faceSize(f);
    for (uint32_t k = 0; k < n; ++k) sides.insert(key(vert(m.loopVert[s0 + k]), vert(m.loopVert[s0 + (k + 1) % n])));
    for (uint32_t k = 2; k + 1 < n; ++k) diagonals.insert(key(vert(m.loopVert[s0]), vert(m.loopVert[s0 + k])));
    for (uint32_t k = 1; k + 1 < n; ++k) {
      uint32_t L[3] = {s0, s0 + k, s0 + k + 1};
      uint32_t V[3] = {vert(m.loopVert[L[0]]), vert(m.loopVert[L[1]]), vert(m.loopVert[L[2]])};
      if (V[0] == V[1] || V[1] == V[2] || V[0] == V[2]) continue;
      vec3 cr = glm::cross(c.positions[V[1]] - c.positions[V[0]], c.positions[V[2]] - c.positions[V[0]]);
      if (glm::length(cr) < tiny * tiny) continue;
      for (int i = 0; i < 3; ++i) {
        c.loopVert.push_back(V[i]);
        c.loopUV.push_back(m.loopUV[L[i]]);
        out.srcLoop.push_back(L[i]);
      }
      c.faceStart.push_back((uint32_t)c.loopVert.size());
      c.faceMaterial.push_back(f < m.faceMaterial.size() ? m.faceMaterial[f] : 0);
    }
  }
  if (c.faceCount() == 0) return out;
  c.finalize();
  // seams the original had between these faces (a seam inside a quad's diagonal can't exist)
  std::unordered_map<uint32_t, uint32_t> back;
  for (auto& [orig, loc] : local) back[loc] = orig;
  for (size_t e = 0; e < c.edges.size(); ++e) {
    uint32_t a = back[c.edges[e].x], b = back[c.edges[e].y];
    if (seam.count({std::min(a, b), std::max(a, b)})) c.edgeFlags[e] |= EdgeSeam;
    auto k = key(c.edges[e].x, c.edges[e].y);
    if (diagonals.count(k) && !sides.count(k)) c.edgeFlags[e] |= kEdgeDiagonal;
  }
  return out;
}

void copyPartUVs(const PartMesh& p, Mesh& m, const std::vector<uint32_t>& faces) {
  std::vector<int64_t> corner(m.loopCount(), -1);  // original loop -> a corner of the part mesh
  for (uint32_t i = 0; i < p.srcLoop.size(); ++i) corner[p.srcLoop[i]] = i;
  float tiny = std::max(m.boundingRadius, 1e-6f) * 1e-6f;
  for (uint32_t f : faces) {
    if (f >= m.faceCount()) continue;
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) {
      if (corner[l] >= 0) {
        m.loopUV[l] = p.mesh.loopUV[corner[l]];
        continue;
      }
      // left out with a degenerate triangle: a corner of this face at the same place
      for (uint32_t k = m.faceStart[f]; k < m.faceStart[f + 1]; ++k)
        if (corner[k] >= 0 && glm::length(m.positions[m.loopVert[k]] - m.positions[m.loopVert[l]]) < tiny) {
          m.loopUV[l] = p.mesh.loopUV[corner[k]];
          break;
        }
    }
  }
}

std::string regionHint(const std::string& name) {
  static const char* head[] = {"head", "face", "skull", "hair", "helmet", "hood", "scalp", "facial"};
  static const char* body[] = {"body", "torso", "chest", "trunk"};
  for (auto& t : tokens(name)) {
    for (auto* h : head)
      if (t == h) return "head";
    for (auto* b : body)
      if (t == b) return "body";
  }
  return "";
}

PartsReport findParts(const Mesh& m) {
  PartsReport R;
  uint32_t F = m.faceCount();
  if (!F) return R;
  bool namedHead = false, namedBody = false;
  auto note = [&](const std::string& name) {
    std::string h = regionHint(name);
    namedHead |= h == "head";
    namedBody |= h == "body";
  };
  // material slots, when the model uses more than one
  std::vector<std::vector<uint32_t>> bySlot;
  for (uint32_t f = 0; f < F; ++f) {
    uint32_t s = f < m.faceMaterial.size() ? m.faceMaterial[f] : 0;
    if (s >= bySlot.size()) bySlot.resize(s + 1);
    bySlot[s].push_back(f);
  }
  size_t usedSlots = 0;
  for (auto& s : bySlot) usedSlots += s.empty() ? 0 : 1;
  if (usedSlots > 1) {  // one slot is the whole model: nothing to tell apart
    std::map<std::string, int> seen;
    for (uint32_t s = 0; s < bySlot.size(); ++s) {
      if (bySlot[s].empty()) continue;
      std::string name = m.slotName(s);
      int n = ++seen[name];
      Part p;
      p.id = "slot:" + name + (n > 1 ? "#" + std::to_string(n) : "");
      p.kind = "slot";
      p.label = name + " (material slot)";
      p.faces = std::move(bySlot[s]);
      note(name);
      R.parts.push_back(std::move(p));
    }
  }
  // vertex groups
  for (auto& g : m.vertexGroups) {
    auto faces = groupFaces(m, g);
    if (faces.empty()) continue;
    Part p;
    p.id = "group:" + g.name;
    p.kind = "group";
    p.label = g.name + " (vertex group)";
    p.faces = std::move(faces);
    note(g.name);
    R.parts.push_back(std::move(p));
  }
  // connected pieces, when there is more than one
  auto piece = facePieces(m, &R.pieceCount);
  if (R.pieceCount > 1) {
    bool named = !R.parts.empty();  // slots or groups already name the model's parts: fewer pieces
    uint32_t shown = std::min<uint32_t>(R.pieceCount, named ? 8 : 16);
    std::vector<std::vector<uint32_t>> faces(shown);
    for (uint32_t f = 0; f < F; ++f)
      if (piece[f] < shown) faces[piece[f]].push_back(f);
    float H = std::max(m.bounds.hi.y - m.bounds.lo.y, 1e-6f), W = std::max(m.bounds.hi.x - m.bounds.lo.x, 1e-6f);
    for (uint32_t i = 0; i < shown; ++i) {
      AABB b;
      for (uint32_t f : faces[i]) b.add(m.faceCenter(f));
      vec3 c = b.center();
      float t = (c.y - m.bounds.lo.y) / H, sx = (c.x - m.bounds.center().x) / W;
      std::string side = sx > 0.15f ? ", on the +X side" : sx < -0.15f ? ", on the -X side" : "";
      int pct = int(std::lround(100.0 * faces[i].size() / F));
      Part p;
      p.id = "piece:" + std::to_string(i + 1);
      p.kind = "piece";
      p.label = std::string(i == 0 ? "the largest piece" : "a piece") + " (" + (pct ? std::to_string(pct) : "<1") +
                "% of the faces), " + vertical(t) + side;
      p.faces = std::move(faces[i]);
      R.parts.push_back(std::move(p));
    }
  }
  // regions from the shape
  R.neck = findNeck(m);
  if (R.neck.found) {
    auto head = headFaces(m, R.neck);
    Part h, b;
    h.id = "region:head";
    h.kind = b.kind = "region";
    h.label = "the head (found from the shape, above the neck)";
    b.id = "region:body";
    b.label = "the body (found from the shape, below the neck)";
    for (uint32_t f = 0; f < F; ++f) (head[f] ? h.faces : b.faces).push_back(f);
    float conf = std::clamp((std::min(R.neck.ratioAbove, R.neck.ratioBelow) - 1.4f) / 0.6f, 0.f, 1.f) * 0.5f + 0.5f;
    h.confidence = b.confidence = conf;
    h.fallback = namedHead;
    b.fallback = namedBody;
    if (!h.faces.empty() && !b.faces.empty()) {
      R.parts.push_back(std::move(h));
      R.parts.push_back(std::move(b));
    } else {
      R.neck.found = false;
      R.neck.why = "the cut at the neck left nothing on one side";
    }
  }
  // a piece with exactly the faces of a part listed before it says nothing new
  std::vector<Part> kept;
  for (auto& p : R.parts) {
    bool same = false;
    if (p.kind == "piece") {  // regions stay: "head" and "body" mean something even when a slot has the same faces
      std::vector<uint32_t> a = p.faces;
      std::sort(a.begin(), a.end());
      for (auto& q : kept) {
        if (q.faces.size() != a.size()) continue;
        std::vector<uint32_t> b = q.faces;
        std::sort(b.begin(), b.end());
        if (a == b) {
          same = true;
          break;
        }
      }
    }
    if (!same) kept.push_back(std::move(p));
  }
  R.parts = std::move(kept);
  return R;
}

}  // namespace patina::parts
