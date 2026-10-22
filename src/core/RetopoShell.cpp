#include "core/RetopoShell.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "core/Parallel.h"

namespace patina::retopo {

Mesh voxelShell(const Mesh& high, const Bvh& bvh, float h, float r, int smoothPasses) {

  Mesh out;
  out.name = "Shell";
  if (high.positions.empty() || h <= 0) return out;
  vec3 lo(1e30f), hi(-1e30f);
  for (auto& p : high.positions) lo = glm::min(lo, p), hi = glm::max(hi, p);
  float pad = r + 3 * h;
  lo -= vec3(pad), hi += vec3(pad);
  // at most about 40 million grid points: coarser cubes for a big model
  for (;;) {
    double n = double(std::ceil((hi.x - lo.x) / h) + 1) * (std::ceil((hi.y - lo.y) / h) + 1) * (std::ceil((hi.z - lo.z) / h) + 1);
    if (n <= 4e7) break;
    h *= 1.25f;
  }
  const int nx = (int)std::ceil((hi.x - lo.x) / h) + 1, ny = (int)std::ceil((hi.y - lo.y) / h) + 1,
            nz = (int)std::ceil((hi.z - lo.z) / h) + 1;
  auto id = [&](int x, int y, int z) { return (size_t(z) * ny + y) * nx + x; };
  // distance to the surface (only near it: further than the offset and a few cubes is "far")
  const float far = r + 2.5f * h;
  std::vector<float> f(size_t(nx) * ny * nz, far - r);
  // a coarse pass first: only grid points near a coarse point close to the surface are measured
  const int C = 4;
  const float coarseReach = far + 1.8f * C * h;
  parallelFor((size_t)((nz + C - 1) / C), [&](size_t cz) {
    for (int cy = 0; cy < ny; cy += C)
      for (int cx = 0; cx < nx; cx += C) {
        vec3 pc = lo + vec3(cx + C / 2.f, cy + C / 2.f, cz * C + C / 2.f) * h;
        RayHit hc = bvh.closestPoint(high, pc, coarseReach);
        if (!hc.hit) continue;
        for (int z = (int)cz * C; z < std::min(nz, (int)cz * C + C); ++z)
          for (int y = cy; y < std::min(ny, cy + C); ++y)
            for (int x = cx; x < std::min(nx, cx + C); ++x) {
              vec3 p = lo + vec3(x, y, z) * h;
              RayHit hp = bvh.closestPoint(high, p, far);
              if (hp.hit) f[id(x, y, z)] = glm::length(hp.position - p) - r;
            }
      }
  });
  // only the outside: grid points beyond the offset that can't be reached from the grid's edge without
  // crossing it are inside the model (else a closed model would also get a second shell within it)
  {
    std::vector<char> outside(f.size(), 0);
    std::vector<size_t> stack;
    auto seed = [&](int x, int y, int z) {
      size_t i = id(x, y, z);
      if (f[i] > 0 && !outside[i]) outside[i] = 1, stack.push_back(i);
    };
    for (int z = 0; z < nz; ++z)
      for (int y = 0; y < ny; ++y)
        for (int x = 0; x < nx; ++x)
          if (x == 0 || y == 0 || z == 0 || x == nx - 1 || y == ny - 1 || z == nz - 1) seed(x, y, z);
    while (!stack.empty()) {
      size_t i = stack.back();
      stack.pop_back();
      int x = int(i % nx), y = int((i / nx) % ny), z = int(i / (size_t(nx) * ny));
      if (x > 0) seed(x - 1, y, z);
      if (x + 1 < nx) seed(x + 1, y, z);
      if (y > 0) seed(x, y - 1, z);
      if (y + 1 < ny) seed(x, y + 1, z);
      if (z > 0) seed(x, y, z - 1);
      if (z + 1 < nz) seed(x, y, z + 1);
    }
    for (size_t i = 0; i < f.size(); ++i)
      if (f[i] > 0 && !outside[i]) f[i] = -f[i];
  }
  // surface nets: a vertex in each cube the surface crosses, at the mean of the crossings on its edges
  const int cx = nx - 1, cy = ny - 1, cz = nz - 1;
  auto cid = [&](int x, int y, int z) { return (size_t(z) * cy + y) * cx + x; };
  std::vector<int32_t> cellVert(size_t(cx) * cy * cz, -1);
  static const int corner[8][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}, {0, 0, 1}, {1, 0, 1}, {0, 1, 1}, {1, 1, 1}};
  static const int edge[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
  for (int z = 0; z < cz; ++z)
    for (int y = 0; y < cy; ++y)
      for (int x = 0; x < cx; ++x) {
        float v[8];
        int inside = 0;
        for (int k = 0; k < 8; ++k) {
          v[k] = f[id(x + corner[k][0], y + corner[k][1], z + corner[k][2])];
          inside += v[k] < 0;
        }
        if (inside == 0 || inside == 8) continue;
        vec3 sum(0);
        int n = 0;
        for (auto& e : edge) {
          float a = v[e[0]], b = v[e[1]];
          if ((a < 0) == (b < 0)) continue;
          float t = a / (a - b);
          vec3 pa(corner[e[0]][0], corner[e[0]][1], corner[e[0]][2]), pb(corner[e[1]][0], corner[e[1]][1], corner[e[1]][2]);
          sum += glm::mix(pa, pb, t), ++n;
        }
        cellVert[cid(x, y, z)] = (int32_t)out.positions.size();
        out.positions.push_back(lo + (vec3(x, y, z) + sum / float(n)) * h);
      }
  // a quad for each grid edge the surface crosses, from the four cubes around it, facing outwards
  out.faceStart.push_back(0);
  auto quad = [&](size_t c0, size_t c1, size_t c2, size_t c3, bool flip) {
    int32_t q[4] = {cellVert[c0], cellVert[c1], cellVert[c2], cellVert[c3]};
    for (int k = 0; k < 4; ++k)
      if (q[k] < 0) return;
    if (flip) std::swap(q[1], q[3]);
    // two triangles, split along the shorter diagonal
    vec3 P[4];
    for (int k = 0; k < 4; ++k) P[k] = out.positions[q[k]];
    bool d02 = glm::length(P[0] - P[2]) <= glm::length(P[1] - P[3]);
    int t[2][3] = {{0, 1, 2}, {0, 2, 3}};
    if (!d02) t[0][0] = 0, t[0][1] = 1, t[0][2] = 3, t[1][0] = 1, t[1][1] = 2, t[1][2] = 3;
    for (auto& tr : t) {
      for (int k = 0; k < 3; ++k) out.loopVert.push_back((uint32_t)q[tr[k]]);
      out.faceStart.push_back((uint32_t)out.loopVert.size());
    }
  };
  for (int z = 1; z < cz; ++z)
    for (int y = 1; y < cy; ++y)
      for (int x = 0; x < cx; ++x) {  // edges along x
        float a = f[id(x, y, z)], b = f[id(x + 1, y, z)];
        if ((a < 0) == (b < 0)) continue;
        quad(cid(x, y - 1, z - 1), cid(x, y, z - 1), cid(x, y, z), cid(x, y - 1, z), b < 0);
      }
  for (int z = 1; z < cz; ++z)
    for (int y = 0; y < cy; ++y)
      for (int x = 1; x < cx; ++x) {  // along y
        float a = f[id(x, y, z)], b = f[id(x, y + 1, z)];
        if ((a < 0) == (b < 0)) continue;
        quad(cid(x - 1, y, z - 1), cid(x - 1, y, z), cid(x, y, z), cid(x, y, z - 1), b < 0);
      }
  for (int z = 0; z < cz; ++z)
    for (int y = 1; y < cy; ++y)
      for (int x = 1; x < cx; ++x) {  // along z
        float a = f[id(x, y, z)], b = f[id(x, y, z + 1)];
        if ((a < 0) == (b < 0)) continue;
        quad(cid(x - 1, y - 1, z), cid(x, y - 1, z), cid(x, y, z), cid(x - 1, y, z), b < 0);
      }
  // the cubes leave little steps that would scatter the field's directions: Taubin smoothing (a
  // shrinking pass and an inflating one) takes them out without shrinking the shell
  {
    std::vector<std::vector<uint32_t>> nb(out.positions.size());
    for (size_t t = 0; t + 1 < out.faceStart.size(); ++t)
      for (uint32_t k = 0; k < 3; ++k) {
        uint32_t a = out.loopVert[out.faceStart[t] + k], b = out.loopVert[out.faceStart[t] + (k + 1) % 3];
        if (std::find(nb[a].begin(), nb[a].end(), b) == nb[a].end()) nb[a].push_back(b), nb[b].push_back(a);
      }
    // onto the offset surface itself (the nearest point of the high-poly, out by the offset): smooth
    // wherever the model is, unlike the cubes' approximation
    auto project = [&] {
      parallelFor(out.positions.size(), [&](size_t v) {
        vec3 p = out.positions[v];
        RayHit hp = bvh.closestPoint(high, p, r + 4 * h);
        if (!hp.hit) return;
        vec3 d = p - hp.position;
        float len = glm::length(d);
        if (len > 1e-6f * h) out.positions[v] = hp.position + d * (r / len);
      });
    };
    project();
    for (int it = 0; it < 2 * smoothPasses; ++it) {
      float w = it % 2 ? -0.53f : 0.5f;
      std::vector<vec3> next = out.positions;
      parallelFor(out.positions.size(), [&](size_t v) {
        if (nb[v].empty()) return;
        vec3 c(0);
        for (uint32_t u : nb[v]) c += out.positions[u];
        c /= float(nb[v].size());
        next[v] = out.positions[v] + w * (c - out.positions[v]);
      });
      out.positions = std::move(next);
      if (it % 2) project();
    }
  }
  if (out.faceStart.size() > 1) out.finalize();
  return out;
}

}  // namespace patina::retopo
