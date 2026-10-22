// GPU integration tests (headless EGL). They exercise the real painting
// pipeline through the same command API the MCP server and Blender use.
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>

#include "Test.h"
#include "TestMeshes.h"
#include "core/Container.h"
#include "core/Image.h"
#include "core/MeshIO.h"
#include "core/UVOps.h"
#include "core/Paths.h"
#include "engine/Assistant.h"
#include "engine/AutoTexture.h"
#include "engine/Engine.h"
#include "platform/HeadlessContext.h"

using namespace patina;
namespace fs = std::filesystem;

namespace {

HeadlessContext* g_ctx = nullptr;
Engine* g_engine = nullptr;

Engine* engine() {
  if (g_engine) return g_engine;
  g_ctx = new HeadlessContext();
  std::string err;
  if (!g_ctx->create(&err)) {
    std::fprintf(stderr, "  no GL context: %s\n", err.c_str());
    return nullptr;
  }
  g_engine = new Engine();
  g_engine->defaultResolution = 512;
  g_engine->init(true);
  g_engine->viewportSize = {512, 512};
  return g_engine;
}

json run(const char* cmd, const json& args = json::object()) { return engine()->execute(cmd, args); }

// color of the final base color map at a UV coordinate
vec4 texelAt(Engine& e, vec2 uv) {
  e.doc.updateComposite(e.materials);
  int R = e.doc.tex.resolution();
  int x = std::clamp(int(uv.x * R), 0, R - 1), y = std::clamp(int(uv.y * R), 0, R - 1);
  float c[4];
  glGetTextureSubImage(e.doc.tex.finColor.id(), 0, x, y, 0, 1, 1, 1, GL_RGBA, GL_FLOAT, sizeof c, c);
  return {c[0], c[1], c[2], c[3]};
}

bool isRed(vec4 c) { return c.x > 0.6f && c.y < 0.25f && c.z < 0.25f; }

void freshSphere(Engine& e) {
  run("new_project", {{"primitive", "sphere"}, {"resolution", 512}});
  run("set_camera", {{"view", "front"}, {"frame", true}});
  run("set_brush", {{"preset", "basic_hard"}, {"size", 60}, {"hardness", 0.9}, {"flow", 1.0}, {"opacity", 1.0}});
  run("set_paint_material", {{"id", ""}, {"color", {1, 0, 0}}, {"roughness", 0.4}, {"metallic", 0}, {"opacity", 1.0},
                             {"sheen", 0.0}, {"coat", 0.0}, {"subsurface", 0.0}});
  run("set_paint_channels", {{"channels", {"all"}}});
  run("set_tool", {{"tool", "paint"}});
  (void)e;
}

}  // namespace

TEST(gpu, context_and_init) {
  Engine* e = engine();
  CHECK(e != nullptr);
  if (!e) return;
  CHECK(e->materials.entries().size() >= 170);
  // every built-in material is a patina_* material; old ids still resolve (projects from 0.1)
  for (auto& m : e->materials.entries())
    if (m->def.builtin) CHECK(m->def.id.rfind("patina_", 0) == 0);
  CHECK(e->materials.find("rust_heavy") == e->materials.find("patina_rust_heavy"));
  CHECK(e->materials.find("patina_rust_heavy") != nullptr);
  CHECK(e->brushes.presets().size() >= 20);
  CHECK(e->doc.hasMesh());
}

TEST(gpu, paint_stroke_changes_texels) {
  Engine* e = engine();
  if (!e) return;
  freshSphere(*e);
  RayHit h = e->pick({256, 256});
  CHECK(h.hit);
  CHECK(!isRed(texelAt(*e, h.uv)));
  json r = run("paint_stroke", {{"points", {{0.45, 0.5}, {0.55, 0.5}}}});
  CHECK(r.value("dabs", 0) > 3);
  CHECK(isRed(texelAt(*e, h.uv)));
  // far away texel untouched
  RayHit far = e->pick({256, 120});
  CHECK(far.hit);
  if (far.hit) {
    vec4 fc = texelAt(*e, far.uv);
    if (isRed(fc)) std::fprintf(stderr, "  far texel uv=(%.3f,%.3f) color=(%.2f,%.2f,%.2f)\n", far.uv.x, far.uv.y, fc.x, fc.y, fc.z);
    CHECK(!isRed(fc));
  }
}

TEST(gpu, stroke_crosses_uv_seam) {
  Engine* e = engine();
  if (!e) return;
  freshSphere(*e);
  // the sphere's seam is the meridian at +X: look at it from the right
  run("set_camera", {{"view", "right"}});
  RayHit h = e->pick({256, 256});
  CHECK(h.hit);
  run("paint_stroke", {{"points", {{0.5, 0.45}, {0.5, 0.55}}}});
  // both sides of the seam (u near 0 and u near 1 at the same v) must be painted
  float v = h.uv.y;
  CHECK(isRed(texelAt(*e, {0.01f, v})));
  CHECK(isRed(texelAt(*e, {0.99f, v})));
}

TEST(gpu, undo_redo_restores) {
  Engine* e = engine();
  if (!e) return;
  freshSphere(*e);
  RayHit h = e->pick({256, 256});
  size_t before = e->doc.undo.bytes();
  run("paint_stroke", {{"points", {{0.5, 0.5}}}});
  CHECK(isRed(texelAt(*e, h.uv)));
  // a single dab must not snapshot whole textures (tile-based undo)
  size_t full = size_t(512) * 512 * 4 * 6;
  CHECK(e->doc.undo.bytes() - before < full / 4);
  run("undo");
  CHECK(!isRed(texelAt(*e, h.uv)));
  run("redo");
  CHECK(isRed(texelAt(*e, h.uv)));
}

TEST(gpu, erase_and_mask) {
  Engine* e = engine();
  if (!e) return;
  freshSphere(*e);
  RayHit h = e->pick({256, 256});
  run("paint_stroke", {{"points", {{0.5, 0.5}}}});
  CHECK(isRed(texelAt(*e, h.uv)));
  run("paint_stroke", {{"points", {{0.5, 0.5}}}, {"tool", "erase"}});
  CHECK(!isRed(texelAt(*e, h.uv)));
  // fill layer revealed only where its mask is painted
  run("add_layer", {{"type", "fill"}, {"material", {{"id", ""}, {"color", {1, 0, 0}}}}});
  CHECK(isRed(texelAt(*e, h.uv)));  // fill layers without a mask cover everything
  run("set_layer_mask", {{"value", 0.0}});
  CHECK(!isRed(texelAt(*e, h.uv)));
  run("select_layer", {{"index", e->doc.selected}, {"edit_mask", true}});
  run("paint_stroke", {{"points", {{0.5, 0.5}}}, {"tool", "paint"}});
  CHECK(isRed(texelAt(*e, h.uv)));
}

TEST(gpu, polygon_fill_island) {
  Engine* e = engine();
  if (!e) return;
  run("new_project", {{"primitive", "cube"}, {"resolution", 512}});
  run("set_camera", {{"view", "front"}, {"frame", true}});
  run("set_paint_material", {{"id", ""}, {"color", {1, 0, 0}}});
  RayHit h = e->pick({256, 256});
  CHECK(h.hit);
  json r = run("fill", {{"mode", "uv_island"}, {"point", {0.5, 0.5}}});
  CHECK(r.value("faces", 0) == 1);  // each cube face is its own island
  CHECK(isRed(texelAt(*e, h.uv)));
  // a different face stays unpainted
  run("set_camera", {{"view", "top"}});
  RayHit top = e->pick({256, 256});
  CHECK(top.hit && !isRed(texelAt(*e, top.uv)));
}

TEST(gpu, uv_edit_keeps_paint) {
  Engine* e = engine();
  if (!e) return;
  freshSphere(*e);
  run("paint_stroke", {{"points", {{0.45, 0.5}, {0.55, 0.5}}}});
  RayHit before = e->pick({256, 256});
  CHECK(isRed(texelAt(*e, before.uv)));
  // rotate every UV island 90 degrees and shrink: the paint must follow
  std::vector<uint32_t> all;
  for (uint32_t f = 0; f < e->doc.mesh.faceCount(); ++f) all.push_back(f);
  run("uv_transform", {{"faces", all}, {"rotate", 90}, {"scale", 0.8}});
  RayHit after = e->pick({256, 256});
  CHECK(glm::length(after.uv - before.uv) > 0.05f);  // UVs really moved
  CHECK(isRed(texelAt(*e, after.uv)));
  // smart unwrap (new islands + seams) also keeps the paint
  run("uv_unwrap", {{"method", "smart"}});
  RayHit after2 = e->pick({256, 256});
  CHECK(isRed(texelAt(*e, after2.uv)));
  json info = run("uv_info");
  CHECK(info.value("islands", 0) > 1);
  CHECK(info.value("overlaps", 1) == 0);
}

TEST(gpu, materials_paint_and_fill) {
  Engine* e = engine();
  if (!e) return;
  freshSphere(*e);
  run("set_paint_material", {{"id", "patina_rust_heavy"}});
  run("paint_stroke", {{"points", {{0.4, 0.5}, {0.6, 0.5}}}});
  RayHit h = e->pick({256, 256});
  vec4 c = texelAt(*e, h.uv);
  CHECK(c.x > c.z);  // rust is orange/brown or steel gray, never blue-dominant
  run("bake_mesh_maps", {{"ao_samples", 16}});
  CHECK(e->doc.tex.hasAO() && e->doc.tex.hasCurvature());
  run("add_layer", {{"type", "fill"}, {"material", {{"id", "gold_polished"}}}, {"generator", {{"type", "edges"}, {"level", 0.5}}}});
  json st = run("get_state");
  CHECK(st["layers"].size() >= 2);
  json img = run("render_view", {{"width", 320}, {"height", 240}});
  CHECK(img.contains("image_png_base64"));
  CHECK(img["image_png_base64"].get<std::string>().size() > 1000);
}

TEST(gpu, project_roundtrip_and_export) {
  Engine* e = engine();
  if (!e) return;
  freshSphere(*e);
  run("paint_stroke", {{"points", {{0.45, 0.5}, {0.55, 0.5}}}});
  RayHit h = e->pick({256, 256});
  auto dir = paths::ensureDir(paths::cacheDir() / "test");
  std::string proj = (dir / "roundtrip.patina").string();
  run("save_project", {{"path", proj}});
  run("new_project", {{"primitive", "cube"}});
  run("open_file", {{"path", proj}});
  CHECK(e->doc.mesh.name == "Sphere");
  CHECK(isRed(texelAt(*e, h.uv)));
  json ex = run("export_textures", {{"directory", (dir / "export").string()}, {"preset", "unreal"}});
  CHECK(ex["files"].size() >= 4);
  for (auto& [k, p] : ex["files"].items()) CHECK(fs::exists(p.get<std::string>()));
}

TEST(gpu, blender_bridge_roundtrip) {
  Engine* e = engine();
  if (!e) return;
  auto dir = paths::ensureDir(paths::cacheDir() / "test" / "bridge");
  Mesh cube = makeCube(2.f);
  std::string path = (dir / "Cube.ptmesh").string();
  std::string err;
  CHECK(savePtMesh(path, cube, &err));
  json r = run("bridge_open_mesh", {{"path", path}, {"object", "Cube"}, {"blender_port", 1}, {"exchange_dir", dir.string()},
                                    {"resolution", 256}});
  CHECK(r.value("faces", 0) == 6);
  CHECK(e->doc.bridge.active && e->doc.bridge.object == "Cube");
  CHECK(!e->doc.bridge.uvsEdited);
  json ex = run("bridge_export");
  CHECK(ex["uv_path"].is_null());
  CHECK(ex["textures"].contains("basecolor") && ex["textures"].contains("normal"));
  run("uv_unwrap", {{"method", "box"}});
  ex = run("bridge_export");
  CHECK(ex["uv_path"].is_string());
  Mesh back = cube;
  CHECK(loadPtUV(ex["uv_path"], back, &err));
  bool changed = false;
  for (size_t l = 0; l < back.loopUV.size(); ++l) changed |= glm::length(back.loopUV[l] - cube.loopUV[l]) > 1e-4f;
  CHECK(changed);
}

TEST(gpu, uv_move_single_island_moves_paint) {
  Engine* e = engine();
  if (!e) return;
  run("new_project", {{"primitive", "cube"}, {"resolution", 512}});
  run("set_paint_material", {{"id", ""}, {"color", {1, 0, 0}}});
  run("fill", {{"mode", "face"}, {"faces", {0}}});
  const Mesh& m = e->doc.mesh;
  Rect2 before;
  for (uint32_t l = m.faceStart[0]; l < m.faceStart[1]; ++l) before.add(m.loopUV[l]);
  run("select_faces", {{"mode", "set"}, {"faces", {0}}});
  // the cube's cross layout leaves cell (0,0) empty: move the front face there
  run("uv_transform", {{"selection", true}, {"translate", {-0.25, -1.0 / 3.0}}});
  Rect2 after;
  for (uint32_t l = m.faceStart[0]; l < m.faceStart[1]; ++l) after.add(m.loopUV[l]);
  CHECK_NEAR(after.lo.x - before.lo.x, -0.25, 1e-5);
  int red = 0, total = 0;
  for (int j = 1; j < 8; ++j)
    for (int i = 1; i < 8; ++i) {
      vec2 uv = after.lo + (after.hi - after.lo) * vec2(i / 8.f, j / 8.f);
      ++total;
      if (isRed(texelAt(*e, uv))) ++red;
    }
  if (red != total) std::fprintf(stderr, "  %d / %d texels red after the move\n", red, total);
  CHECK(red == total);
}

TEST(gpu, uv_overlap_does_not_erase_paint) {
  Engine* e = engine();
  if (!e) return;
  run("new_project", {{"primitive", "cube"}, {"resolution", 512}});
  run("set_paint_material", {{"id", ""}, {"color", {1, 0, 0}}});
  run("fill", {{"mode", "face"}, {"faces", {0}}});
  // move the painted front face on top of the unpainted right face (overlapping UVs)
  run("select_faces", {{"mode", "set"}, {"faces", {0}}});
  run("uv_transform", {{"selection", true}, {"translate", {0.25, 0.0}}});
  const Mesh& m = e->doc.mesh;
  Rect2 r;
  for (uint32_t l = m.faceStart[0]; l < m.faceStart[1]; ++l) r.add(m.loopUV[l]);
  CHECK(isRed(texelAt(*e, r.center())));
}

TEST(gpu, blur_softens_edges) {
  Engine* e = engine();
  if (!e) return;
  freshSphere(*e);
  run("set_brush", {{"size", 60}, {"hardness", 1.0}});
  run("paint_stroke", {{"points", {{0.5, 0.5}}}});
  RayHit outside = e->pick({256 + 36, 256});
  vec4 before = texelAt(*e, outside.uv);
  CHECK(!isRed(before));
  run("set_tool", {{"tool", "blur"}, {"blur_radius", 24}});
  run("paint_stroke", {{"points", {{0.5, 0.5}, {0.58, 0.5}}}, {"brush", {{"size", 200}, {"hardness", 0.5}}}});
  vec4 after = texelAt(*e, outside.uv);
  CHECK(after.y < before.y - 0.05f);  // red bled into the gray edge
  run("set_tool", {{"tool", "paint"}});
}

TEST(gpu, torus_and_paint_through) {
  Engine* e = engine();
  if (!e) return;
  run("new_project", {{"primitive", "torus"}, {"resolution", 512}});
  CHECK(e->doc.mesh.islandCount == 1);
  // orthographic top view: the top and bottom of the ring land on the same pixels
  run("set_camera", {{"view", "top"}, {"frame", true}, {"ortho", true}});
  run("set_paint_material", {{"id", ""}, {"color", {1, 0, 0}}});
  // with occlusion off the stroke also reaches the hidden underside of the ring
  run("set_brush", {{"preset", "paint_through"}, {"size", 40}});
  vec3 o, d;
  e->camera.ray({256, 256}, e->viewportSize, o, d);
  // a point on the ring, seen from above: x = 1 (major radius)
  vec2 px;
  {
    mat4 vp = e->camera.viewProj(1.f);
    vec4 c = vp * vec4(1.f, 0.38f, 0.f, 1.f);
    px = vec2(c.x / c.w * 0.5f + 0.5f, 0.5f - c.y / c.w * 0.5f);
  }
  run("paint_stroke", {{"points", {{px.x, px.y}}}});
  RayHit top = e->doc.bvh.raycast(e->doc.mesh, vec3(1, 5, 0), vec3(0, -1, 0));
  RayHit bottom = e->doc.bvh.raycast(e->doc.mesh, vec3(1, -5, 0), vec3(0, 1, 0));
  CHECK(top.hit && bottom.hit);
  CHECK(isRed(texelAt(*e, top.uv)));
  CHECK(isRed(texelAt(*e, bottom.uv)));
  run("set_camera", {{"ortho", false}});
}

TEST(gpu, topology_change_transfers_paint) {
  Engine* e = engine();
  if (!e) return;
  freshSphere(*e);
  run("paint_stroke", {{"points", {{0.45, 0.5}, {0.55, 0.5}}}});
  RayHit h0 = e->pick({256, 256});
  CHECK(isRed(texelAt(*e, h0.uv)));
  vec3 painted = h0.position;
  // new topology (denser sphere) with a completely different UV layout
  Mesh dense = makeUVSphere(80, 40, 1.f);
  uv::smartProject(dense, 66.f);
  dense.computeUVTopology();
  dense.computeTangents();
  e->doc.setMesh(std::move(dense), 512, true);
  CHECK(e->doc.layers.size() == 1);
  RayHit h1 = e->doc.bvh.raycast(e->doc.mesh, painted * 3.f, -glm::normalize(painted));
  CHECK(h1.hit);
  CHECK(isRed(texelAt(*e, h1.uv)));
  // an unpainted spot stays unpainted
  RayHit back = e->doc.bvh.raycast(e->doc.mesh, -painted * 3.f, glm::normalize(painted));
  CHECK(back.hit && !isRed(texelAt(*e, back.uv)));
}

TEST(gpu, adjust_layer_shifts_hue) {
  Engine* e = engine();
  if (!e) return;
  freshSphere(*e);
  RayHit h = e->pick({256, 256});
  run("paint_stroke", {{"points", {{0.5, 0.5}}}});
  CHECK(isRed(texelAt(*e, h.uv)));
  // a third of a turn: red -> green
  json r = run("add_layer", {{"type", "adjust"}, {"adjust", {{"hue", 1.0 / 3.0}}}});
  CHECK(e->doc.selectedLayer() && e->doc.selectedLayer()->type == LayerType::Adjust);
  vec4 c = texelAt(*e, h.uv);
  CHECK(c.y > 0.6f && c.x < 0.25f);
  // the mask limits the adjustment: a black mask restores the original color
  run("set_layer_mask", {{"value", 0.0}});
  CHECK(isRed(texelAt(*e, h.uv)));
  run("set_layer_mask", {{"value", 1.0}});
  run("update_layer", {{"adjust", {{"hue", 0.0}, {"brightness", 0.0}}}});
  c = texelAt(*e, h.uv);
  CHECK(c.x < 0.05f && c.y < 0.05f && c.z < 0.05f);
  // survives a project round trip
  std::string path = (fs::temp_directory_path() / "patina_adjust_test.patina").string();
  run("save_project", {{"path", path}});
  run("open_file", {{"path", path}});
  CHECK(e->doc.layers.back()->type == LayerType::Adjust);
  CHECK(std::abs(e->doc.layers.back()->adjust.brightness) < 1e-4f);
  fs::remove(path);
}

TEST(gpu, clone_copies_paint) {
  Engine* e = engine();
  if (!e) return;
  freshSphere(*e);
  run("set_brush", {{"size", 50}, {"hardness", 0.9}});
  run("paint_stroke", {{"points", {{0.5, 0.5}}}});
  RayHit target = e->pick({256, 180});
  CHECK(target.hit);
  CHECK(!isRed(texelAt(*e, target.uv)));
  // without a source the clone tool refuses to start
  e->painter.hasCloneSource = false;
  bool threw = false;
  try {
    run("paint_stroke", {{"points", {{0.5, 180.0 / 512}}}, {"tool", "clone"}});
  } catch (...) {
    threw = true;
  }
  json src = run("set_clone_source", {{"point", {0.5, 0.5}}, {"aligned", false}});
  CHECK(src.contains("uv"));
  run("paint_stroke", {{"points", {{0.5, 180.0 / 512}}}, {"tool", "clone"}});
  CHECK(isRed(texelAt(*e, target.uv)));
  // undo removes the cloned paint, the original stays
  run("undo");
  CHECK(!isRed(texelAt(*e, target.uv)));
  CHECK(isRed(texelAt(*e, e->pick({256, 256}).uv)));
  CHECK(threw);
  run("set_tool", {{"tool", "paint"}});
}

TEST(gpu, every_library_material_bakes) {
  Engine* e = engine();
  if (!e) return;
  int procedural = 0;
  for (auto& m : e->materials.entries()) {
    if (!m->def.builtin || m->def.type != MaterialType::Procedural) continue;
    ++procedural;
    CHECK(e->materials.ensureBaked(*m, 256));
    // color or roughness/metal/height must carry some pattern, and nothing may be NaN
    double range = 0;
    bool finite = true;
    for (Texture2D* t : {&m->gpu.color, &m->gpu.rmh}) {
      int w = t->width(), h = t->height();
      std::vector<float> px(size_t(w) * h * 4);
      glGetTextureImage(t->id(), 0, GL_RGBA, GL_FLOAT, (GLsizei)(px.size() * 4), px.data());
      double mn = 1e9, mx = -1e9;
      for (size_t i = 0; i < px.size(); i += 4) {
        double l = px[i] + px[i + 1] + px[i + 2];
        finite &= std::isfinite(l);
        mn = std::min(mn, l), mx = std::max(mx, l);
      }
      range = std::max(range, mx - mn);
    }
    if (!finite || range < 0.004) std::fprintf(stderr, "  %s: flat or invalid bake (range %.4f)\n", m->def.id.c_str(), range);
    CHECK(finite);
    CHECK(range >= 0.004);
  }
  CHECK(procedural >= 140);
}

TEST(gpu, folders_solo_and_undoable_settings) {
  Engine* e = engine();
  if (!e) return;
  freshSphere(*e);
  RayHit h = e->pick({256, 256});
  CHECK(!isRed(texelAt(*e, h.uv)));
  size_t base = e->doc.layers.size();
  int folder = run("add_layer", {{"type", "folder"}, {"name", "Group"}})["index"];
  json fill = run("add_layer", {{"type", "fill"}, {"parent", folder}, {"material", {{"id", ""}, {"color", {1, 0, 0}}}}});
  folder = e->doc.indexOf(e->doc.layers[folder]->uid) >= 0 ? folder : -1;
  int fi = e->doc.indexOf(fill["uid"].get<uint32_t>());
  CHECK(e->doc.effectiveParent(*e->doc.layers[fi]) != 0);
  folder = e->doc.indexOf(e->doc.effectiveParent(*e->doc.layers[fi]));
  CHECK(isRed(texelAt(*e, h.uv)));
  // folder opacity is applied to its contents, and is undoable
  run("update_layer", {{"index", folder}, {"opacity", 0.0}});
  CHECK(!isRed(texelAt(*e, h.uv)));
  CHECK(e->doc.undo.undoLabel().find("Opacity") == 0);
  run("undo");
  CHECK(isRed(texelAt(*e, h.uv)));
  run("redo");
  CHECK(!isRed(texelAt(*e, h.uv)));
  run("undo");
  // folder mask
  run("set_layer_mask", {{"index", folder}, {"value", 0.0}});
  CHECK(!isRed(texelAt(*e, h.uv)));
  run("undo");
  CHECK(isRed(texelAt(*e, h.uv)));
  CHECK(!e->doc.layers[folder]->mask);
  // solo: a green layer on top hides the folder until the folder is soloed
  int green = run("add_layer", {{"type", "fill"}, {"index", (int)e->doc.layers.size()},
                                {"material", {{"id", ""}, {"color", {0, 1, 0}}}}})["index"];
  CHECK(!isRed(texelAt(*e, h.uv)));
  run("solo_layer", {{"index", folder}});
  CHECK(isRed(texelAt(*e, h.uv)));
  run("solo_layer", {{"index", -1}});
  CHECK(!isRed(texelAt(*e, h.uv)));
  run("remove_layer", {{"index", green}});
  CHECK(isRed(texelAt(*e, h.uv)));
  // deleting a folder deletes its contents; undo brings both back
  run("remove_layer", {{"index", folder}});
  CHECK(e->doc.layers.size() == base);
  CHECK(!isRed(texelAt(*e, h.uv)));
  run("undo");
  CHECK(e->doc.layers.size() == base + 2);
  CHECK(isRed(texelAt(*e, h.uv)));
  // duplicate copies the subtree with fresh uids
  run("duplicate_layer", {{"index", e->doc.indexOf(e->doc.effectiveParent(*e->doc.layers[e->doc.indexOf(fill["uid"])]))}});
  CHECK(e->doc.layers.size() == base + 4);
  int folders = 0;
  for (auto& l : e->doc.layers) folders += l->isFolder();
  CHECK(folders == 2);
  // the history panel can jump back to the start
  json hist = run("history");
  CHECK(hist["steps"].size() == 5);  // add folder, add fill, add green, delete green, duplicate
  run("history_jump", {{"position", 0}});
  CHECK(e->doc.layers.size() == base);
  CHECK(!isRed(texelAt(*e, h.uv)));
  run("history_jump", {{"position", hist["position"]}});
  CHECK(e->doc.layers.size() == base + 4);
  CHECK(isRed(texelAt(*e, h.uv)));
}

TEST(gpu, clear_paint_keeps_mask_and_undoes) {
  Engine* e = engine();
  if (!e) return;
  freshSphere(*e);
  RayHit h = e->pick({256, 256});
  run("paint_stroke", {{"points", {{0.5, 0.5}}}});
  run("set_layer_mask", {{"value", 1.0}});
  CHECK(isRed(texelAt(*e, h.uv)));
  run("clear_layer", json::object());
  CHECK(!isRed(texelAt(*e, h.uv)));
  CHECK((bool)e->doc.selectedLayer()->mask);
  run("undo");
  CHECK(isRed(texelAt(*e, h.uv)));
}

TEST(gpu, radial_and_combined_symmetry) {
  Engine* e = engine();
  if (!e) return;
  freshSphere(*e);
  run("set_brush", {{"size", 30}, {"projection", "surface"}});
  run("set_camera", {{"view", "front"}, {"frame", true}});
  // dab right of center on the front; radial 4 around Y also paints the back (180 degrees)
  run("set_symmetry", {{"x", false}, {"radial", 4}, {"radial_axis", "y"}});
  vec3 p = e->pick({330, 256}).position;
  CHECK(e->pick({330, 256}).hit);
  run("paint_world_points", {{"points", {{p.x, p.y, p.z}}}});
  Mesh& m = e->doc.mesh;
  (void)m;
  RayHit back = e->doc.bvh.closestPoint(e->doc.mesh, vec3(-p.x, p.y, -p.z), 1.f);
  RayHit side = e->doc.bvh.closestPoint(e->doc.mesh, vec3(p.z, p.y, -p.x), 1.f);
  CHECK(back.hit && isRed(texelAt(*e, back.uv)));
  CHECK(side.hit && isRed(texelAt(*e, side.uv)));
  // X mirror combined with Z mirror also produces the diagonal copy
  run("undo");
  run("set_symmetry", {{"x", true}, {"z", true}, {"radial", 1}});
  run("paint_world_points", {{"points", {{p.x, p.y, p.z}}}});
  RayHit diag = e->doc.bvh.closestPoint(e->doc.mesh, vec3(-p.x, p.y, -p.z), 1.f);
  CHECK(diag.hit && isRed(texelAt(*e, diag.uv)));
  run("set_symmetry", {{"x", false}, {"z", false}, {"radial", 1}});
  run("set_brush", {{"projection", "screen"}});
}

TEST(gpu, export_templates_and_mask) {
  Engine* e = engine();
  if (!e) return;
  freshSphere(*e);
  run("paint_stroke", {{"points", {{0.5, 0.5}}}});
  run("set_layer_mask", {{"value", 0.5}});
  fs::path dir = fs::temp_directory_path() / "patina_test_export_tpl";
  fs::remove_all(dir);
  json r = run("export_textures", {{"directory", dir.string()}, {"preset", "gltf"}, {"size", 256},
                                   {"name_template", "T_$mesh_$map_$size"}});
  CHECK(r["files"].contains("basecolor"));
  CHECK(fs::path(r["files"]["basecolor"].get<std::string>()).filename().string().find("_baseColor_256.png") != std::string::npos);
  CHECK(fs::path(r["files"]["basecolor"].get<std::string>()).filename().string().rfind("T_", 0) == 0);
  json m = run("export_textures", {{"directory", dir.string()}, {"layer", e->doc.selected}, {"mask", true},
                                   {"name_template", "$layer_$map"}});
  CHECK(m["files"].contains("mask"));
  CHECK(fs::exists(m["files"]["mask"].get<std::string>()));
  bool threw = false;
  try {
    run("export_textures", {{"directory", dir.string()}, {"layer", 99}});
  } catch (std::exception&) {
    threw = true;
  }
  CHECK(threw);
  fs::remove_all(dir);
}

// (0.41) the layers of the stack without the baked map layers (bakes add those)
static size_t stackLayers(const Document& d) {
  size_t n = 0;
  for (auto& l : d.layers) n += !l->isBaked();
  return n;
}

TEST(gpu, smart_materials_and_layer_settings) {
  Engine* e = engine();
  if (!e) return;
  freshSphere(*e);
  RayHit h = e->pick({256, 256});
  json list = run("list_smart_materials");
  CHECK(list.size() >= 19);  // built-in set
  bool builtin = false;
  for (auto& s : list) builtin |= s["id"] == "patina_smart_rusted_iron" && s["builtin"] == true;
  CHECK(builtin);
  size_t base = stackLayers(e->doc);
  json a = run("apply_smart_material", {{"id", "patina_smart_rusted_iron"}});
  CHECK(e->doc.layers[a["index"].get<int>()]->isFolder());
  CHECK(stackLayers(e->doc) == base + 5);
  CHECK(e->doc.tex.hasCurvature() && e->doc.tex.hasAO());  // baked for the smart masks
  CHECK(e->doc.bakedLayer(0) && e->doc.bakedLayer(1));     // (0.41) and they have their layers
  run("undo");
  CHECK(stackLayers(e->doc) == base);
  // save a red folder (with paint) and bring it back
  int f = run("add_layer", {{"type", "folder"}, {"name", "RedTest"}})["index"];
  run("add_layer", {{"type", "paint"}, {"parent", f}});
  run("paint_stroke", {{"points", {{0.5, 0.5}}}});
  CHECK(isRed(texelAt(*e, h.uv)));
  int folder = e->doc.indexOf(e->doc.effectiveParent(*e->doc.selectedLayer()));
  json s = run("save_smart_material", {{"index", folder}, {"name", "zz_test_red"}, {"include_paint", true}});
  run("remove_layer", {{"index", folder}});
  CHECK(!isRed(texelAt(*e, h.uv)));
  run("apply_smart_material", {{"id", "zz_test_red"}});
  CHECK(isRed(texelAt(*e, h.uv)));
  fs::remove(s["path"].get<std::string>());
  // copy settings: opacity and blend travel, the name does not
  int top = e->doc.selected;
  run("update_layer", {{"index", top}, {"opacity", 0.25}, {"blend", "multiply"}});
  int other = run("add_layer", {{"type", "paint"}, {"name", "Other"}, {"index", 0}})["index"];
  run("copy_layer_settings", {{"from", e->doc.indexOf(e->doc.layers[top + 1]->uid)}, {"to", {other}}});
  CHECK(std::abs(e->doc.layers[other]->opacity - 0.25f) < 1e-4f);
  CHECK(e->doc.layers[other]->blend == BlendMode::Multiply);
  CHECK(e->doc.layers[other]->name == "Other");
  run("undo");
  CHECK(e->doc.layers[other]->opacity == 1.f);
}

TEST(gpu, every_builtin_smart_material_applies) {
  Engine* e = engine();
  if (!e) return;
  freshSphere(*e);
  int n = 0;
  for (auto& s : run("list_smart_materials")) {
    if (!s.value("builtin", false)) continue;
    ++n;
    size_t base = e->doc.layers.size();
    json a = run("apply_smart_material", {{"id", s["id"]}});
    int f = a["index"].get<int>();
    CHECK(e->doc.layers[f]->isFolder());
    CHECK(e->doc.layers.size() > base + 1);
    // every fill layer points at a library material (or carries flat values)
    for (auto& l : e->doc.layers)
      if (l->type == LayerType::Fill && !l->material.id.empty()) {
        if (!e->materials.find(l->material.id))
          std::fprintf(stderr, "  %s: unknown material %s\n", s["id"].get<std::string>().c_str(), l->material.id.c_str());
        CHECK(e->materials.find(l->material.id) != nullptr);
      }
    e->doc.updateComposite(e->materials);
    run("undo");
    CHECK(e->doc.layers.size() == base);
  }
  CHECK(n >= 19);
}

TEST(gpu, folders_survive_save_and_load) {
  Engine* e = engine();
  if (!e) return;
  freshSphere(*e);
  int f = run("add_layer", {{"type", "folder"}, {"name", "Group A"}})["index"];
  run("add_layer", {{"type", "fill"}, {"parent", f}, {"name", "Inside"}, {"material", {{"id", ""}, {"color", {1, 0, 0}}}}});
  run("update_layer", {{"index", e->doc.indexOf(e->doc.findLayer(e->doc.layers[e->doc.selected]->uid)->uid)}, {"color_tag", 3}});
  fs::path p = fs::temp_directory_path() / "patina_test_folders.patina";
  run("save_project", {{"path", p.string()}});
  run("new_project", {{"primitive", "cube"}});
  run("open_file", {{"path", p.string()}});
  int inside = -1, folder = -1;
  for (int i = 0; i < (int)e->doc.layers.size(); ++i) {
    if (e->doc.layers[i]->name == "Inside") inside = i;
    if (e->doc.layers[i]->name == "Group A") folder = i;
  }
  CHECK(inside >= 0 && folder >= 0);
  CHECK(e->doc.layers[folder]->isFolder());
  CHECK(e->doc.effectiveParent(*e->doc.layers[inside]) == e->doc.layers[folder]->uid);
  CHECK(e->doc.layers[inside]->colorTag == 3);
  CHECK(!e->doc.undo.canUndo());  // a freshly opened project has no history
  fs::remove(p);
}

// ---------------------------------------------------------------- 0.4
#include "core/UVIsland.h"

namespace {
vec4 texelOf(Engine& e, Texture2D& t, vec2 uv) {
  int R = t.width();
  int x = std::clamp(int(uv.x * R), 0, R - 1), y = std::clamp(int(uv.y * R), 0, R - 1);
  float c[4];
  glGetTextureSubImage(t.id(), 0, x, y, 0, 1, 1, 1, GL_RGBA, GL_FLOAT, sizeof c, c);
  (void)e;
  return {c[0], c[1], c[2], c[3]};
}
std::string tmpFile(const std::string& name) { return (fs::temp_directory_path() / name).string(); }
}  // namespace

TEST(gpu, island_brush_keeps_paint_and_layout) {
  Engine* e = engine();
  if (!e) return;
  run("open_file", {{"path", std::string(PATINA_SOURCE_DIR) + "/samples/Suzanne.glb"}});
  run("set_resolution", {{"resolution", 1024}});
  run("bake_mesh_maps", {{"ao", true}, {"curvature", false}, {"ao_samples", 16}});
  Image aoBefore = e->doc.tex.aoMap.download(4);
  run("set_camera", {{"view", "front"}, {"frame", true}});
  run("set_brush", {{"preset", "basic_hard"}, {"size", 60}, {"hardness", 0.9}, {"flow", 1.0}, {"opacity", 1.0}});
  run("set_paint_material", {{"id", ""}, {"color", {1, 0, 0}}});
  run("set_tool", {{"tool", "paint"}});
  RayHit h = e->pick({256, 200});
  CHECK(h.hit);
  run("paint_stroke", {{"points", {{0.5, 200.0 / 512.0}}}});
  CHECK(isRed(texelAt(*e, h.uv)));
  // "paint" a region: the face under the cursor, grown a few times
  run("select_faces", {{"mode", "set"}, {"faces", {h.face}}});
  for (int i = 0; i < 4; ++i) run("select_faces", {{"mode", "grow"}});
  size_t nsel = e->doc.selectedFaces().size();
  CHECK(nsel > 10);
  int overlapsBefore = uv::overlapCount(e->doc.mesh);
  json r = run("uv_make_island", {{"selection", true}});
  CHECK(r["islands_made"].get<int>() >= 1);
  CHECK(r["overlaps"].get<int>() <= overlapsBefore);
  // one island, paint still at the same place on the model
  const Mesh& m = e->doc.mesh;
  auto sel = e->doc.selectedFaces();
  uint32_t isl = m.loopIsland[m.faceStart[sel[0]]];
  bool oneIsland = true;
  for (uint32_t f : sel) oneIsland &= m.loopIsland[m.faceStart[f]] == isl;
  CHECK(oneIsland);
  RayHit h2 = e->pick({256, 200});
  CHECK(glm::length(h2.uv - h.uv) > 1e-4f);  // the UVs really changed
  CHECK(isRed(texelAt(*e, h2.uv)));
  // undo restores the old UVs in one step
  run("undo");
  RayHit h3 = e->pick({256, 200});
  CHECK(glm::length(h3.uv - h.uv) < 1e-4f);
  CHECK(isRed(texelAt(*e, h3.uv)));
  {
    // the AO comes back exactly as it was (not left in the island's layout)
    Image aoAfter = e->doc.tex.aoMap.download(4);
    double diff = 0;
    for (int y = 0; y < aoAfter.height; y += 4)
      for (int x = 0; x < aoAfter.width; x += 4) diff += std::abs(aoAfter.at(x, y)[0] - aoBefore.at(x, y)[0]);
    CHECK(diff / (aoAfter.width / 4.0 * aoAfter.height / 4.0) < 1e-3);
  }
  json dens = run("uv_density");
  CHECK(dens["texels_per_unit"]["fit"].get<float>() > 0);
}

TEST(gpu, fix_overlaps_command) {
  Engine* e = engine();
  if (!e) return;
  run("new_project", {{"primitive", "cube"}, {"resolution", 512}});
  run("uv_unwrap", {{"method", "smart"}});
  auto isl = uv::facesByIsland(e->doc.mesh);
  CHECK(isl.size() >= 2);
  // stack island 1 onto island 0
  Mesh work = e->doc.mesh;
  auto la = uv::loopsOfFaces(work, isl[0]), lb = uv::loopsOfFaces(work, isl[1]);
  uv::translate(work, lb, uv::boundsOfLoops(work, la).center() - uv::boundsOfLoops(work, lb).center());
  e->doc.applyUVs(work.loopUV, false, "test overlap");
  json chk = run("uv_fix_overlaps", {{"check_only", true}});
  CHECK(chk["overlapping_islands"].get<int>() >= 1);
  json r = run("uv_fix_overlaps");
  CHECK(r["overlaps_left"].get<int>() == 0);
}

TEST(gpu, high_poly_bake_normals_ao_ids) {
  Engine* e = engine();
  if (!e) return;
  run("new_project", {{"primitive", "sphere"}, {"resolution", 512}});
  run("set_camera", {{"view", "front"}, {"frame", true}});
  // high poly: a finer sphere with bumps, two material slots (upper / lower half)
  Mesh hi = makeUVSphere(160, 80, 1.f);
  for (auto& p : hi.positions) {
    vec3 n = glm::normalize(p);
    float b = 0.03f * std::sin(n.x * 25.f) * std::sin(n.y * 25.f);
    p = n * (1.f + b);
  }
  hi.faceMaterial.assign(hi.faceCount(), 0);
  hi.finalize();
  for (uint32_t f = 0; f < hi.faceCount(); ++f) hi.faceMaterial[f] = hi.faceCenter(f).y > 0 ? 1 : 0;
  std::string path = tmpFile("patina_test_high.ptmesh"), err;
  CHECK(savePtMesh(path, hi, &err));
  json st = run("bake_from_high_poly", {{"path", path}, {"ao_samples", 16}, {"distance", 0.08}});
  CHECK(st["hit_fraction"].get<float>() > 0.97f);
  CHECK(e->doc.tex.hasHighNormal());
  CHECK(e->doc.tex.hasAO());
  // the final normal map now has detail: not all texels are flat
  e->doc.updateComposite(e->materials);
  Image n = e->doc.tex.finNormal.download(4);
  int bent = 0, cov = 0;
  for (int y = 0; y < n.height; y += 8)
    for (int x = 0; x < n.width; x += 8) {
      const float* p = n.at(x, y);
      ++cov;
      if (p[2] < 0.97f) ++bent;
    }
  CHECK(bent > cov / 10);
  // material slots from the sculpt: top = 1, bottom = 0, usable as a smart mask
  RayHit top = e->doc.bvh.raycast(e->doc.mesh, vec3(0, 3, 0.05f), vec3(0, -1, 0));
  RayHit bottom = e->doc.bvh.raycast(e->doc.mesh, vec3(0, -3, 0.05f), vec3(0, 1, 0));
  CHECK(top.hit && bottom.hit);
  CHECK(std::abs(texelOf(*e, e->doc.tex.matIdMap, top.uv).x * 255.f - 1.f) < 0.5f);
  CHECK(texelOf(*e, e->doc.tex.matIdMap, bottom.uv).x * 255.f < 0.5f);
  run("add_layer", {{"type", "fill"}, {"material", {{"id", ""}, {"color", {1, 0, 0}}}}, {"generator", {{"type", "material"}, {"slot", 1}}}});
  CHECK(isRed(texelAt(*e, top.uv)));
  CHECK(!isRed(texelAt(*e, bottom.uv)));
  // a fill layer over everything keeps the baked detail (normals are combined, not replaced)
  run("add_layer", {{"type", "fill"}, {"material", {{"id", ""}, {"color", {0.8, 0.8, 0.8}}}}});
  e->doc.updateComposite(e->materials);
  {
    Image n2 = e->doc.tex.finNormal.download(4);
    int bent2 = 0;
    for (int y = 0; y < n2.height; y += 8)
      for (int x = 0; x < n2.width; x += 8)
        if (n2.at(x, y)[2] < 0.97f) ++bent2;
    CHECK(bent2 > bent / 2);
  }
  run("undo");
  // survives a save / load
  std::string proj = tmpFile("patina_test_hp.patina");
  run("save_project", {{"path", proj}});
  run("new_project", {{"primitive", "cube"}});
  run("open_file", {{"path", proj}});
  CHECK(e->doc.tex.hasHighNormal());
  CHECK(e->doc.tex.materialIdsFromHigh);
  CHECK(isRed(texelAt(*e, top.uv)));
  run("bake_from_high_poly", {{"clear", true}});
  CHECK(!e->doc.tex.hasHighNormal());
  fs::remove(path);
  fs::remove(proj);
}

// (1.1) the bake-down: the layers of a painted high-poly model moved onto its low-poly version
TEST(gpu, bake_down_moves_layers) {
  Engine* e = engine();
  if (!e) return;
  Document& d = e->doc;
  // the high: a fine sphere with small bumps; the low: a coarse one a little inside it, with other UVs
  Mesh hi = makeUVSphere(128, 64, 1.f);
  for (auto& p : hi.positions) {
    vec3 n = glm::normalize(p);
    p = n * (1.f + 0.01f * std::sin(n.x * 20.f) * std::sin(n.y * 20.f));
  }
  hi.finalize();
  Mesh lo = makeUVSphere(32, 16, 0.995f);
  uv::smartProject(lo, 66.f);
  lo.computeUVTopology();
  lo.computeTangents();
  Mesh away = lo;
  for (auto& p : away.positions) p += vec3(10, 0, 0);
  away.finalize();
  std::string hp = tmpFile("patina_test_bd_high.ptmesh"), lp = tmpFile("patina_test_bd_low.ptmesh"),
              ap = tmpFile("patina_test_bd_away.ptmesh"), err;
  CHECK(savePtMesh(hp, hi, &err) && savePtMesh(lp, lo, &err) && savePtMesh(ap, away, &err));
  run("open_file", {{"path", hp}});
  run("set_resolution", {{"resolution", 512}});
  // paint the high by where its texels are: a color pattern, a mask (the upper half), and a normal leaning toward +x
  auto pattern = [](vec3 p) {
    return vec3(0.5f + 0.4f * std::sin(3.f * p.x), 0.5f + 0.4f * std::sin(3.f * p.y + 1.f), 0.5f + 0.4f * std::sin(3.f * p.z + 2.f));
  };
  const vec3 lean(0.4f, 0.f, 0.f);
  run("add_layer", {{"type", "paint"}});
  run("add_note", {{"text", "the top"}, {"position", {0.0, 1.2, 0.0}}});
  {
    Layer* L = d.selectedLayer();
    CHECK(L != nullptr);
    int R = d.tex.resolution();
    size_t N = size_t(R) * R;
    std::vector<float> pos(N * 4), nrm(N * 4), tan(N * 4);
    d.tex.posMap.downloadRaw(GL_RGBA, GL_FLOAT, pos.data(), pos.size() * 4);
    d.tex.nrmMap.downloadRaw(GL_RGBA, GL_FLOAT, nrm.data(), nrm.size() * 4);
    d.tex.tanMap.downloadRaw(GL_RGBA, GL_FLOAT, tan.data(), tan.size() * 4);
    std::vector<uint8_t> col(N * 4, 0), nm(N * 4, 0), mk(N, 0);
    for (size_t i = 0; i < N; ++i) {
      if (pos[i * 4 + 3] < 0.5f) continue;
      vec3 P(pos[i * 4], pos[i * 4 + 1], pos[i * 4 + 2]);
      vec3 Nn = glm::normalize(vec3(nrm[i * 4], nrm[i * 4 + 1], nrm[i * 4 + 2]));
      vec3 T = vec3(tan[i * 4], tan[i * 4 + 1], tan[i * 4 + 2]);
      T = glm::normalize(T - Nn * glm::dot(Nn, T));
      vec3 B = glm::cross(Nn, T) * (tan[i * 4 + 3] < 0 ? -1.f : 1.f);
      vec3 c = pattern(P);
      vec3 w = glm::normalize(Nn + lean);
      vec3 t(glm::dot(w, T), glm::dot(w, B), glm::dot(w, Nn));
      for (int k = 0; k < 3; ++k) {
        col[i * 4 + k] = (uint8_t)std::lround(c[k] * 255.f);
        nm[i * 4 + k] = (uint8_t)std::lround((t[k] * 0.5f + 0.5f) * 255.f);
      }
      col[i * 4 + 3] = nm[i * 4 + 3] = 255;
      mk[i] = P.y > 0.f ? 255 : 0;
    }
    L->ensureChannel((int)Channel::Color, d.tex.size()).uploadRaw(GL_RGBA, GL_UNSIGNED_BYTE, col.data());
    L->ensureChannel((int)Channel::Normal, d.tex.size()).uploadRaw(GL_RGBA, GL_UNSIGNED_BYTE, nm.data());
    L->ensureMask(d.tex.size()).uploadRaw(GL_RED, GL_UNSIGNED_BYTE, mk.data());
    d.markComposite();
  }
  // a low that is not where the high is: refused, nothing changes
  uint32_t highFaces = d.mesh.faceCount();
  bool refused = false;
  try {
    run("bake_down", {{"path", ap}});
  } catch (const std::exception& ex) {
    refused = std::string(ex.what()).find("same place") != std::string::npos;
  }
  CHECK(refused);
  CHECK(d.mesh.faceCount() == highFaces && d.layers.size() == 2 && d.layers.back()->hasChannel((int)Channel::Normal));
  // the bake-down
  json r = run("bake_down", {{"path", lp}, {"samples", 9}});
  if (!(r["hit_fraction"].get<double>() > 0.97)) std::fprintf(stderr, "  bake_down: %s\n", r.dump().c_str());
  CHECK(r["hit_fraction"].get<double>() > 0.97);
  CHECK(r["guessed_fraction"].get<double>() < 0.005);
  CHECK(r["samples_per_texel"].get<int>() == 9 && r["layers_moved"].get<int>() >= 1);
  auto own = [&] {  // the layers that are not baked maps' (a bake adds those)
    return std::count_if(d.layers.begin(), d.layers.end(), [](auto& l) { return !l->isBaked(); });
  };
  CHECK(d.mesh.faceCount() == lo.faceCount() && own() == 2);
  CHECK(d.highPoly && d.highPoly->faceCount() == highFaces && d.highPolyPath == hp && d.highPolyBaked);
  CHECK(d.projectPath.empty() && !d.undo.canUndo() && d.meshPath == lp);
  CHECK(d.bakeDownCurrent() && d.bakeDownBefore && d.tex.hasHighNormal());
  {
    // each texel of the low holds what the high surface it lies on was painted with
    Bvh hb;
    hb.build(*d.highPoly);
    d.updateComposite(e->materials);
    int R = d.tex.resolution();
    size_t N = size_t(R) * R;
    std::vector<float> pos(N * 4), nrm(N * 4), tan(N * 4), fin(N * 4);
    std::vector<uint8_t> col(N * 4), mk(N);
    d.tex.posMap.downloadRaw(GL_RGBA, GL_FLOAT, pos.data(), pos.size() * 4);
    d.tex.nrmMap.downloadRaw(GL_RGBA, GL_FLOAT, nrm.data(), nrm.size() * 4);
    d.tex.tanMap.downloadRaw(GL_RGBA, GL_FLOAT, tan.data(), tan.size() * 4);
    d.tex.finNormal.downloadRaw(GL_RGBA, GL_FLOAT, fin.data(), fin.size() * 4);
    Layer* L = nullptr;
    for (auto& l : d.layers)
      if (l->hasChannel((int)Channel::Normal)) L = l.get();
    CHECK(L != nullptr);
    L->channels[(int)Channel::Color]->downloadRaw(GL_RGBA, GL_UNSIGNED_BYTE, col.data(), col.size());
    L->mask->downloadRaw(GL_RED, GL_UNSIGNED_BYTE, mk.data(), mk.size());
    double colErr = 0, angErr = 0;
    long n = 0, bad = 0, maskOk = 0, maskN = 0;
    std::vector<float> angles;
    for (size_t i = 0; i < N; i += 7) {
      if (pos[i * 4 + 3] < 0.5f) continue;
      vec3 P(pos[i * 4], pos[i * 4 + 1], pos[i * 4 + 2]);
      RayHit h = hb.closestPoint(*d.highPoly, P);
      if (!h.hit) continue;
      vec3 want = pattern(h.position);
      float er = 0;
      for (int k = 0; k < 3; ++k) er = std::max(er, std::abs(col[i * 4 + k] / 255.f - want[k]));
      colErr += er, ++n;
      if (er > 0.05f) ++bad;
      if (std::abs(h.position.y) > 0.06f) ++maskN, maskOk += (mk[i] > 128) == (h.position.y > 0.f);
      // the final normal, in object space, against the high's (its shading normal leaning toward +x) where the
      // layer shows (its mask: the upper half)
      if (h.position.y < 0.06f) continue;
      const auto& tri = d.highPoly->tris[h.tri];
      vec3 hn = glm::normalize(d.highPoly->loopNormal[tri[0]] * (1 - h.bary.x - h.bary.y) +
                               d.highPoly->loopNormal[tri[1]] * h.bary.x + d.highPoly->loopNormal[tri[2]] * h.bary.y);
      vec3 wantN = glm::normalize(hn + lean);
      vec3 Nn = glm::normalize(vec3(nrm[i * 4], nrm[i * 4 + 1], nrm[i * 4 + 2]));
      vec3 T = vec3(tan[i * 4], tan[i * 4 + 1], tan[i * 4 + 2]);
      T = glm::normalize(T - Nn * glm::dot(Nn, T));
      vec3 B = glm::cross(Nn, T) * (tan[i * 4 + 3] < 0 ? -1.f : 1.f);
      vec3 g = glm::normalize(T * fin[i * 4] + B * fin[i * 4 + 1] + Nn * fin[i * 4 + 2]);
      angles.push_back(std::acos(std::clamp(glm::dot(g, wantN), -1.f, 1.f)) * 57.29578f);
    }
    std::sort(angles.begin(), angles.end());
    double meanAng = 0;
    for (float a : angles) meanAng += a;
    meanAng /= std::max<size_t>(1, angles.size());
    float p95 = angles.empty() ? 180.f : angles[angles.size() * 95 / 100];
    std::fprintf(stderr, "  bake-down: color error %.4f mean, %.2f%% over 0.05; normal %.2f deg mean, %.2f deg p95; mask %.1f%%; %s\n",
                 colErr / std::max(n, 1l), 100.0 * bad / std::max(n, 1l), meanAng, p95, 100.0 * maskOk / std::max(maskN, 1l),
                 r.dump().c_str());
    CHECK(n > 1000);
    CHECK(colErr / n < 0.01);
    CHECK(bad < n / 100);
    CHECK(maskOk > maskN * 98 / 100);
    CHECK(meanAng < 1.5 && p95 < 3.f);
    (void)angErr;
  }
  // the note is on the low's surface
  CHECK(d.notes.size() == 1 && std::abs(glm::length(d.notes[0].position) - 0.995f) < 0.01f && d.notes[0].position.y > 0.9f);
  // the check view and the high in the compare view
  run("set_view", {{"mode", "bake_check"}});
  CHECK(e->viewOptions.mode == ViewMode::BakeCheck);
  run("render_view", {{"width", 64}, {"height", 64}});
  json cmp = run("set_compare", {{"enabled", true}, {"mode", "high_poly"}});
  CHECK(cmp["mode"] == "high_poly" && e->compareReady());
  run("set_compare", {{"enabled", false}, {"mode", "snapshot"}});
  run("set_view", {{"mode", "material"}});
  // a new resolution bakes the high again (from the copy kept), with the distance the bake-down measured
  float dist = r["distance"].get<float>();
  run("set_resolution", {{"resolution", 256}});
  CHECK(d.tex.hasHighNormal() && std::abs(d.highPolyOptions.distance - dist) < 1e-6f);
  // another model drops what the bake-down left to look at
  run("new_project", {{"primitive", "cube"}});
  CHECK(!d.bakeDownCurrent() && !d.bakeDownBefore);
  for (auto& f : {hp, lp, ap}) fs::remove(f);
}

// (1.1) the bake-down looks along the low's normals: two spheres almost touching, each painted its own color, and a
// low shell around each that sits farther out than half the gap between them. The nearest point of the high from the
// side of one shell that faces the other sphere is on the other sphere (its color); the rays find the right one.
TEST(gpu, bake_down_finds_the_facing_surface) {
  Engine* e = engine();
  if (!e) return;
  Document& d = e->doc;
  auto pair = [](float radius, int seg) {
    Mesh m;
    for (int k = 0; k < 2; ++k) {
      Mesh s = makeUVSphere(seg, seg / 2, radius);
      uint32_t v0 = (uint32_t)m.positions.size(), l0 = (uint32_t)m.loopVert.size();
      for (auto p : s.positions) m.positions.push_back(p + vec3(k ? 1.05f : -1.05f, 0, 0));
      if (m.faceStart.empty()) m.faceStart.push_back(0);
      for (uint32_t f = 0; f < s.faceCount(); ++f) m.faceStart.push_back(l0 + s.faceStart[f + 1]);
      for (uint32_t l = 0; l < s.loopCount(); ++l) {
        m.loopVert.push_back(v0 + s.loopVert[l]);
        m.loopUV.push_back(vec2(s.loopUV[l].x * 0.44f + 0.5f * k + 0.03f, s.loopUV[l].y * 0.94f + 0.03f));
      }
    }
    m.faceMaterial.assign(m.faceCount(), 0);
    m.finalize();
    return m;
  };
  Mesh hi = pair(1.f, 96), lo = pair(1.08f, 40);
  std::string hp = tmpFile("patina_test_bd2_high.ptmesh"), lp = tmpFile("patina_test_bd2_low.ptmesh"), err;
  CHECK(savePtMesh(hp, hi, &err) && savePtMesh(lp, lo, &err));
  run("open_file", {{"path", hp}});
  run("set_resolution", {{"resolution", 512}});
  run("add_layer", {{"type", "paint"}});
  {
    int R = d.tex.resolution();
    size_t N = size_t(R) * R;
    std::vector<float> pos(N * 4);
    d.tex.posMap.downloadRaw(GL_RGBA, GL_FLOAT, pos.data(), pos.size() * 4);
    std::vector<uint8_t> col(N * 4, 0);
    for (size_t i = 0; i < N; ++i)
      if (pos[i * 4 + 3] >= 0.5f) {
        bool left = pos[i * 4] < 0.f;
        col[i * 4 + 0] = left ? 230 : 20, col[i * 4 + 2] = left ? 20 : 230, col[i * 4 + 3] = 255;
      }
    d.selectedLayer()->ensureChannel((int)Channel::Color, d.tex.size()).uploadRaw(GL_RGBA, GL_UNSIGNED_BYTE, col.data());
    d.markComposite();
  }
  json r = run("bake_down", {{"path", lp}, {"samples", 4}});
  CHECK(r["guessed_fraction"].get<double>() < 0.005);
  {
    Bvh hb;
    hb.build(*d.highPoly);
    int R = d.tex.resolution();
    size_t N = size_t(R) * R;
    std::vector<float> pos(N * 4);
    std::vector<uint8_t> col(N * 4);
    d.tex.posMap.downloadRaw(GL_RGBA, GL_FLOAT, pos.data(), pos.size() * 4);
    Layer* L = nullptr;
    for (auto& l : d.layers)
      if (l->hasChannel((int)Channel::Color)) L = l.get();
    CHECK(L != nullptr);
    L->channels[(int)Channel::Color]->downloadRaw(GL_RGBA, GL_UNSIGNED_BYTE, col.data(), col.size());
    long inner = 0, right = 0, nearestWrong = 0;
    for (size_t i = 0; i < N; ++i) {
      if (pos[i * 4 + 3] < 0.5f) continue;
      vec3 P(pos[i * 4], pos[i * 4 + 1], pos[i * 4 + 2]);
      bool leftShell = std::abs(glm::length(P - vec3(-1.05f, 0, 0)) - 1.08f) < 0.01f;
      bool rightShell = std::abs(glm::length(P - vec3(1.05f, 0, 0)) - 1.08f) < 0.01f;
      if (leftShell == rightShell) continue;
      if (!leftShell && !rightShell) continue;
      if (std::abs(P.x) > 0.06f || std::abs(P.y) > 0.25f || std::abs(P.z) > 0.25f) continue;  // the sides facing each other
      ++inner;
      bool red = col[i * 4] > col[i * 4 + 2];
      right += red == leftShell;
      RayHit c = hb.closestPoint(*d.highPoly, P);
      nearestWrong += c.hit && (c.position.x < 0.f) != leftShell;
    }
    std::fprintf(stderr, "  facing: %ld texels where the shells face each other, %.1f%% right; the nearest point would get %.1f%% wrong\n",
                 inner, 100.0 * right / std::max(inner, 1l), 100.0 * nearestWrong / std::max(inner, 1l));
    CHECK(inner > 50);
    CHECK(nearestWrong > inner / 4);  // the case is a hard one
    CHECK(right > inner * 97 / 100);
  }
  for (auto& f : {hp, lp}) fs::remove(f);
}

TEST(gpu, facing_mask_and_material_fill) {
  Engine* e = engine();
  if (!e) return;
  run("new_project", {{"primitive", "sphere"}, {"resolution", 512}});
  RayHit top = e->doc.bvh.raycast(e->doc.mesh, vec3(0.05f, 3, 0.05f), vec3(0, -1, 0));
  RayHit side = e->doc.bvh.raycast(e->doc.mesh, vec3(3, 0.05f, 0.05f), vec3(-1, 0, 0));
  run("add_layer", {{"type", "fill"}, {"material", {{"id", ""}, {"color", {1, 0, 0}}}},
                    {"generator", {{"type", "facing"}, {"direction", {0, 1, 0}}, {"level", 0.8}, {"contrast", 0.9}}}});
  CHECK(isRed(texelAt(*e, top.uv)));
  CHECK(!isRed(texelAt(*e, side.uv)));
  // mask from selection: only the selected faces show the fill
  run("remove_layer", {{"index", e->doc.selected}});
  run("add_layer", {{"type", "fill"}, {"material", {{"id", ""}, {"color", {1, 0, 0}}}}});
  run("select_faces", {{"mode", "set"}, {"faces", {side.face}}});
  run("set_layer_mask", {{"from_selection", true}});
  CHECK(isRed(texelAt(*e, side.uv)));
  CHECK(!isRed(texelAt(*e, top.uv)));
  run("undo");
  CHECK(isRed(texelAt(*e, top.uv)));  // no mask again: the fill covers everything
}

TEST(gpu, render_image_incremental_save_export_again) {
  Engine* e = engine();
  if (!e) return;
  freshSphere(*e);
  std::string png = tmpFile("patina_test_render.png");
  json r = run("render_image", {{"path", png}, {"width", 320}, {"height", 200}, {"transparent", true}});
  CHECK(r["files"].size() == 1);
  Image img;
  {
    std::ifstream f(png, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), {});
    CHECK(decodeImage(bytes.data(), bytes.size(), img, 4));
  }
  CHECK(img.width == 320 && img.height == 200);
  CHECK(img.at(2, 2)[3] < 0.01f);                    // background is transparent
  CHECK(img.at(160, 100)[3] > 0.99f);                // the model is not
  json t = run("render_image", {{"path", tmpFile("patina_test_turn.png")}, {"width", 64}, {"height", 64}, {"turntable_frames", 4}});
  CHECK(t["files"].size() == 4);
  for (auto& f : t["files"]) fs::remove(f.get<std::string>());
  fs::remove(png);
  // incremental save
  fs::path dir = fs::temp_directory_path() / "patina_test_inc";
  fs::remove_all(dir);
  fs::create_directories(dir);
  run("save_project", {{"path", (dir / "crate.patina").string()}});
  json s1 = run("save_incremental");
  CHECK(fs::path(s1["path"].get<std::string>()).filename() == "crate_v002.patina");
  json s2 = run("save_incremental");
  CHECK(fs::path(s2["path"].get<std::string>()).filename() == "crate_v003.patina");
  // export again
  bool threw = false;
  e->doc.hasLastExport = false;
  try {
    run("export_again");
  } catch (std::exception&) {
    threw = true;
  }
  CHECK(threw);
  run("export_textures", {{"directory", (dir / "tex").string()}, {"preset", "gltf"}, {"size", 128}});
  fs::remove_all(dir / "tex");
  json again = run("export_again");
  CHECK(fs::exists(again["files"]["basecolor"].get<std::string>()));
  fs::remove_all(dir);
}

// two separate 4x4 planes side by side (y = 0, facing up), each its own UV island
static Mesh twoPlanes() {
  Mesh m;
  m.name = "TwoPlanes";
  m.faceStart = {0};
  const int n = 4;
  for (int side = 0; side < 2; ++side) {
    float x0 = side == 0 ? -1.05f : 0.05f;
    float u0 = side == 0 ? 0.02f : 0.54f;
    uint32_t base = (uint32_t)m.positions.size();
    for (int y = 0; y <= n; ++y)
      for (int x = 0; x <= n; ++x) m.positions.push_back({x0 + float(x) / n, 0.f, 0.5f - float(y) / n});
    auto uv = [&](int x, int y) { return vec2(u0 + 0.44f * float(x) / n, 0.25f + 0.45f * float(y) / n); };
    for (int y = 0; y < n; ++y)
      for (int x = 0; x < n; ++x) {
        uint32_t a = base + y * (n + 1) + x, b = a + 1, c = a + n + 2, d = a + n + 1;
        m.loopVert.insert(m.loopVert.end(), {a, b, c, d});
        m.loopUV.insert(m.loopUV.end(), {uv(x, y), uv(x + 1, y), uv(x + 1, y + 1), uv(x, y + 1)});
        m.faceStart.push_back((uint32_t)m.loopVert.size());
      }
  }
  m.finalize();
  return m;
}

TEST(gpu, topology_and_island_lock) {
  Engine* e = engine();
  if (!e) return;
  std::string path = tmpFile("patina_test_two_planes.ptmesh"), err;
  CHECK(savePtMesh(path, twoPlanes(), &err));
  run("open_file", {{"path", path}});
  run("set_resolution", {{"resolution", 512}});
  run("set_camera", {{"view", "top"}, {"frame", true}});
  run("set_tool", {{"tool", "paint"}});
  run("set_brush", {{"preset", "basic_hard"}, {"size", 30}, {"hardness", 0.9}, {"flow", 1.0}, {"opacity", 1.0},
                    {"spacing", 0.1}, {"projection", "screen"}, {"lock_topology", false}, {"lock_island", false}});
  run("set_paint_material", {{"id", ""}, {"color", {1, 0, 0}}});
  RayHit l = e->pick({0.3f * 512, 256}), r = e->pick({0.7f * 512, 256});
  CHECK(l.hit && r.hit);
  CHECK(l.uv.x < 0.5f && r.uv.x > 0.5f);
  json stroke = {{"points", {{0.25, 0.5}, {0.5, 0.5}, {0.75, 0.5}}}};
  // no lock: the stroke crosses the gap and paints both planes
  run("paint_stroke", stroke);
  CHECK(isRed(texelAt(*e, l.uv)));
  CHECK(isRed(texelAt(*e, r.uv)));
  run("undo");
  CHECK(!isRed(texelAt(*e, r.uv)));
  // topology lock: only the plane the stroke started on
  json st = run("set_brush", {{"lock_topology", true}});
  CHECK(st["lock_topology"].get<bool>());
  run("paint_stroke", stroke);
  CHECK(isRed(texelAt(*e, l.uv)));
  CHECK(!isRed(texelAt(*e, r.uv)));
  CHECK(e->doc.paintLockFaces.empty());  // released after the stroke
  run("undo");
  // with X symmetry the mirrored part is unlocked too
  run("set_symmetry", {{"x", true}});
  run("paint_stroke", {{"points", {{0.22, 0.5}, {0.32, 0.5}}}});
  CHECK(isRed(texelAt(*e, l.uv)));
  CHECK(isRed(texelAt(*e, r.uv)));
  run("set_symmetry", {{"x", false}});
  run("undo");
  run("set_brush", {{"lock_topology", false}});

  // island lock on a cube: one face per UV island, all connected
  run("new_project", {{"primitive", "cube"}, {"resolution", 512}});
  run("set_camera", {{"view", "front"}, {"frame", true}});
  run("set_camera", {{"yaw", 0.6}, {"pitch", 0.2}});
  run("set_paint_material", {{"id", ""}, {"color", {1, 0, 0}}});
  RayHit a = e->pick({0.36f * 512, 256}), b = e->pick({0.64f * 512, 256});
  CHECK(a.hit && b.hit && a.face != b.face);
  const Mesh& m = e->doc.mesh;
  CHECK(m.loopIsland[m.faceStart[a.face]] != m.loopIsland[m.faceStart[b.face]]);
  json across = {{"points", {{0.36, 0.5}, {0.5, 0.5}, {0.64, 0.5}}}};
  run("paint_stroke", across);
  CHECK(isRed(texelAt(*e, a.uv)) && isRed(texelAt(*e, b.uv)));
  run("undo");
  run("set_brush", {{"lock_topology", true}});  // the cube is one piece: no effect
  run("paint_stroke", across);
  CHECK(isRed(texelAt(*e, a.uv)) && isRed(texelAt(*e, b.uv)));
  run("undo");
  run("set_brush", {{"lock_topology", false}, {"lock_island", true}});
  run("paint_stroke", across);
  CHECK(isRed(texelAt(*e, a.uv)));
  CHECK(!isRed(texelAt(*e, b.uv)));
  // the lock follows the start point: starting on the other face paints only that one
  run("undo");
  run("paint_stroke", {{"points", {{0.64, 0.5}, {0.5, 0.5}, {0.36, 0.5}}}});
  CHECK(!isRed(texelAt(*e, a.uv)));
  CHECK(isRed(texelAt(*e, b.uv)));
  run("undo");
  // UV-space strokes lock to the island too
  vec2 ua = a.uv, ub = b.uv;
  run("paint_stroke", {{"space", "uv"}, {"points", {{ua.x, ua.y}, {ub.x, ub.y}}}});
  CHECK(isRed(texelAt(*e, ua)));
  CHECK(!isRed(texelAt(*e, ub)));
  run("undo");
  run("set_brush", {{"lock_island", false}});
  fs::remove(path);
}

// ---------------------------------------------------------------- 0.5: Substance / packed map import
#include <tinyexr.h>

static void writeSolid(const fs::path& p, int ch, std::initializer_list<float> v, int bits = 8) {
  Image img;
  img.resize(8, 8, ch);
  std::vector<float> vals(v);
  for (int i = 0; i < 64; ++i)
    for (int c = 0; c < ch; ++c) img.pixels[i * ch + c] = vals[std::min<size_t>(c, vals.size() - 1)];
  CHECK(saveImage(p.string(), img, bits));
}
static float firstPixel(const std::string& path) {
  Image img;
  if (!loadImage(path, img, 1)) return -1.f;
  return img.pixels[0];
}

TEST(gpu, substance_exports_import) {
  Engine* e = engine();
  if (!e) return;
  fs::path root = fs::temp_directory_path() / "patina_test_substance";
  fs::remove_all(root);
  // 1. Substance "Unreal Engine 4 (Packed)" preset: ORM + DirectX normal named just "Normal"
  fs::path ue = root / "Crate_UE";
  fs::create_directories(ue);
  writeSolid(ue / "Crate_BaseColor.png", 3, {0.9f, 0.1f, 0.1f});
  writeSolid(ue / "Crate_OcclusionRoughnessMetallic.png", 3, {0.5f, 0.8f, 1.0f});
  writeSolid(ue / "Crate_Normal.png", 3, {0.5f, 0.5f, 1.0f});
  writeSolid(ue / "Crate_Height.png", 1, {0.5f}, 16);
  json r = run("import_material_folder", {{"path", ue.string()}, {"name", "zz test crate"}});
  CHECK(r["materials"].size() == 1);
  json m = r["materials"][0];
  CHECK(m["normal_directx"].get<bool>());
  CHECK(m["maps"].contains("roughness") && m["maps"].contains("metallic") && m["maps"].contains("ao"));
  CHECK(std::abs(firstPixel(m["maps"]["roughness"]) - 0.8f) < 0.01f);
  CHECK(std::abs(firstPixel(m["maps"]["metallic"]) - 1.0f) < 0.01f);
  CHECK(std::abs(firstPixel(m["maps"]["ao"]) - 0.5f) < 0.01f);
  CHECK(m["from"]["roughness"].get<std::string>().find("(G)") != std::string::npos);
  // it paints: fill a plane with it and read back the color
  run("new_project", {{"primitive", "plane"}, {"resolution", 256}});
  run("set_paint_material", {{"id", m["id"]}, {"mapping", "uv"}});
  run("fill", {{"mode", "mesh"}});
  vec4 c = texelAt(*e, vec2(0.5f));
  CHECK(c.x > 0.7f && c.y < 0.25f);
  {
    e->doc.updateComposite(e->materials);
    float rmh[4];
    glGetTextureSubImage(e->doc.tex.finRMH.id(), 0, 128, 128, 0, 1, 1, 1, GL_RGBA, GL_FLOAT, sizeof rmh, rmh);
    CHECK(std::abs(rmh[0] - 0.8f) < 0.06f);  // roughness from the G channel of the ORM map
    CHECK(rmh[1] > 0.9f);                     // metallic from B
  }

  // 2. Substance Painter export with two texture sets in one folder
  fs::path sp = root / "Robot";
  fs::create_directories(sp);
  writeSolid(sp / "Robot_Body_BaseColor.png", 3, {0.2f, 0.4f, 0.8f});
  writeSolid(sp / "Robot_Body_Roughness.png", 1, {0.3f});
  writeSolid(sp / "Robot_Body_Normal_OpenGL.png", 3, {0.5f, 0.5f, 1.0f});
  writeSolid(sp / "Robot_Head_BaseColor.png", 3, {0.8f, 0.8f, 0.2f});
  writeSolid(sp / "Robot_Head_Metallic.png", 1, {1.0f});
  writeSolid(sp / "Robot_Head_Normal_DirectX.png", 3, {0.5f, 0.5f, 1.0f});
  r = run("import_material_folder", {{"path", sp.string()}, {"name", "zz test robot"}});
  CHECK(r["materials"].size() == 2);
  bool body = false, head = false;
  for (auto& mm : r["materials"]) {
    std::string n = mm["name"];
    if (n.find("Body") != std::string::npos) body = mm["maps"].contains("roughness") && !mm["normal_directx"].get<bool>();
    if (n.find("Head") != std::string::npos) head = mm["maps"].contains("metallic") && mm["normal_directx"].get<bool>();
  }
  CHECK(body && head);

  // 3. Unity HDRP MaskMap (R metallic, G AO, A smoothness) and glTF metallicRoughness
  fs::path un = root / "Unity";
  fs::create_directories(un);
  writeSolid(un / "Barrel_BaseMap.png", 3, {0.5f, 0.5f, 0.5f});
  writeSolid(un / "Barrel_MaskMap.png", 4, {1.0f, 0.6f, 0.0f, 0.25f});
  r = run("import_material_folder", {{"path", un.string()}, {"name", "zz test barrel"}});
  m = r["materials"][0];
  CHECK(m["maps"].contains("basecolor"));
  CHECK(std::abs(firstPixel(m["maps"]["roughness"]) - 0.75f) < 0.01f);
  CHECK(std::abs(firstPixel(m["maps"]["metallic"]) - 1.0f) < 0.01f);
  fs::path gl = root / "Gltf";
  fs::create_directories(gl);
  writeSolid(gl / "Helmet_baseColor.png", 3, {0.5f, 0.5f, 0.5f});
  writeSolid(gl / "Helmet_metallicRoughness.png", 3, {0.0f, 0.4f, 0.7f});
  r = run("import_material_folder", {{"path", gl.string()}, {"name", "zz test helmet"}});
  m = r["materials"][0];
  CHECK(std::abs(firstPixel(m["maps"]["roughness"]) - 0.4f) < 0.01f);
  CHECK(std::abs(firstPixel(m["maps"]["metallic"]) - 0.7f) < 0.01f);
  // Poly Haven ARM next to "_arm" lookalikes
  fs::path ph = root / "PolyHaven";
  fs::create_directories(ph);
  writeSolid(ph / "rusty_metal_diff_2k.png", 3, {0.6f, 0.3f, 0.1f});
  writeSolid(ph / "rusty_metal_arm_2k.png", 3, {1.0f, 0.55f, 0.9f});
  writeSolid(ph / "rusty_metal_nor_gl_2k.png", 3, {0.5f, 0.5f, 1.0f});
  r = run("import_material_folder", {{"path", ph.string()}, {"name", "zz test rusty"}});
  m = r["materials"][0];
  CHECK(r["materials"].size() == 1);
  CHECK(std::abs(firstPixel(m["maps"]["roughness"]) - 0.55f) < 0.01f);
  CHECK(!m["normal_directx"].get<bool>());

  // 4. EXR maps: linear color and a height in scene units
  fs::path ex = root / "Exr";
  fs::create_directories(ex);
  {
    std::vector<float> col(16 * 16 * 3, 0.5f), h(16 * 16 * 3);
    for (int i = 0; i < 16 * 16; ++i) h[i * 3] = h[i * 3 + 1] = h[i * 3 + 2] = -2.f + 5.f * float(i % 16) / 15.f;
    const char* exrErr = nullptr;
    CHECK(SaveEXR(col.data(), 16, 16, 3, 0, (ex / "Wall_BaseColor.exr").string().c_str(), &exrErr) == TINYEXR_SUCCESS);
    CHECK(SaveEXR(h.data(), 16, 16, 3, 0, (ex / "Wall_Height.exr").string().c_str(), &exrErr) == TINYEXR_SUCCESS);
    Image back;
    CHECK(loadImage((ex / "Wall_Height.exr").string(), back, 1));
    CHECK(back.width == 16 && std::abs(back.pixels[15] - 3.f) < 1e-3f);
  }
  r = run("import_material_folder", {{"path", ex.string()}, {"name", "zz test wall"}});
  m = r["materials"][0];
  CHECK(m["maps"].contains("basecolor") && m["maps"].contains("height"));
  run("set_paint_material", {{"id", m["id"]}, {"mapping", "uv"}});
  run("fill", {{"mode", "mesh"}});
  c = texelAt(*e, vec2(0.5f));
  CHECK(c.x > 0.6f && c.x < 0.85f);  // linear 0.5 shown as sRGB ~0.74

  // 5. Substance files Patina cannot read say why
  fs::path sb = root / "thing.sbsar";
  { std::ofstream(sb) << "x"; }
  bool threw = false;
  try {
    run("open_file", {{"path", sb.string()}});
  } catch (std::exception& ex2) {
    threw = std::string(ex2.what()).find("Substance") != std::string::npos;
  }
  CHECK(threw);

  std::vector<std::string> ids;
  for (auto& mm : e->materials.entries())
    if (mm->def.name.rfind("zz test", 0) == 0) ids.push_back(mm->def.id);
  for (auto& id : ids) e->materials.removeUserMaterial(id);
  fs::remove_all(root);
}

// ---------------------------------------------------------------- 0.6: mask effect stacks
TEST(gpu, mask_effect_stack) {
  Engine* e = engine();
  if (!e) return;
  run("new_project", {{"primitive", "sphere"}, {"resolution", 512}});
  RayHit top = e->doc.bvh.raycast(e->doc.mesh, vec3(0, 3, 0.05f), vec3(0, -1, 0));
  RayHit bottom = e->doc.bvh.raycast(e->doc.mesh, vec3(0, -3, 0.05f), vec3(0, 1, 0));
  RayHit side = e->doc.bvh.raycast(e->doc.mesh, vec3(3, 0.02f, 0.05f), vec3(-1, 0, 0));
  CHECK(top.hit && bottom.hit && side.hit);
  // a red fill with the height gradient as its simple smart mask: red on top
  run("add_layer", {{"type", "fill"}, {"material", {{"id", ""}, {"color", {1, 0, 0}}}},
                    {"generator", {{"type", "gradient_y"}, {"level", 0.5}, {"contrast", 1.0}}}});
  CHECK(isRed(texelAt(*e, top.uv)) && !isRed(texelAt(*e, bottom.uv)));
  vec4 before = texelAt(*e, side.uv);
  // converting to a stack (adding a no-op fill at opacity 0) keeps the result
  json st = run("add_mask_effect", {{"effect", {{"type", "fill"}, {"value", 0.0}, {"opacity", 0.0}}}});
  CHECK(st.size() == 3 && st[0]["type"] == "paint" && st[1]["type"] == "generator" && st[1]["blend"] == "multiply");
  CHECK(e->doc.selectedLayer()->generator.type == MaskGenerator::None);
  CHECK(isRed(texelAt(*e, top.uv)) && !isRed(texelAt(*e, bottom.uv)));
  CHECK(glm::length(texelAt(*e, side.uv) - before) < 0.02f);
  // invert flips it
  run("add_mask_effect", {{"effect", {{"type", "invert"}}}});
  CHECK(!isRed(texelAt(*e, top.uv)) && isRed(texelAt(*e, bottom.uv)));
  // a replacing fill of 1 on top: red everywhere; at half opacity: half way
  run("add_mask_effect", {{"effect", {{"type", "fill"}, {"value", 1.0}}}});
  CHECK(isRed(texelAt(*e, top.uv)) && isRed(texelAt(*e, bottom.uv)));
  json fx = e->doc.selectedLayer()->toJson()["mask_effects"];
  fx.back()["opacity"] = 0.5;
  run("update_layer", {{"mask_effects", fx}});
  vec4 half = texelAt(*e, top.uv);  // top: invert gave 0, fill 1 at 50% -> 0.5
  CHECK(half.x > 0.75f && half.y > 0.25f && half.y < 0.65f);
  // levels: everything above 0.4 to white
  fx.push_back({{"type", "levels"}, {"in_low", 0.0}, {"in_high", 0.4}});
  run("update_layer", {{"mask_effects", fx}});
  CHECK(isRed(texelAt(*e, top.uv)));
  // a painted black mask, multiplied on top, hides it all
  int li = e->doc.selected;
  e->doc.addMask(li, 0.f);
  fx.push_back({{"type", "paint"}, {"blend", "multiply"}});
  run("update_layer", {{"mask_effects", fx}});
  CHECK(!isRed(texelAt(*e, top.uv)) && !isRed(texelAt(*e, side.uv)));
  // blur softens a hard edge: gradient at full contrast, then blur
  run("update_layer", {{"mask_effects", json::array({{{"type", "generator"},
                                                       {"generator", {{"type", "gradient_y"}, {"level", 0.5}, {"contrast", 1.0}}}}})}});
  RayHit eq = e->doc.bvh.raycast(e->doc.mesh, vec3(3, 0.0f, 0.05f), vec3(-1, 0, 0));
  float sharp = texelAt(*e, eq.uv).y;
  run("update_layer", {{"mask_effects", json::array({{{"type", "generator"},
                                                       {"generator", {{"type", "gradient_y"}, {"level", 0.5}, {"contrast", 1.0}}}},
                                                      {{"type", "blur"}, {"radius", 24.0}}})}});
  float soft = texelAt(*e, eq.uv).y;
  CHECK(std::abs(soft - sharp) > 0.03f || (soft > 0.1f && soft < 0.9f));
  // undo goes back to the previous stack, save / load keeps it
  run("undo");
  CHECK(e->doc.selectedLayer()->maskEffects.size() == 1);
  run("redo");
  CHECK(e->doc.selectedLayer()->maskEffects.size() == 2);
  std::string proj = tmpFile("patina_test_maskfx.patina");
  run("save_project", {{"path", proj}});
  run("new_project", {{"primitive", "cube"}});
  run("open_file", {{"path", proj}});
  bool found = false;
  for (auto& l : e->doc.layers)
    if (l->maskEffects.size() == 2 && l->maskEffects[1].type == MaskEffect::Blur && std::abs(l->maskEffects[1].radius - 24.f) < 1e-3f)
      found = true;
  CHECK(found);
  // removing the stack: [] restores the simple form
  run("update_layer", {{"mask_effects", json::array()}});
  CHECK(e->doc.selectedLayer()->maskEffects.empty());
  fs::remove(proj);
}

TEST(gpu, layer_instances) {
  Engine* e = engine();
  if (!e) return;
  freshSphere(*e);  // one paint layer selected, red brush
  RayHit a = e->pick({256, 256}), b = e->pick({256, 160});
  CHECK(a.hit && b.hit);
  run("paint_stroke", {{"points", {{0.45, 0.5}, {0.55, 0.5}}}});
  CHECK(isRed(texelAt(*e, a.uv)));
  int src = e->doc.selected;
  uint32_t srcUid = e->doc.layers[src]->uid;
  json r = run("add_instance");
  int inst = r["index"];
  CHECK(e->doc.layers[inst]->instanceOf == srcUid);
  // hide the source: the instance still shows its paint
  run("update_layer", {{"index", src}, {"visible", false}});
  CHECK(isRed(texelAt(*e, a.uv)));
  // the instance's own opacity
  run("update_layer", {{"index", inst}, {"opacity", 0.0}});
  CHECK(!isRed(texelAt(*e, a.uv)));
  run("update_layer", {{"index", inst}, {"opacity", 1.0}});
  // painting the instance paints the source
  run("select_layer", {{"index", inst}});
  run("paint_stroke", {{"points", {{0.45, 160.0 / 512.0}, {0.55, 160.0 / 512.0}}}});
  CHECK(!e->doc.layers[inst]->hasChannel(0));
  run("update_layer", {{"index", inst}, {"visible", false}});
  run("update_layer", {{"index", src}, {"visible", true}});
  CHECK(isRed(texelAt(*e, b.uv)));  // the source has the new stroke
  run("update_layer", {{"index", inst}, {"visible", true}});
  // deleting the source keeps the content in the instance
  run("remove_layer", {{"index", src}});
  int now = -1;
  for (int i = 0; i < (int)e->doc.layers.size(); ++i)
    if (e->doc.layers[i]->name.find("(instance)") != std::string::npos) now = i;
  CHECK(now >= 0 && e->doc.layers[now]->instanceOf == 0 && e->doc.layers[now]->hasChannel(0));
  CHECK(isRed(texelAt(*e, a.uv)) && isRed(texelAt(*e, b.uv)));
  // undo brings the source back, and the instance follows it again
  run("undo");
  bool linked = false;
  for (auto& l : e->doc.layers)
    if (l->instanceOf == srcUid) linked = true;
  CHECK(linked);
  CHECK(isRed(texelAt(*e, a.uv)));
  // save / load
  std::string proj = tmpFile("patina_test_instances.patina");
  run("save_project", {{"path", proj}});
  run("new_project", {{"primitive", "cube"}});
  run("open_file", {{"path", proj}});
  linked = false;
  for (auto& l : e->doc.layers)
    if (l->instanceOf) linked = e->doc.findLayer(l->instanceOf) != nullptr;
  CHECK(linked);
  // make independent: a copy, no link
  for (int i = 0; i < (int)e->doc.layers.size(); ++i)
    if (e->doc.layers[i]->instanceOf) {
      run("make_independent", {{"index", i}});
      CHECK(e->doc.layers[i]->instanceOf == 0 && e->doc.layers[i]->hasChannel(0));
    }
  fs::remove(proj);
}

// ---------------------------------------------------------------- 0.6: smudge and seam-crossing blur
TEST(gpu, smudge_drags_paint) {
  Engine* e = engine();
  if (!e) return;
  run("new_project", {{"primitive", "plane"}, {"resolution", 512}});
  run("set_camera", {{"view", "top"}, {"frame", true}});
  run("set_tool", {{"tool", "paint"}});
  run("set_brush", {{"preset", "basic_hard"}, {"size", 40}, {"hardness", 0.9}, {"flow", 1.0}, {"opacity", 1.0},
                    {"spacing", 0.05}, {"projection", "screen"}, {"lock_topology", false}, {"lock_island", false}});
  run("set_paint_material", {{"id", ""}, {"color", {1, 0, 0}}});
  run("paint_stroke", {{"points", {{0.4, 0.5}}}});
  RayHit start = e->pick({0.4f * 512, 256}), mid = e->pick({0.5f * 512, 256}), far = e->pick({0.75f * 512, 256});
  CHECK(start.hit && mid.hit && far.hit);
  CHECK(isRed(texelAt(*e, start.uv)));
  CHECK(!isRed(texelAt(*e, mid.uv)));
  json st = run("set_tool", {{"tool", "smudge"}});
  CHECK(run("get_state")["tool"] == "smudge");
  json r = run("paint_stroke", {{"points", {{0.4, 0.5}, {0.6, 0.5}}}, {"brush", {{"size", 50}, {"hardness", 0.6}}}});
  CHECK(r.value("dabs", 0) > 5);
  vec4 m = texelAt(*e, mid.uv);
  if (!(m.x > m.y + 0.25f)) std::fprintf(stderr, "  smudged texel (%.2f,%.2f,%.2f)\n", m.x, m.y, m.z);
  CHECK(m.x > m.y + 0.25f);                 // paint was carried along the stroke
  CHECK(!isRed(texelAt(*e, far.uv)));       // but not past its end
  CHECK(e->doc.undo.undoLabel() == "Smudge");
  run("undo");
  CHECK(!isRed(texelAt(*e, mid.uv)));
  CHECK(isRed(texelAt(*e, start.uv)));
  // smudging the other way does nothing to the far texel and keeps the dot
  run("paint_stroke", {{"points", {{0.6, 0.5}, {0.45, 0.5}}}, {"brush", {{"size", 50}}}});
  CHECK(!isRed(texelAt(*e, far.uv)));
  run("undo");
  // UV-space smudge (UV editor strokes)
  vec2 a = start.uv, b = start.uv + (mid.uv - start.uv) * 2.f;
  run("paint_stroke", {{"space", "uv"}, {"points", {{a.x, a.y}, {b.x, b.y}}}});
  vec4 mu = texelAt(*e, mid.uv);
  CHECK(mu.x > mu.y + 0.25f);
  run("undo");
  run("set_tool", {{"tool", "paint"}});
}

// one plane (shared vertices) cut into two UV islands at x = 0
static Mesh seamPlane() {
  Mesh m;
  m.name = "SeamPlane";
  m.faceStart = {0};
  const int nx = 8, ny = 4;
  for (int y = 0; y <= ny; ++y)
    for (int x = 0; x <= nx; ++x) m.positions.push_back({-1.f + 2.f * float(x) / nx, 0.f, 1.f - 2.f * float(y) / ny});
  auto uv = [&](int x, int y, bool right) {
    float u = right ? 0.55f + 0.4f * float(x - nx / 2) / (nx / 2) : 0.05f + 0.4f * float(x) / (nx / 2);
    return vec2(u, 0.1f + 0.8f * float(y) / ny);
  };
  for (int y = 0; y < ny; ++y)
    for (int x = 0; x < nx; ++x) {
      bool right = x >= nx / 2;
      uint32_t a = y * (nx + 1) + x, b = a + 1, c = a + nx + 2, d = a + nx + 1;
      m.loopVert.insert(m.loopVert.end(), {a, b, c, d});
      m.loopUV.insert(m.loopUV.end(), {uv(x, y, right), uv(x + 1, y, right), uv(x + 1, y + 1, right), uv(x, y + 1, right)});
      m.faceStart.push_back((uint32_t)m.loopVert.size());
    }
  m.finalize();
  return m;
}

TEST(gpu, blur_and_smudge_cross_uv_seams) {
  Engine* e = engine();
  if (!e) return;
  std::string path = tmpFile("patina_test_seam_plane.ptmesh"), err;
  CHECK(savePtMesh(path, seamPlane(), &err));
  run("open_file", {{"path", path}});
  run("set_resolution", {{"resolution", 512}});
  CHECK(e->doc.mesh.islandCount == 2);
  run("set_camera", {{"view", "top"}, {"frame", true}});
  run("set_tool", {{"tool", "paint"}});
  run("set_brush", {{"preset", "basic_hard"}, {"size", 400}, {"hardness", 1.0}, {"flow", 1.0}, {"opacity", 1.0},
                    {"spacing", 0.1}, {"projection", "screen"}, {"lock_topology", false}, {"lock_island", true}});
  run("set_paint_material", {{"id", ""}, {"color", {1, 0, 0}}});
  // the right half (island B) red up to the seam, the left half (island A) untouched
  run("paint_stroke", {{"points", {{0.75, 0.5}, {0.6, 0.5}}}});
  run("set_brush", {{"lock_island", false}});
  RayHit a = e->pick({256 - 4, 256}), b = e->pick({256 + 4, 256});
  CHECK(a.hit && b.hit);
  CHECK(a.uv.x < 0.5f && b.uv.x > 0.5f);
  CHECK(isRed(texelAt(*e, b.uv)));
  vec4 before = texelAt(*e, a.uv);
  CHECK(!isRed(before));
  // the seam map: gutter texels just right of island A's edge point into island B
  const Texture2D& sm = e->doc.tex.ensureSeamMap(e->doc.mesh, e->doc.bvh);
  {
    int R = e->doc.tex.resolution();
    int gx = int(0.45f * R) + 2, gy = R / 2;
    float v[2];
    glGetTextureSubImage(sm.id(), 0, gx, gy, 0, 1, 1, 1, GL_RG, GL_FLOAT, sizeof v, v);
    if (!(v[0] > 0.55f && v[0] < 0.58f)) std::fprintf(stderr, "  seam map at gutter: (%.3f, %.3f)\n", v[0], v[1]);
    CHECK(v[0] > 0.55f && v[0] < 0.58f);
    CHECK(std::abs(v[1] - 0.5f) < 0.02f);
  }
  // blur without seams: island A's edge only sees its own gray
  json stroke = {{"points", {{0.47, 0.45}, {0.47, 0.55}}}, {"brush", {{"size", 30}, {"hardness", 0.8}}}};
  run("set_tool", {{"tool", "blur"}, {"blur_radius", 12}, {"seam_aware", false}});
  run("paint_stroke", stroke);
  vec4 plain = texelAt(*e, a.uv);
  CHECK(std::abs(plain.y - before.y) < 0.03f);
  run("undo");
  // with seams the red on the other island bleeds across
  run("set_tool", {{"tool", "blur"}, {"seam_aware", true}});
  run("paint_stroke", stroke);
  vec4 across = texelAt(*e, a.uv);
  if (!(across.y < before.y - 0.08f)) std::fprintf(stderr, "  seam blur (%.2f,%.2f,%.2f) before g %.2f\n", across.x, across.y, across.z, before.y);
  CHECK(across.y < before.y - 0.08f);
  CHECK(across.x > across.y);
  run("undo");
  // smudging from island B over the seam onto island A carries the red across
  run("set_tool", {{"tool", "smudge"}});
  run("paint_stroke", {{"points", {{0.56, 0.5}, {0.44, 0.5}}}, {"brush", {{"size", 40}, {"hardness", 0.6}}}});
  vec4 sm2 = texelAt(*e, a.uv);
  if (!(sm2.x > sm2.y + 0.25f)) std::fprintf(stderr, "  seam smudge (%.2f,%.2f,%.2f)\n", sm2.x, sm2.y, sm2.z);
  CHECK(sm2.x > sm2.y + 0.25f);
  run("undo");
  run("set_tool", {{"tool", "paint"}});
  fs::remove(path);
}

TEST(gpu, stencil_mask_decal_and_text) {
  Engine* e = engine();
  if (!e) return;
  run("new_project", {{"primitive", "plane"}, {"resolution", 512}});
  run("set_camera", {{"view", "top"}, {"frame", true}});
  run("set_tool", {{"tool", "paint"}});
  run("set_brush", {{"preset", "basic_hard"}, {"size", 60}, {"hardness", 0.9}, {"flow", 1.0}, {"opacity", 1.0},
                    {"spacing", 0.1}, {"projection", "screen"}, {"lock_topology", false}, {"lock_island", false}});
  run("set_paint_material", {{"id", ""}, {"color", {1, 0, 0}}});
  // a stencil: left half white (paint gets through), right half black; left red / right blue as a decal
  Image img;
  img.resize(64, 32, 4);
  for (int y = 0; y < 32; ++y)
    for (int x = 0; x < 64; ++x) {
      float* p = img.at(x, y);
      bool left = x < 32;
      p[0] = left ? 1.f : 0.f, p[1] = left ? 1.f : 0.f, p[2] = left ? 1.f : 0.f, p[3] = 1.f;
    }
  std::string path = tmpFile("patina_test_stencil.png");
  CHECK(saveImage(path, img));
  json st = run("set_stencil", {{"path", path}, {"mode", "mask"}, {"position", {0.5, 0.5}}, {"scale", 0.3}});
  CHECK(st["enabled"].get<bool>() && st["has_image"].get<bool>());
  CHECK(run("get_state")["stencil"]["size"][0] == 64);
  // the stencil spans x 0.2..0.8, y 0.35..0.65 of the view (aspect 2, height 0.3); the plane 0.22..0.78
  RayHit L = e->pick({0.35f * 512, 256}), R = e->pick({0.65f * 512, 256}), O = e->pick({0.5f * 512, 0.28f * 512});
  CHECK(L.hit && R.hit && O.hit);
  run("paint_stroke", {{"points", {{0.2, 0.5}, {0.9, 0.5}}}});
  run("paint_stroke", {{"points", {{0.5, 0.2}, {0.5, 0.4}}}});
  CHECK(isRed(texelAt(*e, L.uv)));
  CHECK(!isRed(texelAt(*e, R.uv)));   // black part blocks
  CHECK(!isRed(texelAt(*e, O.uv)));   // outside the stencil: no paint
  run("undo");
  run("undo");
  run("set_stencil", {{"invert", true}});
  run("paint_stroke", {{"points", {{0.2, 0.5}, {0.9, 0.5}}}});
  run("paint_stroke", {{"points", {{0.5, 0.2}, {0.5, 0.4}}}});
  CHECK(!isRed(texelAt(*e, L.uv)));
  CHECK(isRed(texelAt(*e, R.uv)) && isRed(texelAt(*e, O.uv)));
  run("undo");
  run("undo");
  run("set_stencil", {{"invert", false}, {"enabled", false}});
  run("paint_stroke", {{"points", {{0.2, 0.5}, {0.9, 0.5}}}});
  CHECK(isRed(texelAt(*e, R.uv)));    // disabled: plain stroke
  run("undo");
  // decal: the stencil's colors, projected in one step
  for (int y = 0; y < 32; ++y)
    for (int x = 32; x < 64; ++x) {
      float* p = img.at(x, y);
      p[0] = 0.f, p[1] = 0.f, p[2] = 1.f;
    }
  for (int y = 0; y < 32; ++y)
    for (int x = 0; x < 32; ++x) img.at(x, y)[1] = img.at(x, y)[2] = 0.f;
  CHECK(saveImage(path, img));
  run("set_stencil", {{"path", path}, {"mode", "decal"}});
  json pr = run("project_stencil");
  CHECK(pr["undo"] == "Project stencil");
  vec4 cl = texelAt(*e, L.uv), cr = texelAt(*e, R.uv), co = texelAt(*e, O.uv);
  CHECK(isRed(cl));
  CHECK(cr.z > 0.6f && cr.x < 0.25f);
  CHECK(!isRed(co) && !(co.z > 0.6f && co.x < 0.25f));  // still the gray base
  run("undo");
  CHECK(!isRed(texelAt(*e, L.uv)));
  // text stencil: a big "T" in decal mode, white with a black outline
  json ts = run("set_stencil", {{"text", "T"}, {"font", "sans-bold"}, {"font_size", 200}, {"color", {0, 1, 0}},
                                {"outline", 6}, {"mode", "decal"}, {"position", {0.5, 0.5}}, {"scale", 0.8}});
  CHECK(ts["text"] == "T" && ts["size"][1].get<int>() > 200);
  CHECK(run("list_fonts")["fonts"][0]["bundled"].get<bool>());
  run("project_stencil");
  // the T's stem runs down the middle; its bar is near the top; the lower corners stay empty
  RayHit stem = e->pick({256, 0.6f * 512}), corner = e->pick({0.3f * 512, 0.75f * 512});
  vec4 cs = texelAt(*e, stem.uv), cc = texelAt(*e, corner.uv);
  if (!(cs.y > 0.6f && cs.x < 0.3f)) std::fprintf(stderr, "  text stem (%.2f,%.2f,%.2f)\n", cs.x, cs.y, cs.z);
  CHECK(cs.y > 0.6f && cs.x < 0.3f);
  if (!(corner.hit && !(cc.y > 0.6f && cc.x < 0.3f))) std::fprintf(stderr, "  corner hit %d (%.2f,%.2f,%.2f)\n", corner.hit, cc.x, cc.y, cc.z);
  CHECK(corner.hit && !(cc.y > 0.6f && cc.x < 0.3f));
  run("set_stencil", {{"save_image", tmpFile("patina_test_T.png")}});
  run("undo");
  std::string png = tmpFile("patina_test_text.png");
  run("set_stencil", {{"save_image", png}});
  Image back;
  CHECK(loadImage(png, back) && back.width > 50);
  run("set_stencil", {{"clear", true}});
  CHECK(!run("get_state")["stencil"]["has_image"].get<bool>());
  fs::remove(path);
  fs::remove(png);
}

TEST(gpu, notes_and_compare_view) {
  Engine* e = engine();
  if (!e) return;
  freshSphere(*e);
  json n1 = run("add_note", {{"text", "Rust is too even here"}, {"point", {0.5, 0.5}}, {"author", "tester"}});
  CHECK(n1["id"] == 1 && n1["author"] == "tester");
  json n2 = run("add_note", {{"text", "Check the seam"}, {"position", {2.0, 0.0, 0.0}}});
  vec3 p2(n2["position"][0].get<float>(), n2["position"][1].get<float>(), n2["position"][2].get<float>());
  CHECK(std::abs(glm::length(p2) - 1.f) < 0.05f);  // snapped onto the unit sphere
  bool missed = false;
  try { run("add_note", {{"text", "x"}, {"point", {0.01, 0.01}}}); } catch (const CommandError&) { missed = true; }
  CHECK(missed);
  run("update_note", {{"id", 2}, {"resolved", true}, {"text", "Seam is fine"}});
  CHECK(run("list_notes", {{"include_resolved", false}})["notes"].size() == 1);
  run("set_camera", {{"view", "back"}});
  run("go_to_note", {{"id", 1}});
  CHECK(e->pick({256, 256}).hit);
  // saved with the project
  std::string path = tmpFile("patina_test_notes.patina"), err;
  CHECK(e->doc.saveProject(path, &err));
  run("delete_note", {{"id", 1}});
  CHECK(e->doc.notes.size() == 1);
  run("open_file", {{"path", path}});
  json all = run("list_notes")["notes"];
  CHECK(all.size() == 2 && all[1]["text"] == "Seam is fine" && all[1]["resolved"].get<bool>());
  CHECK(run("add_note", {{"text", "third"}, {"point", {0.5, 0.5}}})["id"] == 3);
  fs::remove(path);

  // compare: snapshot on the left, live on the right
  run("set_camera", {{"view", "front"}, {"frame", true}});
  run("set_brush", {{"size", 500}});
  json cs = run("set_compare", {{"enabled", true}, {"mode", "snapshot"}, {"split", 0.5}});
  CHECK(cs["has_snapshot"].get<bool>());
  run("paint_stroke", {{"points", {{0.3, 0.5}, {0.7, 0.5}}}});
  json shot = run("render_view", {{"width", 256}, {"height", 256}});
  std::vector<uint8_t> png = base64Decode(shot["image_png_base64"].get<std::string>());
  Image img;
  CHECK(decodeImage(png.data(), png.size(), img, 4));
  if (img.width == 256) {
    const float* l = img.at(100, 128);
    const float* r = img.at(156, 128);
    CHECK(!(l[0] > 0.5f && l[1] < 0.2f));  // left: the snapshot from before the stroke
    CHECK(r[0] > 0.4f && r[1] < 0.2f);     // right: painted
  }
  run("set_compare", {{"mode", "view"}, {"view", "roughness"}});
  CHECK(run("get_state")["compare"]["mode"] == "view");
  run("set_compare", {{"enabled", false}});
}

TEST(gpu, open_fbx_and_paint) {
  Engine* e = engine();
  if (!e) return;
  for (const char* f : {"blender_279_ball_7400_binary.fbx", "maya_cube_7500_binary.fbx"}) {
    json r = run("open_file", {{"path", std::string(PATINA_SOURCE_DIR) + "/tests/data/fbx/" + f}});
    (void)r;
    CHECK(e->doc.hasMesh());
    CHECK(e->doc.mesh.islandCount >= 1);
    run("set_camera", {{"view", "front"}, {"frame", true}});
    run("set_paint_material", {{"id", ""}, {"color", {1, 0, 0}}});
    run("set_tool", {{"tool", "paint"}});
    run("set_brush", {{"preset", "basic_hard"}, {"size", 40}, {"projection", "screen"}});
    RayHit h = e->pick({256, 256});
    CHECK(h.hit);
    run("paint_stroke", {{"points", {{0.48, 0.5}, {0.52, 0.5}}}});
    CHECK(isRed(texelAt(*e, h.uv)));  // UVs (generated for the ball) are paintable
  }
}

TEST(gpu, blur_wraps_across_the_texture_border) {
  Engine* e = engine();
  if (!e) return;
  freshSphere(*e);  // one island spanning u 0..1: the seam is the texture's left / right border
  run("set_camera", {{"view", "right"}});
  RayHit h = e->pick({256, 256});
  CHECK(h.hit);
  float v = h.uv.y;
  run("set_brush", {{"projection", "uv"}, {"size", 40}, {"hardness", 1.0}});
  run("paint_stroke", {{"space", "uv"}, {"points", {{0.985, v - 0.1}, {0.985, v + 0.1}}}});
  run("set_brush", {{"projection", "screen"}});
  vec4 before = texelAt(*e, {0.006f, v});
  CHECK(!isRed(before));
  CHECK(isRed(texelAt(*e, {0.995f, v})));
  json stroke = {{"points", {{0.5, 0.45}, {0.5, 0.55}}}, {"brush", {{"size", 120}, {"hardness", 0.5}}}};
  run("set_tool", {{"tool", "blur"}, {"blur_radius", 12}, {"seam_aware", false}});
  run("paint_stroke", stroke);
  vec4 off = texelAt(*e, {0.006f, v});
  run("undo");
  run("set_tool", {{"tool", "blur"}, {"seam_aware", true}});
  run("paint_stroke", stroke);
  vec4 on = texelAt(*e, {0.006f, v});
  run("undo");
  if (!(on.y < off.y - 0.05f)) std::fprintf(stderr, "  wrap blur: off g %.2f on g %.2f\n", off.y, on.y);
  CHECK(std::abs(off.y - before.y) < 0.03f);
  CHECK(on.y < off.y - 0.05f);
  run("set_tool", {{"tool", "paint"}});
}

// Moving one half of a symmetric island takes the other half along (they stay stacked), pinned
// islands survive every UV edit, and drifted halves from older projects can be restacked.
TEST(gpu, uv_pins_and_linked_mirror_halves) {
  Engine* e = engine();
  if (!e) return;
  run("open_file", {{"path", std::string(PATINA_SOURCE_DIR) + "/samples/Suzanne.glb"}});
  run("set_resolution", {{"resolution", 512}});
  Document& d = e->doc;
  json ear = json::array();
  for (uint32_t f = 0; f < d.mesh.faceCount(); ++f)
    if (d.mesh.faceCenter(f).x > 0.9f) ear.push_back(f);
  run("uv_make_island", {{"faces", ear}, {"symmetry", {"x"}}});
  json info = run("uv_info");
  CHECK(info["mirrored_faces"].get<int>() == (int)ear.size());
  CHECK(info["unstacked_mirror_faces"].get<int>() == 0);
  CHECK(info["overlapping_islands"].get<int>() == 0);
  // the copies (left ear) and the half they copy
  const uv::MirrorLinks& L0 = d.mirrorLinks();
  json copies = json::array();
  uint32_t aCopy = 0, aPrimary = 0;
  for (uint32_t f = 0; f < d.mesh.faceCount(); ++f)
    if (L0.primary[f] >= 0) copies.push_back(f), aCopy = f, aPrimary = (uint32_t)L0.primary[f];
  CHECK(copies.size() == ear.size());
  CHECK(d.mesh.faceFlags[aCopy] & FaceMirrorX);
  // move only the copy: the other ear goes with it
  vec2 before = d.mesh.loopUV[d.mesh.faceStart[aPrimary]];
  run("uv_transform", {{"faces", copies}, {"translate", {0.03, 0.02}}});
  vec2 after = d.mesh.loopUV[d.mesh.faceStart[aPrimary]];
  CHECK(glm::length(after - before - vec2(0.03f, 0.02f)) < 1e-4f);
  info = run("uv_info");
  CHECK(info["unstacked_mirror_faces"].get<int>() == 0);
  CHECK(info["overlapping_islands"].get<int>() == 0);
  // pin the ear: the stacked half is pinned with it
  json pin = run("uv_pin", {{"faces", ear}});
  CHECK(pin["pinned_faces"].get<int>() == 2 * (int)ear.size());
  std::vector<vec2> pinnedUV;
  for (auto& f : ear)
    for (uint32_t l = d.mesh.faceStart[f.get<uint32_t>()]; l < d.mesh.faceStart[f.get<uint32_t>() + 1]; ++l)
      pinnedUV.push_back(d.mesh.loopUV[l]);
  auto pinsKept = [&]() {
    size_t i = 0;
    float worst = 0;
    for (auto& f : ear)
      for (uint32_t l = d.mesh.faceStart[f.get<uint32_t>()]; l < d.mesh.faceStart[f.get<uint32_t>() + 1]; ++l)
        worst = std::max(worst, glm::length(d.mesh.loopUV[l] - pinnedUV[i++]));
    return worst < 1e-6f;
  };
  bool threw = false;
  try {
    run("uv_transform", {{"faces", ear}, {"translate", {0.1, 0}}});
  } catch (std::exception&) {
    threw = true;
  }
  CHECK(threw);
  run("uv_pack");
  CHECK(pinsKept());
  CHECK(run("uv_info")["overlapping_islands"].get<int>() == 0);
  run("uv_unwrap", {{"method", "lscm"}});
  CHECK(pinsKept());
  CHECK(run("uv_info")["overlapping_islands"].get<int>() == 0);
  CHECK(run("uv_info")["unstacked_mirror_faces"].get<int>() == 0);
  run("uv_unwrap", {{"method", "smart"}});
  CHECK(pinsKept());
  CHECK(run("uv_info")["overlapping_islands"].get<int>() == 0);
  // undo brings the unpinned state back; redo pins again
  run("uv_pin", {{"all", true}, {"pin", false}});
  CHECK(uv::pinnedFaceCount(d.mesh) == 0);
  run("undo");
  CHECK(uv::pinnedFaceCount(d.mesh) == 2 * ear.size());
  run("uv_pin", {{"all", true}, {"pin", false}});
  // an older project: the halves drifted apart and the flags have no axis bits
  for (auto& f : copies) {
    uint32_t fi = f.get<uint32_t>();
    d.mesh.faceFlags[fi] = FaceMirrorCopy;
    for (uint32_t l = d.mesh.faceStart[fi]; l < d.mesh.faceStart[fi + 1]; ++l) d.mesh.loopUV[l] += vec2(0.04f, 0.0f);
  }
  d.mesh.computeUVTopology();
  info = run("uv_info");
  CHECK(info["mirrored_faces"].get<int>() == (int)ear.size());
  CHECK(info["unstacked_mirror_faces"].get<int>() == (int)ear.size());
  CHECK(info["overlapping_islands"].get<int>() >= 1);
  json rs = run("uv_restack");
  CHECK(rs["restacked_faces"].get<int>() == (int)ear.size());
  info = run("uv_info");
  CHECK(info["unstacked_mirror_faces"].get<int>() == 0);
  CHECK(info["overlapping_islands"].get<int>() == 0);
  // unstack: each ear gets its own place
  json us = run("uv_unstack", {{"faces", copies}});
  CHECK(us["faces"].get<int>() == (int)ear.size());
  info = run("uv_info");
  CHECK(info["mirrored_faces"].get<int>() == 0);
  CHECK(info["overlapping_islands"].get<int>() == 0);
}

// Paint survives a symmetric island: only the unwrapped half carries paint into the new layout,
// the stacked half shares it (drawing both would interleave their old paint triangle by triangle).
TEST(gpu, symmetric_island_keeps_paint) {
  Engine* e = engine();
  if (!e) return;
  run("open_file", {{"path", std::string(PATINA_SOURCE_DIR) + "/samples/Suzanne.glb"}});
  run("set_resolution", {{"resolution", 512}});
  run("set_camera", {{"view", "front"}, {"frame", true}});
  run("set_brush", {{"preset", "basic_hard"}, {"size", 160}, {"hardness", 0.9}, {"flow", 1.0}, {"opacity", 1.0}});
  run("set_paint_material", {{"id", ""}, {"color", {1, 0, 0}}, {"roughness", 0.4}, {"metallic", 0}});
  run("set_paint_channels", {{"channels", {"all"}}});
  run("set_tool", {{"tool", "paint"}});
  run("set_symmetry", {{"x", false}});
  // the right ear (screen left in the front view is the model's +X side? paint both to be sure of one)
  for (float y = 0.30f; y <= 0.52f; y += 0.04f) run("paint_stroke", {{"points", {{0.74, y}, {0.92, y}}}});
  Document& d = e->doc;
  const Mesh& m = d.mesh;
  auto faceUV = [&](uint32_t f) {
    vec2 c(0);
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) c += m.loopUV[l];
    return c / float(m.faceSize(f));
  };
  json ear = json::array();
  int side = 0;
  for (uint32_t f = 0; f < m.faceCount(); ++f) side += isRed(texelAt(*e, faceUV(f))) ? (m.faceCenter(f).x > 0 ? 1 : -1) : 0;
  float sx = side >= 0 ? 1.f : -1.f;  // the side that was painted
  for (uint32_t f = 0; f < m.faceCount(); ++f)
    if (m.faceCenter(f).x * sx > 0.9f) ear.push_back(f);
  CHECK(ear.size() > 10);
  int redBefore = 0;
  for (auto& f : ear) redBefore += isRed(texelAt(*e, faceUV(f))) ? 1 : 0;
  CHECK(redBefore > (int)ear.size() / 2);
  run("uv_make_island", {{"faces", ear}, {"symmetry", {"x"}}});
  int redAfter = 0, mirrorRed = 0, mirrors = 0;
  for (auto& f : ear) redAfter += isRed(texelAt(*e, faceUV(f))) ? 1 : 0;
  for (uint32_t f = 0; f < m.faceCount(); ++f)
    if (m.faceFlags[f] & FaceMirrorCopy) ++mirrors, mirrorRed += isRed(texelAt(*e, faceUV(f))) ? 1 : 0;
  // (faces at the edge of the paint can drop below the threshold at the island's own density)
  if (redAfter < redBefore * 8 / 10) std::fprintf(stderr, "  red faces %d -> %d (of %zu)\n", redBefore, redAfter, ear.size());
  CHECK(redAfter >= redBefore * 8 / 10);
  CHECK(mirrors > 0 && mirrorRed == redAfter);  // the stacked half shows the same paint
}

TEST(gpu, symmetric_island_stacks_both_halves) {
  Engine* e = engine();
  if (!e) return;
  run("open_file", {{"path", std::string(PATINA_SOURCE_DIR) + "/samples/Suzanne.glb"}});
  run("set_resolution", {{"resolution", 512}});
  const Mesh& m0 = e->doc.mesh;
  // the right ear (x > 0.9); its mirror is the left ear
  json faces = json::array();
  for (uint32_t f = 0; f < m0.faceCount(); ++f)
    if (m0.faceCenter(f).x > 0.9f) faces.push_back(f);
  CHECK(faces.size() > 10);
  int mirrored = 0;
  for (auto& f : faces) mirrored += uv::mirrorFace(m0, e->doc.bvh, f.get<uint32_t>(), 1) >= 0;
  CHECK(mirrored == (int)faces.size());  // Suzanne is symmetric
  json r = run("uv_make_island", {{"faces", faces}, {"symmetry", {"x"}}});
  CHECK(r["stacked_faces"].get<int>() == (int)faces.size());
  const Mesh& m = e->doc.mesh;
  // every corner of the left ear has the UV of the matching corner of the right ear
  float worst = 0;
  for (auto& fj : faces) {
    uint32_t f = fj.get<uint32_t>();
    int g = uv::mirrorFace(m, e->doc.bvh, f, 1);
    if (g < 0) continue;
    for (uint32_t l = m.faceStart[g]; l < m.faceStart[g + 1]; ++l) {
      vec3 p = m.positions[m.loopVert[l]];
      p.x = -p.x;
      float best = 1e9f;
      vec2 uv(0);
      for (uint32_t k = m.faceStart[f]; k < m.faceStart[f + 1]; ++k) {
        float d = glm::length(m.positions[m.loopVert[k]] - p);
        if (d < best) best = d, uv = m.loopUV[k];
      }
      worst = std::max(worst, glm::length(uv - m.loopUV[l]));
    }
  }
  CHECK(worst < 1e-5f);
  // the stacked pair is not reported as an overlap
  CHECK(uv::overlappingIslands(m).empty());
  {  // and still not after saving and opening the project
    std::string path = tmpFile("patina_test_symisland.patina"), err;
    CHECK(e->doc.saveProject(path, &err));
    run("open_file", {{"path", path}});
    CHECK(uv::overlappingIslands(e->doc.mesh).empty());
    CHECK(e->doc.mesh.faceFlags[uv::mirrorFace(e->doc.mesh, e->doc.bvh, faces[0].get<uint32_t>(), 1)] & FaceMirrorCopy);
    fs::remove(path);
    run("open_file", {{"path", std::string(PATINA_SOURCE_DIR) + "/samples/Suzanne.glb"}});
    run("set_resolution", {{"resolution", 512}});
    run("uv_make_island", {{"faces", faces}, {"symmetry", {"x"}}});
  }
  // a region across the mirror plane (the face): the center faces belong to the unwrapped half
  run("undo");
  json front = json::array();
  for (uint32_t f = 0; f < m0.faceCount(); ++f)
    if (e->doc.mesh.faceCenter(f).z > 0.55f && std::abs(e->doc.mesh.faceCenter(f).x) < 0.5f) front.push_back(f);
  json r3 = run("uv_make_island", {{"faces", front}, {"symmetry", {"x"}}});
  CHECK(r3["stacked_faces"].get<int>() > 10);
  CHECK(uv::overlappingIslands(e->doc.mesh).empty());
  run("undo");
  // without symmetry only the right ear moves
  json r2 = run("uv_make_island", {{"faces", faces}});
  CHECK(r2["stacked_faces"].get<int>() == 0);
  run("undo");
}

// ---------------------------------------------------------------- 0.7: thickness, bent normals, detail curvature
// A low-poly box: thin bright edges and neutral faces (0.6's per-vertex curvature lit whole
// faces); a smooth sphere stays close to neutral, so edge masks don't cover it.
TEST(gpu, curvature_is_tessellation_independent) {
  Engine* e = engine();
  if (!e) return;
  Document& d = e->doc;
  auto stats = [&](const char* prim) {
    run("new_project", {{"primitive", prim}, {"resolution", 256}});
    run("bake_mesh_maps", {{"ao", false}, {"curvature", true}});
    int R = d.tex.resolution();
    Image c = d.tex.curvMap.download(1);
    std::vector<vec4> pos(size_t(R) * R);
    d.tex.posMap.downloadRaw(GL_RGBA, GL_FLOAT, pos.data(), pos.size() * sizeof(vec4));
    std::vector<float> v;
    for (int y = 0; y < R; ++y)
      for (int x = 0; x < R; ++x)
        if (pos[size_t(y) * R + x].w > 0.5f) v.push_back(c.at(x, y)[0]);
    std::sort(v.begin(), v.end());
    return std::make_pair(v[v.size() / 2], v[v.size() * 995 / 1000]);  // median, near-max
  };
  auto [cubeMid, cubeMax] = stats("cube");
  auto [sphMid, sphMax] = stats("sphere");
  std::fprintf(stderr, "  curvature cube %.2f / %.2f, sphere %.2f / %.2f\n", cubeMid, cubeMax, sphMid, sphMax);
  CHECK(std::abs(cubeMid - 0.5f) < 0.04f);  // faces flat
  CHECK(cubeMax > 0.75f);                   // edges bright
  CHECK(sphMid < 0.62f);                    // a smooth sphere is not "all edge"
}

TEST(gpu, thickness_bent_normals_and_detail_curvature) {
  Engine* e = engine();
  if (!e) return;
  run("open_file", {{"path", std::string(PATINA_SOURCE_DIR) + "/samples/Suzanne.glb"}});
  run("set_resolution", {{"resolution", 256}});
  json b = run("bake_mesh_maps", {{"ao_samples", 32}, {"thickness", true}, {"thickness_rays", 16}});
  CHECK(b["thickness"].get<bool>() && b["bent_normals"].get<bool>());
  Document& d = e->doc;
  int R = d.tex.resolution();
  auto at = [&](const Texture2D& t, vec2 uv, int c = 0) {
    float v[4];
    int x = std::clamp(int(uv.x * R), 0, R - 1), y = std::clamp(int(uv.y * R), 0, R - 1);
    glGetTextureSubImage(t.id(), 0, x, y, 0, 1, 1, 1, GL_RGBA, GL_FLOAT, sizeof v, v);
    return v[c];
  };
  // the ear (thin) against the middle of the head (thick)
  run("set_camera", {{"view", "front"}, {"frame", true}});
  vec2 vs = e->viewportSize;
  RayHit ear = e->pick({0.81f * vs.x, 0.42f * vs.y}), head = e->pick({0.5f * vs.x, 0.3f * vs.y});
  CHECK(ear.hit && head.hit);
  float te = at(d.tex.thickMap, ear.uv), th = at(d.tex.thickMap, head.uv);
  if (!(te + 0.15f < th)) std::fprintf(stderr, "  thickness ear %.2f head %.2f\n", te, th);
  CHECK(te + 0.15f < th);
  // bent normals are unit vectors close to the normal on open surfaces
  vec3 bn(at(d.tex.bentMap, head.uv, 0), at(d.tex.bentMap, head.uv, 1), at(d.tex.bentMap, head.uv, 2));
  CHECK(std::abs(glm::length(bn) - 1.f) < 0.05f);
  CHECK(glm::dot(glm::normalize(bn), glm::normalize(head.normal)) > 0.7f);
  // the thin-parts smart mask uses it
  run("add_layer", {{"type", "fill"}, {"name", "Thin"}, {"material", {{"id", ""}, {"color", {1, 0, 0}}}},
                    {"generator", {{"type", "thin"}, {"level", 0.5}, {"contrast", 0.8}}}});
  CHECK(isRed(texelAt(*e, ear.uv)));
  CHECK(!isRed(texelAt(*e, head.uv)));
  // exported with the maps; saved with the project
  std::string dir = tmpFile("patina_test_extras");
  json ex = run("export_textures", {{"directory", dir}, {"preset", "unity"}});
  CHECK(ex["files"].contains("thickness") && ex["files"].contains("bent_normal"));
  Image bimg;
  CHECK(loadImage(ex["files"]["bent_normal"].get<std::string>(), bimg));
  std::string path = tmpFile("patina_test_extras.patina"), err;
  CHECK(d.saveProject(path, &err));
  run("open_file", {{"path", path}});
  CHECK(d.tex.hasThickness() && d.tex.hasBentNormals());
  CHECK(std::abs(at(d.tex.thickMap, ear.uv) - te) < 0.02f);
  fs::remove(path);
  fs::remove_all(dir);
  // curvature from a high-poly normal map: the golf-ball dimples show up
  Mesh low = makeUVSphere(24, 12, 1.f), high = makeUVSphere(96, 48, 1.f);
  for (auto& p : high.positions) {
    vec3 n = glm::normalize(p);
    float dimple = 0.f;
    for (int k = 0; k < 40; ++k) {
      float t = (k + 0.5f) / 40.f, z = 1 - 2 * t, r = std::sqrt(1 - z * z), ph = k * 2.39996f;
      vec3 c(r * std::cos(ph), z, r * std::sin(ph));
      dimple = std::max(dimple, std::max(0.f, 1.f - glm::length(n - c) / 0.2f));
    }
    p = n * (1.f - 0.04f * dimple * dimple);
  }
  high.finalize();
  std::string lp = tmpFile("patina_test_low.ptmesh"), hp = tmpFile("patina_test_high_dimples.ptmesh");
  CHECK(savePtMesh(lp, low, &err) && savePtMesh(hp, high, &err));
  run("open_file", {{"path", lp}});
  run("set_resolution", {{"resolution", 256}});
  run("bake_mesh_maps", {{"ao", false}});
  CHECK(!d.tex.curvatureHasDetail());
  auto spread = [&]() {
    Image c = d.tex.curvMap.download(1);
    double s = 0, s2 = 0;
    int n = 0;
    for (int y = R / 4; y < 3 * R / 4; ++y)
      for (int x = 0; x < R; ++x) {
        float v = c.at(x, y)[0];
        s += v, s2 += v * v, ++n;
      }
    return std::sqrt(std::max(0.0, s2 / n - (s / n) * (s / n)));
  };
  double plain = spread();
  run("bake_from_high_poly", {{"path", hp}, {"ao", false}});
  CHECK(d.tex.curvatureHasDetail());
  double detail = spread();
  if (!(detail > plain * 2 + 0.01)) std::fprintf(stderr, "  curvature spread %.3f -> %.3f\n", plain, detail);
  CHECK(detail > plain * 2 + 0.01);
  fs::remove(lp);
  fs::remove(hp);
}

TEST(gpu, mirror_uvs_and_rectify_commands) {
  Engine* e = engine();
  if (!e) return;
  run("open_file", {{"path", std::string(PATINA_SOURCE_DIR) + "/samples/Suzanne.glb"}});
  run("set_resolution", {{"resolution", 256}});
  json r = run("uv_mirror", {{"axes", {"x"}}});
  int n = r["mirrored_faces"].get<int>();
  CHECK(n > 200);
  const Mesh& m = e->doc.mesh;
  int checked = 0;
  float worst = 0;
  for (uint32_t f = 0; f < m.faceCount() && checked < 50; ++f) {
    if (m.faceCenter(f).x > -0.05f) continue;
    int g = uv::mirrorFace(m, e->doc.bvh, f, 1);
    if (g < 0) continue;
    ++checked;
    vec2 a(0), b(0);
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) a += m.loopUV[l];
    for (uint32_t l = m.faceStart[g]; l < m.faceStart[g + 1]; ++l) b += m.loopUV[l];
    worst = std::max(worst, glm::length(a - b) / float(m.faceSize(f)));
  }
  CHECK(checked > 10 && worst < 1e-5f);
  CHECK(uv::overlappingIslands(m).empty());
  CHECK(e->doc.undo.undoLabel() == "Mirror UVs");
  run("undo");
  json rr = run("uv_rectify");
  CHECK(rr.contains("rectified"));
}

// ---------------------------------------------------------------- 0.7: tiled export above the texture set size
TEST(gpu, tiled_export_matches_a_full_size_texture_set) {
  Engine* e = engine();
  if (!e) return;
  run("new_project", {{"primitive", "sphere"}, {"resolution", 256}});
  run("add_layer", {{"type", "fill"}, {"name", "Rust"}, {"material", {{"id", "patina_rust_heavy"}, {"scale", 2.0}}},
                    {"generator", {{"type", "noise"}, {"level", 0.5}, {"contrast", 0.7}, {"noise_scale", 2}}}});
  std::string a = tmpFile("patina_tiled_a"), b = tmpFile("patina_tiled_b");
  fs::remove_all(a);
  fs::remove_all(b);
  // 2048 from a 256 texture set: four 1024 tiles
  json ra = run("export_textures", {{"directory", a}, {"size", 2048}, {"preset", "blender"}});
  run("set_resolution", {{"resolution", 2048}});
  json rb = run("export_textures", {{"directory", b}, {"preset", "blender"}});
  for (const char* key : {"basecolor", "roughness", "normal", "height"}) {
    Image ia, ib;
    CHECK(loadImage(ra["files"][key].get<std::string>(), ia));
    CHECK(loadImage(rb["files"][key].get<std::string>(), ib));
    CHECK(ia.width == 2048 && ia.height == 2048 && ib.width == 2048);
    if (ia.width != ib.width || ia.channels != ib.channels) continue;
    double diff = 0;
    size_t n = 0, big = 0;
    for (size_t i = 0; i < ia.pixels.size(); i += 7) {
      double d = std::abs(ia.pixels[i] - ib.pixels[i]);
      diff += d, ++n;
      big += d > 0.1;
    }
    diff /= double(n);
    if (!(diff < 0.01 && big < n / 100)) std::fprintf(stderr, "  %s: mean diff %.4f, %zu of %zu samples off\n", key, diff, big, n);
    CHECK(diff < 0.01);
    CHECK(big < n / 100);
  }
  fs::remove_all(a);
  fs::remove_all(b);
}

// ---------------------------------------------------------------- 0.7: layered PSD
static std::vector<uint8_t> readAll(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
static uint32_t rd32(const std::vector<uint8_t>& b, size_t o) {
  return (uint32_t(b[o]) << 24) | (uint32_t(b[o + 1]) << 16) | (uint32_t(b[o + 2]) << 8) | b[o + 3];
}

TEST(gpu, layered_psd_export) {
  Engine* e = engine();
  if (!e) return;
  freshSphere(*e);
  run("set_resolution", {{"resolution", 256}});
  run("add_layer", {{"type", "fill"}, {"name", "Rust"}, {"material", {{"id", "patina_rust_heavy"}}},
                    {"generator", {{"type", "noise"}, {"level", 0.5}}}});
  run("add_layer", {{"type", "folder"}, {"name", "Details"}});
  run("add_layer", {{"type", "paint"}, {"name", "Red paint"}});
  run("paint_stroke", {{"points", {{0.4, 0.5}, {0.6, 0.5}}}});
  run("update_layer", {{"opacity", 0.5}, {"blend", "multiply"}});
  run("add_layer", {{"type", "adjust"}, {"name", "Hue"}, {"adjust", {{"hue", 0.3}}}});
  std::string dir = tmpFile("patina_psd");
  fs::remove_all(dir);
  if (const char* keep = std::getenv("PATINA_KEEP_PSD")) dir = keep;
  json r = run("export_textures", {{"directory", dir}, {"format", "psd"}});
  CHECK(r["files"].size() == 6);
  auto b = readAll(r["files"]["basecolor"].get<std::string>());
  CHECK(b.size() > 1000 && b[0] == '8' && b[1] == 'B' && b[2] == 'P' && b[3] == 'S');
  CHECK(rd32(b, 14) == 256 && rd32(b, 18) == 256);  // height, width
  // layer info: header 26 + color mode (4+0) + resources (4+0) + LMI length 4 + LI length 4 + count
  size_t o = 26 + 4 + 4;
  uint32_t lmi = rd32(b, o);
  CHECK(lmi > 0 && o + 4 + lmi < b.size());
  int16_t count = int16_t((b[o + 8] << 8) | b[o + 9]);
  // a record per layer, the base, and a group-end marker per folder
  int want = int(stackLayers(e->doc)) + 1 + 1;  // (baked map layers add nothing to the maps: not there)
  if (count != want) std::fprintf(stderr, "  psd layers: %d, want %d\n", count, want);
  CHECK(count == want);
  std::string all(b.begin(), b.end());
  CHECK(all.find("Red paint") != std::string::npos && all.find("lsct") != std::string::npos);
  CHECK(all.find("mul ") != std::string::npos);
  auto h = readAll(r["files"]["height"].get<std::string>());
  CHECK(h.size() > 26 && h[23] == 16);  // 16-bit height
  if (!std::getenv("PATINA_KEEP_PSD")) fs::remove_all(dir);
}

// A stacked mirror copy shares its texels with the half it copies: the texture-space position map
// (what tri-planar materials, generators and brushes are evaluated at) must hold that half only.
// Both halves writing it left a patchwork of left and right positions, which ghosted patterned
// materials (diamond plate) across symmetric islands.
TEST(gpu, mirror_copies_leave_the_position_map_to_their_other_half) {
  Engine* e = engine();
  if (!e) return;
  run("open_file", {{"path", std::string(PATINA_SOURCE_DIR) + "/samples/Suzanne.glb"}});
  run("set_resolution", {{"resolution", 512}});
  Document& d = e->doc;
  json all = json::array();
  for (uint32_t f = 0; f < d.mesh.faceCount(); ++f) all.push_back(f);
  json r = run("uv_make_island", {{"faces", all}, {"symmetry", {"x"}}, {"density", "fit"}});
  CHECK(r.value("stacked_faces", 0) > 100);
  Image pos = d.tex.posMap.download(4);
  float eps = d.mesh.boundingRadius * 1e-3f;
  int covered = 0, negative = 0;
  for (int y = 0; y < pos.height; ++y)
    for (int x = 0; x < pos.width; ++x) {
      const float* p = pos.at(x, y);
      if (p[3] < 0.5f) continue;
      ++covered;
      if (p[0] < -eps) ++negative;  // a copy (the -X half) wrote here
    }
  if (negative) std::fprintf(stderr, "  %d of %d covered texels hold mirror-copy positions\n", negative, covered);
  CHECK(covered > 1000);
  CHECK(negative == 0);
  // undo brings the old layout back, with its own (unstacked) positions
  run("undo", json::object());
  Image pos2 = d.tex.posMap.download(4);
  int neg2 = 0;
  for (int y = 0; y < pos2.height; ++y)
    for (int x = 0; x < pos2.width; ++x)
      if (pos2.at(x, y)[3] > 0.5f && pos2.at(x, y)[0] < -eps) ++neg2;
  CHECK(neg2 > 1000);
}

// ---- texture tiles (0.9) ---------------------------------------------------------------

namespace {
bool allUVsInUnit(const Mesh& m) {
  for (auto& t : m.loopUV)
    if (t.x < -1e-4f || t.y < -1e-4f || t.x > 1.0001f || t.y > 1.0001f) return false;
  return true;
}
// atlas texel at the center of a face
vec4 faceColor(Engine& e, uint32_t f) {
  const Mesh& m = e.doc.mesh;
  vec2 c(0);
  for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) c += m.texUV(l);
  return texelAt(e, c / float(m.faceSize(f)));
}
std::string writeTempMesh(const Mesh& m, const char* name) {
  fs::path p = fs::temp_directory_path() / name;
  std::string err;
  CHECK(savePtMesh(p.string(), m, &err));
  return p.string();
}
}  // namespace

TEST(gpu, texture_sets_per_material_slot) {
  Engine* e = engine();
  if (!e) return;
  // a cube whose two material slots use the same three UV strips (a character's body and clothes
  // each laid out over the whole texture)
  Mesh m = makeCube(2.f);
  m.name = "SetCube";
  m.materialNames = {"Body", "Clothes"};
  for (uint32_t f = 0; f < m.faceCount(); ++f) {
    m.faceMaterial[f] = f < 3 ? 0 : 1;
    float u0 = (f % 3) / 3.f + 0.02f, u1 = (f % 3 + 1) / 3.f - 0.02f;
    vec2 q[4] = {{u0, 0.1f}, {u1, 0.1f}, {u1, 0.9f}, {u0, 0.9f}};
    for (uint32_t k = 0; k < m.faceSize(f); ++k) m.loopUV[m.faceStart[f] + k] = q[k % 4];
  }
  m.finalize();
  CHECK(Document::detectTileMode(m) == TileBySlot);
  run("open_file", {{"path", writeTempMesh(m, "patina_setcube.ptmesh")}});
  json t = run("texture_tiles");
  CHECK(t["mode"] == "material_slots");
  CHECK(t["tiles"].size() == 2 && t["tiles"][0]["name"] == "Body" && t["tiles"][1]["name"] == "Clothes");
  CHECK(t["grid"] == 2);
  CHECK(e->doc.tex.resolution() == 2 * e->doc.tileRes());
  run("set_camera", {{"view", "front"}, {"frame", true}});
  RayHit h = e->pick({256, 256});
  CHECK(h.hit);
  if (!h.hit) return;
  uint32_t front = h.face, twin = front < 3 ? front + 3 : front - 3;  // same UVs, the other slot
  run("set_paint_material", {{"id", ""}, {"color", {1, 0, 0}}});
  run("fill", {{"mode", "face"}, {"point", {0.5, 0.5}}});
  CHECK(isRed(faceColor(*e, front)));
  CHECK(!isRed(faceColor(*e, twin)));  // its own texture set: no shared texels
  // UV-mapped materials follow the authored UVs, not the atlas
  vec2 a = e->doc.mesh.texUV(e->doc.mesh.faceStart[twin]);
  int tile = 0;
  vec2 back = e->doc.mesh.tiles.fromAtlas(a, &tile);
  CHECK(tile == e->doc.mesh.faceTile(twin));
  CHECK(glm::length(back - e->doc.mesh.loopUV[e->doc.mesh.faceStart[twin]]) < 1e-5f);
  // export: one file set per texture set, named after the slot
  fs::path dir = fs::temp_directory_path() / "patina_sets_export";
  fs::remove_all(dir);
  json ex = run("export_textures", {{"directory", dir.string()}, {"preset", "blender"}});
  CHECK(fs::exists(dir / "SetCube_Body_basecolor.png"));
  CHECK(fs::exists(dir / "SetCube_Clothes_basecolor.png"));
  Image body, clothes;
  CHECK(loadImage((dir / "SetCube_Body_basecolor.png").string(), body, 3));
  CHECK(loadImage((dir / "SetCube_Clothes_basecolor.png").string(), clothes, 3));
  CHECK(body.width == e->doc.tileRes());
  // the painted face's strip is red in its own set's file only
  const Mesh& mm = e->doc.mesh;
  vec2 c(0);
  for (uint32_t l = mm.faceStart[front]; l < mm.faceStart[front + 1]; ++l) c += mm.loopUV[l];
  c /= float(mm.faceSize(front));
  int px = int(c.x * body.width), py = int((1.f - c.y) * body.height);
  const float* pb = body.at(px, py);
  const float* pc = clothes.at(px, py);
  bool frontIsBody = mm.faceMaterial[front] == 0;
  const float* red = frontIsBody ? pb : pc;
  const float* other = frontIsBody ? pc : pb;
  CHECK(red[0] > 0.6f && red[1] < 0.25f);
  CHECK(!(other[0] > 0.6f && other[1] < 0.25f));
  // the project keeps the layout and the paint
  fs::path proj = fs::temp_directory_path() / "patina_sets.patina";
  run("save_project", {{"path", proj.string()}});
  run("new_project", {{"primitive", "sphere"}, {"resolution", 512}});
  run("open_file", {{"path", proj.string()}});
  CHECK(e->doc.mesh.tiles.mode == TileBySlot && e->doc.mesh.tiles.count() == 2);
  CHECK(isRed(faceColor(*e, front)));
  CHECK(!isRed(faceColor(*e, twin)));
  // UV tools work per texture set: the sets don't count as overlapping, and packing keeps each
  // set in its own 0..1 square (both fill it)
  json info = run("uv_info");
  CHECK(info["overlaps"] == 0);
  run("uv_pack", {{"margin", 0.01}});
  CHECK(e->doc.mesh.tiles.count() == 2);
  for (uint32_t f = 0; f < e->doc.mesh.faceCount(); ++f) CHECK(e->doc.mesh.faceTile(f) == (int)e->doc.mesh.faceMaterial[f]);
  CHECK(allUVsInUnit(e->doc.mesh));
  CHECK(run("uv_info")["overlaps"] == 0);
  CHECK(run("uv_info")["coverage"].get<float>() > 0.3f);
  CHECK(isRed(faceColor(*e, front)));
  CHECK(!isRed(faceColor(*e, twin)));
  // a UV-space stroke in the UV editor paints its own set only, even where the other set's
  // faces use the same UVs (and right at the edge of its square)
  {
    const Mesh& mm2 = e->doc.mesh;
    vec2 c2(0);
    for (uint32_t l = mm2.faceStart[twin]; l < mm2.faceStart[twin + 1]; ++l) c2 += mm2.loopUV[l];
    c2 /= float(mm2.faceSize(twin));
    run("set_paint_material", {{"id", ""}, {"color", {0, 0, 1}}});
    run("set_brush", {{"size", 400}, {"hardness", 1.0}, {"flow", 1.0}});
    run("paint_stroke", {{"points", {{c2.x, c2.y}, {c2.x + 0.01, c2.y}}}, {"space", "uv"}, {"tile", mm2.faceTile(twin)}});
    vec4 tw = faceColor(*e, twin);
    CHECK(tw.z > 0.6f && tw.x < 0.3f);    // blue on the face of that set
    CHECK(isRed(faceColor(*e, front)));  // the other set's face with the same UVs keeps its red
    run("set_paint_material", {{"id", ""}, {"color", {1, 0, 0}}});
    run("set_brush", {{"size", 60}});
  }
  // pinned islands in one set don't change how another set packs
  {
    std::vector<uint8_t> flags = e->doc.mesh.faceFlags;
    flags.resize(e->doc.mesh.faceCount(), 0);
    for (uint32_t f = 0; f < e->doc.mesh.faceCount(); ++f)
      if (e->doc.mesh.faceTile(f) == e->doc.mesh.faceTile(twin)) flags[f] |= FacePinned;
    e->doc.applyFaceFlags(flags, "Pin a set");
    run("uv_pack", {{"margin", 0.01}});
    double area = 0;
    for (uint32_t f = 0; f < e->doc.mesh.faceCount(); ++f)
      if (e->doc.mesh.faceTile(f) == e->doc.mesh.faceTile(front)) area += std::abs(e->doc.mesh.faceAreaUV(f));
    CHECK(area > 0.3);  // the unpinned set still fills its square
    run("uv_pin", {{"all", true}, {"pin", false}});
  }
  // the Island brush over faces of both sets makes an island in each
  json mk = run("uv_make_island", {{"faces", {front, twin}}, {"orient", "none"}});
  CHECK(mk["islands_made"].get<int>() >= 2);
  CHECK(mk["overlaps"] == 0);
  CHECK(isRed(faceColor(*e, front)));
  // Blender sends the mesh again with the painted face moved to the other material slot (same
  // UVs): its cell changes, and its paint goes with it
  {
    Mesh m2 = e->doc.mesh;
    uint32_t other = m2.faceMaterial[front] == 0 ? 1 : 0;
    m2.faceMaterial[front] = other;
    m2.tiles = TileLayout();
    m2.loopAtlasUV.clear();
    m2.computeUVTopology();
    e->doc.setMesh(std::move(m2), 0, true);
    CHECK(e->doc.mesh.faceTile(front) == (int)other);
    // it now shares UVs with the other set's painted face, so either paint may win; not the base
    vec4 fc = faceColor(*e, front);
    CHECK(std::abs(fc.x - fc.z) > 0.3f);
  }
  // one texture again: the slots share texels, the paint stays where it was on the model
  run("texture_tiles", {{"mode", "single"}});
  CHECK(e->doc.tex.resolution() == e->doc.tileRes());
  vec4 fc = faceColor(*e, front);
  CHECK(std::abs(fc.x - fc.z) > 0.3f);  // painted (red or the overlapping blue), not the base
}

TEST(gpu, udim_tiles) {
  Engine* e = engine();
  if (!e) return;
  // a plane of 2 x 2 quads: the left half in UDIM 1001, the right half in 1002
  Mesh m = makePlane(2.f, 2);
  m.name = "UdimPlane";
  for (uint32_t f = 0; f < m.faceCount(); ++f) {
    vec3 c = m.faceCenter(f);
    float shift = c.x > 0 ? 1.f : 0.f;
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) {
      vec3 p = m.positions[m.loopVert[l]];
      // each half fills its own tile
      m.loopUV[l] = vec2(shift + (c.x > 0 ? p.x : p.x + 1.f) * 0.98f + 0.01f, (p.z + 1.f) * 0.49f + 0.01f);
    }
  }
  m.finalize();
  CHECK(Document::detectTileMode(m) == TileByUdim);
  run("open_file", {{"path", writeTempMesh(m, "patina_udim.ptmesh")}});
  json t = run("texture_tiles");
  CHECK(t["mode"] == "udim");
  CHECK(t["tiles"].size() == 2 && t["tiles"][0]["udim"] == 1001 && t["tiles"][1]["udim"] == 1002);
  // islands were not moved into 0..1
  float maxU = 0;
  for (auto& uv : e->doc.mesh.loopUV) maxU = std::max(maxU, uv.x);
  CHECK(maxU > 1.5f);
  run("set_camera", {{"view", "top"}, {"frame", true}});
  run("set_paint_material", {{"id", ""}, {"color", {1, 0, 0}}});
  run("fill", {{"mode", "face"}, {"point", {0.25, 0.5}}});
  uint32_t left = 0, right = 0;
  for (uint32_t f = 0; f < e->doc.mesh.faceCount(); ++f) (e->doc.mesh.faceCenter(f).x < 0 ? left : right) = f;
  bool l = isRed(faceColor(*e, left)), r = isRed(faceColor(*e, right));
  if (l == r) {
    vec4 a = faceColor(*e, left), b = faceColor(*e, right);
    RayHit hh = e->pick({128, 256});
    std::fprintf(stderr, "  left (%.2f %.2f %.2f) right (%.2f %.2f %.2f) faces %u %u hit %d face %u uv %.3f %.3f\n", a.x, a.y, a.z,
                 b.x, b.y, b.z, left, right, hh.hit, hh.face, hh.uv.x, hh.uv.y);
  }
  CHECK(l != r);  // exactly one side painted
  fs::path dir = fs::temp_directory_path() / "patina_udim_export";
  fs::remove_all(dir);
  run("export_textures", {{"directory", dir.string()}, {"preset", "blender"}});
  CHECK(fs::exists(dir / "UdimPlane_basecolor.1001.png"));
  CHECK(fs::exists(dir / "UdimPlane_basecolor.1002.png"));
  // packing keeps every island in its UDIM tile
  run("uv_pack", {{"margin", 0.01}});
  maxU = 0;
  for (auto& uv : e->doc.mesh.loopUV) maxU = std::max(maxU, uv.x);
  CHECK(maxU > 1.5f && e->doc.mesh.tiles.count() == 2);
  CHECK(l == isRed(faceColor(*e, left)) && r == isRed(faceColor(*e, right)));
  // moving an island into a new tile grows the atlas and keeps the paint
  uint32_t painted = l ? left : right;
  std::vector<vec2> uvs = e->doc.mesh.loopUV;
  for (uint32_t k = e->doc.mesh.faceStart[painted]; k < e->doc.mesh.faceStart[painted + 1]; ++k) uvs[k].y += 1.f;  // 1011 / 1012
  CHECK(e->doc.applyUVs(uvs, true, "Move to a new UDIM tile"));
  CHECK(e->doc.mesh.tiles.count() == 3);
  CHECK(e->doc.mesh.tiles.grid == 2);
  CHECK(e->doc.tex.resolution() == 2 * e->doc.tileRes());
  CHECK(isRed(faceColor(*e, painted)));
  // one texture would lose the paint of tiles beyond 1001: refused
  bool refused = false;
  try {
    run("texture_tiles", {{"mode", "single"}});
  } catch (const std::exception&) {
    refused = true;
  }
  CHECK(refused && e->doc.mesh.tiles.mode == TileByUdim);
}

TEST(gpu, mesh_symmetry_check) {
  Engine* e = engine();
  if (!e) return;
  Mesh m = makeUVSphere(48, 24, 1.f);
  m.name = "SymSphere";
  m.finalize();
  run("open_file", {{"path", writeTempMesh(m, "patina_sym.ptmesh")}});
  json r = run("mesh_symmetry");
  CHECK(r["verdict"] == "symmetric");
  CHECK(r["beyond"] == 0 && r["unmatched"] == 0);
  CHECK(std::abs(r["plane_x"].get<float>()) < 1e-4f);
  // push one vertex on the +X side outwards: it and its mirror partner show up
  Mesh bent = e->doc.mesh;
  uint32_t v = 0;
  float best = -1;
  for (uint32_t i = 0; i < bent.positions.size(); ++i) {
    vec3 p = bent.positions[i];
    float score = p.x - std::abs(p.y) - std::abs(p.z);  // the vertex nearest +X on the equator
    if (score > best) best = score, v = i;
  }
  vec3 orig = bent.positions[v];
  bent.positions[v] = orig * 1.05f;
  bent.finalize();
  run("open_file", {{"path", writeTempMesh(bent, "patina_sym2.ptmesh")}});
  r = run("mesh_symmetry", {{"show", true}, {"vertices", true}});
  {
    bool found = false;  // the bent vertex is listed (its mirror partner may be too)
    for (auto& i : r["beyond_vertices"]) found |= i.get<uint32_t>() == v;
    CHECK(found);
    CHECK(r["beyond_vertices"].size() == (size_t)r["beyond"].get<int>());
  }
  CHECK(r["verdict"] != "symmetric");
  CHECK(r["beyond"].get<int>() >= 1);
  CHECK(r["max"].get<float>() > 0.02f);
  CHECK(!r["worst"].empty());
  // worst spot is the bent vertex or its mirror; Blender axes are (x, -z, y)
  json p = r["worst"][0]["position"];
  vec3 w(p[0].get<float>(), p[2].get<float>(), -p[1].get<float>());
  CHECK(glm::length(vec3(std::abs(w.x), w.y, w.z) - vec3(std::abs(orig.x), orig.y, orig.z)) < 0.1f);
  CHECK(e->viewOptions.mode == ViewMode::Symmetry);
  // the heat map renders
  std::vector<uint8_t> png = e->renderViewportPng(128, 96);
  CHECK(!png.empty());
  // a custom plane is kept (window, heat map) until set back to automatic
  json cp = run("mesh_symmetry", {{"plane_x", 0.2}});
  CHECK(std::abs(cp["plane_x"].get<float>() - 0.2f) < 1e-6f && std::abs(e->doc.symmetry().planeX - 0.2f) < 1e-6f);
  cp = run("mesh_symmetry", {{"plane_x", nullptr}});
  CHECK(std::abs(cp["plane_x"].get<float>()) < 1e-4f);
  // switched off, the command refuses
  run("features", {{"id", "symmetry_check"}, {"on", false}});
  bool refused = false;
  try {
    run("mesh_symmetry");
  } catch (const std::exception&) {
    refused = true;
  }
  CHECK(refused);
  run("features", {{"id", "symmetry_check"}, {"on", true}});
  run("set_view", {{"mode", "material"}});
}

TEST(gpu, texture_set_lock_and_isolate) {
  Engine* e = engine();
  if (!e) return;
  // a cube: the face in front is "Body", the rest "Clothes"
  run("new_project", {{"primitive", "cube"}, {"resolution", 512}});
  run("set_camera", {{"view", "front"}, {"frame", true}});
  run("set_camera", {{"yaw", 0.6}, {"pitch", 0.2}});
  RayHit a = e->pick({0.36f * 512, 256}), b = e->pick({0.64f * 512, 256});
  CHECK(a.hit && b.hit && a.face != b.face);
  if (!a.hit || !b.hit) return;
  Mesh m = e->doc.mesh;
  m.name = "LockCube";
  m.materialNames = {"Body", "Clothes"};
  for (uint32_t f = 0; f < m.faceCount(); ++f) m.faceMaterial[f] = f == a.face ? 0 : 1;
  m.finalize();
  run("open_file", {{"path", writeTempMesh(m, "patina_lockcube.ptmesh")}});
  run("set_camera", {{"view", "front"}, {"frame", true}});
  run("set_camera", {{"yaw", 0.6}, {"pitch", 0.2}});
  run("set_tool", {{"tool", "paint"}});
  run("set_brush", {{"preset", "basic_hard"}, {"size", 30}, {"hardness", 0.9}, {"flow", 1.0}, {"opacity", 1.0},
                    {"spacing", 0.1}, {"projection", "screen"}, {"lock_topology", false}, {"lock_island", false}});
  run("set_paint_material", {{"id", ""}, {"color", {1, 0, 0}}});
  json across = {{"points", {{0.36, 0.5}, {0.5, 0.5}, {0.64, 0.5}}}};
  auto pa = [&] { return e->pick({0.36f * 512, 256}); };
  auto pb = [&] { return e->pick({0.64f * 512, 256}); };
  run("paint_stroke", across);
  CHECK(isRed(texelAt(*e, pa().uv)) && isRed(texelAt(*e, pb().uv)));
  run("undo");
  // set lock with one texture: the material slot it starts on
  json st = run("set_brush", {{"lock_set", true}});
  CHECK(st["lock_set"].get<bool>());
  run("paint_stroke", across);
  CHECK(isRed(texelAt(*e, pa().uv)));
  CHECK(!isRed(texelAt(*e, pb().uv)));
  run("undo");
  run("set_brush", {{"lock_set", false}});
  // texture sets: isolating "Clothes" ghosts the body and keeps strokes off it
  run("texture_tiles", {{"mode", "material_slots"}});
  CHECK(e->doc.mesh.tiles.count() == 2);
  int clothes = e->doc.mesh.faceTile(b.face);
  json v = run("set_view", {{"isolate_set", true}, {"texture_set", clothes}});
  CHECK(v["isolate_set"].get<bool>() && e->painter.isolateTile == clothes);
  run("paint_stroke", across);  // starts on the body
  CHECK(!isRed(texelAt(*e, pa().uv)));
  CHECK(isRed(texelAt(*e, pb().uv)));
  std::vector<uint8_t> png = e->renderViewportPng(128, 96);
  CHECK(!png.empty());
  run("undo");
  run("set_view", {{"isolate_set", false}});
  CHECK(e->painter.isolateTile == -1);
  {
    bool bad = false;  // no such texture set
    try {
      run("set_view", {{"texture_set", 7}, {"isolate_set", true}});
    } catch (const std::exception&) {
      bad = true;
    }
    CHECK(bad && !e->viewOptions.isolateSet);
  }
  // layered PSD per texture set
  fs::path dir = fs::temp_directory_path() / "patina_sets_psd";
  fs::remove_all(dir);
  json ex = run("export_textures", {{"directory", dir.string()}, {"preset", "blender"}, {"format", "psd"}});
  for (const char* set : {"Body", "Clothes"}) {
    fs::path f = dir / (std::string("LockCube_") + set + "_basecolor.psd");
    CHECK(fs::exists(f));
    if (!fs::exists(f)) continue;
    auto b = readAll(f.string());
    CHECK(b.size() > 26 && std::memcmp(b.data(), "8BPS", 4) == 0);
    CHECK((int)rd32(b, 14) == e->doc.tileRes() && (int)rd32(b, 18) == e->doc.tileRes());  // height, width
  }
  // switched off, the tools refuse
  run("features", {{"id", "texture_set_tools"}, {"on", false}});
  bool refused = false;
  try {
    run("set_view", {{"isolate_set", true}});
  } catch (const std::exception&) {
    refused = true;
  }
  CHECK(refused);
  run("features", {{"id", "texture_set_tools"}, {"on", true}});
}

TEST(gpu, auto_texture_from_slot_names) {
  Engine* e = engine();
  if (!e) return;
  auto best = [&](const std::string& name, const std::string& style = "") {
    Mesh m = makeCube(1.f);
    m.materialNames = {name};
    for (auto& s : m.faceMaterial) s = 0;
    auto r = matchSlotMaterials(m, e->materials, style);
    return r.empty() ? std::string("?") : r[0].materialId;
  };
  for (const char* n : {"Steel", "Wood", "Bricks", "Tire", "Gold", "Concrete", "Hull", "Copper_Pipe", "Dragon"})
    CHECK(!best(n).empty());
  CHECK(best("Shirt").find("linen") != std::string::npos);
  CHECK(best("Skin").find("skin_") != std::string::npos);
  CHECK(best("M_Hair.001").find("hair_") != std::string::npos);
  CHECK(best("Jeans").find("denim") != std::string::npos);
  CHECK(best("boots_leather").find("leather") != std::string::npos);
  CHECK(best("Gun_Metal") == "patina_gunmetal");
  CHECK(best("Toon_Hair").find("hair_anime") != std::string::npos);
  CHECK(best("Eyes").empty() && best("Material.001").empty() && best("Glass").empty());
  // the command: a fill layer per matched slot, in a folder, masked to the slot
  Mesh m = makeCube(2.f);
  m.name = "AutoCube";
  m.materialNames = {"Skin", "Jeans", "Eyes"};
  for (uint32_t f = 0; f < m.faceCount(); ++f) m.faceMaterial[f] = f % 3;
  m.finalize();
  run("open_file", {{"path", writeTempMesh(m, "patina_autocube.ptmesh")}});
  size_t before = e->doc.layers.size();
  json dry = run("auto_texture", {{"apply", false}});
  CHECK(dry["layers_added"] == 0 && e->doc.layers.size() == before);
  CHECK(dry["slots"].size() == 3 && dry["slots"][2]["material"] == "");
  json r = run("auto_texture", {{"overrides", {{"Jeans", "patina_flat_blue"}}}});
  CHECK(r["layers_added"] == 2);
  CHECK(e->doc.layers.size() == before + 3);  // a folder and two fills
  vec4 skin = faceColor(*e, 0), jeans = faceColor(*e, 1);
  CHECK(jeans.z > jeans.x + 0.2f);   // the override: flat blue
  CHECK(skin.x > skin.z + 0.05f);    // skin tone
  run("features", {{"id", "auto_texture"}, {"on", false}});
  bool refused = false;
  try {
    run("auto_texture");
  } catch (const std::exception&) {
    refused = true;
  }
  CHECK(refused);
  run("features", {{"id", "auto_texture"}, {"on", true}});
}

TEST(gpu, macro_recorder) {
  Engine* e = engine();
  if (!e) return;
  run("new_project", {{"primitive", "sphere"}, {"resolution", 512}});
  size_t base = e->doc.layers.size();
  run("macro_record", {{"action", "start"}});
  run("add_layer", {{"type", "fill"}, {"name", "Rust"}, {"material", {{"id", "patina_rust_heavy"}}},
                    {"generator", {{"type", "cavity"}}}});
  run("get_state");  // queries are not recorded
  run("add_layer", {{"type", "paint"}, {"name", "Details"}});
  // what is undone drops out of the macro, and comes back with redo
  run("add_layer", {{"type", "paint"}, {"name", "Oops"}});
  CHECK(e->macroSteps().size() == 3);
  run("undo");
  CHECK(e->macroSteps().size() == 2);
  run("redo");
  CHECK(e->macroSteps().size() == 3);
  run("undo");
  run("select_layer", {{"index", 0}});  // after an undo, a new step drops the redo history: "Oops" is gone for good
  run("add_layer", {{"type", "paint"}, {"name", "Details2"}});
  run("undo");
  CHECK(e->macroSteps().size() == 3);  // add_layer x2 + select_layer
  run("redo");
  CHECK(e->macroSteps().size() == 4);
  run("macro_record", {{"action", "remove"}, {"index", 3}});
  run("macro_record", {{"action", "remove"}, {"index", 2}});
  json st = run("macro_record", {{"action", "stop"}});
  CHECK(!st["recording"].get<bool>());
  CHECK(st["steps"].size() == 2 && st["steps"][0]["cmd"] == "add_layer");
  CHECK(e->doc.layers.size() == base + 3);
  // not recording: nothing is added
  run("add_layer", {{"type", "paint"}});
  CHECK(e->macroSteps().size() == 2);
  fs::path job = fs::temp_directory_path() / "patina_macro_test.json";
  json sv = run("macro_save", {{"name", "rust pass"}, {"path", job.string()}});
  CHECK(fs::exists(job));
  // a fresh model: the saved macro replays the two layers
  run("new_project", {{"primitive", "cube"}, {"resolution", 512}});
  size_t b2 = e->doc.layers.size();
  json r = run("macro_play", {{"path", job.string()}});
  CHECK(r["ok"].get<bool>() && r["played"] == 2);
  CHECK(e->doc.layers.size() == b2 + 2);
  CHECK(e->doc.layers.back()->name == "Details" || e->doc.layers[e->doc.layers.size() - 2]->name == "Details");
  CHECK(e->macroSteps().size() == 2);  // playing does not record
  // a failing step stops the replay and says which
  r = run("macro_play", {{"steps", {{{"cmd", "add_layer"}, {"args", {{"type", "paint"}}}},
                                    {{"cmd", "select_layer"}, {"args", {{"index", 999}}}},
                                    {{"cmd", "add_layer"}, {"args", {{"type", "paint"}}}}}}});
  CHECK(!r["ok"].get<bool>() && r["failed_step"] == 1 && r["played"] == 1);
  run("macro_record", {{"action", "clear"}});
  run("features", {{"id", "macro_recorder"}, {"on", false}});
  bool refused = false;
  try {
    run("macro_record", {{"action", "start"}});
  } catch (const std::exception&) {
    refused = true;
  }
  CHECK(refused);
  run("features", {{"id", "macro_recorder"}, {"on", true}});
}

// ---------------------------------------------------------------- Patina Assistant (0.11)

namespace {
size_t historyCount(Engine& e) { return e.doc.undo.position(); }
bool threw(const char* cmd, const json& args, std::string* msg = nullptr) {
  try {
    run(cmd, args);
  } catch (std::exception& ex) {
    if (msg) *msg = ex.what();
    return true;
  }
  return false;
}
void loadTestMesh(Engine& e, Mesh m) {
  e.doc.setMesh(std::move(m), 512, false);
  e.doc.clearHistory();
  run("set_camera", {{"view", "front"}, {"frame", true}});
}
float maskAt(Engine& e, const Layer& l, uint32_t f) {
  const Mesh& m = e.doc.mesh;
  vec2 c(0);
  for (uint32_t k = m.faceStart[f]; k < m.faceStart[f + 1]; ++k) c += m.texUV(k);
  c /= float(m.faceSize(f));
  int R = l.mask->width();
  float v[4];
  glGetTextureSubImage(l.mask->id(), 0, std::clamp(int(c.x * R), 0, R - 1), std::clamp(int(c.y * R), 0, R - 1), 0, 1, 1, 1,
                       GL_RGBA, GL_FLOAT, sizeof v, v);
  return v[0];
}
}  // namespace

TEST(gpu, undo_groups) {
  Engine* e = engine();
  if (!e) return;
  run("new_project", {{"primitive", "sphere"}, {"resolution", 512}});
  Document& d = e->doc;
  size_t layers = d.layers.size(), hist = historyCount(*e);
  d.beginUndoGroup("Three layers");
  run("add_layer", {{"type", "paint"}, {"name", "A"}});
  run("add_layer", {{"type", "fill"}, {"name", "B"}, {"material", {{"id", "patina_rust_heavy"}}}});
  run("update_layer", {{"name", "C"}, {"opacity", 0.5}});
  run("add_layer", {{"type", "paint"}, {"name", "D"}});
  CHECK(d.endUndoGroup());
  CHECK(historyCount(*e) == hist + 1);
  CHECK(d.undo.undoLabel() == "Three layers");
  CHECK(d.layers.size() == layers + 3);
  CHECK(d.undoStep());
  CHECK(d.layers.size() == layers);
  CHECK(d.redoStep());
  CHECK(d.layers.size() == layers + 3 && d.layers[d.layers.size() - 2]->name == "C");
  // nested: only the outer group folds
  hist = historyCount(*e);
  d.beginUndoGroup("Outer");
  d.beginUndoGroup("Inner");
  run("add_layer", {{"type", "paint"}});
  CHECK(!d.endUndoGroup());
  run("add_layer", {{"type", "paint"}});
  CHECK(d.endUndoGroup());
  CHECK(historyCount(*e) == hist + 1 && d.undo.undoLabel() == "Outer");
  // a group with one entry keeps it, under the group's name
  d.beginUndoGroup("Just one");
  run("add_layer", {{"type", "paint"}});
  d.endUndoGroup();
  CHECK(d.undo.undoLabel() == "Just one");
  // cancel rolls back and leaves no trace
  layers = d.layers.size();
  hist = historyCount(*e);
  size_t total = d.undo.count();
  d.beginUndoGroup("Cancelled");
  run("add_layer", {{"type", "paint"}, {"name", "X"}});
  run("add_layer", {{"type", "fill"}, {"name", "Y"}});
  d.cancelUndoGroup();
  CHECK(d.layers.size() == layers && historyCount(*e) == hist && d.undo.count() == total);
  CHECK(d.redoStep() == false);
  // an empty group pushes nothing
  d.beginUndoGroup("Empty");
  CHECK(!d.endUndoGroup());
  CHECK(historyCount(*e) == hist);
  // the macro recorder sees a group as its steps, all undone and redone together
  run("macro_record", {{"action", "start"}});
  d.beginUndoGroup("Recorded");
  run("add_layer", {{"type", "paint"}, {"name", "M1"}});
  run("add_layer", {{"type", "paint"}, {"name", "M2"}});
  d.endUndoGroup();
  CHECK(e->macroSteps().size() == 2);
  d.undoStep();
  CHECK(e->macroSteps().empty());
  d.redoStep();
  CHECK(e->macroSteps().size() == 2);
  run("macro_record", {{"action", "stop"}});
  run("macro_record", {{"action", "clear"}});
}

TEST(gpu, assistant_parts_and_selection) {
  Engine* e = engine();
  if (!e) return;
  loadTestMesh(*e, testmesh::makeBust(24, 12));
  json p = run("mesh_parts");
  CHECK(p["head_found"].get<bool>());
  std::set<std::string> ids;
  for (auto& x : p["parts"]) ids.insert(x["id"].get<std::string>());
  CHECK(ids.count("slot:Skin") && ids.count("slot:Cloth") && ids.count("region:head") && ids.count("region:body"));
  size_t hist = historyCount(*e);
  json r = run("select_part", {{"part", "region:head"}});
  size_t head = r["selected"];
  CHECK(head == size_t(24 * 12));
  r = run("select_part", {{"part", "slot:Cloth"}, {"mode", "add"}});
  CHECK(r["selected"].get<size_t>() == e->doc.mesh.faceCount());
  r = run("select_part", {{"part", "region:head"}, {"mode", "remove"}});
  CHECK(r["selected"].get<size_t>() == e->doc.mesh.faceCount() - head);
  CHECK(historyCount(*e) == hist + 3);
  run("undo");
  CHECK(e->doc.selectedFaces().size() == e->doc.mesh.faceCount());
  // the selection is a part too
  p = run("mesh_parts");
  CHECK(p["parts"].back()["id"] == "selection");
  // an unknown part names the ones there are; no head on a cube says why
  std::string msg;
  CHECK(threw("select_part", {{"part", "slot:Nope"}}, &msg));
  CHECK(msg.find("slot:Skin") != std::string::npos);
  run("new_project", {{"primitive", "cube"}, {"resolution", 512}});
  CHECK(threw("select_part", {{"part", "region:head"}}, &msg));
  CHECK(msg.find("no head found") != std::string::npos);
}

TEST(gpu, assistant_tint_and_material_layers) {
  Engine* e = engine();
  if (!e) return;
  loadTestMesh(*e, testmesh::makeBust(24, 12));
  Document& d = e->doc;
  const Mesh& m = d.mesh;
  uint32_t skinFace = 0, clothFace = 0;
  for (uint32_t f = 0; f < m.faceCount(); ++f) {
    if (m.faceMaterial[f] == 0 && m.faceCenter(f).y > 1.1f && m.faceCenter(f).z > 0.3f) skinFace = f;
    if (m.faceMaterial[f] == 1 && m.faceCenter(f).y < 0.f && m.faceCenter(f).z > 0.2f) clothFace = f;
  }
  size_t layers = d.layers.size(), hist = historyCount(*e);
  json r = run("add_material_layer", {{"material", "patina_skin_light"}, {"region", {{"part", "slot:Skin"}}}});
  CHECK(historyCount(*e) == hist + 1);
  vec4 clothBefore = faceColor(*e, clothFace);
  // the handoff's first example, as one command
  r = run("add_tint_layer", {{"name", "Skin crease redness"}, {"color", {1.0, 0.0, 0.0}}, {"strength", 1.0},
                             {"region", {{"part", "slot:Skin"}}}, {"mask", {{{"type", "cavity"}, {"level", 0.0}, {"contrast", 0.0}}}}});
  CHECK(historyCount(*e) == hist + 2);
  CHECK(d.undo.undoLabel() == "Tint layer: Skin crease redness");
  Layer& t = *d.layers[r["index"].get<int>()];
  CHECK(t.type == LayerType::Fill && t.channelMask == 1);
  CHECK(t.maskEffects.size() == 2);
  if (t.maskEffects.size() == 2) {
    CHECK(t.maskEffects[0].gen.type == MaskGenerator::MaterialSlot && t.maskEffects[0].gen.slot == 0);
    CHECK(t.maskEffects[1].gen.type == MaskGenerator::Cavity && t.maskEffects[1].blend == MaskEffect::Multiply);
  }
  vec4 skin = faceColor(*e, skinFace);
  CHECK(skin.x > skin.y + 0.3f);  // level 0: the tint shows on every skin face
  vec4 clothAfter = faceColor(*e, clothFace);
  CHECK(glm::length(clothAfter - clothBefore) < 0.02f);  // nothing on the cloth
  run("undo");
  CHECK(d.layers.size() == layers + 1);
  run("undo");
  CHECK(d.layers.size() == layers);
  // a region that is not a slot is a painted mask of its faces; except leaves parts out
  r = run("add_tint_layer", {{"color", {0.2, 0.4, 1.0}}, {"region", {{"parts", {"region:head", "region:body"}}, {"except", {"region:body"}}}}});
  Layer& h = *d.layers[r["index"].get<int>()];
  CHECK(h.mask != nullptr && !h.maskEffects.empty() && h.maskEffects[0].type == MaskEffect::Paint);
  if (h.mask) {
    CHECK(maskAt(*e, h, skinFace) > 0.9f);
    CHECK(maskAt(*e, h, clothFace) < 0.1f);
  }
  CHECK(r["region_faces"].get<size_t>() == size_t(24 * 12));
  json rf = run("layer_region_faces", {{"index", r["index"]}});
  CHECK(!rf["all"].get<bool>() && rf["count"].get<size_t>() == size_t(24 * 12));
  run("undo");
  // bad input changes nothing
  hist = historyCount(*e);
  layers = d.layers.size();
  CHECK(threw("add_material_layer", {{"material", "no_such_material"}}));
  CHECK(threw("add_tint_layer", {{"color", {1, 0, 0}}, {"region", {{"part", "piece:9"}}}}));
  CHECK(threw("add_tint_layer", {{"color", {1, 0, 0}}, {"mask", {{{"type", "sparkles"}}}}}));
  CHECK(d.layers.size() == layers && historyCount(*e) == hist);
}

TEST(gpu, assistant_island_per_part) {
  Engine* e = engine();
  if (!e) return;
  // the handoff's second example on the bust, as one command
  loadTestMesh(*e, testmesh::makeBust(24, 12));
  Document& d = e->doc;
  std::vector<vec2> before = d.mesh.loopUV;
  size_t hist = historyCount(*e);
  json r = run("uv_island_per_part", {{"parts", {"region:head", "region:body"}}});
  CHECK(historyCount(*e) == hist + 1);
  CHECK(r["overlaps"].get<int>() == 0);
  for (auto& p : r["parts"]) CHECK(p["islands"].get<int>() == 1);
  CHECK(r["notes"].is_array() && r["notes"].empty());  // nothing lies over itself: nothing to say
  run("undo");
  CHECK(d.mesh.loopUV == before);
  // one welded figure: the head is cut off at the neck, the body is a tube with two openings
  // (the neck and the bottom) that needs a cut down its back
  loadTestMesh(*e, testmesh::makeLatheFigure());
  r = run("uv_island_per_part", {{"parts", {"region:head", "region:body"}}});
  CHECK(r["parts"].size() == 2);
  if (r["parts"].size() == 2) {
    CHECK(r["parts"][0]["islands"].get<int>() == 1);
    CHECK(r["parts"][1]["islands"].get<int>() == 1);
    CHECK(r["parts"][1]["cut_edges"].get<int>() > 0);
    CHECK(r["parts"][1]["stretch_p95"].get<float>() < 2.5f);
  }
  CHECK(r["islands"].get<int>() == 2 && r["overlaps"].get<int>() == 0);
  // a part that does not exist changes nothing
  hist = historyCount(*e);
  CHECK(threw("uv_island_per_part", {{"parts", {"region:head", "group:Ears"}}}));
  CHECK(historyCount(*e) == hist);
}

TEST(gpu, assistant_context) {
  Engine* e = engine();
  if (!e) return;
  loadTestMesh(*e, testmesh::makeBust(24, 12));
  run("add_layer", {{"type", "fill"}, {"name", "Base skin"}, {"material", {{"id", "patina_skin_light"}}}});
  json c = run("assistant_context", {{"request", "make the armor look rusty"}});
  std::string t = c["text"];
  CHECK(t.find("region:head") != std::string::npos && t.find("slot:Cloth") != std::string::npos);
  CHECK(t.find("\"Base skin\" fill patina_skin_light") != std::string::npos);
  CHECK(c["tokens"].get<int>() < 1500);
  bool rust = false;
  for (auto& id : c["materials"]) rust |= id.get<std::string>().find("rust") != std::string::npos;
  CHECK(rust);
  // a whole look names no material, but means the weathering ones
  c = run("assistant_context", {{"request", "make it look battle-worn"}});
  std::set<std::string> listed;
  for (auto& id : c["materials"]) listed.insert(id.get<std::string>());
  CHECK(listed.count("patina_steel_scratched") && listed.count("patina_dirt_mud") && listed.count("patina_soot"));
  run("new_project", {{"primitive", "cube"}, {"resolution", 512}});
  c = run("assistant_context", {{"request", "make the head red"}});
  CHECK(c["text"].get<std::string>().find("Head: not found") != std::string::npos);
}

TEST(gpu, assistant_plans) {
  Engine* e = engine();
  if (!e) return;
  loadTestMesh(*e, testmesh::makeBust(24, 12));
  Document& d = e->doc;
  size_t layers = stackLayers(d), hist = historyCount(*e);
  // the first example as a plan of two steps; the second step uses the first one's result
  json plan = {{"say", "Adds a soft red tint in the skin's creases."},
               {"steps",
                {{{"cmd", "add_tint_layer"},
                  {"args", {{"name", "Skin crease redness"}, {"color", {0.75, 0.18, 0.15}}, {"strength", 0.25},
                            {"blend", "soft_light"}, {"region", {{"part", "slot:Skin"}}},
                            {"mask", {{{"type", "cavity"}, {"level", 0.55}, {"contrast", 0.6}}}}}}},
                 {{"cmd", "update_layer"}, {"args", {{"index", "$0.index"}, {"opacity", 0.3}}}}}}};
  json chk = run("assistant_check", {{"plan", plan}});
  CHECK(chk["ok"].get<bool>());
  CHECK(chk["steps"][0].get<std::string>().find("creases") != std::string::npos);
  CHECK(chk["steps"][1].get<std::string>().find("the new layer") != std::string::npos);  // not "$0.index"
  json r = run("assistant_apply", {{"plan", plan}});
  CHECK(r["steps"] == 2 && r["results"].size() == 2);
  CHECK(r["notes"].is_array() && r["notes"].empty());
  CHECK(stackLayers(d) == layers + 1 && std::abs(d.layers.back()->opacity - 0.3f) < 1e-5f);
  CHECK(d.layers.back()->colorTag == assistant::kLayerTag);
  CHECK(historyCount(*e) == hist + 1);
  CHECK(d.undo.undoLabel().rfind("Assistant: Adds a soft red tint", 0) == 0);
  run("undo");
  CHECK(stackLayers(d) == layers);
  run("redo");
  CHECK(stackLayers(d) == layers + 1);
  // refused before anything runs
  auto refused = [&](json p, const char* why) {
    std::string msg;
    bool t = threw("assistant_apply", {{"plan", p}}, &msg);
    if (!t || msg.find(why) == std::string::npos) std::fprintf(stderr, "  (expected '%s', got '%s')\n", why, msg.c_str());
    return t && msg.find(why) != std::string::npos;
  };
  hist = historyCount(*e);
  CHECK(refused({{"steps", {{{"cmd", "save_project"}, {"args", {{"path", "/tmp/x.patina"}}}}}}}, "can't be used"));
  CHECK(refused({{"steps", {{{"cmd", "add_tint_layer"}, {"args", {{"color", {1, 0, 0}}, {"sparkle", true}}}}}}}, "unknown argument"));
  CHECK(refused({{"steps", {{{"cmd", "update_layer"}, {"args", {{"index", "$0.index"}}}}}}}, "has not run yet"));
  CHECK(refused({{"steps", {{{"cmd", "add_layer"}, {"args", {{"name", "$HOME"}}}}}}}, "not a reference"));
  CHECK(refused({{"steps", {{{"cmd", "add_tint_layer"}, {"args", {{"color", {1, 0, 0}}, {"strength", 2.0}}}}}}}, "above"));
  json many = json::array();
  for (int i = 0; i < 17; ++i) many.push_back({{"cmd", "add_layer"}, {"args", json::object()}});
  CHECK(refused({{"steps", many}}, "at most 16"));
  CHECK(refused(json::object(), "does nothing"));
  CHECK(historyCount(*e) == hist);
  // a step that fails undoes the ones before it
  layers = stackLayers(d);
  std::string msg;
  CHECK(threw("assistant_apply", {{"plan", {{"steps", {{{"cmd", "add_layer"}, {"args", {{"name", "first"}}}},
                                                          {{"cmd", "add_material_layer"}, {"args", {{"material", "no_such"}}}}}}}}}, &msg));
  CHECK(msg.find("step 1") != std::string::npos && msg.find("undone") != std::string::npos);
  CHECK(stackLayers(d) == layers && historyCount(*e) == hist);
  // several new layers share a folder named after the request, in the same undo step
  hist = historyCount(*e);
  layers = stackLayers(d);
  run("assistant_apply", {{"plan", {{"say", "Rust and dirt"},
                                    {"steps", {{{"cmd", "add_material_layer"}, {"args", {{"material", "patina_rust_heavy"}}}},
                                               {{"cmd", "add_material_layer"}, {"args", {{"material", "patina_dirt_mud"}}}}}}}}});
  CHECK(stackLayers(d) == layers + 3 && historyCount(*e) == hist + 1);
  Layer* folder = nullptr;
  for (auto& l : d.layers)
    if (l->isFolder() && l->name == "Rust and dirt") folder = l.get();
  CHECK(folder != nullptr);
  if (folder) CHECK(d.childrenOf(folder->uid).size() == 2);
  run("undo");
  CHECK(stackLayers(d) == layers);
  // questions and refusals change nothing
  r = run("assistant_apply", {{"plan", {{"ask", "Which part is the head? I can see: slot:Skin, slot:Cloth"}}}});
  CHECK(r["steps"] == 0 && historyCount(*e) == hist);
  // the macro recorder keeps the plan's steps, not the plan
  run("macro_record", {{"action", "start"}});
  run("assistant_apply", {{"plan", plan}});
  auto steps = e->macroSteps();
  CHECK(steps.size() == 2 && steps[0]["cmd"] == "add_tint_layer" && steps[1]["cmd"] == "update_layer");
  run("macro_record", {{"action", "stop"}});
  run("macro_record", {{"action", "clear"}});
  // what a request made shows in the context for follow-ups; undone requests drop out
  run("assistant_apply", {{"plan", plan}, {"request", "make the skin creases a bit redder"}});
  std::string ctx = run("assistant_context", {{"request", "a bit stronger"}})["text"];
  CHECK(ctx.find("Last request: \"make the skin creases a bit redder\" -> made layer") != std::string::npos);
  CHECK(ctx.find("\"Skin crease redness\"") != std::string::npos);
  run("undo");
  ctx = run("assistant_context", {{"request", "a bit stronger"}})["text"];
  CHECK(ctx.find("make the skin creases a bit redder") == std::string::npos);
  // layer numbers mean the stack the model was shown: "delete 1, then 2" deletes those two layers
  // although the first deletion moves the second one down
  int base = (int)d.layers.size();  // (flat indices: the stack the model is shown)
  for (const char* n : {"Keep A", "Gone B", "Gone C"}) run("add_layer", {{"name", n}, {"index", (int)d.layers.size()}});
  json del = {{"say", "Deletes two layers."},
              {"steps", {{{"cmd", "remove_layer"}, {"args", {{"index", base + 1}}}},
                         {{"cmd", "remove_layer"}, {"args", {{"index", base + 2}}}}}}};
  run("assistant_apply", {{"plan", del}});
  CHECK((int)d.layers.size() == base + 1 && d.layers.back()->name == "Keep A");
  // switched off: refused
  run("features", {{"id", "patina_assistant"}, {"on", false}});
  CHECK(threw("assistant_apply", {{"plan", plan}}));
  run("features", {{"id", "patina_assistant"}, {"on", true}});
}

TEST(gpu, assistant_layer_masks_and_smart_regions) {
  Engine* e = engine();
  if (!e) return;
  loadTestMesh(*e, testmesh::makeBust(24, 12));
  Document& d = e->doc;
  json r = run("add_tint_layer", {{"color", {1, 0, 0}}, {"region", {{"part", "slot:Skin"}}}, {"mask", {{{"type", "cavity"}}}}});
  int i = r["index"];
  size_t hist = historyCount(*e);
  // a new region keeps the smart masks
  run("update_layer_mask", {{"index", i}, {"region", {{"part", "slot:Cloth"}}}});
  Layer* l = d.layers[i].get();
  CHECK(l->maskEffects.size() == 2 && l->maskEffects[0].gen.slot == 1 && l->maskEffects[1].gen.type == MaskGenerator::Cavity);
  // new smart masks keep the region
  run("update_layer_mask", {{"index", i}, {"mask", json::array({{{"type", "edges"}}, {{"type", "noise"}}})}});
  CHECK(l->maskEffects.size() == 3 && l->maskEffects[0].gen.slot == 1 && l->maskEffects[1].gen.type == MaskGenerator::Edges &&
        l->maskEffects[2].gen.type == MaskGenerator::Noise);
  // everywhere: only the smart masks
  run("update_layer_mask", {{"index", i}, {"everywhere", true}});
  CHECK(l->maskEffects.size() == 2 && l->maskEffects[0].gen.type == MaskGenerator::Edges &&
        l->maskEffects[0].blend == MaskEffect::Normal);
  CHECK(historyCount(*e) == hist + 3);
  run("undo");
  run("undo");
  CHECK(l->maskEffects.size() == 2 && l->maskEffects[0].gen.slot == 1);
  // a smart material limited to a part, as one undo step
  hist = historyCount(*e);
  size_t layers = d.layers.size();
  r = run("apply_smart_material", {{"id", "patina_smart_worn_leather"}, {"region", {{"part", "slot:Cloth"}}}});
  CHECK(historyCount(*e) == hist + 1 && d.layers.size() > layers);
  Layer& sm = *d.layers[r["index"].get<int>()];
  CHECK(!sm.maskEffects.empty() && sm.maskEffects[0].gen.type == MaskGenerator::MaterialSlot && sm.maskEffects[0].gen.slot == 1);
  run("undo");
  CHECK(d.layers.size() == layers);
  CHECK(threw("apply_smart_material", {{"id", "patina_smart_worn_leather"}, {"region", {{"part", "group:Nope"}}}}));
  CHECK(d.layers.size() == layers);
}

TEST(gpu, assistant_model_end_to_end) {
  // Needs a model file: PATINA_TEST_GGUF=/path/to/model.gguf (kept out of the repo). Skipped without.
  const char* gguf = std::getenv("PATINA_TEST_GGUF");
  if (!gguf || !fs::exists(gguf)) {
    std::fprintf(stderr, "  (skipped: set PATINA_TEST_GGUF to a model file)\n");
    return;
  }
  Engine* e = engine();
  if (!e) return;
  loadTestMesh(*e, testmesh::makeBust(24, 12));
  e->assistantModelPath = gguf;
  json st = run("assistant_model", {{"action", "load"}});
  CHECK(st["state"] == "ready");
  // the grammar holds: whatever the model writes is a plan Patina accepts
  for (const char* req : {"Set up the UVs so I have one UV island for the head and another for the body",
                          "Make the creases of the skin slightly redder", "Show me the curvature map"}) {
    json r = run("assistant_ask", {{"request", req}, {"timeout", 120}});
    CHECK(r["ok"].get<bool>());
    if (!r["ok"].get<bool>()) std::fprintf(stderr, "  %s -> %s (%s)\n", req, r["raw"].dump().c_str(), r["error"].dump().c_str());
    CHECK(r["prompt_tokens"].get<int>() > 100 && r["output_tokens"].get<int>() > 5);
  }
  // a plan applies and undoes as one step
  json r = run("assistant_ask", {{"request", "Set up the UVs so I have one UV island for the head and another for the body"}});
  std::vector<vec2> before = e->doc.mesh.loopUV;
  size_t hist = historyCount(*e);
  if (r["plan"].contains("steps")) {
    std::string msg;
    bool failed = threw("assistant_apply", {{"plan", r["plan"]}}, &msg);
    if (!failed) {
      CHECK(historyCount(*e) == hist + 1);
      run("undo");
      CHECK(e->doc.mesh.loopUV == before);
    } else {
      CHECK(historyCount(*e) == hist);  // a failing plan leaves nothing behind
    }
  }
  run("assistant_model", {{"action", "unload"}});
}

// 0.11.1: "New texture set from selection": the faces get a material slot (texture set) of their
// own, the paint moves with them, and Blender gets the slots back with the next send
TEST(gpu, texture_set_from_selection) {
  Engine* e = engine();
  if (!e) return;
  freshSphere(*e);
  run("paint_stroke", {{"points", {{0.47, 0.5}, {0.53, 0.5}}}});
  RayHit h = e->pick({256, 256});
  CHECK(h.hit && isRed(texelAt(*e, h.uv)));
  // the front of the sphere: faces facing the camera
  std::vector<uint32_t> front;
  for (uint32_t f = 0; f < e->doc.mesh.faceCount(); ++f)
    if (e->doc.mesh.faceNormal(f).z > 0.5f) front.push_back(f);
  json r = run("texture_set_from_faces", {{"faces", front}, {"name", "Face"}});
  CHECK(r["set"] == "Face" && r["index"] == 1 && r["packed"] == true);
  CHECK(e->doc.mesh.tiles.active() && e->doc.mesh.tiles.count() == 2);
  CHECK(e->doc.mesh.faceTile(h.face) == 1);
  CHECK(e->doc.mesh.slotName(e->doc.mesh.faceMaterial[h.face]) == "Face");
  RayHit h2 = e->pick({256, 256});
  CHECK(h2.hit && isRed(texelAt(*e, h2.uv)));  // the paint came along
  CHECK(e->doc.bridge.slotsEdited);
  // the same name again: those faces are there already
  bool threw = false;
  try {
    run("texture_set_from_faces", {{"faces", front}, {"name", "face"}});
  } catch (std::exception&) {
    threw = true;
  }
  CHECK(threw);
  // the slots go to Blender with the UVs
  auto dir = paths::ensureDir(paths::cacheDir() / "test" / "sets");
  std::string uvp = (dir / "Sphere.ptuv").string(), err;
  CHECK(savePtUV(uvp, e->doc.mesh, &err, true));
  container::File f;
  CHECK(container::readFile(uvp, "PTUV0001", f, &err));
  std::vector<uint32_t> slots;
  CHECK(f.read("face_slots", slots, e->doc.mesh.faceCount()));
  CHECK(slots.size() > h.face && slots[h.face] == e->doc.mesh.faceMaterial[h.face]);
  CHECK(f.header["slot_names"].size() == 2 && f.header["slot_names"][1] == "Face");
  // and a project keeps it all
  std::string proj = (dir / "sets.patina").string();
  run("save_project", {{"path", proj}});
  run("new_project", {{"primitive", "cube"}});
  run("open_file", {{"path", proj}});
  CHECK(e->doc.mesh.tiles.count() == 2 && e->doc.bridge.slotsEdited);  // still to go to Blender
  RayHit h3 = e->pick({256, 256});
  CHECK(h3.hit && isRed(texelAt(*e, h3.uv)));
}

// 0.12: suggestions, a last step that clears the undo history, no files, glow, open_window,
// more texture space for a part
TEST(gpu, assistant_plans_round6) {
  Engine* e = engine();
  if (!e) return;
  loadTestMesh(*e, testmesh::makeBust(24, 12));
  Document& d = e->doc;
  auto byName = [&](const std::string& n) -> Layer* {
    for (auto& l : d.layers)
      if (l->name == n) return l.get();
    return nullptr;
  };
  auto refusedWith = [&](json p, const char* why) {
    std::string msg;
    bool t = threw("assistant_check", {{"plan", p}}, &msg);
    json r;
    if (!t) r = run("assistant_check", {{"plan", p}});
    std::string err = t ? msg : r.value("error", std::string());
    bool ok = (t || !r.value("ok", true)) && err.find(why) != std::string::npos;
    if (!ok) std::fprintf(stderr, "  (expected '%s', got '%s')\n", why, err.c_str());
    return ok;
  };
  // can't, with things it can do
  json cant = {{"say", "Patina doesn't change the shape."}, {"suggest", {"Make the cloth leather", "Add dirt in the creases"}}};
  CHECK(run("assistant_check", {{"plan", cant}})["ok"].get<bool>());
  CHECK(refusedWith({{"say", "x"}, {"suggest", {"a", "b", "c", "d", "e"}}}, "1 to 4"));
  CHECK(refusedWith({{"say", "x"}, {"suggest", json::array()}}, "1 to 4"));
  // the grammar allows suggestions after say and after ask
  std::string g = assistant::buildGrammar(*e);
  CHECK(g.find("suggest") != std::string::npos);
  // a step that clears the undo history comes last, and runs after the plan's undo step
  json texset = {{"say", "The head gets its own texture set."},
                 {"steps", {{{"cmd", "add_tint_layer"}, {"args", {{"color", {1, 0, 0}}, {"name", "Red"}}}},
                            {{"cmd", "texture_set_from_faces"}, {"args", {{"name", "Head"}, {"part", "region:head"}}}}}}};
  CHECK(run("assistant_check", {{"plan", texset}})["ok"].get<bool>());
  json wrongOrder = {{"say", "x"}, {"steps", {texset["steps"][1], texset["steps"][0]}}};
  CHECK(refusedWith(wrongOrder, "last step"));
  CHECK(refusedWith({{"say", "x"}, {"steps", {{{"cmd", "set_stencil"}, {"args", {{"path", "/etc/passwd"}}}}}}}, "files"));
  json r = run("assistant_apply", {{"plan", texset}});
  CHECK(r["cleared_history"] == true);
  CHECK(d.mesh.tiles.active() && d.mesh.tiles.count() == 2);
  CHECK(byName("Red") != nullptr);
  // glow: the emissive channel, not the base color
  run("assistant_apply", {{"plan", {{"say", "Glow."},
                                    {"steps", {{{"cmd", "add_tint_layer"},
                                                {"args", {{"color", {0.1, 0.4, 1.0}}, {"glow", true}, {"name", "Blue glow"}}}}}}}}});
  Layer* glow = byName("Blue glow");
  CHECK(glow && glow->channelMask == (1u << (int)Channel::Emissive) && glow->material.flat.emissive.z > 0.9f);
  // open_window without a window says so instead of failing
  json w = run("open_window", {{"window", "export"}, {"preset", "unreal"}});
  CHECK(w["opened"] == false && e->lastOpenedWindow == "export");
  CHECK(run("get_state")["last_window"] == "export");
}

TEST(gpu, uv_density_scale_gives_a_part_more_texture_space) {
  Engine* e = engine();
  if (!e) return;
  loadTestMesh(*e, testmesh::makeLatheFigure());  // one piece: the head shares islands with the body
  run("uv_unwrap", {{"method", "smart"}});
  auto parts = run("mesh_parts", {{"faces", true}})["parts"];
  std::vector<uint32_t> head, rest;
  for (auto& p : parts)
    if (p["id"] == "region:head") head = p["face_indices"].get<std::vector<uint32_t>>();
  CHECK(!head.empty());
  if (head.empty()) return;
  std::vector<uint8_t> isHead(e->doc.mesh.faceCount(), 0);
  for (uint32_t f : head) isHead[f] = 1;
  for (uint32_t f = 0; f < isHead.size(); ++f)
    if (!isHead[f]) rest.push_back(f);
  auto dens = [&](const std::vector<uint32_t>& f) { return run("uv_density", {{"faces", f}})["faces_density"].get<double>(); };
  double before = dens(head) / dens(rest);
  run("select_part", {{"part", "region:head"}});
  run("uv_density", {{"scale", 2.0}, {"selection", true}});
  double after = dens(head) / dens(rest);
  CHECK(after / before > 1.8 && after / before < 2.2);
  CHECK(run("uv_info")["overlapping_islands"] == 0);
}

namespace {

// a baked procedural material's maps (linear float RGBA, row 0 = top)
struct Bake {
  Image color, rmh, extra;
};
Bake bakeProc(Engine& e, const MaterialDef& d, int size = 256) {
  MaterialDef m = d;
  m.id = "__test_bake__";
  m.type = MaterialType::Procedural;
  MaterialLibrary::Entry* en = e.materials.upsertTransient(m);
  e.materials.ensureBaked(*en, size);
  return {en->gpu.color.download(4), en->gpu.rmh.download(4), en->gpu.extra.download(4)};
}
double meanOf(const Image& im, int c) {
  double s = 0;
  for (int y = 0; y < im.height; ++y)
    for (int x = 0; x < im.width; ++x) s += im.at(x, y)[c];
  return s / (double(im.width) * im.height);
}
// how much the image varies, and how its wrap-around seams compare with the strongest edge inside it (a pattern
// may have an edge on the tile's border, as stripes do, but no worse than the same edges inside)
void seamStats(const Image& im, double* range, double* seamRatio, bool rows = true) {
  int W = im.width, H = im.height;
  auto lum = [&](int x, int y) { const float* p = im.at(x, y); return double(p[0] + p[1] + p[2]); };
  double mn = 1e9, mx = -1e9;
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) mn = std::min(mn, lum(x, y)), mx = std::max(mx, lum(x, y));
  double worstCol = 1e-6, worstRow = 1e-6, seamCol = 0, seamRow = 0;
  for (int x = 0; x < W; ++x) {
    double d = 0;
    for (int y = 0; y < H; ++y) d += std::abs(lum(x, y) - lum((x + 1) % W, y));
    d /= H;
    if (x == W - 1) seamCol = d;
    else worstCol = std::max(worstCol, d);
  }
  for (int y = 0; y < H; ++y) {
    double d = 0;
    for (int x = 0; x < W; ++x) d += std::abs(lum(x, y) - lum(x, (y + 1) % H));
    d /= W;
    if (y == H - 1) seamRow = d;
    else worstRow = std::max(worstRow, d);
  }
  *range = mx - mn;
  *seamRatio = rows ? std::max(seamCol / worstCol, seamRow / worstRow) : seamCol / worstCol;
}

}  // namespace

TEST(gpu, procedural_generators_024) {
  Engine* e = engine();
  if (!e) return;
  // every generator has its settings described, in the same order
  auto names = generatorNames();
  CHECK(names.size() == 74);
  for (int i = 0; i < (int)names.size(); ++i) CHECK(names[i] == generatorInfo(i).id);
  // the new generators, in each of their styles: a pattern that tiles
  for (int g = generatorFromName("stripes"); g <= generatorFromName("paper"); ++g) {
    const GeneratorInfo& gi = generatorInfo(g);
    int styleSlot = -1, styles = 1;
    for (int k = 0; k < 4; ++k)
      if (gi.extra[k].options) {
        styleSlot = k;
        styles = 1;
        for (const char* c = gi.extra[k].options; *c; ++c) styles += *c == '|';
        break;
      }
    for (int st = 0; st < styles; ++st) {
      MaterialDef d;
      d.proc.generator = g;
      d.proc.colorA = vec3(0.8f, 0.7f, 0.6f), d.proc.colorB = vec3(0.1f, 0.2f, 0.4f), d.proc.colorC = vec3(0.9f, 0.3f, 0.1f);
      d.proc.scale = 4, d.proc.heightAmp = 0.5f;
      for (int k = 0; k < 4; ++k)
        if (gi.extra[k].label && !gi.extra[k].options) d.proc.extra[k] = gi.extra[k].def > 0 ? 0.f : 0.5f * (gi.extra[k].lo + gi.extra[k].hi);
      if (styleSlot >= 0) d.proc.extra[styleSlot] = (float)st;
      Bake b = bakeProc(*e, d);
      double range, seam;
      seamStats(b.color, &range, &seam);
      double hRange = 0, hSeam = 0;
      seamStats(b.rmh, &hRange, &hSeam);
      bool ok = (range > 0.05 || hRange > 0.05) && seam < 1.5;
      if (!ok) std::fprintf(stderr, "  %s style %d: range %.3f / %.3f, seam x%.2f\n", gi.id, st, range, hRange, seam);
      CHECK(ok);
    }
  }
  // warp, grime and worn edges, on bricks: each changes the maps; grime darkens, light wear lightens; still tiling
  MaterialDef br;
  br.proc.generator = generatorFromName("bricks");
  br.proc.colorA = vec3(0.6f, 0.25f, 0.15f), br.proc.colorB = vec3(0.7f, 0.35f, 0.2f), br.proc.colorC = vec3(0.7f, 0.7f, 0.65f);
  br.proc.heightAmp = 0.6f;
  Bake plain = bakeProc(*e, br);
  MaterialDef gr = br;
  gr.proc.grime = 0.8f;
  Bake grimy = bakeProc(*e, gr);
  MaterialDef wo = br;
  wo.proc.wear = 0.8f;
  Bake worn = bakeProc(*e, wo);
  MaterialDef wa = br;
  wa.proc.warp = 0.5f;
  Bake warped = bakeProc(*e, wa);
  double l0 = meanOf(plain.color, 0) + meanOf(plain.color, 1) + meanOf(plain.color, 2);
  double lg = meanOf(grimy.color, 0) + meanOf(grimy.color, 1) + meanOf(grimy.color, 2);
  double lw = meanOf(worn.color, 0) + meanOf(worn.color, 1) + meanOf(worn.color, 2);
  CHECK(lg < l0 - 0.03);
  CHECK(lw > l0 + 0.01);
  CHECK(meanOf(grimy.rmh, 0) > meanOf(plain.rmh, 0));  // dirt is rough
  double diff = 0;
  for (size_t i = 0; i < plain.color.pixels.size(); ++i) diff += std::abs(plain.color.pixels[i] - warped.color.pixels[i]);
  CHECK(diff / plain.color.pixels.size() > 0.02);
  for (Bake* b : {&grimy, &worn, &warped}) {
    double range, seam;
    seamStats(b->color, &range, &seam);
    CHECK(seam < 1.5);
  }
  // the settings are kept in the material's file
  MaterialDef back;
  MaterialDef src = br;
  src.id = "x", src.proc.warp = 0.3f, src.proc.warpScale = 6, src.proc.grime = 0.4f, src.proc.grimeColor = vec3(0.1f, 0.2f, 0.3f);
  src.proc.wear = 0.7f, src.proc.wearColor = vec3(0.9f, 0.8f, 0.7f);
  std::string err;
  CHECK(MaterialDef::fromJson(src.toJson(), back, ".", &err));
  CHECK(std::abs(back.proc.warp - 0.3f) < 1e-5f && std::abs(back.proc.warpScale - 6.f) < 1e-5f);
  CHECK(std::abs(back.proc.grime - 0.4f) < 1e-5f && std::abs(back.proc.grimeColor.y - 0.2f) < 0.01f);
  CHECK(std::abs(back.proc.wear - 0.7f) < 1e-5f && std::abs(back.proc.wearColor.x - 0.9f) < 0.01f);
}

namespace {

// the selected layer's mask: mean over the covered texels, and its value at a uv
struct MaskRead {
  Image m, pos;
  double mean() const {
    double s = 0;
    long n = 0;
    for (int y = 0; y < m.height; ++y)
      for (int x = 0; x < m.width; ++x)
        if (pos.at(x, y)[3] > 0.5f) s += m.at(x, y)[0], ++n;
    return n ? s / n : 0;
  }
  float at(vec2 uv) const {
    int x = std::clamp(int(uv.x * m.width), 0, m.width - 1), y = std::clamp(m.height - 1 - int(uv.y * m.height), 0, m.height - 1);
    return m.at(x, y)[0];
  }
};
MaskRead readMask(Engine& e, const json& stack) {
  run("update_layer", {{"mask_effects", stack}});
  e.doc.updateComposite(e.materials);
  return {e.doc.tex.layerMaskImage(*e.doc.selectedLayer(), e.doc.mesh.bounds), e.doc.tex.posMap.download(4)};
}

}  // namespace

TEST(gpu, mask_effects_024) {
  Engine* e = engine();
  if (!e) return;
  run("new_project", {{"primitive", "sphere"}, {"resolution", 512}});
  RayHit top = e->doc.bvh.raycast(e->doc.mesh, vec3(0, 3, 0.05f), vec3(0, -1, 0));
  RayHit px = e->doc.bvh.raycast(e->doc.mesh, vec3(3, 0.02f, 0.05f), vec3(-1, 0, 0));
  RayHit nx = e->doc.bvh.raycast(e->doc.mesh, vec3(-3, 0.02f, 0.05f), vec3(1, 0, 0));
  CHECK(top.hit && px.hit && nx.hit);
  run("add_layer", {{"type", "fill"}, {"material", {{"id", ""}, {"color", {1, 0, 0}}}}});
  // a gradient along +X: white at +X, black at -X
  MaskRead g = readMask(*e, json::array({{{"type", "generator"},
                                           {"generator", {{"type", "gradient"}, {"direction", {1, 0, 0}}, {"level", 0.5}, {"contrast", 0.8}}}}}));
  CHECK(g.at(px.uv) > 0.9f && g.at(nx.uv) < 0.1f);
  // the kinds of noise: each covers part of the model, and they differ
  std::vector<double> means;
  for (const char* kind : {"clouds", "cells", "cracks", "ridges", "streaks"}) {
    MaskRead n = readMask(*e, json::array({{{"type", "noise"}, {"scale", 2.0}, {"contrast", 0.6}, {"noise_type", kind}}}));
    double m = n.mean();
    if (m < 0.03 || m > 0.97) std::fprintf(stderr, "  noise %s covers %.3f\n", kind, m);
    CHECK(m > 0.03 && m < 0.97);
    means.push_back(m);
  }
  CHECK(std::abs(means[1] - means[2]) > 0.02 || std::abs(means[0] - means[3]) > 0.02);
  // grow and shrink, edges
  json base = json::array({{{"type", "noise"}, {"scale", 2.0}, {"contrast", 0.8}, {"noise_type", "cells"}},
                           {{"type", "levels"}, {"in_low", 0.6}, {"in_high", 0.7}}});
  double b = readMask(*e, base).mean();
  json grown = base, shrunk = base, edged = base;
  grown.push_back({{"type", "grow"}, {"amount", 4.0}});
  shrunk.push_back({{"type", "grow"}, {"amount", -4.0}});
  edged.push_back({{"type", "edges"}, {"radius", 3.0}});
  double gm = readMask(*e, grown).mean(), sm = readMask(*e, shrunk).mean(), em = readMask(*e, edged).mean();
  CHECK(gm > b + 0.02 && sm < b - 0.02);
  CHECK(em > 0.01 && em < 0.6);
  // drips: on the sides, never on top
  MaskRead d = readMask(*e, json::array({{{"type", "generator"}, {"generator", {{"type", "drips"}, {"level", 0.3}, {"contrast", 0.6}}}}}));
  CHECK(d.mean() > 0.005 && d.mean() < 0.5);
  CHECK(d.at(top.uv) < 0.05f);
  // a material's pattern as a mask; inverted, the rest
  json pat = {{"type", "pattern"}, {"material", {{"id", "patina_bricks_red"}}}, {"source", "height"}, {"level", 0.5}, {"contrast", 0.7}};
  double pm = readMask(*e, json::array({pat})).mean();
  pat["invert"] = true;
  double pi = readMask(*e, json::array({pat})).mean();
  CHECK(pm > 0.1 && pm < 0.95);
  CHECK(std::abs(pm + pi - 1.0) < 0.05);
  // kept when saved and opened again
  json stack = json::array({{{"type", "noise"}, {"noise_type", "cracks"}},
                            {{"type", "grow"}, {"amount", -3.0}},
                            {{"type", "edges"}, {"radius", 5.0}},
                            {{"type", "pattern"}, {"material", {{"id", "patina_bricks_red"}, {"scale", 2.0}}}, {"source", "brightness"},
                             {"level", 0.4}, {"invert", true}},
                            {{"type", "generator"}, {"generator", {{"type", "gradient"}, {"direction", {0, 0, 1}}}}}});
  run("update_layer", {{"mask_effects", stack}});
  std::string proj = tmpFile("patina_test_maskfx024.patina");
  run("save_project", {{"path", proj}});
  run("new_project", {{"primitive", "cube"}});
  run("open_file", {{"path", proj}});
  const Layer* L = nullptr;
  for (auto& l : e->doc.layers)
    if (l->maskEffects.size() == 5) L = l.get();
  CHECK(L != nullptr);
  if (L) {
    CHECK(L->maskEffects[0].noiseType == 2);
    CHECK(std::abs(L->maskEffects[1].amount + 3.f) < 1e-4f);
    CHECK(L->maskEffects[2].type == MaskEffect::Edges && std::abs(L->maskEffects[2].radius - 5.f) < 1e-4f);
    const MaskEffect& p = L->maskEffects[3];
    CHECK(p.type == MaskEffect::Pattern && p.pattern.id == "patina_bricks_red" && std::abs(p.pattern.scale - 2.f) < 1e-4f);
    CHECK(p.source == 1 && std::abs(p.level - 0.4f) < 1e-4f && p.invert);
    CHECK(L->maskEffects[4].gen.type == MaskGenerator::Gradient && std::abs(L->maskEffects[4].gen.direction.z - 1.f) < 1e-4f);
  }
  fs::remove(proj);
  // add_material_layer takes them as smart masks
  run("new_project", {{"primitive", "sphere"}, {"resolution", 512}});
  json r = run("add_material_layer", {{"material", "patina_rust_heavy"},
                                      {"mask", json::array({{{"type", "drips"}}, {{"type", "noise"}, {"kind", "cells"}},
                                                            {{"type", "pattern"}, {"material", "patina_bricks_red"}},
                                                            {{"type", "gradient"}, {"direction", {1, 0, 0}}}})}});
  const Layer* m = e->doc.selectedLayer();
  CHECK(m->maskEffects.size() == 4);
  if (m->maskEffects.size() == 4) {
    CHECK(m->maskEffects[0].gen.type == MaskGenerator::Drips);
    CHECK(m->maskEffects[1].gen.type == MaskGenerator::Noise && m->maskEffects[1].gen.noiseType == 1);
    CHECK(m->maskEffects[2].type == MaskEffect::Pattern && m->maskEffects[2].blend == MaskEffect::Multiply);
    CHECK(m->maskEffects[3].gen.type == MaskGenerator::Gradient);
  }
}

// ---- 0.26: more procedural materials, and opacity, sheen, clearcoat and subsurface

TEST(gpu, procedural_generators_026) {
  Engine* e = engine();
  if (!e) return;
  // the new generators, in each of their styles: a pattern that tiles (hair cards only across: their strands end
  // part way down the card)
  for (int g = generatorFromName("fur"); g <= generatorFromName("velvet"); ++g) {
    const GeneratorInfo& gi = generatorInfo(g);
    int styleSlot = -1, styles = 1;
    for (int k = 0; k < 4; ++k)
      if (gi.extra[k].options) {
        styleSlot = k;
        for (const char* c = gi.extra[k].options; *c; ++c) styles += *c == '|';
        break;
      }
    for (int st = 0; st < styles; ++st) {
      MaterialDef d;
      d.proc.generator = g;
      d.proc.colorA = vec3(0.8f, 0.7f, 0.6f), d.proc.colorB = vec3(0.1f, 0.2f, 0.4f), d.proc.colorC = vec3(0.9f, 0.3f, 0.1f);
      d.proc.scale = 3, d.proc.heightAmp = 0.5f;
      for (int k = 0; k < 4; ++k)
        if (gi.extra[k].label && !gi.extra[k].options) d.proc.extra[k] = gi.extra[k].def > 0 ? 0.f : 0.5f * (gi.extra[k].lo + gi.extra[k].hi);
      if (styleSlot >= 0) d.proc.extra[styleSlot] = (float)st;
      Bake b = bakeProc(*e, d);
      bool rows = std::string(gi.id) != "hair_cards";
      double range, seam, hRange = 0, hSeam = 0;
      seamStats(b.color, &range, &seam, rows);
      seamStats(b.rmh, &hRange, &hSeam, rows);
      bool ok = (range > 0.05 || hRange > 0.05) && seam < 1.5;
      if (!ok) std::fprintf(stderr, "  %s style %d: range %.3f / %.3f, seam x%.2f\n", gi.id, st, range, hRange, seam);
      CHECK(ok);
      // without Cut out holes, everything is solid
      CHECK(meanOf(b.extra, 0) > 0.999);
    }
  }
  // holes: see-through when Cut out holes is on
  MaterialDef lace;
  lace.proc.generator = generatorFromName("lace");
  lace.proc.scale = 2;
  lace.proc.cutout = true;
  Bake cut = bakeProc(*e, lace);
  double op = meanOf(cut.extra, 0);
  CHECK(op > 0.1 && op < 0.9);
  MaterialRef ref;
  ref.id = "__test_bake__";
  CHECK(e->materials.extraChannelsUsed(ref) == 64);
  for (const char* holes : {"net", "leaves", "hair_cards", "perforated", "grille", "chainmail"}) {
    MaterialDef h;
    h.proc.generator = generatorFromName(holes);
    h.proc.cutout = true;
    h.proc.scale = 4;
    if (std::string(holes) == "leaves") h.proc.extra[0] = 2;  // foliage
    double m = meanOf(bakeProc(*e, h).extra, 0);
    if (!(m > 0.03 && m < 0.97)) std::fprintf(stderr, "  %s: opacity %.3f\n", holes, m);
    CHECK(m > 0.03 && m < 0.97);
  }
  // a generator without holes ignores Cut out holes
  MaterialDef br;
  br.proc.generator = generatorFromName("bricks");
  br.proc.cutout = true;
  CHECK(meanOf(bakeProc(*e, br).extra, 0) > 0.999);
  CHECK(e->materials.extraChannelsUsed(ref) == 0);
  // sheen, clearcoat and subsurface go through; crushed velvet varies its sheen
  MaterialDef v;
  v.proc.generator = generatorFromName("velvet");
  v.proc.extra[0] = 1;
  v.proc.sheen = 0.8f, v.proc.coat = 0.3f, v.proc.subsurface = 0.5f, v.proc.opacity = 0.9f;
  Bake vb = bakeProc(*e, v);
  CHECK(meanOf(vb.extra, 1) > 0.4 && meanOf(vb.extra, 1) < 0.81);
  CHECK(std::abs(meanOf(vb.extra, 2) - 0.3) < 0.01 && std::abs(meanOf(vb.extra, 3) - 0.5) < 0.01);
  CHECK(std::abs(meanOf(vb.extra, 0) - 0.9) < 0.01);
  CHECK(e->materials.extraChannelsUsed(ref) == (64 | 128 | 256 | 512));
}

namespace {
// the mean of one channel of the final extra map (opacity, sheen, clearcoat, subsurface) over the model's texels
double extraMean(Engine& e, int c) {
  e.doc.updateComposite(e.materials);
  Image x = e.doc.tex.finExtra.download(4), pos = e.doc.tex.posMap.download(4);
  double s = 0, n = 0;
  for (size_t i = 0; i < size_t(x.width) * x.height; ++i)
    if (pos.pixels[i * 4 + 3] > 0.5f) s += x.pixels[i * 4 + c], n += 1;
  return n > 0 ? s / n : -1;
}
float extraAt(Engine& e, vec2 uv, int c) {
  e.doc.updateComposite(e.materials);
  int R = e.doc.tex.resolution();
  int x = std::clamp(int(uv.x * R), 0, R - 1), y = std::clamp(int(uv.y * R), 0, R - 1);
  float v[4];
  glGetTextureSubImage(e.doc.tex.finExtra.id(), 0, x, y, 0, 1, 1, 1, GL_RGBA, GL_FLOAT, sizeof v, v);
  return v[c];
}
}  // namespace

// (1.2.1) the four channels of 0.26 survive a save: the save's table of samples per channel had only the first six,
// so opacity, sheen and subsurface came back empty and clearcoat crashed the save (and every autosave) outright
TEST(gpu, extra_channels_saved_121) {
  Engine* e = engine();
  if (!e) return;
  freshSphere(*e);
  run("add_layer", {{"type", "paint"}});
  RayHit h = e->pick({256, 256});
  CHECK(h.hit);
  run("set_paint_material", {{"opacity", 0.2}, {"sheen", 0.7}, {"coat", 1.0}, {"subsurface", 0.5}});
  run("paint_stroke", {{"points", {{0.5, 0.5}}}});
  Layer* L = e->doc.selectedLayer();
  CHECK(L && L->hasChannel((int)Channel::Opacity) && L->hasChannel((int)Channel::Sheen) &&
        L->hasChannel((int)Channel::Coat) && L->hasChannel((int)Channel::Subsurface));
  std::string path = (fs::temp_directory_path() / "patina_extra_saved_test.patina").string();
  run("save_project", {{"path", path}});
  run("set_paint_material", {{"opacity", 1.0}, {"sheen", 0.0}, {"coat", 0.0}, {"subsurface", 0.0}});
  run("open_file", {{"path", path}});
  L = e->doc.layers.empty() ? nullptr : e->doc.layers.back().get();
  CHECK(L && L->hasChannel((int)Channel::Coat) && L->hasChannel((int)Channel::Sheen));
  CHECK(std::abs(extraAt(*e, h.uv, 0) - 0.2f) < 0.03f && std::abs(extraAt(*e, h.uv, 1) - 0.7f) < 0.03f);
  CHECK(std::abs(extraAt(*e, h.uv, 2) - 1.f) < 0.03f && std::abs(extraAt(*e, h.uv, 3) - 0.5f) < 0.03f);
  std::error_code ec;
  fs::remove(path, ec);
}

TEST(gpu, extra_channels_026) {
  Engine* e = engine();
  if (!e) return;
  freshSphere(*e);
  Document& d = e->doc;
  // nothing uses them: opaque, no sheen, clearcoat or subsurface; exports leave them out
  CHECK(extraMean(*e, 0) > 0.999 && extraMean(*e, 1) < 0.001 && d.tex.extraUsed == 0);
  // a fill layer whose (flat) material has them
  run("add_layer", {{"type", "fill"},
                    {"material", {{"id", ""}, {"color", {1, 0, 0}}, {"opacity", 0.4}, {"sheen", 0.7}, {"coat", 0.5}, {"subsurface", 0.3}}}});
  CHECK(std::abs(extraMean(*e, 0) - 0.4) < 0.02 && std::abs(extraMean(*e, 1) - 0.7) < 0.02);
  CHECK(std::abs(extraMean(*e, 2) - 0.5) < 0.02 && std::abs(extraMean(*e, 3) - 0.3) < 0.02);
  CHECK(d.tex.extraUsed == kExtraChannels);
  // exports: Blender gets a map each; glTF keeps opacity in the base color's alpha
  std::string dir = (fs::temp_directory_path() / "patina_test_extra026").string();
  json files = run("export_textures", {{"directory", dir}, {"preset", "blender"}})["files"];
  CHECK(files.contains("opacity") && files.contains("sheen") && files.contains("coat") && files.contains("subsurface"));
  Image img;
  if (files.contains("opacity") && loadImage(files["opacity"].get<std::string>(), img, 1)) {
    double s = 0;
    for (float v : img.pixels) s += v;
    CHECK(std::abs(s / img.pixels.size() - 0.4) < 0.03);
  }
  json gl = run("export_textures", {{"directory", dir}, {"preset", "gltf"}})["files"];
  CHECK(!gl.contains("opacity") && gl.contains("basecolor"));
  if (gl.contains("basecolor") && loadImage(gl["basecolor"].get<std::string>(), img)) CHECK(img.channels == 4);
  // the layer's channels can leave them out
  run("update_layer", {{"channels", {"basecolor"}}});
  CHECK(extraMean(*e, 0) > 0.999 && d.tex.extraUsed == 0);
  json plain = run("export_textures", {{"directory", dir}, {"preset", "blender"}})["files"];
  CHECK(!plain.contains("opacity") && !plain.contains("sheen"));
  run("remove_layer", {{"index", d.selected}});
  // painting: a plain brush leaves them alone, a brush with opacity paints it, the eraser takes it away
  run("add_layer", {{"type", "paint"}});
  RayHit h = e->pick({256, 256});
  CHECK(h.hit);
  run("paint_stroke", {{"points", {{0.5, 0.5}}}});
  Layer* L = d.selectedLayer();
  CHECK(L && L->hasChannel((int)Channel::Color) && !L->hasChannel((int)Channel::Opacity) && !L->hasChannel((int)Channel::Sheen));
  run("set_paint_material", {{"opacity", 0.2}, {"coat", 1.0}});
  run("paint_stroke", {{"points", {{0.5, 0.5}}}});
  CHECK(L->hasChannel((int)Channel::Opacity) && L->hasChannel((int)Channel::Coat) && !L->hasChannel((int)Channel::Sheen));
  CHECK(std::abs(extraAt(*e, h.uv, 0) - 0.2f) < 0.03f && std::abs(extraAt(*e, h.uv, 2) - 1.f) < 0.03f);
  run("undo");
  CHECK(extraAt(*e, h.uv, 0) > 0.99f);
  run("redo");
  CHECK(extraAt(*e, h.uv, 0) < 0.25f);
  run("set_paint_material", {{"opacity", 1.0}, {"coat", 0.0}});
  run("paint_stroke", {{"points", {{0.5, 0.5}}}, {"tool", "erase"}});
  CHECK(extraAt(*e, h.uv, 0) > 0.99f && extraAt(*e, h.uv, 2) < 0.01f);
  // a paint layer with only some channels: opacity left out
  run("set_paint_material", {{"opacity", 0.3}});
  run("set_paint_channels", {{"channels", {"basecolor"}}});
  run("paint_stroke", {{"points", {{0.5, 0.5}}}, {"tool", "paint"}});
  CHECK(extraAt(*e, h.uv, 0) > 0.99f);
  // the base material
  run("set_paint_channels", {{"channels", {"all"}}});
  run("set_paint_material", {{"opacity", 1.0}});
  d.tex.base.subsurface = 0.6f;
  d.markComposite();
  CHECK(std::abs(extraMean(*e, 3) - 0.6) < 0.02 && (d.tex.extraUsed & 512));
  d.tex.base.subsurface = 0.f;
  d.markComposite();
  // view modes and render_uv show them
  CHECK(viewModeFromName("opacity") == ViewMode::Opacity && viewModeFromName("subsurface") == ViewMode::Subsurface);
  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST(gpu, mesh_check_and_fix_027) {
  Engine* e = engine();
  if (!e) return;
  // a cube with one face missing, as an OBJ file
  fs::path dir = fs::temp_directory_path() / "patina_test_meshfix027";
  fs::create_directories(dir);
  fs::path obj = dir / "holey.obj";
  {
    std::ofstream f(obj);
    f << "v -1 -1 -1\nv 1 -1 -1\nv 1 1 -1\nv -1 1 -1\nv -1 -1 1\nv 1 -1 1\nv 1 1 1\nv -1 1 1\n";
    f << "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n";
    f << "f 1/1 4/4 3/3 2/2\nf 5/1 6/2 7/3 8/4\nf 1/1 2/2 6/3 5/4\nf 2/1 3/2 7/3 6/4\nf 3/1 4/2 8/3 7/4\n";
  }
  json o = run("open_file", {{"path", obj.string()}});
  Document& d = e->doc;
  CHECK(d.mesh.faceCount() == 5);
  json r = run("mesh_check");
  bool hole = false;
  for (auto& p : r["problems"]) hole |= p["id"] == "holes" && p["count"] == 1;
  CHECK(hole && r["stats"]["closed"] == false && r["stats"]["faces"] == 5);
  // paint it all red, then close the hole: the paint stays, the undo history goes, a copy is kept
  run("add_layer", {{"type", "fill"}, {"material", {{"id", ""}, {"color", {1, 0, 0}}}}});
  size_t layers = d.layers.size();
  json fx = run("mesh_fix", {{"fill_holes", true}});
  CHECK(fx["changed"] == true && fx["holes_filled"] == 1 && d.mesh.faceCount() == 6);
  CHECK(d.layers.size() == layers && fx["can_revert"] == true);
  r = run("mesh_check", {{"show", true}});
  CHECK(r["stats"]["closed"] == true && e->viewOptions.mode == ViewMode::MeshCheck);
  e->viewOptions.mode = ViewMode::Material;
  // a face of the old surface is still red
  CHECK(isRed(texelAt(*e, d.mesh.loopUV[d.mesh.faceStart[1]] * 0.5f + d.mesh.loopUV[d.mesh.faceStart[1] + 2] * 0.5f)));
  // nothing more to mend
  json again = run("mesh_fix", {{"all", true}});
  CHECK(again["changed"] == false);
  // back to before the fix
  run("mesh_fix_revert");
  CHECK(d.mesh.faceCount() == 5 && d.layers.size() == layers && !d.canRevertMeshFix());
  // select a problem's faces
  json sel = run("mesh_check", {{"select", "holes"}});
  CHECK(sel.value("selected", 0) == 4 && d.selectedFaces().size() == 4);
  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST(gpu, export_model_028) {
  Engine* e = engine();
  if (!e) return;
  freshSphere(*e);
  Document& d = e->doc;
  run("add_layer", {{"type", "fill"}, {"material", {{"id", "patina_lace_white"}}}});
  fs::path dir = fs::temp_directory_path() / "patina_test_model028";
  fs::remove_all(dir);
  // .glb: one file, textures inside; lace: see-through (mask) and sheen
  json r = run("export_model", {{"path", (dir / "ball.glb").string()}, {"texture_size", 256}});
  std::string glb = paths::readFile(dir / "ball.glb");
  CHECK(glb.size() > 1000 && glb.compare(0, 4, "glTF") == 0);
  uint32_t jsonLen = 0;
  if (glb.size() > 20) std::memcpy(&jsonLen, glb.data() + 12, 4);
  json g = glb.size() > 20 + jsonLen ? json::parse(glb.substr(20, jsonLen), nullptr, false) : json();
  CHECK(g.is_object() && g["asset"]["version"] == "2.0");
  if (g.is_object()) {
    CHECK(g["meshes"][0]["primitives"].size() == 1 && g["materials"].size() == 1);
    json& mat = g["materials"][0];
    CHECK(mat["pbrMetallicRoughness"].contains("baseColorTexture") && mat.contains("normalTexture"));
    CHECK(mat.value("alphaMode", std::string()) == "MASK" || mat.value("alphaMode", std::string()) == "BLEND");
    CHECK(mat["extensions"].contains("KHR_materials_sheen"));
    CHECK(g["images"].size() >= 3 && g["images"][0].contains("bufferView"));
    size_t verts = g["accessors"][0]["count"];
    CHECK(verts >= d.mesh.positions.size() && verts <= d.mesh.loopCount());
    size_t idx = g["accessors"][4]["count"];
    CHECK(idx == d.mesh.tris.size() * 3);
  }
  CHECK(r["alpha_mode"] != "OPAQUE");
  // .gltf: the buffer and the PNG files beside it
  r = run("export_model", {{"path", (dir / "ball.gltf").string()}, {"texture_size", 256}});
  CHECK(fs::exists(dir / "ball.bin") && r["textures"].contains("basecolor") && fs::exists(r["textures"]["basecolor"].get<std::string>()));
  // .obj: reads back with the same faces; the .mtl names the maps
  r = run("export_model", {{"path", (dir / "ball.obj").string()}, {"texture_size", 256}});
  Mesh back;
  std::string err;
  CHECK(loadMesh((dir / "ball.obj").string(), back, &err) && back.faceCount() == d.mesh.faceCount());
  std::string mtl = paths::readFile(dir / "ball.mtl");
  CHECK(mtl.find("map_Kd ") != std::string::npos && mtl.find("map_d ") != std::string::npos);
  // a mesh only
  r = run("export_model", {{"path", (dir / "plain.glb").string()}, {"textures", false}});
  json g2;
  std::string glb2 = paths::readFile(dir / "plain.glb");
  if (glb2.size() > 20) {
    std::memcpy(&jsonLen, glb2.data() + 12, 4);
    g2 = json::parse(glb2.substr(20, jsonLen), nullptr, false);
  }
  CHECK(g2.is_object() && !g2.contains("images"));
  // a wrong extension
  bool threw = false;
  try {
    run("export_model", {{"path", (dir / "x.fbx").string()}});
  } catch (std::exception&) {
    threw = true;
  }
  CHECK(threw);
  fs::remove_all(dir);
}

TEST(gpu, mesh_reduce_and_lods_029) {
  Engine* e = engine();
  if (!e) return;
  freshSphere(*e);
  Document& d = e->doc;
  RayHit h = e->pick({256, 256});
  run("paint_stroke", {{"points", {{0.45, 0.5}, {0.55, 0.5}}}});
  CHECK(isRed(texelAt(*e, h.uv)));
  size_t tris = d.mesh.tris.size();
  json p = run("mesh_reduce", {{"ratio", 0.5}, {"preview", true}});
  CHECK(p["preview"] == true && d.mesh.tris.size() == tris && p["triangles_after"].get<size_t>() < tris);
  json r = run("mesh_reduce", {{"ratio", 0.5}});
  CHECK(r["changed"] == true && d.mesh.tris.size() < tris * 6 / 10 && r["can_revert"] == true);
  // the paint stays where it was: the textures are untouched, the UVs a subset
  RayHit h2 = e->pick({256, 256});
  CHECK(h2.hit && isRed(texelAt(*e, h2.uv)));
  run("mesh_fix_revert");
  CHECK(d.mesh.tris.size() == tris && d.lastMeshChange() == "reduction");
  // levels of detail in the export: meshes _LOD0.._LOD2, the same materials, MSFT_lod
  fs::path dir = fs::temp_directory_path() / "patina_test_lod029";
  fs::remove_all(dir);
  json x = run("export_model", {{"path", (dir / "ball.glb").string()}, {"texture_size", 128}, {"lods", {0.5, 0.25}}});
  CHECK(x["lods"].size() == 2);
  std::string glb = paths::readFile(dir / "ball.glb");
  uint32_t jsonLen = 0;
  if (glb.size() > 20) std::memcpy(&jsonLen, glb.data() + 12, 4);
  json g = glb.size() > 20 + jsonLen ? json::parse(glb.substr(20, jsonLen), nullptr, false) : json();
  CHECK(g.is_object() && g["meshes"].size() == 3 && g["materials"].size() == 1);
  if (g.is_object() && g["meshes"].size() == 3) {
    CHECK(g["nodes"][0]["name"].get<std::string>().find("_LOD0") != std::string::npos);
    CHECK(g["nodes"][0]["extensions"]["MSFT_lod"]["ids"].size() == 2);
    auto idxCount = [&](int m) { return g["accessors"][(int)g["meshes"][m]["primitives"][0]["indices"]]["count"].get<size_t>(); };
    CHECK(idxCount(1) < idxCount(0) && idxCount(2) < idxCount(1));
    CHECK(g["meshes"][2]["primitives"][0]["material"] == 0);
  }
  json o = run("export_model", {{"path", (dir / "ball.obj").string()}, {"textures", false}, {"lods", {0.5}}});
  CHECK(o["lod_files"].size() == 1 && fs::exists(dir / "ball_LOD1.obj"));
  fs::remove_all(dir);
}

// (0.44.2) While a stroke goes on, each update composites only the region its new dabs reach (and updates that
// region's mip levels): the result is what a whole composite makes, at full size and in the mip levels; a full
// composite follows when the stroke ends.
TEST(gpu, stroke_region_composite_matches_full) {
  Engine* e = engine();
  if (!e) return;
  freshSphere(*e);
  run("paint_stroke", {{"points", {{0.3, 0.4}, {0.4, 0.45}}}, {"material", {{"id", ""}, {"color", {0, 0, 1}}}}});
  run("add_layer", {{"type", "paint"}});
  run("set_paint_material", {{"id", ""}, {"color", {1, 0, 0}}});
  Document& d = e->doc;
  int R = d.tex.resolution();
  auto read = [&](int level) {
    int w = std::max(1, R >> level);
    std::vector<float> px(size_t(w) * w * 4);
    glGetTextureImage(d.tex.finColor.id(), level, GL_RGBA, GL_FLOAT, (GLsizei)(px.size() * sizeof(float)), px.data());
    return px;
  };
  Painter::View v{e->camera, e->viewportSize};
  CHECK(e->painter.beginStroke(d, e->materials, e->brushes, v, {vec2(200, 250)}, false, 1.f / R));
  e->painter.update();
  d.updateComposite(e->materials);
  for (int k = 1; k <= 10; ++k) e->painter.addPoint({vec2(200 + 10 * k, 250 + 3 * k)});
  e->painter.update();
  CHECK(d.compositeDirty && !d.compositeFull);  // a region, not everything
  glm::ivec4 box = d.compositeBox;
  d.updateComposite(e->materials);
  auto part0 = read(0), part3 = read(3);
  d.markComposite();
  d.updateComposite(e->materials);
  auto full0 = read(0), full3 = read(3);
  auto worst = [](const std::vector<float>& a, const std::vector<float>& b) {
    float m = 0.f;
    for (size_t i = 0; i < a.size(); ++i) m = std::max(m, std::abs(a[i] - b[i]));
    return m;
  };
  int red = 0;
  for (size_t i = 0; i < full0.size(); i += 4) red += full0[i] > 0.6f && full0[i + 1] < 0.25f;
  std::fprintf(stderr, "  region %dx%d of %d, %d red texels; full size off by %g, mip 3 by %g\n", box.z - box.x,
               box.w - box.y, R, red, worst(part0, full0), worst(part3, full3));
  CHECK(red > 100 && (box.z - box.x) * (box.w - box.y) < R * R / 2);
  CHECK(worst(part0, full0) < 1.5f / 255.f && worst(part3, full3) < 1.5f / 255.f);
  e->painter.endStroke();
  CHECK(d.compositeDirty && d.compositeFull);  // the end of the stroke: everything once
}

// (0.44.2) A UV edit after a bake leaves the baked maps out of date (they moved along with the paint); a re-bake
// makes them current again.
TEST(gpu, bakes_out_of_date_after_uv_edit) {
  Engine* e = engine();
  if (!e) return;
  run("new_project", {{"primitive", "torus"}, {"resolution", 256}});
  run("bake_mesh_maps", {{"ao", true}, {"curvature", true}, {"thickness", false}});
  Document& d = e->doc;
  uint64_t h0 = uv::uvHash(d.mesh);
  CHECK(d.bakesOutOfDate() == 0);
  run("uv_transform", {{"faces", {0}}, {"islands", true}, {"rotate", 90}});
  uv::uvHashNewFrame();
  uint64_t h1 = uv::uvHash(d.mesh);
  std::fprintf(stderr, "  hash %llx -> %llx, baked for %llx, out of date %d\n", (unsigned long long)h0,
               (unsigned long long)h1, (unsigned long long)d.tex.bakedUV[0], d.bakesOutOfDate());
  CHECK(h1 != h0 && d.bakesOutOfDate() == 3);
  d.rebakeOutOfDate();
  CHECK(d.bakesOutOfDate() == 0);
}
