#include <cstring>
#include <cmath>
#include <filesystem>

#include "Test.h"
#include "core/Bvh.h"
#include "core/Container.h"
#include "core/Image.h"
#include "core/Mesh.h"
#include "core/MeshIO.h"
#include "core/Paths.h"

using namespace patina;

TEST(core, cube_topology) {
  Mesh m = makeCube(2.f);
  CHECK(m.faceCount() == 6);
  CHECK(m.loopCount() == 24);
  CHECK(m.edges.size() == 12);
  CHECK(m.tris.size() == 12);
  CHECK(m.islandCount == 6);  // cross layout with gaps between cells
  std::string err;
  CHECK(m.validate(&err));
}

TEST(core, sphere_islands_and_normals) {
  Mesh m = makeUVSphere(32, 16, 1.f);
  CHECK(m.islandCount == 1);
  for (uint32_t l = 0; l < m.loopCount(); ++l) {
    vec3 p = glm::normalize(m.positions[m.loopVert[l]]);
    if (glm::dot(p, m.loopNormal[l]) < 0.95f) {
      CHECK(false);
      break;
    }
  }
  size_t seams = 0;
  for (auto f : m.edgeFlags) seams += (f & EdgeSeam) ? 1 : 0;
  CHECK(seams == 16);
}

TEST(core, ngon_triangulation) {
  Mesh m;
  int n = 7;
  for (int i = 0; i < n; ++i) {
    float a = 2 * kPi * i / n;
    float r = (i % 2) ? 0.5f : 1.f;  // star-ish concave polygon
    m.positions.push_back({r * std::cos(a), 0, r * std::sin(a)});
  }
  m.faceStart = {0, (uint32_t)n};
  for (int i = 0; i < n; ++i) m.loopVert.push_back(n - 1 - i);
  m.finalize();
  CHECK(m.tris.size() == size_t(n - 2));
  float area = 0;
  for (auto& t : m.tris) {
    vec3 a = m.positions[m.loopVert[t.x]], b = m.positions[m.loopVert[t.y]], c = m.positions[m.loopVert[t.z]];
    area += glm::length(glm::cross(b - a, c - a)) * 0.5f;
  }
  CHECK_NEAR(area, m.faceArea3D(0), 1e-3);
}

TEST(core, bvh_raycast) {
  Mesh m = makeUVSphere(48, 24, 1.f);
  Bvh b;
  b.build(m);
  RayHit h = b.raycast(m, vec3(0, 0, 5), vec3(0, 0, -1));
  CHECK(h.hit);
  CHECK_NEAR(h.t, 4.0, 0.02);
  CHECK(h.normal.z > 0.98f);
  RayHit miss = b.raycast(m, vec3(3, 3, 5), vec3(0, 0, -1));
  CHECK(!miss.hit);
}

TEST(core, ptmesh_roundtrip) {
  Mesh m = makeCube(1.f);
  m.edgeFlags[3] |= EdgeSeam;
  auto dir = paths::cacheDir() / "test";
  paths::ensureDir(dir);
  std::string p = (dir / "cube.ptmesh").string();
  std::string err;
  CHECK(savePtMesh(p, m, &err));
  Mesh r;
  CHECK(loadMesh(p, r, &err));
  CHECK(r.loopCount() == m.loopCount());
  CHECK(r.edges.size() == m.edges.size());
  CHECK(r.edgeFlags[3] & EdgeSeam);
  for (size_t i = 0; i < m.positions.size(); ++i) CHECK(glm::length(r.positions[i] - m.positions[i]) < 1e-5f);
  for (size_t l = 0; l < m.loopUV.size(); ++l) CHECK(glm::length(r.loopUV[l] - m.loopUV[l]) < 1e-6f);
  // UV return file
  r.loopUV[0] = {0.25f, 0.75f};
  std::string up = (dir / "cube.ptuv").string();
  CHECK(savePtUV(up, r, &err));
  Mesh back = m;
  CHECK(loadPtUV(up, back, &err));
  CHECK(glm::length(back.loopUV[0] - vec2(0.25f, 0.75f)) < 1e-6f);
}

TEST(core, obj_roundtrip) {
  Mesh m = makeUVSphere(16, 8, 1.f);
  auto dir = paths::cacheDir() / "test";
  std::string p = (dir / "s.obj").string();
  std::string err;
  CHECK(saveObj(p, m, &err));
  Mesh r;
  CHECK(loadMesh(p, r, &err));
  CHECK(r.faceCount() == m.faceCount());
  CHECK(r.islandCount == m.islandCount);
}

TEST(core, png16_and_base64) {
  Image img;
  img.resize(7, 5, 2);
  for (size_t i = 0; i < img.pixels.size(); ++i) img.pixels[i] = float(i) / img.pixels.size();
  auto bytes = encodePng16(img);
  Image back;
  CHECK(decodeImage(bytes.data(), bytes.size(), back, 2));
  CHECK(back.width == 7 && back.height == 5);
  double maxErr = 0;
  for (size_t i = 0; i < img.pixels.size(); ++i) maxErr = std::max(maxErr, (double)std::abs(img.pixels[i] - back.pixels[i]));
  CHECK(maxErr < 1e-4);
  std::string b64 = base64Encode(bytes.data(), bytes.size());
  auto dec = base64Decode(b64);
  CHECK(dec == bytes);
}

TEST(core, container_rejects_garbage) {
  container::File f;
  std::string err;
  CHECK(!container::decode("not a container", "PTMESH01", f, &err));
  CHECK(!err.empty());
}

TEST(core, bvh_raycast_grid) {
  Mesh m = makeUVSphere(64, 32, 1.f);
  Bvh b;
  b.build(m);
  int misses = 0, total = 0;
  for (int y = -9; y <= 9; ++y)
    for (int x = -9; x <= 9; ++x) {
      vec2 p(x * 0.1f, y * 0.1f);
      if (glm::length(p) > 0.95f) continue;
      ++total;
      RayHit h = b.raycast(m, vec3(p, 5), vec3(0, 0, -1));
      if (!h.hit) {
        ++misses;
        if (misses < 5) std::fprintf(stderr, "  miss at %.1f %.1f\n", p.x, p.y);
      }
    }
  CHECK(misses == 0);
  (void)total;
}

TEST(core, bvh_raycast_through_edges) {
  // rays exactly through the x = 0 meridian plane (shared edges) must hit
  Mesh m = makeUVSphere(64, 32, 1.f);
  Bvh b;
  b.build(m);
  int misses = 0;
  for (int i = -40; i <= 40; ++i) {
    vec3 d = glm::normalize(vec3(0, i * 0.008f, -1));
    if (!b.raycast(m, vec3(0, 0, 5.3f), d).hit && std::abs(i * 0.008f) < 0.17f) ++misses;
  }
  CHECK(misses == 0);
}

TEST(core, bvh_closest_point) {
  Mesh m = makeUVSphere(48, 24, 1.f);
  Bvh b;
  b.build(m);
  Rng rng(7);
  for (int i = 0; i < 200; ++i) {
    vec3 p(rng.signedUniform() * 2, rng.signedUniform() * 2, rng.signedUniform() * 2);
    if (glm::length(p) < 0.1f) continue;
    RayHit h = b.closestPoint(m, p);
    CHECK(h.hit);
    // the closest point on a (finely tessellated) unit sphere lies along p
    CHECK(glm::length(glm::normalize(h.position) - glm::normalize(p)) < 0.08f);
    CHECK_NEAR(h.t, std::abs(glm::length(p) - 1.0), 0.03);
  }
}

#include "platform/HttpApi.h"

TEST(core, http_api_address_filter) {
  using patina::net::addressInRanges;
  using patina::net::privateRanges;
  const auto& p = privateRanges();
  for (const char* a : {"127.0.0.1", "10.4.5.6", "172.16.0.9", "172.31.255.1", "192.168.1.20", "169.254.3.3", "::1",
                        "fd12:3456::1", "fe80::1%eth0", "::ffff:192.168.0.7"})
    CHECK(addressInRanges(a, p));
  for (const char* a : {"8.8.8.8", "172.32.0.1", "172.15.9.9", "192.0.2.2", "100.64.0.1", "2001:db8::1", "::ffff:1.2.3.4",
                        "not-an-ip", ""})
    CHECK(!addressInRanges(a, p));
  CHECK(addressInRanges("203.0.113.9", {"203.0.113.0/24"}));
  CHECK(!addressInRanges("203.0.114.9", {"203.0.113.0/24"}));
  CHECK(addressInRanges("10.1.2.3", {"10.1.2.3"}));
}

// ---------------------------------------------------------------- 0.6: FBX, STL, 3MF
#include <fstream>
#include <miniz.h>

static std::string dataPath(const char* f) { return std::string(PATINA_SOURCE_DIR) + "/tests/data/fbx/" + f; }

TEST(core, fbx_import) {
  std::string err;
  Mesh ref, bin, ascii;
  CHECK(loadMesh(dataPath("blender_279_ball_0_obj.obj"), ref, &err));
  CHECK(loadMesh(dataPath("blender_279_ball_7400_binary.fbx"), bin, &err));
  CHECK(loadMesh(dataPath("blender_279_ball_6100_ascii.fbx"), ascii, &err));
  CHECK(bin.faceCount() == ref.faceCount() && ascii.faceCount() == ref.faceCount());
  CHECK(bin.loopCount() == ref.loopCount());
  // same size (Blender meters); the OBJ is Y up like Patina
  vec3 rs = ref.bounds.hi - ref.bounds.lo, bs = bin.bounds.hi - bin.bounds.lo;
  if (glm::length(rs - bs) > 1e-3f)
    std::fprintf(stderr, "  ref %.3f %.3f %.3f  fbx %.3f %.3f %.3f\n", rs.x, rs.y, rs.z, bs.x, bs.y, bs.z);
  CHECK(glm::length(rs - bs) < 1e-3f * glm::length(rs) + 1e-4f);
  CHECK(bin.hasSourceNormals);
  // Maya file in centimeters: a 1 cm cube in meters, with its UVs
  Mesh cube;
  CHECK(loadMesh(dataPath("maya_cube_7500_binary.fbx"), cube, &err));
  CHECK(cube.faceCount() == 6);
  vec3 cs = cube.bounds.hi - cube.bounds.lo;
  CHECK(std::abs(cs.x - 0.01f) < 1e-4f && std::abs(cs.y - 0.01f) < 1e-4f);
  float umax = 0;
  for (auto& t : cube.loopUV) umax = std::max(umax, t.x);
  CHECK(umax > 0.5f);
  CHECK(cube.islandCount >= 1);
  Mesh bad;
  CHECK(!loadMesh(dataPath("maya_cube.obj") + ".missing.fbx", bad, &err));
}

static void writeTetraStl(const std::string& path, bool binary) {
  // a tetrahedron, 10 mm, Z up
  vec3 v[4] = {{0, 0, 0}, {10, 0, 0}, {0, 10, 0}, {0, 0, 10}};
  int t[4][3] = {{0, 2, 1}, {0, 1, 3}, {0, 3, 2}, {1, 2, 3}};
  std::ofstream f(path, std::ios::binary);
  if (binary) {
    char header[80] = "solid but actually binary";
    f.write(header, 80);
    uint32_t n = 4;
    f.write((const char*)&n, 4);
    for (auto& tri : t) {
      float rec[12] = {0, 0, 0};
      for (int k = 0; k < 3; ++k) rec[3 + k * 3] = v[tri[k]].x, rec[4 + k * 3] = v[tri[k]].y, rec[5 + k * 3] = v[tri[k]].z;
      f.write((const char*)rec, sizeof rec);
      uint16_t attr = 0;
      f.write((const char*)&attr, 2);
    }
  } else {
    f << "solid tetra\n";
    for (auto& tri : t) {
      f << " facet normal 0 0 0\n  outer loop\n";
      for (int k = 0; k < 3; ++k) f << "   vertex " << v[tri[k]].x << " " << v[tri[k]].y << " " << v[tri[k]].z << "\n";
      f << "  endloop\n endfacet\n";
    }
    f << "endsolid tetra\n";
  }
}

TEST(core, stl_import) {
  for (bool binary : {true, false}) {
    std::string path = (std::filesystem::temp_directory_path() / (binary ? "patina_t_bin.stl" : "patina_t_ascii.stl")).string();
    writeTetraStl(path, binary);
    Mesh m;
    std::string err;
    CHECK(loadMesh(path, m, &err));
    CHECK(m.faceCount() == 4);
    CHECK(m.positions.size() == 4);  // welded
    CHECK(m.edges.size() == 6);
    CHECK(m.islandCount >= 1);
    // 10 mm along Z (up) becomes 0.01 m along Y
    CHECK(std::abs(m.bounds.hi.y - 0.01f) < 1e-6f && std::abs(m.bounds.lo.y) < 1e-6f);
    // outward winding survives the axis change
    vec3 c(0.f);  // centroid (inside a convex solid)
    for (auto& p : m.positions) c += p / float(m.positions.size());
    int outward = 0;
    for (uint32_t f = 0; f < m.faceCount(); ++f) outward += glm::dot(m.faceNormal(f), m.faceCenter(f) - c) > 0;
    CHECK(outward == 4);
    std::filesystem::remove(path);
  }
}

TEST(core, threemf_import) {
  std::string path = (std::filesystem::temp_directory_path() / "patina_t.3mf").string();
  std::filesystem::remove(path);
  const char* rels =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
      "<Relationship Target=\"/3D/3dmodel.model\" Id=\"rel0\" Type=\"http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel\"/>"
      "</Relationships>";
  // two instances of a 2 cm tetrahedron (unit = cm), the second moved 5 cm along X and mirrored in Y,
  // one face with texture coordinates, materials on two faces
  const char* model =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      "<model unit=\"centimeter\" xml:lang=\"en-US\" xmlns=\"http://schemas.microsoft.com/3dmanufacturing/core/2015/02\" "
      "xmlns:m=\"http://schemas.microsoft.com/3dmanufacturing/material/2015/02\">\n"
      " <resources>\n"
      "  <basematerials id=\"5\"><base name=\"Red\" displaycolor=\"#FF0000\"/><base name=\"Blue\" displaycolor=\"#0000FF\"/></basematerials>\n"
      "  <m:texture2d id=\"7\" path=\"/3D/Textures/t.png\" contenttype=\"image/png\"/>\n"
      "  <m:texture2dgroup id=\"8\" texid=\"7\"><m:tex2coord u=\"0.1\" v=\"0.1\"/><m:tex2coord u=\"0.9\" v=\"0.1\"/>"
      "<m:tex2coord u=\"0.1\" v=\"0.9\"/></m:texture2dgroup>\n"
      "  <object id=\"1\" type=\"model\"><mesh>\n"
      "   <vertices><vertex x=\"0\" y=\"0\" z=\"0\"/><vertex x=\"2\" y=\"0\" z=\"0\"/><vertex x=\"0\" y=\"2\" z=\"0\"/>"
      "<vertex x=\"0\" y=\"0\" z=\"2\"/></vertices>\n"
      "   <triangles><triangle v1=\"0\" v2=\"2\" v3=\"1\" pid=\"8\" p1=\"0\" p2=\"2\" p3=\"1\"/>"
      "<triangle v1=\"0\" v2=\"1\" v3=\"3\" pid=\"5\" p1=\"0\"/><triangle v1=\"0\" v2=\"3\" v3=\"2\" pid=\"5\" p1=\"1\"/>"
      "<triangle v1=\"1\" v2=\"2\" v3=\"3\"/></triangles>\n"
      "  </mesh></object>\n"
      "  <object id=\"2\" type=\"model\"><components><component objectid=\"1\"/></components></object>\n"
      " </resources>\n"
      " <build><item objectid=\"2\"/><item objectid=\"1\" transform=\"1 0 0 0 -1 0 0 0 1 5 0 0\"/></build>\n"
      "</model>\n";
  CHECK(mz_zip_add_mem_to_archive_file_in_place(path.c_str(), "_rels/.rels", rels, std::strlen(rels), nullptr, 0, MZ_BEST_SPEED));
  CHECK(mz_zip_add_mem_to_archive_file_in_place(path.c_str(), "3D/3dmodel.model", model, std::strlen(model), nullptr, 0, MZ_BEST_SPEED));
  Mesh m;
  std::string err;
  CHECK(loadMesh(path, m, &err));
  if (!err.empty()) std::fprintf(stderr, "  %s\n", err.c_str());
  CHECK(m.faceCount() == 8);
  CHECK(m.positions.size() == 8);  // the two copies stay separate pieces
  // centimeters -> meters; 5 cm offset along X
  CHECK(std::abs(m.bounds.hi.x - 0.07f) < 1e-5f && std::abs(m.bounds.lo.x) < 1e-5f);
  CHECK(std::abs(m.bounds.hi.y - 0.02f) < 1e-5f);  // Z up became Y
  // texture coordinates on the first face, material slots for the base materials
  CHECK(std::abs(m.loopUV[0].x - 0.1f) < 1e-5f && std::abs(m.loopUV[1].y - 0.9f) < 1e-5f);
  CHECK(m.faceMaterial[1] != m.faceMaterial[2]);
  // both copies face outwards (the second one is mirrored)
  int outward = 0;
  for (uint32_t f = 0; f < 8; ++f) {
    uint32_t first = f < 4 ? 0 : 4;
    vec3 ctr(0.f);
    for (uint32_t k = first; k < first + 4; ++k) ctr += m.positions[k] * 0.25f;
    outward += glm::dot(m.faceNormal(f), m.faceCenter(f) - ctr) > 0;
  }
  CHECK(outward == 8);
  std::filesystem::remove(path);
}

#include <nlohmann/json.hpp>
#include "mcp/ClaudeSetup.h"

// "Connect to Claude app": the other servers and settings stay, a backup is kept, a second run
// changes nothing, and a file that isn't JSON is left alone
TEST(core, claude_desktop_setup) {
  namespace fs = std::filesystem;
  fs::path dir = fs::temp_directory_path() / "patina_claude_setup_test";
  fs::remove_all(dir);
  fs::create_directories(dir);
  fs::path cfg = dir / "claude_desktop_config.json";
  paths::writeFileAtomic(cfg, R"({"globalShortcut": "Ctrl+Space", "mcpServers": {"blender": {"command": "uvx", "args": ["blender-mcp"]}}})");
  auto r = mcp::setupClaudeDesktop(cfg);
  CHECK(r.ok && r.changed);
  auto j = nlohmann::json::parse(paths::readFile(cfg));
  CHECK(j["globalShortcut"] == "Ctrl+Space");
  CHECK(j["mcpServers"]["blender"]["command"] == "uvx");
  CHECK(j["mcpServers"]["patina"]["args"][0] == "--mcp");
  CHECK(j["mcpServers"]["patina"]["command"] == mcp::mcpCommand().string());
  CHECK(fs::exists(dir / "claude_desktop_config.json.bak"));
  auto r2 = mcp::setupClaudeDesktop(cfg);
  CHECK(r2.ok && !r2.changed);
  // no file yet: created
  fs::remove(cfg);
  CHECK(mcp::setupClaudeDesktop(cfg).changed && fs::exists(cfg));
  // broken JSON: refused, untouched
  paths::writeFileAtomic(cfg, "{ \"mcpServers\": { oops");
  auto r3 = mcp::setupClaudeDesktop(cfg);
  CHECK(!r3.ok);
  CHECK(paths::readFile(cfg) == "{ \"mcpServers\": { oops");
  fs::remove_all(dir);
}

// ---- the built-in library: every generator name is known, ids are unique, smart materials only use library materials
#include <set>
#include "engine/MaterialLibrary.h"

TEST(core, library_files_are_consistent) {
  namespace fs = std::filesystem;
  using nlohmann::json;
  auto gens = generatorNames();
  CHECK(gens.size() == 74);
  for (size_t i = 0; i < gens.size(); ++i) CHECK(generatorFromName(gens[i]) == (int)i);
  std::set<std::string> ids;
  int count = 0;
  fs::path root(PATINA_SOURCE_DIR);
  for (auto& f : fs::directory_iterator(root / "materials")) {
    if (f.path().extension() != ".json") continue;
    json j = json::parse(paths::readFile(f.path()));
    for (auto& m : j["materials"]) {
      ++count;
      std::string id = m["id"];
      if (!ids.insert(id).second) std::fprintf(stderr, "  duplicate material id %s\n", id.c_str());
      CHECK(id.rfind("patina_", 0) == 0);
      if (m.value("type", std::string("procedural")) == "procedural") {
        std::string g = m.value("generator", std::string("flat"));
        if (g != "flat" && generatorFromName(g) == 0) std::fprintf(stderr, "  %s: unknown generator %s\n", id.c_str(), g.c_str());
        CHECK(g == "flat" || generatorFromName(g) > 0);
      }
      MaterialDef d;
      CHECK(MaterialDef::fromJson(m, d, "", nullptr));
      // Cut out holes only means something for a generator with holes
      if (d.type == MaterialType::Procedural && d.proc.cutout && !generatorHasHoles(d.proc.generator))
        std::fprintf(stderr, "  %s: cutout on a generator without holes\n", id.c_str());
      CHECK(!(d.type == MaterialType::Procedural && d.proc.cutout) || generatorHasHoles(d.proc.generator));
    }
  }
  CHECK((int)ids.size() == count);
  CHECK(count >= 270);
  int smart = 0;
  for (auto& f : fs::directory_iterator(root / "smart_materials")) {
    if (f.path().extension() != ".json") continue;
    ++smart;
    json j = json::parse(paths::readFile(f.path()));
    CHECK(j.value("patina_smart_material", 0) == 1);
    for (auto& l : j["layers"]) {
      if (!l.contains("material")) continue;
      std::string id = l["material"].value("id", std::string());
      if (!id.empty() && !ids.count(id))
        std::fprintf(stderr, "  %s uses unknown material %s\n", f.path().filename().string().c_str(), id.c_str());
      CHECK(id.empty() || ids.count(id) == 1);
    }
  }
  CHECK(smart >= 19);
}

// 0.11.1: project files and exports compress PNGs with miniz on several threads
TEST(core, png_samples_roundtrip) {
  Rng rng(7);
  for (int bits : {8, 16})
    for (int ch = 1; ch <= 4; ++ch)
      for (bool bottomUp : {false, true}) {
        int w = 37, h = 23;
        size_t n = size_t(w) * h * ch;
        std::vector<uint8_t> src(n * (bits / 8));
        for (size_t i = 0; i < n; ++i) {  // smooth ramps with noise, like paint
          unsigned v = unsigned((i % (w * ch)) * 7 + (rng.next() & 15));
          if (bits == 16) {
            uint16_t u = uint16_t(v * 211);
            std::memcpy(&src[i * 2], &u, 2);
          } else {
            src[i] = uint8_t(v);
          }
        }
        auto png = encodePngSamples(src.data(), w, h, ch, bits, bottomUp);
        std::vector<uint8_t> back;
        int bw = 0, bh = 0, bb = 0;
        CHECK(decodeSamples(png.data(), png.size(), ch, bottomUp, back, bw, bh, bb));
        CHECK(bw == w && bh == h && bb == bits);
        CHECK(back == src);
        // and stb reads the rows top first: row 0 of the file is the last row given bottom up
        Image img;
        CHECK(decodeImage(png.data(), png.size(), img, ch));
        size_t row0 = bottomUp ? size_t(h - 1) * w * ch : 0;
        float first = bits == 16 ? (src[row0 * 2] | (src[row0 * 2 + 1] << 8)) / 65535.f : src[row0] / 255.f;
        CHECK_NEAR(img.pixels[0], first, 1e-6);
      }
}

// ---- 0.26: opacity, sheen, clearcoat and subsurface channels
#include "engine/Layer.h"

TEST(core, extra_channels_026) {
  CHECK(kChannelCount == 10);
  CHECK(channelFromKey("opacity") == (int)Channel::Opacity && channelFromKey("alpha") == (int)Channel::Opacity);
  CHECK(channelFromKey("clearcoat") == (int)Channel::Coat && channelFromKey("sss") == (int)Channel::Subsurface);
  CHECK(isExtraChannel((int)Channel::Sheen) && !isExtraChannel((int)Channel::Emissive));
  CHECK(kAllChannels == (1 << kChannelCount) - 1);
  // a project from before 0.26: 63 was "every channel"
  Layer old;
  old.fromJson({{"channel_mask", 63}});
  CHECK(old.channelMask == kAllChannels);
  Layer some;
  some.fromJson({{"channel_mask", 5}});
  CHECK(some.channelMask == 5);
  // 0.26 files say so: 63 then means the first six only
  Layer six;
  six.channelMask = 63;
  Layer back;
  back.fromJson(six.toJson());
  CHECK(back.channelMask == 63);
  // the values are kept in a material's file
  MaterialDef d;
  d.id = "x";
  d.proc.generator = generatorFromName("lace");
  d.proc.opacity = 0.8f, d.proc.sheen = 0.3f, d.proc.coat = 0.2f, d.proc.subsurface = 0.1f, d.proc.cutout = true;
  MaterialDef r;
  CHECK(MaterialDef::fromJson(d.toJson(), r, "", nullptr));
  CHECK(std::abs(r.proc.opacity - 0.8f) < 1e-5f && std::abs(r.proc.sheen - 0.3f) < 1e-5f);
  CHECK(std::abs(r.proc.coat - 0.2f) < 1e-5f && std::abs(r.proc.subsurface - 0.1f) < 1e-5f && r.proc.cutout);
  MaterialDef f;
  f.id = "y", f.type = MaterialType::Flat, f.flat.opacity = 0.5f, f.flat.coat = 1.f;
  MaterialDef fr;
  CHECK(MaterialDef::fromJson(f.toJson(), fr, "", nullptr));
  CHECK(std::abs(fr.flat.opacity - 0.5f) < 1e-5f && std::abs(fr.flat.coat - 1.f) < 1e-5f && fr.flat.sheen == 0.f);
  // an older material file: opaque, no sheen, clearcoat or subsurface
  MaterialDef o;
  CHECK(MaterialDef::fromJson({{"id", "z"}, {"generator", "bricks"}}, o, "", nullptr));
  CHECK(o.proc.opacity == 1.f && o.proc.sheen == 0.f && !o.proc.cutout);
}

// ---- 0.27: the Mesh Check and its fixes
#include "core/MeshCheck.h"

namespace {
const MeshProblem* problem(const MeshCheckReport& r, const char* id) {
  for (auto& p : r.problems)
    if (p.id == id) return &p;
  return nullptr;
}
size_t countOf(const MeshCheckReport& r, const char* id) {
  const MeshProblem* p = problem(r, id);
  return p ? p->count : 0;
}
// rebuild after editing the raw arrays
void refresh(Mesh& m) {
  m.edges.clear();
  m.edgeFlags.clear();
  m.finalize();
}
void reverseFace(Mesh& m, uint32_t f) {
  std::reverse(m.loopVert.begin() + m.faceStart[f] + 1, m.loopVert.begin() + m.faceStart[f + 1]);
  std::reverse(m.loopUV.begin() + m.faceStart[f] + 1, m.loopUV.begin() + m.faceStart[f + 1]);
}
void removeFace(Mesh& m, uint32_t f) {
  uint32_t a = m.faceStart[f], b = m.faceStart[f + 1], n = b - a;
  m.loopVert.erase(m.loopVert.begin() + a, m.loopVert.begin() + b);
  m.loopUV.erase(m.loopUV.begin() + a, m.loopUV.begin() + b);
  m.faceStart.erase(m.faceStart.begin() + f + 1);
  for (size_t i = f + 1; i < m.faceStart.size(); ++i) m.faceStart[i] -= n;
  m.faceMaterial.erase(m.faceMaterial.begin() + f);
  m.faceFlags.erase(m.faceFlags.begin() + f);
  m.loopNormal.clear();
  m.hasSourceNormals = false;
}
void addFace(Mesh& m, std::vector<uint32_t> vs, uint32_t slot = 0) {
  for (uint32_t v : vs) m.loopVert.push_back(v), m.loopUV.push_back(vec2(0.1f * float(v % 7), 0.1f * float(v % 5)));
  m.faceStart.push_back((uint32_t)m.loopVert.size());
  m.faceMaterial.push_back(slot);
  m.faceFlags.push_back(0);
  m.loopNormal.clear();
  m.hasSourceNormals = false;
}
}  // namespace

TEST(core, mesh_check_027) {
  // a clean cube: closed, one part, nothing to mend
  Mesh cube = makeCube(2.f);
  MeshCheckReport r = checkMesh(cube);
  CHECK(r.stats.closed && r.stats.parts == 1 && r.stats.faces == 6 && r.stats.quads == 6);
  CHECK(std::abs(r.stats.volume - 8.0) < 1e-3 && std::abs(r.stats.area - 24.0) < 1e-3);
  for (auto& p : r.problems)
    if (p.severity >= 1) std::fprintf(stderr, "  clean cube: %s %zu\n", p.id.c_str(), p.count);
  CHECK(!problem(r, "holes") && !problem(r, "flipped_faces") && !problem(r, "inside_out") && !problem(r, "open_seams"));
  // a hole, then closed again (the cap takes the slot of the face beside it)
  Mesh open = cube;
  removeFace(open, 2);
  refresh(open);
  r = checkMesh(open);
  CHECK(countOf(r, "holes") == 1 && !r.stats.closed);
  MeshFixOptions fh;
  fh.fillHoles = true;
  MeshFixResult fr;
  Mesh closed = fixMesh(open, fh, &fr);
  CHECK(fr.holesFilled == 1 && closed.faceCount() == 6);
  r = checkMesh(closed);
  CHECK(r.stats.closed && !problem(r, "holes") && !problem(r, "flipped_faces") && !problem(r, "inside_out"));
  CHECK(std::abs(r.stats.volume - 8.0) < 1e-3);
  // one face turned the wrong way, and the whole cube inside out
  Mesh flip = cube;
  reverseFace(flip, 4);
  refresh(flip);
  r = checkMesh(flip);
  CHECK(countOf(r, "flipped_faces") == 1);
  MeshFixOptions fw;
  fw.fixWinding = true;
  Mesh unflip = fixMesh(flip, fw, &fr);
  CHECK(fr.flipped == 1 && !problem(checkMesh(unflip), "flipped_faces"));
  Mesh inside = cube;
  for (uint32_t f = 0; f < 6; ++f) reverseFace(inside, f);
  refresh(inside);
  r = checkMesh(inside);
  CHECK(countOf(r, "inside_out") == 1 && !problem(r, "flipped_faces"));
  Mesh outside = fixMesh(inside, fw, &fr);
  CHECK(fr.flipped == 6 && !problem(checkMesh(outside), "inside_out"));
  // UVs travel with their corners when a face turns
  {
    std::multiset<std::pair<float, float>> a, b;
    for (uint32_t l = inside.faceStart[0]; l < inside.faceStart[1]; ++l) a.insert({inside.loopUV[l].x, inside.loopUV[l].y});
    for (uint32_t l = outside.faceStart[0]; l < outside.faceStart[1]; ++l) b.insert({outside.loopUV[l].x, outside.loopUV[l].y});
    CHECK(a == b);
    CHECK(outside.loopVert[outside.faceStart[0]] == cube.loopVert[cube.faceStart[0]]);
  }
  // a duplicate face, a degenerate one and a loose vertex
  Mesh junk = cube;
  addFace(junk, {junk.loopVert[0], junk.loopVert[1], junk.loopVert[2], junk.loopVert[3]});
  addFace(junk, {0, 1, 1});
  junk.positions.push_back(vec3(5, 5, 5));
  refresh(junk);
  r = checkMesh(junk);
  CHECK(countOf(r, "duplicate_faces") == 1 && countOf(r, "degenerate_faces") >= 1 && countOf(r, "loose_vertices") == 1);
  MeshFixOptions fj;
  fj.removeDuplicateFaces = fj.removeDegenerate = fj.removeLoose = true;
  Mesh clean = fixMesh(junk, fj, &fr);
  CHECK(fr.duplicates == 1 && fr.degenerate == 1 && fr.loose == 1);
  CHECK(clean.faceCount() == 6 && clean.positions.size() == 8);
  // open seams: every face with vertices of its own
  Mesh split;
  for (uint32_t f = 0; f < 6; ++f) {
    for (uint32_t l = cube.faceStart[f]; l < cube.faceStart[f + 1]; ++l) {
      split.loopVert.push_back((uint32_t)split.positions.size());
      split.positions.push_back(cube.positions[cube.loopVert[l]]);
      split.loopUV.push_back(cube.loopUV[l]);
    }
    split.faceStart.push_back(f == 0 ? 0 : split.faceStart.back());
  }
  split.faceStart.clear();
  for (uint32_t f = 0; f <= 6; ++f) split.faceStart.push_back(f * 4);
  split.faceMaterial.assign(6, 0);
  split.faceMaterial[3] = 2;
  split.finalize();
  r = checkMesh(split);
  CHECK(countOf(r, "open_seams") == 24 && countOf(r, "holes") == 6 && r.stats.parts == 6);
  MeshFixOptions fs2;
  fs2.weld = true;
  Mesh welded = fixMesh(split, fs2, &fr);
  CHECK(fr.welded == 16 && welded.positions.size() == 8);
  r = checkMesh(welded);
  CHECK(r.stats.closed && r.stats.parts == 1 && !problem(r, "open_seams"));
  CHECK(welded.faceMaterial[3] == 2);                                     // slots stay with their faces
  CHECK(welded.loopUV[13] == split.loopUV[13] && welded.loopUV[2] == split.loopUV[2]);  // and the UVs
  // specks: a tiny triangle far from a sphere
  Mesh ball = makeUVSphere(24, 12, 1.f);
  uint32_t b0 = (uint32_t)ball.positions.size();
  ball.positions.push_back(vec3(3, 0, 0)), ball.positions.push_back(vec3(3.01f, 0, 0)), ball.positions.push_back(vec3(3, 0.01f, 0));
  addFace(ball, {b0, b0 + 1, b0 + 2});
  refresh(ball);
  r = checkMesh(ball);
  CHECK(countOf(r, "small_parts") == 1 && r.stats.parts == 2);
  MeshFixOptions fsp;
  fsp.removeSmallParts = true;
  Mesh lone = fixMesh(ball, fsp, &fr);
  CHECK(fr.smallParts == 1 && checkMesh(lone).stats.parts == 1);
  // three faces on one edge; two triangles meeting at one vertex
  Mesh fin;
  fin.positions = {vec3(0, 0, 0), vec3(0, 1, 0), vec3(1, 0, 0), vec3(-1, 0, 0), vec3(0, 0, 1)};
  fin.faceStart = {0};
  addFace(fin, {0, 1, 2});
  addFace(fin, {1, 0, 3});
  addFace(fin, {0, 1, 4});
  fin.finalize();
  CHECK(countOf(checkMesh(fin), "non_manifold_edges") == 1);
  Mesh bow;
  bow.positions = {vec3(0, 0, 0), vec3(1, 0, 0), vec3(1, 1, 0), vec3(-1, 0, 0), vec3(-1, -1, 0)};
  bow.faceStart = {0};
  addFace(bow, {0, 1, 2});
  addFace(bow, {0, 3, 4});
  bow.finalize();
  CHECK(countOf(checkMesh(bow), "non_manifold_vertices") == 1);
  // odd scale
  Mesh tiny = makeCube(0.001f);
  CHECK(problem(checkMesh(tiny), "scale") != nullptr);
  // nothing to mend: the same mesh, nothing reported changed
  MeshFixOptions every;
  every.weld = every.removeDegenerate = every.removeDuplicateFaces = every.removeLoose = every.removeSmallParts =
      every.fixWinding = every.fillHoles = true;
  Mesh same = fixMesh(cube, every, &fr);
  CHECK(!fr.changed() && same.faceCount() == 6 && same.positions.size() == 8);
}

// ---- 0.29: fewer polygons, keeping the UV layout
#include "core/MeshReduce.h"

TEST(core, mesh_reduce_029) {
  Mesh ball = makeUVSphere(64, 32, 1.f);
  for (uint32_t f = 0; f < ball.faceCount(); ++f) ball.faceMaterial[f] = ball.faceCenter(f).y > 0.f ? 1 : 0;
  ball.edges.clear(), ball.edgeFlags.clear();
  ball.finalize();
  ReduceOptions o;
  o.ratio = 0.25f;
  ReduceResult r;
  Mesh q = reduceMesh(ball, o, &r);
  CHECK(r.trianglesBefore == ball.tris.size());
  CHECK(r.trianglesAfter > r.trianglesBefore / 6 && r.trianglesAfter < r.trianglesBefore / 3);
  CHECK(q.faceCount() == r.trianglesAfter && q.positions.size() < ball.positions.size());
  // every corner keeps a UV and slot it had before, at a vertex that was there (the textures fit as they are)
  std::set<std::tuple<float, float, float, float, float, uint32_t>> before;
  for (uint32_t f = 0; f < ball.faceCount(); ++f)
    for (uint32_t l = ball.faceStart[f]; l < ball.faceStart[f + 1]; ++l) {
      vec3 p = ball.positions[ball.loopVert[l]];
      before.insert({p.x, p.y, p.z, ball.loopUV[l].x, ball.loopUV[l].y, ball.faceMaterial[f]});
    }
  size_t unknown = 0;
  for (uint32_t f = 0; f < q.faceCount(); ++f)
    for (uint32_t l = q.faceStart[f]; l < q.faceStart[f + 1]; ++l) {
      vec3 p = q.positions[q.loopVert[l]];
      unknown += !before.count({p.x, p.y, p.z, q.loopUV[l].x, q.loopUV[l].y, q.faceMaterial[f]});
    }
  CHECK(unknown == 0);
  // still closed, still round
  MeshCheckReport c = checkMesh(q);
  CHECK(!problem(c, "holes") && !problem(c, "flipped_faces"));
  float rmin = 9, rmax = 0;
  for (auto& p : q.positions) rmin = std::min(rmin, glm::length(p)), rmax = std::max(rmax, glm::length(p));
  CHECK(rmin > 0.999f && rmax < 1.001f && r.error < 0.05f);
  // a plane keeps its outline with keep_borders
  Mesh plane = makePlane(2.f, 16);
  auto borderVerts = [](const Mesh& m) {
    MeshCheckReport rr = checkMesh(m);
    return rr.stats.vertices;
  };
  (void)borderVerts;
  ReduceOptions po;
  po.ratio = 0.2f;
  Mesh pq = reduceMesh(plane, po);
  AABB pb;
  for (auto& p : pq.positions) pb.add(p);
  CHECK(std::abs(pb.hi.x - 1.f) < 1e-5f && std::abs(pb.lo.x + 1.f) < 1e-5f && std::abs(pb.hi.z - 1.f) < 1e-5f);
  size_t corners = 0;
  for (auto& p : pq.positions) corners += std::abs(std::abs(p.x) - 1.f) < 1e-5f && std::abs(std::abs(p.z) - 1.f) < 1e-5f;
  CHECK(corners == 4);
  // a target count
  ReduceOptions to;
  to.targetTriangles = 500;
  Mesh tq = reduceMesh(ball, to, &r);
  CHECK(r.trianglesAfter <= 520 && r.trianglesAfter > 300);
}

#include "core/Collision.h"
TEST(core, collision_034) {
  // a cube's corners and points inside: the hull is the cube
  std::vector<vec3> pts;
  for (int i = 0; i < 8; ++i) pts.push_back(vec3(i & 1, (i >> 1) & 1, (i >> 2) & 1));
  Rng rng(7);
  for (int i = 0; i < 200; ++i) pts.push_back(vec3(rng.uniform(), rng.uniform(), rng.uniform()) * 0.98f + 0.01f);
  ConvexShape h;
  CHECK(convexHull(pts, h, 64));
  CHECK(h.verts.size() == 8);
  CHECK_NEAR(h.volume(), 1.0, 1e-4);
  // every point is inside every face (outward faces)
  auto inside = [](const ConvexShape& s, const std::vector<vec3>& ps, float eps) {
    for (auto& t : s.tris) {
      vec3 a = s.verts[t.x], n = glm::normalize(glm::cross(s.verts[t.y] - a, s.verts[t.z] - a));
      for (auto& p : ps)
        if (glm::dot(p - a, n) > eps) return false;
    }
    return true;
  };
  CHECK(inside(h, pts, 1e-4f));
  // a sphere's points with a corner limit: at most that many corners, and nearly the sphere's volume
  std::vector<vec3> sph;
  for (int i = 0; i < 4000; ++i) {
    vec3 d(rng.signedUniform(), rng.signedUniform(), rng.signedUniform());
    if (glm::length(d) > 0.1f) sph.push_back(glm::normalize(d));
  }
  ConvexShape hs;
  CHECK(convexHull(sph, hs, 32));
  CHECK(hs.verts.size() <= 32 && hs.verts.size() >= 20);
  CHECK(hs.volume() > 0.7f * 4.18879f && hs.volume() < 4.18879f);
  // flat points still give a closed (thin) shape
  std::vector<vec3> flat = {{0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1}, {0.5f, 0, 0.5f}};
  ConvexShape hf;
  CHECK(convexHull(flat, hf, 32));
  CHECK(hf.tris.size() >= 4 && hf.volume() > 0.f && hf.volume() < 0.01f);
  // a turned box: the oriented box finds it again
  glm::mat3 r = glm::mat3(glm::rotate(glm::mat4(1.f), 0.6f, glm::normalize(vec3(1, 2, 3))));
  std::vector<vec3> tb;
  for (int i = 0; i < 8; ++i) tb.push_back(r * (vec3(i & 1, (i >> 1) & 1, (i >> 2) & 1) * vec3(3, 1, 0.5f)));
  ConvexShape ob = orientedBox(tb);
  CHECK(ob.box && ob.verts.size() == 8 && ob.tris.size() == 12);
  CHECK_NEAR(ob.volume(), 1.5, 0.05);
  CHECK(inside(ob, tb, 1e-3f));
  // an L: two convex pieces that fit it closely; one hull holds a quarter more
  Mesh L;
  auto addBox = [&](vec3 lo, vec3 hi) {
    uint32_t b = (uint32_t)L.positions.size();
    for (int i = 0; i < 8; ++i) L.positions.push_back(vec3(i & 1 ? hi.x : lo.x, i & 2 ? hi.y : lo.y, i & 4 ? hi.z : lo.z));
    const uint32_t q[6][4] = {{0, 2, 3, 1}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 4, 6, 2}, {1, 3, 7, 5}};
    for (auto& f : q) {
      for (int k = 0; k < 4; ++k) L.loopVert.push_back(b + f[k]), L.loopUV.push_back(vec2(0.f));
      L.faceStart.push_back((uint32_t)L.loopVert.size());
      L.faceMaterial.push_back(0);
    }
  };
  L.faceStart = {0};
  addBox(vec3(0, 0, 0), vec3(3, 1, 1));
  addBox(vec3(0, 1, 0), vec3(1, 3, 1));
  L.finalize();
  CollisionOptions co;
  co.mode = "pieces";
  CollisionResult cr = makeCollision(L, co);
  CHECK(cr.shapes.size() == 2);
  CHECK_NEAR(cr.meshVolume, 5.0, 1e-3);
  CHECK(cr.shapesVolume > 4.9f && cr.shapesVolume < 5.5f);
  co.mode = "hull";
  CollisionResult one = makeCollision(L, co);
  CHECK(one.shapes.size() == 1 && one.shapesVolume > 6.f);
  co.mode = "parts";  // the two boxes touch at shared corners: one part
  CHECK(makeCollision(L, co).shapes.size() == 1);
  Mesh two;
  std::swap(two, L);
  L = Mesh();
  L.faceStart = {0};
  addBox(vec3(0, 0, 0), vec3(1, 1, 1));
  addBox(vec3(3, 0, 0), vec3(4, 1, 1));
  L.finalize();
  CollisionResult apart = makeCollision(L, co);
  CHECK(apart.shapes.size() == 2);  // apart: a hull each
  CHECK_NEAR(apart.shapesVolume, 2.0, 0.01);
  std::swap(two, L);
  co.mode = "box";
  CollisionResult bx = makeCollision(L, co);
  CHECK(bx.shapes.size() == 1 && bx.shapes[0].box);
  CHECK_NEAR(bx.shapesVolume, 9.0, 0.01);
  // edges for drawing: a box shows its 12 edges (no diagonals across its sides)
  CHECK(collisionEdges(bx.shapes).size() == 24);
}

#include "core/Lightmap.h"
#include "core/UVOps.h"
TEST(core, lightmap_uvs_035) {
  // a cube whose six faces all use the whole UV square (stacked): the lightmap gives each its own space
  Mesh m = makeCube(2.f);
  for (uint32_t f = 0; f < m.faceCount(); ++f) {
    const vec2 q[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    for (uint32_t k = 0; k < 4; ++k) m.loopUV[m.faceStart[f] + k] = q[k];
  }
  m.finalize();
  CHECK(uv::overlapCount(m, 256) > 0);
  LightmapOptions o;
  o.size = 256;
  o.padding = 4;
  std::vector<vec2> uv2;
  LightmapStats st;
  CHECK(makeLightmapUVs(m, o, uv2, &st));
  CHECK(uv2.size() == m.loopCount());
  CHECK(st.islands == 6 && st.overlaps == 0);
  Mesh lm = m;
  lm.loopUV = uv2;
  lm.computeUVTopology();
  CHECK(uv::overlapCount(lm, 512) == 0);
  // inside the square, the padding from its border, and every face the same size (the same detail)
  float lo = 1, hi = 0;
  for (auto& u : uv2) lo = std::min({lo, u.x, u.y}), hi = std::max({hi, u.x, u.y});
  CHECK(lo >= 4.f / 256.f * 0.5f && hi <= 1.f - 4.f / 256.f * 0.5f);
  float amin = 1e9f, amax = 0;
  for (uint32_t f = 0; f < lm.faceCount(); ++f) {
    float a = std::abs(lm.faceAreaUV(f));
    amin = std::min(amin, a), amax = std::max(amax, a);
  }
  CHECK(amin > 0 && amax / amin < 1.05f);
  // the gap between islands is at least the padding: island boxes do not come closer than ~4 texels
  std::vector<Rect2> boxes;
  for (uint32_t f = 0; f < lm.faceCount(); ++f) {
    Rect2 r;
    for (uint32_t l = lm.faceStart[f]; l < lm.faceStart[f + 1]; ++l) r.add(uv2[l]);
    boxes.push_back(r);
  }
  float gap = 1;
  for (size_t i = 0; i < boxes.size(); ++i)
    for (size_t j = i + 1; j < boxes.size(); ++j) {
      float dx = std::max({0.f, boxes[j].lo.x - boxes[i].hi.x, boxes[i].lo.x - boxes[j].hi.x});
      float dy = std::max({0.f, boxes[j].lo.y - boxes[i].hi.y, boxes[i].lo.y - boxes[j].hi.y});
      gap = std::min(gap, std::max(dx, dy));
    }
  CHECK(gap >= 3.f / 256.f);
  // the model's own UVs are not touched
  CHECK(m.loopUV[0] == vec2(0, 0) && m.loopUV[2] == vec2(1, 1));
  // new charts by xatlas work too
  o.source = "auto";
  CHECK(makeLightmapUVs(m, o, uv2, &st) && st.overlaps == 0 && st.islands >= 1 && st.fill > 0.3f);
}

#include "core/VectorIcon.h"

// (0.36) every toolbar icon reads cleanly, stays on its 24x24 grid, and the shapes come out right
TEST(core, toolbar_icons) {
  const auto& all = vicon::embedded();
  CHECK(all.size() >= 50);
  for (auto& e : all) {
    const vicon::Icon* ic = vicon::find(e.name);
    CHECK(ic && !ic->shapes.empty());
    if (!ic) continue;
    for (auto& p : ic->problems) std::fprintf(stderr, "  icon %s: %s\n", e.name, p.c_str());
    CHECK(ic->problems.empty());
    for (auto& sh : ic->shapes) {
      for (auto& piece : sh.pieces)
        for (vec2 v : piece) CHECK(v.x > -0.5f && v.x < 24.5f && v.y > -0.5f && v.y < 24.5f);
      for (auto& f : sh.fills)
        for (vec2 v : f) CHECK(v.x > -0.5f && v.x < 24.5f && v.y > -0.5f && v.y < 24.5f);
      for (size_t f = 0; f < sh.fills.size(); ++f)
        if (!sh.fillConvex[f]) CHECK(sh.fillTris[f].size() == (sh.fills[f].size() - 2) * 3);
    }
  }
  // a circle is one smooth closed loop; a square has four corners; a dashed line breaks into dashes
  vicon::Icon c = vicon::parse(R"svg(<svg viewBox="0 0 24 24" fill="none" stroke="currentColor"><circle cx="12" cy="12" r="8"/></svg>)svg");
  CHECK(c.problems.empty() && c.shapes.size() == 1 && c.shapes[0].pieces.size() == 1 && c.shapes[0].pieceClosed[0] == 1);
  vicon::Icon sq = vicon::parse(R"svg(<svg viewBox="0 0 24 24" fill="none" stroke="#E8852C"><path d="M4 4 H20 V20 H4 Z"/></svg>)svg");
  CHECK(sq.shapes.size() == 1 && sq.shapes[0].pieces.size() == 4 && sq.shapes[0].joints.size() == 4 &&
        sq.shapes[0].stroke == vicon::Paint::Accent);
  vicon::Icon d = vicon::parse(R"svg(<svg viewBox="0 0 24 24" fill="none" stroke="currentColor"><path d="M2 12 H22" stroke-dasharray="2 3"/></svg>)svg");
  CHECK(d.shapes.size() == 1 && d.shapes[0].pieces.size() == 4);
  // an arc and a transform: the half circle's far point lands where it should
  vicon::Icon a = vicon::parse(R"svg(<svg viewBox="0 0 24 24" fill="none" stroke="currentColor"><g transform="translate(2 0)"><path d="M4 12 A8 8 0 0 1 20 12"/></g></svg>)svg");
  float top = 24;
  for (auto& p : a.shapes[0].pieces)
    for (vec2 v : p) top = std::min(top, v.y);
  CHECK_NEAR(top, 4.f, 0.05f);
  CHECK_NEAR(a.shapes[0].pieces[0].back().x, 22.f, 1e-3f);
  // a concave fill is cut into triangles
  vicon::Icon l = vicon::parse(R"svg(<svg viewBox="0 0 24 24" fill="#E8852C"><path d="M2 2 H22 V8 H8 V22 H2 Z"/></svg>)svg");
  CHECK(l.shapes.size() == 1 && l.shapes[0].fills.size() == 1 && !l.shapes[0].fillConvex[0] && l.shapes[0].fillTris[0].size() == 12);
  // unknown things are reported, not guessed
  vicon::Icon u = vicon::parse(R"svg(<svg viewBox="0 0 24 24"><text x="1" y="1">A</text><path d="M1 1 L2 2" style="x"/></svg>)svg");
  CHECK(u.problems.size() == 2);
}

#include "core/I18n.h"

// (0.36) translations: .po files, plural forms, % codes, labels that keep their IDs, the test language
TEST(core, translations) {
  using namespace patina::i18n;
  // plural forms: English, French, Russian, Japanese
  CHECK(pluralIndex("nplurals=2; plural=(n != 1);", 1) == 0 && pluralIndex("nplurals=2; plural=(n != 1);", 0) == 1);
  CHECK(pluralIndex("nplurals=2; plural=(n > 1);", 0) == 0 && pluralIndex("nplurals=2; plural=(n > 1);", 2) == 1);
  const std::string ru = "nplurals=3; plural=(n%10==1 && n%100!=11 ? 0 : n%10>=2 && n%10<=4 && (n%100<10 || n%100>=20) ? 1 : 2);";
  CHECK(pluralIndex(ru, 1) == 0 && pluralIndex(ru, 3) == 1 && pluralIndex(ru, 5) == 2 && pluralIndex(ru, 11) == 2 &&
        pluralIndex(ru, 21) == 0 && pluralIndex(ru, 22) == 1 && pluralIndex(ru, 112) == 2);
  CHECK(pluralIndex("nplurals=1; plural=0;", 7) == 0);
  CHECK(pluralIndex("nplurals=2; plural=(n != ;", 2) == -1);
  // % codes: the same ones in the same order (or fewer at the end); never positional
  CHECK(formatsMatch("%d of %d texts", "%d sur %d textes"));
  CHECK(formatsMatch("%zu faces", "%zu Flächen") && !formatsMatch("%zu faces", "%d Flächen"));
  CHECK(formatsMatch("%s: %.1f s", "%s : %.2f s") && !formatsMatch("%s: %.1f s", "%.1f s : %s"));
  CHECK(formatsMatch("Size %.0f", "Taille") && !formatsMatch("Size", "Taille %d"));
  CHECK(!formatsMatch("%s and %s", "%2$s et %1$s") && formatsMatch("100%% done", "100 %% fait"));
  // a .po file: header, context, plurals, fuzzy entries, multi-line text, escapes
  const char* po = "# comment\n#, fuzzy\nmsgid \"\"\nmsgstr \"\"\n\"Language: fr\\n\"\n\"X-Patina-Language-Name: Fran\xc3\xa7" "ais\\n\"\n"
                   "\"Plural-Forms: nplurals=2; plural=(n > 1);\\n\"\n\"Content-Type: text/plain; charset=UTF-8\\n\"\n\n"
                   "#: src/ui/Toolbars.cpp:1\nmsgid \"Paint\"\nmsgstr \"Peindre\"\n\n"
                   "msgctxt \"layer\"\nmsgid \"Fill\"\nmsgstr \"Remplissage\"\n\n"
                   "msgid \"Fill\"\nmsgstr \"Remplir\"\n\n"
                   "#, c-format\nmsgid \"%d face\"\nmsgid_plural \"%d faces\"\nmsgstr[0] \"%d face\"\nmsgstr[1] \"%d faces (fr)\"\n\n"
                   "#, fuzzy\nmsgid \"Erase\"\nmsgstr \"Gommer\"\n\n"
                   "msgid \"\"\n\"Two\\n\"\n\"lines\"\nmsgstr \"\"\n\"Deux\\n\"\n\"lignes \\\"ok\\\"\"\n\n"
                   "#, c-format\nmsgid \"%s faces\"\nmsgstr \"%d faces\"\n\n"
                   "#~ msgid \"Old\"\n#~ msgstr \"Vieux\"\n";
  PoFile f;
  std::string err;
  CHECK(parsePo(po, f, &err));
  CHECK(f.language == "fr" && f.languageName == "Fran\xc3\xa7" "ais" && f.pluralForms.find("n > 1") != std::string::npos);
  CHECK(f.entries.size() == 7);
  if (f.entries.size() == 7) {
    CHECK(f.entries[1].context == "layer" && f.entries[1].str[0] == "Remplissage");
    CHECK(f.entries[3].idPlural == "%d faces" && f.entries[3].str.size() == 2);
    CHECK(f.entries[4].fuzzy && !f.entries[3].fuzzy);
    CHECK(f.entries[5].id == "Two\nlines" && f.entries[5].str[0] == "Deux\nlignes \"ok\"");
  }
  PoFile bad;
  CHECK(!parsePo("msgid \"x\"\nmsgstr \"y\"\nnonsense\n", bad, &err) && err.find("line 3") != std::string::npos);
  // load it as a user's translation (in a folder of the test's own, never the real one)
  std::string tmp = (std::filesystem::temp_directory_path() / "patina_test_i18n").string();
  std::filesystem::create_directories(tmp + "/patina/user/languages");
  {
    std::ofstream o(tmp + "/patina/user/languages/fr.po", std::ios::binary);
    o << po;
  }
  const char* oldXdg = std::getenv("XDG_DATA_HOME");
  std::string keep = oldXdg ? oldXdg : "";
  setenv("XDG_DATA_HOME", tmp.c_str(), 1);
  bool found = false;
  for (auto& l : available()) found |= l.code == "fr" && l.user && l.name == "Fran\xc3\xa7" "ais";
  CHECK(found);
  CHECK(setLanguage("fr", &err));
  CHECK(std::string(tr("Paint")) == "Peindre" && std::string(tr("Erase")) == "Erase");  // (fuzzy: not used)
  CHECK(std::string(trc("layer", "Fill")) == "Remplissage" && std::string(tr("Fill")) == "Remplir");
  CHECK(std::string(trn("%d face", "%d faces", 1)) == "%d face" && std::string(trn("%d face", "%d faces", 5)) == "%d faces (fr)");
  CHECK(std::string(tr("%s faces")) == "%s faces");  // its % codes differ: rejected
  CHECK(stats().rejected.size() == 1 && stats().fuzzy == 1 && stats().translated == 5);
  // a label keeps the English ID; text after ## stays
  CHECK(std::string(label("Paint")) == "Peindre###Paint");
  CHECK(std::string(label("Paint##tool")) == "Peindre###Paint##tool");
  CHECK(std::string(label("Paint###p")) == "Peindre###p");
  CHECK(std::string(label("Nothing here")) == "Nothing here" && std::string(label("##id")) == "##id");
  const char* lst = list("Paint\0Erase\0Fill\0");
  CHECK(std::string(lst) == "Peindre" && std::string(lst + 8) == "Erase" && std::string(lst + 14) == "Remplir" && lst[22] == 0);
  CHECK(std::string(format("%d of %d", 2, 3)) == "2 of 3");
  // the untranslated text shown is remembered for the translator
  auto seen = untranslatedSeen();
  CHECK(std::find(seen.begin(), seen.end(), "Nothing here") != seen.end() && std::find(seen.begin(), seen.end(), "Paint") == seen.end());
  // a new translation from the template is never written over an existing file
  std::string path;
  CHECK(makeTranslation("fr", "Français", &path, &err) && path.find("fr.po") != std::string::npos);
  {
    bool ok = false;
    CHECK(paths::readFile(path, &ok).find("Peindre") != std::string::npos);
  }
  CHECK(!makeTranslation("e", "", &path, &err));
  // the test language: accents, brackets, longer; % codes and ## IDs untouched
  CHECK(setLanguage("x-test", &err));
  std::string p = tr("Open %d files");
  CHECK(p.front() == '[' && p.find("%d") != std::string::npos && p.find("Öpéñ") != std::string::npos && p.size() > 16);
  CHECK(std::string(label("Open##o")).find("###Open##o") != std::string::npos);
  CHECK(setLanguage("en", &err) && std::string(tr("Paint")) == "Paint" && std::string(label("Paint")) == "Paint");
  if (keep.empty()) unsetenv("XDG_DATA_HOME");
  else setenv("XDG_DATA_HOME", keep.c_str(), 1);
  std::filesystem::remove_all(tmp);
}
