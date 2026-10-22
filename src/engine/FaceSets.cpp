// (1.2) Face sets: named, colored groups of faces kept in the project (Document::faceSets), as Blender's sculpt face
// sets. A face is in one set at most; a hidden set's faces leave the 3D view, the brush and picking.
#include <algorithm>
#include <cmath>

#include "engine/Document.h"

namespace patina {

Document::FaceSet* Document::findFaceSet(uint32_t id) {
  for (auto& s : faceSets)
    if (s.id == id) return &s;
  return nullptr;
}

int Document::faceSetIndex(const std::string& key) const {
  for (size_t i = 0; i < faceSets.size(); ++i)
    if (faceSets[i].name == key) return (int)i;
  auto lower = [](std::string x) {
    for (auto& c : x) c = (char)std::tolower((unsigned char)c);
    return x;
  };
  for (size_t i = 0; i < faceSets.size(); ++i)
    if (lower(faceSets[i].name) == lower(key)) return (int)i;
  if (!key.empty() && std::all_of(key.begin(), key.end(), [](char c) { return c >= '0' && c <= '9'; })) {
    uint32_t id = (uint32_t)std::stoul(key);
    for (size_t i = 0; i < faceSets.size(); ++i)
      if (faceSets[i].id == id) return (int)i;
  }
  return -1;
}

std::vector<uint32_t> Document::faceSetFaces(uint32_t id) const {
  std::vector<uint32_t> out;
  for (uint32_t f = 0; f < faceSetOf.size(); ++f)
    if (faceSetOf[f] == id) out.push_back(f);
  return out;
}

size_t Document::faceSetSize(uint32_t id) const { return (size_t)std::count(faceSetOf.begin(), faceSetOf.end(), id); }

vec3 Document::faceSetColor(uint32_t id) {
  // hues a golden angle apart, light and saturated enough to read over any paint
  float h = std::fmod(0.07f + float(id) * 0.618034f, 1.f);
  vec3 k = glm::clamp(glm::abs(glm::mod(h * 6.f + vec3(0, 4, 2), 6.f) - 3.f) - 1.f, 0.f, 1.f);
  return glm::mix(vec3(1.f), k, 0.62f) * 0.92f;
}

void Document::updateHiddenFaces() {
  bool any = hideUnsetFaces && !faceSets.empty();
  for (auto& s : faceSets) any |= s.hidden;
  if (!any || faceSetOf.size() != mesh.faceCount()) {
    hiddenFaces.clear();
  } else {
    hiddenFaces.assign(mesh.faceCount(), 0);
    for (uint32_t f = 0; f < mesh.faceCount(); ++f) {
      if (!faceSetOf[f]) hiddenFaces[f] = hideUnsetFaces;
      else if (const FaceSet* s = findFaceSet(faceSetOf[f]); s && s->hidden) hiddenFaces[f] = 1;
    }
  }
  updateSelectionFlags();
}

void Document::setFaceSets(std::vector<FaceSet> sets, std::vector<uint32_t> of, const std::string& label, int hideUnset) {
  // (sets without faces go, unless they are the only thing asked for)
  if (of.size() != mesh.faceCount()) of.assign(mesh.faceCount(), 0);
  if (sets.empty() || std::all_of(of.begin(), of.end(), [](uint32_t v) { return v == 0; })) {
    if (sets.empty()) of.clear();
  }
  uint32_t next = nextFaceSetId;
  for (auto& s : sets) next = std::max(next, s.id + 1);
  auto e = std::make_unique<LambdaEntry>();
  e->label = label;
  auto oldSets = faceSets;
  auto oldOf = faceSetOf;
  bool oldHide = hideUnsetFaces, hide = hideUnset < 0 ? hideUnsetFaces : hideUnset > 0;
  if (sets.empty()) hide = false;
  e->onUndo = [oldSets, oldOf, oldHide](Document& d) {
    d.faceSets = oldSets, d.faceSetOf = oldOf, d.hideUnsetFaces = oldHide, d.updateHiddenFaces();
  };
  e->onRedo = [sets, of, hide](Document& d) { d.faceSets = sets, d.faceSetOf = of, d.hideUnsetFaces = hide, d.updateHiddenFaces(); };
  faceSets = std::move(sets);
  faceSetOf = std::move(of);
  hideUnsetFaces = hide;
  nextFaceSetId = next;
  updateHiddenFaces();
  dirty = true;
  undo.push(std::move(e));
}

uint32_t Document::addFaceSet(const std::vector<uint32_t>& faces, const std::string& name, const vec3* color) {
  auto sets = faceSets;
  auto of = faceSetOf;
  if (of.size() != mesh.faceCount()) of.assign(mesh.faceCount(), 0);
  FaceSet s;
  s.id = nextFaceSetId;
  s.name = name.empty() ? "Face Set " + std::to_string(s.id) : name;
  s.color = color ? *color : faceSetColor(s.id);
  for (uint32_t f : faces)
    if (f < of.size()) of[f] = s.id;
  sets.push_back(s);
  // (sets left without faces go)
  sets.erase(std::remove_if(sets.begin(), sets.end(),
                            [&](const FaceSet& x) { return x.id != s.id && std::find(of.begin(), of.end(), x.id) == of.end(); }),
             sets.end());
  setFaceSets(std::move(sets), std::move(of), "Make a face set");
  return s.id;
}

void Document::carryFaceSets(const Mesh& oldMesh, const Bvh& oldBvh, const std::vector<uint32_t>& oldOf) {
  if (faceSets.empty() || oldOf.size() != oldMesh.faceCount() || oldBvh.empty()) {
    if (faceSetOf.size() != mesh.faceCount()) faceSetOf.clear(), faceSets.clear(), hiddenFaces.clear();
    return;
  }
  std::vector<uint32_t> of(mesh.faceCount(), 0);
  for (uint32_t f = 0; f < mesh.faceCount(); ++f) {
    RayHit h = oldBvh.closestPoint(oldMesh, mesh.faceCenter(f));
    if (h.hit && h.face < oldOf.size()) of[f] = oldOf[h.face];
  }
  faceSetOf = std::move(of);
  updateHiddenFaces();
}

}  // namespace patina
