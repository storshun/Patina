// 0.30: recipes (develop presets) and batches of files.
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>

#include "core/Log.h"
#include "core/Paths.h"
#include "engine/Engine.h"
#include "engine/Recipes.h"
#include "platform/Process.h"

namespace patina {

namespace fs = std::filesystem;

namespace {
json prop(const char* type, const char* desc) { return {{"type", type}, {"description", desc}}; }
json num(const char* type, const char* desc, double lo, double hi, json def = json()) {
  json j = {{"type", type}, {"description", desc}};
  if (std::string(type) == "integer") j["minimum"] = (int64_t)lo, j["maximum"] = (int64_t)hi;
  else j["minimum"] = lo, j["maximum"] = hi;
  if (!def.is_null()) j["default"] = def;
  return j;
}
json schema(json props = json::object(), std::vector<std::string> required = {}) {
  json s = {{"type", "object"}, {"properties", props}, {"additionalProperties", false}};
  if (!required.empty()) s["required"] = required;
  return s;
}
json recipeSummary(const Recipe& r) {
  return {{"id", r.id}, {"name", r.name}, {"description", r.description}, {"builtin", r.builtin}, {"steps", r.steps}};
}
double nowSecs() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
}  // namespace

void Engine::registerBatchCommands() {
  CommandRegistry& R = commands;
  auto needOn = [](CommandContext& c) {
    if (!c.engine.features.on("recipes")) throw CommandError("recipes and batches are switched off (features)");
  };
  R.add({"recipe_list", "Recipes (develop presets): the built-in ones and yours, each with its steps.", schema(),
         [needOn](CommandContext& c, const json&) {
           needOn(c);
           json out = json::array();
           for (auto& r : listRecipes()) out.push_back(recipeSummary(r));
           return out;
         }});
  R.add({"recipe_save",
         "Saves a recipe (a list of Patina commands, steps: [{cmd, args}], run in order) among your recipes. Strings "
         "may use ${name} (the model's name), ${file}, ${dir} (its folder) and ${out} (the output folder). An id of a "
         "built-in recipe saves your own version of it.",
         schema({{"id", prop("string", "Id (default from the name)")}, {"name", prop("string", "Name")},
                 {"description", prop("string", "What it is for")}, {"steps", prop("array", "[{cmd, args}]")}},
                {"name", "steps"}),
         [needOn](CommandContext& c, const json& a) {
           needOn(c);
           Recipe r;
           r.id = a.value("id", std::string());
           r.name = a.at("name");
           r.description = a.value("description", std::string());
           r.steps = a.at("steps");
           for (auto& st : r.steps)
             if (!st.is_object() || !st.contains("cmd") || !c.engine.commands.find(st["cmd"].get<std::string>()))
               throw CommandError("each step needs a cmd Patina knows (" + st.dump() + ")");
           std::string err;
           if (!saveRecipe(r, &err)) throw CommandError(err);
           return json{{"id", r.id}, {"path", r.path}};
         }});
  R.add({"recipe_delete", "Deletes one of your recipes.", schema({{"id", prop("string", "Recipe id")}}, {"id"}),
         [needOn](CommandContext& c, const json& a) {
           needOn(c);
           std::string err;
           if (!deleteRecipe(a.at("id"), &err)) throw CommandError(err);
           return json{{"deleted", a.at("id")}};
         }});
  R.add({"recipe_apply",
         "Runs a recipe's steps on the open model, in order (a step that fails stops it). output: the folder ${out} "
         "stands for (default: the model's folder).",
         schema({{"id", prop("string", "Recipe id")}, {"steps", prop("array", "Or these steps")},
                 {"output", prop("string", "The output folder (${out})")}}),
         [needOn](CommandContext& c, const json& a) {
           needOn(c);
           Engine& e = c.engine;
           if (!e.doc.hasMesh()) throw CommandError("no model is open");
           json steps;
           std::string name;
           if (a.contains("id")) {
             Recipe r;
             if (!findRecipe(a["id"], r)) throw CommandError("no recipe '" + a["id"].get<std::string>() + "'");
             steps = r.steps, name = r.name;
           } else if (a.contains("steps")) {
             steps = a["steps"];
           } else {
             throw CommandError("give a recipe id or steps");
           }
           fs::path file = e.doc.meshPath.empty() ? fs::path(e.doc.projectPath) : fs::path(e.doc.meshPath);
           std::string stem = !e.doc.projectPath.empty() ? fs::path(e.doc.projectPath).stem().string()
                            : !file.empty() ? file.stem().string() : e.doc.mesh.name;
           fs::path dir = file.empty() ? paths::homeDir() / "patina_export" : file.parent_path();
           std::map<std::string, std::string> vars = {{"name", paths::sanitizeFileName(stem)}, {"file", file.string()},
                                                      {"dir", dir.string()}, {"index", "1"},
                                                      {"out", a.value("output", dir.string())}};
           std::set<std::string> missing;
           steps = substituteVars(steps, vars, missing);
           if (!missing.empty()) throw CommandError("no value for ${" + *missing.begin() + "}");
           json done = json::array();
           for (size_t i = 0; i < steps.size(); ++i) {
             std::string cmd = steps[i].value("cmd", std::string());
             if (cmd == "open_file") continue;  // the model is open already
             try {
               json r = e.execute(cmd, steps[i].value("args", json::object()));
               done.push_back({{"cmd", cmd}, {"ok", true}, {"result", r}});
             } catch (std::exception& ex) {
               throw CommandError("step " + std::to_string(i + 1) + " (" + cmd + "): " + ex.what());
             }
           }
           return json{{"recipe", name}, {"steps", done}};
         }});
  R.add({"batch_run",
         "Runs a recipe on many model files in a hidden Patina (the open project and the files themselves are not "
         "changed): each file is opened, the recipe's steps run, and the results go to output (${out}; default a "
         "patina_out folder beside the first file). files: model files; folders: every model in them (recursive: "
         "their subfolders too). A file whose step fails is left and the next goes on. batch_status follows it.",
         schema({{"recipe", prop("string", "Recipe id")}, {"steps", prop("array", "Or these steps")},
                 {"files", prop("array", "Model files")}, {"folders", prop("array", "Folders of models")},
                 {"recursive", prop("boolean", "Folders' subfolders too")}, {"output", prop("string", "Output folder")},
                 {"resolution", num("integer", "Texture set size for models opened without a project (a textured model may grow to its textures' size)", 512, 8192, 2048)}}),
         [needOn](CommandContext& c, const json& a) {
           needOn(c);
           Engine& e = c.engine;
           if (e.batch && e.batch->proc && e.batch->proc->running()) throw CommandError("a batch is running already");
           json steps;
           std::string recipeName;
           if (a.contains("recipe")) {
             Recipe r;
             if (!findRecipe(a["recipe"], r)) throw CommandError("no recipe '" + a["recipe"].get<std::string>() + "'");
             steps = r.steps, recipeName = r.name;
           } else if (a.contains("steps")) {
             steps = a["steps"];
           } else {
             throw CommandError("give a recipe or steps");
           }
           std::vector<std::string> paths_;
           for (auto& k : {"files", "folders"})
             if (a.contains(k))
               for (auto& v : a[k]) paths_.push_back(v.get<std::string>());
           std::vector<std::string> files = collectModelFiles(paths_, a.value("recursive", false));
           if (files.empty()) throw CommandError("no model files found (.glb .gltf .obj .fbx .stl .3mf .ply .ptmesh .patina)");
           std::string output = a.value("output", (fs::path(files[0]).parent_path() / "patina_out").string());
           std::error_code ec;
           fs::create_directories(output, ec);
           // the job: open each file, then the recipe's steps
           json perFile = json::array({{{"cmd", "open_file"}, {"args", {{"path", "${file}"}}}}});
           for (auto& st : steps)
             if (st.value("cmd", std::string()) != "open_file") perFile.push_back(st);
           char stamp[32];
           std::time_t now = std::time(nullptr);
           std::strftime(stamp, sizeof stamp, "%Y%m%d_%H%M%S", std::localtime(&now));
           fs::path dir = paths::ensureDir(paths::cacheDir() / "batch" / stamp);
           auto b = std::make_unique<BatchRun>();
           b->id = stamp;
           b->dir = dir.string();
           b->job = (dir / "job.json").string();
           b->progress = (dir / "progress.jsonl").string();
           b->report = (dir / "report.json").string();
           b->recipe = recipeName;
           b->output = output;
           b->files = files.size();
           b->started = nowSecs();
           json job = {{"name", recipeName.empty() ? std::string("batch") : recipeName}, {"files", files},
                       {"per_file", perFile}, {"vars", {{"out", output}}}};
           if (!paths::writeFileAtomic(b->job, job.dump(1))) throw CommandError("could not write " + b->job);
           b->proc = std::make_shared<ChildProcess>();
           std::string err;
           std::vector<std::string> args = {"run", b->job, "--progress", b->progress, "--report", b->report, "--quiet"};
           if (a.contains("resolution")) args.push_back("--resolution"), args.push_back(std::to_string(a["resolution"].get<int>()));
           if (!b->proc->start(selfExecutable(), args, &err)) throw CommandError(err);
           PLOG_I("Batch %s: %zu files, recipe %s, to %s", b->id.c_str(), files.size(), recipeName.c_str(), output.c_str());
           e.batch = std::move(b);
           return json{{"id", e.batch->id}, {"files", files}, {"output", output}, {"job", e.batch->job}};
         }});
  R.add({"batch_status",
         "The running (or last) batch: how many files are done, each file's result (ok, the error, its steps), and "
         "the report when it has finished.",
         schema(),
         [](CommandContext& c, const json&) {
           Engine& e = c.engine;
           if (!e.batch) return json{{"batch", nullptr}};
           BatchRun& b = *e.batch;
           json done = json::array();
           bool ok = false;
           std::string text = paths::readFile(b.progress, &ok);
           size_t failed = 0;
           if (ok) {
             size_t p = 0;
             while (p < text.size()) {
               size_t q = text.find('\n', p);
               if (q == std::string::npos) break;  // a line still being written
               json line = json::parse(text.substr(p, q - p), nullptr, false);
               if (line.is_object()) {
                 failed += !line.value("ok", false);
                 done.push_back(line);
               }
               p = q + 1;
             }
           }
           bool running = b.proc && b.proc->running();
           json out = {{"id", b.id}, {"recipe", b.recipe}, {"output", b.output}, {"files", b.files},
                       {"done", done.size()}, {"failed", failed}, {"running", running}, {"results", done},
                       {"seconds", nowSecs() - b.started}};
           if (!running) {
             json rep = json::parse(paths::readFile(b.report, &ok), nullptr, false);
             if (ok && rep.is_object()) out["cancelled"] = rep.value("cancelled", false), out["report"] = b.report;
             if (b.proc) out["exit_code"] = b.proc->exitCode();
           }
           return out;
         }});
  R.add({"batch_cancel", "Stops the running batch after the file it is on.", schema(),
         [](CommandContext& c, const json&) {
           Engine& e = c.engine;
           if (!e.batch || !e.batch->proc || !e.batch->proc->running()) return json{{"cancelled", false}};
           std::ofstream(fs::path(e.batch->progress + ".cancel")) << "cancel\n";
           return json{{"cancelled", true}};
         }});
}

}  // namespace patina
