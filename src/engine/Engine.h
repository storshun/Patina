#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>

#include "core/MeshParts.h"
#include "core/MeshSelect.h"
#include "core/Retopo.h"
#include "core/RetopoField.h"
#include "core/RetopoLoops.h"
#include "core/RetopoQuads.h"
#include "core/SurfaceReach.h"
#include "core/UVOps.h"
#include "engine/AssistantModel.h"
#include "engine/BrushLibrary.h"
#include "engine/Camera.h"
#include "engine/Features.h"
#include "engine/Document.h"
#include "engine/Environment.h"
#include "engine/MaterialLibrary.h"
#include "engine/Painter.h"
#include "engine/Renderer.h"

namespace patina {

using json = nlohmann::json;
class Engine;

struct CommandError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

struct CommandContext {
  Engine& engine;
  std::shared_ptr<std::promise<json>> promise;  // fulfill manually when deferred
  bool deferred = false;
};

// A command of the automation API (TCP server, MCP tools, Blender bridge, UI console).
struct Command {
  std::string name;
  std::string description;
  json schema;  // JSON Schema for the arguments (MCP inputSchema)
  std::function<json(CommandContext&, const json& args)> run;
  bool exposeToMcp = true;
  bool noCheckpoint = false;  // don't record pending layer edits around this command
};

// File > Render image (F12)
struct RenderImageOptions {
  std::string path;            // PNG file; turntable frames get _0001.png ... suffixes
  int width = 1920, height = 1080;
  bool transparent = false;    // alpha 0 behind the model
  int supersample = 2;         // render larger and filter down (anti-aliasing)
  int turntableFrames = 0;     // > 1: frames of a full turn around the model
  int pathSamples = 0;         // (0.39) > 0: path traced with this many samples per pixel (no supersampling)
};

class CommandRegistry {
 public:
  void add(Command c) { cmds_[c.name] = std::move(c); }
  const Command* find(const std::string& n) const {
    auto it = cmds_.find(n);
    return it == cmds_.end() ? nullptr : &it->second;
  }
  const std::map<std::string, Command>& all() const { return cmds_; }
  json describe(bool mcpOnly = true) const;

 private:
  std::map<std::string, Command> cmds_;
};

// Ties everything together. Owned by the GUI app or the headless runner; all
// GL work happens on the thread that calls tick()/processQueue().
class Engine {
 public:
  bool init(bool headless);
  void shutdown();

  MaterialLibrary materials;
  BrushLibrary brushes;
  Environment env;
  Document doc;
  Painter painter;
  Renderer renderer;
  Camera camera;
  ViewportOptions viewOptions;
  UVViewOptions uvOptions;
  CommandRegistry commands;
  int defaultResolution = 2048;
  vec2 viewportSize{1280, 800};  // size of the 3D view used by screen-space strokes
  bool headless = false;
  std::string selectedMaterialId;
  std::string selectedBrushId = "basic_hard";
  bool autoPushToBlender = false;
  double lastPushTime = 0;
  uint64_t lastPushedComposite = 0;
  Features features;  // switches for newer features (Preferences > Features)
  // 0.10: macro recorder: the commands run while recording (GUI menus, scripts, Claude), to
  // replay or save as a job for `patina run`. Brush strokes in the viewport are not commands.
  struct Macro {
    bool recording = false;
    bool playing = false;
    struct Step {
      json step;            // {"cmd", "args"}
      uint64_t serial = 0;  // the undo entry the command made (0 = none): undone steps drop out
    };
    std::vector<Step> recorded;
    static bool recordable(const std::string& cmd, const json& args);
    void start(bool append) {
      if (!append) recorded.clear();
      recording = true;
    }
    void clear() { recorded.clear(); }
    // the recorded steps still in effect (what you undid is left out; redo brings it back)
    std::vector<json> steps(const UndoStack& u) const {
      std::vector<json> out;
      for (auto& s : recorded)
        if (!s.serial || u.isApplied(s.serial)) out.push_back(s.step);
      return out;
    }
    bool erase(const UndoStack& u, size_t index) {  // index into steps()
      for (size_t i = 0, k = 0; i < recorded.size(); ++i) {
        if (recorded[i].serial && !u.isApplied(recorded[i].serial)) continue;
        if (k++ == index) {
          recorded.erase(recorded.begin() + i);
          return true;
        }
      }
      return false;
    }
  } macro;
  std::vector<json> macroSteps() const { return macro.steps(doc.undo); }
  double autosaveMinutes = 0;  // 0 = off (the GUI enables it)
  double lastAutosave = 0;
  std::future<bool> autosaveTask;  // the file is compressed and written in the background
  std::unique_ptr<Document::SaveJob> autosaveJob;  // read back on the GPU, then written by autosaveTask
  int serverPort = 0;

  // Split / compare view: the left side of the 3D view shows a snapshot of the textures
  // (taken on request) or another view mode; `split` is where the divider is (0..1).
  struct CompareView {
    bool enabled = false;
    int mode = 0;                        // 0 snapshot, 1 view mode, 2 (1.1) the high-poly before the bake-down
    ViewMode view = ViewMode::BaseColor;  // mode 1
    float split = 0.5f;
    bool hasSnapshot = false;
    double takenAt = 0;                   // nowSeconds()
    std::string label;                    // e.g. the time it was taken
    Texture2D color, rmh, normal, emissive, extra;
    int extraUsed = 0;
  } compare;
  void takeCompareSnapshot();
  // 0.30: the batch running in a hidden `patina run` (one at a time)
  // (0.31) what opening the model brought in of its own materials (null: nothing)
  json modelTextures;
  // (0.34) collision shapes shown over the model while true (and while they fit the mesh)
  bool showCollision = false;
  std::vector<vec3> collisionLines;
  uint64_t collisionLinesKey = 0;
  void collisionViewOptions(ViewportOptions& o);
  struct BatchRun {
    std::string id, dir, job, progress, report, recipe, output;
    size_t files = 0;
    double started = 0;
    std::shared_ptr<class ChildProcess> proc;
  };
  std::unique_ptr<BatchRun> batch;
  bool compareReady() const {
    return compare.enabled && (compare.mode == 1 || (compare.mode == 2 ? doc.bakeDownBefore != nullptr : compare.hasSnapshot));
  }
  // renders the compare side (the full view; the caller shows the part left of the divider)
  void renderCompare(const Camera& cam, RenderTarget& rt, int w, int h, const ViewportOptions& opt, GLuint maskTex);
  json compareState() const;
  // what the Layer Mask view shows: the selected layer's effective mask (painted x smart mask, or its stack)
  GLuint maskViewTexture(bool effective = true);

  // Frame update: drain commands, advance painter, composite.
  void tick();
  bool uiInteracting = false;  // set by the GUI while a widget or mouse button is held
  // (0.22.2) set by the GUI while a retopology line, flow stroke or density stroke is drawn: the rest waits
  // (no live preview starts, the 3D view and the UV editor reuse their last pictures where nothing changed)
  bool drawingStroke = false;
  // Thread-safe: enqueue a request {"cmd":..., "args":...}; result on the GL thread.
  std::future<json> post(const json& request);
  // Execute immediately (GL thread only).
  json execute(const std::string& cmd, const json& args);
  size_t processQueue();
  bool waitForWork(int timeoutMs);  // headless main loop helper

  // High-level helpers used by UI and commands
  bool openFile(const std::string& path, std::string* err = nullptr);  // mesh or .patina
  // (1.2) separate: a file with several meshes opens as an object each (1), as one (0), or (-1) an object each when
  // there are at most kMaxObjectsOnOpen (more, such as a kit of many small parts, open as one)
  bool openMesh(const std::string& path, int resolution, bool keepLayers, std::string* err = nullptr, int separate = -1);
  static constexpr size_t kMaxObjectsOnOpen = 24;
  // (1.1) Bakes the layers down from the model open (the painted high-poly) onto `low` (Document::bakeDown), which
  // becomes the model; lowPath is its file. The high-poly's file (o.highPath) when it has none (or it came from
  // Blender, whose files are only for the exchange): a copy in the user folder, written once the bake-down is done.
  bool bakeDown(Mesh&& low, const std::string& lowPath, Document::BakeDownOptions o, json* report,
                std::string* err = nullptr);
  void frameMesh();
  // (1.2.11) the selected faces fill the 3D view (as Blender's View Selected); none selected: frameMesh. False then.
  bool frameSelection();
  // (0.38) Side, top and bottom views (the numpad, the axis gizmo, Alt while orbiting, set_camera's view presets)
  // remember the view before them; viewBack() goes back to it, and a second time to the side view again.
  // `ortho`: the side view is orthographic until the camera turns away from it (Blender's Auto Perspective).
  struct ViewMemory {
    bool snapped = false;      // the camera is on the side view it was snapped to
    float yaw = 0, pitch = 0;  // (that view's angles: turning away leaves it)
    bool orthoBySnap = false;  // the snap made it orthographic: leaving it goes back to perspective
    bool hasBack = false;
    Camera back;               // the view viewBack() goes to
    bool backSnapped = false, backOrthoBySnap = false;  // (it is a side view itself)
  } viewMemory;
  // `before`: the view to remember instead of the camera now (the start of a drag or a turn)
  void snapView(float yaw, float pitch, bool ortho, const Camera* before = nullptr);
  bool viewBack();
  void checkViewSnap();  // turned away from a side view: it is left (and perspective again)
  bool renderImage(const RenderImageOptions& opt, std::vector<std::string>* written, std::string* err = nullptr);
  // (0.39) a path traced render image in steps, so the window keeps going (the app: a frame's time each step, a
  // progress bar); renderImage() makes it in one go (MCP). renderJobStep: true once it is done (or failed: error).
  struct RenderJob {
    bool active = false;
    RenderImageOptions opt;
    int frame = 0, frames = 1, w = 0, h = 0;
    bool frameStarted = false;
    Camera cam;
    std::vector<std::string> written;
    std::string error;
    float progress = 0.f;
  } renderJob;
  bool startRenderJob(const RenderImageOptions& opt, std::string* err);
  bool renderJobStep(double budgetMs);
  void cancelRenderJob();
  void selectBrush(const std::string& id);
  void selectMaterial(const std::string& id);
  bool pickAt(vec2 viewportPx, bool applyToBrush = true, json* out = nullptr);
  RayHit pick(vec2 viewportPx) const;
  // (0.43) the Island brush: the faces a dab of `radiusPx` at a viewport point takes, with their mirror images
  // under the painter's symmetry. Screen: the faces seen inside the circle; Surface: the faces within the brush's
  // radius along the surface from the one under the cursor. Either way, faces turned away from the camera are
  // left out unless they are the first thing seen (the far side, the inside of a cloth with thickness). `skip`
  // (without symmetry) leaves out faces already as wanted.
  enum class BrushReach { Screen = 0, Surface = 1 };
  std::vector<uint32_t> facesUnderBrush(vec2 viewportPx, float radiusPx, BrushReach reach,
                                        const std::function<bool(uint32_t)>& skip = {});
  // (0.45) The faces of `faces` the camera sees: turned toward it, on screen, and not hidden behind other faces (any of
  // a few points of the face is the first thing a ray from the camera meets). The Select tool's Visible only.
  std::vector<uint32_t> visibleFaces(const std::vector<uint32_t>& faces) const;
  // (0.36) the layers that paint the spot at `uv` (a texture coordinate as RayHit::uv gives it): paint there, or a
  // fill / adjustment, through a mask that lets it in; top of the stack first, folders left out
  struct LayerHere {
    int index;
    float amount;  // coverage x mask x opacity, 0..1
    bool shown;    // it and its folders are visible
  };
  std::vector<LayerHere> layersAt(vec2 uv);
  std::string requestedTool;   // (0.36) set_tool: the tool the window should use
  std::string windowTool;      // (0.38) the tool the window has (get_state's window_tool; set_tool's names)
  uint64_t toolRequest = 0;    // ... counted, so a request for the tool already in use still counts
  std::vector<uint8_t> renderViewportPng(int w, int h, const Camera* cam = nullptr, const ViewportOptions* opt = nullptr);
  std::vector<uint8_t> renderUVPng(int w, int h, GLuint tex, int texMode);
  json state();
  // Blender bridge
  // (1.2) `which`: an object of the scene (default: the active one)
  bool pushToBlender(std::string* err = nullptr, json* result = nullptr, Document* which = nullptr);
  json exportForBlender(std::string* err, Document* which = nullptr);
  // 0.28: the model as it is in Patina (mended, its UVs, slots) to Blender as an object - a new one next to the linked
  // object, or (replace) the linked object's own mesh - with its textures; Patina links to it from then on.
  json sendModelToBlender(const std::string& name, bool replace, int port, std::string* err);

  // GUI hook: capture the real window on the next frame (MCP "screenshot_window").
  std::function<void(std::shared_ptr<std::promise<json>>, json args)> requestWindowCapture;
  // GUI hook (0.45, tests): answer after `frames` more frames are on screen (unset when headless)
  std::function<void(std::shared_ptr<std::promise<json>>, int frames)> requestFrameWait;
  // For tests: where some widgets were last drawn (window coordinates: x, y, width, height), by name
  std::map<std::string, std::array<float, 4>> uiRects;
  // For tests (PATINA_UI_ID_CHECK): widgets drawn with the same ID as another in their window ("window: label")
  std::vector<std::string> uiIdConflicts;
  // GUI hook: open the Assistant window (0.11), asking `request` when it isn't empty
  std::function<void(const std::string& request, bool preview)> openAssistantPanel;
  // 0.12: opens one of the GUI's windows (export, render, texel density...); unset when headless
  std::function<void(const std::string& window, const nlohmann::json& args)> openWindow;
  std::string lastOpenedWindow;  // what open_window was last asked for (get_state, tests)
  // GUI hook (0.46): the `layout` command (the Guided or Full layout, the Guided step; the steps' states); unset when
  // headless
  std::function<nlohmann::json(const nlohmann::json& args)> layoutHook;
  // GUI hook: notify that a document/material changed outside the UI
  std::function<void(const std::string& what)> onExternalChange;
  // GUI hook (0.44): the window's recent files, where Blender's objects look for the project they were painted in
  std::function<std::vector<std::string>()> recentFiles;

  // ---- (1.2) the scene: several objects (a body, its clothes, eyes, hair), each a Document of its own with its layers,
  // textures and history. `doc` is the active one, the one painted, listed in the Layers panel and the UV editor and
  // changed by the commands; the others wait in objects_ (scene order; the active one's place there is empty).
  int objectCount() const { return (int)objects_.size(); }
  int activeObject() const { return active_; }
  Document& object(int i) { return i == active_ ? doc : *objects_.at(i); }
  const Document& object(int i) const { return i == active_ ? doc : *objects_.at(i); }
  // an object by its name (as listed) or its index ("2"); -1 when there is none
  int findObject(const std::string& nameOrIndex) const;
  // The object painted and listed from now on (strokes end, pending layer edits become a step first)
  bool activateObject(int i);
  // A model as a new object at the end of the scene (its own texture set at `resolution`, its own layers: none, or its
  // file's materials as a layer), placed by `placement`; returns its index. Not made active.
  int addObject(Mesh&& m, const std::string& path, const std::string& name, const Document::Placement& placement,
                int resolution, bool modelTextures = true);
  // The model files' meshes as objects (a file with several meshes: an object each, unless `oneObject`); returns
  // the index of the first one added (-1 with `err`)
  int addObjectsFromFile(const std::string& path, int resolution, bool oneObject, std::string* err,
                         json* report = nullptr);
  bool removeObject(int i);
  int duplicateObject(int i, std::string* err = nullptr);
  void clearScene();  // only the active object stays (opening a model or a project starts a new scene)
  void newScene();    // the same, and the object left is a plain one (no name, where its model's numbers put it)
  void compositeObjects();    // the other objects' layer stacks composited where they changed (tick)
  void markSceneComposite();  // every object composited again (a library material they may use changed)
  bool sceneDirty() const;  // unsaved changes in any object, or to the scene itself
  bool sceneChanged = false;  // objects added, removed or renamed since the scene was saved
  // the visible objects' world bounds (just the active object's when it is alone)
  AABB sceneBounds(bool visibleOnly = true) const;
  // The camera in the active object's space (Camera::inSpace): painting, picking and its overlays work in it
  Camera objectCamera() const { return camera.inSpace(doc.toWorld()); }
  Camera objectCamera(const Camera& world) const { return world.inSpace(doc.toWorld()); }
  vec3 toWorld(const vec3& p) const { return vec3(doc.toWorld() * vec4(p, 1.f)); }
  // the 3D view's near and far planes round the scene (the active object's bounds when it is alone)
  AABB clipBounds() const { return objectCount() == 1 && !doc.placed() ? doc.mesh.bounds : sceneBounds(); }
  void updateClip() { camera.updateClip(clipBounds()); }
  // What a ray through a viewport pixel meets first among the visible objects: the object (-1: none), the hit in its
  // own space and how far along the world ray it is
  struct ScenePick {
    int object = -1;
    RayHit hit;
    float t = 0.f;
  };
  ScenePick pickScene(vec2 viewportPx, const Camera* cam = nullptr) const;
  // The other visible objects as seen in the active object's space (they hide what is behind them from the brush)
  std::vector<std::pair<const GpuMesh*, mat4>> occluders() const;
  Painter::View paintView(const Camera& cam, vec2 viewport) const;
  json objectsJson() const;
  // (1.2) the active object's faces on each edge (loops, rings, linked faces), made again when the mesh changes
  const select::EdgeFaces& edgeFaces();
  // Saves every object into one project file (Document::appendToSave; the first object's part is the header itself,
  // so earlier versions open it); `job`: autosave's background save
  bool saveScene(const std::string& path, std::string* err = nullptr);
  std::unique_ptr<Document::SaveJob> prepareSceneSave(const std::string& path, std::string* err);
  bool isolateActive = false;  // (Local view) only the active object is shown
  // The model file of the scene (SceneExport.cpp): every object shown (allObjects), placed as it is in the scene, or
  // the active one alone
  bool exportSceneModel(const ModelExportOptions& opt, bool allObjects, json* out, std::string* err);
  // The textures of every object shown (allObjects: each object's files named after it), or of the active one
  bool exportSceneTextures(const ExportOptions& opt, bool allObjects, std::map<std::string, std::string>* files,
                           json* perObject, std::string* err);
  void updateCompositeOf(int i);  // an object's composite brought up to date (any object)

  // Stroke helper for automation: paints a polyline in viewport pixels or UV.
  json scriptedStroke(const std::vector<StrokePoint>& pts, bool uv);

  // ---- 0.11: named parts of the model (Patina Assistant) -----------------------------------
  // Slots, pieces, vertex groups and head / body regions (cached until the mesh changes), plus
  // "selection" when faces are selected.
  const parts::PartsReport& meshParts();
  // The faces of a part id (see meshParts); throws CommandError naming the parts there are.
  std::vector<uint32_t> partFaces(const std::string& id);
  // The last few Assistant requests that changed something, newest last: what was asked, what
  // the plan said, and the layers it made (by uid), so follow-ups ("a bit stronger") can find them.
  struct AssistantTurn {
    std::string request, say;
    std::vector<uint32_t> layers;
    uint64_t serial = 0;  // its undo entry: undone requests drop out
  };
  std::vector<AssistantTurn> assistantTurns;
  // The Assistant's model (created on first use; it has its own worker thread). The file is
  // assistantModelPath, or else PATINA_ASSISTANT_MODEL, or else userDataDir/models/patina-assistant.gguf.
  AssistantModel& assistantModel();
  std::string assistantModelFile() const;
  std::string assistantModelPath;

  // Retopology (0.13): patches and density painted on the open mesh, and the quads made from them.
  // It belongs to the mesh: a different mesh starts afresh.
  struct RetopoState {
    std::vector<int32_t> faceLabel;  // per face of doc.mesh: 0 none, > 0 its patch
    std::vector<float> density;      // per vertex: a multiplier on quadsPerM2
    float quadsPerM2 = 2500.f;
    int nextLabel = 1;
    std::unique_ptr<retopo::Result> result;
    uint64_t version = 0;            // bumped on every change (the viewport redraws its overlays)
    uint32_t faces = 0, verts = 0;   // the mesh they belong to
    retopo::FaceGraph graph;         // face neighbors, for the brushes (built when first needed)
    std::vector<retopo::Stroke> strokes;            // 0.14: where edge loops should go
    int method = 0;                                 // 0: quads along the flow (whole model), 1: each patch filled
    // 0.14 zones: lines drawn on the surface (where the flow changes; each is an edge loop) split the model
    // into zones, each with its own resolution (set at a point inside it). 0.14.1: free lines on the
    // surface (points where the pen was), no longer the mesh's edges.
    std::vector<retopo::Stroke> lines;              // the zone lines (edge = true)
    std::vector<std::pair<uint32_t, float>> zoneSeeds;  // (face, resolution): the zone holding the face
    std::vector<int32_t> zones;                     // per face: its zone from 1 (cached; zonesVersion)
    int zoneCount = 0;
    uint64_t linesVersion = 1;                      // bumped when the lines or the painted patches change
    uint64_t densityVersion = 1;                    // bumped when the density brush paints
    uint64_t zonesVersion = ~0ull;                  // the linesVersion the zones were found for
    std::vector<uint32_t> faceAdjStart, faceAdj;    // faces across each edge shared by two (built once a mesh)
    std::vector<vec3> zoneLineSegs, strokeSegs;     // for drawing: segment end points in pairs (cached)
    std::vector<vec3> doneLineSegs;                 // (0.22) the confirmed zone lines' (drawn darker)
    uint64_t segsVersion = ~0ull;
    std::vector<vec3> markSegs;                     // crosses where the quads left the surface out
    uint64_t markSegsVersion = ~0ull;
    // 0.16: the quads' poles, and the edge loop under the pointer (Loops in the window)
    std::shared_ptr<retopo::EdgeLoops> loops;
    std::shared_ptr<retopo::LoopFinder> loopFinder;
    uint64_t loopsVersion = ~0ull;
    int hoverLoop = -1, drawnLoop = -2;
    std::vector<vec3> loopPts;
    std::vector<vec4> loopCols;
    uint64_t loopSegsKey = 0;
    int selectedZone = 0;
    std::vector<std::vector<vec3>> flowLines;       // the lines the last flow layout traced
    uint64_t resultVersion = 0;      // the version the result was built from
    std::future<retopo::Result> building;  // a build running in the background (startRetopoBuild)
    double buildStarted = 0;
    // 0.15 live preview: while a retopology tool or the window is in use (wantPreviewAt, set each frame),
    // a quick build starts by itself a moment after the inputs last changed; the full one is Make quads
    bool livePreview = true;
    bool remakeAll = false;  // (0.23) the next build makes all the quads again (else only those near what changed)
    double wantPreviewAt = -1e9;
    uint64_t inputsVersion = 1;        // bumped by every change to what the quads are made from
    uint64_t buildInputs = 0, resultInputs = 0, previewTried = 0;
    bool buildIsPreview = false, resultIsPreview = false, fullPending = false;
    std::shared_ptr<std::atomic<bool>> cancel;
    std::shared_ptr<retopo::FlowCache> flowCache;  // the working surface, kept between builds
    // 0.15 symmetry (with the Sym buttons): across planes through the model's middle; points within the
    // deadzone of a plane count as on it (deadzone < 0: 1% of the model's size)
    vec3 symCenter{0};
    int symAxesSeen = 0;  // (the Sym buttons when last looked: the quads are mirrored with them)
    float symDeadzone = -1.f, modelSize = 1.f;
    // 0.16: a zone between two edge lines (round an eye, along an arm) is filled with rings that follow both
    bool ringZones = true;
    // 0.21: the quads' counts solved again from their own layout so close pairs of poles cancel (kept only
    // with fewer poles and no more folded faces)
    bool fewerPoles = true;
    bool keepAway = true;  // (0.23) only the quads near what changed made again, the rest kept as they were
    // 0.22.3: each flow stroke an edge loop as well (it runs exactly along it); else the strokes only turn the quads
    // (0.22.4: on by default)
    bool flowLoops = true;
    std::string meshPath;            // where the patches are kept between sessions (by this mesh)
    uint64_t savedVersion = 0;
    double changedAt = 0;
    // (1.2.9) the quads asked for over the model (zones and the density brush counted), for inputsVersion
    double quadsAsked = 0;
    uint64_t quadsAskedInputs = 0;
    bool previewTooBig = false;  // the live preview didn't start: too many quads for one (Make quads does it)
  } retopoData;
  RetopoState& retopo();  // the state for the open mesh
  // 0.16: the edge loop of the quads nearest this point of the model is shown (none: -1); returns its edges
  int retopoHoverLoop(const vec3* p);
  // Builds the quads in the background; tick() keeps the result. False when one is running already.
  bool startRetopoBuild(int relax = 30, bool preview = false);
  bool retopoRemakeAll();  // (0.23) all the quads made again (not only those near what changed)
  bool retopoBuilding() const { return retopoData.building.valid(); }
  void waitRetopoBuild();
  void retopoChanged();   // after the patches or density change: redraw, save a little later
  void retopoLinesChanged();  // the zone lines or the painted patches changed (zones are found again)
  void retopoSetLines(std::vector<retopo::Stroke> lines, const char* label);  // the zone lines, as one undo step
  // 0.22: which zone lines are confirmed (one flag per line), as one undo step; the zones and quads don't change
  void retopoSetConfirmed(std::vector<char> confirmed, const char* label);
  // 0.22: a new zone line's ends snap onto the lines near them (not onto confirmed ones); off: they stay
  // where they are drawn (tight folds: lips, fingers, the crotch)
  bool retopoSnapEnds = true;
  // a zone line right round the part at p (an arm, a leg, the neck), mirrored on the axes given, as one undo
  // step; how many lines were added (0: none, why says why)
  // 0.22: mode 0 round the part, or (no part to go round) the cut facing the camera; 1 round the part only;
  // 2 the cut through p facing the camera (dir: the way it faces; null: parallel to the screen).
  // used, if given: 1 or 2, whichever was made
  size_t retopoAddRing(const vec3& p, int mirrorAxes, std::string* why = nullptr, int mode = 0, const vec3* dir = nullptr, int* used = nullptr);
  std::vector<vec3> retopoRingLoop(const vec3& p, int mode, const vec3* dir, std::string* why, int* used);
  // A stroke and its mirror images across the planes in axes (bit 0 x, 1 y, 2 z), back on the surface
  std::vector<retopo::Stroke> retopoMirror(const retopo::Stroke& st, int axes);
  float retopoDeadzone();  // in model units
  int retopoSymmetryAxes() const;  // the Sym buttons (mirror symmetry) as axes bits
  // The retopology overlays (patches / density / the quads) for a render, from viewOptions' settings.
  void retopoViewOptions(ViewportOptions& o);
  // Patches and density are kept between sessions in the user folder, by mesh file and face count.
  void saveRetopo();
  bool loadRetopo();
  void retopoTick();      // from tick(): a finished build, a pending save
  // per vertex: the density brush times the resolution of the zones around it
  std::vector<float> retopoDensity();                 // the density brush times the zones' resolutions, per vertex
  double retopoQuadsAsked();                          // (1.2.9) about how many quads the settings ask for
  static constexpr double kRetopoPreviewMaxQuads = 150000;  // (more: no live preview, only Make quads)
  std::vector<float> retopoFaceResolution();          // per face, its zone's resolution (empty: all 1)
  // 0.17: zones for the parts too thin for the quads (fingers, strands): lines round them and resolutions
  // for about quadsAround quads round each; one undo step. Returns what was found (and added).
  nlohmann::json retopoThinZones(bool apply, int quadsAround = 6, float maxResolution = 16.f);
  const std::vector<int32_t>& retopoZones();          // per face (see RetopoState::zones)
  float retopoZoneResolution(int zone);
  std::vector<retopo::Stroke> retopoLines(bool forBuild = true);  // the strokes, and the zone lines as edge loops

 private:
  void registerCommands();
  void registerAssistantCommands();
  void registerRetopoCommands();
  void registerBatchCommands();  // 0.30: recipes and batches
  void registerInterfaceCommands();  // 0.36: layers_at, languages
  void registerSceneCommands();  // (1.2) objects
  void registerSelectCommands(); // (1.2) loops, rings, linked faces, face sets
  select::EdgeFaces edgeFaces_;
  uint64_t edgeFacesKey_ = 0;
  void attachDocument(Document& d);  // the engine's hooks on the active document
  void objectChanged();              // the active object is another one: caches of the old one go
  std::vector<std::unique_ptr<Document>> objects_ = std::vector<std::unique_ptr<Document>>(1);
  int active_ = 0;
  std::function<void()> beforeMeshChange_;
  std::unique_ptr<AssistantModel> assistantModel_;
  parts::PartsReport parts_;
  FaceGraph faceGraph_;  // (0.43) the Island brush's Surface reach
  uint64_t partsKey_ = 0;
  struct Pending {
    json request;
    std::shared_ptr<std::promise<json>> promise;
  };
  std::mutex qMutex_;
  std::condition_variable qCv_;
  std::deque<Pending> queue_;
  RenderTarget offscreen_, uvOffscreen_, renderImageRT_, compareOffscreen_;
};

}  // namespace patina
