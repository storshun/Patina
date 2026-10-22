#include "engine/PathTracer.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "core/Log.h"
#include "core/Mesh.h"
#include "engine/Document.h"
#include "engine/Environment.h"
#include "gfx/Shader.h"

namespace patina {

static_assert(sizeof(Bvh::Node) == 32, "pathtrace.cs.glsl reads a node as 8 words");

namespace {
constexpr int kEnvW = 512, kEnvH = 256;   // the environment's luminance map
constexpr double kBudgetNs = 11.0e6;      // the GPU time a frame of the view may take
constexpr int kBand = 16;                 // rows: a pass goes down the picture in bands of these
}  // namespace

PathTracer::~PathTracer() {
  if (nodeBuf_) glDeleteBuffers(1, &nodeBuf_);
  if (triIdxBuf_) glDeleteBuffers(1, &triIdxBuf_);
  for (unsigned b : {sceneVbo_, sceneIbo_, sceneObj_})
    if (b) glDeleteBuffers(1, &b);
  for (unsigned& t : arr_)
    if (t) glDeleteTextures(1, &t);
  for (unsigned& f : fbo_)
    if (f) glDeleteFramebuffers(1, &f);
  for (unsigned& q : queries_)
    if (q) glDeleteQueries(1, &q);
}

void PathTracer::release() {
  accumA_.destroy();
  accumB_.destroy();
  w_ = h_ = 0;
  key_ = ~0ull;
}

void PathTracer::uploadBvh(const Bvh& bvh) {
  const auto& nodes = bvh.nodes();
  const auto& idx = bvh.triIndices();
  if (!nodeBuf_) glCreateBuffers(1, &nodeBuf_);
  if (!triIdxBuf_) glCreateBuffers(1, &triIdxBuf_);
  glNamedBufferData(nodeBuf_, GLsizeiptr(std::max<size_t>(nodes.size() * sizeof(Bvh::Node), 32)), nodes.data(), GL_STATIC_DRAW);
  glNamedBufferData(triIdxBuf_, GLsizeiptr(std::max<size_t>(idx.size() * 4, 4)), idx.data(), GL_STATIC_DRAW);
  bvhSerial_ = bvh.serial();
}

uint64_t PathTracer::sceneKey(const std::vector<const Document*>& docs) {
  uint64_t k = 1469598103934665603ull;
  auto mix = [&](uint64_t v) { k = (k ^ v) * 1099511628211ull; };
  for (const Document* d : docs) {
    mix(d->gpu.serial()), mix(d->tex.compositeCount), mix(uint64_t(d->tex.resolution()));
    mat4 m = d->toWorld();
    const uint32_t* w = reinterpret_cast<const uint32_t*>(&m[0][0]);
    for (int i = 0; i < 16; ++i) mix(w[i]);
  }
  return k;
}

// (1.2) Several objects: their loops in the world (the layout of GpuMesh's buffer), their triangles with the object
// each belongs to, one BVH over them all, and their final maps as the layers of texture arrays (at one size).
bool PathTracer::prepareScene() {
  uint64_t gk = 1469598103934665603ull;
  auto mix = [&](uint64_t v) { gk = (gk ^ v) * 1099511628211ull; };
  for (const Document* d : scene_) {
    mix(d->gpu.serial()), mix(d->mesh.loopCount());
    mat4 m = d->toWorld();
    const uint32_t* w = reinterpret_cast<const uint32_t*>(&m[0][0]);
    for (int i = 0; i < 16; ++i) mix(w[i]);
  }
  if (gk != sceneGeomKey_ || !sceneVbo_) {
    sceneGeomKey_ = gk;
    size_t L = 0, T = 0;
    for (const Document* d : scene_) L += d->mesh.loopCount(), T += d->mesh.tris.size();
    std::vector<float> v(L * 14);
    std::vector<uint32_t> tris(T * 3), obj(T);
    Mesh merged;  // (only what the BVH reads: a position per loop, the triangles)
    merged.positions.resize(L);
    merged.loopVert.resize(L);
    merged.tris.resize(T);
    size_t lo = 0, to = 0;
    AABB box;
    for (size_t k = 0; k < scene_.size(); ++k) {
      const Document& d = *scene_[k];
      const Mesh& m = d.mesh;
      const mat4 M = d.toWorld();
      const mat3 N = glm::transpose(glm::inverse(mat3(M)));
      const float flip = glm::determinant(mat3(M)) < 0.f ? -1.f : 1.f;
      for (uint32_t l = 0; l < m.loopCount(); ++l) {
        vec3 p = vec3(M * vec4(m.positions[m.loopVert[l]], 1.f));
        vec3 n = glm::normalize(N * m.loopNormal[l]);
        vec4 t = m.loopTangent[l];
        vec3 tw = mat3(M) * vec3(t);
        float tl = glm::length(tw);
        tw = tl > 1e-20f ? tw / tl : vec3(1, 0, 0);
        vec2 uv = m.texUV(l), muv = m.loopUV[l];
        float* o = &v[(lo + l) * 14];
        float vals[14] = {p.x, p.y, p.z, n.x, n.y, n.z, uv.x, uv.y, tw.x, tw.y, tw.z, t.w * flip, muv.x, muv.y};
        std::copy(vals, vals + 14, o);
        merged.positions[lo + l] = p;
        merged.loopVert[lo + l] = uint32_t(lo + l);
        box.add(p);
      }
      for (size_t i = 0; i < m.tris.size(); ++i) {
        glm::uvec3 tr = m.tris[i] + glm::uvec3(uint32_t(lo));
        merged.tris[to + i] = tr;
        tris[(to + i) * 3] = tr.x, tris[(to + i) * 3 + 1] = tr.y, tris[(to + i) * 3 + 2] = tr.z;
        obj[to + i] = uint32_t(k);
      }
      lo += m.loopCount(), to += m.tris.size();
    }
    auto upload = [](unsigned& b, const void* data, size_t bytes) {
      if (!b) glCreateBuffers(1, &b);
      glNamedBufferData(b, GLsizeiptr(std::max<size_t>(bytes, 4)), data, GL_STATIC_DRAW);
    };
    upload(sceneVbo_, v.data(), v.size() * 4);
    upload(sceneIbo_, tris.data(), tris.size() * 4);
    upload(sceneObj_, obj.data(), obj.size() * 4);
    sceneBvh_.build(merged);
    sceneGroundY_ = box.valid() ? box.lo.y : 0.f;
    sceneEps_ = std::max(box.valid() ? box.radius() : 1.f, 1e-3f) * 2e-5f;
  }
  // the textures: one size for all (the largest, smaller when there are many), copied again where they changed
  const int n = (int)scene_.size();
  int R = 256;
  for (const Document* d : scene_) R = std::max(R, d->tex.resolution());
  R = std::min(R, 4096);
  while (R > 512 && double(n) * 5 * 4 * double(R) * R > 1.2e9) R /= 2;
  const Texture2D* first[5] = {&scene_[0]->tex.finColor, &scene_[0]->tex.finRMH, &scene_[0]->tex.finNormal,
                               &scene_[0]->tex.finEmissive, &scene_[0]->tex.finExtra};
  bool fresh = R != arrRes_ || n != arrLayers_;
  for (int c = 0; c < 5 && !fresh; ++c) fresh = arrFormat_[c] != (unsigned)first[c]->format();
  if (fresh) {
    for (int c = 0; c < 5; ++c) {
      if (arr_[c]) glDeleteTextures(1, &arr_[c]);
      glCreateTextures(GL_TEXTURE_2D_ARRAY, 1, &arr_[c]);
      arrFormat_[c] = first[c]->format();
      glTextureStorage3D(arr_[c], 1, arrFormat_[c], R, R, n);
      glTextureParameteri(arr_[c], GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTextureParameteri(arr_[c], GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTextureParameteri(arr_[c], GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTextureParameteri(arr_[c], GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    arrRes_ = R, arrLayers_ = n;
    arrKeys_.assign(n, 0);
  }
  if (!fbo_[0]) glCreateFramebuffers(2, fbo_);
  sceneOpacity_ = 0;
  for (int k = 0; k < n; ++k) {
    const TextureSet& t = scene_[k]->tex;
    sceneOpacity_ |= (t.extraUsed & 64) ? 1 : 0;
    uint64_t key = (scene_[k]->gpu.serial() * 0x9E3779B97F4A7C15ull) ^ (t.compositeCount << 20) ^ uint64_t(t.resolution());
    if (arrKeys_[k] == key) continue;
    arrKeys_[k] = key;
    const Texture2D* maps[5] = {&t.finColor, &t.finRMH, &t.finNormal, &t.finEmissive, &t.finExtra};
    for (int c = 0; c < 5; ++c) {
      if (!maps[c]->valid()) continue;
      glNamedFramebufferTexture(fbo_[0], GL_COLOR_ATTACHMENT0, maps[c]->id(), 0);
      glNamedFramebufferReadBuffer(fbo_[0], GL_COLOR_ATTACHMENT0);
      glNamedFramebufferTextureLayer(fbo_[1], GL_COLOR_ATTACHMENT0, arr_[c], 0, k);
      glNamedFramebufferDrawBuffer(fbo_[1], GL_COLOR_ATTACHMENT0);
      glBlitNamedFramebuffer(fbo_[0], fbo_[1], 0, 0, maps[c]->width(), maps[c]->height(), 0, 0, R, R, GL_COLOR_BUFFER_BIT,
                             GL_LINEAR);
    }
  }
  return true;
}

// The environment's light as CDFs to sample it by: the luminance map (made on the GPU from the cube), each row
// weighed by sin(theta) for the area it covers, a little uniform light everywhere so no direction is impossible.
void PathTracer::buildEnvCdf(const Environment& env) {
  envSerial_ = env.serial();
  Texture2D lum(kEnvW, kEnvH, GL_R32F);
  Shader& s = ShaderCache::compute("pathtrace.cs.glsl", {"ENV_LUM"});
  s.tex("uEnvSpec", 0, env.specular(), GL_TEXTURE_CUBE_MAP);
  s.set("uEnvSize", ivec2(kEnvW, kEnvH));
  s.set("uLod", 1.f);
  s.use();
  glBindImageTexture(0, lum.id(), 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_R32F);
  glDispatchCompute((kEnvW + 7) / 8, (kEnvH + 7) / 8, 1);
  glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
  std::vector<float> L(size_t(kEnvW) * kEnvH);
  lum.downloadRaw(GL_RED, GL_FLOAT, L.data(), L.size() * 4);
  double mean = 0;
  for (float v : L) mean += std::max(v, 0.f);
  mean /= double(L.size());
  std::vector<float> cond(size_t(kEnvW + 1) * kEnvH), marg(kEnvH + 1);
  std::vector<double> rows(kEnvH);
  double total = 0;
  for (int y = 0; y < kEnvH; ++y) {
    // (texture rows are as the GPU wrote them: y = 0 is v = 0, the top of the sky)
    double st = std::sin(kPi * (y + 0.5) / kEnvH), acc = 0;
    cond[size_t(y) * (kEnvW + 1)] = 0.f;
    std::vector<double> c(kEnvW + 1, 0.0);
    for (int x = 0; x < kEnvW; ++x) {
      double v = (std::max(L[size_t(y) * kEnvW + x], 0.f) + 0.01 * mean + 1e-8) * st;
      acc += v;
      c[x + 1] = acc;
    }
    for (int x = 0; x <= kEnvW; ++x) cond[size_t(y) * (kEnvW + 1) + x] = acc > 0 ? float(c[x] / acc) : float(x) / kEnvW;
    rows[y] = acc;
    total += acc;
  }
  double acc = 0;
  marg[0] = 0.f;
  for (int y = 0; y < kEnvH; ++y) acc += rows[y], marg[y + 1] = total > 0 ? float(acc / total) : float(y + 1) / kEnvH;
  envCond_.create(kEnvW + 1, kEnvH, GL_R32F);
  envCond_.uploadRaw(GL_RED, GL_FLOAT, cond.data());
  envMarg_.create(kEnvH + 1, 1, GL_R32F);
  envMarg_.uploadRaw(GL_RED, GL_FLOAT, marg.data());
  envTotal_ = mean > 1e-6 ? float(total) : 0.f;
}

bool PathTracer::prepare(Document& doc, const Environment& env, int w, int h) {
  if (sceneMode()) {
    for (const Document* d : scene_)
      if (!d->hasMesh()) return false;
  } else if (!doc.hasMesh() || doc.bvh.empty() || !doc.gpu.vertexBuffer() || !doc.gpu.indexBuffer()) {
    return false;
  }
  if (w != w_ || h != h_ || !accumA_.valid()) {
    accumA_.create(w, h, GL_RGBA32F);
    accumB_.create(w, h, GL_RGBA32F);
    accumA_.setFilter(GL_NEAREST, GL_NEAREST);
    accumB_.setFilter(GL_NEAREST, GL_NEAREST);
    accumA_.clear(vec4(0.f));
    accumB_.clear(vec4(0.f));
    w_ = w, h_ = h;
    key_ = ~0ull;
  }
  if (sceneMode()) {
    prepareScene();
    if (sceneBvh_.serial() != bvhSerial_ || !nodeBuf_) uploadBvh(sceneBvh_);
  } else if (doc.bvh.serial() != bvhSerial_ || !nodeBuf_) {
    uploadBvh(doc.bvh);
  }
  if (env.serial() != envSerial_ || !envCond_.valid()) buildEnvCdf(env);
  return true;
}

void PathTracer::dispatch(Document& doc, const Environment& env, const Camera& cam, const PathTraceSettings& st, int block,
                          int y0, int y1, bool overwrite) {
  const TextureSet& t = doc.tex;
  const bool scene = sceneMode();
  Shader& s = scene ? ShaderCache::compute("pathtrace.cs.glsl", {"SCENE"}) : ShaderCache::compute("pathtrace.cs.glsl");
  s.tex("uColorTex", 0, t.finColor.id());
  s.tex("uRMHTex", 1, t.finRMH.id());
  s.tex("uNormalTex", 2, t.finNormal.id());
  s.tex("uEmissiveTex", 3, t.finEmissive.id());
  s.tex("uExtraTex", 4, t.finExtra.id());  // (made with the texture set, as the others)
  if (scene) {
    s.tex("uColorArr", 8, arr_[0], GL_TEXTURE_2D_ARRAY);
    s.tex("uRMHArr", 9, arr_[1], GL_TEXTURE_2D_ARRAY);
    s.tex("uNormalArr", 10, arr_[2], GL_TEXTURE_2D_ARRAY);
    s.tex("uEmissiveArr", 11, arr_[3], GL_TEXTURE_2D_ARRAY);
    s.tex("uExtraArr", 12, arr_[4], GL_TEXTURE_2D_ARRAY);
  }
  s.tex("uEnvSpec", 5, env.specular(), GL_TEXTURE_CUBE_MAP);
  s.tex("uEnvCond", 6, envCond_.id());
  s.tex("uEnvMarg", 7, envMarg_.id());
  s.set("uEnvSize", ivec2(kEnvW, kEnvH));
  s.set("uEnvTotal", envTotal_);
  s.set("uEnvRot", env.rotationMatrix());
  s.set("uEnvIntensity", env.intensity);
  float aspect = float(w_) / float(std::max(h_, 1));
  s.set("uInvViewProj", glm::inverse(cam.viewProj(aspect)));
  s.set("uSize", ivec2(w_, h_));
  s.set("uY0", y0);
  s.set("uBlock", block);
  s.set("uOverwrite", overwrite ? 1 : 0);
  s.set("uFrame", ++frame_);
  s.set("uBounces", std::clamp(st.bounces, 0, 32));
  s.set("uClamp", std::max(st.clamp, 0.f));
  s.set("uGround", st.ground ? 1 : 0);
  s.set("uGroundY", scene ? sceneGroundY_ : doc.mesh.bounds.valid() ? doc.mesh.bounds.lo.y : 0.f);
  s.set("uOpacity", scene ? sceneOpacity_ : (t.extraUsed & 64) ? 1 : 0);
  s.set("uEps", scene ? sceneEps_ : std::max(doc.mesh.boundingRadius, 1e-3f) * 2e-5f);
  s.use();
  glBindImageTexture(0, accumA_.id(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA32F);
  glBindImageTexture(1, accumB_.id(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA32F);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, scene ? sceneVbo_ : doc.gpu.vertexBuffer());
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, scene ? sceneIbo_ : doc.gpu.indexBuffer());
  if (scene) glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, sceneObj_);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, nodeBuf_);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, triIdxBuf_);
  const int cw = (w_ + block - 1) / block, ch = (y1 - y0 + block - 1) / block;
  // (timed, so the view knows how much fits in a frame)
  int qi = queryNext_;
  if (!queries_[qi]) glCreateQueries(GL_TIME_ELAPSED, 1, &queries_[qi]);
  const bool timed = !queryBusy_[qi];
  if (timed) glBeginQuery(GL_TIME_ELAPSED, queries_[qi]);
  glDispatchCompute(GLuint((cw + 7) / 8), GLuint((ch + 7) / 8), 1);
  if (timed) {
    glEndQuery(GL_TIME_ELAPSED);
    queryBusy_[qi] = true;
    querySamples_[qi] = double(cw) * ch;
    queryNext_ = (qi + 1) % 3;
  }
  glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
  glBindImageTexture(0, 0, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA32F);
  glBindImageTexture(1, 0, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA32F);
}

void PathTracer::pollTimer() {
  for (int i = 0; i < 3; ++i) {
    if (!queryBusy_[i]) continue;
    GLint ready = 0;
    glGetQueryObjectiv(queries_[i], GL_QUERY_RESULT_AVAILABLE, &ready);
    if (!ready) continue;
    GLuint64 ns = 0;
    glGetQueryObjectui64v(queries_[i], GL_QUERY_RESULT, &ns);
    queryBusy_[i] = false;
    if (querySamples_[i] > 0) {
      double per = double(ns) / querySamples_[i];
      nsPerSample_ = std::clamp(nsPerSample_ * 0.6 + per * 0.4, 1.0, 1e6);
    }
  }
}

void PathTracer::progress(Document& doc, const Environment& env, const Camera& cam, int w, int h, const PathTraceSettings& st,
                          uint64_t key) {
  if (!prepare(doc, env, w, h)) return;
  pollTimer();
  if (key != key_) {
    // something changed: a quick picture in blocks small enough for the frame's time, then the passes again
    key_ = key;
    samples_ = 0, bandY_ = 0;
    int block = 1;
    while (block < 16 && double(w) * h / (double(block) * block) * nsPerSample_ > kBudgetNs) block *= 2;
    dispatch(doc, env, cam, st, block, 0, h, true);
    previewShown_ = block > 1;
    if (block == 1) samples_ = 1;  // (fast enough: that was a full sample)
    return;
  }
  if (done(st)) return;
  // as many rows of the pass as fit (at least one band); the first pass replaces the quick picture
  double rowsFit = kBudgetNs / std::max(1.0, double(w) * nsPerSample_);
  int rows = std::clamp(int(rowsFit / kBand) * kBand, kBand, std::max(kBand, h));
  int frameRows = 0;
  while (frameRows < rows && !done(st)) {
    int y1 = std::min(h, bandY_ + std::min(rows - frameRows, h - bandY_));
    dispatch(doc, env, cam, st, 1, bandY_, y1, samples_ == 0 && previewShown_);
    frameRows += y1 - bandY_;
    bandY_ = y1;
    if (bandY_ >= h) {
      bandY_ = 0;
      ++samples_;
      if (samples_ >= 1) previewShown_ = false;
    }
  }
}

void PathTracer::render(Document& doc, const Environment& env, const Camera& cam, int w, int h, const PathTraceSettings& st,
                        int samples) {
  if (!prepare(doc, env, w, h)) return;
  key_ = ~0ull;  // (the view starts again after this)
  samples_ = 0, bandY_ = 0, previewShown_ = false;
  // in bands of about a tenth of a second (from the measured cost), so no dispatch runs long enough for the driver to
  // give up on it (Windows resets the GPU after two seconds)
  for (int i = 0; i < samples; ++i) {
    const int band = std::clamp(int(1.0e8 / std::max(1.0, double(w) * nsPerSample_)), 16, std::max(16, h));
    for (int y = 0; y < h; y += band) dispatch(doc, env, cam, st, 1, y, std::min(h, y + band), i == 0);
    glFinish();
    pollTimer();
    ++samples_;
  }
}

void PathTracer::start(Document& doc, const Environment& env, int w, int h) {
  prepare(doc, env, w, h);
  key_ = ~0ull;  // (the view starts again after this)
  samples_ = 0, bandY_ = 0, previewShown_ = false;
}

bool PathTracer::step(Document& doc, const Environment& env, const Camera& cam, const PathTraceSettings& st, int samples,
                      double budgetNs) {
  if (!accumA_.valid() || !prepare(doc, env, w_, h_)) return true;
  pollTimer();
  double used = 0;
  while (samples_ < samples && used < budgetNs) {
    int rows = std::clamp(int((budgetNs - used) / std::max(1.0, double(w_) * nsPerSample_)), kBand, std::max(kBand, h_));
    int y1 = std::min(h_, bandY_ + rows);
    dispatch(doc, env, cam, st, 1, bandY_, y1, samples_ == 0);
    used += double(y1 - bandY_) * w_ * nsPerSample_;
    bandY_ = y1;
    if (bandY_ >= h_) bandY_ = 0, ++samples_;
  }
  glFlush();
  return samples_ >= samples;
}

void PathTracer::draw(const Environment& env, const Camera& cam, int w, int h, bool envBackground, float blurLod,
                      bool transparent) {
  if (!accumA_.valid() || w != w_ || h != h_) return;
  Shader& s = ShaderCache::get("fullscreen.vs.glsl", "pathtrace_show.fs.glsl");
  s.tex("uAccumA", 0, accumA_.id());
  s.tex("uAccumB", 1, accumB_.id());
  Camera bg = cam;
  bg.ortho = false;  // (the background through a perspective lens, as skybox.fs.glsl)
  s.set("uInvViewProjBg", glm::inverse(bg.viewProj(float(w) / float(std::max(h, 1)))));
  s.tex("uEnvSpec", 2, env.specular(), GL_TEXTURE_CUBE_MAP);
  s.set("uEnvRot", env.rotationMatrix());
  s.set("uBlurLod", blurLod);
  s.set("uExposure", env.exposure);
  s.set("uEnvIntensity", env.intensity);
  s.set("uMode", envBackground ? 0 : 1);
  s.set("uTop", vec3(0.2f, 0.21f, 0.23f));
  s.set("uBottom", vec3(0.07f, 0.075f, 0.08f));
  s.set("uTransparent", transparent ? 1 : 0);
  s.use();
  glDisable(GL_DEPTH_TEST);
  glDepthMask(GL_FALSE);
  drawFullscreenTriangle();
  glDepthMask(GL_TRUE);
  glEnable(GL_DEPTH_TEST);
}

}  // namespace patina
