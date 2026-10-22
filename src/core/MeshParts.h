#pragma once
// Named parts of a model, for commands that act on "the head", "the Jeans" or "piece 2"
// (Patina Assistant, 0.11): material slots, connected pieces, Blender vertex groups and
// regions found from the shape (head and body). Each part is a list of faces.
#include <string>
#include <vector>

#include "core/Mesh.h"

namespace patina::parts {

struct Part {
  std::string id;     // slot:<name> | piece:<n> | group:<name> | region:<name>
  std::string kind;   // slot | piece | group | region
  std::string label;  // plain words: "Skin (material slot)", "the largest piece, at the top"
  std::vector<uint32_t> faces;
  bool fallback = false;   // region: a slot or group is named like it and should be preferred
  float confidence = 1.f;  // region: how clearly the shape showed it (0..1)
};

// The neck: the narrowest horizontal slice in the top of the model with clearly wider slices
// both above (the head) and below (the shoulders or body).
struct NeckResult {
  bool found = false;
  float y = 0.f;           // height of the cut (object space, Y up)
  float width = 0.f;       // sqrt of the slice's convex hull area
  float ratioAbove = 0.f;  // widest slice above / neck width
  float ratioBelow = 0.f;  // widest slice below (within reach) / neck width
  std::string why;         // when not found: why, in plain words
  int piece = -1;          // found on this piece alone (facePieces numbering; -1: the whole model)
};

// Widths (sqrt of the convex hull area of the cross-section) of `bands` horizontal slices,
// bottom to top. A slice that cuts nothing has width 0.
std::vector<float> sliceWidths(const Mesh& m, int bands = 100, const std::vector<uint8_t>* faceMask = nullptr);
// Looks at the whole model first; when long hair or a cape hides the neck, at each of the largest
// pieces on its own (the body under the hair).
NeckResult findNeck(const Mesh& m);

// Connected pieces (vertices at the same position count as one): a piece id per face, the
// pieces numbered by size (0 = most faces). `count` receives the number of pieces.
std::vector<uint32_t> facePieces(const Mesh& m, uint32_t* count);

// The faces of the head above a found neck: everything connected above the cut, plus whole
// pieces (eyes, hair, a hat) that sit above it within the head's footprint.
std::vector<uint8_t> headFaces(const Mesh& m, const NeckResult& neck);

// Faces of a vertex group: those with every corner in the group (or, when that gives none,
// at least half of them).
std::vector<uint32_t> groupFaces(const Mesh& m, const VertexGroup& g);

// "head", "body" or "" for a slot or group name such as "M_Head", "face_skin", "Body.001".
std::string regionHint(const std::string& name);

// Seams that make each connected piece of `faces` one UV island: seams on the border of the
// faces, none inside, and, for a closed piece or one with several border loops (a sphere, a
// tube), a cut along its back (-Z, away from the front view) from its top to its bottom, the
// way a head or a sleeve is cut. `backCut` forces that cut on every piece (a deep sack that
// unwraps badly in one go). Returns the number of seam edges the back cuts added.
int islandSeams(Mesh& m, const std::vector<uint32_t>& faces, bool backCut = false);

// Cuts from the far tips of the faces (hands, feet, a tail) back to their border or to an
// earlier cut, kept to the back of the model, so a whole body or a limb unrolls flat as one
// island: the seams a character artist would cut along the limbs. Tips closer than
// `minLength` (a share of the longest distance from the border) to an existing cut are left
// alone (fingers). Returns the number of seam edges added.
int tipCuts(Mesh& m, const std::vector<uint32_t>& faces, float minLength = 0.12f);

// The fewest extra seams that open every piece of the faces into a disk, which is what an
// unwrap needs to lie flat without folding: through handles (a ring, an arm touching the hip),
// along edges shared by more than two faces, and joining the existing seams and borders. The
// cuts prefer short edges at the back. Returns the number of seam edges added.
int diskCuts(Mesh& m, const std::vector<uint32_t>& faces);

// Connected pieces of the faces (joined across shared edges, seams or not).
size_t pieceCount(const Mesh& m, const std::vector<uint32_t>& faces);

// The faces as a small triangle mesh to unwrap on its own: vertices joined by an edge of zero
// length are one (a sphere pole of duplicated vertices), triangles without area are left out,
// seams carry over. Every corner remembers the loop it came from, so the UVs copy back.
// On a part mesh: the edge is the diagonal of one of the original faces (it is split in two
// triangles here, but a cut along it could not be made in the real mesh).
constexpr uint8_t kEdgeDiagonal = 0x80;
struct PartMesh {
  Mesh mesh;
  std::vector<uint32_t> srcLoop;  // per corner of `mesh`: the loop of the original mesh
};
PartMesh partMesh(const Mesh& m, const std::vector<uint32_t>& faces);
// Copies the part's UVs back onto the original faces (a corner left out with a degenerate
// triangle takes the UV its vertex has in the same face).
void copyPartUVs(const PartMesh& p, Mesh& m, const std::vector<uint32_t>& faces);

struct PartsReport {
  std::vector<Part> parts;
  NeckResult neck;
  uint32_t pieceCount = 0;
  bool symmetric = false;  // set by the caller (needs the symmetry check)
};
// Every part of the mesh: slots (when more than one is used), pieces (when more than one; the
// largest 16), vertex groups, and the head / body regions when a neck is found.
PartsReport findParts(const Mesh& m);

}  // namespace patina::parts
