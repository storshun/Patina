// Patina Assistant (0.11), the parts that need no GPU: named parts, the head / body regions,
// vertex groups in the .ptmesh format.
#include <algorithm>

#include "Test.h"
#include "TestMeshes.h"
#include "core/Container.h"
#include "core/MeshIO.h"
#include "core/MeshParts.h"

using namespace patina;

namespace {

const parts::Part* findPart(const parts::PartsReport& r, const std::string& id) {
  for (auto& p : r.parts)
    if (p.id == id) return &p;
  return nullptr;
}

// share of a part's faces whose center lies above `y`
float shareAbove(const Mesh& m, const std::vector<uint32_t>& faces, float y) {
  size_t n = 0;
  for (uint32_t f : faces) n += m.faceCenter(f).y > y ? 1 : 0;
  return faces.empty() ? 0.f : float(n) / faces.size();
}

}  // namespace

TEST(assistant, bust_has_head_and_body) {
  Mesh m = testmesh::makeBust();
  auto r = parts::findParts(m);
  CHECK(r.neck.found);
  CHECK(r.neck.y > 0.55f && r.neck.y < 0.75f);
  const parts::Part* head = findPart(r, "region:head");
  const parts::Part* body = findPart(r, "region:body");
  CHECK(head && body);
  if (!head || !body) return;
  // the head is the sphere: slot 0, every one of its faces
  size_t skin = 0;
  for (uint32_t f = 0; f < m.faceCount(); ++f) skin += m.faceMaterial[f] == 0;
  CHECK(head->faces.size() == skin);
  for (uint32_t f : head->faces) CHECK(m.faceMaterial[f] == 0);
  CHECK(head->faces.size() + body->faces.size() == m.faceCount());
  CHECK(findPart(r, "slot:Skin") && findPart(r, "slot:Cloth"));
  CHECK(!findPart(r, "piece:1"));  // the two pieces are exactly the two slots: not listed again
}

TEST(assistant, test_meshes_face_outward) {
  for (Mesh m : {testmesh::makeBust(), testmesh::makeSphereOnCylinder(), testmesh::makeLatheFigure()}) {
    size_t out = 0, total = 0;
    for (uint32_t f = 0; f < m.faceCount(); ++f) {
      vec3 c = m.faceCenter(f), n = m.faceNormal(f);
      if (glm::length(n) < 0.5f || std::abs(n.y) > 0.9f) continue;  // caps and poles say little
      out += glm::dot(n, vec3(c.x, 0, c.z)) > 0;
      ++total;
    }
    CHECK(out == total);
  }
}

TEST(assistant, sphere_on_cylinder_head) {
  Mesh m = testmesh::makeSphereOnCylinder();
  auto r = parts::findParts(m);
  CHECK(r.neck.found);
  const parts::Part* head = findPart(r, "region:head");
  CHECK(head != nullptr);
  if (!head) return;
  CHECK(shareAbove(m, head->faces, 1.6f) > 0.85f);
  // the cylinder (its side and top cap) is all body
  for (uint32_t f : head->faces) CHECK(glm::length(vec2(m.faceCenter(f).x, m.faceCenter(f).z)) < 0.33f);
}

TEST(assistant, one_piece_figure_head_is_cut_at_the_neck) {
  Mesh m = testmesh::makeLatheFigure();
  auto r = parts::findParts(m);
  CHECK(r.pieceCount == 1);
  CHECK(r.neck.found);
  CHECK(r.neck.y > 1.55f && r.neck.y < 1.7f);
  const parts::Part* head = findPart(r, "region:head");
  CHECK(head != nullptr);
  if (!head) return;
  CHECK(shareAbove(m, head->faces, 1.58f) == 1.f);
  size_t topFaces = 0;
  for (uint32_t f = 0; f < m.faceCount(); ++f) topFaces += m.faceCenter(f).y > 1.7f;
  CHECK(head->faces.size() >= topFaces);
}

TEST(assistant, no_neck_no_head) {
  Mesh cube = makeCube(1.f);
  auto r = parts::findParts(cube);
  CHECK(!r.neck.found);
  CHECK(!findPart(r, "region:head"));
  CHECK(!r.neck.why.empty());
  Mesh sphere = makeUVSphere(32, 16, 1.f);
  CHECK(!parts::findParts(sphere).neck.found);
  Mesh torus = makeTorus();
  CHECK(!parts::findParts(torus).neck.found);
}

TEST(assistant, named_parts_win_over_regions) {
  Mesh m = testmesh::makeBust();
  m.materialNames = {"M_Head", "Body.001"};
  auto r = parts::findParts(m);
  CHECK(findPart(r, "slot:M_Head") && findPart(r, "slot:Body.001"));
  const parts::Part* head = findPart(r, "region:head");
  CHECK(head && head->fallback);
  CHECK(parts::regionHint("face_skin") == "head");
  CHECK(parts::regionHint("HairCards") == "head");
  CHECK(parts::regionHint("Shirt_01") == "");
  CHECK(parts::regionHint("TorsoLow") == "body");
}

TEST(assistant, vertex_groups_round_trip) {
  Mesh m = testmesh::makeBust(16, 8);
  VertexGroup g;
  g.name = "Head";
  for (uint32_t v = 0; v < m.positions.size(); ++v)
    if (m.positions[v].y > 0.64f) g.verts.push_back(v);
  m.vertexGroups.push_back(g);
  m.vertexGroups.push_back({"Empty", {}});
  std::string bytes = encodePtMesh(m, true);
  container::File f;
  std::string err;
  CHECK(container::decode(bytes, "PTMESH01", f, &err));
  Mesh back;
  CHECK(decodePtMesh(f, back, &err));
  CHECK(back.vertexGroups.size() == 2);
  if (back.vertexGroups.size() != 2) return;
  CHECK(back.vertexGroups[0].name == "Head");
  CHECK(back.vertexGroups[0].verts == g.verts);
  CHECK(back.vertexGroups[1].verts.empty());
  back.finalize();
  auto r = parts::findParts(back);
  const parts::Part* gp = findPart(r, "group:Head");
  CHECK(gp != nullptr);
  if (gp) CHECK(gp->faces.size() == size_t(16 * 8));  // the whole sphere
  CHECK(!findPart(r, "group:Empty"));
  // files without the chunk still load, with no groups
  m.vertexGroups.clear();
  container::File f2;
  CHECK(container::decode(encodePtMesh(m, true), "PTMESH01", f2, &err));
  Mesh plain;
  CHECK(decodePtMesh(f2, plain, &err) && plain.vertexGroups.empty());
}

