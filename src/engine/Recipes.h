#pragma once
// 0.30: recipes (develop presets): a named list of Patina commands that turns a model into a finished one - mend it,
// reduce it, texture it, export it. Applied to the open model, or to a batch of files by a hidden `patina run`, so
// the files themselves are never changed. Strings in the steps may use ${name} (the model's name), ${file}, ${dir}
// (its folder) and ${out} (the output folder).
#include <map>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace patina {

using json = nlohmann::json;

struct Recipe {
  std::string id, name, description;
  json steps = json::array();  // [{"cmd": ..., "args": {...}}]
  bool builtin = false;
  std::string path;
  json toJson() const;
};

std::vector<Recipe> listRecipes();  // built-in (patina_*), then the user's
bool findRecipe(const std::string& id, Recipe& out);
bool saveRecipe(Recipe& r, std::string* err);  // into the user's folder (a built-in id gets a copy)
bool deleteRecipe(const std::string& id, std::string* err);

// ${name} -> value in every string; "${n}" alone takes the value's JSON type (numbers, true/false)
json substituteVars(const json& j, const std::map<std::string, std::string>& vars, std::set<std::string>& missing);

// The files a batch takes: these paths, and the models in these folders (subfolders too with recursive)
std::vector<std::string> collectModelFiles(const std::vector<std::string>& paths, bool recursive);

}  // namespace patina
