#pragma once
#include <map>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "engine/Notes.h"
#include "core/Bvh.h"
#include "core/Collision.h"
#include "core/Container.h"
#include "core/Mesh.h"
#include "core/MeshCheck.h"
#include "core/MeshReduce.h"
#include "core/MeshSymmetry.h"
#include "core/UVIsland.h"
#include "engine/HighPolyBake.h"
#include "engine/Layer.h"
#include "engine/TextureSet.h"
#include "engine/Undo.h"
#include "gfx/GpuMesh.h"

namespace patina {

class MaterialLibrary;

// Blender live-link state for the current document.
struct BridgeState {
  bool active = false;
  std::string object;        // Blender object name
  int blenderPort = 7861;
  std::string exchangeDir;
  bool uvsEdited = false;    // UVs differ from what Blender sent
  bool slotsEdited = false;  // faces moved to other material slots (texture sets): sent back too
  bool autoPush = false;     // push textures to Blender after every stroke (throttled)
};

enum class ExportPreset { Blender = 0, GLTF, Unreal, Unity, Count };
const char* exportPresetName(ExportPreset p);
ExportPreset exportPresetFromName(const std::string& s);

struct ExportOptions {
  ExportPreset preset = ExportPreset::Blender;
  std::string directory;
  std::string baseName;      // file prefix; default mesh name
  int size = 0;              // 0 = texture set resolution
  bool includeAO = true;
  bool includeThickness = true;   // when baked
  bool includeBentNormal = true;  // when baked (with the AO)
  bool psd = false;               // layered Photoshop files (one per map, a layer per Patina layer)
  bool height16 = true;
  // File names: $mesh (base name / mesh name), $project, $map (preset map name), $size,
  // $preset, $layer (name of the exported layer, or "all").
  std::string nameTemplate = "$mesh_$map";
  uint32_t layerUid = 0;  // export only this layer / folder (as if soloed); 0 = the whole stack
  bool maskOnly = false;  // with layerUid: write that layer's painted mask instead of the maps
  bool remember = true;   // becomes Document::lastExport ("export again"); off for Blender bridge pushes
  // opacity, sheen, clearcoat and subsurface maps: 0 when the texture set uses them, 1 always, 2 never
  int extraMaps = 0;
};
std::string expandExportName(const std::string& tmpl, const std::map<std::string, std::string>& vars);
struct ExportMap {
  std::string key, suffix;
  int mode, channels, bits;  // POST_EXPORT output, PNG channels and bits
};
// (0.47) a file an export would write (Document::exportPlan): shown before exporting
struct ExportPlanFile {
  std::string key;       // the map (basecolor, orm...), "@<tile>" after it with texture sets / UDIM tiles
  std::string file;      // the full path
  std::string contents;  // what its channels hold: "R occlusion, G roughness, B metallic"
  int channels = 3, bits = 8, size = 0;
  bool srgb = false;     // a color (sRGB) or data (linear: Blender's Non-Color)
};

// 0.28: the whole model with its textures (File > Export Model, export_model)
struct ModelExportOptions {
  std::string path;          // .glb, .gltf or .obj
  int textureSize = 0;       // 0: the texture set's size
  bool textures = true;      // false: the mesh only
  std::vector<float> lods;   // levels of detail after the model: the share of its triangles each keeps (0.29)
  // (0.34) collision shapes after the model: "" none, "unreal" (UCX_ / UBX_ names), "godot" (-convcolonly), "plain"
  std::string collision;
  // (1.2) where the object is in the scene: the glTF node's matrix; the vertices of an OBJ, STL or PLY are moved by it
  mat4 transform{1};
  std::string materialPrefix;  // (1.2) put before its material names (a scene's objects in one file)
};

// The open project: mesh, texture set, layer stack, undo history.
class Document {
 public:
  Mesh mesh;
  GpuMesh gpu;
  Bvh bvh;
  TextureSet tex;
  std::vector<std::unique_ptr<Layer>> layers;  // index 0 = bottom
  int selected = -1;
  bool editMask = false;  // paint the selected layer's mask instead of its content
  UndoStack undo;
  BridgeState bridge;
  std::string projectPath;
  std::string meshPath;
  std::vector<uint8_t> faceSelection;  // UV editor / fill selection (per face)
  bool dirty = false;                  // unsaved changes
  bool compositeDirty = true;
  bool compositeFull = true;   // (0.44.2) else only compositeBox changed
  glm::ivec4 compositeBox{0};
  int aoSamples = 64;

  bool hasMesh() const { return mesh.faceCount() > 0; }

  // ---- (1.2) the scene: a document is one object of it (a model with its layers and history; Engine::objects)
  // Its mesh stays in its own (object) space, where everything in texture space is made (positions for fills, bakes,
  // symmetry); the object -> world matrix places it in the scene: the placement it came with (the file's, Blender's
  // matrix_world), then moved, turned and scaled about its middle (`pivot`, in the world).
  struct Placement {
    mat4 base{1};          // where it came in
    vec3 pivot{0};         // what it turns and scales about (the middle of its model where it came in)
    vec3 location{0};      // moved from where it came in
    vec3 rotation{0};      // degrees about X, Y, Z (in that order)
    vec3 scale{1};
    mat4 matrix() const;   // object -> world
    bool moved() const { return location != vec3(0) || rotation != vec3(0) || scale != vec3(1); }
    bool operator==(const Placement&) const = default;
  };
  Placement placement;
  mat4 toWorld() const { return placement.matrix(); }
  bool placed() const { return toWorld() != mat4(1); }  // anywhere but where the model's own numbers are
  std::string objectName;    // its name in the Objects list ("": the mesh's)
  bool objectHidden = false;  // left out of the 3D view, renders and exports of the scene
  std::string displayName() const { return objectName.empty() ? mesh.name : objectName; }
  // A move, turn or scale as one undo step (`label`: "Move", "Turn"...)
  void setPlacement(const Placement& p, const std::string& label);

  // ---- (1.2) face sets: named, colored groups of faces kept in the project (a face is in one at most), as Blender's
  // sculpt face sets: one click selects, hides, fills or masks one; a stroke can be kept inside one (FaceSets.cpp)
  struct FaceSet {
    uint32_t id = 0;
    std::string name;
    vec3 color{0.8f};
    bool hidden = false;  // its faces left out of the 3D view, the brush and picking
    bool operator==(const FaceSet&) const = default;
  };
  std::vector<FaceSet> faceSets;
  std::vector<uint32_t> faceSetOf;   // per face: its set's id (0: none); empty: no face is in one
  uint32_t nextFaceSetId = 1;
  std::vector<uint8_t> hiddenFaces;  // per face: hidden (its set is); empty when no face is
  bool hideUnsetFaces = false;       // the faces in no set are hidden too (a set shown alone)
  const std::vector<uint8_t>* hiddenMask() const { return hiddenFaces.empty() ? nullptr : &hiddenFaces; }
  FaceSet* findFaceSet(uint32_t id);
  int faceSetIndex(const std::string& nameOrId) const;  // by name or id; -1 when there is none
  std::vector<uint32_t> faceSetFaces(uint32_t id) const;
  size_t faceSetSize(uint32_t id) const;
  // The face sets and which face is in which, as one undo step `label`
  // (hideUnset: the faces in no set hidden, 0 shown, -1 as they are)
  void setFaceSets(std::vector<FaceSet> sets, std::vector<uint32_t> of, const std::string& label, int hideUnset = -1);
  // A new set (its id) of `faces` (they leave the sets they were in), as one undo step
  uint32_t addFaceSet(const std::vector<uint32_t>& faces, const std::string& name, const vec3* color = nullptr);
  void updateHiddenFaces();  // hiddenFaces from the sets, and the 3D view's flags
  static vec3 faceSetColor(uint32_t id);  // a color of its own for each set (hues a golden angle apart)
  // The face sets moved onto a new mesh by where its faces are on the old one (a bake-down, a mended mesh)
  void carryFaceSets(const Mesh& oldMesh, const Bvh& oldBvh, const std::vector<uint32_t>& oldOf);
  static constexpr uint32_t kHiddenFlag = 32u;  // loop flag of the hidden faces (shaders leave them out)

  // Loads a mesh. keepLayers: keep the layer stack if the mesh topology matches
  // (same loop count), reprojecting paint when UVs differ.
  bool loadMesh(const std::string& path, int resolution, bool keepLayers, std::string* err = nullptr);
  // (0.31) The model's own materials (textures and colors of its material slots, from a .glb / .gltf / .obj /
  // .fbx) as a paint layer at the bottom of the stack, one undo step. `automatic`: right after opening it, only
  // when the materials are more than a plain gray (the texture set may grow to the textures' size, up to 4K).
  // Returns false with `err` empty when there was nothing to bring in. (0.44) A .ptmesh from Blender: its slots
  // as the add-on describes them (blenderMaterials), and flat vertex colors are left out. (1.2.2) `colorAlpha`: an
  // OBJ / FBX color texture's alpha, when the file says nothing about opacity: 0 the opacity when it cuts out (hair
  // cards, leaves), 1 the opacity whenever the image has alpha, 2 left out.
  bool importModelTextures(const std::string& path, bool automatic, json* report, std::string* err,
                           bool growResolution = true, int colorAlpha = 0);
  bool uvsGenerated = false;  // the model came without UVs (Patina made them)
  float uvsCarried = -1.f;    // (0.44) the last setMesh came without UVs and kept these (share of faces matched)
  bool blenderMaterials = true;  // (0.44) a .ptmesh's material slots count (feature bridge_materials)
  // (0.34) Collision shapes for game engines (Mesh > Collision...), for the mesh as it was when they were made:
  // collisionShapes() gives them while the mesh is still that one (else null).
  bool makeCollision(const CollisionOptions& opt, json* report, std::string* err);
  const CollisionResult* collisionShapes();
  void clearCollision() { collision_ = CollisionResult(), collisionKey_ = 0; }
  CollisionOptions collisionOptions;
  uint64_t collisionDrawKey() const { return collisionKey_; }
  uint64_t meshShapeKey() const;  // changes when the mesh's shape or corners do (not its UVs)
  // Called before the mesh is replaced (a load, a project): work still reading it finishes first.
  std::function<void()> beforeMeshChange;
  void setMesh(Mesh&& m, int resolution, bool keepLayers);
  bool startupScene = false;  // (0.46) the sphere Patina starts with, nothing opened or made since (any setMesh and,
                              // since 1.2.3, opening a project clear it)
  // (1.2.3) The sphere Patina starts with, and nothing painted or changed on it (nothing to undo): a model added or
  // dropped takes its place. Anything else stays, and the model comes in beside it.
  bool untouchedStartup() const { return startupScene && !undo.canUndo(); }
  void newTextureSet(int resolution);  // clears layers; resolution per tile
  void changeResolution(int resolution);  // resamples layer textures; resolution per tile

  // ---- texture tiles (0.9): texture sets per material slot and / or UDIM tiles ----------
  // The resolution of one tile (texture set / UDIM tile); the atlas is tileRes() * grid.
  int tileRes() const { return tex.resolution() / std::max(1, mesh.tiles.grid); }
  // Switches the layout (TileModeBits; 0 = one texture). Paint moves with it. Clears undo.
  bool setTileMode(uint8_t mode, std::string* why = nullptr);
  // A texture set of their own for some faces (0.11.1): they go into the material slot called
  // `name` (a new slot unless one has that name) and one texture becomes texture sets per slot.
  // Sets left without faces go. Paint moves with the faces; the undo history is cleared.
  // Returns the set's index (mesh.tiles), or -1 with `why`.
  int moveFacesToSlotSet(const std::vector<uint32_t>& faces, const std::string& name, std::string* why = nullptr);
  // UDIM tiles: the faces move to the next free tile, paint with them (one undo step, unless the
  // atlas has to grow). Returns the UDIM number, or 0 with `why`.
  int moveFacesToNewUdim(const std::vector<uint32_t>& faces, std::string* why = nullptr);
  // What a freshly loaded mesh calls for: UDIM when its islands sit in several UV tiles,
  // texture sets when material slots overlap each other in UV space.
  static uint8_t detectTileMode(const Mesh& m);
  // Whether an atlas of tileR x grid fits the GPU's largest texture (and a sane memory budget).
  static bool atlasFits(int tileR, int grid, std::string* why = nullptr);
  void rebakeMeshMaps(bool ao, bool curvature, bool thickness = false);
  // (0.44.2) baked maps made for other UVs (bit k: BakedMap k): UV edits since moved them along with the paint
  int bakesOutOfDate() const;
  void rebakeOutOfDate();  // those maps baked again (from the high-poly model when they came from it); fixes stay
  // ---- (0.41) baked map layers: AO, curvature and thickness as layers of their own (LayerType::Baked)
  bool bakedLayersOn = true;  // feature "baked_layers" (set by the engine)
  Layer* bakedLayer(int map);  // the layer of a baked map, or null
  // Gives every baked map without a layer one (at the bottom of the stack), once after each bake: checkpoint() and
  // updateComposite() call it. Part of the undo step being recorded (or of a fresh history).
  void syncBakedLayers();
  // A baked map's layer (made if the map is baked and has none): true if there is one now.
  bool addBakedLayer(int map);
  // Bakes the mesh maps a layer's smart masks read (curvature, AO, thickness) if they are missing.
  void ensureBakedFor(const Layer& l);
  int thicknessRays = 32;

  Layer& addPaintLayer(const std::string& name = "", int index = -1);
  Layer& addFillLayer(const MaterialRef& mat, const std::string& name = "", int index = -1);
  Layer& addAdjustLayer(const std::string& name = "", int index = -1);
  bool removeLayer(int index);
  // Instances: the layer whose content `l` shows (itself when it is not an instance)
  Layer* contentLayer(Layer& l);
  const Layer* contentLayer(const Layer& l) const;
  int addInstance(int index);      // a new instance of the layer above it; returns its index (-1: not possible)
  bool makeIndependent(int index); // copies the source's content into the instance
  bool moveLayer(int from, int to);
  int duplicateLayer(int index);
  Layer* selectedLayer() { return selected >= 0 && selected < (int)layers.size() ? layers[selected].get() : nullptr; }
  Layer* findLayer(uint32_t uid);
  int indexOf(uint32_t uid) const;
  void markComposite() { compositeDirty = true, compositeFull = true, dirty = true; }
  // (0.44.2) only this texel region changed (a brush stroke): the next composite does that region alone
  void markCompositeBox(const glm::ivec4& box) {
    if (!compositeDirty) compositeBox = box, compositeFull = false;
    else if (!compositeFull)
      compositeBox = glm::ivec4(glm::min(glm::ivec2(compositeBox), glm::ivec2(box)),
                                glm::max(glm::ivec2(compositeBox.z, compositeBox.w), glm::ivec2(box.z, box.w)));
    compositeDirty = true, dirty = true;
  }
  void updateComposite(MaterialLibrary& lib);
  uint64_t syncedBakeSerial() const { return syncedBakeSerial_; }

  // UV editing ---------------------------------------------------------------
  // Replaces all loop UVs (undoable). With reprojectPaint the painted layers and
  // masks are transferred to the new layout so existing paint stays in place.
  // Seams (newEdgeFlags) and face flags (pins, mirror copies) can change in the same step.
  // Faces that were pinned keep their UVs, and mirrored copies stay stacked on the faces they
  // copy (editing either half moves both). Returns false when nothing changed.
  // movePinned: pinned faces move too (a new texture set's layout) and stay pinned.
  bool applyUVs(const std::vector<vec2>& newUVs, bool reprojectPaint, const std::string& label,
                const std::vector<uint8_t>* newEdgeFlags = nullptr, const std::vector<uint8_t>* newFaceFlags = nullptr,
                bool movePinned = false);
  // Face flags only (pin / unpin), undoable.
  void applyFaceFlags(const std::vector<uint8_t>& flags, const std::string& label);
  // The mirror links of the current mesh and flags (cached).
  const uv::MirrorLinks& mirrorLinks();
  // Mesh symmetry (the Symmetry view mode, the mesh_symmetry command), cached until the geometry
  // or the plane changes. symmetryPlane: NAN = automatic; the window and the command set it.
  float symmetryPlane = NAN;
  const SymmetryReport& symmetry();
  // a face selection change (Island brush, Select tool) as one undo step; `before` is the old selection
  void pushSelectionUndo(const std::vector<uint8_t>& before, const std::string& label);
  uint64_t symmetryKey() const { return symmetryKey_; }
  // 0.27: the Mesh Check (numbers and problems), cached until the mesh or its UVs change
  const MeshCheckReport& meshCheck();
  uint64_t meshCheckKey() const { return meshCheckKey_; }
  // Mends the mesh. A copy of the project is kept first (revertMeshFix goes back to it: a mended mesh can't be
  // undone step by step), and the paint moves over to the new mesh. False with nothing to mend.
  bool fixMesh(const MeshFixOptions& o, MeshFixResult* r, std::string* err);
  bool canRevertMeshFix() const;
  bool revertMeshFix(MaterialLibrary& lib, std::string* err);
  // 0.29: fewer polygons, keeping the UV layout (the textures stay as they are); kept like a fix (revertMeshFix).
  // False when nothing would change.
  bool reduceMesh(const ReduceOptions& o, ReduceResult* r, std::string* err);
  const std::string& lastMeshChange() const { return meshChangeLabel_; }  // "fix", "reduction", "transform"
  // 0.30: size, units, turn and place the model (the same faces and UVs; kept like a fix)
  bool transformMesh(const TransformOptions& o, std::string* err);
  void applySeams(const std::vector<uint8_t>& edgeFlags, const std::string& label);
  void updateSelectionFlags();
  // Faces the current stroke may paint (topology / island lock); empty = no lock.
  // Shown to the stamp shader as loop flag kPaintLockFlag.
  std::vector<uint8_t> paintLockFaces;
  static constexpr uint32_t kPaintLockFlag = 4u;
  // (0.37.1) Loop flag of the pinned faces (FacePinned), for the 3D view's stripes on them.
  static constexpr uint32_t kPinnedFlag = 8u;
  std::vector<uint32_t> selectedFaces() const;

  // Persistence ----------------------------------------------------------------
  bool saveProject(const std::string& path, std::string* err = nullptr);
  // A save in two halves: prepareSave reads the textures back (GL thread, quick); the job's
  // write() compresses them in parallel and writes the file, on any thread (autosave runs it in
  // the background). Neither changes projectPath or dirty.
  struct SaveJob {
    std::string path;
    nlohmann::json header;
    struct Item {
      std::string name;
      std::vector<uint8_t> bytes;  // a ready block (bits 0)
      int w = 0, h = 0;
      int srcChannels = 0, srcType = 0;  // the samples read: channels, 0 = 8 bit, 1 = half float, 2 = float
      int channels = 0, bits = 0;        // the PNG; bits 0 = a ready block
      bool signedFloat = false;          // -1..1 values (normals), stored as 0..1
      unsigned pbo = 0;                  // the samples, read back into this buffer (rows bottom first)
      const uint8_t* mapped = nullptr;   // ... mapped once the read-back has finished
      size_t size = 0;
    };
    std::vector<Item> items;
    void* fence = nullptr;  // the GPU has copied the textures into the buffers when it signals
    ~SaveJob();             // GL thread: frees the buffers
    bool ready(bool wait);  // GL thread: the read-back has finished (wait: block until it has)
    bool write(std::string* err);  // any thread, once ready
  };
  std::unique_ptr<SaveJob> prepareSave(const std::string& path, std::string* err = nullptr);
  // (1.2) This document's blocks into a save (their names after `prefix`: "" for the scene's first object, "O<k>/" for
  // the others) and its part of the header, returned. prepareSave is this with a fresh job.
  nlohmann::json appendToSave(SaveJob& job, const std::string& prefix);
  bool bentLoaded_ = false;  // loadProject: the bent normal map came back
  // The project's first object (a project from before 1.2 has only that one); Engine::openFile brings in the others.
  bool loadProject(const std::string& path, MaterialLibrary& lib, std::string* err = nullptr);
  // (1.2) One object of a project read already: its header part and its blocks' prefix (see appendToSave)
  bool loadFrom(const container::File& f, const nlohmann::json& header, const std::string& prefix,
                const std::string& path, MaterialLibrary& lib, std::string* err);
  nlohmann::json objectJson() const;            // (1.2) the name, shown or hidden, and the placement
  void objectFromJson(const nlohmann::json& j);
  // The model and its textures as a glTF (.glb with everything inside, or .gltf with .bin and PNG files) or an
  // OBJ (.mtl and PNG files). A material per texture set / UDIM tile. Returns what was written.
  bool exportModel(const ModelExportOptions& opt, nlohmann::json* out, std::string* err);
  // (0.47) the files exportTextures would write, and their names and maps
  std::vector<ExportPlanFile> exportPlan(const ExportOptions& opt);
  std::filesystem::path exportFileName(const ExportOptions& opt, const std::string& map, int tile);
  std::vector<ExportMap> exportMaps(const ExportOptions& opt);
  bool exportTextures(const ExportOptions& opt, std::map<std::string, std::string>* outFiles,
                      std::string* err = nullptr);
  // exportTextures with opt.psd (called by it)
  bool exportPsd(const ExportOptions& opt, const std::function<std::filesystem::path(const std::string&, int)>& fileFor,
                 std::map<std::string, std::string>* outFiles, std::string* err);
  // exportTextures for sizes above the texture set (called by it)
  bool exportTiled(const ExportOptions& opt, const std::vector<ExportMap>& outs,
                   const std::function<std::filesystem::path(const std::string&)>& fileFor,
                   std::map<std::string, std::string>* outFiles, std::string* err, int atlasTile = -1);

  uint32_t nextUid = 1;
  // Notes pinned to the model (saved in the project, not part of undo)
  std::vector<Note> notes;
  uint32_t nextNoteId = 1;
  Note* findNote(uint32_t id) {
    for (auto& n : notes)
      if (n.id == id) return &n;
    return nullptr;
  }
  json layerStackJson() const;

  // ---- 0.4 ---------------------------------------------------------------------------
  // High-poly bake: the sculpt stays in memory so a resolution change can re-bake it.
  std::unique_ptr<Mesh> highPoly;
  std::string highPolyPath;
  HighPolyBakeOptions highPolyOptions;
  bool highPolyBaked = false;
  bool bakeHighPoly(const std::string& path, const HighPolyBakeOptions& opt, json* stats, std::string* err);
  void clearHighPoly();

  // ---- (1.1) bake-down: the layers of a painted high-poly model moved onto its low-poly version
  struct BakeDownOptions {
    int resolution = 0;     // per texture set or tile; 0: the high-poly's
    int samples = 9;        // points per texel: 1, 4, 9, 16 (at most 4 above a 4K atlas)
    float distance = -1.f;  // how far along the low's normals; <= 0: measured from the two models
    bool bakeHigh = true;   // the high's shape into the low's normal map, ambient occlusion and material slots
    std::string lowPath;    // the low-poly's file (the model's file afterward)
    std::string highPath;   // the high-poly's file, kept for the bakes made again later (a resolution change, new UVs)
  };
  // The document becomes the low-poly model's project: every layer comes along (paint, masks and a baked map's
  // fixes resampled texel by texel, normals turned to the low's surface; fills, adjustments, folders and smart masks
  // as they are, over the low's new bakes), the high stays as the high-poly model, and the notes move onto the low's
  // surface. A new project (no file yet, not linked to Blender); the undo history starts afresh. Nothing changes
  // when it fails (the two models not in the same place, cancelled).
  bool bakeDown(Mesh&& low, const BakeDownOptions& opt, json* report, std::string* err);
  // What the last bake-down left to look at, while its UVs are still the model's: the check map (RGBA8: r the share
  // of a texel's points found by the nearest point, g guessed) and the high-poly as it looked before (its mesh and
  // final maps), to compare. The next model or project drops them.
  struct BakeDownBefore {
    GpuMesh gpu;
    Texture2D color, rmh, normal, emissive, extra;
    int extraUsed = 0;
  };
  Texture2D bakeDownCheck;
  std::unique_ptr<BakeDownBefore> bakeDownBefore;
  json bakeDownReport;
  uint64_t bakeDownUV = 0;
  bool bakeDownCurrent() const { return bakeDownCheck.valid() && bakeDownUV == uv::uvHash(mesh); }
  void clearBakeDown() { bakeDownCheck.destroy(), bakeDownBefore.reset(), bakeDownReport = json(), bakeDownUV = 0; }
  // The UVs and texture tiles of a model opened afresh (freshTiles) or not: islands outside 0..1 moved in, a smart
  // projection when it has none (uvsGenerated). True when its UVs changed.
  bool prepareUVs(Mesh& m, bool freshTiles);
  // Export settings of the last successful export (saved in the project): "export again".
  ExportOptions lastExport;
  bool hasLastExport = false;
  // Modification time of meshPath when it was loaded (file watching), 0 = unknown.
  int64_t meshFileTime = 0;
  static int64_t fileTime(const std::string& path);
  // name_v002.patina next to the current project (the first free number).
  std::string incrementalPath() const;
  // Layer mask from the selected faces (white = selected), undoable. Replaces an existing mask.
  bool maskFromSelection(int index);
  // The same from a list of faces (white on them, black elsewhere).
  bool maskFromFaces(int index, const std::vector<uint32_t>& faces, const std::string& label);

  // ---- folders, solo, undoable stack edits -----------------------------------------------
  // Every change to the layer stack's structure or properties is recorded by checkpoint():
  // it compares the stack with the last recorded state and pushes one undo entry for the
  // difference (so a slider drag becomes a single step). Removed layers are parked in
  // `graveyard` until no undo entry can bring them back.
  std::map<uint32_t, std::unique_ptr<Layer>> graveyard;
  uint32_t soloUid = 0;          // show only this layer / folder (0 = off)
  std::string pendingLabel;      // label for the next checkpoint ("" = "Layer settings")
  MaterialLibrary* library = nullptr;  // set by the engine; lets export ignore solo
  Layer& addFolder(const std::string& name = "", int index = -1);
  std::vector<int> childrenOf(uint32_t parentUid) const;  // indices, bottom to top
  std::vector<int> subtree(int index) const;              // index and all descendants
  int depthOf(int index) const;
  bool isDescendant(uint32_t uid, uint32_t ancestorUid) const;
  uint32_t effectiveParent(const Layer& l) const;  // parent uid if that folder exists, else 0
  bool moveSibling(int index, int dir);            // +1 up / -1 down within the same folder
  // drag & drop: above target (below = under it), or into a folder (top of its contents);
  // target -1 = bottom of the stack
  bool moveOnto(int from, int target, bool into, bool below = false);
  // (0.22.5) drag and drop onto the stack. Where something dropped goes, as moveOnto places a moved
  // layer: the index to insert at and the folder (target -1: the bottom of the stack)
  int dropIndex(int target, bool into, bool below, uint32_t* parent);
  // a new fill layer where a material lands (one undo step)
  Layer& addFillLayerAt(const MaterialRef& mat, const std::string& name, int target, bool into, bool below = false);
  // a material dropped on a fill layer: the layer (an instance's source) takes it, its mask and smart masks
  // staying; named after it if it was named after the old one
  bool replaceFillMaterial(int index, const MaterialRef& mat, const std::string& oldName, const std::string& newName);
  // a layer Ctrl+dragged: a copy (with its contents) where it lands; returns its index (-1: none)
  int copyLayerTo(int from, int target, bool into, bool below = false);
  // a mask dragged onto a layer: its painted mask, smart mask and mask effects copied over (one undo step)
  bool copyMaskTo(int from, int to);
  // a smart mask dropped on a layer: its smart mask (with a mask effect stack, a smart mask effect on top)
  bool addSmartMask(int index, int generatorType);
  bool setParent(int index, uint32_t folderUid);
  int groupIntoFolder(int index);                  // new folder in its place, containing it
  bool clearPaint(int index, int channelMask = kAllChannels);  // removes painted channels, keeps the mask (undoable)
  void addMask(int index, float value);            // undoable
  void removeMask(int index);                      // undoable
  StackState captureState() const;
  void applyState(const StackState& s);
  bool checkpoint(const std::string& label = "");  // true if an undo entry was pushed
  // Undo groups (0.11): everything between begin and end is one undo step called `label`;
  // cancel rolls the group's edits back. Pending layer edits are recorded first.
  void beginUndoGroup(const std::string& label);
  bool endUndoGroup();
  void cancelUndoGroup();
  void clearHistory();
  bool undoStep();
  bool redoStep();
  void jumpTo(size_t position);  // history panel

 private:
  void reprojectAll(const std::vector<vec2>& newUVs);
  // Moves all paint and baked maps to `target` (same UVs, another tile layout / atlas size).
  void relayout(const TileLayout& target);
  // Resamples the layers to an atlas of r x r and reallocates the texture set (no re-bake).
  void resizeAtlas(int r);
  // `source` (optional): an unchanged copy of each texture to read from (channel -1 = mask;
  // for mesh maps the UVEditEntry map index), which saves a copy per texture
  using ReprojectSource = std::function<const Texture2D*(uint32_t uid, int channel)>;
  void reprojectLayers(const std::vector<vec2>& newUVs, const ReprojectSource& source = nullptr);  // paint layers and masks
  void reprojectMeshMaps(const std::vector<vec2>& newUVs, const ReprojectSource& source = nullptr);  // AO, curvature, high-poly maps
  int insertAt(std::unique_ptr<Layer> l, int index);
  void pruneGraveyard();
  StackState committed_;
  bool committedValid_ = false;
  uint64_t syncedBakeSerial_ = 0;  // (0.41) TextureSet::bakeSerial() at the last syncBakedLayers()
 private:
  SymmetryReport symmetry_;
  uint64_t symmetryKey_ = 0;
  MeshCheckReport meshCheck_;
  CollisionResult collision_;
  uint64_t collisionKey_ = 0;
  uint64_t meshCheckKey_ = 0;
  std::string meshFixBackup_;  // the project as it was before the last mesh fix or reduction
  std::string meshChangeLabel_;
  bool keepCopyBeforeMeshChange(std::string* err);
  // a mesh whose corners keep their UVs (a reduction): the layers' textures stay as they are
  void replaceMeshKeepTextures(Mesh&& m);
  uv::MirrorLinks mirrorLinks_;
  uint64_t mirrorLinksKey_ = 0;
};

}  // namespace patina
