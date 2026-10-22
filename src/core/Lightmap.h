#pragma once
#include <string>
#include <vector>

#include "core/Math.h"

namespace patina {

struct Mesh;

// (0.35) A second UV map for baked lighting (Unity, Unreal, Godot lightmaps): every face its own space (no island
// overlaps another, mirrored and stacked islands get their own), the same texel density everywhere, and a gap
// between islands of `padding` texels at the lightmap's size.
struct LightmapOptions {
  int size = 512;               // the lightmap resolution the padding is measured at
  float padding = 4.f;          // texels between islands (and from the border)
  std::string source = "uv";    // uv: the model's own UV islands, repacked; auto: new charts by xatlas
};

struct LightmapStats {
  int islands = 0;
  float fill = 0.f;        // share of the square the islands cover
  int overlaps = 0;        // faces that overlap another one (0 when it worked)
  int flatIslands = 0;     // islands with no UV area that were projected flat first
  int refolded = 0;        // faces of islands that overlapped themselves, projected again
  int neededSize = 0;      // auto: the charts did not fit this lightmap size with this padding; the size they need
};

// Makes the lightmap UVs of `m` (one per face corner). False when they did not fit at any scale.
bool makeLightmapUVs(const Mesh& m, const LightmapOptions& opt, std::vector<vec2>& uv2, LightmapStats* stats = nullptr);

}  // namespace patina
