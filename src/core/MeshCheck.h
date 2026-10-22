#pragma once
// 0.27: what a model is made of, what is wrong with it, and fixes for it (the Mesh Check window,
// `mesh_check`, `mesh_fix`). Fixes make a new mesh; UVs, material slots and face flags stay with
// their faces, and the document moves the paint over.
#include <string>
#include <vector>

#include "core/Mesh.h"

namespace patina {

struct MeshProblem {
  std::string id;               // "holes", "non_manifold_edges", ...
  std::string name;             // "Holes"
  std::string what;             // what it is and why it matters, in a sentence
  size_t count = 0;             // how many (holes, edges, faces, vertices or parts: `unit`)
  std::string unit;             // "holes", "edges", "faces", "vertices", "parts"
  std::vector<uint32_t> faces;  // the faces it concerns (to show and select)
  std::vector<vec3> where;      // a few places to look (object space)
  std::string fix;              // the fix that mends it (a MeshFixOptions switch), "" none
  int severity = 1;             // 0 a note, 1 a warning, 2 a problem for engines, printers and bakes
};

struct MeshStats {
  uint32_t vertices = 0, faces = 0, triangles = 0, quads = 0, ngons = 0, edges = 0, parts = 0;
  uint32_t slots = 0, uvIslands = 0;
  uint64_t trianglesRendered = 0;  // after triangulation (what a game engine draws)
  vec3 size{0.f};                  // bounding box (Blender units: meters)
  double area = 0;                 // surface area
  double volume = 0;               // enclosed volume (meaningful when closed)
  bool closed = false;             // every edge has exactly two faces
  float uvCoverage = 0;            // share of the UV square covered (0..1, faces' UV area summed)
};

struct MeshCheckReport {
  MeshStats stats;
  std::vector<MeshProblem> problems;   // the ones found, worst first
  std::vector<uint8_t> faceSeverity;   // per face: 0 fine, else 1 + the worst problem's severity
};

struct MeshCheckOptions {
  float weldDistance = 0.f;  // vertices closer than this count as an open seam (<= 0: 1e-5 of the size)
  bool uvOverlaps = true;    // look for overlapping UV islands (slower on big models)
};
MeshCheckReport checkMesh(const Mesh& m, const MeshCheckOptions& o = {});

struct MeshFixOptions {
  bool weld = false;                 // join vertices closer than weldDistance (open seams)
  float weldDistance = 0.f;          // <= 0: 1e-5 of the model's size
  bool removeDegenerate = false;     // faces with no area, or a corner twice
  bool removeDuplicateFaces = false;
  bool removeLoose = false;          // vertices no face uses
  bool removeSmallParts = false;     // specks and floaters
  float smallPartSize = 0.f;         // <= 0: 3% of the model's size
  bool fixWinding = false;           // faces turned like their neighbors, closed parts facing out
  bool fillHoles = false;
  int fillHoleEdges = 0;             // fill holes with up to this many edges (<= 0: 64)
};
struct MeshFixResult {
  size_t welded = 0, degenerate = 0, duplicates = 0, loose = 0, smallParts = 0, smallPartFaces = 0, flipped = 0,
         holesFilled = 0;
  bool changed() const { return welded || degenerate || duplicates || loose || smallParts || flipped || holesFilled; }
};
// The mended mesh (finalized). Unchanged parts keep their order.
Mesh fixMesh(const Mesh& m, const MeshFixOptions& o, MeshFixResult* r = nullptr);

}  // namespace patina
