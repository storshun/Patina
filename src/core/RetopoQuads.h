#pragma once
// Retopology, 0.14: the quads made along a smooth field over the whole model (QuadriFlow,
// third_party/quadriflow), steered by what the user sets: edge lines (zone lines, painted patches'
// borders) become exact edge loops, flow strokes turn the field, zones of different resolution are made
// apart and joined by rows of three-to-one steps, and the density brush sets the quads' size. Poles go
// only where the field needs them, so nearly every vertex has four edges. See docs/RETOPO_PLAN.md.
//
// QuadriFlow needs a clean (manifold) triangle mesh: a copy of the high-poly (or of a voxel shell round
// it, for big or messy models) is made one first, by cutting it apart where more than two triangles
// share an edge or neighbors face opposite ways; tiny loose pieces are left out. The quads are then
// put back on the high-poly's own surface.
#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "core/Bvh.h"
#include "core/Mesh.h"
#include "core/Retopo.h"
#include "core/RetopoField.h"

namespace patina::retopo {

struct FlowOptions {
  float quadsPerM2 = 2500.f;
  bool loopsAtBorders = true;  // the painted patches' borders are edge loops
  int relaxIterations = 4;     // smoothing on the high-poly's surface after QuadriFlow
  int shell = -1;              // work on a voxel shell (RetopoShell): 1 yes, 0 no, -1 when the model needs it
  float shellCubesPerQuad = 3; // its cube size: the quads' size over this
  int fieldSmoothing = -1;     // passes of normal averaging the field sees (-1: about a quad's worth)
  int seed = 0;
  int variants = 6;            // fields tried at once (seeds and smoothing); the one with fewest poles wins
  int passes = 3;              // runs at most to bring the quad count to the one asked for
  bool ringZones = true;       // a zone between two edge loops is filled with rings that follow both
  bool layout = false;         // (0.21, in progress) quads from a quad layout of the lines (RetopoQuadLayout)
  bool requantize = false;     // 0.21 (Fewer poles): the quads' counts solved again from their own layout
  int mirrorAxes = 0;          // 0.22.3 (Mirror X/Y/Z): the quads made on one side of the middle and mirrored (bits: 1 x, 2 y, 4 z)
  vec3 mirrorCenter{0};        // the middle planes' place
  bool mirrored = false;       // (the middle laid as lines already: the half is mirrored by the caller)
  std::vector<vec3> noRingsAt; // (not a ring zone: the zones holding these points, after rings there failed to join)
  int ringRetries = 0;         // (how many times rings have been taken out so far)
  const std::atomic<bool>* cancel = nullptr;  // set: stop (between steps) and return Result::cancelled
  std::shared_ptr<struct FlowCache> cache;    // kept between builds (the working surface), or none
  // 0.23: the last quads (with what they were made from): only those near what changed since are made
  // again (localReach quads round it), the rest kept as they were; none: all made again
  std::shared_ptr<const Result> previous;
  float localReach = 3.f;
};

// (0.23) what a build was made from (Result::builtFrom)
struct FlowInputs {
  std::vector<Stroke> strokes;
  std::vector<int32_t> faceLabel;
  std::vector<float> density, faceResolution;
  float quadsPerM2 = 0;
  bool loopsAtBorders = true, ringZones = true, requantize = false, layout = false, mirrored = false;
  int mirrorAxes = 0, shell = -1;
  vec3 mirrorCenter{0};
  uint32_t highFaces = 0;
  size_t highVertices = 0;
};

// faceLabel: painted patches (their borders become loops; may be empty). vertexDensity: per vertex
// multiplier on quadsPerM2 (the density brush; may be empty). faceResolution: per face, the zone's
// multiplier on quadsPerM2 (may be empty): zones of different resolution are made apart and joined by
// rows of 3-to-1 steps along the edge lines between them.
std::shared_ptr<FlowCache> makeFlowCache();

Result flowQuads(const Mesh& high, const Bvh& bvh, const std::vector<Stroke>& strokes, const std::vector<int32_t>& faceLabel,
                 const std::vector<float>& vertexDensity, const FlowOptions& opt, const std::vector<float>& faceResolution = {});

// (0.22.4, for the build and its tests) the holes of three or four edges in a set of quads (and triangles), each
// in the direction its faces run along it, found even where the boundary elsewhere isn't simple (strands of
// hair touching)
std::vector<std::vector<uint32_t>> smallHoles(const std::vector<std::array<uint32_t, 4>>& quads,
                                              const std::vector<std::array<uint32_t, 3>>* tris = nullptr);

}  // namespace patina::retopo
