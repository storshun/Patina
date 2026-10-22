#include "engine/Engine.h"

#include "core/Progress.h"

#include <cstring>
#include <ctime>
#include <filesystem>

#include "core/Image.h"
#include "core/Log.h"
#include "core/Parallel.h"
#include "core/MeshIO.h"
#include "core/Paths.h"
#include "core/UVIsland.h"
#include "platform/Net.h"

namespace patina {

namespace fs = std::filesystem;

json CommandRegistry::describe(bool mcpOnly) const {
  json arr = json::array();
  for (auto& [n, c] : cmds_) {
    if (mcpOnly && !c.exposeToMcp) continue;
    arr.push_back({{"name", n}, {"description", c.description}, {"inputSchema", c.schema}});
  }
  return arr;
}

bool Engine::init(bool hl) {
  beforeMeshChange_ = [this] {
    // a build running is for the mesh going away: stopped, and nothing new started
    auto& S = retopoData;
    if (S.cancel) S.cancel->store(true);
    S.fullPending = false;
    if (S.building.valid()) S.building.wait(), (void)S.building.get();
    saveRetopo();
  };
  doc.beforeMeshChange = beforeMeshChange_;
  renderer.sceneObjects = [this](const std::function<void(const Document&)>& fn) {
    if (isolateActive) return;
    for (int i = 0; i < objectCount(); ++i)
      if (i != active_ && objects_[i] && !objects_[i]->objectHidden) fn(*objects_[i]);
  };
  headless = hl;
  materials.loadAll();
  brushes.loadAll();
  if (!env.initStudio()) PLOG_W("Environment init failed");
  painter.init();
  selectBrush(selectedBrushId);
  if (materials.find("patina_iron_raw")) selectMaterial("patina_iron_raw");
  else if (!materials.entries().empty()) selectMaterial(materials.entries().front()->def.id);
  registerCommands();
  registerAssistantCommands();
  registerRetopoCommands();
  registerBatchCommands();
  registerInterfaceCommands();
  registerSceneCommands();
  registerSelectCommands();
  doc.library = &materials;
  materials.atlas = &doc.mesh.tiles;
  doc.tex.materials = &materials;
  doc.setMesh(makeUVSphere(64, 32, 1.f), defaultResolution, false);
  doc.startupScene = true;
  frameMesh();
  return true;
}

void Engine::shutdown() {
  if (painter.active()) painter.endStroke();
  // an autosave still writing finishes first; its buffers go while the GL context is there
  if (autosaveTask.valid()) autosaveTask.get();
  autosaveJob.reset();
}

void Engine::selectBrush(const std::string& id) {
  if (const BrushPreset* p = brushes.find(id)) {
    painter.brush = *p;
    selectedBrushId = id;
  }
}

void Engine::selectMaterial(const std::string& id) {
  if (auto* e = materials.find(id)) {
    MaterialRef ref = painter.material;
    ref.id = e->def.id;  // canonical id (old unprefixed ids resolve to patina_*)
    painter.material = ref;
    selectedMaterialId = e->def.id;
  }
}

void Engine::frameMesh() {
  if (!doc.hasMesh()) return;
  if (objectCount() == 1 && !doc.placed()) camera.frame(doc.mesh.bounds, doc.mesh.boundingRadius);
  else camera.frame(sceneBounds());  // (1.2) the whole scene
}

void Engine::snapView(float yaw, float pitch, bool ortho, const Camera* before) {
  ViewMemory& M = viewMemory;
  checkViewSnap();
  if (!M.snapped) {
    // from a view of your own: that is the one to go back to
    M.back = before ? *before : camera;
    M.hasBack = true;
    M.backSnapped = M.backOrthoBySnap = false;
    M.orthoBySnap = false;
  }
  camera.yaw = yaw, camera.pitch = pitch;
  if (ortho && !camera.ortho) camera.ortho = true, M.orthoBySnap = true;
  M.snapped = true, M.yaw = yaw, M.pitch = pitch;
}

bool Engine::viewBack() {
  ViewMemory& M = viewMemory;
  checkViewSnap();
  if (!M.hasBack) return false;
  // swap: the view now becomes the one to go back to
  Camera now = camera;
  const bool nowSnapped = M.snapped, nowOrthoBySnap = M.orthoBySnap;
  camera = M.back;
  M.snapped = M.backSnapped, M.orthoBySnap = M.backOrthoBySnap;
  M.yaw = camera.yaw, M.pitch = camera.pitch;
  M.back = now;
  M.backSnapped = nowSnapped, M.backOrthoBySnap = nowOrthoBySnap;
  camera.updateClip(clipBounds());
  return true;
}

void Engine::checkViewSnap() {
  ViewMemory& M = viewMemory;
  if (!M.snapped || (std::abs(camera.yaw - M.yaw) < 1e-4f && std::abs(camera.pitch - M.pitch) < 1e-4f)) return;
  M.snapped = false;
  if (M.orthoBySnap) camera.ortho = false;
  M.orthoBySnap = false;
}

// ------------------------------------------------------------------ queue

std::future<json> Engine::post(const json& request) {
  auto p = std::make_shared<std::promise<json>>();
  auto f = p->get_future();
  {
    std::lock_guard<std::mutex> lk(qMutex_);
    queue_.push_back({request, p});
  }
  qCv_.notify_all();
  return f;
}

bool Engine::waitForWork(int timeoutMs) {
  std::unique_lock<std::mutex> lk(qMutex_);
  return qCv_.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&] { return !queue_.empty(); });
}

bool Engine::Macro::recordable(const std::string& cmd, const json& args) {
  // left out: questions that change nothing, the recorder itself, undo (undone steps drop out
  // on their own), and what ties a macro to one file or to Blender (opening, saving, the bridge)
  static const char* skip[] = {"ping", "features", "mesh_symmetry", "history", "history_jump", "undo", "redo",
                               "render_view", "render_uv", "assistant_grammar", "assistant_model", "assistant_panel", "screenshot_window", "pick", "pick_face", "uv_info",
                               "uv_islands", "go_to_note", "set_viewport_size", "open_file", "open_mesh",
                               "new_project", "save_project", "save_incremental", "reload_mesh", "send_to_blender"};
  if (cmd.rfind("get_", 0) == 0 || cmd.rfind("list_", 0) == 0 || cmd.rfind("macro_", 0) == 0 ||
      cmd.rfind("bridge_", 0) == 0 || cmd.rfind("assistant_", 0) == 0 || cmd == "mesh_parts" || cmd == "layer_region_faces")  // a plan records its steps
    return false;
  for (const char* s : skip)
    if (cmd == s) return false;
  if (cmd == "texture_tiles" && !args.contains("mode")) return false;  // a report
  if (cmd == "auto_texture" && !args.value("apply", true)) return false;
  return true;
}

json Engine::execute(const std::string& cmd, const json& args) {
  auto p = std::make_shared<std::promise<json>>();
  CommandContext ctx{*this, p};
  const Command* c = commands.find(cmd);
  if (!c) throw CommandError("unknown command '" + cmd + "'");
  if (!c->noCheckpoint) doc.checkpoint();
  uv::uvHashNewFrame();  // a command may change the UVs: cached fingerprints are worked out again
  json a = args.is_null() ? json::object() : args;
  uint64_t before = doc.undo.topSerial();
  json r = c->run(ctx, a);
  uv::uvHashNewFrame();
  if (!c->noCheckpoint && !painter.active()) doc.checkpoint();
  if (macro.recording && !macro.playing && Macro::recordable(cmd, a)) {
    uint64_t after = doc.undo.topSerial();
    macro.recorded.push_back({{{"cmd", cmd}, {"args", a}}, after != before ? after : 0});
  }
  return r;
}

size_t Engine::processQueue() {
  std::deque<Pending> work;
  {
    std::lock_guard<std::mutex> lk(qMutex_);
    work.swap(queue_);
  }
  for (auto& w : work) {
    json resp;
    std::string cmd = w.request.value("cmd", std::string());
    json args = w.request.value("args", json::object());
    const Command* c = commands.find(cmd);
    if (!c) {
      w.promise->set_value({{"ok", false}, {"error", "unknown command '" + cmd + "'"}});
      continue;
    }
    CommandContext ctx{*this, w.promise};
    try {
      doc.bakedLayersOn = doc.tex.bakedLayersOn = features.on("baked_layers");  // (a command may have switched it)
      if (!c->noCheckpoint) doc.checkpoint();
      uv::uvHashNewFrame();  // (0.44.2) as execute() does: headless, nothing else works the fingerprints out again
      uint64_t before = doc.undo.topSerial();
      json result = c->run(ctx, args.is_null() ? json::object() : args);
      uv::uvHashNewFrame();
      if (!c->noCheckpoint && !painter.active()) doc.checkpoint();
      if (macro.recording && !macro.playing && Macro::recordable(cmd, args.is_null() ? json::object() : args)) {
        uint64_t after = doc.undo.topSerial();
        macro.recorded.push_back({{{"cmd", cmd}, {"args", args.is_null() ? json::object() : args}}, after != before ? after : 0});
      }
      if (ctx.deferred) continue;  // the command fulfills the promise later
      w.promise->set_value({{"ok", true}, {"result", result}});
    } catch (std::exception& e) {
      PLOG_W("Command %s failed: %s", cmd.c_str(), e.what());
      if (!ctx.deferred) w.promise->set_value({{"ok", false}, {"error", e.what()}});
    }
    // keep paint/composite current between scripted commands
    painter.update();
    doc.updateComposite(materials);
    compositeObjects();
  }
  return work.size();
}

void Engine::tick() {
  doc.bakedLayersOn = doc.tex.bakedLayersOn = features.on("baked_layers");
  processQueue();
  painter.update();
  retopoTick();
  // a finished edit of layer settings becomes one undo step (a slider drag = one step)
  if (!uiInteracting && !painter.active()) doc.checkpoint();
  doc.updateComposite(materials);
  compositeObjects();
  // live link: push when the result changed, at most every 2 s, never mid-stroke
  if (autoPushToBlender && doc.bridge.active && !painter.active() && doc.tex.compositeCount != lastPushedComposite &&
      nowSeconds() - lastPushTime > 2.0) {
    lastPushTime = nowSeconds();
    lastPushedComposite = doc.tex.compositeCount;
    pushToBlender();
  }
  // autosave (GUI only) so a crash never costs more than a few minutes of work
  // only while idle (no stroke in the last 8 s) so the save never interrupts painting
  // Autosave without stopping the window (it used to stop for several seconds with a 4K
  // texture set, every few minutes): the GPU copies the textures into buffers, then another
  // thread compresses and writes them; the buffers are freed here once it has.
  if (autosaveJob) {
    if (!autosaveTask.valid()) {
      if (autosaveJob->ready(false)) {
        Document::SaveJob* job = autosaveJob.get();
        double t0 = lastAutosave;
        autosaveTask = std::async(std::launch::async, [job, t0] {
          std::string e;
          bool ok = job->write(&e);
          if (ok) PLOG_I("Autosaved to %s (%.1f s)", job->path.c_str(), nowSeconds() - t0);
          else PLOG_W("Autosave failed: %s", e.c_str());
          return ok;
        });
      }
    } else if (autosaveTask.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
      autosaveTask.get();
      autosaveJob.reset();
    }
  } else if (autosaveMinutes > 0 && sceneDirty() && !painter.active() && doc.hasMesh() &&
             nowSeconds() - lastAutosave > autosaveMinutes * 60.0 && nowSeconds() - painter.lastStrokeEnd > 8.0) {
    lastAutosave = nowSeconds();
    std::string err;
    autosaveJob = prepareSceneSave((paths::cacheDir() / "autosave.patina").string(), &err);
    if (!autosaveJob) PLOG_W("Autosave failed: %s", err.c_str());
    else PLOG_I("Autosave: reading the textures back took %.2f s", nowSeconds() - lastAutosave);
  }
}

// ------------------------------------------------------------------ helpers

bool Engine::openMesh(const std::string& path, int resolution, bool keepLayers, std::string* err, int separate) {
  if (painter.active()) painter.endStroke();
  std::vector<Mesh> parts;
  std::vector<std::string> names;
  if (!keepLayers) {
    // (1.2) a file with several meshes (a body, its clothes, eyes, hair): an object each, with layers of its own
    Mesh m;
    if (!patina::loadMesh(path, m, err)) return false;
    size_t n = m.objectNames.size();
    if (features.on("scene_objects") && separate != 0 && n >= 2 && (separate > 0 || n <= kMaxObjectsOnOpen))
      splitObjects(m, parts, names);
    if (parts.size() < 2) parts.clear(), names.clear(), parts.push_back(std::move(m));
    if (beforeMeshChange_) beforeMeshChange_();
    doc.meshPath = path;
    doc.setMesh(std::move(parts[0]), resolution > 0 ? resolution : defaultResolution, false);
    doc.meshFileTime = Document::fileTime(path);
  } else if (!doc.loadMesh(path, resolution > 0 ? resolution : defaultResolution, keepLayers, err)) {
    return false;
  }
  if (!keepLayers) newScene();  // (1.2) another model: a scene of its own
  if (!names.empty()) {
    doc.objectName = names[0];
    double area0 = 0;
    for (uint32_t f = 0; f < doc.mesh.faceCount(); ++f) area0 += doc.mesh.faceArea3D(f);
    for (size_t k = 1; k < parts.size(); ++k) {
      // each object's texture set as many texels per meter as the first one's, at most its size
      int base = resolution > 0 ? resolution : defaultResolution, res = base;
      double a = 0;
      for (uint32_t f = 0; f < parts[k].faceCount(); ++f) a += parts[k].faceArea3D(f);
      if (area0 > 0) {
        double want = base * std::sqrt(std::max(a, 1e-12) / area0);
        res = 512;
        while (res < base && res < want * 0.9) res *= 2;
      }
      Document::Placement pl;
      pl.pivot = parts[k].bounds.valid() ? parts[k].bounds.center() : vec3(0);
      std::string nm = names[k];
      for (int i = 1; findObject(nm) >= 0 && i < 1000; ++i) {
        char buf[16];
        snprintf(buf, sizeof buf, ".%03d", i);
        nm = names[k] + buf;
      }
      addObject(std::move(parts[k]), path, nm, pl, res);
    }
    doc.placement.pivot = doc.mesh.bounds.valid() ? doc.mesh.bounds.center() : vec3(0);
    sceneChanged = false;
    PLOG_I("Opened %s as %d objects", path.c_str(), objectCount());
  }
  if (!keepLayers) assistantTurns.clear();  // another model: earlier requests are about something else
  modelTextures = json();
  if (!keepLayers && features.on("model_textures")) {
    std::string why;
    json r;
    doc.blenderMaterials = features.on("bridge_materials");
    if (doc.importModelTextures(path, true, &r, &why, resolution <= 0)) modelTextures = r, modelTextures["path"] = path;
    else if (!why.empty()) PLOG_W("The model's textures: %s", why.c_str());
  }
  frameMesh();
  if (onExternalChange) onExternalChange("mesh");
  return true;
}

bool Engine::bakeDown(Mesh&& low, const std::string& lowPath, Document::BakeDownOptions o, json* report,
                      std::string* err) {
  if (painter.active()) painter.endStroke();
  std::error_code ec;
  bool copyHigh = o.highPath.empty() && (doc.meshPath.empty() || !fs::exists(doc.meshPath, ec) || doc.bridge.active);
  if (o.highPath.empty() && !copyHigh) o.highPath = doc.meshPath;
  std::string name = doc.mesh.name.empty() ? fs::path(doc.meshPath).stem().string() : doc.mesh.name;
  o.lowPath = lowPath;
  if (!doc.bakeDown(std::move(low), o, report, err)) return false;
  if (copyHigh && doc.highPoly) {
    fs::path p = paths::ensureDir(paths::userDataDir() / "bakedown") /
                 (paths::sanitizeFileName(name.empty() ? "model" : name) + "_high.ptmesh");
    for (int k = 2; fs::exists(p, ec) && k < 1000; ++k)
      p = p.parent_path() / (paths::sanitizeFileName(name.empty() ? "model" : name) + "_high_" + std::to_string(k) + ".ptmesh");
    std::string why;
    if (savePtMesh(p.string(), *doc.highPoly, &why)) doc.highPolyPath = p.string();
    else PLOG_W("Bake-down: could not keep a copy of the high-poly model (%s): bakes made again later need it", why.c_str());
    if (report) (*report)["high_poly"] = doc.highPolyPath;
  } else if (report) {
    (*report)["high_poly"] = doc.highPolyPath;
  }
  modelTextures = json();
  frameMesh();
  if (onExternalChange) onExternalChange("mesh");
  return true;
}

void Engine::collisionViewOptions(ViewportOptions& o) {
  if (!showCollision) return;
  const CollisionResult* c = doc.collisionShapes();
  if (!c) return;
  if (collisionLinesKey != doc.collisionDrawKey() || collisionLines.empty()) {
    collisionLines = collisionEdges(c->shapes);
    collisionLinesKey = doc.collisionDrawKey() ^ c->shapes.size() * 0x9E3779B97F4A7C15ull;
  }
  o.collisionLines = &collisionLines;
  o.collisionKey = collisionLinesKey;
}

std::vector<uint32_t> Engine::visibleFaces(const std::vector<uint32_t>& faces) const {
  const Camera cam = objectCamera();  // (1.2) in the active object's space
  std::vector<uint32_t> out;
  const Mesh& m = doc.mesh;
  if (!doc.hasMesh() || faces.empty() || doc.bvh.empty()) return out;
  float aspect = viewportSize.x / std::max(viewportSize.y, 1.f);
  mat4 vpm = cam.viewProj(aspect);
  vec3 eye = cam.eye(), fwd = cam.forward();
  float tol = 1e-3f * glm::length(m.bounds.hi - m.bounds.lo);
  std::vector<uint8_t> seen(faces.size(), 0);
  parallelFor(faces.size(), [&](size_t i) {
    uint32_t f = faces[i];
    if (f >= m.faceCount()) return;
    vec3 c = m.faceCenter(f);
    vec3 toEye = cam.ortho ? -fwd : eye - c;
    if (glm::dot(m.faceNormal(f), toEye) <= 0.f) return;  // turned away
    // the center, then the corners pulled a third of the way in (a face half behind a silhouette still counts)
    vec3 pts[9];
    int n = 0;
    pts[n++] = c;
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1] && n < 9; ++l)
      pts[n++] = glm::mix(m.positions[m.loopVert[l]], c, 0.33f);
    for (int k = 0; k < n; ++k) {
      vec4 clip = vpm * vec4(pts[k], 1.f);
      if (clip.w <= 1e-6f) continue;
      vec2 ndc = vec2(clip) / clip.w;
      if (std::abs(ndc.x) > 1.f || std::abs(ndc.y) > 1.f) continue;  // off screen
      vec2 sp((ndc.x * 0.5f + 0.5f) * viewportSize.x, (0.5f - ndc.y * 0.5f) * viewportSize.y);
      vec3 o, d;
      cam.ray(sp, viewportSize, o, d);
      RayHit h = doc.bvh.raycast(m, o, d, 1e30f, doc.hiddenMask());
      if (h.hit && (h.face == f || glm::length(h.position - pts[k]) < tol)) {
        seen[i] = 1;
        return;
      }
    }
  });
  for (size_t i = 0; i < faces.size(); ++i)
    if (seen[i]) out.push_back(faces[i]);
  return out;
}

bool Engine::openFile(const std::string& path, std::string* err) {
  std::string ext = fs::path(path).extension().string();
  for (auto& c : ext) c = (char)tolower(c);
  if (std::string why = MaterialLibrary::unsupportedMaterialReason(path); !why.empty()) {
    if (err) *err = why;
    return false;
  }
  if (ext == ".patina") {
    if (painter.active()) painter.endStroke();
    container::File f;
    if (!container::readFile(path, "PATINA01", f, err)) return false;
    if (beforeMeshChange_) beforeMeshChange_();
    if (!doc.loadFrom(f, f.header, "", path, materials, err)) return false;
    clearScene();
    // (1.2) the scene's other objects
    int activeWas = 0;
    if (f.header.contains("objects") && f.header["objects"].is_array()) {
      for (auto& oh : f.header["objects"]) {
        auto d = std::make_unique<Document>();
        attachDocument(*d);
        std::string why;
        if (!d->loadFrom(f, oh, oh.value("block_prefix", std::string()), path, materials, &why)) {
          PLOG_W("Project %s: an object could not be opened (%s)", path.c_str(), why.c_str());
          continue;
        }
        objects_.push_back(std::move(d));
      }
      activeWas = f.header.value("scene", json::object()).value("active", 0);
    }
    objectChanged();
    if (activeWas > 0 && activeWas < objectCount()) activateObject(activeWas);
    sceneChanged = false;
    assistantTurns.clear();
    frameMesh();
    if (onExternalChange) onExternalChange("project");
    return true;
  }
  return openMesh(path, 0, false, err);
}

void Engine::newScene() {
  clearScene();
  doc.placement = Document::Placement();
  doc.placement.pivot = doc.mesh.bounds.valid() ? doc.mesh.bounds.center() : vec3(0);  // (it turns about its middle)
  doc.objectName.clear();
  doc.objectHidden = false;
  objectChanged();
}

RayHit Engine::pick(vec2 px) const {
  vec3 o, d;
  Camera cam = objectCamera();
  cam.ray(px, viewportSize, o, d);
  RayHit h = doc.bvh.raycast(doc.mesh, o, d, 1e30f, doc.hiddenMask());
  if (h.hit && objectCount() > 1 && !isolateActive) {
    // (1.2) another object in front of it: the active one isn't seen there
    ScenePick sp = pickScene(px);
    if (sp.object >= 0 && sp.object != active_) {
      float k = glm::length(mat3(doc.toWorld()) * d);
      if (sp.t < h.t * k * 0.9999f) h.hit = false;
    }
  }
  return h;
}

std::vector<uint32_t> Engine::facesUnderBrush(vec2 px, float rpx, BrushReach reach, const std::function<bool(uint32_t)>& skip) {
  const Camera cam = objectCamera();  // (1.2) in the active object's space
  std::vector<uint32_t> out;
  RayHit c = pick(px);
  if (!c.hit) return out;
  const Mesh& m = doc.mesh;
  float pw = cam.pixelWorld(viewportSize.y);
  float worldR = rpx * (cam.ortho ? pw : pw * c.t);
  // the first thing seen along the ray through a face's center: that face, or (the ray grazing the
  // faces round it) a point of the surface right by its center
  auto seen = [&](uint32_t f, const vec3& ctr, vec2 sp) {
    vec3 o, d;
    cam.ray(sp, viewportSize, o, d);
    RayHit h = doc.bvh.raycast(m, o, d, 1e30f, doc.hiddenMask());
    if (!h.hit) return 0;
    if (h.face == f) return 2;
    return glm::length(h.position - ctr) < worldR * 0.25f ? 1 : 0;
  };
  auto facing = [&](uint32_t f, const vec3& ctr) {
    vec3 toEye = cam.ortho ? -cam.forward() : cam.eye() - ctr;
    return glm::dot(m.faceNormal(f), toEye) > 0.f;
  };
  float aspect = viewportSize.x / std::max(viewportSize.y, 1.f);
  mat4 vpm = cam.viewProj(aspect);
  auto screenPos = [&](const vec3& p, vec2& sp) {
    vec4 clip = vpm * vec4(p, 1.f);
    if (clip.w <= 1e-6f) return false;
    vec2 ndc = vec2(clip) / clip.w;
    sp = vec2((ndc.x * 0.5f + 0.5f) * viewportSize.x, (0.5f - ndc.y * 0.5f) * viewportSize.y);
    return true;
  };
  std::vector<uint32_t> faces;
  if (reach == BrushReach::Surface) {
    if (!faceGraph_.matches(m)) faceGraph_.build(m);
    // faces turned away pass the brush on only where they are what the camera sees (a face wound the other way)
    faces = facesAlongSurface(m, faceGraph_, c.face, c.position, worldR, [&](uint32_t f) {
      vec3 ctr = m.faceCenter(f);
      if (facing(f, ctr)) return true;
      vec2 sp;
      return screenPos(ctr, sp) && seen(f, ctr, sp) == 2;
    });
  } else {
    std::vector<uint32_t> tris;
    doc.bvh.querySphere(c.position, worldR * 1.5f, tris);
    for (uint32_t t : tris)
      if (t < m.triFace.size()) faces.push_back(m.triFace[t]);
    std::sort(faces.begin(), faces.end());
    faces.erase(std::unique(faces.begin(), faces.end()), faces.end());
    std::vector<uint32_t> kept;
    for (uint32_t f : faces) {
      if (f == c.face) {
        kept.push_back(f);
        continue;
      }
      vec3 ctr = m.faceCenter(f);
      vec2 sp;
      if (!screenPos(ctr, sp) || glm::length(sp - px) > rpx) continue;
      int s = seen(f, ctr, sp);
      // (0.43) a face just behind the one seen (the inner layer of a cloth with thickness) is turned away
      if (s == 2 || (s == 1 && facing(f, ctr))) kept.push_back(f);
    }
    faces = std::move(kept);
  }
  int axes = (painter.symmetry[0] ? 1 : 0) | (painter.symmetry[1] ? 2 : 0) | (painter.symmetry[2] ? 4 : 0);
  for (uint32_t f : faces) {
    if (!axes && skip && skip(f)) continue;
    out.push_back(f);
    // mirror symmetry: the mirrored faces too, matched by their corners, so halves that are
    // triangulated differently still get the whole mirrored region
    for (int sub = 1; sub < 8; ++sub) {
      if ((sub & axes) != sub) continue;
      for (uint32_t g : uv::mirrorFaces(m, doc.bvh, f, sub)) out.push_back(g);
    }
  }
  return out;
}

bool Engine::pickAt(vec2 px, bool apply, json* out) {
  RayHit h = pick(px);
  if (!h.hit) return false;
  doc.updateComposite(materials);
  int R = doc.tex.resolution();
  int x = std::clamp(int(h.uv.x * R), 0, R - 1), y = std::clamp(int(h.uv.y * R), 0, R - 1);
  float c[4], r[4], e[4];
  glGetTextureSubImage(doc.tex.finColor.id(), 0, x, y, 0, 1, 1, 1, GL_RGBA, GL_FLOAT, sizeof c, c);
  glGetTextureSubImage(doc.tex.finRMH.id(), 0, x, y, 0, 1, 1, 1, GL_RGBA, GL_FLOAT, sizeof r, r);
  glGetTextureSubImage(doc.tex.finEmissive.id(), 0, x, y, 0, 1, 1, 1, GL_RGBA, GL_FLOAT, sizeof e, e);
  FlatValues f;
  f.color = {c[0], c[1], c[2]};
  f.roughness = r[0];
  f.metallic = r[1];
  f.height = r[2];
  f.emissive = {e[0], e[1], e[2]};
  if (apply) {
    painter.material.id.clear();
    painter.material.flat = f;
    selectedMaterialId.clear();
  }
  if (out)
    *out = {{"uv", {h.meshUV.x, h.meshUV.y}},
            {"face", h.face},
            {"position", {h.position.x, h.position.y, h.position.z}},
            {"color", {f.color.x, f.color.y, f.color.z}},
            {"roughness", f.roughness},
            {"metallic", f.metallic},
            {"height", f.height}};
  // (0.41) a baked map layer selected: the map's value there becomes the brush's
  if (const Layer* sel = doc.selectedLayer(); sel && sel->isBaked()) {
    float v[4] = {0, 0, 0, 0};
    glGetTextureSubImage(doc.tex.bakedView(sel->bakedMap).id(), 0, x, y, 0, 1, 1, 1, GL_RGBA, GL_FLOAT, sizeof v, v);
    if (apply) painter.maskValue = v[0];
    if (out) (*out)["map_value"] = v[0];
  }
  return true;
}

std::vector<Engine::LayerHere> Engine::layersAt(vec2 uv) {
  std::vector<LayerHere> out;
  if (!doc.hasMesh()) return out;
  for (int i = (int)doc.layers.size() - 1; i >= 0; --i) {
    const Layer& l = *doc.layers[i];
    if (l.isFolder() || l.isBaked()) continue;
    const Layer& content = *doc.contentLayer(l);
    float cover = 1.f;
    if (content.type == LayerType::Paint) {
      cover = 0.f;
      for (int c = 0; c < kChannelCount; ++c) {
        if (!content.hasChannel(c) || !(l.channelMask & (1 << c))) continue;
        const Texture2D& t = *content.channels[c];
        int tx = std::clamp(int(uv.x * t.width()), 0, t.width() - 1), ty = std::clamp(int(uv.y * t.height()), 0, t.height() - 1);
        float v[4] = {0, 0, 0, 0};
        glGetTextureSubImage(t.id(), 0, tx, ty, 0, 1, 1, 1, GL_RGBA, GL_FLOAT, sizeof v, v);
        // (the coverage is the last component: alpha, or green for the two-component channels)
        GLenum f = channelInfo((Channel)c).layerFormat;
        cover = std::max(cover, f == GL_RG8 || f == GL_RG16F || f == GL_RG32F ? v[1] : v[3]);
      }
    }
    float mask = 1.f;
    if (cover > 0.f && (l.mask || l.generator.type != MaskGenerator::None || !l.maskEffects.empty())) {
      doc.ensureBakedFor(l);
      const Texture2D& m = doc.tex.layerMaskTexture(l, doc.mesh.bounds);
      int mx = std::clamp(int(uv.x * m.width()), 0, m.width() - 1), my = std::clamp(int(uv.y * m.height()), 0, m.height() - 1);
      float v[4] = {0, 0, 0, 0};
      glGetTextureSubImage(m.id(), 0, mx, my, 0, 1, 1, 1, GL_RGBA, GL_FLOAT, sizeof v, v);
      mask = v[0];
    }
    float amount = cover * mask * l.opacity;
    if (amount < 0.004f) continue;
    bool shown = l.visible;
    for (uint32_t p = doc.effectiveParent(l); p && shown; ) {
      int k = doc.indexOf(p);
      if (k < 0) break;
      shown = doc.layers[k]->visible;
      p = doc.effectiveParent(*doc.layers[k]);
    }
    out.push_back({i, std::min(amount, 1.f), shown});
  }
  return out;
}

std::vector<uint8_t> Engine::renderViewportPng(int w, int h, const Camera* cam, const ViewportOptions* opt) {
  doc.updateComposite(materials);
  GLuint maskTex = maskViewTexture((opt ? opt->mode : viewOptions.mode) == ViewMode::LayerMask);
  ViewportOptions o = opt ? *opt : viewOptions;
  if (o.mode == ViewMode::PathTraced && o.pathSamplesNow <= 0) o.pathSamplesNow = 32;  // (an image now, not the view's)
  retopoViewOptions(o);
  collisionViewOptions(o);
  renderer.renderViewport(doc, env, cam ? *cam : camera, offscreen_, w, h, o, maskTex);
  auto px = Renderer::readPixels(offscreen_);
  if (compareReady()) {
    // the left part from the compare side, and a thin divider
    renderCompare(cam ? *cam : camera, compareOffscreen_, w, h, opt ? *opt : viewOptions, maskTex);
    auto cpx = Renderer::readPixels(compareOffscreen_);
    int sx = std::clamp(int(compare.split * w), 0, w);
    for (int y = 0; y < h; ++y) {
      std::memcpy(&px[size_t(y) * w * 4], &cpx[size_t(y) * w * 4], size_t(sx) * 4);
      for (int x = std::max(0, sx - 1); x < std::min(w, sx + 1); ++x)
        for (int c = 0; c < 3; ++c) px[(size_t(y) * w + x) * 4 + c] = 235;
    }
  }
  return encodePngRGBA8(px.data(), w, h, 4);
}

GLuint Engine::maskViewTexture(bool effective) {
  Layer* sel = doc.selectedLayer();
  if (!sel || !doc.hasMesh()) return 0;
  if (!sel->mask && sel->generator.type == MaskGenerator::None && sel->maskEffects.empty()) return 0;
  if (!effective) return sel->mask ? sel->mask->id() : 0;
  doc.ensureBakedFor(*sel);
  return doc.tex.layerMaskTexture(*sel, doc.mesh.bounds).id();
}

void Engine::takeCompareSnapshot() {
  doc.updateComposite(materials);
  TextureSet& t = doc.tex;
  auto snap = [](Texture2D& dst, const Texture2D& src) {
    if (!src.valid()) return;
    if (!dst.valid() || dst.size() != src.size() || dst.format() != src.format() || dst.levels() != src.levels())
      dst.create(src.width(), src.height(), src.format(), src.levels());
    dst.copyFrom(src);
    dst.generateMips();
    dst.setWrap(GL_REPEAT, GL_REPEAT);
    if (GLAD_GL_EXT_texture_filter_anisotropic) glTextureParameterf(dst.id(), GL_TEXTURE_MAX_ANISOTROPY_EXT, 8.f);
  };
  snap(compare.color, t.finColor);
  snap(compare.rmh, t.finRMH);
  snap(compare.normal, t.finNormal);
  snap(compare.emissive, t.finEmissive);
  snap(compare.extra, t.finExtra);
  compare.extraUsed = t.extraUsed;
  compare.hasSnapshot = true;
  compare.takenAt = nowSeconds();
  std::time_t now = std::time(nullptr);
  char buf[32];
  std::strftime(buf, sizeof buf, "%H:%M:%S", std::localtime(&now));
  compare.label = std::string("Snapshot ") + buf;
}

void Engine::renderCompare(const Camera& cam, RenderTarget& rt, int w, int h, const ViewportOptions& opt, GLuint maskTex) {
  TextureSet& t = doc.tex;
  if (compare.mode == 2 && doc.bakeDownBefore) {
    // (1.1) the high-poly model as it looked before the bake-down: its mesh with its final maps, lit the same way
    auto& b = *doc.bakeDownBefore;
    ViewportOptions o = opt;
    o.meshOverride = &b.gpu;
    o.collisionLines = nullptr;
    o.retopoQuads = nullptr, o.retopoLines = nullptr, o.retopoLinesDone = nullptr, o.retopoStrokes = nullptr;
    o.retopoMarks = nullptr, o.retopoView = 0;
    o.selectionTint = 0.f;
    switch (o.mode) {  // the views that read the model's faces or bakes show the material
      case ViewMode::Material: case ViewMode::BaseColor: case ViewMode::Roughness: case ViewMode::Metallic:
      case ViewMode::Normal: case ViewMode::Height: case ViewMode::Emissive: case ViewMode::Opacity:
      case ViewMode::Sheen: case ViewMode::Coat: case ViewMode::Subsurface: case ViewMode::Solid: break;
      default: o.mode = ViewMode::Material;
    }
    o.useAO = false;
    std::swap(t.finColor, b.color);
    std::swap(t.finRMH, b.rmh);
    std::swap(t.finNormal, b.normal);
    std::swap(t.finEmissive, b.emissive);
    std::swap(t.finExtra, b.extra);
    std::swap(t.extraUsed, b.extraUsed);
    renderer.renderViewport(doc, env, cam, rt, w, h, o, maskTex);
    std::swap(t.finColor, b.color);
    std::swap(t.finRMH, b.rmh);
    std::swap(t.finNormal, b.normal);
    std::swap(t.finEmissive, b.emissive);
    std::swap(t.finExtra, b.extra);
    std::swap(t.extraUsed, b.extraUsed);
    return;
  }
  if (compare.mode == 0) {
    bool same = compare.hasSnapshot && compare.color.size() == t.finColor.size();
    if (!same) {
      compare.hasSnapshot = false;  // the texture size changed: the snapshot no longer fits
      renderer.renderViewport(doc, env, cam, rt, w, h, opt, maskTex);
      return;
    }
    std::swap(t.finColor, compare.color);
    std::swap(t.finRMH, compare.rmh);
    std::swap(t.finNormal, compare.normal);
    std::swap(t.finEmissive, compare.emissive);
    std::swap(t.finExtra, compare.extra);
    std::swap(t.extraUsed, compare.extraUsed);
    renderer.renderViewport(doc, env, cam, rt, w, h, opt, maskTex);
    std::swap(t.finColor, compare.color);
    std::swap(t.finRMH, compare.rmh);
    std::swap(t.finNormal, compare.normal);
    std::swap(t.finEmissive, compare.emissive);
    std::swap(t.finExtra, compare.extra);
    std::swap(t.extraUsed, compare.extraUsed);
  } else {
    ViewportOptions o = opt;
    o.mode = compare.view;
    renderer.renderViewport(doc, env, cam, rt, w, h, o, maskTex);
  }
}

json Engine::compareState() const {
  return {{"enabled", compare.enabled},
          {"mode", compare.mode == 0 ? "snapshot" : compare.mode == 2 ? "high_poly" : "view"},
          {"view", viewModeName(compare.view)},
          {"split", compare.split},
          {"has_snapshot", compare.hasSnapshot},
          {"label", compare.label}};
}

std::vector<uint8_t> Engine::renderUVPng(int w, int h, GLuint tex, int texMode) {
  doc.updateComposite(materials);
  UVViewOptions o = uvOptions;
  float aspect = float(w) / float(h);
  o.scale = vec2(0.92f / aspect, 0.92f);
  o.offset = vec2(0.5f) - o.scale * 0.5f;
  o.texMode = texMode;
  renderer.renderUV(doc, uvOffscreen_, w, h, o, tex);
  auto px = Renderer::readPixels(uvOffscreen_);
  return encodePngRGBA8(px.data(), w, h, 4);
}

json Engine::scriptedStroke(const std::vector<StrokePoint>& pts, bool uv) {
  if (pts.empty()) throw CommandError("stroke needs at least one point");
  if (!doc.hasMesh()) throw CommandError("no mesh loaded");
  Painter::View v = paintView(camera, viewportSize);
  uint64_t before = painter.dabCount();
  if (!painter.beginStroke(doc, materials, brushes, v, pts[0], uv, 1.f / std::max(1, doc.tex.resolution())))
    throw CommandError("could not start stroke (no paintable target / no channels enabled)");
  for (size_t i = 1; i < pts.size(); ++i) painter.addPoint(pts[i]);
  painter.endStroke();
  doc.updateComposite(materials);
  return {{"dabs", painter.dabCount() - before}};
}

json Engine::state() {
  json s;
  s["app"] = "patina";
  s["version"] = PATINA_VERSION;
  s["headless"] = headless;
  s["server_port"] = serverPort;
  if (doc.hasMesh()) {
    s["mesh"] = {{"name", doc.mesh.name},
                 {"path", doc.meshPath},
                 {"vertices", doc.mesh.positions.size()},
                 {"faces", doc.mesh.faceCount()},
                 {"triangles", doc.mesh.tris.size()},
                 {"uv_islands", doc.mesh.islandCount},
                 {"bounds_min", {doc.mesh.bounds.lo.x, doc.mesh.bounds.lo.y, doc.mesh.bounds.lo.z}},
                 {"bounds_max", {doc.mesh.bounds.hi.x, doc.mesh.bounds.hi.y, doc.mesh.bounds.hi.z}}};
    s["mesh"]["lightmap_uvs"] = doc.mesh.loopUV2.size() == doc.mesh.loopCount() && !doc.mesh.loopUV2.empty();
    s["mesh"]["vertex_colors"] = doc.mesh.loopColor.size() == doc.mesh.loopCount() && !doc.mesh.loopColor.empty();
    s["mesh"]["vertex_colours"] = s["mesh"]["vertex_colors"];  // the 0.32 to 0.35 name
    if (modelTextures.is_object() && modelTextures.value("path", "") == doc.meshPath)
      s["mesh"]["model_textures"] = modelTextures;
  } else {
    s["mesh"] = nullptr;
  }
  s["resolution"] = doc.tileRes();
  s["atlas_resolution"] = doc.tex.resolution();
  s["texture_tiles"] = doc.mesh.tiles.active() ? doc.mesh.tiles.count() : 0;
  s["last_window"] = lastOpenedWindow;
  s["layers"] = doc.layerStackJson();
  s["selected_layer"] = doc.selected;
  s["edit_mask"] = doc.editMask;
  s["project"] = doc.projectPath;
  s["unsaved_changes"] = sceneDirty();
  s["brush"] = painter.brush.toJson();
  s["brush"]["preset"] = selectedBrushId;
  s["brush"]["lock_topology"] = painter.lockTopology;
  s["brush"]["lock_island"] = painter.lockIsland;
  s["brush"]["lock_set"] = painter.lockSet;
  s["material"] = painter.material.toJson();
  static const char* tools[] = {"paint", "erase", "fill", "picker", "blur", "clone", "smudge"};
  s["tool"] = tools[(int)painter.tool];
  if (!windowTool.empty()) s["window_tool"] = windowTool;
  s["stencil"] = painter.stencil.toJson();
  s["compare"] = compareState();
  s["paint_channels"] = json::array();
  for (int c = 0; c < kChannelCount; ++c)
    if (painter.channelMask & (1 << c)) s["paint_channels"].push_back(channelInfo((Channel)c).key);
  s["symmetry"] = {painter.symmetry[0], painter.symmetry[1], painter.symmetry[2]};
  s["solo"] = doc.soloUid ? doc.indexOf(doc.soloUid) : -1;
  s["radial_symmetry"] = {{"count", painter.radialCount}, {"axis", std::string(1, "xyz"[painter.radialAxis])}};
  s["camera"] = camera.toJson();
  s["objects"] = objectsJson();  // (1.2) the scene
  s["active_object"] = active_;
  s["viewport"] = {viewportSize.x, viewportSize.y};
  s["view_mode"] = viewModeName(viewOptions.mode);
  s["undo"] = {{"can_undo", doc.undo.canUndo()}, {"next", doc.undo.undoLabel()}, {"can_redo", doc.undo.canRedo()}};
  s["baked"] = {{"ao", doc.tex.hasAO()}, {"curvature", doc.tex.hasCurvature()}, {"thickness", doc.tex.hasThickness()},
                {"bent_normals", doc.tex.hasBentNormals()}, {"curvature_detail", doc.tex.curvatureHasDetail()}};
  s["bridge"] = {{"active", doc.bridge.active},
                 {"object", doc.bridge.object},
                 {"blender_port", doc.bridge.blenderPort},
                 {"uvs_edited", doc.bridge.uvsEdited},
                 {"texture_sets_edited", doc.bridge.slotsEdited},
                 {"auto_push", autoPushToBlender}};
  return s;
}

// a frame's pixels (RGBA8, row 0 = top, w*ss by h*ss) filtered down to w by h and written as a PNG
static bool writeRenderPng(const std::vector<uint8_t>& px, int w, int h, int ss, bool transparent, const fs::path& file,
                           std::string* err) {
  // box filter, premultiplied so transparent edges do not darken
  std::vector<uint8_t> out(size_t(w) * h * 4);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      float acc[4] = {0, 0, 0, 0};
      for (int j = 0; j < ss; ++j)
        for (int k = 0; k < ss; ++k) {
          const uint8_t* p = &px[(size_t(y * ss + j) * w * ss + x * ss + k) * 4];
          float a = p[3] / 255.f;
          for (int c = 0; c < 3; ++c) acc[c] += p[c] / 255.f * a;
          acc[3] += a;
        }
      uint8_t* q = &out[(size_t(y) * w + x) * 4];
      float a = acc[3] / float(ss * ss);
      for (int c = 0; c < 3; ++c) q[c] = (uint8_t)std::clamp(acc[3] > 0 ? acc[c] / acc[3] * 255.f + 0.5f : 0.f, 0.f, 255.f);
      q[3] = (uint8_t)std::clamp(a * 255.f + 0.5f, 0.f, 255.f);
    }
  std::vector<uint8_t> png;
  if (transparent) png = encodePngRGBA8(out.data(), w, h, 4);
  else {
    std::vector<uint8_t> rgb(size_t(w) * h * 3);
    for (size_t k = 0; k < size_t(w) * h; ++k)
      for (int c = 0; c < 3; ++c) rgb[k * 3 + c] = out[k * 4 + c];
    png = encodePngRGBA8(rgb.data(), w, h, 3);
  }
  if (!paths::writeFileAtomic(file, std::string(png.begin(), png.end()))) {
    if (err) *err = "Could not write " + file.string();
    return false;
  }
  return true;
}

// the file of frame i (turntables: name_0001.png ...)
static fs::path renderFrameFile(const RenderImageOptions& opt, int frames, int i) {
  fs::path base(opt.path);
  if (base.extension() != ".png") base += ".png";
  if (frames <= 1) return base;
  char suf[16];
  snprintf(suf, sizeof suf, "_%04d.png", i + 1);
  return base.parent_path() / (base.stem().string() + suf);
}

// the view options of a render image: no overlays
static ViewportOptions renderImageViewOptions(const ViewportOptions& v, const RenderImageOptions& opt) {
  ViewportOptions o = v;
  o.wireframe = o.seams = o.islandTint = o.isolateSet = false;
  o.selectionTint = 0.f;
  o.hoverFace = 0xFFFFFFFFu;
  o.showPins = false;
  o.transparentBackground = opt.transparent;
  if (opt.pathSamples > 0) o.mode = ViewMode::PathTraced, o.pathSamplesNow = opt.pathSamples;
  return o;
}

bool Engine::renderImage(const RenderImageOptions& opt, std::vector<std::string>* written, std::string* err) {
  if (opt.path.empty()) {
    if (err) *err = "No file name given";
    return false;
  }
  int ss = opt.pathSamples > 0 ? 1 : std::clamp(opt.supersample, 1, 4);  // (path traced: the samples smooth the edges)
  int w = std::clamp(opt.width, 16, 8192), h = std::clamp(opt.height, 16, 8192);
  if ((long)w * ss > 16384 || (long)h * ss > 16384) ss = 1;
  doc.updateComposite(materials);
  ViewportOptions o = renderImageViewOptions(viewOptions, opt);
  int frames = std::max(1, opt.turntableFrames);
  GLuint maskTex = o.mode == ViewMode::LayerMask ? maskViewTexture(true) : 0;
  fs::path base = renderFrameFile(opt, 1, 0);
  std::error_code ec;
  if (base.has_parent_path()) fs::create_directories(base.parent_path(), ec);
  progress::Scope prog(frames > 1 ? "Rendering turntable" : "Rendering image");
  for (int i = 0; i < frames; ++i) {
    progress::set(float(i) / float(frames));
    if (progress::cancelled()) {
      if (err) *err = "cancelled";
      return false;
    }
    Camera cam = camera;
    if (frames > 1) cam.yaw += 2.f * kPi * float(i) / float(frames);
    renderer.renderViewport(doc, env, cam, renderImageRT_, w * ss, h * ss, o, maskTex);
    auto px = Renderer::readPixels(renderImageRT_);
    fs::path file = renderFrameFile(opt, frames, i);
    if (!writeRenderPng(px, w, h, ss, opt.transparent, file, err)) return false;
    if (written) written->push_back(fs::absolute(file).string());
  }
  PLOG_I("Rendered %d image(s) at %dx%d to %s", frames, w, h, base.string().c_str());
  return true;
}

bool Engine::startRenderJob(const RenderImageOptions& opt, std::string* err) {
  if (opt.path.empty()) {
    if (err) *err = "No file name given";
    return false;
  }
  if (!doc.hasMesh()) {
    if (err) *err = "No model open";
    return false;
  }
  RenderJob& J = renderJob;
  J = RenderJob();
  J.active = true;
  J.opt = opt;
  J.opt.pathSamples = std::max(1, opt.pathSamples);
  J.w = std::clamp(opt.width, 16, 8192), J.h = std::clamp(opt.height, 16, 8192);
  J.frames = std::max(1, opt.turntableFrames);
  std::error_code ec;
  fs::path base = renderFrameFile(opt, 1, 0);
  if (base.has_parent_path()) fs::create_directories(base.parent_path(), ec);
  doc.updateComposite(materials);
  return true;
}

void Engine::cancelRenderJob() {
  if (!renderJob.active) return;
  renderJob.active = false;
  renderJob.error = "cancelled";
  renderer.renderTracer().release();
}

bool Engine::renderJobStep(double budgetMs) {
  RenderJob& J = renderJob;
  if (!J.active) return true;
  PathTracer& T = renderer.renderTracer();
  if (!J.frameStarted) {
    J.cam = camera;
    if (J.frames > 1) J.cam.yaw += 2.f * kPi * float(J.frame) / float(J.frames);
    J.cam.updateClip(clipBounds());
    T.setScene(renderer.shownObjects(doc));  // (1.2) every object shown
    T.start(doc, env, J.w, J.h);
    J.frameStarted = true;
  }
  doc.updateComposite(materials);
  const bool frameDone = T.step(doc, env, J.cam, viewOptions.pathTrace, J.opt.pathSamples, budgetMs * 1e6);
  J.progress = (float(J.frame) + float(T.samples()) / float(J.opt.pathSamples)) / float(J.frames);
  if (!frameDone) return false;
  // the frame's picture, as renderImage() draws it
  ViewportOptions o = renderImageViewOptions(viewOptions, J.opt);
  o.pathSamplesNow = -1;
  renderer.renderViewport(doc, env, J.cam, renderImageRT_, J.w, J.h, o);
  auto px = Renderer::readPixels(renderImageRT_);
  T.release();
  fs::path file = renderFrameFile(J.opt, J.frames, J.frame);
  std::string err;
  if (!writeRenderPng(px, J.w, J.h, 1, J.opt.transparent, file, &err)) {
    J.error = err, J.active = false;
    return true;
  }
  J.written.push_back(fs::absolute(file).string());
  J.frameStarted = false;
  if (++J.frame >= J.frames) {
    J.active = false, J.progress = 1.f;
    PLOG_I("Rendered %d path traced image(s) at %dx%d, %d samples, to %s", J.frames, J.w, J.h, J.opt.pathSamples,
           J.written.front().c_str());
    return true;
  }
  return false;
}

json Engine::exportForBlender(std::string* err, Document* which) {
  Document& doc = which ? *which : this->doc;  // (1.2) any object of the scene linked to Blender
  materials.atlas = &doc.mesh.tiles;
  bool wasAsleep = doc.tex.asleep();
  doc.updateComposite(materials);
  if (wasAsleep && &doc != &this->doc) doc.tex.sleep();
  materials.atlas = &this->doc.mesh.tiles;
  std::string object = doc.bridge.object.empty() ? doc.mesh.name : doc.bridge.object;
  fs::path dir = doc.bridge.exchangeDir.empty() ? paths::bridgeDir() : fs::path(doc.bridge.exchangeDir);
  dir /= paths::sanitizeFileName(object);
  paths::ensureDir(dir);
  ExportOptions o;
  o.preset = ExportPreset::Blender;
  o.directory = dir.string();
  o.baseName = object;
  o.remember = false;
  std::map<std::string, std::string> files;
  if (!doc.exportTextures(o, &files, err)) return nullptr;
  json r = {{"object", object}, {"textures", json::object()}, {"uv_path", nullptr}};
  const TileLayout& tl = doc.mesh.tiles;
  if (!tl.active()) {
    r["textures"] = files;
  } else {
    // texture tiles: one entry per tile ("basecolor@3" -> tile 3); Blender builds a material per
    // material slot and tiled (UDIM) images
    json sets = json::array();
    for (int t = 0; t < tl.count(); ++t) {
      const TileInfo& ti = tl.tiles[t];
      sets.push_back({{"index", t}, {"name", ti.name}, {"set", doc.mesh.slotName(ti.slot)}, {"slot", ti.slot}, {"udim", ti.udim},
                      {"textures", json::object()}});
    }
    for (auto& [k, path] : files) {
      auto at = k.find('@');
      if (at == std::string::npos) continue;
      int t = std::atoi(k.c_str() + at + 1);
      if (t >= 0 && t < (int)sets.size()) sets[t]["textures"][k.substr(0, at)] = path;
    }
    r["texture_sets"] = sets;
    r["tile_mode"] = {{"material_slots", (tl.mode & TileBySlot) != 0}, {"udim", (tl.mode & TileByUdim) != 0}};
  }
  // The textures are painted for Patina's UVs, so Blender needs them whenever they differ from
  // what it sent - checked against the sent mesh itself, not only the edit flag (which a
  // restart of Patina or a reopened project would otherwise lose).
  bool sendUV = doc.bridge.uvsEdited || doc.bridge.slotsEdited;
  if (!sendUV) {
    Mesh sent;
    if (doc.meshPath.empty() || !patina::loadMesh(doc.meshPath, sent, nullptr) || sent.loopCount() != doc.mesh.loopCount() ||
        sent.loopUV.size() != doc.mesh.loopUV.size()) {
      sendUV = true;
    } else {
      for (size_t i = 0; i < sent.loopUV.size() && !sendUV; ++i)
        sendUV = glm::any(glm::greaterThan(glm::abs(sent.loopUV[i] - doc.mesh.loopUV[i]), vec2(1e-6f)));
    }
    if (sendUV) PLOG_I("UVs differ from the mesh Blender sent: sending them back");
  }
  if (sendUV) {
    fs::path uvp = dir / (paths::sanitizeFileName(object) + ".ptuv");
    if (savePtUV(uvp.string(), doc.mesh, err, doc.bridge.slotsEdited)) r["uv_path"] = fs::absolute(uvp).string();
    if (doc.bridge.slotsEdited) r["slots_changed"] = true;
  }
  return r;
}

json Engine::sendModelToBlender(const std::string& wantName, bool replace, int port, std::string* err) {
  if (!doc.hasMesh()) {
    if (err) *err = "No mesh loaded";
    return nullptr;
  }
  if (replace && !doc.bridge.active) {
    if (err) *err = "replace needs a linked Blender object (use Send to Patina in Blender first)";
    return nullptr;
  }
  std::string source = doc.bridge.active ? doc.bridge.object : std::string();
  std::string base = source.empty() ? doc.mesh.name : source;
  std::string name = replace ? source : !wantName.empty() ? wantName : base + "_patina";
  if (port <= 0) port = doc.bridge.active ? doc.bridge.blenderPort : 7861;
  // the mesh, in Blender's axes, where the exchange folder keeps the object's files
  fs::path dir = paths::ensureDir((doc.bridge.exchangeDir.empty() ? paths::bridgeDir() : fs::path(doc.bridge.exchangeDir)) /
                                  paths::sanitizeFileName(name));
  fs::path ptm = dir / (paths::sanitizeFileName(name) + ".ptmesh");
  Mesh m = doc.mesh;
  m.name = name;
  if (!savePtMesh(ptm.string(), m, err)) return nullptr;
  // its textures, as the live link sends them, for that object
  auto keep = doc.bridge;
  std::string keepPath = doc.meshPath;
  doc.bridge.object = name;
  doc.bridge.uvsEdited = doc.bridge.slotsEdited = false;
  doc.meshPath = ptm.string();  // the object's UVs are Patina's own: no UV file
  json tex = exportForBlender(err);
  auto restore = [&] {
    doc.bridge = keep;
    doc.meshPath = keepPath;
  };
  if (tex.is_null()) {
    restore();
    return nullptr;
  }
  json args = {{"path", fs::absolute(ptm).string()}, {"name", name}, {"source", source}, {"replace", replace},
               {"textures", tex.value("textures", json::object())}};
  if (tex.contains("texture_sets")) args["texture_sets"] = tex["texture_sets"], args["tile_mode"] = tex["tile_mode"];
  net::JsonLineClient cl;
  json resp;
  std::string e;
  if (!cl.connect("127.0.0.1", port, 1500, &e)) {
    restore();
    if (err) *err = "Blender is not listening on port " + std::to_string(port) +
                    ": start Blender's live link (N panel > Patina), with the Patina Bridge add-on 0.9 or newer";
    return nullptr;
  }
  if (!cl.call("patina_new_object", args, resp, 15000, &e) || !resp.value("ok", false)) {
    restore();
    std::string why = resp.value("error", e);
    if (why.find("Unknown command") != std::string::npos)
      why = "the Patina Bridge add-on in Blender is older than 0.9: install the one that came with this Patina";
    if (err) *err = "Blender: " + why;
    return nullptr;
  }
  // linked to the new object from now on
  doc.bridge.active = true;
  doc.bridge.object = name;
  doc.bridge.blenderPort = port;
  if (doc.bridge.exchangeDir.empty()) doc.bridge.exchangeDir = paths::bridgeDir().string();
  doc.meshPath = ptm.string();
  PLOG_I("Sent the model to Blender as %s%s", name.c_str(), replace ? " (its mesh replaced)" : "");
  return json{{"object", name}, {"source", source}, {"replaced", replace}, {"faces", doc.mesh.faceCount()},
              {"port", port}, {"textures", args["textures"].size()}};
}

bool Engine::pushToBlender(std::string* err, json* result, Document* which) {
  Document& d = which ? *which : doc;
  json r = exportForBlender(err, &d);
  if (r.is_null()) return false;
  if (result) *result = r;
  int port = d.bridge.blenderPort;
  net::callAsync(port, "patina_textures_updated", r, [port](bool ok, const json& resp) {
    if (ok) PLOG_I("Pushed textures to Blender (port %d)", port);
    else PLOG_W("Blender did not accept the update (port %d): %s", port, resp.value("error", std::string("?")).c_str());
  });
  return true;
}

}  // namespace patina
