#include "engine/Recipes.h"

#include <algorithm>
#include <filesystem>

#include "core/Log.h"
#include "core/Paths.h"

namespace patina {

namespace fs = std::filesystem;

static fs::path userRecipeDir() { return paths::userDataDir() / "recipes"; }
static fs::path builtinRecipeDir() { return paths::dataDir() / "recipes"; }

json Recipe::toJson() const {
  return {{"patina_recipe", 1}, {"id", id}, {"name", name}, {"description", description}, {"steps", steps}};
}

static bool readRecipe(const fs::path& p, bool builtin, Recipe& r) {
  bool ok = false;
  std::string text = paths::readFile(p, &ok);
  if (!ok) return false;
  try {
    json j = json::parse(text);
    if (j.value("patina_recipe", 0) != 1 || !j.contains("steps") || !j["steps"].is_array()) return false;
    r.id = j.value("id", p.stem().string());
    r.name = j.value("name", r.id);
    r.description = j.value("description", std::string());
    r.steps = j["steps"];
    r.builtin = builtin;
    r.path = p.string();
    return true;
  } catch (std::exception& e) {
    PLOG_W("Recipe %s: %s", p.string().c_str(), e.what());
    return false;
  }
}

std::vector<Recipe> listRecipes() {
  std::vector<Recipe> out;
  for (auto [dir, builtin] : {std::pair{builtinRecipeDir(), true}, std::pair{userRecipeDir(), false}}) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) continue;
    std::vector<fs::path> files;
    for (auto& e : fs::directory_iterator(dir, ec))
      if (e.path().extension() == ".json") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    for (auto& f : files) {
      Recipe r;
      if (!readRecipe(f, builtin, r)) continue;
      // a user recipe with a built-in's id replaces it
      auto it = std::find_if(out.begin(), out.end(), [&](const Recipe& x) { return x.id == r.id; });
      if (it != out.end()) *it = r;
      else out.push_back(r);
    }
  }
  return out;
}

bool findRecipe(const std::string& id, Recipe& out) {
  for (auto& r : listRecipes())
    if (r.id == id) {
      out = r;
      return true;
    }
  return false;
}

bool saveRecipe(Recipe& r, std::string* err) {
  if (r.name.empty()) {
    if (err) *err = "a recipe needs a name";
    return false;
  }
  if (r.id.empty()) {
    std::string id = "user_" + paths::sanitizeFileName(r.name);
    for (auto& c : id) c = c == ' ' ? '_' : (char)std::tolower((unsigned char)c);
    r.id = id;
  }
  fs::path dir = paths::ensureDir(userRecipeDir());
  fs::path p = dir / (paths::sanitizeFileName(r.id) + ".json");
  if (!paths::writeFileAtomic(p, r.toJson().dump(1) + "\n")) {
    if (err) *err = "could not write " + p.string();
    return false;
  }
  r.builtin = false;
  r.path = p.string();
  return true;
}

bool deleteRecipe(const std::string& id, std::string* err) {
  Recipe r;
  if (!findRecipe(id, r)) {
    if (err) *err = "no recipe '" + id + "'";
    return false;
  }
  if (r.builtin) {
    if (err) *err = "the built-in recipes can't be deleted (a copy can)";
    return false;
  }
  std::error_code ec;
  fs::remove(r.path, ec);
  return !ec;
}

json substituteVars(const json& j, const std::map<std::string, std::string>& vars, std::set<std::string>& missing) {
  if (j.is_string()) {
    std::string s = j, out;
    // "${n}" alone takes the value's type: --set resolution=4096 gives a number
    if (s.size() > 3 && s.rfind("${", 0) == 0 && s.back() == '}' && s.find('}') == s.size() - 1) {
      auto it = vars.find(s.substr(2, s.size() - 3));
      if (it != vars.end()) {
        try {
          json v = json::parse(it->second);
          if (v.is_number() || v.is_boolean()) return v;
        } catch (...) {
        }
      }
    }
    for (size_t i = 0; i < s.size(); ++i) {
      if (s[i] == '$' && i + 1 < s.size() && s[i + 1] == '{') {
        size_t e = s.find('}', i);
        if (e != std::string::npos) {
          std::string name = s.substr(i + 2, e - i - 2);
          auto it = vars.find(name);
          if (it != vars.end()) out += it->second;
          else missing.insert(name);
          i = e;
          continue;
        }
      }
      out += s[i];
    }
    return out;
  }
  if (j.is_array()) {
    json o = json::array();
    for (auto& v : j) o.push_back(substituteVars(v, vars, missing));
    return o;
  }
  if (j.is_object()) {
    json o = json::object();
    for (auto it = j.begin(); it != j.end(); ++it) o[it.key()] = substituteVars(it.value(), vars, missing);
    return o;
  }
  return j;
}

std::vector<std::string> collectModelFiles(const std::vector<std::string>& in, bool recursive) {
  static const char* exts[] = {".glb", ".gltf", ".obj", ".fbx", ".stl", ".3mf", ".ply", ".ptmesh", ".patina"};
  auto isModel = [](const fs::path& p) {
    std::string e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), ::tolower);
    return std::find(std::begin(exts), std::end(exts), e) != std::end(exts);
  };
  std::vector<std::string> out;
  for (auto& s : in) {
    std::error_code ec;
    fs::path p(s);
    if (fs::is_directory(p, ec)) {
      std::vector<std::string> found;
      if (recursive) {
        for (auto& e : fs::recursive_directory_iterator(p, fs::directory_options::skip_permission_denied, ec))
          if (e.is_regular_file(ec) && isModel(e.path())) found.push_back(e.path().string());
      } else {
        for (auto& e : fs::directory_iterator(p, ec))
          if (e.is_regular_file(ec) && isModel(e.path())) found.push_back(e.path().string());
      }
      std::sort(found.begin(), found.end());
      out.insert(out.end(), found.begin(), found.end());
    } else if (fs::is_regular_file(p, ec) && isModel(p)) {
      out.push_back(p.string());
    }
  }
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

}  // namespace patina
