#pragma once
// Faces along the surface (0.43), for the Island brush's Surface reach: from the face under the cursor
// the brush spreads to the faces that share a corner with it, then to theirs, as far as its radius measured
// along the surface. So it never jumps to a surface that is only near in space: the other layer of a cloth
// with thickness, the other leg, a strand of hair lying over the face. And how much it takes doesn't depend
// on the angle the surface is seen at.
#include <cstdint>
#include <functional>
#include <vector>

#include "core/Mesh.h"

namespace patina {

// Which faces share a corner (vertices at one position count as one), kept for many brush dabs on one mesh.
struct FaceGraph {
  uint64_t key = 0;                         // the mesh it was built for (0: not built)
  std::vector<uint32_t> weld;               // vertex -> welded vertex
  std::vector<uint32_t> vfStart, vfFaces;   // welded vertex -> its faces
  void build(const Mesh& m);
  bool matches(const Mesh& m) const;
  static uint64_t keyOf(const Mesh& m);
};

// The faces whose centers lie within `radius` of `point` along the surface, spreading from `face` (the face
// under the cursor, always in) to neighbors that `pass` allows; a face it refuses neither joins nor passes the
// brush on. Distances go from face center to face center, so they run a little long across faces.
std::vector<uint32_t> facesAlongSurface(const Mesh& m, const FaceGraph& g, uint32_t face, const vec3& point,
                                        float radius, const std::function<bool(uint32_t)>& pass = {});

}  // namespace patina
