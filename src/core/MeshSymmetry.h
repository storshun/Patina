#pragma once
#include <cmath>
#include <string>
#include <vector>

#include "core/Bvh.h"
#include "core/Mesh.h"

namespace patina {

// How far the model is from mirror-symmetric: every vertex is mirrored across a plane
// (x = planeX in object space, Blender's X) and the distance from there to the surface is
// measured. That works whatever the topology does: it compares shapes, not vertex lists.
struct SymmetryReport {
  float planeX = 0.f;
  float size = 0.f;         // the model's largest extent (object units)
  float tolerance = 0.f;    // what counts as asymmetric: 0.1% of the size
  float maxDist = 0.f, meanDist = 0.f, p95 = 0.f, median = 0.f;
  int vertices = 0;
  int beyond = 0;           // vertices further than the tolerance
  int unmatched = 0;        // no surface within 5% of the size on the other side
  std::vector<float> vertexDist;  // per vertex (< 0: unmatched)
  std::vector<float> faceDist;    // per face: the mean of its vertices (< 0: unmatched)
  struct Spot {
    vec3 position;
    float dist;
    uint32_t vertex;
  };
  std::vector<Spot> worst;  // up to 10 spots, far apart from each other, worst first
  struct Group {
    std::string name;
    int vertices = 0, beyond = 0;
    float maxDist = 0.f, meanDist = 0.f;
  };
  std::vector<Group> bySlot;    // per material slot
  std::vector<Group> byHeight;  // bottom, middle and top third of the model
};

// planeX: NAN = automatic (x = 0 when the model is centered on it, else the middle of the 2nd..98th
// percentile of x).
SymmetryReport measureSymmetry(const Mesh& m, const Bvh& bvh, float planeX = NAN);

}  // namespace patina
