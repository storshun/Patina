// (1.2) Commands for the scene's objects: several models in one project, each with its own layers (engine/Scene.cpp).
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
json schema(json props = json::object(), std::vector<std::string> required = {}) {
  json s = {{"type", "object"}, {"properties", props}, {"additionalProperties", false}};
  if (!required.empty()) s["required"] = required;
  return s;
}
const char* kObjectArg = "The object: its name as list_objects gives it, or its index (default: the active one)";

void needFeature(Engine& e) {
  if (!e.features.on("scene_objects")) throw CommandError("objects in a scene are switched off (features: scene_objects)");
}

int objectArg(Engine& e, const json& a, const char* key = "object") {
  if (!a.contains(key) || a[key].is_null()) return e.activeObject();
  std::string s = a[key].is_number_integer() ? std::to_string(a[key].get<int>()) : a[key].get<std::string>();
  int i = e.findObject(s);
  if (i < 0) {
    std::string names;
    for (int k = 0; k < e.objectCount(); ++k) names += (k ? ", " : "") + e.object(k).displayName();
    throw CommandError("no object called '" + s + "' (the objects: " + names + ")");
  }
  return i;
}

vec3 vec3Arg(const json& j, const char* what) {
  if (!j.is_array() || j.size() != 3) throw CommandError(std::string(what) + " is [x, y, z]");
  return vec3(j[0].get<float>(), j[1].get<float>(), j[2].get<float>());
}
}  // namespace

void Engine::registerSceneCommands() {
  CommandRegistry& R = commands;

  R.add({"list_objects",
         "The scene's objects (1.2): each model in the project is an object with its own layers, texture set and "
         "history (a body, its clothes, its eyes, its hair). The active one is the one painted, listed by "
         "list_layers and changed by the other commands. Each with its name, faces, layers, texture resolution, "
         "whether it is shown, its placement (location in meters, rotation in degrees about X, Y, Z, scale) and its "
         "bounds in the scene.",
         schema(), [](CommandContext& c, const json&) {
           return json{{"objects", c.engine.objectsJson()}, {"active", c.engine.activeObject()},
                       {"isolated", c.engine.isolateActive}};
         }});

  R.add({"add_object",
         "Adds a model file to the scene as a new object (1.2), next to the ones there: its own layers and texture "
         "set, placed where the file puts it (a character's clothes, eyes and hair exported from the same scene fit "
         "the body). A file with several meshes adds an object for each (separate: false adds them as one). Its "
         "texture set gets as many texels per meter as the active object's (at most its size), unless resolution "
         "is given; the model's own materials come in as a layer. The active object stays active unless select is "
         "true. Into the startup sphere's scene, the model takes its place.",
         schema({{"path", prop("string", "Model file (.obj .gltf .glb .fbx .stl .3mf .ply .ptmesh)")},
                 {"resolution", num("integer", "Texture set resolution per tile, 512 to 8192, a power of two (0: as many texels per meter as the active object, at most its size)", 0, 8192, 0)},
                 {"separate", prop("boolean", "A file with several meshes: an object for each (default true)")},
                 {"select", prop("boolean", "Make the (first) new object the active one (default false)")}},
                {"path"}),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           needFeature(e);
           std::string err;
           json r;
           int first = e.addObjectsFromFile(a.at("path"), a.value("resolution", 0), !a.value("separate", true), &err, &r);
           if (first < 0) throw CommandError(err);
           if (a.value("select", false)) e.activateObject(first);
           r["active"] = e.activeObject();
           r["count"] = e.objectCount();
           return r;
         }});

  R.add({"select_object",
         "Makes an object the active one (1.2): the one the brush paints, whose layers list_layers gives and the "
         "Layers panel shows, whose UVs the UV editor shows, and that the other commands change. Its own undo "
         "history comes with it.",
         schema({{"object", prop("string", kObjectArg)}}, {"object"}),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           int i = objectArg(e, a);
           e.activateObject(i);
           return json{{"active", i}, {"name", e.doc.displayName()}, {"layers", e.doc.layers.size()}};
         }});

  R.add({"set_object",
         "Changes an object (1.2): its name, whether it is shown, and where it is: location (meters, moved from where "
         "its file put it), rotation (degrees about X, then Y, then Z, about the middle of the model) and scale (1 = as "
         "it came; one number scales evenly). reset: back to where it came in. by: add the location and rotation to "
         "the object's instead of setting them. One undo step in the object's history.",
         schema({{"object", prop("string", kObjectArg)},
                 {"name", prop("string", "A new name")},
                 {"visible", prop("boolean", "Shown in the 3D view, renders and exports")},
                 {"location", prop("array", "[x, y, z] in meters")},
                 {"rotation", prop("array", "[x, y, z] in degrees")},
                 {"scale", {{"description", "[x, y, z], or one number for all three"}}},
                 {"by", prop("boolean", "location and rotation are added to the object's (default false)")},
                 {"reset", prop("boolean", "Back to where it came in (before location, rotation and scale)")},
                 {"on_ground", prop("boolean", "Moved up or down so its lowest point is at height 0")}}),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           needFeature(e);
           int i = objectArg(e, a);
           Document& d = e.object(i);
           if (a.contains("name")) {
             std::string n = a["name"];
             int other = e.findObject(n);
             if (n.empty()) throw CommandError("a name can't be empty");
             if (other >= 0 && other != i && e.object(other).displayName() == n) throw CommandError("another object is called '" + n + "'");
             if (n != d.displayName()) d.objectName = n, e.sceneChanged = true;
           }
           if (a.contains("visible")) {
             bool hide = !a["visible"].get<bool>();
             if (hide != d.objectHidden) d.objectHidden = hide, e.sceneChanged = true;
           }
           Document::Placement p = d.placement;
           bool by = a.value("by", false);
           if (a.value("reset", false)) p.location = vec3(0), p.rotation = vec3(0), p.scale = vec3(1);
           if (a.contains("location")) p.location = (by ? p.location : vec3(0)) + vec3Arg(a["location"], "location");
           if (a.contains("rotation")) p.rotation = (by ? p.rotation : vec3(0)) + vec3Arg(a["rotation"], "rotation");
           if (a.contains("scale")) {
             p.scale = a["scale"].is_number() ? vec3(a["scale"].get<float>()) : vec3Arg(a["scale"], "scale");
             if (glm::any(glm::lessThanEqual(glm::abs(p.scale), vec3(1e-6f)))) throw CommandError("a scale can't be 0");
           }
           if (a.value("on_ground", false) && d.mesh.bounds.valid()) {
             mat4 m = p.matrix();
             float lo = 1e30f;
             for (auto& v : d.mesh.positions) lo = std::min(lo, (m * vec4(v, 1.f)).y);
             p.location.y -= lo;
           }
           if (!(p == d.placement)) d.setPlacement(p, a.value("reset", false) ? "Reset the placement" : "Move the object");
           return e.objectsJson()[i];
         }});

  R.add({"remove_object",
         "Removes an object and its layers from the scene (1.2). The last object can't go. Not undoable: the object's "
         "history goes with it.",
         schema({{"object", prop("string", kObjectArg)}}, {"object"}),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           needFeature(e);
           int i = objectArg(e, a);
           std::string name = e.object(i).displayName();
           if (!e.removeObject(i)) throw CommandError("the scene's last object can't be removed (open another model instead)");
           return json{{"removed", name}, {"count", e.objectCount()}, {"active", e.activeObject()}};
         }});

  R.add({"duplicate_object",
         "A copy of an object (1.2), with its layers and textures, right after it in the list, in the same place "
         "(move it with set_object). Not linked to Blender.",
         schema({{"object", prop("string", kObjectArg)}}),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           needFeature(e);
           std::string err;
           int i = e.duplicateObject(objectArg(e, a), &err);
           if (i < 0) throw CommandError(err.empty() ? "could not copy the object" : err);
           return e.objectsJson()[i];
         }});

  R.add({"isolate_object",
         "Shows only the active object (on) or every object again (off), like Blender's Local view; the brush paints "
         "the active object either way (1.2). Hidden objects stay hidden.",
         schema({{"on", prop("boolean", "Only the active object (default: switch)")}}),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           e.isolateActive = a.value("on", !e.isolateActive);
           return json{{"isolated", e.isolateActive}};
         }});

  R.add({"pick_object",
         "The object under a viewport point (1.2): the first one a ray from the camera meets among those shown, with "
         "the face and the point on it (in the object's own space) and the point in the scene. Hidden objects and, "
         "in the Local view, the others are left out.",
         schema({{"point", prop("array", "[x,y] normalized viewport point (0..1, 0,0 top left)")}}, {"point"}),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           if (!a["point"].is_array() || a["point"].size() < 2) throw CommandError("point is [x, y]");
           vec2 p(a["point"][0].get<float>(), a["point"][1].get<float>());
           Engine::ScenePick sp = e.pickScene(p * e.viewportSize);
           if (sp.object < 0) return json{{"hit", false}};
           const Document& d = e.object(sp.object);
           vec3 w = vec3(d.toWorld() * vec4(sp.hit.position, 1.f));
           return json{{"hit", true}, {"object", sp.object}, {"name", d.displayName()}, {"active", sp.object == e.activeObject()},
                       {"face", sp.hit.face}, {"position", {sp.hit.position.x, sp.hit.position.y, sp.hit.position.z}},
                       {"world", {w.x, w.y, w.z}}};
         }});

  R.add({"frame_object",
         "Frames an object in the 3D view (1.2), the whole scene (all: true), or (1.2.11) the active object's "
         "selected faces (selection: true, as Shift+F / Blender's View Selected; the whole object when none are "
         "selected, framed_selection false then).",
         schema({{"object", prop("string", kObjectArg)},
                 {"all", prop("boolean", "The whole scene")},
                 {"selection", prop("boolean", "The selected faces of the active object")}}),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           if (a.value("selection", false)) {
             bool framed = e.frameSelection();
             json r = e.camera.toJson();
             r["framed_selection"] = framed;
             return r;
           }
           if (a.value("all", false)) {
             e.camera.frame(e.sceneBounds());
           } else {
             const Document& d = e.object(objectArg(e, a));
             AABB b;
             mat4 m = d.toWorld();
             for (auto& p : d.mesh.positions) b.add(vec3(m * vec4(p, 1.f)));
             e.camera.frame(b);
             e.camera.updateClip(e.sceneBounds());  // (the rest of the scene stays inside the view's depth)
           }
           return e.camera.toJson();
         }});
}

}  // namespace patina
