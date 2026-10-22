#pragma once
// Retopology, 0.16: the quads' edge loops (for showing the flow): from each edge, on through every
// vertex with four edges to the edge across from it, until a pole, an open edge or back round. Short
// loops (ending at poles close by) show where the flow is broken up.
#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "core/Math.h"
#include "core/Mesh.h"

namespace patina::retopo {

struct EdgeLoops {
  std::vector<std::vector<uint32_t>> loops;  // vertices in order (closed: the first not repeated)
  std::vector<char> closed;
  std::vector<uint32_t> poles;               // vertices with other than four edges (not on an open edge)
  std::vector<int> poleEdges;                // their edge counts
};

EdgeLoops edgeLoops(const Mesh& m);

// For finding the loop under the pointer: each edge's loop, and the edges by where their middles are
struct LoopFinder {
  std::vector<std::array<uint32_t, 2>> edges;
  std::vector<int> loopOf;  // per edge
  float cell = 1;
  std::unordered_map<uint64_t, std::vector<uint32_t>> grid;
  void build(const Mesh& m, const EdgeLoops& L);
  // the loop of the edge nearest p (within maxDist), or -1
  int nearest(const Mesh& m, const vec3& p, float maxDist) const;
};

// Segments (pairs of points) and a color per point for drawing: the poles as small crosses (three
// edges blue, five orange, more red) and the chosen loop (if any) in bright green.
void loopSegments(const Mesh& m, const EdgeLoops& L, float crossSize, int highlight, std::vector<vec3>& pts, std::vector<vec4>& cols);

}  // namespace patina::retopo
