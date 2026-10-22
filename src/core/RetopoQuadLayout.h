#pragma once
// Retopology, 0.21 (in progress): a quad layout from the lines. One cross field over the working
// surface, held along the lines; separatrices traced from its singularities and from the lines' ends and
// corners, each cut into the working triangles as it goes and stopped where it meets a line, another
// trace or the model's edge (a motorcycle graph). The regions between are the patches, four-sided in the
// field's terms; their sides' counts are solved for all at once and each is filled with a grid.
#include <array>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

#include "core/Mesh.h"

namespace patina::retopo {

struct QuadLayoutInput {
  std::vector<vec3> pos;
  std::vector<glm::uvec3> tri;
  std::unordered_set<uint64_t> lineEdges;  // working edges on the lines ((min << 32) | max)
  std::vector<std::vector<vec3>> lines;    // the lines as drawn (the field follows their own way)
  std::vector<char> lineIsEdge;            // per line: an edge loop runs along it (else a direction only)
  std::vector<int> triZone;                // per triangle: its zone
  std::vector<float> triL;                 // per triangle: the quads' size there
  int fieldSmoothing = -1;                 // normal smoothing for the field (-1: about a quad)
};

struct QuadLayout {
  // the working triangles, cut along the lines and traces
  std::vector<vec3> pos;
  std::vector<glm::uvec3> tri;
  std::vector<int> triPatch, triZone;
  std::vector<float> triL;
  std::vector<vec3> triDir;  // the field: one of its four directions per triangle
  std::vector<std::array<uint32_t, 4>> srcQuads;  // (a layout from quads: those quads, and each triangle's)
  std::vector<int64_t> triSrcQuad;
  // edges of the layout: kind 1 a line, 2 the model's open edge, 3 a trace, 4 a triangle's edge (quad layouts)
  std::vector<std::array<uint32_t, 2>> pathEdges;
  std::vector<uint8_t> pathKind;
  std::vector<uint32_t> singular;  // singular vertices
  std::vector<int8_t> singIndex;   // +1: three quads meet there, -1: five
  struct Patch {
    std::vector<std::vector<uint32_t>> loops;  // boundary loops (the patch on the left going along)
    std::vector<std::vector<int8_t>> turn;     // per loop vertex: +1 a corner, 0 straight, -1 a corner inwards
    int corners = 0;                           // corners minus inward corners, over all loops
    double area = 0;
    int zone = -1;
  };
  std::vector<Patch> patches;
  int traces = 0, tracesCut = 0;  // traces, and those stopped at their length limit (left open)
  int joined = 0;                 // pairs of separatrices joined (singularity to singularity)
  int singOnLines = 0;
  int pairsCancelled = 0;
  int looseCarried = 0;
  int fixTraces = 0;
  int patchesMerged = 0;
  int snapped = 0;                // traces that came close alongside another path and went to it          // pairs of patches made one (four corners together)              // traces added from corners inwards and singularities' double sectors           // loose ends carried on to the layout (traces that stopped short)         // a three and a five closer than a quad, left out            // singular triangles touching a line (left: the line holds the field there)
  std::vector<std::vector<uint32_t>> traceVerts;
  double seconds = 0;
};

QuadLayout buildQuadLayout(const QuadLayoutInput& in);

// The quads from a layout: every patch with one loop gets four corners (its own when it has four, else the
// four that suit it best), the counts of all the arcs between the layout's nodes solved at once (each
// patch's opposite sides equal; libSatsuma), and each patch filled with a grid through a harmonic map of
// its triangles onto the rectangle. Patches it can't fill (rings, more loops) are left open for now.
struct LayoutQuads {
  std::vector<vec3> pos;
  std::vector<std::array<uint32_t, 4>> quads;
  std::vector<std::array<uint32_t, 3>> tris;  // (small holes closed)
  std::vector<int8_t> quadKind;                // (per quad, for looking at: 1 grid, 2 pattern, 3 corners chosen, 4 stepped, 5 hole)
  int holesFilled = 0, wedges = 0, patchesKept = 0;
  int patchesFilled = 0, patchesForced = 0, patchesLeft = 0, arcs = 0;
  int patchesCollapsed = 0, quadsDropped = 0, patchesPattern = 0, patchesStepped = 0;
  int whyLeft[5] = {0, 0, 0, 0, 0};  // (not one loop, a vertex twice in it, sides not found, uneven counts: grid, pattern)  // (patches a quad thin or less folded away; quads that lost a corner)
  double solveCost = 0, seconds = 0;
};
struct LayoutQuadsOptions {
  float polePull = 0;          // a run of edges between a three- and a five-edged pole drawn toward nothing (they cancel)
  bool smoothLengths = true;   // count from each run's length smoothed (a zigzag along working triangles); not for quads
  bool keepLeftRuns = false;   // runs beside a patch left open keep their edges' count (a quad mesh's triangles stay as they were)
  bool keepCounts = false;     // each run's count drawn toward its edges' count now (a quad mesh), not its length
  bool capHoles = false;       // holes up to 32 edges capped (a quad mesh's triangles whose runs changed); else 3-4, none on an open edge
  std::vector<char> hold;      // (0.23, a layout from quads) per vertex: runs mostly through these keep their count, very strongly
};
LayoutQuads quadsFromLayout(const QuadLayout& L, const LayoutQuadsOptions& opt = {});

// A layout from a quad mesh (to solve its counts again): its poles' separatrices along its edges (on straight
// through four-edged vertices until they meet another, a pole, a line or the edge), the lines' runs (edges
// along the drawn lines) and open edges kept; the patches are grids of its quads, their corners where a
// patch has one quad at a vertex. Triangles are patches of their own (left, closed again as small holes).
// vL: per vertex, the quads' size wanted there.
QuadLayout layoutFromQuads(const std::vector<vec3>& P, const std::vector<std::array<uint32_t, 4>>& quads,
                           const std::vector<std::array<uint32_t, 3>>& tris, const std::vector<float>& vL,
                           const std::vector<std::vector<vec3>>& lines, const std::vector<char>& lineIsEdge);
// For looking at: <prefix>_patches.obj (an object per patch: rect = four corners, other = not), _paths.obj
// (lines by kind), _sing.obj (singular vertices as small tetrahedra).
void dumpQuadLayout(const QuadLayout& L, const std::string& prefix);

}  // namespace patina::retopo
