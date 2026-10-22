#pragma once
// Retopology, 0.15: symmetry for what is drawn (zone lines, flow strokes). A stroke is mirrored across the
// plane through the model's middle on each axis asked for. Points within the deadzone of the plane are put
// on it, so a stroke that ends near the middle joins its mirror image there (one line through the middle,
// or one closed loop when both its ends reach it), as with Blender's mirror clipping. A stroke drawn
// across the middle keeps its longer side, which is then mirrored: the result is symmetric, never two
// crossing copies. A stroke along the middle mirrors onto itself.
#include <vector>

#include "core/Mesh.h"
#include "core/RetopoField.h"

namespace patina::retopo {

// axes: bit 0 x, 1 y, 2 z; center: the planes' positions (the model's middle); deadzone: in model units
std::vector<Stroke> mirrorStroke(const Stroke& st, int axes, const vec3& center, float deadzone);

// a point mirrored across the planes in mask (the same bits)
vec3 mirrorPoint(const vec3& p, int mask, const vec3& center);

// 0.22.3: where the plane across axis (0 x, 1 y, 2 z) at `at` cuts the model's surface: its pieces in order,
// closed or open (at an open edge of the model, or where more than two pieces meet), each point exactly on it
struct SectionPiece {
  std::vector<vec3> points;
  bool closed = false;
};
std::vector<SectionPiece> middleSection(const Mesh& m, int axis, float at);

}  // namespace patina::retopo
