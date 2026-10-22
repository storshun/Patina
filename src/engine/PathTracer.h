#pragma once
// (0.39) A progressive path tracer for the 3D view's Path traced mode and the render image: the model with its final
// textures (base color, roughness and metallic, normal, emissive, opacity, sheen, clearcoat, subsurface), lit by the
// environment (importance sampled, so a sun in an HDRI gives sharp shadows), a shadow-catching ground under it.
// The rays run in a compute shader over the model's BVH (core/Bvh, copied to the GPU); the samples add up in a
// floating-point image while nothing changes, and start again when anything does.
#include <cstdint>
#include <vector>

#include "core/Bvh.h"
#include "engine/Camera.h"
#include "gfx/Texture.h"

namespace patina {

class Document;
class Environment;

struct PathTraceSettings {
  int maxSamples = 512;   // the view stops adding samples there
  int bounces = 6;        // light bounces after the first hit
  bool ground = true;     // a ground under the model that catches its shadow (invisible otherwise)
  float clamp = 12.f;     // the brightest an indirect sample may be (fewer fireflies; 0 = off)
};

class PathTracer {
 public:
  ~PathTracer();
  // The view: adds as many samples as fit in a frame's time (a coarse picture first while things change).
  // `key`: changes whenever the camera, the model, its textures, the light or the settings do.
  void progress(Document& doc, const Environment& env, const Camera& cam, int w, int h, const PathTraceSettings& s,
                uint64_t key);
  // The render image and MCP: `samples` full samples now (the GPU is waited for).
  void render(Document& doc, const Environment& env, const Camera& cam, int w, int h, const PathTraceSettings& s,
              int samples);
  // The render image in steps (the app, a frame's time at a time): start(), then step() until it returns true.
  void start(Document& doc, const Environment& env, int w, int h);
  bool step(Document& doc, const Environment& env, const Camera& cam, const PathTraceSettings& s, int samples, double budgetNs);
  // Draws the picture (over the raster pass, which keeps its depth for the overlays). `envBackground`, `blurLod`
  // and the gradient: the background, as the raster view draws it.
  void draw(const Environment& env, const Camera& cam, int w, int h, bool envBackground, float blurLod, bool transparent);
  int samples() const { return samples_; }  // full samples so far (every pixel has at least these)
  bool done(const PathTraceSettings& s) const { return samples_ >= s.maxSamples; }
  void reset() { key_ = ~0ull; }
  void release();  // frees the sample images (a render image's tracer, once its picture is drawn)
  // (1.2) the scene's objects to trace (those shown), set before each call; with one or none, the document the call
  // is given. Several are traced together: their triangles in the world in one BVH, their final maps in texture arrays.
  void setScene(std::vector<const Document*> docs) { scene_ = std::move(docs); }
  // changes whenever the objects, where they are or their textures do (part of progress()'s key)
  static uint64_t sceneKey(const std::vector<const Document*>& docs);

 private:
  bool prepare(Document& doc, const Environment& env, int w, int h);
  void dispatch(Document& doc, const Environment& env, const Camera& cam, const PathTraceSettings& s, int block,
                int y0, int y1, bool overwrite);
  void uploadBvh(const Bvh& bvh);
  // (1.2) several objects
  std::vector<const Document*> scene_;
  bool sceneMode() const { return scene_.size() > 1; }
  bool prepareScene();
  unsigned sceneVbo_ = 0, sceneIbo_ = 0, sceneObj_ = 0, arr_[5] = {0, 0, 0, 0, 0}, fbo_[2] = {0, 0};
  uint64_t sceneGeomKey_ = 0;
  Bvh sceneBvh_;
  int arrRes_ = 0, arrLayers_ = 0;
  unsigned arrFormat_[5] = {0, 0, 0, 0, 0};
  std::vector<uint64_t> arrKeys_;  // per layer: the object's mesh, composite and size it was copied at
  float sceneGroundY_ = 0.f, sceneEps_ = 1e-5f;
  int sceneOpacity_ = 0;
  void buildEnvCdf(const Environment& env);
  void pollTimer();

  Texture2D accumA_, accumB_;  // A: rgb the model's light, a samples; B: ground visible, ground weight, ground samples, model samples
  int w_ = 0, h_ = 0;
  uint64_t key_ = ~0ull, bvhSerial_ = 0, envSerial_ = 0;
  unsigned nodeBuf_ = 0, triIdxBuf_ = 0;
  Texture2D envCond_, envMarg_;
  float envTotal_ = 0.f;
  int samples_ = 0;       // full passes done
  int bandY_ = 0;         // the next row of the pass under way
  bool previewShown_ = false;
  uint32_t frame_ = 0;
  // GPU time per traced sample (nanoseconds), measured with timer queries
  double nsPerSample_ = 60.0;
  unsigned queries_[3] = {0, 0, 0};
  double querySamples_[3] = {0, 0, 0};
  int queryNext_ = 0;
  bool queryBusy_[3] = {false, false, false};
};

}  // namespace patina
