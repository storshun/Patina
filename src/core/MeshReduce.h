#pragma once
// 0.29: fewer polygons (decimation) and levels of detail, keeping the UV layout: UV seams, material borders and
// (optionally) open borders stay where they are, and every corner keeps its UV, so the textures fit as they are.
#include <string>

#include "core/Mesh.h"

namespace patina {

struct ReduceOptions {
  float ratio = 0.5f;            // share of the triangles to keep (0..1]
  size_t targetTriangles = 0;    // > 0: this many instead of the ratio
  float maxError = 1.f;          // stop before the shape moves further than this share of its size (1: reach the target)
  bool keepBorders = true;       // open borders don't move (clothes, planes)
  float normalWeight = 0.5f;     // how much keeping the shading counts against keeping the shape
};
struct ReduceResult {
  size_t trianglesBefore = 0, trianglesAfter = 0;
  float error = 0.f;             // how far the shape moved, as a share of its size
};
// The reduced mesh: triangles, the same vertices (a subset), the same UVs, slots, seams and vertex groups.
Mesh reduceMesh(const Mesh& m, const ReduceOptions& o, ReduceResult* r = nullptr);

// 0.30: size, units, turn and place (the same faces and UVs). Rotations are in Blender's axes (Z up), degrees, X then
// Y then Z; origin: "keep", "center" (the bounding box's center), "base" (the center of its bottom); ground puts the
// lowest point at height 0.
struct TransformOptions {
  float scale = 1.f;
  vec3 rotateDeg{0.f};
  std::string origin = "keep";
  bool ground = false;
};
void transformMesh(Mesh& m, const TransformOptions& o);

}  // namespace patina
