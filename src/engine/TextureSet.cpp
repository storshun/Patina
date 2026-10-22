#include "engine/TextureSet.h"

#include "core/UVIsland.h"

#include "core/Progress.h"

#include <algorithm>
#include <functional>
#include <atomic>
#include <thread>

#include <algorithm>
#include <cmath>

#include "core/Bvh.h"
#include "core/Log.h"
#include "core/Mesh.h"
#include "engine/MaterialLibrary.h"
#include "gfx/GpuMesh.h"
#include "gfx/Shader.h"

namespace patina {

static void setGeneratorUniforms(Shader& s, const MaskGenerator& g, const AABB& bounds);

namespace {
struct RasterState {
  RasterState() {
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  }
};
}  // namespace

void TextureSet::allocate(int r) {
  res_ = r;
  posMap.create(r, r, GL_RGBA32F, 1);
  nrmMap.create(r, r, GL_RGBA16F, 1);
  tanMap.create(r, r, GL_RGBA16F, 1);
  seedMap.create(r, r, GL_RG32F, 1);  // nearest covered texel; x < 0 = none
  for (Texture2D* t : {&posMap, &nrmMap, &tanMap, &seedMap}) t->setFilter(GL_NEAREST, GL_NEAREST);
  aoMap.create(r, r, GL_RGBA8, 1);
  aoMap.clear(vec4(1));
  curvMap.create(r, r, GL_RGBA8, 1);
  curvMap.clear(vec4(0.5f, 0.5f, 0.5f, 1));
  thickMap.create(r, r, GL_RGBA8, 1);
  thickMap.clear(vec4(1));
  bentMap.create(r, r, GL_RGBA16F, 1);
  bentMap.clear(vec4(0, 0, 0, 1));
  matIdMap.create(r, r, GL_RGBA8, 1);
  matIdMap.setFilter(GL_NEAREST, GL_NEAREST);
  matIdMap.clear(vec4(0, 0, 0, 1));
  highNormal.create(r, r, GL_RGBA16F, 1);
  highNormal.clear(vec4(0, 0, 0, 0));
  hasAO_ = hasCurv_ = hasHighNormal_ = hasThick_ = hasBent_ = curvDetail_ = false;
  for (bool& on : bakedOn_) on = false;
  materialIdsFromHigh = false;
  makeAccumulators();
  finColor.create(r, r, GL_RGBA8, 0);
  finRMH.create(r, r, GL_RGBA16F, 0);
  finNormalDetail.create(r, r, GL_RGBA16F, 1);
  finEmissive.create(r, r, GL_RGBA8, 0);
  finNormal.create(r, r, GL_RGBA16F, 0);
  finExtra.create(r, r, GL_RGBA8, 0);
  for (Texture2D* t : {&finColor, &finRMH, &finEmissive, &finNormal, &finExtra}) {
    t->setWrap(GL_REPEAT, GL_REPEAT);
    if (GLAD_GL_EXT_texture_filter_anisotropic) glTextureParameterf(t->id(), GL_TEXTURE_MAX_ANISOTROPY_EXT, 8.f);
  }
  if (!white.valid()) {
    white.create(4, 4, GL_RGBA8, 1);
    white.clear(vec4(1));
    transparent.create(4, 4, GL_RGBA8, 1);
    transparent.clear(vec4(0));
    flatNormalSigned.create(4, 4, GL_RGBA16F, 1);
    flatNormalSigned.clear(vec4(0, 0, 1, 1));
  }
  PLOG_I("Texture set allocated at %dx%d", r, r);
}

void TextureSet::makeAccumulators() {
  const int r = res_;
  accColor.create(r, r, GL_RGBA8, 1);
  accRMH.create(r, r, GL_RGBA16F, 1);
  accNormal.create(r, r, GL_RGBA16F, 1);
  accEmissive.create(r, r, GL_RGBA8, 1);
  accExtra.create(r, r, GL_RGBA8, 1);
}

void TextureSet::sleep() {
  for (Texture2D* t : {&accColor, &accRMH, &accNormal, &accEmissive, &accExtra}) t->destroy();
  folderSnaps_.clear();
  for (auto& t : maskFx_) t.destroy();
  maskView_.destroy();
  scratch_.clear();
  seamMap.destroy();
  seamValid_ = false;
}

void TextureSet::rasterizeGeometry(GpuMesh& gpu, Texture2D& pos, Texture2D& nrm, Texture2D& tan) {
  if (!gpu.valid() || !pos.valid()) return;
  RasterState rs;
  Framebuffer fbo;
  fbo.attach(0, pos);
  fbo.attach(1, nrm);
  fbo.attach(2, tan);
  fbo.setDrawBuffers(3);
  gl::FramebufferScope fs(fbo.id());
  gl::ViewportScope vp(0, 0, pos.width(), pos.height());
  float zero[4] = {0, 0, 0, 0};
  for (int i = 0; i < 3; ++i) glClearBufferfv(GL_COLOR, i, zero);
  Shader& s = ShaderCache::get("uvraster.vs.glsl", "meshmaps.fs.glsl");
  s.set("uSkipFlag", GpuMesh::kMirrorCopyFlag);  // copies share the texels of the other half
  s.set("uUVTransform", vec4(1, 1, 0, 0));
  s.set("uUseAuxAsUV", 0);
  s.use();
  gpu.drawTriangles();
}

void TextureSet::bakeMeshMaps(const Mesh& mesh, GpuMesh& gpu) {
  if (!res_ || !gpu.valid()) return;
  rasterizeGeometry(gpu, posMap, nrmMap, tanMap);
  computeSeeds();
  seamValid_ = false;
  if (!materialIdsFromHigh) bakeMaterialIds(mesh, gpu);
}

void TextureSet::bakeMaterialIds(const Mesh& mesh, GpuMesh& gpu) {
  if (!res_ || !gpu.valid()) return;
  std::vector<vec4> aux(mesh.loopCount(), vec4(0));
  auto lf = mesh.loopFace();
  for (uint32_t l = 0; l < mesh.loopCount(); ++l) {
    uint32_t f = lf[l];
    float id = f < mesh.faceMaterial.size() ? float(std::min<uint32_t>(mesh.faceMaterial[f], 255)) : 0.f;
    aux[l] = vec4(id / 255.f, 0, 0, 1);
  }
  rasterizeAux(gpu, aux, matIdMap, vec4(0, 0, 0, 1));
  dilateInPlace(matIdMap);  // gutters and sliver triangles get the nearest island's slot
}

void TextureSet::rasterizeAux(GpuMesh& gpu, const std::vector<vec4>& aux, Texture2D& target, vec4 clear) {
  gpu.setAux(aux);
  RasterState rs;
  Framebuffer fbo;
  fbo.attach(0, target);
  gl::FramebufferScope fs(fbo.id());
  gl::ViewportScope vp(0, 0, target.width(), target.height());
  GLenum bufs[4] = {GL_NONE, GL_NONE, GL_NONE, GL_COLOR_ATTACHMENT0};
  glNamedFramebufferDrawBuffers(fbo.id(), 1, bufs + 3);
  glClearBufferfv(GL_COLOR, 0, &clear.x);
  glNamedFramebufferDrawBuffers(fbo.id(), 4, bufs);  // meshmaps.fs writes aux to output 3
  Shader& mm = ShaderCache::get("uvraster.vs.glsl", "meshmaps.fs.glsl");
  mm.set("uSkipFlag", GpuMesh::kMirrorCopyFlag);  // copies share the texels of the other half
  mm.set("uUVTransform", vec4(1, 1, 0, 0));
  mm.set("uUseAuxAsUV", 0);
  mm.use();
  gpu.drawTriangles();
}

void TextureSet::computeSeeds() {
  RasterState rs;
  Texture2D ping(res_, res_, GL_RG32F, 1);
  ping.setFilter(GL_NEAREST, GL_NEAREST);
  Shader& s = ShaderCache::get("fullscreen.vs.glsl", "jfa.fs.glsl");
  Framebuffer fbo;
  gl::ViewportScope vp(0, 0, res_, res_);
  Texture2D* a = &seedMap;
  Texture2D* b = &ping;
  // init
  fbo.attach(0, *a);
  fbo.setDrawBuffers(1);
  {
    gl::FramebufferScope fs(fbo.id());
    s.tex("uCoverage", 0, posMap.id());
    s.tex("uPrev", 1, posMap.id());
    s.set("uInit", 1);
    s.use();
    drawFullscreenTriangle();
  }
  s.set("uInit", 0);
  int step = res_ / 2;
  std::vector<int> steps;
  while (step >= 1) {
    steps.push_back(step);
    step /= 2;
  }
  steps.push_back(1);  // JFA+1 for accuracy
  for (int st : steps) {
    fbo.attach(0, *b);
    gl::FramebufferScope fs(fbo.id());
    s.tex("uPrev", 1, a->id());
    s.set("uStep", st);
    s.use();
    drawFullscreenTriangle();
    std::swap(a, b);
  }
  if (a != &seedMap) seedMap.copyFrom(*a);
}

const Texture2D& TextureSet::ensureSeamMap(const Mesh& mesh, const Bvh& bvh) {
  if (seamValid_ && seamMap.valid() && seamMap.width() == res_) return seamMap;
  const int r = res_;
  if (!seamMap.valid() || seamMap.width() != r) {
    seamMap.create(r, r, GL_RG32F, 1);
    seamMap.setFilter(GL_NEAREST, GL_NEAREST);
  }
  seamValid_ = true;
  double t0 = nowSeconds();
  std::vector<vec2> out(size_t(r) * r, vec2(-1.f));
  if (bvh.empty() || !r) {
    seamMap.uploadRaw(GL_RG, GL_FLOAT, out.data());
    return seamMap;
  }
  std::vector<vec4> pos(size_t(r) * r), nrm(size_t(r) * r);
  std::vector<vec2> seed(size_t(r) * r);
  posMap.downloadRaw(GL_RGBA, GL_FLOAT, pos.data(), pos.size() * sizeof(vec4));
  nrmMap.downloadRaw(GL_RGBA, GL_FLOAT, nrm.data(), nrm.size() * sizeof(vec4));
  seedMap.downloadRaw(GL_RG, GL_FLOAT, seed.data(), seed.size() * sizeof(vec2));
  const int R = std::max(16, r / 64);  // reach of the map beyond the island edge, texels
  auto covered = [&](int x, int y) { return x >= 0 && y >= 0 && x < r && y < r && pos[size_t(y) * r + x].w > 0.5f; };
  auto P = [&](int x, int y) { return vec3(pos[size_t(y) * r + x]); };
  // surface derivative per texel step at a covered texel (central difference where possible)
  auto deriv = [&](int x, int y, int dx, int dy, vec3& d) {
    bool a = covered(x + dx, y + dy), b = covered(x - dx, y - dy);
    if (a && b) d = (P(x + dx, y + dy) - P(x - dx, y - dy)) * 0.5f;
    else if (a) d = P(x + dx, y + dy) - P(x, y);
    else if (b) d = P(x, y) - P(x - dx, y - dy);
    else return false;
    return true;
  };
  unsigned nt = std::max(1u, std::min(16u, std::thread::hardware_concurrency()));
  std::vector<std::thread> th;
  for (unsigned t = 0; t < nt; ++t)
    th.emplace_back([&, t] {
      for (int y = int(t); y < r; y += int(nt))
        for (int x = 0; x < r; ++x) {
          size_t i = size_t(y) * r + x;
          if (pos[i].w > 0.5f || seed[i].x < 0.f) continue;
          int cx = int(seed[i].x), cy = int(seed[i].y);
          float gx = float(x - cx), gy = float(y - cy);
          float dist = std::sqrt(gx * gx + gy * gy);
          if (dist > float(R) || !covered(cx, cy)) continue;
          vec3 dPx, dPy;
          if (!deriv(cx, cy, 1, 0, dPx) || !deriv(cx, cy, 0, 1, dPy)) continue;
          vec3 c = P(cx, cy);
          vec3 Q = c + dPx * gx + dPy * gy;
          float step = std::max(glm::length(dPx), glm::length(dPy));
          float L = glm::length(Q - c);
          if (L < 1e-9f) continue;
          RayHit h = bvh.closestPoint(mesh, Q, L);
          if (!h.hit) continue;
          // the surface carries on there (a seam), not just the island's own open edge
          if (h.t > 0.5f * L + 0.25f * step) continue;
          vec2 ht = h.uv * float(r);
          if (glm::length(ht - vec2(cx + 0.5f, cy + 0.5f)) < 2.5f) continue;
          vec3 n0 = vec3(nrm[size_t(cy) * r + cx]);
          auto tri = mesh.tris[h.tri];
          vec3 fn = glm::cross(mesh.positions[mesh.loopVert[tri[1]]] - mesh.positions[mesh.loopVert[tri[0]]],
                               mesh.positions[mesh.loopVert[tri[2]]] - mesh.positions[mesh.loopVert[tri[0]]]);
          if (glm::dot(n0, n0) > 0 && glm::dot(fn, fn) > 0 && glm::dot(glm::normalize(n0), glm::normalize(fn)) < 0.2f)
            continue;  // the other side of a thin shell
          out[i] = h.uv;
        }
    });
  for (auto& t : th) t.join();
  seamMap.uploadRaw(GL_RG, GL_FLOAT, out.data());
  size_t n = 0;
  for (auto& v : out) n += v.x >= 0.f;
  PLOG_I("Seam map: %zu gutter texels reach across seams (%.0f ms)", n, (nowSeconds() - t0) * 1000.0);
  return seamMap;
}

Image TextureSet::layerMaskImage(const Layer& L, const AABB& bounds) { return layerMaskTexture(L, bounds).download(1); }

const Texture2D& TextureSet::layerMaskTexture(const Layer& L, const AABB& bounds) {
  if (!maskView_.valid() || maskView_.width() != res_) {
    maskView_.create(res_, res_, GL_RGBA16F, 1);
    maskView_.setFilter(GL_LINEAR, GL_LINEAR);
  }
  Texture2D& out = maskView_;
  if (!L.maskEffects.empty()) {
    resampleRegion(evalMaskStack(L, bounds), out, vec2(0), vec2(1));
  } else {
    RasterState rs;
    Framebuffer fbo;
    fbo.attach(0, out);
    fbo.setDrawBuffers(1);
    gl::FramebufferScope fs(fbo.id());
    gl::ViewportScope vp(0, 0, res_, res_);
    Shader& s = ShaderCache::get("fullscreen.vs.glsl", "mask_final.fs.glsl");
    s.tex("uLayerMask", 4, L.mask ? L.mask->id() : white.id());
    s.tex("uPosMap", 5, posMap.id());
    s.tex("uNrmMap", 6, nrmMap.id());
    s.tex("uTanMap", 7, tanMap.id());
    s.tex("uAOMap", 8, aoView().id());
    s.tex("uCurvMap", 9, curvView().id());
    s.tex("uMatIdMap", 14, matIdMap.id());
    s.tex("uThickMap", 15, thickView().id());
    setGeneratorUniforms(s, L.generator, bounds);
    s.use();
    drawFullscreenTriangle();
  }
  return out;
}

void TextureSet::resampleRegion(const Texture2D& src, Texture2D& dst, vec2 uv0, vec2 uvSize, bool nearest) {
  if (!src.valid() || !dst.valid()) return;
  RasterState rs;
  Framebuffer fbo;
  fbo.attach(0, dst);
  fbo.setDrawBuffers(1);
  gl::FramebufferScope fs(fbo.id());
  gl::ViewportScope vp(0, 0, dst.width(), dst.height());
  Shader& s = ShaderCache::get("fullscreen.vs.glsl", "post.fs.glsl", {"POST_REGION"});
  s.tex("uSrc", 0, src.id());
  s.set("uRegion", vec4(uv0, uvSize));
  s.set("uNearest", nearest ? 1 : 0);
  s.use();
  drawFullscreenTriangle();
}

void TextureSet::setupTile(const TextureSet& full, const Mesh& mesh, GpuMesh& gpu, vec2 uv0, vec2 uvSize, int fullRes) {
  if (!res_ || !gpu.valid()) return;
  {
    RasterState rs;
    Framebuffer fbo;
    fbo.attach(0, posMap);
    fbo.attach(1, nrmMap);
    fbo.attach(2, tanMap);
    fbo.setDrawBuffers(3);
    gl::FramebufferScope fs(fbo.id());
    gl::ViewportScope vp(0, 0, res_, res_);
    float zero[4] = {0, 0, 0, 0};
    for (int i = 0; i < 3; ++i) glClearBufferfv(GL_COLOR, i, zero);
    Shader& s = ShaderCache::get("uvraster.vs.glsl", "meshmaps.fs.glsl");
    s.set("uSkipFlag", GpuMesh::kMirrorCopyFlag);  // copies share the texels of the other half
    // raster position = (uv - uv0) / size
    s.set("uUVTransform", vec4(1.f / uvSize.x, 1.f / uvSize.y, -uv0.x / uvSize.x, -uv0.y / uvSize.y));
    s.set("uUseAuxAsUV", 0);
    s.use();
    gpu.drawTriangles();
    s.set("uUVTransform", vec4(1, 1, 0, 0));
  }
  computeSeeds();
  resampleRegion(full.aoMap, aoMap, uv0, uvSize);
  resampleRegion(full.curvMap, curvMap, uv0, uvSize);
  resampleRegion(full.thickMap, thickMap, uv0, uvSize);
  resampleRegion(full.bentMap, bentMap, uv0, uvSize);
  resampleRegion(full.highNormal, highNormal, uv0, uvSize);
  resampleRegion(full.matIdMap, matIdMap, uv0, uvSize, true);
  hasAO_ = full.hasAO_, hasCurv_ = full.hasCurv_, hasHighNormal_ = full.hasHighNormal_;
  materials = full.materials;
  hasThick_ = full.hasThick_, hasBent_ = full.hasBent_, curvDetail_ = full.curvDetail_;
  bakedLayersOn = full.bakedLayersOn;
  materialIdsFromHigh = full.materialIdsFromHigh;
  base = full.base;
  baseNormal = full.baseNormal;
  heightNormalStrength = full.heightNormalStrength;
  dilate = full.dilate;
  effectiveRes_ = fullRes;
  tileUV_ = vec4(uv0, uvSize);
  (void)mesh;
}

void TextureSet::dilateInPlace(Texture2D& t) {
  Texture2D tmp(t.width(), t.height(), t.format(), 1);
  dilateInto(t, tmp);
  t.copyFrom(tmp);
}

void TextureSet::dilateInto(const Texture2D& src, Texture2D& dst) {
  RasterState rs;
  Framebuffer fbo;
  fbo.attach(0, dst);
  fbo.setDrawBuffers(1);
  gl::FramebufferScope fs(fbo.id());
  gl::ViewportScope vp(0, 0, dst.width(), dst.height());
  Shader& s = ShaderCache::get("fullscreen.vs.glsl", "post.fs.glsl", {"POST_DILATE"});
  s.tex("uSrc", 0, src.id());
  s.tex("uSeed", 1, seedMap.id());
  s.use();
  drawFullscreenTriangle();
}

Texture2D& TextureSet::scratch(int w, int h, GLenum fmt) {
  for (auto& t : scratch_)
    if (t->width() == w && t->height() == h && t->format() == fmt) return *t;
  scratch_.push_back(std::make_unique<Texture2D>(w, h, fmt, 1));
  return *scratch_.back();
}

void TextureSet::bakeAO(const Mesh& mesh, GpuMesh& gpu, int samples, float strength, GpuMesh* occluder,
                        const Texture2D* posTex, const Texture2D* nrmTex, const AABB* occluderBounds) {
  if (!res_ || !gpu.valid()) return;
  AABB b = mesh.bounds;
  if (occluderBounds) b.add(occluderBounds->lo), b.add(occluderBounds->hi);
  GpuMesh& occ = occluder && occluder->valid() ? *occluder : gpu;
  bool useTex = posTex && nrmTex;
  vec3 c = b.center();
  float r = std::max(b.radius(), 1e-4f);
  const int S = 1024;
  Texture2D shadow(S, S, GL_DEPTH_COMPONENT32F, 1);
  shadow.setFilter(GL_NEAREST, GL_NEAREST);
  Framebuffer sfbo;
  sfbo.attachDepth(shadow);
  glNamedFramebufferDrawBuffer(sfbo.id(), GL_NONE);
  Texture2D accum(res_, res_, GL_RGBA32F, 1), bentAccum(res_, res_, GL_RGBA32F, 1);
  accum.clear(vec4(0));
  bentAccum.clear(vec4(0));
  Framebuffer afbo;
  afbo.attach(0, accum);
  afbo.attach(1, bentAccum);
  afbo.setDrawBuffers(2);
  Shader& depth = ShaderCache::get("mesh.vs.glsl", "depth.fs.glsl");
  Shader& bake = ShaderCache::get("uvraster.vs.glsl", "bake.fs.glsl", {"BAKE_AO"});
  bake.set("uSkipFlag", GpuMesh::kMirrorCopyFlag);
  progress::Scope prog("Baking ambient occlusion");
  for (int i = 0; i < samples; ++i) {
    progress::set(float(i) / float(samples));
    if ((i & 15) == 15) glFinish();  // keeps progress honest and lets a cancel land
    if (progress::cancelled()) {
      PLOG_W("Ambient occlusion bake canceled");
      return;
    }
    // Fibonacci sphere direction
    float t = (i + 0.5f) / samples;
    float z = 1.f - 2.f * t;
    float rad = std::sqrt(std::max(0.f, 1.f - z * z));
    float phi = i * 2.399963229728653f;
    vec3 dir(rad * std::cos(phi), z, rad * std::sin(phi));
    vec3 up = std::abs(dir.y) < 0.99f ? vec3(0, 1, 0) : vec3(1, 0, 0);
    mat4 view = glm::lookAt(c + dir * r * 2.f, c, up);
    mat4 proj = glm::ortho(-r, r, -r, r, 0.f, r * 4.f);
    mat4 vp = proj * view;
    {
      gl::FramebufferScope fs(sfbo.id());
      gl::ViewportScope v(0, 0, S, S);
      glEnable(GL_DEPTH_TEST);
      glDepthFunc(GL_LESS);
      glDepthMask(GL_TRUE);
      glDisable(GL_CULL_FACE);
      glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
      glClear(GL_DEPTH_BUFFER_BIT);
      depth.set("uModel", mat4(1));
      depth.set("uViewProj", vp);
      depth.use();
      occ.drawTriangles();
      glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
      glDisable(GL_DEPTH_TEST);
    }
    {
      gl::FramebufferScope fs(afbo.id());
      gl::ViewportScope v(0, 0, res_, res_);
      glEnable(GL_BLEND);
      glBlendFunc(GL_ONE, GL_ONE);
      bake.tex("uShadow", 0, shadow.id());
      bake.set("uLightVP", vp);
      bake.set("uDir", dir);
      bake.set("uBias", r * 0.004f);
      bake.set("uUVTransform", vec4(1, 1, 0, 0));
      bake.set("uUseAuxAsUV", 0);
      bake.set("uUseSurfaceTex", useTex ? 1 : 0);
      if (useTex) {
        bake.tex("uSurfPos", 1, posTex->id());
        bake.tex("uSurfNrm", 2, nrmTex->id());
      }
      bake.use();
      gpu.drawTriangles();
      glDisable(GL_BLEND);
    }
  }
  {
    RasterState rs;
    Framebuffer fbo;
    fbo.attach(0, aoMap);
    fbo.setDrawBuffers(1);
    gl::FramebufferScope fs(fbo.id());
    gl::ViewportScope v(0, 0, res_, res_);
    Shader& s = ShaderCache::get("fullscreen.vs.glsl", "post.fs.glsl", {"POST_AO_RESOLVE"});
    s.tex("uAccum", 0, accum.id());
    s.set("uStrength", strength);
    s.use();
    drawFullscreenTriangle();
  }
  dilateInPlace(aoMap);
  hasAO_ = true;
  ++bakeSerial_;
  // bent normals: the average open direction, the normal where nothing is open
  {
    size_t n = size_t(res_) * res_;
    std::vector<vec4> sum(n), nrm(n), out(n);
    bentAccum.downloadRaw(GL_RGBA, GL_FLOAT, sum.data(), n * sizeof(vec4));
    const Texture2D& nsrc = useTex ? *nrmTex : nrmMap;
    nsrc.downloadRaw(GL_RGBA, GL_FLOAT, nrm.data(), n * sizeof(vec4));
    for (size_t i = 0; i < n; ++i) {
      vec3 N = vec3(nrm[i]);
      float ln = glm::length(N);
      N = ln > 1e-6f ? N / ln : vec3(0, 0, 1);
      vec3 b = vec3(sum[i]);
      float lb = glm::length(b);
      vec3 B = lb > 1e-6f && sum[i].w > 0.5f ? b / lb : N;
      out[i] = vec4(B, 1.f);
    }
    bentMap.uploadRaw(GL_RGBA, GL_FLOAT, out.data());
    dilateInPlace(bentMap);
    hasBent_ = true;
  }
  bakedUV[0] = uv::uvHash(mesh);
  PLOG_I("Baked ambient occlusion and bent normals (%d directions)", samples);
}

bool TextureSet::bakeCurvatureRays(const Mesh& mesh, const Bvh& bvh, Texture2D& out, float strength, int rays,
                                   float radius) {
  // at most 2048 texels across; larger sets get the result resampled (curvature is smooth)
  const int R = std::min(res_, 1024);
  const int step = res_ / R;
  size_t full = size_t(res_) * res_;
  std::vector<vec4> pos(full), nrm(full);
  posMap.downloadRaw(GL_RGBA, GL_FLOAT, pos.data(), full * sizeof(vec4));
  nrmMap.downloadRaw(GL_RGBA, GL_FLOAT, nrm.data(), full * sizeof(vec4));
  float br = std::max(mesh.boundingRadius, 1e-4f);
  float maxD = radius > 0 ? radius : br * 0.03f;
  float eps = br * 1e-4f;
  rays = std::clamp(rays, 4, 256);
  std::vector<vec3> dirs(rays);
  for (int i = 0; i < rays; ++i) {
    float u = (i + 0.5f) / rays, phi = i * 2.399963229728653f;
    float rr = std::sqrt(u);
    dirs[i] = vec3(rr * std::cos(phi), rr * std::sin(phi), std::sqrt(std::max(0.f, 1.f - u)));
  }
  const float gain = 4.f * strength;
  // the smooth (per-vertex) part is already in `out`: the ray pass adds the edges and creases
  std::vector<uint8_t> base(full * 4);
  out.downloadRaw(GL_RGBA, GL_UNSIGNED_BYTE, base.data(), base.size());
  std::vector<uint8_t> px(size_t(R) * R * 4, 255);
  for (int y = 0; y < R; ++y)
    for (int x = 0; x < R; ++x) {
      size_t o4 = (size_t(y) * R + x) * 4, s4 = (size_t(y * step) * res_ + size_t(x * step)) * 4;
      px[o4] = px[o4 + 1] = px[o4 + 2] = base[s4];
    }
  std::atomic<int> rowsDone{0};
  std::atomic<bool> stop{false};
  progress::Scope prog("Baking curvature");
  unsigned nt = std::max(1u, std::min(32u, std::thread::hardware_concurrency()));
  std::vector<std::thread> th;
  for (unsigned t = 0; t < nt; ++t)
    th.emplace_back([&, t] {
      for (int y = int(t); y < R && !stop; y += int(nt)) {
        for (int x = 0; x < R; ++x) {
          size_t si = size_t(y * step) * res_ + size_t(x * step);
          if (pos[si].w < 0.5f) continue;
          vec3 N = vec3(nrm[si]);
          float ln = glm::length(N);
          if (ln < 1e-6f) continue;
          N /= ln;
          vec3 tA = std::abs(N.x) < 0.9f ? glm::normalize(glm::cross(N, vec3(1, 0, 0)))
                                         : glm::normalize(glm::cross(N, vec3(0, 1, 0)));
          vec3 tB = glm::cross(N, tA);
          float rot = float((uint32_t(x) * 73856093u) ^ (uint32_t(y) * 19349663u)) * (6.2831853f / 4294967296.f);
          float cr = std::cos(rot), sr = std::sin(rot);
          vec3 P = vec3(pos[si]);
          float convex = 0.f, concave = 0.f;
          for (int side = 0; side < 2; ++side) {
            float sgn = side == 0 ? -1.f : 1.f;  // 0: inside (convexity), 1: outside (concavity)
            vec3 o = P + N * (eps * sgn);
            float sum = 0.f;
            for (const vec3& d0 : dirs) {
              vec2 xy(d0.x * cr - d0.y * sr, d0.x * sr + d0.y * cr);
              vec3 d = tA * xy.x + tB * xy.y + N * (d0.z * sgn);
              RayHit h = bvh.raycast(mesh, o, d, maxD);
              if (!h.hit || h.t >= maxD) continue;
              vec3 fn = glm::length(h.faceNormal) > 1e-8f ? glm::normalize(h.faceNormal) : N;
              float c = glm::dot(fn, N);
              sum += (1.f - c * c) * (1.f - h.t / maxD);
            }
            (side == 0 ? convex : concave) = sum / float(rays);
          }
          size_t o4 = (size_t(y) * R + x) * 4;
          float v = std::clamp(px[o4] / 255.f + 0.5f * gain * (convex - concave), 0.f, 1.f);
          px[o4] = px[o4 + 1] = px[o4 + 2] = (uint8_t)std::lround(v * 255.f);
        }
        int done = ++rowsDone;
        if (t == 0) {
          progress::set(float(done) / float(R));
          if (progress::cancelled()) stop = true;
        }
      }
    });
  for (auto& t : th) t.join();
  if (stop) {
    PLOG_W("Curvature bake canceled");
    return false;
  }
  if (R == res_) {
    out.uploadRaw(GL_RGBA, GL_UNSIGNED_BYTE, px.data());
  } else {
    Texture2D small(R, R, GL_RGBA8, 1);
    small.uploadRaw(GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    small.setFilter(GL_LINEAR, GL_LINEAR);
    resampleRegion(small, out, vec2(0), vec2(1));
  }
  return true;
}

void TextureSet::bakeCurvatureVertices(const Mesh& m, GpuMesh& gpu, Texture2D& tmp, float strength, bool smoothOnly) {
  size_t V = m.positions.size();
  std::vector<vec3> vn(V, vec3(0));
  for (uint32_t l = 0; l < m.loopCount(); ++l) vn[m.loopVert[l]] += m.loopNormal[l];
  for (auto& n : vn) n = glm::length(n) > 1e-12f ? glm::normalize(n) : vec3(0, 1, 0);
  std::vector<float> k(V, 0.f), w(V, 0.f);
  for (auto& e : m.edges) {
    vec3 d = m.positions[e.y] - m.positions[e.x];
    float l2 = glm::dot(d, d);
    if (l2 < 1e-20f) continue;
    // hard edges (a low-poly box: normals 30+ degrees apart) are left to the ray pass; here they
    // would spread over whole faces
    if (smoothOnly && glm::dot(vn[e.x], vn[e.y]) < 0.866f) continue;
    float c = glm::dot(vn[e.y] - vn[e.x], d) / l2;  // > 0 convex
    float len = std::sqrt(l2);
    k[e.x] += c * len, w[e.x] += len;
    k[e.y] += c * len, w[e.y] += len;
  }
  std::vector<float> absk;
  for (size_t i = 0; i < V; ++i) {
    if (w[i] > 0) k[i] /= w[i];
    absk.push_back(std::abs(k[i]));
  }
  float scale = 1.f;
  if (!absk.empty()) {
    std::nth_element(absk.begin(), absk.begin() + absk.size() * 9 / 10, absk.end());
    scale = std::max(absk[absk.size() * 9 / 10], 1e-6f);
  }
  // never amplify gentle, even curvature (a sphere) to full strength
  if (smoothOnly) scale = std::max(scale, 8.f / std::max(m.boundingRadius, 1e-4f));
  std::vector<vec4> aux(m.loopCount());
  for (uint32_t l = 0; l < m.loopCount(); ++l) {
    float v = std::clamp(0.5f + 0.5f * k[m.loopVert[l]] / scale * strength, 0.f, 1.f);
    aux[l] = vec4(v, 0, 0, 0);
  }
  gpu.setAux(aux);
  {
    Framebuffer fbo;
    fbo.attach(0, tmp);
    fbo.setDrawBuffers(1);
    gl::FramebufferScope fs(fbo.id());
    gl::ViewportScope vp(0, 0, res_, res_);
    // meshmaps.fs writes the aux attribute to output 3; route only that output to our texture
    Shader& mm = ShaderCache::get("uvraster.vs.glsl", "meshmaps.fs.glsl");
    mm.set("uSkipFlag", GpuMesh::kMirrorCopyFlag);  // copies share the texels of the other half
    GLenum bufs[4] = {GL_NONE, GL_NONE, GL_NONE, GL_COLOR_ATTACHMENT0};
    glNamedFramebufferDrawBuffers(fbo.id(), 4, bufs);
    mm.set("uUVTransform", vec4(1, 1, 0, 0));
    mm.set("uUseAuxAsUV", 0);
    mm.use();
    gpu.drawTriangles();
  }
}

// Curvature. With a BVH (the usual case) it is measured per texel, at a scale that does not
// depend on how the mesh is tessellated: short rays from just inside the surface that reach the
// surface again within `radius` mean a convex edge nearby; short rays from just outside mean a
// crease. Hits on surfaces facing the same way (a smooth sphere) or the opposite way (the far
// side of a thin plate) count for little, so flat faces, smooth large shapes and thin parts read
// as neutral gray, and a low-poly box gets thin bright edges instead of face-wide gradients.
// Without a BVH: per-vertex curvature interpolated across faces (the 0.6 method).
void TextureSet::bakeCurvature(const Mesh& m, GpuMesh& gpu, const Bvh* bvh, float strength, bool detail, int rays,
                               float radius) {
  if (!res_ || !gpu.valid()) return;
  RasterState rs;
  Texture2D tmp(res_, res_, GL_RGBA8, 1);
  tmp.clear(vec4(0.5f, 0.5f, 0.5f, 1));
  bool useRays = bvh && !bvh->empty();
  bakeCurvatureVertices(m, gpu, tmp, strength, useRays);
  if (useRays && !bakeCurvatureRays(m, *bvh, tmp, strength, rays, radius)) return;
  dilateInPlace(tmp);  // island borders are often the mesh edges: keep their value through the blur
  // light blur then dilate
  Texture2D tmp2(res_, res_, GL_RGBA8, 1);
  Shader& blur = ShaderCache::get("fullscreen.vs.glsl", "post.fs.glsl", {"POST_BLUR"});
  for (int pass = 0; pass < 2; ++pass) {
    Framebuffer fbo;
    Texture2D& src = pass == 0 ? tmp : tmp2;
    Texture2D& dst = pass == 0 ? tmp2 : curvMap;
    fbo.attach(0, dst);
    fbo.setDrawBuffers(1);
    gl::FramebufferScope fs(fbo.id());
    gl::ViewportScope vp(0, 0, res_, res_);
    blur.tex("uSrc", 0, src.id());
    blur.set("uDirPx", pass == 0 ? vec2(1, 0) : vec2(0, 1));
    blur.use();
    drawFullscreenTriangle();
  }
  curvDetail_ = false;
  if (detail && hasHighNormal_) addNormalMapCurvature(strength);
  dilateInPlace(curvMap);
  hasCurv_ = true;
  ++bakeSerial_;
  bakedUV[1] = uv::uvHash(m);
  PLOG_I("Baked curvature%s", curvDetail_ ? " (with the high-poly normal map's detail)" : "");
}

// Curvature of the baked normal map: how fast the normal turns, per unit of surface, from
// neighboring texels (the divergence of the normal field). Added onto the mesh curvature.
void TextureSet::addNormalMapCurvature(float strength) {
  const int r = res_;
  size_t n = size_t(r) * r;
  std::vector<vec4> pos(n), hn(n);
  std::vector<uint8_t> cur(n * 4);
  posMap.downloadRaw(GL_RGBA, GL_FLOAT, pos.data(), n * sizeof(vec4));
  highNormal.downloadRaw(GL_RGBA, GL_FLOAT, hn.data(), n * sizeof(vec4));
  curvMap.downloadRaw(GL_RGBA, GL_UNSIGNED_BYTE, cur.data(), cur.size());
  auto ok = [&](int x, int y) {
    if (x < 0 || y < 0 || x >= r || y >= r) return false;
    size_t i = size_t(y) * r + x;
    return pos[i].w > 0.5f && glm::dot(vec3(hn[i]), vec3(hn[i])) > 0.25f;
  };
  std::vector<float> k(n, 0.f);
  std::vector<uint8_t> has(n, 0);
  unsigned nt = std::max(1u, std::min(16u, std::thread::hardware_concurrency()));
  std::vector<std::thread> th;
  for (unsigned t = 0; t < nt; ++t)
    th.emplace_back([&, t] {
      for (int y = int(t); y < r; y += int(nt))
        for (int x = 0; x < r; ++x) {
          if (!ok(x, y)) continue;
          float sum = 0.f;
          int axes = 0;
          for (int a = 0; a < 2; ++a) {
            int dx = a == 0, dy = a == 1;
            int x0 = x - dx, y0 = y - dy, x1 = x + dx, y1 = y + dy;
            if (!ok(x1, y1)) x1 = x, y1 = y;
            if (!ok(x0, y0)) x0 = x, y0 = y;
            if (x0 == x1 && y0 == y1) continue;
            size_t i0 = size_t(y0) * r + x0, i1 = size_t(y1) * r + x1;
            vec3 dP = vec3(pos[i1]) - vec3(pos[i0]);
            float l2 = glm::dot(dP, dP);
            if (l2 < 1e-20f) continue;
            vec3 dN = glm::normalize(vec3(hn[i1])) - glm::normalize(vec3(hn[i0]));
            sum += glm::dot(dN, dP) / l2;  // > 0 where the surface bulges (convex)
            ++axes;
          }
          if (!axes) continue;
          k[size_t(y) * r + x] = sum / float(axes);
          has[size_t(y) * r + x] = 1;
        }
    });
  for (auto& t : th) t.join();
  // scale by the 90th percentile, like the mesh curvature
  std::vector<float> a;
  a.reserve(n / 4);
  for (size_t i = 0; i < n; i += 3)
    if (has[i]) a.push_back(std::abs(k[i]));
  if (a.empty()) return;
  std::nth_element(a.begin(), a.begin() + a.size() * 9 / 10, a.end());
  float scale = std::max(a[a.size() * 9 / 10], 1e-6f);
  for (size_t i = 0; i < n; ++i) {
    if (!has[i]) continue;
    float base = cur[i * 4] / 255.f;
    float v = std::clamp(base + 0.5f * k[i] / scale * strength, 0.f, 1.f);
    uint8_t b = (uint8_t)std::lround(v * 255.f);
    cur[i * 4] = cur[i * 4 + 1] = cur[i * 4 + 2] = b;
  }
  curvMap.uploadRaw(GL_RGBA, GL_UNSIGNED_BYTE, cur.data());
  curvDetail_ = true;
}

void TextureSet::bakeThickness(const Mesh& mesh, const Bvh& bvh, int rays, float distance) {
  if (!res_ || bvh.empty()) return;
  const int r = res_;
  size_t n = size_t(r) * r;
  std::vector<vec4> pos(n), nrm(n);
  posMap.downloadRaw(GL_RGBA, GL_FLOAT, pos.data(), n * sizeof(vec4));
  nrmMap.downloadRaw(GL_RGBA, GL_FLOAT, nrm.data(), n * sizeof(vec4));
  float maxD = distance > 0 ? distance : std::max(mesh.boundingRadius, 1e-4f) * 0.5f;
  float eps = std::max(mesh.boundingRadius, 1e-4f) * 1e-4f;
  rays = std::clamp(rays, 4, 512);
  // cosine-weighted directions around +Z (rotated onto -N per texel), the same set everywhere
  std::vector<vec3> dirs(rays);
  for (int i = 0; i < rays; ++i) {
    float u = (i + 0.5f) / rays, phi = i * 2.399963229728653f;
    float rr = std::sqrt(u);
    dirs[i] = vec3(rr * std::cos(phi), rr * std::sin(phi), std::sqrt(std::max(0.f, 1.f - u)));
  }
  std::vector<uint8_t> out(n * 4, 255);
  std::atomic<int> rowsDone{0};
  std::atomic<bool> stop{false};
  progress::Scope prog("Baking thickness");
  unsigned nt = std::max(1u, std::min(32u, std::thread::hardware_concurrency()));
  std::vector<std::thread> th;
  for (unsigned t = 0; t < nt; ++t)
    th.emplace_back([&, t] {
      for (int y = int(t); y < r && !stop; y += int(nt)) {
        for (int x = 0; x < r; ++x) {
          size_t i = size_t(y) * r + x;
          if (pos[i].w < 0.5f) continue;
          vec3 N = vec3(nrm[i]);
          float ln = glm::length(N);
          if (ln < 1e-6f) continue;
          N /= ln;
          vec3 in = -N;
          vec3 tA = std::abs(in.x) < 0.9f ? glm::normalize(glm::cross(in, vec3(1, 0, 0)))
                                          : glm::normalize(glm::cross(in, vec3(0, 1, 0)));
          vec3 tB = glm::cross(in, tA);
          // a per-texel rotation of the set hides the pattern
          float rot = float((x * 73856093u) ^ (y * 19349663u)) * (6.2831853f / 4294967296.f);
          float cr = std::cos(rot), sr = std::sin(rot);
          vec3 o = vec3(pos[i]) - N * eps;
          float sum = 0.f;
          for (const vec3& d0 : dirs) {
            vec2 xy(d0.x * cr - d0.y * sr, d0.x * sr + d0.y * cr);
            vec3 d = tA * xy.x + tB * xy.y + in * d0.z;
            RayHit h = bvh.raycast(mesh, o, d, maxD);
            sum += h.hit ? std::min(h.t, maxD) / maxD : 1.f;
          }
          uint8_t v = (uint8_t)std::lround(std::clamp(sum / float(rays), 0.f, 1.f) * 255.f);
          out[i * 4] = out[i * 4 + 1] = out[i * 4 + 2] = v;
        }
        int done = ++rowsDone;
        if (t == 0) {
          progress::set(float(done) / float(r));
          if (progress::cancelled()) stop = true;
        }
      }
    });
  for (auto& t : th) t.join();
  if (stop) {
    PLOG_W("Thickness bake canceled");
    return;
  }
  thickMap.uploadRaw(GL_RGBA, GL_UNSIGNED_BYTE, out.data());
  dilateInPlace(thickMap);
  hasThick_ = true;
  ++bakeSerial_;
  bakedUV[2] = uv::uvHash(mesh);
  PLOG_I("Baked thickness (%d rays per texel, up to %.3g units deep)", rays, maxD);
}

static void setGeneratorUniforms(Shader& s, const MaskGenerator& g, const AABB& bounds) {
  s.set("uGenType", g.type);
  s.set("uGenParams", vec4(g.level, g.contrast, g.noise, g.noiseScale));
  s.set("uGenInvert", g.invert ? 1 : 0);
  s.set("uGenBounds", vec4(bounds.lo.y, bounds.hi.y, bounds.radius(), 0));
  s.set("uGenCenter", (bounds.lo + bounds.hi) * 0.5f);
  s.set("uGenHalf", glm::max((bounds.hi - bounds.lo) * 0.5f, vec3(1e-5f)));
  s.set("uGenNoiseType", g.noiseType);
  s.set("uGenDir", g.direction);
  s.set("uGenSlot", g.slot);
}

const Texture2D& TextureSet::evalMaskStack(const Layer& L, const AABB& bounds) {
  for (auto& t : maskFx_)
    if (!t.valid() || t.width() != res_) {
      t = Texture2D(res_, res_, GL_R16F, 1);
      t.setFilter(GL_LINEAR, GL_LINEAR);
    }
  RasterState rs;
  gl::ViewportScope vp(0, 0, res_, res_);
  glDisable(GL_BLEND);
  int cur = 0;
  {
    Framebuffer f;
    f.attach(0, maskFx_[0]);
    f.setDrawBuffers(1);
    gl::FramebufferScope fs(f.id());
    if (L.isBaked()) {
      // (0.41) a baked map layer's adjustments start from the bake with its fixes
      Shader& s = ShaderCache::get("fullscreen.vs.glsl", "baked_map.fs.glsl");
      s.tex("uRaw", 0, bakedRaw(L.bakedMap).id());
      s.tex("uFixes", 1, L.mask ? L.mask->id() : transparent.id());
      s.set("uHasFixes", L.mask && L.mask->size() == ivec2(res_) ? 1 : 0);
      s.use();
      drawFullscreenTriangle();
    } else {
      float one[4] = {1, 1, 1, 1};
      glClearBufferfv(GL_COLOR, 0, one);  // the stack starts white
    }
  }
  float resScale = float(effectiveRes()) / 1024.f;
  // patterns: bake their materials first (baking uses framebuffers and texture units of its own)
  for (auto& e : L.maskEffects)
    if (e.enabled && e.type == MaskEffect::Pattern && materials && !e.pattern.id.empty())
      if (auto* en = materials->find(e.pattern.id)) materials->ensureBaked(*en, materials->bakeSize);
  // (a baked map layer's own smart masks read the bakes: the views are what it makes)
  auto map = [&](int k) -> const Texture2D& { return L.isBaked() ? bakedRaw(k) : bakedView(k); };
  auto bindMaps = [&](Shader& s) {
    s.tex("uPosMap", 5, posMap.id());
    s.tex("uNrmMap", 6, nrmMap.id());
    s.tex("uTanMap", 7, tanMap.id());
    s.tex("uAOMap", 8, map(0).id());
    s.tex("uCurvMap", 9, map(1).id());
    s.tex("uMatIdMap", 14, matIdMap.id());
    s.tex("uThickMap", 15, map(2).id());
  };
  auto pass = [&](const char* variant, int src, int dst, const MaskEffect& e, int blurSrc) {
    Framebuffer f;
    f.attach(0, maskFx_[dst]);
    f.setDrawBuffers(1);
    gl::FramebufferScope fs(f.id());
    Shader& s = variant ? ShaderCache::get("fullscreen.vs.glsl", "mask_effect.fs.glsl", {variant})
                        : ShaderCache::get("fullscreen.vs.glsl", "mask_effect.fs.glsl");
    s.tex("uPrev", 0, maskFx_[src].id());
    s.tex("uPaint", 1, L.mask && !L.isBaked() ? L.mask->id() : white.id());
    s.tex("uBlurSrc", 2, maskFx_[blurSrc >= 0 ? blurSrc : src].id());
    bindMaps(s);
    setGeneratorUniforms(s, e.type == MaskEffect::Generator ? e.gen : MaskGenerator(), bounds);
    if (e.type == MaskEffect::Pattern && materials) materials->bind(s, e.pattern, 10);
    s.set("uTileUV", tileUV_);
    s.set("uFxType", e.type);
    s.set("uFxNoiseType", e.noiseType);
    s.set("uFxGrow", e.amount >= 0.f ? 1 : 0);
    s.set("uFxEdges", e.type == MaskEffect::Edges ? 1 : 0);
    s.set("uFxPattern", vec4((float)e.source, e.level, e.contrast, e.invert ? 1.f : 0.f));
    s.set("uFxBlend", e.blend);
    s.set("uFxOpacity", e.opacity);
    s.set("uFxValue", e.value);
    s.set("uFxNoise", vec4(e.scale, e.seed, e.contrast, 0));
    s.set("uFxLevelsIn", vec4(e.inLow, e.inHigh, e.gamma, 0));
    s.set("uFxLevelsOut", vec2(e.outLow, e.outHigh));
    s.set("uFxRadius", std::max(0.5f, (e.type == MaskEffect::Grow ? std::abs(e.amount) : e.radius) * resScale));
    s.use();
    drawFullscreenTriangle();
  };
  for (auto& e : L.maskEffects) {
    if (!e.enabled || e.opacity <= 0.f) continue;
    if (L.isBaked() && e.type == MaskEffect::Paint) continue;  // (its fixes come in at the start)
    int a = (cur + 1) % 3, b = (cur + 2) % 3;
    if (e.type == MaskEffect::Blur || e.type == MaskEffect::Edges) {
      if (e.radius <= 0.f) continue;
      pass("MASKFX_BLUR_H", cur, a, e, cur);  // horizontal blur of the stack into a
      pass("MASKFX_BLUR_V", cur, b, e, a);    // vertical blur of a, mixed with the stack, into b
      cur = b;
    } else if (e.type == MaskEffect::Grow) {
      if (e.amount == 0.f) continue;
      pass("MASKFX_GROW_H", cur, a, e, cur);
      pass("MASKFX_GROW_V", cur, b, e, a);
      cur = b;
    } else {
      pass(nullptr, cur, a, e, -1);
      cur = a;
    }
  }
  return maskFx_[cur];
}

void TextureSet::composite(const std::vector<std::unique_ptr<Layer>>& layers, MaterialLibrary& lib, const AABB& bounds,
                           uint32_t soloUid, const glm::ivec4* box) {
  materials = &lib;
  if (!res_) return;
  if (!accColor.valid()) makeAccumulators(), box = nullptr;  // (1.2) after sleep(): all of it
  applyBakedLayers(layers, bounds);  // (0.41) the maps the layers' smart masks read
  RasterState rs;
  gl::ViewportScope vp(0, 0, res_, res_);
  // (0.44.2) a stroke's region only: every pass below works texel by texel (the height -> normal pass reads its
  // neighbors: a margin), so the rest of the atlas keeps what the last composite made. Mask stacks (blurs and the
  // like read far) need everything.
  bool boxed = box != nullptr;
  for (auto& lp : layers)
    if (lp->visible && !lp->isBaked())
      for (auto& e : lp->maskEffects) boxed &= !e.enabled;
  glm::ivec4 bx(0, 0, res_, res_);
  if (boxed) {
    bx = glm::ivec4(glm::max(glm::ivec2(box->x, box->y) - 4, glm::ivec2(0)), glm::min(glm::ivec2(box->z, box->w) + 4, glm::ivec2(res_)));
    if (bx.x >= bx.z || bx.y >= bx.w) return;
  }
  auto scissor = [&](bool on) {
    if (boxed && on) {
      glEnable(GL_SCISSOR_TEST);
      glScissor(bx.x, bx.y, bx.z - bx.x, bx.w - bx.y);
    } else {
      glDisable(GL_SCISSOR_TEST);
    }
  };
  Framebuffer fbo;
  fbo.attach(0, accColor);
  fbo.attach(1, accRMH);
  fbo.attach(2, accNormal);
  fbo.attach(3, accEmissive);
  fbo.attach(4, accExtra);
  fbo.setDrawBuffers(5);
  {
    gl::FramebufferScope fs(fbo.id());
    vec4 c0(base.color, 1), c1(base.roughness, base.metallic, base.height, 0), c2(baseNormal, 0), c3(base.emissive, 1);
    vec4 c4 = base.extra();
    scissor(true);
    glClearBufferfv(GL_COLOR, 0, &c0.x);
    glClearBufferfv(GL_COLOR, 1, &c1.x);
    glClearBufferfv(GL_COLOR, 2, &c2.x);
    glClearBufferfv(GL_COLOR, 3, &c3.x);
    glClearBufferfv(GL_COLOR, 4, &c4.x);
    scissor(false);
    Texture2D* acc[5] = {&accColor, &accRMH, &accNormal, &accEmissive, &accExtra};
    extraUsed = (base.opacity < 0.999f ? 64 : 0) | (base.sheen > 0.001f ? 128 : 0) | (base.coat > 0.001f ? 256 : 0) |
                (base.subsurface > 0.001f ? 512 : 0);

    auto common = [&](Shader& s, const Layer& L) {
      // first: evaluating a mask stack uses texture units of its own
      const Texture2D* stackMask = L.maskEffects.empty() ? nullptr : &evalMaskStack(L, bounds);
      s.tex("uAccColor", 0, accColor.id());
      s.tex("uAccRMH", 1, accRMH.id());
      s.tex("uAccNormal", 2, accNormal.id());
      s.tex("uAccEmissive", 3, accEmissive.id());
      s.tex("uAccExtra", 18, accExtra.id());
      s.tex("uLayerMask", 4, L.mask ? L.mask->id() : white.id());
      s.tex("uPosMap", 5, posMap.id());
      s.tex("uNrmMap", 6, nrmMap.id());
      s.tex("uTanMap", 7, tanMap.id());
      s.tex("uAOMap", 8, aoView().id());
      s.tex("uCurvMap", 9, curvView().id());
      s.set("uLayerOpacity", L.opacity);
      s.set("uBlendMode", (int)L.blend);
      s.set("uChannelMask", L.channelMask);
      s.set("uChanOpacity0", vec4(L.channelOpacity[0], L.channelOpacity[1], L.channelOpacity[2], L.channelOpacity[3]));
      s.set("uChanOpacity1", vec2(L.channelOpacity[4], L.channelOpacity[5]));
      s.set("uChanOpacity2", vec4(L.channelOpacity[6], L.channelOpacity[7], L.channelOpacity[8], L.channelOpacity[9]));
      s.set("uTileUV", tileUV_);
      setGeneratorUniforms(s, L.generator, bounds);
      if (stackMask) {
        // the stack replaces painted mask x generator
        s.tex("uLayerMask", 4, stackMask->id());
        s.set("uGenType", 0);
      }
      // (units 10-15 are the paint channels / material textures, 18 the extra accumulator, 19-22 the extra
      // paint channels)
      s.tex("uMatIdMap", 16, matIdMap.id());
      s.tex("uThickMap", 17, thickView().id());
    };

    // instances: content from the source layer, everything else from the instance
    auto contentOf = [&](const Layer& L) -> const Layer& {
      const Layer* cur = &L;
      for (int guard = 0; guard < 8 && cur->instanceOf; ++guard) {
        const Layer* src = nullptr;
        for (auto& lp : layers)
          if (lp->uid == cur->instanceOf) src = lp.get();
        if (!src || src == &L || src->isFolder()) break;
        cur = src;
      }
      return *cur;
    };
    auto renderLayer = [&](const Layer& L) {
      const Layer& C = contentOf(L);
      bool fill = C.type == LayerType::Fill;
      bool adjust = C.type == LayerType::Adjust;
      if (fill) extraUsed |= lib.extraChannelsUsed(C.material) & L.channelMask;
      else if (!adjust)
        for (int c = kFirstExtraChannel; c < kChannelCount; ++c)
          if (C.hasChannel(c) && (L.channelMask & (1 << c))) extraUsed |= 1 << c;
      if (!fill && !adjust) {
        bool any = false;
        for (int c = 0; c < kChannelCount; ++c) any |= C.hasChannel(c);
        if (!any) return;
      }
      Shader& s = ShaderCache::get("fullscreen.vs.glsl", "composite.fs.glsl",
                                   {fill ? "LAYER_FILL" : adjust ? "LAYER_ADJUST" : "LAYER_PAINT"});
      if (!s.valid()) return;
      if (fill) lib.bind(s, C.material, 10);  // first: may bake (uses texture units)
      common(s, L);
      if (adjust) {
        const AdjustParams& a = C.adjust;
        s.set("uAdj0", vec4(a.hue, a.saturation, a.brightness, a.contrast));
        s.set("uAdj1", vec3(a.roughness, a.metallic, a.height));
        s.set("uAdj2", vec4(a.exposure, a.temperature, a.tint, a.vibrance));
        s.set("uAdj3", vec4(a.highlights, a.shadows, a.delight, 0.f));
      } else if (!fill) {
        static const char* names[] = {"uLColor", "uLRough",  "uLMetal", "uLHeight", "uLNormal",
                                      "uLEmissive", "uLOpacity", "uLSheen", "uLCoat", "uLSubsurface"};
        static_assert(sizeof(names) / sizeof(names[0]) == kChannelCount);
        for (int c = 0; c < kChannelCount; ++c)
          s.tex(names[c], c < kFirstExtraChannel ? 10 + c : 19 + (c - kFirstExtraChannel),
                C.hasChannel(c) ? C.channels[c]->id() : transparent.id());
      }
      s.use();
      scissor(true);
      drawFullscreenTriangle();
      scissor(false);
      glTextureBarrier();
    };

    // folder: remember the accumulator, composite the children, then blend back through the folder's mask
    auto snapshot = [&](int depth) {
      while ((int)folderSnaps_.size() < (depth + 1) * 5) folderSnaps_.push_back(nullptr);
      for (int i = 0; i < 5; ++i) {
        auto& t = folderSnaps_[depth * 5 + i];
        if (!t || t->width() != res_ || t->format() != acc[i]->format())
          t = std::make_unique<Texture2D>(res_, res_, acc[i]->format(), 1);
        glCopyImageSubData(acc[i]->id(), GL_TEXTURE_2D, 0, bx.x, bx.y, 0, t->id(), GL_TEXTURE_2D, 0, bx.x, bx.y, 0,
                           bx.z - bx.x, bx.w - bx.y, 1);
      }
    };
    auto closeFolder = [&](const Layer& F, int depth) {
      Shader& s = ShaderCache::get("fullscreen.vs.glsl", "composite.fs.glsl", {"LAYER_FOLDER"});
      if (!s.valid()) return;
      common(s, F);
      static const char* names[] = {"uBeforeColor", "uBeforeRMH", "uBeforeNormal", "uBeforeEmissive", "uBeforeExtra"};
      for (int i = 0; i < 5; ++i) s.tex(names[i], 10 + i, folderSnaps_[depth * 5 + i]->id());
      s.use();
      scissor(true);
      drawFullscreenTriangle();
      scissor(false);
      glTextureBarrier();
    };

    std::vector<uint32_t> folders;
    for (auto& lp : layers)
      if (lp->isFolder()) folders.push_back(lp->uid);
    auto parentOf = [&](const Layer& L) {
      return std::find(folders.begin(), folders.end(), L.parent) != folders.end() ? L.parent : 0u;
    };
    std::function<void(uint32_t, int)> group = [&](uint32_t parent, int depth) {
      for (auto& lp : layers) {
        const Layer& L = *lp;
        if (parentOf(L) != parent) continue;
        if (!L.visible || L.opacity <= 0.f || L.channelMask == 0) continue;
        if (L.isFolder()) {
          if (depth > 32) continue;
          snapshot(depth);
          group(L.uid, depth + 1);
          closeFolder(L, depth);
        } else {
          renderLayer(L);
        }
      }
    };
    const Layer* solo = nullptr;
    for (auto& lp : layers)
      if (soloUid && lp->uid == soloUid) solo = lp.get();
    if (solo) {
      if (solo->isFolder()) {
        snapshot(0);
        group(solo->uid, 1);
        closeFolder(*solo, 0);
      } else {
        renderLayer(*solo);
      }
    } else {
      group(0, 0);
    }
    if (hasHighNormal_) {
      // the baked high-poly detail is combined with whatever the layers produced
      // (like Substance's "normal map combine"), so fill layers don't wipe it out
      Framebuffer nfbo;
      nfbo.attach(0, accNormal);
      nfbo.setDrawBuffers(1);
      gl::FramebufferScope nfs(nfbo.id());
      Shader& bn = ShaderCache::get("fullscreen.vs.glsl", "post.fs.glsl", {"POST_BASE_NORMAL"});
      bn.tex("uHighNormal", 0, highNormal.id());
      bn.tex("uNrmMap", 1, nrmMap.id());
      bn.tex("uTanMap", 2, tanMap.id());
      bn.tex("uAccNormal", 3, accNormal.id());
      bn.use();
      scissor(true);
      drawFullscreenTriangle();
      scissor(false);
      glTextureBarrier();
    }
  }
  // finalize: dilate into the outputs
  Framebuffer ffbo;
  ffbo.attach(0, finColor);
  ffbo.attach(1, finRMH);
  ffbo.attach(2, finNormalDetail);
  ffbo.attach(3, finEmissive);
  ffbo.attach(4, finExtra);
  ffbo.setDrawBuffers(5);
  {
    gl::FramebufferScope fs(ffbo.id());
    Shader& s = ShaderCache::get("fullscreen.vs.glsl", "finalize.fs.glsl", {"FINALIZE_DILATE"});
    s.tex("uAccColor", 0, accColor.id());
    s.tex("uAccRMH", 1, accRMH.id());
    s.tex("uAccNormal", 2, accNormal.id());
    s.tex("uAccEmissive", 3, accEmissive.id());
    s.tex("uAccExtra", 6, accExtra.id());
    s.tex("uSeed", 4, seedMap.id());
    s.tex("uAOMap", 5, aoView().id());
    s.set("uHasAO", hasAO_ && bakedShown_[0] ? 1 : 0);
    s.set("uDilate", dilate ? 1 : 0);
    s.use();
    scissor(true);
    drawFullscreenTriangle();
    scissor(false);
  }
  Framebuffer nfbo;
  nfbo.attach(0, finNormal);
  nfbo.setDrawBuffers(1);
  {
    gl::FramebufferScope fs(nfbo.id());
    Shader& s = ShaderCache::get("fullscreen.vs.glsl", "finalize.fs.glsl", {"FINALIZE_NORMAL"});
    s.tex("uRMH", 0, finRMH.id());
    s.tex("uNormalIn", 1, finNormalDetail.id());
    s.tex("uCoverage", 2, posMap.id());
    s.set("uHeightStrength", heightNormalStrength * effectiveRes() / 1024.f);
    s.use();
    scissor(true);
    drawFullscreenTriangle();
    scissor(false);
  }
  for (Texture2D* t : {&finColor, &finRMH, &finEmissive, &finNormal, &finExtra}) {
    if (boxed) updateMipsRegion(*t, bx);
    else t->generateMips();
  }
  ++compositeCount;
}

void TextureSet::updateMipsRegion(Texture2D& t, glm::ivec4 box) {
  if (t.levels() <= 1 || !t.valid()) return;
  Shader& s = ShaderCache::get("fullscreen.vs.glsl", "post.fs.glsl", {"POST_MIP_DOWN"});
  if (!s.valid()) {
    t.generateMips();
    return;
  }
  RasterState rs;
  Framebuffer f;
  glm::ivec2 lo(box.x, box.y), hi(box.z, box.w);
  int w = t.width(), h = t.height();
  for (int level = 1; level < t.levels(); ++level) {
    w = std::max(1, w / 2), h = std::max(1, h / 2);
    lo = lo / 2, hi = glm::min((hi + 1) / 2, glm::ivec2(w, h));
    if (lo.x >= hi.x || lo.y >= hi.y) break;
    // the level above as the only one bound for reading (no feedback with the level written)
    glTextureParameteri(t.id(), GL_TEXTURE_BASE_LEVEL, level - 1);
    glTextureParameteri(t.id(), GL_TEXTURE_MAX_LEVEL, level - 1);
    f.attach(0, t, level);
    f.setDrawBuffers(1);
    gl::FramebufferScope fs(f.id());
    gl::ViewportScope vp(0, 0, w, h);
    glEnable(GL_SCISSOR_TEST);
    glScissor(lo.x, lo.y, hi.x - lo.x, hi.y - lo.y);
    s.tex("uSrc", 0, t.id());
    s.use();
    drawFullscreenTriangle();
    glDisable(GL_SCISSOR_TEST);
  }
  glTextureParameteri(t.id(), GL_TEXTURE_BASE_LEVEL, 0);
  glTextureParameteri(t.id(), GL_TEXTURE_MAX_LEVEL, t.levels() - 1);
}

void TextureSet::applyBakedLayers(const std::vector<std::unique_ptr<Layer>>& layers, const AABB& bounds) {
  for (int k = 0; k < kBakedMapCount; ++k) {
    bakedOn_[k] = false;
    bakedShown_[k] = true;
    if (!bakedLayersOn) continue;
    const Layer* L = nullptr;
    for (auto& lp : layers)
      if (lp->isBaked() && lp->bakedMap == k) {
        L = lp.get();
        break;
      }
    if (L && !L->visible) bakedShown_[k] = false;  // (0.44.2) switched off: out of the view and the exports
    if (!L || !L->visible || !hasBakedMap(k)) continue;  // the bake as it is
    bool fixes = L->mask && L->mask->size() == ivec2(res_);
    bool stack = false;
    for (auto& e : L->maskEffects) stack |= e.enabled && e.opacity > 0.f;
    if (!fixes && !stack && L->bakedStrength == 1.f) continue;  // nothing changes it
    const Texture2D* fx = stack ? &evalMaskStack(*L, bounds) : nullptr;  // (first: it binds framebuffers of its own)
    Texture2D& out = bakedEff_[k];
    if (!out.valid() || out.width() != res_) {
      out = Texture2D(res_, res_, GL_RGBA8, 1);
      out.setFilter(GL_LINEAR, GL_LINEAR);
    }
    Texture2D& before = scratch(res_, res_, GL_RGBA8);  // (then dilated into `out`)
    {
      RasterState rs;
      Framebuffer f;
      f.attach(0, before);
      f.setDrawBuffers(1);
      gl::FramebufferScope fs(f.id());
      gl::ViewportScope vp(0, 0, res_, res_);
      glDisable(GL_BLEND);
      Shader& s = ShaderCache::get("fullscreen.vs.glsl", "baked_map.fs.glsl", {"BAKED_FINAL"});
      s.tex("uRaw", 0, bakedRaw(k).id());
      s.tex("uFixes", 1, fixes ? L->mask->id() : transparent.id());
      s.set("uHasFixes", fixes ? 1 : 0);
      s.tex("uStack", 2, fx ? fx->id() : white.id());
      s.set("uHasStack", fx ? 1 : 0);
      s.set("uStrength", L->bakedStrength);
      s.set("uNeutral", bakedMapNeutral(k));
      s.set("uOpacity", std::clamp(L->opacity, 0.f, 1.f));
      s.use();
      drawFullscreenTriangle();
    }
    dilateInto(before, out);  // (the gutters: as the bakes have them)
    bakedOn_[k] = true;
  }
}

void TextureSet::bakedWithFixes(const Layer& L, Texture2D& out) {
  if (!out.valid() || out.width() != res_ || out.format() != GL_R16F) {
    out = Texture2D(res_, res_, GL_R16F, 1);
    out.setFilter(GL_LINEAR, GL_LINEAR);
  }
  RasterState rs;
  Framebuffer f;
  f.attach(0, out);
  f.setDrawBuffers(1);
  gl::FramebufferScope fs(f.id());
  gl::ViewportScope vp(0, 0, res_, res_);
  glDisable(GL_BLEND);
  Shader& s = ShaderCache::get("fullscreen.vs.glsl", "baked_map.fs.glsl");
  s.tex("uRaw", 0, bakedRaw(L.bakedMap).id());
  s.tex("uFixes", 1, L.mask ? L.mask->id() : transparent.id());
  s.set("uHasFixes", L.mask && L.mask->size() == ivec2(res_) ? 1 : 0);
  s.use();
  drawFullscreenTriangle();
}

void TextureSet::resampleByLookup(const Texture2D& lookup, Texture2D& tex) {
  if (!tex.valid()) return;
  RasterState rs;
  Texture2D dst(tex.width(), tex.height(), tex.format(), 1);
  Framebuffer fbo;
  fbo.attach(0, dst);
  fbo.setDrawBuffers(1);
  {
    gl::FramebufferScope fs(fbo.id());
    gl::ViewportScope vp(0, 0, tex.width(), tex.height());
    Shader& s = ShaderCache::get("fullscreen.vs.glsl", "post.fs.glsl", {"POST_LOOKUP"});
    s.tex("uSrc", 0, tex.id());
    s.tex("uLookup", 1, lookup.id());
    s.use();
    drawFullscreenTriangle();
  }
  tex.copyFrom(dst);
}

void TextureSet::reproject(GpuMesh& gpu, const std::vector<vec2>& newUVs, Texture2D& tex, int newSize,
                           const Texture2D* source) {
  if (!tex.valid()) return;
  RasterState rs;
  // the source, dilated with the OLD layout's seeds so bilinear taps at island borders do not
  // pull in empty gutter texels. Scratch textures are reused: allocating three textures per
  // channel made every UV edit of a 4K texture set take about a second.
  const Texture2D& orig = source && source->valid() && source->size() == tex.size() && source->format() == tex.format() ? *source : tex;
  Texture2D& src = scratch(tex.width(), tex.height(), tex.format());
  if (tex.width() == res_) dilateInto(orig, src);
  else src.copyFrom(orig);
  int W = newSize > 0 ? newSize : tex.width(), H = newSize > 0 ? newSize : tex.height();
  bool inPlace = W == tex.width() && H == tex.height();
  Texture2D fresh;
  if (!inPlace) fresh.create(W, H, tex.format(), 1);
  Texture2D& dst = inPlace ? tex : fresh;
  dst.clear(vec4(0));
  std::vector<vec4> aux(newUVs.size());
  for (size_t i = 0; i < newUVs.size(); ++i) aux[i] = vec4(newUVs[i], 0, 0);
  gpu.setAux(aux);
  Framebuffer fbo;
  fbo.attach(0, dst);
  fbo.setDrawBuffers(1);
  {
    gl::FramebufferScope fs(fbo.id());
    gl::ViewportScope vp(0, 0, W, H);
    Shader& s = ShaderCache::get("uvraster.vs.glsl", "bake.fs.glsl", {"BAKE_REPROJECT"});
    s.tex("uSource", 0, src.id());
    s.set("uUVTransform", vec4(1, 1, 0, 0));
    s.set("uUseAuxAsUV", 1);
    int alphaMode = tex.format() == GL_RGBA8 && &tex != &aoMap && &tex != &curvMap && &tex != &thickMap ? 1
                  : (tex.format() == GL_RG8 || tex.format() == GL_RG16F) ? 2 : 0;
    s.set("uAlphaMode", alphaMode);
    s.use();
    gpu.drawTriangles();
    s.set("uUseAuxAsUV", 0);
  }
  if (!inPlace) std::swap(tex, fresh);
  tex.generateMips();
}

}  // namespace patina
