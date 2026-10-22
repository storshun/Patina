#include "core/RetopoMirror.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace patina::retopo {

vec3 mirrorPoint(const vec3& p, int mask, const vec3& center) {
  vec3 q = p;
  for (int k = 0; k < 3; ++k)
    if (mask & (1 << k)) q[k] = 2.f * center[k] - q[k];
  return q;
}

std::vector<SectionPiece> middleSection(const Mesh& m, int axis, float at) {
  // each triangle the plane crosses gives a segment between two of its edges (a corner on the plane counts as
  // on the far side: no triangle gives a point alone); segments are chained through the edges they share
  auto key = [](uint32_t a, uint32_t b) { return a < b ? (uint64_t(a) << 32) | b : (uint64_t(b) << 32) | a; };
  std::unordered_map<uint64_t, std::vector<uint64_t>> at2;  // edge -> the edges it is joined to
  std::unordered_map<uint64_t, vec3> pointOf;
  for (const auto& t : m.tris) {
    uint32_t v[3] = {m.loopVert[t[0]], m.loopVert[t[1]], m.loopVert[t[2]]};
    float d[3];
    for (int k = 0; k < 3; ++k) d[k] = m.positions[v[k]][axis] - at;
    uint64_t ek[2];
    int got = 0;
    for (int k = 0; k < 3 && got < 2; ++k) {
      int j = (k + 1) % 3;
      if ((d[k] < 0) == (d[j] < 0)) continue;
      uint64_t e = key(v[k], v[j]);
      if (!pointOf.count(e)) {
        vec3 p = glm::mix(m.positions[v[k]], m.positions[v[j]], d[k] / (d[k] - d[j]));
        p[axis] = at;
        pointOf[e] = p;
      }
      ek[got++] = e;
    }
    if (got != 2 || ek[0] == ek[1]) continue;
    at2[ek[0]].push_back(ek[1]), at2[ek[1]].push_back(ek[0]);
  }
  std::vector<SectionPiece> out;
  std::unordered_map<uint64_t, char> used;
  auto walk = [&](uint64_t e0) {
    SectionPiece piece;
    uint64_t prev = e0, cur = e0;
    for (size_t guard = 0; guard <= at2.size(); ++guard) {
      used[cur] = 1;
      const vec3& p = pointOf[cur];
      if (piece.points.empty() || piece.points.back() != p) piece.points.push_back(p);
      const auto& list = at2[cur];
      if (list.size() > 2 && cur != e0) break;  // (more than two pieces meet: each ends here)
      uint64_t next = 0;
      bool found = false;
      for (uint64_t o : list)
        if (o != prev && !used.count(o)) {
          next = o, found = true;
          break;
        }
      if (!found) {
        for (uint64_t o : list) piece.closed |= o == e0 && cur != e0 && at2[e0].size() == 2;
        break;
      }
      prev = cur, cur = next;
    }
    if (piece.closed && piece.points.size() > 1 && piece.points.front() == piece.points.back()) piece.points.pop_back();
    if (piece.points.size() >= 2) out.push_back(std::move(piece));
  };
  // open pieces from their ends first, then the loops
  for (auto& [e, list] : at2)
    if (list.size() != 2 && !used.count(e)) walk(e);
  for (auto& [e, list] : at2)
    if (!used.count(e)) walk(e);
  return out;
}

namespace {

// every point of A within tol of the polyline B (closed: B's last point joins its first)
bool near(const std::vector<vec3>& A, const std::vector<vec3>& B, bool closed, float tol) {
  if (B.size() < 2) return false;
  for (const vec3& p : A) {
    float bd = 1e30f;
    const size_t n = closed ? B.size() : B.size() - 1;
    for (size_t i = 0; i < n && bd > tol * tol; ++i) {
      vec3 a = B[i], b = B[(i + 1) % B.size()], ab = b - a;
      float l2 = glm::dot(ab, ab), t = l2 > 0 ? std::clamp(glm::dot(p - a, ab) / l2, 0.f, 1.f) : 0.f;
      vec3 q = a + t * ab;
      bd = std::min(bd, glm::dot(q - p, q - p));
    }
    if (bd > tol * tol) return false;
  }
  return true;
}

std::vector<Stroke> mirrorOne(const Stroke& st, int k, float c, float dz) {
  std::vector<vec3> P = st.points;
  if (P.size() < 2) return {st};
  // (put on the middle: all of a stroke drawn along it, else only an open stroke's ends near it, so it
  // joins its mirror image there; points in between keep their place, or a small loop near the middle
  // would fold flat onto it)
  bool allNear = true;
  for (auto& p : P) allNear &= std::abs(p[k] - c) < dz;
  if (allNear)
    for (auto& p : P) p[k] = c;
  else if (!st.loop)
    for (vec3* p : {&P.front(), &P.back()})
      if (std::abs((*p)[k] - c) < dz) (*p)[k] = c;
  auto side = [&](const vec3& p) { return p[k] > c ? 1 : p[k] < c ? -1 : 0; };
  const bool loop = st.loop && P.size() > 2;
  // how much of it is on each side
  double len[2] = {0, 0};
  bool has[2] = {false, false};
  size_t n = P.size() + (loop ? 1 : 0);
  for (size_t i = 0; i + 1 < n; ++i) {
    const vec3 &a = P[i], &b = P[(i + 1) % P.size()];
    int s = side(0.5f * (a + b));
    if (s) len[s > 0] += glm::length(b - a);
  }
  for (auto& p : P)
    if (int s = side(p)) has[s > 0] = true;
  auto copyOf = [&](std::vector<vec3> pts, bool closed) {
    Stroke out = st;
    out.points = std::move(pts), out.loop = closed;
    return out;
  };
  auto mirrored = [&](std::vector<vec3> pts) {
    for (auto& p : pts) p[k] = 2.f * c - p[k];
    return pts;
  };
  if (!has[0] && !has[1]) return {copyOf(P, loop)};  // along the middle: it is its own mirror image
  const int keep = has[0] && has[1] ? (len[1] >= len[0] ? 1 : -1) : (has[1] ? 1 : -1);
  if (!has[keep > 0 ? 0 : 1]) {
    // all on one side (perhaps reaching the middle): handled below as one run, unless a closed loop
    if (loop) {
      // (a loop that is its own mirror image, within the deadzone: one loop, not two on top of each other)
      std::vector<vec3> M = mirrored(P);
      if (near(M, P, true, std::max(dz, 1e-6f))) return {copyOf(P, true)};
      return {copyOf(P, true), copyOf(M, true)};
    }
  }
  // the points in order (a closed loop from a point off the kept side, and back to it)
  std::vector<vec3> seq;
  if (loop) {
    size_t start = 0;
    for (size_t i = 0; i < P.size(); ++i)
      if (side(P[i]) == -keep) {
        start = i;
        break;
      }
    for (size_t i = 0; i <= P.size(); ++i) seq.push_back(P[(start + i) % P.size()]);
  } else {
    seq = P;
  }
  auto crossing = [&](const vec3& a, const vec3& b) {
    float t = (c - a[k]) / (b[k] - a[k]);
    vec3 q = a + t * (b - a);
    q[k] = c;
    return q;
  };
  // the runs on the kept side, each from and to where it reaches the middle
  std::vector<std::vector<vec3>> runs;
  std::vector<vec3> cur;
  for (size_t i = 0; i < seq.size(); ++i) {
    const vec3& p = seq[i];
    int s = side(p);
    if (s != -keep) {
      if (cur.empty() && i > 0 && side(seq[i - 1]) == -keep && s == keep) cur.push_back(crossing(seq[i - 1], p));
      if (cur.empty() || glm::length(cur.back() - p) > 0) cur.push_back(p);
    } else if (!cur.empty()) {
      if (side(cur.back()) == keep) cur.push_back(crossing(cur.back(), p));
      runs.push_back(std::move(cur));
      cur.clear();
    }
  }
  if (!cur.empty()) runs.push_back(std::move(cur));
  std::vector<Stroke> out;
  for (auto& run : runs) {
    if (run.size() < 2) continue;
    bool off = false;
    for (auto& p : run) off |= side(p) != 0;
    if (!off) continue;  // (only along the middle: nothing on the kept side)
    bool onStart = run.front()[k] == c, onEnd = run.back()[k] == c;
    std::vector<vec3> M = mirrored(run);
    if (onStart && onEnd) {
      // reaching the middle at both ends: with its mirror image, one closed loop
      std::vector<vec3> pts = run;
      for (size_t i = M.size() - 1; i-- > 1;) pts.push_back(M[i]);
      out.push_back(copyOf(pts, true));
    } else if (onEnd) {
      std::vector<vec3> pts = run;
      for (size_t i = M.size() - 1; i-- > 0;) pts.push_back(M[i]);
      out.push_back(copyOf(pts, false));
    } else if (onStart) {
      std::vector<vec3> pts;
      for (size_t i = M.size(); i-- > 1;) pts.push_back(M[i]);
      pts.insert(pts.end(), run.begin(), run.end());
      out.push_back(copyOf(pts, false));
    } else {
      out.push_back(copyOf(run, false));
      if (!near(M, run, false, std::max(dz, 1e-6f))) out.push_back(copyOf(M, false));
    }
  }
  if (out.empty()) return {copyOf(P, loop)};
  return out;
}

}  // namespace

std::vector<Stroke> mirrorStroke(const Stroke& st, int axes, const vec3& center, float deadzone) {
  std::vector<Stroke> list = {st};
  for (int k = 0; k < 3; ++k) {
    if (!(axes & (1 << k))) continue;
    std::vector<Stroke> next;
    for (auto& s : list)
      for (auto& m : mirrorOne(s, k, center[k], deadzone)) next.push_back(std::move(m));
    list = std::move(next);
  }
  return list;
}

}  // namespace patina::retopo
