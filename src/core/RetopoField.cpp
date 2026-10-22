#include "core/RetopoField.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <tuple>
#include <unordered_map>

#include "core/Parallel.h"

namespace patina::retopo {

namespace {

using cplx = std::complex<double>;

// faces on each edge
struct EdgeFaces {
  std::vector<uint32_t> start, items;
  uint32_t size(uint32_t e) const { return start[e + 1] - start[e]; }
  const uint32_t* begin(uint32_t e) const { return items.data() + start[e]; }
};

EdgeFaces edgeFaces(const Mesh& m) {
  EdgeFaces c;
  c.start.assign(m.edges.size() + 1, 0);
  for (uint32_t l = 0; l < m.loopCount(); ++l) c.start[m.loopEdge[l] + 1]++;
  for (size_t i = 1; i < c.start.size(); ++i) c.start[i] += c.start[i - 1];
  c.items.resize(m.loopCount());
  std::vector<uint32_t> fill(c.start.begin(), c.start.end() - 1);
  for (uint32_t f = 0; f < m.faceCount(); ++f)
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) c.items[fill[m.loopEdge[l]]++] = f;
  return c;
}

// the angle of a vector in a face's frame
double angleIn(const CrossField& F, uint32_t f, vec3 d) {
  vec3 b = glm::cross(F.normal[f], F.axis[f]);
  return std::atan2((double)glm::dot(d, b), (double)glm::dot(d, F.axis[f]));
}

double wrapQuarter(double a) {  // to (-pi/4, pi/4]
  const double q = kPi / 2;
  a = std::fmod(a + kPi / 4, q);
  if (a <= 0) a += q;
  return a - kPi / 4;
}

// Hermitian positive definite system, rows of (column, value), solved with Jacobi-preconditioned CG
struct HermitianSystem {
  std::vector<uint32_t> start, col;
  std::vector<cplx> val;
  std::vector<double> diag;
  void mul(const std::vector<cplx>& x, std::vector<cplx>& y) const {
    parallelFor(diag.size(), [&](size_t i) {
      cplx s = diag[i] * x[i];
      for (uint32_t k = start[i]; k < start[i + 1]; ++k) s += val[k] * x[col[k]];
      y[i] = s;
    });
  }
};

void solveCG(const HermitianSystem& A, const std::vector<cplx>& b, std::vector<cplx>& x, int maxIt, double tol) {
  size_t n = b.size();
  std::vector<cplx> r(n), z(n), p(n), q(n);
  A.mul(x, q);
  double bn = 0;
  for (size_t i = 0; i < n; ++i) r[i] = b[i] - q[i], bn += std::norm(b[i]);
  bn = std::sqrt(std::max(bn, 1e-300));
  for (size_t i = 0; i < n; ++i) z[i] = r[i] / A.diag[i], p[i] = z[i];
  cplx rz = 0;
  for (size_t i = 0; i < n; ++i) rz += std::conj(r[i]) * z[i];
  for (int it = 0; it < maxIt; ++it) {
    A.mul(p, q);
    cplx pq = 0;
    for (size_t i = 0; i < n; ++i) pq += std::conj(p[i]) * q[i];
    if (std::abs(pq) < 1e-300) break;
    cplx a = rz / pq;
    double rn = 0;
    for (size_t i = 0; i < n; ++i) x[i] += a * p[i], r[i] -= a * q[i], rn += std::norm(r[i]);
    if (std::sqrt(rn) < tol * bn) break;
    cplx rz2 = 0;
    for (size_t i = 0; i < n; ++i) z[i] = r[i] / A.diag[i], rz2 += std::conj(r[i]) * z[i];
    cplx beta = rz2 / rz;
    rz = rz2;
    for (size_t i = 0; i < n; ++i) p[i] = z[i] + beta * p[i];
  }
}

// ---- multilevel: faces paired with their most strongly joined neighbor, level by level; a coarse
// unknown stands for its faces with the turn between them built in (z_fine = phase * z_coarse), so
// the coarse systems (P^H A P) keep the field's geometry. Solved coarse first, each level's answer
// then refined by a few CG steps on the next finer one.
struct Level {
  HermitianSystem A;
  std::vector<uint32_t> parent;  // node -> node of the next coarser level
  std::vector<cplx> phase;
};

HermitianSystem coarsen(const HermitianSystem& A, std::vector<uint32_t>& parent, std::vector<cplx>& phase) {
  size_t n = A.diag.size();
  parent.assign(n, ~0u), phase.assign(n, cplx(1, 0));
  std::vector<uint32_t> order(n);
  for (size_t i = 0; i < n; ++i) order[i] = (uint32_t)i;
  for (size_t i = n; i > 1; --i) std::swap(order[i - 1], order[(i * 2654435761u) % i]);
  uint32_t next = 0;
  for (uint32_t i : order) {
    if (parent[i] != ~0u) continue;
    uint32_t best = ~0u;
    double bw = 0;
    cplx bv;
    for (uint32_t k = A.start[i]; k < A.start[i + 1]; ++k) {
      uint32_t j = A.col[k];
      if (parent[j] != ~0u || j == i) continue;
      if (double w = std::abs(A.val[k]); w > bw) bw = w, best = j, bv = A.val[k];
    }
    parent[i] = next;
    if (best != ~0u) parent[best] = next, phase[best] = -std::conj(bv) / bw;  // the turn that costs nothing
    ++next;
  }
  HermitianSystem C;
  C.diag.assign(next, 0.0);
  std::vector<std::tuple<uint32_t, uint32_t, cplx>> el;
  el.reserve(A.col.size());
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t ci = parent[i];
    C.diag[ci] += A.diag[i];
    for (uint32_t k = A.start[i]; k < A.start[i + 1]; ++k) {
      uint32_t j = A.col[k], cj = parent[j];
      cplx v = std::conj(phase[i]) * A.val[k] * phase[j];
      if (ci == cj) C.diag[ci] += v.real();
      else el.emplace_back(ci, cj, v);
    }
  }
  std::sort(el.begin(), el.end(), [](const auto& a, const auto& b) {
    return std::get<0>(a) != std::get<0>(b) ? std::get<0>(a) < std::get<0>(b) : std::get<1>(a) < std::get<1>(b);
  });
  C.start.assign(next + 1, 0);
  for (size_t i = 0; i < el.size();) {
    auto [a, b, v] = el[i];
    size_t j = i + 1;
    while (j < el.size() && std::get<0>(el[j]) == a && std::get<1>(el[j]) == b) v += std::get<2>(el[j]), ++j;
    C.col.push_back(b), C.val.push_back(v), C.start[a + 1]++;
    i = j;
  }
  for (size_t i = 1; i < C.start.size(); ++i) C.start[i] += C.start[i - 1];
  return C;
}

void solveMultilevel(std::vector<Level>& lv, const std::vector<cplx>& b, std::vector<cplx>& x) {
  size_t L = lv.size() - 1;
  std::vector<std::vector<cplx>> bs(lv.size()), xs(lv.size());
  bs[0] = b;
  for (size_t l = 1; l <= L; ++l) {
    bs[l].assign(lv[l].A.diag.size(), 0.0);
    for (size_t i = 0; i < bs[l - 1].size(); ++i) bs[l][lv[l - 1].parent[i]] += std::conj(lv[l - 1].phase[i]) * bs[l - 1][i];
  }
  xs[L].assign(bs[L].size(), 0.0);
  solveCG(lv[L].A, bs[L], xs[L], 3000, 1e-9);
  for (size_t l = L; l-- > 0;) {
    xs[l].resize(bs[l].size());
    for (size_t i = 0; i < xs[l].size(); ++i) xs[l][i] = lv[l].phase[i] * xs[l + 1][lv[l].parent[i]];
    solveCG(lv[l].A, bs[l], xs[l], l == 0 ? 60 : 40, 1e-9);
  }
  x = xs[0];
}

}  // namespace

vec3 CrossField::dir(uint32_t f, int k) const {
  float a = angle[f] + float(k) * kPi / 2;
  return axis[f] * std::cos(a) + glm::cross(normal[f], axis[f]) * std::sin(a);
}

CrossField computeField(const Mesh& m, const std::vector<Stroke>& strokes, const std::vector<int32_t>& faceLabel,
                        const FieldOptions& opt) {
  CrossField F;
  const uint32_t nf = m.faceCount();
  F.normal.resize(nf), F.axis.resize(nf), F.angle.assign(nf, 0.f), F.strength.assign(nf, 0.f);
  F.index.assign(m.positions.size(), 0);
  if (nf == 0) return F;
  std::vector<vec3> center(nf);
  std::vector<double> area(nf);
  for (uint32_t f = 0; f < nf; ++f) {
    vec3 n(0), c(0);
    uint32_t a = m.faceStart[f], e = m.faceStart[f + 1];
    for (uint32_t l = a; l < e; ++l) {
      vec3 p = m.positions[m.loopVert[l]], q = m.positions[m.loopVert[l + 1 < e ? l + 1 : a]];
      n += glm::cross(p, q), c += p;
    }
    area[f] = 0.5 * glm::length(n);
    F.normal[f] = glm::length(n) > 1e-30f ? glm::normalize(n) : vec3(0, 1, 0);
    center[f] = c / float(e - a);
    vec3 ax = m.positions[m.loopVert[a + 1]] - m.positions[m.loopVert[a]];
    ax -= F.normal[f] * glm::dot(ax, F.normal[f]);
    F.axis[f] = glm::length(ax) > 1e-30f ? glm::normalize(ax) : glm::normalize(glm::cross(F.normal[f], vec3(0.577f)));
  }
  EdgeFaces ef = edgeFaces(m);

  // ---- smoothness: across each edge between two faces, the field turns as little as it can
  std::vector<std::vector<std::pair<uint32_t, cplx>>> rows(nf);
  std::vector<double> diag(nf, 0.0);
  for (uint32_t e = 0; e < m.edges.size(); ++e) {
    if (ef.size(e) != 2) continue;
    uint32_t f = ef.begin(e)[0], g = ef.begin(e)[1];
    vec3 d = m.positions[m.edges[e].y] - m.positions[m.edges[e].x];
    if (glm::length(d) < 1e-30f) continue;
    d = glm::normalize(d);
    double kf = angleIn(F, f, d), kg = angleIn(F, g, d);
    cplx c = std::polar(1.0, 4 * (kf - kg));
    rows[f].push_back({g, -c}), rows[g].push_back({f, -std::conj(c)});
    diag[f] += 1, diag[g] += 1;
  }

  // ---- what holds it: (weight, weight * target) per face
  std::vector<double> hold(nf, 0.0);
  std::vector<cplx> want(nf, 0.0);
  auto holdTo = [&](uint32_t f, vec3 d, double w) {
    d -= F.normal[f] * glm::dot(d, F.normal[f]);
    if (glm::length(d) < 1e-20f || w <= 0) return;
    double a = angleIn(F, f, glm::normalize(d));
    hold[f] += w, want[f] += w * std::polar(1.0, 4 * a);
  };
  // open edges and patch borders: along them
  if (opt.borders > 0)
    for (uint32_t e = 0; e < m.edges.size(); ++e) {
      vec3 d = m.positions[m.edges[e].y] - m.positions[m.edges[e].x];
      if (ef.size(e) == 1) holdTo(ef.begin(e)[0], d, 10.0 * opt.borders);
      else if (ef.size(e) == 2 && faceLabel.size() == nf) {
        uint32_t f = ef.begin(e)[0], g = ef.begin(e)[1];
        if (faceLabel[f] != faceLabel[g] && (faceLabel[f] > 0 || faceLabel[g] > 0))
          holdTo(f, d, 10.0 * opt.borders), holdTo(g, d, 10.0 * opt.borders);
      }
    }
  // strokes: the faces within two quads of a stroke run along it (most strongly on it)
  if (!strokes.empty()) {
    double band = 2.0 * opt.edgeLength, cell = band;
    std::unordered_map<uint64_t, std::vector<uint32_t>> grid;
    auto key = [&](vec3 p) {
      int64_t x = (int64_t)std::floor(p.x / cell), y = (int64_t)std::floor(p.y / cell), z = (int64_t)std::floor(p.z / cell);
      return (uint64_t(x & 0x1FFFFF) << 42) | (uint64_t(y & 0x1FFFFF) << 21) | uint64_t(z & 0x1FFFFF);
    };
    for (uint32_t f = 0; f < nf; ++f) grid[key(center[f])].push_back(f);
    std::vector<float> best(nf, 1e30f);
    std::vector<vec3> bestDir(nf, vec3(0));
    for (auto& s : strokes) {
      size_t n = s.points.size();
      if (n < 2) continue;
      size_t segs = s.loop ? n : n - 1;
      for (size_t i = 0; i < segs; ++i) {
        vec3 a = s.points[i], b = s.points[(i + 1) % n], t = b - a;
        float len = glm::length(t);
        if (len < 1e-12f) continue;
        t /= len;
        int steps = std::max(1, (int)std::ceil(len / (cell * 0.5)));
        for (int k = 0; k <= steps; ++k) {
          vec3 p = glm::mix(a, b, float(k) / steps);
          int64_t cx = (int64_t)std::floor(p.x / cell), cy = (int64_t)std::floor(p.y / cell), cz = (int64_t)std::floor(p.z / cell);
          for (int64_t dx = -1; dx <= 1; ++dx)
            for (int64_t dy = -1; dy <= 1; ++dy)
              for (int64_t dz = -1; dz <= 1; ++dz) {
                uint64_t kk = (uint64_t((cx + dx) & 0x1FFFFF) << 42) | (uint64_t((cy + dy) & 0x1FFFFF) << 21) | uint64_t((cz + dz) & 0x1FFFFF);
                auto it = grid.find(kk);
                if (it == grid.end()) continue;
                for (uint32_t f : it->second) {
                  // distance from the face's center to the segment
                  vec3 q = center[f] - a;
                  float u = std::clamp(glm::dot(q, t), 0.f, len);
                  float dist = glm::length(q - t * u);
                  if (dist < band && dist < best[f]) best[f] = dist, bestDir[f] = t;
                }
              }
        }
      }
    }
    for (uint32_t f = 0; f < nf; ++f)
      if (best[f] < band) holdTo(f, bestDir[f], 200.0 * (1.0 - 0.8 * best[f] / band));
  }
  // curvature: the directions the surface bends most and least, where they differ clearly
  if (opt.curvature > 0) {
    std::vector<vec3> vn(m.positions.size(), vec3(0));
    for (uint32_t f = 0; f < nf; ++f)
      for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) vn[m.loopVert[l]] += F.normal[f] * float(area[f]);
    for (auto& n : vn) n = glm::length(n) > 1e-30f ? glm::normalize(n) : vec3(0, 1, 0);
    for (uint32_t f = 0; f < nf; ++f) {
      // shape operator S in the face's frame from dn = S dp along its edges (least squares)
      vec3 ax = F.axis[f], bx = glm::cross(F.normal[f], ax);
      double M[3][3] = {{0}}, r[3] = {0};
      uint32_t a = m.faceStart[f], e = m.faceStart[f + 1];
      for (uint32_t l = a; l < e; ++l) {
        uint32_t i = m.loopVert[l], j = m.loopVert[l + 1 < e ? l + 1 : a];
        vec3 dp = m.positions[j] - m.positions[i], dn = vn[j] - vn[i];
        double ex = glm::dot(dp, ax), ey = glm::dot(dp, bx), dx = glm::dot(dn, ax), dy = glm::dot(dn, bx);
        // dx = s11 ex + s12 ey;  dy = s12 ex + s22 ey
        double r1[3] = {ex, ey, 0}, r2[3] = {0, ex, ey};
        for (int p = 0; p < 3; ++p) {
          for (int q = 0; q < 3; ++q) M[p][q] += r1[p] * r1[q] + r2[p] * r2[q];
          r[p] += r1[p] * dx + r2[p] * dy;
        }
      }
      for (int p = 0; p < 3; ++p) M[p][p] += 1e-12;
      // 3x3 solve (Cramer)
      auto det3 = [](double A[3][3]) {
        return A[0][0] * (A[1][1] * A[2][2] - A[1][2] * A[2][1]) - A[0][1] * (A[1][0] * A[2][2] - A[1][2] * A[2][0]) +
               A[0][2] * (A[1][0] * A[2][1] - A[1][1] * A[2][0]);
      };
      double D = det3(M);
      if (std::abs(D) < 1e-30) continue;
      double sol[3];
      for (int c = 0; c < 3; ++c) {
        double T[3][3];
        for (int p = 0; p < 3; ++p)
          for (int q = 0; q < 3; ++q) T[p][q] = q == c ? r[p] : M[p][q];
        sol[c] = det3(T) / D;
      }
      double s11 = sol[0], s12 = sol[1], s22 = sol[2];
      double aniso = std::sqrt((s11 - s22) * (s11 - s22) + 4 * s12 * s12);  // |k1 - k2|
      double phi = 0.5 * std::atan2(2 * s12, s11 - s22);
      double w = opt.curvature * std::min(1.0, std::pow(aniso * opt.edgeLength * 2, 2));
      if (w > 1e-6) hold[f] += w, want[f] += w * std::polar(1.0, 4 * phi);
    }
  }

  // ---- solve (L + H + e) z = H w + e z': inverse iteration, so where nothing holds it the field is
  // the smoothest one there is (on a sphere: eight singularities like a cube's corners, not the
  // mesh's own poles)
  HermitianSystem A;
  A.start.assign(nf + 1, 0);
  A.diag.resize(nf);
  for (uint32_t f = 0; f < nf; ++f) A.start[f + 1] = A.start[f] + (uint32_t)rows[f].size();
  A.col.resize(A.start[nf]), A.val.resize(A.start[nf]);
  double meanDiag = 0;
  for (uint32_t f = 0; f < nf; ++f) meanDiag += diag[f] / nf;
  const double eps = 1e-3 * std::max(meanDiag, 1.0);
  std::vector<cplx> b(nf), z(nf);
  for (uint32_t f = 0; f < nf; ++f) {
    for (size_t k = 0; k < rows[f].size(); ++k) A.col[A.start[f] + k] = rows[f][k].first, A.val[A.start[f] + k] = rows[f][k].second;
    A.diag[f] = diag[f] + hold[f] + eps;
    uint32_t hsh = f * 2654435761u;
    z[f] = hold[f] > 0 ? want[f] / hold[f] : std::polar(1.0, double(hsh % 6283) / 1000.0);
  }
  std::vector<Level> lv(1);
  lv[0].A = A;
  while (lv.back().A.diag.size() > 1500 && lv.size() < 30) {
    Level next;
    next.A = coarsen(lv.back().A, lv.back().parent, lv.back().phase);
    if (next.A.diag.size() > lv.back().A.diag.size() * 0.85) {
      lv.back().parent.clear();
      break;
    }
    lv.push_back(std::move(next));
  }
  for (int round = 0; round < 6; ++round) {
    double rms = 0;
    for (uint32_t f = 0; f < nf; ++f) rms += std::norm(z[f]) / nf;
    rms = std::sqrt(std::max(rms, 1e-300));
    for (uint32_t f = 0; f < nf; ++f) b[f] = want[f] + eps * z[f] / rms;
    if (lv.size() > 1) solveMultilevel(lv, b, z);
    else solveCG(A, b, z, 3000, 1e-9);
  }
  float maxAbs = 0;
  for (uint32_t f = 0; f < nf; ++f) maxAbs = std::max(maxAbs, (float)std::abs(z[f]));
  for (uint32_t f = 0; f < nf; ++f) {
    F.angle[f] = float(std::arg(z[f]) / 4);
    F.strength[f] = maxAbs > 0 ? float(std::abs(z[f]) / maxAbs) : 0.f;
  }

  // ---- singularities: how far the field turns going once round each inner vertex
  std::vector<double> turn(m.positions.size(), 0.0), angleSum(m.positions.size(), 0.0);
  std::vector<char> skip(m.positions.size(), 0);
  for (uint32_t f = 0; f < nf; ++f) {
    uint32_t a = m.faceStart[f], e = m.faceStart[f + 1], n = e - a;
    for (uint32_t k = 0; k < n; ++k) {
      uint32_t v = m.loopVert[a + k];
      vec3 p = m.positions[v], u1 = m.positions[m.loopVert[a + (k + 1) % n]] - p, u2 = m.positions[m.loopVert[a + (k + n - 1) % n]] - p;
      if (glm::length(u1) > 1e-30f && glm::length(u2) > 1e-30f)
        angleSum[v] += std::acos(std::clamp((double)glm::dot(glm::normalize(u1), glm::normalize(u2)), -1.0, 1.0));
    }
  }
  for (uint32_t e = 0; e < m.edges.size(); ++e) {
    if (ef.size(e) != 2) {
      skip[m.edges[e].x] = skip[m.edges[e].y] = 1;
      continue;
    }
    for (int end = 0; end < 2; ++end) {
      uint32_t v = end ? m.edges[e].y : m.edges[e].x, u = end ? m.edges[e].x : m.edges[e].y;
      vec3 d = m.positions[u] - m.positions[v];
      if (glm::length(d) < 1e-30f) continue;
      d = glm::normalize(d);
      // f: the face with the directed edge v -> u (to the left of it, after it going counterclockwise)
      uint32_t f = ef.begin(e)[0], g = ef.begin(e)[1];
      bool fHas = false;
      for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l)
        if (m.loopVert[l] == v && m.loopVert[m.nextLoop(l, f)] == u) fHas = true;
      if (!fHas) std::swap(f, g);
      double psiF = F.angle[f] - angleIn(F, f, d), psiG = F.angle[g] - angleIn(F, g, d);
      turn[v] += wrapQuarter(psiF - psiG);
    }
  }
  for (uint32_t v = 0; v < m.positions.size(); ++v) {
    if (skip[v] || angleSum[v] == 0) continue;
    double q = (turn[v] + (2 * kPi - angleSum[v])) / (kPi / 2);
    int qi = (int)std::lround(q);
    if (qi != 0) F.index[v] = (int8_t)std::clamp(qi, -4, 4), F.singular.push_back(v);
  }
  return F;
}

}  // namespace patina::retopo
