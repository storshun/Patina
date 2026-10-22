#pragma once
// Island-level UV tools: turn a set of faces into a well-behaved UV island
// (seams around it, unwrapped, upright, at the right texel density, placed in
// free space), density targets, overlap repair and stretch metrics.
// Like UVOps these modify mesh.loopUV (and edge seam flags) in place; callers
// run mesh.computeUVTopology() + computeTangents() afterward.
#include <string>
#include <vector>

#include <functional>

#include "core/Mesh.h"
#include "core/Bvh.h"

namespace patina::uv {

// ---- texel density -----------------------------------------------------------
// Density is measured in UV units per object unit (multiply by the texture
// resolution for texels per unit). Area weighted: sqrt(sum UV area / sum 3D area).
float areaDensity(const Mesh& m, const std::vector<uint32_t>& faces = {});
// Fingerprint of the UV layout and topology (for caching derived data). A two-million-face model has
// 46 MB of UVs and several panels ask every frame, so the value is kept until uvHashNewFrame() (the app
// calls it each frame, the engine before each command).
uint64_t uvHash(const Mesh& m);
void uvHashNewFrame();
// (0.22.2) while held, a new frame keeps the value (a line being drawn on the model: the UVs can't change)
void uvHashHold(bool hold);

struct DensityTargets {
  float fit = 0;     // the whole surface (mirror copies share texels, so they don't count) fills `fill` of the texture
  float screen = 0;  // one texel per screen pixel with the model framed in a `screenHeight` tall view
  float layout = 0;  // what the existing islands in 0..1 use: area-weighted median (0 = no usable layout)
  int resolution = 0;
  // the recommended target: the layout when there is one (at most twice fit, so heavily
  // overlapping layouts don't ask for more than the texture holds), otherwise the smaller of
  // fit and screen
  float recommended() const;
  float texelsPerUnit(float uvDensity) const { return uvDensity * resolution; }
};
// `exclude` (per face, optional): faces left out of the layout measurement.
DensityTargets densityTargets(const Mesh& m, int resolution, const std::vector<uint8_t>* exclude = nullptr,
                              float screenHeight = 1080.f, float fill = 0.7f);

// ---- texture tiles ----------------------------------------------------------------
// Runs `fn` once per texture tile that `faces` touch (empty: every tile), with that tile's faces
// moved so the tile is the 0..1 square and every other tile's faces collapsed out of the way, so
// placement, packing and overlap checks see one tile at a time. `fn` gets the tile's faces (an
// explicit list) and may change their UVs, seams and flags. Without tiles: fn(faces) once.
void forEachTile(Mesh& m, const std::vector<uint32_t>& faces,
                 const std::function<void(const std::vector<uint32_t>& tileFaces)>& fn);

// ---- island operations ---------------------------------------------------------
enum class Orient { None = 0, Upright, LongestAxis };
const char* orientName(Orient o);
Orient orientFromName(const std::string& s);

// Rotates one connected chart (its UVs) about its center: Upright turns the
// direction in which the surface goes up (object +Y) to +V; LongestAxis lays
// the minimum-area bounding box along U. Upright falls back to LongestAxis on
// surfaces that are (nearly) horizontal.
void orientChart(Mesh& m, const std::vector<uint32_t>& chart, Orient o);
// Uniformly scales a chart about its UV center to `uvDensity`.
void scaleChartToDensity(Mesh& m, const std::vector<uint32_t>& chart, float uvDensity);

// Moves the given charts into empty parts of 0..1, leaving every other face
// where it is. Larger charts are placed first; rotation by 90 degrees is tried
// when `allowRotate`. Returns false (and leaves the charts that did not fit
// unchanged) if some chart finds no room.
bool placeInFreeSpace(Mesh& m, const std::vector<std::vector<uint32_t>>& charts, float margin = 0.005f,
                      bool allowRotate = false, int gridRes = 512);

struct IslandOptions {
  Orient orient = Orient::Upright;
  float uvDensity = 0.f;          // <= 0: DensityTargets::recommended() for `resolution`
  int resolution = 2048;          // texture size, for the automatic density
  float margin = 0.005f;          // gap to other islands (UV units)
  bool markSeams = true;          // seams around the region, none inside it
  bool repackIfFull = true;       // no free space: repack every island at equal density
};
struct IslandResult {
  bool ok = false;
  int charts = 0;                 // islands made (a selection in several pieces gives several)
  bool repacked = false;          // the whole layout was repacked to make room
  float uvDensity = 0.f;          // density used
  std::string message;
};
// Makes the faces one UV island (one per connected piece). Empty = all faces.
IslandResult makeIsland(Mesh& m, const std::vector<uint32_t>& faces, const IslandOptions& opt = {});

// ---- symmetry -------------------------------------------------------------------
// axisMask: bit 0 X, 1 Y, 2 Z (mirror planes through the object origin).
// Per vertex: the vertex it maps onto when mirrored by `flipMask` (one of the vertices at that position), or -1.
// A vertex on the middle maps onto itself.
std::vector<int> mirrorVertices(const Mesh& m, const Bvh& bvh, int flipMask);
// The face a face maps onto when mirrored by `flipMask` (a subset of the axes), or -1.
int mirrorFace(const Mesh& m, const Bvh& bvh, uint32_t f, int flipMask);
// The face(s) a face maps onto: the exact mirror image, or where the other side is triangulated
// differently (a quad split along the other diagonal), the faces covering its mirror image.
std::vector<uint32_t> mirrorFaces(const Mesh& m, const Bvh& bvh, uint32_t f, int flipMask);
// `faces` plus their mirror images for every combination of the axes.
std::vector<uint32_t> withMirroredFaces(const Mesh& m, const Bvh& bvh, const std::vector<uint32_t>& faces, int axisMask);
// makeIsland for a symmetric region: the faces on the positive side of the axes become the
// island(s); their mirror images get the same UVs (mirrored), so both halves share one place
// in the texture. Faces without a mirror partner in the region are unwrapped normally.
// res.message tells how many faces were stacked.
IslandResult makeSymmetricIsland(Mesh& m, const Bvh& bvh, const std::vector<uint32_t>& faces, int axisMask,
                                 const IslandOptions& opt = {}, int* stackedFaces = nullptr);

// Copies the UVs of the faces on the positive side of the axes onto their mirror images on
// the negative side (mirrored), so both halves share one place in the texture. Only faces
// with a mirror partner change. Returns the number of faces that took their partner's UVs.
int mirrorUVs(Mesh& m, const Bvh& bvh, int axisMask, const std::vector<uint32_t>& faces = {});

// ---- mirror links: copies follow the half they copy -------------------------------
// For every face flagged FaceMirrorCopy: the face on the other side of the mirror it takes
// its UVs from, and for each of its corners the corner it copies.
struct MirrorLinks {
  std::vector<int> primary;       // per face: the face it copies, -1 = not a copy
  std::vector<uint32_t> source;   // per loop: the loop whose UV it takes (itself if not a copy)
  std::vector<uint32_t> stale;    // flagged faces whose mirror image was not found
  size_t copies = 0;
  bool empty() const { return copies == 0; }
};
// Builds the links from m.faceFlags. Faces from older files without axis bits get them
// worked out from the geometry (written back into `upgradedFlags` when given).
// Where several faces could be the source, the one whose UVs the copy already has wins: first in
// `newUV` (the layout being applied), then in the mesh's own.
MirrorLinks mirrorLinks(const Mesh& m, const Bvh& bvh, std::vector<uint8_t>* upgradedFlags = nullptr,
                        const std::vector<vec2>* newUV = nullptr);
// After a UV edit (oldUV -> uv): a copy that moved while its source did not takes the source
// with it, then every copy takes its source's UVs, so stacked halves stay stacked whichever
// half was edited. Sources that are pinned don't follow. Returns the number of copy faces whose
// UVs had to be restacked.
int syncMirrorCopies(const Mesh& m, const MirrorLinks& links, const std::vector<vec2>& oldUV, std::vector<vec2>& uv,
                     const std::vector<uint8_t>& flags);
// Copies take their source's UVs again (repairs halves that were moved apart). Returns the
// number of faces changed.
int restackMirrorCopies(Mesh& m, const MirrorLinks& links);
// Copy faces that are not exactly on their source.
int unstackedCopies(const Mesh& m, const MirrorLinks& links);
// `faces` plus the faces linked to them (their copies, or the faces they copy).
std::vector<uint32_t> withLinkedFaces(const Mesh& m, const MirrorLinks& links, const std::vector<uint32_t>& faces);

// ---- pins ---------------------------------------------------------------------------
inline bool isPinned(const Mesh& m, uint32_t f) { return f < m.faceFlags.size() && (m.faceFlags[f] & FacePinned); }
size_t pinnedFaceCount(const Mesh& m);
// Faces with no area in UV space (corners on one point or one line) although they have area
// in 3D: they get no paint, and texture tiles put them wherever their collapsed corner lands.
std::vector<uint32_t> collapsedFaces(const Mesh& m);
// The faces (empty = all) without pinned ones.
std::vector<uint32_t> unpinnedFaces(const Mesh& m, const std::vector<uint32_t>& faces);
// The faces (empty = all) that can be laid out: not pinned and not mirror copies (copies follow
// the faces they copy).
std::vector<uint32_t> layoutFaces(const Mesh& m, const std::vector<uint32_t>& faces);
// Packs the islands of `faces` (empty = every island that is not pinned) at equal texel density
// into the space the other islands leave free, as large as they fit. Pinned islands and
// islands not listed stay where they are; the density matches the pinned islands when there
// are any. Returns false if nothing could be placed.
bool packAroundFixed(Mesh& m, const std::vector<uint32_t>& faces = {}, float margin = 0.005f, bool keepSizes = false);
// Repacks every island that can move: an ordinary pack, or packAroundFixed when islands are pinned.
void repackMovable(Mesh& m, float margin = 0.005f, bool rotate = true);

// ---- rectify ----------------------------------------------------------------------
struct RectifyResult {
  int islands = 0;     // islands straightened into grids
  int skipped = 0;     // islands left alone (triangles / n-gons, or not a clean grid of quads)
  std::string message;
};
// Straightens islands made of quads (strips and grids: belts, pipes, trims, planks) into
// rectangles on an axis-aligned grid: rows and columns become straight lines, each quad as
// long and wide as it is on the model. Density and position are kept; an island that then
// overlaps others moves into free space.
RectifyResult rectifyQuadIslands(Mesh& m, const std::vector<uint32_t>& faces = {}, float margin = 0.005f);

// Islands that overlap another island (the smaller one of each overlapping pair, or the one
// that is not pinned), as face lists. The halves of a symmetric island (the copy's faces
// flagged FaceMirrorCopy, each exactly on a face of the other island) are intended and not
// reported.
std::vector<std::vector<uint32_t>> overlappingIslands(const Mesh& m, int res = 512);
// Moves overlapping islands into free space (repacking everything that is not pinned if there
// is no room). Pinned islands never move. Returns the number of islands moved, or -1 if a full
// repack was needed.
int fixOverlaps(Mesh& m, float margin = 0.005f, bool repackIfFull = true);

// Orients the islands containing `faces` (empty = all) in place; islands that
// then overlap others are moved into free space.
void orientIslands(Mesh& m, const std::vector<uint32_t>& faces, Orient o, float margin = 0.005f);
// Scales the islands containing `faces` to `uvDensity` (about their centers);
// overlapping results are moved into free space (repack if needed).
void setIslandDensity(Mesh& m, const std::vector<uint32_t>& faces, float uvDensity, float margin = 0.005f);
// Places the islands containing `faces` into free space without moving the others.
bool packIntoFreeSpace(Mesh& m, const std::vector<uint32_t>& faces, float margin = 0.005f);

// ---- metrics ---------------------------------------------------------------------
// Per-face stretch: largest ratio of the two singular values of the 3D -> UV
// map over the face's triangles (1 = no stretch, 2 = one direction twice as
// dense as the other). 0 for faces without UV area.
std::vector<float> faceStretch(const Mesh& m);
// Per-face density in UV units per object unit (0 without UV area).
std::vector<float> faceDensity(const Mesh& m);

// ---- selection helpers -------------------------------------------------------------
// Faces sharing an edge with the selection (grow) / selected faces on its border removed (shrink).
void growSelection(const Mesh& m, std::vector<uint8_t>& sel);
void shrinkSelection(const Mesh& m, std::vector<uint8_t>& sel);

}  // namespace patina::uv
