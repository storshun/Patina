#pragma once
// Retopology (0.13): quads laid over a high-poly mesh, in patches the user paints, at a density per
// square meter the user paints too. See docs/RETOPO_PLAN.md.
//
// A patch is a connected set of high-poly faces with the same label. Its boundary is cut into
// curves where the region on the other side changes (another patch, or nothing). Where three
// patches meet, two have a corner and the flattest runs straight through (a four-valent vertex).
// Disk patches with 3 to 6 corners are filled with quads: a grid for four sides, a pole and four-
// sided parts around it otherwise; a ring patch (two boundary loops: an arm, a finger) becomes a
// cylinder. Edge counts are chosen for every curve at once, so neighboring patches share their
// vertices; where the density changes, edge loops end in three-to-one steps.
#include <array>
#include <memory>
#include <string>
#include <vector>

#include "core/Bvh.h"
#include "core/Mesh.h"

namespace patina::retopo {

struct Options {
  float quadsPerM2 = 2500.f;  // target density; model units are meters (2500: 2 cm quads)
  int relaxIterations = 12;   // smoothing of the quads' interior vertices, on the surface
  int maxSides = 6;           // a disk patch with more corners keeps its sharpest ones
};

struct PatchInfo {
  int label = 0;
  int sides = 0;        // a disk: 3..6; a ring: 0
  bool ring = false;
  int quads = 0;
  uint32_t faces = 0;   // high-poly faces
  std::string problem;  // why it wasn't filled (empty when it was)
};

struct Result {
  Mesh low;                           // the quads (no UVs yet)
  std::vector<int32_t> quadPatch;     // per low-poly face: the label of its patch
  std::vector<PatchInfo> patches;
  int quads = 0;
  float edgeLength = 0;               // the edge length the density means (density 1)
  std::vector<std::string> problems;  // patches left out, in plain words
  std::vector<vec3> uncovered;        // points of the surface the quads don't reach (parts too thin for them)
  bool cancelled = false;             // (flowQuads: stopped before it finished)
  int poles = 0;                      // inside vertices with other than four edges
  std::vector<int32_t> labels;        // the patches as built: the painted ones, tidied
  size_t tidied = 0;                  // faces the tidying moved to another patch
  int parityFixes = 0;                // curves that got one more edge so their patches add up
  // quality: of the vertices inside the quads (not on an open edge), how many have four edges
  // (regular), three or five (poles), or more; and of the faces, how many are quads
  struct Quality {
    int vertices = 0, regular = 0, three = 0, five = 0, other = 0;
    int faces = 0, quads = 0;
    double pct(int n) const { return vertices ? 100.0 * n / vertices : 0; }
  } quality;
  double seconds = 0;
  bool resolved = false;              // (flowQuads, Fewer poles: the quads' counts were solved again)
  // (0.23, flowQuads) what the quads were made from: the next build remakes only what changed
  std::shared_ptr<const struct FlowInputs> builtFrom;
  int localKept = -1;                 // (0.23) faces kept from the last quads (-1: all made again)
  bool unchanged = false;             // (0.23) nothing changed since the last quads: they are those (mirrored already)
};

// faceLabel: per high-poly face, 0 = leave it out, > 0 = its patch (faces with the same label that
// don't touch are separate patches). vertexDensity: per high-poly vertex, a multiplier on the
// density (1 = as set; 2 = twice the quads per m²); empty means 1 everywhere.
Result build(const Mesh& high, const Bvh& bvh, const std::vector<int32_t>& faceLabel,
             const std::vector<float>& vertexDensity, const Options& opt);

// Faces next to each face (across edges), for the brushes: built once per mesh.
struct FaceGraph {
  std::vector<uint32_t> start, items;
  uint32_t faces = 0;
  void build(const Mesh& m);
  bool builtFor(const Mesh& m) const { return faces == m.faceCount() && !start.empty(); }
};
// The faces within radius of a point, reached across the surface from the face there, so a brush
// doesn't reach through to the other side of a thin part (a hand, an ear).
std::vector<uint32_t> facesWithin(const Mesh& m, const FaceGraph& g, uint32_t face, vec3 center, float radius);

// Patches to start from: geodesic cells of about the same size over the given faces (seeds spread
// by farthest points, then moved to their cells' middles). A cell that isn't a disk or a ring (one
// around a tunnel in the mesh, or a whole closed piece) is split with another seed. Per face: 0 (not
// given) or its cell, from 1; cells: how many there are (count or a few more).
std::vector<int32_t> autoPatches(const Mesh& m, const std::vector<uint32_t>& faces, int count, int* cells = nullptr);

// The quads of a single patch in the plane, for tests: a polygon with the given number of edges
// along each side (3 to 6 sides; a pole for 3, 5 and 6), or a ring between two circles. The boundary
// points come first, in order around.
struct PlaneLayout {
  std::vector<vec2> points;
  std::vector<std::array<uint32_t, 4>> quads;
  std::string problem;  // empty when it worked
};
PlaneLayout layoutPolygon(const std::vector<int>& sideCounts);
PlaneLayout layoutRing(int outer, int inner, int rows);

}  // namespace patina::retopo
