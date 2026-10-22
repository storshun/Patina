#pragma once
// Retopology, 0.17: parts too thin for the quads (fingers, thin strands). A face's thickness is how far
// a ray goes into the model from it before leaving it again; where fewer than about four quads would go
// round (thickness under 4/pi of a quad), the part is thin. Each thin tube (not a thin sheet, as a lock of
// hair, whose quads can stay as big) becomes a zone: its edge with the rest of the model as zone lines
// (round a finger's root), and a resolution that gives it about six quads round.
#include <cstdint>
#include <string>
#include <vector>

#include "core/Bvh.h"
#include "core/Math.h"
#include "core/Mesh.h"

namespace patina::retopo {

struct ThinPart {
  std::vector<std::vector<vec3>> loops;  // its edges with the thicker rest: zone lines (closed, the first point not repeated)
  uint32_t seedFace = 0;                 // a face inside it
  std::vector<uint32_t> sampleFaces;     // some of its faces, spread over it
  float thickness = 0;                   // how thick its thinner quarter is
  float resolution = 1;                  // the zone resolution it needs (a multiplier on the quads per m², a power of two)
  double area = 0;
  double loopLength = 0;                 // its lines' length, all told
  bool tube = true;                      // a tube (given a zone), or a thin sheet (left as it is)
  bool clean = true;                     // every edge with the rest cut cleanly across (a ring round the limb)
};

// faceL: the quads' size at each face (the quads per m², zones and density as they are now). Parts
// smaller than minQuads of the quads there now are left out (strands of hair, specks).
std::vector<ThinPart> findThinParts(const Mesh& m, const Bvh& bvh, const std::vector<float>& faceL, int quadsAround = 6,
                                    double minQuads = 2);

// 0.20.4: a loop right round the part of the model at p (an arm, a leg, the neck, a finger), across the
// way it runs there: its thickness from a ray into it, its axis from the normals round it (they all lie
// across a tube's axis), and the section of the model in the plane through p across that axis, the loop
// through p. Empty when the part there is no tube (flat, or a blob), or the cut doesn't close round it.
// why, if given, says which.
std::vector<vec3> ringAround(const Mesh& m, const Bvh& bvh, const vec3& p, std::string* why = nullptr);

// 0.22: a loop where a plane through p facing the viewer (its normal the way the eye looks, dir) cuts the
// model, the loop through p: round a breast, the face, a knee, wherever the model stands out toward you
// at that depth. Empty when that cut doesn't close round anything there.
std::vector<vec3> facingLoop(const Mesh& m, const Bvh& bvh, const vec3& p, const vec3& dir, std::string* why = nullptr);

}  // namespace patina::retopo
