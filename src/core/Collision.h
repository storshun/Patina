#pragma once
#include <string>
#include <vector>

#include "core/Math.h"

namespace patina {

struct Mesh;

// (0.34) Collision shapes for game engines: closed convex meshes (a box is one too).
struct ConvexShape {
  std::vector<vec3> verts;
  std::vector<glm::uvec3> tris;  // outward-facing
  bool box = false;              // an oriented box (engines name these UBX_)
  float volume() const;
};

// The convex hull of a set of points. With more points than `maxVerts` allows, the hull of the points furthest out
// in evenly spread directions (a little inside the exact hull). Flat or straight sets get a thin slab around them.
// False for fewer than 2 distinct points.
bool convexHull(const std::vector<vec3>& pts, ConvexShape& out, int maxVerts = 64);

// The smallest box (of the principal axes and the axis-aligned box) around the points, as a closed mesh.
ConvexShape orientedBox(const std::vector<vec3>& pts);

struct CollisionOptions {
  std::string mode = "pieces";  // box, boxes, hull, parts, pieces
  int maxShapes = 16;           // parts and pieces
  int maxVerts = 32;            // per hull (engines like 32 or fewer; PhysX allows 255)
  float concavity = 0.05f;      // pieces: split a piece while its hull holds more than this share of empty space
  int resolution = 40;          // pieces: voxels along the longest side
};

struct CollisionResult {
  std::vector<ConvexShape> shapes;
  float meshVolume = 0.f;   // of the model, when it is closed (else the voxel estimate)
  float shapesVolume = 0.f;
  std::string note;         // e.g. the model is open, so its inside was guessed
};

// The collision shapes of a model (object space). Modes: box (one oriented box), boxes (a box per separate part),
// hull (one convex hull), parts (a hull per separate part), pieces (the model split into convex pieces: its voxels,
// split where a hull would hold too much empty space).
CollisionResult makeCollision(const Mesh& mesh, const CollisionOptions& opt);

// Line segments (pairs of points) of the shapes' edges, for drawing them.
std::vector<vec3> collisionEdges(const std::vector<ConvexShape>& shapes);

}  // namespace patina
