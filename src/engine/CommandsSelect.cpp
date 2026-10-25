// (1.2) Selecting faces in batches (core/MeshSelect): face loops and rings, the connected part, flat regions.
#include <algorithm>
#include <unordered_map>

#include "core/MeshSelect.h"
#include "engine/Engine.h"

namespace patina {

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
json schema(json props = json::object(), std::vector<std::string> required = {}) {
  json s = {{"type", "object"}, {"properties", props}, {"additionalProperties", false}};
  if (!required.empty()) s["required"] = required;
  return s;
}

// the faces given to the selection (set, add, remove), as one undo step `label`
json applySelection(Engine& e, const std::vector<uint32_t>& faces, const std::string& mode, const char* label) {
  Document& d = e.doc;
  auto& s = d.faceSelection;
  s.resize(d.mesh.faceCount(), 0);
  std::vector<uint8_t> before = s;
  if (mode == "set") std::fill(s.begin(), s.end(), 0);
  for (uint32_t f : faces)
    if (f < s.size()) s[f] = mode == "remove" ? 0 : 1;
  if (s != before) {
    d.updateSelectionFlags();
    d.pushSelectionUndo(before, label);
  }
  return json{{"faces", faces.size()}, {"selected", d.selectedFaces().size()}};
}

// the face (and the point on it) under a viewport point, or given as face (and edge)
bool faceArg(Engine& e, const json& a, uint32_t& face, vec3* where) {
  if (a.contains("face")) {
    face = a["face"].get<uint32_t>();
    if (face >= e.doc.mesh.faceCount()) throw CommandError("face: out of range");
    if (where) *where = e.doc.mesh.faceCenter(face);
    return true;
  }
  if (!a.contains("point")) return false;
  if (!a["point"].is_array() || a["point"].size() < 2) throw CommandError("point is [x, y]");
  vec2 p(a["point"][0].get<float>(), a["point"][1].get<float>());
  RayHit h = e.pick(p * e.viewportSize);
  if (!h.hit) throw CommandError("the point is not on the model (the active object)");
  face = h.face;
  if (where) *where = h.position;
  return true;
}
}  // namespace

const select::EdgeFaces& Engine::edgeFaces() {
  uint64_t key = doc.gpu.serial() ^ (uint64_t(doc.mesh.faceCount()) << 40);
  if (key != edgeFacesKey_ || !edgeFaces_.builtFor(doc.mesh)) edgeFaces_.build(doc.mesh), edgeFacesKey_ = key;
  return edgeFaces_;
}

namespace {
// a face set by name or id (`set`); throws naming the sets there are
int setArg(Engine& e, const json& a) {
  if (!a.contains("set")) throw CommandError("set: the face set's name or id");
  std::string key = a["set"].is_number_integer() ? std::to_string(a["set"].get<int>()) : a["set"].get<std::string>();
  int i = e.doc.faceSetIndex(key);
  if (i < 0) {
    std::string names;
    for (auto& s : e.doc.faceSets) names += (names.empty() ? "" : ", ") + s.name;
    throw CommandError("no face set '" + key + "'" + (names.empty() ? std::string(" (there are none)") : " (the face sets: " + names + ")"));
  }
  return i;
}

std::vector<uint32_t> facesArg(Engine& e, const json& v) {
  std::vector<uint32_t> out;
  if (v.is_string() && v.get<std::string>() == "selection") return e.doc.selectedFaces();
  if (v.is_array())
    for (auto& f : v)
      if (f.get<uint32_t>() < e.doc.mesh.faceCount()) out.push_back(f.get<uint32_t>());
  return out;
}

json faceSetJson(const Document& d, const Document::FaceSet& s) {
  return {{"id", s.id}, {"name", s.name}, {"color", {s.color.x, s.color.y, s.color.z}}, {"hidden", s.hidden},
          {"faces", d.faceSetSize(s.id)}};
}

void needSets(Engine& e) {
  if (!e.features.on("face_sets")) throw CommandError("face sets are switched off (features: face_sets)");
}
}  // namespace

void Engine::registerSelectCommands() {
  CommandRegistry& R = commands;
  const json modeProp = propEnum({"set", "add", "remove"}, "What the faces do to the selection (default set)");

  R.add({"select_loop",
         "Selects a band of quads (1.2), as Blender's Alt+click by an edge in face mode: from the face under point "
         "(or face), across its edge nearest that point (or edge, the face's corner index), into the next face and out "
         "of its opposite edge, both ways, until a face that isn't a quad, a border or the band closes. ring: the band "
         "along that edge instead (Ctrl+Alt+click). One undo step.",
         schema({{"point", prop("array", "[x,y] normalized viewport point on the active object (0..1, 0,0 top left)")},
                 {"face", num("integer", "A face instead of point", 0, 1e9)},
                 {"edge", num("integer", "With face: its edge (corner index from 0); default the first", 0, 1e6)},
                 {"ring", prop("boolean", "The band along the edge instead of across it (default false)")},
                 {"mode", modeProp}}),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           uint32_t f = 0;
           vec3 p;
           if (!faceArg(e, a, f, &p)) throw CommandError("give point or face");
           const Mesh& m = e.doc.mesh;
           uint32_t k = a.contains("point") && !a.contains("face") ? select::nearestEdge(m, f, p) : a.value("edge", 0u) % m.faceSize(f);
           const auto& ef = e.edgeFaces();
           bool ring = a.value("ring", false);
           auto faces = ring ? select::faceRing(m, ef, f, k) : select::faceLoop(m, ef, f, k);
           json r = applySelection(e, faces, a.value("mode", std::string("set")), ring ? "Select a ring" : "Select a loop");
           r["quads"] = m.faceSize(f) == 4;
           return r;
         }});

  R.add({"select_linked",
         "Selects the faces linked to the face under point (or face; neither: to the faces selected) (1.2): "
         "connected (the whole connected part, by shared corners), flat (spreading over edges where the surface bends "
         "less than angle: a box's side, a panel), island (its UV islands) or material (its material slots). One undo "
         "step.",
         schema({{"point", prop("array", "[x,y] normalized viewport point on the active object")},
                 {"face", num("integer", "A face instead of point", 0, 1e9)},
                 {"by", propEnum({"connected", "flat", "island", "material"}, "What links them (default connected)")},
                 {"angle", num("number", "by flat: the most the surface may bend from face to face, in degrees", 0, 180, 10)},
                 {"mode", modeProp}}),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           const Mesh& m = e.doc.mesh;
           std::vector<uint32_t> seeds;
           uint32_t f = 0;
           if (faceArg(e, a, f, nullptr)) seeds = {f};
           else seeds = e.doc.selectedFaces();
           if (seeds.empty()) throw CommandError("no face: give point or face, or select some first");
           std::string by = a.value("by", std::string("connected"));
           std::vector<uint32_t> faces;
           if (by == "flat") {
             faces = select::flatRegion(m, e.edgeFaces(), seeds, a.value("angle", 10.f));
           } else if (by == "island" || by == "material") {
             std::vector<uint8_t> want(by == "island" ? m.islandCount : 256, 0);
             auto keyOf = [&](uint32_t g) -> uint32_t {
               return by == "island" ? m.loopIsland[m.faceStart[g]] : std::min<uint32_t>(g < m.faceMaterial.size() ? m.faceMaterial[g] : 0, 255);
             };
             for (uint32_t s : seeds) want[keyOf(s)] = 1;
             for (uint32_t g = 0; g < m.faceCount(); ++g)
               if (want[keyOf(g)]) faces.push_back(g);
           } else {
             faces = select::linked(m, e.edgeFaces(), seeds, true);
           }
           std::string label = by == "flat" ? "Select a flat region" : by == "island" ? "Select the islands"
                             : by == "material" ? "Select the material slots" : "Select the connected part";
           return applySelection(e, faces, a.value("mode", std::string("set")), label.c_str());
         }});

  // ---------------------------------------------------------------- face sets
  R.add({"list_face_sets",
         "The active object's face sets (1.2): named, colored groups of faces kept in the project (a face is in one "
         "at most), each with its id, color, face count and whether it is hidden. Blender's sculpt face sets come in "
         "with the model from the add-on 0.14.",
         schema(), [](CommandContext& c, const json&) {
           Document& d = c.engine.doc;
           json a = json::array();
           for (auto& s : d.faceSets) a.push_back(faceSetJson(d, s));
           size_t none = 0;
           for (uint32_t f = 0; f < d.mesh.faceCount(); ++f) none += f >= d.faceSetOf.size() || !d.faceSetOf[f];
           return json{{"face_sets", a}, {"faces_in_none", none}};
         }});

  R.add({"make_face_set",
         "Makes face sets (1.2). from selection (default) or faces: one set of those faces, named name (they leave "
         "the sets they were in; (1.2.10) a set left with no faces stays, empty, until delete_face_set). from material, island or connected: a set for each material slot, UV island or "
         "connected part (replacing the sets there were). One undo step.",
         schema({{"from", propEnum({"selection", "faces", "material", "island", "connected"}, "What the sets are made of (default selection)")},
                 {"faces", prop("array", "from faces: the face indices")},
                 {"name", prop("string", "The new set's name (one set)")},
                 {"color", prop("array", "[r,g,b] 0..1: its color (default: one of its own)")}}),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           needSets(e);
           Document& d = e.doc;
           std::string from = a.value("from", std::string(a.contains("faces") ? "faces" : "selection"));
           if (from == "selection" || from == "faces") {
             auto faces = from == "faces" ? facesArg(e, a.value("faces", json::array())) : d.selectedFaces();
             if (faces.empty()) throw CommandError(from == "faces" ? "no faces given" : "no faces are selected");
             vec3 col;
             bool hasCol = a.contains("color") && a["color"].is_array() && a["color"].size() == 3;
             if (hasCol) col = vec3(a["color"][0].get<float>(), a["color"][1].get<float>(), a["color"][2].get<float>());
             std::string name = a.value("name", std::string());
             if (!name.empty() && d.faceSetIndex(name) >= 0 && d.faceSets[d.faceSetIndex(name)].name == name)
               throw CommandError("another face set is called '" + name + "'");
             uint32_t id = d.addFaceSet(faces, name, hasCol ? &col : nullptr);
             return faceSetJson(d, *d.findFaceSet(id));
           }
           // a set per slot, island or connected part
           const Mesh& m = d.mesh;
           std::vector<uint32_t> key(m.faceCount(), 0);
           std::vector<std::string> names;
           if (from == "material") {
             for (uint32_t f = 0; f < m.faceCount(); ++f) key[f] = f < m.faceMaterial.size() ? m.faceMaterial[f] : 0;
           } else if (from == "island") {
             for (uint32_t f = 0; f < m.faceCount(); ++f) key[f] = m.loopIsland[m.faceStart[f]];
           } else if (from == "connected") {
             std::vector<uint8_t> done(m.faceCount(), 0);
             uint32_t n = 0;
             for (uint32_t f = 0; f < m.faceCount(); ++f) {
               if (done[f]) continue;
               for (uint32_t g : select::linked(m, e.edgeFaces(), {f}, true)) done[g] = 1, key[g] = n;
               ++n;
             }
           } else {
             throw CommandError("from: selection, faces, material, island or connected");
           }
           std::vector<uint32_t> distinct(key);
           std::sort(distinct.begin(), distinct.end());
           distinct.erase(std::unique(distinct.begin(), distinct.end()), distinct.end());
           if (distinct.size() > 4096) throw CommandError("that would be " + std::to_string(distinct.size()) + " face sets: at most 4096");
           std::vector<Document::FaceSet> sets;
           std::vector<uint32_t> of(m.faceCount(), 0);
           uint32_t id = 1;
           std::unordered_map<uint32_t, uint32_t> idOf;
           for (uint32_t k : distinct) {
             Document::FaceSet s;
             s.id = id++;
             s.name = from == "material" ? m.slotName(k) : from == "island" ? "Island " + std::to_string(k + 1) : "Part " + std::to_string(k + 1);
             s.color = Document::faceSetColor(s.id);
             idOf[k] = s.id;
             sets.push_back(s);
           }
           for (uint32_t f = 0; f < m.faceCount(); ++f) of[f] = idOf[key[f]];
           d.nextFaceSetId = id;
           d.setFaceSets(std::move(sets), std::move(of), "Make face sets");
           json out = json::array();
           for (auto& s : d.faceSets) out.push_back(faceSetJson(d, s));
           return json{{"face_sets", out}};
         }});

  R.add({"edit_face_set",
         "Changes a face set (1.2): its name, color, hidden (its faces leave the 3D view, the brush and picking; "
         "renders and exports keep them), and its faces: add or remove \"selection\" or a list of face indices "
         "(faces added leave the sets they were in; (1.2.10) a set left with no faces stays, empty). One undo step.",
         schema({{"set", prop("string", "The face set: its name or id")},
                 {"name", prop("string", "A new name")},
                 {"color", prop("array", "[r,g,b] 0..1")},
                 {"hidden", prop("boolean", "Hidden (true) or shown")},
                 {"add", {{"description", "\"selection\" or face indices to put in it"}}},
                 {"remove", {{"description", "\"selection\" or face indices to take out of it"}}}},
                {"set"}),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           needSets(e);
           Document& d = e.doc;
           int i = setArg(e, a);
           auto sets = d.faceSets;
           auto of = d.faceSetOf;
           if (of.size() != d.mesh.faceCount()) of.assign(d.mesh.faceCount(), 0);
           Document::FaceSet& s = sets[i];
           if (a.contains("name")) {
             std::string n = a["name"];
             int other = d.faceSetIndex(n);
             if (n.empty()) throw CommandError("a name can't be empty");
             if (other >= 0 && other != i && d.faceSets[other].name == n) throw CommandError("another face set is called '" + n + "'");
             s.name = n;
           }
           if (a.contains("color") && a["color"].is_array() && a["color"].size() == 3)
             s.color = glm::clamp(vec3(a["color"][0].get<float>(), a["color"][1].get<float>(), a["color"][2].get<float>()), 0.f, 1.f);
           if (a.contains("hidden")) s.hidden = a["hidden"].get<bool>();
           if (a.contains("add"))
             for (uint32_t f : facesArg(e, a["add"])) of[f] = s.id;
           if (a.contains("remove"))
             for (uint32_t f : facesArg(e, a["remove"]))
               if (of[f] == s.id) of[f] = 0;
           uint32_t id = s.id;
           // (1.2.10: sets left without faces stay, empty: only delete_face_set takes a set away)
           d.setFaceSets(std::move(sets), std::move(of), "Change a face set");
           return faceSetJson(d, *d.findFaceSet(id));
         }});

  R.add({"delete_face_set",
         "Deletes a face set (its faces stay, in no set), or all of them (all: true) (1.2). One undo step.",
         schema({{"set", prop("string", "The face set: its name or id")}, {"all", prop("boolean", "Every face set")}}),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           needSets(e);
           Document& d = e.doc;
           if (a.value("all", false)) {
             d.setFaceSets({}, {}, "Delete the face sets");
             return json{{"face_sets", 0}};
           }
           int i = setArg(e, a);
           uint32_t id = d.faceSets[i].id;
           auto sets = d.faceSets;
           sets.erase(sets.begin() + i);
           auto of = d.faceSetOf;
           for (auto& v : of)
             if (v == id) v = 0;
           d.setFaceSets(std::move(sets), std::move(of), "Delete a face set");
           return json{{"face_sets", d.faceSets.size()}};
         }});

  R.add({"use_face_set",
         "Does something with a face set's faces in one go (1.2): select (the selection becomes them), add_to_selection, "
         "(1.2.10) deselect (they leave the selection), "
         "hide, show, isolate (hide every other set and the faces in none; again: show_all), show_all, fill (the "
         "selected layer gets the paint material on them, as the polygon fill does) or mask (the selected layer's "
         "mask: white on them, black elsewhere). One undo step.",
         schema({{"set", prop("string", "The face set: its name or id (not needed for show_all)")},
                 {"action", propEnum({"select", "add_to_selection", "deselect", "hide", "show", "isolate", "show_all", "fill", "mask"}, "What to do")}},
                {"action"}),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           needSets(e);
           Document& d = e.doc;
           std::string act = a.at("action");
           if (act == "show_all") {
             auto sets = d.faceSets;
             for (auto& s : sets) s.hidden = false;
             d.setFaceSets(std::move(sets), d.faceSetOf, "Show every face set", 0);
             return json{{"hidden", 0}};
           }
           int i = setArg(e, a);
           uint32_t id = d.faceSets[i].id;
           auto faces = d.faceSetFaces(id);
           if (act == "select" || act == "add_to_selection")
             return applySelection(e, faces, act == "select" ? "set" : "add", "Select a face set");
           if (act == "deselect") return applySelection(e, faces, "remove", "Deselect a face set");
           if (act == "hide" || act == "show" || act == "isolate") {
             auto sets = d.faceSets;
             for (auto& s : sets) s.hidden = act == "isolate" ? s.id != id : s.id == id ? act == "hide" : s.hidden;
             // (isolate: the faces in no set go too)
             d.setFaceSets(std::move(sets), d.faceSetOf,
                           act == "isolate" ? "Show a face set alone" : act == "hide" ? "Hide a face set" : "Show a face set",
                           act == "isolate" ? 1 : -1);
             size_t hidden = 0;
             for (uint8_t h : d.hiddenFaces) hidden += h;
             return json{{"hidden_faces", hidden}};
           }
           if (act == "fill") {
             if (faces.empty()) throw CommandError("the face set has no faces");
             if (!e.painter.fillFaces(d, e.materials, faces)) throw CommandError("nothing to fill: select a layer and enable a channel");
             return json{{"faces", faces.size()}};
           }
           if (act == "mask") {
             if (d.selected < 0) throw CommandError("select a layer first");
             if (!d.maskFromFaces(d.selected, faces, "Mask from a face set")) throw CommandError("could not make the mask");
             return json{{"faces", faces.size()}, {"layer", d.selected}};
           }
           throw CommandError("action: select, add_to_selection, deselect, hide, show, isolate, show_all, fill or mask");
         }});
}

}  // namespace patina
