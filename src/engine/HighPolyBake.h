#pragma once
#include <string>

#include <nlohmann/json.hpp>

#include "core/BakeProjection.h"

namespace patina {

class Document;
class Bvh;
class Texture2D;
struct Mesh;

// Bakes detail from a high-poly mesh (a sculpt) into the low-poly mesh's texture set. Every texel is projected onto
// the high surface (core/BakeProjection: points spread over the texel, rays along the low's normal both ways out to
// `distance`, the nearest point when they find nothing). What they find gives the normal (object space, averaged over
// the texel's points, stored in TextureSet::highNormal and combined with the stack's normal), the surface the AO is
// evaluated on, and the material slot.
struct HighPolyBakeOptions {
  float distance = -1.f;   // how far along the normals, object units; < 0: measured (1.1; it was 2% of the radius)
  bool normal = true;
  bool ao = true;
  bool materialIds = true;
  int aoSamples = 64;
  int samples = 4;         // (1.1) points per texel (1, 4, 9, 16): edges of the detail without stair steps
};

bool bakeFromHighPoly(Document& doc, const Mesh& high, const HighPolyBakeOptions& opt, nlohmann::json* stats,
                      std::string* err);

// (1.1) Projects the texels of `low` onto `high`. The GPU's coverage of them is read from `pos` and `nrm` (the low's
// posMap and nrmMap, or TextureSet::rasterizeGeometry's), so every texel it shades gets a value.
bool projectTexels(const Texture2D& pos, const Texture2D& nrm, const Mesh& low, const Mesh& high, const Bvh& highBvh,
                   ProjectionOptions opt, Projection& out, std::string* err);
// (1.1) The high-poly bake from a projection already made (with bakeData when the AO or the material slots are
// baked): what bakeFromHighPoly does after its own projection. The bake-down uses the one it moved the layers with.
void applyHighPolyBake(Document& doc, const Mesh& high, const Projection& p, const HighPolyBakeOptions& opt);
// The bake's report: hits along the rays, by the nearest point, guessed; the distance; a warning when many missed.
nlohmann::json projectionStats(const Projection& p);

}  // namespace patina
