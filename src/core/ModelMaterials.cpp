#include "core/ModelMaterials.h"

#include <cgltf.h>
#include <tiny_obj_loader.h>
#include <ufbx.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string_view>

#include "core/Container.h"
#include "core/Image.h"
#include "core/Log.h"
#include "core/Paths.h"

namespace patina {

namespace fs = std::filesystem;

static std::string lowerExt(const std::string& p) {
  std::string e = fs::path(p).extension().string();
  for (auto& c : e) c = (char)tolower(c);
  return e;
}

static bool setErr(std::string* err, const std::string& m) {
  if (err) *err = m;
  return false;
}

std::string ModelMap::key() const {
  if (!path.empty()) return path;
  if (data.empty()) return "";
  // embedded: the bytes themselves
  size_t h = std::hash<std::string_view>()(std::string_view((const char*)data.data(), data.size()));
  return "#" + std::to_string(data.size()) + ":" + std::to_string(h);
}

static bool mapsOf(const ModelMaterial& m, std::initializer_list<const ModelMap*> l) {
  for (auto* p : l)
    if (p->valid()) return true;
  return false;
}

bool ModelMaterial::hasAnyMap() const {
  return mapsOf(*this, {&baseColorMap, &roughnessMap, &metallicMap, &normalMap, &bumpMap, &emissiveMap, &opacityMap,
                        &coatMap, &sheenMap});
}

bool ModelMaterial::interesting() const {
  if (!defined) return false;
  if (hasAnyMap()) return true;
  vec3 c = vec3(baseColor);
  float hi = std::max({c.x, c.y, c.z}), lo = std::min({c.x, c.y, c.z});
  bool gray = hi - lo < 0.02f && hi > 0.5f;  // a default light gray
  return !gray || metallic > 0.5f || std::max({emissive.x, emissive.y, emissive.z}) > 0.01f || baseColor.w < 0.99f ||
         opacity < 0.99f || sheen > 0.01f || coat > 0.01f;
}

bool ModelMaterials::hasAnyMap() const {
  for (auto& s : slots)
    if (s.hasAnyMap()) return true;
  return false;
}

bool ModelMaterials::interesting() const {
  int n = 0;
  for (auto& s : slots)
    if (s.interesting()) ++n;
  if (n) return true;
  // plain grays, but different ones per slot: still a look worth keeping
  for (size_t i = 1; i < slots.size(); ++i)
    if (slots[i].defined && slots[0].defined && glm::length(vec3(slots[i].baseColor - slots[0].baseColor)) > 0.05f)
      return true;
  return false;
}

std::string findModelTexture(const std::string& modelPath, const std::string& named) {
  if (named.empty()) return "";
  std::string n = named;
  std::replace(n.begin(), n.end(), '\\', '/');
  std::error_code ec;
  fs::path dir = fs::path(modelPath).parent_path();
  fs::path given(n);
  std::vector<fs::path> tries;
  if (given.is_absolute()) tries.push_back(given);
  tries.push_back(dir / given);
  fs::path base = given.filename();
  std::string stem = fs::path(modelPath).stem().string();
  for (const char* sub : {"", "textures", "Textures", "texture", "tex", "images", "maps", "../textures", "../Textures"})
    tries.push_back(dir / sub / base);
  tries.push_back(dir / (stem + ".fbm") / base);
  for (auto& t : tries)
    if (fs::is_regular_file(t, ec)) return t.lexically_normal().string();
  // the same name in another case (files made on Windows)
  std::string lower = base.string();
  for (auto& c : lower) c = (char)tolower(c);
  for (const char* sub : {"", "textures", "Textures"}) {
    fs::path d = dir / sub;
    if (!fs::is_directory(d, ec)) continue;
    for (auto& e : fs::directory_iterator(d, ec)) {
      std::string f = e.path().filename().string();
      for (auto& c : f) c = (char)tolower(c);
      if (f == lower && e.is_regular_file(ec)) return e.path().lexically_normal().string();
    }
  }
  return "";
}

// ------------------------------------------------------------------ glTF

static ModelMap gltfMap(const cgltf_texture_view& v, const std::string& modelPath, std::vector<std::string>& notes) {
  ModelMap m;
  if (!v.texture) return m;
  const cgltf_image* img = v.texture->image;
  if (!img) img = v.texture->basisu_image;
  if (!img) {
    notes.push_back("a texture in a format Patina cannot read (WebP or another extension)");
    return m;
  }
  if (v.texcoord != 0 && !(v.has_transform && v.transform.has_texcoord && v.transform.texcoord == 0)) {
    notes.push_back("a texture on a second UV map (Patina reads the first)");
    return m;
  }
  if (img == v.texture->basisu_image) {
    notes.push_back("KTX2 / Basis textures (Patina cannot read them)");
    return m;
  }
  m.name = img->name ? img->name : img->uri ? img->uri : "embedded image";
  if (img->buffer_view) {
    const uint8_t* p = cgltf_buffer_view_data(img->buffer_view);
    if (p) m.data.assign(p, p + img->buffer_view->size);
  } else if (img->uri) {
    std::string uri = img->uri;
    if (uri.rfind("data:", 0) == 0) {
      size_t b = uri.find(";base64,");
      if (b != std::string::npos) m.data = base64Decode(uri.substr(b + 8));
    } else {
      std::vector<char> buf(uri.begin(), uri.end());
      buf.push_back(0);
      cgltf_decode_uri(buf.data());
      std::string decoded(buf.data());
      m.path = findModelTexture(modelPath, decoded);
      if (m.path.empty()) m.missing = true;
    }
  }
  if (v.has_transform) {
    // glTF: g' = T R S g, with g = (u, 1 - v); Patina samples at (g'.x, 1 - g'.y)
    const auto& t = v.transform;
    float c = std::cos(t.rotation), s = std::sin(t.rotation);
    mat3 T(1.f), R(1.f), S(1.f), F(1.f);
    T[2] = vec3(t.offset[0], t.offset[1], 1.f);
    R[0] = vec3(c, -s, 0.f);  // column-major: [cos sin; -sin cos] as rows
    R[1] = vec3(s, c, 0.f);
    S[0][0] = t.scale[0];
    S[1][1] = t.scale[1];
    F[1][1] = -1.f;
    F[2] = vec3(0.f, 1.f, 1.f);
    m.uvTransform = F * T * R * S * F;
  }
  return m;
}

static bool loadGltfMaterials(const std::string& path, ModelMaterials& out, std::string* err) {
  cgltf_options o{};
  cgltf_data* data = nullptr;
  if (cgltf_parse_file(&o, path.c_str(), &data) != cgltf_result_success) return setErr(err, "glTF parse failed: " + path);
  std::unique_ptr<cgltf_data, void (*)(cgltf_data*)> guard(data, cgltf_free);
  if (cgltf_load_buffers(&o, data, path.c_str()) != cgltf_result_success)
    return setErr(err, "glTF buffer load failed: " + path);
  auto& notes = out.notes;
  for (size_t i = 0; i < data->materials_count; ++i) {
    const cgltf_material& g = data->materials[i];
    ModelMaterial m;
    m.defined = true;
    m.name = g.name ? g.name : "";
    if (g.has_pbr_metallic_roughness) {
      const auto& p = g.pbr_metallic_roughness;
      m.baseColor = vec4(p.base_color_factor[0], p.base_color_factor[1], p.base_color_factor[2], p.base_color_factor[3]);
      m.metallic = p.metallic_factor;
      m.roughness = p.roughness_factor;
      m.baseColorMap = gltfMap(p.base_color_texture, path, notes);
      ModelMap mr = gltfMap(p.metallic_roughness_texture, path, notes);
      if (mr.valid()) {
        m.roughnessMap = mr, m.roughnessMap.channel = 1;
        m.metallicMap = mr, m.metallicMap.channel = 2;
      }
    } else if (g.has_pbr_specular_glossiness) {
      const auto& p = g.pbr_specular_glossiness;
      m.baseColor = vec4(p.diffuse_factor[0], p.diffuse_factor[1], p.diffuse_factor[2], p.diffuse_factor[3]);
      m.metallic = 0.f;
      m.roughness = 1.f - p.glossiness_factor;
      m.baseColorMap = gltfMap(p.diffuse_texture, path, notes);
      ModelMap sg = gltfMap(p.specular_glossiness_texture, path, notes);
      if (sg.valid()) {
        m.roughnessMap = sg, m.roughnessMap.channel = 3, m.roughnessMap.invert = true;
        m.roughness = 1.f;  // the map's glossiness times the factor, as roughness
        notes.push_back("specular-glossiness materials (read as roughness; specular color left out)");
      }
    } else {
      m.metallic = 1.f, m.roughness = 1.f;
    }
    m.normalMap = gltfMap(g.normal_texture, path, notes);
    m.normalScale = g.normal_texture.texture ? g.normal_texture.scale : 1.f;
    m.occlusionMap = gltfMap(g.occlusion_texture, path, notes);
    m.emissive = vec3(g.emissive_factor[0], g.emissive_factor[1], g.emissive_factor[2]);
    if (g.has_emissive_strength) m.emissive *= g.emissive_strength.emissive_strength;
    m.emissiveMap = gltfMap(g.emissive_texture, path, notes);
    m.alphaMode = g.alpha_mode == cgltf_alpha_mode_mask ? 1 : g.alpha_mode == cgltf_alpha_mode_blend ? 2 : 0;
    m.alphaCutoff = g.alpha_cutoff;
    if (g.has_clearcoat) {
      m.coat = g.clearcoat.clearcoat_factor;
      m.coatMap = gltfMap(g.clearcoat.clearcoat_texture, path, notes);
    }
    if (g.has_sheen) {
      const float* c = g.sheen.sheen_color_factor;
      m.sheen = std::max({c[0], c[1], c[2]});
      m.sheenMap = gltfMap(g.sheen.sheen_color_texture, path, notes);
    }
    if (g.has_transmission && g.transmission.transmission_factor > 0.f)
      notes.push_back("glass (transmission): Patina shows it with opacity instead");
    if (g.has_transmission && g.transmission.transmission_factor > 0.f && m.alphaMode == 0) {
      m.alphaMode = 2;
      m.baseColor.w *= 1.f - 0.85f * g.transmission.transmission_factor;
    }
    out.slots.push_back(std::move(m));
  }
  return true;
}

// ------------------------------------------------------------------ OBJ

static ModelMap objMap(const std::string& name, const tinyobj::texture_option_t& opt, const std::string& path) {
  ModelMap m;
  if (name.empty()) return m;
  m.name = name;
  m.path = findModelTexture(path, name);
  m.missing = m.path.empty();
  if (opt.scale[0] != 1.0 || opt.scale[1] != 1.0 || opt.origin_offset[0] != 0.0 || opt.origin_offset[1] != 0.0) {
    m.uvTransform[0][0] = (float)opt.scale[0];
    m.uvTransform[1][1] = (float)opt.scale[1];
    m.uvTransform[2] = vec3((float)opt.origin_offset[0], (float)opt.origin_offset[1], 1.f);
  }
  return m;
}

static bool loadObjMaterials(const std::string& path, ModelMaterials& out, std::string* err) {
  tinyobj::ObjReaderConfig cfg;
  cfg.triangulate = false;
  cfg.vertex_color = false;
  tinyobj::ObjReader r;
  if (!r.ParseFromFile(path, cfg)) return setErr(err, "OBJ: " + r.Error());
  for (const auto& t : r.GetMaterials()) {
    ModelMaterial m;
    m.defined = true;
    m.name = t.name;
    m.mapReplacesValue = true;
    m.baseColor = vec4((float)t.diffuse[0], (float)t.diffuse[1], (float)t.diffuse[2], 1.f);
    m.opacity = std::clamp((float)t.dissolve, 0.f, 1.f);
    if (m.opacity < 0.999f) m.alphaMode = 2;
    m.metallic = std::clamp((float)t.metallic, 0.f, 1.f);
    // Pr when given, else from the shininess (Ns), as Blender reads OBJ files
    if (t.roughness > 0.0) m.roughness = std::clamp((float)t.roughness, 0.f, 1.f);
    else m.roughness = 1.f - std::sqrt(std::clamp((float)t.shininess, 0.f, 1000.f) / 1000.f);
    m.emissive = vec3((float)t.emission[0], (float)t.emission[1], (float)t.emission[2]);
    m.sheen = std::clamp((float)t.sheen, 0.f, 1.f);
    m.coat = std::clamp((float)t.clearcoat_thickness, 0.f, 1.f);
    m.baseColorMap = objMap(t.diffuse_texname, t.diffuse_texopt, path);
    m.roughnessMap = objMap(t.roughness_texname, t.roughness_texopt, path);
    m.metallicMap = objMap(t.metallic_texname, t.metallic_texopt, path);
    m.normalMap = objMap(t.normal_texname, t.normal_texopt, path);
    m.bumpMap = objMap(t.bump_texname, t.bump_texopt, path);
    if (m.bumpMap.valid() || !m.bumpMap.name.empty()) m.normalScale = (float)t.bump_texopt.bump_multiplier;
    if (m.normalMap.valid()) m.normalScale = 1.f;
    m.emissiveMap = objMap(t.emissive_texname, t.emissive_texopt, path);
    m.opacityMap = objMap(t.alpha_texname, t.alpha_texopt, path);
    if (m.opacityMap.valid() && m.alphaMode == 0) m.alphaMode = 2;
    m.sheenMap = objMap(t.sheen_texname, t.sheen_texopt, path);
    out.slots.push_back(std::move(m));
  }
  return true;
}

// ------------------------------------------------------------------ FBX

static ModelMap fbxMap(const ufbx_texture* tex, const std::string& path, std::vector<std::string>& notes) {
  ModelMap m;
  if (!tex) return m;
  if (tex->type != UFBX_TEXTURE_FILE && tex->file_textures.count) tex = tex->file_textures.data[0];
  if (tex->type != UFBX_TEXTURE_FILE) {
    notes.push_back("procedural FBX textures");
    return m;
  }
  m.name = std::string(tex->relative_filename.data, tex->relative_filename.length);
  if (m.name.empty()) m.name = std::string(tex->filename.data, tex->filename.length);
  if (tex->content.size) {
    const uint8_t* p = (const uint8_t*)tex->content.data;
    m.data.assign(p, p + tex->content.size);
  } else {
    for (const ufbx_string* s : {&tex->filename, &tex->absolute_filename, &tex->relative_filename}) {
      if (!s->length) continue;
      m.path = findModelTexture(path, std::string(s->data, s->length));
      if (!m.path.empty()) break;
    }
    m.missing = m.path.empty();
  }
  if (tex->has_uv_transform) {
    const ufbx_matrix& x = tex->uv_to_texture;
    m.uvTransform[0] = vec3((float)x.m00, (float)x.m10, 0.f);
    m.uvTransform[1] = vec3((float)x.m01, (float)x.m11, 0.f);
    m.uvTransform[2] = vec3((float)x.m03, (float)x.m13, 1.f);
  }
  return m;
}

static bool loadFbxMaterials(const std::string& path, ModelMaterials& out, std::string* err) {
  ufbx_load_opts opts = {};
  opts.ignore_geometry = true;
  opts.ignore_animation = true;
  opts.load_external_files = false;
  ufbx_error error;
  ufbx_scene* scene = ufbx_load_file(path.c_str(), &opts, &error);
  if (!scene) return setErr(err, "FBX: " + std::string(error.description.data, error.description.length));
  std::unique_ptr<ufbx_scene, void (*)(ufbx_scene*)> guard(scene, ufbx_free_scene);
  auto& notes = out.notes;
  for (size_t i = 0; i < scene->materials.count; ++i) {
    const ufbx_material* f = scene->materials.data[i];
    const auto& p = f->pbr;
    ModelMaterial m;
    m.defined = true;
    m.name = std::string(f->name.data, f->name.length);
    m.mapReplacesValue = true;
    float bf = p.base_factor.has_value ? (float)p.base_factor.value_real : 1.f;
    m.baseColor = vec4(vec3((float)p.base_color.value_vec3.x, (float)p.base_color.value_vec3.y,
                            (float)p.base_color.value_vec3.z) * bf, 1.f);
    m.roughness = p.roughness.has_value ? std::clamp((float)p.roughness.value_real, 0.f, 1.f) : 0.5f;
    m.metallic = p.metalness.has_value ? std::clamp((float)p.metalness.value_real, 0.f, 1.f) : 0.f;
    float ef = p.emission_factor.has_value ? (float)p.emission_factor.value_real : 1.f;
    m.emissive = vec3((float)p.emission_color.value_vec3.x, (float)p.emission_color.value_vec3.y,
                      (float)p.emission_color.value_vec3.z) * ef;
    m.opacity = p.opacity.has_value ? std::clamp((float)p.opacity.value_real, 0.f, 1.f) : 1.f;
    if (m.opacity < 0.999f) m.alphaMode = 2;
    m.baseColorMap = fbxMap(p.base_color.texture, path, notes);
    m.roughnessMap = fbxMap(p.roughness.texture, path, notes);
    if (!m.roughnessMap.valid() && p.glossiness.texture) {
      m.roughnessMap = fbxMap(p.glossiness.texture, path, notes);
      m.roughnessMap.invert = true;
    }
    m.metallicMap = fbxMap(p.metalness.texture, path, notes);
    m.normalMap = fbxMap(p.normal_map.texture, path, notes);
    if (!m.normalMap.valid()) m.bumpMap = fbxMap(f->fbx.bump.texture, path, notes);
    m.emissiveMap = fbxMap(p.emission_color.texture, path, notes);
    m.opacityMap = fbxMap(p.opacity.texture, path, notes);
    if (!m.opacityMap.valid()) {
      // Blender (and others) put the alpha map on TransparencyFactor or TransparentColor
      const ufbx_texture* t = f->fbx.transparency_factor.texture ? f->fbx.transparency_factor.texture
                                                                  : f->fbx.transparency_color.texture;
      m.opacityMap = fbxMap(t, path, notes);
      m.opacityMap.alphaOnly = true;
    }
    if (m.opacityMap.valid() && m.alphaMode == 0) m.alphaMode = 2;
    m.occlusionMap = fbxMap(p.ambient_occlusion.texture, path, notes);
    if (p.coat_factor.has_value) m.coat = std::clamp((float)p.coat_factor.value_real, 0.f, 1.f);
    if (p.sheen_factor.has_value) m.sheen = std::clamp((float)p.sheen_factor.value_real, 0.f, 1.f);
    out.slots.push_back(std::move(m));
  }
  return true;
}

// (0.44) Blender's material slots as the add-on (0.13) describes them: the Principled BSDF's values (linear) and the
// image files behind its inputs. Linked inputs show the image alone, as in Blender (mapReplacesValue).
static bool loadPtMeshMaterials(const std::string& path, ModelMaterials& out, std::string* err) {
  nlohmann::json h;
  if (!container::readHeader(path, "PTMESH01", h, err)) return false;
  if (!h.contains("materials") || !h["materials"].is_array()) return true;  // an older add-on: none described
  auto num = [](const nlohmann::json& j, const char* k, float d) {
    return j.contains(k) && j[k].is_number() ? j[k].get<float>() : d;
  };
  auto color = [](const nlohmann::json& j, const char* k, vec4 d) {
    if (!j.contains(k) || !j[k].is_array()) return d;
    auto& a = j[k];
    for (int i = 0; i < 4 && i < (int)a.size(); ++i)
      if (a[i].is_number()) d[i] = a[i].get<float>();
    return d;
  };
  auto map = [&](const nlohmann::json& maps, const char* k, ModelMap& m) {
    if (!maps.contains(k) || !maps[k].is_object()) return;
    auto& j = maps[k];
    std::string p = j.value("path", std::string());
    m.name = j.value("image", fs::path(p).filename().string());
    std::error_code ec;
    if (p.empty() || !fs::is_regular_file(fs::path(p), ec)) {
      m.missing = true;
      return;
    }
    m.path = p;
    m.channel = std::clamp(j.value("channel", 0), 0, 3);
    m.invert = j.value("invert", false);
  };
  for (auto& j : h["materials"]) {
    ModelMaterial m;
    if (!j.is_object()) {  // an empty slot
      out.slots.push_back(m);
      continue;
    }
    m.defined = true;
    m.name = j.value("name", std::string());
    m.mapReplacesValue = true;
    m.baseColor = color(j, "base_color", vec4(0.8f, 0.8f, 0.8f, 1.f));
    m.roughness = num(j, "roughness", 0.5f);
    m.metallic = num(j, "metallic", 0.f);
    m.emissive = vec3(color(j, "emission", vec4(0.f)));
    m.opacity = num(j, "alpha", 1.f);
    m.baseColor.w = 1.f;  // Blender's alpha is its own input
    m.coat = num(j, "coat", 0.f);
    m.sheen = num(j, "sheen", 0.f);
    std::string am = j.value("alpha_mode", std::string());
    m.alphaMode = am == "clip" ? 1 : am == "blend" ? 2 : 0;
    m.alphaCutoff = num(j, "alpha_cutoff", 0.5f);
    m.vertexColors = j.value("vertex_colors", false) ? 1 : 0;
    if (m.vertexColors == 1) m.baseColor = vec4(1.f);  // a Color Attribute into Base Color: the colors alone
    if (j.contains("maps") && j["maps"].is_object()) {
      auto& maps = j["maps"];
      map(maps, "base_color", m.baseColorMap);
      map(maps, "roughness", m.roughnessMap);
      map(maps, "metallic", m.metallicMap);
      map(maps, "normal", m.normalMap);
      map(maps, "bump", m.bumpMap);
      map(maps, "emission", m.emissiveMap);
      map(maps, "alpha", m.opacityMap);
      map(maps, "coat", m.coatMap);
      map(maps, "sheen", m.sheenMap);
      if (maps.contains("normal") && maps["normal"].is_object()) m.normalScale = num(maps["normal"], "strength", 1.f);
      if (m.opacityMap.valid() && !m.alphaMode) m.alphaMode = 2;
    }
    for (auto& n : j.value("notes", nlohmann::json::array()))
      if (n.is_string()) out.notes.push_back(n);
    out.slots.push_back(m);
  }
  return true;
}

bool loadModelMaterials(const std::string& path, ModelMaterials& out, std::string* err) {
  out = ModelMaterials();
  std::string e = lowerExt(path);
  bool ok;
  if (e == ".gltf" || e == ".glb") ok = loadGltfMaterials(path, out, err);
  else if (e == ".obj") ok = loadObjMaterials(path, out, err);
  else if (e == ".fbx") ok = loadFbxMaterials(path, out, err);
  else if (e == ".ptmesh") ok = loadPtMeshMaterials(path, out, err);
  else return true;  // STL, 3MF, Patina's own: no materials to read
  // each thing once
  std::sort(out.notes.begin(), out.notes.end());
  out.notes.erase(std::unique(out.notes.begin(), out.notes.end()), out.notes.end());
  return ok;
}

}  // namespace patina
