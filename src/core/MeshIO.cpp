#include "core/MeshIO.h"

#include <cgltf.h>
#include <tiny_obj_loader.h>

#include <cstring>
#include <fstream>
#include <functional>
#include <nlohmann/json.hpp>
#include <unordered_map>

#include "core/Container.h"
#include "core/Log.h"
#include "core/Paths.h"

namespace patina {

using json = nlohmann::json;

static std::string extOf(const std::string& p) {
  auto d = p.find_last_of('.');
  std::string e = d == std::string::npos ? "" : p.substr(d + 1);
  for (auto& c : e) c = (char)tolower(c);
  return e;
}

static bool setErr(std::string* err, const std::string& msg) {
  if (err) *err = msg;
  PLOG_E("%s", msg.c_str());
  return false;
}

// Vertices at exactly the same position become one (glTF does this as it reads; FBX files made
// from glTF, such as AI-generated models, keep a vertex per UV seam side, which leaves the model
// in pieces for unwrapping, symmetry and parts). A face that would get a corner twice keeps its own.
static void weldPositions(Mesh& m) {
  std::unordered_map<std::string, uint32_t> first;
  std::vector<uint32_t> to(m.positions.size());
  for (uint32_t i = 0; i < m.positions.size(); ++i) {
    std::string key((const char*)&m.positions[i], sizeof(vec3));
    to[i] = first.emplace(std::move(key), i).first->second;
  }
  size_t merged = 0;
  for (uint32_t f = 0; f + 1 < m.faceStart.size(); ++f) {
    bool repeat = false;
    for (uint32_t a = m.faceStart[f]; a < m.faceStart[f + 1] && !repeat; ++a)
      for (uint32_t b = a + 1; b < m.faceStart[f + 1]; ++b)
        if (to[m.loopVert[a]] == to[m.loopVert[b]] && m.loopVert[a] != m.loopVert[b]) repeat = true;
    if (repeat) continue;
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) {
      merged += to[m.loopVert[l]] != m.loopVert[l];
      m.loopVert[l] = to[m.loopVert[l]];
    }
  }
  if (!merged) return;
  std::vector<uint32_t> keep(m.positions.size(), UINT32_MAX);
  std::vector<vec3> pos;
  for (auto& v : m.loopVert) {
    if (keep[v] == UINT32_MAX) keep[v] = (uint32_t)pos.size(), pos.push_back(m.positions[v]);
    v = keep[v];
  }
  size_t before = m.positions.size();
  m.positions = std::move(pos);
  m.edges.clear();
  m.edgeFlags.clear();
  PLOG_I("Welded %zu vertices at the same positions into %zu", before, m.positions.size());
}

bool loadMesh(const std::string& path, Mesh& out, std::string* err, const MeshImportOptions& opt) {
  std::string e = extOf(path);
  bool ok = false;
  if (e == "obj") ok = loadObj(path, out, err);
  else if (e == "gltf" || e == "glb") ok = loadGltf(path, out, err, opt);
  else if (e == "ptmesh") ok = loadPtMesh(path, out, err);
  else if (e == "fbx") ok = loadFbx(path, out, err);
  else if (e == "stl") ok = loadStl(path, out, err);
  else if (e == "3mf") ok = load3mf(path, out, err);
  else if (e == "ply") ok = loadPly(path, out, err);
  else return setErr(err, "Unsupported mesh format: " + path);
  if (!ok) return false;
  if (opt.weldVertices && e == "fbx") weldPositions(out);
  if (opt.scale != 1.f)
    for (auto& p : out.positions) p *= opt.scale;
  std::string verr;
  if (!out.validate(&verr)) return setErr(err, "Invalid mesh: " + verr);
  // (one named mesh: its name stays, for the object it becomes; no faces to tell apart)
  if (out.faceObject.size() != out.faceCount() || out.objectNames.size() < 2) out.faceObject.clear();
  if (out.faceObject.empty() && out.objectNames.size() > 1) out.objectNames.clear();
  out.finalize();
  PLOG_I("Loaded mesh '%s': %zu verts, %u faces, %zu tris, %u UV islands", out.name.c_str(),
         out.positions.size(), out.faceCount(), out.tris.size(), out.islandCount);
  return true;
}

void splitObjects(const Mesh& m, std::vector<Mesh>& parts, std::vector<std::string>& names) {
  parts.clear(), names.clear();
  const uint32_t F = m.faceCount(), K = (uint32_t)m.objectNames.size();
  if (m.faceObject.size() != F || K < 2) return;
  std::vector<std::vector<uint32_t>> faces(K);
  for (uint32_t f = 0; f < F; ++f)
    if (m.faceObject[f] < K) faces[m.faceObject[f]].push_back(f);
  std::vector<uint32_t> remap(m.positions.size(), ~0u);
  for (uint32_t k = 0; k < K; ++k) {
    if (faces[k].empty()) continue;
    Mesh p;
    p.name = m.objectNames[k];
    p.materialNames = m.materialNames;
    p.hasSourceNormals = m.hasSourceNormals;
    p.autoSmoothAngleDeg = m.autoSmoothAngleDeg;
    p.faceStart = {0};
    std::vector<uint32_t> used;
    const bool colors = m.loopColor.size() == m.loopCount(), uv2 = m.loopUV2.size() == m.loopCount();
    for (uint32_t f : faces[k]) {
      for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) {
        uint32_t v = m.loopVert[l];
        if (remap[v] == ~0u) remap[v] = (uint32_t)p.positions.size(), p.positions.push_back(m.positions[v]), used.push_back(v);
        p.loopVert.push_back(remap[v]);
        p.loopUV.push_back(m.loopUV[l]);
        if (m.hasSourceNormals) p.loopNormal.push_back(m.loopNormal[l]);
        if (colors) p.loopColor.push_back(m.loopColor[l]);
        if (uv2) p.loopUV2.push_back(m.loopUV2[l]);
      }
      p.faceStart.push_back((uint32_t)p.loopVert.size());
      p.faceMaterial.push_back(f < m.faceMaterial.size() ? m.faceMaterial[f] : 0);
      if (f < m.faceFlags.size()) p.faceFlags.push_back(m.faceFlags[f]);
    }
    if (p.faceFlags.size() != p.faceCount()) p.faceFlags.clear();
    for (uint32_t v : used) remap[v] = ~0u;
    p.finalize();
    parts.push_back(std::move(p));
    names.push_back(m.objectNames[k]);
  }
}

const std::vector<std::string>& meshExtensions() {
  static const std::vector<std::string> e = {"obj", "gltf", "glb", "fbx", "stl", "3mf", "ply", "ptmesh"};
  return e;
}

// ------------------------------------------------------------------ OBJ

bool loadObj(const std::string& path, Mesh& m, std::string* err) {
  tinyobj::ObjReaderConfig cfg;
  cfg.triangulate = false;
  cfg.vertex_color = true;
  tinyobj::ObjReader r;
  if (!r.ParseFromFile(path, cfg)) return setErr(err, "OBJ: " + r.Error());
  if (!r.Warning().empty()) PLOG_W("OBJ: %s", r.Warning().c_str());
  const auto& a = r.GetAttrib();
  // "v x y z r g b": sRGB, as Blender and MeshLab read them (white when a vertex has none)
  bool colors = a.colors.size() == a.vertices.size() &&
                std::any_of(a.colors.begin(), a.colors.end(), [](tinyobj::real_t c) { return c < 0.999f; });
  m = Mesh();
  auto slash = path.find_last_of('/');
  m.name = path.substr(slash == std::string::npos ? 0 : slash + 1);
  m.name = m.name.substr(0, m.name.find_last_of('.'));
  m.positions.resize(a.vertices.size() / 3);
  for (size_t i = 0; i < m.positions.size(); ++i)
    m.positions[i] = {a.vertices[i * 3], a.vertices[i * 3 + 1], a.vertices[i * 3 + 2]};
  bool normals = !a.normals.empty();
  m.faceStart = {0};
  for (auto& sh : r.GetShapes()) {
    size_t off = 0;
    uint32_t obj = (uint32_t)m.objectNames.size();
    m.objectNames.push_back(sh.name.empty() ? "Object " + std::to_string(obj + 1) : sh.name);
    for (size_t f = 0; f < sh.mesh.num_face_vertices.size(); ++f) {
      int n = sh.mesh.num_face_vertices[f];
      for (int k = 0; k < n; ++k) {
        auto idx = sh.mesh.indices[off + k];
        m.loopVert.push_back((uint32_t)idx.vertex_index);
        if (colors) {
          const tinyobj::real_t* c = &a.colors[size_t(idx.vertex_index) * 3];
          m.loopColor.push_back(vec4(srgbToLinear((float)c[0]), srgbToLinear((float)c[1]), srgbToLinear((float)c[2]), 1.f));
        }
        vec2 uv(0);
        if (idx.texcoord_index >= 0)
          uv = {a.texcoords[idx.texcoord_index * 2], a.texcoords[idx.texcoord_index * 2 + 1]};
        m.loopUV.push_back(uv);
        if (idx.normal_index >= 0)
          m.loopNormal.push_back(glm::normalize(vec3(a.normals[idx.normal_index * 3], a.normals[idx.normal_index * 3 + 1],
                                                     a.normals[idx.normal_index * 3 + 2])));
        else
          normals = false;
      }
      off += n;
      m.faceStart.push_back((uint32_t)m.loopVert.size());
      int mat = f < sh.mesh.material_ids.size() ? sh.mesh.material_ids[f] : 0;
      m.faceMaterial.push_back(mat < 0 ? 0 : (uint32_t)mat);
      m.faceObject.push_back(obj);
    }
  }
  for (auto& mt : r.GetMaterials()) m.materialNames.push_back(mt.name);
  m.hasSourceNormals = normals && m.loopNormal.size() == m.loopVert.size();
  if (!m.hasSourceNormals) m.loopNormal.clear();
  return true;
}

bool saveObj(const std::string& path, const Mesh& m, std::string* err) {
  std::ofstream f(path);
  if (!f) return setErr(err, "Cannot write " + path);
  f << "# Patina export\no " << m.name << "\n";
  for (auto& p : m.positions) f << "v " << p.x << ' ' << p.y << ' ' << p.z << '\n';
  for (auto& t : m.loopUV) f << "vt " << t.x << ' ' << t.y << '\n';
  for (auto& n : m.loopNormal) f << "vn " << n.x << ' ' << n.y << ' ' << n.z << '\n';
  bool hasN = m.loopNormal.size() == m.loopVert.size();
  for (uint32_t fc = 0; fc < m.faceCount(); ++fc) {
    f << 'f';
    for (uint32_t l = m.faceStart[fc]; l < m.faceStart[fc + 1]; ++l) {
      f << ' ' << m.loopVert[l] + 1 << '/' << l + 1;
      if (hasN) f << '/' << l + 1;
    }
    f << '\n';
  }
  return f.good() || setErr(err, "Write failed: " + path);
}

// ------------------------------------------------------------------ glTF

bool loadGltf(const std::string& path, Mesh& m, std::string* err, const MeshImportOptions& opt) {
  cgltf_options o{};
  cgltf_data* data = nullptr;
  if (cgltf_parse_file(&o, path.c_str(), &data) != cgltf_result_success) return setErr(err, "glTF parse failed: " + path);
  std::unique_ptr<cgltf_data, void (*)(cgltf_data*)> guard(data, cgltf_free);
  if (cgltf_load_buffers(&o, data, path.c_str()) != cgltf_result_success)
    return setErr(err, "glTF buffer load failed: " + path);

  m = Mesh();
  auto slash = path.find_last_of('/');
  m.name = path.substr(slash == std::string::npos ? 0 : slash + 1);
  m.name = m.name.substr(0, m.name.find_last_of('.'));
  m.faceStart = {0};
  bool allNormals = true, anyColor = false, anyUV2 = false;
  std::unordered_map<std::string, uint32_t> weld;

  uint32_t object = 0;  // (1.2) the node's index among the meshes (Mesh::faceObject)
  auto addPrim = [&](const cgltf_primitive& prim, const mat4& xf) {
    if (prim.type != cgltf_primitive_type_triangles) return;
    const cgltf_accessor *pos = nullptr, *nrm = nullptr, *uv = nullptr, *col = nullptr, *uv1 = nullptr;
    for (size_t i = 0; i < prim.attributes_count; ++i) {
      auto& at = prim.attributes[i];
      if (at.type == cgltf_attribute_type_position) pos = at.data;
      else if (at.type == cgltf_attribute_type_normal) nrm = at.data;
      else if (at.type == cgltf_attribute_type_texcoord && at.index == 0) uv = at.data;
      else if (at.type == cgltf_attribute_type_color && at.index == 0) col = at.data;
      else if (at.type == cgltf_attribute_type_texcoord && at.index == 1) uv1 = at.data;
    }
    if (uv1 && !anyUV2) {
      anyUV2 = true;
      m.loopUV2.assign(m.loopVert.size(), vec2(0.f));  // the primitives before this one: none
    }
    if (col && !anyColor) {
      anyColor = true;
      m.loopColor.assign(m.loopVert.size(), vec4(1.f));  // the primitives before this one: white
    }
    if (!pos) return;
    if (!nrm) allNormals = false;
    mat3 nxf = glm::transpose(glm::inverse(mat3(xf)));
    std::vector<uint32_t> vmap(pos->count);
    for (size_t i = 0; i < pos->count; ++i) {
      vec3 p;
      cgltf_accessor_read_float(pos, i, &p.x, 3);
      p = vec3(xf * vec4(p, 1));
      if (opt.weldVertices) {
        std::string key((const char*)&p, sizeof p);
        auto it = weld.find(key);
        if (it != weld.end()) {
          vmap[i] = it->second;
          continue;
        }
        weld.emplace(key, (uint32_t)m.positions.size());
      }
      vmap[i] = (uint32_t)m.positions.size();
      m.positions.push_back(p);
    }
    size_t count = prim.indices ? prim.indices->count : pos->count;
    uint32_t matId = prim.material ? uint32_t(prim.material - data->materials) : 0;
    for (size_t i = 0; i + 2 < count; i += 3) {
      for (int k = 0; k < 3; ++k) {
        size_t vi = prim.indices ? cgltf_accessor_read_index(prim.indices, i + k) : i + k;
        m.loopVert.push_back(vmap[vi]);
        vec2 t(0);
        if (uv) {
          cgltf_accessor_read_float(uv, vi, &t.x, 2);
          t.y = 1.f - t.y;  // glTF UV origin is top-left
        }
        m.loopUV.push_back(t);
        vec3 n(0, 1, 0);
        if (nrm) {
          cgltf_accessor_read_float(nrm, vi, &n.x, 3);
          n = glm::normalize(nxf * n);
        }
        m.loopNormal.push_back(n);
        if (anyUV2) {
          vec2 t2(0.f);
          if (uv1) {
            cgltf_accessor_read_float(uv1, vi, &t2.x, 2);
            t2.y = 1.f - t2.y;
          }
          m.loopUV2.push_back(t2);
        }
        if (anyColor) {
          vec4 c(1.f);  // glTF vertex colors are linear
          if (col) cgltf_accessor_read_float(col, vi, &c.x, col->type == cgltf_type_vec4 ? 4 : 3);
          m.loopColor.push_back(c);
        }
      }
      m.faceStart.push_back((uint32_t)m.loopVert.size());
      m.faceMaterial.push_back(matId);
      m.faceObject.push_back(object);
    }
  };

  std::function<void(const cgltf_node*)> visit = [&](const cgltf_node* n) {
    mat4 world;
    cgltf_node_transform_world(n, glm::value_ptr(world));
    if (n->mesh) {
      object = (uint32_t)m.objectNames.size();
      m.objectNames.push_back(n->name && *n->name ? n->name : n->mesh->name && *n->mesh->name ? n->mesh->name
                                                                                             : "Mesh " + std::to_string(object + 1));
      for (size_t i = 0; i < n->mesh->primitives_count; ++i) addPrim(n->mesh->primitives[i], world);
    }
    for (size_t i = 0; i < n->children_count; ++i) visit(n->children[i]);
  };
  const cgltf_scene* scene = data->scene ? data->scene : (data->scenes_count ? &data->scenes[0] : nullptr);
  if (scene) {
    for (size_t i = 0; i < scene->nodes_count; ++i) visit(scene->nodes[i]);
  } else {
    for (size_t i = 0; i < data->meshes_count; ++i) {
      object = (uint32_t)m.objectNames.size();
      m.objectNames.push_back(data->meshes[i].name && *data->meshes[i].name ? data->meshes[i].name
                                                                          : "Mesh " + std::to_string(object + 1));
      for (size_t p = 0; p < data->meshes[i].primitives_count; ++p) addPrim(data->meshes[i].primitives[p], mat4(1));
    }
  }
  if (m.loopVert.empty()) return setErr(err, "glTF contains no triangle meshes: " + path);
  for (size_t i = 0; i < data->materials_count; ++i)
    m.materialNames.push_back(data->materials[i].name ? data->materials[i].name : "");
  m.hasSourceNormals = allNormals;
  if (!allNormals) m.loopNormal.clear();
  return true;
}

// ------------------------------------------------------------------ Patina bridge format

namespace {
// Blender (Z up) <-> Patina (Y up)
vec3 fromBlender(const vec3& p) { return {p.x, p.z, -p.y}; }
vec3 toBlender(const vec3& p) { return {p.x, -p.z, p.y}; }
}  // namespace

bool decodePtMesh(const container::File& file, Mesh& m, std::string* err) {
  const json& h = file.header;
  size_t V, F, L, E;
  try {
    V = h.at("vertex_count");
    F = h.at("face_count");
    L = h.at("loop_count");
    E = h.value("edge_count", 0);
  } catch (std::exception& e) {
    return setErr(err, std::string("ptmesh header: ") + e.what());
  }
  m = Mesh();
  m.name = h.value("name", std::string("mesh"));
  bool zUp = h.value("up", std::string("Z")) == "Z";
  std::vector<float> pos, uvs, nrm;
  std::vector<uint32_t> fsz, lv, edges, fmat;
  std::vector<uint8_t> seams, sharp;
  if (!file.read("positions", pos, V * 3)) return setErr(err, "ptmesh: missing/invalid positions");
  if (!file.read("face_sizes", fsz, F)) return setErr(err, "ptmesh: missing/invalid face_sizes");
  if (!file.read("loop_verts", lv, L)) return setErr(err, "ptmesh: missing/invalid loop_verts");
  m.positions.resize(V);
  for (size_t i = 0; i < V; ++i) {
    vec3 p(pos[i * 3], pos[i * 3 + 1], pos[i * 3 + 2]);
    m.positions[i] = zUp ? fromBlender(p) : p;
  }
  m.faceStart.resize(F + 1);
  m.faceStart[0] = 0;
  for (size_t f = 0; f < F; ++f) m.faceStart[f + 1] = m.faceStart[f] + fsz[f];
  if (m.faceStart[F] != L) return setErr(err, "ptmesh: face_sizes do not sum to loop_count");
  m.loopVert = std::move(lv);
  m.loopUV.assign(L, vec2(0));
  if (file.read("loop_uvs", uvs, L * 2) && h.value("had_uvs", true))
    for (size_t l = 0; l < L; ++l) m.loopUV[l] = {uvs[l * 2], uvs[l * 2 + 1]};
  std::vector<float> uv2;
  if (file.read("loop_uvs2", uv2, L * 2)) {  // 0.35: a second UV map (lightmaps)
    m.loopUV2.resize(L);
    for (size_t l = 0; l < L; ++l) m.loopUV2[l] = {uv2[l * 2], uv2[l * 2 + 1]};
  }
  std::vector<float> cols;
  if (file.read("loop_colors", cols, L * 4)) {  // add-on 0.10: the active color attribute (linear)
    m.loopColor.resize(L);
    for (size_t l = 0; l < L; ++l) m.loopColor[l] = vec4(cols[l * 4], cols[l * 4 + 1], cols[l * 4 + 2], cols[l * 4 + 3]);
  }
  if (file.read("loop_normals", nrm, L * 3)) {
    m.loopNormal.resize(L);
    for (size_t l = 0; l < L; ++l) {
      vec3 n(nrm[l * 3], nrm[l * 3 + 1], nrm[l * 3 + 2]);
      m.loopNormal[l] = zUp ? fromBlender(n) : n;
    }
    m.hasSourceNormals = true;
  }
  if (E && file.read("edges", edges, E * 2)) {
    m.edges.resize(E);
    for (size_t e = 0; e < E; ++e) m.edges[e] = {edges[e * 2], edges[e * 2 + 1]};
    m.edgeFlags.assign(E, 0);
    if (file.read("edge_seams", seams, E))
      for (size_t e = 0; e < E; ++e) m.edgeFlags[e] |= seams[e] ? EdgeSeam : 0;
    if (file.read("edge_sharp", sharp, E))
      for (size_t e = 0; e < E; ++e) m.edgeFlags[e] |= sharp[e] ? EdgeSharp : 0;
  }
  if (file.read("face_material", fmat, F)) m.faceMaterial = fmat;
  for (auto& n : h.value("material_names", json::array()))
    if (n.is_string()) m.materialNames.push_back(n.get<std::string>());
  std::vector<uint8_t> fflags;
  if (file.read("face_flags", fflags, F)) m.faceFlags = fflags;
  std::vector<uint32_t> fsets;
  if (file.read("face_sets", fsets, F)) m.faceSetIds = std::move(fsets);  // (1.2) Blender's sculpt face sets
  // optional (0.11): vertex groups, names in the header, sizes + vertex indices in two blocks
  json gnames = h.value("vertex_groups", json::array());
  std::vector<uint32_t> gsz, gverts;
  if (gnames.is_array() && !gnames.empty() && file.read("group_sizes", gsz, gnames.size())) {
    size_t total = 0;
    for (uint32_t n : gsz) total += n;
    if (file.read("group_verts", gverts, total)) {
      size_t at = 0;
      for (size_t g = 0; g < gsz.size(); ++g) {
        VertexGroup vg;
        vg.name = gnames[g].is_string() ? gnames[g].get<std::string>() : "Group " + std::to_string(g + 1);
        for (uint32_t k = 0; k < gsz[g]; ++k, ++at)
          if (gverts[at] < V) vg.verts.push_back(gverts[at]);
        m.vertexGroups.push_back(std::move(vg));
      }
    }
  }
  return true;
}

bool hadUVs(const std::string& path) {
  container::File f;
  if (!container::readFile(path, "PTMESH01", f, nullptr)) return true;
  return f.header.value("had_uvs", true);
}

bool loadPtMesh(const std::string& path, Mesh& m, std::string* err) {
  container::File f;
  std::string e;
  if (!container::readFile(path, "PTMESH01", f, &e)) return setErr(err, path + ": " + e);
  return decodePtMesh(f, m, err);
}

std::string encodePtMesh(const Mesh& m, bool blenderAxes) {
  std::vector<float> pos;
  pos.reserve(m.positions.size() * 3);
  for (auto& p : m.positions) {
    vec3 b = blenderAxes ? toBlender(p) : p;
    pos.insert(pos.end(), {b.x, b.y, b.z});
  }
  std::vector<uint32_t> fsz(m.faceCount());
  for (uint32_t i = 0; i < m.faceCount(); ++i) fsz[i] = m.faceSize(i);
  std::vector<uint32_t> edges;
  std::vector<uint8_t> seams, sharp;
  for (size_t e = 0; e < m.edges.size(); ++e) {
    edges.push_back(m.edges[e].x);
    edges.push_back(m.edges[e].y);
    seams.push_back(m.edgeFlags[e] & EdgeSeam ? 1 : 0);
    sharp.push_back(m.edgeFlags[e] & EdgeSharp ? 1 : 0);
  }
  std::vector<container::Block> blocks = {
      container::Block::of("positions", "f32", pos.data(), pos.size()),
      container::Block::of("face_sizes", "u32", fsz.data(), fsz.size()),
      container::Block::of("loop_verts", "u32", m.loopVert.data(), m.loopVert.size()),
      container::Block::of("loop_uvs", "f32", (const float*)m.loopUV.data(), m.loopUV.size() * 2),
      container::Block::of("edges", "u32", edges.data(), edges.size()),
      container::Block::of("edge_seams", "u8", seams.data(), seams.size()),
      container::Block::of("edge_sharp", "u8", sharp.data(), sharp.size()),
      container::Block::of("face_material", "u32", m.faceMaterial.data(), m.faceMaterial.size()),
  };
  if (m.faceFlags.size() == m.faceCount())
    blocks.push_back(container::Block::of("face_flags", "u8", m.faceFlags.data(), m.faceFlags.size()));
  if (m.loopUV2.size() == m.loopVert.size())  // 0.35: the lightmap UVs
    blocks.push_back(container::Block::of("loop_uvs2", "f32", (const float*)m.loopUV2.data(), m.loopUV2.size() * 2));
  if (m.loopColor.size() == m.loopVert.size())  // 0.32: vertex colors (linear)
    blocks.push_back(container::Block::of("loop_colors", "f32", (const float*)m.loopColor.data(), m.loopColor.size() * 4));
  std::vector<uint32_t> gsz, gverts;
  json gnames = json::array();
  for (auto& g : m.vertexGroups) {
    gnames.push_back(g.name);
    gsz.push_back((uint32_t)g.verts.size());
    gverts.insert(gverts.end(), g.verts.begin(), g.verts.end());
  }
  if (!m.vertexGroups.empty()) {
    blocks.push_back(container::Block::of("group_sizes", "u32", gsz.data(), gsz.size()));
    blocks.push_back(container::Block::of("group_verts", "u32", gverts.data(), gverts.size()));
  }
  if (m.hasSourceNormals && m.loopNormal.size() == m.loopVert.size()) {
    std::vector<float> n;
    for (auto& v : m.loopNormal) {
      vec3 b = blenderAxes ? toBlender(v) : v;
      n.insert(n.end(), {b.x, b.y, b.z});
    }
    blocks.push_back(container::Block::of("loop_normals", "f32", n.data(), n.size()));
  }
  json h = {{"version", 1},
            {"name", m.name},
            {"up", blenderAxes ? "Z" : "Y"},
            {"vertex_count", m.positions.size()},
            {"face_count", m.faceCount()},
            {"loop_count", m.loopCount()},
            {"edge_count", m.edges.size()}};
  if (!m.materialNames.empty()) h["material_names"] = m.materialNames;
  if (!m.vertexGroups.empty()) h["vertex_groups"] = gnames;
  return container::encode("PTMESH01", h, blocks);
}

bool savePtMesh(const std::string& path, const Mesh& m, std::string* err) {
  if (!paths::writeFileAtomic(path, encodePtMesh(m, true))) return setErr(err, "Cannot write " + path);
  return true;
}

bool savePtUV(const std::string& path, const Mesh& m, std::string* err, bool withSlots) {
  std::vector<uint8_t> seams(m.edges.size());
  for (size_t e = 0; e < m.edges.size(); ++e) seams[e] = m.edgeFlags[e] & EdgeSeam ? 1 : 0;
  std::vector<container::Block> blocks = {
      container::Block::of("loop_uvs", "f32", (const float*)m.loopUV.data(), m.loopUV.size() * 2),
      container::Block::of("edge_seams", "u8", seams.data(), seams.size()),
  };
  json h = {{"version", 1}, {"name", m.name}, {"loop_count", m.loopCount()}, {"edge_count", m.edges.size()}};
  if (withSlots) {
    std::vector<uint32_t> slots(m.faceCount(), 0);
    uint32_t n = std::max<uint32_t>(1, (uint32_t)m.materialNames.size());
    for (uint32_t f = 0; f < m.faceCount(); ++f) {
      slots[f] = f < m.faceMaterial.size() ? m.faceMaterial[f] : 0;
      n = std::max(n, slots[f] + 1);
    }
    json names = json::array();
    for (uint32_t i = 0; i < n; ++i) names.push_back(m.slotName(i));
    blocks.push_back(container::Block::of("face_slots", "u32", slots.data(), slots.size()));
    h["face_count"] = m.faceCount();
    h["slot_names"] = names;
  }
  std::string e;
  if (!container::writeFile(path, "PTUV0001", h, blocks, &e)) return setErr(err, e);
  return true;
}

bool loadPtUV(const std::string& path, Mesh& m, std::string* err) {
  container::File f;
  std::string e;
  if (!container::readFile(path, "PTUV0001", f, &e)) return setErr(err, path + ": " + e);
  size_t L = f.header.value("loop_count", 0), E = f.header.value("edge_count", 0);
  if (L != m.loopCount()) return setErr(err, "UV file loop count does not match the mesh");
  std::vector<float> uvs;
  if (!f.read("loop_uvs", uvs, L * 2)) return setErr(err, "ptuv: missing loop_uvs");
  for (size_t l = 0; l < L; ++l) m.loopUV[l] = {uvs[l * 2], uvs[l * 2 + 1]};
  std::vector<uint8_t> seams;
  if (E == m.edges.size() && f.read("edge_seams", seams, E))
    for (size_t i = 0; i < E; ++i) m.edgeFlags[i] = (m.edgeFlags[i] & ~EdgeSeam) | (seams[i] ? EdgeSeam : 0);
  return true;
}

}  // namespace patina
