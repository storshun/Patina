#include "engine/Document.h"

#include "core/Parallel.h"

#include <cstring>
#include <fstream>
#include <glm/gtc/packing.hpp>
#include <map>

#include "core/Progress.h"

#include <stb_image_resize2.h>

#include <atomic>
#include <functional>
#include <set>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "core/Container.h"
#include "core/Image.h"
#include "core/Log.h"
#include "core/PngStream.h"
#include "core/PsdWriter.h"
#include "core/MeshIO.h"
#include "core/Paths.h"
#include "core/UVOps.h"
#include "engine/BridgeProjects.h"
#include "engine/MaterialLibrary.h"
#include "gfx/Shader.h"

namespace patina {

namespace fs = std::filesystem;

static const char* kPresetNames[] = {"blender", "gltf", "unreal", "unity"};
const char* exportPresetName(ExportPreset p) { return kPresetNames[std::clamp((int)p, 0, 3)]; }
ExportPreset exportPresetFromName(const std::string& s) {
  for (int i = 0; i < 4; ++i)
    if (s == kPresetNames[i]) return (ExportPreset)i;
  return ExportPreset::Blender;
}

// ------------------------------------------------------------------ mesh

static bool uvsLookEmpty(const Mesh& m) {
  if (m.loopUV.empty()) return true;
  double area = 0;
  for (uint32_t f = 0; f < m.faceCount(); ++f) area += std::abs(m.faceAreaUV(f));
  return area < 1e-8;
}

// Islands lying entirely in another integer UV tile (e.g. UVs in [1,2]) are moved
// into 0..1: identical look with repeating textures, and paintable here.
static bool normalizeUVTiles(Mesh& m) {
  if (m.loopIsland.size() != m.loopCount()) m.computeUVTopology();
  std::vector<Rect2> box(m.islandCount);
  for (uint32_t l = 0; l < m.loopCount(); ++l) box[m.loopIsland[l]].add(m.loopUV[l]);
  std::vector<vec2> shift(m.islandCount, vec2(0));
  bool any = false;
  for (uint32_t i = 0; i < m.islandCount; ++i) {
    vec2 lo = glm::floor(box[i].lo + 1e-5f), hi = glm::floor(box[i].hi - 1e-5f);
    for (int a = 0; a < 2; ++a)
      if (lo[a] == hi[a] && lo[a] != 0.f) shift[i][a] = -lo[a], any = true;
  }
  if (!any) return false;
  for (uint32_t l = 0; l < m.loopCount(); ++l) m.loopUV[l] += shift[m.loopIsland[l]];
  m.computeTangents();
  return true;
}

// ------------------------------------------------------------------ texture tiles

uint8_t Document::detectTileMode(const Mesh& mc) {
  Mesh& m = const_cast<Mesh&>(mc);
  if (m.loopUV.size() != m.loopCount() || m.faceCount() == 0) return 0;
  if (m.loopIsland.size() != m.loopCount()) m.computeUVTopology();
  uint8_t mode = 0;
  // UDIM: every island inside one unit tile (u 0..9, v >= 0), and more than one tile used
  {
    std::vector<Rect2> box(m.islandCount);
    for (uint32_t l = 0; l < m.loopCount(); ++l) box[m.loopIsland[l]].add(m.loopUV[l]);
    std::vector<int> used;
    bool clean = true;
    for (auto& b : box) {
      if (b.lo.x > b.hi.x) continue;
      vec2 lo = glm::floor(b.lo + 1e-4f), hi = glm::floor(b.hi - 1e-4f);
      if (lo != hi || lo.x < 0 || lo.x > 9 || lo.y < 0) {
        clean = false;
        break;
      }
      int u = udimOf(lo + 0.5f);
      if (std::find(used.begin(), used.end(), u) == used.end()) used.push_back(u);
    }
    if (clean && used.size() > 1) mode |= TileByUdim;
  }
  // texture sets: two or more material slots whose UVs cover the same texels
  {
    std::vector<uint32_t> slots;
    for (uint32_t s : m.faceMaterial)
      if (std::find(slots.begin(), slots.end(), s) == slots.end()) slots.push_back(s);
    if (slots.size() > 1) {
      const int N = 256;
      // coverage per UDIM tile (every face wraps into its tile's square)
      std::map<int, std::vector<uint8_t>> any;
      double sum = 0;
      for (uint32_t slot : slots) {
        std::map<int, std::vector<uint8_t>> cov;
        for (size_t t = 0; t < m.tris.size(); ++t) {
          uint32_t f = m.triFace[t];
          if (m.faceMaterial[f] != slot) continue;
          vec2 cen = (m.loopUV[m.tris[t].x] + m.loopUV[m.tris[t].y] + m.loopUV[m.tris[t].z]) / 3.f;
          vec2 o = glm::floor(cen);
          int key = (mode & TileByUdim) ? udimOf(cen) : 0;
          auto& c = cov[key];
          if (c.empty()) c.assign(size_t(N) * N, 0);
          vec2 p[3];
          for (int k = 0; k < 3; ++k) p[k] = (m.loopUV[m.tris[t][k]] - o) * float(N);
          vec2 lo = glm::min(p[0], glm::min(p[1], p[2])), hi = glm::max(p[0], glm::max(p[1], p[2]));
          float area = (p[1].x - p[0].x) * (p[2].y - p[0].y) - (p[2].x - p[0].x) * (p[1].y - p[0].y);
          if (std::abs(area) < 1e-9f) continue;
          for (int y = std::max(0, (int)lo.y); y <= std::min(N - 1, (int)hi.y); ++y)
            for (int x = std::max(0, (int)lo.x); x <= std::min(N - 1, (int)hi.x); ++x) {
              vec2 q(x + 0.5f, y + 0.5f);
              float w0 = (p[1].x - q.x) * (p[2].y - q.y) - (p[2].x - q.x) * (p[1].y - q.y);
              float w1 = (p[2].x - q.x) * (p[0].y - q.y) - (p[0].x - q.x) * (p[2].y - q.y);
              float w2 = (p[0].x - q.x) * (p[1].y - q.y) - (p[1].x - q.x) * (p[0].y - q.y);
              if ((w0 >= 0 && w1 >= 0 && w2 >= 0) || (w0 <= 0 && w1 <= 0 && w2 <= 0)) c[size_t(y) * N + x] = 1;
            }
        }
        for (auto& [key, c] : cov) {
          auto& u = any[key];
          if (u.empty()) u.assign(size_t(N) * N, 0);
          for (size_t i = 0; i < c.size(); ++i) {
            sum += c[i];
            u[i] |= c[i];
          }
        }
      }
      double uni = 0;
      for (auto& [key, u] : any)
        for (uint8_t v : u) uni += v;
      if (uni > 0 && sum > uni * 1.1 + 64) mode |= TileBySlot;
    }
  }
  return mode;
}

void Document::relayout(const TileLayout& target) {
  if (!hasMesh()) return;
  int R = tileRes();
  TileLayout keepT = mesh.tiles;
  std::vector<vec2> keepA = mesh.loopAtlasUV;
  mesh.tiles = target;
  mesh.tiles.faceTile.clear();
  if (target.mode) mesh.tiles.tiles = target.tiles;
  mesh.updateTiles();
  TileLayout nt = mesh.tiles;
  std::vector<vec2> newAtlas = nt.active() ? mesh.loopAtlasUV : mesh.loopUV;
  mesh.tiles = keepT;
  mesh.loopAtlasUV = keepA;
  int newSize = R * nt.grid;
  // mirror copies share the texels of the half they copy: only that half carries paint
  std::vector<vec2> draw = newAtlas;
  if (mesh.faceFlags.size() == mesh.faceCount())
    for (uint32_t f = 0; f < mesh.faceCount(); ++f)
      if (mesh.faceFlags[f] & FaceMirrorCopy)
        for (uint32_t l = mesh.faceStart[f]; l < mesh.faceStart[f + 1]; ++l) draw[l] = vec2(-10.f);
  for (auto& l : layers) {
    for (auto& c : l->channels)
      if (c && c->valid()) tex.reproject(gpu, draw, *c, newSize);
    if (l->mask) tex.reproject(gpu, draw, *l->mask, newSize);
  }
  // maps that can't be baked again from the low-poly mesh move like paint
  bool rebakeHigh = highPoly && highPolyBaked;
  Texture2D hn, mid;
  bool keepHn = tex.hasHighNormal() && !rebakeHigh, keepMid = tex.materialIdsFromHigh && !rebakeHigh;
  if (keepHn) {
    hn = Texture2D(tex.highNormal.width(), tex.highNormal.height(), tex.highNormal.format(), 1);
    hn.copyFrom(tex.highNormal);
    tex.reproject(gpu, draw, hn, newSize);
  }
  if (keepMid) {
    mid = Texture2D(tex.matIdMap.width(), tex.matIdMap.height(), tex.matIdMap.format(), 1);
    mid.copyFrom(tex.matIdMap);
    tex.reproject(gpu, draw, mid, newSize);
  }
  tex.releaseScratch();
  bool ao = tex.hasAO(), curv = tex.hasCurvature(), thick = tex.hasThickness();
  mesh.tiles = nt;
  mesh.computeUVTopology();
  gpu.updateUVs(mesh);
  tex.allocate(newSize);
  rebakeMeshMaps(ao, curv, thick);
  if (keepHn) {
    std::swap(tex.highNormal, hn);
    tex.setHighNormal(true);
  }
  if (keepMid) {
    std::swap(tex.matIdMap, mid);
    tex.materialIdsFromHigh = true;
  }
  if (rebakeHigh) {
    std::string err;
    if (!bakeFromHighPoly(*this, *highPoly, highPolyOptions, nullptr, &err)) PLOG_W("High-poly re-bake: %s", err.c_str());
  }
  clearHistory();
  markComposite();
  PLOG_I("Texture tiles: %d tile(s) in a %dx%d atlas of %d px (%d px per tile)", nt.count(), nt.grid, nt.grid, newSize, R);
}

bool Document::atlasFits(int tileR, int grid, std::string* why) {
  GLint maxTex = 16384;
  glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTex);
  int limit = std::min<int>(maxTex, 16384);
  if (tileR * grid <= limit) return true;
  if (why)
    *why = std::to_string(grid * grid) + " tiles at " + std::to_string(tileR) + " px need a " + std::to_string(tileR * grid) +
           " px atlas; the limit is " + std::to_string(limit) + ". Use a lower texture set resolution.";
  return false;
}

bool Document::setTileMode(uint8_t mode, std::string* why) {
  mode &= TileBySlot | TileByUdim;
  if (!hasMesh() || mode == mesh.tiles.mode) return false;
  if ((mesh.tiles.mode & TileByUdim) && !(mode & TileByUdim)) {
    // without UDIM tiles, islands beyond 0..1 would repeat tile 1001 and lose their own paint
    for (uint32_t f = 0; f < mesh.faceCount(); ++f) {
      if (mesh.faceHasNoUVArea(f, mesh.loopUV)) continue;  // no texels, nothing to lose
      vec2 c(0);
      for (uint32_t l = mesh.faceStart[f]; l < mesh.faceStart[f + 1]; ++l) c += mesh.loopUV[l];
      c /= float(std::max(1u, mesh.faceSize(f)));
      if (glm::floor(c) != vec2(0.f)) {
        if (why) *why = "Some islands sit in UDIM tiles other than 1001: their paint would be lost. Move them into 0-1 first (UV editor), then switch.";
        return false;
      }
    }
  }
  TileLayout t;
  t.mode = mode;
  relayout(t);
  return true;
}

int Document::moveFacesToSlotSet(const std::vector<uint32_t>& faces, const std::string& nameIn, std::string* why) {
  auto fail = [&](const std::string& w) {
    if (why) *why = w;
    return -1;
  };
  if (!hasMesh()) return fail("no mesh loaded");
  if (faces.empty()) return fail("no faces given (select some first)");
  if ((mesh.tiles.mode & TileByUdim) && !(mesh.tiles.mode & TileBySlot))
    return fail("the layout is UDIM tiles: a new texture set is a new UDIM tile");
  std::string name = nameIn;
  while (!name.empty() && std::isspace((unsigned char)name.back())) name.pop_back();
  while (!name.empty() && std::isspace((unsigned char)name.front())) name.erase(name.begin());
  if (name.empty()) return fail("the texture set needs a name");
  const uint32_t F = mesh.faceCount();
  std::vector<uint32_t> before = mesh.faceMaterial;
  std::vector<std::string> namesBefore = mesh.materialNames;
  if (mesh.faceMaterial.size() != F) mesh.faceMaterial.resize(F, 0);
  uint32_t nSlots = std::max<uint32_t>(1, (uint32_t)mesh.materialNames.size());
  for (uint32_t m : mesh.faceMaterial) nSlots = std::max(nSlots, m + 1);
  // the slot with that name (as Patina shows it, any case), or a new one
  auto lower = [](std::string x) {
    for (auto& ch : x) ch = (char)std::tolower((unsigned char)ch);
    return x;
  };
  uint32_t slot = nSlots;
  for (uint32_t i = 0; i < nSlots; ++i)
    if (lower(mesh.slotName(i)) == lower(name)) slot = i;
  if (slot == nSlots) {
    mesh.materialNames.resize(nSlots + 1);
    for (uint32_t i = 0; i < nSlots; ++i)
      if (mesh.materialNames[i].empty()) mesh.materialNames[i] = mesh.slotName(i);
    mesh.materialNames[slot] = name;
  }
  size_t moved = 0;
  for (uint32_t f : faces)
    if (f < F && mesh.faceMaterial[f] != slot) mesh.faceMaterial[f] = slot, ++moved;
  if (!moved) {
    mesh.faceMaterial = before;
    mesh.materialNames = namesBefore;
    return fail("those faces are in the texture set '" + mesh.slotName(slot) + "' already");
  }
  // the sets that still have faces, in their order; the new one goes last
  TileLayout t;
  t.mode = uint8_t(mesh.tiles.mode | TileBySlot);
  if (mesh.tiles.mode & TileBySlot) {
    TileLayout probe = mesh.tiles;
    probe.faceTile.clear();
    Mesh tmp;  // which of the listed sets keep faces
    tmp.faceStart = mesh.faceStart, tmp.loopVert = mesh.loopVert, tmp.loopUV = mesh.loopUV;
    tmp.faceMaterial = mesh.faceMaterial, tmp.materialNames = mesh.materialNames, tmp.tiles = probe;
    tmp.updateTiles();
    std::vector<int> used(tmp.tiles.tiles.size(), 0);
    for (uint32_t f = 0; f < F; ++f) used[std::clamp(tmp.tiles.faceTile[f], 0, (int)used.size() - 1)]++;
    for (size_t i = 0; i < mesh.tiles.tiles.size() && i < used.size(); ++i)
      if (used[i]) t.tiles.push_back(mesh.tiles.tiles[i]);
  }
  {  // does the atlas still fit?
    Mesh tmp;
    tmp.faceStart = mesh.faceStart, tmp.loopVert = mesh.loopVert, tmp.loopUV = mesh.loopUV;
    tmp.faceMaterial = mesh.faceMaterial, tmp.materialNames = mesh.materialNames, tmp.tiles = t;
    tmp.updateTiles();
    std::string w;
    if (!atlasFits(tileRes(), tmp.tiles.grid, &w)) {
      mesh.faceMaterial = before;
      mesh.materialNames = namesBefore;
      return fail(w);
    }
  }
  relayout(t);
  // a fresh name for every set (relayout keeps the names of the sets it was given)
  for (auto& ti : mesh.tiles.tiles) {
    std::string n = (mesh.tiles.mode & TileByUdim) ? mesh.slotName(ti.slot) + " " + udimName(ti.udim) : mesh.slotName(ti.slot);
    ti.name = n;
  }
  bridge.slotsEdited = true;
  dirty = true;
  for (int i = 0; i < (int)mesh.tiles.tiles.size(); ++i)
    if (mesh.tiles.tiles[i].slot == slot) return i;
  return 0;
}

int Document::moveFacesToNewUdim(const std::vector<uint32_t>& faces, std::string* why) {
  auto fail = [&](const std::string& w) {
    if (why) *why = w;
    return 0;
  };
  if (!hasMesh()) return fail("no mesh loaded");
  if (faces.empty()) return fail("no faces given (select some first)");
  if (!(mesh.tiles.mode & TileByUdim)) return fail("the layout has no UDIM tiles");
  const uint32_t F = mesh.faceCount();
  // the faces themselves: an island that crosses the part's border comes apart there, as with
  // texture sets per slot
  std::vector<uint8_t> isl(F, 0);
  for (uint32_t f : faces)
    if (f < F) isl[f] = 1;
  std::set<int> used;  // UDIMs with faces (a listed tile left empty can take them)
  for (uint32_t f = 0; f < F; ++f)
    if (!isl[f]) used.insert(mesh.tiles.tiles[std::clamp(mesh.faceTile(f), 0, mesh.tiles.count() - 1)].udim);
  int udim = 0;
  for (int u = 1002; u <= 1100 && !udim; ++u)
    if (!used.count(u)) udim = u;
  if (!udim) return fail("no free UDIM tile left");
  glm::ivec2 to = udimCorner(udim);
  std::vector<vec2> uvs = mesh.loopUV;
  for (uint32_t f = 0; f < F; ++f) {
    if (!isl[f]) continue;
    glm::ivec2 from = mesh.tiles.uvOrigin(std::clamp(mesh.faceTile(f), 0, mesh.tiles.count() - 1));
    for (uint32_t l = mesh.faceStart[f]; l < mesh.faceStart[f + 1]; ++l) uvs[l] += vec2(to - from);
  }
  // pinned islands go too (and stay pinned): the user asked for these faces to move
  if (!applyUVs(uvs, true, "Move to UDIM " + std::to_string(udim), nullptr, nullptr, true))
    return fail("the atlas has no room for another tile");
  return udim;
}

bool Document::loadMesh(const std::string& path, int resolution, bool keepLayers, std::string* err) {
  if (beforeMeshChange) beforeMeshChange();
  Mesh m;
  if (!patina::loadMesh(path, m, err)) return false;
  meshPath = path;
  setMesh(std::move(m), resolution, keepLayers);
  meshFileTime = fileTime(path);
  return true;
}

int64_t Document::fileTime(const std::string& path) {
  std::error_code ec;
  auto t = fs::last_write_time(path, ec);
  if (ec) return 0;
  return (int64_t)t.time_since_epoch().count();
}

std::string Document::incrementalPath() const {
  if (projectPath.empty()) return "";
  fs::path p(projectPath);
  std::string stem = p.stem().string();
  int num = 1, width = 3;
  size_t v = stem.rfind("_v");
  if (v != std::string::npos && v + 2 < stem.size() &&
      std::all_of(stem.begin() + v + 2, stem.end(), [](char c) { return c >= '0' && c <= '9'; })) {
    width = int(stem.size() - v - 2);
    num = std::stoi(stem.substr(v + 2));
    stem = stem.substr(0, v);
  }
  for (int n = num + 1; n < 100000; ++n) {
    char buf[32];
    snprintf(buf, sizeof buf, "_v%0*d", width, n);
    fs::path c = p.parent_path() / (stem + buf + p.extension().string());
    std::error_code ec;
    if (!fs::exists(c, ec)) return c.string();
  }
  return "";
}

bool Document::bakeHighPoly(const std::string& path, const HighPolyBakeOptions& opt, json* stats, std::string* err) {
  if (!highPoly || path != highPolyPath) {
    auto m = std::make_unique<Mesh>();
    if (!patina::loadMesh(path, *m, err)) return false;
    if (m->tris.empty()) m->finalize();
    highPoly = std::move(m);
    highPolyPath = path;
  }
  highPolyOptions = opt;
  checkpoint();
  if (!bakeFromHighPoly(*this, *highPoly, opt, stats, err)) return false;
  highPolyBaked = true;
  // a new base under every layer: not part of undo (like the other bakes)
  dirty = true;
  return true;
}

void Document::clearHighPoly() {
  highPoly.reset();
  highPolyPath.clear();
  highPolyBaked = false;
  tex.setHighNormal(false);
  tex.materialIdsFromHigh = false;
  if (hasMesh()) tex.bakeMaterialIds(mesh, gpu);
  markComposite();
}

bool Document::maskFromSelection(int index) {
  return maskFromFaces(index, selectedFaces(), "Mask from selection");
}

bool Document::maskFromFaces(int index, const std::vector<uint32_t>& faces, const std::string& label) {
  if (index < 0 || index >= (int)layers.size() || layers[index]->isBaked()) return false;
  if (faces.empty()) return false;
  checkpoint();
  std::vector<vec4> aux(mesh.loopCount(), vec4(0));
  for (uint32_t f : faces)
    for (uint32_t l = mesh.faceStart[f]; l < mesh.faceStart[f + 1]; ++l) aux[l] = vec4(1);
  Layer& l = *layers[index];
  auto mask = std::make_unique<Texture2D>(tex.resolution(), tex.resolution(), GL_R8, 1);
  tex.rasterizeAux(gpu, aux, *mask, vec4(0));
  tex.dilateInPlace(*mask);
  auto e = std::make_unique<TextureSwapEntry>();
  e->label = label + ": " + l.name;
  e->layerUid = l.uid;
  e->slots.push_back({-1, std::move(mask)});
  e->swap(*this);  // the new mask goes on the layer, the old one (or none) into the entry
  undo.push(std::move(e));
  pruneGraveyard();
  markComposite();
  return true;
}

// Paint transfer for a mesh with different topology: every texel of the new
// layout looks up the closest point on the old surface and samples the old
// layer textures at that point's UV.
static void transferPaint(Document& d, const Mesh& oldMesh, std::vector<std::unique_ptr<Layer>>& layers) {
  double t0 = nowSeconds();
  TextureSet& tex = d.tex;
  int R = tex.resolution();
  Image pos = tex.posMap.download(4);  // row 0 = top
  Bvh oldBvh;
  oldBvh.build(oldMesh);
  float maxDist = std::max(oldMesh.boundingRadius, d.mesh.boundingRadius) * 0.15f;
  std::vector<float> lookup(size_t(R) * R * 2, -1.f);
  unsigned nThreads = std::max(1u, std::min(16u, std::thread::hardware_concurrency()));
  std::vector<std::thread> pool;
  std::atomic<int> nextRow{0};
  for (unsigned t = 0; t < nThreads; ++t)
    pool.emplace_back([&] {
      for (int y; (y = nextRow++) < R;) {
        for (int x = 0; x < R; ++x) {
          const float* p = pos.at(x, y);
          if (p[3] < 0.5f) continue;
          RayHit h = oldBvh.closestPoint(oldMesh, vec3(p[0], p[1], p[2]), maxDist);
          if (!h.hit) continue;
          size_t gy = size_t(R - 1 - y);  // GL rows are bottom-up
          lookup[(gy * R + x) * 2] = h.uv.x;
          lookup[(gy * R + x) * 2 + 1] = h.uv.y;
        }
      }
    });
  for (auto& th : pool) th.join();
  Texture2D lut(R, R, GL_RG32F, 1);
  lut.setFilter(GL_NEAREST, GL_NEAREST);
  lut.uploadRaw(GL_RG, GL_FLOAT, lookup.data());
  for (auto& l : layers) {
    for (auto& c : l->channels)
      if (c && c->valid()) tex.resampleByLookup(lut, *c);
    if (l->mask) tex.resampleByLookup(lut, *l->mask);
  }
  PLOG_I("Transferred paint to the new topology in %.1f s (%u threads)", nowSeconds() - t0, nThreads);
}

bool Document::prepareUVs(Mesh& m, bool freshTiles) {
  if (freshTiles && !uvsLookEmpty(m)) {
    m.tiles = TileLayout();
    m.tiles.mode = detectTileMode(m);
    if (m.tiles.mode) {
      m.computeUVTopology();
      PLOG_I("Texture tiles: %s%s%s, %d tile(s)", (m.tiles.mode & TileBySlot) ? "a texture set per material slot" : "",
             m.tiles.mode == (TileBySlot | TileByUdim) ? " and " : "", (m.tiles.mode & TileByUdim) ? "UDIM tiles" : "",
             m.tiles.count());
    }
  }
  uvsGenerated = uvsLookEmpty(m);
  if (uvsGenerated) {
    PLOG_I("Mesh has no usable UVs - generating a smart UV projection");
    auto t0 = std::chrono::steady_clock::now();
    auto since = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
    uv::smartProject(m, 66.f);
    double tProject = since();
    m.computeUVTopology();
    double tTopo = since();
    m.computeTangents();
    PLOG_D("Smart UV projection %.2f s, UV topology %.2f s, tangents %.2f s", tProject, tTopo - tProject, since() - tTopo);
    return true;
  }
  if (!(m.tiles.mode & TileByUdim) && normalizeUVTiles(m)) {
    PLOG_I("Moved UV islands from other UV tiles into 0..1");
    return true;
  }
  return false;
}

void Document::setMesh(Mesh&& m, int resolution, bool keepLayers) {
  clearBakeDown();
  startupScene = false;
  bool topologyMatches = hasMesh() && m.loopCount() == mesh.loopCount() && m.faceCount() == mesh.faceCount();
  bool keep = keepLayers && topologyMatches && !layers.empty();
  bool transfer = keepLayers && !topologyMatches && hasMesh() && !layers.empty() && tex.resolution() > 0;
  // (0.44) the same object again without its UVs (Blender sent it from Edit Mode, or its UV map went): Patina's own
  // UVs carried over by where the faces are, not a new unwrap that throws the islands away
  uvsCarried = -1.f;
  if (keepLayers && hasMesh() && uvsLookEmpty(m) && !uvsLookEmpty(mesh)) {
    float share = uv::carryUVs(mesh, m);
    if (share >= 0.5f) {
      uvsCarried = share;
      m.computeUVTopology();
      m.computeTangents();
      PLOG_I("The mesh came without UVs: kept Patina's (%.1f%% of the faces where they were)", share * 100.f);
    } else {
      std::fill(m.loopUV.begin(), m.loopUV.end(), vec2(0.f));
      PLOG_W("The mesh came without UVs and only %.0f%% of its faces are where they were: unwrapping it again",
             share * 100.f);
    }
  }
  // texture tiles: the same object keeps its layout; a new one gets what its UVs call for
  if ((keep || transfer) && mesh.tiles.active()) {
    m.tiles.mode = mesh.tiles.mode;
    m.tiles.tiles = mesh.tiles.tiles;
    m.tiles.grid = mesh.tiles.grid;
    m.computeUVTopology();
  }
  if (prepareUVs(m, !keep && !transfer)) bridge.uvsEdited = true;
  if (transfer) {
    // different topology: keep the layers, transfer their paint through 3D
    bool ao = tex.hasAO(), curv = tex.hasCurvature(), thick = tex.hasThickness();
    for (auto& l : layers) {  // fill gutters so bilinear lookups at island borders stay clean
      for (auto& c : l->channels)
        if (c && c->valid()) tex.dilateInPlace(*c);
      if (l->mask) tex.dilateInPlace(*l->mask);
    }
    int R = resolution > 0 ? resolution : tileRes();
    while (R > 256 && !atlasFits(R, m.tiles.grid)) R /= 2;
    Mesh oldMesh = std::move(mesh);
    Bvh oldBvh = std::move(bvh);
    std::vector<uint32_t> oldSets = faceSetOf;
    bvh = Bvh();
    mesh = std::move(m);
    gpu.upload(mesh);
    bvh.build(mesh);
    carryFaceSets(oldMesh, oldBvh, oldSets);  // (1.2) by where the faces are
    if (R * mesh.tiles.grid != tex.resolution()) resizeAtlas(R * mesh.tiles.grid);
    tex.bakeMeshMaps(mesh, gpu);
    transferPaint(*this, oldMesh, layers);
    tex.setBaked(false, false);
    if (ao) tex.bakeAO(mesh, gpu, aoSamples);
    if (curv) tex.bakeCurvature(mesh, gpu, &bvh);
    if (thick) tex.bakeThickness(mesh, bvh, thicknessRays);
    clearHistory();
  } else if (keep) {
    // same topology: move paint from the old UV layout to the new one
    int R = resolution > 0 ? resolution : tileRes();
    std::vector<vec2> newUVs = m.loopUV;
    bool uvChanged = false;
    for (size_t i = 0; i < newUVs.size() && !uvChanged; ++i)
      uvChanged = glm::any(glm::greaterThan(glm::abs(newUVs[i] - mesh.loopUV[i]), vec2(1e-6f)));
    // texture sets: faces moved to another material slot change cell without changing UVs
    if (m.tiles.active() && m.tiles.faceTile != mesh.tiles.faceTile) uvChanged = true;
    if (m.tiles.active() && m.tiles.grid != mesh.tiles.grid) {  // a new tile: the atlas grows
      if (!atlasFits(R, m.tiles.grid)) {
        int r = R;
        while (r > 256 && !atlasFits(r, m.tiles.grid)) r /= 2;
        PLOG_W("%d texture tiles don't fit at %d px each: using %d px", m.tiles.count(), R, r);
        changeResolution(r);
        R = r;
      }
      relayout(m.tiles);
    }
    if (uvChanged) reprojectAll(m.loopAtlasUV.empty() ? newUVs : m.loopAtlasUV);
    mesh = std::move(m);
    gpu.upload(mesh);
    bvh.build(mesh);
    if (R * mesh.tiles.grid != tex.resolution()) changeResolution(R);
    tex.bakeMeshMaps(mesh, gpu);
    clearHistory();
  } else {
    int R = resolution > 0 ? resolution : (tex.resolution() ? tileRes() : 2048);
    while (R > 256 && !atlasFits(R, m.tiles.grid)) R /= 2;  // many tiles: a smaller texture per tile
    mesh = std::move(m);
    gpu.upload(mesh);
    bvh.build(mesh);
    newTextureSet(R);
    symmetryPlane = NAN;  // another model: back to the automatic plane
    faceSets.clear(), faceSetOf.clear(), hiddenFaces.clear(), nextFaceSetId = 1, hideUnsetFaces = false;
  }
  // (1.2) Blender's sculpt face sets, when Patina has none for the model yet
  if (mesh.faceSetIds.size() == mesh.faceCount() && faceSets.empty()) {
    std::vector<uint32_t> ids = mesh.faceSetIds;
    std::vector<uint32_t> distinct(ids);
    std::sort(distinct.begin(), distinct.end());
    distinct.erase(std::unique(distinct.begin(), distinct.end()), distinct.end());
    distinct.erase(std::remove(distinct.begin(), distinct.end(), 0u), distinct.end());
    if (distinct.size() > 1 && distinct.size() <= 4096) {
      nextFaceSetId = 1;
      std::vector<uint32_t> of(mesh.faceCount(), 0);
      for (uint32_t blenderId : distinct) {
        FaceSet s;
        s.id = nextFaceSetId++;
        s.name = "Face Set " + std::to_string(blenderId);
        s.color = faceSetColor(s.id);
        faceSets.push_back(s);
        for (uint32_t f = 0; f < mesh.faceCount(); ++f)
          if (ids[f] == blenderId) of[f] = s.id;
      }
      faceSetOf = std::move(of);
      PLOG_I("%zu face sets came with the model (Blender's sculpt face sets)", faceSets.size());
    }
  }
  if (faceSetOf.size() != mesh.faceCount()) faceSetOf.clear(), hiddenFaces.clear();
  faceSelection.assign(mesh.faceCount(), 0);
  updateHiddenFaces();
  markComposite();
}

void Document::newTextureSet(int resolution) {
  layers.clear();
  notes.clear();
  nextNoteId = 1;
  selected = -1;
  editMask = false;
  clearHistory();
  tex.allocate(resolution * std::max(1, mesh.tiles.grid));
  if (hasMesh()) tex.bakeMeshMaps(mesh, gpu);
  soloUid = 0;
  addPaintLayer("Layer 1");
  clearHistory();
  markComposite();
}

void Document::changeResolution(int tileR) {
  int r = tileR * std::max(1, mesh.tiles.grid);
  if (r == tex.resolution()) return;
  bool ao = tex.hasAO(), curv = tex.hasCurvature(), thick = tex.hasThickness();
  resizeAtlas(r);
  if (hasMesh()) rebakeMeshMaps(ao, curv, thick);
  if (hasMesh() && highPoly && highPolyBaked) {
    std::string err;
    if (!bakeFromHighPoly(*this, *highPoly, highPolyOptions, nullptr, &err)) PLOG_W("High-poly re-bake: %s", err.c_str());
  }
  clearHistory();
  markComposite();
}

void Document::resizeAtlas(int r) {
  auto resample = [&](std::unique_ptr<Texture2D>& t) {
    if (!t || !t->valid()) return;
    auto n = std::make_unique<Texture2D>(r, r, t->format(), 1);
    Framebuffer src, dst;
    src.attach(0, *t);
    dst.attach(0, *n);
    glBlitNamedFramebuffer(src.id(), dst.id(), 0, 0, t->width(), t->height(), 0, 0, r, r, GL_COLOR_BUFFER_BIT,
                           GL_LINEAR);
    t = std::move(n);
  };
  for (auto& l : layers) {
    for (auto& c : l->channels) resample(c);
    resample(l->mask);
  }
  bool ao = tex.hasAO(), curv = tex.hasCurvature(), thick = tex.hasThickness();
  tex.allocate(r);
  tex.setBaked(false, false);
  (void)ao, (void)curv, (void)thick;
}

void Document::rebakeMeshMaps(bool ao, bool curvature, bool thickness) {
  if (!hasMesh()) return;
  tex.bakeMeshMaps(mesh, gpu);
  if (ao) tex.bakeAO(mesh, gpu, aoSamples);
  if (curvature) tex.bakeCurvature(mesh, gpu, &bvh);
  if (thickness) tex.bakeThickness(mesh, bvh, thicknessRays);
  markComposite();
}

int Document::bakesOutOfDate() const {
  if (!hasMesh()) return 0;
  uint64_t now = uv::uvHash(mesh);
  int out = 0;
  for (int k = 0; k < kBakedMapCount; ++k)
    if (tex.hasBakedMap(k) && tex.bakedUV[k] && tex.bakedUV[k] != now) out |= 1 << k;
  return out;
}

void Document::rebakeOutOfDate() {
  int k = bakesOutOfDate();
  if (!k) return;
  rebakeMeshMaps(k & 1, k & 2, k & 4);
  if (highPoly && highPolyBaked) {
    std::string err;
    if (!bakeFromHighPoly(*this, *highPoly, highPolyOptions, nullptr, &err)) PLOG_W("High-poly re-bake: %s", err.c_str());
  }
  PLOG_I("Baked again for the UVs as they are: %s%s%s", k & 1 ? "ambient occlusion " : "", k & 2 ? "curvature " : "",
         k & 4 ? "thickness" : "");
}

void Document::ensureBakedFor(const Layer& l) {
  if (!hasMesh()) return;
  if (l.needsCurvature() && !tex.hasCurvature()) tex.bakeCurvature(mesh, gpu, &bvh);
  if (l.needsAO() && !tex.hasAO()) tex.bakeAO(mesh, gpu, aoSamples);
  if (l.needsThickness() && !tex.hasThickness()) tex.bakeThickness(mesh, bvh, thicknessRays);
}

Layer* Document::bakedLayer(int map) {
  for (auto& l : layers)
    if (l->isBaked() && l->bakedMap == map) return l.get();
  return nullptr;
}

static std::unique_ptr<Layer> makeBakedLayer(uint32_t uid, int map) {
  auto l = std::make_unique<Layer>();
  l->uid = uid;
  l->type = LayerType::Baked;
  l->bakedMap = map;
  l->name = bakedMapLabel(map);
  return l;
}

void Document::syncBakedLayers() {
  if (!bakedLayersOn || !hasMesh()) return;  // (switched on later: the bakes there are get layers then)
  syncedBakeSerial_ = tex.bakeSerial();
  bool added = false;
  // at the bottom of the stack, AO lowest (they add nothing to the material: where they sit does not matter)
  for (int k = kBakedMapCount - 1; k >= 0; --k) {
    if (!tex.hasBakedMap(k) || bakedLayer(k)) continue;
    layers.insert(layers.begin(), makeBakedLayer(nextUid++, k));
    if (selected >= 0) ++selected;
    added = true;
  }
  if (added) {
    markComposite();
    if (committedValid_ && pendingLabel.empty()) pendingLabel = "Baked map layers";
  }
}

bool Document::addBakedLayer(int map) {
  if (map < 0 || map >= kBakedMapCount || !tex.hasBakedMap(map)) return false;
  if (bakedLayer(map)) return true;
  checkpoint();
  layers.insert(layers.begin(), makeBakedLayer(nextUid++, map));
  selected = 0;
  editMask = false;
  markComposite();
  checkpoint(std::string("Add ") + bakedMapLabel(map));
  return true;
}

// ------------------------------------------------------------------ layers

static std::unique_ptr<Layer> makeLayer(uint32_t uid, LayerType type, const std::string& name, const char* prefix) {
  auto l = std::make_unique<Layer>();
  l->uid = uid;
  l->type = type;
  l->name = name.empty() ? std::string(prefix) + " " + std::to_string(uid) : name;
  return l;
}

// Inserts a new layer. index < 0: above the selected layer, in the same folder, or at
// the top of the selected folder. index >= 0: at that flat index (parent as set).
int Document::insertAt(std::unique_ptr<Layer> l, int index) {
  checkpoint();
  int at;
  if (index >= 0) {
    at = std::min(index, (int)layers.size());
  } else if (Layer* sel = selectedLayer()) {
    if (sel->isFolder()) {
      l->parent = sel->uid;
      sel->expanded = true;
      at = selected;  // directly below the folder row = top of its children
    } else {
      l->parent = effectiveParent(*sel);
      at = selected + 1;
    }
  } else {
    at = (int)layers.size();
  }
  layers.insert(layers.begin() + at, std::move(l));
  selected = at;
  editMask = false;
  markComposite();
  checkpoint("Add " + layers[at]->name);
  return at;
}

Layer& Document::addPaintLayer(const std::string& name, int index) {
  return *layers[insertAt(makeLayer(nextUid++, LayerType::Paint, name, "Paint"), index)];
}

Layer& Document::addFillLayer(const MaterialRef& mat, const std::string& name, int index) {
  auto l = makeLayer(nextUid++, LayerType::Fill, name, "Fill");
  l->material = mat;
  return *layers[insertAt(std::move(l), index)];
}

Layer& Document::addAdjustLayer(const std::string& name, int index) {
  auto l = makeLayer(nextUid++, LayerType::Adjust, name, "Adjust");
  l->blend = BlendMode::Normal;
  l->channelMask = 1 | 2 | 4 | 8;  // color, roughness, metallic, height
  return *layers[insertAt(std::move(l), index)];
}

Layer& Document::addFolder(const std::string& name, int index) {
  auto l = makeLayer(nextUid++, LayerType::Folder, name, "Folder");
  return *layers[insertAt(std::move(l), index)];
}

// ---- tree helpers

uint32_t Document::effectiveParent(const Layer& l) const {
  if (!l.parent || l.parent == l.uid) return 0;
  for (auto& o : layers)
    if (o->uid == l.parent) return o->isFolder() ? l.parent : 0;
  return 0;
}

std::vector<int> Document::childrenOf(uint32_t parentUid) const {
  std::vector<int> r;
  for (size_t i = 0; i < layers.size(); ++i)
    if (effectiveParent(*layers[i]) == parentUid) r.push_back((int)i);
  return r;
}

bool Document::isDescendant(uint32_t uid, uint32_t ancestorUid) const {
  int i = indexOf(uid);
  for (int guard = 0; i >= 0 && guard < 64; ++guard) {
    uint32_t p = effectiveParent(*layers[i]);
    if (!p) return false;
    if (p == ancestorUid) return true;
    i = indexOf(p);
  }
  return false;
}

std::vector<int> Document::subtree(int index) const {
  std::vector<int> r;
  if (index < 0 || index >= (int)layers.size()) return r;
  uint32_t root = layers[index]->uid;
  for (size_t i = 0; i < layers.size(); ++i)
    if ((int)i == index || isDescendant(layers[i]->uid, root)) r.push_back((int)i);
  return r;
}

int Document::depthOf(int index) const {
  int d = 0;
  for (int i = index; i >= 0 && d < 64; ++d) {
    uint32_t p = effectiveParent(*layers[i]);
    if (!p) return d;
    i = indexOf(p);
  }
  return d;
}

// Reorders the flat vector so that every folder's subtree is contiguous and directly
// below the folder (bottom-to-top order of siblings is kept). Layers caught in a parent
// cycle are moved to the top level.
static void normalizeOrder(Document& d) {
  std::vector<std::unique_ptr<Layer>> out;
  out.reserve(d.layers.size());
  std::vector<uint8_t> used(d.layers.size(), 0);
  std::vector<uint32_t> eff(d.layers.size());
  for (size_t i = 0; i < d.layers.size(); ++i) eff[i] = d.effectiveParent(*d.layers[i]);
  std::function<void(uint32_t, int)> emit = [&](uint32_t p, int depth) {
    for (size_t i = 0; i < d.layers.size(); ++i) {
      if (used[i] || eff[i] != p) continue;
      used[i] = 1;
      if (d.layers[i]->isFolder() && depth < 32) emit(d.layers[i]->uid, depth + 1);
      out.push_back(std::move(d.layers[i]));
    }
  };
  uint32_t selUid = d.selectedLayer() ? d.selectedLayer()->uid : 0;
  emit(0, 0);
  for (size_t i = 0; i < d.layers.size(); ++i)
    if (!used[i]) {
      d.layers[i]->parent = 0;
      out.push_back(std::move(d.layers[i]));
    }
  d.layers = std::move(out);
  if (selUid) d.selected = d.indexOf(selUid);
}

// Takes the (contiguous, after normalizeOrder) subtree of `index` out of the stack.
static std::vector<std::unique_ptr<Layer>> takeBlock(Document& d, int index) {
  std::vector<int> idx = d.subtree(index);
  std::vector<std::unique_ptr<Layer>> block;
  for (int i : idx) block.push_back(std::move(d.layers[i]));
  for (auto it = idx.rbegin(); it != idx.rend(); ++it) d.layers.erase(d.layers.begin() + *it);
  return block;
}

const Layer* Document::contentLayer(const Layer& l) const {
  const Layer* cur = &l;
  for (int guard = 0; guard < 8 && cur->instanceOf; ++guard) {
    const Layer* src = const_cast<Document*>(this)->findLayer(cur->instanceOf);
    if (!src || src == &l) break;
    cur = src;
  }
  return cur;
}
Layer* Document::contentLayer(Layer& l) { return const_cast<Layer*>(static_cast<const Document*>(this)->contentLayer(static_cast<const Layer&>(l))); }

static void copyContent(const Layer& from, Layer& to, ivec2 res) {
  to.type = from.type == LayerType::Folder ? LayerType::Paint : from.type;
  to.material = from.material;
  to.adjust = from.adjust;
  for (int c = 0; c < kChannelCount; ++c) {
    if (from.hasChannel(c)) to.ensureChannel(c, res).copyFrom(*from.channels[c]);
    else to.channels[c].reset();
  }
}

int Document::addInstance(int index) {
  if (index < 0 || index >= (int)layers.size()) return -1;
  Layer& src = *layers[index];
  if (src.isFolder() || src.isBaked()) return -1;
  checkpoint();
  const Layer* content = contentLayer(src);
  auto l = std::make_unique<Layer>();
  l->uid = nextUid++;
  l->type = content->type;
  l->name = content->name + " (instance)";
  l->instanceOf = content->uid;
  l->parent = src.parent;
  l->material = content->material;  // shown in the settings; the source's is used
  l->adjust = content->adjust;
  layers.insert(layers.begin() + index + 1, std::move(l));
  selected = index + 1;
  editMask = false;
  markComposite();
  checkpoint("Instance of " + content->name);
  return selected;
}

bool Document::makeIndependent(int index) {
  if (index < 0 || index >= (int)layers.size() || !layers[index]->instanceOf) return false;
  checkpoint();
  Layer& l = *layers[index];
  const Layer* src = contentLayer(l);
  if (src != &l) copyContent(*src, l, tex.size());
  l.instanceOf = 0;
  markComposite();
  checkpoint("Make independent: " + l.name);
  return true;
}

bool Document::removeLayer(int index) {
  if (index < 0 || index >= (int)layers.size()) return false;
  checkpoint();
  std::string name = layers[index]->name;
  {
    // instances of a deleted layer keep its content: the first one gets a copy and the
    // others follow it (the deleted layer itself stays intact for undo)
    std::vector<uint32_t> gone;
    for (auto& l : layers)
      if (l->uid == layers[index]->uid || isDescendant(l->uid, layers[index]->uid)) gone.push_back(l->uid);
    for (uint32_t g : gone) {
      Layer* src = findLayer(g);
      Layer* heir = nullptr;
      for (auto& l : layers) {
        if (l->instanceOf != g || std::find(gone.begin(), gone.end(), l->uid) != gone.end()) continue;
        if (!heir) {
          heir = l.get();
          copyContent(*src, *heir, tex.size());
          heir->instanceOf = 0;
        } else {
          l->instanceOf = heir->uid;
        }
      }
    }
  }
  for (auto& l : takeBlock(*this, index)) {
    if (soloUid == l->uid) soloUid = 0;
    uint32_t uid = l->uid;
    graveyard[uid] = std::move(l);
  }
  selected = std::min(index, (int)layers.size() - 1);
  editMask = false;
  markComposite();
  checkpoint("Delete " + name);
  return true;
}

// Flat move (automation API): moves the layer (with its contents if it is a folder) so
// that it ends up at flat index `to`; it joins the folder of the layer it lands beside.
bool Document::moveLayer(int from, int to) {
  if (from < 0 || from >= (int)layers.size() || to < 0 || to >= (int)layers.size() || from == to) return false;
  checkpoint();
  normalizeOrder(*this);
  uint32_t uid = layers[from]->uid, toUid = layers[to]->uid;
  if (isDescendant(toUid, uid)) return false;
  auto block = takeBlock(*this, indexOf(uid));
  int t = indexOf(toUid);
  int at = to > from ? t + 1 : t;
  block.back()->parent = effectiveParent(*layers[t]);
  int n = (int)block.size();
  for (auto& l : block) layers.insert(layers.begin() + at++, std::move(l));
  selected = at - 1;
  (void)n;
  normalizeOrder(*this);
  selected = indexOf(uid);
  markComposite();
  checkpoint("Move layer");
  return true;
}

bool Document::moveSibling(int index, int dir) {
  if (index < 0 || index >= (int)layers.size() || dir == 0 || layers[index]->isBaked()) return false;
  checkpoint();
  normalizeOrder(*this);
  uint32_t uid = layers[index]->uid;
  index = indexOf(uid);
  std::vector<int> sib = childrenOf(effectiveParent(*layers[index]));
  int k = int(std::find(sib.begin(), sib.end(), index) - sib.begin());
  int k2 = k + (dir > 0 ? 1 : -1);
  if (k2 < 0 || k2 >= (int)sib.size()) return false;
  uint32_t other = layers[sib[k2]]->uid;
  auto block = takeBlock(*this, indexOf(uid));
  int o = indexOf(other);
  int at = dir > 0 ? o + 1 : subtree(o).front();
  for (auto& l : block) layers.insert(layers.begin() + at++, std::move(l));
  selected = indexOf(uid);
  markComposite();
  checkpoint(dir > 0 ? "Move layer up" : "Move layer down");
  return true;
}

bool Document::moveOnto(int from, int target, bool into, bool below) {
  if (from < 0 || from >= (int)layers.size() || target >= (int)layers.size() || from == target) return false;
  if (layers[from]->isBaked() || (target >= 0 && into && layers[target]->isBaked())) return false;
  uint32_t uid = layers[from]->uid;
  uint32_t tUid = target >= 0 ? layers[target]->uid : 0;
  if (tUid && isDescendant(tUid, uid)) return false;
  if (into && (!tUid || !layers[target]->isFolder())) into = false;
  checkpoint();
  normalizeOrder(*this);
  auto block = takeBlock(*this, indexOf(uid));
  int at;
  if (!tUid) {  // bottom of the stack
    at = 0;
    block.back()->parent = 0;
  } else if (into) {
    at = indexOf(tUid);  // top of the folder's children
    block.back()->parent = tUid;
    layers[at]->expanded = true;
  } else {
    int t = indexOf(tUid);
    // above the target (and above its contents if it is a folder), or right under its block
    at = below ? subtree(t).front() : t + 1;
    block.back()->parent = effectiveParent(*layers[t]);
  }
  for (auto& l : block) layers.insert(layers.begin() + at++, std::move(l));
  selected = indexOf(uid);
  markComposite();
  checkpoint(into ? "Move into folder" : "Move layer");
  return true;
}

int Document::dropIndex(int target, bool into, bool below, uint32_t* parent) {
  uint32_t tUid = target >= 0 && target < (int)layers.size() ? layers[target]->uid : 0;
  normalizeOrder(*this);
  if (into && (!tUid || !findLayer(tUid)->isFolder())) into = false;
  if (!tUid) {
    if (parent) *parent = 0;
    return 0;
  }
  int t = indexOf(tUid);
  if (into) {
    if (parent) *parent = tUid;
    layers[t]->expanded = true;
    return t;  // (top of the folder's children)
  }
  if (parent) *parent = effectiveParent(*layers[t]);
  return below ? subtree(t).front() : t + 1;
}

Layer& Document::addFillLayerAt(const MaterialRef& mat, const std::string& name, int target, bool into, bool below) {
  checkpoint();
  uint32_t parent = 0;
  int at = dropIndex(target, into, below, &parent);
  auto l = makeLayer(nextUid++, LayerType::Fill, name, "Fill");
  l->material = mat;
  l->parent = parent;
  layers.insert(layers.begin() + at, std::move(l));
  selected = at;
  editMask = false;
  markComposite();
  checkpoint("Add " + layers[at]->name);
  return *layers[at];
}

bool Document::replaceFillMaterial(int index, const MaterialRef& mat, const std::string& oldName, const std::string& newName) {
  if (index < 0 || index >= (int)layers.size()) return false;
  Layer* c = contentLayer(*layers[index]);
  if (!c || c->type != LayerType::Fill) return false;
  checkpoint();
  c->material = mat;
  for (Layer* l : {layers[index].get(), c})
    if (!oldName.empty() && !newName.empty() && oldName != newName && l->name == oldName) l->name = newName;
  markComposite();
  checkpoint("Material: " + layers[index]->name);
  return true;
}

int Document::copyLayerTo(int from, int target, bool into, bool below) {
  if (from < 0 || from >= (int)layers.size() || layers[from]->isBaked()) return -1;
  uint32_t tUid = target >= 0 && target < (int)layers.size() ? layers[target]->uid : 0;
  beginUndoGroup("Copy " + layers[from]->name);
  int c = duplicateLayer(from);
  if (c >= 0 && tUid != layers[c]->uid) moveOnto(c, tUid ? indexOf(tUid) : -1, into, below);
  endUndoGroup();
  return selected;
}

bool Document::copyMaskTo(int from, int to) {
  if (from < 0 || to < 0 || from >= (int)layers.size() || to >= (int)layers.size() || from == to) return false;
  if (layers[from]->isBaked() || layers[to]->isBaked()) return false;  // (their "mask" is their fixes)
  const Layer& a = *layers[from];
  if (!a.mask && a.maskEffects.empty() && a.generator.type == MaskGenerator::None) return false;
  beginUndoGroup("Copy the mask of " + a.name + " to " + layers[to]->name);
  Layer& b = *layers[to];
  // (the painted mask: swapped in, the one it had kept for undo)
  auto e = std::make_unique<TextureSwapEntry>();
  e->label = "Copy mask";
  e->layerUid = b.uid;
  std::unique_ptr<Texture2D> copy;
  if (a.mask) {
    copy = std::make_unique<Texture2D>(a.mask->width(), a.mask->height(), a.mask->format(), 1);
    copy->copyFrom(*a.mask);
  }
  if (copy || b.mask) {
    e->slots.push_back({-1, std::move(copy)});
    e->swap(*this);
    undo.push(std::move(e));
  }
  b.generator = a.generator;
  b.maskEffects = a.maskEffects;
  ensureBakedFor(b);
  markComposite();
  endUndoGroup();
  return true;
}

bool Document::addSmartMask(int index, int generatorType) {
  if (index < 0 || index >= (int)layers.size() || generatorType <= MaskGenerator::None || generatorType >= MaskGenerator::Count) return false;
  if (layers[index]->isBaked()) return false;
  Layer& l = *layers[index];
  checkpoint();
  MaskGenerator g;
  g.type = generatorType;
  if (l.maskEffects.empty()) {
    l.generator = g;
  } else {
    MaskEffect fx;
    fx.type = MaskEffect::Generator, fx.blend = MaskEffect::Multiply, fx.gen = g;
    l.maskEffects.push_back(fx);
  }
  ensureBakedFor(l);
  markComposite();
  checkpoint(std::string("Smart mask: ") + maskGeneratorName(generatorType) + " on " + l.name);
  return true;
}

bool Document::setParent(int index, uint32_t folderUid) {
  if (index < 0 || index >= (int)layers.size() || (folderUid && layers[index]->isBaked())) return false;
  if (!folderUid) {
    if (!effectiveParent(*layers[index])) return true;
    checkpoint();
    normalizeOrder(*this);
    uint32_t uid = layers[index]->uid;
    auto block = takeBlock(*this, indexOf(uid));
    block.back()->parent = 0;
    for (auto& l : block) layers.push_back(std::move(l));
    selected = indexOf(uid);
    markComposite();
    checkpoint("Move out of folder");
    return true;
  }
  int f = indexOf(folderUid);
  if (f < 0 || !layers[f]->isFolder() || f == index) return false;
  return moveOnto(index, f, true);
}

int Document::groupIntoFolder(int index) {
  if (index < 0 || index >= (int)layers.size() || layers[index]->isBaked()) return -1;
  checkpoint();
  normalizeOrder(*this);
  uint32_t uid = layers[index]->uid;
  index = indexOf(uid);  // the root of its (contiguous) block
  auto f = makeLayer(nextUid++, LayerType::Folder, "", "Folder");
  f->parent = effectiveParent(*layers[index]);
  layers[index]->parent = f->uid;
  layers.insert(layers.begin() + index + 1, std::move(f));
  selected = index + 1;
  editMask = false;
  markComposite();
  checkpoint("Group into folder");
  return selected;
}

int Document::duplicateLayer(int index) {
  if (index < 0 || index >= (int)layers.size() || layers[index]->isBaked()) return -1;  // (one layer per baked map)
  checkpoint();
  normalizeOrder(*this);
  uint32_t rootUid = layers[index]->uid;
  index = indexOf(rootUid);
  std::vector<int> idx = subtree(index);  // contiguous, root last
  std::map<uint32_t, uint32_t> remap;
  for (int i : idx) remap[layers[i]->uid] = nextUid++;
  std::vector<std::unique_ptr<Layer>> copies;
  for (int i : idx) {
    const Layer& src = *layers[i];
    auto l = std::make_unique<Layer>();
    l->fromJson(src.toJson());
    l->uid = remap[src.uid];
    if (remap.count(src.parent) && i != index) l->parent = remap[src.parent];
    if (i == index) l->name = src.name + " copy";
    for (int c = 0; c < kChannelCount; ++c)
      if (src.hasChannel(c)) l->ensureChannel(c, src.channels[c]->size()).copyFrom(*src.channels[c]);
    if (src.mask) l->ensureMask(src.mask->size()).copyFrom(*src.mask);
    copies.push_back(std::move(l));
  }
  int at = index + 1;
  for (auto& l : copies) layers.insert(layers.begin() + at++, std::move(l));
  selected = at - 1;
  markComposite();
  checkpoint("Duplicate " + layers[index]->name);
  return selected;
}

bool Document::clearPaint(int index, int channelMask) {
  if (index < 0 || index >= (int)layers.size()) return false;
  checkpoint();
  Layer& l = *layers[index];
  auto e = std::make_unique<TextureSwapEntry>();
  e->label = "Clear paint: " + l.name;
  e->layerUid = l.uid;
  for (int c = 0; c < kChannelCount; ++c)
    if (l.hasChannel(c) && (channelMask & (1 << c))) e->slots.push_back({c, nullptr});
  if (e->slots.empty()) return false;
  e->swap(*this);  // channels move into the entry; the mask stays
  undo.push(std::move(e));
  pruneGraveyard();
  markComposite();
  return true;
}

void Document::addMask(int index, float value) {
  if (index < 0 || index >= (int)layers.size() || layers[index]->mask || layers[index]->isBaked()) return;
  checkpoint();
  Layer& l = *layers[index];
  l.ensureMask(tex.size(), value);
  auto e = std::make_unique<TextureSwapEntry>();
  e->label = std::string(value > 0.5f ? "Add white mask: " : "Add black mask: ") + l.name;
  e->layerUid = l.uid;
  e->slots.push_back({-1, nullptr});  // undo swaps the new mask out
  undo.push(std::move(e));
  pruneGraveyard();
  markComposite();
}

void Document::removeMask(int index) {
  if (index < 0 || index >= (int)layers.size() || !layers[index]->mask) return;
  checkpoint();
  Layer& l = *layers[index];
  auto e = std::make_unique<TextureSwapEntry>();
  e->label = (l.isBaked() ? "Reset the fixes: " : "Remove mask: ") + l.name;  // (a baked map layer's mask: its fixes)
  e->layerUid = l.uid;
  e->slots.push_back({-1, nullptr});
  e->swap(*this);
  undo.push(std::move(e));
  pruneGraveyard();
  if (selected == index) editMask = false;
  markComposite();
}

// ---- undoable stack state

StackState Document::captureState() const {
  StackState s;
  s.order.reserve(layers.size());
  s.meta.reserve(layers.size());
  for (auto& l : layers) {
    json j = l->toJson();
    j.erase("has_mask");
    j.erase("expanded");
    s.order.push_back(l->uid);
    s.meta.push_back(j.dump());
  }
  return s;
}

void Document::applyState(const StackState& s) {
  uint32_t selUid = selectedLayer() ? selectedLayer()->uid : 0;
  int oldSel = selected;
  std::map<uint32_t, std::unique_ptr<Layer>> pool;
  for (auto& l : layers) {
    uint32_t uid = l->uid;
    pool[uid] = std::move(l);
  }
  layers.clear();
  for (size_t i = 0; i < s.order.size(); ++i) {
    uint32_t uid = s.order[i];
    std::unique_ptr<Layer> l;
    if (auto it = pool.find(uid); it != pool.end()) {
      l = std::move(it->second);
      pool.erase(it);
    } else if (auto g = graveyard.find(uid); g != graveyard.end()) {
      l = std::move(g->second);
      graveyard.erase(g);
    } else {
      PLOG_W("Undo: layer %u is gone", uid);
      continue;
    }
    bool exp = l->expanded;
    l->fromJson(json::parse(s.meta[i]));
    l->expanded = exp;
    layers.push_back(std::move(l));
  }
  for (auto& [uid, l] : pool) graveyard[uid] = std::move(l);
  selected = selUid ? indexOf(selUid) : -1;
  if (selected < 0) selected = std::min(oldSel, (int)layers.size() - 1);
  if (selected < 0 && !layers.empty()) selected = 0;
  if (soloUid && indexOf(soloUid) < 0) soloUid = 0;
  if (Layer* l = selectedLayer(); editMask && l && !l->masksOnly() && !l->mask) editMask = false;
  markComposite();
}

// Names the change between two states for the history list.
static std::string describeChange(const StackState& a, const StackState& b) {
  if (a.order != b.order) {
    if (b.order.size() > a.order.size()) return "Add layer";
    if (b.order.size() < a.order.size()) return "Delete layer";
    return "Move layer";
  }
  static const std::pair<const char*, const char*> names[] = {
      {"opacity", "Opacity"},        {"blend", "Blend mode"},       {"visible", "Visibility"},
      {"name", "Rename"},            {"material", "Material"},      {"adjust", "Adjustment"},
      {"generator", "Smart mask"},   {"channel_mask", "Channels"},  {"channel_opacity", "Channel opacity"},
      {"parent", "Move to folder"},  {"color_tag", "Color tag"},   {"type", "Layer type"}};
  for (size_t i = 0; i < a.meta.size(); ++i) {
    if (a.meta[i] == b.meta[i]) continue;
    json ja = json::parse(a.meta[i]), jb = json::parse(b.meta[i]);
    std::string layer = jb.value("name", std::string());
    for (auto& [key, label] : names)
      if (ja.value(key, json()) != jb.value(key, json())) return std::string(label) + ": " + layer;
    return "Layer settings: " + layer;
  }
  return "Layer settings";
}

bool Document::checkpoint(const std::string& label) {
  if (tex.bakeSerial() != syncedBakeSerial_) syncBakedLayers();  // (0.41) a bake: its map's layer
  if (!committedValid_) {
    committed_ = captureState();
    committedValid_ = true;
    pendingLabel.clear();
    return false;
  }
  StackState s = captureState();
  if (s == committed_) {
    pendingLabel.clear();  // the labeled change did not happen (or was reverted)
    return false;
  }
  auto e = std::make_unique<StackStateEntry>();
  e->label = !label.empty() ? label : !pendingLabel.empty() ? pendingLabel : describeChange(committed_, s);
  for (uint32_t uid : committed_.order)  // layers this step removed count against the undo budget
    if (auto g = graveyard.find(uid); g != graveyard.end()) e->size += g->second->gpuBytes();
  e->before = std::move(committed_);
  e->after = s;
  committed_ = std::move(s);
  pendingLabel.clear();
  undo.push(std::move(e));
  pruneGraveyard();
  return true;
}

void Document::pruneGraveyard() {
  if (graveyard.empty()) return;
  std::set<uint32_t> keep;
  for (size_t i = 0; i < undo.count(); ++i)
    undo.entry(i).visit([&](const UndoEntry& entry) {  // groups hold stack edits too
      if (auto* e = dynamic_cast<const StackStateEntry*>(&entry)) {
        keep.insert(e->before.order.begin(), e->before.order.end());
        keep.insert(e->after.order.begin(), e->after.order.end());
      }
    });
  for (auto it = graveyard.begin(); it != graveyard.end();)
    it = keep.count(it->first) ? std::next(it) : graveyard.erase(it);
}

void Document::clearHistory() {
  undo.clear();
  graveyard.clear();
  committedValid_ = false;
  pendingLabel.clear();
}

void Document::beginUndoGroup(const std::string& label) {
  checkpoint();
  undo.beginGroup(label);
}

bool Document::endUndoGroup() {
  checkpoint();
  bool pushed = undo.endGroup();
  pruneGraveyard();
  return pushed;
}

void Document::cancelUndoGroup() {
  checkpoint();
  undo.cancelGroup(*this);
  committed_ = captureState();
  committedValid_ = true;
  pruneGraveyard();
  markComposite();
}

bool Document::undoStep() {
  checkpoint();
  bool ok = undo.undo(*this);
  committed_ = captureState();
  committedValid_ = true;
  return ok;
}

bool Document::redoStep() {
  checkpoint();
  bool ok = undo.redo(*this);
  committed_ = captureState();
  committedValid_ = true;
  return ok;
}

void Document::jumpTo(size_t position) {
  checkpoint();
  position = std::min(position, undo.count());
  while (undo.position() > position && undo.undo(*this)) {}
  while (undo.position() < position && undo.redo(*this)) {}
  committed_ = captureState();
  committedValid_ = true;
}

Layer* Document::findLayer(uint32_t uid) {
  for (auto& l : layers)
    if (l->uid == uid) return l.get();
  return nullptr;
}

int Document::indexOf(uint32_t uid) const {
  for (size_t i = 0; i < layers.size(); ++i)
    if (layers[i]->uid == uid) return (int)i;
  return -1;
}

void Document::updateComposite(MaterialLibrary& lib) {
  if (tex.bakeSerial() != syncedBakeSerial_) syncBakedLayers();
  if (!compositeDirty || !hasMesh()) return;
  if (soloUid && indexOf(soloUid) < 0) soloUid = 0;
  tex.composite(layers, lib, mesh.bounds, soloUid, compositeFull ? nullptr : &compositeBox);
  compositeDirty = false;
  compositeFull = true;
}

json Document::layerStackJson() const {
  json arr = json::array();
  for (size_t i = 0; i < layers.size(); ++i) {
    json j = layers[i]->toJson();
    j["index"] = i;
    j["selected"] = (int)i == selected;
    json ch = json::array();
    for (int c = 0; c < kChannelCount; ++c)
      if (layers[i]->hasChannel(c)) ch.push_back(channelInfo((Channel)c).key);
    j["painted_channels"] = ch;
    arr.push_back(j);
  }
  return arr;
}

// ------------------------------------------------------------------ UVs

void Document::reprojectAll(const std::vector<vec2>& newUVs) {
  reprojectLayers(newUVs);
  reprojectMeshMaps(newUVs);
  tex.releaseScratch();
}

void Document::reprojectLayers(const std::vector<vec2>& newUVs, const ReprojectSource& source) {
  for (auto& l : layers) {
    for (int c = 0; c < (int)l->channels.size(); ++c)
      if (l->channels[c] && l->channels[c]->valid())
        tex.reproject(gpu, newUVs, *l->channels[c], 0, source ? source(l->uid, c) : nullptr);
    if (l->mask) tex.reproject(gpu, newUVs, *l->mask, 0, source ? source(l->uid, -1) : nullptr);
  }
}

void Document::reprojectMeshMaps(const std::vector<vec2>& newUVs, const ReprojectSource& source) {
  auto src = [&](int which) { return source ? source(0, which) : nullptr; };
  if (tex.hasAO()) tex.reproject(gpu, newUVs, tex.aoMap, 0, src(0));
  if (tex.hasCurvature()) tex.reproject(gpu, newUVs, tex.curvMap, 0, src(1));
  if (tex.hasThickness()) tex.reproject(gpu, newUVs, tex.thickMap, 0, src(4));
  if (tex.hasBentNormals()) tex.reproject(gpu, newUVs, tex.bentMap, 0, src(5));
  // object-space, so moving and rotating islands keeps it valid
  if (tex.hasHighNormal()) tex.reproject(gpu, newUVs, tex.highNormal, 0, src(2));
  if (tex.materialIdsFromHigh) tex.reproject(gpu, newUVs, tex.matIdMap, 0, src(3));
}

namespace {
struct UVEditEntry : UndoEntry {
  std::vector<vec2> oldUVs, newUVs;
  std::vector<uint8_t> oldFlags, newFlags;
  std::vector<uint8_t> oldFaceFlags, newFaceFlags;
  struct Slot {
    uint32_t uid;
    int channel;
    std::unique_ptr<Texture2D> tex;
  };
  std::vector<Slot> slots;
  // mesh-derived maps of the texture set (AO, curvature, high-poly normal / IDs)
  struct MapSlot {
    int which;
    Texture2D tex;
  };
  std::vector<MapSlot> maps;
  static Texture2D& mapRef(TextureSet& t, int which) {
    switch (which) {
      case 0: return t.aoMap;
      case 1: return t.curvMap;
      case 2: return t.highNormal;
      case 4: return t.thickMap;
      case 5: return t.bentMap;
      default: return t.matIdMap;
    }
  }
  void swapTextures(Document& d) {
    for (auto& s : slots) {
      Layer* l = d.findLayer(s.uid);
      if (!l) continue;
      std::unique_ptr<Texture2D>& dst = s.channel < 0 ? l->mask : l->channels[s.channel];
      std::swap(dst, s.tex);
    }
    for (auto& m : maps) {
      Texture2D& dst = mapRef(d.tex, m.which);
      if (dst.width() == m.tex.width() && dst.height() == m.tex.height()) std::swap(dst, m.tex);
    }
  }
  void apply(Document& d, const std::vector<vec2>& uvs, const std::vector<uint8_t>& flags,
             const std::vector<uint8_t>& faceFlags) {
    swapTextures(d);
    d.mesh.loopUV = uvs;
    if (flags.size() == d.mesh.edgeFlags.size()) d.mesh.edgeFlags = flags;
    if (faceFlags.size() == d.mesh.faceCount()) d.mesh.faceFlags = faceFlags;
    d.mesh.computeUVTopology();
    d.mesh.computeTangents();
    d.gpu.updateUVs(d.mesh);
    d.gpu.updateEdges(d.mesh);
    d.tex.bakeMeshMaps(d.mesh, d.gpu);
    d.bridge.uvsEdited = true;
    d.updateSelectionFlags();
    d.markComposite();
  }
  void undo(Document& d) override { apply(d, oldUVs, oldFlags, oldFaceFlags); }
  void redo(Document& d) override { apply(d, newUVs, newFlags, newFaceFlags); }
  size_t bytes() const override {
    size_t b = 0;
    for (auto& s : slots)
      if (s.tex) b += size_t(s.tex->width()) * s.tex->height() * 4;
    return b;
  }
};
}  // namespace

bool Document::applyUVs(const std::vector<vec2>& requestedUVs, bool reprojectPaint, const std::string& label,
                        const std::vector<uint8_t>* newEdgeFlags, const std::vector<uint8_t>* newFaceFlags, bool movePinned) {
  if (requestedUVs.size() != mesh.loopUV.size()) return false;
  uint32_t F = mesh.faceCount();
  if (mesh.faceFlags.size() != F) mesh.faceFlags.assign(F, 0);
  std::vector<uint8_t> faceFlags = newFaceFlags && newFaceFlags->size() == F ? *newFaceFlags : mesh.faceFlags;
  std::vector<vec2> newUVs = requestedUVs;
  // pinned faces keep their UVs (unless this very edit unpins them)
  for (uint32_t f = 0; f < F && !movePinned; ++f)
    if ((mesh.faceFlags[f] & FacePinned) && (faceFlags[f] & FacePinned))
      for (uint32_t l = mesh.faceStart[f]; l < mesh.faceStart[f + 1]; ++l) newUVs[l] = mesh.loopUV[l];
  // mirrored copies stay on the faces they copy, whichever half was edited
  {
    std::swap(mesh.faceFlags, faceFlags);
    std::vector<uint8_t> upgraded = mesh.faceFlags;
    uv::MirrorLinks links = uv::mirrorLinks(mesh, bvh, &upgraded, &newUVs);
    std::swap(mesh.faceFlags, faceFlags);
    faceFlags = upgraded;
    for (uint32_t f : links.stale) faceFlags[f] &= ~kFaceMirrorBits;
    uv::syncMirrorCopies(mesh, links, mesh.loopUV, newUVs, faceFlags);
  }
  bool edgesChange = newEdgeFlags && newEdgeFlags->size() == mesh.edgeFlags.size() && *newEdgeFlags != mesh.edgeFlags;
  if (newUVs == mesh.loopUV && faceFlags == mesh.faceFlags && !edgesChange) return false;
  // texture tiles: where the new UVs land in the atlas
  auto atlasOf = [&](const std::vector<vec2>& uvs, TileLayout* out) {
    if (!mesh.tiles.active()) return uvs;
    TileLayout keepT = mesh.tiles;
    std::vector<vec2> keepA = mesh.loopAtlasUV;
    mesh.updateTiles(&uvs);
    std::vector<vec2> a = mesh.loopAtlasUV;
    if (out) *out = mesh.tiles;
    mesh.tiles = keepT;
    mesh.loopAtlasUV = keepA;
    return a;
  };
  {
    TileLayout nt;
    atlasOf(newUVs, &nt);
    if (nt.active() && nt.grid != mesh.tiles.grid) {  // UVs moved into a new UDIM tile: the atlas grows
      std::string why;
      if (!atlasFits(tileRes(), nt.grid, &why)) {
        PLOG_W("UV edit refused: %s", why.c_str());
        return false;
      }
      PLOG_I("UVs moved into a new UDIM tile: the atlas grows to %dx%d tiles (undo history cleared)", nt.grid, nt.grid);
      relayout(nt);
    }
  }
  auto e = std::make_unique<UVEditEntry>();
  e->label = label;
  e->oldUVs = mesh.loopUV;
  e->newUVs = newUVs;
  e->oldFlags = mesh.edgeFlags;
  e->newFlags = newEdgeFlags && newEdgeFlags->size() == mesh.edgeFlags.size() ? *newEdgeFlags : mesh.edgeFlags;
  e->oldFaceFlags = mesh.faceFlags;
  e->newFaceFlags = faceFlags;
  // mirror copies share the texels of the half they copy: only that half carries paint into the
  // new layout (drawing both would interleave their old paint triangle by triangle)
  std::vector<vec2> drawUVs = atlasOf(newUVs, nullptr);
  for (uint32_t f = 0; f < F; ++f)
    if (faceFlags[f] & FaceMirrorCopy)
      for (uint32_t l = mesh.faceStart[f]; l < mesh.faceStart[f + 1]; ++l) drawUVs[l] = vec2(-10.f);
  if (reprojectPaint) {
    // keep copies of the pre-edit textures for undo
    for (auto& l : layers) {
      for (int c = 0; c < kChannelCount; ++c)
        if (l->hasChannel(c)) {
          auto copy = std::make_unique<Texture2D>(l->channels[c]->width(), l->channels[c]->height(),
                                                  l->channels[c]->format(), 1);
          copy->copyFrom(*l->channels[c]);
          e->slots.push_back({l->uid, c, std::move(copy)});
        }
      if (l->mask) {
        auto copy = std::make_unique<Texture2D>(l->mask->width(), l->mask->height(), l->mask->format(), 1);
        copy->copyFrom(*l->mask);
        e->slots.push_back({l->uid, -1, std::move(copy)});
      }
    }
    reprojectLayers(drawUVs, [&](uint32_t uid, int ch) -> const Texture2D* {
      for (auto& sl : e->slots)
        if (sl.uid == uid && sl.channel == ch) return sl.tex.get();
      return nullptr;
    });
  }
  // maps that come from the mesh always follow it, and are restored on undo
  {
    auto keep = [&](int which, bool on) {
      if (!on) return;
      Texture2D& src = UVEditEntry::mapRef(tex, which);
      if (!src.valid()) return;
      UVEditEntry::MapSlot ms{which, Texture2D(src.width(), src.height(), src.format(), 1)};
      ms.tex.copyFrom(src);
      e->maps.push_back(std::move(ms));
    };
    keep(0, tex.hasAO());
    keep(1, tex.hasCurvature());
    keep(2, tex.hasHighNormal());
    keep(3, tex.materialIdsFromHigh);
    keep(4, tex.hasThickness());
    keep(5, tex.hasBentNormals());
    reprojectMeshMaps(drawUVs, [&](uint32_t, int which) -> const Texture2D* {
      for (auto& ms : e->maps)
        if (ms.which == which) return &ms.tex;
      return nullptr;
    });
    tex.releaseScratch();
  }
  mesh.loopUV = newUVs;
  mesh.faceFlags = faceFlags;
  if (e->newFlags != mesh.edgeFlags) {
    mesh.edgeFlags = e->newFlags;
    gpu.updateEdges(mesh);
  }
  mesh.computeUVTopology();
  mesh.computeTangents();
  gpu.updateUVs(mesh);
  tex.bakeMeshMaps(mesh, gpu);
  bridge.uvsEdited = true;
  updateSelectionFlags();
  markComposite();
  undo.push(std::move(e));
  return true;
}

void Document::pushSelectionUndo(const std::vector<uint8_t>& before, const std::string& label) {
  if (before.size() != faceSelection.size() || before == faceSelection) return;
  auto e = std::make_unique<LambdaEntry>();
  e->label = label;
  auto after = faceSelection;
  e->size = before.size() * 2;
  e->onUndo = [before](Document& d) {
    if (d.faceSelection.size() == before.size()) d.faceSelection = before, d.updateSelectionFlags();
  };
  e->onRedo = [after](Document& d) {
    if (d.faceSelection.size() == after.size()) d.faceSelection = after, d.updateSelectionFlags();
  };
  undo.push(std::move(e));
}

void Document::applyFaceFlags(const std::vector<uint8_t>& flags, const std::string& label) {
  if (flags.size() != mesh.faceCount() || flags == mesh.faceFlags) return;
  auto e = std::make_unique<LambdaEntry>();
  e->label = label;
  auto oldF = mesh.faceFlags;
  e->onUndo = [oldF](Document& d) { d.mesh.faceFlags = oldF, d.updateSelectionFlags(); };
  e->onRedo = [flags](Document& d) { d.mesh.faceFlags = flags, d.updateSelectionFlags(); };
  mesh.faceFlags = flags;
  updateSelectionFlags();  // (the pins show in the 3D view)
  dirty = true;
  undo.push(std::move(e));
}

const SymmetryReport& Document::symmetry() {
  const float planeX = symmetryPlane;
  uint64_t k = 1469598103934665603ull ^ (uint64_t(mesh.faceCount()) << 32) ^ mesh.positions.size();
  for (auto& p : mesh.positions) {
    uint32_t bits[3];
    std::memcpy(bits, &p, sizeof bits);
    k = (k ^ bits[0] ^ (uint64_t(bits[1]) << 21) ^ (uint64_t(bits[2]) << 42)) * 1099511628211ull;
  }
  uint32_t pb;
  float pk = std::isnan(planeX) ? -12345.f : planeX;
  std::memcpy(&pb, &pk, 4);
  k ^= uint64_t(pb) * 0x9E3779B97F4A7C15ull;
  if (k != symmetryKey_ || symmetry_.vertexDist.size() != mesh.positions.size()) {
    symmetryKey_ = k;
    symmetry_ = measureSymmetry(mesh, bvh, planeX);
  }
  return symmetry_;
}

const MeshCheckReport& Document::meshCheck() {
  // keyed on the counts, a sample of the positions and corners (asked every frame while the window is open) and
  // the UVs' hash (kept per frame)
  uint64_t k = meshShapeKey() ^ uv::uvHash(mesh) * 0x9E3779B97F4A7C15ull;
  if (k != meshCheckKey_ || meshCheck_.faceSeverity.size() != mesh.faceCount()) {
    meshCheckKey_ = k;
    meshCheck_ = checkMesh(mesh);
  }
  return meshCheck_;
}

uint64_t Document::meshShapeKey() const {
  uint64_t k = 1469598103934665603ull ^ (uint64_t(mesh.faceCount()) << 32) ^ mesh.loopCount() ^
               (uint64_t(mesh.positions.size()) << 17);
  size_t step = std::max<size_t>(1, mesh.positions.size() / 4096);
  for (size_t i = 0; i < mesh.positions.size(); i += step) {
    uint32_t bits[3];
    std::memcpy(bits, &mesh.positions[i], sizeof bits);
    k = (k ^ bits[0] ^ (uint64_t(bits[1]) << 21) ^ (uint64_t(bits[2]) << 42)) * 1099511628211ull;
  }
  step = std::max<size_t>(1, mesh.loopVert.size() / 4096);
  for (size_t i = 0; i < mesh.loopVert.size(); i += step) k = (k ^ mesh.loopVert[i]) * 1099511628211ull;
  return k ? k : 1;
}

bool Document::makeCollision(const CollisionOptions& opt, json* report, std::string* err) {
  if (!hasMesh()) {
    if (err) *err = "No model is open";
    return false;
  }
  collisionOptions = opt;
  collision_ = patina::makeCollision(mesh, opt);
  collisionKey_ = meshShapeKey();
  if (collision_.shapes.empty()) {
    if (err) *err = "No collision shapes could be made for this model";
    return false;
  }
  if (report) {
    json shapes = json::array();
    for (auto& sh : collision_.shapes)
      shapes.push_back({{"vertices", sh.verts.size()}, {"triangles", sh.tris.size()}, {"box", sh.box},
                        {"volume", sh.volume()}});
    *report = {{"mode", opt.mode},
               {"count", collision_.shapes.size()},
               {"shapes", shapes},
               {"model_volume", collision_.meshVolume},
               {"shapes_volume", collision_.shapesVolume},
               {"fit", collision_.meshVolume > 0 ? collision_.shapesVolume / collision_.meshVolume : 0.f}};
    if (!collision_.note.empty()) (*report)["note"] = collision_.note;
  }
  PLOG_I("Collision: %zu %s shape(s)", collision_.shapes.size(), opt.mode.c_str());
  return true;
}

const CollisionResult* Document::collisionShapes() {
  if (collision_.shapes.empty() || !collisionKey_) return nullptr;
  if (meshShapeKey() != collisionKey_) return nullptr;  // the mesh changed since: they no longer fit
  return &collision_;
}

bool Document::fixMesh(const MeshFixOptions& o, MeshFixResult* res, std::string* err) {
  if (!hasMesh()) {
    if (err) *err = "No mesh loaded";
    return false;
  }
  MeshFixResult r;
  Mesh fixed = patina::fixMesh(mesh, o, &r);
  if (res) *res = r;
  if (!r.changed()) return false;
  if (fixed.faceCount() == 0) {
    if (err) *err = "the fix would leave no faces";
    return false;
  }
  // a copy of the project first: the new mesh clears the undo history
  if (!keepCopyBeforeMeshChange(err)) return false;
  meshChangeLabel_ = "fix";
  if (beforeMeshChange) beforeMeshChange();  // (retopology builds wait)
  std::string keepName = mesh.name;
  fixed.name = keepName;
  setMesh(std::move(fixed), 0, true);
  dirty = true;
  markComposite();
  PLOG_I("Mended the mesh: %zu welded, %zu degenerate, %zu duplicate faces, %zu loose vertices, %zu small parts, "
         "%zu faces turned, %zu holes filled",
         r.welded, r.degenerate, r.duplicates, r.loose, r.smallParts, r.flipped, r.holesFilled);
  return true;
}

bool Document::keepCopyBeforeMeshChange(std::string* err) {
  fs::path backup = paths::ensureDir(paths::cacheDir() / "mesh_fix") / "before_fix.patina";
  auto job = prepareSave(backup.string(), err);
  if (!job || !job->ready(true) || !job->write(err)) {
    if (err && err->empty()) *err = "could not keep a copy of the project before changing the mesh";
    return false;
  }
  meshFixBackup_ = backup.string();
  return true;
}

void Document::replaceMeshKeepTextures(Mesh&& m) {
  if (mesh.tiles.active()) {
    m.tiles.mode = mesh.tiles.mode;
    m.tiles.tiles = mesh.tiles.tiles;
    m.tiles.grid = mesh.tiles.grid;
    m.computeUVTopology();
  }
  bool ao = tex.hasAO(), curv = tex.hasCurvature(), thick = tex.hasThickness();
  // fewer triangles cover the islands a little differently: fill the gutters so newly covered texels have paint
  for (auto& l : layers) {
    for (auto& c : l->channels)
      if (c && c->valid()) tex.dilateInPlace(*c);
    if (l->mask) tex.dilateInPlace(*l->mask);
  }
  Mesh oldMesh = std::move(mesh);
  Bvh oldBvh = std::move(bvh);
  std::vector<uint32_t> oldSets = faceSetOf;
  bvh = Bvh();
  mesh = std::move(m);
  gpu.upload(mesh);
  bvh.build(mesh);
  if (faceSetOf.size() != mesh.faceCount() || !faceSets.empty()) carryFaceSets(oldMesh, oldBvh, oldSets);  // (1.2)
  tex.bakeMeshMaps(mesh, gpu);
  tex.setBaked(false, false);
  if (ao) tex.bakeAO(mesh, gpu, aoSamples);
  if (curv) tex.bakeCurvature(mesh, gpu, &bvh);
  if (thick) tex.bakeThickness(mesh, bvh, thicknessRays);
  clearHistory();
  faceSelection.assign(mesh.faceCount(), 0);
  updateHiddenFaces();
  markComposite();
}

bool Document::reduceMesh(const ReduceOptions& o, ReduceResult* res, std::string* err) {
  if (!hasMesh()) {
    if (err) *err = "No mesh loaded";
    return false;
  }
  ReduceResult r;
  Mesh q = patina::reduceMesh(mesh, o, &r);
  if (res) *res = r;
  if (q.faceCount() == 0) {
    if (err) *err = "the reduction would leave no faces";
    return false;
  }
  if (r.trianglesAfter >= r.trianglesBefore) return false;
  if (!keepCopyBeforeMeshChange(err)) return false;
  meshChangeLabel_ = "reduction";
  if (beforeMeshChange) beforeMeshChange();
  q.name = mesh.name;
  replaceMeshKeepTextures(std::move(q));
  dirty = true;
  PLOG_I("Reduced the mesh from %zu to %zu triangles (the shape moved by %.3f%% of its size)", r.trianglesBefore,
         r.trianglesAfter, r.error * 100.f);
  return true;
}

bool Document::transformMesh(const TransformOptions& o, std::string* err) {
  if (!hasMesh()) {
    if (err) *err = "No mesh loaded";
    return false;
  }
  if (o.scale == 1.f && o.rotateDeg == vec3(0.f) && o.origin == "keep" && !o.ground) return false;
  if (!(o.scale > 0.f)) {
    if (err) *err = "the scale must be above 0";
    return false;
  }
  if (!keepCopyBeforeMeshChange(err)) return false;
  meshChangeLabel_ = "transform";
  if (beforeMeshChange) beforeMeshChange();
  Mesh q = mesh;
  patina::transformMesh(q, o);
  replaceMeshKeepTextures(std::move(q));
  dirty = true;
  return true;
}

bool Document::canRevertMeshFix() const {
  std::error_code ec;
  return !meshFixBackup_.empty() && fs::exists(meshFixBackup_, ec);
}

bool Document::revertMeshFix(MaterialLibrary& lib, std::string* err) {
  if (!canRevertMeshFix()) {
    if (err) *err = "there is no mended mesh to go back from";
    return false;
  }
  std::string keepPath = projectPath, keepMesh = meshPath;
  if (!loadProject(meshFixBackup_, lib, err)) return false;
  projectPath = keepPath, meshPath = keepMesh;
  dirty = true;
  std::error_code ec;
  fs::remove(meshFixBackup_, ec);
  meshFixBackup_.clear();
  PLOG_I("Back to the mesh before the fix");
  return true;
}

const uv::MirrorLinks& Document::mirrorLinks() {
  // keyed on the flags and the geometry (positions sampled)
  uint64_t k = 1469598103934665603ull ^ (uint64_t(mesh.faceCount()) << 32) ^ mesh.loopCount();
  for (uint8_t f : mesh.faceFlags) k = (k ^ f) * 1099511628211ull;
  size_t step = std::max<size_t>(1, mesh.positions.size() / 97);
  for (size_t i = 0; i < mesh.positions.size(); i += step) {
    uint32_t bits[3];
    std::memcpy(bits, &mesh.positions[i], sizeof bits);
    k = (k ^ bits[0] ^ (uint64_t(bits[1]) << 21) ^ (uint64_t(bits[2]) << 42)) * 1099511628211ull;
  }
  if (k != mirrorLinksKey_ || mirrorLinks_.primary.size() != mesh.faceCount()) {
    mirrorLinksKey_ = k;
    mirrorLinks_ = uv::mirrorLinks(mesh, bvh);
  }
  return mirrorLinks_;
}

void Document::applySeams(const std::vector<uint8_t>& flags, const std::string& label) {
  if (flags.size() != mesh.edgeFlags.size()) return;
  auto e = std::make_unique<LambdaEntry>();
  e->label = label;
  auto oldF = mesh.edgeFlags;
  e->onUndo = [oldF](Document& d) {
    d.mesh.edgeFlags = oldF;
    d.gpu.updateEdges(d.mesh);
    d.bridge.uvsEdited = true;
  };
  e->onRedo = [flags](Document& d) {
    d.mesh.edgeFlags = flags;
    d.gpu.updateEdges(d.mesh);
    d.bridge.uvsEdited = true;
  };
  mesh.edgeFlags = flags;
  gpu.updateEdges(mesh);
  bridge.uvsEdited = true;
  dirty = true;
  undo.push(std::move(e));
}

void Document::updateSelectionFlags() {
  if (!hasMesh()) return;
  if (faceSelection.size() != mesh.faceCount()) faceSelection.assign(mesh.faceCount(), 0);
  std::vector<uint32_t> flags(mesh.loopCount(), 0);
  bool lock = paintLockFaces.size() == mesh.faceCount();
  bool pins = mesh.faceFlags.size() == mesh.faceCount();
  for (uint32_t f = 0; f < mesh.faceCount(); ++f) {
    uint32_t bits = (faceSelection[f] ? 1u : 0u) | (lock && paintLockFaces[f] ? kPaintLockFlag : 0u) |
                    (pins && (mesh.faceFlags[f] & FacePinned) ? kPinnedFlag : 0u) |
                    (f < hiddenFaces.size() && hiddenFaces[f] ? kHiddenFlag : 0u);
    if (bits)
      for (uint32_t l = mesh.faceStart[f]; l < mesh.faceStart[f + 1]; ++l) flags[l] |= bits;
  }
  gpu.updateFlags(flags);
}

std::vector<uint32_t> Document::selectedFaces() const {
  std::vector<uint32_t> out;
  for (uint32_t f = 0; f < faceSelection.size(); ++f)
    if (faceSelection[f]) out.push_back(f);
  return out;
}

// ------------------------------------------------------------------ project files

// Reads a texture back for the project file in its own format (a conversion by the driver, such
// as half floats to 16 bit or RGBA to one channel, is slow) into a buffer the GPU fills without
// stopping this thread; rows bottom first as GL returns them. write() turns the samples into
// `channels` channels of `bits` bits.
static Document::SaveJob::Item readBack(const std::string& name, const Texture2D& t, int channels, int bits,
                                        bool signedFloat = false) {
  Document::SaveJob::Item it;
  it.name = name, it.w = t.width(), it.h = t.height(), it.channels = channels, it.bits = bits;
  it.signedFloat = signedFloat;
  switch (t.format()) {
    case GL_R8: it.srcChannels = 1, it.srcType = 0; break;
    case GL_RG8: it.srcChannels = 2, it.srcType = 0; break;
    case GL_RGBA8: it.srcChannels = 4, it.srcType = 0; break;
    case GL_R16F: it.srcChannels = 1, it.srcType = 1; break;
    case GL_RG16F: it.srcChannels = 2, it.srcType = 1; break;
    case GL_RGBA16F: it.srcChannels = 4, it.srcType = 1; break;
    default: it.srcChannels = channels, it.srcType = 2; break;  // anything else: floats, converted by GL
  }
  GLenum fmt[] = {0, GL_RED, GL_RG, GL_RGB, GL_RGBA};
  GLenum type[] = {GL_UNSIGNED_BYTE, GL_HALF_FLOAT, GL_FLOAT};
  size_t sampleBytes[] = {1, 2, 4};
  it.size = size_t(it.w) * it.h * it.srcChannels * sampleBytes[it.srcType];
  GLuint buf = 0;
  glCreateBuffers(1, &buf);
  glNamedBufferStorage(buf, (GLsizeiptr)it.size, nullptr,
                       GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT | GL_CLIENT_STORAGE_BIT);
  it.pbo = buf;
  GLint prev = 0;
  glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &prev);
  glBindBuffer(GL_PIXEL_PACK_BUFFER, buf);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glGetTextureImage(t.id(), 0, fmt[it.srcChannels], type[it.srcType], (GLsizei)it.size, nullptr);
  glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)prev);
  return it;
}

Document::SaveJob::~SaveJob() {
  for (auto& it : items)
    if (it.pbo) {
      if (it.mapped) glUnmapNamedBuffer(it.pbo);
      glDeleteBuffers(1, &it.pbo);
    }
  if (fence) glDeleteSync((GLsync)fence);
}

bool Document::SaveJob::ready(bool wait) {
  if (fence) {
    GLenum r = glClientWaitSync((GLsync)fence, GL_SYNC_FLUSH_COMMANDS_BIT, wait ? 120'000'000'000ull : 0);
    if (r == GL_TIMEOUT_EXPIRED || r == GL_WAIT_FAILED) return false;
    glDeleteSync((GLsync)fence);
    fence = nullptr;
    for (auto& it : items)
      if (it.pbo && !it.mapped)
        it.mapped = (const uint8_t*)glMapNamedBufferRange(it.pbo, 0, (GLsizeiptr)it.size,
                                                          GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);
  }
  return true;
}

// The samples of a read-back texture as the PNG stores them (`channels` x `bits`).
static std::vector<uint8_t> convertSamples(const Document::SaveJob::Item& it) {
  size_t n = size_t(it.w) * it.h;
  int sc = it.srcChannels, oc = it.channels;
  const uint8_t* src = it.mapped;
  if (it.srcType == 0 && it.bits == 8 && sc == oc) return std::vector<uint8_t>(src, src + it.size);
  std::vector<uint8_t> out(n * oc * (it.bits / 8));
  auto value = [&](size_t i) -> float {  // sample i of the source as a float
    if (it.srcType == 0) return src[i] / 255.f;
    if (it.srcType == 1) {
      uint16_t hbits;
      std::memcpy(&hbits, &src[i * 2], 2);
      return glm::unpackHalf1x16(hbits);
    }
    float f;
    std::memcpy(&f, &src[i * 4], 4);
    return f;
  };
  for (size_t p = 0; p < n; ++p)
    for (int c = 0; c < oc; ++c) {
      size_t si = p * sc + std::min(c, sc - 1), oi = p * oc + c;
      if (it.srcType == 0 && it.bits == 8) {
        out[oi] = src[si];
        continue;
      }
      float v = value(si);
      if (it.signedFloat) v = v * 0.5f + 0.5f;
      v = std::clamp(v, 0.f, 1.f);
      if (it.bits == 16) {
        uint16_t u = (uint16_t)std::lround(v * 65535.f);
        std::memcpy(&out[oi * 2], &u, 2);
      } else {
        out[oi] = (uint8_t)std::lround(v * 255.f);
      }
    }
  return out;
}

bool Document::SaveJob::write(std::string* err) {
  for (auto& it : items)
    if (it.bits && !it.mapped) {
      if (err) *err = "the textures were not read back";
      return false;
    }
  // compress the images in parallel (stb's single-threaded PNG writer took ~14 s for a 4K set)
  std::mutex failMutex;
  std::string failure;
  parallelFor(items.size(), [&](size_t i) {
    Item& it = items[i];
    if (!it.bits) return;
    // (1.2.1) an exception that leaves a worker thread ends the program: a save that fails says so instead
    try {
      if (it.channels < 1 || it.channels > 4) throw std::runtime_error("bad channel count " + std::to_string(it.channels));
      std::vector<uint8_t> samples = convertSamples(it);
      it.bytes = encodePngSamples(samples.data(), it.w, it.h, it.channels, it.bits, true);
      it.bits = 0;
    } catch (const std::exception& e) {
      std::lock_guard<std::mutex> lock(failMutex);
      if (failure.empty()) failure = it.name + ": " + e.what();
    }
  });
  if (!failure.empty()) {
    if (err) *err = "could not compress " + failure;
    return false;
  }
  std::vector<container::Block> blocks;
  blocks.reserve(items.size());
  for (auto& it : items) blocks.push_back(container::Block::raw(it.name, std::move(it.bytes)));
  return container::writeFile(path, "PATINA01", header, blocks, err);
}

bool Document::saveProject(const std::string& path, std::string* err) {
  auto job = prepareSave(path, err);
  if (!job || !job->ready(true) || !job->write(err)) {
    if (job && err && err->empty()) *err = "the textures could not be read back";
    return false;
  }
  projectPath = path;
  dirty = false;
  if (bridge.active) bridgeprojects::remember(bridge.object, path);  // (0.44) Blender sends it again: this project
  PLOG_I("Saved project %s", path.c_str());
  return true;
}

std::unique_ptr<Document::SaveJob> Document::prepareSave(const std::string& path, std::string* err) {
  if (!hasMesh()) {
    if (err) *err = "Nothing to save (no mesh loaded)";
    return nullptr;
  }
  auto job = std::make_unique<SaveJob>();
  job->path = path;
  job->header = appendToSave(*job, "");
  job->fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
  return job;
}

json Document::appendToSave(SaveJob& sj, const std::string& prefix) {
  SaveJob* job = &sj;
  auto readBack = [&](const std::string& name, const Texture2D& t, int channels, int bits, bool signedFloat = false) {
    return ::patina::readBack(prefix + name, t, channels, bits, signedFloat);
  };
  auto ready = [&](const std::string& name, std::vector<uint8_t> bytes) {
    SaveJob::Item it;
    it.name = prefix + name;
    it.bytes = std::move(bytes);
    job->items.push_back(std::move(it));
  };
  std::string meshBytes = encodePtMesh(mesh, false);
  ready("mesh", std::vector<uint8_t>(meshBytes.begin(), meshBytes.end()));
  json layerArr = json::array();
  // (1.2.1) the samples each channel's layer texture holds: color and coverage (RGBA8) or value and coverage (RG8,
  // RG16F). A table of the first six channels stood here: the four of 0.26 read past its end, so opacity, sheen and
  // subsurface were saved as images of no channels (lost when opened) and clearcoat as millions of them (a crash).
  auto layerSamples = [](int c) { return channelInfo((Channel)c).layerFormat == GL_RGBA8 ? 4 : 2; };
  for (auto& l : layers) {
    json lj = l->toJson();
    json stored = json::array();
    for (int c = 0; c < kChannelCount; ++c) {
      if (!l->hasChannel(c)) continue;
      std::string name = "L" + std::to_string(l->uid) + "_" + channelInfo((Channel)c).key;
      job->items.push_back(readBack(name, *l->channels[c], layerSamples(c), c == (int)Channel::Height ? 16 : 8));
      stored.push_back(channelInfo((Channel)c).key);
    }
    // (a baked map layer's fixes: the value and how much of it covers the bake)
    if (l->mask) job->items.push_back(readBack("L" + std::to_string(l->uid) + "_mask", *l->mask, l->isBaked() ? 2 : 1, 8));
    lj["stored_channels"] = stored;
    layerArr.push_back(lj);
  }
  if (tex.hasAO()) job->items.push_back(readBack("ao", tex.aoMap, 1, 8));
  if (tex.hasCurvature()) job->items.push_back(readBack("curvature", tex.curvMap, 1, 8));
  if (tex.hasThickness()) job->items.push_back(readBack("thickness", tex.thickMap, 1, 8));
  if (tex.hasBentNormals()) job->items.push_back(readBack("bent_normal", tex.bentMap, 3, 16, true));
  if (tex.hasHighNormal()) job->items.push_back(readBack("high_normal", tex.highNormal, 3, 16, true));
  if (tex.materialIdsFromHigh) job->items.push_back(readBack("material_ids", tex.matIdMap, 1, 8));
  json h = {{"app", "patina"},
            {"version", 1},
            {"resolution", tex.resolution()},
            {"selected", selected},
            {"next_uid", nextUid},
            {"layers", layerArr},
            {"baked_layers", 1},  // (0.41) baked maps have layers: a project without this gets them when opened
            {"mesh_path", meshPath},
            {"mesh_file_time", meshFileTime},
            {"baked_uv", {std::to_string(tex.bakedUV[0]), std::to_string(tex.bakedUV[1]), std::to_string(tex.bakedUV[2])}},
            {"export", hasLastExport ? json{{"preset", exportPresetName(lastExport.preset)}, {"directory", lastExport.directory},
                                            {"base_name", lastExport.baseName}, {"size", lastExport.size},
                                            {"name_template", lastExport.nameTemplate}, {"include_ao", lastExport.includeAO},
                                            {"include_thickness", lastExport.includeThickness},
                                            {"include_bent_normal", lastExport.includeBentNormal}}
                                     : json()},
            {"base", {{"color", {tex.base.color.x, tex.base.color.y, tex.base.color.z}},
                      {"roughness", tex.base.roughness},
                      {"metallic", tex.base.metallic},
                      {"height", tex.base.height},
                      {"opacity", tex.base.opacity},
                      {"sheen", tex.base.sheen},
                      {"coat", tex.base.coat},
                      {"subsurface", tex.base.subsurface}}},
            {"height_normal_strength", tex.heightNormalStrength},
            {"notes", [&] {
               json a = json::array();
               for (auto& n : notes) a.push_back(n.toJson());
               return a;
             }()},
            {"next_note_id", nextNoteId},
            {"high_poly", {{"path", highPolyPath}, {"baked", highPolyBaked}, {"distance", highPolyOptions.distance},
                           {"normal", highPolyOptions.normal}, {"ao", highPolyOptions.ao},
                           {"material_ids", highPolyOptions.materialIds}, {"ao_samples", highPolyOptions.aoSamples}}},
            {"bridge", {{"object", bridge.object}, {"port", bridge.blenderPort}, {"exchange_dir", bridge.exchangeDir},
                        {"active", bridge.active}, {"uvs_edited", bridge.uvsEdited}, {"slots_edited", bridge.slotsEdited}}},
            {"tiles", [&] {
               json t = {{"mode", mesh.tiles.mode}, {"grid", mesh.tiles.grid}, {"list", json::array()}};
               for (auto& ti : mesh.tiles.tiles) t["list"].push_back({{"slot", ti.slot}, {"udim", ti.udim}, {"name", ti.name}});
               return t;
             }()},
            {"object", objectJson()}};
  // (1.2) the face sets: the list in the header, which face is in which in a block
  if (!faceSets.empty() && faceSetOf.size() == mesh.faceCount()) {
    json fs = json::array();
    for (auto& s : faceSets)
      fs.push_back({{"id", s.id}, {"name", s.name}, {"color", {s.color.x, s.color.y, s.color.z}}, {"hidden", s.hidden}});
    h["face_sets"] = fs;
    h["next_face_set_id"] = nextFaceSetId;
    h["face_sets_hide_unset"] = hideUnsetFaces;
    ready("face_set_of", std::vector<uint8_t>((const uint8_t*)faceSetOf.data(), (const uint8_t*)(faceSetOf.data() + faceSetOf.size())));
  }
  return h;
}

// Texture restores of a project, decoded on several threads and uploaded on this one
// (decoding one after the other took most of the ~8 s to open a 4K texture set).
struct TextureLoads {
  struct Job {
    const container::Block* block;
    Texture2D* tex;
    bool signedFloat;                    // 16-bit 0..1 -> -1..1 normals (float texture, alpha 1)
    std::function<void(bool)> done;      // called after the upload (or failure), on this thread
    std::vector<uint8_t> samples;
    int w = 0, h = 0, bits = 0;
    bool ok = false;
  };
  std::vector<Job> jobs;
  void add(const container::Block* b, Texture2D& t, std::function<void(bool)> done = nullptr, bool signedFloat = false) {
    if (!b) {
      if (done) done(false);
      return;
    }
    jobs.push_back({b, &t, signedFloat, std::move(done)});
  }
  void run() {
    // in batches, so only a few decoded images are in memory at once
    const size_t batch = std::max(2u, std::min(16u, std::thread::hardware_concurrency()));
    for (size_t start = 0; start < jobs.size(); start += batch) {
      size_t end = std::min(jobs.size(), start + batch);
      parallelFor(end - start, [&](size_t k) {
        Job& j = jobs[start + k];
        int ch = j.signedFloat ? 4 : Texture2D::channelsOf(j.tex->format());
        j.ok = decodeSamples(j.block->bytes.data(), j.block->bytes.size(), ch, true, j.samples, j.w, j.h, j.bits) &&
               j.w == j.tex->width() && j.h == j.tex->height();
        if (j.ok && j.signedFloat) {  // stored 0..1, the texture holds -1..1 with alpha 1
          std::vector<uint8_t> f(size_t(j.w) * j.h * 4 * sizeof(float));
          float* o = (float*)f.data();
          for (size_t i = 0; i < size_t(j.w) * j.h * 4; ++i) {
            float v = j.bits == 16 ? ((const uint16_t*)j.samples.data())[i] / 65535.f : j.samples[i] / 255.f;
            o[i] = (i % 4) == 3 ? 1.f : v * 2.f - 1.f;
          }
          j.samples.swap(f);
        }
      });
      for (size_t i = start; i < end; ++i) {
        Job& j = jobs[i];
        if (j.ok) {
          int ch = j.signedFloat ? 4 : Texture2D::channelsOf(j.tex->format());
          GLenum fmt = ch == 1 ? GL_RED : ch == 2 ? GL_RG : ch == 3 ? GL_RGB : GL_RGBA;
          j.tex->uploadRaw(fmt, j.signedFloat ? GL_FLOAT : j.bits == 16 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE,
                           j.samples.data());
        }
        std::vector<uint8_t>().swap(j.samples);
        if (j.done) j.done(j.ok);
      }
    }
    jobs.clear();
  }
};

bool Document::loadProject(const std::string& path, MaterialLibrary& lib, std::string* err) {
  if (beforeMeshChange) beforeMeshChange();
  container::File f;
  if (!container::readFile(path, "PATINA01", f, err)) return false;
  return loadFrom(f, f.header, "", path, lib, err);
}

bool Document::loadFrom(const container::File& f, const json& h, const std::string& prefix, const std::string& path,
                        MaterialLibrary& lib, std::string* err) {
  auto block = [&](const std::string& name) { return f.get(prefix + name); };
  const container::Block* mb = block("mesh");
  if (!mb) {
    if (err) *err = "project has no mesh";
    return false;
  }
  container::File mf;
  if (!container::decode(std::string(mb->bytes.begin(), mb->bytes.end()), "PTMESH01", mf, err)) return false;
  Mesh m;
  if (!decodePtMesh(mf, m, err)) return false;
  m.finalize();
  int res = h.value("resolution", 2048);
  if (h.contains("tiles") && h["tiles"].is_object() && h["tiles"].value("mode", 0) != 0) {
    // the saved tile order and grid: the layer textures are atlases laid out by them
    auto& tj = h["tiles"];
    m.tiles.mode = (uint8_t)tj.value("mode", 0);
    m.tiles.grid = std::clamp(tj.value("grid", 1), 1, 8);
    for (auto& t : tj.value("list", json::array()))
      m.tiles.tiles.push_back({t.value("slot", 0u), t.value("udim", 1001), t.value("name", std::string())});
    m.computeUVTopology();
    if (m.tiles.grid != tj.value("grid", 1)) PLOG_W("Project tiles: the atlas grid changed while loading");
  }
  layers.clear();
  soloUid = 0;
  clearBakeDown();
  // (1.2.3) a project is not the sphere Patina starts with: before, opening one kept that mark, and a model added to a
  // one-object project then took its object's place (a model dropped on any project opened instead of being added)
  startupScene = false;
  mesh = std::move(m);
  gpu.upload(mesh);
  bvh.build(mesh);
  tex.allocate(res);
  tex.bakeMeshMaps(mesh, gpu);
  clearHistory();
  if (h.contains("base")) {
    auto& b = h["base"];
    if (b.contains("color")) tex.base.color = {b["color"][0], b["color"][1], b["color"][2]};
    tex.base.roughness = b.value("roughness", tex.base.roughness);
    tex.base.metallic = b.value("metallic", tex.base.metallic);
    tex.base.height = b.value("height", tex.base.height);
    tex.base.opacity = b.value("opacity", 1.f);
    tex.base.sheen = b.value("sheen", 0.f);
    tex.base.coat = b.value("coat", 0.f);
    tex.base.subsurface = b.value("subsurface", 0.f);
  }
  tex.heightNormalStrength = h.value("height_normal_strength", tex.heightNormalStrength);
  notes.clear();
  nextNoteId = 1;
  for (auto& nj : h.value("notes", json::array())) {
    notes.push_back(Note::fromJson(nj));
    nextNoteId = std::max(nextNoteId, notes.back().id + 1);
  }
  nextNoteId = std::max(nextNoteId, h.value("next_note_id", 1u));
  TextureLoads loads;
  for (auto& lj : h.value("layers", json::array())) {
    auto l = std::make_unique<Layer>();
    l->fromJson(lj);
    for (auto& key : lj.value("stored_channels", json::array())) {
      int c = channelFromKey(key);
      if (c < 0) continue;
      Texture2D& t = l->ensureChannel(c, tex.size());
      loads.add(block("L" + std::to_string(l->uid) + "_" + key.get<std::string>()), t,
                [name = l->name, key = key.get<std::string>()](bool ok) {
                  if (!ok) PLOG_W("Layer %s: could not restore channel %s", name.c_str(), key.c_str());
                });
    }
    if (lj.value("has_mask", false)) loads.add(block("L" + std::to_string(l->uid) + "_mask"), l->ensureMask(tex.size()));
    if (l->type == LayerType::Fill && !l->material.id.empty() && !lib.find(l->material.id))
      PLOG_W("Layer %s uses missing material '%s'", l->name.c_str(), l->material.id.c_str());
    layers.push_back(std::move(l));
  }
  // restore baked maps (stored at project resolution)
  highPoly.reset();
  highPolyPath.clear();
  highPolyBaked = false;
  loads.add(block("ao"), tex.aoMap, [this](bool ok) { if (ok) tex.setBaked(true, tex.hasCurvature()); });
  loads.add(block("curvature"), tex.curvMap, [this](bool ok) { if (ok) tex.setBaked(tex.hasAO(), true); });
  loads.add(block("thickness"), tex.thickMap, [this](bool ok) { if (ok) tex.setThicknessBaked(true); });
  // bent normals go with the AO: restored after it
  loads.add(block("bent_normal"), tex.bentMap, [this](bool ok) { if (ok) bentLoaded_ = true; }, true);
  loads.add(block("high_normal"), tex.highNormal, [this](bool ok) { if (ok) tex.setHighNormal(true); }, true);
  loads.add(block("material_ids"), tex.matIdMap, [this](bool ok) { if (ok) tex.materialIdsFromHigh = true; });
  bentLoaded_ = false;
  loads.run();
  if (bentLoaded_) tex.setBentBaked(tex.hasAO());
  if (h.contains("high_poly") && h["high_poly"].is_object()) {
    auto& hp = h["high_poly"];
    highPolyPath = hp.value("path", std::string());
    highPolyBaked = hp.value("baked", false) && tex.hasHighNormal();
    highPolyOptions.distance = hp.value("distance", -1.f);
    highPolyOptions.normal = hp.value("normal", true);
    highPolyOptions.ao = hp.value("ao", true);
    highPolyOptions.materialIds = hp.value("material_ids", true);
    highPolyOptions.aoSamples = hp.value("ao_samples", 64);
  }
  hasLastExport = false;
  if (h.contains("export") && h["export"].is_object()) {
    auto& ex = h["export"];
    lastExport.preset = exportPresetFromName(ex.value("preset", std::string("blender")));
    lastExport.directory = ex.value("directory", std::string());
    lastExport.baseName = ex.value("base_name", std::string());
    lastExport.size = ex.value("size", 0);
    lastExport.nameTemplate = ex.value("name_template", std::string("$mesh_$map"));
    lastExport.includeAO = ex.value("include_ao", true);
    lastExport.includeThickness = ex.value("include_thickness", true);
    lastExport.includeBentNormal = ex.value("include_bent_normal", true);
    hasLastExport = !lastExport.directory.empty();
  }
  nextUid = h.value("next_uid", (uint32_t)layers.size() + 1);
  // (0.44.2) the UVs the bakes were made for (older projects: taken to be these)
  for (int k = 0; k < kBakedMapCount; ++k) {
    tex.bakedUV[k] = uv::uvHash(mesh);
    if (h.contains("baked_uv") && h["baked_uv"].is_array() && k < (int)h["baked_uv"].size() && h["baked_uv"][k].is_string()) {
      try {
        tex.bakedUV[k] = std::stoull(h["baked_uv"][k].get<std::string>());
      } catch (std::exception&) {
      }
    }
  }
  for (auto& l : layers) nextUid = std::max(nextUid, l->uid + 1);
  selected = std::clamp(h.value("selected", 0), -1, (int)layers.size() - 1);
  // (0.41) the baked maps of a project from before baked map layers get theirs now (below the stack: the selection
  // moves up with it); a newer project keeps what it has (a layer deleted stays deleted until the map is baked again)
  syncedBakeSerial_ = tex.bakeSerial();
  if (!h.contains("baked_layers")) syncBakedLayers();
  meshPath = h.value("mesh_path", std::string());
  // the time the mesh file had when this project last saw it: a newer file prompts an update
  meshFileTime = h.value("mesh_file_time", (int64_t)0);
  if (!meshFileTime && !meshPath.empty()) meshFileTime = fileTime(meshPath);
  if (h.contains("bridge")) {
    auto& b = h["bridge"];
    bridge.object = b.value("object", std::string());
    bridge.blenderPort = b.value("port", 7861);
    bridge.exchangeDir = b.value("exchange_dir", std::string());
    bridge.active = !bridge.object.empty();
    bridge.uvsEdited = b.value("uvs_edited", false);
    bridge.slotsEdited = b.value("slots_edited", false);
  }
  objectFromJson(h.value("object", json::object()));
  // (1.2) the face sets
  faceSets.clear(), faceSetOf.clear(), hiddenFaces.clear(), nextFaceSetId = 1, hideUnsetFaces = false;
  if (const container::Block* fb = block("face_set_of"); fb && fb->bytes.size() == size_t(mesh.faceCount()) * 4) {
    faceSetOf.resize(mesh.faceCount());
    std::memcpy(faceSetOf.data(), fb->bytes.data(), fb->bytes.size());
    for (auto& sj : h.value("face_sets", json::array())) {
      FaceSet s;
      s.id = sj.value("id", 0u);
      s.name = sj.value("name", std::string());
      if (sj.contains("color") && sj["color"].is_array() && sj["color"].size() == 3)
        s.color = vec3(sj["color"][0].get<float>(), sj["color"][1].get<float>(), sj["color"][2].get<float>());
      s.hidden = sj.value("hidden", false);
      if (s.id) faceSets.push_back(s), nextFaceSetId = std::max(nextFaceSetId, s.id + 1);
    }
    nextFaceSetId = std::max(nextFaceSetId, h.value("next_face_set_id", 1u));
    hideUnsetFaces = h.value("face_sets_hide_unset", false);
  }
  projectPath = path;
  faceSelection.assign(mesh.faceCount(), 0);
  updateHiddenFaces();
  markComposite();
  dirty = false;
  if (bridge.active) bridgeprojects::remember(bridge.object, path);
  PLOG_I("Opened project %s (%zu layers)", path.c_str(), layers.size());
  return true;
}

// ------------------------------------------------------------------ export

std::string expandExportName(const std::string& tmpl, const std::map<std::string, std::string>& vars) {
  std::string out;
  for (size_t i = 0; i < tmpl.size();) {
    if (tmpl[i] == '$') {
      size_t j = i + 1;
      while (j < tmpl.size() && (std::isalnum((unsigned char)tmpl[j]) || tmpl[j] == '_') && j - i <= 8) ++j;
      // longest known token wins, so "$mesh_$map" reads $mesh then "_"
      bool hit = false;
      for (size_t k = j; k > i + 1; --k) {
        auto it = vars.find(tmpl.substr(i + 1, k - i - 1));
        if (it != vars.end()) {
          out += it->second;
          i = k;
          hit = true;
          break;
        }
      }
      if (hit) continue;
    }
    out += tmpl[i++];
  }
  return out;
}

// The file a map of an export goes to: the name template with $mesh, $project, $map, $size, $preset, $layer, and for
// texture tiles $set, $udim, $tile (templates without them get the set before the map name and the UDIM number before
// the extension: Mesh_Body_basecolor.png, Mesh_basecolor.1001.png). `tile` -1: no tiles.
fs::path Document::exportFileName(const ExportOptions& opt, const std::string& map, int tile) {
  fs::path dir = opt.directory.empty() ? fs::current_path() : fs::path(opt.directory);
  std::string tmpl = opt.nameTemplate.empty() ? "$mesh_$map" : opt.nameTemplate;
  const Layer* only = opt.layerUid ? findLayer(opt.layerUid) : nullptr;
  std::map<std::string, std::string> vars = {
      {"mesh", paths::sanitizeFileName(opt.baseName.empty() ? mesh.name : opt.baseName)},
      {"project", projectPath.empty() ? std::string("untitled") : fs::path(projectPath).stem().string()},
      {"size", std::to_string(opt.size > 0 ? opt.size : tileRes())},
      {"preset", exportPresetName(opt.preset)},
      {"layer", only ? paths::sanitizeFileName(only->name) : std::string("all")}};
  const TileLayout& TL = mesh.tiles;
  std::string m = map;
  std::string udimSuffix;
  if (tile >= 0 && TL.active()) {
    const TileInfo& ti = TL.tiles[tile];
    vars["set"] = paths::sanitizeFileName(mesh.slotName(ti.slot));
    vars["udim"] = std::to_string(ti.udim);
    vars["tile"] = paths::sanitizeFileName(ti.name);
    if ((TL.mode & TileBySlot) && tmpl.find("$set") == std::string::npos && tmpl.find("$tile") == std::string::npos)
      m = vars["set"] + "_" + m;
    if ((TL.mode & TileByUdim) && tmpl.find("$udim") == std::string::npos && tmpl.find("$tile") == std::string::npos)
      udimSuffix = "." + vars["udim"];
  }
  vars["map"] = m;
  std::string n = paths::sanitizeFileName(expandExportName(tmpl, vars));
  return dir / ((n.empty() ? m : n) + udimSuffix + ".png");
}

// The maps an export of the whole stack writes for its preset, with the extra channels the texture set uses and the
// baked maps it is asked for
std::vector<ExportMap> Document::exportMaps(const ExportOptions& opt) {
  std::vector<ExportMap> outs;
  bool ao = opt.includeAO && tex.hasAO() && tex.bakedShown(0);  // (0.44.2) a map switched off stays out
  bool thick = opt.includeThickness && tex.hasThickness() && tex.bakedShown(2);
  bool bent = opt.includeBentNormal && tex.hasBentNormals();
  int hb = opt.height16 ? 16 : 8;
  switch (opt.preset) {
    case ExportPreset::Blender:
      outs = {{"basecolor", "basecolor", 0, 3, 8}, {"roughness", "roughness", 1, 1, 8}, {"metallic", "metallic", 2, 1, 8},
              {"normal", "normal", 4, 3, 8},       {"height", "height", 3, 1, hb},       {"emission", "emission", 6, 3, 8}};
      if (ao) outs.push_back({"ao", "ao", 7, 1, 8});
      break;
    case ExportPreset::GLTF:
      outs = {{"basecolor", "baseColor", 0, 3, 8}, {"metallicRoughness", "metallicRoughness", 9, 3, 8},
              {"normal", "normal", 4, 3, 8},       {"emissive", "emissive", 6, 3, 8}};
      if (ao) outs.push_back({"occlusion", "occlusion", 7, 1, 8});
      break;
    case ExportPreset::Unreal:
      outs = {{"basecolor", "BaseColor", 0, 3, 8}, {"orm", "OcclusionRoughnessMetallic", 8, 3, 8},
              {"normal", "Normal", 5, 3, 8},       {"height", "Height", 3, 1, hb}, {"emissive", "Emissive", 6, 3, 8}};
      break;
    case ExportPreset::Unity:
      outs = {{"basecolor", "BaseMap", 0, 3, 8}, {"mask", "MaskMap", 10, 4, 8}, {"normal", "Normal", 4, 3, 8},
              {"height", "Height", 3, 1, hb},    {"emission", "Emission", 6, 3, 8}};
      break;
    default: break;
  }
  {  // 0.26: opacity, sheen, clearcoat and subsurface, when the texture set uses them (or asked for)
    int used = opt.extraMaps == 1 ? kExtraChannels : opt.extraMaps == 2 ? 0 : tex.extraUsed;
    bool gl = opt.preset == ExportPreset::GLTF, ue = opt.preset == ExportPreset::Unreal, un = opt.preset == ExportPreset::Unity;
    if (used & 64) {
      if (gl || un) {  // glTF and Unity keep opacity in the base color's alpha
        for (auto& o : outs)
          if (o.key == "basecolor") o.mode = 19, o.channels = 4;
      } else {
        outs.push_back({"opacity", ue ? "Opacity" : "opacity", 15, 1, 8});
      }
    }
    if (used & 128) outs.push_back({"sheen", gl ? "sheenColor" : ue || un ? "Sheen" : "sheen", 16, 1, 8});
    if (used & 256) outs.push_back({"coat", gl ? "clearcoat" : ue ? "ClearCoat" : un ? "CoatMask" : "coat", 17, 1, 8});
    if (used & 512) outs.push_back({"subsurface", gl ? "subsurface" : ue ? "Subsurface" : un ? "SubsurfaceMask" : "subsurface", 18, 1, 8});
  }
  {  // baked extras, named the way each engine's documentation calls them
    bool dx = opt.preset == ExportPreset::Unreal;
    const char* tn = opt.preset == ExportPreset::Unity ? "Thickness" : opt.preset == ExportPreset::Unreal ? "Thickness" : "thickness";
    const char* bn = opt.preset == ExportPreset::Unity ? "BentNormal" : opt.preset == ExportPreset::Unreal ? "BentNormal" : "bentNormal";
    if (thick) outs.push_back({"thickness", tn, 12, 1, 8});
    if (bent) outs.push_back({"bent_normal", bn, dx ? 14 : 13, 3, 8});
  }
  return outs;
}

// (0.47) What an export writes, without writing anything: each file, what its channels hold, its bits and color space
// (the Export Textures window and the Guided layout's Export step list it; export_textures with dry_run)
std::vector<ExportPlanFile> Document::exportPlan(const ExportOptions& opt) {
  std::vector<ExportPlanFile> plan;
  if (!hasMesh()) return plan;
  const TileLayout& TL = mesh.tiles;
  const int nTiles = TL.active() ? TL.count() : 1;
  const int size = opt.size > 0 ? opt.size : tileRes();
  Layer* only = opt.layerUid ? findLayer(opt.layerUid) : nullptr;
  auto add = [&](const std::string& key, const std::string& map, int mode, int channels, int bits, bool psd) {
    for (int t = 0; t < (only && (opt.maskOnly || only->isBaked()) ? 1 : nTiles); ++t) {
      ExportPlanFile f;
      f.key = TL.active() && !(only && (opt.maskOnly || only->isBaked())) ? key + "@" + std::to_string(t) : key;
      fs::path p = exportFileName(opt, map, TL.active() ? t : -1);
      if (psd) p.replace_extension(".psd");
      f.file = p.string();
      f.channels = channels, f.bits = bits, f.size = size;
      f.srgb = mode == 0 || mode == 6 || mode == 19;
      static const char* kWhat[] = {"base color", "roughness", "metallic", "height", "normal, OpenGL (Y up)",
                                    "normal, DirectX (Y down)", "emission", "ambient occlusion",
                                    "R occlusion, G roughness, B metallic", "G roughness, B metallic (R is 1)",
                                    "R metallic, G occlusion, A smoothness", "smoothness", "thickness",
                                    "bent normal, OpenGL (Y up)", "bent normal, DirectX (Y down)", "opacity", "sheen",
                                    "clearcoat", "subsurface", "RGB base color, A opacity"};
      f.contents = mode >= 0 && mode < 20 ? kWhat[mode] : key;
      if (mode == -1) f.contents = "the layer's painted mask";
      if (psd) f.contents += ", a layer per Patina layer";
      plan.push_back(f);
    }
  };
  if (only && only->isBaked()) {
    std::string key = bakedMapKey(only->bakedMap);
    add(key, key, only->bakedMap == 0 ? 7 : only->bakedMap == 2 ? 12 : 1, 1, 8, false);
    if (only->bakedMap == 1) plan.back().contents = "curvature";
    return plan;
  }
  if (opt.maskOnly) {
    add("mask", "mask", -1, 1, 8, false);
    return plan;
  }
  if (opt.psd) {
    bool dx = opt.preset == ExportPreset::Unreal;
    add("basecolor", "basecolor", 0, 4, 8, true), add("roughness", "roughness", 1, 1, 8, true);
    add("metallic", "metallic", 2, 1, 8, true), add("height", "height", 3, 1, opt.height16 ? 16 : 8, true);
    add("normal", "normal", dx ? 5 : 4, 3, 8, true), add("emission", "emission", 6, 3, 8, true);
    int used = opt.extraMaps == 1 ? kExtraChannels : opt.extraMaps == 2 ? 0 : tex.extraUsed;
    static const char* keys[] = {"opacity", "sheen", "coat", "subsurface"};
    for (int k = 0; k < 4; ++k)
      if (used & (64 << k)) add(keys[k], keys[k], 15 + k, 1, 8, true);
    return plan;
  }
  for (const ExportMap& m : exportMaps(opt)) add(m.key, m.suffix, m.mode, m.channels, m.bits, false);
  return plan;
}

bool Document::exportTextures(const ExportOptions& opt, std::map<std::string, std::string>* outFiles, std::string* err) {
  progress::Scope prog("Exporting textures");
  if (!hasMesh()) {
    if (err) *err = "No mesh loaded";
    return false;
  }
  fs::path dir = opt.directory.empty() ? fs::current_path() : fs::path(opt.directory);
  paths::ensureDir(dir);
  {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
      if (err) *err = "Could not create the folder " + dir.string();
      return false;
    }
  }
  // solo is a viewing aid: export the full stack (or just opt.layerUid), then restore the view
  struct SoloScope {
    Document& d;
    uint32_t keep;
    bool changed;
    SoloScope(Document& doc, uint32_t want) : d(doc), keep(doc.soloUid), changed(want != doc.soloUid && doc.library) {
      if (changed) {
        d.soloUid = want;
        d.compositeDirty = true;
        d.updateComposite(*d.library);
      }
    }
    ~SoloScope() {
      if (changed) {
        d.soloUid = keep;
        d.compositeDirty = true;
      }
    }
  };
  Layer* only = opt.layerUid ? findLayer(opt.layerUid) : nullptr;
  if (opt.layerUid && !only) {
    if (err) *err = "The layer to export does not exist";
    return false;
  }
  if (opt.maskOnly && (!only || !only->mask)) {
    if (err) *err = only ? "Layer '" + only->name + "' has no painted mask" : "Pick a layer whose mask to export";
    return false;
  }
  SoloScope soloScope(*this, opt.layerUid);
  const TileLayout& TL = mesh.tiles;
  int curTile = -1;
  auto fileFor = [&](const std::string& map) { return exportFileName(opt, map, curTile); };
  // The maps are rendered and read back one by one; compressing and writing the PNG files then
  // happens on several threads at once (pending), which made exports and sends to Blender several
  // times faster.
  struct PendingPng {
    fs::path file;
    std::vector<uint8_t> samples;  // 8 or 16 bit (host order), rows top first
    int w, h, channels, bits;
  };
  std::vector<PendingPng> pending;
  auto writePng = [&](const fs::path& file, Image& img, int bits) {
    if (opt.size > 0 && opt.size != img.width) {
      Image r;
      r.resize(opt.size, opt.size, img.channels);
      stbir_resize_float_linear(img.pixels.data(), img.width, img.height, 0, r.pixels.data(), opt.size, opt.size, 0,
                                (stbir_pixel_layout)img.channels);
      img = std::move(r);
    }
    PendingPng p{file, {}, img.width, img.height, img.channels, bits == 16 ? 16 : 8};
    size_t n = img.pixels.size();
    p.samples.resize(n * (p.bits / 8));
    for (size_t i = 0; i < n; ++i) {
      float v = std::clamp(img.pixels[i], 0.f, 1.f);
      if (p.bits == 16) {
        uint16_t u = (uint16_t)std::lround(v * 65535.f);
        std::memcpy(&p.samples[i * 2], &u, 2);
      } else {
        p.samples[i] = (uint8_t)std::lround(v * 255.f);
      }
    }
    pending.push_back(std::move(p));
    return true;
  };
  auto flushPngs = [&]() {
    std::vector<std::string> errors(pending.size());
    parallelFor(pending.size(), [&](size_t i) {
      PendingPng& p = pending[i];
      std::vector<uint8_t> png = encodePngSamples(p.samples.data(), p.w, p.h, p.channels, p.bits, false);
      std::vector<uint8_t>().swap(p.samples);
      fs::path tmp = p.file;
      tmp += ".tmp.png";
      {
        std::ofstream f(tmp, std::ios::binary);
        f.write((const char*)png.data(), (std::streamsize)png.size());
        if (!f.good()) {
          errors[i] = "Could not write " + p.file.string() + " (is the folder writable?)";
          return;
        }
      }
      std::error_code ec;
      fs::rename(tmp, p.file, ec);  // atomic replace so watchers never see half-written files
      if (ec) errors[i] = "Could not replace " + p.file.string() + ": " + ec.message();
    });
    pending.clear();
    for (auto& e : errors)
      if (!e.empty()) {
        if (err) *err = e;
        return false;
      }
    return true;
  };
  if (only && only->isBaked()) {
    // (0.41) a baked map layer: its map as everything reads it (the bake with the fixes and adjustments)
    tex.applyBakedLayers(layers, mesh.bounds);
    Image img = tex.bakedView(only->bakedMap).download(1);
    std::string key = bakedMapKey(only->bakedMap);
    fs::path file = fileFor(key);
    if (!writePng(file, img, 8) || !flushPngs()) return false;
    if (outFiles) (*outFiles)[key] = fs::absolute(file).string();
    PLOG_I("Exported the %s map to %s", key.c_str(), file.string().c_str());
    return true;
  }
  if (opt.maskOnly) {
    Image img = only->mask->download(1);
    fs::path file = fileFor("mask");
    if (!writePng(file, img, 8) || !flushPngs()) return false;
    if (outFiles) (*outFiles)["mask"] = fs::absolute(file).string();
    PLOG_I("Exported the mask of %s to %s", only->name.c_str(), file.string().c_str());
    return true;
  }
  std::vector<ExportMap> outs = exportMaps(opt);
  if (opt.psd && library) {
    // texture sets / UDIM tiles: a set of PSD files per tile, named like the PNG maps
    auto psdFor = [&](const std::string& map, int tile) {
      curTile = TL.active() ? tile : -1;
      fs::path p = fileFor(map);
      curTile = -1;
      p.replace_extension(".psd");
      return p;
    };
    return exportPsd(opt, psdFor, outFiles, err);
  }
  const int nTiles = TL.active() ? TL.count() : 1;
  auto keyFor = [&](const std::string& key) { return curTile >= 0 ? key + "@" + std::to_string(curTile) : key; };
  if (opt.size > tileRes() && library) {
    if (!TL.active()) return exportTiled(opt, outs, fileFor, outFiles, err);
    for (int t = 0; t < nTiles; ++t) {
      curTile = t;
      std::map<std::string, std::string> part;
      if (!exportTiled(opt, outs, fileFor, &part, err, t)) return false;
      if (outFiles)
        for (auto& [k, v] : part) (*outFiles)[keyFor(k)] = v;
    }
    return true;
  }
  int R = tex.resolution();
  Texture2D target(R, R, GL_RGBA16F, 1);
  Framebuffer fbo;
  fbo.attach(0, target);
  fbo.setDrawBuffers(1);
  Shader& s = ShaderCache::get("fullscreen.vs.glsl", "post.fs.glsl", {"POST_EXPORT"});
  for (auto& o : outs) {
    {
      gl::FramebufferScope fs(fbo.id());
      gl::ViewportScope vp(0, 0, R, R);
      glDisable(GL_BLEND);
      glDisable(GL_DEPTH_TEST);
      s.tex("uColor", 0, tex.finColor.id());
      s.tex("uRMH", 1, tex.finRMH.id());
      s.tex("uNormal", 2, tex.finNormal.id());
      s.tex("uEmissive", 3, tex.finEmissive.id());
      s.tex("uThick", 4, tex.thickView().id());
      s.tex("uBent", 5, tex.bentMap.id());
      s.tex("uNrmMap", 6, tex.nrmMap.id());
      s.tex("uTanMap", 7, tex.tanMap.id());
      s.tex("uExtra", 8, tex.finExtra.id());
      s.set("uOut", o.mode);
      s.use();
      drawFullscreenTriangle();
    }
    Image img = target.download(o.channels);  // row 0 = top
    if (!TL.active()) {
      fs::path file = fileFor(o.suffix);
      if (!writePng(file, img, o.bits)) return false;
      if (outFiles) (*outFiles)[o.key] = fs::absolute(file).string();
      continue;
    }
    const int T = tileRes();
    for (int t = 0; t < nTiles; ++t) {
      curTile = t;
      int cx = t % TL.grid, cy = t / TL.grid;
      int x0 = cx * T, y0 = (TL.grid - 1 - cy) * T;  // image rows go down, atlas cells go up
      Image cell;
      cell.resize(T, T, img.channels);
      for (int y = 0; y < T; ++y)
        std::memcpy(&cell.pixels[size_t(y) * T * img.channels], &img.pixels[(size_t(y0 + y) * img.width + x0) * img.channels],
                    size_t(T) * img.channels * sizeof(float));
      fs::path file = fileFor(o.suffix);
      if (!writePng(file, cell, o.bits)) return false;
      if (outFiles) (*outFiles)[keyFor(o.key)] = fs::absolute(file).string();
    }
    curTile = -1;
  }
  if (!flushPngs()) return false;
  PLOG_I("Exported %zu textures (%s) to %s", outs.size(), exportPresetName(opt.preset), dir.string().c_str());
  if (!opt.layerUid && !opt.maskOnly && opt.remember) {
    lastExport = opt;
    lastExport.directory = dir.string();
    hasLastExport = true;
  }
  return true;
}

// ------------------------------------------------------------------ tiled export
// Exports larger than the texture set are rendered in tiles: each tile gets its own mesh maps
// at full detail, so procedural fill materials, smart masks and noise are evaluated at the
// export size; painted layers and baked maps are resampled from the texture set. The PNG files
// are written band by band, so even 32K maps never sit in memory whole.
bool Document::exportTiled(const ExportOptions& opt, const std::vector<ExportMap>& outs,
                           const std::function<fs::path(const std::string&)>& fileFor,
                           std::map<std::string, std::string>* outFiles, std::string* err, int atlasTile) {
  // texture tiles: the export covers one cell of the atlas
  const bool cellOnly = atlasTile >= 0 && mesh.tiles.active();
  const vec2 cell0 = cellOnly ? mesh.tiles.cellOrigin(atlasTile) : vec2(0);
  const float cellSz = cellOnly ? mesh.tiles.cellSize() : 1.f;
  const int S = std::clamp(opt.size, 1, 32768);
  const int C = std::min(S, 1024);  // output pixels per tile
  const int P = 32;                 // overlap: height-to-normal and dilation see across tile borders
  const int T = C + 2 * P;
  progress::Scope prog("Exporting at " + std::to_string(S) + " x " + std::to_string(S));
  TextureSet tile;
  tile.allocate(T);
  const float inv = 1.f / float(S);
  // the stack's settings with tile-sized textures (resampled per tile)
  std::vector<std::unique_ptr<Layer>> tl;
  for (auto& l : layers) {
    auto c = std::make_unique<Layer>();
    c->fromJson(l->toJson());
    tl.push_back(std::move(c));
  }
  struct Writer {
    ExportMap m;
    PngStream png;
    fs::path file;
    std::vector<uint8_t> band;
    int bytes = 1;
  };
  std::vector<std::unique_ptr<Writer>> ws;
  for (auto& o : outs) {
    auto w = std::make_unique<Writer>();
    w->m = o;
    w->bytes = o.bits == 16 ? 2 : 1;
    w->file = fileFor(o.suffix);
    fs::path tmp = w->file;
    tmp += ".tmp.png";
    if (!w->png.open(tmp.string(), S, S, o.channels, o.bits, err)) return false;
    ws.push_back(std::move(w));
  }
  Texture2D target(T, T, GL_RGBA16F, 1);
  Framebuffer fbo;
  fbo.attach(0, target);
  fbo.setDrawBuffers(1);
  Shader& ex = ShaderCache::get("fullscreen.vs.glsl", "post.fs.glsl", {"POST_EXPORT"});
  uint32_t solo = opt.layerUid;
  int rowsOfTiles = (S + C - 1) / C, colsOfTiles = rowsOfTiles;
  std::vector<float> px;
  int done = 0;
  // PNG rows go top first: the top of the texture is v = 1
  for (int ty = rowsOfTiles - 1; ty >= 0; --ty) {
    int y0 = ty * C, h = std::min(C, S - y0);
    for (auto& w : ws) w->band.assign(size_t(S) * h * w->m.channels * w->bytes, 0);
    for (int tx = 0; tx < colsOfTiles; ++tx) {
      if (progress::cancelled()) {
        if (err) *err = "cancelled";
        return false;
      }
      progress::set(float(done++) / float(rowsOfTiles * colsOfTiles));
      int x0 = tx * C, w = std::min(C, S - x0);
      vec2 uv0 = cell0 + vec2((x0 - P) * inv, (y0 - P) * inv) * cellSz, uvs = vec2(T * inv, T * inv) * cellSz;
      tile.setupTile(tex, mesh, gpu, uv0, uvs, cellOnly ? S * mesh.tiles.grid : S);
      for (size_t i = 0; i < layers.size(); ++i) {
        Layer& src = *layers[i];
        Layer& dst = *tl[i];
        for (int c = 0; c < kChannelCount; ++c) {
          if (!src.hasChannel(c)) {
            dst.channels[c].reset();
            continue;
          }
          if (!dst.channels[c] || dst.channels[c]->format() != src.channels[c]->format())
            dst.channels[c] = std::make_unique<Texture2D>(T, T, src.channels[c]->format(), 1);
          TextureSet::resampleRegion(*src.channels[c], *dst.channels[c], uv0, uvs);
        }
        if (src.mask) {
          if (!dst.mask || dst.mask->format() != src.mask->format())
            dst.mask = std::make_unique<Texture2D>(T, T, src.mask->format(), 1);
          TextureSet::resampleRegion(*src.mask, *dst.mask, uv0, uvs);
        } else {
          dst.mask.reset();
        }
      }
      tile.composite(tl, *library, mesh.bounds, solo);
      for (auto& wr : ws) {
        {
          gl::FramebufferScope fs(fbo.id());
          gl::ViewportScope vp(0, 0, T, T);
          glDisable(GL_BLEND);
          glDisable(GL_DEPTH_TEST);
          ex.tex("uColor", 0, tile.finColor.id());
          ex.tex("uRMH", 1, tile.finRMH.id());
          ex.tex("uNormal", 2, tile.finNormal.id());
          ex.tex("uEmissive", 3, tile.finEmissive.id());
          ex.tex("uThick", 4, tile.thickView().id());
          ex.tex("uBent", 5, tile.bentMap.id());
          ex.tex("uNrmMap", 6, tile.nrmMap.id());
          ex.tex("uTanMap", 7, tile.tanMap.id());
          ex.tex("uExtra", 8, tile.finExtra.id());
          ex.set("uOut", wr->m.mode);
          ex.use();
          drawFullscreenTriangle();
        }
        px.resize(size_t(w) * h * 4);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glGetTextureSubImage(target.id(), 0, P, P, 0, w, h, 1, GL_RGBA, GL_FLOAT, GLsizei(px.size() * sizeof(float)),
                             px.data());
        int ch = wr->m.channels;
        for (int r = 0; r < h; ++r) {
          int bandRow = h - 1 - r;  // GL rows go up, PNG rows go down
          for (int x = 0; x < w; ++x) {
            const float* s = &px[(size_t(r) * w + x) * 4];
            size_t o = (size_t(bandRow) * S + x0 + x) * ch;
            for (int k = 0; k < ch; ++k) {
              float v = std::clamp(s[k], 0.f, 1.f);
              if (wr->bytes == 2) ((uint16_t*)wr->band.data())[o + k] = (uint16_t)std::lround(v * 65535.f);
              else wr->band[o + k] = (uint8_t)std::lround(v * 255.f);
            }
          }
        }
      }
    }
    for (auto& wr : ws)
      if (!wr->png.writeRows(wr->band.data(), h)) {
        if (err) *err = "Could not write " + wr->file.string();
        return false;
      }
  }
  for (auto& wr : ws) {
    fs::path tmp = wr->file;
    tmp += ".tmp.png";
    if (!wr->png.close()) {
      if (err) *err = "Could not write " + wr->file.string() + " (disk full?)";
      return false;
    }
    std::error_code ec;
    fs::rename(tmp, wr->file, ec);
    if (ec) {
      if (err) *err = "Could not replace " + wr->file.string() + ": " + ec.message();
      return false;
    }
    if (outFiles) (*outFiles)[wr->m.key] = fs::absolute(wr->file).string();
  }
  PLOG_I("Exported %zu maps at %d x %d in %d tiles", ws.size(), S, S, rowsOfTiles * colsOfTiles);
  if (!opt.layerUid && opt.remember) {
    lastExport = opt;
    lastExport.directory = fileFor("x").parent_path().string();
    hasLastExport = true;
  }
  return true;
}

// ------------------------------------------------------------------ layered PSD
// One PSD per map (base color, roughness, metallic, height, normal, emissive): a layer per
// Patina layer, with its mask as a layer mask, its opacity and blend mode; folders become
// groups. Paint layers carry their own paint; fill layers their material rendered alone;
// adjustment layers the result of the stack below them (they can't be Photoshop adjustments).
// The flattened image is Patina's own composite.
bool Document::exportPsd(const ExportOptions& opt, const std::function<fs::path(const std::string&, int)>& fileFor,
                         std::map<std::string, std::string>* outFiles, std::string* err) {
  const int R = tex.resolution();
  // texture sets / UDIM tiles: every tile is its own set of files, cut from the atlas
  const TileLayout& TL = mesh.tiles;
  const int nT = TL.active() ? TL.count() : 1;
  const int T = TL.active() ? tileRes() : R;
  progress::Scope prog("Exporting layered PSD files");
  bool dx = opt.preset == ExportPreset::Unreal;
  struct MapDef {
    const char* key;
    int mode;  // POST_EXPORT output
    int channel;  // Patina channel
    int bits;
  };
  std::vector<MapDef> maps = {{"basecolor", 0, 0, 8}, {"roughness", 1, 1, 8}, {"metallic", 2, 2, 8},
                              {"height", 3, 3, opt.height16 ? 16 : 8}, {"normal", dx ? 5 : 4, 4, 8}, {"emission", 6, 5, 8}};
  {  // 0.26: the other channels the texture set uses
    int used = opt.extraMaps == 1 ? kExtraChannels : opt.extraMaps == 2 ? 0 : tex.extraUsed;
    static const char* keys[] = {"opacity", "sheen", "coat", "subsurface"};
    for (int k = 0; k < 4; ++k)
      if (used & (64 << k)) maps.push_back({keys[k], 15 + k, kFirstExtraChannel + k, 8});
  }
  const int M = (int)maps.size();
  std::vector<std::unique_ptr<PsdWriter>> ws;  // [tile * M + map]
  for (int t = 0; t < nT; ++t)
    for (auto& m : maps) ws.push_back(std::make_unique<PsdWriter>(T, T, m.bits));
  // the part of an atlas-sized buffer (top row first, bpp bytes a pixel) that belongs to tile t
  std::vector<uint8_t> cropBuf;
  auto crop = [&](const void* src, int bpp, int t) -> const void* {
    if (!src || nT == 1) return src;
    int cx = t % TL.grid, cy = t / TL.grid;
    int x0 = cx * T, y0 = (TL.grid - 1 - cy) * T;  // image rows go down, atlas cells go up
    cropBuf.resize(size_t(T) * T * bpp);
    const uint8_t* s = (const uint8_t*)src;
    for (int y = 0; y < T; ++y)
      std::memcpy(&cropBuf[size_t(y) * T * bpp], s + (size_t(y0 + y) * R + x0) * bpp, size_t(T) * bpp);
    return cropBuf.data();
  };
  auto W = [&](int t, int k) -> PsdWriter& { return *ws[size_t(t) * M + k]; };
  Texture2D target(R, R, GL_RGBA16F, 1);
  Framebuffer fbo;
  fbo.attach(0, target);
  fbo.setDrawBuffers(1);
  // a fully composited map as RGBA (alpha 1), top row first, in the map's bit depth
  auto mapPixels = [&](const MapDef& m) {
    {
      gl::FramebufferScope fs(fbo.id());
      gl::ViewportScope vp(0, 0, R, R);
      glDisable(GL_BLEND);
      glDisable(GL_DEPTH_TEST);
      Shader& s = ShaderCache::get("fullscreen.vs.glsl", "post.fs.glsl", {"POST_EXPORT"});
      s.tex("uColor", 0, tex.finColor.id());
      s.tex("uRMH", 1, tex.finRMH.id());
      s.tex("uNormal", 2, tex.finNormal.id());
      s.tex("uEmissive", 3, tex.finEmissive.id());
      s.tex("uThick", 4, tex.thickView().id());
      s.tex("uBent", 5, tex.bentMap.id());
      s.tex("uNrmMap", 6, tex.nrmMap.id());
      s.tex("uTanMap", 7, tex.tanMap.id());
      s.tex("uExtra", 8, tex.finExtra.id());
      s.set("uOut", m.mode);
      s.use();
      drawFullscreenTriangle();
    }
    return target.download(4);
  };
  auto toBytes = [&](const Image& img, int bits, bool rgbOnly) {
    size_t n = size_t(img.width) * img.height;
    int ch = rgbOnly ? 3 : 4;
    std::vector<uint8_t> out(n * ch * (bits / 8));
    for (size_t i = 0; i < n; ++i)
      for (int c = 0; c < ch; ++c) {
        float v = std::clamp(img.pixels[i * img.channels + std::min(c, img.channels - 1)], 0.f, 1.f);
        if (bits == 16) ((uint16_t*)out.data())[i * ch + c] = (uint16_t)std::lround(v * 65535.f);
        else out[i * ch + c] = (uint8_t)std::lround(v * 255.f);
      }
    return out;
  };
  auto maskBytes = [&](const Layer& L, int bits) {
    Image m = tex.layerMaskImage(L, mesh.bounds);
    size_t n = size_t(R) * R;
    std::vector<uint8_t> out(n * (bits / 8));
    for (size_t i = 0; i < n; ++i) {
      float v = std::clamp(m.pixels[i], 0.f, 1.f);
      if (bits == 16) ((uint16_t*)out.data())[i] = (uint16_t)std::lround(v * 65535.f);
      else out[i] = (uint8_t)std::lround(v * 255.f);
    }
    return out;
  };
  // temporarily composite with only some layers (and some settings) changed
  struct Saved {
    bool visible;
    float opacity;
    uint32_t parent;
    BlendMode blend;
    int channelMask;
    MaskGenerator gen;
    std::vector<MaskEffect> fx;
    std::unique_ptr<Texture2D> mask;
    std::array<float, kChannelCount> chanOpacity;
  };
  std::vector<Saved> saved(layers.size());
  for (size_t i = 0; i < layers.size(); ++i) {
    Layer& L = *layers[i];
    saved[i] = {L.visible, L.opacity, L.parent, L.blend, L.channelMask, L.generator, L.maskEffects, nullptr, L.channelOpacity};
  }
  uint32_t keepSolo = soloUid;
  soloUid = 0;
  auto restore = [&]() {
    for (size_t i = 0; i < layers.size(); ++i) {
      Layer& L = *layers[i];
      L.visible = saved[i].visible, L.opacity = saved[i].opacity, L.parent = saved[i].parent, L.blend = saved[i].blend;
      L.channelMask = saved[i].channelMask, L.generator = saved[i].gen, L.maskEffects = saved[i].fx;
      L.channelOpacity = saved[i].chanOpacity;
      if (saved[i].mask) L.mask = std::move(saved[i].mask);
    }
    soloUid = keepSolo;
    markComposite();
  };
  auto plain = [&](Layer& L, size_t i) {  // full strength, no mask, normal blend, at the top level
    L.opacity = 1.f, L.parent = 0, L.blend = BlendMode::Normal, L.channelMask = kAllChannels;
    L.generator = MaskGenerator(), L.maskEffects.clear();
    L.channelOpacity.fill(1.f);
    if (L.mask) saved[i].mask = std::move(L.mask);
  };
  auto putBack = [&](Layer& L, size_t i) {
    L.opacity = saved[i].opacity, L.parent = saved[i].parent, L.blend = saved[i].blend;
    L.channelMask = saved[i].channelMask, L.generator = saved[i].gen, L.maskEffects = saved[i].fx;
    L.channelOpacity = saved[i].chanOpacity;
    if (saved[i].mask) L.mask = std::move(saved[i].mask);
  };
  auto compositeNow = [&]() {
    compositeDirty = true;
    tex.composite(layers, *library, mesh.bounds, 0);
    compositeDirty = true;
  };
  // base (everything hidden): flat base values and the baked high-poly normal
  for (auto& l : layers) l->visible = l->isBaked() && l->visible;  // (baked map layers stay: smart masks read them)
  compositeNow();
  for (int k = 0; k < M; ++k) {
    Image img = mapPixels(maps[k]);
    auto b = toBytes(img, maps[k].bits, false);
    for (int t = 0; t < nT; ++t)
      W(t, k).addLayer(tex.hasHighNormal() && k == 4 ? "Base (with the high-poly normal)" : "Base",
                       crop(b.data(), 4 * maps[k].bits / 8, t), nullptr, 255, "norm", true);
  }
  for (size_t i = 0; i < layers.size(); ++i) layers[i]->visible = saved[i].visible;
  auto contentOf = [&](Layer& L) -> Layer& { return *contentLayer(L); };
  int doneLayers = 0;
  std::function<bool(uint32_t)> group = [&](uint32_t parent) -> bool {
    for (size_t i = 0; i < layers.size(); ++i) {
      Layer& L = *layers[i];
      if (saved[i].parent != parent || L.isBaked()) continue;  // (baked map layers add nothing to the maps)
      if (progress::cancelled()) return false;
      progress::set(float(doneLayers++) / float(std::max<size_t>(1, layers.size())));
      bool hasMask = saved[i].mask || L.mask || L.generator.type != MaskGenerator::None || !L.maskEffects.empty();
      std::vector<uint8_t> mask8, mask16;
      if (hasMask) mask8 = maskBytes(L, 8), mask16 = maskBytes(L, 16);
      auto maskFor = [&](int k) -> const void* {
        if (!hasMask) return nullptr;
        return maps[k].bits == 16 ? (const void*)mask16.data() : (const void*)mask8.data();
      };
      auto opacityFor = [&](int k) {
        return (uint8_t)std::lround(std::clamp(saved[i].opacity * saved[i].chanOpacity[maps[k].channel], 0.f, 1.f) * 255.f);
      };
      auto visibleFor = [&](int k) { return saved[i].visible && (saved[i].channelMask & (1 << maps[k].channel)); };
      const char* bk = PsdWriter::blendKeyFor(blendModeName(saved[i].blend));
      if (L.isFolder()) {
        for (auto& w : ws) w->beginGroup();
        if (!group(L.uid)) return false;
        for (int t = 0; t < nT; ++t)
          for (int k = 0; k < M; ++k)
            W(t, k).endGroup(L.name, crop(maskFor(k), maps[k].bits / 8, t), opacityFor(k),
                             saved[i].blend == BlendMode::Normal ? "pass" : bk, saved[i].visible);
        continue;
      }
      Layer& C = contentOf(L);
      std::vector<std::vector<uint8_t>> px(M);
      std::string name = L.name;
      if (C.type == LayerType::Paint) {
        // the layer's own paint: straight color / value + coverage
        for (int k = 0; k < M; ++k) {
          int c = maps[k].channel;
          size_t n = size_t(R) * R;
          Image img;
          img.resize(R, R, 4);
          if (C.hasChannel(c)) {
            Image t = C.channels[c]->download(Texture2D::channelsOf(C.channels[c]->format()) == 2 ? 2 : 4);
            for (size_t p = 0; p < n; ++p) {
              float* o = &img.pixels[p * 4];
              const float* s = &t.pixels[p * t.channels];
              if (t.channels == 2) o[0] = o[1] = o[2] = s[0], o[3] = s[1];
              else o[0] = s[0], o[1] = s[1], o[2] = s[2], o[3] = s[3];
              if (c == 4 && dx) o[1] = 1.f - o[1];
            }
          }
          px[k] = toBytes(img, maps[k].bits, false);
        }
      } else {
        // fill: its material alone; adjustment: the stack below it with the adjustment applied
        bool adjust = C.type == LayerType::Adjust;
        for (size_t j = 0; j < layers.size(); ++j)
          layers[j]->visible = layers[j]->isBaked() ? saved[j].visible : adjust ? (j <= i && saved[j].visible) : j == i;
        // ancestors stay visible for an adjustment; the layer itself goes to the top level
        plain(L, i);
        compositeNow();
        for (int k = 0; k < M; ++k) {
          Image img = mapPixels(maps[k]);
          px[k] = toBytes(img, maps[k].bits, false);
        }
        putBack(L, i);
        for (size_t j = 0; j < layers.size(); ++j) layers[j]->visible = saved[j].visible;
        if (adjust) name += " (adjustment, baked)";
      }
      for (int t = 0; t < nT; ++t)
        for (int k = 0; k < M; ++k) {
          // the pixels are cropped first: crop() reuses one buffer, so the mask gets a copy
          std::vector<uint8_t> pxT;
          const void* pp = crop(px[k].data(), 4 * maps[k].bits / 8, t);
          if (nT > 1) pxT.assign((const uint8_t*)pp, (const uint8_t*)pp + size_t(T) * T * 4 * maps[k].bits / 8), pp = pxT.data();
          W(t, k).addLayer(name, pp, crop(maskFor(k), maps[k].bits / 8, t), opacityFor(k), bk, visibleFor(k));
        }
    }
    return true;
  };
  bool ok = group(0);
  // the flattened image: Patina's composite
  restore();
  compositeNow();
  if (!ok) {
    if (err) *err = "cancelled";
    return false;
  }
  for (int k = 0; k < M; ++k) {
    Image img = mapPixels(maps[k]);
    auto b = toBytes(img, maps[k].bits, true);
    for (int t = 0; t < nT; ++t) {
      W(t, k).setComposite(crop(b.data(), 3 * maps[k].bits / 8, t));
      fs::path file = fileFor(maps[k].key, t);
      if (!W(t, k).save(file.string(), err)) return false;
      std::string key = nT > 1 ? std::string(maps[k].key) + "@" + std::to_string(t) : std::string(maps[k].key);
      if (outFiles) (*outFiles)[key] = fs::absolute(file).string();
    }
  }
  markComposite();
  PLOG_I("Exported %d layered PSD files (%zu layers) to %s", M * nT, layers.size(),
         fileFor("x", 0).parent_path().string().c_str());
  return true;
}

}  // namespace patina
