#pragma once
// As-rigid-as-possible UV relaxation (Liu et al. 2008, "A Local/Global Approach to Mesh
// Parameterization"). Starts from an existing layout (LSCM) and makes every triangle as close
// to its true shape and size as the layout allows: LSCM keeps angles but can blow up or shrink
// long parts (limbs) and fold over; ARAP evens the sizes out and removes most fold-overs.
#include <vector>

#include "core/Mesh.h"

namespace patina::uv {

struct ArapOptions {
  int iterations = 12;
  int cgIterations = 200;
  double tolerance = 1e-7;
};

// Relaxes the UVs of `faces` (empty = all) in place. Corners of a vertex that share a UV stay
// together; seams (different UVs) stay open. The result is in object units (texel density
// 1 unit per unit): pack or scale it afterward. Returns false when there was nothing to solve.
bool arapRelax(Mesh& m, const std::vector<uint32_t>& faces = {}, const ArapOptions& opt = {});

}  // namespace patina::uv
