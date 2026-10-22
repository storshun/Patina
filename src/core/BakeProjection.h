#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "core/Math.h"

namespace patina {

struct Mesh;
class Bvh;

// (1.1) Where the texels of a low-poly mesh lie on a high-poly surface: what the bake-down (the layers moved from a
// painted high-poly onto its low-poly version) and the high-poly bakes (normals, ambient occlusion, material slots)
// read. Each texel is sampled at several points spread over it. Each point looks along the low surface's normal
// (smoothed, so the rays do not part at hard edges) both ways out to `distance`, and takes the nearest hit that faces
// the same way; failing that, the nearest point of the high surface within twice the distance; failing that, the
// nearest point anywhere (a guess, which the check map shows).
enum class ProjectionKind : uint8_t { None = 0, Ray, Near, Far };

struct ProjectionOptions {
  int resolution = 0;      // the low mesh's texture atlas, texels across
  int samples = 9;         // points per texel: 1, 4, 9 or 16 (a rotated grid over the texel)
  float distance = -1.f;   // how far along the normal both ways; <= 0: measured (cageDistance)
  bool lookups = true;     // the high's atlas UV of every point (lookup)
  bool frames = false;     // the high's tangent frame per texel (highTangent): layers with a normal channel
  bool bakeData = false;   // the hit position and material slot per texel (the AO and material slot bakes)
  // The GPU's coverage of the low's texels (posMap, RGBA32F in GL row order: xyz, w = covered) and normals (nrmMap,
  // RGBA): a covered texel none of whose points falls inside a triangle (a sliver) is sampled at its center from them.
  const float* coverPos = nullptr;
  const float* coverNrm = nullptr;
};

struct Projection {
  int resolution = 0, samples = 0;
  float distance = 0.f;
  bool distanceMeasured = false;
  // lookups, point by point: point k of texel i (GL row order) at (k * resolution^2 + i) * 2, the high's atlas UV in
  // 65535ths (at most 65534); 65535, 65535 = none. What the GPU reads as RG16 / RGBA16 (two points) textures.
  std::vector<uint16_t> look;
  // per texel: the high's shading normal averaged over the points (xyz; w = 1 if any point found the surface), the
  // tangent at the texel's middle point (xyz, w = bitangent sign), the hit position there (w = 1 if any) and its
  // material slot (255 = none)
  std::vector<vec4> highNormal, highTangent, hitPos;
  std::vector<uint8_t> hitMaterial;
  // per texel: the share of its points found by the nearest point (r) and guessed (g), 0..255; b = 255 where the
  // low's face faces away from the high (turned inside out); a = 255
  std::vector<uint8_t> check;  // RGBA8
  // counts over all points
  long points = 0, byRay = 0, byNear = 0, guessed = 0;
  long texels = 0;
  long flipped = 0;  // texels whose low face faces away from the high
  float largestDistance = 0.f;  // the farthest point found by a ray or the nearest point
  double seconds = 0.0;
};

// How far apart the two surfaces are: the low's vertices and face centers, each to the nearest point of the high
// surface. Returns the distance to look along the normals (1.5 x the 99th percentile, at least 0.3% of the radius);
// < 0 when more than 5% of the low lies farther than 30% of its radius from the high (`err` says so: the two are not
// in the same place, or not at the same scale).
float cageDistance(const Mesh& low, const Mesh& high, const Bvh& highBvh, std::string* err = nullptr);

// Smooth normals for the rays: per position (welded by place), area-weighted.
std::vector<vec3> rayNormals(const Mesh& m);

bool projectOntoHigh(const Mesh& low, const Mesh& high, const Bvh& highBvh, const ProjectionOptions& opt,
                     Projection& out, std::string* err = nullptr);

// The point pattern: samples (1, 4, 9, 16) offsets within the texel (0..1), the middle one first.
std::vector<vec2> texelPattern(int samples);

}  // namespace patina
