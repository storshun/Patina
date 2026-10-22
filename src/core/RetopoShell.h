#pragma once
// Retopology: a clean working surface for the quads. The high-poly's surface, offset outwards by a
// little and rebuilt on a grid of small cubes (surface nets on its distance field): always closed and
// manifold, at about the detail the quads can show, with strands, specks, gaps and thin bits merged
// into the shapes around them. The quads are made on it, then put back on the high-poly.
#include "core/Bvh.h"
#include "core/Mesh.h"

namespace patina::retopo {

// voxel: the cube size; offset: how far outside the high-poly's surface the shell runs (about one cube);
// smoothPasses: Taubin smoothing of the cubes' steps.
Mesh voxelShell(const Mesh& high, const Bvh& bvh, float voxel, float offset, int smoothPasses = 3);

}  // namespace patina::retopo
