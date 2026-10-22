#include "engine/HighPolyBake.h"

#include "core/Bvh.h"
#include "core/Image.h"
#include "core/Log.h"
#include "core/Mesh.h"
#include "core/Progress.h"
#include "engine/Document.h"
#include "gfx/GpuMesh.h"

namespace patina {

bool projectTexels(const Texture2D& posTex, const Texture2D& nrmTex, const Mesh& low, const Mesh& high,
                   const Bvh& highBvh, ProjectionOptions opt, Projection& out, std::string* err) {
  int R = posTex.width();
  size_t N = size_t(R) * R;
  std::vector<float> pos(N * 4), nrm(N * 4);
  posTex.downloadRaw(GL_RGBA, GL_FLOAT, pos.data(), pos.size() * sizeof(float));
  nrmTex.downloadRaw(GL_RGBA, GL_FLOAT, nrm.data(), nrm.size() * sizeof(float));
  opt.resolution = R;
  opt.coverPos = pos.data();
  opt.coverNrm = nrm.data();
  return projectOntoHigh(low, high, highBvh, opt, out, err);
}

void applyHighPolyBake(Document& d, const Mesh& high, const Projection& p, const HighPolyBakeOptions& opt) {
  TextureSet& tex = d.tex;
  const int R = tex.resolution();
  const size_t N = size_t(R) * R;
  if (p.resolution != R || p.highNormal.size() != N) return;
  if (opt.normal) {
    tex.highNormal.uploadRaw(GL_RGBA, GL_FLOAT, p.highNormal.data());
    tex.dilateInPlace(tex.highNormal);
    tex.setHighNormal(true);
  }
  bool data = p.hitPos.size() == N && p.hitMaterial.size() == N;
  if (opt.materialIds && data) {
    // the low's own slot where nothing of the high was found
    std::vector<float> ids(N * 4);
    tex.matIdMap.downloadRaw(GL_RGBA, GL_FLOAT, ids.data(), ids.size() * sizeof(float));
    for (size_t i = 0; i < N; ++i) {
      if (p.hitMaterial[i] != 255) ids[i * 4] = float(p.hitMaterial[i]) / 255.f;
      ids[i * 4 + 3] = 1.f;
    }
    tex.matIdMap.uploadRaw(GL_RGBA, GL_FLOAT, ids.data());
    tex.dilateInPlace(tex.matIdMap);
    tex.materialIdsFromHigh = true;
  }
  if (opt.ao && data) {
    // where the high has no hit, the low's own surface (the AO there is the low's)
    std::vector<vec4> P(p.hitPos), Nn(p.highNormal);
    std::vector<float> pos(N * 4), nrm(N * 4);
    tex.posMap.downloadRaw(GL_RGBA, GL_FLOAT, pos.data(), pos.size() * sizeof(float));
    tex.nrmMap.downloadRaw(GL_RGBA, GL_FLOAT, nrm.data(), nrm.size() * sizeof(float));
    for (size_t i = 0; i < N; ++i)
      if (P[i].w < 0.5f && pos[i * 4 + 3] >= 0.5f) {
        P[i] = vec4(pos[i * 4], pos[i * 4 + 1], pos[i * 4 + 2], 1.f);
        Nn[i] = vec4(nrm[i * 4], nrm[i * 4 + 1], nrm[i * 4 + 2], 1.f);
      }
    Texture2D sp(R, R, GL_RGBA32F, 1), sn(R, R, GL_RGBA16F, 1);
    sp.setFilter(GL_NEAREST, GL_NEAREST);
    sn.setFilter(GL_NEAREST, GL_NEAREST);
    sp.uploadRaw(GL_RGBA, GL_FLOAT, P.data());
    sn.uploadRaw(GL_RGBA, GL_FLOAT, Nn.data());
    GpuMesh hg;
    hg.upload(high);
    tex.bakeAO(d.mesh, d.gpu, opt.aoSamples, 1.f, &hg, &sp, &sn, &high.bounds);
  }
  // the sculpt's detail shows in the curvature too (edges and bumps of the normal map)
  if (opt.normal) tex.bakeCurvature(d.mesh, d.gpu, &d.bvh);
  d.markComposite();
}

nlohmann::json projectionStats(const Projection& p) {
  double n = double(std::max(p.points, 1l));
  double ray = p.byRay / n, byNearest = p.byNear / n, guessed = p.guessed / n;
  double flipped = double(p.flipped) / double(std::max(p.texels, 1l));
  std::string warning;
  if (flipped > 0.001)
    warning = "Parts of the low-poly model face away from the high-poly (magenta in the check view): turned inside out "
              "(flip them: Mesh > Mesh Check, or Blender's Recalculate Outside), or folded over a hollow the high-poly has "
              "(a denser low-poly follows it). They keep the low-poly's own shading";
  else if (ray < 0.9)
    warning = guessed > 0.02 ? "Some of the texture found nothing of the high-poly surface nearby and was guessed: check that both "
                           "models are in the same place, or raise the distance"
                         : "Many texels missed the high-poly mesh along the normals: raise the distance or check that both "
                           "meshes are in the same place";
  return {{"texels", p.texels},
          {"samples_per_texel", p.samples},
          {"points", p.points},
          {"hit_fraction", ray},
          {"nearest_point_fraction", byNearest},
          {"guessed_fraction", guessed},
          {"distance", p.distance},
          {"distance_measured", p.distanceMeasured},
          {"largest_distance", p.largestDistance},
          {"flipped_fraction", flipped},
          {"warning", warning}};
}

bool bakeFromHighPoly(Document& d, const Mesh& high, const HighPolyBakeOptions& opt, nlohmann::json* stats,
                      std::string* err) {
  if (!d.hasMesh() || d.tex.resolution() <= 0) {
    if (err) *err = "No low-poly mesh loaded";
    return false;
  }
  if (high.faceCount() == 0 || high.tris.empty()) {
    if (err) *err = "The high-poly mesh has no faces";
    return false;
  }
  if (d.tex.resolution() > 8192) {  // several float images of R x R on the CPU: about 6 GB at 8192
    if (err)
      *err = "The texture set is " + std::to_string(d.tex.resolution()) + " px across all its tiles; high-poly baking "
             "needs 8192 or less. Lower the resolution (or bake per texture set at a lower one and raise it after).";
    return false;
  }
  double t0 = nowSeconds();
  progress::Scope prog("Baking from the high-poly mesh");
  Bvh bvh;
  bvh.build(high);
  ProjectionOptions po;
  po.samples = d.tex.resolution() > 4096 ? std::min(opt.samples, 4) : opt.samples;
  po.distance = opt.distance;
  po.lookups = false;
  po.bakeData = opt.ao || opt.materialIds;
  Projection p;
  if (!projectTexels(d.tex.posMap, d.tex.nrmMap, d.mesh, high, bvh, po, p, err)) return false;
  double tRays = nowSeconds() - t0;
  applyHighPolyBake(d, high, p, opt);
  PLOG_I("High-poly bake: %ld texels x %d points, %.1f%% along the normals, %.1f%% by the nearest point, %.2f%% "
         "guessed; distance %.4g%s; rays %.2f s, total %.2f s",
         p.texels, p.samples, 100.0 * p.byRay / std::max(p.points, 1l), 100.0 * p.byNear / std::max(p.points, 1l),
         100.0 * p.guessed / std::max(p.points, 1l), p.distance, p.distanceMeasured ? " (measured)" : "", tRays,
         nowSeconds() - t0);
  if (stats) {
    *stats = projectionStats(p);
    (*stats)["seconds"] = nowSeconds() - t0;
    (*stats)["high_poly_faces"] = high.faceCount();
  }
  return true;
}

}  // namespace patina
