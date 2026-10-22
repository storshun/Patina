#pragma once
#include <memory>
#include <vector>

#include "core/Bvh.h"
#include "engine/BrushLibrary.h"
#include "engine/Camera.h"
#include "engine/MaterialLibrary.h"
#include "engine/Stencil.h"
#include "engine/Undo.h"
#include "gfx/Texture.h"

namespace patina {

class Document;
class GpuMesh;
class Shader;

enum class PaintTool { Paint = 0, Erase, Fill, Picker, Blur, Clone, Smudge };
enum class FillMode { Face = 0, UVIsland, Connected, Mesh, Selection, Material };

struct StrokePoint {
  vec2 pos;              // viewport pixels (origin top-left) or UV coordinates (uv strokes)
  float pressure = 1.f;  // 0..1 (pen), 1 for mouse
  vec2 tilt{0.f};        // pen tilt in degrees (x left-right, y top-down), 0 for mouse
};

// The stroke engine. Converts input paths into brush dabs, accumulates them in a
// per-stroke coverage mask in texture space and blends the brush material into
// the target layer (or its mask). See docs/PLAN.md "How UV-independent painting works".
class Painter {
 public:
  // --- settings (edited by UI / automation)
  BrushPreset brush;
  MaterialRef material;   // what the brush paints
  PaintTool tool = PaintTool::Paint;
  FillMode fillMode = FillMode::UVIsland;
  int channelMask = kAllChannels;  // channels the brush writes (opacity, sheen, coat and subsurface only when
                                   // the material has them or the layer has them already)
  std::array<float, kChannelCount> channelOpacity{1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
  bool symmetry[3] = {false, false, false};  // mirror across object X/Y/Z (any combination)
  int radialCount = 1;                       // radial symmetry: copies around radialAxis (1 = off)
  int radialAxis = 1;                        // 0 X, 1 Y, 2 Z
  bool anySymmetry() const { return symmetry[0] || symmetry[1] || symmetry[2] || radialCount > 1; }
  int heightMode = 0;     // 0 replace, 1 add, 2 max
  float blurRadius = 6.f; // texels
  bool seamAware = true;  // blur and smudge reach across UV seams (Document's seam map)
  // Clone: source in UV space; "aligned" keeps the source offset for later strokes
  bool hasCloneSource = false;
  vec2 cloneSourceUV{0.f};
  bool cloneAligned = true;
  void setCloneSource(vec2 uv) {
    cloneSourceUV = uv;
    hasCloneSource = true;
    cloneOffsetValid_ = false;
  }
  vec2 cloneOffsetUV() const { return cloneOffsetUV_; }
  bool cloneOffsetValid() const { return cloneOffsetValid_; }
  float maskValue = 1.f;  // value painted into masks with the Paint tool
  Stencil stencil;        // screen-fixed picture strokes paint through (mask) or paint (decal)
  // Stroke locks: a stroke only paints the connected part of the mesh (topology) or the
  // UV island (island) under its first dab, plus their symmetry counterparts.
  bool lockTopology = false;
  bool lockIsland = false;
  // 0.10: the texture set (or material slot, with one texture) under the first dab
  bool lockSet = false;
  bool lockFaceSet = false;  // (1.2) strokes stay inside the face set they start in (Document::faceSets)
  int isolateTile = -1;  // the viewport isolates this texture set: strokes leave the others alone
  bool lockEnabled() const { return lockTopology || lockIsland || lockSet || lockFaceSet || isolateTile >= 0; }
  // Faces of the lock region for a stroke starting on `face` (and mirrored seeds).
  std::vector<uint8_t> lockRegion(const Document& doc, const std::vector<uint32_t>& seeds) const;
  std::vector<uint8_t> lockRegionNoSets(const Document& doc, const std::vector<uint32_t>& seeds) const;

  struct View {
    Camera camera;  // (1.2) in the painted object's space (Engine::paintView)
    vec2 viewport{1, 1};
    // (1.2) the scene's other objects, in that space: they hide what is behind them from the brush
    std::vector<std::pair<const GpuMesh*, mat4>> occluders;
  };

  bool init();  // GL resources
  // UV strokes: points are UV coordinates, `uvPerPixel` converts the brush size.
  bool beginStroke(Document& doc, MaterialLibrary& mats, BrushLibrary& brushes, const View& view,
                   const StrokePoint& p, bool uvSpace = false, float uvPerPixel = 0.001f);
  void addPoint(const StrokePoint& p);
  void endStroke();
  void cancelStroke();
  bool active() const { return doc_ != nullptr; }
  // Renders pending dabs and refreshes the target layer. Returns true if anything changed.
  bool update();
  // Paints the whole stencil onto the surface visible from `view` in one step (decal projection).
  bool projectStencil(Document& doc, MaterialLibrary& mats, BrushLibrary& brushes, const View& view);
  // Fills faces with the brush material (polygon fill tool).
  bool fillFaces(Document& doc, MaterialLibrary& mats, const std::vector<uint32_t>& faces);
  std::vector<uint32_t> facesForFill(Document& doc, uint32_t face, FillMode mode) const;

  // Straight line from the last stroke end (shift-click)
  bool hasLastPoint() const { return hasLast_; }
  StrokePoint lastPoint() const { return last_; }

  uint64_t dabCount() const { return totalDabs_; }
  double lastStrokeEnd = 0;  // nowSeconds() of the last finished stroke
  const Texture2D& strokeMask() const { return strokeMask_; }

 private:
  struct Dab {
    vec2 pos;
    float radius;
    float angle;
    float flow;
    float roundness;
    vec3 c3{0};
    vec3 n3{0};
    int chain = 0;     // which symmetry copy (smudge follows each copy's own path)
    bool hit = true;   // smudge: the dab landed on the surface (c3 valid)
    float worldR = 0;  // smudge: radius on the surface (object units), for the texture-space bounds
  };
  void emitAlong(vec2 from, vec2 to, float p0, float p1);
  void emitDab(vec2 pos, float pressure, vec2 dir);
  bool prepareTarget();
  bool establishLock(const RayHit* hit, vec2 uv);  // first dab of a locked stroke
  void releaseLock();
  void renderDepth();
  void renderDabs();
  void renderDabsInto(Texture2D& target, const Dab* dabs, size_t n);
  void smudgeDabs();
  bool dabTexelBox(const Dab& dab, int pad, glm::ivec4& box) const;  // x0, y0, x1, y1 (exclusive)
  // (0.44.2) the texels the pending dabs can reach (false: anywhere): the passes of an update stay inside it
  bool pendingBox(glm::ivec4& box) const;
  void scissor(bool on) const;
  bool boxed_ = false;
  glm::ivec4 box_{0};
  float appliedOpacity_ = -1.f;
  // (0.44.2) screen dabs paint whatever is seen in their circle: per screen cell (kCell pixels, the camera is fixed
  // during a stroke), the atlas texels of every triangle that projects there
  static constexpr int kCell = 16;
  std::vector<glm::ivec4> cells_;
  int cellsW_ = 0, cellsH_ = 0;
  void buildScreenCells();  // the stroke's opacity at the last update (a pen's pressure raises it: all again)
  void applyStroke();
  void pushUndo(const std::string& label);
  float diameter(float pressure) const;

  Document* doc_ = nullptr;
  MaterialLibrary* mats_ = nullptr;
  BrushLibrary* brushes_ = nullptr;
  View view_;
  bool uvSpace_ = false;
 public:
  // UV-space strokes with texture tiles: only the faces of this tile take paint (-1: all)
  int uvTile = -1;
 private:
  float uvPerPixel_ = 0.001f;
  bool targetMask_ = false;
  bool maskCreated_ = false;
  bool bakedTarget_ = false;  // (0.41) a baked map layer's fixes
  Texture2D bakedComp_;       // its bake with the fixes at the stroke's start (blur and clone read it)
  vec2 cloneOffsetUV_{0.f};
  bool cloneOffsetValid_ = false;
  bool stencilActive_ = false;
  void bindStencil(Shader& s);
  uint32_t layerUid_ = 0;
  std::vector<int> targetChannels_;
  std::unique_ptr<TextureSwapEntry> snapshot_;

  RenderTarget depth_;
  Texture2D strokeMask_;
  Texture2D dummyTransparent_;
  Texture2D dabMask_;          // smudge: one dab's coverage
  Texture2D smudgeTmp_[kChannelCount];  // smudge: copies of the live channels around the dab
  std::vector<vec4> smudgePrev_;  // smudge: last dab center per symmetry chain (w = valid)
  std::vector<uint32_t> boxTris_;
  std::vector<Dab> pending_;
  std::vector<Dab> allDabs_;  // for re-rendering on resize (not needed today)
  vec2 lastPos_{0}, lazyPos_{0};
  float lastPressure_ = 1.f;
  vec2 tilt_{0.f};
  float distToNext_ = 0.f;
  vec2 lastDir_{1, 0};
  float maxPressure_ = 0.f;
  bool firstDab_ = true;
  bool dirty_ = false;
  Rng rng_;
  uint64_t totalDabs_ = 0;
  bool hasLast_ = false;
  StrokePoint last_;
  enum class Lock { Off, Pending, Active } lock_ = Lock::Off;
};

}  // namespace patina
