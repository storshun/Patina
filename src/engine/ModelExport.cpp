// 0.28: the whole model with its textures, as glTF (.glb / .gltf) or OBJ; 0.30 STL; 0.32 PLY with vertex colors.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <unordered_map>

#include "core/Image.h"
#include "core/Collision.h"
#include "core/Log.h"
#include "core/MeshReduce.h"
#include "core/Paths.h"
#include "engine/Document.h"

namespace patina {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

struct Vertex {
  vec3 p, n;
  vec4 t;
  vec2 uv;
  vec2 atlas;  // where the vertex reads the texture set (PLY vertex colors)
  vec2 uv2{0.f};  // the lightmap UV (0.35)
};
struct Prim {
  int tile = 0;
  std::vector<uint32_t> idx;  // triangles
  std::vector<uint32_t> faces;
};

// Vertices shared by corners with the same position, UV and normal; triangles per texture set / UDIM tile.
void buildGeometry(const Mesh& m, std::vector<Vertex>& verts, std::vector<Prim>& prims, bool flipV,
                   std::vector<uint32_t>* loopMap = nullptr) {
  std::unordered_map<std::string, uint32_t> seen;
  seen.reserve(m.loopCount());
  int nt = m.tiles.active() ? m.tiles.count() : 1;
  prims.assign(nt, Prim());
  for (int t = 0; t < nt; ++t) prims[t].tile = t;
  std::vector<uint32_t> loopOut(m.loopCount());
  for (uint32_t f = 0; f < m.faceCount(); ++f) {
    int t = m.tiles.active() ? std::clamp(m.faceTile(f), 0, nt - 1) : 0;
    vec2 origin = m.tiles.active() ? vec2(m.tiles.uvOrigin(t)) : vec2(0.f);
    prims[t].faces.push_back(f);
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) {
      Vertex v;
      v.p = m.positions[m.loopVert[l]];
      v.n = l < m.loopNormal.size() ? m.loopNormal[l] : vec3(0, 1, 0);
      v.t = l < m.loopTangent.size() ? m.loopTangent[l] : vec4(1, 0, 0, 1);
      v.uv = m.loopUV[l] - origin;
      v.atlas = m.loopAtlasUV.size() == m.loopCount() ? m.loopAtlasUV[l] : m.loopUV[l];
      if (m.loopUV2.size() == m.loopCount()) {
        v.uv2 = m.loopUV2[l];
        if (flipV) v.uv2.y = 1.f - v.uv2.y;
      }
      if (flipV) v.uv.y = 1.f - v.uv.y;
      char key[sizeof(uint32_t) + 2 * sizeof(vec2) + sizeof(vec3)];
      std::memcpy(key, &m.loopVert[l], sizeof(uint32_t));
      std::memcpy(key + 4, &v.uv, sizeof(vec2));
      std::memcpy(key + 12, &v.n, sizeof(vec3));
      std::memcpy(key + 24, &v.uv2, sizeof(vec2));
      auto it = seen.emplace(std::string(key, sizeof key), (uint32_t)verts.size());
      if (it.second) verts.push_back(v);
      loopOut[l] = it.first->second;
    }
  }
  // triangles: Mesh::tris holds loop indices, triFace the face
  for (size_t i = 0; i < m.tris.size(); ++i) {
    uint32_t f = m.triFace[i];
    int t = m.tiles.active() ? std::clamp(m.faceTile(f), 0, nt - 1) : 0;
    const glm::uvec3& tr = m.tris[i];
    prims[t].idx.insert(prims[t].idx.end(), {loopOut[tr.x], loopOut[tr.y], loopOut[tr.z]});
  }
  prims.erase(std::remove_if(prims.begin(), prims.end(), [](const Prim& p) { return p.idx.empty(); }), prims.end());
  if (loopMap) *loopMap = std::move(loopOut);
}

// The final base color (sRGB, top row first) at a texture-set UV, filtered between texels.
vec3 sampleColor(const Image& img, vec2 uv) {
  float x = uv.x * img.width - 0.5f, y = (1.f - uv.y) * img.height - 0.5f;
  int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
  float fx = x - x0, fy = y - y0;
  auto at = [&](int xi, int yi) {
    xi = std::clamp(xi, 0, img.width - 1), yi = std::clamp(yi, 0, img.height - 1);
    const float* p = img.at(xi, yi);
    return vec3(p[0], p[1], p[2]);
  };
  return glm::mix(glm::mix(at(x0, y0), at(x0 + 1, y0), fx), glm::mix(at(x0, y0 + 1), at(x0 + 1, y0 + 1), fx), fy);
}

std::string readBytes(const std::string& path) { return paths::readFile(path); }

std::string materialName(const Mesh& m, int tile) {
  if (!m.tiles.active()) return m.materialNames.empty() ? std::string("Patina") : m.slotName(0);
  const TileInfo& ti = m.tiles.tiles[tile];
  return ti.name.empty() ? m.slotName(ti.slot) : ti.name;
}

}  // namespace

bool Document::exportModel(const ModelExportOptions& opt, json* out, std::string* err) {
  auto fail = [&](const std::string& e) {
    if (err) *err = e;
    return false;
  };
  if (!hasMesh()) return fail("No mesh loaded");
  fs::path path = opt.path;
  std::string ext = path.extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
  if (ext != ".glb" && ext != ".gltf" && ext != ".obj" && ext != ".stl" && ext != ".ply")
    return fail("export_model: the file must end in .glb, .gltf, .obj, .stl or .ply");
  // (1.2) an object placed in a scene: glTF keeps the model as it is under a node with the matrix; the other formats
  // have no such thing, so its vertices are moved (a copy: `mesh` below is the one written)
  const bool gltfFile = ext == ".glb" || ext == ".gltf";
  Mesh placed;
  if (opt.transform != mat4(1) && !gltfFile) {
    placed = this->mesh;
    const mat3 N = glm::transpose(glm::inverse(mat3(opt.transform)));
    for (auto& p : placed.positions) p = vec3(opt.transform * vec4(p, 1.f));
    for (auto& n : placed.loopNormal) n = glm::normalize(N * n);
    for (auto& t : placed.loopTangent) t = vec4(glm::normalize(mat3(opt.transform) * vec3(t)), t.w);
    placed.computeBounds();
  }
  const Mesh& mesh = placed.faceCount() ? placed : this->mesh;
  auto materialName = [&](const Mesh& m, int tile) { return opt.materialPrefix + ::patina::materialName(m, tile); };
  if (ext == ".ply") {
    // (0.32) binary PLY with the base color as vertex colors (sRGB), UVs and normals; Z up, meters, as Blender
    // reads it. Faces keep their quads and n-gons.
    std::error_code ec2;
    fs::create_directories(path.parent_path().empty() ? fs::path(".") : path.parent_path(), ec2);
    if (library) updateComposite(*library);
    Image col = tex.finColor.download(4);
    std::vector<Vertex> verts;
    std::vector<Prim> prims;
    std::vector<uint32_t> loopMap;
    buildGeometry(mesh, verts, prims, false, &loopMap);
    // faces of more than 255 corners go as their triangles
    std::vector<std::vector<uint32_t>> big;
    size_t nFaces = 0;
    for (uint32_t f = 0; f < mesh.faceCount(); ++f) nFaces += mesh.faceStart[f + 1] - mesh.faceStart[f] <= 255;
    for (size_t i = 0; i < mesh.tris.size(); ++i) {
      uint32_t f = mesh.triFace[i];
      if (mesh.faceStart[f + 1] - mesh.faceStart[f] > 255) {
        const glm::uvec3& t = mesh.tris[i];
        big.push_back({loopMap[t.x], loopMap[t.y], loopMap[t.z]});
      }
    }
    std::string file = std::string("ply\nformat binary_little_endian 1.0\ncomment Patina ") + PATINA_VERSION +
                       ": Z up, meters; vertex colors are the base color (sRGB)\n" +
                       "element vertex " + std::to_string(verts.size()) +
                       "\nproperty float x\nproperty float y\nproperty float z\nproperty float nx\nproperty float ny\n"
                       "property float nz\nproperty float s\nproperty float t\nproperty uchar red\nproperty uchar green\n"
                       "property uchar blue\nproperty uchar alpha\nelement face " + std::to_string(nFaces + big.size()) +
                       "\nproperty list uchar int vertex_indices\nend_header\n";
    auto put = [&](const void* p, size_t n) { file.append((const char*)p, n); };  // x86 / ARM: little endian
    for (const Vertex& v : verts) {
      float f[8] = {v.p.x, -v.p.z, v.p.y, v.n.x, -v.n.z, v.n.y, v.uv.x, v.uv.y};
      put(f, sizeof f);
      vec3 c = glm::clamp(sampleColor(col, v.atlas), vec3(0.f), vec3(1.f));
      uint8_t rgba[4] = {(uint8_t)std::lround(c.x * 255), (uint8_t)std::lround(c.y * 255), (uint8_t)std::lround(c.z * 255), 255};
      put(rgba, 4);
    }
    for (uint32_t f = 0; f < mesh.faceCount(); ++f) {
      uint32_t n = mesh.faceStart[f + 1] - mesh.faceStart[f];
      if (n > 255) continue;
      uint8_t cnt = (uint8_t)n;
      put(&cnt, 1);
      for (uint32_t l = mesh.faceStart[f]; l < mesh.faceStart[f + 1]; ++l) {
        int32_t i = (int32_t)loopMap[l];
        put(&i, 4);
      }
    }
    for (auto& t : big) {
      uint8_t cnt = 3;
      put(&cnt, 1);
      for (uint32_t i : t) put(&i, 4);
    }
    if (!paths::writeFileAtomic(path, file)) return fail("Could not write " + path.string());
    if (out) *out = {{"path", fs::absolute(path).string()}, {"format", "ply"}, {"vertices", verts.size()},
                     {"faces", nFaces + big.size()}};
    PLOG_I("Exported %zu vertices with colors to %s", verts.size(), path.string().c_str());
    return true;
  }
  if (ext == ".stl") {
    // 3D printing: binary STL, Z up, millimeters (the way slicers read it); triangles only, no textures
    std::error_code ec2;
    fs::create_directories(path.parent_path().empty() ? fs::path(".") : path.parent_path(), ec2);
    std::string file(80, '\0');
    std::string head = std::string("Patina ") + PATINA_VERSION + " STL (mm, Z up)";
    file.replace(0, std::min<size_t>(head.size(), 80), head.substr(0, 80));
    uint32_t n = (uint32_t)mesh.tris.size();
    file.append((const char*)&n, 4);
    auto toPrint = [](const vec3& p) { return vec3(p.x, -p.z, p.y) * 1000.f; };
    for (auto& t : mesh.tris) {
      vec3 a = toPrint(mesh.positions[mesh.loopVert[t.x]]), b = toPrint(mesh.positions[mesh.loopVert[t.y]]),
           c = toPrint(mesh.positions[mesh.loopVert[t.z]]);
      vec3 nrm = glm::cross(b - a, c - a);
      float len = glm::length(nrm);
      nrm = len > 0 ? nrm / len : vec3(0, 0, 1);
      float f[12] = {nrm.x, nrm.y, nrm.z, a.x, a.y, a.z, b.x, b.y, b.z, c.x, c.y, c.z};
      file.append((const char*)f, sizeof f);
      file.append(2, '\0');
    }
    if (!paths::writeFileAtomic(path, file)) return fail("Could not write " + path.string());
    if (out) *out = {{"path", fs::absolute(path).string()}, {"format", "stl"}, {"triangles", n}};
    PLOG_I("Exported %u triangles to %s", n, path.string().c_str());
    return true;
  }
  std::error_code ec;
  fs::create_directories(path.parent_path().empty() ? fs::path(".") : path.parent_path(), ec);
  const std::string stem = path.stem().string();
  const bool glb = ext == ".glb", obj = ext == ".obj";
  // the textures: next to the model (embedded for .glb)
  std::map<std::string, std::string> tex;
  fs::path texDir = glb ? paths::ensureDir(paths::cacheDir() / "model_export" / paths::sanitizeFileName(stem)) : path.parent_path();
  if (opt.textures) {
    if (library) updateComposite(*library);
    ExportOptions eo;
    eo.preset = obj ? ExportPreset::Blender : ExportPreset::GLTF;
    eo.directory = texDir.string();
    eo.baseName = stem;
    eo.size = opt.textureSize;
    eo.remember = false;
    eo.includeThickness = false;
    eo.includeBentNormal = false;
    eo.height16 = false;
    if (!exportTextures(eo, &tex, err)) return false;
  }
  const int nt = mesh.tiles.active() ? mesh.tiles.count() : 1;
  auto texFor = [&](const std::string& key, int tile) -> std::string {
    auto it = tex.find(mesh.tiles.active() ? key + "@" + std::to_string(tile) : key);
    return it == tex.end() ? std::string() : it->second;
  };
  json result = {{"path", fs::absolute(path).string()}, {"format", ext.substr(1)}, {"faces", mesh.faceCount()},
                 {"materials", nt}};
  // the model, then its levels of detail (the same UVs: they share the textures)
  std::vector<Mesh> lodMeshes;
  json lodInfo = json::array();
  for (float ratio : opt.lods) {
    ReduceOptions ro;
    ro.ratio = ratio;
    ReduceResult rr;
    lodMeshes.push_back(patina::reduceMesh(mesh, ro, &rr));
    lodInfo.push_back({{"ratio", ratio}, {"triangles", rr.trianglesAfter}});
  }
  std::vector<const Mesh*> levels = {&mesh};
  for (auto& l : lodMeshes) levels.push_back(&l);
  const bool hasLods = levels.size() > 1;
  if (hasLods) result["lods"] = lodInfo;
  const std::string baseName = mesh.name.empty() ? stem : mesh.name;
  auto levelName = [&](size_t i) { return hasLods ? baseName + "_LOD" + std::to_string(i) : baseName; };
  // (0.34) collision shapes: the ones made in Mesh > Collision, else made now with the usual settings
  std::vector<ConvexShape> coll;
  if (!opt.collision.empty()) {
    const CollisionResult* cr = collisionShapes();
    if (cr) coll = cr->shapes;
    else coll = patina::makeCollision(mesh, collisionOptions).shapes;
    result["collision"] = {{"naming", opt.collision}, {"shapes", coll.size()}};
  }
  auto collisionName = [&](size_t i) {
    char num[8];
    snprintf(num, sizeof num, "%02zu", i);
    const std::string& n = opt.collision;
    if (n == "godot") return levelName(0) + "_col" + num + "-convcolonly";
    if (n == "plain") return levelName(0) + "_collision_" + num;
    return std::string(coll[i].box ? "UBX_" : "UCX_") + levelName(0) + "_" + num;  // Unreal
  };

  if (obj) {
    fs::path mtl = path;
    mtl.replace_extension(".mtl");
    auto writeObj = [&](const Mesh& M, const fs::path& file, const std::string& objName) {
      std::ofstream f(file);
      if (!f) return false;
      f << "# Patina " << PATINA_VERSION << "\n";
      if (opt.textures) f << "mtllib " << mtl.filename().string() << "\n";
      f << "o " << objName << "\n";
      for (auto& p : M.positions) f << "v " << p.x << " " << p.y << " " << p.z << "\n";
      // one vt / vn per corner (the faces keep their n-gons)
      for (uint32_t f2 = 0; f2 < M.faceCount(); ++f2) {
        vec2 origin = M.tiles.active() ? vec2(M.tiles.uvOrigin(std::clamp(M.faceTile(f2), 0, nt - 1))) : vec2(0.f);
        for (uint32_t l = M.faceStart[f2]; l < M.faceStart[f2 + 1]; ++l) {
          vec2 uv = M.loopUV[l] - origin;
          f << "vt " << uv.x << " " << uv.y << "\n";
        }
      }
      for (uint32_t l = 0; l < M.loopCount(); ++l) {
        vec3 n = l < M.loopNormal.size() ? M.loopNormal[l] : vec3(0, 1, 0);
        f << "vn " << n.x << " " << n.y << " " << n.z << "\n";
      }
      for (int t = 0; t < nt; ++t) {
        f << "usemtl " << paths::sanitizeFileName(materialName(mesh, t)) << "\n";
        for (uint32_t f2 = 0; f2 < M.faceCount(); ++f2) {
          if (M.tiles.active() && std::clamp(M.faceTile(f2), 0, nt - 1) != t) continue;
          f << "f";
          for (uint32_t l = M.faceStart[f2]; l < M.faceStart[f2 + 1]; ++l)
            f << " " << M.loopVert[l] + 1 << "/" << l + 1 << "/" << l + 1;
          f << "\n";
        }
      }
      return f.good();
    };
    json lodFiles = json::array();
    for (size_t i = 0; i < levels.size(); ++i) {
      fs::path file = i == 0 ? path : path.parent_path() / (stem + "_LOD" + std::to_string(i) + ".obj");
      if (!writeObj(*levels[i], file, levelName(i))) return fail("Could not write " + file.string());
      if (i > 0) lodFiles.push_back(fs::absolute(file).string());
    }
    if (!coll.empty()) {
      // the collision shapes as objects of their own after the model (no UVs, no material)
      std::ofstream f(path, std::ios::app);
      size_t base = mesh.positions.size();
      for (size_t i = 0; i < coll.size(); ++i) {
        f << "o " << collisionName(i) << "\nusemtl None\n";
        for (auto& p : coll[i].verts) f << "v " << p.x << " " << p.y << " " << p.z << "\n";
        for (auto& t : coll[i].tris) f << "f " << base + t.x + 1 << " " << base + t.y + 1 << " " << base + t.z + 1 << "\n";
        base += coll[i].verts.size();
      }
      if (!f.good()) return fail("Could not write " + path.string());
    }
    if (hasLods) result["lod_files"] = lodFiles;
    if (opt.textures) {
      std::ofstream m(mtl);
      m << "# Patina " << PATINA_VERSION << "\n";
      auto rel = [&](const std::string& p) { return fs::path(p).filename().string(); };
      for (int t = 0; t < nt; ++t) {
        m << "\nnewmtl " << paths::sanitizeFileName(materialName(mesh, t)) << "\nKd 1 1 1\nKs 0 0 0\n";
        if (auto p = texFor("basecolor", t); !p.empty()) m << "map_Kd " << rel(p) << "\n";
        if (auto p = texFor("roughness", t); !p.empty()) m << "map_Pr " << rel(p) << "\n";
        if (auto p = texFor("metallic", t); !p.empty()) m << "map_Pm " << rel(p) << "\n";
        if (auto p = texFor("normal", t); !p.empty()) m << "norm " << rel(p) << "\nmap_Bump -bm 1 " << rel(p) << "\n";
        if (auto p = texFor("emission", t); !p.empty()) m << "map_Ke " << rel(p) << "\nKe 1 1 1\n";
        if (auto p = texFor("opacity", t); !p.empty()) m << "map_d " << rel(p) << "\n";
        if (auto p = texFor("sheen", t); !p.empty()) m << "map_Ps " << rel(p) << "\n";
        if (auto p = texFor("coat", t); !p.empty()) m << "map_Pc " << rel(p) << "\n";
      }
      if (!m.good()) return fail("Could not write " + mtl.string());
      result["mtl"] = fs::absolute(mtl).string();
    }
    json files = json::object();
    for (auto& [k, p] : tex) files[k] = p;
    result["textures"] = files;
    if (out) *out = result;
    PLOG_I("Exported the model to %s", path.string().c_str());
    return true;
  }

  // ---- glTF 2.0
  std::vector<uint8_t> bin;
  json bufferViews = json::array(), accessors = json::array();
  auto align4 = [&]() {
    while (bin.size() % 4) bin.push_back(0);
  };
  auto addView = [&](const void* data, size_t bytes, int target) {
    align4();
    json v = {{"buffer", 0}, {"byteOffset", bin.size()}, {"byteLength", bytes}};
    if (target) v["target"] = target;
    bin.insert(bin.end(), (const uint8_t*)data, (const uint8_t*)data + bytes);
    bufferViews.push_back(v);
    return (int)bufferViews.size() - 1;
  };
  json images = json::array(), textures = json::array(), materials = json::array();
  json samplers = json::array({{{"magFilter", 9729}, {"minFilter", 9987}, {"wrapS", 10497}, {"wrapT", 10497}}});
  std::unordered_map<std::string, int> textureOf;  // file -> texture index
  std::vector<std::string> embedded;
  auto textureIndex = [&](const std::string& file) -> int {
    if (file.empty()) return -1;
    auto it = textureOf.find(file);
    if (it != textureOf.end()) return it->second;
    json img = {{"name", fs::path(file).stem().string()}};
    if (glb) {
      std::string bytes = readBytes(file);
      if (bytes.empty()) return -1;
      img["bufferView"] = addView(bytes.data(), bytes.size(), 0);
      img["mimeType"] = "image/png";
      embedded.push_back(file);
    } else {
      img["uri"] = fs::path(file).filename().string();
    }
    images.push_back(img);
    textures.push_back({{"sampler", 0}, {"source", (int)images.size() - 1}});
    textureOf[file] = (int)textures.size() - 1;
    return textureOf[file];
  };
  // see-through: Mask when the opacity is (nearly) all 0 or 1, Blend otherwise
  std::string alphaMode = "OPAQUE";
  if (opt.textures && (tex.count("basecolor") || tex.count("basecolor@0")) && (this->tex.extraUsed & 64)) {
    Image x = this->tex.finExtra.download(4), cov = this->tex.posMap.download(4);
    size_t mid = 0, n = 0;
    for (size_t i = 0; i < size_t(x.width) * x.height; ++i)
      if (cov.pixels[i * 4 + 3] > 0.5f) {
        float a = x.pixels[i * 4];
        n++;
        mid += a > 0.05f && a < 0.95f;
      }
    alphaMode = n && mid * 50 < n ? "MASK" : "BLEND";
  }
  std::set<std::string> extensionsUsed;
  for (int t = 0; t < nt; ++t) {
    json mat = {{"name", materialName(mesh, t)}, {"pbrMetallicRoughness", {{"metallicFactor", 1.0}, {"roughnessFactor", 1.0}}}};
    if (opt.textures) {
      if (int i = textureIndex(texFor("basecolor", t)); i >= 0) mat["pbrMetallicRoughness"]["baseColorTexture"] = {{"index", i}};
      if (int i = textureIndex(texFor("metallicRoughness", t)); i >= 0)
        mat["pbrMetallicRoughness"]["metallicRoughnessTexture"] = {{"index", i}};
      if (int i = textureIndex(texFor("normal", t)); i >= 0) mat["normalTexture"] = {{"index", i}};
      if (int i = textureIndex(texFor("occlusion", t)); i >= 0) mat["occlusionTexture"] = {{"index", i}};
      if (int i = textureIndex(texFor("emissive", t)); i >= 0) {
        mat["emissiveTexture"] = {{"index", i}};
        mat["emissiveFactor"] = {1.0, 1.0, 1.0};
      }
      if (alphaMode != "OPAQUE") {
        mat["alphaMode"] = alphaMode;
        if (alphaMode == "MASK") mat["alphaCutoff"] = 0.5;
        mat["doubleSided"] = true;
      }
      if (int i = textureIndex(texFor("sheen", t)); i >= 0) {
        mat["extensions"]["KHR_materials_sheen"] = {{"sheenColorFactor", {1.0, 1.0, 1.0}}, {"sheenColorTexture", {{"index", i}}},
                                                    {"sheenRoughnessFactor", 0.5}};
        extensionsUsed.insert("KHR_materials_sheen");
      }
      if (int i = textureIndex(texFor("coat", t)); i >= 0) {
        mat["extensions"]["KHR_materials_clearcoat"] = {{"clearcoatFactor", 1.0}, {"clearcoatTexture", {{"index", i}}},
                                                        {"clearcoatRoughnessFactor", 0.04}};
        extensionsUsed.insert("KHR_materials_clearcoat");
      }
    }
    materials.push_back(mat);
  }
  // the meshes: vertices (shared by the mesh's primitives), a primitive per material
  json meshes = json::array(), nodes = json::array(), sceneNodes = json::array();
  size_t N = 0;
  for (size_t li = 0; li < levels.size(); ++li) {
    std::vector<Vertex> verts;
    std::vector<Prim> prims;
    buildGeometry(*levels[li], verts, prims, true);
    std::vector<float> pos, nrm, tan, uv, uv2;
    const bool hasUV2 = levels[li]->loopUV2.size() == levels[li]->loopCount();
    vec3 lo(1e30f), hi(-1e30f);
    for (auto& v : verts) {
      if (hasUV2) uv2.insert(uv2.end(), {v.uv2.x, v.uv2.y});
      pos.insert(pos.end(), {v.p.x, v.p.y, v.p.z});
      lo = glm::min(lo, v.p), hi = glm::max(hi, v.p);
      vec3 n = glm::length(v.n) > 1e-12f ? glm::normalize(v.n) : vec3(0, 1, 0);
      nrm.insert(nrm.end(), {n.x, n.y, n.z});
      vec3 t3 = glm::length(vec3(v.t)) > 1e-12f ? glm::normalize(vec3(v.t)) : vec3(1, 0, 0);
      tan.insert(tan.end(), {t3.x, t3.y, t3.z, v.t.w < 0.f ? -1.f : 1.f});
      uv.insert(uv.end(), {v.uv.x, v.uv.y});
    }
    size_t n = verts.size();
    if (li == 0) N = n;
    int a0 = (int)accessors.size();
    int vPos = addView(pos.data(), pos.size() * 4, 34962), vNrm = addView(nrm.data(), nrm.size() * 4, 34962);
    int vTan = addView(tan.data(), tan.size() * 4, 34962), vUV = addView(uv.data(), uv.size() * 4, 34962);
    accessors.push_back({{"bufferView", vPos}, {"componentType", 5126}, {"count", n}, {"type", "VEC3"},
                         {"min", {lo.x, lo.y, lo.z}}, {"max", {hi.x, hi.y, hi.z}}});
    accessors.push_back({{"bufferView", vNrm}, {"componentType", 5126}, {"count", n}, {"type", "VEC3"}});
    accessors.push_back({{"bufferView", vTan}, {"componentType", 5126}, {"count", n}, {"type", "VEC4"}});
    accessors.push_back({{"bufferView", vUV}, {"componentType", 5126}, {"count", n}, {"type", "VEC2"}});
    int aUV2 = -1;
    if (hasUV2) {
      int v2 = addView(uv2.data(), uv2.size() * 4, 34962);
      accessors.push_back({{"bufferView", v2}, {"componentType", 5126}, {"count", n}, {"type", "VEC2"}});
      aUV2 = (int)accessors.size() - 1;
    }
    json primitives = json::array();
    for (auto& p : prims) {
      int vi = addView(p.idx.data(), p.idx.size() * 4, 34963);
      accessors.push_back({{"bufferView", vi}, {"componentType", 5125}, {"count", p.idx.size()}, {"type", "SCALAR"}});
      json attrs = {{"POSITION", a0}, {"NORMAL", a0 + 1}, {"TANGENT", a0 + 2}, {"TEXCOORD_0", a0 + 3}};
      if (aUV2 >= 0) attrs["TEXCOORD_1"] = aUV2;  // the lightmap UVs (0.35)
      primitives.push_back({{"attributes", attrs},
                            {"indices", (int)accessors.size() - 1},
                            {"material", std::min(p.tile, (int)materials.size() - 1)}});
    }
    meshes.push_back({{"name", levelName(li)}, {"primitives", primitives}});
    nodes.push_back({{"name", levelName(li)}, {"mesh", (int)li}});
    if (opt.transform != mat4(1)) {  // (1.2) where the object is in the scene
      json mm = json::array();
      for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) mm.push_back(opt.transform[c][r]);
      nodes.back()["matrix"] = mm;
    }
    sceneNodes.push_back((int)li);
  }
  for (size_t ci = 0; ci < coll.size(); ++ci) {
    // collision shapes: meshes without a material, named the way the engine finds them
    const ConvexShape& sh = coll[ci];
    std::vector<float> pos;
    vec3 lo(1e30f), hi(-1e30f);
    for (auto& p : sh.verts) pos.insert(pos.end(), {p.x, p.y, p.z}), lo = glm::min(lo, p), hi = glm::max(hi, p);
    std::vector<uint32_t> idx;
    for (auto& t : sh.tris) idx.insert(idx.end(), {t.x, t.y, t.z});
    int vp = addView(pos.data(), pos.size() * 4, 34962), vi = addView(idx.data(), idx.size() * 4, 34963);
    accessors.push_back({{"bufferView", vp}, {"componentType", 5126}, {"count", sh.verts.size()}, {"type", "VEC3"},
                         {"min", {lo.x, lo.y, lo.z}}, {"max", {hi.x, hi.y, hi.z}}});
    accessors.push_back({{"bufferView", vi}, {"componentType", 5125}, {"count", idx.size()}, {"type", "SCALAR"}});
    int a = (int)accessors.size() - 2;
    meshes.push_back({{"name", collisionName(ci)},
                      {"primitives", {{{"attributes", {{"POSITION", a}}}, {"indices", a + 1}}}}});
    nodes.push_back({{"name", collisionName(ci)}, {"mesh", (int)meshes.size() - 1}, {"extras", {{"collision", true}}}});
    sceneNodes.push_back((int)nodes.size() - 1);
  }
  if (hasLods) {
    // MSFT_lod on the full model; engines without it see the levels as separate meshes named _LOD0, _LOD1...
    json ids = json::array(), cover = json::array();
    float c = 0.5f;
    for (size_t i = 1; i < levels.size(); ++i) ids.push_back((int)i), cover.push_back(c), c *= 0.5f;
    cover.push_back(0.0f);
    nodes[0]["extensions"]["MSFT_lod"] = {{"ids", ids}};
    nodes[0]["extras"]["MSFT_screencoverage"] = cover;
    extensionsUsed.insert("MSFT_lod");
  }
  align4();
  json g = {{"asset", {{"version", "2.0"}, {"generator", std::string("Patina ") + PATINA_VERSION}}},
            {"scene", 0},
            {"scenes", {{{"nodes", sceneNodes}}}},
            {"nodes", nodes},
            {"meshes", meshes},
            {"materials", materials},
            {"accessors", accessors},
            {"bufferViews", bufferViews},
            {"buffers", {{{"byteLength", bin.size()}}}}};
  if (!images.empty()) g["images"] = images, g["textures"] = textures, g["samplers"] = samplers;
  if (!extensionsUsed.empty()) g["extensionsUsed"] = json(std::vector<std::string>(extensionsUsed.begin(), extensionsUsed.end()));
  if (glb) {
    std::string js = g.dump();
    while (js.size() % 4) js += ' ';
    std::string file;
    auto u32 = [&](uint32_t v) { file.append((const char*)&v, 4); };
    u32(0x46546C67), u32(2), u32(uint32_t(12 + 8 + js.size() + 8 + bin.size()));
    u32((uint32_t)js.size()), u32(0x4E4F534A), file += js;
    u32((uint32_t)bin.size()), u32(0x004E4942), file.append((const char*)bin.data(), bin.size());
    if (!paths::writeFileAtomic(path, file)) return fail("Could not write " + path.string());
    for (auto& e : embedded) fs::remove(e, ec);
    fs::remove(texDir, ec);
  } else {
    fs::path binPath = path;
    binPath.replace_extension(".bin");
    g["buffers"][0]["uri"] = binPath.filename().string();
    if (!paths::writeFileAtomic(binPath, std::string((const char*)bin.data(), bin.size())) ||
        !paths::writeFileAtomic(path, g.dump(1)))
      return fail("Could not write " + path.string());
    json files = json::object();
    for (auto& [k, p] : tex) files[k] = p;
    result["textures"] = files;
    result["bin"] = fs::absolute(binPath).string();
  }
  result["vertices"] = N;
  result["alpha_mode"] = alphaMode;
  result["extensions"] = json(std::vector<std::string>(extensionsUsed.begin(), extensionsUsed.end()));
  if (out) *out = result;
  PLOG_I("Exported the model to %s (%zu vertices, %d material%s)", path.string().c_str(), N, nt, nt == 1 ? "" : "s");
  return true;
}

}  // namespace patina
