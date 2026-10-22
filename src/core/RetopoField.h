#pragma once
// Retopology, 0.14: a smooth cross field (four directions at right angles on each face) for the quads
// to follow. It runs along the strokes drawn where edge loops should go, along the mesh's open edges
// and the patches' borders, and (more weakly) along the directions the surface bends most and least.
// Where it can't be smooth everywhere (a sphere needs eight such places) it has singularities: where
// three quads meet (+1, a quarter turn) or five (-1).
#include <cstdint>
#include <vector>

#include "core/Mesh.h"

namespace patina::retopo {

struct Stroke {
  std::vector<vec3> points;  // on the surface, in order
  bool loop = false;         // the last point joins the first
  bool edge = false;         // an edge loop runs exactly along it (else it only sets the flow's direction)
  bool confirmed = false;    // (0.22, zone lines) confirmed: drawn darker, and new lines don't snap to it
};

struct FieldOptions {
  float edgeLength = 0.02f;  // the quads' size: a stroke holds the field within about two quads of it
  float curvature = 0.3f;    // how much the surface's bends steer it (0: strokes and borders only)
  float borders = 1.f;       // how much the open edges and patch borders hold it (0: not at all)
};

struct CrossField {
  std::vector<vec3> normal, axis;  // per face: unit normal, and the direction angles are measured from
  std::vector<float> angle;        // per face: one of the four directions (radians from axis)
  std::vector<float> strength;     // per face: how settled the field is there (0..1; low near singularities)
  std::vector<int8_t> index;       // per vertex: singularity in quarter turns (+1: three quads meet, -1: five)
  std::vector<uint32_t> singular;  // the vertices with index != 0
  vec3 dir(uint32_t f, int k) const;  // direction k (0..3) on face f
};

CrossField computeField(const Mesh& m, const std::vector<Stroke>& strokes, const std::vector<int32_t>& faceLabel,
                        const FieldOptions& opt);

}  // namespace patina::retopo
