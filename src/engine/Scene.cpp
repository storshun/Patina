// (1.2) The scene: several objects (a body, its clothes, its eyes and hair), each a Document of its own with its
// layers, textures and history. Engine::doc is the active one; the others wait in Engine::objects_.
#include <filesystem>
#include <type_traits>

#include <glm/gtc/matrix_transform.hpp>

#include "core/Log.h"
#include "core/MeshIO.h"
#include "core/Paths.h"
#include "engine/BridgeProjects.h"
#include "engine/Engine.h"
#include "gfx/GL.h"

namespace fs = std::filesystem;

namespace patina {

// (an object goes in and out of Engine::doc by moving: every part of a document has to move, never copy)
static_assert(std::is_move_constructible_v<Document> && std::is_move_assignable_v<Document>);

// ------------------------------------------------------------------ placement

mat4 Document::Placement::matrix() const {
  if (!moved()) return base;
  mat4 m = glm::translate(mat4(1), location + pivot);
  m = glm::rotate(m, glm::radians(rotation.z), vec3(0, 0, 1));
  m = glm::rotate(m, glm::radians(rotation.y), vec3(0, 1, 0));
  m = glm::rotate(m, glm::radians(rotation.x), vec3(1, 0, 0));
  m = glm::scale(m, scale);
  m = glm::translate(m, -pivot);
  return m * base;
}

void Document::setPlacement(const Placement& p, const std::string& label) {
  if (p == placement) return;
  auto e = std::make_unique<LambdaEntry>();
  e->label = label;
  Placement before = placement;
  e->onUndo = [before](Document& d) { d.placement = before; };
  e->onRedo = [p](Document& d) { d.placement = p; };
  placement = p;
  dirty = true;
  undo.push(std::move(e));
}

namespace {
json vec3Json(const vec3& v) { return json::array({v.x, v.y, v.z}); }
vec3 vec3From(const json& j, vec3 def) {
  if (!j.is_array() || j.size() != 3) return def;
  for (auto& x : j)
    if (!x.is_number()) return def;
  return vec3(j[0].get<float>(), j[1].get<float>(), j[2].get<float>());
}
}  // namespace

json Document::objectJson() const {
  json base = json::array();
  for (int c = 0; c < 4; ++c)
    for (int r = 0; r < 4; ++r) base.push_back(placement.base[c][r]);
  return {{"name", objectName}, {"hidden", objectHidden}, {"base", base}, {"pivot", vec3Json(placement.pivot)},
          {"location", vec3Json(placement.location)}, {"rotation", vec3Json(placement.rotation)},
          {"scale", vec3Json(placement.scale)}};
}

void Document::objectFromJson(const json& j) {
  placement = Placement();
  objectName = j.value("name", std::string());
  objectHidden = j.value("hidden", false);
  if (j.contains("base") && j["base"].is_array() && j["base"].size() == 16) {
    for (int c = 0; c < 4; ++c)
      for (int r = 0; r < 4; ++r) placement.base[c][r] = j["base"][c * 4 + r].get<float>();
  }
  placement.pivot = vec3From(j.value("pivot", json()), vec3(0));
  placement.location = vec3From(j.value("location", json()), vec3(0));
  placement.rotation = vec3From(j.value("rotation", json()), vec3(0));
  placement.scale = vec3From(j.value("scale", json()), vec3(1));
}

// ------------------------------------------------------------------ the objects

void Engine::attachDocument(Document& d) {
  d.library = &materials;
  d.tex.materials = &materials;
  d.bakedLayersOn = d.tex.bakedLayersOn = features.on("baked_layers");
  d.blenderMaterials = features.on("bridge_materials");
}

int Engine::findObject(const std::string& s) const {
  for (int i = 0; i < objectCount(); ++i)
    if (object(i).displayName() == s) return i;
  auto lower = [](std::string x) {
    for (auto& c : x) c = (char)std::tolower((unsigned char)c);
    return x;
  };
  for (int i = 0; i < objectCount(); ++i)
    if (lower(object(i).displayName()) == lower(s)) return i;
  if (!s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; })) {
    int i = std::atoi(s.c_str());
    if (i >= 0 && i < objectCount()) return i;
  }
  return -1;
}

void Engine::objectChanged() {
  // what belonged to the object that was active: its retopology, parts, brush reach, compare snapshot...
  retopoData.faces = ~0u;  // (the state is made afresh, and loaded for this mesh, when next asked for)
  partsKey_ = 0;
  faceGraph_ = FaceGraph();
  edgeFacesKey_ = 0;
  if (compare.mode != 1) compare.enabled = false;
  compare.hasSnapshot = false;
  modelTextures = json();
  collisionLinesKey = 0;
  assistantTurns.clear();
  painter.hasCloneSource = false;
  materials.atlas = &doc.mesh.tiles;
}

bool Engine::activateObject(int i) {
  if (i < 0 || i >= objectCount()) return false;
  if (i == active_) return true;
  if (painter.active()) painter.endStroke();
  if (beforeMeshChange_) beforeMeshChange_();  // (a retopology build of the object going stops, its patches are kept)
  doc.checkpoint();
  std::string project = doc.projectPath;
  objects_[active_] = std::make_unique<Document>(std::move(doc));
  objects_[active_]->beforeMeshChange = nullptr;
  objects_[active_]->tex.sleep();  // (its composite's working textures, until it composites again)
  doc = std::move(*objects_[i]);
  objects_[i].reset();
  active_ = i;
  doc.projectPath = project;
  doc.beforeMeshChange = beforeMeshChange_;
  attachDocument(doc);
  objectChanged();
  if (onExternalChange) onExternalChange("object");
  return true;
}

int Engine::addObject(Mesh&& m, const std::string& path, const std::string& name, const Document::Placement& placement,
                      int resolution, bool withTextures) {
  auto d = std::make_unique<Document>();
  attachDocument(*d);
  d->meshPath = path;
  d->setMesh(std::move(m), resolution > 0 ? resolution : defaultResolution, false);
  d->meshFileTime = path.empty() ? 0 : Document::fileTime(path);
  d->objectName = name;
  d->placement = placement;
  if (withTextures && !path.empty() && features.on("model_textures")) {
    std::string why;
    json r;
    // (the model's own materials: the atlas the library samples them for is this object's)
    materials.atlas = &d->mesh.tiles;
    if (!d->importModelTextures(path, true, &r, &why, false) && !why.empty())
      PLOG_W("The model's textures: %s", why.c_str());
    materials.atlas = &doc.mesh.tiles;
  }
  d->clearHistory();
  d->dirty = true;
  d->projectPath = doc.projectPath;
  objects_.push_back(std::move(d));
  sceneChanged = true;
  return objectCount() - 1;
}

int Engine::addObjectsFromFile(const std::string& path, int resolution, bool oneObject, std::string* err, json* report) {
  if (painter.active()) painter.endStroke();
  std::vector<Mesh> parts;
  std::vector<std::string> names;
  {
    Mesh m;
    if (!patina::loadMesh(path, m, err)) return -1;
    if (!oneObject && m.objectNames.size() > 1) splitObjects(m, parts, names);
    if (parts.empty()) {
      names.push_back(m.objectNames.size() == 1 ? m.objectNames[0] : fs::path(path).stem().string());
      parts.push_back(std::move(m));
    }
  }
  // a texture set to match the model already open (as many texels per meter), at most the resolution asked for
  const bool startup = doc.untouchedStartup() && objectCount() == 1;
  double area0 = 0;
  if (!startup)
    for (uint32_t f = 0; f < doc.mesh.faceCount(); ++f) area0 += doc.mesh.faceArea3D(f);
  int baseRes = resolution > 0 ? resolution : (startup ? defaultResolution : doc.tileRes());
  int first = -1;
  json added = json::array();
  for (size_t k = 0; k < parts.size(); ++k) {
    Mesh& m = parts[k];
    int res = baseRes;
    if (resolution <= 0 && area0 > 0) {
      double a = 0;
      for (uint32_t f = 0; f < m.faceCount(); ++f) a += m.faceArea3D(f);
      double want = baseRes * std::sqrt(std::max(a, 1e-12) / area0);
      res = 512;
      while (res < baseRes && res < want * 0.9) res *= 2;
    }
    Document::Placement pl;  // (where the file puts it: it turns and scales about the middle of its model)
    pl.pivot = m.bounds.valid() ? m.bounds.center() : vec3(0);
    std::string nm = names[k];
    for (int n = 1; findObject(nm) >= 0 && n < 1000; ++n) {
      char buf[16];
      snprintf(buf, sizeof buf, ".%03d", n);
      nm = names[k] + buf;
    }
    uint32_t faces = m.faceCount();
    int idx = addObject(std::move(m), path, nm, pl, res, parts.size() == 1 || oneObject);
    if (first < 0) first = idx;
    added.push_back({{"index", idx}, {"name", nm}, {"faces", faces}, {"resolution", res}});
  }
  if (startup && first >= 0) {
    // the sphere Patina starts with goes: the first part takes its place
    int sphere = active_;
    activateObject(first);
    removeObject(sphere);
    first = active_;
    sceneChanged = false;
    frameMesh();
  }
  if (report) (*report)["objects"] = added;
  PLOG_I("Added %zu object%s from %s", added.size(), added.size() == 1 ? "" : "s", path.c_str());
  if (onExternalChange) onExternalChange("objects");
  return first;
}

void Engine::compositeObjects() {
  for (int i = 0; i < objectCount(); ++i) {
    if (i == active_ || !objects_[i]) continue;
    Document& d = *objects_[i];
    if (!d.compositeDirty && d.tex.bakeSerial() == d.syncedBakeSerial()) continue;
    materials.atlas = &d.mesh.tiles;  // (fills sample the library's materials for this object's texture tiles)
    d.updateComposite(materials);
    d.tex.sleep();
  }
  materials.atlas = &doc.mesh.tiles;
}

void Engine::markSceneComposite() {
  for (int i = 0; i < objectCount(); ++i) object(i).markComposite();
}

bool Engine::removeObject(int i) {
  if (i < 0 || i >= objectCount() || objectCount() < 2) return false;
  if (i == active_) activateObject(i + 1 < objectCount() ? i + 1 : i - 1);
  objects_.erase(objects_.begin() + i);
  if (i < active_) --active_;
  sceneChanged = true;
  if (onExternalChange) onExternalChange("objects");
  return true;
}

int Engine::duplicateObject(int i, std::string* err) {
  if (i < 0 || i >= objectCount()) return -1;
  Document& src = object(i);
  if (i == active_) doc.checkpoint();
  // through a project file of its own: every texture, layer and setting comes along
  fs::path tmp = paths::cacheDir() / "duplicate.patina";
  auto job = src.prepareSave(tmp.string(), err);
  if (!job || !job->ready(true) || !job->write(err)) return -1;
  auto d = std::make_unique<Document>();
  attachDocument(*d);
  bool ok = d->loadProject(tmp.string(), materials, err);
  std::error_code ec;
  fs::remove(tmp, ec);
  if (!ok) return -1;
  std::string base = src.displayName(), nm = base;
  for (int n = 1; findObject(nm) >= 0 && n < 1000; ++n) {
    char buf[16];
    snprintf(buf, sizeof buf, ".%03d", n);
    nm = base + buf;
  }
  d->objectName = nm;
  d->projectPath = doc.projectPath;
  d->bridge = BridgeState();  // (the copy isn't Blender's object)
  d->dirty = true;
  objects_.insert(objects_.begin() + i + 1, std::move(d));
  if (i + 1 <= active_) ++active_;
  sceneChanged = true;
  if (onExternalChange) onExternalChange("objects");
  return i + 1;
}

void Engine::clearScene() {
  objects_.clear();
  objects_.resize(1);
  active_ = 0;
  isolateActive = false;
  sceneChanged = false;
}

bool Engine::sceneDirty() const {
  if (sceneChanged) return true;
  for (int i = 0; i < objectCount(); ++i)
    if (object(i).dirty) return true;
  return false;
}

AABB Engine::sceneBounds(bool visibleOnly) const {
  AABB b;
  for (int i = 0; i < objectCount(); ++i) {
    const Document& d = object(i);
    if (i != active_ && (isolateActive || (visibleOnly && d.objectHidden))) continue;
    if (!d.hasMesh() || !d.mesh.bounds.valid()) continue;
    if (!d.placed()) {
      b.add(d.mesh.bounds.lo), b.add(d.mesh.bounds.hi);
      continue;
    }
    mat4 m = d.toWorld();
    for (int c = 0; c < 8; ++c) {
      vec3 p((c & 1) ? d.mesh.bounds.hi.x : d.mesh.bounds.lo.x, (c & 2) ? d.mesh.bounds.hi.y : d.mesh.bounds.lo.y,
             (c & 4) ? d.mesh.bounds.hi.z : d.mesh.bounds.lo.z);
      b.add(vec3(m * vec4(p, 1.f)));
    }
  }
  return b;
}

Engine::ScenePick Engine::pickScene(vec2 px, const Camera* cam) const {
  ScenePick best;
  const Camera& c = cam ? *cam : camera;
  vec3 o, d;
  c.inSpace(mat4(1)).ray(px, viewportSize, o, d);
  for (int i = 0; i < objectCount(); ++i) {
    const Document& ob = object(i);
    if (i != active_ && (isolateActive || ob.objectHidden)) continue;
    if (!ob.hasMesh() || ob.bvh.empty()) continue;
    vec3 lo = o, ld = d;
    float k = 1.f;
    if (ob.placed()) {
      mat4 inv = glm::inverse(ob.toWorld());
      lo = vec3(inv * vec4(o, 1.f));
      ld = mat3(inv) * d;
      k = glm::length(ld);
      if (k < 1e-20f) continue;
      ld /= k;
    }
    RayHit h = ob.bvh.raycast(ob.mesh, lo, ld, 1e30f, ob.hiddenMask());
    if (!h.hit) continue;
    float t = h.t / k;
    if (best.object < 0 || t < best.t) best = {i, h, t};
  }
  return best;
}

std::vector<std::pair<const GpuMesh*, mat4>> Engine::occluders() const {
  std::vector<std::pair<const GpuMesh*, mat4>> out;
  if (isolateActive || objectCount() < 2) return out;
  mat4 inv = glm::inverse(doc.toWorld());
  for (int i = 0; i < objectCount(); ++i) {
    if (i == active_) continue;
    const Document& ob = object(i);
    if (ob.objectHidden || !ob.hasMesh() || !ob.gpu.valid()) continue;
    out.push_back({&ob.gpu, inv * ob.toWorld()});
  }
  return out;
}

Painter::View Engine::paintView(const Camera& cam, vec2 viewport) const {
  Painter::View v{objectCamera(cam), viewport};
  v.occluders = occluders();
  return v;
}

json Engine::objectsJson() const {
  json a = json::array();
  for (int i = 0; i < objectCount(); ++i) {
    const Document& d = object(i);
    json o = {{"index", i},
              {"name", d.displayName()},
              {"active", i == active_},
              {"visible", !d.objectHidden},
              {"faces", d.mesh.faceCount()},
              {"layers", d.layers.size()},
              {"resolution", d.tileRes()},
              {"location", vec3Json(d.placement.location)},
              {"rotation", vec3Json(d.placement.rotation)},
              {"scale", vec3Json(d.placement.scale)},
              {"unsaved_changes", d.dirty}};
    if (!d.meshPath.empty()) o["path"] = d.meshPath;
    if (d.bridge.active) o["blender_object"] = d.bridge.object;
    AABB b;
    if (d.mesh.bounds.valid()) {
      mat4 m = d.toWorld();
      for (int c = 0; c < 8; ++c) {
        vec3 p((c & 1) ? d.mesh.bounds.hi.x : d.mesh.bounds.lo.x, (c & 2) ? d.mesh.bounds.hi.y : d.mesh.bounds.lo.y,
               (c & 4) ? d.mesh.bounds.hi.z : d.mesh.bounds.lo.z);
        b.add(vec3(m * vec4(p, 1.f)));
      }
      o["bounds"] = {{"min", vec3Json(b.lo)}, {"max", vec3Json(b.hi)}};
    }
    a.push_back(o);
  }
  return a;
}

// ------------------------------------------------------------------ saving

std::unique_ptr<Document::SaveJob> Engine::prepareSceneSave(const std::string& path, std::string* err) {
  if (objectCount() == 1) return doc.prepareSave(path, err);
  for (int i = 0; i < objectCount(); ++i)
    if (!object(i).hasMesh()) {
      if (err) *err = "Object " + std::to_string(i) + " has no mesh";
      return nullptr;
    }
  auto job = std::make_unique<Document::SaveJob>();
  job->path = path;
  json others = json::array();
  for (int i = 0; i < objectCount(); ++i) {
    std::string prefix = i == 0 ? std::string() : "O" + std::to_string(i) + "/";
    json h = object(i).appendToSave(*job, prefix);
    if (i == 0) {
      job->header = std::move(h);
    } else {
      h["block_prefix"] = prefix;
      others.push_back(std::move(h));
    }
  }
  // (the first object is the header itself: Patina before 1.2 opens it, and leaves the others)
  job->header["objects"] = std::move(others);
  job->header["scene"] = {{"version", 2}, {"active", active_}};
  job->fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
  return job;
}

bool Engine::saveScene(const std::string& path, std::string* err) {
  if (objectCount() == 1) {
    if (!doc.saveProject(path, err)) return false;
    sceneChanged = false;
    return true;
  }
  doc.checkpoint();
  auto job = prepareSceneSave(path, err);
  if (!job || !job->ready(true) || !job->write(err)) {
    if (job && err && err->empty()) *err = "the textures could not be read back";
    return false;
  }
  for (int i = 0; i < objectCount(); ++i) {
    Document& d = object(i);
    d.projectPath = path;
    d.dirty = false;
    if (d.bridge.active) bridgeprojects::remember(d.bridge.object, path);
  }
  sceneChanged = false;
  PLOG_I("Saved project %s (%d objects)", path.c_str(), objectCount());
  return true;
}

}  // namespace patina
