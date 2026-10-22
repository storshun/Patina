#pragma once
#include <string>
#include <vector>

#include "core/Math.h"

namespace patina {

enum EdgeFlag : uint8_t { EdgeSeam = 1, EdgeSharp = 2 };
// FaceMirrorCopy: the face's UVs are copied from its mirror image (a symmetric island), so
// the island it is in is stacked on that one on purpose. FaceMirrorX/Y/Z: the axes it was
// mirrored across (older files have none: found again from the geometry).
// FacePinned: the face's UVs are locked; UV edits leave them where they are.
enum FaceFlag : uint8_t {
  FaceMirrorCopy = 1,
  FaceMirrorX = 2,
  FaceMirrorY = 4,
  FaceMirrorZ = 8,
  FacePinned = 16,
};
constexpr uint8_t kFaceMirrorBits = FaceMirrorCopy | FaceMirrorX | FaceMirrorY | FaceMirrorZ;

// ---- texture tiles (0.9) ---------------------------------------------------------------
// A texture set per material slot (TileBySlot), UDIM tiles (TileByUdim), or both. All tiles share
// one square atlas texture of grid x grid cells; Mesh::loopAtlasUV maps each face corner into its
// tile's cell, so every texture-space pass (painting, compositing, baking) works on the atlas
// while loopUV stays exactly as authored. Mode 0: one texture, atlas UV = UV.
enum TileModeBits : uint8_t { TileBySlot = 1, TileByUdim = 2 };
constexpr int kMaxTiles = 64;  // grid <= 8
struct TileInfo {
  uint32_t slot = 0;  // material slot (TileBySlot)
  int udim = 1001;    // UDIM number (TileByUdim): 1001 + u + 10 v
  std::string name;
  bool operator==(const TileInfo& o) const { return slot == o.slot && udim == o.udim; }
};
struct TileLayout {
  uint8_t mode = 0;
  int grid = 1;
  std::vector<TileInfo> tiles;
  std::vector<int32_t> faceTile;  // per face
  bool active() const { return mode != 0; }
  int count() const { return std::max<int>(1, (int)tiles.size()); }
  vec2 cellOrigin(int t) const { return vec2(float(t % grid), float(t / grid)) / float(grid); }
  float cellSize() const { return 1.f / float(grid); }
  // the UV corner a tile's local 0..1 square starts at (UDIM tile corner; 0,0 for texture sets)
  glm::ivec2 uvOrigin(int t) const;
  // which tile a UV position (in a tile's local square) and cell-relative atlas position map to
  vec2 toAtlas(vec2 uv, int t) const;
  vec2 fromAtlas(vec2 atlas, int* tile = nullptr) const;  // the tile's UV for an atlas position
  int tileAtAtlas(vec2 atlas) const;
};
std::string udimName(int udim);
int udimOf(vec2 uv);              // UDIM number of the tile containing uv (1001 ..)
glm::ivec2 udimCorner(int udim);  // (u, v) of the tile's lower left corner

// A Blender vertex group as sent by the add-on: the vertices with a weight of 0.5 or more.
struct VertexGroup {
  std::string name;
  std::vector<uint32_t> verts;
};

// Polygon mesh in the same "loop" model Blender uses:
//   vertices -> positions
//   faces    -> ranges of loops (faceStart[f] .. faceStart[f+1])
//   loops    -> a face corner: vertex index + UV + normal
// Loop order is preserved through import/export so UV edits map 1:1 back to Blender.
struct Mesh {
  std::string name = "mesh";

  std::vector<vec3> positions;
  std::vector<uint32_t> faceStart;  // size F+1
  std::vector<uint32_t> loopVert;   // size L
  std::vector<vec2> loopUV;         // size L
  std::vector<vec3> loopNormal;     // size L
  std::vector<vec4> loopTangent;    // size L (xyz tangent, w = bitangent sign)
  std::vector<vec4> loopColor;      // size L when the file has vertex colors (linear RGBA), else empty
  std::vector<vec2> loopUV2;        // size L with a second UV map (lightmaps, 0.35), else empty
  std::vector<uint32_t> faceMaterial;  // size F (material slot, informational)
  std::vector<std::string> materialNames;  // per slot, when the file names them
  TileLayout tiles;                        // texture sets / UDIM tiles (mode 0: off)
  std::vector<vec2> loopAtlasUV;           // size L when tiles are active, else empty
  std::vector<uint8_t> faceFlags;      // size F (FaceFlag bits)
  std::vector<VertexGroup> vertexGroups;  // from Blender (optional; names the parts of a model)
  // (1.2) the file's meshes (glTF and FBX nodes, OBJ objects) the faces came from, when it has several: the object of
  // each face and their names (empty with one). Loaded files only; splitObjects() makes an object of each.
  std::vector<uint32_t> faceObject;
  std::vector<std::string> objectNames;
  // (1.2) Blender's sculpt face set of each face (0: none), as the add-on 0.14 sends them; empty without
  std::vector<uint32_t> faceSetIds;
  bool hasSourceNormals = false;
  float autoSmoothAngleDeg = 60.f;

  // Edges (size E). For Blender meshes these keep Blender's edge indices.
  std::vector<glm::uvec2> edges;
  std::vector<uint8_t> edgeFlags;
  std::vector<uint32_t> loopEdge;  // edge between loop i and the next loop of its face

  // Triangulation: indices are loop indices.
  std::vector<glm::uvec3> tris;
  std::vector<uint32_t> triFace;

  // UV topology
  std::vector<uint32_t> loopIsland;  // island id per loop
  uint32_t islandCount = 0;
  std::vector<uint32_t> loopUVVert;  // shared "UV vertex" id per loop (vertex + identical UV)
  uint32_t uvVertCount = 0;

  AABB bounds;
  float boundingRadius = 1.f;  // max distance of a vertex from bounds.center()

  uint32_t faceCount() const { return faceStart.empty() ? 0 : uint32_t(faceStart.size() - 1); }
  uint32_t loopCount() const { return uint32_t(loopVert.size()); }
  uint32_t faceSize(uint32_t f) const { return faceStart[f + 1] - faceStart[f]; }
  uint32_t nextLoop(uint32_t l, uint32_t f) const {
    return l + 1 < faceStart[f + 1] ? l + 1 : faceStart[f];
  }
  vec3 faceNormal(uint32_t f) const;
  vec3 faceCenter(uint32_t f) const;
  float faceArea3D(uint32_t f) const;
  float faceAreaUV(uint32_t f) const;  // signed
  std::vector<uint32_t> loopFace() const;

  // Must be called after the raw arrays are filled. Builds everything derived.
  // If edges are already present (from Blender) they are kept and loopEdge is matched.
  void finalize();

  void buildEdges();
  void triangulate();
  void computeNormals();
  void computeTangents();
  void computeBounds();
  void computeUVTopology();  // tiles + islands + uv-verts; call after UV changes
  // The texture coordinate of a corner: its place in the atlas (= loopUV without tiles).
  vec2 texUV(uint32_t l) const { return loopAtlasUV.empty() ? loopUV[l] : loopAtlasUV[l]; }
  int faceTile(uint32_t f) const { return f < tiles.faceTile.size() ? tiles.faceTile[f] : 0; }
  // Assigns faces to tiles from `uvs` (empty: loopUV), keeping the listed tiles (new ones are
  // appended, the grid grows as needed), and fills loopAtlasUV. Returns true if tiles or grid changed.
  bool updateTiles(const std::vector<vec2>* uvs = nullptr);
  // Atlas UVs of `uvs` under `layout` (whose faceTile must be assigned for them).
  std::vector<vec2> atlasUVs(const std::vector<vec2>& uvs, const TileLayout& layout) const;
  std::string slotName(uint32_t slot) const;
  // The face has no area in UV space (its corners on one point or one line): it gets no texels.
  bool faceHasNoUVArea(uint32_t f, const std::vector<vec2>& uvs) const;
  bool validate(std::string* err = nullptr) const;
};

// Helper to build a mesh in code (tests, primitives).
Mesh makeCube(float size = 1.f);
Mesh makeUVSphere(int segments = 48, int rings = 24, float radius = 1.f);
Mesh makePlane(float size = 2.f, int subdiv = 1);
Mesh makeTorus(float major = 1.f, float minor = 0.35f, int segU = 64, int segV = 24);

}  // namespace patina
