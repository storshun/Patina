#include "core/UVArap.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include "core/Log.h"

namespace patina::uv {

using glm::dvec2;
using glm::dvec3;

namespace {

struct Csr {
  int n = 0;
  std::vector<int> start, col;
  std::vector<double> val, diag;
  void mul(const std::vector<double>& x, std::vector<double>& y) const {
    for (int i = 0; i < n; ++i) {
      double s = 0;
      for (int k = start[i]; k < start[i + 1]; ++k) s += val[k] * x[col[k]];
      y[i] = s;
    }
  }
};

// Jacobi-preconditioned conjugate gradients, warm-started from x.
int solveCg(const Csr& A, const std::vector<double>& b, std::vector<double>& x, int maxIt, double tol) {
  int n = A.n;
  std::vector<double> r(n), z(n), p(n), q(n);
  A.mul(x, q);
  double bb = 0;
  for (int i = 0; i < n; ++i) r[i] = b[i] - q[i], bb += b[i] * b[i];
  if (bb <= 0) bb = 1;
  for (int i = 0; i < n; ++i) z[i] = r[i] / A.diag[i];
  p = z;
  double rz = 0;
  for (int i = 0; i < n; ++i) rz += r[i] * z[i];
  int it = 0;
  for (; it < maxIt; ++it) {
    double rr = 0;
    for (int i = 0; i < n; ++i) rr += r[i] * r[i];
    if (rr <= tol * tol * bb) break;
    A.mul(p, q);
    double pq = 0;
    for (int i = 0; i < n; ++i) pq += p[i] * q[i];
    if (pq <= 0) break;
    double alpha = rz / pq;
    for (int i = 0; i < n; ++i) x[i] += alpha * p[i], r[i] -= alpha * q[i];
    for (int i = 0; i < n; ++i) z[i] = r[i] / A.diag[i];
    double rz2 = 0;
    for (int i = 0; i < n; ++i) rz2 += r[i] * z[i];
    double beta = rz2 / rz;
    rz = rz2;
    for (int i = 0; i < n; ++i) p[i] = z[i] + beta * p[i];
  }
  return it;
}

}  // namespace

bool arapRelax(Mesh& m, const std::vector<uint32_t>& faces, const ArapOptions& opt) {
  if (m.loopUV.size() != m.loopCount()) return false;
  if (m.loopUVVert.size() != m.loopCount()) m.computeUVTopology();
  uint32_t F = m.faceCount();
  std::vector<uint8_t> in(F, faces.empty() ? 1 : 0);
  for (uint32_t f : faces)
    if (f < F) in[f] = 1;
  // unknowns: the UV vertices of the faces
  std::unordered_map<uint32_t, int> idx;
  std::vector<uint32_t> anyLoop;  // a loop of each unknown
  struct Tri {
    int v[3];
    dvec2 x[3];      // the triangle in its own plane, true size
    double cot[3];   // cotangent of the angle at each corner (weight of the opposite edge)
  };
  std::vector<Tri> tris;
  double area3 = 0, areaUV = 0;
  for (size_t t = 0; t < m.tris.size(); ++t) {
    if (!in[m.triFace[t]]) continue;
    const glm::uvec3& L = m.tris[t];
    uint32_t loops[3] = {L.x, L.y, L.z};
    dvec3 p[3];
    for (int i = 0; i < 3; ++i) p[i] = dvec3(m.positions[m.loopVert[loops[i]]]);
    dvec3 e1 = p[1] - p[0], e2 = p[2] - p[0];
    double l1 = glm::length(e1);
    dvec3 cr = glm::cross(e1, e2);
    double a2 = glm::length(cr);
    if (l1 <= 0 || !(a2 > 1e-14 * std::max(1.0, glm::dot(e2, e2) + l1 * l1))) continue;
    Tri tr;
    dvec3 ex = e1 / l1;
    dvec3 ey = glm::normalize(e2 - glm::dot(e2, ex) * ex);
    tr.x[0] = {0, 0};
    tr.x[1] = {l1, 0};
    tr.x[2] = {glm::dot(e2, ex), glm::dot(e2, ey)};
    for (int i = 0; i < 3; ++i) {
      dvec2 a = tr.x[(i + 1) % 3] - tr.x[i], b = tr.x[(i + 2) % 3] - tr.x[i];
      double c = std::abs(a.x * b.y - a.y * b.x);
      tr.cot[i] = std::max(1e-3, glm::dot(a, b) / std::max(c, 1e-300));
    }
    for (int i = 0; i < 3; ++i) {
      uint32_t u = m.loopUVVert[loops[i]];
      auto [it, fresh] = idx.emplace(u, (int)anyLoop.size());
      if (fresh) anyLoop.push_back(loops[i]);
      tr.v[i] = it->second;
    }
    area3 += a2 * 0.5;
    vec2 q0 = m.loopUV[loops[0]], q1 = m.loopUV[loops[1]], q2 = m.loopUV[loops[2]];
    areaUV += 0.5 * double((q1.x - q0.x) * (q2.y - q0.y) - (q1.y - q0.y) * (q2.x - q0.x));
    tris.push_back(tr);
  }
  int n = (int)anyLoop.size();
  if (n < 3 || tris.empty()) return false;
  // start from the layout, at true size; a mirrored layout is solved mirrored back
  bool mirrored = areaUV < 0;
  double s = std::abs(areaUV) > 0 ? std::sqrt(area3 / std::abs(areaUV)) : 1.0;
  std::vector<double> U(n), V(n);
  for (int i = 0; i < n; ++i) {
    vec2 q = m.loopUV[anyLoop[i]];
    U[i] = (mirrored ? -q.x : q.x) * s;
    V[i] = q.y * s;
  }
  // the cotangent Laplacian, with unknown 0 held in place
  std::vector<std::unordered_map<int, double>> rows(n);
  for (auto& t : tris)
    for (int i = 0; i < 3; ++i) {
      int a = t.v[(i + 1) % 3], b = t.v[(i + 2) % 3];
      double w = t.cot[i];
      rows[a][a] += w;
      rows[b][b] += w;
      rows[a][b] -= w;
      rows[b][a] -= w;
    }
  const int fixedVar = 0;
  Csr A;
  A.n = n;
  A.start.push_back(0);
  for (int i = 0; i < n; ++i) {
    if (i == fixedVar) {
      A.col.push_back(i), A.val.push_back(1.0);
    } else {
      for (auto& [c, w] : rows[i]) {
        if (c == fixedVar) continue;  // moves to the right-hand side
        A.col.push_back(c), A.val.push_back(w);
      }
    }
    A.start.push_back((int)A.col.size());
  }
  A.diag.assign(n, 1.0);
  for (int i = 0; i < n; ++i)
    for (int k = A.start[i]; k < A.start[i + 1]; ++k)
      if (A.col[k] == i) A.diag[i] = std::max(A.val[k], 1e-12);
  std::vector<double> bu(n), bv(n);
  int cgTotal = 0;
  for (int it = 0; it < opt.iterations; ++it) {
    std::fill(bu.begin(), bu.end(), 0.0);
    std::fill(bv.begin(), bv.end(), 0.0);
    for (auto& t : tris) {
      // local step: the rotation closest to how the layout maps this triangle
      double s00 = 0, s01 = 0, s10 = 0, s11 = 0;
      for (int i = 0; i < 3; ++i) {
        int a = t.v[(i + 1) % 3], b = t.v[(i + 2) % 3];
        dvec2 du(U[a] - U[b], V[a] - V[b]);
        dvec2 dx = t.x[(i + 1) % 3] - t.x[(i + 2) % 3];
        double w = t.cot[i];
        s00 += w * du.x * dx.x, s01 += w * du.x * dx.y, s10 += w * du.y * dx.x, s11 += w * du.y * dx.y;
      }
      double ca = s00 + s11, sb = s10 - s01, len = std::sqrt(ca * ca + sb * sb);
      double c = len > 0 ? ca / len : 1.0, sn = len > 0 ? sb / len : 0.0;
      // global step's right-hand side
      for (int i = 0; i < 3; ++i) {
        int a = t.v[(i + 1) % 3], b = t.v[(i + 2) % 3];
        dvec2 dx = t.x[(i + 1) % 3] - t.x[(i + 2) % 3];
        dvec2 r(c * dx.x - sn * dx.y, sn * dx.x + c * dx.y);
        double w = t.cot[i];
        bu[a] += w * r.x, bv[a] += w * r.y;
        bu[b] -= w * r.x, bv[b] -= w * r.y;
      }
    }
    // the held unknown keeps its value; its neighbors see it on the right-hand side
    for (int i = 0; i < n; ++i) {
      if (i == fixedVar) continue;
      auto f = rows[i].find(fixedVar);
      if (f != rows[i].end()) bu[i] -= f->second * U[fixedVar], bv[i] -= f->second * V[fixedVar];
    }
    bu[fixedVar] = U[fixedVar];
    bv[fixedVar] = V[fixedVar];
    cgTotal += solveCg(A, bu, U, opt.cgIterations, opt.tolerance);
    cgTotal += solveCg(A, bv, V, opt.cgIterations, opt.tolerance);
  }
  // write back to every corner of the faces
  for (uint32_t f = 0; f < F; ++f) {
    if (!in[f]) continue;
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) {
      auto it = idx.find(m.loopUVVert[l]);
      if (it == idx.end()) continue;
      double u = U[it->second], v = V[it->second];
      m.loopUV[l] = vec2(float(mirrored ? -u : u), float(v));
    }
  }
  PLOG_I("ARAP: %zu triangles, %d UV vertices, %d iterations, %d CG steps", tris.size(), n, opt.iterations, cgTotal);
  return true;
}

}  // namespace patina::uv
