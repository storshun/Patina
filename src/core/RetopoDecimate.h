#pragma once
// Retopology, 0.15: a lighter copy of the high-poly's own surface for making the quads on (instead of a
// voxel shell, which joined fingers and wrapped hair like shrink-wrap). Edges shorter than the target
// are collapsed, cheapest change of shape first (quadric error), never making the mesh non-manifold,
// turning a triangle over or leaving an edge much longer than the target; open edges stay where they are.
#include <cstdint>
#include <vector>

#include "core/Math.h"

namespace patina::retopo {

// pos/tri: a manifold triangle mesh (changed in place). Returns the triangles left.
size_t decimateToEdge(std::vector<vec3>& pos, std::vector<glm::uvec3>& tri, float targetEdge);
// The same with a target edge length per vertex (kept with the vertices left).
size_t decimateToEdge(std::vector<vec3>& pos, std::vector<glm::uvec3>& tri, std::vector<float>& targetPerVertex);

// The other way, for a coarse mesh: edges longer than the larger of their ends' targets are split (and
// their triangles with them, so the mesh stays whole), again until none is. Targets go with the vertices.
size_t refineToEdge(std::vector<vec3>& pos, std::vector<glm::uvec3>& tri, std::vector<float>& targetPerVertex,
                    size_t maxTriangles = 2000000);

}  // namespace patina::retopo
