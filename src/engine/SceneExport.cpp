// (1.2) The scene's objects in one model file: each object exported as it is (Document::exportModel, placed by its
// matrix), then the files put together. glTF: one scene with a node per object (its matrix), its meshes, materials and
// textures. OBJ: one file, the objects' vertices moved into the scene, a material file for all. STL and PLY: the
// triangles or faces of all, moved into the scene.
#include <algorithm>
#include <cstring>
#include <functional>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#include "core/Log.h"
#include "core/Paths.h"
#include "engine/Engine.h"

namespace fs = std::filesystem;

namespace patina {

namespace {

// a texture's file for a scene's folder: the object's name first (the textures are named after it already)
std::string withPrefix(const std::string& prefix, const std::string& file) {
  std::string p = paths::sanitizeFileName(prefix);
  return file.rfind(p, 0) == 0 ? file : p + file;
}

bool failWith(std::string* err, const std::string& e) {
  if (err) *err = e;
  return false;
}

// glTF: the parts of `g` (with its buffer `b`) after those already in `G` (and `B`); images embedded (glb) or copied
// next to `dir` as `prefix` + their name
bool mergeGltf(json& G, std::vector<uint8_t>& B, const json& g, const std::string& b, const fs::path& srcDir, bool glb,
               const fs::path& dir, const std::string& prefix, std::vector<int>& roots, std::string* err) {
  auto count = [&](const char* k) { return G.contains(k) ? (int)G[k].size() : 0; };
  const int bv0 = count("bufferViews"), acc0 = count("accessors"), img0 = count("images"), tex0 = count("textures"),
            mat0 = count("materials"), mesh0 = count("meshes"), node0 = count("nodes"), smp0 = count("samplers");
  while (B.size() % 4) B.push_back(0);
  const size_t off = B.size();
  B.insert(B.end(), b.begin(), b.end());
  for (json v : g.value("bufferViews", json::array())) {
    v["buffer"] = 0;
    v["byteOffset"] = v.value("byteOffset", size_t(0)) + off;
    G["bufferViews"].push_back(v);
  }
  for (json a : g.value("accessors", json::array())) {
    if (a.contains("bufferView")) a["bufferView"] = a["bufferView"].get<int>() + bv0;
    G["accessors"].push_back(a);
  }
  for (json im : g.value("images", json::array())) {
    if (im.contains("uri")) {
      fs::path src = srcDir / im["uri"].get<std::string>();
      if (glb) {
        std::string bytes = paths::readFile(src);
        if (bytes.empty()) return failWith(err, "Could not read " + src.string());
        while (B.size() % 4) B.push_back(0);
        G["bufferViews"].push_back({{"buffer", 0}, {"byteOffset", B.size()}, {"byteLength", bytes.size()}});
        B.insert(B.end(), bytes.begin(), bytes.end());
        im.erase("uri");
        im["bufferView"] = (int)G["bufferViews"].size() - 1;
        im["mimeType"] = "image/png";
      } else {
        std::string name = withPrefix(prefix, src.filename().string());
        std::error_code ec;
        fs::copy_file(src, dir / name, fs::copy_options::overwrite_existing, ec);
        if (ec) return failWith(err, "Could not write " + (dir / name).string());
        im["uri"] = name;
      }
    } else if (im.contains("bufferView")) {
      im["bufferView"] = im["bufferView"].get<int>() + bv0;
    }
    G["images"].push_back(im);
  }
  for (json t : g.value("textures", json::array())) {
    if (t.contains("source")) t["source"] = t["source"].get<int>() + img0;
    if (t.contains("sampler")) t["sampler"] = t["sampler"].get<int>() + smp0;
    G["textures"].push_back(t);
  }
  for (auto& s : g.value("samplers", json::array())) G["samplers"].push_back(s);
  // every {"index": n} under a material points at a texture
  std::function<void(json&)> shiftTex = [&](json& j) {
    if (j.is_object()) {
      for (auto it = j.begin(); it != j.end(); ++it) {
        if (it.key() == "index" && it->is_number_integer()) *it = it->get<int>() + tex0;
        else shiftTex(*it);
      }
    } else if (j.is_array()) {
      for (auto& x : j) shiftTex(x);
    }
  };
  for (json m : g.value("materials", json::array())) {
    shiftTex(m);
    G["materials"].push_back(m);
  }
  for (json m : g.value("meshes", json::array())) {
    for (auto& p : m["primitives"]) {
      for (auto& [k, v] : p["attributes"].items()) v = v.get<int>() + acc0;
      if (p.contains("indices")) p["indices"] = p["indices"].get<int>() + acc0;
      if (p.contains("material")) p["material"] = p["material"].get<int>() + mat0;
    }
    G["meshes"].push_back(m);
  }
  for (json n : g.value("nodes", json::array())) {
    if (n.contains("mesh")) n["mesh"] = n["mesh"].get<int>() + mesh0;
    if (n.contains("children"))
      for (auto& c : n["children"]) c = c.get<int>() + node0;
    if (n.contains("extensions") && n["extensions"].contains("MSFT_lod"))
      for (auto& id : n["extensions"]["MSFT_lod"]["ids"]) id = id.get<int>() + node0;
    G["nodes"].push_back(n);
  }
  for (auto& r : g["scenes"][0]["nodes"]) roots.push_back(r.get<int>() + node0);
  for (auto& e : g.value("extensionsUsed", json::array())) {
    auto& u = G["extensionsUsed"];
    if (std::find(u.begin(), u.end(), e) == u.end()) u.push_back(e);
  }
  return true;
}

// OBJ: `text` (one object's file, its indices from 1) after `out`, its indices moved past what is there
void mergeObj(std::string& out, const std::string& text, size_t& nv, size_t& nvt, size_t& nvn) {
  std::istringstream in(text);
  std::string line;
  size_t v = 0, vt = 0, vn = 0;
  while (std::getline(in, line)) {
    if (line.rfind("mtllib", 0) == 0 || line.rfind("# Patina", 0) == 0) continue;
    if (line.rfind("v ", 0) == 0) ++v;
    else if (line.rfind("vt ", 0) == 0) ++vt;
    else if (line.rfind("vn ", 0) == 0) ++vn;
    if (line.rfind("f ", 0) != 0) {
      out += line + "\n";
      continue;
    }
    std::istringstream cs(line.substr(2));
    std::string corner, f = "f";
    while (cs >> corner) {
      size_t a = corner.find('/'), b = a == std::string::npos ? std::string::npos : corner.find('/', a + 1);
      auto num = [&](size_t from, size_t to) { return std::stoull(corner.substr(from, to == std::string::npos ? std::string::npos : to - from)); };
      std::string c = std::to_string(num(0, a) + nv);
      if (a != std::string::npos) {
        std::string t = corner.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1);
        c += "/" + (t.empty() ? std::string() : std::to_string(std::stoull(t) + nvt));
        if (b != std::string::npos) c += "/" + std::to_string(num(b + 1, std::string::npos) + nvn);
      }
      f += " " + c;
    }
    out += f + "\n";
  }
  nv += v, nvt += vt, nvn += vn;
}

}  // namespace

bool Engine::exportSceneModel(const ModelExportOptions& opt, bool allObjects, json* out, std::string* err) {
  std::vector<int> objs;
  for (int i = 0; i < objectCount(); ++i)
    if (i == active_ || (allObjects && !object(i).objectHidden && !isolateActive)) objs.push_back(i);
  if (!allObjects || objs.size() < 2) {
    ModelExportOptions o = opt;
    o.transform = allObjects || doc.placed() ? doc.toWorld() : mat4(1);
    return doc.exportModel(o, out, err);
  }
  fs::path path = opt.path;
  std::string ext = path.extension().string();
  for (auto& c : ext) c = (char)std::tolower((unsigned char)c);
  if (ext != ".glb" && ext != ".gltf" && ext != ".obj" && ext != ".stl" && ext != ".ply")
    return failWith(err, "export_model: the file must end in .glb, .gltf, .obj, .stl or .ply");
  std::error_code ec;
  fs::path dir = path.parent_path().empty() ? fs::path(".") : path.parent_path();
  fs::create_directories(dir, ec);
  fs::path work = paths::cacheDir() / "scene_export";
  fs::remove_all(work, ec);
  const bool gltf = ext == ".glb" || ext == ".gltf";
  json G = {{"asset", {{"version", "2.0"}, {"generator", std::string("Patina ") + PATINA_VERSION}}},
            {"scene", 0},
            {"bufferViews", json::array()},
            {"accessors", json::array()},
            {"images", json::array()},
            {"textures", json::array()},
            {"samplers", json::array()},
            {"materials", json::array()},
            {"meshes", json::array()},
            {"nodes", json::array()},
            {"extensionsUsed", json::array()}};
  std::vector<uint8_t> B;
  std::vector<int> roots;
  std::string objText, mtlText, stlTris, plyVerts, plyFaces;
  size_t nv = 0, nvt = 0, nvn = 0, stlCount = 0, plyNV = 0, plyNF = 0;
  json parts = json::array();
  for (int i : objs) {
    Document& d = object(i);
    if (d.library) updateCompositeOf(i);
    std::string name = d.displayName();
    fs::path sub = paths::ensureDir(work / std::to_string(i));
    ModelExportOptions o = opt;
    o.transform = d.toWorld();
    o.materialPrefix = name + "_";
    o.path = (sub / (paths::sanitizeFileName(name) + (gltf ? ".gltf" : ext))).string();
    if (!gltf) o.lods.clear();  // (levels of detail: glTF only, for a scene)
    json r;
    if (!d.exportModel(o, &r, err)) return false;
    parts.push_back({{"object", name}, {"faces", d.mesh.faceCount()}});
    if (gltf) {
      json g = json::parse(paths::readFile(o.path), nullptr, false);
      if (g.is_discarded()) return failWith(err, "Could not read back " + o.path);
      fs::path binPath = fs::path(o.path).replace_extension(".bin");
      if (!mergeGltf(G, B, g, paths::readFile(binPath), sub, ext == ".glb", dir, name + "_", roots, err)) return false;
    } else if (ext == ".obj") {
      objText += "\n";
      mergeObj(objText, paths::readFile(o.path), nv, nvt, nvn);
      fs::path mtl = fs::path(o.path).replace_extension(".mtl");
      if (fs::exists(mtl, ec)) {
        // the textures copied next to the model as <object>_<file>, the material file pointing at them
        std::istringstream in(paths::readFile(mtl));
        std::string line;
        while (std::getline(in, line)) {
          if (line.rfind("# Patina", 0) == 0) continue;
          size_t sp = line.rfind(' ');
          bool map = line.rfind("map_", 0) == 0 || line.rfind("norm ", 0) == 0;
          if (map && sp != std::string::npos) {
            std::string file = line.substr(sp + 1), to = withPrefix(name + "_", file);
            fs::copy_file(sub / file, dir / to, fs::copy_options::overwrite_existing, ec);
            line = line.substr(0, sp + 1) + to;
          }
          mtlText += line + "\n";
        }
      }
    } else if (ext == ".stl") {
      std::string f = paths::readFile(o.path);
      if (f.size() < 84) return failWith(err, "Could not read back " + o.path);
      uint32_t n;
      std::memcpy(&n, f.data() + 80, 4);
      stlTris.append(f, 84, std::string::npos);
      stlCount += n;
    } else {
      // PLY as Document::exportModel writes it: 36 bytes a vertex, then the faces (a count, then int indices)
      std::string f = paths::readFile(o.path);
      size_t h = f.find("end_header\n");
      if (h == std::string::npos) return failWith(err, "Could not read back " + o.path);
      size_t verts = 0, faces = 0;
      std::istringstream hs(f.substr(0, h));
      std::string w1, w2;
      size_t n;
      std::string line;
      while (std::getline(hs, line)) {
        std::istringstream ls(line);
        if (ls >> w1 >> w2 >> n && w1 == "element") (w2 == "vertex" ? verts : faces) = n;
      }
      size_t at = h + 11;
      plyVerts.append(f, at, verts * 36);
      at += verts * 36;
      for (size_t k = 0; k < faces && at < f.size(); ++k) {
        uint8_t c = (uint8_t)f[at];
        plyFaces.push_back((char)c);
        for (int j = 0; j < c; ++j) {
          int32_t idx;
          std::memcpy(&idx, f.data() + at + 1 + 4 * j, 4);
          idx += (int32_t)plyNV;
          plyFaces.append((const char*)&idx, 4);
        }
        at += 1 + 4 * size_t(c);
      }
      plyNV += verts, plyNF += faces;
    }
  }
  bool ok = true;
  if (gltf) {
    while (B.size() % 4) B.push_back(0);
    G["scenes"] = {{{"nodes", roots}}};
    G["buffers"] = {{{"byteLength", B.size()}}};
    for (const char* k : {"images", "textures", "samplers", "extensionsUsed"})
      if (G[k].empty()) G.erase(k);
    if (ext == ".glb") {
      std::string js = G.dump();
      while (js.size() % 4) js += ' ';
      std::string file;
      auto u32 = [&](uint32_t v) { file.append((const char*)&v, 4); };
      u32(0x46546C67), u32(2), u32(uint32_t(12 + 8 + js.size() + 8 + B.size()));
      u32((uint32_t)js.size()), u32(0x4E4F534A), file += js;
      u32((uint32_t)B.size()), u32(0x004E4942), file.append((const char*)B.data(), B.size());
      ok = paths::writeFileAtomic(path, file);
    } else {
      fs::path binPath = path;
      binPath.replace_extension(".bin");
      G["buffers"][0]["uri"] = binPath.filename().string();
      ok = paths::writeFileAtomic(binPath, std::string((const char*)B.data(), B.size())) && paths::writeFileAtomic(path, G.dump(1));
    }
  } else if (ext == ".obj") {
    std::string head = std::string("# Patina ") + PATINA_VERSION + ": " + std::to_string(objs.size()) + " objects\n";
    fs::path mtl = path;
    mtl.replace_extension(".mtl");
    if (!mtlText.empty()) {
      head += "mtllib " + mtl.filename().string() + "\n";
      ok = paths::writeFileAtomic(mtl, std::string("# Patina ") + PATINA_VERSION + "\n" + mtlText);
    }
    ok = ok && paths::writeFileAtomic(path, head + objText);
  } else if (ext == ".stl") {
    std::string file(80, '\0');
    std::string head = std::string("Patina ") + PATINA_VERSION + " STL (mm, Z up)";
    file.replace(0, std::min<size_t>(head.size(), 80), head.substr(0, 80));
    uint32_t n = (uint32_t)stlCount;
    file.append((const char*)&n, 4);
    ok = paths::writeFileAtomic(path, file + stlTris);
  } else {
    std::string file = std::string("ply\nformat binary_little_endian 1.0\ncomment Patina ") + PATINA_VERSION +
                       ": Z up, meters; vertex colors are the base color (sRGB)\nelement vertex " + std::to_string(plyNV) +
                       "\nproperty float x\nproperty float y\nproperty float z\nproperty float nx\nproperty float ny\n"
                       "property float nz\nproperty float s\nproperty float t\nproperty uchar red\nproperty uchar green\n"
                       "property uchar blue\nproperty uchar alpha\nelement face " + std::to_string(plyNF) +
                       "\nproperty list uchar int vertex_indices\nend_header\n";
    ok = paths::writeFileAtomic(path, file + plyVerts + plyFaces);
  }
  fs::remove_all(work, ec);
  if (!ok) return failWith(err, "Could not write " + path.string());
  if (out) *out = {{"path", fs::absolute(path).string()}, {"format", ext.substr(1)}, {"objects", parts}};
  PLOG_I("Exported %zu objects to %s", objs.size(), path.string().c_str());
  return true;
}

bool Engine::exportSceneTextures(const ExportOptions& opt, bool allObjects, std::map<std::string, std::string>* files,
                                 json* perObject, std::string* err) {
  std::vector<int> objs;
  for (int i = 0; i < objectCount(); ++i)
    if (i == active_ || (allObjects && !object(i).objectHidden)) objs.push_back(i);
  json list = json::array();
  for (int i : objs) {
    Document& d = object(i);
    updateCompositeOf(i);
    ExportOptions o = opt;
    // (each object's files by its name: $mesh is the object's name, put first when the names don't have it)
    if (objs.size() > 1) {
      o.baseName = paths::sanitizeFileName(d.displayName());
      if (o.nameTemplate.find("$mesh") == std::string::npos) o.nameTemplate = "$mesh_" + o.nameTemplate;
      o.remember = o.remember && i == active_;
    }
    std::map<std::string, std::string> f;
    if (!d.exportTextures(o, &f, err)) return false;
    for (auto& [k, v] : f) (*files)[objs.size() > 1 ? d.displayName() + "/" + k : k] = v;
    list.push_back({{"object", d.displayName()}, {"files", f}});
  }
  if (perObject) *perObject = list;
  return true;
}

void Engine::updateCompositeOf(int i) {
  Document& d = object(i);
  materials.atlas = &d.mesh.tiles;
  bool wasAsleep = d.tex.asleep();
  d.updateComposite(materials);
  if (wasAsleep && i != active_) d.tex.sleep();
  materials.atlas = &doc.mesh.tiles;
}

}  // namespace patina
