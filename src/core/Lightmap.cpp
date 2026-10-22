#include "core/Lightmap.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "core/Log.h"
#include "core/Mesh.h"
#include "core/UVOps.h"

#include <xatlas.h>

namespace patina {

namespace {

// The faces whose UVs overlap another face's (a grid of `res` over the square: a cell center inside two faces).
// (The model's own triangles: what an engine draws; a fan from the first corner overlaps itself in concave n-gons.)
std::vector<uint32_t> overlappingFaces(const Mesh& m, int res) {
  std::vector<int32_t> owner(size_t(res) * res, -1);
  std::vector<uint8_t> bad(m.faceCount(), 0);
  for (size_t t = 0; t < m.tris.size(); ++t) {
    uint32_t f = m.triFace[t];
    {
      vec2 a = m.loopUV[m.tris[t].x] * float(res), b = m.loopUV[m.tris[t].y] * float(res), c = m.loopUV[m.tris[t].z] * float(res);
      float area = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
      if (std::abs(area) < 1e-9f) continue;
      int x0 = std::max(0, (int)std::floor(std::min({a.x, b.x, c.x}))), x1 = std::min(res - 1, (int)std::ceil(std::max({a.x, b.x, c.x})));
      int y0 = std::max(0, (int)std::floor(std::min({a.y, b.y, c.y}))), y1 = std::min(res - 1, (int)std::ceil(std::max({a.y, b.y, c.y})));
      for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
          vec2 p(x + 0.5f, y + 0.5f);
          float w0 = (b.x - p.x) * (c.y - p.y) - (c.x - p.x) * (b.y - p.y);
          float w1 = (c.x - p.x) * (a.y - p.y) - (a.x - p.x) * (c.y - p.y);
          float w2 = (a.x - p.x) * (b.y - p.y) - (b.x - p.x) * (a.y - p.y);
          bool in = area > 0 ? (w0 > 0 && w1 > 0 && w2 > 0) : (w0 < 0 && w1 < 0 && w2 < 0);
          if (!in) continue;
          int32_t& o = owner[size_t(y) * res + x];
          if (o < 0) o = (int32_t)f;
          else if (o != (int32_t)f) bad[f] = 1, bad[o] = 1;
        }
    }
  }
  std::vector<uint32_t> out;
  for (uint32_t f = 0; f < m.faceCount(); ++f)
    if (bad[f]) out.push_back(f);
  return out;
}

// Does a UV island overlap itself (a fold, or faces stacked on each other)? Its faces are drawn into a grid over
// the island's box; a cell center inside two of its faces is an overlap.
bool folds(const Mesh& m, const std::vector<uint32_t>& faces) {
  Rect2 box;
  for (uint32_t f : faces)
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) box.add(m.loopUV[l]);
  vec2 ext = box.hi - box.lo;
  if (ext.x <= 0 || ext.y <= 0) return true;  // no area: stacked on a point or a line
  const int G = 192;
  std::vector<int32_t> owner(size_t(G) * G, -1);
  size_t covered = 0, twice = 0;
  for (uint32_t f : faces) {
    uint32_t s0 = m.faceStart[f], n = m.faceStart[f + 1] - s0;
    for (uint32_t k = 1; k + 1 < n; ++k) {  // a fan of triangles
      vec2 a = (m.loopUV[s0] - box.lo) / ext * float(G), b = (m.loopUV[s0 + k] - box.lo) / ext * float(G),
           c = (m.loopUV[s0 + k + 1] - box.lo) / ext * float(G);
      float area = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
      if (std::abs(area) < 1e-9f) continue;
      int x0 = std::max(0, (int)std::floor(std::min({a.x, b.x, c.x}))), x1 = std::min(G - 1, (int)std::ceil(std::max({a.x, b.x, c.x})));
      int y0 = std::max(0, (int)std::floor(std::min({a.y, b.y, c.y}))), y1 = std::min(G - 1, (int)std::ceil(std::max({a.y, b.y, c.y})));
      for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
          vec2 p(x + 0.5f, y + 0.5f);
          float w0 = (b.x - p.x) * (c.y - p.y) - (c.x - p.x) * (b.y - p.y);
          float w1 = (c.x - p.x) * (a.y - p.y) - (a.x - p.x) * (c.y - p.y);
          float w2 = (a.x - p.x) * (b.y - p.y) - (b.x - p.x) * (a.y - p.y);
          bool in = area > 0 ? (w0 > 0 && w1 > 0 && w2 > 0) : (w0 < 0 && w1 < 0 && w2 < 0);
          if (!in) continue;
          int32_t& o = owner[size_t(y) * G + x];
          if (o < 0) o = (int32_t)f, ++covered;
          else if (o != (int32_t)f) ++twice;
        }
    }
  }
  return covered && twice * 200 > covered;  // over half a percent of the island covered twice
}

}  // namespace

// xatlas: its own charts (cut where the surface turns or a seam runs), unwrapped and packed for a lightmap of `size`
// with `padding` texels round each chart (bilinear filtering). One atlas page: packed again smaller until it fits.
bool xatlasUVs(const Mesh& m, int size, int padding, std::vector<vec2>& uv2, LightmapStats& st) {
  uint32_t F = m.faceCount();
  bool polygons = true;
  std::vector<uint8_t> counts(F);
  for (uint32_t f = 0; f < F; ++f) {
    uint32_t n = m.faceStart[f + 1] - m.faceStart[f];
    if (n > 255) polygons = false;
    counts[f] = (uint8_t)std::min<uint32_t>(n, 255);
  }
  // faces of more than 255 corners: give xatlas the triangles instead
  std::vector<uint32_t> idx;
  std::vector<uint32_t> triOfLoop;  // triangles path: for each output triangle corner, its loop
  if (polygons) idx = m.loopVert;
  else
    for (auto& t : m.tris) idx.insert(idx.end(), {m.loopVert[t.x], m.loopVert[t.y], m.loopVert[t.z]}),
        triOfLoop.insert(triOfLoop.end(), {t.x, t.y, t.z});
  xatlas::Atlas* atlas = xatlas::Create();
  xatlas::MeshDecl md;
  md.vertexPositionData = m.positions.data();
  md.vertexPositionStride = sizeof(vec3);
  md.vertexCount = (uint32_t)m.positions.size();
  md.indexData = idx.data();
  md.indexCount = (uint32_t)idx.size();
  md.indexFormat = xatlas::IndexFormat::UInt32;
  if (polygons) {
    md.faceVertexCount = counts.data();
    md.faceCount = F;
  }
  if (xatlas::AddMesh(atlas, md, 1) != xatlas::AddMeshError::Success) {
    xatlas::Destroy(atlas);
    return false;
  }
  xatlas::ChartOptions co;
  co.fixWinding = true;  // no chart may be mirrored onto itself
  xatlas::ComputeCharts(atlas, co);
  xatlas::PackOptions po;
  po.resolution = (uint32_t)size;
  po.padding = (uint32_t)padding;
  po.bilinear = true;
  po.texelsPerUnit = 0.f;
  xatlas::PackCharts(atlas, po);  // a first guess at the scale
  // then at exactly the lightmap's size (so the padding is true in its pixels), a little smaller until it is one page
  float tpu = atlas->texelsPerUnit, firstGuess = tpu;
  for (int tries = 0; tries < 24; ++tries) {
    po.texelsPerUnit = tpu;
    xatlas::PackCharts(atlas, po);
    if (atlas->atlasCount == 1) break;
    tpu *= 0.92f;
  }
  if (atlas->atlasCount != 1) {
    // too many charts for this size with this padding: one page of the size they need (the gaps are then smaller
    // in the lightmap asked for); the result says so
    po.resolution = 0;
    po.texelsPerUnit = firstGuess;
    xatlas::PackCharts(atlas, po);
    st.neededSize = (int)std::max(atlas->width, atlas->height);
  }
  bool ok = atlas->meshCount == 1 && atlas->atlasCount <= 1 && atlas->width > 0;
  if (ok) {
    const xatlas::Mesh& om = atlas->meshes[0];
    float W = float(atlas->width), H = float(atlas->height), S = std::max(W, H);
    uv2.assign(m.loopCount(), vec2(0.f));
    if (polygons) {
      // with polygons the output indices follow the input's face corners one for one
      if (om.indexCount < m.loopCount()) ok = false;
      for (uint32_t l = 0; ok && l < m.loopCount(); ++l) {
        uint32_t vi = om.indexArray[l];
        if (vi >= om.vertexCount) continue;
        const xatlas::Vertex& v = om.vertexArray[vi];
        uv2[l] = vec2(v.uv[0] / S, v.uv[1] / S);
      }
    } else {
      for (uint32_t i = 0; i < om.indexCount && i < triOfLoop.size(); ++i) {
        if (om.indexArray[i] >= om.vertexCount) continue;
        const xatlas::Vertex& v = om.vertexArray[om.indexArray[i]];
        uv2[triOfLoop[i]] = vec2(v.uv[0] / S, v.uv[1] / S);
      }
    }
    st.islands = (int)atlas->chartCount;
  }
  xatlas::Destroy(atlas);
  return ok;
}

bool makeLightmapUVs(const Mesh& m, const LightmapOptions& opt, std::vector<vec2>& uv2, LightmapStats* stats) {
  if (m.faceCount() == 0) return false;
  if (opt.source == "auto" || opt.source == "smart") {
    LightmapStats st;
    int size = std::clamp(opt.size, 32, 8192);
    if (!xatlasUVs(m, size, (int)std::lround(std::clamp(opt.padding, 0.f, 64.f)), uv2, st)) return false;
    Mesh work = m;
    work.loopUV = uv2;
    st.fill = uv::coverage(work, 256);
    st.overlaps = (int)overlappingFaces(work, 1024).size();
    PLOG_I("Lightmap UVs (xatlas): %d charts, %.0f%% of the square, %d overlapping faces", st.islands, st.fill * 100.f,
           st.overlaps);
    if (st.overlaps == 0) {
      if (stats) *stats = st;
      return true;
    }
    // some of xatlas's charts overlap (it triangulates big n-gons its own way): its charts go through the
    // repair and packing the model's own islands get
    LightmapOptions o2 = opt;
    o2.source = "uv";
    bool ok = makeLightmapUVs(work, o2, uv2, stats);
    if (ok && stats) stats->neededSize = st.neededSize;
    return ok;
  }
  Mesh work = m;
  // every face gets space of its own: mirror copies (stacked on purpose for painting) are islands like any other
  for (auto& f : work.faceFlags) f &= uint8_t(~kFaceMirrorBits);
  work.tiles = TileLayout();
  work.loopAtlasUV.clear();
  int size = std::clamp(opt.size, 32, 8192);
  float margin = std::clamp(opt.padding, 0.f, 64.f) / float(size);
  LightmapStats st;
  {
    work.computeUVTopology();
    // islands without UV area (a model with no or broken UVs) are projected flat first
    for (auto& faces : uv::facesByIsland(work)) {
      double area = 0;
      vec3 n(0.f);
      for (uint32_t f : faces) area += std::abs(work.faceAreaUV(f)), n += work.faceNormal(f) * work.faceArea3D(f);
      if (area > 1e-12) continue;
      ++st.flatIslands;
      uv::projectPlanar(work, glm::length(n) > 0 ? glm::normalize(n) : vec3(0, 1, 0), faces);
    }
    if (st.flatIslands) work.computeUVTopology();
  }
  // islands that overlap themselves (faces sharing UV corners, folds) are projected again in smaller pieces:
  // a smart projection at 45, 30 and 15 degrees, then each face on its own
  for (int pass = 0; pass < 4; ++pass) {
    std::vector<uint32_t> folded;
    for (auto& isl : uv::facesByIsland(work))
      if (folds(work, isl)) folded.insert(folded.end(), isl.begin(), isl.end());
    if (folded.empty()) break;
    st.refolded += (int)folded.size();
    static const float angles[] = {45.f, 30.f, 15.f};
    if (pass < 3) {
      uv::smartProject(work, angles[pass], folded, margin);
    } else {
      for (uint32_t f : folded) uv::projectPlanar(work, work.faceNormal(f), {f});
      // apart from each other, so faces that met do not join up again
      for (size_t i = 0; i < folded.size(); ++i) {
        uint32_t f = folded[i];
        for (uint32_t l = work.faceStart[f]; l < work.faceStart[f + 1]; ++l) work.loopUV[l] += vec2(float(i) * 2.f, 0.f);
      }
    }
    work.computeUVTopology();
  }
  // pack; then the faces that still overlap (small folds the check above lets through) have their islands
  // projected again in smaller pieces, and everything is packed again
  uv::PackOptions po;
  po.margin = margin;
  po.rotate = true;
  po.normalizeDensity = true;
  po.shapes = true;
  po.resolution = std::clamp(size, 256, 1024);
  auto packAll = [&]() {
    auto charts = uv::facesByIsland(work);
    st.islands = (int)charts.size();
    uv::ShapePackStats ps;
    if (!uv::packShapes(work, charts, po, {}, 0, &ps)) {
      po.shapes = false;  // the boxes packer always finds a place
      uv::packCharts(work, charts, po);
      po.shapes = true;
    }
  };
  packAll();
  for (int round = 0; round < 4; ++round) {
    std::vector<uint32_t> bad = overlappingFaces(work, 1024);
    if (bad.empty()) break;
    std::vector<uint8_t> hit(work.faceCount(), 0);
    for (uint32_t f : bad) hit[f] = 1;
    std::vector<uint32_t> again;
    for (auto& isl : uv::facesByIsland(work)) {
      bool any = false;
      for (uint32_t f : isl) any |= hit[f] != 0;
      if (any) again.insert(again.end(), isl.begin(), isl.end());
    }
    st.refolded += (int)again.size();
    // first solved again conformally (LSCM rarely folds), then cut smaller and solved again, then face by face
    static const float angles[] = {0.f, 30.f, 15.f};
    if (round < 3) {
      if (angles[round] > 0) {
        uv::smartProject(work, angles[round], again, margin);
        work.computeUVTopology();
      }
      uv::relaxIslands(work, again, 1.f);
    } else {
      for (size_t i = 0; i < again.size(); ++i) {
        uint32_t f = again[i];
        uv::projectPlanar(work, work.faceNormal(f), {f});
        for (uint32_t l = work.faceStart[f]; l < work.faceStart[f + 1]; ++l) work.loopUV[l] += vec2(float(i) * 2.f, 0.f);
      }
    }
    work.computeUVTopology();
    packAll();
  }
  uv2 = work.loopUV;
  st.fill = uv::coverage(work, 256);
  st.overlaps = (int)overlappingFaces(work, 1024).size();  // faces that overlap another (0 when it worked)
  if (stats) *stats = st;
  PLOG_I("Lightmap UVs: %d islands, %.0f%% of the square, %d overlaps", st.islands, st.fill * 100.f, st.overlaps);
  return true;
}

}  // namespace patina
