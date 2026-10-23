#include "engine/Painter.h"

#include <climits>
#include <queue>

#include "core/Image.h"
#include "core/Log.h"
#include "engine/Document.h"
#include "gfx/Shader.h"

namespace patina {

static constexpr int kMaxDabsPerPass = 64;
static constexpr uint32_t kFillFlag = 2u;

bool Painter::init() {
  if (!dummyTransparent_.valid()) {
    dummyTransparent_.create(4, 4, GL_RGBA8, 1);
    dummyTransparent_.clear(vec4(0));
  }
  return true;
}

float Painter::diameter(float pressure) const {
  float d = brush.size * (brush.pressureSize ? std::max(0.05f, pressure) : 1.f);
  return uvSpace_ ? d * uvPerPixel_ : d;
}

bool Painter::prepareTarget() {
  Document& d = *doc_;
  if (!d.hasMesh()) return false;
  d.checkpoint();  // record pending layer-setting edits before the stroke's own entry
  Layer* layer = d.selectedLayer();
  if (!layer) {
    layer = &d.addPaintLayer();
  }
  targetMask_ = d.editMask || layer->masksOnly();
  bakedTarget_ = layer->isBaked();
  // painting an instance paints its source (the mask is the instance's own)
  if (!targetMask_ && layer->instanceOf) layer = d.contentLayer(*layer);
  layerUid_ = layer->uid;
  targetChannels_.clear();
  ivec2 res = d.tex.size();
  snapshot_ = std::make_unique<TextureSwapEntry>();
  snapshot_->layerUid = layer->uid;
  if (targetMask_) {
    maskCreated_ = !layer->mask;
    Texture2D& m = layer->ensureMask(res, layer->masksOnly() ? 0.f : 1.f);
    auto copy = std::make_unique<Texture2D>(res.x, res.y, m.format(), 1);
    copy->copyFrom(m);
    snapshot_->slots.push_back({-1, std::move(copy)});
    if (bakedTarget_) d.tex.bakedWithFixes(*layer, bakedComp_);
  } else {
    // opacity, sheen, clearcoat and subsurface: only where the brush material has them, or the layer already
    // does (to erase, blur, smudge or clone them); otherwise every stroke would add four more textures
    int extraUsed = tool == PaintTool::Paint && mats_ ? mats_->extraChannelsUsed(material) : 0;
    for (int c = 0; c < kChannelCount; ++c) {
      if (!(channelMask & (1 << c))) continue;
      if (isExtraChannel(c) && !layer->hasChannel(c) && !(extraUsed & (1 << c))) continue;
      Texture2D& t = layer->ensureChannel(c, res);
      auto copy = std::make_unique<Texture2D>(res.x, res.y, t.format(), 1);
      copy->copyFrom(t);
      snapshot_->slots.push_back({c, std::move(copy)});
      targetChannels_.push_back(c);
    }
    if (targetChannels_.empty()) return false;
  }
  if (!strokeMask_.valid() || strokeMask_.size() != res) {
    strokeMask_.create(res.x, res.y, GL_R16F, 0);  // mips: used to find touched tiles for undo
    strokeMask_.setFilter(GL_NEAREST, GL_NEAREST);
  }
  strokeMask_.clear(vec4(0));
  return true;
}

void Painter::renderDepth() {
  // (1.2.5) every dab stamped from the screen tests against it: Screen brushes and UV brushes used in the 3D view
  // (before, only Screen ones drew it, and a UV brush tested against an older stroke's view)
  if (uvSpace_ || brush.projection == BrushProjection::Surface) return;
  ivec2 vp = glm::max(ivec2(view_.viewport), ivec2(1));
  depth_.ensure(vp.x, vp.y, GL_R8);
  gl::FramebufferScope fs(depth_.fbo.id());
  gl::ViewportScope v(0, 0, vp.x, vp.y);
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LESS);
  glDepthMask(GL_TRUE);
  glDisable(GL_BLEND);
  glDisable(GL_CULL_FACE);
  glClearDepth(1.0);
  glClear(GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT);
  Shader& s = ShaderCache::get("mesh.vs.glsl", "depth.fs.glsl");
  s.set("uModel", mat4(1));
  s.set("uViewProj", view_.camera.viewProj(view_.viewport.x / view_.viewport.y));
  s.use();
  doc_->gpu.drawTriangles();
  for (auto& [g, m] : view_.occluders) {  // (1.2) the other objects in front hide the model from the brush
    s.set("uModel", m);
    s.use();
    g->drawTriangles();
  }
  glDisable(GL_DEPTH_TEST);
}

bool Painter::beginStroke(Document& doc, MaterialLibrary& mats, BrushLibrary& brushes, const View& view,
                          const StrokePoint& p, bool uvSpace, float uvPerPixel) {
  appliedOpacity_ = -1.f;
  if (active()) endStroke();
  init();
  doc_ = &doc;
  mats_ = &mats;
  brushes_ = &brushes;
  view_ = view;
  uvSpace_ = uvSpace || brush.projection == BrushProjection::UV;
  uvPerPixel_ = uvPerPixel;
  if (brush.projection == BrushProjection::UV && !uvSpace) {
    // UV brush used in the 3D view: dabs are still placed by projecting the cursor
    uvSpace_ = false;
  }
  if (tool == PaintTool::Clone) {
    if (!hasCloneSource) {
      PLOG_W("Clone: set a source first (Ctrl+click)");
      doc_ = nullptr;
      return false;
    }
    vec2 startUV = p.pos;
    if (!uvSpace_) {
      vec3 o, d;
      view_.camera.ray(p.pos, view_.viewport, o, d);
      RayHit h = doc.bvh.raycast(doc.mesh, o, d, 1e30f, doc.hiddenMask());
      if (!h.hit) {
        doc_ = nullptr;
        return false;
      }
      startUV = h.uv;
    }
    if (!cloneAligned || !cloneOffsetValid_) {
      cloneOffsetUV_ = cloneSourceUV - startUV;
      cloneOffsetValid_ = true;
    }
  }
  if (!prepareTarget()) {
    doc_ = nullptr;
    return false;
  }
  renderDepth();
  buildScreenCells();
  pending_.clear();
  smudgePrev_.assign(8 * 64, vec4(0));
  if (seamAware && (tool == PaintTool::Blur || tool == PaintTool::Smudge)) doc.tex.ensureSeamMap(doc.mesh, doc.bvh);
  stencilActive_ = stencil.enabled && stencil.valid() && !uvSpace_ && (tool == PaintTool::Paint || tool == PaintTool::Erase);
  releaseLock();
  lock_ = lockEnabled() ? Lock::Pending : Lock::Off;
  rng_ = Rng(0x9e3779b97f4a7c15ULL ^ totalDabs_);
  lastPos_ = lazyPos_ = p.pos;
  lastPressure_ = p.pressure;
  tilt_ = p.tilt;
  maxPressure_ = p.pressure;
  distToNext_ = 0.f;
  firstDab_ = true;
  emitDab(p.pos, p.pressure, lastDir_);
  dirty_ = true;
  return true;
}

void Painter::addPoint(const StrokePoint& p) {
  if (!active()) return;
  maxPressure_ = std::max(maxPressure_, p.pressure);
  tilt_ = p.tilt;
  vec2 target = p.pos;
  float lazy = uvSpace_ ? brush.lazyRadius * uvPerPixel_ : brush.lazyRadius;
  if (lazy > 0.f) {
    vec2 d = target - lazyPos_;
    float len = glm::length(d);
    if (len <= lazy) return;
    target = lazyPos_ + d * (1.f - lazy / len);
    lazyPos_ = target;
  }
  emitAlong(lastPos_, target, lastPressure_, p.pressure);
  lastPos_ = target;
  lastPressure_ = p.pressure;
}

void Painter::emitAlong(vec2 from, vec2 to, float p0, float p1) {
  vec2 d = to - from;
  float len = glm::length(d);
  if (len < 1e-6f) return;
  vec2 dir = d / len;
  lastDir_ = dir;
  float t = 0.f;
  for (int guard = 0; guard < 4096; ++guard) {
    float pr = p0 + (p1 - p0) * (t / len);
    // smudge: every dab is its own pass, and a tenth of the diameter already drags smoothly
    float sp = tool == PaintTool::Smudge ? std::max(brush.spacing, 0.1f) : std::max(brush.spacing, 0.01f);
    float spacing = std::max(diameter(pr) * sp, uvSpace_ ? 1e-5f : 0.5f);
    float need = distToNext_ > 0 ? distToNext_ : spacing;
    if (t + need > len) {
      distToNext_ = need - (len - t);
      return;
    }
    t += need;
    distToNext_ = 0.f;
    float pt = p0 + (p1 - p0) * (t / len);
    emitDab(from + dir * t, pt, dir);
  }
}

void Painter::emitDab(vec2 pos, float pressure, vec2 dir) {
  Dab dab;
  float dia = diameter(pressure);
  if (brush.sizeJitter > 0) dia *= std::max(0.05f, 1.f + brush.sizeJitter * rng_.signedUniform());
  vec2 perp(-dir.y, dir.x);
  if (brush.scatter > 0) pos += perp * (rng_.signedUniform() * brush.scatter * dia) + dir * (rng_.signedUniform() * brush.scatter * dia * 0.5f);
  // angle in the input space (pixels are y-down, UVs y-up); screen dabs are flipped below
  float angle = uvSpace_ ? glm::radians(brush.angleDeg) : -glm::radians(brush.angleDeg);
  if (brush.followPath) angle += std::atan2(dir.y, dir.x);
  // the way the pen leans (tilt is in screen space, y down; UV space is y up)
  if (brush.tiltAngle && glm::length(tilt_) > 6.f) angle += std::atan2(uvSpace_ ? -tilt_.y : tilt_.y, tilt_.x);
  if (brush.angleJitter > 0) angle += rng_.signedUniform() * brush.angleJitter * kPi;
  float flow = brush.flow * (brush.pressureFlow ? pressure : 1.f);
  if (brush.flowJitter > 0) flow *= std::clamp(1.f - brush.flowJitter * rng_.uniform(), 0.f, 1.f);
  dab.pos = pos;
  dab.radius = std::max(dia * 0.5f, uvSpace_ ? 1e-6f : 0.5f);
  dab.angle = angle;
  dab.flow = std::clamp(flow, 0.f, 1.f);
  dab.roundness = std::clamp(brush.roundness, 0.05f, 1.f);

  Document& d = *doc_;
  // Symmetry copies: every combination of the enabled mirror axes, each repeated
  // radialCount times around radialAxis (all through the object origin).
  auto addMirrors = [&](const Dab& base, const vec3& c, const vec3& n) {
    int nr = std::clamp(radialCount, 1, 64);
    for (int mm = 0; mm < 8; ++mm) {
      if (((mm & 1) && !symmetry[0]) || ((mm & 2) && !symmetry[1]) || ((mm & 4) && !symmetry[2])) continue;
      int flips = (mm & 1) + ((mm >> 1) & 1) + ((mm >> 2) & 1);
      for (int r = 0; r < nr; ++r) {
        if (mm == 0 && r == 0) continue;  // the original dab
        vec3 mc = c, mn = n;
        for (int ax = 0; ax < 3; ++ax)
          if ((mm >> ax) & 1) mc[ax] = -mc[ax], mn[ax] = -mn[ax];
        if (r) {
          vec3 axis(0.f);
          axis[std::clamp(radialAxis, 0, 2)] = 1.f;
          glm::mat3 R = glm::mat3(glm::rotate(glm::mat4(1.f), 2.f * kPi * float(r) / float(nr), axis));
          mc = R * mc;
          mn = R * mn;
        }
        Dab m = base;
        m.chain = mm * nr + r;
        m.c3 = mc;
        m.hit = true;
        if (flips & 1) m.angle = -base.angle;
        if (brush.projection == BrushProjection::Surface) {
          m.c3 = mc;
          m.n3 = mn;
          pending_.push_back(m);
        } else {
          // project the copy back to the screen where the camera sees that spot; (1.2.5) where it doesn't (off
          // screen, facing away or behind something), stamp it on the surface instead, as a Surface brush would:
          // symmetry follows the model, not the view
          float aspect = view_.viewport.x / view_.viewport.y;
          vec4 clip = view_.camera.viewProj(aspect) * vec4(mc, 1);
          bool seen = false;
          if (clip.w > 0) {
            vec2 ndc = vec2(clip) / clip.w;
            m.pos = {(ndc.x * 0.5f + 0.5f) * view_.viewport.x, (ndc.y * 0.5f + 0.5f) * view_.viewport.y};
            if (std::abs(ndc.x) <= 1.f && std::abs(ndc.y) <= 1.f) {
              vec3 o, rd;
              view_.camera.ray(vec2(m.pos.x, view_.viewport.y - m.pos.y), view_.viewport, o, rd);
              RayHit h = d.bvh.raycast(d.mesh, o, rd, 1e30f, d.hiddenMask());
              seen = h.hit && glm::dot(mn, rd) < 0 &&
                     glm::length(h.position - mc) < std::max(base.worldR * 0.25f, d.mesh.boundingRadius * 1e-3f);
            }
          }
          if (!seen) {
            if (base.worldR <= 0) continue;
            m.onSurface = true;
            m.n3 = mn;
            m.radius = base.worldR;
          }
          pending_.push_back(m);
        }
      }
    }
  };

  if (uvSpace_) {
    // a locked stroke starts at the first dab that lands on a face
    if (lock_ == Lock::Pending && !establishLock(nullptr, pos)) return;
    pending_.push_back(dab);
  } else {
    // screen position: convert to GL convention (origin bottom-left) for the shader
    vec2 glPos(pos.x, view_.viewport.y - pos.y);
    vec3 o, rd;
    view_.camera.ray(pos, view_.viewport, o, rd);
    RayHit hit;
    bool needHit = brush.projection == BrushProjection::Surface || anySymmetry() || lock_ == Lock::Pending ||
                   tool == PaintTool::Smudge;
    if (needHit) hit = d.bvh.raycast(d.mesh, o, rd, 1e30f, d.hiddenMask());
    if (lock_ == Lock::Pending && (!hit.hit || !establishLock(&hit, vec2(0)))) return;
    if (brush.projection == BrushProjection::Surface) {
      if (!hit.hit) return;
      float pw = view_.camera.pixelWorld(view_.viewport.y);
      float worldR = dab.radius * (view_.camera.ortho ? pw : pw * hit.t);
      dab.c3 = hit.position;
      dab.n3 = glm::dot(hit.faceNormal, rd) > 0 ? -hit.normal : hit.normal;
      dab.radius = worldR;
      dab.worldR = worldR;
      pending_.push_back(dab);
      addMirrors(dab, hit.position, dab.n3);
    } else {
      dab.pos = glPos;
      dab.angle = -dab.angle;
      dab.hit = hit.hit;
      if (hit.hit) {
        float pw = view_.camera.pixelWorld(view_.viewport.y);
        dab.c3 = hit.position;
        dab.worldR = dab.radius * (view_.camera.ortho ? pw : pw * hit.t);
      }
      pending_.push_back(dab);
      if (hit.hit) addMirrors(dab, hit.position, hit.normal);
    }
  }
  ++totalDabs_;
  dirty_ = true;
}

void Painter::renderDabs() {
  if (pending_.empty()) return;
  renderDabsInto(strokeMask_, pending_.data(), pending_.size());
  pending_.clear();
}

void Painter::renderDabsInto(Texture2D& target, const Dab* dabs, size_t count) {
  if (!count) return;
  Document& d = *doc_;
  ivec2 res = d.tex.size();
  Framebuffer fbo;
  fbo.attach(0, target);
  fbo.setDrawBuffers(1);
  gl::FramebufferScope fs(fbo.id());
  gl::ViewportScope vp(0, 0, res.x, res.y);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glEnable(GL_BLEND);
  glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_COLOR);
  Shader& s = ShaderCache::get("uvraster.vs.glsl", "paint_stamp.fs.glsl");
  int mode = uvSpace_ ? 2 : (brush.projection == BrushProjection::Surface ? 1 : 0);
  // (1.2.5) a screen stroke's mirror copies the camera can't see go on the surface: a pass of their own after
  std::vector<Dab> onSurface;
  std::vector<Dab> onScreen;
  if (mode == 0 && std::any_of(dabs, dabs + count, [](const Dab& x) { return x.onSurface; })) {
    for (size_t i = 0; i < count; ++i) (dabs[i].onSurface ? onSurface : onScreen).push_back(dabs[i]);
    dabs = onScreen.data();
    count = onScreen.size();
  }
  s.set("uMode", mode);
  GLuint alpha = brushes_->alphaTexture(brush.alpha);
  s.set("uAlphaIsRound", alpha ? 0 : 1);
  s.tex("uAlpha", 0, alpha ? alpha : dummyTransparent_.id());
  s.set("uHardness", brush.hardness);
  s.set("uUVTransform", vec4(1, 1, 0, 0));
  s.set("uUseAuxAsUV", 0);
  s.set("uTile", uvSpace_ && d.mesh.tiles.active() ? uvTile : -1);
  s.set("uLockFlag", lock_ == Lock::Active ? Document::kPaintLockFlag : 0u);
  if (mode == 0) {
    float aspect = view_.viewport.x / view_.viewport.y;
    s.set("uViewProj", view_.camera.viewProj(aspect));
    s.set("uCamPos", view_.camera.ortho ? -view_.camera.forward() : view_.camera.eye());
    s.set("uViewport", view_.viewport);
    s.tex("uDepth", 1, depth_.depth.id());
    s.set("uNear", view_.camera.ortho ? -view_.camera.farPlane : view_.camera.nearPlane);
    s.set("uFar", view_.camera.farPlane);
    s.set("uOrtho", view_.camera.ortho ? 1 : 0);
    s.set("uPixelWorld", view_.camera.pixelWorld(view_.viewport.y));
    s.set("uBackfaceCull", brush.backfaceCulling ? 1 : 0);
    s.set("uOcclusion", brush.occlusion ? 1 : 0);
    s.set("uAngleFalloff", brush.angleFalloff);
  }
  vec4 A[kMaxDabsPerPass], B[kMaxDabsPerPass], N[kMaxDabsPerPass];
  auto stamp = [&](const Dab* list, size_t total) {
    for (size_t i = 0; i < total; i += kMaxDabsPerPass) {
      int n = (int)std::min<size_t>(kMaxDabsPerPass, total - i);
      for (int k = 0; k < n; ++k) {
        const Dab& dab = list[i + k];
        if (mode == 1) {
          A[k] = vec4(dab.c3, dab.radius);
          B[k] = vec4(dab.flow, dab.roundness, dab.angle, 0);
          N[k] = vec4(dab.n3, 0);
        } else {
          A[k] = vec4(dab.pos, dab.radius, dab.angle);
          B[k] = vec4(dab.flow, dab.roundness, 0, 0);
          N[k] = vec4(0);
        }
      }
      s.set("uStampCount", n);
      s.setArray("uStampA", A, n);
      s.setArray("uStampB", B, n);
      s.setArray("uStampN", N, n);
      s.use();
      scissor(true);
      d.gpu.drawTriangles();
      scissor(false);
    }
  };
  stamp(dabs, count);
  if (!onSurface.empty()) {
    mode = 1;
    s.set("uMode", 1);
    stamp(onSurface.data(), onSurface.size());
  }
  s.set("uTile", -1);  // the program is shared (fills, locks): no tile filter left behind
  glDisable(GL_BLEND);
}

bool Painter::dabTexelBox(const Dab& dab, int pad, glm::ivec4& box) const {
  const Document& d = *doc_;
  ivec2 res = d.tex.size();
  vec2 lo(1e30f), hi(-1e30f);
  if (uvSpace_) {
    float r = dab.radius / std::max(dab.roundness, 0.05f);
    lo = dab.pos - vec2(r);
    hi = dab.pos + vec2(r);
  } else {
    // triangles near the dab on the surface (screen dabs stretch on slanted faces: generous)
    std::vector<uint32_t>& tris = const_cast<std::vector<uint32_t>&>(boxTris_);
    tris.clear();
    float r = dab.worldR * (brush.projection == BrushProjection::Surface || dab.onSurface ? 1.1f : 2.5f) /
              std::max(dab.roundness, 0.05f);
    d.bvh.querySphere(dab.c3, r, tris);
    for (uint32_t t : tris)
      for (int k = 0; k < 3; ++k) {
        vec2 uv = d.mesh.texUV(d.mesh.tris[t][k]);
        lo = glm::min(lo, uv);
        hi = glm::max(hi, uv);
      }
    if (tris.empty()) return false;
  }
  ivec2 a = glm::max(ivec2(glm::floor(lo * vec2(res))) - pad, ivec2(0));
  ivec2 b = glm::min(ivec2(glm::ceil(hi * vec2(res))) + pad, res);
  if (a.x >= b.x || a.y >= b.y) return false;
  box = glm::ivec4(a, b);
  return true;
}

// Smudge: each dab drags what is under it along the motion since the previous dab of the
// same symmetry copy. Per dab: its coverage into dabMask_, the live channels around it
// copied aside, then a pass that pulls each covered texel from where the surface point
// "behind" it is (the 3D motion mapped to texels through the position map's derivatives).
void Painter::smudgeDabs() {
  if (pending_.empty()) return;
  Document& d = *doc_;
  Layer* layer = d.findLayer(layerUid_);
  if (!layer) {
    pending_.clear();
    return;
  }
  ivec2 res = d.tex.size();
  if (!dabMask_.valid() || dabMask_.size() != res) {
    dabMask_.create(res.x, res.y, GL_R16F, 1);
    dabMask_.setFilter(GL_NEAREST, GL_NEAREST);
    dabMask_.clear(vec4(0));
  }
  struct Target { int loc; Texture2D* live; };
  std::vector<Target> targets;
  for (auto& slot : snapshot_->slots) {
    int loc = slot.channel < 0 ? 0 : slot.channel;
    Texture2D* live = slot.channel < 0 ? layer->mask.get() : layer->channels[slot.channel].get();
    if (!live) continue;
    Texture2D& tmp = smudgeTmp_[loc];
    if (!tmp.valid() || tmp.size() != res || tmp.format() != live->format()) {
      tmp.create(res.x, res.y, live->format(), 1);
      tmp.setFilter(GL_NEAREST, GL_NEAREST);
    }
    targets.push_back({loc, live});
  }
  if (targets.empty()) {
    pending_.clear();
    return;
  }
  const int maxOff = std::max(8, res.x / 64);  // texels a texel can be pulled from per dab
  bool seams = seamAware && d.tex.seamMapValid();
  std::vector<Dab> dabs;
  dabs.swap(pending_);
  for (const Dab& dab : dabs) {
    if ((size_t)dab.chain >= smudgePrev_.size()) continue;
    vec4& prev = smudgePrev_[dab.chain];
    vec3 cur = uvSpace_ ? vec3(dab.pos, 0) : dab.c3;
    if (!dab.hit) {
      prev.w = 0;
      continue;
    }
    if (prev.w == 0) {  // the first dab of this path: nothing to drag yet
      prev = vec4(cur, 1);
      continue;
    }
    vec3 delta = cur - vec3(prev);
    prev = vec4(cur, 1);
    if (glm::dot(delta, delta) <= 0.f) continue;
    glm::ivec4 box;
    if (!dabTexelBox(dab, maxOff + 2, box)) continue;
    int bw = box.z - box.x, bh = box.w - box.y;
    float zero = 0.f;
    glClearTexSubImage(dabMask_.id(), 0, box.x, box.y, 0, bw, bh, 1, GL_RED, GL_FLOAT, &zero);
    glEnable(GL_SCISSOR_TEST);
    glScissor(box.x, box.y, bw, bh);
    renderDabsInto(dabMask_, &dab, 1);
    for (auto& t : targets)
      glCopyImageSubData(t.live->id(), GL_TEXTURE_2D, 0, box.x, box.y, 0, smudgeTmp_[t.loc].id(), GL_TEXTURE_2D, 0,
                         box.x, box.y, 0, bw, bh, 1);
    for (int pass = 0; pass < 2; ++pass) {  // the first six channels (or the mask), then the other four
      bool extra = pass == 1;
      Framebuffer fbo;
      GLenum bufs[6] = {GL_NONE, GL_NONE, GL_NONE, GL_NONE, GL_NONE, GL_NONE};
      int maxLoc = 0;
      for (auto& t : targets) {
        if (isExtraChannel(t.loc) != extra) continue;
        int loc = extra ? t.loc - kFirstExtraChannel : t.loc;
        fbo.attach(loc, *t.live);
        bufs[loc] = GL_COLOR_ATTACHMENT0 + loc;
        maxLoc = std::max(maxLoc, loc + 1);
      }
      if (!maxLoc) continue;
      glNamedFramebufferDrawBuffers(fbo.id(), maxLoc, bufs);
      gl::FramebufferScope fs(fbo.id());
      gl::ViewportScope vp(0, 0, res.x, res.y);
      glDisable(GL_BLEND);
      glDisable(GL_DEPTH_TEST);
      Shader& s = extra ? ShaderCache::get("fullscreen.vs.glsl", "paint_apply.fs.glsl", {"EXTRA_PASS"})
                        : ShaderCache::get("fullscreen.vs.glsl", "paint_apply.fs.glsl");
      static const char* names[] = {"uSnapColor", "uSnapRough", "uSnapMetal", "uSnapHeight", "uSnapNormal",
                                    "uSnapEmissive", "uSnapX0", "uSnapX1", "uSnapX2", "uSnapX3"};
      for (int i = extra ? kFirstExtraChannel : 0; i < (extra ? kChannelCount : kFirstExtraChannel); ++i) {
        bool have = false;
        for (auto& t : targets) have |= t.loc == i;
        s.tex(names[i], extra ? i - kFirstExtraChannel : i, have ? smudgeTmp_[i].id() : dummyTransparent_.id());
      }
      s.tex("uStrokeMask", 6, dabMask_.id());
      s.tex("uPosMap", 7, d.tex.posMap.id());
      s.tex("uNrmMap", 8, d.tex.nrmMap.id());
      s.tex("uTanMap", 9, d.tex.tanMap.id());
      s.tex("uSeamMap", 16, seams ? d.tex.seamMap.id() : dummyTransparent_.id());
      s.set("uUseSeams", seams ? 1 : 0);
      s.set("uTool", targetMask_ ? 9 : 8);
      s.set("uBaked", bakedTarget_ && !extra ? 1 : 0);
      if (bakedTarget_) s.tex("uBakedRaw", 17, d.tex.bakedRaw(layer->bakedMap).id());
      s.set("uSmudgeUV", uvSpace_ ? 1 : 0);
      s.set("uSmudgeDelta", uvSpace_ ? vec3(vec2(delta) * vec2(res), 0) : delta);
      s.set("uSmudgeMax", float(maxOff));
      s.set("uOpacity", std::clamp(brush.opacity, 0.f, 1.f));
      s.set("uChanOpacity0", vec4(channelOpacity[0], channelOpacity[1], channelOpacity[2], channelOpacity[3]));
      s.set("uChanOpacity1", vec2(channelOpacity[4], channelOpacity[5]));
      s.set("uChanOpacity2", vec4(channelOpacity[6], channelOpacity[7], channelOpacity[8], channelOpacity[9]));
      s.use();
      drawFullscreenTriangle();
    }
    glDisable(GL_SCISSOR_TEST);
  }
  // the stroke mask still collects every dab: undo keeps the tiles it touched
  renderDabsInto(strokeMask_, dabs.data(), dabs.size());
  d.markComposite();
}

void Painter::applyStroke() {
  Document& d = *doc_;
  Layer* layer = d.findLayer(layerUid_);
  if (!layer) return;
  ivec2 res = d.tex.size();
  // two passes: the first six channels (or the mask), then opacity, sheen, clearcoat and subsurface
  for (int pass = 0; pass < 2; ++pass) {
    bool extra = pass == 1;
    // targets: copy of snapshot slots -> live textures
    Framebuffer fbo;
    GLenum bufs[6] = {GL_NONE, GL_NONE, GL_NONE, GL_NONE, GL_NONE, GL_NONE};
    const Texture2D* snaps[6] = {};
    int maxLoc = 0;
    for (auto& slot : snapshot_->slots) {
      if ((slot.channel >= 0 && isExtraChannel(slot.channel)) != extra) continue;
      int loc = slot.channel < 0 ? 0 : extra ? slot.channel - kFirstExtraChannel : slot.channel;
      Texture2D* live = slot.channel < 0 ? layer->mask.get() : layer->channels[slot.channel].get();
      if (!live) continue;
      fbo.attach(loc, *live);
      bufs[loc] = GL_COLOR_ATTACHMENT0 + loc;
      snaps[loc] = slot.tex.get();
      maxLoc = std::max(maxLoc, loc + 1);
    }
    if (!maxLoc) continue;
    glNamedFramebufferDrawBuffers(fbo.id(), maxLoc, bufs);
    gl::FramebufferScope fs(fbo.id());
    gl::ViewportScope vp(0, 0, res.x, res.y);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    Shader& s = extra ? ShaderCache::get("fullscreen.vs.glsl", "paint_apply.fs.glsl", {"EXTRA_PASS"})
                      : ShaderCache::get("fullscreen.vs.glsl", "paint_apply.fs.glsl");
    // bind the material first: a first-time procedural bake uses texture units itself
    float aspect = view_.viewport.x / std::max(view_.viewport.y, 1.f);
    mats_->bind(s, material, 10, view_.camera.viewProj(aspect), aspect);
    static const char* names[] = {"uSnapColor", "uSnapRough", "uSnapMetal", "uSnapHeight", "uSnapNormal", "uSnapEmissive"};
    static const char* xnames[] = {"uSnapX0", "uSnapX1", "uSnapX2", "uSnapX3"};
    for (int i = 0; i < (extra ? 4 : 6); ++i)
      s.tex(extra ? xnames[i] : names[i], i, snaps[i] ? snaps[i]->id() : dummyTransparent_.id());
    s.tex("uStrokeMask", 6, strokeMask_.id());
    s.tex("uPosMap", 7, d.tex.posMap.id());
    s.tex("uNrmMap", 8, d.tex.nrmMap.id());
    s.tex("uTanMap", 9, d.tex.tanMap.id());
    bool seams = seamAware && tool == PaintTool::Blur && d.tex.seamMapValid();
    s.tex("uSeamMap", 16, seams ? d.tex.seamMap.id() : dummyTransparent_.id());
    s.set("uUseSeams", seams ? 1 : 0);
    int toolId = tool == PaintTool::Erase ? 1 : tool == PaintTool::Blur ? 4 : tool == PaintTool::Clone ? 6 : 0;
    if (targetMask_) toolId = tool == PaintTool::Erase ? 3 : tool == PaintTool::Blur ? 5 : tool == PaintTool::Clone ? 7 : 2;
    s.set("uCloneOffset", cloneOffsetUV_ * vec2(res));
    s.set("uBlurRadius", blurRadius);
    s.set("uTool", toolId);
    float opacity = brush.opacity * (brush.pressureOpacity ? maxPressure_ : 1.f);
    s.set("uOpacity", std::clamp(opacity, 0.f, 1.f));
    s.set("uChanOpacity0", vec4(channelOpacity[0], channelOpacity[1], channelOpacity[2], channelOpacity[3]));
    s.set("uChanOpacity1", vec2(channelOpacity[4], channelOpacity[5]));
    s.set("uChanOpacity2", vec4(channelOpacity[6], channelOpacity[7], channelOpacity[8], channelOpacity[9]));
    s.set("uMaskValue", maskValue);
    s.set("uHeightMode", heightMode);
    s.set("uBaked", bakedTarget_ && !extra ? 1 : 0);
    if (bakedTarget_) {
      s.tex("uBakedRaw", 17, d.tex.bakedRaw(layer->bakedMap).id());
      s.tex("uBakedComp", 18, bakedComp_.valid() ? bakedComp_.id() : dummyTransparent_.id());
    }
    bindStencil(s);
    s.use();
    scissor(true);
    drawFullscreenTriangle();
    scissor(false);
  }
  if (boxed_) d.markCompositeBox(box_);
  else d.markComposite();
}

void Painter::bindStencil(Shader& s) {
  bool on = stencilActive_ && stencil.valid();
  s.set("uStencilOn", on ? 1 : 0);
  s.tex("uStencil", 15, on ? stencil.tex.id() : dummyTransparent_.id());
  if (!on) return;
  vec2 vp = glm::max(view_.viewport, vec2(1));
  s.set("uStencilXform", stencil.screenToStencil(vp));
  s.set("uStencilViewProj", view_.camera.viewProj(vp.x / vp.y));
  s.set("uStencilViewport", vp);
  s.set("uStencilMode", stencil.mode);
  s.set("uStencilInvert", stencil.invert ? 1 : 0);
  s.set("uStencilTile", stencil.tile ? 1 : 0);
  float texelsPerPixel = float(stencil.image.height) / std::max(stencil.scale * vp.y, 1.f);
  s.set("uStencilLod", std::max(0.f, std::log2(std::max(texelsPerPixel, 1.f))));
}

bool Painter::projectStencil(Document& doc, MaterialLibrary& mats, BrushLibrary& brushes, const View& view) {
  if (!stencil.valid() || !doc.hasMesh()) return false;
  if (active()) endStroke();
  init();
  doc_ = &doc;
  mats_ = &mats;
  brushes_ = &brushes;
  view_ = view;
  uvSpace_ = false;
  PaintTool keepTool = tool;
  if (tool != PaintTool::Erase) tool = PaintTool::Paint;
  BrushPreset keep = brush;
  auto restore = [&] {
    brush = keep;
    tool = keepTool;
    stencilActive_ = false;
    doc_ = nullptr;
  };
  if (!prepareTarget()) {
    restore();
    return false;
  }
  // one screen dab over the whole view, hard edged: the stencil decides the shape
  brush.projection = BrushProjection::Screen;
  brush.hardness = 1.f;
  brush.alpha = BrushAlpha{};
  renderDepth();
  lock_ = Lock::Off;
  Dab big;
  big.pos = view_.viewport * 0.5f;
  big.radius = glm::length(view_.viewport);
  big.flow = 1.f;
  big.roundness = 1.f;
  big.angle = 0.f;
  renderDabsInto(strokeMask_, &big, 1);
  brush = keep;
  brush.projection = BrushProjection::Screen;  // applyStroke reads nothing else from it but opacity
  stencilActive_ = true;
  applyStroke();
  pushUndo(tool == PaintTool::Erase ? "Erase through stencil" : "Project stencil");
  doc.dirty = true;
  restore();
  return true;
}

bool Painter::update() {
  if (!active() || !dirty_) return false;
  if (tool == PaintTool::Smudge) {
    smudgeDabs();
  } else {
    // (0.44.2) only the region the new dabs reach: on a big atlas (texture sets, 4K and up) every pass over all of
    // it made painting crawl
    boxed_ = pendingBox(box_);
    float opacity = brush.opacity * (brush.pressureOpacity ? maxPressure_ : 1.f);
    if (opacity != appliedOpacity_) boxed_ = false;  // (the whole stroke takes the new opacity)
    appliedOpacity_ = opacity;
    renderDabs();
    applyStroke();
    boxed_ = false;
  }
  dirty_ = false;
  return true;
}

void Painter::buildScreenCells() {
  cells_.clear();
  cellsW_ = cellsH_ = 0;
  if (uvSpace_ || brush.projection == BrushProjection::Surface || !doc_) return;
  const Document& d = *doc_;
  ivec2 res = d.tex.size();
  cellsW_ = std::max(1, (int)std::ceil(view_.viewport.x / kCell));
  cellsH_ = std::max(1, (int)std::ceil(view_.viewport.y / kCell));
  const glm::ivec4 none(INT_MAX, INT_MAX, INT_MIN, INT_MIN);
  cells_.assign(size_t(cellsW_) * cellsH_, none);
  float aspect = view_.viewport.x / std::max(view_.viewport.y, 1.f);
  mat4 vp = view_.camera.viewProj(aspect);
  auto grow = [](glm::ivec4& c, const glm::ivec4& b) {
    c = glm::ivec4(glm::min(glm::ivec2(c), glm::ivec2(b)), glm::max(glm::ivec2(c.z, c.w), glm::ivec2(b.z, b.w)));
  };
  for (size_t t = 0; t < d.mesh.tris.size(); ++t) {
    vec2 lo(1e30f), hi(-1e30f), tlo(1e30f), thi(-1e30f);
    bool crossing = false;
    for (int k = 0; k < 3; ++k) {
      uint32_t l = d.mesh.tris[t][k];
      vec4 c = vp * vec4(d.mesh.positions[d.mesh.loopVert[l]], 1.f);
      if (c.w <= 1e-6f) {
        crossing = true;
      } else {
        vec2 sp = (vec2(c) / c.w * 0.5f + 0.5f) * view_.viewport;  // (GL pixels: origin bottom left, as dab.pos)
        lo = glm::min(lo, sp), hi = glm::max(hi, sp);
      }
      vec2 uv = d.mesh.texUV(l);
      tlo = glm::min(tlo, uv), thi = glm::max(thi, uv);
    }
    glm::ivec4 tb(glm::max(glm::ivec2(glm::floor(tlo * vec2(res))) - 1, glm::ivec2(0)),
                  glm::min(glm::ivec2(glm::ceil(thi * vec2(res))) + 1, res));
    glm::ivec2 c0(0), c1(cellsW_ - 1, cellsH_ - 1);
    if (!crossing) {  // (a triangle through the camera's plane may cover any of the screen)
      if (hi.x < 0 || hi.y < 0 || lo.x > view_.viewport.x || lo.y > view_.viewport.y) continue;
      glm::ivec2 last = c1;
      c0 = glm::clamp(glm::ivec2(glm::floor(lo / float(kCell))), glm::ivec2(0), last);
      c1 = glm::clamp(glm::ivec2(glm::floor(hi / float(kCell))), glm::ivec2(0), last);
    }
    for (int y = c0.y; y <= c1.y; ++y)
      for (int x = c0.x; x <= c1.x; ++x) grow(cells_[size_t(y) * cellsW_ + x], tb);
  }
}

bool Painter::pendingBox(glm::ivec4& box) const {
  if (pending_.empty() || !doc_) return false;
  if (uvSpace_ && doc_->mesh.tiles.active()) return false;  // (dab positions in a tile's own UVs)
  bool screen = !uvSpace_ && brush.projection != BrushProjection::Surface;
  if (screen && cells_.empty()) return false;
  ivec2 res = doc_->tex.size();
  int pad = 4 + (tool == PaintTool::Blur ? (int)std::ceil(blurRadius) : 0);
  glm::ivec4 all(res.x, res.y, 0, 0);
  bool any = false;
  glm::ivec2 last(cellsW_ - 1, cellsH_ - 1);
  for (const Dab& dab : pending_) {
    glm::ivec4 b(INT_MAX, INT_MAX, INT_MIN, INT_MIN);
    if (screen && !dab.onSurface) {
      float r = dab.radius / std::max(dab.roundness, 0.05f) + 2.f;
      glm::ivec2 c0 = glm::clamp(glm::ivec2(glm::floor((dab.pos - r) / float(kCell))), glm::ivec2(0), last);
      glm::ivec2 c1 = glm::clamp(glm::ivec2(glm::floor((dab.pos + r) / float(kCell))), glm::ivec2(0), last);
      for (int y = c0.y; y <= c1.y; ++y)
        for (int x = c0.x; x <= c1.x; ++x) {
          const glm::ivec4& c = cells_[size_t(y) * cellsW_ + x];
          b = glm::ivec4(glm::min(glm::ivec2(b), glm::ivec2(c)), glm::max(glm::ivec2(b.z, b.w), glm::ivec2(c.z, c.w)));
        }
      if (b.x >= b.z || b.y >= b.w) continue;
      b = glm::ivec4(glm::max(glm::ivec2(b) - pad, glm::ivec2(0)), glm::min(glm::ivec2(b.z, b.w) + pad, res));
    } else if (!dabTexelBox(dab, pad, b)) {
      continue;
    }
    all = glm::ivec4(glm::min(glm::ivec2(all), glm::ivec2(b)), glm::max(glm::ivec2(all.z, all.w), glm::ivec2(b.z, b.w)));
    any = true;
  }
  if (!any) {
    box = glm::ivec4(0);  // (nothing in reach: an empty region)
    return true;
  }
  box = all;
  return true;
}

void Painter::scissor(bool on) const {
  if (on && boxed_) {
    glEnable(GL_SCISSOR_TEST);
    glScissor(box_.x, box_.y, std::max(0, box_.z - box_.x), std::max(0, box_.w - box_.y));
  } else {
    glDisable(GL_SCISSOR_TEST);
  }
}

void Painter::pushUndo(const std::string& label) {
  if (!snapshot_) return;
  snapshot_->label = label;
  if (maskCreated_) {  // undo removes the mask that this stroke created
    for (auto& s : snapshot_->slots)
      if (s.channel < 0) s.tex.reset();
    maskCreated_ = false;
    doc_->undo.push(std::move(snapshot_));
    return;
  }
  // Find the 64x64 tiles the stroke touched (mip of the stroke mask) and keep
  // only those: a typical stroke costs a few MB of undo instead of whole textures.
  const int ts = 64;
  ivec2 res = strokeMask_.size();
  int level = 0;
  while ((ts >> level) > 1) ++level;
  std::vector<ivec2> tiles;
  if (res.x >= ts * 2 && res.x % ts == 0 && res.y % ts == 0 && level < strokeMask_.levels()) {
    strokeMask_.generateMips();
    Image m = strokeMask_.download(1, level);  // row 0 = top
    for (int y = 0; y < m.height; ++y)
      for (int x = 0; x < m.width; ++x)
        if (m.at(x, y)[0] > 0.f) tiles.push_back({x, m.height - 1 - y});
    size_t total = size_t(m.width) * m.height;
    if (tiles.empty() || tiles.size() * 10 > total * 7) tiles.clear();  // nothing / almost everything: keep full copies
  }
  if (tiles.empty()) {
    doc_->undo.push(std::move(snapshot_));
    return;
  }
  auto e = std::make_unique<TileSwapEntry>();
  e->label = label;
  e->layerUid = snapshot_->layerUid;
  e->tileSize = ts;
  e->tiles = tiles;
  ivec2 as = TileSwapEntry::atlasSize(tiles.size(), ts);
  for (auto& s : snapshot_->slots) {
    if (!s.tex) continue;
    auto atlas = std::make_unique<Texture2D>(as.x, as.y, s.tex->format(), 1);
    for (size_t i = 0; i < tiles.size(); ++i)
      glCopyImageSubData(s.tex->id(), GL_TEXTURE_2D, 0, tiles[i].x * ts, tiles[i].y * ts, 0, atlas->id(), GL_TEXTURE_2D,
                         0, int(i % 32) * ts, int(i / 32) * ts, 0, ts, ts, 1);
    e->slots.push_back({s.channel, std::move(atlas)});
  }
  snapshot_.reset();
  doc_->undo.push(std::move(e));
}

void Painter::endStroke() {
  if (!active()) return;
  update();
  const char* label = tool == PaintTool::Blur ? "Blur" : tool == PaintTool::Clone ? "Clone"
                    : tool == PaintTool::Smudge ? (targetMask_ ? "Smudge mask" : "Smudge")
                    : targetMask_ ? (tool == PaintTool::Erase ? "Erase mask" : "Paint mask")
                                  : (tool == PaintTool::Erase ? "Erase" : "Paint stroke");
  if (bakedTarget_ && (tool == PaintTool::Paint || tool == PaintTool::Erase || tool == PaintTool::Smudge))
    label = tool == PaintTool::Erase ? "Erase fixes" : tool == PaintTool::Smudge ? "Smudge" : "Paint fixes";  // (0.41)
  pushUndo(label);
  releaseLock();
  doc_->markComposite();  // (0.44.2) all of it once: the gutters and mip levels a stroke's regions leave out
  doc_->dirty = true;
  hasLast_ = true;
  last_ = {lastPos_, lastPressure_};
  lastStrokeEnd = nowSeconds();
  doc_ = nullptr;
}

void Painter::cancelStroke() {
  if (!active()) return;
  // restore the snapshot
  if (maskCreated_)
    for (auto& s : snapshot_->slots)
      if (s.channel < 0) s.tex.reset();
  maskCreated_ = false;
  snapshot_->swap(*doc_);
  snapshot_.reset();
  pending_.clear();
  releaseLock();
  doc_ = nullptr;
}

// ------------------------------------------------------------------ stroke locks

std::vector<uint8_t> Painter::lockRegion(const Document& d, const std::vector<uint32_t>& seeds) const {
  std::vector<uint8_t> out = lockRegionNoSets(d, seeds);
  // (1.2) the face set lock: only the faces of the sets the stroke starts in (or, from a face in none, those in none)
  if (lockFaceSet && d.faceSetOf.size() == d.mesh.faceCount()) {
    std::vector<uint32_t> sets;
    for (uint32_t s : seeds)
      if (s < d.faceSetOf.size()) sets.push_back(d.faceSetOf[s]);
    for (uint32_t f = 0; f < out.size(); ++f)
      if (out[f] && std::find(sets.begin(), sets.end(), d.faceSetOf[f]) == sets.end()) out[f] = 0;
  }
  return out;
}

std::vector<uint8_t> Painter::lockRegionNoSets(const Document& d, const std::vector<uint32_t>& seeds) const {
  const Mesh& m = d.mesh;
  std::vector<uint8_t> out(m.faceCount(), 0);
  // set lock: the texture set when there are several, else the material slot
  auto setOf = [&](uint32_t f) -> int {
    if (m.tiles.active()) return m.faceTile(f);
    return f < m.faceMaterial.size() ? (int)m.faceMaterial[f] : 0;
  };
  const bool iso = isolateTile >= 0 && m.tiles.active() && isolateTile < m.tiles.count();
  if (!lockTopology && !lockIsland && !lockSet) {  // only the isolation (or a stale one: no limit)
    if (!iso) {
      std::fill(out.begin(), out.end(), 1);
      return out;
    }  // isolated: the shown texture set, wherever the stroke starts
    for (uint32_t f = 0; f < m.faceCount(); ++f) out[f] = m.faceTile(f) == isolateTile;
    return out;
  }
  for (uint32_t seed : seeds) {
    if (seed >= m.faceCount() || out[seed]) continue;
    int set = setOf(seed);
    if (!lockTopology && !lockIsland) {  // the set lock alone: every face of the set
      for (uint32_t f = 0; f < m.faceCount(); ++f)
        if (setOf(f) == set) out[f] = 1;
      continue;
    }
    // the UV island lies inside one connected piece, so with both locks on the island decides
    auto faces = facesForFill(const_cast<Document&>(d), seed, lockIsland ? FillMode::UVIsland : FillMode::Connected);
    for (uint32_t f : faces)
      if (!lockSet || setOf(f) == set) out[f] = 1;
  }
  if (iso)
    for (uint32_t f = 0; f < m.faceCount(); ++f)
      if (m.faceTile(f) != isolateTile) out[f] = 0;
  return out;
}

bool Painter::establishLock(const RayHit* hit, vec2 uv) {
  Document& d = *doc_;
  const Mesh& m = d.mesh;
  std::vector<uint32_t> seeds;
  if (hit) {
    seeds.push_back(hit->face);
    // symmetry copies paint the mirrored part, so lock onto that one as well
    if (anySymmetry() && !d.bvh.empty()) {
      int nr = std::clamp(radialCount, 1, 64);
      float tol = std::max(m.boundingRadius, 1e-4f) * 0.05f;
      for (int mm = 0; mm < 8; ++mm) {
        if (((mm & 1) && !symmetry[0]) || ((mm & 2) && !symmetry[1]) || ((mm & 4) && !symmetry[2])) continue;
        for (int r = 0; r < nr; ++r) {
          if (mm == 0 && r == 0) continue;
          vec3 c = hit->position;
          for (int ax = 0; ax < 3; ++ax)
            if ((mm >> ax) & 1) c[ax] = -c[ax];
          if (r) {
            vec3 axis(0.f);
            axis[std::clamp(radialAxis, 0, 2)] = 1.f;
            c = glm::mat3(glm::rotate(glm::mat4(1.f), 2.f * kPi * float(r) / float(nr), axis)) * c;
          }
          RayHit h = d.bvh.closestPoint(m, c, tol);
          if (h.hit) seeds.push_back(h.face);
        }
      }
    }
  } else {
    // UV-space stroke: the face whose UV triangle contains the point
    for (size_t t = 0; t < m.tris.size(); ++t) {
      if (pointInTriangle2(m.texUV(m.tris[t].x), m.texUV(m.tris[t].y), m.texUV(m.tris[t].z), uv)) {
        seeds.push_back(m.triFace[t]);
        break;
      }
    }
  }
  if (seeds.empty()) return false;
  d.paintLockFaces = lockRegion(d, seeds);
  d.updateSelectionFlags();
  lock_ = Lock::Active;
  return true;
}

void Painter::releaseLock() {
  bool had = lock_ == Lock::Active;
  lock_ = Lock::Off;
  if (doc_ && (had || !doc_->paintLockFaces.empty())) {
    doc_->paintLockFaces.clear();
    doc_->updateSelectionFlags();
  }
}

std::vector<uint32_t> Painter::facesForFill(Document& d, uint32_t face, FillMode mode) const {
  std::vector<uint32_t> out;
  const Mesh& m = d.mesh;
  if (face >= m.faceCount() && mode != FillMode::Mesh && mode != FillMode::Selection) return out;
  switch (mode) {
    case FillMode::Face: out.push_back(face); break;
    case FillMode::Mesh:
      for (uint32_t f = 0; f < m.faceCount(); ++f) out.push_back(f);
      break;
    case FillMode::Selection: out = d.selectedFaces(); break;
    case FillMode::Material: {
      uint32_t slot = face < m.faceMaterial.size() ? m.faceMaterial[face] : 0;
      for (uint32_t f = 0; f < m.faceCount(); ++f)
        if ((f < m.faceMaterial.size() ? m.faceMaterial[f] : 0) == slot) out.push_back(f);
      break;
    }
    case FillMode::UVIsland: {
      uint32_t isl = m.loopIsland[m.faceStart[face]];
      for (uint32_t f = 0; f < m.faceCount(); ++f)
        if (m.loopIsland[m.faceStart[f]] == isl) out.push_back(f);
      break;
    }
    case FillMode::Connected: {
      std::vector<std::vector<uint32_t>> edgeFaces(m.edges.size());
      for (uint32_t f = 0; f < m.faceCount(); ++f)
        for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) edgeFaces[m.loopEdge[l]].push_back(f);
      std::vector<uint8_t> seen(m.faceCount(), 0);
      std::queue<uint32_t> q;
      q.push(face);
      seen[face] = 1;
      while (!q.empty()) {
        uint32_t f = q.front();
        q.pop();
        out.push_back(f);
        for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l)
          for (uint32_t g : edgeFaces[m.loopEdge[l]])
            if (!seen[g]) seen[g] = 1, q.push(g);
      }
      break;
    }
  }
  return out;
}

bool Painter::fillFaces(Document& doc, MaterialLibrary& mats, const std::vector<uint32_t>& faces) {
  if (faces.empty() || !doc.hasMesh()) return false;
  if (active()) endStroke();
  init();
  doc_ = &doc;
  mats_ = &mats;
  uvSpace_ = false;
  stencilActive_ = false;
  if (!prepareTarget()) {
    doc_ = nullptr;
    return false;
  }
  // flag the faces, rasterize them into the stroke mask at full coverage
  std::vector<uint32_t> flags(doc.mesh.loopCount(), 0);
  for (uint32_t f = 0; f < doc.mesh.faceCount(); ++f)
    if (doc.faceSelection.size() > f && doc.faceSelection[f])
      for (uint32_t l = doc.mesh.faceStart[f]; l < doc.mesh.faceStart[f + 1]; ++l) flags[l] |= 1u;
  for (uint32_t f : faces)
    for (uint32_t l = doc.mesh.faceStart[f]; l < doc.mesh.faceStart[f + 1]; ++l) flags[l] |= kFillFlag;
  doc.gpu.updateFlags(flags);
  {
    ivec2 res = doc.tex.size();
    Framebuffer fbo;
    fbo.attach(0, strokeMask_);
    fbo.setDrawBuffers(1);
    gl::FramebufferScope fs(fbo.id());
    gl::ViewportScope vp(0, 0, res.x, res.y);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    Shader& s = ShaderCache::get("uvraster.vs.glsl", "paint_stamp.fs.glsl");
    s.set("uMode", 3);
    s.set("uFillFlag", kFillFlag);
    s.set("uLockFlag", 0u);
    s.set("uUVTransform", vec4(1, 1, 0, 0));
    s.set("uUseAuxAsUV", 0);
    s.use();
    doc.gpu.drawTriangles();
  }
  doc.updateSelectionFlags();
  applyStroke();
  pushUndo(tool == PaintTool::Erase ? (targetMask_ ? "Erase mask (fill)" : "Erase (fill)")
                                    : (targetMask_ ? "Fill mask" : "Polygon fill"));
  doc.dirty = true;
  doc_ = nullptr;
  return true;
}

}  // namespace patina
