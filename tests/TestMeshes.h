#pragma once
// Simple character-like meshes for the tests (parts, regions, the Assistant's commands).
#include <algorithm>
#include <cmath>

#include "core/Mesh.h"

namespace testmesh {

using namespace patina;

// Appends a latitude / longitude grid of quads: p(r, k) for r in 0..rows, k in 0..seg (the
// seam column duplicated, as an OBJ export would). Faces get material `slot`.
// `welded`: the seam column shares the first column's vertices (as a mesh from Blender would).
template <class Fn>
inline void addGrid(Mesh& m, int rows, int seg, uint32_t slot, Fn p, bool flip = false, bool welded = false) {
  uint32_t base = (uint32_t)m.positions.size();
  int cols = welded ? seg : seg + 1;
  for (int r = 0; r <= rows; ++r)
    for (int k = 0; k < cols; ++k) m.positions.push_back(p(r, k));
  if (m.faceStart.empty()) m.faceStart.push_back(0);
  auto id = [&](int r, int k) { return base + uint32_t(r * cols + (k % cols)); };
  for (int r = 0; r < rows; ++r)
    for (int k = 0; k < seg; ++k) {
      uint32_t q[4] = {id(r, k), id(r, k + 1), id(r + 1, k + 1), id(r + 1, k)};
      vec2 uv[4] = {{float(k) / seg, 1 - float(r) / rows}, {float(k + 1) / seg, 1 - float(r) / rows},
                    {float(k + 1) / seg, 1 - float(r + 1) / rows}, {float(k) / seg, 1 - float(r + 1) / rows}};
      for (int i = 0; i < 4; ++i) {
        int j = flip ? i : 3 - i;
        m.loopVert.push_back(q[j]);
        m.loopUV.push_back(uv[j]);
      }
      m.faceStart.push_back((uint32_t)m.loopVert.size());
      m.faceMaterial.push_back(slot);
    }
}

// Turns every face the other way round when the faces point inward (negative signed volume,
// measured about the model's center line, which works for open tubes too).
inline void orientOutward(Mesh& m) {
  vec3 c(0);
  for (auto& p : m.positions) c += p;
  c /= float(std::max<size_t>(1, m.positions.size()));
  double vol = 0;
  for (uint32_t f = 0; f + 1 < m.faceStart.size(); ++f)
    for (uint32_t l = m.faceStart[f] + 1; l + 1 < m.faceStart[f + 1]; ++l) {
      vec3 a = m.positions[m.loopVert[m.faceStart[f]]], b = m.positions[m.loopVert[l]], d = m.positions[m.loopVert[l + 1]];
      vol += glm::dot(a - vec3(c.x, a.y, c.z), glm::cross(b - a, d - a));  // radial part only
    }
  if (vol >= 0) return;
  for (uint32_t f = 0; f + 1 < m.faceStart.size(); ++f) {
    std::reverse(m.loopVert.begin() + m.faceStart[f], m.loopVert.begin() + m.faceStart[f + 1]);
    std::reverse(m.loopUV.begin() + m.faceStart[f], m.loopUV.begin() + m.faceStart[f + 1]);
  }
}

// The bust from scripts/manual_scenes.py: a sphere head (slot Skin) on a separate tapered tube
// with shoulders (slot Cloth).
inline Mesh makeBust(int seg = 40, int rings = 20) {
  Mesh m;
  m.name = "Bust";
  addGrid(m, rings, seg, 0, [&](int r, int k) {
    float th = float(M_PI) * r / rings, ph = 2 * float(M_PI) * k / seg;
    return vec3(0.42f * std::sin(th) * std::cos(ph), 1.15f + 0.5f * std::cos(th), 0.42f * std::sin(th) * std::sin(ph));
  });
  const int rows = 12;
  addGrid(m, rows, seg, 1, [&](int r, int k) {
    float y = 0.62f - 1.4f * r / rows, t = float(r) / rows;
    float rad = 0.16f + 0.42f * std::min(1.f, t * 5.f) - 0.12f * std::max(0.f, t - 0.35f);
    float ph = 2 * float(M_PI) * k / seg;
    return vec3(rad * std::cos(ph), y, 0.75f * rad * std::sin(ph));
  });
  m.materialNames = {"Skin", "Cloth"};
  orientOutward(m);
  m.finalize();
  return m;
}

// A sphere resting on a wider cylinder: two pieces, one material slot.
inline Mesh makeSphereOnCylinder(int seg = 32) {
  Mesh m;
  m.name = "SphereOnCylinder";
  const float R = 0.5f, h = 1.6f, r = 0.32f, cy = h + r * 0.8f;
  addGrid(m, 8, seg, 0, [&](int i, int k) {  // the side
    float ph = 2 * float(M_PI) * k / seg;
    return vec3(R * std::cos(ph), h * (1 - float(i) / 8), R * std::sin(ph));
  });
  addGrid(m, 4, seg, 0, [&](int i, int k) {  // top cap (rings toward the center)
    float ph = 2 * float(M_PI) * k / seg, rr = R * (1 - float(i) / 4);
    return vec3(rr * std::cos(ph), h, rr * std::sin(ph));
  }, true);
  addGrid(m, 16, seg, 0, [&](int i, int k) {
    float th = float(M_PI) * i / 16, ph = 2 * float(M_PI) * k / seg;
    return vec3(r * std::sin(th) * std::cos(ph), cy + r * std::cos(th), r * std::sin(th) * std::sin(ph));
  });
  orientOutward(m);
  m.finalize();
  return m;
}

// One welded surface of revolution shaped like a figure: legs-and-body column, shoulders,
// a narrow neck, a round head. Everything is one piece, so the head is cut out of it. Welded
// all round (no seam column), open at the bottom and with a small hole at the crown.
inline Mesh makeLatheFigure(int seg = 32, int rows = 60) {
  Mesh m;
  m.name = "Figure";
  auto radius = [](float t) {  // t = 0 bottom .. 1 top
    if (t < 0.70f) return 0.30f + 0.10f * t;                        // body
    if (t < 0.76f) return 0.37f + (0.45f - 0.37f) * (t - 0.70f) / 0.06f;  // shoulders
    if (t < 0.80f) return 0.45f - (0.45f - 0.10f) * (t - 0.76f) / 0.04f;  // into the neck
    if (t < 0.83f) return 0.10f;                                   // neck
    float u = (t - 0.83f) / 0.17f;                                 // head: half circle profile
    return std::max(0.02f, 0.22f * std::sin(float(M_PI) * (0.15f + 0.85f * u)));
  };
  addGrid(m, rows, seg, 0, [&](int i, int k) {
    float t = 1 - float(i) / rows, ph = 2 * float(M_PI) * k / seg;
    float rr = radius(t);
    return vec3(rr * std::cos(ph), 2.0f * t, rr * std::sin(ph));
  }, false, true);
  orientOutward(m);
  m.finalize();
  return m;
}

}  // namespace testmesh
