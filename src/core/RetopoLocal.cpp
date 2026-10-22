#include "core/RetopoLocal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <cstdio>
#include <cstdlib>

#include "core/Log.h"

namespace patina::retopo {
namespace {

using EdgeKey = uint64_t;
inline EdgeKey dkey(uint32_t a, uint32_t b) { return (uint64_t(a) << 32) | b; }
inline EdgeKey ukey(uint32_t a, uint32_t b) { return dkey(std::min(a, b), std::max(a, b)); }

struct Faces {
  std::vector<vec3> P;
  std::vector<std::vector<uint32_t>> F;
  std::unordered_map<EdgeKey, uint32_t> faceOf;  // directed edge -> face
  void read(const Mesh& m) {
    P = m.positions;
    const uint32_t n = m.faceStart.empty() ? 0 : (uint32_t)m.faceStart.size() - 1;
    F.resize(n);
    for (uint32_t f = 0; f < n; ++f) {
      F[f].assign(m.loopVert.begin() + m.faceStart[f], m.loopVert.begin() + m.faceStart[f + 1]);
      for (size_t k = 0; k < F[f].size(); ++k) faceOf[dkey(F[f][k], F[f][(k + 1) % F[f].size()])] = f;
    }
  }
  // the face across a face's edge a -> b (or -1: an open edge)
  int across(uint32_t a, uint32_t b) const {
    auto it = faceOf.find(dkey(b, a));
    return it == faceOf.end() ? -1 : (int)it->second;
  }
};

struct PointGrid {
  float cell = 1;
  std::unordered_map<uint64_t, std::vector<uint32_t>> m;
  static uint64_t key(int64_t x, int64_t y, int64_t z) {
    return (uint64_t(x & 0x1FFFFF) << 42) | (uint64_t(y & 0x1FFFFF) << 21) | uint64_t(z & 0x1FFFFF);
  }
  glm::i64vec3 at(const vec3& p) const { return {(int64_t)std::floor(p.x / cell), (int64_t)std::floor(p.y / cell), (int64_t)std::floor(p.z / cell)}; }
  void add(const vec3& p, uint32_t i) {
    auto c = at(p);
    m[key(c.x, c.y, c.z)].push_back(i);
  }
  template <class Fn>
  void near(const vec3& p, Fn&& fn) const {
    auto c = at(p);
    for (int dz = -1; dz <= 1; ++dz)
      for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
          auto it = m.find(key(c.x + dx, c.y + dy, c.z + dz));
          if (it != m.end())
            for (uint32_t i : it->second) fn(i);
        }
  }
};

float segDist(const vec3& p, const vec3& a, const vec3& b) {
  vec3 e = b - a;
  float l2 = glm::dot(e, e), t = l2 > 0 ? std::clamp(glm::dot(p - a, e) / l2, 0.f, 1.f) : 0.f;
  return glm::length(a + e * t - p);
}

// how far from what changed: d < reach is inside what is made again
struct ChangeDistance {
  float reach, L;
  std::vector<vec3> pts;
  PointGrid grid;
  struct Disc {
    vec3 c;
    float rho;
  };
  std::vector<Disc> discs;
  ChangeDistance(const LocalChange& ch, float L_, float reach_) : reach(reach_), L(L_) {
    const float step = 0.25f * L;
    for (auto& l : ch.lines)
      for (size_t i = 0; i < l.size(); ++i) {
        pts.push_back(l[i]);
        if (i + 1 < l.size()) {
          int n = (int)std::ceil(glm::length(l[i + 1] - l[i]) / step);
          for (int k = 1; k < n; ++k) pts.push_back(glm::mix(l[i], l[i + 1], float(k) / n));
        }
      }
    pts.insert(pts.end(), ch.points.begin(), ch.points.end());
    grid.cell = reach + 2 * L;
    for (uint32_t i = 0; i < pts.size(); ++i) grid.add(pts[i], i);
    // (the inside of a small loop: a disc round its middle; a loop round a limb has its middle inside the
    // limb, and the disc takes in a band of the limb either side, which does no harm)
    for (auto& l : ch.loops) {
      if (l.size() < 3) continue;
      vec3 c(0);
      for (auto& p : l) c += p / float(l.size());
      float rho = 0;
      for (auto& p : l) rho += glm::length(p - c) / float(l.size());
      if (rho < 12 * L) discs.push_back({c, rho});
    }
  }
  float operator()(const vec3& p) const {
    float d = grid.cell;
    grid.near(p, [&](uint32_t i) { d = std::min(d, glm::length(pts[i] - p)); });
    for (auto& dc : discs) d = std::min(d, glm::length(p - dc.c) - (dc.rho + 0.5f * reach) + reach);
    return d;
  }
};

// a set of faces made tidy at its outline where it meets the faces not in it (seam edges): no face with
// three or more seam edges, no vertex the outline passes twice, no small loose pieces. drop: which face
// goes when a vertex is passed twice (the one with the smallest score)
void tidy(const Faces& M, std::vector<char>& in, const std::vector<float>& score, int minPiece) {
  const uint32_t nf = (uint32_t)M.F.size();
  for (int round = 0; round < 40; ++round) {
    bool changed = false;
    std::unordered_map<uint32_t, std::vector<uint32_t>> outAt;  // vertex -> faces with a seam edge leaving it
    for (uint32_t f = 0; f < nf; ++f) {
      if (!in[f]) continue;
      const auto& V = M.F[f];
      int seam = 0;
      for (size_t k = 0; k < V.size(); ++k) {
        int g = M.across(V[k], V[(k + 1) % V.size()]);
        if (g >= 0 && !in[g]) ++seam, outAt[V[k]].push_back(f);
      }
      if (seam >= (int)V.size() - 1 && seam > 0) in[f] = 0, changed = true;
    }
    if (changed) continue;
    for (auto& [v, fs] : outAt) {
      if (fs.size() < 2) continue;
      uint32_t worst = fs[0];
      for (uint32_t f : fs)
        if (score[f] < score[worst]) worst = f;
      in[worst] = 0, changed = true;
    }
    if (!changed) break;
  }
  // small pieces (by edges between faces in the set) go
  std::vector<int> comp(nf, -1);
  for (uint32_t f0 = 0; f0 < nf; ++f0) {
    if (!in[f0] || comp[f0] >= 0) continue;
    std::vector<uint32_t> stack = {f0}, members;
    comp[f0] = (int)f0;
    while (!stack.empty()) {
      uint32_t f = stack.back();
      stack.pop_back();
      members.push_back(f);
      const auto& V = M.F[f];
      for (size_t k = 0; k < V.size(); ++k) {
        int g = M.across(V[k], V[(k + 1) % V.size()]);
        if (g >= 0 && in[g] && comp[g] < 0) comp[g] = (int)f0, stack.push_back((uint32_t)g);
      }
    }
    if ((int)members.size() < minPiece)
      for (uint32_t f : members) in[f] = 0;
  }
}

// the outline where a set of faces meets the faces of the same mesh not in it, as chains of vertices, each
// in the direction its faces run along it (the faces on its left); closed chains don't repeat the first
struct Chain {
  std::vector<uint32_t> v;
  bool closed = false;
};
std::vector<Chain> seamChains(const Faces& M, const std::vector<char>& in) {
  std::unordered_map<uint32_t, uint32_t> next;
  std::unordered_set<uint32_t> hasIn;
  for (uint32_t f = 0; f < M.F.size(); ++f) {
    if (!in[f]) continue;
    const auto& V = M.F[f];
    for (size_t k = 0; k < V.size(); ++k) {
      uint32_t a = V[k], b = V[(k + 1) % V.size()];
      int g = M.across(a, b);
      if (g >= 0 && !in[g]) next[a] = b, hasIn.insert(b);
    }
  }
  std::vector<Chain> out;
  std::unordered_set<uint32_t> used;
  auto walk = [&](uint32_t s, bool open) {
    Chain c;
    uint32_t v = s;
    for (size_t guard = 0; guard <= next.size() + 1; ++guard) {
      c.v.push_back(v);
      used.insert(v);
      auto it = next.find(v);
      if (it == next.end()) break;
      v = it->second;
      if (v == s) {
        c.closed = true;
        break;
      }
      if (used.count(v)) break;  // (shouldn't: tidy took such vertices out)
    }
    (void)open;
    if (c.v.size() >= 2) out.push_back(std::move(c));
  };
  std::vector<uint32_t> starts;
  for (auto& [a, b] : next)
    if (!hasIn.count(a)) starts.push_back(a);
  std::sort(starts.begin(), starts.end());
  for (uint32_t s : starts)
    if (!used.count(s)) walk(s, true);
  std::vector<uint32_t> rest;
  for (auto& [a, b] : next) rest.push_back(a);
  std::sort(rest.begin(), rest.end());
  for (uint32_t s : rest)
    if (!used.count(s)) walk(s, false);
  return out;
}

// which edge line (if any) a point lies on
struct LineFinder {
  float tol;
  std::vector<std::pair<vec3, vec3>> segs;
  std::vector<int> segLine;
  PointGrid grid;
  LineFinder(const std::vector<std::vector<vec3>>& lines, float L) : tol(0.05f * L) {
    grid.cell = L;
    for (int li = 0; li < (int)lines.size(); ++li)
      for (size_t i = 0; i + 1 < lines[li].size(); ++i) {
        uint32_t s = (uint32_t)segs.size();
        segs.push_back({lines[li][i], lines[li][i + 1]}), segLine.push_back(li);
        vec3 a = lines[li][i], b = lines[li][i + 1];
        int n = std::max(1, (int)std::ceil(glm::length(b - a) / (0.5f * L)));
        std::unordered_set<uint64_t> seen;
        for (int k = 0; k <= n; ++k) {
          vec3 p = glm::mix(a, b, float(k) / n);
          auto c = grid.at(p);
          if (seen.insert(PointGrid::key(c.x, c.y, c.z)).second) grid.add(p, s);
        }
      }
  }
  int at(const vec3& p) const {
    int best = -1;
    float bd = tol;
    grid.near(p, [&](uint32_t s) {
      float d = segDist(p, segs[s].first, segs[s].second);
      if (d < bd) bd = d, best = segLine[s];
    });
    return best;
  }
};

// a quad's shape: 1 square, down to 0 (a corner flat or turned in)
float quadShape(const vec3& a, const vec3& b, const vec3& c, const vec3& d) {
  vec3 n = glm::cross(c - a, d - b);
  float nl = glm::length(n);
  if (nl <= 0) return 0;
  n /= nl;
  const vec3 P[4] = {a, b, c, d};
  float worst = 1;
  for (int k = 0; k < 4; ++k) {
    vec3 e0 = P[(k + 1) % 4] - P[k], e1 = P[(k + 3) % 4] - P[k];
    e0 -= n * glm::dot(n, e0), e1 -= n * glm::dot(n, e1);
    float l0 = glm::length(e0), l1 = glm::length(e1);
    if (l0 <= 0 || l1 <= 0) return 0;
    float s = glm::dot(glm::cross(e0, e1), n) / (l0 * l1);  // sin of the corner's angle, signed (turned in: < 0)
    worst = std::min(worst, s);
  }
  return worst;
}

}  // namespace

bool keepAwayFromChange(const Mesh& old, const Mesh& fresh, const LocalChange& change, float L, float reach,
                        const std::vector<std::vector<vec3>>& edgeLines, const std::function<vec3(const vec3&)>& snap, Mesh& out,
                        LocalStats& st, std::vector<char>* near) {
  st = {};
  Faces O, N;
  O.read(old), N.read(fresh);
  if (O.F.empty() || N.F.empty()) return st.why = "no quads", false;
  ChangeDistance dist(change, L, reach);
  std::vector<float> dO(O.P.size()), dN(N.P.size());
  for (size_t i = 0; i < O.P.size(); ++i) dO[i] = dist(O.P[i]);
  for (size_t i = 0; i < N.P.size(); ++i) dN[i] = dist(N.P[i]);
  // the new faces wholly within the reach are taken; the old ones kept are those not within about two
  // thirds of a quad of them, so the row between is about a quad wide all round
  std::vector<char> keep(O.F.size(), 0), take(N.F.size(), 0);
  std::vector<float> scoreO(O.F.size()), scoreN(N.F.size());
  for (size_t f = 0; f < N.F.size(); ++f) {
    float hi = -1e30f, sum = 0;
    for (uint32_t v : N.F[f]) hi = std::max(hi, dN[v]), sum += dN[v];
    take[f] = hi < reach, scoreN[f] = -sum;  // (the one furthest from it goes first)
  }
  tidy(N, take, scoreN, 4);
  {
    PointGrid eg;
    eg.cell = L;
    std::vector<std::pair<vec3, vec3>> segs;
    std::unordered_set<EdgeKey> seen;
    for (size_t f = 0; f < N.F.size(); ++f)
      if (take[f])
        for (size_t k = 0; k < N.F[f].size(); ++k) {
          uint32_t a = N.F[f][k], b = N.F[f][(k + 1) % N.F[f].size()];
          if (!seen.insert(ukey(a, b)).second) continue;
          vec3 pa = N.P[a], pb = N.P[b];
          uint32_t si = (uint32_t)segs.size();
          segs.push_back({pa, pb});
          std::unordered_set<uint64_t> cells;
          for (int t = 0; t <= 4; ++t) {
            auto c = eg.at(glm::mix(pa, pb, t / 4.f));
            if (cells.insert(PointGrid::key(c.x, c.y, c.z)).second) eg.add(glm::mix(pa, pb, t / 4.f), si);
          }
        }
    const float clear = 0.66f * L;
    std::vector<char> nearNew(O.P.size(), 0);
    for (size_t v = 0; v < O.P.size(); ++v) {
      if (dO[v] > reach + 3 * L) continue;
      eg.near(O.P[v], [&](uint32_t si) {
        if (!nearNew[v] && segDist(O.P[v], segs[si].first, segs[si].second) < clear) nearNew[v] = 1;
      });
    }
    for (size_t f = 0; f < O.F.size(); ++f) {
      bool k = true;
      float sum = 0;
      for (uint32_t v : O.F[f]) k &= !nearNew[v] && dO[v] > 0.5f * reach, sum += dO[v];
      keep[f] = k, scoreO[f] = sum;  // (the one nearest the change goes first)
    }
  }
  tidy(O, keep, scoreO, 12);
  for (char k : keep) st.keptFaces += k;
  for (char t : take) st.newFaces += t;
  auto dumpParts = [&](const char* why) {
    const char* dbg = getenv("PATINA_LOCAL_DEBUG");
    if (!dbg) return;
    static int n = 0;
    std::string path = std::string(dbg) + "_fail" + std::to_string(n++) + ".obj";
    if (FILE* fo = fopen(path.c_str(), "w")) {
      fprintf(fo, "# %s\n", why);
      for (auto& p : O.P) fprintf(fo, "v %f %f %f\n", p.x, p.y, p.z);
      for (auto& p : N.P) fprintf(fo, "v %f %f %f\n", p.x, p.y, p.z);
      fprintf(fo, "g kept\n");
      for (size_t f = 0; f < O.F.size(); ++f)
        if (keep[f]) {
          fprintf(fo, "f");
          for (uint32_t v : O.F[f]) fprintf(fo, " %u", v + 1);
          fprintf(fo, "\n");
        }
      fprintf(fo, "g new\n");
      for (size_t f = 0; f < N.F.size(); ++f)
        if (take[f]) {
          fprintf(fo, "f");
          for (uint32_t v : N.F[f]) fprintf(fo, " %u", (uint32_t)(v + 1 + O.P.size()));
          fprintf(fo, "\n");
        }
      fclose(fo);
    }
  };
  if (!st.newFaces) return st.why = "nothing near the change", false;
  if (!st.keptFaces) return st.why = "nothing far from the change", false;
  auto chO = seamChains(O, keep), chN = seamChains(N, take);
  if (chO.size() != chN.size() || chO.empty()) {
    st.why = "the outlines don't pair up (" + std::to_string(chO.size()) + " / " + std::to_string(chN.size()) + ")";
    dumpParts(st.why.c_str());
    return false;
  }
  // each kept outline with the new one beside it
  PointGrid gN;
  gN.cell = 2 * L;
  std::vector<std::pair<uint32_t, uint32_t>> nAt;  // (chain, index)
  for (uint32_t c = 0; c < chN.size(); ++c)
    for (uint32_t i = 0; i < chN[c].v.size(); ++i) gN.add(N.P[chN[c].v[i]], (uint32_t)nAt.size()), nAt.push_back({c, i});
  std::vector<int> pairOf(chO.size(), -1);
  std::vector<char> nUsed(chN.size(), 0);
  for (uint32_t c = 0; c < chO.size(); ++c) {
    std::map<uint32_t, int> votes;
    double sum = 0;
    int n = 0;
    for (uint32_t v : chO[c].v) {
      float bd = 3 * L;
      int bi = -1;
      gN.near(O.P[v], [&](uint32_t k) {
        float d = glm::length(N.P[chN[nAt[k].first].v[nAt[k].second]] - O.P[v]);
        if (d < bd) bd = d, bi = (int)k;
      });
      if (bi >= 0) votes[nAt[bi].first]++, sum += bd, ++n;
    }
    int best = -1, bv = 0;
    for (auto& [k, cnt] : votes)
      if (cnt > bv) bv = cnt, best = (int)k;
    if (best < 0 || nUsed[best] || chN[best].closed != chO[c].closed || n < 0.8 * chO[c].v.size()) {
      st.why = "an outline has no new one beside it (" + std::to_string(n) + " of " + std::to_string(chO[c].v.size()) + " near, " +
               (best >= 0 && chN[best].closed != chO[c].closed ? "one closed, one not" : "") + ")";
      dumpParts(st.why.c_str());
      return false;
    }
    pairOf[c] = best, nUsed[best] = 1;
  }
  // ---- the vertices of the result: the kept ones, then the taken ones
  std::vector<int> mapO(O.P.size(), -1), mapN(N.P.size(), -1);
  out = Mesh();
  std::vector<vec3>& P = out.positions;
  std::vector<std::vector<uint32_t>> faces;
  std::vector<char> fromOld;
  for (size_t f = 0; f < O.F.size(); ++f) {
    if (!keep[f]) continue;
    std::vector<uint32_t> fv;
    for (uint32_t v : O.F[f]) {
      if (mapO[v] < 0) mapO[v] = (int)P.size(), P.push_back(O.P[v]);
      fv.push_back((uint32_t)mapO[v]);
    }
    faces.push_back(fv), fromOld.push_back(1);
  }
  const size_t firstNew = P.size();
  for (size_t f = 0; f < N.F.size(); ++f) {
    if (!take[f]) continue;
    std::vector<uint32_t> fv;
    for (uint32_t v : N.F[f]) {
      if (mapN[v] < 0) mapN[v] = (int)P.size(), P.push_back(N.P[v]);
      fv.push_back((uint32_t)mapN[v]);
    }
    faces.push_back(fv), fromOld.push_back(0);
  }
  // each vertex's edges in its own part (to give the row's vertices four edges where they can)
  std::vector<int> deg(P.size(), 0);
  std::vector<char> onOpen(P.size(), 0);
  {
    std::unordered_map<EdgeKey, int> uses;
    for (auto& f : faces)
      for (size_t k = 0; k < f.size(); ++k) uses[ukey(f[k], f[(k + 1) % f.size()])]++;
    for (auto& [e, n] : uses) deg[uint32_t(e >> 32)]++, deg[uint32_t(e & 0xFFFFFFFF)]++;
  }
  // (on the model's own open edge: an edge of the old or new quads with nothing across it)
  for (size_t f = 0; f < O.F.size(); ++f)
    if (keep[f])
      for (size_t k = 0; k < O.F[f].size(); ++k) {
        uint32_t a = O.F[f][k], b = O.F[f][(k + 1) % O.F[f].size()];
        if (O.across(a, b) < 0) onOpen[mapO[a]] = onOpen[mapO[b]] = 1;
      }
  for (size_t f = 0; f < N.F.size(); ++f)
    if (take[f])
      for (size_t k = 0; k < N.F[f].size(); ++k) {
        uint32_t a = N.F[f][k], b = N.F[f][(k + 1) % N.F[f].size()];
        if (N.across(a, b) < 0) onOpen[mapN[a]] = onOpen[mapN[b]] = 1;
      }
  LineFinder lf(edgeLines, L);
  const float P1 = 4.f, P2 = 10.f;  // (a pole; worse)
  auto pen = [&](uint32_t v, int rungs) {
    int k = deg[v] + rungs, want = onOpen[v] ? 3 : 4, d = std::abs(k - want);
    return d == 0 ? 0.f : d == 1 ? P1 : P2 * d;
  };
  std::vector<char> rowV(P.size(), 0);
  // ---- one row for each pair of outlines
  for (uint32_t c = 0; c < chO.size(); ++c) {
    std::vector<uint32_t> Fv, Cv;
    for (uint32_t v : chO[c].v) Fv.push_back((uint32_t)mapO[v]);
    for (uint32_t v : chN[pairOf[c]].v) Cv.push_back((uint32_t)mapN[v]);
    std::reverse(Cv.begin(), Cv.end());  // (the same way as the kept one: the row between them)
    const bool closed = chO[c].closed;
    const size_t nF = Fv.size(), nC = Cv.size();
    ++st.seams;
    // where both cross an edge line: the line runs on across the row there (a rung along it)
    std::vector<int> lineF(nF), lineC(nC);
    for (size_t i = 0; i < nF; ++i) lineF[i] = lf.at(P[Fv[i]]);
    for (size_t j = 0; j < nC; ++j) lineC[j] = lf.at(P[Cv[j]]);
    std::vector<std::pair<size_t, size_t>> fixedR;  // (index in F, in C)
    if (closed) {
      // (start where they are nearest, or at a line both cross)
      size_t bi = 0, bj = 0;
      float bd = 1e30f;
      bool onLine = false;
      for (size_t i = 0; i < nF; ++i)
        for (size_t j = 0; j < nC; ++j) {
          bool l = lineF[i] >= 0 && lineF[i] == lineC[j];
          float d = glm::length(P[Fv[i]] - P[Cv[j]]);
          if (d > 2.5f * L) continue;
          if ((l && !onLine) || (l == onLine && d < bd)) bd = d, bi = i, bj = j, onLine = l;
        }
      if (bd > 1e29f) return st.why = "an outline is too far from the new one", false;
      std::rotate(Fv.begin(), Fv.begin() + bi, Fv.end()), std::rotate(lineF.begin(), lineF.begin() + bi, lineF.end());
      std::rotate(Cv.begin(), Cv.begin() + bj, Cv.end()), std::rotate(lineC.begin(), lineC.begin() + bj, lineC.end());
      Fv.push_back(Fv[0]), Cv.push_back(Cv[0]), lineF.push_back(lineF[0]), lineC.push_back(lineC[0]);
    } else if (glm::length(P[Fv[0]] - P[Cv[0]]) > 3 * L || glm::length(P[Fv.back()] - P[Cv.back()]) > 3 * L) {
      return st.why = "an outline's ends are far from the new one's", false;
    }
    const size_t a = Fv.size() - 1, b = Cv.size() - 1;
    fixedR.push_back({0, 0});
    {
      size_t lastJ = 0;
      for (size_t i = 1; i < a; ++i) {
        if (lineF[i] < 0) continue;
        size_t bj = 0;
        float bd = 2 * L;
        for (size_t j = lastJ + 1; j < b; ++j)
          if (lineC[j] == lineF[i])
            if (float d = glm::length(P[Fv[i]] - P[Cv[j]]); d < bd) bd = d, bj = j;
        if (bj) fixedR.push_back({i, bj}), lastJ = bj, ++st.linesAcross;
      }
    }
    fixedR.push_back({a, b});
    // the row along each run between fixed rungs: a search over (F used, C used, rungs at the last F vertex, at
    // the last C vertex) for the cheapest: poles, then shape
    for (size_t r = 0; r + 1 < fixedR.size();) {
      const size_t i0 = fixedR[r].first, j0 = fixedR[r].second, ia = fixedR[r + 1].first - i0, jb = fixedR[r + 1].second - j0;
      auto f = [&](size_t i) { return Fv[i0 + i]; };
      auto cv = [&](size_t j) { return Cv[j0 + j]; };
      const size_t W = ia + 1, H = jb + 1;
      auto idx = [&](size_t i, size_t j, int ri, int rj) { return ((j * W + i) * 3 + (ri - 1)) * 3 + (rj - 1); };
      std::vector<float> cost(W * H * 9, 1e30f);
      std::vector<int32_t> back(W * H * 9, -1);
      std::vector<int8_t> how(W * H * 9, 0);
      cost[idx(0, 0, 1, 1)] = 0;
      auto rungCost = [&](size_t i, size_t j) {
        float l = glm::length(P[f(i)] - P[cv(j)]) / L;
        return 0.5f * (l - 1) * (l - 1) + (l > 3 ? 10.f : 0.f);
      };
      auto shapeCost = [&](const vec3& p0, const vec3& p1, const vec3& p2, const vec3& p3) {
        float s = quadShape(p0, p1, p2, p3);
        return 2 * (1 - s) + (s < 0.1f ? 6.f : 0.f);
      };
      for (size_t j = 0; j < H; ++j)
        for (size_t i = 0; i < W; ++i)
          for (int ri = 1; ri <= 3; ++ri)
            for (int rj = 1; rj <= 3; ++rj) {
              const size_t s = idx(i, j, ri, rj);
              const float c0 = cost[s];
              if (c0 >= 1e29f) continue;
              auto relax = [&](size_t i2, size_t j2, int ri2, int rj2, float add, int mv) {
                if (i2 > ia || j2 > jb || ri2 > 3 || rj2 > 3) return;
                size_t t = idx(i2, j2, ri2, rj2);
                if (c0 + add < cost[t]) cost[t] = c0 + add, back[t] = (int32_t)s, how[t] = (int8_t)mv;
              };
              // A: one edge of each
              if (i < ia && j < jb)
                relax(i + 1, j + 1, 1, 1, pen(f(i), ri) + pen(cv(j), rj) + rungCost(i + 1, j + 1) + shapeCost(P[f(i + 1)], P[f(i)], P[cv(j)], P[cv(j + 1)]), 1);
              // B: two of F round one vertex of C (C's outline turns there); C: two of C round one vertex of F. A
              // run's first vertices take no more rungs than the one it starts with (the run before decides
              // theirs), its last ones may
              if (i + 2 <= ia && j > 0)
                relax(i + 2, j, 1, rj + 1, pen(f(i), ri) + pen(f(i + 1), 0) + rungCost(i + 2, j) + shapeCost(P[f(i + 2)], P[f(i + 1)], P[f(i)], P[cv(j)]), 2);
              if (j + 2 <= jb && i > 0)
                relax(i, j + 2, ri + 1, 1, pen(cv(j), rj) + pen(cv(j + 1), 0) + rungCost(i, j + 2) + shapeCost(P[f(i)], P[cv(j)], P[cv(j + 1)], P[cv(j + 2)]), 3);
              // triangles (an odd count)
              if (i < ia && j > 0) relax(i + 1, j, 1, rj + 1, pen(f(i), ri) + rungCost(i + 1, j) + 100.f, 4);
              if (j < jb && i > 0) relax(i, j + 1, ri + 1, 1, pen(cv(j), rj) + rungCost(i, j + 1) + 100.f, 5);
            }
      size_t endS = 0;
      float endC = 1e30f;
      for (int ri = 1; ri <= 3; ++ri)
        for (int rj = 1; rj <= 3; ++rj) {
          size_t s = idx(ia, jb, ri, rj);
          if (cost[s] >= 1e29f) continue;
          float c2 = cost[s] + pen(f(ia), ri) + pen(cv(jb), rj);
          if (c2 < endC) endC = c2, endS = s;
        }
      if (endC >= 1e29f) {
        // (a line's crossing paired wrong: the run joined with the next)
        if (r + 2 < fixedR.size()) {
          fixedR.erase(fixedR.begin() + r + 1);
          --st.linesAcross;
          continue;
        }
        st.why = "no row between two outlines (" + std::to_string(ia) + " / " + std::to_string(jb) + " edges)";
        dumpParts(st.why.c_str());
        return false;
      }
      // the faces, from the end back
      std::vector<std::pair<int, std::pair<size_t, size_t>>> moves;  // (move, the state it came from: i, j)
      for (size_t s = endS; back[s] >= 0; s = (size_t)back[s]) {
        size_t from = (size_t)back[s];
        size_t fi = (from / 9) % W, fj = (from / 9) / W;
        moves.push_back({how[s], {fi, fj}});
      }
      std::reverse(moves.begin(), moves.end());
      for (auto& [mv, ij] : moves) {
        size_t i = ij.first, j = ij.second;
        switch (mv) {
          case 1: faces.push_back({f(i + 1), f(i), cv(j), cv(j + 1)}); break;
          case 2: faces.push_back({f(i + 2), f(i + 1), f(i), cv(j)}); break;
          case 3: faces.push_back({f(i), cv(j), cv(j + 1), cv(j + 2)}); break;
          case 4: faces.push_back({f(i + 1), f(i), cv(j)}), ++st.rowTriangles; break;
          case 5: faces.push_back({f(i), cv(j), cv(j + 1)}), ++st.rowTriangles; break;
        }
        fromOld.push_back(0), ++st.rowFaces;
        for (uint32_t v : faces.back()) rowV[v] = 1;
      }
      ++r;
    }
  }
  if (const char* dbg = getenv("PATINA_LOCAL_DEBUG")) {
    static int n = 0;
    std::string path = std::string(dbg) + "_" + std::to_string(n++) + ".obj";
    if (FILE* fo = fopen(path.c_str(), "w")) {
      for (auto& p : P) fprintf(fo, "v %f %f %f\n", p.x, p.y, p.z);
      size_t k = 0, keptN = (size_t)st.keptFaces, newN = (size_t)st.newFaces;
      for (auto& fc : faces) {
        if (k == 0) fprintf(fo, "g kept\n");
        if (k == keptN) fprintf(fo, "g new\n");
        if (k == keptN + newN) fprintf(fo, "g row\n");
        fprintf(fo, "f");
        for (uint32_t v : fc) fprintf(fo, " %u", v + 1);
        fprintf(fo, "\n");
        ++k;
      }
      fclose(fo);
    }
  }
  // ---- checks: every edge on at most two faces, each way once
  {
    // (only the row's edges: the kept and new quads come as they were, a collapsed corner and all)
    std::unordered_map<EdgeKey, int> uses;
    const size_t rowFrom = (size_t)(st.keptFaces + st.newFaces);
    for (size_t fi = 0; fi < faces.size(); ++fi) {
      auto& fc = faces[fi];
      for (size_t k = 0; k < fc.size(); ++k)
        if (fc[k] != fc[(k + 1) % fc.size()] && uses[dkey(fc[k], fc[(k + 1) % fc.size()])]++ && fi >= rowFrom) {
          if (getenv("PATINA_LOCAL_DEBUG")) {
            uint32_t a = fc[k], b = fc[(k + 1) % fc.size()];
            for (size_t gi = 0; gi < faces.size(); ++gi)
              for (size_t m = 0; m < faces[gi].size(); ++m)
                if (faces[gi][m] == a && faces[gi][(m + 1) % faces[gi].size()] == b)
                  PLOG_I("  doubled edge %u-%u (%s, %s) in face %zu (%s, %zu corners)", a, b, a < firstNew ? "old" : "new", b < firstNew ? "old" : "new",
                         gi, gi < (size_t)st.keptFaces ? "kept" : gi < (size_t)(st.keptFaces + st.newFaces) ? "new" : "row", faces[gi].size());
          }
          st.why = "the row doubled an edge";
          dumpParts(st.why.c_str());
          return false;
        }
    }
  }
  // ---- the new side of the row (and a ring inside it) smoothed, on the surface; the kept quads stay put
  {
    std::vector<std::vector<uint32_t>> nb(P.size());
    for (auto& fc : faces)
      for (size_t k = 0; k < fc.size(); ++k) {
        uint32_t x = fc[k], y = fc[(k + 1) % fc.size()];
        if (std::find(nb[x].begin(), nb[x].end(), y) == nb[x].end()) nb[x].push_back(y), nb[y].push_back(x);
      }
    std::vector<char> moves(P.size(), 0);
    for (size_t v = firstNew; v < P.size(); ++v)
      if (rowV[v] && !onOpen[v] && lf.at(P[v]) < 0) moves[v] = 1;
    for (size_t v = firstNew; v < P.size(); ++v)
      if (!moves[v] && !rowV[v] && !onOpen[v] && lf.at(P[v]) < 0)
        for (uint32_t w : nb[v])
          if (rowV[w] && w >= firstNew) {
            moves[v] = 2;
            break;
          }
    for (int it = 0; it < 8; ++it) {
      std::vector<vec3> next = P;
      for (size_t v = firstNew; v < P.size(); ++v) {
        if (!moves[v] || nb[v].empty()) continue;
        vec3 m(0);
        for (uint32_t w : nb[v]) m += P[w] / float(nb[v].size());
        next[v] = glm::mix(P[v], m, 0.5f);
        if (snap) next[v] = snap(next[v]);
      }
      P = std::move(next);
    }
  }
  // the row's vertices with other than four edges (three on an open edge)
  {
    std::vector<std::unordered_set<uint32_t>> nb(P.size());
    for (auto& fc : faces)
      for (size_t k = 0; k < fc.size(); ++k) nb[fc[k]].insert(fc[(k + 1) % fc.size()]), nb[fc[(k + 1) % fc.size()]].insert(fc[k]);
    for (size_t v = 0; v < P.size(); ++v)
      if (rowV[v]) st.rowPoles += (int)nb[v].size() != (onOpen[v] ? 3 : 4);
  }
  if (near) {
    near->assign(P.size(), 0);
    std::vector<std::vector<uint32_t>> nb(P.size());
    for (auto& fc : faces)
      for (size_t k = 0; k < fc.size(); ++k) nb[fc[k]].push_back(fc[(k + 1) % fc.size()]), nb[fc[(k + 1) % fc.size()]].push_back(fc[k]);
    for (size_t v = 0; v < P.size(); ++v) (*near)[v] = rowV[v];
    for (int ring = 0; ring < 3; ++ring) {
      std::vector<char> next = *near;
      for (size_t v = 0; v < P.size(); ++v)
        if ((*near)[v])
          for (uint32_t w : nb[v]) next[w] = 1;
      *near = std::move(next);
    }
  }
  out.faceStart.push_back(0);
  for (auto& fc : faces) {
    out.loopVert.insert(out.loopVert.end(), fc.begin(), fc.end());
    out.faceStart.push_back((uint32_t)out.loopVert.size());
  }
  out.autoSmoothAngleDeg = 180.f;
  out.finalize();
  return true;
}

}  // namespace patina::retopo
