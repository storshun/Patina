// (1.1) The bake-down: the layers of a painted high-poly model moved onto its low-poly version (Document::bakeDown).
#include <string>

#include "core/BakeProjection.h"
#include "core/Log.h"
#include "core/Progress.h"
#include "engine/Document.h"
#include "engine/HighPolyBake.h"
#include "gfx/GL.h"
#include "gfx/Shader.h"
#include "gfx/Texture.h"

namespace patina {

namespace {

// a copy with every mip level (the views sample the final maps with mips)
void snapshot(const Texture2D& src, Texture2D& dst) {
  if (!src.valid()) return;
  dst.create(src.width(), src.height(), src.format(), src.levels());
  dst.copyFrom(src);
  if (src.levels() > 1) dst.generateMips();
}

}  // namespace

bool Document::bakeDown(Mesh&& lowIn, const BakeDownOptions& o, json* report, std::string* err) {
  auto fail = [&](const std::string& why) {
    if (err) *err = why;
    return false;
  };
  if (!hasMesh() || tex.resolution() <= 0) return fail("Open the painted high-poly model first");
  Mesh low = std::move(lowIn);
  if (low.tris.empty() && low.faceCount() > 0) low.finalize();
  if (low.faceCount() == 0 || low.tris.empty()) return fail("The low-poly model has no faces");
  if (beforeMeshChange) beforeMeshChange();
  double t0 = nowSeconds();
  progress::Scope prog("Baking the layers down onto the low-poly model");
  if (bvh.empty()) bvh.build(mesh);

  // ---- the low's UVs and texture tiles, as for a model opened afresh
  bool uvsMade = prepareUVs(low, true);
  int tileR = o.resolution > 0 ? o.resolution : tileRes();
  while (tileR > 256 && !atlasFits(tileR, low.tiles.grid)) tileR /= 2;
  const int R = tileR * std::max(1, low.tiles.grid);
  if (R > 8192)
    return fail("The low-poly model's textures would be " + std::to_string(R) + " px across all its tiles; the "
                "bake-down needs 8192 or less. Lower the resolution.");
  const int S = R > 4096 ? std::min(o.samples, 4) : std::clamp(o.samples, 1, 16);

  // ---- where every texel of the low lies on the high (the document is not touched until this is done)
  bool normals = false;
  for (auto& l : layers) normals |= l->hasChannel((int)Channel::Normal);
  Projection p;
  {
    GpuMesh lowGpu;
    lowGpu.upload(low);
    Texture2D pos(R, R, GL_RGBA32F, 1), nrm(R, R, GL_RGBA16F, 1), tan(R, R, GL_RGBA16F, 1);
    TextureSet::rasterizeGeometry(lowGpu, pos, nrm, tan);
    ProjectionOptions po;
    po.samples = S;
    po.distance = o.distance;
    po.lookups = true;
    po.frames = normals;
    po.bakeData = o.bakeHigh;
    if (!projectTexels(pos, nrm, low, mesh, bvh, po, p, err)) return false;
  }
  double tProject = nowSeconds() - t0;

  // ---- the high as it looks now, to compare with
  if (compositeDirty && tex.materials) updateComposite(*tex.materials);
  auto before = std::make_unique<BakeDownBefore>();
  snapshot(tex.finColor, before->color);
  snapshot(tex.finRMH, before->rmh);
  snapshot(tex.finNormal, before->normal);
  snapshot(tex.finEmissive, before->emissive);
  snapshot(tex.finExtra, before->extra);
  before->extraUsed = tex.extraUsed;

  // ---- paint at island borders: the gutters filled from the islands, so taps there stay clean (the high's layout)
  for (auto& l : layers) {
    for (auto& c : l->channels)
      if (c && c->valid()) tex.dilateInPlace(*c);
    if (l->mask) tex.dilateInPlace(*l->mask);
  }
  // what the high had baked, and what the layers read
  bool ao = tex.hasAO(), curv = tex.hasCurvature(), thick = tex.hasThickness();
  for (auto& l : layers) ao |= l->needsAO(), curv |= l->needsCurvature(), thick |= l->needsThickness();

  // ---- the low becomes the model
  std::vector<uint32_t> highSets = faceSetOf;  // (1.2) the face sets follow the faces
  Mesh high = std::move(mesh);
  mesh = std::move(low);
  gpu.upload(mesh);
  Bvh highBvh = std::move(bvh);
  bvh = Bvh();
  bvh.build(mesh);
  carryFaceSets(high, highBvh, highSets);
  tex.materialIdsFromHigh = false;
  tex.setHighNormal(false);
  tex.allocate(R);
  tex.bakeMeshMaps(mesh, gpu);

  // ---- every layer's textures through the lookups: two points per RGBA16 texture
  const size_t N = size_t(R) * R;
  const int pairs = (S + 1) / 2;
  std::vector<std::unique_ptr<Texture2D>> looks;
  {
    std::vector<uint16_t> buf(N * 4);
    for (int j = 0; j < pairs; ++j) {
      for (int h = 0; h < 2; ++h) {
        int k = 2 * j + h;
        for (size_t i = 0; i < N; ++i) {
          buf[i * 4 + h * 2] = k < S ? p.look[(size_t(k) * N + i) * 2] : 0xFFFF;
          buf[i * 4 + h * 2 + 1] = k < S ? p.look[(size_t(k) * N + i) * 2 + 1] : 0xFFFF;
        }
      }
      auto t = std::make_unique<Texture2D>(R, R, GL_RGBA16, 1);
      t->setFilter(GL_NEAREST, GL_NEAREST);
      t->uploadRaw(GL_RGBA, GL_UNSIGNED_SHORT, buf.data());
      looks.push_back(std::move(t));
    }
    std::vector<uint16_t>().swap(p.look);
  }
  Texture2D highN, highT;
  if (normals) {
    highN.create(R, R, GL_RGBA16F, 1);
    highT.create(R, R, GL_RGBA16F, 1);
    highN.uploadRaw(GL_RGBA, GL_FLOAT, p.highNormal.data());
    highT.uploadRaw(GL_RGBA, GL_FLOAT, p.highTangent.data());
  }
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glDisable(GL_BLEND);
  glDisable(GL_SCISSOR_TEST);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  Shader& sh = ShaderCache::get("fullscreen.vs.glsl", "post.fs.glsl", {"POST_BAKEDOWN"});
  int moved = 0;
  auto move = [&](std::unique_ptr<Texture2D>& t, int mode) {
    if (!t || !t->valid()) return;
    auto n = std::make_unique<Texture2D>(R, R, t->format(), 1);
    Framebuffer fbo;
    fbo.attach(0, *n);
    fbo.setDrawBuffers(1);
    gl::FramebufferScope fs(fbo.id());
    gl::ViewportScope vp(0, 0, R, R);
    sh.tex("uSrc", 0, t->id());
    for (int j = 0; j < 8; ++j) sh.tex(("uLook" + std::to_string(j)).c_str(), 1 + j, looks[std::min(j, pairs - 1)]->id());
    sh.set("uPairs", pairs);
    sh.set("uMode", mode);
    sh.tex("uHighN", 9, normals ? highN.id() : tex.white.id());
    sh.tex("uHighT", 10, normals ? highT.id() : tex.white.id());
    sh.tex("uNrmMap", 11, tex.nrmMap.id());
    sh.tex("uTanMap", 12, tex.tanMap.id());
    sh.set("uOverHigh", o.bakeHigh ? 1 : 0);
    sh.use();
    drawFullscreenTriangle();
    t = std::move(n);
    ++moved;
  };
  auto modeOf = [](const Texture2D& t) {
    int c = Texture2D::channelsOf(t.format());
    return c <= 1 ? 0 : c == 2 ? 1 : 2;
  };
  int layersMoved = 0;
  for (auto& l : layers) {
    int movedBefore = moved;
    for (int c = 0; c < kChannelCount; ++c) {
      auto& t = l->channels[c];
      if (t && t->valid()) move(t, c == (int)Channel::Normal ? 3 : modeOf(*t));
    }
    if (l->mask && l->mask->valid()) move(l->mask, modeOf(*l->mask));
    if (moved > movedBefore) ++layersMoved;
  }
  looks.clear();
  highN.destroy(), highT.destroy();

  // ---- the bakes, for the low: the high's shape into its normal map, AO and slots; the rest from the low itself
  HighPolyBakeOptions hb = highPolyOptions;
  hb.distance = p.distance;
  hb.samples = S;
  hb.normal = o.bakeHigh;
  hb.ao = o.bakeHigh && ao;
  hb.materialIds = o.bakeHigh;
  if (o.bakeHigh) applyHighPolyBake(*this, high, p, hb);
  if (!hb.ao && ao) tex.bakeAO(mesh, gpu, aoSamples);
  if (!hb.normal && curv) tex.bakeCurvature(mesh, gpu, &bvh);
  if (thick) tex.bakeThickness(mesh, bvh, thicknessRays);

  // ---- what is left to look at: the check map, and the high as it was
  bakeDownCheck.create(R, R, GL_RGBA8, 1);
  bakeDownCheck.uploadRaw(GL_RGBA, GL_UNSIGNED_BYTE, p.check.data());
  bakeDownUV = uv::uvHash(mesh);
  before->gpu.upload(high);
  bakeDownBefore = std::move(before);

  // ---- the notes onto the low's surface
  for (auto& n : notes) {
    RayHit h = bvh.closestPoint(mesh, n.position);
    if (!h.hit) continue;
    n.position = h.position;
    const auto& tri = mesh.tris[h.tri];
    vec3 nn = mesh.loopNormal[tri[0]] * (1.f - h.bary.x - h.bary.y) + mesh.loopNormal[tri[1]] * h.bary.x +
              mesh.loopNormal[tri[2]] * h.bary.y;
    if (glm::length(nn) > 1e-8f) n.normal = glm::normalize(nn);
  }

  // ---- the high stays as the high-poly model; the project is a new one
  highPoly = std::make_unique<Mesh>(std::move(high));
  highPolyPath = o.highPath;
  highPolyOptions = hb;
  highPolyBaked = o.bakeHigh;
  meshPath = o.lowPath;
  meshFileTime = o.lowPath.empty() ? 0 : fileTime(o.lowPath);
  projectPath.clear();
  bridge.active = false;
  bridge.object.clear();
  bridge.uvsEdited = uvsMade;
  startupScene = false;
  uvsCarried = -1.f;
  symmetryPlane = NAN;
  clearCollision();
  faceSelection.assign(mesh.faceCount(), 0);
  updateHiddenFaces();
  clearHistory();
  markComposite();

  json r = projectionStats(p);
  r["seconds"] = nowSeconds() - t0;
  r["projection_seconds"] = tProject;
  r["layers"] = layers.size();
  r["layers_moved"] = layersMoved;
  r["textures_moved"] = moved;
  r["resolution"] = tileR;
  r["low_poly_faces"] = mesh.faceCount();
  r["high_poly_faces"] = highPoly->faceCount();
  r["high_poly_baked"] = o.bakeHigh;
  r["generated_uvs"] = uvsGenerated;
  bakeDownReport = r;
  if (report) *report = r;
  PLOG_I("Bake-down: %d layers (%d textures) onto %u faces at %d px, %d points per texel: %.1f%% along the normals, "
         "%.1f%% by the nearest point, %.2f%% guessed; distance %.4g%s; %.1f s (projection %.1f s)",
         layersMoved, moved, mesh.faceCount(), tileR, p.samples, 100.0 * r["hit_fraction"].get<double>(),
         100.0 * r["nearest_point_fraction"].get<double>(), 100.0 * r["guessed_fraction"].get<double>(), p.distance,
         p.distanceMeasured ? " (measured)" : "", nowSeconds() - t0, tProject);
  return true;
}

}  // namespace patina
