// Commands the Patina Assistant (0.11) aims at: one request maps to one or two of these. They
// work from the console, MCP and macros too. Each is one undo step.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <numeric>
#include <set>
#include <thread>

#include "core/UVArap.h"
#include "core/UVIsland.h"
#include "core/UVOps.h"
#include "core/Image.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Progress.h"
#include "core/Sha256.h"
#include "engine/Assistant.h"
#include "engine/AssistantContext.h"
#include "engine/Engine.h"
#include "engine/Regions.h"

namespace patina {

using regions::Region;
using regions::regionArg;
using regions::applyRegionAndMasks;
using regions::regionSchema;
using regions::maskSchema;

namespace {

json prop(const char* type, const char* desc) { return {{"type", type}, {"description", desc}}; }
json num(const char* type, const char* desc, double lo, double hi, json def = json()) {
  json j = {{"type", type}, {"description", desc}};
  if (std::string(type) == "integer") j["minimum"] = (int64_t)lo, j["maximum"] = (int64_t)hi;
  else j["minimum"] = lo, j["maximum"] = hi;
  if (!def.is_null()) j["default"] = def;
  return j;
}
json propEnum(std::vector<std::string> values, const char* desc) {
  return {{"type", "string"}, {"enum", values}, {"description", desc}};
}
json propNum(const char* desc, double lo, double hi) {
  return {{"type", "number"}, {"minimum", lo}, {"maximum", hi}, {"description", desc}};
}
json propColor(const char* desc) {
  return {{"type", "array"}, {"items", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}}, {"minItems", 3},
          {"maxItems", 3}, {"description", desc}};
}
json schema(json props = json::object(), std::vector<std::string> required = {}) {
  json s = {{"type", "object"}, {"properties", props}, {"additionalProperties", false}};
  if (!required.empty()) s["required"] = required;
  return s;
}

const std::vector<std::string> kBlendNames = {"normal",   "multiply", "screen",  "overlay",    "add",
                                              "subtract", "darken",   "lighten", "add_signed", "soft_light"};

// An undo group that rolls back if the command throws.
struct GroupGuard {
  Document& d;
  bool done = false;
  GroupGuard(Document& doc, const std::string& label) : d(doc) { d.beginUndoGroup(label); }
  void commit() {
    d.endUndoGroup();
    done = true;
  }
  ~GroupGuard() {
    if (!done) d.cancelUndoGroup();
  }
};

std::string shortName(const std::string& s, size_t n = 40) { return s.size() <= n ? s : s.substr(0, n - 1) + "…"; }

float percentile(std::vector<float> v, float q) {
  if (v.empty()) return 0.f;
  size_t k = std::min(v.size() - 1, size_t(q * (v.size() - 1)));
  std::nth_element(v.begin(), v.begin() + k, v.end());
  return v[k];
}

}  // namespace

const parts::PartsReport& Engine::meshParts() {
  const Mesh& m = doc.mesh;
  uint64_t k = 1469598103934665603ull ^ (uint64_t(m.faceCount()) << 32) ^ m.positions.size();
  auto mix = [&](uint64_t v) { k = (k ^ v) * 1099511628211ull; };
  size_t step = std::max<size_t>(1, m.positions.size() / 211);
  for (size_t i = 0; i < m.positions.size(); i += step) {
    uint32_t b[3];
    std::memcpy(b, &m.positions[i], sizeof b);
    mix(b[0] ^ (uint64_t(b[1]) << 21) ^ (uint64_t(b[2]) << 42));
  }
  for (size_t i = 0; i < m.faceMaterial.size(); i += std::max<size_t>(1, m.faceMaterial.size() / 97)) mix(m.faceMaterial[i]);
  for (auto& n : m.materialNames) mix(std::hash<std::string>()(n));
  for (auto& g : m.vertexGroups) mix(std::hash<std::string>()(g.name) ^ g.verts.size());
  if (k != partsKey_) {
    partsKey_ = k;
    parts_ = parts::findParts(m);
  }
  return parts_;
}

std::vector<uint32_t> Engine::partFaces(const std::string& id) {
  if (id == "selection") {
    auto f = doc.selectedFaces();
    if (f.empty()) throw CommandError("no faces are selected");
    return f;
  }
  if (id == "all") {
    std::vector<uint32_t> all(doc.mesh.faceCount());
    for (uint32_t f = 0; f < all.size(); ++f) all[f] = f;
    return all;
  }
  const auto& P = meshParts();
  for (auto& p : P.parts)
    if (p.id == id) return p.faces;
  std::string list;
  for (auto& p : P.parts) list += (list.empty() ? "" : ", ") + p.id;
  if (!doc.selectedFaces().empty()) list += (list.empty() ? "" : ", ") + std::string("selection");
  std::string why = id.rfind("region:head", 0) == 0 && !P.neck.found ? " (no head found: " + P.neck.why + ")" : "";
  throw CommandError("no part '" + id + "'" + why + ". Parts: " + (list.empty() ? "all (the model is one part)" : list));
}

AssistantModel& Engine::assistantModel() {
  if (!assistantModel_) assistantModel_ = std::make_unique<AssistantModel>();
  return *assistantModel_;
}

std::string Engine::assistantModelFile() const {
  if (!assistantModelPath.empty()) return assistantModelPath;
  if (const char* env = std::getenv("PATINA_ASSISTANT_MODEL"); env && *env) return env;
  return (paths::userDataDir() / "models" / "patina-assistant.gguf").string();
}

void Engine::registerAssistantCommands() {
  CommandRegistry& R = commands;

  R.add({"mesh_parts",
         "The named parts of the model, for the commands that take a part or region: material slots (slot:<name>), "
         "connected pieces by size (piece:<n>), Blender vertex groups (group:<name>), the head and body found from "
         "the shape (region:head, region:body, only when a neck is found) and the selected faces (selection).",
         schema({{"faces", prop("boolean", "Include each part's face indices (default false)")}}),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           if (!e.doc.hasMesh()) throw CommandError("no mesh open");
           const auto& P = e.meshParts();
           uint32_t F = e.doc.mesh.faceCount();
           bool withFaces = a.value("faces", false);
           json list = json::array();
           auto add = [&](const std::string& id, const std::string& kind, const std::string& label,
                          const std::vector<uint32_t>& faces, json extra) {
             json p = {{"id", id}, {"kind", kind}, {"label", label}, {"faces", faces.size()},
                       {"share", F ? double(faces.size()) / F : 0.0}};
             p.update(extra);
             if (withFaces) p["face_indices"] = faces;
             list.push_back(p);
           };
           for (auto& p : P.parts) {
             json extra = json::object();
             if (p.kind == "region") extra = {{"confidence", p.confidence}, {"fallback", p.fallback}};
             add(p.id, p.kind, p.label, p.faces, extra);
           }
           auto sel = e.doc.selectedFaces();
           if (!sel.empty()) add("selection", "selection", "the selected faces", sel, json::object());
           json out = {{"parts", list}, {"pieces", P.pieceCount}, {"head_found", P.neck.found}};
           if (P.neck.found) out["neck_height"] = P.neck.y;
           else out["why_no_head"] = P.neck.why;
           return out;
         }});

  R.add({"select_part",
         "Sets, adds to or removes from the face selection by part id (see mesh_parts). One undo step.",
         schema({{"part", prop("string", "Part id")},
                 {"mode", propEnum({"set", "add", "remove"}, "What to do with the selection (default set)")}},
                {"part"}),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           Document& d = e.doc;
           std::string id = a.at("part");
           auto faces = e.partFaces(id);
           std::string mode = a.value("mode", std::string("set"));
           d.faceSelection.resize(d.mesh.faceCount(), 0);
           auto before = d.faceSelection;
           if (mode == "set") std::fill(d.faceSelection.begin(), d.faceSelection.end(), 0);
           for (uint32_t f : faces) d.faceSelection[f] = mode == "remove" ? 0 : 1;
           d.updateSelectionFlags();
           d.pushSelectionUndo(before, "Select " + id);
           return json{{"selected", d.selectedFaces().size()}, {"part_faces", faces.size()}};
         }});

  R.add({"add_tint_layer",
         "Adds a fill layer that changes only the base color, limited to a region of the model and to a smart mask "
         "stack: for example a soft red in the skin's creases. strength is the layer's opacity. glow: the color "
         "lights up instead (emissive), for glowing eyes or lights. One undo step.",
         schema({{"name", prop("string", "Layer name")},
                 {"color", propColor("sRGB color [r, g, b], 0..1")},
                 {"glow", prop("boolean", "The color glows (emissive channel) instead of tinting the base color")},
                 {"strength", propNum("How strong the tint is (layer opacity, default 0.5)", 0, 1)},
                 {"blend", {{"type", "string"}, {"enum", kBlendNames}, {"description", "Blend mode (default normal)"}}},
                 {"region", regionSchema()},
                 {"mask", maskSchema()}},
                {"color"}),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           Document& d = e.doc;
           if (!d.hasMesh()) throw CommandError("no mesh open");
           Region r = regionArg(e, a);
           std::string name = a.value("name", std::string("Tint"));
           GroupGuard g(d, "Tint layer: " + shortName(name));
           MaterialRef ref;
           ref.id.clear();
           const json& col = a.at("color");
           if (!col.is_array() || col.size() != 3) throw CommandError("color must be [r, g, b]");
           vec3 rgb(col[0].get<float>(), col[1].get<float>(), col[2].get<float>());
           bool glow = a.value("glow", false);
           if (glow) ref.flat.emissive = rgb;
           else ref.flat.color = rgb;
           Layer& l = d.addFillLayer(ref, name);
           uint32_t uid = l.uid;
           l.channelMask = 1 << (int)(glow ? Channel::Emissive : Channel::Color);
           l.opacity = std::clamp(a.value("strength", 0.5f), 0.f, 1.f);
           if (a.contains("blend")) l.blend = blendModeFromName(a["blend"]);
           d.checkpoint();
           applyRegionAndMasks(e, d.indexOf(uid), r, a.value("mask", json::array()));
           g.commit();
           return json{{"index", d.indexOf(uid)}, {"uid", uid}, {"name", name}, {"region_faces", r.given ? r.faces.size() : d.mesh.faceCount()}};
         }});

  R.add({"ui_rect",
         "For tests: where a widget was last drawn in the window (x, y, width, height in window pixels), by its name "
         "(for example retopo.zone_resolution).",
         schema({{"name", prop("string", "The widget")},
                 {"prefix", prop("string", "Instead: every widget whose name starts so (for example materials.tile:)")}}),
         [](CommandContext& c, const json& a) {
           if (a.contains("prefix")) {
             std::string pre = a["prefix"];
             json out = json::object();
             for (auto& [k, r] : c.engine.uiRects)
               if (k.compare(0, pre.size(), pre) == 0) out[k] = {{"x", r[0]}, {"y", r[1]}, {"w", r[2]}, {"h", r[3]}};
             return json{{"rects", out}};
           }
           if (!a.contains("name")) throw CommandError("name or prefix needed");
           auto it = c.engine.uiRects.find(a.at("name").get<std::string>());
           if (it == c.engine.uiRects.end()) return json{{"found", false}};
           auto& r = it->second;
           return json{{"found", true}, {"x", r[0]}, {"y", r[1]}, {"w", r[2]}, {"h", r[3]}};
         }});

  R.add({"ui_id_conflicts",
         "For tests: widgets drawn with the same ID as another in their window (they fight over clicks), each "
         "once since Patina started. Only checked when Patina was started with PATINA_UI_ID_CHECK set.",
         schema(),
         [](CommandContext& c, const json&) {
           return json{{"checking", std::getenv("PATINA_UI_ID_CHECK") != nullptr}, {"conflicts", c.engine.uiIdConflicts}};
         }});

  R.add({"open_window",
         "Opens one of Patina's windows, set up for what was asked: export (textures, with a preset), render, "
         "texel_density, symmetry (the mesh symmetry check), high_poly (bake from a high-poly mesh), bake_down (1.1: the layers onto a low-poly model), "
         "new_texture_set (a texture set from the selection), texture_set (the Texture Set panel), blender (the "
         "Blender link), preferences, shortcuts, macros, material_editor (a library material to edit or copy, or a "
         "new one with a generator), mesh_check (what is wrong with the model, and fixes), export_model (the model with its textures), reduce (fewer polygons), batch (recipes on many files), transform (units, size, turn, origin); and (0.36) a panel brought to the front: layers, properties, materials, brushes, console, history, notes, colors; about, help (shortcuts), welcome, pen_test; and (1.2.12) tutorial: Patina in 5 Minutes, the guided tutorial, in the browser (the result says which page). Only with the window open (not headless).",
         schema({{"window", propEnum({"export", "render", "texel_density", "symmetry", "high_poly", "bake_down", "new_texture_set",
                                      "texture_set", "blender", "preferences", "shortcuts", "macros", "retopology", "mesh_check", "export_model", "reduce", "batch", "transform", "collision", "lightmap",
                                      "material_editor", "layers", "properties", "materials", "brushes", "console",
                                      "history", "notes", "colors", "about", "help", "welcome", "pen_test", "face_sets", "tutorial"},
                                     "The window")},
                 {"preset", propEnum({"blender", "gltf", "unreal", "unity"}, "export: the map layout to start with")},
                 {"what", propEnum({"all", "layer", "mask"}, "export: all layers, only the selected layer, or its mask")},
                 {"path", prop("string", "bake_down: the low-poly model's file to start with")},
                 {"keymap", propEnum({"patina", "blender"}, "preferences: switch to this keymap (and its mouse navigation)")},
                 {"section", propEnum({"language", "http", "features", "pies", "undo"}, "preferences: scrolled to this section")},
                 {"material", prop("string", "material_editor: the library material to edit (a built-in one is copied)")},
                 {"generator", prop("string", "material_editor: a new procedural material with this generator")}},
                {"window"}),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           std::string w = a.at("window");
           e.lastOpenedWindow = w;
           if (!e.openWindow) return json{{"opened", false}, {"window", w}, {"why", "Patina runs without its window"}};
           e.lastOpenedPage.clear();
           e.openWindow(w, a);
           json r{{"opened", true}, {"window", w}};
           if (w == "tutorial") {  // (1.2.12) the page, or why it isn't there
             r["opened"] = !e.lastOpenedPage.empty();
             r["page"] = e.lastOpenedPage;
             if (e.lastOpenedPage.empty()) r["why"] = "docs/tutorial/index.html was not found next to Patina's data";
           }
           return r;
         }});

  R.add({"layout",
         "(0.46) The window's layout: guided (the work in five steps along the top, each a screen with what it needs: "
         "model, uvs, bake, paint, export), full (every panel docked) or (1.2.7) blender (arranged as Blender is: "
         "the steps as workspace tabs, the Outliner and a Properties editor with vertical tabs, an asset shelf), and "
         "(1.2.7) its theme: patina (gray and copper) or "
         "vivid_dark (KDE Plasma's Vivid Dark: navy and cyan, as the Vivid Dark Blender theme). With no arguments it "
         "only reports. Returns the layout, the theme, the Guided step shown, and each step's state (done, look: needs "
         "a look, todo) with why, in a few words: the same marks the step bar shows, and (1.2.10) the Blender "
         "layout's Properties tab and its pin. Only with the window open (not "
         "headless).",
         schema({{"mode", propEnum({"guided", "full", "blender"}, "Switch to this layout")},
                 {"step", propEnum({"model", "uvs", "bake", "paint", "export"},
                                   "Go to this step (switches to the Guided layout; in the Blender layout, its workspace tab)")},
                 {"theme", propEnum({"patina", "vivid_dark"}, "Switch to this theme (Edit > Preferences > Display)")},
                 {"properties_tab", propEnum({"step", "object", "layer", "texture_set", "face_sets", "retopology", "history", "notes", "console"},
                                             "(1.2.10) The Blender layout's Properties tab to show")},
                 {"pin_properties", prop("boolean", "(1.2.10) Pin the Properties tab (the pin above its tabs): true keeps the "
                                                    "tab when a tool or a workspace is chosen; false (default) shows the "
                                                    "tool's tab (Select: Step, Paint: Layer, a retopology tool: Retopology)")}}),
         [](CommandContext& c, const json& a) {
           if (!c.engine.layoutHook) return json{{"available", false}, {"why", "Patina runs without its window"}};
           return c.engine.layoutHook(a);
         }});

  R.add({"add_material_layer",
         "Adds a fill layer with a library material on every channel (see list_materials), limited to a region and "
         "a smart mask stack: for example rust where the armor's edges are. One undo step.",
         schema({{"material", prop("string", "Library material id")},
                 {"name", prop("string", "Layer name (default: the material's name)")},
                 {"strength", propNum("Layer opacity (default 1)", 0, 1)},
                 {"mapping", propEnum({"triplanar", "uv"}, "Projection (default triplanar)")},
                 {"scale", propNum("Material scale multiplier (default 1)", 0.05, 20)},
                 {"tint", propColor("Color multiplier [r, g, b] (default white)")},
                 {"region", regionSchema()},
                 {"mask", maskSchema()}},
                {"material"}),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           Document& d = e.doc;
           if (!d.hasMesh()) throw CommandError("no mesh open");
           std::string id = a.at("material");
           auto* entry = e.materials.find(id);
           if (!entry) throw CommandError("unknown material '" + id + "' (see list_materials)");
           Region r = regionArg(e, a);
           std::string name = a.value("name", entry->def.name);
           GroupGuard g(d, "Material layer: " + shortName(name));
           MaterialRef ref;
           ref.id = entry->def.id;
           ref.mapping = a.value("mapping", std::string("triplanar")) == "uv" ? 1 : 0;
           ref.scale = std::clamp(a.value("scale", 1.f), 0.05f, 20.f);
           if (a.contains("tint") && a["tint"].is_array() && a["tint"].size() == 3)
             ref.tint = vec3(a["tint"][0].get<float>(), a["tint"][1].get<float>(), a["tint"][2].get<float>());
           Layer& l = d.addFillLayer(ref, name);
           uint32_t uid = l.uid;
           l.opacity = std::clamp(a.value("strength", 1.f), 0.f, 1.f);
           d.checkpoint();
           applyRegionAndMasks(e, d.indexOf(uid), r, a.value("mask", json::array()));
           g.commit();
           return json{{"index", d.indexOf(uid)}, {"uid", uid}, {"name", name}, {"region_faces", r.given ? r.faces.size() : d.mesh.faceCount()}};
         }});

  R.add({"update_layer_mask",
         "Changes where a layer shows: a new region (parts from mesh_parts) and / or a new smart mask stack. What is "
         "not given stays: a new region keeps the smart masks, new smart masks keep the region. One undo step.",
         schema({{"index", num("integer", "Layer index (0 = bottom). Default: the selected layer", 0, 100000)}, {"region", regionSchema()},
                 {"mask", maskSchema()},
                 {"everywhere", prop("boolean", "Drop the region: the layer covers every part again")}}),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           Document& d = e.doc;
           int i = a.value("index", d.selected);
           if (i < 0 || i >= (int)d.layers.size()) throw CommandError("layer index out of range");
           Region r = regionArg(e, a);
           bool everywhere = a.value("everywhere", false);
           if (!r.given && !a.contains("mask") && !everywhere) throw CommandError("give a region, a mask or everywhere");
           uint32_t uid = d.layers[i]->uid;
           GroupGuard g(d, "Layer mask: " + shortName(d.layers[i]->name));
           applyRegionAndMasks(e, i, r, a.value("mask", json::array()), !r.given && !everywhere, !a.contains("mask"));
           g.commit();
           Layer& l = *d.layers[d.indexOf(uid)];
           return json{{"index", d.indexOf(uid)}, {"mask_effects", l.toJson().value("mask_effects", json::array())}};
         }});

  R.add({"layer_region_faces",
         "Which faces a layer's region covers: the material-slot masks and painted mask at the start of its mask "
         "stack (or the painted mask of a simple mask), sampled at each face's center. all = no region.",
         schema({{"index", num("integer", "Layer index (0 = bottom). Default: the selected layer", 0, 100000)},
                 {"faces", prop("boolean", "Include the face indices (default true)")}}),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           Document& d = e.doc;
           int i = a.value("index", d.selected);
           if (i < 0 || i >= (int)d.layers.size()) throw CommandError("layer index out of range");
           const Layer& l = *d.layers[i];
           const Mesh& m = d.mesh;
           uint32_t F = m.faceCount();
           // the painted mask at each face's center (texture space)
           std::vector<float> painted;
           auto paintedAt = [&](uint32_t f) {
             if (!l.mask) return 1.f;
             if (painted.empty()) {
               Image img = l.mask->download(1, 0);
               painted.resize(F);
               for (uint32_t g = 0; g < F; ++g) {
                 vec2 cc(0);
                 for (uint32_t k = m.faceStart[g]; k < m.faceStart[g + 1]; ++k) cc += m.texUV(k);
                 cc /= float(m.faceSize(g));
                 // row 0 of the download is the top of the texture (v = 1)
                 int x = std::clamp(int(cc.x * img.width), 0, img.width - 1), y = std::clamp(int((1.f - cc.y) * img.height), 0, img.height - 1);
                 painted[g] = img.at(x, y)[0];
               }
             }
             return painted[f];
           };
           std::vector<uint8_t> in(F, 0);
           bool any = false;
           if (!l.maskEffects.empty()) {
             for (auto& fx : l.maskEffects) {
               bool slot = fx.type == MaskEffect::Generator && fx.gen.type == MaskGenerator::MaterialSlot;
               if (!(fx.type == MaskEffect::Paint || slot) || !fx.enabled) break;  // the region ends here
               bool first = !any;
               any = true;
               for (uint32_t f = 0; f < F; ++f) {
                 bool v = slot ? (f < m.faceMaterial.size() && (int)m.faceMaterial[f] == fx.gen.slot) : paintedAt(f) > 0.5f;
                 if (fx.blend == MaskEffect::Subtract) in[f] = in[f] && !v;
                 else if (fx.blend == MaskEffect::Multiply || fx.blend == MaskEffect::Darken) in[f] = (first || in[f]) && v;
                 else in[f] = first ? v : (in[f] || v);
               }
             }
           } else if (l.mask) {
             any = true;
             for (uint32_t f = 0; f < F; ++f) in[f] = paintedAt(f) > 0.5f;
           } else if (l.generator.type == MaskGenerator::MaterialSlot) {
             any = true;
             for (uint32_t f = 0; f < F; ++f) in[f] = f < m.faceMaterial.size() && (int)m.faceMaterial[f] == l.generator.slot;
           }
           json faces = json::array();
           size_t n = 0;
           for (uint32_t f = 0; f < F; ++f)
             if (!any || in[f]) {
               ++n;
               if (a.value("faces", true)) faces.push_back(f);
             }
           json out = {{"all", !any}, {"count", n}};
           if (a.value("faces", true)) out["face_indices"] = faces;
           return out;
         }});

  R.add({"uv_island_per_part",
         "One UV island per part: seams on the parts' borders and none inside them, each part unwrapped as one "
         "island (closed parts and tubes get a cut along the back, as a head or sleeve would), then everything is "
         "packed at an equal texel density. A part made of separate pieces gives an island per piece. Paint is "
         "kept. One undo step.",
         schema({{"parts", {{"type", "array"}, {"items", {{"type", "string"}}}, {"minItems", 1}, {"maxItems", 16},
                            {"description", "Part ids (mesh_parts); a face in several parts goes to the first"}}},
                 {"pack", prop("boolean", "Pack all islands afterward (default true)")},
                 {"density", prop("string", "auto (default) | layout | fit | screen")},
                 {"margin", propNum("Gap between islands in UV units (default 0.005)", 0, 0.1)},
                 {"reproject", prop("boolean", "Keep existing paint (default true)")}},
                {"parts"}),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           Document& d = e.doc;
           if (!d.hasMesh()) throw CommandError("no mesh open");
           const json& ids = a.at("parts");
           if (!ids.is_array() || ids.empty()) throw CommandError("give at least one part");
           uint32_t F = d.mesh.faceCount();
           std::vector<uint8_t> taken(F, 0);
           struct Job {
             std::string id;
             std::vector<uint32_t> faces;
           };
           std::vector<Job> jobs;
           for (auto& j : ids) {
             Job job{j.get<std::string>(), {}};
             for (uint32_t f : uv::unpinnedFaces(d.mesh, e.partFaces(job.id)))
               if (!taken[f]) taken[f] = 1, job.faces.push_back(f);
             jobs.push_back(std::move(job));
           }
           Mesh work = d.mesh;
           if (work.faceFlags.size() != F) work.faceFlags.assign(F, 0);
           float margin = std::clamp(a.value("margin", 0.005f), 0.f, 0.1f);
           std::string dens = a.value("density", std::string("auto"));
           auto targets = uv::densityTargets(work, d.tileRes());
           float density = dens == "fit" ? targets.fit : dens == "screen" ? targets.screen
                         : dens == "layout" && targets.layout > 0 ? targets.layout : targets.recommended();
           bool pack = a.value("pack", true);
           json report = json::array();
           std::vector<std::string> folded;  // parts whose layout lies over itself
           for (auto& job : jobs) {
             if (job.faces.empty()) {
               report.push_back({{"part", job.id}, {"faces", 0}, {"note", "nothing left (pinned, or in an earlier part)"}});
               continue;
             }
             for (uint32_t f : job.faces) work.faceFlags[f] &= ~kFaceMirrorBits;  // an island of its own
             // The part is unwrapped as a small mesh of its own (one per texture tile it is in).
             // Candidates, best first: LSCM inside its border seams (with a back cut where the part
             // is closed or a tube), LSCM with a back cut anyway, the sphere and the cylinder
             // projections. The first with one island, no fold-overs and little stretch wins;
             // otherwise the best of them.
             struct Result {
               std::string method;
               std::vector<std::pair<std::vector<uint32_t>, parts::PartMesh>> tiles;  // faces, unwrapped part
               int cuts = 0;
               size_t islands = 0, pieces = 0;  // the aim: an island per connected piece
               int overlaps = 0;
               std::vector<float> stretch;
               float p95 = 0;
               size_t faces = 0;
               float sizeP90 = 0;  // how unevenly sized the faces are: |log2| of area ratio, 90th percentile
               size_t extra() const { return islands > pieces ? islands - pieces : 0; }
               bool good() const { return extra() == 0 && overlaps == 0 && p95 <= 2.f && sizeP90 <= 0.6f; }
               // lower is better: an island more than asked for counts 3 (one island is what was
               // asked for), folding over 2% of the faces counts 1, so does each step of 2 in stretch
               // above 1.5, and parts drawn at half or double size (the 90th percentile)
               float cost() const {
                 return 3.f * float(extra()) + 50.f * float(overlaps) / float(std::max<size_t>(1, faces)) +
                        0.5f * std::max(0.f, p95 - 1.5f) + std::max(0.f, sizeP90 - 0.3f);
               }
               bool better(const Result& o) const { return cost() < o.cost(); }
             };
             // each connected piece of the part gets the unwrap that suits it (a torso, an arm held out
             // and a foot want different cuts)
             std::vector<std::vector<uint32_t>> pieceList;
             {
               uint32_t np = 0;
               auto pieceOf = parts::facePieces(work, &np);
               std::map<uint32_t, std::vector<uint32_t>> byPiece;
               for (uint32_t f : job.faces) byPiece[pieceOf[f]].push_back(f);
               for (auto& [k, v] : byPiece) pieceList.push_back(std::move(v));
             }
             Result total;
             std::set<std::string> methods;
             std::vector<float> allSizes;
             for (const std::vector<uint32_t>& pf : pieceList) {
               auto attempt = [&](const std::string& method) {
                 Result r{method};
                 std::vector<float> sizes;
                 Mesh probe = work;  // forEachTile needs the tile layout; the probe is thrown away
                 uv::forEachTile(probe, pf, [&](const std::vector<uint32_t>& tf) {
                   parts::PartMesh pm = parts::partMesh(probe, tf);
                   Mesh& mm = pm.mesh;
                   if (!mm.faceCount()) return;
                   std::vector<uint32_t> all(mm.faceCount());  // every face of the part mesh
                   std::iota(all.begin(), all.end(), 0u);
                   r.pieces += parts::pieceCount(mm, all);
                   r.faces += all.size();
                   r.cuts += parts::islandSeams(mm, all, method.find("back_cut") != std::string::npos);
                   if (method.find("tip_cuts") != std::string::npos) r.cuts += parts::tipCuts(mm, all);
                   if (method.rfind("lscm", 0) == 0) r.cuts += parts::diskCuts(mm, all);  // handles, odd edges
                   uv::PackOptions po;
                   po.margin = margin;
                   if (method.rfind("lscm", 0) == 0) {
                     uv::UnwrapOptions uo;
                     uo.pack = false;
                     uv::unwrapLSCM(mm, {}, uo);
                     mm.computeUVTopology();
                     if (method.find("arap") != std::string::npos)
                       uv::arapRelax(mm);  // evens out the sizes LSCM leaves (long limbs) and most fold-overs
                     uv::pack(mm, po, {});
                   } else if (method == "sphere") {
                     uv::projectSphere(mm, {}, -1, margin);
                   } else {
                     uv::projectCylinder(mm, {}, -1, margin);
                   }
                   mm.computeUVTopology();
                   r.islands += mm.islandCount;
                   r.overlaps += uv::overlapCount(mm);
                   auto st = uv::faceStretch(mm);
                   double a3 = 0, auv = 0;
                   for (uint32_t f = 0; f < mm.faceCount(); ++f) a3 += mm.faceArea3D(f), auv += std::abs(mm.faceAreaUV(f));
                   double k = auv > 0 ? a3 / auv : 1.0;
                   for (uint32_t f = 0; f < mm.faceCount(); ++f) {
                     if (st[f] > 0) r.stretch.push_back(st[f]);
                     double a = mm.faceArea3D(f);
                     if (a > 0) sizes.push_back(float(std::abs(std::log2(std::max(1e-12, k * std::abs(mm.faceAreaUV(f)) / a)))));
                   }
                   r.tiles.push_back({tf, std::move(pm)});
                 });
                 r.p95 = percentile(r.stretch, 0.95f);
                 r.sizeP90 = percentile(sizes, 0.9f);
                 return r;
               };
               Result best;
               bool first = true;
               for (const char* m : {"lscm", "lscm_arap", "lscm_tip_cuts_arap", "lscm_tip_cuts", "lscm_back_cut_arap",
                                     "sphere", "cylinder"}) {
                 if (!first && best.good()) break;
                 Result r = attempt(m);
                 if (pf.size() * 20 > job.faces.size())  // the pieces that matter
                   PLOG_I("uv_island_per_part %s (%zu faces): %s -> %zu island(s), %d folded, stretch p95 %.2f, size p90 %.2f, "
                          "cost %.2f", job.id.c_str(), pf.size(), m, r.islands, r.overlaps, r.p95, r.sizeP90, r.cost());
                 if (first || r.better(best)) best = std::move(r);
                 first = false;
               }
               if (best.tiles.empty()) continue;  // nothing to unwrap in this piece
               // onto the real mesh: tile by tile, then oriented, sized and placed like an Island brush island
               size_t ti = 0;
               uv::forEachTile(work, pf, [&](const std::vector<uint32_t>& tf) {
                 if (ti >= best.tiles.size()) return;
                 auto& [faces, pm] = best.tiles[ti++];
                 parts::copyPartUVs(pm, work, faces);
                 work.computeUVTopology();
                 for (auto& chart : uv::facesByIsland(work, faces)) {
                   uv::orientChart(work, chart, uv::Orient::Upright);
                   uv::scaleChartToDensity(work, chart, density);
                 }
                 if (!pack) uv::packIntoFreeSpace(work, faces, margin);
               });
               total.islands += best.islands;
               total.pieces += best.pieces;
               total.overlaps += best.overlaps;
               total.cuts += best.cuts;
               total.stretch.insert(total.stretch.end(), best.stretch.begin(), best.stretch.end());
               allSizes.push_back(best.sizeP90);
               methods.insert(best.method);
             }
             Result best = std::move(total);
             best.method = methods.size() == 1 ? *methods.begin() : "per piece";
             best.p95 = percentile(best.stretch, 0.95f);
             best.sizeP90 = allSizes.empty() ? 0.f : *std::max_element(allSizes.begin(), allSizes.end());
             work.computeUVTopology();
             double mean = 0;
             for (float v : best.stretch) mean += v;
             report.push_back({{"part", job.id}, {"faces", job.faces.size()}, {"islands", best.islands}, {"pieces", best.pieces},
                               {"method", best.method}, {"cut_edges", best.cuts}, {"folded_faces", best.overlaps},
                               {"size_spread_p90", best.sizeP90},
                               {"stretch_mean", best.stretch.empty() ? 1.0 : mean / best.stretch.size()},
                               {"stretch_p95", best.p95}});
             // a part that lies over itself shares texels between two places (a whole body as one
             // island: the arms and legs end up on the torso)
             if (best.overlaps * 100 > int(std::max<size_t>(1, best.faces))) folded.push_back(job.id);
           }
           // plain words for the panel: what the user may want to do about it
           json notes = json::array();
           if (!folded.empty()) {
             std::string names;
             for (size_t i = 0; i < folded.size(); ++i) {
               std::string n = folded[i].substr(folded[i].find(':') + 1);
               names += (i == 0 ? "The " : i + 1 == folded.size() ? " and the " : ", the ") + shortName(n);
             }
             notes.push_back(names + (folded.size() > 1 ? " lie over themselves" : " lies over itself") +
                             " in places, so some spots share texture. For a cleaner layout, make more islands: paint "
                             "an arm, a leg or a strand of hair with the Island brush (L) and press Enter.");
           }
           if (pack) {
             uv::PackOptions po;
             po.margin = margin;
             po.rotate = true;
             po.normalizeDensity = true;
             bool pinned = uv::pinnedFaceCount(work) > 0;
             uv::forEachTile(work, {}, [&](const std::vector<uint32_t>& f) {
               if (pinned) uv::packAroundFixed(work, uv::unpinnedFaces(work, f), margin);
               else uv::pack(work, po, work.tiles.active() ? f : std::vector<uint32_t>{});
             });
             work.computeUVTopology();
           }
           uv::seamsFromIslands(work, true);  // projections cut their own islands: seams follow them
           std::string label = "One island per part";
           d.applyUVs(work.loopUV, a.value("reproject", true), label, &work.edgeFlags, &work.faceFlags);
           return json{{"parts", report}, {"islands", d.mesh.islandCount}, {"coverage", uv::coverage(d.mesh)},
                       {"overlaps", uv::overlapCount(d.mesh)}, {"notes", notes}};
         }});

  auto needAssistant = [](CommandContext& c) {
    if (!c.engine.features.on("patina_assistant")) throw CommandError("the Patina Assistant is switched off (features)");
  };
  R.add({"assistant_check",
         "Checks an Assistant plan without running it: the command allowlist, argument schemas, $n.field references "
         "and the step limit. Returns the steps in plain words and warnings for steps that delete, clear or rebuild.",
         schema({{"plan", prop("object", "{say, ask, steps: [{cmd, args}]}")}}, {"plan"}),
         [needAssistant](CommandContext& c, const json& a) {
           needAssistant(c);
           auto pc = assistant::checkPlan(c.engine.commands, a.at("plan"));
           json words = json::array();
           auto names = [&c](const std::string& id) {
             auto* m = c.engine.materials.find(id);
             return m ? m->def.name : std::string();
           };
           for (auto& st : a["plan"].value("steps", json::array())) words.push_back(assistant::describeStep(st, names));
           return json{{"ok", pc.ok}, {"error", pc.error}, {"warnings", pc.warnings}, {"steps", words}};
         }});
  auto ensureModel = [](Engine& e, const std::string& file, double waitSeconds) {
    AssistantModel& m = e.assistantModel();
    if (m.state() == AssistantModel::State::NoModel || m.state() == AssistantModel::State::Failed ||
        (m.state() == AssistantModel::State::Ready && m.path() != file)) {
      if (!std::filesystem::exists(file))
        throw CommandError("no Assistant model at " + file + " (Preferences > Features > Patina Assistant: Choose model file)");
      m.load(file);
    }
    double t0 = nowSeconds();
    while ((m.state() == AssistantModel::State::Loading) && nowSeconds() - t0 < waitSeconds)
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    if (m.state() == AssistantModel::State::Failed) throw CommandError(m.error());
  };
  R.add({"assistant_model",
         "The Assistant's model: action load (the file from path, or the configured one), unload or status "
         "(default). Status gives the file, its state and where it runs.",
         schema({{"action", propEnum({"status", "load", "unload"}, "What to do")},
                 {"path", prop("string", "A GGUF model file (load)")},
                 {"sha256", prop("boolean", "status: also give the file's SHA-256 (reads the whole file)")}}),
         [needAssistant, ensureModel](CommandContext& c, const json& a) {
           needAssistant(c);
           Engine& e = c.engine;
           std::string act = a.value("action", std::string("status"));
           if (act == "load") {
             if (a.contains("path")) e.assistantModelPath = a["path"];
             ensureModel(e, e.assistantModelFile(), 300);
           } else if (act == "unload") {
             e.assistantModel().unload();
           }
           AssistantModel& m = e.assistantModel();
           static const char* states[] = {"none", "loading", "ready", "busy", "failed"};
           json out = {{"file", e.assistantModelFile()}, {"state", states[(int)m.state()]}, {"backend", m.backendInfo()},
                       {"error", m.error()}, {"exists", std::filesystem::exists(e.assistantModelFile())}};
           if (a.value("sha256", false) && out["exists"].get<bool>()) out["sha256"] = sha256File(e.assistantModelFile());
           return out;
         }});
  R.add({"assistant_ask",
         "Asks the Assistant's model for a plan for a request about the open project, without running it: the "
         "project description and the grammar are made here, the model writes the plan, and the plan is checked "
         "(see assistant_apply to run it). Needs a model file.",
         schema({{"request", prop("string", "What the user asked, in plain words")},
                 {"timeout", num("number", "Seconds to wait for the local model before giving up", 1, 600, 30)}},
                {"request"}),
         [needAssistant, ensureModel](CommandContext& c, const json& a) {
           needAssistant(c);
           Engine& e = c.engine;
           std::string request = a.at("request");
           ensureModel(e, e.assistantModelFile(), 300);
           json ctx = buildAssistantContext(e, request);
           std::string text = ctx["text"];
           while (!text.empty() && std::isspace((unsigned char)text.back())) text.pop_back();
           std::string user = text + "\nRequest: " + request;  // as tools/assistant/build_dataset.py writes it
           auto fut = e.assistantModel().ask(ctx["system"], user, assistant::buildGrammar(e), a.value("timeout", 30.0));
           AssistantModel::Answer ans = fut.get();
           json out = {{"raw", ans.text}, {"seconds", ans.seconds}, {"prompt_tokens", ans.promptTokens},
                       {"reused_tokens", ans.reusedTokens}, {"output_tokens", ans.outputTokens}};
           if (!ans.ok) throw CommandError("the Assistant did not answer: " + ans.error);
           json plan = json::parse(ans.text, nullptr, false);
           if (plan.is_discarded()) throw CommandError("the Assistant's answer is not a plan: " + ans.text);
           auto pc = assistant::checkPlan(e.commands, plan);
           json words = json::array();
           auto names = [&e](const std::string& id) {
             auto* m = e.materials.find(id);
             return m ? m->def.name : std::string();
           };
           for (auto& st : plan.value("steps", json::array())) words.push_back(assistant::describeStep(st, names));
           out.update({{"plan", plan}, {"ok", pc.ok}, {"error", pc.error}, {"warnings", pc.warnings}, {"steps", words}});
           return out;
         }});
  R.add({"assistant_panel",
         "Opens the Assistant window in the desktop app, and asks it a request when one is given (the plan then "
         "waits there for the user to preview and keep). Not available headless.",
         schema({{"request", prop("string", "What to ask (optional)")},
                 {"preview", prop("boolean", "Preview the plan as soon as it arrives (default false)")}}),
         [needAssistant](CommandContext& c, const json& a) {
           needAssistant(c);
           if (!c.engine.openAssistantPanel) throw CommandError("the Assistant window needs the desktop app (not headless)");
           c.engine.openAssistantPanel(a.value("request", std::string()), a.value("preview", false));
           return json{{"opened", true}};
         }});
  R.add({"assistant_grammar",
         "The GBNF grammar the Assistant's model is held to for the open project (plan shape, allowlisted commands "
         "and their arguments, the part ids, layer indices and materials there are).",
         schema(), [](CommandContext& c, const json&) { return json{{"grammar", assistant::buildGrammar(c.engine)}}; }});
  R.add({"assistant_apply",
         "Runs an Assistant plan ({say, steps: [{cmd, args}]}) as one undo step: only allowlisted commands, at most 16 "
         "steps, \"$n.field\" for step n's result. A step that fails undoes the steps before it.",
         schema({{"plan", prop("object", "{say, ask, steps: [{cmd, args}]}")},
                 {"request", prop("string", "What was asked (remembered for follow-ups)")},
                 {"label", prop("string", "Undo step name (default: Assistant + the plan's say)")}},
                {"plan"}),
         [needAssistant](CommandContext& c, const json& a) {
           needAssistant(c);
           Engine& e = c.engine;
           const json& plan = a.at("plan");
           std::string say = plan.value("say", std::string());
           std::string label = a.value("label", "Assistant" + (say.empty() ? std::string() : ": " + shortName(say, 48)));
           uint64_t before = e.doc.undo.topSerial();
           json r = assistant::applyPlan(e, plan, label);
           uint64_t after = e.doc.undo.topSerial();
           if (after != before) {  // it changed something: remember it for follow-ups
             Engine::AssistantTurn t{a.value("request", say), say, {}, after};
             for (auto& res : r["results"])
               if (res.is_object() && res.contains("uid") && res["uid"].is_number_unsigned()) t.layers.push_back(res["uid"]);
             e.assistantTurns.push_back(std::move(t));
             if (e.assistantTurns.size() > 8) e.assistantTurns.erase(e.assistantTurns.begin());
           }
           return r;
         }});

  R.add({"assistant_context",
         "The short text description of the open project that the Patina Assistant's model reads with a request: "
         "the mesh, its parts, texture layout, layer stack and library materials that fit the request.",
         schema({{"request", prop("string", "What the user asked (picks the materials listed)")}}),
         [](CommandContext& c, const json& a) {
           return buildAssistantContext(c.engine, a.value("request", std::string()));
         }});
}

}  // namespace patina
