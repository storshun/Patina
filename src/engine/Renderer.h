#pragma once
#include <functional>
#include <memory>
#include <vector>

#include "engine/Camera.h"
#include "engine/PathTracer.h"
#include "gfx/Texture.h"

namespace patina {

class Document;
class Environment;
class GpuMesh;
class Shader;
struct Mesh;

enum class ViewMode {
  Material = 0, BaseColor, Roughness, Metallic, Normal, Height, Emissive, AO, Curvature, UVChecker, LayerMask,
  WorldNormal, UVIslands, TexelDensity, UVStretch, MaterialIds, Thickness, BentNormal, Symmetry,
  Opacity, Sheen, Coat, Subsurface,  // 0.26
  MeshCheck,                         // 0.27
  PbrRange,                          // 0.33
  Solid,                             // 0.38: gray, lit, no textures
  PathTraced,                        // 0.39: the path tracer (engine/PathTracer)
  BakeCheck,                         // 1.1: where the last bake-down found the high-poly surface
  FaceSets,                          // 1.2: each face set in its color
  Count
};
const char* viewModeName(ViewMode m);
ViewMode viewModeFromName(const std::string& s);

struct ViewportOptions {
  ViewMode mode = ViewMode::Material;
  bool wireframe = false;
  bool seams = true;
  bool islandTint = false;
  float selectionTint = 0.35f;
  bool showPins = false;  // (0.37.1) pinned UV islands striped blue (the Select and Island tools)
  // (0.39) the Path traced mode: its settings, and (render image, MCP) how many samples to make now (0: the view's
  // progressive ones)
  PathTraceSettings pathTrace;
  int pathSamplesNow = 0;  // (< 0: the render image's tracer, sampled already by Engine::renderJobStep)
  bool envBackground = true;
  float backgroundBlur = 3.f;
  // (1.1) drawn instead of the model, without its seams (the bake-down's compare: the high-poly as it was)
  const GpuMesh* meshOverride = nullptr;
  uint32_t hoverFace = 0xFFFFFFFFu;
  bool useAO = true;
  bool transparentBackground = false;  // render image: alpha 0 behind the model
  // view-mode data per face (texel density / stretch heat maps): set by the caller
  float densityTarget = 0.f;           // UV units per object unit the density map compares against
  // 0.10: the other texture sets are drawn as dim ghosts (isolateTile: the shown set, -1 = none)
  bool isolateSet = false;
  int isolateTile = -1;
  // 0.13 retopology: the patches (1) or the painted density (2) over the model, and the quads
  int retopoView = 0;
  const std::vector<int32_t>* retopoLabels = nullptr;  // per face of the model
  const std::vector<float>* retopoDensity = nullptr;   // per vertex
  uint64_t retopoKey = 0;                              // changes whenever they do
  const Mesh* retopoQuads = nullptr;                   // drawn as lines over the model
  uint64_t retopoQuadsKey = 0;
  bool retopoQuadsOnly = false;                        // the quads shaded instead of the model
  bool retopoQuadsPreview = false;                     // they are a live preview: drawn fainter
  bool retopoShowQuads = true;                         // (Engine::retopoViewOptions fills the pointers)
  const std::vector<vec3>* retopoLines = nullptr;      // zone lines: segment end points, in pairs
  const std::vector<vec3>* retopoLinesDone = nullptr;  // (0.22) confirmed zone lines: the same, drawn darker
  const std::vector<vec3>* retopoStrokes = nullptr;    // flow strokes: the same
  const std::vector<vec3>* retopoMarks = nullptr;      // where the quads left the surface out: the same
  bool retopoLoops = false;                            // 0.16: the quads' edge loops in color, and the poles
  const std::vector<vec3>* retopoLoopPts = nullptr;    // (segments in pairs)
  const std::vector<vec4>* retopoLoopCols = nullptr;
  uint64_t retopoLoopsKey = 0;
  uint64_t retopoLinesKey = 0;
  int retopoLinesBehind = 1;                           // (0.22) lines behind the model: 0 shown, 1 darkened, 2 hidden
  // (0.37) the top bar's Retopology button: the zones or density and the quads shown in the app's 3D view without
  // a retopology tool (the tools show what they set; renders for MCP show what retopo_view sets either way)
  bool retopoPreview = false;
  bool axisGizmo = true;                               // (0.37) the X/Y/Z gizmo in the 3D view's corner (the app only)
  int retopoSelectedZone = 0;                          // drawn brighter (0: none)
  // 0.34 collision shapes: their edges (segments in pairs), drawn over the model in green
  const std::vector<vec3>* collisionLines = nullptr;
  uint64_t collisionKey = 0;
};

struct UVViewOptions {
  vec2 scale{1.f};   // screenUV = uv * scale + offset (both in [0,1] view units)
  vec2 offset{0.f};
  int texMode = 0;   // see uvview.fs.glsl
  float dim = 0.7f;
  bool wireframe = true;
  bool seams = true;
  bool selection = true;
  bool distortion = false;
  bool tiling = false;
  bool faceSets = false;  // (1.2) each face set's faces tinted in its color
  // Texture tiles: the tile shown (its local 0..1 square is the view's 0..1; -1 = all / no tiles)
  int tile = -1;
};

class Renderer {
 public:
  void renderViewport(Document& doc, const Environment& env, const Camera& cam, RenderTarget& rt, int w, int h,
                      const ViewportOptions& opt, GLuint maskTex = 0);
  void renderUV(Document& doc, RenderTarget& rt, int w, int h, const UVViewOptions& opt, GLuint tex);
  static mat4 uvViewMatrix(const UVViewOptions& opt);
  // Read back RGBA8 (row 0 = top)
  static std::vector<uint8_t> readPixels(const RenderTarget& rt);
  uint64_t frames = 0;
  // (1.2) calls its argument for each of the scene's other objects that is shown (set by the engine): the 3D view
  // draws them, with their own textures, where they are
  std::function<void(const std::function<void(const Document&)>&)> sceneObjects;
  std::vector<const Document*> shownObjects(const Document& active) const;  // the active one (if shown) and those
  // (0.39) the view's path tracer (its samples so far); render images use one of their own
  const PathTracer& pathTracer() const { return pathView_; }
  PathTracer& renderTracer() { return pathRender_; }

 private:
  void drawSceneObject(const Document& d, const Environment& env, const Camera& cam, float aspect,
                       const ViewportOptions& opt, bool seeThrough);
  void drawSeeThrough(Shader& s, const GpuMesh& g);
  PathTracer pathView_, pathRender_;
  uint64_t faceDataKey_ = 0;
  uint64_t uvAuxKey_ = 0, uvAuxSerial_ = 0;  // (1.2) what the GPU mesh's aux holds for the UV editor (face set colors)
  Texture2D faceData_;  // per-face value for the density / stretch views (R32F, 2048 wide)
  uint64_t retopoDataKey_ = ~0ull;
  Texture2D retopoData_;  // per face: the patch, or log2 of the painted density
  std::shared_ptr<GpuMesh> retopoGpu_;
  uint64_t retopoGpuKey_ = ~0ull;
  GLuint retopoLineVao_ = 0, retopoLineVbo_ = 0;
  uint64_t retopoLineKey_ = ~0ull;
  GLuint collisionVao_ = 0, collisionVbo_ = 0;
  GLsizei collisionCount_ = 0;
  uint64_t collisionKey_ = ~0ull;
  GLsizei retopoLineCount_ = 0, retopoDoneCount_ = 0, retopoStrokeCount_ = 0, retopoMarkCount_ = 0;
  GLuint retopoLoopVao_ = 0, retopoLoopVbo_ = 0;
  uint64_t retopoLoopKey_ = ~0ull;
  GLsizei retopoLoopCount_ = 0;
};

}  // namespace patina
