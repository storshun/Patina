#include "core/MeshSymmetry.h"

#include <algorithm>
#include <atomic>
#include <numeric>
#include <thread>

namespace patina {

SymmetryReport measureSymmetry(const Mesh& m, const Bvh& bvh, float planeX) {
  SymmetryReport r;
  uint32_t V = (uint32_t)m.positions.size(), F = m.faceCount();
  r.vertices = (int)V;
  if (!V || !F) return r;
  vec3 ext = m.bounds.hi - m.bounds.lo;
  r.size = std::max(ext.x, std::max(ext.y, ext.z));
  if (std::isnan(planeX)) {
    // x = 0 when the model sits on it (the usual case: modeled around the origin); otherwise the
    // middle of the 2nd..98th percentile of x, so one stray vertex or a lopsided bump can't move it
    float c = (m.bounds.lo.x + m.bounds.hi.x) * 0.5f;
    std::vector<float> xs(V);
    for (uint32_t i = 0; i < V; ++i) xs[i] = m.positions[i].x;
    size_t a = V / 50, b = V - 1 - V / 50;
    std::nth_element(xs.begin(), xs.begin() + a, xs.end());
    float lo = xs[a];
    std::nth_element(xs.begin(), xs.begin() + b, xs.end());
    float robust = (lo + xs[b]) * 0.5f;
    planeX = std::abs(c) < 0.03f * r.size || std::abs(robust) < 0.01f * r.size ? 0.f : robust;
  }
  r.planeX = planeX;
  r.tolerance = 0.001f * r.size;
  const float maxDist = 0.05f * r.size;
  r.vertexDist.assign(V, -1.f);
  // vertices that no face uses are left out
  std::vector<uint8_t> used(V, 0);
  for (uint32_t v : m.loopVert) used[v] = 1;
  unsigned nThreads = std::max(1u, std::min(16u, std::thread::hardware_concurrency()));
  std::atomic<uint32_t> next{0};
  std::vector<std::thread> pool;
  for (unsigned t = 0; t < nThreads; ++t)
    pool.emplace_back([&] {
      for (uint32_t v; (v = next.fetch_add(256)) < V;)
        for (uint32_t i = v; i < std::min(V, v + 256); ++i) {
          if (!used[i]) continue;
          vec3 p = m.positions[i];
          vec3 q(2.f * planeX - p.x, p.y, p.z);
          RayHit h = bvh.closestPoint(m, q, maxDist);
          if (h.hit) r.vertexDist[i] = glm::length(h.position - q);
        }
    });
  for (auto& th : pool) th.join();
  for (uint32_t i = 0; i < V; ++i)
    if (!used[i]) r.vertexDist[i] = 0.f;  // loose vertices: nothing to compare
  std::vector<float> ds;
  ds.reserve(V);
  double sum = 0;
  for (uint32_t i = 0; i < V; ++i) {
    if (!used[i]) continue;
    float d = r.vertexDist[i];
    if (d < 0) {
      ++r.unmatched;
      continue;
    }
    ds.push_back(d);
    sum += d;
    r.maxDist = std::max(r.maxDist, d);
    if (d > r.tolerance) ++r.beyond;
  }
  if (!ds.empty()) {
    r.meanDist = float(sum / ds.size());
    std::sort(ds.begin(), ds.end());
    r.median = ds[ds.size() / 2];
    r.p95 = ds[std::min(ds.size() - 1, size_t(ds.size() * 0.95))];
  }
  r.faceDist.assign(F, -1.f);
  for (uint32_t f = 0; f < F; ++f) {
    float s = 0;
    int n = 0;
    bool lost = false;
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) {
      float d = r.vertexDist[m.loopVert[l]];
      if (d < 0) lost = true;
      else s += d, ++n;
    }
    r.faceDist[f] = lost || !n ? -1.f : s / n;
  }
  // worst spots, at least 8% of the size apart so one bad area doesn't fill the list
  std::vector<uint32_t> order(V);
  std::iota(order.begin(), order.end(), 0u);
  std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return r.vertexDist[a] > r.vertexDist[b]; });
  for (uint32_t i : order) {
    float d = r.vertexDist[i];
    if (d <= r.tolerance || r.worst.size() >= 10) break;
    bool near = false;
    for (auto& s : r.worst) near |= glm::length(s.position - m.positions[i]) < 0.08f * r.size;
    if (!near) r.worst.push_back({m.positions[i], d, i});
  }
  // groups: material slots, and thirds of the height
  auto addTo = [&](SymmetryReport::Group& g, float d) {
    if (d < 0) return;
    g.meanDist = (g.meanDist * g.vertices + d) / float(g.vertices + 1);
    ++g.vertices;
    g.maxDist = std::max(g.maxDist, d);
    if (d > r.tolerance) ++g.beyond;
  };
  uint32_t slots = 0;
  for (uint32_t s : m.faceMaterial) slots = std::max(slots, s + 1);
  r.bySlot.resize(std::max(1u, slots));
  for (uint32_t s = 0; s < r.bySlot.size(); ++s) r.bySlot[s].name = m.slotName(s);
  std::vector<int32_t> vertSlot(V, -1);
  for (uint32_t f = 0; f < F; ++f)
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l)
      vertSlot[m.loopVert[l]] = f < m.faceMaterial.size() ? (int32_t)m.faceMaterial[f] : 0;
  r.byHeight = {{"bottom third"}, {"middle third"}, {"top third"}};
  float y0 = m.bounds.lo.y, h = std::max(1e-9f, ext.y);
  for (uint32_t i = 0; i < V; ++i) {
    if (!used[i]) continue;
    if (vertSlot[i] >= 0) addTo(r.bySlot[vertSlot[i]], r.vertexDist[i]);
    int band = std::clamp(int((m.positions[i].y - y0) / h * 3.f), 0, 2);
    addTo(r.byHeight[band], r.vertexDist[i]);
  }
  return r;
}

}  // namespace patina
