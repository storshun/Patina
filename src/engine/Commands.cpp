// The automation API: every command is reachable over TCP (Blender add-on, MCP
// proxy), through `patina --mcp` (as MCP tools) and from the UI console.
#include <ctime>
#include <filesystem>

#include "core/Image.h"
#include "core/Lightmap.h"
#include "core/Log.h"
#include "core/MeshIO.h"
#include "core/Paths.h"
#include "core/UVOps.h"
#include "core/UVIsland.h"
#include "engine/AutoTexture.h"
#include "engine/BridgeProjects.h"
#include "engine/Engine.h"
#include "engine/Regions.h"
#include "core/Progress.h"
#include "engine/SmartMaterial.h"
#include "platform/Net.h"

namespace patina {

namespace fs = std::filesystem;

namespace {

json prop(const char* type, const char* desc) { return {{"type", type}, {"description", desc}}; }
json propEnum(std::vector<std::string> values, const char* desc) {
  return {{"type", "string"}, {"enum", values}, {"description", desc}};
}
json schema(json props = json::object(), std::vector<std::string> required = {}) {
  json s = {{"type", "object"}, {"properties", props}};
  if (!required.empty()) s["required"] = required;
  return s;
}
// (0.34) A number with its range and default (JSON Schema minimum / maximum / default), so agents know what they
// can set; the description says what it does to the result.
json num(const char* type, const char* desc, double lo, double hi, json def = json()) {
  json j = {{"type", type}, {"description", desc}};
  if (std::string(type) == "integer") j["minimum"] = (int64_t)lo, j["maximum"] = (int64_t)hi;
  else j["minimum"] = lo, j["maximum"] = hi;
  if (!def.is_null()) j["default"] = def;
  return j;
}
json viewModeEnum() {
  std::vector<std::string> names;
  for (int i = 0; i < (int)ViewMode::Count; ++i) names.push_back(viewModeName((ViewMode)i));
  return propEnum(names, "View mode: material (lit), a single channel, a bake, or a check (mesh_check, pbr_range, "
                         "texel_density, uv_stretch, symmetry)");
}
json flag(const char* desc, bool def) { return {{"type", "boolean"}, {"description", desc}, {"default", def}}; }
// The settings of an adjustment layer (add_layer / update_layer "adjust"): each one's range, default and effect.
json adjustSchema() {
  json p = {
      {"hue", num("number", "Turns the colors round the color wheel: 0.5 = the opposite color, 0.1 = red to orange.", -0.5, 0.5, 0)},
      {"saturation", num("number", "Multiplies the colorfulness: 0 gray, 2 twice as colorful. Changes every color alike (see vibrance).", 0, 3, 1)},
      {"brightness", num("number", "Multiplies the color values (sRGB): 1.2 = 20% brighter. exposure is the gentler, linear-light control.", 0, 3, 1)},
      {"contrast", num("number", "Spreads the colors away from (above 1) or toward (below 1) mid gray.", 0, 3, 1)},
      {"roughness", num("number", "Added to the roughness: +0.2 makes everything more matte, -0.2 shinier.", -1, 1, 0)},
      {"metallic", num("number", "Added to the metallic value.", -1, 1, 0)},
      {"height", num("number", "Added to the height (0.5 is flat).", -0.5, 0.5, 0)},
      {"exposure", num("number", "In stops, in linear light as a camera: +1 = twice the light, -1 = half. The first control to fix a texture that is too dark or too bright overall.", -5, 5, 0)},
      {"temperature", num("number", "White balance: +1 warmer (about +30% red, -30% blue), -1 cooler; brightness kept. Fixes a color cast from the light a scan or photo was made in.", -1, 1, 0)},
      {"tint", num("number", "White balance: +1 more magenta (about 25% less green), -1 greener; brightness kept.", -1, 1, 0)},
      {"highlights", num("number", "The bright parts only: up to one stop darker (-1) or brighter (+1) on the brightest, fading out below mid brightness. Brings back a top that came out too bright.", -1, 1, 0)},
      {"shadows", num("number", "The dark parts only: up to one stop darker (-1) or brighter (+1) on the darkest, fading out above 60% brightness. Lifts dark areas without washing out the rest.", -1, 1, 0)},
      {"vibrance", num("number", "More (+) or less (-) color, mostly in dull colors: saturation x (1 + v x (1 - saturation)); strong colors and skin change little.", -1, 1, 0)},
      {"delight", num("number", "Takes shadows baked into the color out (scans, generated models): divides the color by the ambient occlusion (floored at 0.25). 1 = fully: a texel with AO 0.5 becomes twice as bright. Bakes the AO if needed. Use texture_check before and after.", 0, 1, 0)}};
  return {{"type", "object"}, {"properties", p},
          {"description", "Adjustment layer settings (any of them; the rest stay). Order: de-light, white balance, "
                          "exposure, highlights / shadows in linear light, then vibrance, saturation, hue, brightness, "
                          "contrast. At the defaults nothing changes."}};
}
json imageResult(const std::vector<uint8_t>& png, int w, int h, json extra = json::object()) {
  extra["image_png_base64"] = base64Encode(png.data(), png.size());
  extra["width"] = w;
  extra["height"] = h;
  return extra;
}
vec3 jv3(const json& j, vec3 d = vec3(0)) {
  if (j.is_array() && j.size() >= 3) return {j[0].get<float>(), j[1].get<float>(), j[2].get<float>()};
  if (j.is_number()) return vec3(j.get<float>());
  if (j.is_string()) {
    std::string s = j;
    if (s.size() == 7 && s[0] == '#') {
      unsigned v = std::stoul(s.substr(1), nullptr, 16);
      return {((v >> 16) & 255) / 255.f, ((v >> 8) & 255) / 255.f, (v & 255) / 255.f};
    }
  }
  return d;
}
int layerIndexArg(Engine& e, const json& a) {
  int i = a.value("index", e.doc.selected);
  if (i < 0 || i >= (int)e.doc.layers.size()) throw CommandError("layer index out of range");
  return i;
}
int channelMaskFrom(const json& arr) {
  if (arr.is_number_integer()) return arr.get<int>();
  int m = 0;
  for (auto& c : arr) {
    std::string k = c;
    if (k == "all") return kAllChannels;
    int idx = channelFromKey(k);
    if (idx < 0) throw CommandError("unknown channel '" + k + "' (basecolor roughness metallic height normal emissive opacity sheen coat subsurface)");
    m |= 1 << idx;
  }
  return m;
}
json channelList(int mask) {
  json a = json::array();
  for (int c = 0; c < kChannelCount; ++c)
    if (mask & (1 << c)) a.push_back(channelInfo((Channel)c).key);
  return a;
}
std::vector<uint32_t> facesArg(Engine& e, const json& a) {
  std::vector<uint32_t> f;
  if (a.contains("faces")) {
    for (auto& v : a["faces"]) {
      uint32_t i = v;
      if (i < e.doc.mesh.faceCount()) f.push_back(i);
    }
  } else if (a.value("selection", false)) {
    f = e.doc.selectedFaces();
  }
  return f;
}
// Faces a layout operation (unwrap, pack, orient, density, rectify) acts on: a mirror copy
// stands for the face it copies (copies follow it), pinned faces are left out. Empty input =
// all faces; the result is empty only when nothing can move (`blocked` = pins were the reason).
std::vector<uint32_t> layoutTargets(Engine& e, const std::vector<uint32_t>& faces, bool* blocked = nullptr) {
  Document& d = e.doc;
  uint32_t F = d.mesh.faceCount();
  const uv::MirrorLinks& L = d.mirrorLinks();
  std::vector<uint8_t> in(F, 0);
  bool pinnedHit = false;
  auto add = [&](uint32_t f) {
    if (f < L.primary.size() && L.primary[f] >= 0) f = (uint32_t)L.primary[f];
    if (uv::isPinned(d.mesh, f)) {
      pinnedHit = true;
      return;
    }
    in[f] = 1;
  };
  if (faces.empty())
    for (uint32_t f = 0; f < F; ++f) add(f);
  else
    for (uint32_t f : faces) add(f);
  std::vector<uint32_t> out;
  for (uint32_t f = 0; f < F; ++f)
    if (in[f]) out.push_back(f);
  if (blocked) *blocked = out.empty() && pinnedHit;
  return out;
}

const char* channelSchemaDesc = "Channel keys: basecolor, roughness, metallic, height, normal, emissive, opacity, sheen, coat (clearcoat), subsurface (or 'all')";

void applyBrushArgs(Engine& e, const json& a) {
  if (a.contains("preset")) {
    std::string id = a["preset"];
    if (!e.brushes.find(id)) throw CommandError("unknown brush preset '" + id + "'");
    e.selectBrush(id);
  }
  if (a.contains("mask_value")) e.painter.maskValue = std::clamp(a["mask_value"].get<float>(), 0.f, 1.f);
  BrushPreset& b = e.painter.brush;
  json cur = b.toJson();
  for (auto& [k, v] : a.items())
    if (k != "preset") cur[k] = v;
  if (a.contains("alpha") && a["alpha"].is_string()) cur["alpha"] = {{"shape", a["alpha"]}, {"image", ""}, {"seed", 1}};
  b.fromJson(cur);
}

void applyMaterialArgs(Engine& e, const json& a) {
  MaterialRef& m = e.painter.material;
  if (a.contains("id")) {
    std::string id = a["id"];
    if (!id.empty() && !e.materials.find(id)) throw CommandError("unknown material '" + id + "' (see list_materials)");
    m.id = id;
    e.selectedMaterialId = id;
  }
  bool flatGiven = a.contains("color") || a.contains("roughness") || a.contains("metallic") || a.contains("height") ||
                   a.contains("emissive") || a.contains("opacity") || a.contains("sheen") || a.contains("coat") ||
                   a.contains("subsurface");
  if (flatGiven && !a.contains("id")) {
    m.id.clear();
    e.selectedMaterialId.clear();
  }
  if (a.contains("color")) m.flat.color = jv3(a["color"], m.flat.color);
  m.flat.roughness = a.value("roughness", m.flat.roughness);
  m.flat.metallic = a.value("metallic", m.flat.metallic);
  m.flat.height = a.value("height", m.flat.height);
  if (a.contains("emissive")) m.flat.emissive = jv3(a["emissive"], m.flat.emissive);
  m.flat.opacity = std::clamp(a.value("opacity", m.flat.opacity), 0.f, 1.f);
  m.flat.sheen = std::clamp(a.value("sheen", m.flat.sheen), 0.f, 1.f);
  m.flat.coat = std::clamp(a.value("coat", m.flat.coat), 0.f, 1.f);
  m.flat.subsurface = std::clamp(a.value("subsurface", m.flat.subsurface), 0.f, 1.f);
  json rest = a;
  for (auto k : {"color", "roughness", "metallic", "height", "emissive", "flat", "opacity", "sheen", "coat", "subsurface"})
    rest.erase(k);
  if (rest.contains("mapping") && rest["mapping"].is_string()) {
    std::string mp = rest["mapping"];
    rest["mapping"] = mp == "uv" ? 1 : mp == "screen" ? 2 : 0;
  }
  m.fromJson(rest);
}

}  // namespace

void Engine::registerCommands() {
  CommandRegistry& R = commands;

  // ---------------------------------------------------------------- general
  R.add({"features",
         "Newer features that can be switched off (Preferences > Features). Without arguments it lists them; "
         "with id and on, it switches one.",
         schema({{"id", prop("string", "Feature id from the list")}, {"on", prop("boolean", "Switch it on or off")}}),
         [](CommandContext& c, const json& a) {
           Features& F = c.engine.features;
           if (a.contains("id")) {
             if (!a.contains("on")) throw CommandError("give 'on': true or false");
             if (!F.set(a["id"].get<std::string>(), a["on"].get<bool>())) throw CommandError("unknown feature");
           }
           json list = json::array();
           for (auto& f : Features::all())
             list.push_back({{"id", f.id}, {"name", f.name}, {"description", f.description}, {"on", F.on(f.id)},
                             {"default", f.defaultOn}, {"since", f.since}});
           return json{{"features", list}};
         }});
  // ---------------------------------------------------------------- macros (0.10)
  auto macroDir = [] { return paths::ensureDir(paths::configDir() / "macros"); };
  auto needMacros = [](CommandContext& c) {
    if (!c.engine.features.on("macro_recorder")) throw CommandError("the macro recorder is switched off (features)");
  };
  R.add({"macro_record",
         "Macro recorder: action 'start' (clears the steps unless append), 'stop', 'clear', 'status', or "
         "'remove' (index). While recording, every command that changes something is kept as a step - from the "
         "menus, scripts or Claude. Viewport brush strokes are not commands and are not recorded.",
         schema({{"action", propEnum({"start", "stop", "clear", "status", "remove"}, "What to do (default status)")},
                 {"append", prop("boolean", "start: keep the steps already recorded")},
                 {"index", num("integer", "remove: the step to drop (0 = the first)", 0, 100000)}}),
         [needMacros](CommandContext& c, const json& a) {
           needMacros(c);
           auto& M = c.engine.macro;
           std::string act = a.value("action", std::string("status"));
           if (act == "start") {
             M.start(a.value("append", false));
           } else if (act == "stop") {
             M.recording = false;
           } else if (act == "clear") {
             M.clear();
           } else if (act == "remove") {
             int i = a.value("index", -1);
             if (i < 0 || !M.erase(c.engine.doc.undo, (size_t)i)) throw CommandError("no such step");
           }
           return json{{"recording", M.recording}, {"steps", c.engine.macroSteps()}};
         }});
  R.add({"macro_save",
         "Saves the recorded steps as a job file (the format of `patina run`): to 'path', or by 'name' into the "
         "macros folder so macro_play and the Macro window find it.",
         schema({{"name", prop("string", "Macro name")}, {"path", prop("string", "Job file path (.json)")}}),
         [needMacros, macroDir](CommandContext& c, const json& a) {
           needMacros(c);
           std::vector<json> steps = c.engine.macroSteps();
           if (steps.empty()) throw CommandError("nothing recorded");
           std::string name = a.value("name", std::string("macro"));
           fs::path p = a.contains("path") ? fs::path(a["path"].get<std::string>())
                                           : macroDir() / (paths::sanitizeFileName(name) + ".json");
           json job = {{"name", name}, {"patina", PATINA_VERSION}, {"stop_on_error", true}, {"steps", steps}};
           if (!paths::writeFileAtomic(p, job.dump(2))) throw CommandError("could not write " + p.string());
           return json{{"path", p.string()}, {"steps", steps.size()}};
         }});
  R.add({"macro_list", "Saved macros (the macros folder).", schema(),
         [needMacros, macroDir](CommandContext& c, const json&) {
           needMacros(c);
           json out = json::array();
           std::error_code ec;
           for (auto& e : fs::directory_iterator(macroDir(), ec)) {
             if (e.path().extension() != ".json") continue;
             bool ok = false;
             std::string text = paths::readFile(e.path(), &ok);
             json j = json::parse(text, nullptr, false);
             if (!ok || j.is_discarded()) continue;
             size_t n = j.is_array() ? j.size() : j.value("steps", json::array()).size();
             out.push_back({{"name", j.is_object() ? j.value("name", e.path().stem().string()) : e.path().stem().string()},
                            {"path", e.path().string()}, {"steps", n}});
           }
           std::sort(out.begin(), out.end(), [](const json& x, const json& y) { return x["name"] < y["name"]; });
           return out;
         }});
  R.add({"macro_play",
         "Replays steps: the recorded ones, a saved macro by 'name', a job file by 'path', or 'steps' given here. "
         "Stops at the first step that fails and reports it. Each step is its own undo step.",
         schema({{"name", prop("string", "Saved macro name")}, {"path", prop("string", "Job file")},
                 {"steps", prop("array", "[{cmd, args}]")}}),
         [needMacros, macroDir](CommandContext& c, const json& a) {
           needMacros(c);
           Engine& e = c.engine;
           json steps;
           if (a.contains("steps")) steps = a["steps"];
           else if (a.contains("name") || a.contains("path")) {
             fs::path p = a.contains("path") ? fs::path(a["path"].get<std::string>())
                                             : macroDir() / (paths::sanitizeFileName(a["name"].get<std::string>()) + ".json");
             bool ok = false;
             std::string text = paths::readFile(p, &ok);
             if (!ok) throw CommandError("cannot read " + p.string());
             json j = json::parse(text, nullptr, false);
             if (j.is_discarded()) throw CommandError(p.string() + " is not valid JSON");
             steps = j.is_array() ? j : j.value("steps", json::array());
           } else steps = e.macroSteps();
           if (!steps.is_array() || steps.empty()) throw CommandError("no steps to play");
           for (auto& s : steps) {  // jobs with ${variables} need `patina run --set`
             std::string d = s.dump();
             if (d.find("${") != std::string::npos) throw CommandError("the macro uses ${variables}: run it with patina run --set");
           }
           struct Playing {
             Engine::Macro& m;
             bool was;
             ~Playing() { m.playing = was; }
           } guard{e.macro, e.macro.playing};
           e.macro.playing = true;
           json done = json::array();
           for (size_t i = 0; i < steps.size(); ++i) {
             std::string cmd = steps[i].value("cmd", std::string());
             if (cmd.rfind("macro_", 0) == 0) throw CommandError("a macro cannot play macros (step " + std::to_string(i) + ")");
             try {
               e.execute(cmd, steps[i].value("args", json::object()));
               e.painter.update();
               done.push_back(cmd);
             } catch (std::exception& ex) {
               return json{{"ok", false}, {"played", done.size()}, {"failed_step", i}, {"command", cmd}, {"error", ex.what()}};
             }
           }
           return json{{"ok", true}, {"played", done.size()}};
         }});
  R.add({"ping", "Health check.", schema(), [](CommandContext& c, const json&) {
           return json{{"app", "patina"}, {"version", PATINA_VERSION}, {"headless", c.engine.headless}};
         }});
  R.add({"get_state",
         "Full state snapshot: mesh, layer stack, brush, paint material, tool, camera, view, undo, Blender bridge.",
         schema(), [](CommandContext& c, const json&) { return c.engine.state(); }});
  R.add({"list_commands", "Lists all commands with their JSON schemas.", schema(),
         [](CommandContext& c, const json&) { return c.engine.commands.describe(false); }, false});
  R.add({"get_log", "Recent log lines (warnings, errors, info).",
         schema({{"count", num("integer", "Number of lines, the newest last", 1, 1000, 50)}}),
         [](CommandContext&, const json& a) {
           json out = json::array();
           static const char* lv[] = {"debug", "info", "warn", "error"};
           for (auto& e : Log::snapshot(a.value("count", 50))) out.push_back({{"level", lv[(int)e.level]}, {"text", e.text}});
           return out;
         }});

  // ---------------------------------------------------------------- files
  R.add({"open_file", "Opens a mesh (.obj .gltf .glb .fbx .stl .3mf .ply .ptmesh) or a Patina project (.patina).",
         schema({{"path", prop("string", "Absolute file path")}}, {"path"}),
         [](CommandContext& c, const json& a) {
           std::string err;
           if (!c.engine.openFile(a.at("path"), &err)) throw CommandError(err);
           return c.engine.state()["mesh"];
         }});
  R.add({"open_mesh", "Loads a mesh and starts a new texture set (or keeps layers when topology matches).",
         schema({{"path", prop("string", "Mesh path (.obj .gltf .glb .fbx .stl .3mf .ply .ptmesh)")},
                 {"resolution", num("integer", "Texture set resolution per texture set / UDIM tile (512 to 8192, a power of two). 0 = the default (2048); a textured model's texture set grows to its textures' size unless this is given", 0, 8192, 0)},
                 {"keep_layers", prop("boolean", "Keep painted layers if the mesh has the same topology")},
                 {"separate", prop("boolean", "(1.2) A file with several meshes (a character's body, clothes, eyes, hair): an object each, with its own layers (true), or one object (false). Default: an object each when there are at most 24")}},
                {"path"}),
         [](CommandContext& c, const json& a) {
           std::string err;
           int separate = !a.contains("separate") ? -1 : a["separate"].get<bool>() ? 1 : 0;
           if (!c.engine.openMesh(a.at("path"), a.value("resolution", 0), a.value("keep_layers", false), &err, separate))
             throw CommandError(err);
           json r = c.engine.state()["mesh"];
           if (c.engine.objectCount() > 1) r["objects"] = c.engine.objectsJson();
           return r;
         }});
  R.add({"import_model_textures",
         "Brings in the model's own materials (textures and colors of each material slot, and vertex colors, from "
         "a .glb, .gltf, .obj with its .mtl, .fbx or .ply) as a paint layer, From the model, at the bottom of the "
         "stack (one undo step). Opening a model does this by itself when it has textures, vertex colors or "
         "materials that are more than plain gray. (1.2.2) An OBJ or FBX color texture's alpha becomes the opacity "
         "channel when the file says nothing about opacity and the alpha cuts out (hair cards, leaves, lace).",
         schema({{"path", prop("string", "The model file (default: the open model's file)")},
                 {"color_alpha", propEnum({"auto", "opacity", "ignore"},
                                          "OBJ / FBX: the color texture's alpha as the opacity. auto (default): when it "
                                          "has see-through and solid parts (a cut-out); opacity: whenever the image has "
                                          "alpha; ignore: never (alpha that holds gloss, as in some Unity assets)")}}),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           if (!e.features.on("model_textures")) throw CommandError("the model's textures are switched off (features)");
           std::string path = a.value("path", e.doc.meshPath);
           if (path.empty()) throw CommandError("The model has no file to read its materials from");
           std::string err;
           json r;
           std::string ca = a.value("color_alpha", std::string("auto"));
           int colorAlpha = ca == "opacity" ? 1 : ca == "ignore" ? 2 : 0;
           if (!e.doc.importModelTextures(path, false, &r, &err, true, colorAlpha))
             throw CommandError(err.empty() ? "Nothing to bring in" : err);
           if (path == e.doc.meshPath) e.modelTextures = r, e.modelTextures["path"] = path;
           return r;
         }});
  R.add({"new_project", "Starts a new project on a primitive mesh (sphere, cube, plane, torus).",
         schema({{"primitive", propEnum({"sphere", "cube", "plane", "torus"}, "Primitive mesh")},
                 {"resolution", num("integer", "Texture set resolution: 512, 1024, 2048, 4096 or 8192 (default: the preference, 2048)", 512, 8192)}}),
         [](CommandContext& c, const json& a) {
           std::string p = a.value("primitive", std::string("sphere"));
           Mesh m = p == "cube" ? makeCube(1.5f) : p == "plane" ? makePlane(2.f, 8)
                  : p == "torus" ? makeTorus(1.f, 0.38f) : makeUVSphere(64, 32, 1.f);
           c.engine.doc.meshPath.clear();
           c.engine.doc.projectPath.clear();
           c.engine.doc.bridge = BridgeState();
           c.engine.doc.setMesh(std::move(m), a.value("resolution", c.engine.defaultResolution), false);
           c.engine.newScene();
           c.engine.assistantTurns.clear();
           c.engine.frameMesh();
           return c.engine.state()["mesh"];
         }});
  R.add({"save_project", "Saves the project (.patina single file).",
         schema({{"path", prop("string", "Destination; default: current project path")}}),
         [](CommandContext& c, const json& a) {
           std::string path = a.value("path", c.engine.doc.projectPath);
           if (path.empty()) throw CommandError("no path given and the project has never been saved");
           if (fs::path(path).extension() != ".patina") path += ".patina";
           std::string err;
           if (!c.engine.saveScene(path, &err)) throw CommandError(err);
           return json{{"path", path}, {"objects", c.engine.objectCount()}};
         }});
  R.add({"save_incremental", "Saves the project as the next version next to it: name_v002.patina, _v003...",
         schema(), [](CommandContext& c, const json&) {
           Document& d = c.engine.doc;
           if (d.projectPath.empty()) throw CommandError("save the project once first (save_project with a path)");
           std::string path = d.incrementalPath(), err;
           if (path.empty()) throw CommandError("could not find a free version number");
           if (!c.engine.saveScene(path, &err)) throw CommandError(err);
           return json{{"path", path}};
         }});
  R.add({"export_again", "Exports the textures again with the settings of the last export (also stored in the project).",
         schema(), [](CommandContext& c, const json&) {
           Document& d = c.engine.doc;
           if (!d.hasLastExport) throw CommandError("nothing exported yet in this project: use export_textures first");
           std::map<std::string, std::string> files;
           std::string err;
           d.updateComposite(c.engine.materials);
           ExportOptions o = d.lastExport;
           if (!d.exportTextures(o, &files, &err)) throw CommandError(err);
           return json{{"files", files}, {"directory", o.directory}};
         }});
  R.add({"reload_mesh",
         "Reloads the mesh file the project came from (after it was re-exported, e.g. from Blender), keeping the "
         "layers: paint is moved to changed UVs, or transferred through 3D when the topology changed.",
         schema({{"path", prop("string", "Mesh file (default: the current mesh's file)")}}),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           std::string path = a.value("path", e.doc.meshPath), err;
           if (path.empty()) throw CommandError("the project has no mesh file to reload");
           if (!e.doc.loadMesh(path, 0, true, &err)) throw CommandError(err);
           return json{{"path", path}, {"faces", e.doc.mesh.faceCount()}, {"layers", e.doc.layers.size()}};
         }});
  R.add({"bake_from_high_poly",
         "Bakes a high-poly mesh (sculpt) onto the low-poly one: tangent-space normal detail (becomes the base "
         "normal of the stack and is exported with it), ambient occlusion and material slots. Every texel is "
         "sampled at several points; each looks along the low-poly's normal both ways out to 'distance' (the "
         "nearest point of the sculpt when the rays find nothing). Both meshes must be in the same place. Returns "
         "the shares found along the normals (hit_fraction), by the nearest point and guessed.",
         schema({{"path", prop("string", "High-poly mesh file (.glb .gltf .fbx .obj .stl .3mf .ply)")},
                 {"distance", num("number", "How far above and below the low-poly surface to look for the sculpt, object units; -1 = measured from the two meshes (1.1). Too small misses detail, too large catches the wrong side of thin parts", -1, 1000000, -1)},
                 {"samples", num("integer", "(1.1) Points per texel: 1, 4, 9 or 16. More: smoother edges of the detail, slower", 1, 16, 4)},
                 {"normal", prop("boolean", "Bake the normal map (default true)")},
                 {"ao", prop("boolean", "Bake ambient occlusion from the high-poly mesh (default true)")},
                 {"material_ids", prop("boolean", "Take material slots from the high-poly mesh (default true)")},
                 {"ao_samples", num("integer", "AO directions: more is smoother and slower", 8, 1024, 64)},
                 {"clear", prop("boolean", "Remove the high-poly bake instead")}}),
         [](CommandContext& c, const json& a) {
           Document& d = c.engine.doc;
           if (a.value("clear", false)) {
             d.clearHighPoly();
             return json{{"cleared", true}};
           }
           std::string path = a.value("path", d.highPolyPath);
           if (path.empty()) throw CommandError("give the path of the high-poly mesh");
           HighPolyBakeOptions o;
           o.distance = a.value("distance", -1.f);
           o.normal = a.value("normal", true);
           o.ao = a.value("ao", true);
           o.materialIds = a.value("material_ids", true);
           o.aoSamples = std::clamp(a.value("ao_samples", 64), 8, 1024);
           o.samples = std::clamp(a.value("samples", 4), 1, 16);
           json stats;
           std::string err;
           if (!d.bakeHighPoly(path, o, &stats, &err)) throw CommandError(err);
           return stats;
         }});
  R.add({"bake_down",
         "(1.1) Bakes the layers down from the painted high-poly model (the one open) onto its low-poly version, "
         "which becomes the model: every layer comes along. Paint, masks and a baked map's fixes are resampled "
         "texel by texel: several points per texel, each looking along the low's normal both ways for the high "
         "surface (the nearest point of it when the rays find nothing); normals are turned to the low's surface. "
         "Fills, adjustments, folders and smart masks stay as they are, over the low's new bakes: the high's shape "
         "baked into the normal map, ambient occlusion and material slots. The result is a new project (no file "
         "yet, not linked to Blender); the high stays as its high-poly model. Both models must be in the same "
         "place. For the retopology's quads use retopo_use_as_model (it bakes down too). Returns the shares of "
         "the texture found along the normals (hit_fraction), by the nearest point and guessed, the distance, the "
         "largest gap between the two surfaces; then check it with set_view mode bake_check (amber: by the "
         "nearest point, red: guessed) and set_compare mode high_poly (the high as it was beside the low).",
         schema({{"path", prop("string", "The low-poly model's file (.glb .gltf .fbx .obj .ply .stl .3mf .ptmesh)")},
                 {"resolution", num("integer", "Per texture set or UDIM tile (default the high-poly's)", 256, 8192)},
                 {"samples", num("integer", "Points per texel: 1, 4, 9 or 16. More: finer, smoother paint at edges and in fine detail; slower. At most 4 above a 4K atlas", 1, 16, 9)},
                 {"distance", num("number", "How far along the low's normals to look for the high surface, object units; -1: measured from the two models (1.5 times the gap between them at almost every point)", -1, 1000000, -1)},
                 {"bake_high_poly", prop("boolean", "Bake the high-poly's shape into the low's normal map, ambient occlusion and material slots (default true)")}},
                {"path"}),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           if (!e.features.on("bake_down")) throw CommandError("the bake-down is switched off (features)");
           if (!e.doc.hasMesh()) throw CommandError("open the painted high-poly model first");
           std::string path = a.at("path"), err;
           Mesh low;
           if (!loadMesh(path, low, &err)) throw CommandError(err);
           Document::BakeDownOptions o;
           o.resolution = a.value("resolution", 0);
           o.samples = std::clamp(a.value("samples", 9), 1, 16);
           o.distance = a.value("distance", -1.f);
           o.bakeHigh = a.value("bake_high_poly", true);
           json r;
           if (!e.bakeDown(std::move(low), path, o, &r, &err)) throw CommandError(err);
           return r;
         }});
  R.add({"render_image",
         "Renders the 3D view (current camera, lighting and view mode, without overlays) to a PNG. Optional "
         "transparent background and turntable frames (a full turn, files name_0001.png ...).",
         schema({{"path", prop("string", "PNG file")}, {"width", num("integer", "Image width in pixels", 16, 16384, 1920)},
                 {"height", num("integer", "Image height in pixels", 16, 16384, 1080)}, {"transparent", prop("boolean", "Transparent background")},
                 {"supersample", num("integer", "Rendered this many times larger and scaled down: smoother edges, slower", 1, 4, 2)}, {"turntable_frames", num("integer", "Frames of a turntable round the model (0 = one image)", 0, 720, 0)},
                 {"path_traced", prop("boolean", "Path traced: real shadows, reflections and light bouncing between the model's parts, lit by the environment (a shadow on the ground with path_trace's ground). Slower; no supersampling needed")},
                 {"samples", num("integer", "Path traced: samples per pixel (more: less noise, slower)", 1, 8192, 256)}},
                {"path"}),
         [](CommandContext& c, const json& a) {
           RenderImageOptions o;
           o.path = a.at("path");
           o.width = a.value("width", 1920);
           o.height = a.value("height", 1080);
           o.transparent = a.value("transparent", false);
           o.supersample = a.value("supersample", 2);
           o.turntableFrames = a.value("turntable_frames", 0);
           if (a.value("path_traced", false)) {
             if (!c.engine.features.on("path_tracing")) throw CommandError("path tracing is switched off (features)");
             o.pathSamples = std::clamp(a.value("samples", 256), 1, 8192);
           }
           std::vector<std::string> files;
           std::string err;
           if (!c.engine.renderImage(o, &files, &err)) throw CommandError(err);
           return json{{"files", files}};
         }});
  R.add({"set_resolution", "Changes the texture set resolution (layers are resampled).",
         schema({{"resolution", num("integer", "Resolution per texture set / UDIM tile: 512, 1024, 2048, 4096 or 8192. Layers are resampled. Higher is sharper and takes more memory (a full paint layer: about 85 MB at 2K, 330 MB at 4K)", 512, 8192)}}, {"resolution"}),
         [](CommandContext& c, const json& a) {
           int r = a.at("resolution");
           if (r < 64 || r > 8192) throw CommandError("resolution must be between 64 and 8192");
           std::string why;
           if (!c.engine.doc.atlasFits(r, c.engine.doc.mesh.tiles.grid, &why)) throw CommandError(why);
           c.engine.doc.changeResolution(r);
           return json{{"resolution", r}};
         }});
  R.add({"texture_tiles",
         "Texture sets per material slot and / or UDIM tiles. Without 'mode' it reports the layout. mode: "
         "'single' (one texture), 'material_slots' (a texture set per slot), 'udim' (a texture per UDIM tile) or "
         "'material_slots_udim'. Paint moves with the layout; undo history is cleared. Every tile has the "
         "texture set resolution.",
         schema({{"mode", propEnum({"single", "material_slots", "udim", "material_slots_udim"}, "The layout")},
                 {"faces", prop("boolean", "Report each tile's face indices too")}}),
         [](CommandContext& c, const json& a) {
           Document& d = c.engine.doc;
           if (a.contains("mode")) {
             if (!d.hasMesh()) throw CommandError("no mesh loaded");
             std::string m = a["mode"];
             uint8_t mode = m == "material_slots" ? TileBySlot : m == "udim" ? TileByUdim
                          : m == "material_slots_udim" ? (TileBySlot | TileByUdim) : 0;
             TileLayout probe;
             probe.mode = mode;
             Mesh tmp;  // how many tiles the layout would make, before any texture is touched
             tmp.faceStart = d.mesh.faceStart;
             tmp.loopVert = d.mesh.loopVert;
             tmp.loopUV = d.mesh.loopUV;
             tmp.faceMaterial = d.mesh.faceMaterial;
             tmp.materialNames = d.mesh.materialNames;
             tmp.tiles = probe;
             tmp.updateTiles();
             std::string why;
             if (!d.atlasFits(d.tileRes(), tmp.tiles.grid, &why)) throw CommandError(why);
             if (mode != d.mesh.tiles.mode && !d.setTileMode(mode, &why)) throw CommandError(why);
             // the texture set shown / isolated may be gone
             c.engine.uvOptions.tile = std::clamp(c.engine.uvOptions.tile, 0, std::max(0, d.mesh.tiles.count() - 1));
             if (!d.mesh.tiles.active()) c.engine.viewOptions.isolateSet = false, c.engine.painter.isolateTile = -1;
             if (c.engine.onExternalChange) c.engine.onExternalChange("tiles");
           }
           const TileLayout& t = d.mesh.tiles;
           json list = json::array();
           std::vector<int> faces(t.count(), 0);
           for (uint32_t f = 0; f < d.mesh.faceCount(); ++f) faces[std::clamp(d.mesh.faceTile(f), 0, t.count() - 1)]++;
           for (int i = 0; i < (int)t.tiles.size(); ++i) {
             list.push_back({{"index", i}, {"name", t.tiles[i].name}, {"slot", t.tiles[i].slot}, {"udim", t.tiles[i].udim},
                             {"faces", faces[i]}});
             if (a.value("faces", false)) {
               json fi = json::array();
               for (uint32_t f = 0; f < d.mesh.faceCount(); ++f)
                 if (d.mesh.faceTile(f) == i) fi.push_back(f);
               list.back()["face_indices"] = fi;
             }
           }
           std::string mode = t.mode == TileBySlot ? "material_slots" : t.mode == TileByUdim ? "udim"
                            : t.mode == (TileBySlot | TileByUdim) ? "material_slots_udim" : "single";
           return json{{"mode", mode}, {"tiles", list}, {"grid", t.grid}, {"resolution", d.tileRes()},
                       {"atlas_resolution", d.tex.resolution()}, {"suggested", [&] {
                          uint8_t s = Document::detectTileMode(d.mesh);
                          return s == TileBySlot ? "material_slots" : s == TileByUdim ? "udim"
                               : s == (TileBySlot | TileByUdim) ? "material_slots_udim" : "single";
                        }()}};
         }});
  R.add({"mesh_check",
         "The Mesh Check: what the model is made of (vertices, faces, triangles, parts, material slots, UV islands, "
         "size in Blender's axes, area, volume, closed or not, UV coverage) and what is wrong with it: holes, "
         "non-manifold edges, bowtie vertices, flipped faces, inside-out parts, degenerate and duplicate faces, open "
         "seams (vertices at one place, not joined), loose vertices, specks and floaters, faces without UV space, "
         "overlapping UVs, odd scale. Each problem says how many, where (Blender axes) and which mesh_fix mends it. "
         "show: the Mesh Check heat map in the viewport; select: select a problem's faces.",
         schema({{"show", flag("Switch the viewport to the Mesh Check heat map (red problem, orange warning, blue note)", false)},
                 {"select", prop("string", "Select the faces of this problem (its id from the list), e.g. to mask or delete them")},
                 {"faces", flag("List each problem's faces (up to 20000 each)", false)}}),
         [](CommandContext& c, const json& a) {
           if (!c.engine.features.on("mesh_check")) throw CommandError("the mesh check is switched off (features)");
           Document& d = c.engine.doc;
           if (!d.hasMesh()) throw CommandError("no mesh loaded");
           const MeshCheckReport& r = d.meshCheck();
           if (a.value("show", false)) c.engine.viewOptions.mode = ViewMode::MeshCheck;
           auto blender = [](vec3 p) { return json::array({p.x, -p.z, p.y}); };
           const MeshStats& st = r.stats;
           json stats = {{"vertices", st.vertices}, {"faces", st.faces}, {"triangles", st.triangles}, {"quads", st.quads},
                         {"ngons", st.ngons}, {"triangles_drawn", st.trianglesRendered}, {"edges", st.edges},
                         {"parts", st.parts}, {"material_slots", st.slots}, {"uv_islands", st.uvIslands},
                         {"size", json::array({st.size.x, st.size.z, st.size.y})}, {"area", st.area},
                         {"volume", st.volume}, {"closed", st.closed}, {"uv_coverage", st.uvCoverage}};
           json probs = json::array();
           for (auto& p : r.problems) {
             json w = json::array();
             for (size_t i = 0; i < p.where.size() && i < 5; ++i) w.push_back(blender(p.where[i]));
             json j = {{"id", p.id}, {"name", p.name}, {"what", p.what}, {"count", p.count}, {"unit", p.unit},
                       {"severity", p.severity == 2 ? "problem" : p.severity == 1 ? "warning" : "note"},
                       {"fix", p.fix}, {"faces_affected", p.faces.size()}, {"where", w}};
             if (a.value("faces", false))
               j["faces"] = std::vector<uint32_t>(p.faces.begin(), p.faces.begin() + std::min<size_t>(p.faces.size(), 20000));
             probs.push_back(j);
           }
           json out = {{"stats", stats}, {"problems", probs}, {"can_revert_fix", d.canRevertMeshFix()},
                       {"object", d.bridge.active ? d.bridge.object : std::string()}};
           if (a.contains("select")) {
             std::string id = a["select"];
             auto it = std::find_if(r.problems.begin(), r.problems.end(), [&](const MeshProblem& p) { return p.id == id; });
             if (it == r.problems.end()) throw CommandError("no problem '" + id + "' on this mesh");
             d.faceSelection.assign(d.mesh.faceCount(), 0);
             for (uint32_t f : it->faces) d.faceSelection[f] = 1;
             d.updateSelectionFlags();
             out["selected"] = it->faces.size();
           }
           return out;
         }});
  R.add({"mesh_fix",
         "Mends the mesh (see mesh_check): weld (join vertices closer than weld_distance), remove_degenerate, "
         "remove_duplicate_faces, remove_loose, remove_small_parts (smaller than small_part_size, default 3% of the "
         "model), fix_winding (faces like their neighbors, closed parts facing out), fill_holes (up to "
         "fill_hole_edges edges, default 64; the new faces take their border's UVs), or all. The paint moves to the "
         "new mesh. It clears the undo history, so a copy of the project is kept first: mesh_fix_revert goes back.",
         schema({{"all", flag("Every fix below, with its default settings (the usual first step for a downloaded or generated model)", false)},
                 {"weld", flag("Join vertices at the same place (open seams); needed before holes can be found", false)},
                 {"weld_distance", num("number", "Join vertices closer than this, object units. 0 = automatic (1e-5 of the model's size). Larger joins more but can merge separate parts that touch", 0, 1, 0)},
                 {"remove_degenerate", flag("Remove faces with no area", false)},
                 {"remove_duplicate_faces", flag("Remove a face on the same corners as another", false)},
                 {"remove_loose", flag("Remove vertices no face uses", false)},
                 {"remove_small_parts", flag("Remove specks and floaters (separate parts smaller than small_part_size)", false)},
                 {"small_part_size", num("number", "Separate parts smaller than this go, in object units (the diagonal of their box; only parts with under 1% of the faces). 0 = automatic (3% of the model's size). Raise it to remove bigger floaters; lower it to keep small real parts (buttons, eyes)", 0, 1000000, 0)},
                 {"fix_winding", flag("Turn faces like their neighbors; closed parts face out", false)},
                 {"fill_holes", flag("Close holes with a face (the new faces take their border's UVs)", false)},
                 {"fill_hole_edges", num("integer", "Only holes with up to this many edges. 0 = automatic (64). Raise it for big openings (to print: 512); real openings (a cup's top) also close", 0, 100000, 0)}}),
         [](CommandContext& c, const json& a) {
           if (!c.engine.features.on("mesh_check")) throw CommandError("the mesh check is switched off (features)");
           Document& d = c.engine.doc;
           if (!d.hasMesh()) throw CommandError("no mesh loaded");
           bool all = a.value("all", false);
           MeshFixOptions o;
           o.weld = a.value("weld", all);
           o.weldDistance = a.value("weld_distance", 0.f);
           o.removeDegenerate = a.value("remove_degenerate", all);
           o.removeDuplicateFaces = a.value("remove_duplicate_faces", all);
           o.removeLoose = a.value("remove_loose", all);
           o.removeSmallParts = a.value("remove_small_parts", all);
           o.smallPartSize = a.value("small_part_size", 0.f);
           o.fixWinding = a.value("fix_winding", all);
           o.fillHoles = a.value("fill_holes", all);
           o.fillHoleEdges = a.value("fill_hole_edges", 0);
           MeshFixResult r;
           std::string err;
           bool changed = d.fixMesh(o, &r, &err);
           if (!changed && !err.empty()) throw CommandError(err);
           if (changed && c.engine.onExternalChange) c.engine.onExternalChange("mesh");
           json out = {{"changed", changed}, {"welded", r.welded}, {"degenerate_removed", r.degenerate},
                       {"duplicates_removed", r.duplicates}, {"loose_removed", r.loose}, {"small_parts_removed", r.smallParts},
                       {"small_part_faces", r.smallPartFaces}, {"faces_turned", r.flipped}, {"holes_filled", r.holesFilled},
                       {"faces", d.mesh.faceCount()}, {"vertices", d.mesh.positions.size()}, {"can_revert", d.canRevertMeshFix()}};
           if (changed && d.bridge.active)
             out["note"] = "the mesh no longer matches Blender's object " + d.bridge.object +
                           ": its textures fit the mended mesh; export the model, or mend it in Blender";
           return out;
         }});
  R.add({"mesh_reduce",
         "Fewer polygons (decimation): keeps ratio of the triangles (or triangles), the UV layout (seams, material "
         "borders and every corner's UV stay, so the textures fit as they are) and, with keep_borders (default), open "
         "borders. preview: only say how many triangles it would leave. The undo history is cleared, so a copy of the "
         "project is kept first: mesh_fix_revert goes back.",
         schema({{"ratio", num("number", "Share of the triangles to keep: 0.5 halves them, 0.1 keeps a tenth. Lower is lighter but rounder shapes go faceted; the result's error says how far the shape moved", 0.001, 1, 0.5)},
                 {"triangles", num("integer", "Keep this many triangles instead of a ratio", 0, 100000000)},
                 {"keep_borders", flag("Open borders (a cloth's edge, a cut) don't move", true)},
                 {"max_error", num("number", "Stop early rather than let the shape move more than this share of its size: 0.01 = 1%. 1 = always reach the target", 0, 1, 1)},
                 {"preview", flag("Only say how many triangles it would leave and the error, without changing the model", false)}}),
         [](CommandContext& c, const json& a) {
           if (!c.engine.features.on("mesh_reduce")) throw CommandError("polygon reduction is switched off (features)");
           Document& d = c.engine.doc;
           if (!d.hasMesh()) throw CommandError("no mesh loaded");
           ReduceOptions o;
           o.ratio = std::clamp(a.value("ratio", 0.5f), 0.001f, 1.f);
           o.targetTriangles = (size_t)std::max(0, a.value("triangles", 0));
           o.keepBorders = a.value("keep_borders", true);
           o.maxError = std::clamp(a.value("max_error", 1.f), 0.f, 1.f);
           ReduceResult r;
           if (a.value("preview", false)) {
             Mesh q = reduceMesh(d.mesh, o, &r);
             return json{{"preview", true}, {"triangles_before", r.trianglesBefore}, {"triangles_after", r.trianglesAfter},
                         {"vertices_after", q.positions.size()}, {"error", r.error}};
           }
           std::string err;
           bool changed = d.reduceMesh(o, &r, &err);
           if (!changed && !err.empty()) throw CommandError(err);
           if (changed && c.engine.onExternalChange) c.engine.onExternalChange("mesh");
           json out = {{"changed", changed}, {"triangles_before", r.trianglesBefore}, {"triangles_after", r.trianglesAfter},
                       {"vertices", d.mesh.positions.size()}, {"error", r.error}, {"can_revert", d.canRevertMeshFix()}};
           if (changed && d.bridge.active)
             out["note"] = "the mesh no longer matches Blender's object " + d.bridge.object + ": send_model_to_blender sends it";
           return out;
         }});
  R.add({"mesh_transform",
         "Sizes, turns and places the model: scale (or from_units: the units it was made in, cm, mm, in or ft, to "
         "Blender's meters), rotate [x, y, z] in degrees around Blender's axes, origin (keep, center: the middle of "
         "its box, base: the middle of its bottom) and ground (the lowest point at height 0). The faces and UVs stay, "
         "so the paint does too. A copy of the project is kept first: mesh_fix_revert goes back.",
         schema({{"scale", num("number", "Multiply its size (after from_units)", 0.000001, 1000000, 1)},
                 {"from_units", propEnum({"m", "cm", "mm", "in", "ft"}, "The units it was made in; it is scaled to Blender's meters (cm: x0.01). Default m (no change)")},
                 {"rotate", {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 3}, {"maxItems", 3},
                             {"description", "[x, y, z] degrees around Blender's axes, applied in that order: [90, 0, 0] stands up a model lying on its back"}}},
                 {"origin", propEnum({"keep", "center", "base"}, "Where the origin goes: center = the middle of its box, base = the middle of its bottom (for props that stand on the floor). Default keep")},
                 {"ground", flag("Move it so its lowest point is at height 0", false)}}),
         [](CommandContext& c, const json& a) {
           if (!c.engine.features.on("recipes")) throw CommandError("recipes and batches are switched off (features)");
           Document& d = c.engine.doc;
           TransformOptions o;
           o.scale = a.value("scale", 1.f);
           std::string u = a.value("from_units", std::string("m"));
           o.scale *= u == "cm" ? 0.01f : u == "mm" ? 0.001f : u == "in" ? 0.0254f : u == "ft" ? 0.3048f : 1.f;
           if (a.contains("rotate")) o.rotateDeg = vec3(a["rotate"][0], a["rotate"][1], a["rotate"][2]);
           o.origin = a.value("origin", std::string("keep"));
           if (o.origin == "centre") o.origin = "center";  // either spelling
           o.ground = a.value("ground", false);
           std::string err;
           bool changed = d.transformMesh(o, &err);
           if (!changed && !err.empty()) throw CommandError(err);
           if (changed && c.engine.onExternalChange) c.engine.onExternalChange("mesh");
           vec3 sz = d.mesh.bounds.valid() ? d.mesh.bounds.hi - d.mesh.bounds.lo : vec3(0);
           return json{{"changed", changed}, {"size", json::array({sz.x, sz.z, sz.y})}, {"can_revert", d.canRevertMeshFix()}};
         }});
  R.add({"mesh_fix_revert",
         "Goes back to the project as it was before the last mesh_fix, mesh_reduce or mesh_transform (mesh, layers and paint).",
         schema(),
         [](CommandContext& c, const json&) {
           Document& d = c.engine.doc;
           std::string err;
           if (!d.revertMeshFix(c.engine.materials, &err)) throw CommandError(err);
           if (c.engine.onExternalChange) c.engine.onExternalChange("mesh");
           return json{{"reverted", true}, {"faces", d.mesh.faceCount()}, {"layers", d.layers.size()}};
         }});
  R.add({"mesh_symmetry",
         "Measures how far the mesh is from mirror-symmetric across X (Blender's X): every vertex is mirrored and "
         "its distance to the surface on the other side is measured. Reports the worst spots (positions in "
         "Blender axes), the vertices beyond tolerance (0.1% of the model's size) and a breakdown per material slot "
         "and height band. show: true switches the viewport to the Symmetry heat map.",
         schema({{"plane_x", num("number", "Mirror plane x (object units); kept until changed; null = automatic (the default)", -1000000, 1000000)},
                 {"show", prop("boolean", "Switch the viewport to the Symmetry view")},
                 {"vertices", prop("boolean", "Also list the vertex indices beyond tolerance and without a match (Blender's vertex order for meshes sent from Blender)")}}),
         [](CommandContext& c, const json& a) {
           if (!c.engine.features.on("symmetry_check")) throw CommandError("the symmetry check is switched off (features)");
           Document& d = c.engine.doc;
           if (!d.hasMesh()) throw CommandError("no mesh loaded");
           if (a.contains("plane_x"))  // a number sets the plane, null goes back to automatic
             d.symmetryPlane = a["plane_x"].is_number() ? a["plane_x"].get<float>() : NAN;
           const SymmetryReport& r = d.symmetry();
           if (a.value("show", false)) c.engine.viewOptions.mode = ViewMode::Symmetry;
           auto blender = [](vec3 p) { return json::array({p.x, -p.z, p.y}); };
           json worst = json::array();
           for (auto& s : r.worst) worst.push_back({{"position", blender(s.position)}, {"distance", s.dist},
                                                    {"tolerances", s.dist / std::max(r.tolerance, 1e-12f)}, {"vertex", s.vertex}});
           auto groups = [](const std::vector<SymmetryReport::Group>& g) {
             json out = json::array();
             for (auto& x : g) out.push_back({{"name", x.name}, {"vertices", x.vertices}, {"beyond", x.beyond},
                                              {"max", x.maxDist}, {"mean", x.meanDist}});
             return out;
           };
           std::string verdict = r.unmatched * 50 > std::max(r.vertices, 1) ? "not mirror-shaped"
                               : r.beyond == 0 ? "symmetric"
                               : r.maxDist < r.tolerance * 10 ? "slightly asymmetric" : "asymmetric";
           json out = {{"verdict", verdict}, {"plane_x", r.planeX}, {"size", r.size}, {"tolerance", r.tolerance},
                       {"max", r.maxDist}, {"mean", r.meanDist}, {"median", r.median}, {"p95", r.p95},
                       {"vertices", r.vertices}, {"beyond", r.beyond}, {"unmatched", r.unmatched},
                       {"worst", worst}, {"by_slot", groups(r.bySlot)}, {"by_height", groups(r.byHeight)},
                       {"object", d.bridge.active ? d.bridge.object : std::string()}};
           if (a.value("vertices", false)) {
             json bv = json::array(), uv = json::array();
             for (uint32_t i = 0; i < r.vertexDist.size(); ++i) {
               float dd = r.vertexDist[i];
               if (dd > r.tolerance) bv.push_back(i);
               else if (dd < 0 && i < d.mesh.positions.size()) uv.push_back(i);
             }
             out["beyond_vertices"] = bv;
             out["unmatched_vertices"] = uv;
           }
           return out;
         }});
  R.add({"auto_texture",
         "Guesses a library material for each material slot from its name (Skin, Jeans, M_Boots_Leather, Hair.001...) "
         "and adds a fill layer per slot, masked to that slot's faces, in an 'Auto texture' folder at the bottom of "
         "the stack. Slots named after parts that want painting by hand (eyes, teeth, glass, decals) or with no "
         "confident match are skipped. apply: false only reports the guesses.",
         schema({{"apply", prop("boolean", "Add the layers (default true); false = only report the guesses")},
                 {"style", propEnum({"any", "realistic", "stylized"}, "Prefer realistic or stylized materials")},
                 {"overrides", prop("object", "Slot name or index -> material id, to replace a guess (\"\" skips the slot)")},
                 {"mapping", propEnum({"triplanar", "uv"}, "How the materials are projected (default triplanar)")}}),
         [](CommandContext& c, const json& a) {
           if (!c.engine.features.on("auto_texture")) throw CommandError("auto texture is switched off (features)");
           Engine& e = c.engine;
           Document& d = e.doc;
           if (!d.hasMesh()) throw CommandError("no mesh loaded");
           std::string style = a.value("style", std::string("any"));
           auto matches = matchSlotMaterials(d.mesh, e.materials, style == "any" ? "" : style);
           json ov = a.value("overrides", json::object());
           for (auto& m : matches) {
             const json* o = nullptr;
             if (ov.contains(m.slotName)) o = &ov[m.slotName];
             else if (ov.contains(std::to_string(m.slot))) o = &ov[std::to_string(m.slot)];
             if (!o) continue;
             std::string id = o->get<std::string>();
             if (id.empty()) {
               m.materialId.clear(), m.materialName.clear();
               continue;
             }
             auto* ent = e.materials.find(id);
             if (!ent) throw CommandError("unknown material '" + id + "' for slot " + m.slotName);
             m.materialId = id, m.materialName = ent->def.name, m.score = 99.f;
           }
           bool apply = a.value("apply", true);
           int added = 0;
           if (apply) {
             const Command* addLayer = e.commands.find("add_layer");
             int folder = -1;
             for (auto& m : matches) {
               if (m.materialId.empty()) continue;
               if (folder < 0) {
                 json f = addLayer->run(c, {{"type", "folder"}, {"name", "Auto texture"}, {"index", 0}});
                 folder = f["index"].get<int>();
               }
               json args = {{"type", "fill"}, {"parent", folder},
                            {"name", m.slotName + ": " + m.materialName},
                            {"material", {{"id", m.materialId}, {"mapping", a.value("mapping", std::string("triplanar"))}}},
                            {"generator", {{"type", "material"}, {"slot", m.slot}}}};
               json r = addLayer->run(c, args);
               folder = d.indexOf(d.layers[r["index"].get<int>()]->parent);
               ++added;
             }
           }
           json list = json::array();
           for (auto& m : matches)
             list.push_back({{"slot", m.slot}, {"slot_name", m.slotName}, {"faces", m.faces}, {"material", m.materialId},
                             {"material_name", m.materialName}, {"score", m.score}, {"alternatives", m.alternatives}});
           return json{{"slots", list}, {"layers_added", added}};
         }});
  R.add({"export_textures",
         "Exports the final texture maps as PNG files, or as layered PSD files (format psd: one per map, a "
         "layer per Patina layer with its mask, opacity and blend mode; a set per texture set / UDIM tile). "
         "Sizes above the texture set render "
         "in tiles at full detail (up to 32768).",
         schema({{"directory", prop("string", "Output folder")},
                 {"format", propEnum({"png", "psd"}, "png (default) or layered psd")},
                 {"preset", propEnum({"blender", "gltf", "unreal", "unity"}, "Map layout/naming preset")},
                 {"size", num("integer", "Output size in pixels; 0 = the texture set's. Up to 32768 (rendered in tiles above the texture set's size)", 0, 32768, 0)},
                 {"base_name", prop("string", "Value of $mesh (default: mesh name)")},
                 {"name_template", prop("string", "File name template, default \"$mesh_$map\". Tokens: $mesh $project $map $size $preset $layer")},
                 {"layer", num("integer", "Export only this layer / folder (its index), as if soloed", 0, 100000)},
                 {"mask", prop("boolean", "With layer: export that layer's painted mask as a grayscale PNG")},
                 {"include_ao", prop("boolean", "Write the AO map when baked (default true)")},
                 {"include_thickness", prop("boolean", "Write the thickness map when baked (default true)")},
                 {"include_bent_normal", prop("boolean", "Write the bent normal map when baked (default true)")},
                 {"dry_run", prop("boolean", "(0.47) Write nothing: answer `plan`, the files it would write, each with "
                                             "what its channels hold, its bits, size and color space (srgb false: data, "
                                             "Blender's Non-Color)")},
                 {"objects", propEnum({"active", "all"}, "(1.2) A scene of several objects: the active object's maps (the default), or every object's shown, each set named after its object ($mesh)")}},
                {"directory"}),
         [](CommandContext& c, const json& a) {
           ExportOptions o;
           o.directory = a.at("directory");
           o.preset = exportPresetFromName(a.value("preset", std::string("blender")));
           o.size = a.value("size", 0);
           o.baseName = a.value("base_name", std::string());
           o.nameTemplate = a.value("name_template", o.nameTemplate);
           if (a.contains("layer")) {
             int i = a["layer"];
             if (i < 0 || i >= (int)c.engine.doc.layers.size()) throw CommandError("invalid layer index");
             o.layerUid = c.engine.doc.layers[i]->uid;
           }
           o.maskOnly = a.value("mask", false);
           o.includeAO = a.value("include_ao", true);
           o.psd = a.value("format", std::string("png")) == "psd";
           if (o.size > 32768) throw CommandError("size: at most 32768");
           o.includeThickness = a.value("include_thickness", true);
           o.includeBentNormal = a.value("include_bent_normal", true);
           if (a.value("dry_run", false)) {
             json plan = json::array();
             for (auto& f : c.engine.doc.exportPlan(o))
               plan.push_back({{"map", f.key}, {"file", f.file}, {"contents", f.contents}, {"channels", f.channels},
                               {"bits", f.bits}, {"size", f.size}, {"srgb", f.srgb}});
             return json{{"plan", plan}};
           }
           std::map<std::string, std::string> files;
           std::string err;
           json per;
           bool all = a.value("objects", std::string("active")) == "all" && c.engine.objectCount() > 1;
           if (all && o.layerUid) throw CommandError("layer: one object's layer (objects: active)");
           if (!c.engine.exportSceneTextures(o, all, &files, &per, &err)) throw CommandError(err);
           json r = {{"files", files}};
           if (all) r["objects"] = per;
           return r;
         }});

  // ---------------------------------------------------------------- materials
  R.add({"list_materials", "Lists library materials (id, name, category, type).",
         schema({{"category", prop("string", "Optional category filter")}}),
         [](CommandContext& c, const json& a) {
           json out = json::array();
           std::string cat = a.value("category", std::string());
           for (auto& e : c.engine.materials.entries()) {
             if (e->transient || (!cat.empty() && e->def.category != cat)) continue;
             const char* t = e->def.type == MaterialType::Flat ? "flat" : e->def.type == MaterialType::Image ? "image" : "procedural";
             out.push_back({{"id", e->def.id}, {"name", e->def.name}, {"category", e->def.category}, {"type", t},
                            {"description", e->def.description}});
           }
           return out;
         }});
  R.add({"get_material", "Full definition of a library material, optionally with a preview image.",
         schema({{"id", prop("string", "Material id")}, {"preview", prop("boolean", "Include a 128px sphere preview")}},
                {"id"}),
         [](CommandContext& c, const json& a) {
           auto* e = c.engine.materials.find(a.at("id"));
           if (!e) throw CommandError("unknown material");
           json r = e->def.toJson();
           if (a.value("preview", false)) {
             GLuint t = c.engine.materials.thumbnail(*e, c.engine.env);
             Texture2D* tex = &e->gpu.thumbnail;
             (void)t;
             Image img = tex->download(4);
             auto png = encodePng(img);
             r = imageResult(png, img.width, img.height, r);
           }
           return r;
         }});
  R.add({"export_material_maps",
         "Bakes library materials and writes their tiling maps (<id>_color, _rmh [r rough, g metal, b "
         "height], _normal [OpenGL], _emissive, _opacity [see-through materials], _thumb) plus manifest.json (with each "
         "material's average values, sheen, clearcoat and subsurface among them). Used to build the web painter "
         "library and Blender's Patina asset library; handy for using Patina materials in other tools.",
         schema({{"directory", prop("string", "Output folder")},
                 {"ids", prop("array", "Material ids (default: see which)")},
                 {"which", propEnum({"builtin", "user", "all"}, "Without ids: the built-in materials (default), your own, or both")},
                 {"size", num("integer", "Map size in pixels", 32, 4096, 512)},
                 {"format", propEnum({"png", "jpg"}, "Color, normal and glow maps as PNG (default) or JPEG; the rmh map is always PNG")},
                 {"quality", num("integer", "JPEG quality: lower is smaller and softer", 1, 100, 92)},
                 {"height16", prop("boolean", "Also write the height alone as a 16-bit PNG (<id>_height; smooth bump mapping)")},
                 {"thumbnails", prop("boolean", "Also write sphere previews (default true)")}},
                {"directory"}),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           fs::path dir = paths::ensureDir(a.at("directory").get<std::string>());
           int size = std::clamp(a.value("size", 512), 32, 4096);
           bool thumbs = a.value("thumbnails", true);
           bool jpg = a.value("format", std::string("png")) == "jpg";
           int quality = std::clamp(a.value("quality", 92), 1, 100);
           bool height16 = a.value("height16", false);
           std::string which = a.value("which", std::string("builtin"));
           if (which != "builtin" && which != "user" && which != "all") throw CommandError("which: builtin, user or all");
           std::vector<MaterialLibrary::Entry*> list;
           if (a.contains("ids")) {
             for (auto& id : a["ids"]) {
               auto* m = e.materials.find(id.get<std::string>());
               if (!m) throw CommandError("unknown material " + id.get<std::string>());
               list.push_back(m);
             }
           } else {
             for (auto& m : e.materials.entries())
               if (!m->transient && (which == "all" || m->def.builtin == (which == "builtin"))) list.push_back(m.get());
           }
           auto save = [&](const Texture2D& t, const fs::path& p) {
             Image img = t.download(4);
             auto png = encodePng(img);
             paths::writeFileAtomic(p, std::string(png.begin(), png.end()));
           };
           // color, normal and glow maps: JPEG when asked (much smaller); returns the file name
           auto saveMap = [&](const Texture2D& t, const fs::path& dir, const std::string& stem) {
             if (!jpg) {
               save(t, dir / (stem + ".png"));
               return stem + ".png";
             }
             Image img = t.download(3);
             auto bytes = encodeJpg(img, quality);
             paths::writeFileAtomic(dir / (stem + ".jpg"), std::string(bytes.begin(), bytes.end()));
             return stem + ".jpg";
           };
           json manifest = json::array();
           for (auto* m : list) {
             // flat materials bake to 1x1 colors; give them a small tile so every entry looks the same
             int sz = m->def.type == MaterialType::Flat ? 4 : size;
             int keep = m->gpu.bakedSize;
             if (m->def.type == MaterialType::Procedural && keep != sz) m->gpu = MaterialGpu();
             if (!e.materials.ensureBaked(*m, sz)) continue;
             std::string id = paths::sanitizeFileName(m->def.id);
             std::string colorFile = saveMap(m->gpu.color, dir, id + "_color");
             save(m->gpu.rmh, dir / (id + "_rmh.png"));
             std::string normalFile = saveMap(m->gpu.normal, dir, id + "_normal");
             json item = m->def.toJson();
             item["builtin"] = m->def.builtin;
             item["maps"] = {{"color", colorFile}, {"rmh", id + "_rmh.png"}, {"normal", normalFile}};
             FlatValues avg = e.materials.averageValues(*m);
             item["average"] = {{"color", json::array({avg.color.x, avg.color.y, avg.color.z})}, {"roughness", avg.roughness},
                                {"metallic", avg.metallic}, {"opacity", avg.opacity}, {"sheen", avg.sheen},
                                {"coat", avg.coat}, {"subsurface", avg.subsurface}};
             MaterialRef ref;
             ref.id = m->def.id;
             if ((e.materials.extraChannelsUsed(ref) & 64) && m->gpu.extra.valid()) {
               // see-through parts (lace, nets, leaves): the opacity alone, as a gray PNG
               Image x = m->gpu.extra.download(4), op;
               op.resize(x.width, x.height, 1);
               for (size_t i = 0; i < op.pixels.size(); ++i) op.pixels[i] = x.pixels[i * 4];
               auto png = encodePng(op);
               paths::writeFileAtomic(dir / (id + "_opacity.png"), std::string(png.begin(), png.end()));
               item["maps"]["opacity"] = id + "_opacity.png";
             }
             if (glm::length(avg.emissive) > 0.002f || m->def.type == MaterialType::Flat) {
               item["maps"]["emissive"] = saveMap(m->gpu.emissive, dir, id + "_emissive");
             }
             if (height16 && m->def.type != MaterialType::Flat) {
               Image rmh = m->gpu.rmh.download(4), h;
               h.resize(rmh.width, rmh.height, 1);
               for (size_t i = 0; i < h.pixels.size(); ++i) h.pixels[i] = rmh.pixels[i * 4 + 2];
               auto png = encodePng16(h);
               paths::writeFileAtomic(dir / (id + "_height.png"), std::string(png.begin(), png.end()));
               item["maps"]["height"] = id + "_height.png";
             }
             if (thumbs) {
               e.materials.thumbnail(*m, e.env);
               save(m->gpu.thumbnail, dir / (id + "_thumb.png"));
               item["thumbnail"] = id + "_thumb.png";
             }
             manifest.push_back(item);
             if (m->def.type == MaterialType::Procedural && keep && keep != sz) m->gpu = MaterialGpu();  // re-bake lazily
           }
           json out = {{"library", "Patina default library"}, {"materials", manifest}};
           paths::writeFileAtomic(dir / "manifest.json", out.dump(1));
           return json{{"count", manifest.size()}, {"directory", dir.string()}};
         }});
  R.add({"list_generators",
         "The procedural generators (create_material's generator): each one's name, group, what colors A, B and C "
         "are for, and what its four extra values do (label, range, the default that 0 stands for, the choices of a "
         "style setting).",
         schema({{"generator", prop("string", "Only this generator")}}),
         [](CommandContext&, const json& a) {
           json out = json::array();
           auto names = generatorNames();
           std::string only = a.value("generator", std::string());
           for (int i = 0; i < (int)names.size(); ++i) {
             const GeneratorInfo& g = generatorInfo(i);
             if (!only.empty() && only != g.id) continue;
             json cols = json::object();
             std::string c = g.colors;
             for (int k = 0; k < 3; ++k) {
               size_t bar = c.find('|');
               std::string part = c.substr(0, bar);
               if (!part.empty()) cols[std::string(1, char('a' + k))] = part;
               c = bar == std::string::npos ? "" : c.substr(bar + 1);
             }
             json extra = json::array();
             for (int k = 0; k < 4; ++k) {
               const GeneratorSetting& st = g.extra[k];
               if (!st.label) {
                 extra.push_back(nullptr);
                 continue;
               }
               json e = {{"label", st.label}, {"min", st.lo}, {"max", st.hi}};
               if (st.def > 0.f) e["zero_means"] = st.def;
               if (st.options) {
                 json o = json::array();
                 std::string opts = st.options;
                 for (size_t p = 0;;) {
                   size_t q = opts.find('|', p);
                   o.push_back(opts.substr(p, q == std::string::npos ? std::string::npos : q - p));
                   if (q == std::string::npos) break;
                   p = q + 1;
                 }
                 e["choices"] = o;
               }
               if (st.help) e["help"] = st.help;
               extra.push_back(e);
             }
             out.push_back({{"id", g.id}, {"name", g.name}, {"group", g.group}, {"colors", cols}, {"extra", extra}});
           }
           if (!only.empty() && out.empty()) throw CommandError("no generator '" + only + "'");
           return out;
         }});
  R.add({"create_material",
         "Creates or updates a user material in the library. type 'procedural' uses a generator (" +
             [] { std::string g; for (auto& n : generatorNames()) g += (g.empty() ? "" : " ") + n; return g; }() +
             ") with params {color_a,color_b,color_c (sRGB 0-1 or "
         "#hex), roughness [a,b], metallic [a,b], scale, detail, seed, height, variation, normal_strength, extra[4], "
         "emissive, and for every generator warp, warp_scale (wobble), grime, grime_color (dirt in the low parts), wear, "
         "wear_color (worn raised edges), opacity, sheen, coat (clearcoat), subsurface (0-1 each), cutout (true: the "
         "holes of net, lace, leaves, hair cards, grilles... are see-through)}; list_generators says what each "
         "generator's colors and extra values are for. type 'flat' takes color/roughness/metallic/height/emissive/"
         "opacity/sheen/coat/subsurface.",
         schema({{"id", prop("string", "Unique id (default derived from name)")},
                 {"name", prop("string", "Display name")},
                 {"category", prop("string", "Category")},
                 {"type", propEnum({"procedural", "flat"}, "Material type")},
                 {"generator", prop("string", "Procedural generator name")},
                 {"params", prop("object", "Generator parameters")},
                 {"color", prop("array", "Flat: sRGB color")},
                 {"roughness", num("number", "Flat roughness: 0 mirror-smooth, 1 fully matte", 0, 1)},
                 {"metallic", num("number", "Flat metallic: 0 non-metal, 1 metal (in between only along borders)", 0, 1)},
                 {"opacity", num("number", "Flat opacity: 1 opaque, 0 invisible", 0, 1, 1)},
                 {"sheen", num("number", "Flat sheen: a soft bright rim at grazing angles (velvet, satin)", 0, 1, 0)},
                 {"coat", num("number", "Flat clearcoat: a smooth varnish over the surface (lacquer, car paint)", 0, 1, 0)},
                 {"subsurface", num("number", "Flat subsurface: light going in and out softened (skin, wax, leaves)", 0, 1, 0)},
                 {"default_scale", num("number", "Pattern repeats per object unit: higher is a smaller pattern", 0.01, 100)}},
                {"name"}),
         [](CommandContext& c, const json& a) {
           json j = a;
           if (!j.contains("id")) {
             std::string id = "user_" + paths::sanitizeFileName(a.at("name").get<std::string>());
             for (auto& ch : id) ch = ch == ' ' ? '_' : (char)tolower(ch);
             j["id"] = id;
           }
           j["category"] = a.value("category", std::string("User"));
           MaterialDef d;
           std::string err;
           if (!MaterialDef::fromJson(j, d, "", &err)) throw CommandError(err);
           auto* e = c.engine.materials.saveUserMaterial(d, &err);
           if (!e) throw CommandError(err);
           c.engine.markSceneComposite();  // (fills with it change in every object)
           if (c.engine.onExternalChange) c.engine.onExternalChange("materials");
           return json{{"id", d.id}};
         }});
  R.add({"import_material_folder",
         "Imports a folder of PBR texture maps as library materials. Understands ambientCG, Poly Haven, Quixel and "
         "Substance export naming, packed maps (Unreal OcclusionRoughnessMetallic / ORM / ARM, glTF "
         "metallicRoughness, Unity MaskMap and MetallicSmoothness), OpenGL / DirectX normals, EXR, HDR and 16-bit "
         "maps. A Substance Painter export of several texture sets in one folder gives one material per texture set. "
         ".sbsar / .spsm files are not readable (Adobe's engine only): export their outputs as images first.",
         schema({{"path", prop("string", "Folder path")},
                 {"name", prop("string", "Material name (with several texture sets: the name prefix)")},
                 {"normal", propEnum({"auto", "opengl", "directx"},
                                     "Normal map convention (auto: from the file name, or DirectX for Substance's "
                                     "Unreal preset)")}},
                {"path"}),
         [](CommandContext& c, const json& a) {
           std::string err;
           MaterialLibrary::ImageImportOptions o;
           o.name = a.value("name", std::string());
           std::string nc = a.value("normal", std::string("auto"));
           o.normal = nc == "opengl" ? MaterialLibrary::NormalConvention::OpenGL
                    : nc == "directx" ? MaterialLibrary::NormalConvention::DirectX
                                      : MaterialLibrary::NormalConvention::Auto;
           std::vector<MaterialLibrary::ImportReport> rep;
           auto mats = c.engine.materials.importImageFolderAll(a.at("path"), o, &rep, &err);
           if (mats.empty()) throw CommandError(err);
           json list = json::array();
           for (size_t i = 0; i < mats.size(); ++i) {
             json maps = json::object();
             for (auto& [k, p] : mats[i]->def.maps) maps[k] = p;
             json from = json::object();
             if (i < rep.size())
               for (auto& [k, f] : rep[i].files) from[k] = f;
             list.push_back({{"id", mats[i]->def.id}, {"name", mats[i]->def.name}, {"maps", maps}, {"from", from},
                             {"normal_directx", mats[i]->def.normalDirectX},
                             {"notes", i < rep.size() ? json(rep[i].notes) : json::array()}});
           }
           if (c.engine.onExternalChange) c.engine.onExternalChange("materials");
           // the first material's id / maps at the top level, as before 0.5
           return json{{"id", list[0]["id"]}, {"maps", list[0]["maps"]}, {"materials", list}};
         }});
  R.add({"import_image_material",
         "Imports a single image (logo, decal, photo) as a color material. Paint it with mapping 'screen' to "
         "project it from the view, or 'uv'.",
         schema({{"path", prop("string", "Image file (png, jpg, tga)")}}, {"path"}),
         [](CommandContext& c, const json& a) {
           std::string err;
           auto* e = c.engine.materials.importImageFile(a.at("path"), &err);
           if (!e) throw CommandError(err);
           return json{{"id", e->def.id}};
         }});
  R.add({"set_paint_material",
         "Sets what the brush paints: a library material (id) and/or flat values, plus projection settings.",
         schema({{"id", prop("string", "Library material id; empty string = flat values")},
                 {"color", prop("array", "Flat sRGB color [r,g,b] 0-1 or '#rrggbb'")},
                 {"roughness", num("number", "Flat roughness: 0 mirror-smooth, 1 fully matte", 0, 1)},
                 {"metallic", num("number", "Flat metallic: 0 non-metal, 1 metal (in between only along borders)", 0, 1)},
                 {"height", num("number", "Flat height: 0.5 neutral, above raises (bumps), below lowers (dents)", 0, 1)},
                 {"emissive", prop("array", "Flat emissive sRGB color")},
                 {"opacity", num("number", "Flat opacity: 1 opaque, 0 invisible (lace, cut-outs)", 0, 1)},
                 {"sheen", num("number", "Flat sheen: a soft bright rim at grazing angles (velvet)", 0, 1)},
                 {"coat", num("number", "Flat clearcoat: a smooth varnish (lacquer, car paint)", 0, 1)},
                 {"subsurface", num("number", "Flat subsurface: light going in and out softened (skin, wax)", 0, 1)},
                 {"mapping", propEnum({"triplanar", "uv", "screen"}, "Material projection")},
                 {"scale", num("number", "Multiplies the material's pattern repeats per unit: 2 = pattern half the size, 0.5 = twice the size", 0.05, 20)},
                 {"rotation", num("number", "Turns the projected pattern, degrees", -180, 180)},
                 {"triplanar_sharpness", num("number", "Tri-planar: how quickly the three projections hand over at corners: 1 wide soft blends, 16 crisp seams", 1, 16, 4)},
                 {"triplanar_blend", propEnum({"height", "smooth"}, "Tri-planar seams: by height (crisp, default) or a smooth cross-fade")},
                 {"tint", prop("array", "Color multiplier [r,g,b]")},
                 {"roughness_scale", num("number", "Multiplies the material's roughness: below 1 shinier, above 1 more matte", 0, 4)},
                 {"roughness_offset", num("number", "Added to the material's roughness", -1, 1)},
                 {"metallic_override", num("number", "Forces the metallic value everywhere; -1 keeps the material's own", -1, 1)},
                 {"height_scale", num("number", "Scales the height relief: 0 flat, 2 twice as deep", 0, 4)},
                 {"normal_strength", num("number", "Scales the normal detail: 0 none, 2 twice as strong", 0, 4)}}),
         [](CommandContext& c, const json& a) {
           applyMaterialArgs(c.engine, a);
           return c.engine.painter.material.toJson();
         }});

  // ---------------------------------------------------------------- brushes / tools
  R.add({"list_brushes",
         "Lists brush presets: Patina's, saved ones, and (1.2) a brush for each image in the user's brushes folder "
         "(and its folders, a category each) and each tip of a Photoshop .abr file there (ids file:<path>).",
         schema(), [](CommandContext& c, const json&) {
           json out = json::array();
           for (auto& p : c.engine.brushes.presets()) {
             json b = {{"id", p.id}, {"name", p.name}, {"category", p.category}, {"alpha", p.alpha.shape}};
             if (!p.alpha.image.empty()) b["image"] = p.alpha.image;
             out.push_back(b);
           }
           return out;
         }});
  R.add({"reload_brushes",
         "Looks at the user's brushes folder again (1.2): new images and .abr files there become brushes, removed ones "
         "go (Patina also looks every few seconds by itself). Returns the folder and how many brushes there are.",
         schema(), [](CommandContext& c, const json&) {
           int changed = c.engine.brushes.rescan();
           if (!c.engine.brushes.find(c.engine.selectedBrushId)) c.engine.selectBrush("basic_hard");
           return json{{"folder", BrushLibrary::userDir().string()}, {"brushes", c.engine.brushes.presets().size()}, {"change", changed}};
         }});
  R.add({"import_brushes",
         "Copies brush images (PNG, JPEG, TGA, BMP, GIF, PSD, EXR) or Photoshop brush files (.abr: each sampled tip "
         "a brush) into the user's brushes folder, in a folder named category if given (the Brushes panel's group), "
         "and makes brushes of them (1.2). A tip that is dark on a light ground paints with its dark part, light on "
         "dark with the light part, one with transparency with its opaque part. select: paint with the first one.",
         schema({{"paths", prop("array", "Files to import")},
                 {"category", prop("string", "Its group in the Brushes panel (a folder of that name; default the top)")},
                 {"select", prop("boolean", "Paint with the first brush imported (default true)")}},
                {"paths"}),
         [](CommandContext& c, const json& a) {
           std::vector<std::string> files;
           for (auto& f : a.at("paths")) files.push_back(f.get<std::string>());
           std::string err;
           auto ids = c.engine.brushes.importFiles(files, a.value("category", std::string()), &err);
           if (ids.empty()) throw CommandError(err.empty() ? "no brush came of those files" : err);
           if (a.value("select", true)) c.engine.selectBrush(ids[0]);
           json r = {{"ids", ids}, {"folder", BrushLibrary::userDir().string()}};
           if (!err.empty()) r["warning"] = err;
           return r;
         }});
  R.add({"set_brush",
         "Selects a brush preset and/or overrides parameters. Sizes are in viewport pixels (diameter); for UV strokes "
         "in texels.",
         schema({{"preset", prop("string", "Preset id from list_brushes")},
                 {"size", num("number", "Diameter in screen pixels", 1, 1000)},
                 {"hardness", num("number", "Edge: 0 soft falloff, 1 hard edge", 0, 1)},
                 {"opacity", num("number", "The most one stroke can add (a stroke does not build past it)", 0, 1)},
                 {"flow", num("number", "How much each dab adds: low flow builds up gradually within a stroke", 0.01, 1)},
                 {"spacing", num("number", "Distance between dabs as a share of the diameter: small is smooth, large is dotted", 0.01, 3)},
                 {"angle", num("number", "Tip angle in degrees", -180, 180)},
                 {"angle_jitter", num("number", "Random tip rotation per dab: 1 = any angle", 0, 1)},
                 {"follow_path", prop("boolean", "Rotate dabs along the stroke")},
                 {"size_jitter", num("number", "Random size per dab: 1 = anything from nothing to full size", 0, 1)},
                 {"flow_jitter", num("number", "Random flow per dab", 0, 1)},
                 {"scatter", num("number", "Random sideways offset of dabs, as a share of the diameter", 0, 3)},
                 {"roundness", num("number", "Tip roundness: 1 round, lower squashes it to an ellipse", 0.05, 1)},
                 {"alpha", prop("string", "Tip shape: round square star splatter noise scratches grunge dots hatch cracks bristle spray leaf ring cloud rust")},
                 {"projection", propEnum({"screen", "surface", "uv"}, "Dab projection")},
                 {"backface_culling", prop("boolean", "Do not paint faces pointing away")},
                 {"occlusion", prop("boolean", "Only paint visible surfaces (false = paint through)")},
                 {"angle_falloff", num("number", "Fades the paint on surfaces seen at a grazing angle: 0 off, 0.1 to 0.6 typical", 0, 0.8)},
                 {"lazy_radius", num("number", "Lazy mouse: the stroke trails the cursor by this many pixels, for smooth lines", 0, 100)},
                 {"lock_topology", prop("boolean", "Topology lock: a stroke only paints the connected part of the mesh it starts on")},
                 {"lock_island", prop("boolean", "Island lock: a stroke only paints the UV island it starts on")},
                 {"lock_set", prop("boolean", "Set lock: a stroke only paints the texture set (or material slot) it starts on")},
                 {"lock_face_set", prop("boolean", "(1.2) Lock Face Sets: a stroke only paints the face set it starts on")},
                 {"mask_value", num("number", "The value the brush paints into a mask, or into a baked map layer (its map's value: "
                                              "ambient occlusion 1 = no shadow, curvature 0.5 = flat, thickness 1 = thick)", 0, 1)},
                 {"tool", propEnum({"paint", "erase", "blur", "clone", "smudge"},
                                   "(1.2.13) Change this tool's own brush instead of the current tool's (feature tool_brushes; "
                                   "get_state tool_brushes lists them). The current tool stays")}}),
         [](CommandContext& c, const json& a) {
           if (a.value("lock_set", false) && !c.engine.features.on("texture_set_tools"))  // before anything changes
             throw CommandError("the texture set tools are switched off (features)");
           Engine& E = c.engine;
           int other = -1;
           if (a.contains("tool")) {
             if (!E.features.on("tool_brushes")) throw CommandError("a brush for each tool is switched off (features: tool_brushes)");
             int s = Engine::brushSlotNamed(a["tool"].get<std::string>());
             if (s < 0) throw CommandError("tool: paint, erase, blur, clone or smudge");
             if (E.painter.active()) throw CommandError("a stroke is being painted");
             if (s != E.brushSlot) other = E.brushSlot, E.useToolBrush(Engine::brushSlotTool(s));
           }
           json args = a;
           args.erase("tool");
           try {
             applyBrushArgs(E, args);
           } catch (...) {
             if (other >= 0) E.useToolBrush(Engine::brushSlotTool(other));
             throw;
           }
           json r = E.painter.brush.toJson();  // (the brush that was set, before going back to the current tool's)
           json toolOf = E.features.on("tool_brushes") ? json(Engine::brushSlotName(E.brushSlot)) : json("every");
           if (other >= 0) E.useToolBrush(Engine::brushSlotTool(other));
           Painter& P = c.engine.painter;
           if (a.contains("lock_topology")) P.lockTopology = a["lock_topology"].get<bool>();
           if (a.contains("lock_island")) P.lockIsland = a["lock_island"].get<bool>();
           if (a.contains("lock_set")) P.lockSet = a["lock_set"].get<bool>();
           if (a.contains("lock_face_set")) P.lockFaceSet = a["lock_face_set"].get<bool>();
           r["mask_value"] = P.maskValue;
           r["lock_topology"] = P.lockTopology;
           r["lock_island"] = P.lockIsland;
           r["lock_set"] = P.lockSet;
           r["lock_face_set"] = P.lockFaceSet;
           r["tool"] = toolOf;
           return r;
         }});
  R.add({"save_brush_preset", "Saves the current brush settings as a user preset.",
         schema({{"id", prop("string", "Preset id")}, {"name", prop("string", "Display name")},
                 {"category", prop("string", "Category")}},
                {"id"}),
         [](CommandContext& c, const json& a) {
           BrushPreset p = c.engine.painter.brush;
           p.id = a.at("id");
           p.name = a.value("name", p.id);
           p.category = a.value("category", std::string("User"));
           std::string err;
           if (!c.engine.brushes.saveUserPreset(p, &err)) throw CommandError(err);
           return json{{"id", p.id}};
         }});
  R.add({"set_tool", "Selects the active tool (in the window: also the tools that are not brushes, such as select, "
                     "seams, island, note and the retopology tools). (1.2.13) Paint, erase, blur, clone and smudge each "
                     "keep their own brush (feature tool_brushes): set_tool brings the tool's brush, set_brush then "
                     "changes it.",
         schema({{"tool", propEnum({"paint", "erase", "fill", "picker", "blur", "clone", "smudge", "select", "seams", "island",
                                    "note", "zone_lines", "zones", "flow", "density"}, "Tool")},
                 {"blur_radius", num("number", "Blur tool radius in texels: larger smooths more", 1, 64, 6)},
                 {"seam_aware", prop("boolean", "Blur and smudge reach across UV seams (default true)")},
                 {"fill_mode", propEnum({"face", "uv_island", "connected", "mesh", "selection"}, "Polygon fill mode")}},
                {"tool"}),
         [](CommandContext& c, const json& a) {
           std::string t = a.at("tool");
           Painter& p = c.engine.painter;
           // (0.36) the window follows every request, also to the tool it already paints with
           c.engine.requestedTool = t;
           ++c.engine.toolRequest;
           bool brush = t == "paint" || t == "erase" || t == "fill" || t == "picker" || t == "blur" || t == "clone" || t == "smudge";
           if (brush)
             p.tool = t == "erase" ? PaintTool::Erase : t == "fill" ? PaintTool::Fill : t == "picker" ? PaintTool::Picker
                    : t == "blur" ? PaintTool::Blur : t == "clone" ? PaintTool::Clone
                    : t == "smudge" ? PaintTool::Smudge : PaintTool::Paint;
           if (brush) c.engine.useToolBrush(p.tool);  // (1.2.13) the tool's own brush (Features: tool_brushes)
           p.blurRadius = a.value("blur_radius", p.blurRadius);
           p.seamAware = a.value("seam_aware", p.seamAware);
           std::string fm = a.value("fill_mode", std::string());
           if (fm == "face") p.fillMode = FillMode::Face;
           else if (fm == "uv_island") p.fillMode = FillMode::UVIsland;
           else if (fm == "connected") p.fillMode = FillMode::Connected;
           else if (fm == "mesh") p.fillMode = FillMode::Mesh;
           else if (fm == "selection") p.fillMode = FillMode::Selection;
           return json{{"tool", t}};
         }});
  // ---------------------------------------------------------------- stencil and text (0.6)
  R.add({"set_stencil",
         "Stencil: a picture laid over the 3D view that strokes paint through (mode mask: white / opaque "
         "lets paint through) or whose colors they paint (mode decal). It stays fixed on screen while the "
         "camera moves. Give an image path, or text to make a text stencil. Returns the stencil state.",
         schema({{"enabled", prop("boolean", "Strokes use the stencil")},
                 {"path", prop("string", "Image file for the stencil")},
                 {"text", prop("string", "Make the stencil from text (\\n for new lines)")},
                 {"font", prop("string", "Font: sans, sans-bold, serif, mono (bundled), a system font name (list_fonts) or a file path")},
                 {"font_size", num("number", "Text line height in pixels of the stencil image", 16, 512, 128)},
                 {"color", prop("array", "Text color, sRGB [r,g,b] (decal mode paints it)")},
                 {"outline", num("number", "Text outline width in pixels (0 = none)", 0, 64, 0)},
                 {"outline_color", prop("array", "Outline color, sRGB")},
                 {"align", propEnum({"left", "center", "right"}, "Alignment of text lines")},
                 {"letter_spacing", num("number", "Extra pixels between letters (negative tightens)", -64, 256, 0)},
                 {"line_spacing", num("number", "Line spacing factor", 0.5, 4, 1.15)},
                 {"mode", propEnum({"mask", "decal"}, "mask: the stencil only shapes the paint; decal: paints its colors")},
                 {"invert", prop("boolean", "Paint where the stencil is dark / transparent instead")},
                 {"tile", prop("boolean", "Repeat the stencil across the view")},
                 {"position", prop("array", "Center in the view, fractions [x, y] (0,0 = top left)")},
                 {"scale", num("number", "Stencil height as a share of the view height", 0.01, 50)},
                 {"rotation", num("number", "Rotation in degrees (clockwise)", -360, 360)},
                 {"overlay_opacity", num("number", "How strongly the stencil is shown over the view (display only)", 0, 1)},
                 {"show_overlay", prop("boolean", "Show the stencil over the view")},
                 {"fit", prop("boolean", "Center the stencil and fit it into the view")},
                 {"clear", prop("boolean", "Remove the stencil")},
                 {"save_image", prop("string", "Also save the stencil image (e.g. rendered text) as a PNG")}}),
         [](CommandContext& c, const json& a) {
           Stencil& st = c.engine.painter.stencil;
           std::string err;
           if (a.value("clear", false)) st.clear();
           if (a.contains("path")) {
             if (!st.loadFile(a["path"].get<std::string>(), &err)) throw CommandError(err);
             st.enabled = true;
           }
           bool textArgs = a.contains("text");
           for (auto k : {"font", "font_size", "color", "outline", "outline_color", "align", "letter_spacing", "line_spacing"})
             if (a.contains(k) && !st.text.empty()) textArgs = true;
           if (textArgs) {
             TextStyle ts = st.textStyle;
             ts.font = a.value("font", ts.font);
             ts.size = a.value("font_size", ts.size);
             if (a.contains("color")) ts.color = jv3(a["color"], ts.color);
             ts.outline = a.value("outline", ts.outline);
             if (a.contains("outline_color")) ts.outlineColor = jv3(a["outline_color"], ts.outlineColor);
             std::string al = a.value("align", std::string(ts.align == 0 ? "left" : ts.align == 2 ? "right" : "center"));
             ts.align = al == "left" ? 0 : al == "right" ? 2 : 1;
             ts.letterSpacing = a.value("letter_spacing", ts.letterSpacing);
             ts.lineSpacing = a.value("line_spacing", ts.lineSpacing);
             std::string text = a.value("text", st.text);
             if (text.empty()) throw CommandError("text is empty");
             if (!st.setText(text, ts, &err)) throw CommandError(err);
             if (a.contains("text")) st.enabled = true;
           }
           if (a.contains("mode")) st.mode = a["mode"] == "decal" ? Stencil::Decal : Stencil::Mask;
           st.invert = a.value("invert", st.invert);
           st.tile = a.value("tile", st.tile);
           if (a.contains("position") && a["position"].is_array() && a["position"].size() >= 2)
             st.center = {a["position"][0].get<float>(), a["position"][1].get<float>()};
           st.scale = std::clamp(a.value("scale", st.scale), 0.01f, 50.f);
           st.rotationDeg = a.value("rotation", st.rotationDeg);
           st.overlayOpacity = std::clamp(a.value("overlay_opacity", st.overlayOpacity), 0.f, 1.f);
           st.showOverlay = a.value("show_overlay", st.showOverlay);
           if (a.value("fit", false)) st.fit();
           st.enabled = a.value("enabled", st.enabled) && st.valid();
           if (a.contains("save_image")) {
             if (!st.valid()) throw CommandError("no stencil image to save");
             if (!saveImage(a["save_image"].get<std::string>(), st.image)) throw CommandError("cannot write image");
           }
           return st.toJson();
         }});
  R.add({"project_stencil",
         "Paints the whole stencil onto the surface seen from the current camera in one step (decal "
         "projection), with the paint material and channels (decal mode: the stencil's colors). "
         "erase=true erases through the stencil instead.",
         schema({{"erase", prop("boolean", "Erase through the stencil")}}),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           if (!e.painter.stencil.valid()) throw CommandError("no stencil: set_stencil with a path or text first");
           PaintTool keep = e.painter.tool;
           e.painter.tool = a.value("erase", false) ? PaintTool::Erase : PaintTool::Paint;
           bool ok = e.painter.projectStencil(e.doc, e.materials, e.brushes, e.paintView(e.camera, e.viewportSize));
           e.painter.tool = keep;
           if (!ok) throw CommandError("could not project (no mesh, or no paintable layer / channels)");
           e.doc.updateComposite(e.materials);
           return json{{"ok", true}, {"undo", e.doc.undo.undoLabel()}};
         }});
  R.add({"set_compare",
         "Split / compare view: the left side of the 3D view shows a snapshot of the textures or another "
         "view mode, the right side the live result. snapshot=true takes the snapshot now.",
         schema({{"enabled", prop("boolean", "Show the split view")},
                 {"mode", propEnum({"snapshot", "view", "high_poly"}, "Left side: the snapshot, another view mode, or (1.1) the high-poly model as it was before the last bake-down")},
                 {"view", prop("string", "View mode for mode=view (basecolor, roughness, metallic, normal, height, ...)")},
                 {"split", num("number", "Divider position, 0 left edge to 1 right edge", 0, 1)},
                 {"snapshot", prop("boolean", "Take a snapshot of the current textures")}}),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           auto& cv = e.compare;
           if (a.value("snapshot", false)) e.takeCompareSnapshot();
           if (a.contains("mode")) {
             cv.mode = a["mode"] == "view" ? 1 : a["mode"] == "high_poly" ? 2 : 0;
             if (cv.mode == 2 && !e.doc.bakeDownBefore) throw CommandError("no bake-down to compare with (bake_down first)");
           }
           if (a.contains("view")) cv.view = viewModeFromName(a["view"].get<std::string>());
           cv.split = std::clamp(a.value("split", cv.split), 0.f, 1.f);
           if (a.contains("enabled")) {
             cv.enabled = a["enabled"].get<bool>();
             if (cv.enabled && cv.mode == 0 && !cv.hasSnapshot) e.takeCompareSnapshot();
           }
           return e.compareState();
         }});
  // ---------------------------------------------------------------- notes (annotations, 0.6)
  R.add({"add_note",
         "Pins a note (comment) to the model, saved in the project. Place it by a 3D position, or by a point "
         "in the view (normalized [x, y], 0,0 = top left) that hits the model.",
         schema({{"text", prop("string", "The note")},
                 {"position", prop("array", "Object-space point [x,y,z] (snapped to the surface)")},
                 {"point", prop("array", "View point [x, y] in 0..1 (or pixels with space=viewport)")},
                 {"space", propEnum({"normalized", "viewport"}, "Coordinates of point (default normalized)")},
                 {"author", prop("string", "Who wrote it (default: the user name)")}},
                {"text"}),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           Document& d = e.doc;
           if (!d.hasMesh()) throw CommandError("no mesh loaded");
           RayHit h;
           if (a.contains("point")) {
             auto& p = a["point"];
             if (!p.is_array() || p.size() < 2) throw CommandError("point must be [x, y]");
             vec2 px(p[0].get<float>(), p[1].get<float>());
             if (a.value("space", std::string("normalized")) != "viewport") px *= e.viewportSize;
             h = e.pick(px);
             if (!h.hit) throw CommandError("the point does not hit the model");
           } else if (a.contains("position")) {
             vec3 q = jv3(a["position"]);
             h = d.bvh.closestPoint(d.mesh, q);
             if (!h.hit) throw CommandError("no surface near the position");
             h.normal = d.mesh.faceNormal(h.face);
           } else {
             throw CommandError("give position or point");
           }
           Note n;
           n.id = d.nextNoteId++;
           n.position = h.position;
           n.normal = glm::length(h.normal) > 0 ? glm::normalize(h.normal) : vec3(0, 1, 0);
           n.text = a.at("text").get<std::string>();
           const char* user = std::getenv("USER");
           if (!user) user = std::getenv("USERNAME");
           n.author = a.value("author", std::string(user ? user : ""));
           n.created = (int64_t)std::time(nullptr);
           n.view = e.camera.toJson();
           d.notes.push_back(n);
           d.dirty = true;
           return n.toJson();
         }});
  R.add({"list_notes", "The notes pinned to the model.",
         schema({{"include_resolved", prop("boolean", "Also list resolved notes (default true)")}}),
         [](CommandContext& c, const json& a) {
           bool all = a.value("include_resolved", true);
           json out = json::array();
           for (auto& n : c.engine.doc.notes)
             if (all || !n.resolved) out.push_back(n.toJson());
           return json{{"notes", out}};
         }});
  R.add({"update_note", "Edits a note: text, resolved, position.",
         schema({{"id", num("integer", "Note id (from list_notes)", 1, 4294967295)},
                 {"text", prop("string", "New text")},
                 {"resolved", prop("boolean", "Mark resolved / open")},
                 {"position", prop("array", "Move it to the surface point nearest [x,y,z]")}},
                {"id"}),
         [](CommandContext& c, const json& a) {
           Document& d = c.engine.doc;
           Note* n = d.findNote(a.at("id").get<uint32_t>());
           if (!n) throw CommandError("no note with that id");
           if (a.contains("text")) n->text = a["text"].get<std::string>();
           n->resolved = a.value("resolved", n->resolved);
           if (a.contains("position")) {
             RayHit h = d.bvh.closestPoint(d.mesh, jv3(a["position"]));
             if (h.hit) n->position = h.position, n->normal = d.mesh.faceNormal(h.face);
           }
           d.dirty = true;
           return n->toJson();
         }});
  R.add({"delete_note", "Removes a note.", schema({{"id", num("integer", "Note id (from list_notes)", 1, 4294967295)}}, {"id"}),
         [](CommandContext& c, const json& a) {
           Document& d = c.engine.doc;
           uint32_t id = a.at("id").get<uint32_t>();
           auto it = std::find_if(d.notes.begin(), d.notes.end(), [&](const Note& n) { return n.id == id; });
           if (it == d.notes.end()) throw CommandError("no note with that id");
           d.notes.erase(it);
           d.dirty = true;
           return json{{"ok", true}, {"count", d.notes.size()}};
         }});
  R.add({"go_to_note", "Turns the camera to a note (the view it was written from).",
         schema({{"id", num("integer", "Note id (from list_notes)", 1, 4294967295)}}, {"id"}),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           Note* n = e.doc.findNote(a.at("id").get<uint32_t>());
           if (!n) throw CommandError("no note with that id");
           if (n->view.is_object()) e.camera.fromJson(n->view);
           else e.camera.target = e.toWorld(n->position);
           return e.camera.toJson();
         }});
  R.add({"list_fonts", "Fonts for text stencils: the bundled ones, then fonts found on this computer.", schema(),
         [](CommandContext&, const json&) {
           json out = json::array();
           for (auto& f : availableFonts()) out.push_back({{"name", f.name}, {"path", f.path}, {"bundled", f.bundled}});
           return json{{"fonts", out}};
         }});
  R.add({"set_paint_channels", std::string("Selects which material channels strokes write. ") + channelSchemaDesc,
         schema({{"channels", prop("array", channelSchemaDesc)},
                 {"opacity", prop("object", "Optional per-channel opacity, e.g. {\"height\":0.5}")}},
                {"channels"}),
         [](CommandContext& c, const json& a) {
           c.engine.painter.channelMask = channelMaskFrom(a.at("channels"));
           if (a.contains("opacity"))
             for (auto& [k, v] : a["opacity"].items()) {
               int idx = channelFromKey(k);
               if (idx >= 0) c.engine.painter.channelOpacity[idx] = v;
             }
           return json{{"channels", channelList(c.engine.painter.channelMask)}};
         }});
  R.add({"set_symmetry",
         "Symmetric painting through the object origin: any combination of X/Y/Z mirrors, plus radial symmetry "
         "(radial = number of copies around radial_axis; 1 = off).",
         schema({{"x", prop("boolean", "Mirror X")}, {"y", prop("boolean", "Mirror Y")}, {"z", prop("boolean", "Mirror Z")},
                 {"radial", num("integer", "Radial copies around the symmetry axis: 1 = off, 8 for a wheel", 1, 32)},
                 {"radial_axis", propEnum({"x", "y", "z"}, "Axis for radial symmetry")}}),
         [](CommandContext& c, const json& a) {
           Painter& P = c.engine.painter;
           auto& s = P.symmetry;
           s[0] = a.value("x", s[0]);
           s[1] = a.value("y", s[1]);
           s[2] = a.value("z", s[2]);
           P.radialCount = std::clamp(a.value("radial", P.radialCount), 1, 32);
           if (a.contains("radial_axis")) {
             std::string ax = a["radial_axis"];
             P.radialAxis = ax == "x" ? 0 : ax == "z" ? 2 : 1;
           }
           return json{{"x", s[0]}, {"y", s[1]}, {"z", s[2]}, {"radial", P.radialCount},
                       {"radial_axis", std::string(1, "xyz"[P.radialAxis])}};
         }});

  // ---------------------------------------------------------------- painting
  R.add({"paint_stroke",
         "Paints one stroke with the current brush/material/tool onto the selected layer (or its mask). "
         "Points are [x,y] or [x,y,pressure]. space 'normalized' = 0..1 of the 3D viewport (origin top-left), "
         "'viewport' = pixels, 'uv' = texture coordinates (0..1, origin bottom-left; brush size in texels). "
         "Optional brush/material/tool objects are applied first (persistently); (1.2.13) a tool comes first and "
         "brings its own brush (feature tool_brushes), so brush arguments change that tool's brush.",
         schema({{"points", prop("array", "List of [x,y] or [x,y,pressure]")},
                 {"space", propEnum({"normalized", "viewport", "uv"}, "Coordinate space (default normalized)")},
                 {"brush", prop("object", "set_brush arguments")},
                 {"material", prop("object", "set_paint_material arguments")},
                 {"tool", propEnum({"paint", "erase", "blur", "clone", "smudge"}, "Tool for this stroke")},
                 {"tile", num("integer", "space uv with texture sets / UDIM tiles: the tile (index from texture_tiles); -1 = the UDIM of the first point, else 0", -1, 1023, -1)}},
                {"points"}),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           // (1.2.13) the tool first: its own brush is the one the brush arguments change (Features: tool_brushes)
           if (a.contains("tool"))
             e.painter.tool = a["tool"] == "erase" ? PaintTool::Erase : a["tool"] == "blur" ? PaintTool::Blur
                            : a["tool"] == "clone" ? PaintTool::Clone
                            : a["tool"] == "smudge" ? PaintTool::Smudge : PaintTool::Paint;
           if (e.painter.tool == PaintTool::Fill || e.painter.tool == PaintTool::Picker) e.painter.tool = PaintTool::Paint;
           e.useToolBrush(e.painter.tool);
           if (a.contains("brush")) applyBrushArgs(e, a["brush"]);
           if (a.contains("material")) applyMaterialArgs(e, a["material"]);
           std::string space = a.value("space", std::string("normalized"));
           std::vector<StrokePoint> pts;
           for (auto& p : a.at("points")) {
             if (!p.is_array() || p.size() < 2) throw CommandError("points must be [x,y] or [x,y,pressure]");
             StrokePoint sp;
             sp.pos = {p[0].get<float>(), p[1].get<float>()};
             sp.pressure = p.size() > 2 ? p[2].get<float>() : 1.f;
             if (space == "normalized") sp.pos *= e.viewportSize;
             pts.push_back(sp);
           }
           // texture tiles: UVs as authored (each tile's own 0..1, UDIM numbers beyond it) -> the atlas
           const TileLayout& tl = e.doc.mesh.tiles;
           int tile = -1;
           if (space == "uv" && tl.active() && !pts.empty()) {
             tile = a.value("tile", -1);
             if (tile < 0 && (tl.mode & TileByUdim))
               for (int t = (int)tl.tiles.size() - 1; t >= 0; --t)
                 if (tl.tiles[t].udim == udimOf(pts[0].pos)) tile = t;  // the first tile with that UDIM
             tile = std::clamp(tile, 0, tl.count() - 1);
             for (auto& p : pts) {
               vec2 uv = p.pos;
               if (!(tl.mode & TileByUdim)) uv -= glm::floor(uv);
               p.pos = tl.toAtlas(uv, tile);
             }
           }
           e.painter.uvTile = tile;
           json r = e.scriptedStroke(pts, space == "uv");
           e.painter.uvTile = -1;
           return r;
         }});
  R.add({"paint_world_points",
         "Paints a stroke through 3D points (object space). Points are projected through the current camera; "
         "hidden points are skipped.",
         schema({{"points", prop("array", "List of [x,y,z]")}, {"brush", prop("object", "set_brush arguments")},
                 {"material", prop("object", "set_paint_material arguments")}},
                {"points"}),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           if (a.contains("brush")) applyBrushArgs(e, a["brush"]);
           if (a.contains("material")) applyMaterialArgs(e, a["material"]);
           mat4 vp = e.objectCamera().viewProj(e.viewportSize.x / e.viewportSize.y);  // (the active object's points)
           std::vector<StrokePoint> pts;
           for (auto& p : a.at("points")) {
             vec4 cl = vp * vec4(jv3(p), 1);
             if (cl.w <= 0) continue;
             vec2 ndc = vec2(cl) / cl.w;
             pts.push_back({{(ndc.x * 0.5f + 0.5f) * e.viewportSize.x, (0.5f - ndc.y * 0.5f) * e.viewportSize.y}, 1.f});
           }
           if (pts.empty()) throw CommandError("no point is in front of the camera");
           return e.scriptedStroke(pts, false);
         }});
  R.add({"fill",
         "Polygon fill with the current material: mode face | uv_island | connected | mesh | selection. Target "
         "faces via 'faces', or 'point' [x,y] (normalized viewport) to pick a face.",
         schema({{"mode", propEnum({"face", "uv_island", "connected", "mesh", "selection", "material"}, "Fill extent (material = every face of the clicked face's material slot)")},
                 {"faces", prop("array", "Face indices")},
                 {"point", prop("array", "[x,y] normalized viewport position")},
                 {"material", prop("object", "set_paint_material arguments")},
                 {"erase", prop("boolean", "Erase the faces instead (like Alt+click with the Fill tool)")}}),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           if (a.contains("material")) applyMaterialArgs(e, a["material"]);
           std::string mode = a.value("mode", std::string("mesh"));
           FillMode fm = mode == "face" ? FillMode::Face : mode == "uv_island" ? FillMode::UVIsland
                       : mode == "connected" ? FillMode::Connected : mode == "selection" ? FillMode::Selection
                       : mode == "material" ? FillMode::Material : FillMode::Mesh;
           std::vector<uint32_t> faces = facesArg(e, a);
           if (faces.empty()) {
             uint32_t face = 0;
             if (a.contains("point")) {
               vec2 p(a["point"][0].get<float>(), a["point"][1].get<float>());
               RayHit h = e.pick(p * e.viewportSize);
               if (!h.hit) throw CommandError("point does not hit the mesh");
               face = h.face;
             } else if (fm != FillMode::Mesh && fm != FillMode::Selection) {
               throw CommandError("give 'faces' or 'point'");
             }
             faces = e.painter.facesForFill(e.doc, face, fm);
           } else if (fm != FillMode::Face) {
             std::vector<uint32_t> all;
             for (uint32_t f : faces) {
               auto more = e.painter.facesForFill(e.doc, f, fm);
               all.insert(all.end(), more.begin(), more.end());
             }
             std::sort(all.begin(), all.end());
             all.erase(std::unique(all.begin(), all.end()), all.end());
             faces = all;
           }
           PaintTool keep = e.painter.tool;
           e.painter.tool = a.value("erase", false) ? PaintTool::Erase : PaintTool::Paint;
           bool ok = e.painter.fillFaces(e.doc, e.materials, faces);
           e.painter.tool = keep;
           if (!ok) throw CommandError("fill failed");
           return json{{"faces", faces.size()}};
         }});
  R.add({"pick", "Samples the composite (color, roughness, metallic, height) at a normalized viewport point.",
         schema({{"point", prop("array", "[x,y] normalized")}, {"apply", prop("boolean", "Use as flat paint values")}},
                {"point"}),
         [](CommandContext& c, const json& a) {
           vec2 p(a["point"][0].get<float>(), a["point"][1].get<float>());
           json out;
           if (!c.engine.pickAt(p * c.engine.viewportSize, a.value("apply", false), &out))
             throw CommandError("point does not hit the mesh");
           return out;
         }});
  R.add({"set_clone_source",
         "Sets the Clone tool source: 'uv' [u,v] or 'point' [x,y] (normalized viewport, picks the surface). "
         "Aligned (default) keeps the offset between strokes.",
         schema({{"uv", prop("array", "[u,v] (with texture tiles: as authored, in 'tile')")},
                 {"tile", num("integer", "Texture sets / UDIM tiles: the tile of 'uv'; -1 = its UDIM, else 0", -1, 1023, -1)},
                 {"point", prop("array", "[x,y] normalized viewport")},
                 {"aligned", prop("boolean", "Keep the source offset across strokes")}}),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           vec2 uv, shown;  // texture coordinate (atlas), and the UV as authored
           const TileLayout& tl = e.doc.mesh.tiles;
           if (a.contains("uv")) {
             shown = uv = {a["uv"][0].get<float>(), a["uv"][1].get<float>()};
             if (tl.active()) {
               int tile = a.value("tile", -1);
               if (tile < 0 && (tl.mode & TileByUdim))
                 for (int t = 0; t < (int)tl.tiles.size(); ++t)
                   if (tl.tiles[t].udim == udimOf(uv)) tile = t;
               tile = std::clamp(tile, 0, tl.count() - 1);
               uv = tl.toAtlas((tl.mode & TileByUdim) ? uv : uv - glm::floor(uv), tile);
             }
           } else if (a.contains("point")) {
             RayHit h = e.pick(vec2(a["point"][0].get<float>(), a["point"][1].get<float>()) * e.viewportSize);
             if (!h.hit) throw CommandError("point does not hit the mesh");
             uv = h.uv, shown = h.meshUV;
           } else {
             throw CommandError("give 'uv' or 'point'");
           }
           e.painter.cloneAligned = a.value("aligned", e.painter.cloneAligned);
           e.painter.setCloneSource(uv);
           return json{{"uv", {shown.x, shown.y}}};
         }});
  R.add({"undo", "Undo the last operation.", schema(), [](CommandContext& c, const json&) {
           bool ok = c.engine.doc.undoStep();
           return json{{"undone", ok}, {"next", c.engine.doc.undo.undoLabel()}};
         }});
  R.add({"redo", "Redo.", schema(), [](CommandContext& c, const json&) {
           bool ok = c.engine.doc.redoStep();
           return json{{"redone", ok}};
         }});
  R.add({"history", "Undo history: every step with its label; 'position' = number of steps currently applied.",
         schema(), [](CommandContext& c, const json&) {
           Document& d = c.engine.doc;
           json steps = json::array();
           for (size_t i = 0; i < d.undo.count(); ++i) steps.push_back(d.undo.label(i));
           return json{{"steps", steps}, {"position", d.undo.position()}};
         }});
  R.add({"undo_settings",
         "(1.2.7) How much undo keeps, for every object's history together, and what it uses now. Recent steps keep "
         "their texture copies on the graphics card (instant undo, up to gpu_mb); older ones are compressed into RAM, "
         "and past ram_mb the oldest steps are dropped. Undo is never written to disk. Each value given is set (the "
         "user's preferences); without any, it only reports.",
         schema({{"steps", num("integer", "Steps each object's history keeps. More: undo further back; fewer: less memory", 10, 1000, 100)},
                 {"ram_mb", num("integer", "Compressed undo in RAM, in MB; past it the oldest steps go. Default: a quarter of the machine's memory, 1 to 8 GB", 1, 1048576)},
                 {"gpu_mb", num("integer", "Undo copies kept on the graphics card for instant undo, in MB. Less leaves more for textures; more makes older steps undo faster", 256, 65536, 1024)}}),
         [](CommandContext& c, const json& a) {
           UndoLimits& L = UndoStack::limits();
           bool set = false;
           if (a.contains("steps")) L.steps = std::clamp<size_t>(a["steps"].get<size_t>(), 10, 1000), set = true;
           if (a.contains("ram_mb")) L.ramBytes = std::clamp<size_t>(a["ram_mb"].get<size_t>(), 1, 1048576) << 20, set = true;
           if (a.contains("gpu_mb")) L.gpuBytes = std::clamp<size_t>(a["gpu_mb"].get<size_t>(), 256, 65536) << 20, set = true;
           if (set) UndoStack::enforceAll();
           UndoStack::settle();  // (the packing that waits for a quiet moment: done now, so the numbers are final)
           Document& d = c.engine.doc;
           return json{{"steps", L.steps}, {"ram_mb", L.ramBytes >> 20}, {"gpu_mb", L.gpuBytes >> 20},
                       {"default_ram_mb", UndoLimits::defaultRam() >> 20},
                       {"used_gpu_mb", UndoStack::totalBytes() / 1048576.0}, {"used_ram_mb", UndoStack::totalRamBytes() / 1048576.0},
                       {"object_steps", d.undo.count()}, {"object_gpu_mb", d.undo.bytes() / 1048576.0},
                       {"object_ram_mb", d.undo.ramBytes() / 1048576.0}};
         }});
  R.add({"history_jump", "Undoes or redoes until 'position' steps of the history are applied (0 = before all).",
         schema({{"position", num("integer", "Target position (from history; 0 = the start)", 0, 1000000)}}, {"position"}),
         [](CommandContext& c, const json& a) {
           c.engine.doc.jumpTo(a.at("position").get<size_t>());
           return json{{"position", c.engine.doc.undo.position()}};
         }});

  // ---------------------------------------------------------------- layers
  R.add({"list_layers", "Layer stack (index 0 = bottom).", schema(),
         [](CommandContext& c, const json&) { return c.engine.doc.layerStackJson(); }});
  R.add({"add_layer",
         "Adds a paint layer, or a fill layer covering the mesh with a material (optionally masked by a smart "
         "generator: edges, cavity, occlusion, gradient_y, noise).",
         schema({{"type", propEnum({"paint", "fill", "adjust", "folder", "baked"}, "Layer type (adjust = HSV/contrast/roughness tweak of everything below; folder = group; baked = a baked map's layer, with map: see baked_maps)")},
                 {"map", propEnum({"ao", "curvature", "thickness"}, "type baked: which baked map")},
                 {"parent", num("integer", "Index of the folder to create the layer in (default: beside the selection)", 0, 100000)},
                 {"mask", propEnum({"none", "white", "black"}, "Start with a mask (black = hidden until painted)")},
                 {"adjust", adjustSchema()},
                 {"name", prop("string", "Layer name")},
                 {"material", prop("object", "Fill: set_paint_material style object (id, color, mapping, scale...)")},
                 {"generator", prop("object", "Smart mask {type: edges|cavity|occlusion|gradient_y|noise|facing|material|thin|gradient|drips, level, contrast, noise, noise_scale, invert, direction [x,y,z] (facing, gradient), slot (material), noise_type: clouds|cells|cracks|ridges|streaks (noise)}")},
                 {"index", num("integer", "Insert position, a layer index (default: above the selection)", 0, 100000)}}),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           Layer* l;
           std::string type = a.value("type", std::string("paint"));
           if (a.contains("parent")) {
             int p = a["parent"].get<int>();
             if (p < 0 || p >= (int)e.doc.layers.size() || !e.doc.layers[p]->isFolder())
               throw CommandError("parent is not a folder");
             e.doc.selected = p;  // new layers go to the top of the selected folder
           }
           if (type == "baked") {
             int k = bakedMapFromKey(a.value("map", std::string("ao")));
             if (!e.features.on("baked_layers")) throw CommandError("baked map layers are switched off (features)");
             if (k < 0 || !e.doc.addBakedLayer(k)) throw CommandError("that map is not baked (bake_mesh_maps first)");
             l = e.doc.bakedLayer(k);
             return json{{"index", e.doc.indexOf(l->uid)}, {"uid", l->uid}, {"name", l->name}};
           }
           if (type == "folder") {
             l = &e.doc.addFolder(a.value("name", std::string()), a.value("index", -1));
           } else if (type == "adjust") {
             l = &e.doc.addAdjustLayer(a.value("name", std::string()), a.value("index", -1));
             if (a.contains("adjust")) l->adjust.fromJson(a["adjust"]), e.doc.ensureBakedFor(*l);
           } else if (type == "fill") {
             MaterialRef keep = e.painter.material;
             std::string keepId = e.selectedMaterialId;
             if (a.contains("material")) applyMaterialArgs(e, a["material"]);
             MaterialRef ref = e.painter.material;
             e.painter.material = keep;
             e.selectedMaterialId = keepId;
             l = &e.doc.addFillLayer(ref, a.value("name", std::string()), a.value("index", -1));
           } else {
             l = &e.doc.addPaintLayer(a.value("name", std::string()), a.value("index", -1));
           }
           if (a.contains("generator")) l->generator.fromJson(a["generator"]);
           std::string mask = a.value("mask", std::string("none"));
           uint32_t uid = l->uid;
           if (mask == "white" || mask == "black") e.doc.addMask(e.doc.indexOf(uid), mask == "white" ? 1.f : 0.f);
           e.doc.checkpoint();  // generator / mask set at creation: its own, named step
           return json{{"index", e.doc.indexOf(uid)}, {"uid", uid}, {"name", l->name}};
         }});
  R.add({"remove_layer", "Deletes a layer (undoable).", schema({{"index", num("integer", "Layer index (0 = bottom; see list_layers)", 0, 100000)}}),
         [](CommandContext& c, const json& a) {
           int i = layerIndexArg(c.engine, a);
           c.engine.doc.removeLayer(i);
           return json{{"removed", i}};
         }});
  R.add({"select_layer", "Selects the layer that strokes affect; edit_mask paints its mask instead.",
         schema({{"index", num("integer", "Layer index (0 = bottom; see list_layers)", 0, 100000)}, {"edit_mask", prop("boolean", "Paint the layer mask")}},
                {"index"}),
         [](CommandContext& c, const json& a) {
           int i = layerIndexArg(c.engine, a);
           c.engine.doc.selected = i;
           c.engine.doc.editMask = a.value("edit_mask", false);
           return json{{"selected", i}, {"edit_mask", c.engine.doc.editMask}};
         }});
  R.add({"update_layer",
         "Changes layer properties: name, visible, opacity, blend (normal multiply screen overlay add subtract darken "
         "lighten add_signed soft_light), channels, material (fill), generator (smart mask).",
         schema({{"index", num("integer", "Layer index (0 = bottom; see list_layers). Default: the selected layer", 0, 100000)},
                 {"name", prop("string", "Name")},
                 {"visible", prop("boolean", "Visibility")},
                 {"opacity", num("number", "Layer opacity: 0 hidden, 1 full", 0, 1)},
                 {"blend", prop("string", "Blend mode")},
                 {"channels", prop("array", channelSchemaDesc)},
                 {"material", prop("object", "Fill layer material settings")},
                 {"generator", prop("object", "Smart mask {type: edges|cavity|occlusion|gradient_y|noise|facing|material|thin|gradient|drips, level, contrast, noise, noise_scale, invert, direction [x,y,z] (facing, gradient), slot (material), noise_type: clouds|cells|cracks|ridges|streaks (noise)}")},
                 {"adjust", adjustSchema()},
                 {"color_tag", num("integer", "Color tag: 0 none, 1 to 7 the swatches in the layer list", 0, 7)},
                 {"mask_effects", prop("array", "Replace the mask effect stack: [{type: paint|generator|fill|noise|levels|blur|invert|"
                                                "grow|edges|pattern, enabled, blend: normal|multiply|add|subtract|lighten|darken|screen, "
                                                "opacity, generator {...} (generator), value (fill), scale, seed, contrast, noise_type: "
                                                "clouds|cells|cracks|ridges|streaks (noise), in_low, in_high, gamma, out_low, out_high "
                                                "(levels), radius (blur, edges: texels at 1K), amount (grow: texels at 1K, below 0 "
                                                "shrinks), material {id, mapping, scale, rotation...}, source: height|brightness|"
                                                "roughness|metallic, level, contrast, invert (pattern: a library material as a mask)}]. "
                                                "[] removes the stack.")},
                 {"parent", num("integer", "Move into the folder at this index (-1 = top level)", -1, 100000)},
                 {"strength", num("number", "A baked map layer: how strong its map is (0 off, 1 as baked, 2 twice as strong)", 0, 2)}}),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           Layer& l = *e.doc.layers[layerIndexArg(e, a)];
           if (a.contains("strength")) l.bakedStrength = std::clamp(a["strength"].get<float>(), 0.f, 2.f);
           if (a.contains("mask_effects")) {
             if (!a["mask_effects"].is_array()) throw CommandError("mask_effects must be an array");
             l.maskEffects.clear();
             for (auto& j : a["mask_effects"]) {
               MaskEffect fx;
               fx.fromJson(j);
               l.maskEffects.push_back(fx);
             }
             e.doc.ensureBakedFor(l);
           }
           l.name = a.value("name", l.name);
           l.colorTag = std::clamp(a.value("color_tag", l.colorTag), 0, 7);
           l.visible = a.value("visible", l.visible);
           l.opacity = a.value("opacity", l.opacity);
           if (a.contains("blend")) l.blend = blendModeFromName(a["blend"]);
           if (a.contains("channels")) l.channelMask = channelMaskFrom(a["channels"]);
           if (a.contains("generator")) l.generator.fromJson(a["generator"]);
           if (a.contains("adjust")) l.adjust.fromJson(a["adjust"]), e.doc.ensureBakedFor(l);
           if (a.contains("material")) {
             MaterialRef keep = e.painter.material;
             e.painter.material = l.material;
             applyMaterialArgs(e, a["material"]);
             l.material = e.painter.material;
             e.painter.material = keep;
           }
           e.doc.markComposite();
           if (a.contains("parent")) {
             int p = a["parent"].get<int>();
             uint32_t folder = p >= 0 && p < (int)e.doc.layers.size() ? e.doc.layers[p]->uid : 0;
             if (p >= 0 && (!folder || !e.doc.setParent(e.doc.indexOf(l.uid), folder)))
               throw CommandError("cannot move the layer into that folder");
             if (p < 0) e.doc.setParent(e.doc.indexOf(l.uid), 0);
           }
           return l.toJson();
         }});
  R.add({"baked_maps",
         "(0.41) The baked ambient occlusion, curvature and thickness and their layers. Each baked map has a layer at the "
         "bottom of the stack (type baked): paint on it (select it, set_brush mask_value, paint_stroke) to fix the bake, "
         "the erase tool brings the bake back, blur / smudge / clone work on it; its mask effects (update_layer "
         "mask_effects: levels, blur, fill, noise...) adjust the bake with the fixes; strength scales the result. What it "
         "makes is the map smart masks, the 3D view (view modes ao, curvature, thickness) and exports read. (0.44.2) "
         "Its layer hidden (update_layer visible false) switches the map off: the 3D view no longer shades with the "
         "ambient occlusion and exports leave it out (shown false); smart masks keep the bake. Lists the maps (mean "
         "values: as baked, and now; out_of_date: baked for other UVs, so rebake); with map: select its layer, add it "
         "again after a delete, take its painted fixes away or its adjustments off, set its strength.",
         schema({{"map", propEnum({"ao", "curvature", "thickness"}, "The baked map")},
                 {"select", prop("boolean", "Select its layer (brush strokes paint fixes on it)")},
                 {"add", prop("boolean", "Give the map a layer again (after the layer was deleted)")},
                 {"reset_fixes", prop("boolean", "Take the painted fixes away: the bake shows again (undoable)")},
                 {"reset_adjustments", prop("boolean", "Remove the layer's adjustments (mask effects) and set its strength to 1")},
                 {"strength", num("number", "How strong the map is: 0 off (the neutral value), 1 as baked, 2 twice as strong", 0, 2)},
                 {"rebake", prop("boolean", "(0.44.2) Bake again the maps that are out_of_date (made for other UVs: UV edits "
                                            "since moved them along with the paint, blurred and broken where islands changed); fixes stay")}}),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           Document& d = e.doc;
           if (a.value("rebake", false)) d.rebakeOutOfDate();
           if (a.contains("map")) {
             int k = bakedMapFromKey(a.value("map", std::string()));
             if (k < 0) throw CommandError("unknown map (ao, curvature, thickness)");
             bool wants = a.value("add", false) || a.value("select", false) || a.value("reset_fixes", false) ||
                          a.value("reset_adjustments", false) || a.contains("strength");
             if (wants && !e.features.on("baked_layers")) throw CommandError("baked map layers are switched off (features)");
             if (wants && !d.tex.hasBakedMap(k)) throw CommandError(std::string("the ") + bakedMapKey(k) + " map is not baked (bake_mesh_maps first)");
             if (a.value("add", false)) d.addBakedLayer(k);
             Layer* L = d.bakedLayer(k);
             if (wants && !L) throw CommandError("that map has no layer (add: true gives it one)");
             if (L) {
               int i = d.indexOf(L->uid);
               if (a.value("select", false)) d.selected = i, d.editMask = false;
               if (a.value("reset_fixes", false) && L->mask) d.removeMask(i);
               L = d.bakedLayer(k);
               if (a.value("reset_adjustments", false)) L->maskEffects.clear(), L->bakedStrength = 1.f, d.markComposite();
               if (a.contains("strength")) L->bakedStrength = std::clamp(a["strength"].get<float>(), 0.f, 2.f), d.markComposite();
             }
             d.checkpoint();
           }
           d.updateComposite(e.materials);
           d.tex.applyBakedLayers(d.layers, d.mesh.bounds);
           // the mean of a map (all texels; small copies keep the read-back light)
           auto mean = [&](const Texture2D& t) {
             Texture2D small(256, 256, GL_RGBA16F, 1);
             TextureSet::resampleRegion(t, small, vec2(0), vec2(1));
             Image img = small.download(1);
             double sum = 0;
             for (float v : img.pixels) sum += v;
             return img.pixels.empty() ? 0.0 : sum / double(img.pixels.size());
           };
           json maps = json::array();
           int stale = d.bakesOutOfDate();
           for (int k = 0; k < kBakedMapCount; ++k) {
             json m = {{"map", bakedMapKey(k)}, {"baked", d.tex.hasBakedMap(k)}};
             // (0.44.2) made for other UVs; switched off (its layer hidden): out of the 3D view and the exports
             m["out_of_date"] = ((stale >> k) & 1) != 0;
             m["shown"] = d.tex.bakedShown(k);
             if (d.tex.hasBakedMap(k)) {
               m["mean_baked"] = mean(d.tex.bakedRaw(k));
               m["mean"] = mean(d.tex.bakedView(k));
               m["edited"] = d.tex.bakedEdited(k);
             }
             if (Layer* L = d.bakedLayer(k)) {
               m["layer"] = {{"index", d.indexOf(L->uid)}, {"uid", L->uid}, {"name", L->name}, {"visible", L->visible},
                             {"opacity", L->opacity}, {"strength", L->bakedStrength}, {"fixes", (bool)L->mask},
                             {"effects", L->maskEffects.size()}, {"selected", d.selected == d.indexOf(L->uid)}};
             } else {
               m["layer"] = nullptr;
             }
             maps.push_back(m);
           }
           return json{{"maps", maps}, {"feature", e.features.on("baked_layers")}};
         }});
  R.add({"add_instance",
         "Adds an instance of a layer right above it: the same content (paint, fill material or adjustment) with its "
         "own opacity, blend mode, channels and mask. Painting either one changes both.",
         schema({{"index", num("integer", "Layer index (0 = bottom; see list_layers). Default: the selected layer", 0, 100000)}}),
         [](CommandContext& c, const json& a) {
           int i = c.engine.doc.addInstance(layerIndexArg(c.engine, a));
           if (i < 0) throw CommandError("folders can't be instanced");
           return json{{"index", i}, {"layer", c.engine.doc.layers[i]->toJson()}};
         }});
  R.add({"make_independent", "Turns an instance into an ordinary layer with a copy of the content.",
         schema({{"index", num("integer", "Layer index (0 = bottom; see list_layers). Default: the selected layer", 0, 100000)}}),
         [](CommandContext& c, const json& a) {
           int i = layerIndexArg(c.engine, a);
           if (!c.engine.doc.makeIndependent(i)) throw CommandError("not an instance");
           return c.engine.doc.layers[i]->toJson();
         }});
  R.add({"add_mask_effect",
         "Adds an effect to a layer's mask effect stack (the simple mask is converted first: painted mask, then the "
         "smart mask multiplied). Returns the stack.",
         schema({{"index", num("integer", "Layer index (0 = bottom; see list_layers). Default: the selected layer", 0, 100000)},
                 {"effect", prop("object", "The effect, as in update_layer mask_effects")},
                 {"position", num("integer", "Where in the mask's effect stack (0 = the bottom); default: on top", 0, 1000)}},
                {"effect"}),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           Layer& l = *e.doc.layers[layerIndexArg(e, a)];
           l.convertMaskToStack();
           MaskEffect fx;
           fx.fromJson(a.at("effect"));
           int pos = std::clamp(a.value("position", (int)l.maskEffects.size()), 0, (int)l.maskEffects.size());
           l.maskEffects.insert(l.maskEffects.begin() + pos, fx);
           e.doc.ensureBakedFor(l);
           e.doc.markComposite();
           return l.toJson().value("mask_effects", json::array());
         }});
  R.add({"move_layer", "Moves a layer in the stack.",
         schema({{"from", num("integer", "Current index (0 = bottom)", 0, 100000)}, {"to", num("integer", "New index (0 = bottom)", 0, 100000)}}, {"from", "to"}),
         [](CommandContext& c, const json& a) {
           if (!c.engine.doc.moveLayer(a.at("from"), a.at("to"))) throw CommandError("invalid indices");
           return c.engine.doc.layerStackJson();
         }});
  R.add({"duplicate_layer", "Duplicates a layer.", schema({{"index", num("integer", "Layer index (0 = bottom; see list_layers)", 0, 100000)}}),
         [](CommandContext& c, const json& a) {
           int i = c.engine.doc.duplicateLayer(layerIndexArg(c.engine, a));
           return json{{"index", i}, {"uid", i >= 0 ? c.engine.doc.layers[i]->uid : 0}};
         }});
  R.add({"list_smart_materials", "Smart materials (saved layer stacks) from the built-in set and your library.", schema(),
         [](CommandContext&, const json&) {
           json out = json::array();
           for (auto& m : listSmartMaterials())
             out.push_back({{"id", m.id}, {"name", m.name}, {"description", m.description}, {"builtin", m.builtin},
                            {"layers", m.layerCount}, {"includes_paint", m.hasPaint}});
           return out;
         }});
  R.add({"save_smart_material",
         "Saves a layer or folder (with everything in it) as a smart material that can be applied to any project. "
         "include_paint also stores painted textures and masks (they only fit meshes with the same UVs).",
         schema({{"index", num("integer", "Layer / folder index. Default: the selected one", 0, 100000)}, {"name", prop("string", "Name")},
                 {"description", prop("string", "Short description")},
                 {"include_paint", prop("boolean", "Store painted channels and masks too (default false)")}}),
         [](CommandContext& c, const json& a) {
           std::string path, err;
           if (!saveSmartMaterial(c.engine.doc, layerIndexArg(c.engine, a), a.value("name", std::string()),
                                  a.value("description", std::string()), a.value("include_paint", false), &path, &err))
             throw CommandError(err);
           return json{{"path", path}};
         }});
  R.add({"apply_smart_material",
         "Adds a smart material above the selected layer (or into the selected folder), optionally limited to a "
         "region of the model (parts from mesh_parts) and a smart mask stack.",
         schema({{"id", prop("string", "Smart material id or name (see list_smart_materials), or a file path")},
                 {"region", regions::regionSchema()}, {"mask", regions::maskSchema()}},
                {"id"}),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           regions::Region r = regions::regionArg(e, a);  // before anything changes: a bad part changes nothing
           bool masked = r.given || (a.contains("mask") && !a["mask"].empty());
           if (masked) e.doc.beginUndoGroup("Smart material");
           std::string err;
           int i = applySmartMaterial(e.doc, a.at("id"), &err);
           if (i < 0) {
             if (masked) e.doc.cancelUndoGroup();
             throw CommandError(err);
           }
           uint32_t uid = e.doc.layers[i]->uid;
           if (masked) {
             e.doc.checkpoint();
             regions::applyRegionAndMasks(e, i, r, a.value("mask", json::array()));
             e.doc.endUndoGroup();
           }
           i = e.doc.indexOf(uid);
           return json{{"index", i}, {"uid", uid}, {"name", e.doc.layers[i]->name}};
         }});
  R.add({"copy_layer_settings",
         "Copies opacity, blend mode, channels, smart mask and (between layers of the same type) the fill material "
         "or adjustment from one layer to others.",
         schema({{"from", num("integer", "Source layer index", 0, 100000)}, {"to", prop("array", "Target layer indices")}},
                {"from", "to"}),
         [](CommandContext& c, const json& a) {
           Document& d = c.engine.doc;
           int f = a.at("from");
           if (f < 0 || f >= (int)d.layers.size()) throw CommandError("invalid source index");
           json settings = copyLayerSettings(*d.layers[f]);
           json to = a.at("to").is_array() ? a["to"] : json::array({a["to"]});
           for (auto& t : to) {
             int i = t.get<int>();
             if (i < 0 || i >= (int)d.layers.size()) throw CommandError("invalid target index");
             pasteLayerSettings(*d.layers[i], settings);
           }
           d.pendingLabel = "Paste layer settings";
           d.markComposite();
           return d.layerStackJson();
         }});
  R.add({"group_into_folder", "Puts a layer into a new folder that takes its place in the stack.",
         schema({{"index", num("integer", "Layer index (0 = bottom; see list_layers). Default: the selected layer", 0, 100000)}}),
         [](CommandContext& c, const json& a) {
           int i = c.engine.doc.groupIntoFolder(layerIndexArg(c.engine, a));
           if (i < 0) throw CommandError("invalid layer");
           return json{{"index", i}, {"uid", c.engine.doc.layers[i]->uid}};
         }});
  R.add({"solo_layer", "Shows only this layer (or folder) in the viewport and exports nothing else until "
         "turned off. index -1 = off.",
         schema({{"index", num("integer", "Layer index; -1 turns solo off", -1, 100000, -1)}}),
         [](CommandContext& c, const json& a) {
           Document& d = c.engine.doc;
           int i = a.value("index", -1);
           d.soloUid = i >= 0 && i < (int)d.layers.size() ? d.layers[i]->uid : 0;
           d.compositeDirty = true;
           return json{{"solo", d.soloUid ? i : -1}};
         }});
  R.add({"move_layer_into", "Moves a layer (with its contents if it is a folder) into a folder, or out to the top "
         "level with folder = -1.",
         schema({{"index", num("integer", "Layer index (0 = bottom; see list_layers)", 0, 100000)}, {"folder", num("integer", "Folder index; -1 = top level", -1, 100000)}},
                {"index", "folder"}),
         [](CommandContext& c, const json& a) {
           Document& d = c.engine.doc;
           int i = layerIndexArg(c.engine, a), f = a.at("folder");
           uint32_t uid = f >= 0 && f < (int)d.layers.size() ? d.layers[f]->uid : 0;
           if ((f >= 0 && !uid) || !d.setParent(i, uid)) throw CommandError("cannot move the layer there");
           return d.layerStackJson();
         }});
  R.add({"clear_layer", "Clears painted channels (and/or mask) of a layer.",
         schema({{"index", num("integer", "Layer index (0 = bottom; see list_layers)", 0, 100000)}, {"channels", prop("array", channelSchemaDesc)},
                 {"mask", prop("boolean", "Remove the mask")}}),
         [](CommandContext& c, const json& a) {
           int i = layerIndexArg(c.engine, a);
           int m = a.contains("channels") ? channelMaskFrom(a["channels"]) : kAllChannels;
           c.engine.doc.clearPaint(i, m);  // undoable; the mask stays unless asked
           if (a.value("mask", false)) c.engine.doc.removeMask(i);
           return json{{"cleared", channelList(m)}};
         }});
  R.add({"set_layer_mask", "Adds (filled with value) or removes a layer mask.",
         schema({{"index", num("integer", "Layer index (0 = bottom; see list_layers)", 0, 100000)}, {"value", num("number", "Fill value of a new mask: 1 white (the layer shows), 0 black (hidden until painted)", 0, 1, 1)},
                 {"remove", prop("boolean", "Remove the mask")},
                 {"from_selection", prop("boolean", "Mask = the selected faces (white), replacing any mask")}}),
         [](CommandContext& c, const json& a) {
           Document& d = c.engine.doc;
           int i = layerIndexArg(c.engine, a);
           if (a.value("from_selection", false)) {
             if (!d.maskFromSelection(i)) throw CommandError("no faces selected");
           } else if (a.value("remove", false)) {
             d.removeMask(i);
           } else {
             if (d.layers[i]->mask) d.removeMask(i);  // replacing = two undo steps
             d.addMask(i, a.value("value", 1.f));
           }
           return json{{"has_mask", (bool)d.layers[i]->mask}};
         }});

  // ---------------------------------------------------------------- camera / view
  R.add({"set_camera",
         "Orbit camera: yaw/pitch (radians), distance, target [x,y,z], fov, ortho; view preset front back left right "
         "top bottom persp; frame=true frames the mesh. A side, top or bottom view remembers the view before it: "
         "view=previous goes back to that one (and a second time to the side view again), like Numpad 0 in the app.",
         schema({{"yaw", num("number", "Turn round the model, radians: 0 front, 1.5708 a quarter turn", -6.2832, 6.2832)}, {"pitch", num("number", "Look from above (+) or below (-), radians", -1.55, 1.55)},
                 {"distance", num("number", "Distance from the target, object units: smaller is closer", 0.001, 1000000)}, {"target", prop("array", "[x,y,z]")},
                 {"fov", num("number", "Vertical field of view, radians: 0.6 normal, smaller flattens the perspective", 0.05, 2.5)}, {"ortho", prop("boolean", "Orthographic")},
                 {"view", propEnum({"front", "back", "left", "right", "top", "bottom", "persp", "previous"}, "Preset")},
                 {"frame", prop("boolean", "Frame the mesh first")}}),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           Camera& cam = e.camera;
           e.checkViewSnap();
           if (a.value("frame", false)) e.frameMesh();
           if (a.contains("view")) {
             std::string v = a["view"];
             float y, p;
             if (v == "previous") {
               if (!e.viewBack()) throw CommandError("no view to go back to: go to a side, top or bottom view first");
             } else if (Camera::viewAngles(v, y, p)) {
               e.snapView(y, p, false);  // (agents set ortho themselves)
             } else {
               cam.setView(v);
             }
           }
           cam.fromJson(a);
           cam.updateClip(e.clipBounds());
           e.checkViewSnap();
           json r = cam.toJson();
           r["side_view"] = e.viewMemory.snapped;
           r["can_go_back"] = e.viewMemory.hasBack;
           return r;
         }});
  R.add({"set_viewport_size", "Size of the 3D viewport used for normalized stroke coordinates and renders (headless).",
         schema({{"width", num("integer", "Pixels", 16, 16384)}, {"height", num("integer", "Pixels", 16, 16384)}}, {"width", "height"}),
         [](CommandContext& c, const json& a) {
           if (!c.engine.headless) throw CommandError("the GUI controls the viewport size; resize the window instead");
           c.engine.viewportSize = {std::max(16, a["width"].get<int>()), std::max(16, a["height"].get<int>())};
           return json{{"width", c.engine.viewportSize.x}, {"height", c.engine.viewportSize.y}};
         }});
  R.add({"set_view",
         "Viewport display: mode, wireframe, seams. Checks among the modes: mesh_check (problems), pbr_range "
         "(colors no real material has), texel_density, uv_stretch, symmetry, thickness, bake_check (1.1: where the "
         "last bake-down found the high-poly only by the nearest point, amber, or guessed, red).",
         schema({{"mode", viewModeEnum()}, {"wireframe", prop("boolean", "Wireframe overlay")},
                 {"seams", prop("boolean", "Show UV seams")},
                 {"isolate_set", prop("boolean", "Draw the other texture sets as dim ghosts (texture set tools)")},
                 {"texture_set", num("integer", "The texture set to show or isolate (index from texture_tiles; -1 = all)", -1, 1023)},
                 {"axis_gizmo", prop("boolean", "The X/Y/Z axis gizmo in the corner of the app's 3D view (not in renders)")}}),
         [](CommandContext& c, const json& a) {
           auto& o = c.engine.viewOptions;
           // checks first, so a refused call changes nothing
           ViewMode mode = a.contains("mode") ? viewModeFromName(a["mode"]) : o.mode;
           if (a.contains("mode") && mode == ViewMode::Symmetry && !c.engine.features.on("symmetry_check"))
             throw CommandError("the symmetry check is switched off (features)");
           if (a.value("isolate_set", false) && !c.engine.features.on("texture_set_tools"))
             throw CommandError("the texture set tools are switched off (features)");
           const TileLayout& tl = c.engine.doc.mesh.tiles;
           bool all = false;
           if (a.contains("texture_set")) {
             int t = a["texture_set"].get<int>();
             all = t == -1;  // (as described: every set, none isolated)
             if (!all && (t < 0 || t >= std::max(1, tl.count()))) throw CommandError("no texture set " + std::to_string(t));
             if (!all) c.engine.uvOptions.tile = t;
           }
           o.mode = mode;
           if (a.contains("isolate_set")) o.isolateSet = a["isolate_set"].get<bool>();
           if (all) o.isolateSet = false;
           o.isolateTile = c.engine.uvOptions.tile;
           c.engine.painter.isolateTile = o.isolateSet && c.engine.doc.mesh.tiles.active() ? o.isolateTile : -1;
           o.wireframe = a.value("wireframe", o.wireframe);
           o.seams = a.value("seams", o.seams);
           o.axisGizmo = a.value("axis_gizmo", o.axisGizmo);
           return json{{"mode", viewModeName(o.mode)}, {"wireframe", o.wireframe}, {"seams", o.seams},
                       {"isolate_set", o.isolateSet}, {"texture_set", c.engine.uvOptions.tile}, {"axis_gizmo", o.axisGizmo}};
         }});
  R.add({"set_environment", "Lighting: optional .hdr equirect path, intensity, rotation (degrees), exposure.",
         schema({{"hdr_path", prop("string", "Equirectangular .hdr")}, {"intensity", num("number", "Strength of the lighting", 0, 10)},
                 {"rotation", num("number", "Turns the lighting round the model, degrees", -360, 360)}, {"exposure", num("number", "Viewport exposure multiplier (display only: exports are not affected)", 0.05, 10)},
                 {"studio", prop("boolean", "Back to procedural studio lighting")}}),
         [](CommandContext& c, const json& a) {
           Environment& env = c.engine.env;
           if (a.contains("hdr_path") && !env.loadHDR(a["hdr_path"])) throw CommandError("could not load HDR");
           if (a.value("studio", false)) env.initStudio();
           env.intensity = a.value("intensity", env.intensity);
           if (a.contains("rotation")) env.rotation = glm::radians(a["rotation"].get<float>());
           env.exposure = a.value("exposure", env.exposure);
           return json{{"name", env.name}, {"intensity", env.intensity}, {"exposure", env.exposure}};
         }});
  R.add({"path_trace",
         "(0.39) The Path traced view mode's settings, and how far its samples have got (set_view {mode: path_traced} "
         "shows it in the app's 3D view; render_view and render_image {path_traced} use the same settings).",
         schema({{"max_samples", num("integer", "The view stops adding samples here", 1, 65536, 512)},
                 {"bounces", num("integer", "Light bounces after the first hit: more lights the insides of folds and cavities", 0, 32, 6)},
                 {"ground", prop("boolean", "A ground under the model that catches its shadow (otherwise invisible; default true)")},
                 {"clamp", num("number", "The brightest an indirect sample may be: lower removes bright speckles (fireflies) sooner; 0 = off", 0, 1000, 12)}}),
         [](CommandContext& c, const json& a) {
           PathTraceSettings& s = c.engine.viewOptions.pathTrace;
           s.maxSamples = std::clamp(a.value("max_samples", s.maxSamples), 1, 65536);
           s.bounces = std::clamp(a.value("bounces", s.bounces), 0, 32);
           s.ground = a.value("ground", s.ground);
           s.clamp = std::clamp(a.value("clamp", s.clamp), 0.f, 1000.f);
           const PathTracer& t = c.engine.renderer.pathTracer();
           return json{{"max_samples", s.maxSamples}, {"bounces", s.bounces}, {"ground", s.ground}, {"clamp", s.clamp},
                       {"samples", t.samples()}, {"done", t.done(s)},
                       {"shown", c.engine.viewOptions.mode == ViewMode::PathTraced}};
         }});
  R.add({"render_view",
         "Renders the 3D view offscreen and returns a PNG (does not change the live window). Optional camera "
         "overrides and view mode.",
         schema({{"width", num("integer", "Image width in pixels", 16, 4096, 1024)}, {"height", num("integer", "Image height in pixels", 16, 4096, 768)},
                 {"mode", viewModeEnum()}, {"camera", prop("object", "set_camera arguments, e.g. {view: \"front\", frame: true}")},
                 {"samples", num("integer", "Path traced mode: samples per pixel (more: less noise, slower)", 1, 4096, 64)}}),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           int w = std::clamp(a.value("width", 1024), 16, 4096), h = std::clamp(a.value("height", 768), 16, 4096);
           Camera cam = e.camera;
           if (a.contains("camera")) {
             if (a["camera"].contains("view")) cam.setView(a["camera"]["view"]);
             cam.fromJson(a["camera"]);
             cam.updateClip(e.clipBounds());
           }
           ViewportOptions o = e.viewOptions;
           if (a.contains("mode")) o.mode = viewModeFromName(a["mode"]);
           if (o.mode == ViewMode::Symmetry && !e.features.on("symmetry_check")) o.mode = ViewMode::Material;
           if (o.mode == ViewMode::PathTraced && !e.features.on("path_tracing")) o.mode = ViewMode::Material;
           if (o.mode == ViewMode::PathTraced) o.pathSamplesNow = std::clamp(a.value("samples", 64), 1, 4096);
           return imageResult(e.renderViewportPng(w, h, &cam, &o), w, h);
         }});
  R.add({"screenshot_window",
         "Captures the live Patina window (what the user sees). In headless mode falls back to render_view.",
         schema({{"max_width", num("integer", "Downscale to at most this width", 64, 8192, 1600)}}),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           if (e.requestWindowCapture) {
             c.deferred = true;
             e.requestWindowCapture(c.promise, a);
             return json();
           }
           int w = (int)e.viewportSize.x, h = (int)e.viewportSize.y;
           return imageResult(e.renderViewportPng(w, h), w, h, {{"note", "headless: rendered 3D view"}});
         }});
  R.add({"wait_frames",
         "Tests: answers once the window has drawn this many more frames (after synthetic input, a click is then "
         "handled and drawn). Headless: at once.",
         schema({{"count", num("integer", "Frames to wait for", 1, 600, 2)}}),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           int n = std::clamp(a.value("count", 2), 1, 600);
           if (e.requestFrameWait) {
             c.deferred = true;
             e.requestFrameWait(c.promise, n);
             return json();
           }
           return json{{"frames", 0}};
         }, false});
  R.add({"render_uv",
         "Renders the UV layout over a texture: channel basecolor | roughness | metallic | height | normal | "
         "emissive | opacity | sheen | coat | subsurface | ao | curvature | checker.",
         schema({{"width", num("integer", "Image width in pixels", 16, 4096, 1024)}, {"height", num("integer", "Image height in pixels", 16, 4096, 1024)},
                 {"channel", prop("string", "Texture under the wireframe")},
                 {"distortion", prop("boolean", "Show area distortion heat map")},
                 {"tile", num("integer", "Texture sets / UDIM tiles: which tile to show (default: the UV editor's)", 0, 1023)}}),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           int w = std::clamp(a.value("width", 1024), 16, 4096), h = std::clamp(a.value("height", 1024), 16, 4096);
           if (a.contains("tile"))
             e.uvOptions.tile = e.viewOptions.isolateTile =
                 std::clamp(a["tile"].get<int>(), 0, std::max(0, e.doc.mesh.tiles.count() - 1));
           std::string ch = a.value("channel", std::string("basecolor"));
           TextureSet& t = e.doc.tex;
           GLuint tex = t.finColor.id();
           int mode = 0;
           if (ch == "roughness") tex = t.finRMH.id(), mode = 1;
           else if (ch == "metallic") tex = t.finRMH.id(), mode = 2;
           else if (ch == "height") tex = t.finRMH.id(), mode = 3;
           else if (ch == "normal") tex = t.finNormal.id(), mode = 5;
           else if (ch == "emissive") tex = t.finEmissive.id();
           else if (ch == "opacity") tex = t.finExtra.id(), mode = 1;
           else if (ch == "sheen") tex = t.finExtra.id(), mode = 2;
           else if (ch == "coat") tex = t.finExtra.id(), mode = 3;
           else if (ch == "subsurface") tex = t.finExtra.id(), mode = 4;
           else if (ch == "ao") tex = t.aoView().id(), mode = 1;
           else if (ch == "curvature") tex = t.curvView().id(), mode = 1;
           else if (ch == "thickness") tex = t.thickView().id(), mode = 1;
           else if (ch == "bent_normal") tex = t.bentMap.id(), mode = 5;
           else if (ch == "checker") tex = 0;
           bool keep = e.uvOptions.distortion;
           e.uvOptions.distortion = a.value("distortion", false);
           auto png = e.renderUVPng(w, h, tex, mode);
           e.uvOptions.distortion = keep;
           return imageResult(png, w, h);
         }});
  R.add({"get_texture", "Returns a final texture map as PNG (basecolor roughness metallic height normal emissive opacity sheen coat subsurface ao).",
         schema({{"channel", prop("string", "Map")}, {"size", num("integer", "Downscale to this size in pixels (0 = full size)", 0, 8192, 512)},
                 {"tile", num("integer", "Texture sets / UDIM tiles: which tile (index from texture_tiles)", 0, 1023, 0)}},
                {"channel"}),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           e.doc.updateComposite(e.materials);
           ExportOptions o;
           o.directory = (paths::cacheDir() / "mcp_tex").string();
           o.size = a.value("size", 512);
           o.baseName = "tex";
           o.extraMaps = 1;
           std::map<std::string, std::string> files;
           std::string err;
           if (!e.doc.exportTextures(o, &files, &err)) throw CommandError(err);
           std::string ch = a.at("channel");
           if (ch == "emissive") ch = "emission";
           auto it = files.find(ch);
           if (e.doc.mesh.tiles.active()) it = files.find(ch + "@" + std::to_string(a.value("tile", 0)));
           if (it == files.end()) throw CommandError("unknown or unavailable channel (or tile)");
           std::string data = paths::readFile(it->second);
           Image img;
           loadImage(it->second, img);
           return imageResult(std::vector<uint8_t>(data.begin(), data.end()), img.width, img.height, {{"path", it->second}});
         }});

  R.add({"texture_check",
         "Measures the finished textures (0.34), so an agent can tune an adjustment layer's develop settings by "
         "numbers: how much of the base color no real material has (non-metals darker than sRGB 30 or brighter than "
         "240, metals darker than 180, metallic half-way: the PBR Range view), the base color's brightness "
         "(percentiles of its sRGB luminance) and color cast, roughness spread, and hints naming the setting to move. "
         "Run it, change a setting with update_layer, run it again.",
         schema({{"tile", num("integer", "Texture sets / UDIM tiles: only this one (index from texture_tiles); -1 = all", -1, 1023, -1)}}),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           Document& d = e.doc;
           if (!d.hasMesh()) throw CommandError("no mesh loaded");
           d.updateComposite(e.materials);
           ivec2 sz = d.tex.size();
           std::vector<uint8_t> col(size_t(sz.x) * sz.y * 4), rmh(col.size()), cov(col.size());
           d.tex.finColor.downloadRaw(GL_RGBA, GL_UNSIGNED_BYTE, col.data(), col.size());
           d.tex.finRMH.downloadRaw(GL_RGBA, GL_UNSIGNED_BYTE, rmh.data(), rmh.size());
           d.tex.posMap.downloadRaw(GL_RGBA, GL_UNSIGNED_BYTE, cov.data(), cov.size());
           int tile = a.value("tile", -1);
           int grid = std::max(1, d.mesh.tiles.grid);
           int tileRes = sz.x / grid;
           size_t step = std::max<size_t>(1, size_t(sz.x) * sz.y / (1u << 20));  // about a million samples at most
           std::vector<float> lum, rough;
           double sr = 0, sg = 0, sb = 0;
           size_t n = 0, dark = 0, bright = 0, metalDark = 0, half = 0, metals = 0, nonMetals = 0;
           for (size_t i = 0; i < size_t(sz.x) * sz.y; i += step) {
             if (cov[i * 4 + 3] < 128) continue;
             if (tile >= 0) {
               int x = int(i % sz.x) / tileRes, y = int(i / sz.x) / tileRes;
               if (y * grid + x != tile) continue;
             }
             float r = col[i * 4] / 255.f, g = col[i * 4 + 1] / 255.f, b = col[i * 4 + 2] / 255.f;
             float L = 0.2126f * r + 0.7152f * g + 0.0722f * b;
             float m = rmh[i * 4 + 1] / 255.f;
             ++n;
             lum.push_back(L);
             rough.push_back(rmh[i * 4] / 255.f);
             if (m > 0.1f && m < 0.9f) ++half;
             if (m >= 0.9f) {
               ++metals;
               metalDark += L < 180.f / 255.f;
             } else if (m <= 0.1f) {
               ++nonMetals;
               dark += L < 30.f / 255.f;
               bright += L > 240.f / 255.f;
               sr += r, sg += g, sb += b;
             }
           }
           if (!n) throw CommandError("no texels to measure (no model faces in this tile?)");
           auto pct = [](std::vector<float>& v, float q) {
             size_t k = std::min(v.size() - 1, size_t(q * (v.size() - 1)));
             std::nth_element(v.begin(), v.begin() + k, v.end());
             return v[k];
           };
           auto share = [](size_t a, size_t b) { return b ? std::round(1000.0 * a / b) / 10.0 : 0.0; };
           float p5 = pct(lum, 0.05f), p50 = pct(lum, 0.5f), p95 = pct(lum, 0.95f);
           float r5 = pct(rough, 0.05f), r50 = pct(rough, 0.5f), r95 = pct(rough, 0.95f);
           vec3 mean = nonMetals ? vec3(float(sr / nonMetals), float(sg / nonMetals), float(sb / nonMetals)) : vec3(0.f);
           float warm = mean.x - mean.z, green = mean.y - 0.5f * (mean.x + mean.z);
           json hints = json::array();
           double pDark = share(dark, nonMetals), pBright = share(bright, nonMetals), pMetal = share(metalDark, metals),
                  pHalf = share(half, n);
           if (pDark > 2) hints.push_back("non-metal color darker than sRGB 30 on " + std::to_string(int(pDark)) +
                                          "%: raise exposure (+0.3 to +1) or shadows; if it is baked-in shading, delight (bake the AO first)");
           if (pBright > 2) hints.push_back("non-metal color brighter than sRGB 240 on " + std::to_string(int(pBright)) +
                                            "%: lower exposure or highlights");
           if (pMetal > 5) hints.push_back("metal darker than sRGB 180 on " + std::to_string(int(pMetal)) +
                                           "%: brighten the metal (an adjustment layer masked to it, exposure up), or check it should be metal");
           if (pHalf > 5) hints.push_back("metallic half-way on " + std::to_string(int(pHalf)) +
                                          "%: engines expect 0 or 1 except along borders (a metallic offset on an adjustment layer, or the material's metallic)");
           if (nonMetals && warm > 0.12f) hints.push_back("a warm (orange) cast: temperature below 0 makes it neutral, if it should be");
           if (nonMetals && warm < -0.12f) hints.push_back("a cool (blue) cast: temperature above 0 makes it neutral, if it should be");
           if (nonMetals && std::abs(green) > 0.08f)
             hints.push_back(green > 0 ? "a green cast: tint above 0 takes it out" : "a magenta cast: tint below 0 takes it out");
           if (p95 - p5 < 0.1f && n > 100) hints.push_back("very flat color: contrast above 1 or vibrance spreads it, if that is wanted");
           return json{{"texels", n},
                       {"basecolor", {{"luminance", {{"p5", p5}, {"p50", p50}, {"p95", p95}}},
                                      {"mean_non_metal", {mean.x, mean.y, mean.z}},
                                      {"cast", {{"warm", warm}, {"green", green}}},
                                      {"non_metal_too_dark_pct", pDark},
                                      {"non_metal_too_bright_pct", pBright},
                                      {"metal_too_dark_pct", pMetal}}},
                       {"metallic", {{"metal_pct", share(metals, n)}, {"half_metal_pct", pHalf}}},
                       {"roughness", {{"p5", r5}, {"p50", r50}, {"p95", r95}}},
                       {"pbr_ok", pDark <= 2 && pBright <= 2 && pMetal <= 5 && pHalf <= 5},
                       {"hints", hints}};
         }});

  // ---------------------------------------------------------------- bakes
  R.add({"bake_mesh_maps",
         "Bakes mesh maps: ambient occlusion (with bent normals), curvature (with the high-poly normal map's "
         "detail when one is baked) and thickness. Smart masks use them; AO, thickness and bent normals export too.",
         schema({{"ao", prop("boolean", "Bake AO and bent normals (default true)")},
                 {"curvature", prop("boolean", "Bake curvature (default true)")},
                 {"curvature_detail", prop("boolean", "Add the high-poly normal map's detail to the curvature (default true)")},
                 {"thickness", prop("boolean", "Bake thickness (default false: it casts rays, so it takes longer)")},
                 {"ao_samples", num("integer", "AO directions: 64 is fast, 256 smoother on complex models", 4, 1024, 64)},
                 {"thickness_rays", num("integer", "Rays per texel for thickness: more is smoother and slower", 4, 512, 32)},
                 {"thickness_distance", num("number", "How deep thickness looks, object units; -1 = half the model's radius. Lower it to find thinness at a smaller scale", -1, 1000000, -1)}}),
         [](CommandContext& c, const json& a) {
           Document& d = c.engine.doc;
           if (!d.hasMesh()) throw CommandError("no mesh loaded");
           d.aoSamples = std::clamp(a.value("ao_samples", d.aoSamples), 4, 1024);
           d.thicknessRays = std::clamp(a.value("thickness_rays", d.thicknessRays), 4, 512);
           if (a.value("ao", true)) d.tex.bakeAO(d.mesh, d.gpu, d.aoSamples);
           if (progress::cancelled()) throw CommandError("cancelled");
           if (a.value("curvature", true)) d.tex.bakeCurvature(d.mesh, d.gpu, &d.bvh, 1.f, a.value("curvature_detail", true));
           if (a.value("thickness", false))
             d.tex.bakeThickness(d.mesh, d.bvh, d.thicknessRays, a.value("thickness_distance", -1.f));
           if (progress::cancelled()) throw CommandError("cancelled");
           d.markComposite();
           return json{{"ao", d.tex.hasAO()}, {"bent_normals", d.tex.hasBentNormals()}, {"curvature", d.tex.hasCurvature()},
                       {"curvature_detail", d.tex.curvatureHasDetail()}, {"thickness", d.tex.hasThickness()}};
         }});

  // ---------------------------------------------------------------- UVs
  auto commitUV = [](Engine& e, Mesh& work, bool reproject, const std::string& label) {
    // (1.2.7) UVs that moved less than a quarter of a texel look the same: no undo step (with "keep paint" one
    // copies every layer), and the result says nothing changed. Orienting an island that was already upright
    // turned it by a hair, and the editor said "Oriented upright" for a change no one could see.
    const Mesh& m = e.doc.mesh;
    float tol = 0.25f / float(std::max(e.doc.tex.resolution(), 1024)), moved = 0;
    for (size_t l = 0; l < work.loopUV.size() && l < m.loopUV.size(); ++l)
      moved = std::max(moved, glm::length(work.loopUV[l] - m.loopUV[l]));
    bool flags = work.edgeFlags != m.edgeFlags || (work.faceFlags != m.faceFlags && work.faceFlags.size() == m.faceCount());
    if (moved < tol && !flags) return false;
    // seams and face flags may have changed too: one undo step for all of it
    return e.doc.applyUVs(work.loopUV, reproject, label, &work.edgeFlags, &work.faceFlags);
  };
  auto needTargets = [](Engine& e, const std::vector<uint32_t>& faces) {
    bool blocked = false;
    auto t = layoutTargets(e, faces, &blocked);
    if (t.empty())
      throw CommandError(blocked ? "those islands are pinned (unpin them first: uv_pin with pin=false)"
                                 : "no faces to work on");
    return t;
  };
  R.add({"uv_unwrap",
         "Unwraps UVs: lscm (respects seams), smart (angle-based charts), box, planar (view direction), cylinder "
         "(unrolled around an axis, ends planar; for limbs, barrels, tubes), sphere (longitude / latitude). Painted "
         "layers are reprojected onto the new layout unless reproject=false.",
         schema({{"method", propEnum({"lscm", "smart", "box", "planar", "cylinder", "sphere"}, "Unwrap method")},
                 {"angle", num("number", "Smart project: faces bending more than this many degrees start a new island. Lower: more, flatter islands; higher: fewer, more stretched ones", 1, 89, 66)},
                 {"axis", propEnum({"auto", "x", "y", "z"}, "Cylinder / sphere axis (default auto: the longest extent; sphere: y)")},
                 {"faces", prop("array", "Only these faces (default all)")},
                 {"selection", prop("boolean", "Only selected faces")},
                 {"margin", num("number", "Gap between islands in UV units: 0.005 = half a percent of the square. More avoids color bleeding between islands at small texture sizes; less uses the space better", 0, 0.1, 0.005)},
                 {"reproject", prop("boolean", "Keep existing paint (default true)")}},
                {"method"}),
         [commitUV, needTargets](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           Mesh work = e.doc.mesh;
           auto targets = needTargets(e, facesArg(e, a));
           bool pins = uv::pinnedFaceCount(work) > 0;
           std::string m = a.at("method");
           float margin = a.value("margin", 0.005f);
           // texture tiles: each tile is unwrapped and packed into its own square
           uv::forEachTile(work, targets.size() == work.faceCount() ? std::vector<uint32_t>{} : targets,
                           [&](const std::vector<uint32_t>& tileFaces) {
           auto faces = tileFaces;
           if (!work.tiles.active() && faces.size() == work.faceCount()) faces.clear();  // everything: same as no list
           if (m == "lscm") {
             uv::UnwrapOptions o;
             o.margin = margin;
             o.pack = !pins;
             uv::unwrapLSCM(work, faces, o);
           } else if (m == "smart") {
             uv::smartProject(work, a.value("angle", 66.f), faces, margin);
           } else if (m == "box") {
             uv::projectBox(work, faces, margin);
           } else if (m == "planar") {
             uv::projectPlanar(work, -e.camera.forward(), faces);
           } else if (m == "cylinder" || m == "sphere") {
             std::string ax = a.value("axis", std::string("auto"));
             int axis = ax == "x" ? 0 : ax == "y" ? 1 : ax == "z" ? 2 : -1;
             if (m == "cylinder") uv::projectCylinder(work, faces, axis, margin);
             else uv::projectSphere(work, faces, axis, margin);
           } else {
             throw CommandError("unknown method");
           }
           if (pins) {  // the new islands go around the pinned ones
             work.computeUVTopology();
             uv::packAroundFixed(work, faces, margin);
           }
           });
           commitUV(e, work, a.value("reproject", true), "Unwrap (" + m + ")");
           return json{{"islands", e.doc.mesh.islandCount}, {"coverage", uv::coverage(e.doc.mesh)}};
         }});
  R.add({"uv_relax",
         "Relaxes UV islands: each island of the faces is solved again without stretch (conformal) and fitted back "
         "onto where it was, at the same size. Pinned islands are left alone. Paint is reprojected. (1.2.7) changed is "
         "false when nothing moved by a quarter of a texel or more (the islands were already relaxed): no undo step then.",
         schema({{"faces", prop("array", "Faces whose islands to relax (default all)")},
                 {"selection", prop("boolean", "Use the selected faces")},
                 {"strength", num("number", "How far toward the relaxed layout: 0 keeps the old one, 1 fully relaxed", 0, 1, 1)},
                 {"reproject", prop("boolean", "Keep existing paint (default true)")}}),
         [commitUV, needTargets](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           Mesh work = e.doc.mesh;
           auto faces = needTargets(e, facesArg(e, a));
           if (faces.empty()) throw CommandError("no unpinned faces to relax");
           int n = uv::relaxIslands(work, faces, a.value("strength", 1.f));
           bool changed = commitUV(e, work, a.value("reproject", true), "Relax UVs");
           return json{{"islands_relaxed", n}, {"changed", changed}};
         }});
  R.add({"uv_pack", "Packs UV islands into 0..1 (optionally rotating and equalizing texel density). Islands are "
                    "packed by their outlines, nesting into each other's gaps (quality \"tight\", default), or by "
                    "bounding boxes (\"fast\"); with \"tight\" the better of the two is kept.",
         schema({{"margin", num("number", "Gap between islands in UV units: 0.005 = half a percent of the square. More avoids color bleeding between islands at small texture sizes; less uses the space better", 0, 0.1, 0.005)}, {"rotate", prop("boolean", "Allow rotation")},
                 {"quality", propEnum({"tight", "fast"}, "tight: island outlines (default); fast: bounding boxes")},
                 {"normalize_density", prop("boolean", "Equal texel density")}, {"faces", prop("array", "Only islands of these faces")},
                 {"free_space", prop("boolean", "Place only the islands of faces / selection into free space; others stay")},
                 {"reproject", prop("boolean", "Keep existing paint (default true)")}}),
         [commitUV, needTargets](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           Mesh work = e.doc.mesh;
           uv::PackOptions o;
           o.margin = a.value("margin", 0.005f);
           o.rotate = a.value("rotate", true);
           o.normalizeDensity = a.value("normalize_density", true);
           o.shapes = a.value("quality", std::string("tight")) != "fast";
           bool tiles = work.tiles.active();
           if (a.value("free_space", false)) {
             auto faces = facesArg(e, a);
             if (faces.empty()) faces = e.doc.selectedFaces();
             if (faces.empty()) throw CommandError("free_space needs faces or a selection");
             faces = needTargets(e, faces);
             bool ok = true;
             uv::forEachTile(work, faces, [&](const std::vector<uint32_t>& f) { ok &= uv::packIntoFreeSpace(work, f, o.margin); });
             if (!ok) throw CommandError("not enough free UV space");
           } else {
             auto given = facesArg(e, a);
             auto targets = needTargets(e, given);
             bool pinned = uv::pinnedFaceCount(work) > 0, ok = true;
             // texture tiles: every tile is packed into its own square
             uv::forEachTile(work, given.empty() ? std::vector<uint32_t>{} : targets, [&](const std::vector<uint32_t>& f) {
               std::vector<uint32_t> faces = given.empty() && !tiles ? std::vector<uint32_t>{} : uv::unpinnedFaces(work, f);
               if (tiles && faces.empty()) return;
               if (pinned) {
                 // pinned islands stay; the others are packed into the space around them
                 ok &= uv::packAroundFixed(work, faces, o.margin);
               } else {
                 uv::pack(work, o, faces);
               }
             });
             if (!ok) throw CommandError("not enough free UV space around the pinned islands");
           }
           commitUV(e, work, a.value("reproject", true), "Pack UVs");
           return json{{"islands", e.doc.mesh.islandCount}, {"coverage", uv::coverage(e.doc.mesh)}};
         }});
  R.add({"texture_set_from_faces",
         "Gives faces a texture set (their own texture space) of their own, such as one for the head and one for "
         "the body. With one texture or texture sets per material slot, the faces go into the material slot called "
         "'name' (a new slot unless one has that name; Blender gets it with the next send). With UDIM tiles, their "
         "islands move to the next free UDIM tile. pack (default true) then packs each set the faces left or joined, "
         "so every set fills its own space. Paint moves with the faces. Clears the undo history.",
         schema({{"faces", prop("array", "Face indices")}, {"selection", prop("boolean", "Use the selected faces")},
                 {"part", prop("string", "A part from mesh_parts (such as region:head, slot:2, piece:1)")},
                 {"name", prop("string", "The texture set's name (material slot), such as Head")},
                 {"pack", prop("boolean", "Pack the sets the faces left and joined (default true)")},
                 {"margin", num("number", "Gap between islands in UV units: 0.005 = half a percent of the square. More avoids color bleeding between islands at small texture sizes; less uses the space better", 0, 0.1, 0.005)}}),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           Document& d = e.doc;
           std::vector<uint32_t> faces = a.contains("part") ? e.partFaces(a["part"].get<std::string>()) : facesArg(e, a);
           if (faces.empty()) throw CommandError("no faces given: select the faces for the new texture set first");
           // mirrored halves stacked on each other share their texels: both go into the set
           faces = uv::withLinkedFaces(d.mesh, d.mirrorLinks(), faces);
           const uint32_t F = d.mesh.faceCount();
           auto keyOf = [&](int t) { return t >= 0 && t < (int)d.mesh.tiles.tiles.size() ? d.mesh.tiles.tiles[t] : TileInfo{}; };
           std::vector<TileInfo> left;
           if (d.mesh.tiles.active())
             for (uint32_t f : faces)
               if (f < F) {
                 TileInfo k = keyOf(d.mesh.faceTile(f));
                 if (std::find(left.begin(), left.end(), k) == left.end()) left.push_back(k);
               }
           std::string why, setName;
           int udim = 0, index = -1;
           bool udimOnly = (d.mesh.tiles.mode & TileByUdim) && !(d.mesh.tiles.mode & TileBySlot);
           if (udimOnly) {
             udim = d.moveFacesToNewUdim(faces, &why);
             if (!udim) throw CommandError(why);
             for (int t = 0; t < d.mesh.tiles.count(); ++t)
               if (d.mesh.tiles.tiles[t].udim == udim) index = t;
           } else {
             std::string name = a.value("name", std::string());
             if (name.empty()) name = "Set " + std::to_string(d.mesh.tiles.active() ? d.mesh.tiles.count() + 1 : 2);
             index = d.moveFacesToSlotSet(faces, name, &why);
             if (index < 0) throw CommandError(why);
           }
           if (index >= 0) setName = d.mesh.tiles.tiles[index].name;
           bool packed = false;
           json keptLayout = json::array();
           if (a.value("pack", true)) {
             // The new set fills its own square: its islands are in a texture of their own now, so
             // pinned ones move too (and stay pinned). The sets the faces left are packed again
             // when none of their islands is pinned; otherwise they keep their layout.
             std::vector<int> sets = {index};
             for (auto& k : left)
               for (int t = 0; t < d.mesh.tiles.count(); ++t)
                 if (d.mesh.tiles.tiles[t] == k && std::find(sets.begin(), sets.end(), t) == sets.end()) sets.push_back(t);
             Mesh work = d.mesh;
             uv::PackOptions o;
             o.margin = a.value("margin", 0.005f);
             o.rotate = true;
             o.normalizeDensity = false;  // the islands keep their sizes against each other
             const uv::MirrorLinks& L = d.mirrorLinks();
             for (int t : sets) {
               std::vector<uint8_t> in(F, 0);
               bool anyPinned = false;
               for (uint32_t f = 0; f < F; ++f) {
                 if (work.faceTile(f) != t) continue;
                 anyPinned |= uv::isPinned(work, f);
                 uint32_t p = f < L.primary.size() && L.primary[f] >= 0 ? (uint32_t)L.primary[f] : f;  // copies follow
                 in[p] = 1;
               }
               if (t != index && anyPinned) {
                 keptLayout.push_back(d.mesh.tiles.tiles[t].name);
                 continue;
               }
               std::vector<uint32_t> tf;
               for (uint32_t f = 0; f < F; ++f)
                 if (in[f]) tf.push_back(f);
               if (tf.empty()) continue;
               uv::forEachTile(work, tf, [&](const std::vector<uint32_t>& f) { uv::pack(work, o, f); });
               packed = true;
             }
             if (packed) d.applyUVs(work.loopUV, true, "Pack the texture sets", &work.edgeFlags, &work.faceFlags, true);
           }
           if (e.onExternalChange) e.onExternalChange("tiles");
           json tiles = json::array();
           for (int t = 0; t < d.mesh.tiles.count(); ++t) {
             int n = 0;
             for (uint32_t f = 0; f < F; ++f) n += d.mesh.faceTile(f) == t;
             tiles.push_back({{"index", t}, {"name", d.mesh.tiles.tiles[t].name}, {"faces", n}});
           }
           return json{{"set", setName}, {"index", index}, {"udim", udim}, {"faces", faces.size()}, {"packed", packed},
                       {"kept_layout", keptLayout}, {"tiles", tiles}};
         }});
  R.add({"uv_transform",
         "Moves/rotates/scales the UVs of faces (or whole islands containing them). Paint follows (reprojected).",
         schema({{"faces", prop("array", "Face indices")}, {"selection", prop("boolean", "Use selected faces")},
                 {"islands", prop("boolean", "Expand to whole UV islands")},
                 {"translate", prop("array", "[du,dv]")}, {"rotate", num("number", "Turns the islands, degrees counter-clockwise", -360, 360)},
                 {"scale", prop("array", "[su,sv] or number")}, {"pivot", prop("array", "[u,v] (default bounds center)")},
                 {"reproject", prop("boolean", "Keep existing paint (default true)")}}),
         [commitUV](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           Mesh work = e.doc.mesh;
           auto faces = facesArg(e, a);
           if (faces.empty()) throw CommandError("no faces given");
           if (a.value("islands", false)) {
             std::vector<uint8_t> isl(work.islandCount, 0);
             for (uint32_t f : faces) isl[work.loopIsland[work.faceStart[f]]] = 1;
             faces.clear();
             for (uint32_t f = 0; f < work.faceCount(); ++f)
               if (isl[work.loopIsland[work.faceStart[f]]]) faces.push_back(f);
           }
           faces = uv::unpinnedFaces(work, faces);
           if (faces.empty()) throw CommandError("those faces are pinned (unpin them first: uv_pin with pin=false)");
           auto loops = uv::loopsOfFaces(work, faces);
           Rect2 b = uv::boundsOfLoops(work, loops);
           vec2 pivot = a.contains("pivot") ? vec2(a["pivot"][0].get<float>(), a["pivot"][1].get<float>()) : b.center();
           if (a.contains("scale")) {
             vec2 s = a["scale"].is_number() ? vec2(a["scale"].get<float>())
                                             : vec2(a["scale"][0].get<float>(), a["scale"][1].get<float>());
             uv::scale(work, loops, s, pivot);
           }
           if (a.contains("rotate")) uv::rotate(work, loops, glm::radians(a["rotate"].get<float>()), pivot);
           if (a.contains("translate"))
             uv::translate(work, loops, vec2(a["translate"][0].get<float>(), a["translate"][1].get<float>()));
           commitUV(e, work, a.value("reproject", true), "Transform UVs");
           return json{{"loops", loops.size()}};
         }});
  R.add({"uv_seams", "Seam editing: from_islands, by_angle, clear, mark or unmark edges (edge indices), or path: a "
                     "seam along the shortest chain of edges through the given vertices or points (like the Seams "
                     "brush; vertices at one position count as one).",
         schema({{"mode", propEnum({"from_islands", "by_angle", "clear", "mark", "unmark", "path"}, "Operation")},
                 {"angle", num("number", "by_angle: edges sharper than this many degrees become seams", 1, 179, 60)},
                 {"edges", prop("array", "Edge indices for mark/unmark")},
                 {"vertices", prop("array", "path: mesh vertex indices to pass through, in order")},
                 {"points", prop("array", "path: [x,y,z] model-space points (nearest vertex each), in order")},
                 {"unmark", prop("boolean", "path: clear the seam along the path instead")}},
                {"mode"}),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           Mesh work = e.doc.mesh;
           std::string m = a.at("mode");
           if (m == "from_islands") uv::seamsFromIslands(work, true);
           else if (m == "by_angle") uv::seamsByAngle(work, a.value("angle", 60.f), false);
           else if (m == "clear")
             for (auto& f : work.edgeFlags) f &= ~EdgeSeam;
           else if (m == "path") {
             uv::SeamGraph g;
             g.build(work);
             std::vector<uint32_t> via;
             for (auto& v : a.value("vertices", json::array())) {
               uint32_t i = v;
               if (i >= work.positions.size()) throw CommandError("vertex out of range");
               via.push_back(g.weld[i]);
             }
             for (auto& p : a.value("points", json::array())) {
               vec3 q(p.at(0).get<float>(), p.at(1).get<float>(), p.at(2).get<float>());
               uint32_t best = 0;
               float bd = 1e30f;
               for (uint32_t i = 0; i < g.pos.size(); ++i)
                 if (float d = glm::length(g.pos[i] - q); d < bd) bd = d, best = i;
               via.push_back(best);
             }
             if (via.size() < 2) throw CommandError("path needs at least two vertices or points");
             size_t edges = 0;
             for (size_t i = 1; i < via.size(); ++i) {
               auto p = g.path(via[i - 1], via[i]);
               if (p.empty() && via[i - 1] != via[i]) throw CommandError("no path between those vertices (separate parts)");
               g.apply(work.edgeFlags, p, !a.value("unmark", false));
               edges += p.size();
             }
             e.doc.applySeams(work.edgeFlags, "Seam path");
             return json{{"path_edges", edges}};
           } else
             for (auto& ei : a.value("edges", json::array())) {
               uint32_t i = ei;
               if (i >= work.edgeFlags.size()) continue;
               if (m == "mark") work.edgeFlags[i] |= EdgeSeam;
               else work.edgeFlags[i] &= ~EdgeSeam;
             }
           e.doc.applySeams(work.edgeFlags, "Seams: " + m);
           size_t n = 0;
           for (auto f : e.doc.mesh.edgeFlags) n += (f & EdgeSeam) ? 1 : 0;
           return json{{"seam_edges", n}};
         }});
  R.add({"uv_info", "UV statistics: islands, coverage of 0..1, overlaps, texel density, seam count, pinned faces, "
                    "mirrored (stacked) faces and how many of those have drifted off their other half, and faces with no "
                    "UV area (collapsed onto a point or a line: they get no paint).", schema(),
         [](CommandContext& c, const json&) {
           Mesh& m = c.engine.doc.mesh;
           size_t seams = 0;
           for (auto f : m.edgeFlags) seams += (f & EdgeSeam) ? 1 : 0;
           Rect2 b = uv::boundsOfLoops(m, uv::loopsOfFaces(m, {}));
           const uv::MirrorLinks& L = c.engine.doc.mirrorLinks();
           return json{{"islands", m.islandCount},     {"coverage", uv::coverage(m)},
                       {"overlaps", uv::overlapCount(m)}, {"overlapping_islands", uv::overlappingIslands(m).size()},
                       {"texel_density", uv::texelDensity(m)},
                       {"seam_edges", seams},           {"bounds", {b.lo.x, b.lo.y, b.hi.x, b.hi.y}},
                       {"pinned_faces", uv::pinnedFaceCount(m)}, {"mirrored_faces", L.copies},
                       {"unstacked_mirror_faces", uv::unstackedCopies(m, L)},
                       {"faces_without_uv_area", uv::collapsedFaces(m).size()}};
         }});
  auto densityJson = [](Engine& e, const uv::DensityTargets& t) {
    float sel = 0;
    auto faces = e.doc.selectedFaces();
    if (!faces.empty()) sel = uv::areaDensity(e.doc.mesh, faces);
    int R = e.doc.tileRes();
    auto px = [&](float d) { return d * R; };
    return json{{"resolution", R},
                {"texels_per_unit", {{"fit", px(t.fit)}, {"screen", px(t.screen)}, {"layout", px(t.layout)},
                                     {"recommended", px(t.recommended())}, {"selection", px(sel)}}},
                {"texture_too_small", t.screen > t.fit * 1.05f},
                {"note", "texels per object unit (1 unit = 1 m in Blender); fit = whole surface fills 70% of the "
                         "texture; screen = 1 texel per pixel with the model framed in a 1080p view"}};
  };
  auto densityArg = [](Engine& e, const json& a, const std::vector<uint8_t>* exclude) -> float {
    auto t = uv::densityTargets(e.doc.mesh, e.doc.tileRes(), exclude);
    if (!a.contains("density")) return t.recommended();
    const json& d = a["density"];
    if (d.is_number()) return d.get<float>() / float(e.doc.tileRes());  // texels per unit
    std::string s = d;
    if (s == "fit") return t.fit;
    if (s == "screen") return t.screen;
    if (s == "layout") return t.layout > 0 ? t.layout : t.recommended();
    return t.recommended();
  };
  R.add({"uv_make_island",
         "Island brush: turns faces into one UV island. Cuts seams around them, unwraps (LSCM), orients it "
         "(upright = object up is texture up), scales it to the target texel density and places it in free UV "
         "space without moving other islands (repacking everything only if there is no room). Paint is kept.",
         schema({{"faces", prop("array", "Face indices")}, {"selection", prop("boolean", "Use the selected faces (default when no faces)")},
                 {"orient", propEnum({"upright", "longest_axis", "none"}, "Island orientation (default upright)")},
                 {"density", prop("string", "auto (default) | layout | fit | screen, or a number of texels per unit")},
                 {"margin", num("number", "Gap between islands in UV units: 0.005 = half a percent of the square. More avoids color bleeding between islands at small texture sizes; less uses the space better", 0, 0.1, 0.005)},
                 {"repack_if_full", prop("boolean", "Repack all islands when there is no free space (default true)")},
                 {"symmetry", prop("array", "Mirror axes, e.g. [\"x\"]: the mirrored half gets the same (mirrored) UVs, "
                                            "stacked in one place; its faces are added when missing")},
                 {"pin", prop("boolean", "Pin the new island(s) so later UV edits leave them in place (default false)")},
                 {"reproject", prop("boolean", "Keep existing paint (default true)")}}),
         [commitUV, densityArg](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           auto faces = facesArg(e, a);
           if (faces.empty()) faces = e.doc.selectedFaces();
           if (faces.empty()) throw CommandError("no faces: paint over the model with the Island brush or pass faces");
           size_t asked = faces.size();
           faces = uv::unpinnedFaces(e.doc.mesh, faces);
           if (faces.empty()) throw CommandError("those faces are pinned (unpin them first: uv_pin with pin=false)");
           int pinnedLeft = int(asked - faces.size());
           Mesh work = e.doc.mesh;
           std::vector<uint8_t> in(work.faceCount(), 0);
           for (uint32_t f : faces) in[f] = 1;
           uv::IslandOptions o;
           o.orient = uv::orientFromName(a.value("orient", std::string("upright")));
           o.resolution = e.doc.tileRes();
           o.uvDensity = densityArg(e, a, &in);
           o.margin = a.value("margin", 0.005f);
           o.repackIfFull = a.value("repack_if_full", true);
           int axes = 0;
           if (a.contains("symmetry")) {
             const json& sy = a["symmetry"];
             auto axis = [&](const std::string& n) { return n == "x" ? 1 : n == "y" ? 2 : n == "z" ? 4 : 0; };
             if (sy.is_array())
               for (auto& v : sy) axes |= v.is_string() ? axis(v.get<std::string>()) : 0;
             else if (sy.is_object())
               for (auto k : {"x", "y", "z"}) axes |= sy.value(k, false) ? axis(k) : 0;
             else if (sy.is_string())
               axes = axis(sy.get<std::string>());
           }
           int stacked = 0;
           uv::IslandResult r;
           if (work.faceFlags.size() != work.faceCount()) work.faceFlags.assign(work.faceCount(), 0);
           if (axes) faces = uv::unpinnedFaces(work, uv::withMirroredFaces(work, e.doc.bvh, faces, axes));
           else
             for (uint32_t f : faces) work.faceFlags[f] &= ~kFaceMirrorBits;  // a plain island copies nothing
           // texture tiles: a region spanning several tiles becomes an island in each, at one density
           r.ok = false;
           std::string failed;
           uv::forEachTile(work, faces, [&](const std::vector<uint32_t>& f) {
             int st = 0;
             uv::IslandResult ri = axes ? uv::makeSymmetricIsland(work, e.doc.bvh, f, axes, o, &st) : uv::makeIsland(work, f, o);
             stacked += st;
             if (!ri.ok) {
               failed = ri.message;
               return;
             }
             r.charts += r.ok ? ri.charts : 0;
             if (!r.ok) r = ri;
             r.repacked |= ri.repacked;
           });
           if (!failed.empty() && r.ok) r.message = failed;
           if (!r.ok) throw CommandError(r.message.empty() ? "could not make the island" : r.message);
           bool pin = a.value("pin", false);
           if (pin)
             for (uint32_t f : faces) work.faceFlags[f] |= FacePinned;
           commitUV(e, work, a.value("reproject", true), axes ? "Make symmetric island" : "Make island");
           return json{{"islands_made", r.charts}, {"repacked", r.repacked}, {"message", r.message}, {"stacked_faces", stacked},
                       {"pinned", pin}, {"pinned_faces_left_alone", pinnedLeft},
                       {"texels_per_unit", r.uvDensity * e.doc.tileRes()}, {"islands", e.doc.mesh.islandCount},
                       {"overlaps", uv::overlapCount(e.doc.mesh)}};
         }});
  R.add({"uv_density",
         "Texel density: targets (fit the texture, 1 texel per pixel on screen, the existing layout) and the "
         "selection's density; with set, scales the islands of the faces / selection to a density. With scale, the "
         "islands get that many times their texel density (2: twice the texture detail, 0.5: half) and the layout is "
         "packed again so they fit (pack: false leaves it), for example more texture space for the head.",
         schema({{"set", prop("string", "auto | layout | fit | screen, or texels per unit (omit to only read)")},
                 {"scale", json{{"type", "number"}, {"minimum", 0.25}, {"maximum", 4.0}, {"description", "Times the islands' texel density (0.25..4)"}}},
                 {"pack", prop("boolean", "With scale: pack every island again so they fit (default true)")},
                 {"faces", prop("array", "Faces whose islands to scale (default selection)")},
                 {"selection", prop("boolean", "Use the selected faces")},
                 {"reproject", prop("boolean", "Keep existing paint (default true)")}}),
         [commitUV, densityArg, densityJson, needTargets](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           if (a.contains("scale")) {
             auto faces = facesArg(e, a);
             if (faces.empty()) faces = e.doc.selectedFaces();
             if (faces.empty()) throw CommandError("no faces to work on (select some, or give faces)");
             // the faces asked about change size, pinned or not (they stay pinned); mirrored copies follow
             const uv::MirrorLinks& L = e.doc.mirrorLinks();
             std::vector<uint8_t> mine(e.doc.mesh.faceCount(), 0);
             for (uint32_t f : faces) {
               if (f >= mine.size()) continue;
               mine[f < L.primary.size() && L.primary[f] >= 0 ? (uint32_t)L.primary[f] : f] = 1;
             }
             faces.clear();
             for (uint32_t f = 0; f < mine.size(); ++f)
               if (mine[f]) faces.push_back(f);
             float k = std::clamp(a["scale"].get<float>(), 0.25f, 4.f);
             Mesh work = e.doc.mesh;
             // each island's share of the faces about its own center (an island that crosses the
             // part's border comes apart there)
             for (auto& isl : uv::facesByIsland(work, faces)) {
               auto loops = uv::loopsOfFaces(work, isl);
               uv::scale(work, loops, vec2(k), uv::boundsOfLoops(work, loops).center());
             }
             if (a.value("pack", true)) {
               // everything again, keeping the islands' sizes against each other; pinned islands
               // elsewhere stay where they are
               work.computeUVTopology();
               std::vector<uint8_t> flags = work.faceFlags;
               if (work.faceFlags.size() == work.faceCount())
                 for (uint32_t f : uv::withLinkedFaces(work, L, faces)) work.faceFlags[f] &= ~FacePinned;
               uv::PackOptions o;
               o.normalizeDensity = false;
               o.rotate = true;
               bool tiles = work.tiles.active(), pinned = uv::pinnedFaceCount(work) > 0;
               uv::forEachTile(work, {}, [&](const std::vector<uint32_t>& f) {
                 std::vector<uint32_t> fs = !tiles ? std::vector<uint32_t>{} : uv::unpinnedFaces(work, f);
                 if (tiles && fs.empty()) return;
                 if (pinned) uv::packAroundFixed(work, fs, o.margin, true);
                 else uv::pack(work, o, fs);
               });
               if (flags.size() == work.faceFlags.size()) work.faceFlags = flags;
             }
             e.doc.applyUVs(work.loopUV, a.value("reproject", true), k >= 1 ? "More texture space" : "Less texture space",
                            &work.edgeFlags, &work.faceFlags, true);
           } else if (!a.contains("set") && a.contains("faces")) {  // only measuring these faces
             json r = densityJson(e, uv::densityTargets(e.doc.mesh, e.doc.tileRes()));
             r["faces_density"] = uv::areaDensity(e.doc.mesh, facesArg(e, a)) * e.doc.tileRes();
             return r;
           } else if (a.contains("set")) {
             auto faces = facesArg(e, a);
             if (faces.empty()) faces = e.doc.selectedFaces();
             faces = needTargets(e, faces);
             Mesh work = e.doc.mesh;
             std::vector<uint8_t> in(work.faceCount(), 0);
             for (uint32_t f : faces) in[f] = 1;
             json da = {{"density", a["set"]}};
             float dens = densityArg(e, da, &in);
             uv::forEachTile(work, faces, [&](const std::vector<uint32_t>& f) { uv::setIslandDensity(work, f, dens); });
             commitUV(e, work, a.value("reproject", true), "Set texel density");
           }
           return densityJson(e, uv::densityTargets(e.doc.mesh, e.doc.tileRes()));
         }});
  R.add({"uv_rectify",
         "Straightens islands made of quads (strips and grids: belts, pipes, trims, planks) into rectangles on an "
         "axis-aligned grid, each quad as long and wide as on the model. Islands with triangles or n-gons, or that "
         "are not a clean grid, are left alone. Paint is kept.",
         schema({{"faces", prop("array", "Faces whose islands to straighten (default selection, or all)")},
                 {"selection", prop("boolean", "Use the selected faces")},
                 {"reproject", prop("boolean", "Keep paint (default true)")}}),
         [commitUV, needTargets](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           auto faces = facesArg(e, a);
           if (faces.empty()) faces = e.doc.selectedFaces();
           faces = needTargets(e, faces);
           Mesh work = e.doc.mesh;
           uv::RectifyResult r;
           uv::forEachTile(work, faces, [&](const std::vector<uint32_t>& f) {
             auto ri = uv::rectifyQuadIslands(work, f);
             r.islands += ri.islands, r.skipped += ri.skipped;
             if (!ri.message.empty()) r.message = ri.message;
           });
           if (r.islands) commitUV(e, work, a.value("reproject", true), "Rectify quad islands");
           return json{{"rectified", r.islands}, {"skipped", r.skipped}, {"message", r.message},
                       {"overlaps", uv::overlapCount(e.doc.mesh)}};
         }});
  R.add({"uv_mirror",
         "Mirrors UVs across the model's symmetry: faces on the negative side of the axes take the (mirrored) UVs "
         "of their mirror image on the positive side, so both halves share one place in the texture.",
         schema({{"axes", prop("array", "Mirror axes, e.g. [\"x\"] (default x)")},
                 {"faces", prop("array", "Only these faces (either side; default all)")},
                 {"selection", prop("boolean", "Use the selected faces")},
                 {"reproject", prop("boolean", "Keep paint (default true; the negative half then shows the positive half's paint)")}}),
         [commitUV](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           int axes = 0;
           for (auto& v : a.value("axes", json::array({"x"})))
             if (v.is_string()) axes |= v == "x" ? 1 : v == "y" ? 2 : v == "z" ? 4 : 0;
           if (!axes) throw CommandError("give axes: x, y and / or z");
           auto faces = facesArg(e, a);
           Mesh work = e.doc.mesh;
           int n = uv::mirrorUVs(work, e.doc.bvh, axes, faces);
           if (n) {
             commitUV(e, work, a.value("reproject", true), "Mirror UVs");
             e.doc.mesh.faceFlags = work.faceFlags;
           }
           return json{{"mirrored_faces", n}, {"islands", e.doc.mesh.islandCount}};
         }});
  R.add({"uv_orient", "Orients UV islands: upright (object up = texture up) or along their longest side. (1.2.7) changed is false "
                       "when the islands already were (nothing moved by a quarter of a texel): no undo step then.",
         schema({{"mode", propEnum({"upright", "longest_axis"}, "Orientation")}, {"faces", prop("array", "Faces (default selection, or all)")},
                 {"selection", prop("boolean", "Use the selected faces")}, {"reproject", prop("boolean", "Keep paint (default true)")}},
                {"mode"}),
         [commitUV, needTargets](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           auto faces = facesArg(e, a);
           if (faces.empty()) faces = e.doc.selectedFaces();
           faces = needTargets(e, faces);
           Mesh work = e.doc.mesh;
           uv::forEachTile(work, faces, [&](const std::vector<uint32_t>& f) { uv::orientIslands(work, f, uv::orientFromName(a.at("mode"))); });
           bool changed = commitUV(e, work, a.value("reproject", true), "Orient islands");
           return json{{"islands", e.doc.mesh.islandCount}, {"overlaps", uv::overlapCount(e.doc.mesh)}, {"changed", changed}};
         }});
  R.add({"uv_fix_overlaps",
         "Finds islands that overlap (paint would appear twice) and moves them into free UV space; repacks "
         "everything if there is no room. With check_only, just reports them.",
         schema({{"check_only", prop("boolean", "Only report")}, {"margin", num("number", "Gap between islands in UV units: 0.005 = half a percent of the square. More avoids color bleeding between islands at small texture sizes; less uses the space better", 0, 0.1, 0.005)},
                 {"reproject", prop("boolean", "Keep paint (default true)")}}),
         [commitUV](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           auto ov = uv::overlappingIslands(e.doc.mesh);
           json faces = json::array();
           for (auto& isl : ov)
             for (uint32_t f : isl) faces.push_back(f);
           if (a.value("check_only", false) || ov.empty())
             return json{{"overlapping_islands", ov.size()}, {"faces", faces}};
           Mesh work = e.doc.mesh;
           // mirrored halves that drifted apart go back on their other half first
           int restacked = uv::restackMirrorCopies(work, e.doc.mirrorLinks());
           if (restacked) work.computeUVTopology();
           int moved = 0;
           uv::forEachTile(work, {}, [&](const std::vector<uint32_t>&) {
             int mv = uv::fixOverlaps(work, a.value("margin", 0.005f));
             moved = (mv < 0 || moved < 0) ? -1 : moved + mv;
           });
           commitUV(e, work, a.value("reproject", true), "Fix overlapping UVs");
           return json{{"overlapping_islands", ov.size()}, {"moved", moved}, {"repacked", moved < 0},
                       {"restacked_mirror_faces", restacked}, {"overlaps_left", uv::overlappingIslands(e.doc.mesh).size()}};
         }});
  R.add({"uv_islands",
         "Lists the UV islands: face count, UV bounds [u0, v0, u1, v1], pinned, and mirrored halves (an island that is the "
         "stacked copy of another, or that has one stacked on it). With faces, only the islands of those faces.",
         schema({{"faces", prop("array", "Only the islands of these faces")}, {"selection", prop("boolean", "Only the selected faces' islands")},
                 {"limit", num("integer", "At most this many islands in the result", 1, 100000, 1000)}}),
         [](CommandContext& c, const json& a) {
           Document& d = c.engine.doc;
           Mesh& m = d.mesh;
           if (m.loopIsland.size() != m.loopCount()) m.computeUVTopology();
           auto only = facesArg(c.engine, a);
           std::vector<uint8_t> want(m.islandCount + 1, only.empty() ? 1 : 0);
           for (uint32_t f : only) want[std::min(m.loopIsland[m.faceStart[f]], m.islandCount)] = 1;
           const uv::MirrorLinks& L = d.mirrorLinks();
           struct I {
             int faces = 0, copyFaces = 0, stacked = 0;
             bool pinned = false;
             Rect2 b;
             uint32_t first = 0;
           };
           std::vector<I> isl(m.islandCount);
           for (uint32_t f = 0; f < m.faceCount(); ++f) {
             uint32_t i = m.loopIsland[m.faceStart[f]];
             if (i >= m.islandCount) continue;
             if (!isl[i].faces) isl[i].first = f;
             ++isl[i].faces;
             for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) isl[i].b.add(m.loopUV[l]);
             isl[i].pinned |= uv::isPinned(m, f);
             if (f < L.primary.size() && L.primary[f] >= 0) {
               ++isl[i].copyFaces;
               uint32_t j = m.loopIsland[m.faceStart[L.primary[f]]];
               if (j < m.islandCount) ++isl[j].stacked;
             }
           }
           json out = json::array();
           int limit = a.value("limit", 1000);
           for (uint32_t i = 0; i < m.islandCount && (int)out.size() < limit; ++i) {
             if (!want[i] || !isl[i].faces) continue;
             out.push_back({{"island", i}, {"faces", isl[i].faces}, {"first_face", isl[i].first},
                            {"bounds", {isl[i].b.lo.x, isl[i].b.lo.y, isl[i].b.hi.x, isl[i].b.hi.y}},
                            {"pinned", isl[i].pinned}, {"mirror_copy", isl[i].copyFaces == isl[i].faces},
                            {"stacked_faces", isl[i].stacked}});
           }
           return json{{"islands", out}, {"count", m.islandCount}};
         }});
  R.add({"uv_pin",
         "Pins (or unpins) UV islands: pinned islands keep their UVs through every UV edit (moving, unwrapping, "
         "packing, fixing overlaps, the Island brush); packing and unwrapping place other islands around them.",
         schema({{"pin", prop("boolean", "true pins (default), false unpins")},
                 {"faces", prop("array", "Faces whose islands to pin (default: the selection)")},
                 {"selection", prop("boolean", "Use the selected faces")},
                 {"all", prop("boolean", "Every island")},
                 {"islands", prop("boolean", "Whole islands and their stacked mirror halves (default true)")}}),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           Document& d = e.doc;
           Mesh& m = d.mesh;
           bool pin = a.value("pin", true);
           std::vector<uint32_t> faces;
           if (a.value("all", false)) {
             for (uint32_t f = 0; f < m.faceCount(); ++f) faces.push_back(f);
           } else {
             faces = facesArg(e, a);
             if (faces.empty()) faces = d.selectedFaces();
           }
           if (faces.empty()) throw CommandError("no faces: select islands first (or all=true)");
           if (a.value("islands", true)) {
             std::vector<uint8_t> isl(m.islandCount + 1, 0);
             for (uint32_t f : faces) isl[std::min(m.loopIsland[m.faceStart[f]], m.islandCount)] = 1;
             faces.clear();
             for (uint32_t f = 0; f < m.faceCount(); ++f)
               if (isl[std::min(m.loopIsland[m.faceStart[f]], m.islandCount)]) faces.push_back(f);
             faces = uv::withLinkedFaces(m, d.mirrorLinks(), faces);
           }
           std::vector<uint8_t> flags = m.faceFlags;
           if (flags.size() != m.faceCount()) flags.assign(m.faceCount(), 0);
           for (uint32_t f : faces) flags[f] = pin ? (flags[f] | FacePinned) : (flags[f] & ~FacePinned);
           d.applyFaceFlags(flags, pin ? "Pin UV islands" : "Unpin UV islands");
           return json{{"faces", faces.size()}, {"pinned_faces", uv::pinnedFaceCount(m)}};
         }});
  R.add({"uv_restack",
         "Puts mirrored halves (from symmetric islands or Mirror UVs) back exactly on the half they copy, if they were "
         "moved apart.",
         schema({{"reproject", prop("boolean", "Keep paint (default true)")}}),
         [commitUV](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           Mesh work = e.doc.mesh;
           int n = uv::restackMirrorCopies(work, e.doc.mirrorLinks());
           if (n) commitUV(e, work, a.value("reproject", true), "Re-stack mirrored halves");
           return json{{"restacked_faces", n}, {"mirrored_faces", e.doc.mirrorLinks().copies}};
         }});
  R.add({"uv_unstack",
         "Makes mirrored halves independent: the copies among the faces (or the copies of the faces) stop following "
         "the other half and move into free UV space, so each side can be painted on its own.",
         schema({{"faces", prop("array", "Faces (default: the selection)")}, {"selection", prop("boolean", "Use the selected faces")},
                 {"reproject", prop("boolean", "Keep paint (default true)")}}),
         [commitUV](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           Document& d = e.doc;
           auto faces = facesArg(e, a);
           if (faces.empty()) faces = d.selectedFaces();
           if (faces.empty()) throw CommandError("no faces: select a mirrored island first");
           const uv::MirrorLinks& L = d.mirrorLinks();
           auto linked = uv::withLinkedFaces(d.mesh, L, faces);
           Mesh work = d.mesh;
           std::vector<uint32_t> copies;
           for (uint32_t f : linked)
             if (f < L.primary.size() && L.primary[f] >= 0 && !uv::isPinned(work, f)) copies.push_back(f);
           if (copies.empty()) throw CommandError("no mirrored halves among those faces (or they are pinned)");
           for (uint32_t f : copies) work.faceFlags[f] &= ~kFaceMirrorBits;
           work.computeUVTopology();
           bool placed = true;
           uv::forEachTile(work, copies, [&](const std::vector<uint32_t>& f) {
             auto charts = uv::facesByIsland(work, f);
             bool ok = uv::placeInFreeSpace(work, charts, 0.005f, true);
             if (!ok) {  // no room: repack everything that may move (in this tile)
               work.computeUVTopology();
               uv::repackMovable(work);
             }
             placed &= ok;
           });
           commitUV(e, work, a.value("reproject", true), "Unstack mirrored halves");
           return json{{"faces", copies.size()}, {"placed", placed}, {"repacked", !placed}};
         }});
  R.add({"select_faces",
         "Face selection for UV tools, the island brush and fills: set, add, remove, clear, all, invert, by island, "
         "by material slot, grow or shrink. (0.45) visible: of the faces this selects (or, with remove, takes off), "
         "only those the camera sees (set_camera; turned toward it, on screen, not behind other faces), like the "
         "Select tool's Visible only: an island that wraps inside the model, or a skin under clothes, gives just what "
         "is seen. (0.47) no_uv_area adds the faces whose UVs collapsed onto a point or a line (they get no paint; "
         "uv_info counts them): unwrap them with uv_unwrap selection. (1.2.7) mirror adds the mirror images of the "
         "faces (default: of the selection) across the model's symmetry (axes, default x), and the halves stacked on "
         "them: one half of a face's UVs selects the other half, side by side or stacked.",
         schema({{"mode", propEnum({"set", "add", "remove", "clear", "all", "invert", "island", "material", "grow", "shrink", "no_uv_area", "mirror"}, "Operation")},
                 {"faces", prop("array", "Face indices")},
                 {"axes", prop("array", "mirror: the symmetry axes, e.g. [\"x\"] (default x)")},
                 {"material", num("integer", "Material slot (mode material; adds to the selection)", 0, 255)},
                 {"visible", flag("Only the faces the camera sees (default false)", false)}},
                {"mode"}),
         [](CommandContext& c, const json& a) {
           Document& d = c.engine.doc;
           std::string m = a.at("mode");
           auto& s = d.faceSelection;
           s.resize(d.mesh.faceCount(), 0);
           bool visibleOnly = a.value("visible", false) && c.engine.features.on("select_visible");
           std::vector<uint8_t> before = visibleOnly ? s : std::vector<uint8_t>();
           std::vector<uint32_t> faces;
           for (auto& f : a.value("faces", json::array()))
             if (f.get<uint32_t>() < d.mesh.faceCount()) faces.push_back(f);
           if (m == "set" || m == "clear") std::fill(s.begin(), s.end(), 0);
           if (visibleOnly && m == "set") before.assign(s.size(), 0);  // (set: the faces seen of those given)
           if (m == "all") std::fill(s.begin(), s.end(), 1);
           if (m == "invert")
             for (auto& v : s) v = !v;
           if (m == "set" || m == "add")
             for (auto f : faces) s[f] = 1;
           if (m == "remove")
             for (auto f : faces) s[f] = 0;
           if (m == "island") {
             std::vector<uint8_t> isl(d.mesh.islandCount, 0);
             for (auto f : faces) isl[d.mesh.loopIsland[d.mesh.faceStart[f]]] = 1;
             for (uint32_t f = 0; f < d.mesh.faceCount(); ++f)
               if (isl[d.mesh.loopIsland[d.mesh.faceStart[f]]]) s[f] = 1;
           }
           if (m == "material") {
             uint32_t slot = a.value("material", 0u);
             for (uint32_t f = 0; f < d.mesh.faceCount(); ++f)
               if (f < d.mesh.faceMaterial.size() && d.mesh.faceMaterial[f] == slot) s[f] = 1;
           }
           if (m == "no_uv_area")
             for (uint32_t f : uv::collapsedFaces(d.mesh)) s[f] = 1;
           if (m == "mirror") {
             int axes = 0;
             for (auto& v : a.value("axes", json::array({"x"})))
               if (v.is_string()) axes |= v == "x" ? 1 : v == "y" ? 2 : v == "z" ? 4 : 0;
             if (!axes) throw CommandError("give axes: x, y and / or z");
             if (faces.empty()) faces = d.selectedFaces();
             for (uint32_t f : uv::withMirroredFaces(d.mesh, d.bvh, faces, axes)) s[f] = 1;
             for (uint32_t f : uv::withLinkedFaces(d.mesh, d.mirrorLinks(), faces)) s[f] = 1;
           }
           if (m == "grow") uv::growSelection(d.mesh, s);
           if (m == "shrink") uv::shrinkSelection(d.mesh, s);
           size_t unseen = 0;
           if (visibleOnly) {  // (0.45) the faces it would select (or take off) that the camera doesn't see: as before
             uint8_t want = m == "remove" ? 0 : 1;
             std::vector<uint32_t> changed;
             for (uint32_t f = 0; f < s.size(); ++f)
               if (s[f] != before[f] && s[f] == want) changed.push_back(f);
             std::vector<uint8_t> seen(s.size(), 0);
             for (uint32_t f : c.engine.visibleFaces(changed)) seen[f] = 1;
             for (uint32_t f : changed)
               if (!seen[f]) s[f] = before[f], ++unseen;
           }
           d.updateSelectionFlags();
           json r{{"selected", d.selectedFaces().size()}};
           if (visibleOnly) r["left_out_unseen"] = unseen;
           return r;
         }});
  R.add({"island_brush",
         "One dab of the Island brush at a normalized viewport point: the faces it takes join the selection (remove: "
         "leave it), with their mirror images under set_symmetry. reach surface (default): the faces within the "
         "brush's radius along the surface from the face under the point, so never a layer behind (a cloth with "
         "thickness) or a part only near in space, whatever the angle; screen: the faces seen inside the circle. "
         "Faces turned away from the camera are left out either way. One undo step. Then uv_make_island.",
         schema({{"point", prop("array", "[x,y] normalized viewport point (0..1, y down)")},
                 {"size", num("number", "Brush diameter in viewport pixels (default: the brush size)", 1, 2000)},
                 {"reach", propEnum({"surface", "screen"}, "What a dab takes (default surface)")},
                 {"remove", prop("boolean", "Take the faces out of the selection instead")},
                 {"list", prop("boolean", "Also return the indices of the faces the dab took")}},
                {"point"}),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           Document& d = e.doc;
           if (!d.hasMesh()) throw CommandError("no mesh");
           vec2 p(a["point"][0].get<float>(), a["point"][1].get<float>());
           float size = a.value("size", e.painter.brush.size);
           std::string reach = a.value("reach", std::string("surface"));
           if (!e.features.on("island_reach")) reach = "screen";
           bool remove = a.value("remove", false);
           auto& s = d.faceSelection;
           s.resize(d.mesh.faceCount(), 0);
           auto before = s;
           uint8_t want = remove ? 0 : 1;
           auto faces = e.facesUnderBrush(p * e.viewportSize, std::max(1.f, size * 0.5f),
                                          reach == "screen" ? Engine::BrushReach::Screen : Engine::BrushReach::Surface);
           size_t changed = 0;
           for (uint32_t f : faces)
             if (f < s.size() && s[f] != want) s[f] = want, ++changed;
           if (changed) {
             d.updateSelectionFlags();
             d.pushSelectionUndo(before, "Island brush");
           }
           std::sort(faces.begin(), faces.end());
           faces.erase(std::unique(faces.begin(), faces.end()), faces.end());
           json r{{"faces", faces.size()}, {"changed", changed}, {"selected", d.selectedFaces().size()}, {"reach", reach}};
           if (a.value("list", false)) r["face_list"] = faces;
           return r;
         }});
  R.add({"pick_face", "Returns the face/UV/position under a normalized viewport point.",
         schema({{"point", prop("array", "[x,y] normalized")}}, {"point"}),
         [](CommandContext& c, const json& a) {
           vec2 p(a["point"][0].get<float>(), a["point"][1].get<float>());
           RayHit h = c.engine.pick(p * c.engine.viewportSize);
           if (!h.hit) return json{{"hit", false}};
           const Mesh& m = c.engine.doc.mesh;
           return json{{"hit", true}, {"face", h.face}, {"island", m.loopIsland[m.faceStart[h.face]]},
                       {"uv", {h.meshUV.x, h.meshUV.y}}, {"tile", c.engine.doc.mesh.faceTile(h.face)},
                       {"position", {h.position.x, h.position.y, h.position.z}},
                       {"normal", {h.normal.x, h.normal.y, h.normal.z}}};
         }});

  // ---------------------------------------------------------------- Blender bridge
  auto bridgeOpen = [](CommandContext& c, const json& a, bool update) {
    Engine& e = c.engine;
    std::string path = a.at("path");
    std::string err;
    Mesh m;
    if (!loadMesh(path, m, &err)) throw CommandError(err);
    bool hadUV = hadUVs(path);
    std::string object = a.value("object", std::string());
    // (1.2) the scene: an object already in it is updated there (whichever is active stays so); "add" brings a new
    // one in beside the others; else (auto, replace) a new object starts a scene of its own
    const bool scenes = e.features.on("scene_objects");
    std::string sceneMode = a.value("scene", std::string("auto"));
    int inScene = -1;
    for (int i = 0; scenes && !object.empty() && i < e.objectCount(); ++i)
      if (e.object(i).bridge.active && e.object(i).bridge.object == object) inScene = i;
    // (Blender's automatic sync: only an object Patina has; one taken out of the scene stays out)
    if (sceneMode == "update" && inScene < 0 && e.doc.bridge.object != object)
      return json{{"skipped", "Patina's scene doesn't have " + object}};
    const int wasActive = e.activeObject();
    if (inScene >= 0 && inScene != wasActive && sceneMode != "replace") e.activateObject(inScene);
    const bool adding = scenes && inScene < 0 && sceneMode == "add" && e.doc.hasMesh() && !e.doc.untouchedStartup();
    // Blender's placement of the object (matrix_world, column by column, Z up): its place in the scene
    bool placed = a.contains("matrix") && a["matrix"].is_array() && a["matrix"].size() == 16;
    mat4 base(1);
    if (placed) {
      mat4 mb;
      for (int k = 0; k < 16; ++k) mb[k / 4][k % 4] = a["matrix"][k].get<float>();
      const mat4 C(vec4(1, 0, 0, 0), vec4(0, 0, -1, 0), vec4(0, 1, 0, 0), vec4(0, 0, 0, 1));  // Blender -> Patina axes
      base = C * mb * glm::inverse(C);
    }
    auto placeFromBlender = [&](Document& d) {
      if (!placed || !scenes) return;
      d.placement.base = base;
      vec3 c = d.mesh.bounds.valid() ? d.mesh.bounds.center() : vec3(0);
      d.placement.pivot = vec3(base * vec4(c, 1.f));
    };
    int resolution = a.value("resolution", e.defaultResolution);
    if (adding) {
      // a new object in the scene: its own texture set (as many texels per meter as the active one's) and layers
      Document::Placement pl;
      int res = resolution;
      double a0 = 0, a1 = 0;
      for (uint32_t f = 0; f < e.doc.mesh.faceCount(); ++f) a0 += e.doc.mesh.faceArea3D(f);
      for (uint32_t f = 0; f < m.faceCount(); ++f) a1 += m.faceArea3D(f);
      if (a0 > 0) {
        double want = e.doc.tileRes() * std::sqrt(std::max(a1, 1e-12) / a0);
        res = 512;
        while (res < e.doc.tileRes() && res < want * 0.9) res *= 2;
      }
      int idx = e.addObject(std::move(m), path, object, pl, res, false);
      {
        Document& d = e.object(idx);
        placeFromBlender(d);
        d.bridge.active = true;
        d.bridge.object = object.empty() ? d.mesh.name : object;
        d.bridge.blenderPort = a.value("blender_port", 7861);
        d.bridge.exchangeDir = a.value("exchange_dir", paths::bridgeDir().string());
        d.bridge.uvsEdited = !hadUV;
        d.blenderMaterials = e.features.on("bridge_materials");
      }
      json vc;
      if (e.features.on("model_textures")) {
        // (its materials are brought in as the active object; the one painted stays so after)
        std::string why;
        e.activateObject(idx);
        if (e.doc.importModelTextures(path, true, &vc, &why, false)) e.doc.clearHistory();
        e.activateObject(wasActive);
      }
      Document& d = e.object(idx);  // (moved while it was active: found again)
      d.dirty = false;
      e.sceneChanged = true;
      if (e.onExternalChange) e.onExternalChange("objects");
      json r{{"faces", d.mesh.faceCount()}, {"loops", d.mesh.loopCount()}, {"islands", d.mesh.islandCount},
             {"kept_layers", false}, {"generated_uvs", !hadUV}, {"object_index", idx}, {"objects", e.objectCount()}};
      if (!vc.is_null()) r["model_textures"] = vc;
      return r;
    }
    BridgeState prev = e.doc.bridge;
    bool sameObject = prev.object == object;
    bool wantLayers = update || a.value("keep_layers", true);
    bool keep = wantLayers && sameObject;
    // (0.44) an object Patina doesn't have open (after a restart), or has only just started (never saved, nothing
    // done since): the saved project it was painted in, with its layers (reprojected below if the mesh changed)
    std::string project;
    if (wantLayers && !object.empty() && e.features.on("bridge_projects") &&
        (!sameObject || (e.doc.projectPath.empty() && !e.doc.undo.canUndo()))) {
      bool replaced = false;
      for (auto& p : bridgeprojects::candidates(object, e.recentFiles ? e.recentFiles() : std::vector<std::string>())) {
        if (e.painter.active()) e.painter.endStroke();
        std::string perr;
        if (!e.openFile(p, &perr)) {  // (1.2) the whole scene it was in
          PLOG_W("Blender's %s: could not open %s: %s", object.c_str(), p.c_str(), perr.c_str());
          continue;
        }
        replaced = true;
        for (int i = 0; i < e.objectCount(); ++i)
          if (e.object(i).bridge.object == object) e.activateObject(i);
        if (!bridgeprojects::sameModel(e.doc.mesh, m)) {
          PLOG_I("%s is linked to a %s that is another model: not used", p.c_str(), object.c_str());
          continue;
        }
        project = p;
        break;
      }
      if (!project.empty()) {
        keep = true;
        resolution = 0;  // the project's
        e.assistantTurns.clear();
        PLOG_I("Blender's %s: opened %s, the project it was painted in (%zu layers)", object.c_str(), project.c_str(),
               e.doc.layers.size());
      } else if (replaced) {  // only projects of another model: a new project after all
        e.newScene();
        e.doc.projectPath.clear();
        e.doc.bridge = BridgeState();
        keep = false;
      }
    }
    const bool newModel = !keep && project.empty();
    e.doc.meshPath = path;
    if (!keep) e.doc.bridge.uvsEdited = false;  // fresh object: UVs are Blender's until edited here
    e.doc.bridge.slotsEdited = false;            // the mesh brings Blender's material slots
    e.doc.setMesh(std::move(m), resolution, keep);  // may generate UVs
    if (newModel && (inScene < 0 || sceneMode == "replace")) e.newScene();  // (1.2) another model: a scene of its own
    placeFromBlender(e.doc);
    // (0.32) a fresh object with vertex colors starts from them
    json vc;
    if (!keep && e.features.on("model_textures")) {
      std::string why;
      e.doc.blenderMaterials = e.features.on("bridge_materials");  // (0.44) what Blender's materials show
      if (e.doc.importModelTextures(path, true, &vc, &why, false)) e.modelTextures = vc, e.modelTextures["path"] = path;
    }
    e.doc.bridge.active = true;
    e.doc.bridge.object = a.value("object", e.doc.mesh.name);
    e.doc.bridge.blenderPort = a.value("blender_port", 7861);
    e.doc.bridge.exchangeDir = a.value("exchange_dir", paths::bridgeDir().string());
    if (!hadUV) e.doc.bridge.uvsEdited = true;
    json r{{"faces", e.doc.mesh.faceCount()}, {"loops", e.doc.mesh.loopCount()}, {"islands", e.doc.mesh.islandCount},
           {"kept_layers", keep}, {"generated_uvs", !hadUV}};
    if (!project.empty()) r["project"] = project, r["layers"] = e.doc.layers.size();
    if (e.doc.uvsCarried >= 0.f) r["kept_uvs"] = e.doc.uvsCarried, r["generated_uvs"] = false;
    if (!vc.is_null()) r["model_textures"] = vc;
    if (e.objectCount() > 1) r["objects"] = e.objectCount(), r["object_index"] = e.activeObject();
    // (an update of another object than the one painted: that one stays the one painted)
    if (inScene >= 0 && wasActive != inScene && wasActive < e.objectCount() && project.empty()) e.activateObject(wasActive);
    if (!update && (newModel || !project.empty())) e.frameMesh();
    if (e.onExternalChange) e.onExternalChange(project.empty() ? "bridge" : "bridge_project");
    return r;
  };
  R.add({"bridge_open_mesh", "Blender bridge: open a .ptmesh sent by the add-on.", schema(json::object(), {"path"}),
         [bridgeOpen](CommandContext& c, const json& a) { return bridgeOpen(c, a, false); }, false});
  R.add({"bridge_update_mesh", "Blender bridge: update the mesh, keeping layers.", schema(json::object(), {"path"}),
         [bridgeOpen](CommandContext& c, const json& a) { return bridgeOpen(c, a, true); }, false});
  R.add({"bridge_export", "Blender bridge: write textures (+UVs) to the exchange folder and return paths.", schema(),
         [](CommandContext& c, const json& a) {
           std::string err;
           // (1.2) the scene's object linked to the Blender object asked for (else the active one)
           Document* which = nullptr;
           std::string object = a.value("object", std::string());
           for (int i = 0; !object.empty() && i < c.engine.objectCount(); ++i)
             if (c.engine.object(i).bridge.active && c.engine.object(i).bridge.object == object) which = &c.engine.object(i);
           // (1.2.5) never another object's textures and UVs: a Patina without it (another one running, the
           // startup sphere) answered with its own, and the add-on got UVs that didn't fit
           if (!which && !object.empty() && !c.engine.doc.bridge.active)
             throw CommandError("This Patina has no object linked to '" + object + "'. Is another Patina running? "
                                "Send it to Patina again from Blender, or close the other Patina.");
           json r = c.engine.exportForBlender(&err, which);
           if (r.is_null()) throw CommandError(err);
           return r;
         }, false});
  R.add({"send_to_blender",
         "Exports textures (and edited UVs) and tells the Blender add-on to apply them to the linked object.",
         schema({{"auto_push", prop("boolean", "Also push automatically after each change")},
                 {"objects", propEnum({"all", "active"}, "(1.2) Every object of the scene linked to Blender (default), or the active one")}}),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           if (a.contains("auto_push")) e.autoPushToBlender = a["auto_push"];
           // (1.2) every object of the scene linked to Blender (objects: "active" for the one painted)
           bool all = a.value("objects", std::string("all")) == "all";
           json pushed = json::array();
           for (int i = 0; i < e.objectCount(); ++i) {
             Document& d = e.object(i);
             if (!d.bridge.active || (!all && i != e.activeObject())) continue;
             std::string err;
             json r;
             if (!e.pushToBlender(&err, &r, &d)) throw CommandError(d.displayName() + ": " + err);
             pushed.push_back(r);
           }
           if (pushed.empty()) throw CommandError("no Blender object is linked (use 'Send to Patina' in Blender first)");
           if (pushed.size() == 1) return pushed[0];
           return json{{"objects", pushed}};
         }});
  R.add({"send_model_to_blender",
         "Sends the model as it is in Patina (mended, with its UVs, material slots, seams and vertex groups) to "
         "Blender as an object with its textures: a new object (name, default <object>_patina) next to the linked one, "
         "in its place and collections, or with replace the linked object's own mesh. Patina links to that object "
         "from then on. Needs Blender's live link and the Patina Bridge add-on 0.9 or newer.",
         schema({{"name", prop("string", "The new object's name (default: the linked object's name + _patina)")},
                 {"replace", prop("boolean", "Replace the linked object's mesh instead (its modifiers stay)")},
                 {"port", num("integer", "Blender's live link port; 0 = the linked object's, else 7861", 0, 65535, 0)}}),
         [](CommandContext& c, const json& a) {
           if (!c.engine.features.on("model_export")) throw CommandError("model export is switched off (features)");
           std::string err;
           json r = c.engine.sendModelToBlender(a.value("name", std::string()), a.value("replace", false), a.value("port", 0), &err);
           if (r.is_null()) throw CommandError(err);
           return r;
         }});
  R.add({"export_model",
         "Exports the model with its textures: .glb (one file, textures inside), .gltf (with a .bin and PNG files) or "
         ".obj (with a .mtl and PNG files). A material per texture set / UDIM tile; glTF gets base color (with "
         "opacity as alpha: MASK or BLEND), metallic-roughness, normal, emissive, occlusion when baked, and the "
         "KHR_materials_sheen / KHR_materials_clearcoat extensions when used. lods adds levels of detail that share the "
         "textures: glTF nodes name_LOD0, name_LOD1... (with MSFT_lod on LOD0), or name_LOD1.obj... beside the .obj. "
         ".stl: 3D printing (mm, Z up, no textures); .ply: the base color as sRGB vertex colors, with UVs and normals "
         "(Z up, meters, faces as they are).",
         schema({{"path", prop("string", "Output file; its extension picks the format: .glb (one file, textures inside: games, web, Blender), .gltf, .obj, .stl (3D printing), .ply (vertex colors)")},
                 {"texture_size", num("integer", "Texture size in pixels (512, 1024, 2048, 4096). 0 = the texture set's. Smaller files load faster; web and mobile: 1024", 0, 8192, 0)},
                 {"textures", flag("Include the textures", true)},
                 {"lods", {{"type", "array"}, {"items", {{"type", "number"}, {"minimum", 0.001}, {"maximum", 1}}},
                           {"description", "Levels of detail besides the model: the share of its triangles each keeps, e.g. [0.5, 0.25, 0.125]; "
                                           "they share the textures (glTF nodes name_LOD0, name_LOD1... with MSFT_lod; OBJ files name_LOD1.obj...)"}}},
                 {"collision", propEnum({"none", "unreal", "godot", "plain"},
                                        "Collision shapes after the model (glTF, OBJ): the ones made with mesh_collision, "
                                        "else made now; named UCX_ / UBX_ (Unreal), -convcolonly (Godot) or _collision_")},
                 {"objects", propEnum({"all", "active"}, "(1.2) A scene of several objects: every object shown in one file (glTF: a node each, where it is; OBJ, STL, PLY: their vertices moved into the scene), the default; or the active one alone")}},
                {"path"}),
         [](CommandContext& c, const json& a) {
           if (!c.engine.features.on("model_export")) throw CommandError("model export is switched off (features)");
           ModelExportOptions o;
           o.path = a.at("path");
           o.textureSize = std::clamp(a.value("texture_size", 0), 0, 8192);
           o.textures = a.value("textures", true);
           if (a.contains("lods"))
             for (auto& v : a["lods"]) o.lods.push_back(std::clamp(v.get<float>(), 0.001f, 1.f));
           o.collision = a.value("collision", std::string("none"));
           if (o.collision == "none") o.collision.clear();
           if (!o.collision.empty() && !c.engine.features.on("collision"))
             throw CommandError("collision shapes are switched off (features)");
           json out;
           std::string err;
           if (!c.engine.exportSceneModel(o, a.value("objects", std::string("all")) == "all", &out, &err)) throw CommandError(err);
           return out;
         }});
  R.add({"mesh_collision",
         "Makes collision shapes for game engines and shows them over the model (green): box (one oriented box), "
         "boxes (a box per separate part), hull (one convex hull), parts (a hull per separate part) or pieces (the "
         "model split into convex pieces where one hull would hold too much empty space). export_model with "
         "collision writes them after the model. clear removes them; show switches the view.",
         schema({{"mode", propEnum({"box", "boxes", "hull", "parts", "pieces"},
                                   "box: one oriented box (crates, simple props; cheapest). boxes: a box per separate part. "
                                   "hull: one convex hull (rocks, bottles, round props). parts: a hull per separate part (kits, "
                                   "furniture made of pieces). pieces: the model split into convex pieces where it bends in "
                                   "(chairs, arches, rings, characters; the closest fit). Default pieces")},
                 {"max_shapes", num("integer", "Most shapes (boxes, parts, pieces). More fits concave models closer but costs the physics engine more; 4-16 is usual for game props", 1, 256, 16)},
                 {"max_vertices", num("integer", "Most corners per hull. Fewer are cheaper and a little smaller than the model; engines like 32 or fewer (PhysX allows 255)", 8, 255, 32)},
                 {"concavity", num("number", "Pieces: a piece is split again while its hull holds more than this share of empty space. Lower: more, closer pieces (0.02); higher: fewer, rougher ones (0.15)", 0, 1, 0.05)},
                 {"resolution", num("integer", "Pieces: voxels along the longest side. Higher follows thin parts better and is slower; the hulls stand off the surface by about one voxel", 8, 96, 40)},
                 {"show", flag("Show them in green over the model", true)},
                 {"clear", flag("Remove them", false)}}),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           if (!e.features.on("collision")) throw CommandError("collision shapes are switched off (features)");
           if (a.value("clear", false)) {
             e.doc.clearCollision();
             e.showCollision = false;
             return json{{"cleared", true}};
           }
           if (a.size() == 1 && a.contains("show")) {
             e.showCollision = a["show"];
             return json{{"show", e.showCollision}, {"shapes", e.doc.collisionShapes() ? e.doc.collisionShapes()->shapes.size() : 0}};
           }
           CollisionOptions o = e.doc.collisionOptions;
           o.mode = a.value("mode", o.mode);
           if (o.mode != "box" && o.mode != "boxes" && o.mode != "hull" && o.mode != "parts" && o.mode != "pieces")
             throw CommandError("mode must be box, boxes, hull, parts or pieces");
           o.maxShapes = std::clamp(a.value("max_shapes", o.maxShapes), 1, 256);
           o.maxVerts = std::clamp(a.value("max_vertices", o.maxVerts), 8, 255);
           o.concavity = std::clamp(a.value("concavity", o.concavity), 0.f, 1.f);
           o.resolution = std::clamp(a.value("resolution", o.resolution), 8, 96);
           json r;
           std::string err;
           if (!e.doc.makeCollision(o, &r, &err)) throw CommandError(err);
           e.showCollision = a.value("show", true);
           return r;
         }});
  R.add({"mesh_lightmap_uvs",
         "Makes a second UV map for baked lighting (Unity, Unreal and Godot lightmaps): every face gets space of its "
         "own (mirrored and stacked islands too), the same texel density everywhere, and a gap of padding texels at "
         "the lightmap's size. It is saved with the project and exported as the glTF's second UV map (TEXCOORD_1); "
         "Blender gets it as the UV map Lightmap. The paint's own UVs do not change. A fix or a reduction drops it "
         "(make it again); a transform keeps it.",
         schema({{"lightmap_size", num("integer", "The lightmap resolution the engine will bake at (128, 256, 512, 1024, 2048). The padding is measured at this size: a smaller lightmap needs more room between islands", 32, 8192, 512)},
                 {"padding", num("number", "Texels between islands (and from the border) at lightmap_size. Too little lets light bleed between islands; 2 to 4 is usual", 0, 64, 4)},
                 {"source", propEnum({"uv", "auto"}, "uv: the model's own UV islands, repacked (keeps their shapes; best for models with clean UVs; islands that fold are unwrapped again). auto: new charts made for lightmaps by xatlas (for models whose UVs are missing, stretched or a mess, such as scans and generated models; slower on dense models). Default uv")},
                 {"clear", flag("Remove the lightmap UVs", false)}}),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           if (!e.features.on("lightmap_uvs")) throw CommandError("lightmap UVs are switched off (features)");
           Document& d = e.doc;
           if (!d.hasMesh()) throw CommandError("no mesh loaded");
           if (a.value("clear", false)) {
             d.mesh.loopUV2.clear();
             d.dirty = true;
             return json{{"cleared", true}};
           }
           LightmapOptions o;
           o.size = std::clamp(a.value("lightmap_size", 512), 32, 8192);
           o.padding = std::clamp(a.value("padding", 4.f), 0.f, 64.f);
           o.source = a.value("source", std::string("uv"));
           if (o.source == "smart") o.source = "auto";
           if (o.source != "uv" && o.source != "auto") throw CommandError("source must be uv or auto");
           std::vector<vec2> uv2;
           LightmapStats st;
           if (!makeLightmapUVs(d.mesh, o, uv2, &st)) throw CommandError("the lightmap UVs could not be made");
           d.mesh.loopUV2 = std::move(uv2);
           d.dirty = true;
           json r = {{"islands", st.islands}, {"fill", st.fill}, {"overlaps", st.overlaps}, {"lightmap_size", o.size},
                     {"padding", o.padding}, {"source", o.source}};
           if (st.flatIslands) r["flat_islands_projected"] = st.flatIslands;
           if (st.refolded) r["faces_reprojected"] = st.refolded;  // islands that overlapped themselves
           if (st.neededSize) {
             r["needed_size"] = st.neededSize;
             r["note"] = "the charts need a lightmap of about " + std::to_string(st.neededSize) +
                         " pixels for this padding; at the size asked the gaps are smaller: raise lightmap_size or "
                         "lower padding, or reduce the model first";
           }
           if (st.overlaps) r["note"] = o.source == "uv" ? "some faces still overlap (folds in the model's UVs): try source auto"
                                                         : "some faces overlap: mend the mesh (mesh_fix) and try again";
           return r;
         }});
  R.add({"blender_material_library",
         "Puts Patina's material library in Blender's Asset Browser. Blender (Patina Bridge add-on 0.8 or newer, live "
         "link on) has Patina bake every material and builds a library of ready materials in Documents/Patina "
         "Materials (or the add-on's folder setting), added to its asset libraries. Run it again to update it.",
         schema({{"size", num("integer", "Map size: 512, 1024 or 2048. Bigger is sharper, slower to build and larger on disk", 512, 2048, 1024)},
                 {"mapping", propEnum({"box", "uv"}, "box (default): projected like Patina, no UVs needed; uv: the object's UV map")},
                 {"which", propEnum({"all", "builtin"}, "Patina's materials and your own (default), or Patina's only")},
                 {"port", num("integer", "Blender's live link port (default: the linked object's, else 7861)", 1, 65535)}}),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           if (!e.features.on("blender_library")) throw CommandError("materials in Blender are switched off (features)");
           int size = a.value("size", 1024);
           if (size != 512 && size != 1024 && size != 2048) throw CommandError("size: 512, 1024 or 2048");
           std::string mapping = a.value("mapping", std::string("box")), which = a.value("which", std::string("all"));
           if (mapping != "box" && mapping != "uv") throw CommandError("mapping: box or uv");
           if (which != "all" && which != "builtin") throw CommandError("which: all or builtin");
           int port = a.value("port", e.doc.bridge.active ? e.doc.bridge.blenderPort : 7861);
           net::JsonLineClient cl;
           std::string err;
           if (!cl.connect("127.0.0.1", port, 1500, &err))
             throw CommandError("Blender is not listening on port " + std::to_string(port) +
                                ": start Blender's live link (N panel > Patina), with the Patina Bridge add-on 0.8 or newer");
           json resp;
           if (!cl.call("patina_material_library", {{"size", size}, {"mapping", mapping}, {"which", which}}, resp, 8000, &err))
             throw CommandError("Blender did not answer: " + err);
           if (!resp.value("ok", false)) {
             std::string why = resp.value("error", std::string("?"));
             if (why.find("Unknown command") != std::string::npos)
               why = "the Patina Bridge add-on in Blender is older than 0.8: install the one that came with this Patina";
             throw CommandError("Blender: " + why);
           }
           json r = resp.value("result", json::object());
           r["port"] = port;
           return r;
         }});
}

}  // namespace patina
