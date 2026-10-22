// FBX (ufbx), STL and 3MF import. All three come in as Patina meshes in meters, Y up.
// STL and 3MF carry no UVs as a rule; Document::setMesh unwraps meshes without usable UVs.
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <array>
#include <functional>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <unordered_map>

#include <miniz.h>
#include <ufbx.h>

#include "core/Log.h"
#include "core/MeshIO.h"
#include "core/Paths.h"

namespace patina {

static bool fail(std::string* err, const std::string& msg) {
  if (err) *err = msg;
  PLOG_E("%s", msg.c_str());
  return false;
}

static std::string stemOf(const std::string& path) {
  auto slash = path.find_last_of("/\\");
  std::string n = path.substr(slash == std::string::npos ? 0 : slash + 1);
  return n.substr(0, n.find_last_of('.'));
}

// Merges identical positions (STL and 3MF triangles) so faces share vertices.
struct Welder {
  std::unordered_map<std::string, uint32_t> map;
  uint32_t add(Mesh& m, const vec3& p) {
    std::string key((const char*)&p, sizeof p);
    auto it = map.find(key);
    if (it != map.end()) return it->second;
    uint32_t i = (uint32_t)m.positions.size();
    m.positions.push_back(p);
    map.emplace(std::move(key), i);
    return i;
  }
};

// 3D printing files are Z up in millimeters: to Patina's Y up, meters
static vec3 fromPrintSpace(const vec3& p, float toMeters) { return vec3(p.x, p.z, -p.y) * toMeters; }

// ------------------------------------------------------------------ FBX

bool loadFbx(const std::string& path, Mesh& m, std::string* err) {
  ufbx_load_opts opts = {};
  opts.target_axes = ufbx_axes_right_handed_y_up;
  opts.target_unit_meters = 1.0f;
  opts.space_conversion = UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY;
  opts.geometry_transform_handling = UFBX_GEOMETRY_TRANSFORM_HANDLING_MODIFY_GEOMETRY;
  opts.ignore_animation = true;
  opts.ignore_embedded = true;
  opts.load_external_files = false;
  ufbx_error error;
  ufbx_scene* scene = ufbx_load_file(path.c_str(), &opts, &error);
  if (!scene) {
    char buf[1024];
    ufbx_format_error(buf, sizeof buf, &error);
    return fail(err, std::string("FBX: ") + buf);
  }
  std::unique_ptr<ufbx_scene, void (*)(ufbx_scene*)> guard(scene, ufbx_free_scene);
  m = Mesh();
  m.name = stemOf(path);
  m.faceStart = {0};
  bool allNormals = true, anyUV = false, anyColor = false;
  size_t meshNodes = 0;
  for (size_t ni = 0; ni < scene->nodes.count; ++ni) {
    ufbx_node* node = scene->nodes.data[ni];
    ufbx_mesh* mesh = node->mesh;
    if (!mesh || mesh->num_faces == 0) continue;
    ++meshNodes;
    if (meshNodes == 1 && node->name.length) m.name = scene->nodes.count > 2 ? m.name : std::string(node->name.data);
    const uint32_t object = (uint32_t)m.objectNames.size();  // (1.2) Mesh::faceObject
    m.objectNames.push_back(node->name.length ? std::string(node->name.data, node->name.length)
                                              : "Mesh " + std::to_string(object + 1));
    const ufbx_matrix& xf = node->geometry_to_world;
    ufbx_matrix nxf = ufbx_matrix_for_normals(&xf);
    uint32_t base = (uint32_t)m.positions.size();
    for (size_t v = 0; v < mesh->vertices.count; ++v) {
      ufbx_vec3 p = ufbx_transform_position(&xf, mesh->vertices.data[v]);
      m.positions.push_back(vec3((float)p.x, (float)p.y, (float)p.z));
    }
    bool hasN = mesh->vertex_normal.exists, hasUV = mesh->vertex_uv.exists, hasC = mesh->vertex_color.exists;
    if (!hasN) allNormals = false;
    anyUV |= hasUV;
    if (hasC && !anyColor) {
      anyColor = true;
      m.loopColor.assign(m.loopVert.size(), vec4(1.f));  // the meshes before this one: white
    }
    // a mirrored transform flips the winding
    ufbx_vec3 c0{xf.m00, xf.m10, xf.m20}, c1{xf.m01, xf.m11, xf.m21}, c2{xf.m02, xf.m12, xf.m22};
    double det = c0.x * (c1.y * c2.z - c2.y * c1.z) - c1.x * (c0.y * c2.z - c2.y * c0.z) + c2.x * (c0.y * c1.z - c1.y * c0.z);
    bool flip = det < 0;
    for (size_t f = 0; f < mesh->faces.count; ++f) {
      ufbx_face face = mesh->faces.data[f];
      if (face.num_indices < 3) continue;
      for (uint32_t k = 0; k < face.num_indices; ++k) {
        uint32_t c = flip ? face.index_begin + (face.num_indices - k) % face.num_indices : face.index_begin + k;
        m.loopVert.push_back(base + mesh->vertex_indices.data[c]);
        vec2 uv(0.f);
        if (hasUV) {
          ufbx_vec2 t = ufbx_get_vertex_vec2(&mesh->vertex_uv, c);
          uv = vec2((float)t.x, (float)t.y);
        }
        m.loopUV.push_back(uv);
        vec3 n(0, 1, 0);
        if (hasN) {
          ufbx_vec3 nn = ufbx_transform_direction(&nxf, ufbx_get_vertex_vec3(&mesh->vertex_normal, c));
          n = vec3((float)nn.x, (float)nn.y, (float)nn.z);
          float l = glm::length(n);
          n = l > 1e-12f ? n / l : vec3(0, 1, 0);
        }
        m.loopNormal.push_back(n);
        if (anyColor) {
          vec4 col(1.f);  // sRGB, as Blender writes and reads FBX colors by default
          if (hasC) {
            ufbx_vec4 cc = ufbx_get_vertex_vec4(&mesh->vertex_color, c);
            col = vec4(srgbToLinear((float)cc.x), srgbToLinear((float)cc.y), srgbToLinear((float)cc.z), (float)cc.w);
          }
          m.loopColor.push_back(col);
        }
      }
      m.faceStart.push_back((uint32_t)m.loopVert.size());
      uint32_t slot = 0;
      if (mesh->face_material.count > f) {
        uint32_t mi = mesh->face_material.data[f];
        const ufbx_material* mat = mi < node->materials.count ? node->materials.data[mi]
                                 : mi < mesh->materials.count ? mesh->materials.data[mi] : nullptr;
        if (mat) slot = mat->typed_id;
      }
      m.faceMaterial.push_back(slot);
      m.faceObject.push_back(object);
    }
  }
  if (m.loopVert.empty()) return fail(err, "FBX contains no polygon meshes: " + path);
  for (size_t i = 0; i < scene->materials.count; ++i)
    m.materialNames.push_back(std::string(scene->materials.data[i]->name.data, scene->materials.data[i]->name.length));
  m.hasSourceNormals = allNormals;
  if (!allNormals) m.loopNormal.clear();
  PLOG_I("FBX: %zu mesh node(s), %s, originally %s up, %.4g m per unit", meshNodes, anyUV ? "with UVs" : "no UVs",
         scene->settings.axes.up == UFBX_COORDINATE_AXIS_POSITIVE_Z ? "Z" : "Y", (double)scene->settings.unit_meters);
  return true;
}

// ------------------------------------------------------------------ STL

bool loadStl(const std::string& path, Mesh& m, std::string* err) {
  bool ok = false;
  std::string data = paths::readFile(path, &ok);
  if (!ok) return fail(err, "Cannot read " + path);
  m = Mesh();
  m.name = stemOf(path);
  m.faceStart = {0};
  Welder weld;
  const float mm = 0.001f;
  auto addTri = [&](const vec3& a, const vec3& b, const vec3& c) {
    vec3 p[3] = {fromPrintSpace(a, mm), fromPrintSpace(b, mm), fromPrintSpace(c, mm)};
    uint32_t i0 = weld.add(m, p[0]), i1 = weld.add(m, p[1]), i2 = weld.add(m, p[2]);
    if (i0 == i1 || i1 == i2 || i0 == i2) return;  // degenerate
    m.loopVert.insert(m.loopVert.end(), {i0, i1, i2});
    m.loopUV.insert(m.loopUV.end(), 3, vec2(0.f));
    m.faceStart.push_back((uint32_t)m.loopVert.size());
    m.faceMaterial.push_back(0);
  };
  // binary: 80 byte header, uint32 count, 50 bytes per triangle (some binary files start with "solid" too)
  bool binary = false;
  uint32_t count = 0;
  if (data.size() >= 84) {
    std::memcpy(&count, data.data() + 80, 4);
    binary = data.size() == 84 + size_t(count) * 50 || data.compare(0, 5, "solid") != 0;
    if (binary && data.size() < 84 + size_t(count) * 50) return fail(err, "STL: the file is cut short: " + path);
  }
  if (binary) {
    for (uint32_t t = 0; t < count; ++t) {
      const char* rec = data.data() + 84 + size_t(t) * 50;
      float v[12];
      std::memcpy(v, rec, sizeof v);
      addTri({v[3], v[4], v[5]}, {v[6], v[7], v[8]}, {v[9], v[10], v[11]});
    }
  } else {
    std::istringstream in(data);
    std::string word;
    std::vector<vec3> corner;
    while (in >> word) {
      if (word == "vertex") {
        vec3 p;
        in >> p.x >> p.y >> p.z;
        corner.push_back(p);
      } else if (word == "endloop" || word == "endfacet") {
        // facets are triangles; a polygon facet (rare) becomes a fan
        for (size_t i = 1; i + 1 < corner.size(); ++i) addTri(corner[0], corner[i], corner[i + 1]);
        corner.clear();
      }
    }
  }
  if (m.loopVert.empty()) return fail(err, "STL contains no triangles: " + path);
  PLOG_I("STL (%s): %zu triangles, read as millimeters, Z up", binary ? "binary" : "ASCII", m.faceStart.size() - 1);
  return true;
}

// ------------------------------------------------------------------ 3MF

namespace {

// A minimal XML reader for 3MF model parts: element names and attributes, no text content.
struct XmlTag {
  std::string name;  // without namespace prefix
  std::map<std::string, std::string> attr;  // attribute names without prefix
  bool closing = false, selfClosing = false;
  std::string attrOr(const std::string& k, const std::string& d = "") const {
    auto it = attr.find(k);
    return it == attr.end() ? d : it->second;
  }
};

std::string localName(const std::string& n) {
  auto c = n.find(':');
  return c == std::string::npos ? n : n.substr(c + 1);
}

std::vector<XmlTag> parseXml(const std::string& s) {
  std::vector<XmlTag> out;
  size_t i = 0, n = s.size();
  while (i < n) {
    size_t lt = s.find('<', i);
    if (lt == std::string::npos) break;
    if (s.compare(lt, 4, "<!--") == 0) {
      size_t e = s.find("-->", lt);
      i = e == std::string::npos ? n : e + 3;
      continue;
    }
    if (lt + 1 < n && (s[lt + 1] == '?' || s[lt + 1] == '!')) {
      size_t e = s.find('>', lt);
      i = e == std::string::npos ? n : e + 1;
      continue;
    }
    XmlTag t;
    size_t p = lt + 1;
    if (p < n && s[p] == '/') t.closing = true, ++p;
    size_t ns = p;
    while (p < n && !isspace((unsigned char)s[p]) && s[p] != '>' && s[p] != '/') ++p;
    t.name = localName(s.substr(ns, p - ns));
    for (;;) {
      while (p < n && isspace((unsigned char)s[p])) ++p;
      if (p >= n) break;
      if (s[p] == '/') {
        t.selfClosing = true;
        ++p;
        continue;
      }
      if (s[p] == '>') {
        ++p;
        break;
      }
      size_t ks = p;
      while (p < n && s[p] != '=' && !isspace((unsigned char)s[p]) && s[p] != '>') ++p;
      std::string key = localName(s.substr(ks, p - ks));
      while (p < n && isspace((unsigned char)s[p])) ++p;
      if (p < n && s[p] == '=') {
        ++p;
        while (p < n && isspace((unsigned char)s[p])) ++p;
        char q = p < n ? s[p] : '"';
        if (q == '"' || q == '\'') {
          size_t vs = ++p;
          while (p < n && s[p] != q) ++p;
          t.attr[key] = s.substr(vs, p - vs);
          if (p < n) ++p;
        }
      }
    }
    out.push_back(std::move(t));
    i = p;
  }
  return out;
}

struct Mat3mf {  // 3MF affine transform, row-vector convention: p' = p * M
  float m[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
  vec3 apply(const vec3& p) const {
    return {p.x * m[0] + p.y * m[3] + p.z * m[6] + m[9], p.x * m[1] + p.y * m[4] + p.z * m[7] + m[10],
            p.x * m[2] + p.y * m[5] + p.z * m[8] + m[11]};
  }
  Mat3mf then(const Mat3mf& b) const {  // apply this, then b
    Mat3mf r;
    for (int row = 0; row < 4; ++row)
      for (int col = 0; col < 3; ++col) {
        float v = row == 3 ? b.m[9 + col] : 0.f;
        for (int k = 0; k < 3; ++k) v += m[row * 3 + k] * b.m[k * 3 + col];
        r.m[row * 3 + col] = v;
      }
    return r;
  }
  float det() const {
    return m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) + m[2] * (m[3] * m[7] - m[4] * m[6]);
  }
  static Mat3mf parse(const std::string& s) {
    Mat3mf t;
    if (s.empty()) return t;
    std::istringstream in(s);
    for (int i = 0; i < 12 && in; ++i) in >> t.m[i];
    return t;
  }
};

struct Object3mf {
  std::vector<vec3> verts;
  std::vector<std::array<uint32_t, 3>> tris;
  std::vector<std::array<int, 4>> props;  // pid, p1, p2, p3 (-1 = none)
  struct Component {
    std::string objectId, path;
    Mat3mf xf;
  };
  std::vector<Component> components;
};

struct Model3mf {
  float unitToMeters = 0.001f;
  std::map<std::string, Object3mf> objects;
  std::map<std::string, std::vector<vec2>> texGroups;  // texture2dgroup id -> coordinates
  std::map<std::string, bool> materialGroups;          // basematerials / colorgroup ids
  struct Item {
    std::string objectId, path;
    Mat3mf xf;
  };
  std::vector<Item> build;
};

float unitScale(const std::string& u) {
  if (u == "micron") return 1e-6f;
  if (u == "centimeter") return 0.01f;
  if (u == "inch") return 0.0254f;
  if (u == "foot") return 0.3048f;
  if (u == "meter") return 1.f;
  return 0.001f;  // millimeter (the default)
}

Model3mf parseModel(const std::string& xml) {
  Model3mf md;
  Object3mf* obj = nullptr;
  std::string objId, groupId;
  bool inTexGroup = false;
  for (const XmlTag& t : parseXml(xml)) {
    if (t.closing) {
      if (t.name == "object") obj = nullptr;
      if (t.name == "texture2dgroup") inTexGroup = false;
      continue;
    }
    if (t.name == "model") md.unitToMeters = unitScale(t.attrOr("unit", "millimeter"));
    else if (t.name == "object") {
      objId = t.attrOr("id");
      obj = &md.objects[objId];
    } else if (t.name == "vertex" && obj) {
      obj->verts.push_back({std::strtof(t.attrOr("x", "0").c_str(), nullptr), std::strtof(t.attrOr("y", "0").c_str(), nullptr),
                            std::strtof(t.attrOr("z", "0").c_str(), nullptr)});
    } else if (t.name == "triangle" && obj) {
      obj->tris.push_back({(uint32_t)std::strtoul(t.attrOr("v1", "0").c_str(), nullptr, 10),
                           (uint32_t)std::strtoul(t.attrOr("v2", "0").c_str(), nullptr, 10),
                           (uint32_t)std::strtoul(t.attrOr("v3", "0").c_str(), nullptr, 10)});
      auto num = [&](const char* k) { auto v = t.attrOr(k); return v.empty() ? -1 : std::atoi(v.c_str()); };
      int pid = t.attr.count("pid") ? std::atoi(t.attrOr("pid").c_str()) : -1;
      int p1 = num("p1"), p2 = num("p2"), p3 = num("p3");
      obj->props.push_back({pid, p1, p2 < 0 ? p1 : p2, p3 < 0 ? p1 : p3});
    } else if (t.name == "component" && obj) {
      obj->components.push_back({t.attrOr("objectid"), t.attrOr("path"), Mat3mf::parse(t.attrOr("transform"))});
    } else if (t.name == "item") {
      md.build.push_back({t.attrOr("objectid"), t.attrOr("path"), Mat3mf::parse(t.attrOr("transform"))});
    } else if (t.name == "texture2dgroup") {
      groupId = t.attrOr("id");
      md.texGroups[groupId];
      inTexGroup = !t.selfClosing;
    } else if (t.name == "tex2coord" && inTexGroup) {
      md.texGroups[groupId].push_back({std::strtof(t.attrOr("u", "0").c_str(), nullptr), std::strtof(t.attrOr("v", "0").c_str(), nullptr)});
    } else if (t.name == "basematerials" || t.name == "colorgroup") {
      md.materialGroups[t.attrOr("id")] = true;
    }
  }
  return md;
}

}  // namespace

bool load3mf(const std::string& path, Mesh& m, std::string* err) {
  mz_zip_archive zip;
  std::memset(&zip, 0, sizeof zip);
  if (!mz_zip_reader_init_file(&zip, path.c_str(), 0)) return fail(err, "3MF: not a zip package: " + path);
  auto readEntry = [&](std::string name, std::string& out) {
    if (!name.empty() && name[0] == '/') name = name.substr(1);
    int idx = mz_zip_reader_locate_file(&zip, name.c_str(), nullptr, 0);
    if (idx < 0) return false;
    size_t sz = 0;
    void* p = mz_zip_reader_extract_to_heap(&zip, (mz_uint)idx, &sz, 0);
    if (!p) return false;
    out.assign((const char*)p, sz);
    mz_free(p);
    return true;
  };
  // the root model part: from _rels/.rels, else the usual name, else any .model
  std::string rootPath = "3D/3dmodel.model", rels;
  if (readEntry("_rels/.rels", rels))
    for (const XmlTag& t : parseXml(rels))
      if (t.name == "Relationship" && t.attrOr("Type").find("3dmodel") != std::string::npos) rootPath = t.attrOr("Target");
  std::map<std::string, Model3mf> parts;  // path -> parsed
  auto part = [&](const std::string& p) -> Model3mf* {
    std::string key = p.empty() ? rootPath : p;
    if (!key.empty() && key[0] == '/') key = key.substr(1);
    auto it = parts.find(key);
    if (it != parts.end()) return &it->second;
    std::string xml;
    if (!readEntry(key, xml)) return nullptr;
    return &(parts[key] = parseModel(xml));
  };
  Model3mf* root = part(rootPath);
  if (!root) {
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&zip) && !root; ++i) {
      char name[512];
      mz_zip_reader_get_filename(&zip, i, name, sizeof name);
      std::string n = name;
      if (n.size() > 6 && n.substr(n.size() - 6) == ".model") rootPath = n, root = part(n);
    }
  }
  if (!root) {
    mz_zip_reader_end(&zip);
    return fail(err, "3MF: no 3D model part in " + path);
  }
  m = Mesh();
  m.name = stemOf(path);
  m.faceStart = {0};
  std::map<std::string, uint32_t> slots;  // "part:pid:p1" -> material slot
  bool anyUV = false;
  int depthGuard = 0;
  std::function<void(Model3mf&, const std::string&, const std::string&, const Mat3mf&)> emit =
      [&](Model3mf& md, const std::string& partKey, const std::string& id, const Mat3mf& xf) {
        if (++depthGuard > 10000) return;
        auto it = md.objects.find(id);
        if (it == md.objects.end()) return;
        const Object3mf& o = it->second;
        bool flip = xf.det() < 0;
        uint32_t base = (uint32_t)m.positions.size();
        for (auto& v : o.verts) m.positions.push_back(fromPrintSpace(xf.apply(v), md.unitToMeters));
        for (size_t t = 0; t < o.tris.size(); ++t) {
          auto tri = o.tris[t];
          if (tri[0] >= o.verts.size() || tri[1] >= o.verts.size() || tri[2] >= o.verts.size()) continue;
          if (tri[0] == tri[1] || tri[1] == tri[2] || tri[0] == tri[2]) continue;
          auto pr = t < o.props.size() ? o.props[t] : std::array<int, 4>{-1, -1, -1, -1};
          std::string pid = pr[0] >= 0 ? std::to_string(pr[0]) : "";
          const std::vector<vec2>* tex = nullptr;
          if (!pid.empty())
            if (auto g = md.texGroups.find(pid); g != md.texGroups.end()) tex = &g->second;
          int order[3] = {0, 1, 2};
          if (flip) order[1] = 2, order[2] = 1;
          for (int k : order) {
            m.loopVert.push_back(base + tri[k]);
            vec2 uv(0.f);
            if (tex && pr[1 + k] >= 0 && pr[1 + k] < (int)tex->size()) uv = (*tex)[pr[1 + k]], anyUV = true;
            m.loopUV.push_back(uv);
          }
          m.faceStart.push_back((uint32_t)m.loopVert.size());
          uint32_t slot = 0;
          if (!pid.empty() && md.materialGroups.count(pid)) {
            std::string key = partKey + ":" + pid + ":" + std::to_string(std::max(pr[1], 0));
            auto s = slots.find(key);
            slot = s == slots.end() ? (slots[key] = (uint32_t)slots.size()) : s->second;
          }
          m.faceMaterial.push_back(slot);
        }
        for (auto& c : o.components) {
          Model3mf* target = c.path.empty() ? &md : part(c.path);
          if (target) emit(*target, c.path.empty() ? partKey : c.path, c.objectId, c.xf.then(xf));
        }
      };
  if (root->build.empty()) {
    for (auto& [id, o] : root->objects) emit(*root, rootPath, id, Mat3mf());
  } else {
    for (auto& item : root->build) {
      Model3mf* md = item.path.empty() ? root : part(item.path);
      if (md) emit(*md, item.path.empty() ? rootPath : item.path, item.objectId, item.xf);
    }
  }
  mz_zip_reader_end(&zip);
  if (m.loopVert.empty()) return fail(err, "3MF contains no triangles: " + path);
  if (!anyUV) std::fill(m.loopUV.begin(), m.loopUV.end(), vec2(0.f));
  PLOG_I("3MF: %zu triangles, %s, %zu material slot(s)", m.faceStart.size() - 1, anyUV ? "with texture coordinates" : "no UVs",
         std::max<size_t>(slots.size(), 1));
  return true;
}

// ------------------------------------------------------------------ PLY

namespace {

struct PlyProp {
  std::string name;
  int type = 0;       // byte size, negative for signed ints, 0x10|4 float, 0x10|8 double
  bool list = false;
  int countType = 0;
};
struct PlyElement {
  std::string name;
  size_t count = 0;
  std::vector<PlyProp> props;
};

int plyType(const std::string& t) {
  if (t == "char" || t == "int8") return -1;
  if (t == "uchar" || t == "uint8") return 1;
  if (t == "short" || t == "int16") return -2;
  if (t == "ushort" || t == "uint16") return 2;
  if (t == "int" || t == "int32") return -4;
  if (t == "uint" || t == "uint32") return 4;
  if (t == "float" || t == "float32") return 0x14;
  if (t == "double" || t == "float64") return 0x18;
  return 0;
}

// Reads values one at a time from the body (ASCII tokens or binary of either byte order).
struct PlyReader {
  const char* p;
  const char* end;
  int format;  // 0 ascii, 1 little endian, 2 big endian
  bool ok = true;
  double next(int type) {
    if (format == 0) {
      while (p < end && std::isspace((unsigned char)*p)) ++p;
      if (p >= end) return ok = false, 0.0;
      char* e = nullptr;
      double v = std::strtod(p, &e);
      if (e == p) return ok = false, 0.0;
      p = e;
      return v;
    }
    int n = type == 0x14 ? 4 : type == 0x18 ? 8 : std::abs(type);
    if (n == 0 || p + n > end) return ok = false, 0.0;
    unsigned char b[8];
    std::memcpy(b, p, n);
    p += n;
    bool little = true;
    {
      uint16_t one = 1;
      little = *(const unsigned char*)&one == 1;
    }
    if ((format == 1) != little) std::reverse(b, b + n);
    switch (type) {
      case -1: return (double)(int8_t)b[0];
      case 1: return (double)b[0];
      case -2: { int16_t v; std::memcpy(&v, b, 2); return v; }
      case 2: { uint16_t v; std::memcpy(&v, b, 2); return v; }
      case -4: { int32_t v; std::memcpy(&v, b, 4); return v; }
      case 4: { uint32_t v; std::memcpy(&v, b, 4); return v; }
      case 0x14: { float v; std::memcpy(&v, b, 4); return v; }
      case 0x18: { double v; std::memcpy(&v, b, 8); return v; }
    }
    return ok = false, 0.0;
  }
};

}  // namespace

bool loadPly(const std::string& path, Mesh& m, std::string* err) {
  bool ok = false;
  std::string data = paths::readFile(path, &ok);
  if (!ok) return fail(err, "Cannot read " + path);
  if (data.compare(0, 3, "ply") != 0) return fail(err, "Not a PLY file: " + path);
  size_t hdrEnd = data.find("end_header");
  if (hdrEnd == std::string::npos) return fail(err, "PLY: no end_header in " + path);
  size_t body = data.find('\n', hdrEnd);
  if (body == std::string::npos) return fail(err, "PLY: nothing after the header in " + path);
  ++body;
  std::istringstream hs(data.substr(0, hdrEnd));
  std::string line;
  int format = -1;
  std::vector<PlyElement> elements;
  while (std::getline(hs, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::istringstream ls(line);
    std::string w;
    ls >> w;
    if (w == "format") {
      std::string f;
      ls >> f;
      format = f == "ascii" ? 0 : f == "binary_little_endian" ? 1 : f == "binary_big_endian" ? 2 : -1;
    } else if (w == "element") {
      PlyElement e;
      ls >> e.name >> e.count;
      elements.push_back(e);
    } else if (w == "property" && !elements.empty()) {
      PlyProp pr;
      std::string t;
      ls >> t;
      if (t == "list") {
        std::string ct, it;
        ls >> ct >> it >> pr.name;
        pr.list = true;
        pr.countType = plyType(ct);
        pr.type = plyType(it);
      } else {
        pr.type = plyType(t);
        ls >> pr.name;
      }
      if (!pr.type || (pr.list && !pr.countType)) return fail(err, "PLY: unknown property type in '" + line + "'");
      elements.back().props.push_back(pr);
    }
  }
  if (format < 0) return fail(err, "PLY: unknown format in " + path);

  m = Mesh();
  m.name = stemOf(path);
  m.faceStart = {0};
  std::vector<vec3> vnormal;
  std::vector<vec2> vuv;
  std::vector<vec4> vcolor;
  bool hasN = false, hasUV = false, hasC = false, colorBytes = true, faceUV = false;
  PlyReader rd{data.data() + body, data.data() + data.size(), format};
  for (auto& e : elements) {
    if (e.name == "vertex") {
      auto find = [&](std::initializer_list<const char*> names) {
        for (const char* n : names)
          for (size_t i = 0; i < e.props.size(); ++i)
            if (!e.props[i].list && e.props[i].name == n) return (int)i;
        return -1;
      };
      int ix = find({"x"}), iy = find({"y"}), iz = find({"z"});
      int inx = find({"nx"}), iny = find({"ny"}), inz = find({"nz"});
      int iu = find({"s", "u", "texture_u", "texture_s"}), iv = find({"t", "v", "texture_v", "texture_t"});
      int ir = find({"red", "diffuse_red", "r"}), ig = find({"green", "diffuse_green", "g"});
      int ib = find({"blue", "diffuse_blue", "b"}), ia = find({"alpha", "diffuse_alpha", "a"});
      if (ix < 0 || iy < 0 || iz < 0) return fail(err, "PLY: vertices without x, y, z in " + path);
      hasN = inx >= 0 && iny >= 0 && inz >= 0;
      hasUV = iu >= 0 && iv >= 0;
      hasC = ir >= 0 && ig >= 0 && ib >= 0;
      if (hasC) colorBytes = e.props[ir].type != 0x14 && e.props[ir].type != 0x18;
      std::vector<double> val(e.props.size());
      m.positions.reserve(std::min<size_t>(e.count, data.size() / 12 + 1));  // a header can say anything
      for (size_t k = 0; k < e.count && rd.ok; ++k) {
        for (size_t i = 0; i < e.props.size(); ++i) {
          const PlyProp& pr = e.props[i];
          if (pr.list) {
            size_t n = (size_t)rd.next(pr.countType);
            for (size_t j = 0; j < n; ++j) rd.next(pr.type);
            continue;
          }
          val[i] = rd.next(pr.type);
        }
        // Z up, as Blender reads PLY: to Patina's Y up
        m.positions.push_back(vec3((float)val[ix], (float)val[iz], -(float)val[iy]));
        if (hasN) vnormal.push_back(vec3((float)val[inx], (float)val[inz], -(float)val[iny]));
        if (hasUV) vuv.push_back(vec2((float)val[iu], (float)val[iv]));
        if (hasC) {
          float sc = colorBytes ? 1.f / 255.f : 1.f;
          vcolor.push_back(vec4((float)val[ir] * sc, (float)val[ig] * sc, (float)val[ib] * sc,
                                ia >= 0 ? (float)val[ia] * sc : 1.f));
        }
      }
    } else if (e.name == "face") {
      for (size_t k = 0; k < e.count && rd.ok; ++k) {
        std::vector<uint32_t> idx;
        std::vector<vec2> tc;
        for (auto& pr : e.props) {
          if (!pr.list) {
            rd.next(pr.type);
            continue;
          }
          size_t n = (size_t)rd.next(pr.countType);
          if (pr.name == "vertex_indices" || pr.name == "vertex_index") {
            for (size_t j = 0; j < n; ++j) idx.push_back((uint32_t)rd.next(pr.type));
          } else if (pr.name == "texcoord") {
            for (size_t j = 0; j + 1 < n; j += 2) {
              float u = (float)rd.next(pr.type), v = (float)rd.next(pr.type);
              tc.push_back(vec2(u, v));
            }
            if (n % 2) rd.next(pr.type);
          } else {
            for (size_t j = 0; j < n; ++j) rd.next(pr.type);
          }
        }
        if (idx.size() < 3) continue;
        bool perCorner = tc.size() == idx.size();
        faceUV |= perCorner;
        for (size_t j = 0; j < idx.size(); ++j) {
          if (idx[j] >= m.positions.size()) return fail(err, "PLY: a face uses a vertex that is not there in " + path);
          m.loopVert.push_back(idx[j]);
          m.loopUV.push_back(perCorner ? tc[j] : hasUV ? vuv[idx[j]] : vec2(0.f));
          if (hasN) m.loopNormal.push_back(glm::length(vnormal[idx[j]]) > 1e-12f ? glm::normalize(vnormal[idx[j]])
                                                                                   : vec3(0, 1, 0));
          if (hasC) {
            const vec4& c = vcolor[idx[j]];
            m.loopColor.push_back(vec4(srgbToLinear(c.x), srgbToLinear(c.y), srgbToLinear(c.z), c.w));
          }
        }
        m.faceStart.push_back((uint32_t)m.loopVert.size());
        m.faceMaterial.push_back(0);
      }
    } else {
      for (size_t k = 0; k < e.count && rd.ok; ++k)
        for (auto& pr : e.props) {
          size_t n = pr.list ? (size_t)rd.next(pr.countType) : 1;
          for (size_t j = 0; j < n; ++j) rd.next(pr.type);
        }
    }
    if (!rd.ok) return fail(err, "PLY: the file ends early or has a bad value (" + e.name + ") in " + path);
  }
  if (m.loopVert.empty()) return fail(err, "The PLY file has no faces (a point cloud?): " + path);
  m.hasSourceNormals = hasN;
  if (!hasN) m.loopNormal.clear();
  if (hasC && std::all_of(vcolor.begin(), vcolor.end(), [](const vec4& c) { return c.x > 0.999f && c.y > 0.999f && c.z > 0.999f; }))
    m.loopColor.clear();  // all white: no colors
  PLOG_I("PLY: %zu vertices, %u faces%s%s%s", m.positions.size(), m.faceCount(), hasN ? ", normals" : "",
         (hasUV || faceUV) ? ", UVs" : "", m.loopColor.empty() ? "" : ", vertex colors");
  return true;
}

}  // namespace patina
