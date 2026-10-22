#pragma once
// (1.2) Selecting faces in batches: face loops and rings through quads (Blender's Alt+click and Ctrl+Alt+click), the
// connected part, flat regions, and the faces' edge neighbors these walk over.
#include <cstdint>
#include <vector>

#include "core/Mesh.h"

namespace patina::select {

// Each edge's faces (two for an edge inside the surface, one on a border, more where faces meet a third time)
struct EdgeFaces {
  std::vector<uint32_t> start, faces;  // edge e: faces[start[e] .. start[e + 1])
  void build(const Mesh& m);
  bool builtFor(const Mesh& m) const { return start.size() == m.edges.size() + 1 && !m.edges.empty(); }
};

// The band of quads through face `f` across its edge `localEdge` (the edge from corner localEdge to the next): from
// f over that edge into the next face, out of it over the opposite edge, and so on both ways until a face that isn't
// a quad, a border, or the band closes. Blender's face loop (Alt+click by an edge). With f not a quad: {f}.
std::vector<uint32_t> faceLoop(const Mesh& m, const EdgeFaces& ef, uint32_t f, uint32_t localEdge);
// The band along that edge instead: across the face's two other edges (the ring, Ctrl+Alt+click)
std::vector<uint32_t> faceRing(const Mesh& m, const EdgeFaces& ef, uint32_t f, uint32_t localEdge);
// The corner (local edge index) of f whose edge is nearest a point on the face
uint32_t nearestEdge(const Mesh& m, uint32_t f, const vec3& p);
// The faces connected to the seeds by shared edges (bySharedVertex: by a shared vertex too)
std::vector<uint32_t> linked(const Mesh& m, const EdgeFaces& ef, const std::vector<uint32_t>& seeds, bool bySharedVertex = true);
// The faces reached from the seeds over edges where the surface bends less than `angleDeg` (a flat region: a box's
// side, a panel), the seeds included
std::vector<uint32_t> flatRegion(const Mesh& m, const EdgeFaces& ef, const std::vector<uint32_t>& seeds, float angleDeg);

}  // namespace patina::select
