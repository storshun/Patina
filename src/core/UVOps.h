#pragma once
// CPU UV algorithms operating on patina::Mesh (see core/Mesh.h).
// All functions modify mesh.loopUV in place and DO NOT call mesh.finalize();
// callers call mesh.computeUVTopology() + mesh.computeTangents() afterward.
//
// Conventions:
//  * UV (0,0) is bottom-left of the texture, (1,1) top-right.
//  * A "chart" for unwrapping = faces connected across edges that are NOT seams
//    (EdgeSeam flag in mesh.edgeFlags[mesh.loopEdge[loop]]).
//  * An "island" = mesh.loopIsland (connectivity in existing UV space).
//  * `faces` arguments: empty vector means "all faces".
#include <vector>

#include "core/Mesh.h"

namespace patina::uv {

// ---- topology helpers -------------------------------------------------------
// Faces grouped into charts separated by seam edges (and mesh boundaries).
std::vector<std::vector<uint32_t>> chartsFromSeams(const Mesh& m, const std::vector<uint32_t>& faces = {});
// Faces grouped by current UV island (mesh.loopIsland must be up to date).
std::vector<std::vector<uint32_t>> facesByIsland(const Mesh& m, const std::vector<uint32_t>& faces = {});
// Marks every edge whose two sides are in different UV islands (or that is a
// UV boundary) as a seam. Clears other seams if `clearOthers`.
void seamsFromIslands(Mesh& m, bool clearOthers = true);
// Marks edges whose dihedral angle exceeds `angleDeg` as seams.
void seamsByAngle(Mesh& m, float angleDeg, bool clearOthers = false);

// ---- carrying UVs over (0.44) -------------------------------------------------
// UVs for `to` from `from`, the same model before an edit (faces added or deleted, or the mesh sent again without
// its UVs): a face whose corners sit where a face of `from` has its corners gets that face's UVs, corner by corner;
// any other face takes its UVs from the nearest face of `from` that shares a corner with it (else the nearest of
// all), extended over it, so it lands in one island. Places match within `tolerance` (model units; 0: a millionth
// of the model's size). Sets every UV of `to`; returns the share of its faces that matched exactly (0..1).
float carryUVs(const Mesh& from, Mesh& to, float tolerance = 0.f);

// ---- unwrapping -------------------------------------------------------------
struct UnwrapOptions {
  int maxIterations = 2000;   // conjugate-gradient iterations per chart
  float tolerance = 1e-7f;
  bool pack = true;           // pack the resulting charts into 0..1 afterward
  float margin = 0.005f;      // packing margin (UV units)
};
// Least Squares Conformal Maps (Levy et al. 2002) per chart. Two vertices per
// chart (the farthest pair) are pinned. Charts without boundary (closed) are
// split with a fallback planar projection so the call never fails.
void unwrapLSCM(Mesh& m, const std::vector<uint32_t>& faces = {}, const UnwrapOptions& opt = {});

// Projects faces onto the plane perpendicular to `normal` (object space),
// result scaled to fit the 0..1 square preserving aspect.
void projectPlanar(Mesh& m, const vec3& normal, const std::vector<uint32_t>& faces = {});
// Box (cube) projection: each face uses the axis plane closest to its normal.
// Each of the 6 directions becomes its own set of charts; packed afterward.
void projectBox(Mesh& m, const std::vector<uint32_t>& faces = {}, float margin = 0.005f);
// "Smart UV project": groups faces into charts whose normals stay within
// `angleLimitDeg` of the chart's seed normal (region growing over adjacent
// faces), projects each chart planarly, then packs. Also marks chart borders as seams.
void smartProject(Mesh& m, float angleLimitDeg = 66.f, const std::vector<uint32_t>& faces = {}, float margin = 0.005f);

// Cylinder and sphere projections around an axis (0 x, 1 y, 2 z; -1: the direction the faces
// spread most for a cylinder, Y for a sphere), in model units, unrolled where the angle wraps;
// the cut becomes a seam, cylinder ends get a planar projection, and the pieces are packed.
void projectCylinder(Mesh& m, const std::vector<uint32_t>& faces = {}, int axis = -1, float margin = 0.005f);
void projectSphere(Mesh& m, const std::vector<uint32_t>& faces = {}, int axis = -1, float margin = 0.005f);
// Relaxes the UV islands containing `faces`: each is solved again conformally (LSCM over its own
// faces) and fitted back onto where it was (same center, rotation, area), so stretch goes away and
// the layout stays. strength 0..1 blends from the old layout. Returns the number of islands.
int relaxIslands(Mesh& m, const std::vector<uint32_t>& faces, float strength = 1.f);

// Seam paths over the model: vertices at the same position count as one (meshes split at UV
// seams or hard edges), and a path is the shortest chain of edges between two of them.
struct SeamGraph {
  static constexpr uint32_t kNoVertex = 0xffffffffu;
  std::vector<uint32_t> weld;                              // mesh vertex -> welded vertex
  std::vector<vec3> pos;                                   // welded vertex positions
  std::vector<uint32_t> adjStart;                          // welded vertex -> range in adj
  std::vector<std::pair<uint32_t, uint32_t>> adj;          // (neighbor, welded edge)
  std::vector<glm::uvec2> wedgeEnds;                       // welded edge -> its welded vertices
  std::vector<std::vector<uint32_t>> wedgeMesh;            // welded edge -> mesh edges on it
  size_t meshVerts = 0, meshEdges = 0;
  void build(const Mesh& m);
  bool matches(const Mesh& m) const;                       // built for this mesh (and still valid)
  uint32_t nearestVertex(const Mesh& m, uint32_t face, const vec3& p) const;  // welded
  std::vector<uint32_t> path(uint32_t fromWelded, uint32_t toWelded) const;   // welded edges
  void apply(std::vector<uint8_t>& edgeFlags, const std::vector<uint32_t>& wedges, bool mark) const;
};

// ---- packing ----------------------------------------------------------------
struct PackOptions {
  float margin = 0.005f;        // gap between islands, UV units at the final scale
  bool rotate = true;           // rotate islands to their minimum-area bounding box
  bool normalizeDensity = true; // scale islands to equal texel density (3D area / UV area)
  bool keepInPlace = false;     // false: pack into [0,1]^2
  bool shapes = true;           // pack the islands' outlines (nesting into each other's gaps); false: boxes
  int resolution = 512;         // grid of the shape packer over 0..1
  int candidates = 8;           // places the shape packer compares per island and rotation
};
struct ShapePackStats {
  int islands = 0;
  double scale = 0;  // the packed scale (texel density in UV units per model unit when equalized)
  double fill = 0;   // share of 0..1 the islands cover
};
// Packs the islands `charts` (face lists) by their outlines into 0..1, around `obstacles` (faces
// whose UVs stay where they are, kept `margin` away). Islands are equalized in density (unless
// opt.normalizeDensity is off), rotated to their smallest bounding box and tried at 0/90/180/270
// degrees (opt.rotate), and scaled up as far as they all fit, but not past `maxScale` (> 0).
// Mirror copies are skipped. False (UVs unchanged) if they don't fit at any scale.
bool packShapes(Mesh& m, const std::vector<std::vector<uint32_t>>& charts, const PackOptions& opt,
                const std::vector<uint32_t>& obstacles = {}, double maxScale = 0, ShapePackStats* stats = nullptr);
// Packs the UV islands containing `faces` (empty = all). Islands are found from
// current UV connectivity (mesh.loopIsland must be up to date) unless
// `useCharts` lists explicit face groups.
void pack(Mesh& m, const PackOptions& opt = {}, const std::vector<uint32_t>& faces = {});
void packCharts(Mesh& m, const std::vector<std::vector<uint32_t>>& charts, const PackOptions& opt = {});

// ---- transforms on a set of loops ------------------------------------------
// Loops of the given faces (deduplicated).
std::vector<uint32_t> loopsOfFaces(const Mesh& m, const std::vector<uint32_t>& faces);
Rect2 boundsOfLoops(const Mesh& m, const std::vector<uint32_t>& loops);
void translate(Mesh& m, const std::vector<uint32_t>& loops, const vec2& d);
void rotate(Mesh& m, const std::vector<uint32_t>& loops, float radians, const vec2& pivot);
void scale(Mesh& m, const std::vector<uint32_t>& loops, const vec2& s, const vec2& pivot);
void flip(Mesh& m, const std::vector<uint32_t>& loops, bool horizontal);
// Moves all loops sharing a UV vertex (mesh.loopUVVert) to their average.
void weld(Mesh& m, const std::vector<uint32_t>& loops);
// Fits the given loops into the 0..1 square (uniform scale, centered).
void fitToUnitSquare(Mesh& m, const std::vector<uint32_t>& loops, float margin = 0.f);

// ---- metrics ----------------------------------------------------------------
// Texel density in UV units per model unit: sqrt(UV area / 3D area) over the faces with UV area.
float texelDensity(const Mesh& m, const std::vector<uint32_t>& faces = {});
// Per-face area distortion (log2 of uv/3d area ratio relative to mesh mean); for heat-map display.
std::vector<float> areaDistortion(const Mesh& m);
// Fraction of the unit square covered by UVs (0..1), ignoring overlaps, estimated by rasterizing at `res`.
float coverage(const Mesh& m, int res = 256);
// Number of face pairs whose UV triangles overlap (estimated by rasterizing at `res`).
int overlapCount(const Mesh& m, int res = 256);

}  // namespace patina::uv
