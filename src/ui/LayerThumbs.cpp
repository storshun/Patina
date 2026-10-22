// Thumbnails for the layer list: the layer's content (paint or fill material) and its
// mask, downsampled on the GPU. A few are refreshed per frame so long stacks stay cheap.
#include "core/Log.h"
#include "gfx/Shader.h"
#include "ui/App.h"

namespace patina {

static void downsample(Texture2D& dst, GLuint src, int mode, int size) {
  if (!dst.valid() || dst.width() != size) {
    dst = Texture2D(size, size, GL_RGBA8, 1);
    dst.setFilter(GL_LINEAR, GL_LINEAR);
  }
  Framebuffer f;
  f.attach(0, dst);
  f.setDrawBuffers(1);
  gl::FramebufferScope fs(f.id());
  gl::ViewportScope vp(0, 0, size, size);
  glDisable(GL_BLEND);
  glDisable(GL_DEPTH_TEST);
  Shader& s = ShaderCache::get("fullscreen.vs.glsl", "thumb_layer.fs.glsl");
  s.tex("uSrc", 0, src);
  s.set("uMode", mode);
  s.use();
  drawFullscreenTriangle();
}

const App::LayerThumb& App::layerThumb(const Layer& l) {
  LayerThumb& t = layerThumbs[l.uid];
  double now = nowSeconds();
  bool due = t.updated < 0 || (now - t.updated > 0.6 && !engine.painter.active()) || (now - t.updated > 2.0);
  if (!due || thumbBudget <= 0) return t;
  --thumbBudget;
  t.updated = now;
  const int S = int(48 * uiScale);
  Document& d = engine.doc;
  const Layer* c = d.contentLayer(l);
  t.hasContent = false;
  if (l.isBaked()) {  // (0.41) the map it makes
    downsample(t.content, d.tex.bakedView(l.bakedMap).id(), 1, S);
    t.hasContent = true;
    t.hasMask = false;
    return t;
  }
  if (c->type == LayerType::Paint && c->hasChannel(0)) {
    downsample(t.content, c->channels[0]->id(), 0, S);
    t.hasContent = true;
  } else if (c->type == LayerType::Fill) {
    if (MaterialLibrary::Entry* e = c->material.id.empty() ? nullptr : engine.materials.find(c->material.id)) {
      if (GLuint th = engine.materials.thumbnail(*e, engine.env)) downsample(t.content, th, 0, S), t.hasContent = true;
    } else {
      // a flat color
      if (!t.content.valid() || t.content.width() != S) t.content = Texture2D(S, S, GL_RGBA8, 1);
      vec3 col = c->material.flat.color;
      t.content.clear(vec4(col, 1.f));
      t.hasContent = true;
    }
  }
  t.hasMask = false;
  if (!l.maskEffects.empty()) {
    const Texture2D& m = d.tex.evalMaskStack(l, d.mesh.bounds);
    downsample(t.mask, m.id(), 1, S);
    t.hasMask = true;
  } else if (l.mask) {
    downsample(t.mask, l.mask->id(), 1, S);
    t.hasMask = true;
  }
  return t;
}

}  // namespace patina
