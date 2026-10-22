#include "engine/BridgeProjects.h"

#include <algorithm>
#include <filesystem>
#include <nlohmann/json.hpp>

#include "core/Container.h"
#include "core/Log.h"
#include "core/Paths.h"

namespace patina::bridgeprojects {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

fs::path storeFile() { return paths::configDir() / "blender_projects.json"; }

json load() {
  bool ok = false;
  std::string s = paths::readFile(storeFile(), &ok);
  if (!ok || s.empty()) return json::object();
  try {
    json j = json::parse(s);
    return j.is_object() ? j : json::object();
  } catch (std::exception&) {
    return json::object();
  }
}

std::string normal(const std::string& p) {
  std::error_code ec;
  fs::path a = fs::absolute(fs::path(p), ec);
  return (ec ? fs::path(p) : a).lexically_normal().string();
}

}  // namespace

void remember(const std::string& object, const std::string& project) {
  if (object.empty() || project.empty()) return;
  std::string p = normal(project);
  // the autosave and other copies in the cache are not the user's project
  std::string cache = normal(paths::cacheDir().string());
  if (p.rfind(cache, 0) == 0) return;
  json j = load();
  json& objects = j["objects"];
  if (!objects.is_object()) objects = json::object();
  std::vector<std::string> list;
  if (objects.contains(object) && objects[object].is_array())
    for (auto& v : objects[object])
      if (v.is_string() && v.get<std::string>() != p) list.push_back(v);
  list.insert(list.begin(), p);
  if (list.size() > 8) list.resize(8);
  objects[object] = list;
  paths::ensureDir(paths::configDir());
  if (!paths::writeFileAtomic(storeFile(), j.dump(1))) PLOG_W("Could not write %s", storeFile().string().c_str());
}

std::vector<std::string> remembered(const std::string& object) {
  json j = load();
  std::vector<std::string> out;
  if (j.contains("objects") && j["objects"].is_object() && j["objects"].contains(object) && j["objects"][object].is_array())
    for (auto& v : j["objects"][object])
      if (v.is_string()) out.push_back(v);
  return out;
}

std::string objectOf(const std::string& project) {
  json h;
  if (!container::readHeader(project, "PATINA01", h)) return {};
  if (!h.contains("bridge") || !h["bridge"].is_object()) return {};
  return h["bridge"].value("object", std::string());
}

bool holds(const std::string& project, const std::string& object) {
  json h;
  if (!container::readHeader(project, "PATINA01", h)) return false;
  auto linked = [&](const json& part) {
    return part.contains("bridge") && part["bridge"].is_object() && part["bridge"].value("object", std::string()) == object;
  };
  if (linked(h)) return true;
  for (auto& o : h.value("objects", json::array()))  // (1.2) the scene's other objects
    if (o.is_object() && linked(o)) return true;
  return false;
}

std::vector<std::string> candidates(const std::string& object, const std::vector<std::string>& others) {
  std::vector<std::string> all = remembered(object);
  for (auto& o : others)
    if (fs::path(o).extension() == ".patina") all.push_back(normal(o));
  std::vector<std::pair<fs::file_time_type, std::string>> found;
  for (auto& p : all) {
    if (std::any_of(found.begin(), found.end(), [&](auto& f) { return f.second == p; })) continue;
    std::error_code ec;
    auto t = fs::last_write_time(fs::path(p), ec);
    if (ec || !holds(p, object)) continue;
    found.push_back({t, p});
  }
  std::stable_sort(found.begin(), found.end(), [](auto& a, auto& b) { return a.first > b.first; });
  std::vector<std::string> out;
  for (auto& f : found) out.push_back(f.second);
  return out;
}

bool sameModel(const Mesh& a, const Mesh& b) {
  if (a.positions.empty() || b.positions.empty()) return false;
  if (a.faceStart == b.faceStart) return true;
  auto box = [](const Mesh& m, vec3& lo, vec3& hi) {
    lo = vec3(1e30f), hi = vec3(-1e30f);
    for (auto& p : m.positions) lo = glm::min(lo, p), hi = glm::max(hi, p);
  };
  vec3 alo, ahi, blo, bhi;
  box(a, alo, ahi);
  box(b, blo, bhi);
  float da = glm::length(ahi - alo), db = glm::length(bhi - blo);
  if (da <= 0.f || db <= 0.f) return false;
  float ratio = da / db;
  float off = glm::length((alo + ahi) * 0.5f - (blo + bhi) * 0.5f);
  return ratio > 0.67f && ratio < 1.5f && off < 0.25f * std::max(da, db);
}

}  // namespace patina::bridgeprojects
