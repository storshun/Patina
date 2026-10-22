#pragma once
// Retopology, 0.14: patches traced along a cross field. From every singularity the field's lines are
// followed until they meet a line already there, the mesh's edge, a painted patch's border or another
// singularity (a "motorcycle graph"); strokes are lines of their own. The regions between the lines
// are the patches, four-sided in the field's terms, so their quads follow the field and the only
// poles are its singularities.
#include <cstdint>
#include <vector>

#include "core/Mesh.h"
#include "core/RetopoField.h"

namespace patina::retopo {

struct LayoutOptions {
  float edgeLength = 0.02f;  // the quads' size (patches thinner than about one quad join a neighbor)
};

struct FieldLayout {
  std::vector<int32_t> faceLabel;            // per face: its patch, from 1
  std::vector<std::vector<vec3>> lines;      // the traced lines (for showing them)
  int patches = 0;
  int singularities = 0;
  int merged = 0;                            // thin patches joined to a neighbor
};

// painted: per face, patches painted by hand (their borders are lines too); may be empty.
FieldLayout traceLayout(const Mesh& m, const CrossField& F, const std::vector<Stroke>& strokes,
                        const std::vector<int32_t>& painted, const LayoutOptions& opt);

}  // namespace patina::retopo
