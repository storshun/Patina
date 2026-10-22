#include "engine/Renderer.h"

#include "core/Parallel.h"

#include "gfx/GpuMesh.h"

#include <cstring>
#include <unordered_map>

#include "core/UVIsland.h"
#include "core/UVOps.h"
#include "engine/Document.h"
#include "engine/Environment.h"
#include "gfx/Shader.h"

namespace patina {

static const char* kViewNames[] = {"material",   "basecolor",  "roughness",  "metallic",     "normal",
                                   "height",     "emissive",   "ao",         "curvature",    "uv_checker",
                                   "mask",       "world_normal", "uv_islands", "texel_density", "uv_stretch",
                                   "material_ids", "thickness", "bent_normal", "symmetry", "opacity", "sheen", "coat",
                                   "subsurface", "mesh_check", "pbr_range", "solid", "path_traced", "bake_check", "face_sets"};
static_assert(sizeof(kViewNames) / sizeof(kViewNames[0]) == (int)ViewMode::Count);
const char* viewModeName(ViewMode m) { return kViewNames[std::clamp((int)m, 0, (int)ViewMode::Count - 1)]; }
ViewMode viewModeFromName(const std::string& s) {
  for (int i = 0; i < (int)ViewMode::Count; ++i)
    if (s == kViewNames[i]) return (ViewMode)i;
  return ViewMode::Material;
}

void Renderer::renderViewport(Document& doc, const Environment& env, const Camera& cam, RenderTarget& rt, int w, int h,
                              const ViewportOptions& opt, GLuint maskTex) {
  rt.ensure(w, h, GL_RGBA8);
  gl::FramebufferScope fs(rt.fbo.id());
  gl::ViewportScope vp(0, 0, w, h);
  glDisable(GL_BLEND);
  glDisable(GL_SCISSOR_TEST);
  glDepthMask(GL_TRUE);
  if (opt.transparentBackground) glClearColor(0.f, 0.f, 0.f, 0.f);
  else glClearColor(0.1f, 0.105f, 0.115f, 1.f);
  glClearDepth(1.0);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  float aspect = float(w) / float(std::max(h, 1));
  mat4 vpm = cam.viewProj(aspect);
  mat3 envRot = env.rotationMatrix();
  // (1.2) the model is drawn where its object is in the scene; the lines over it (wireframe, seams, retopology,
  // collision shapes) are in its own space, so they are drawn with the camera in that space
  const mat4 toWorld = doc.toWorld();
  const Camera lc = cam.inSpace(toWorld);
  const mat4 lvpm = lc.viewProj(aspect);

  // background
  if (!opt.transparentBackground) {
    glDisable(GL_DEPTH_TEST);
    Shader& s = ShaderCache::get("fullscreen.vs.glsl", "skybox.fs.glsl");
    // (0.38) seen through a perspective lens even when the view is orthographic: an orthographic view looks the same
    // way at every pixel, so the environment behind would be one flat color
    Camera bg = cam;
    bg.ortho = false;
    s.set("uInvViewProj", glm::inverse(bg.viewProj(aspect)));
    s.tex("uEnvSpec", 0, env.specular(), GL_TEXTURE_CUBE_MAP);
    s.set("uEnvRot", envRot);
    s.set("uBlurLod", opt.backgroundBlur);
    s.set("uExposure", env.exposure);
    s.set("uEnvIntensity", env.intensity);
    s.set("uMode", opt.envBackground ? 0 : 1);
    s.set("uTop", vec3(0.2f, 0.21f, 0.23f));
    s.set("uBottom", vec3(0.07f, 0.075f, 0.08f));
    s.use();
    drawFullscreenTriangle();
  }
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LESS);
  glDisable(GL_CULL_FACE);
  // (1.2) the scene's other objects, with their own textures
  if (sceneObjects && opt.mode != ViewMode::PathTraced)
    sceneObjects([&](const Document& d) { drawSceneObject(d, env, cam, aspect, opt, false); });
  if (!doc.hasMesh() || doc.objectHidden) {
    glDisable(GL_DEPTH_TEST);
    ++frames;
    return;
  }
  {
    Shader& s = ShaderCache::get("mesh.vs.glsl", "pbr.fs.glsl");
    TextureSet& t = doc.tex;
    s.set("uModel", toWorld);
    s.set("uViewProj", vpm);
    s.tex("uColorTex", 0, t.finColor.id());
    s.tex("uRMHTex", 1, t.finRMH.id());
    s.tex("uNormalTex", 2, t.finNormal.id());
    s.tex("uEmissiveTex", 3, t.finEmissive.id());
    s.tex("uAOMap", 4, t.aoView().id());
    s.tex("uCurvMap", 5, t.curvView().id());
    s.tex("uMaskTex", 6, maskTex ? maskTex : t.white.id());
    env.bind(s, 7, 8);
    s.set("uEnvIntensity", env.intensity);
    s.set("uEnvRot", envRot);
    s.set("uExposure", env.exposure);
    s.set("uCamPos", cam.eye());
    s.set("uView", (int)opt.mode);
    s.set("uUseAO", opt.useAO && t.hasAO() && t.bakedShown(0) ? 1 : 0);
    s.set("uSelTint", opt.selectionTint);
    s.set("uShowPins", opt.showPins ? 1 : 0);
    glProgramUniform1ui(s.id(), s.loc("uHoverFace"), opt.hoverFace);
    s.set("uShowUVIslands", opt.islandTint ? 1 : 0);
    s.set("uIsolateTile", opt.isolateSet && doc.mesh.tiles.active() && opt.isolateTile >= 0 ? opt.isolateTile : -1);
    s.tex("uMatIdTex", 9, t.matIdMap.id());
    s.tex("uThickMap", 10, t.thickView().id());
    s.tex("uBentMap", 11, t.bentMap.id());
    s.tex("uExtraTex", 13, t.finExtra.id());
    bool check = doc.bakeDownCurrent();
    s.tex("uBakeCheck", 14, check ? doc.bakeDownCheck.id() : t.white.id());
    s.set("uHasBakeCheck", check ? 1 : 0);
    s.set("uAlphaPass", 0);
    if (opt.mode == ViewMode::TexelDensity || opt.mode == ViewMode::UVStretch || opt.mode == ViewMode::Symmetry ||
        opt.mode == ViewMode::MeshCheck || opt.mode == ViewMode::FaceSets) {
      // per-face values: log2(density / target) or the stretch ratio; -100 = no UV area
      const Mesh& m = doc.mesh;
      uint32_t F = m.faceCount();
      const int W = 2048;
      int H = std::max(1, int((F + W - 1) / W));
      uint64_t key = uv::uvHash(m) ^ (uint64_t(opt.mode) << 56) ^ (uint64_t(doc.tileRes()) << 40);
      if (opt.mode == ViewMode::Symmetry) {
        doc.symmetry();  // brings the cache up to date (positions, plane)
        key ^= doc.symmetryKey() * 0xC2B2AE3D27D4EB4Full;
      }
      if (opt.mode == ViewMode::MeshCheck) {
        doc.meshCheck();
        key ^= doc.meshCheckKey() * 0x165667B19E3779F9ull;
      }
      if (opt.mode == ViewMode::FaceSets) {  // (1.2) the sets and their colors
        uint64_t k = 1469598103934665603ull;
        for (uint32_t id : doc.faceSetOf) k = (k ^ id) * 1099511628211ull;
        for (auto& fs : doc.faceSets) {
          uint32_t c[3];
          std::memcpy(c, &fs.color, sizeof c);
          k = (k ^ fs.id ^ (uint64_t(c[0]) << 20) ^ (uint64_t(c[1]) << 40) ^ c[2]) * 1099511628211ull;
        }
        key ^= k * 0x27D4EB2F165667C5ull;
      }
      float tgtKey = opt.densityTarget;
      uint32_t tk;
      std::memcpy(&tk, &tgtKey, 4);
      key ^= uint64_t(tk) * 0x9E3779B97F4A7C15ull;
      if (key != faceDataKey_ || faceData_.width() != W || faceData_.height() != H) {
        faceDataKey_ = key;
        std::vector<float> v(size_t(W) * H, -100.f);
        if (opt.mode == ViewMode::FaceSets) {
          // the set's color as 8-bit red, green and blue in one number (exact in a float); -100: in no set
          std::unordered_map<uint32_t, float> col;
          for (auto& fs : doc.faceSets) {
            glm::ivec3 c = glm::clamp(glm::ivec3(glm::round(fs.color * 255.f)), glm::ivec3(0), glm::ivec3(255));
            col[fs.id] = float(c.x * 65536 + c.y * 256 + c.z);
          }
          for (uint32_t f = 0; f < F && f < doc.faceSetOf.size(); ++f)
            if (auto it = col.find(doc.faceSetOf[f]); doc.faceSetOf[f] && it != col.end()) v[f] = it->second;
        } else if (opt.mode == ViewMode::MeshCheck) {
          // 0 fine, 1 a note, 2 a warning, 3 a problem
          const MeshCheckReport& mc = doc.meshCheck();
          for (uint32_t f = 0; f < F && f < mc.faceSeverity.size(); ++f) v[f] = float(mc.faceSeverity[f]);
        } else if (opt.mode == ViewMode::Symmetry) {
          // distance to the mirrored surface, in tolerances (0.1% of the model's size)
          const SymmetryReport& sr = doc.symmetry();
          for (uint32_t f = 0; f < F && f < sr.faceDist.size(); ++f)
            v[f] = sr.faceDist[f] < 0 ? -100.f : sr.faceDist[f] / std::max(sr.tolerance, 1e-9f);
        } else if (opt.mode == ViewMode::TexelDensity) {
          float target = opt.densityTarget > 0 ? opt.densityTarget
                                               : uv::densityTargets(m, doc.tileRes()).recommended();
          auto d = uv::faceDensity(m);
          for (uint32_t f = 0; f < F; ++f)
            if (d[f] > 0 && target > 0) v[f] = std::log2(d[f] / target);
        } else {
          auto st = uv::faceStretch(m);
          for (uint32_t f = 0; f < F; ++f)
            if (st[f] > 0) v[f] = st[f];
        }
        if (faceData_.width() != W || faceData_.height() != H) {
          faceData_ = Texture2D(W, H, GL_R32F, 1);
          faceData_.setFilter(GL_NEAREST, GL_NEAREST);
        }
        faceData_.uploadRaw(GL_RED, GL_FLOAT, v.data());
      }
      s.tex("uFaceData", 10, faceData_.id());
      s.set("uFaceDataW", W);
    }
    // retopology: patches / density over the model
    const Mesh& rm = doc.mesh;
    int rv = 0;
    if (opt.retopoView == 1 && opt.retopoLabels && opt.retopoLabels->size() == rm.faceCount()) rv = 1;
    if (opt.retopoView == 2 && opt.retopoDensity && opt.retopoDensity->size() == rm.positions.size()) rv = 2;
    if (rv) {
      uint32_t F = rm.faceCount();
      const int W = 2048;
      int H = std::max(1, int((F + W - 1) / W));
      uint64_t key = opt.retopoKey * 0x9E3779B97F4A7C15ull ^ uint64_t(rv) ^ (uint64_t(F) << 32);
      if (key != retopoDataKey_ || retopoData_.width() != W || retopoData_.height() != H) {
        retopoDataKey_ = key;
        std::vector<float> v(size_t(W) * H, 0.f);
        if (rv == 1) {
          parallelFor(F, [&](size_t f) { v[f] = float((*opt.retopoLabels)[f]); });
        } else {
          const auto& dv = *opt.retopoDensity;
          parallelFor(F, [&](size_t f) {
            float sum = 0;
            for (uint32_t l = rm.faceStart[f]; l < rm.faceStart[f + 1]; ++l) sum += dv[rm.loopVert[l]];
            v[f] = std::log2(std::max(sum / float(rm.faceStart[f + 1] - rm.faceStart[f]), 1e-3f));
          });
        }
        if (retopoData_.width() != W || retopoData_.height() != H) {
          retopoData_ = Texture2D(W, H, GL_R32F, 1);
          retopoData_.setFilter(GL_NEAREST, GL_NEAREST);
        }
        retopoData_.uploadRaw(GL_RED, GL_FLOAT, v.data());
      }
      s.tex("uRetopoData", 12, retopoData_.id());
    }
    s.set("uRetopoView", rv);
    s.set("uRetopoSel", opt.retopoSelectedZone);
    s.set("uFaceDataW", 2048);
    // the quads, when there are some
    bool quads = opt.retopoQuads && opt.retopoQuads->faceCount() > 0;
    if (quads && (!retopoGpu_ || retopoGpuKey_ != opt.retopoQuadsKey)) {
      retopoGpu_ = std::make_shared<GpuMesh>();
      retopoGpu_->upload(*opt.retopoQuads);
      retopoGpuKey_ = opt.retopoQuadsKey;
    }
    s.use();
    if (quads && opt.retopoQuadsOnly) {
      s.set("uRetopoView", 3);
      s.set("uSelTint", 0.f);
      glProgramUniform1ui(s.id(), s.loc("uHoverFace"), 0xFFFFFFFFu);
      s.use();
      retopoGpu_->drawTriangles();
    } else {
      // see-through parts (0.26): the opaque parts first, then the others blended over them
      bool see = opt.mode == ViewMode::Material && (t.extraUsed & 64);
      if (see) s.set("uAlphaPass", 1);
      const GpuMesh& g = opt.meshOverride ? *opt.meshOverride : doc.gpu;
      g.drawTriangles();
      if (see) drawSeeThrough(s, g);
    }
    // (1.2.2) the other objects' see-through parts, over every object's opaque parts (hair over the body it hangs on,
    // whichever of them is painted)
    if (sceneObjects && opt.mode != ViewMode::PathTraced)
      sceneObjects([&](const Document& d) { drawSceneObject(d, env, cam, aspect, opt, true); });
    // (0.39) the path tracer's picture over the raster one, whose depth stays for the lines drawn next
    if (opt.mode == ViewMode::PathTraced) {
      uint64_t key = 1469598103934665603ull;
      auto mixIn = [&](uint64_t v) { key = (key ^ v) * 1099511628211ull; };
      for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) {
          uint32_t b;
          std::memcpy(&b, &vpm[c][r], 4);
          mixIn(b);
        }
      uint32_t er, ei, cl;
      std::memcpy(&er, &env.rotation, 4), std::memcpy(&ei, &env.intensity, 4), std::memcpy(&cl, &opt.pathTrace.clamp, 4);
      mixIn(uint64_t(w) << 32 | uint32_t(h)), mixIn(env.serial()), mixIn(er), mixIn(ei), mixIn(cl);
      mixIn(doc.tex.compositeCount), mixIn(doc.gpu.serial()), mixIn(doc.bvh.serial());
      mixIn(uint64_t(opt.pathTrace.bounces) << 1 | (opt.pathTrace.ground ? 1 : 0));
      // (1.2) the scene's other objects are traced with it
      std::vector<const Document*> shown = shownObjects(doc);
      if (shown.size() > 1) mixIn(PathTracer::sceneKey(shown));
      pathRender_.setScene(shown), pathView_.setScene(shown);
      if (opt.pathSamplesNow > 0) {
        pathRender_.render(doc, env, cam, w, h, opt.pathTrace, opt.pathSamplesNow);
        pathRender_.draw(env, cam, w, h, opt.envBackground, opt.backgroundBlur, opt.transparentBackground);
        pathRender_.release();
      } else if (opt.pathSamplesNow < 0) {
        pathRender_.draw(env, cam, w, h, opt.envBackground, opt.backgroundBlur, opt.transparentBackground);
      } else {
        pathView_.progress(doc, env, cam, w, h, opt.pathTrace, key);
        pathView_.draw(env, cam, w, h, opt.envBackground, opt.backgroundBlur, opt.transparentBackground);
      }
    }
    // (0.34) collision shapes: green edges, fainter where the model hides them
    if (opt.collisionLines && !opt.collisionLines->empty()) {
      if (collisionKey_ != opt.collisionKey || !collisionVao_) {
        if (!collisionVbo_) glCreateBuffers(1, &collisionVbo_);
        if (!collisionVao_) {
          glCreateVertexArrays(1, &collisionVao_);
          glVertexArrayVertexBuffer(collisionVao_, 0, collisionVbo_, 0, sizeof(vec3));
          glEnableVertexArrayAttrib(collisionVao_, 0);
          glVertexArrayAttribFormat(collisionVao_, 0, 3, GL_FLOAT, GL_FALSE, 0);
          glVertexArrayAttribBinding(collisionVao_, 0, 0);
        }
        glNamedBufferData(collisionVbo_, GLsizeiptr(opt.collisionLines->size() * sizeof(vec3)), opt.collisionLines->data(),
                          GL_DYNAMIC_DRAW);
        collisionCount_ = (GLsizei)opt.collisionLines->size();
        collisionKey_ = opt.collisionKey;
      }
      glEnable(GL_BLEND);
      glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
      glDepthMask(GL_FALSE);
      Shader& ls = ShaderCache::get("lines.vs.glsl", "lines.fs.glsl");
      ls.set("uViewProj", lvpm);
      ls.set("uSpace", 0);
      ls.set("uDepthOffset", 0.f);
      ls.set("uCamPos", lc.eye());
      ls.set("uPull", 0.002f * lc.eyeDistance());
      glProgramUniform1ui(ls.id(), ls.loc("uSelFlag"), 0u);
      glBindVertexArray(collisionVao_);
      glDepthFunc(GL_GREATER);
      ls.set("uColor", vec4(0.25f, 0.95f, 0.35f, 0.35f));
      ls.use();
      glLineWidth(1.f);
      glDrawArrays(GL_LINES, 0, collisionCount_);
      glDepthFunc(GL_LEQUAL);
      ls.set("uColor", vec4(0.3f, 1.f, 0.4f, 0.95f));
      ls.use();
      glLineWidth(2.f);
      glDrawArrays(GL_LINES, 0, collisionCount_);
      glLineWidth(1.f);
      glDepthFunc(GL_LESS);
      glDepthMask(GL_TRUE);
      glDisable(GL_BLEND);
    }
    // zone lines (where the flow changes; confirmed ones darker) and flow strokes: bright lines on the model,
    // pulled a little toward the eye (a distance, not a share of the depth range: that let them show through
    // the model far from the camera); behind the model shown, darkened or hidden (0.22)
    size_t nl = opt.retopoLines ? opt.retopoLines->size() : 0, nd = opt.retopoLinesDone ? opt.retopoLinesDone->size() : 0,
           ns = opt.retopoStrokes ? opt.retopoStrokes->size() : 0, nm = opt.retopoMarks ? opt.retopoMarks->size() : 0;
    if (nl + nd + ns + nm > 0) {
      if (retopoLineKey_ != opt.retopoLinesKey || !retopoLineVao_ || GLsizei(nl) != retopoLineCount_ || GLsizei(nd) != retopoDoneCount_ ||
          GLsizei(ns) != retopoStrokeCount_ || GLsizei(nm) != retopoMarkCount_) {
        if (!retopoLineVbo_) glCreateBuffers(1, &retopoLineVbo_);
        if (!retopoLineVao_) {
          glCreateVertexArrays(1, &retopoLineVao_);
          glVertexArrayVertexBuffer(retopoLineVao_, 0, retopoLineVbo_, 0, sizeof(vec3));
          glEnableVertexArrayAttrib(retopoLineVao_, 0);
          glVertexArrayAttribFormat(retopoLineVao_, 0, 3, GL_FLOAT, GL_FALSE, 0);
          glVertexArrayAttribBinding(retopoLineVao_, 0, 0);
        }
        std::vector<vec3> all;
        all.reserve(nl + nd + ns + nm);
        if (nl) all.insert(all.end(), opt.retopoLines->begin(), opt.retopoLines->end());
        if (nd) all.insert(all.end(), opt.retopoLinesDone->begin(), opt.retopoLinesDone->end());
        if (ns) all.insert(all.end(), opt.retopoStrokes->begin(), opt.retopoStrokes->end());
        if (nm) all.insert(all.end(), opt.retopoMarks->begin(), opt.retopoMarks->end());
        glNamedBufferData(retopoLineVbo_, GLsizeiptr(all.size() * sizeof(vec3)), all.data(), GL_DYNAMIC_DRAW);
        retopoLineCount_ = (GLsizei)nl, retopoDoneCount_ = (GLsizei)nd, retopoStrokeCount_ = (GLsizei)ns, retopoMarkCount_ = (GLsizei)nm;
        retopoLineKey_ = opt.retopoLinesKey;
      }
      glEnable(GL_BLEND);
      glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
      glDepthMask(GL_FALSE);
      Shader& ls = ShaderCache::get("lines.vs.glsl", "lines.fs.glsl");
      ls.set("uViewProj", lvpm);
      ls.set("uSpace", 0);
      ls.set("uDepthOffset", 0.f);
      ls.set("uCamPos", lc.eye());
      ls.set("uPull", 0.004f * lc.eyeDistance());
      glProgramUniform1ui(ls.id(), ls.loc("uSelFlag"), 0u);
      glBindVertexArray(retopoLineVao_);
      struct Range {
        GLint first;
        GLsizei count;
        vec4 col;
      };
      const Range ranges[] = {{0, retopoLineCount_, vec4(1.f, 0.78f, 0.15f, 0.95f)},
                              {retopoLineCount_, retopoDoneCount_, vec4(0.58f, 0.40f, 0.09f, 1.f)},
                              {retopoLineCount_ + retopoDoneCount_, retopoStrokeCount_, vec4(0.35f, 0.86f, 1.f, 0.95f)},
                              {retopoLineCount_ + retopoDoneCount_ + retopoStrokeCount_, retopoMarkCount_, vec4(1.f, 0.15f, 0.1f, 1.f)}};
      auto drawAll = [&](float dim, float alpha, float width) {
        glLineWidth(width);
        for (const Range& r : ranges) {
          if (!r.count) continue;
          ls.set("uColor", vec4(vec3(r.col) * dim, r.col.w * alpha));
          ls.use();
          glDrawArrays(GL_LINES, r.first, r.count);
        }
      };
      if (opt.retopoLinesBehind == 0) {
        glDisable(GL_DEPTH_TEST);
        drawAll(1.f, 1.f, 3.f);
        glEnable(GL_DEPTH_TEST);
      } else {
        if (opt.retopoLinesBehind == 1) {
          glDepthFunc(GL_GREATER);  // (the parts behind the model: dark, thin and faint)
          drawAll(0.45f, 0.45f, 2.f);
        }
        glDepthFunc(GL_LEQUAL);
        drawAll(1.f, 1.f, 3.f);
      }
      glLineWidth(1.f);
      glDepthMask(GL_TRUE);
      glDisable(GL_BLEND);
      glDepthFunc(GL_LESS);
    }
    if (quads) {
      glEnable(GL_BLEND);
      glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
      glDepthMask(GL_FALSE);
      glDepthFunc(GL_LEQUAL);
      Shader& ls = ShaderCache::get("lines.vs.glsl", "lines.fs.glsl");
      ls.set("uViewProj", lvpm);
      ls.set("uSpace", 0);
      // (pulled a little toward the eye: the far side's edges stay hidden behind the model)
      ls.set("uDepthOffset", 0.f);
      ls.set("uCamPos", lc.eye());
      ls.set("uPull", 0.003f * lc.eyeDistance());
      glProgramUniform1ui(ls.id(), ls.loc("uSelFlag"), 0u);
      vec4 col = opt.retopoQuadsOnly ? vec4(0.05f, 0.07f, 0.12f, 0.85f)
                 : opt.retopoView ? vec4(0.02f, 0.03f, 0.06f, 0.75f) : vec4(0.1f, 0.85f, 1.f, 0.85f);
      if (opt.retopoQuadsPreview && !opt.retopoQuadsOnly) col = opt.retopoView ? vec4(0.95f, 0.97f, 1.f, 0.55f) : vec4(0.6f, 0.92f, 1.f, 0.55f);
      ls.set("uColor", col);
      ls.use();
      glLineWidth(1.f);
      const bool loops = opt.retopoLoops && opt.retopoLoopPts && !opt.retopoLoopPts->empty() && opt.retopoLoopCols &&
                         opt.retopoLoopCols->size() == opt.retopoLoopPts->size();
      if (loops) ls.set("uColor", vec4(vec3(col), col.w * 0.6f)), ls.use();  // (dimmer under the loop and the poles)
      retopoGpu_->drawEdges();
      ls.set("uPull", 0.f);
      if (loops) {
        // each edge loop in a color of its own, and the poles as small crosses
        if (retopoLoopKey_ != opt.retopoLoopsKey || !retopoLoopVao_) {
          if (!retopoLoopVbo_) glCreateBuffers(1, &retopoLoopVbo_);
          if (!retopoLoopVao_) {
            glCreateVertexArrays(1, &retopoLoopVao_);
            glVertexArrayVertexBuffer(retopoLoopVao_, 0, retopoLoopVbo_, 0, sizeof(vec3) + sizeof(vec4));
            glEnableVertexArrayAttrib(retopoLoopVao_, 0);
            glVertexArrayAttribFormat(retopoLoopVao_, 0, 3, GL_FLOAT, GL_FALSE, 0);
            glVertexArrayAttribBinding(retopoLoopVao_, 0, 0);
            glEnableVertexArrayAttrib(retopoLoopVao_, 1);
            glVertexArrayAttribFormat(retopoLoopVao_, 1, 4, GL_FLOAT, GL_FALSE, sizeof(vec3));
            glVertexArrayAttribBinding(retopoLoopVao_, 1, 0);
          }
          std::vector<float> buf;
          buf.reserve(opt.retopoLoopPts->size() * 7);
          for (size_t i = 0; i < opt.retopoLoopPts->size(); ++i) {
            const vec3& p = (*opt.retopoLoopPts)[i];
            const vec4& c = (*opt.retopoLoopCols)[i];
            buf.insert(buf.end(), {p.x, p.y, p.z, c.x, c.y, c.z, c.w});
          }
          glNamedBufferData(retopoLoopVbo_, GLsizeiptr(buf.size() * sizeof(float)), buf.data(), GL_DYNAMIC_DRAW);
          retopoLoopCount_ = (GLsizei)opt.retopoLoopPts->size();
          retopoLoopKey_ = opt.retopoLoopsKey;
        }
        Shader& lp = ShaderCache::get("retopo_loops.vs.glsl", "retopo_loops.fs.glsl");
        lp.set("uViewProj", lvpm);
        lp.set("uCamPos", lc.eye());
        lp.set("uPull", 0.004f * lc.eyeDistance());
        lp.set("uAlpha", opt.retopoQuadsPreview ? 0.7f : 1.f);
        lp.use();
        glBindVertexArray(retopoLoopVao_);
        glLineWidth(4.f);
        glDrawArrays(GL_LINES, 0, retopoLoopCount_);
        glLineWidth(1.f);
      }
      glLineWidth(1.f);
      glDepthMask(GL_TRUE);
      glDisable(GL_BLEND);
      glDepthFunc(GL_LESS);
    }
  }
  // overlays
  if (opt.wireframe || opt.seams) {
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    glDepthFunc(GL_LEQUAL);
    Shader& s = ShaderCache::get("lines.vs.glsl", "lines.fs.glsl");
    s.set("uViewProj", lvpm);
    s.set("uSpace", 0);
    // (0.40.1) a couple of pixels toward the camera, not a share of the depth range: that came to a third of the
    // model and more, so the far side's lines showed through
    s.set("uDepthOffset", 0.f);
    s.set("uPull", 0.f);
    s.set("uCamPos", lc.eye());
    s.set("uPixel", lc.pixelWorld(float(h)));
    s.set("uOrtho", lc.ortho ? 1 : 0);
    s.set("uToEye", -lc.forward());
    s.set("uPullPx", 1.5f);
    glProgramUniform1ui(s.id(), s.loc("uSelFlag"), 0u);
    s.use();
    if (opt.wireframe) {
      s.set("uColor", vec4(0.05f, 0.05f, 0.06f, 0.55f));
      s.set("uSelColor", vec4(1.f, 0.6f, 0.2f, 0.9f));
      (opt.meshOverride ? *opt.meshOverride : doc.gpu).drawEdges();
    }
    if (opt.seams && !opt.meshOverride && opt.mode != ViewMode::BakeCheck) {  // (the check's red is its guesses)
      glLineWidth(2.f);
      s.set("uColor", vec4(0.95f, 0.15f, 0.15f, 0.95f));
      s.set("uPullPx", 2.5f);
      doc.gpu.drawSeams();
      glLineWidth(1.f);
    }
    s.set("uPullPx", 0.f);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
  }
  glDisable(GL_DEPTH_TEST);
  ++frames;
}

std::vector<const Document*> Renderer::shownObjects(const Document& active) const {
  std::vector<const Document*> v;
  if (active.hasMesh() && !active.objectHidden) v.push_back(&active);
  if (sceneObjects) sceneObjects([&](const Document& d) {
      if (d.hasMesh()) v.push_back(&d);
    });
  return v;
}

// (1.2.2) A model's see-through parts, after every opaque part is drawn: first the depth of the parts at least half
// opaque, so the nearest layer of hair cards (or leaves, or lace) hides the layers behind it whatever order the faces
// come in, then their colors blended over what is there. (Blending alone let back cards land on front ones.)
void Renderer::drawSeeThrough(Shader& s, const GpuMesh& g) {
  s.set("uAlphaPass", 3);
  s.use();
  glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
  g.drawTriangles();
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  s.set("uAlphaPass", 2);
  s.use();
  glDepthFunc(GL_LEQUAL);
  glEnable(GL_BLEND);
  glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  glDepthMask(GL_FALSE);
  g.drawTriangles();
  glDepthMask(GL_TRUE);
  glDisable(GL_BLEND);
  glDepthFunc(GL_LESS);
  s.set("uAlphaPass", 0);
}

void Renderer::drawSceneObject(const Document& doc, const Environment& env, const Camera& cam, float aspect,
                               const ViewportOptions& opt, bool seeThrough) {
  if (!doc.hasMesh() || !doc.gpu.valid()) return;
  const TextureSet& t = doc.tex;
  // (1.2.2) two calls: the opaque parts, then (seeThrough) the see-through parts once every object's opaque parts
  // are drawn
  ViewMode vm = opt.mode;
  if (vm == ViewMode::TexelDensity || vm == ViewMode::UVStretch || vm == ViewMode::Symmetry || vm == ViewMode::MeshCheck ||
      vm == ViewMode::LayerMask || vm == ViewMode::BakeCheck || vm == ViewMode::FaceSets)
    vm = ViewMode::Solid;
  bool see = vm == ViewMode::Material && (t.extraUsed & 64);
  if (seeThrough && !see) return;
  Shader& s = ShaderCache::get("mesh.vs.glsl", "pbr.fs.glsl");
  s.set("uModel", doc.toWorld());
  s.set("uViewProj", cam.viewProj(aspect));
  s.tex("uColorTex", 0, t.finColor.id());
  s.tex("uRMHTex", 1, t.finRMH.id());
  s.tex("uNormalTex", 2, t.finNormal.id());
  s.tex("uEmissiveTex", 3, t.finEmissive.id());
  s.tex("uAOMap", 4, t.aoView().id());
  s.tex("uCurvMap", 5, t.curvView().id());
  s.tex("uMaskTex", 6, t.white.id());
  env.bind(s, 7, 8);
  s.set("uEnvIntensity", env.intensity);
  s.set("uEnvRot", env.rotationMatrix());
  s.set("uExposure", env.exposure);
  s.set("uCamPos", cam.eye());
  // the views that show the active object's own data (its faces' numbers, its layer's mask, its bake-down): the
  // others plain gray
  ViewMode m = opt.mode;
  if (m == ViewMode::TexelDensity || m == ViewMode::UVStretch || m == ViewMode::Symmetry || m == ViewMode::MeshCheck ||
      m == ViewMode::LayerMask || m == ViewMode::BakeCheck || m == ViewMode::FaceSets)
    m = ViewMode::Solid;
  s.set("uView", (int)m);
  s.set("uUseAO", opt.useAO && t.hasAO() && t.bakedShown(0) ? 1 : 0);
  s.set("uSelTint", 0.f);
  s.set("uShowPins", 0);
  glProgramUniform1ui(s.id(), s.loc("uHoverFace"), 0xFFFFFFFFu);
  s.set("uShowUVIslands", opt.islandTint ? 1 : 0);
  s.set("uIsolateTile", -1);
  s.tex("uMatIdTex", 9, t.matIdMap.id());
  s.tex("uThickMap", 10, t.thickView().id());
  s.tex("uBentMap", 11, t.bentMap.id());
  s.tex("uExtraTex", 13, t.finExtra.id());
  s.tex("uBakeCheck", 14, t.white.id());
  s.set("uHasBakeCheck", 0);
  s.set("uRetopoView", 0);
  if (seeThrough) {
    drawSeeThrough(s, doc.gpu);
    return;
  }
  s.set("uAlphaPass", see ? 1 : 0);
  s.use();
  doc.gpu.drawTriangles();
  s.set("uAlphaPass", 0);
}

mat4 Renderer::uvViewMatrix(const UVViewOptions& o) {
  // uv -> clip: ((uv * scale + offset) * 2 - 1)
  mat4 m(1);
  m = glm::translate(m, vec3(-1, -1, 0));
  m = glm::scale(m, vec3(2, 2, 1));
  m = glm::translate(m, vec3(o.offset, 0));
  m = glm::scale(m, vec3(o.scale, 1));
  return m;
}

void Renderer::renderUV(Document& doc, RenderTarget& rt, int w, int h, const UVViewOptions& opt, GLuint tex) {
  rt.ensure(w, h, GL_RGBA8);
  gl::FramebufferScope fs(rt.fbo.id());
  gl::ViewportScope vp(0, 0, w, h);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  glClearColor(0.07f, 0.075f, 0.08f, 1);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  {
    Shader& s = ShaderCache::get("fullscreen.vs.glsl", "uvview.fs.glsl");
    s.set("uView", vec4(opt.scale, opt.offset));
    s.tex("uTex", 0, tex ? tex : doc.tex.white.id());
    s.set("uMode", tex ? opt.texMode : 6);
    s.set("uDim", opt.dim);
    s.set("uViewportPx", vec2(w, h));
    s.set("uTiling", opt.tiling ? 1 : 0);
    const TileLayout& tl = doc.mesh.tiles;
    int tile = tl.active() ? std::clamp(opt.tile, 0, tl.count() - 1) : 0;
    s.set("uAtlasCell", tl.active() ? vec3(tl.cellOrigin(tile), tl.cellSize()) : vec3(0, 0, 1));
    s.use();
    drawFullscreenTriangle();
  }
  if (!doc.hasMesh()) return;
  // mesh UVs as authored, shifted so the shown tile's square is the view's 0..1
  UVViewOptions mo = opt;
  const TileLayout& tl = doc.mesh.tiles;
  int tile = tl.active() ? std::clamp(opt.tile, 0, tl.count() - 1) : -1;
  if (tile >= 0) mo.offset = opt.offset - vec2(tl.uvOrigin(tile)) * opt.scale;
  mat4 m = uvViewMatrix(mo);
  vec4 uvXform(mo.scale, mo.offset);
  auto faceShader = [&](Shader& s) {
    s.set("uUseMeshUV", 1);
    s.set("uTile", tile);
  };
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  if (opt.faceSets && !opt.distortion && !doc.faceSets.empty() && doc.faceSetOf.size() == doc.mesh.faceCount()) {
    // (1.2) each face set's faces in its color
    uint64_t k = 1469598103934665603ull ^ doc.gpu.serial();
    for (uint32_t id : doc.faceSetOf) k = (k ^ id) * 1099511628211ull;
    for (auto& fs : doc.faceSets) {
      uint32_t c[3];
      std::memcpy(c, &fs.color, sizeof c);
      k = (k ^ fs.id ^ (uint64_t(c[0]) << 21) ^ (uint64_t(c[1]) << 42) ^ c[2]) * 1099511628211ull;
    }
    if (k != uvAuxKey_ || doc.gpu.auxSerial() != uvAuxSerial_) {
      std::unordered_map<uint32_t, vec3> col;
      for (auto& fs : doc.faceSets) col[fs.id] = fs.color;
      std::vector<vec4> aux(doc.mesh.loopCount(), vec4(0));
      for (uint32_t f = 0; f < doc.mesh.faceCount(); ++f)
        if (auto it = col.find(doc.faceSetOf[f]); doc.faceSetOf[f] && it != col.end())
          for (uint32_t l = doc.mesh.faceStart[f]; l < doc.mesh.faceStart[f + 1]; ++l) aux[l] = vec4(it->second, 1.f);
      doc.gpu.setAux(aux);
      uvAuxKey_ = k, uvAuxSerial_ = doc.gpu.auxSerial();
    }
    Shader& s = ShaderCache::get("uvraster.vs.glsl", "uvfaces.fs.glsl");
    s.set("uUVTransform", uvXform);
    s.set("uUseAuxAsUV", 0);
    faceShader(s);
    s.set("uMode", 3);
    s.set("uColor", vec4(1, 1, 1, 0.45f));
    s.use();
    doc.gpu.drawTriangles();
    s.set("uUseMeshUV", 0);
    s.set("uTile", -1);
  }
  if (opt.distortion) {
    uvAuxKey_ = 0;  // (the aux holds the distortion now)
    std::vector<float> dist = uv::areaDistortion(doc.mesh);
    std::vector<vec4> aux(doc.mesh.loopCount());
    for (uint32_t f = 0; f < doc.mesh.faceCount(); ++f)
      for (uint32_t l = doc.mesh.faceStart[f]; l < doc.mesh.faceStart[f + 1]; ++l) aux[l] = vec4(dist[f], 0, 0, 0);
    doc.gpu.setAux(aux);
    Shader& s = ShaderCache::get("uvraster.vs.glsl", "uvfaces.fs.glsl");
    s.set("uUVTransform", uvXform);
    s.set("uUseAuxAsUV", 0);
    faceShader(s);
    s.set("uMode", 1);
    s.set("uColor", vec4(1, 1, 1, 0.45f));
    s.use();
    doc.gpu.drawTriangles();
    s.set("uUseMeshUV", 0);
    s.set("uTile", -1);
  }
  if (opt.selection) {
    Shader& s = ShaderCache::get("uvraster.vs.glsl", "uvfaces.fs.glsl");
    s.set("uUVTransform", uvXform);
    s.set("uUseAuxAsUV", 0);
    faceShader(s);
    s.set("uMode", 0);
    s.set("uColor", vec4(1.f, 0.55f, 0.1f, 0.28f));
    glProgramUniform1ui(s.id(), s.loc("uSelFlag"), 1u);
    s.use();
    doc.gpu.drawTriangles();
    s.set("uUseMeshUV", 0);
    s.set("uTile", -1);
  }
  Shader& ls = ShaderCache::get("lines.vs.glsl", "lines.fs.glsl");
  ls.set("uViewProj", m);
  ls.set("uSpace", 1);
  ls.set("uTile", tile);
  ls.set("uDepthOffset", 0.f);
  glProgramUniform1ui(ls.id(), ls.loc("uSelFlag"), 1u);
  ls.use();
  if (opt.wireframe) {
    ls.set("uColor", vec4(0.3f, 0.85f, 1.0f, 0.8f));
    ls.set("uSelColor", vec4(1.f, 0.65f, 0.25f, 1.f));
    doc.gpu.drawEdges();
  }
  if (opt.seams) {
    glLineWidth(2.f);
    ls.set("uColor", vec4(1.f, 0.2f, 0.2f, 0.9f));
    ls.set("uSelColor", vec4(1.f, 0.2f, 0.2f, 0.9f));
    doc.gpu.drawSeams();
    glLineWidth(1.f);
  }
  glDisable(GL_BLEND);
}

std::vector<uint8_t> Renderer::readPixels(const RenderTarget& rt) {
  int w = rt.width(), h = rt.height();
  std::vector<uint8_t> px(size_t(w) * h * 4);
  rt.color.downloadRaw(GL_RGBA, GL_UNSIGNED_BYTE, px.data(), px.size());
  // flip to top-down
  std::vector<uint8_t> row(size_t(w) * 4);
  for (int y = 0; y < h / 2; ++y) {
    uint8_t* a = &px[size_t(y) * w * 4];
    uint8_t* b = &px[size_t(h - 1 - y) * w * 4];
    std::memcpy(row.data(), a, row.size());
    std::memcpy(a, b, row.size());
    std::memcpy(b, row.data(), row.size());
  }
  return px;
}

}  // namespace patina
