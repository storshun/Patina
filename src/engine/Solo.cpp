// (1.2.19) Solo: pieces of the mesh hidden by hand so one piece (the hair, the lashes, a sleeve) is shown and painted
// alone, as Blender's H / Shift+H / Alt+H. The pieces are UV islands, connected parts, material slots or face sets.
// Hidden faces leave the 3D view, the brush, fills and picking (Document::updateHiddenFaces); the paint stays.
#include <algorithm>
#include <queue>

#include "engine/Engine.h"

namespace patina {

size_t Document::pieceHiddenCount() const { return (size_t)std::count(pieceHidden.begin(), pieceHidden.end(), uint8_t(1)); }

std::vector<uint32_t> Document::pieceFaces(uint32_t face, PieceBy by) const {
  std::vector<uint32_t> out;
  const Mesh& m = mesh;
  if (face >= m.faceCount()) return out;
  switch (by) {
    case PieceBy::Island: {
      if (m.loopIsland.size() != m.loopCount()) return {face};
      uint32_t isl = m.loopIsland[m.faceStart[face]];
      for (uint32_t f = 0; f < m.faceCount(); ++f)
        if (m.loopIsland[m.faceStart[f]] == isl) out.push_back(f);
      break;
    }
    case PieceBy::Material: {
      uint32_t slot = face < m.faceMaterial.size() ? m.faceMaterial[face] : 0;
      for (uint32_t f = 0; f < m.faceCount(); ++f)
        if ((f < m.faceMaterial.size() ? m.faceMaterial[f] : 0) == slot) out.push_back(f);
      break;
    }
    case PieceBy::FaceSet: {
      uint32_t id = face < faceSetOf.size() ? faceSetOf[face] : 0;
      for (uint32_t f = 0; f < m.faceCount(); ++f)
        if ((f < faceSetOf.size() ? faceSetOf[f] : 0) == id) out.push_back(f);
      break;
    }
    case PieceBy::Connected: {
      // faces sharing a vertex (not only an edge): a part's corners that touch stay one part
      std::vector<std::vector<uint32_t>> vertFaces(m.positions.size());
      for (uint32_t f = 0; f < m.faceCount(); ++f)
        for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) vertFaces[m.loopVert[l]].push_back(f);
      std::vector<uint8_t> seen(m.faceCount(), 0);
      std::queue<uint32_t> q;
      q.push(face);
      seen[face] = 1;
      while (!q.empty()) {
        uint32_t f = q.front();
        q.pop();
        out.push_back(f);
        for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l)
          for (uint32_t g : vertFaces[m.loopVert[l]])
            if (!seen[g]) seen[g] = 1, q.push(g);
      }
      std::sort(out.begin(), out.end());
      break;
    }
  }
  return out;
}

void Document::setPieceHidden(std::vector<uint8_t> hidden, const std::string& label, bool showSets) {
  if (!hasMesh()) return;
  if (!hidden.empty() && hidden.size() != mesh.faceCount()) hidden.assign(mesh.faceCount(), 0);
  if (std::find(hidden.begin(), hidden.end(), uint8_t(1)) == hidden.end()) hidden.clear();
  auto sets = faceSets;
  bool hideUnset = hideUnsetFaces;
  if (showSets) {
    for (auto& s : sets) s.hidden = false;
    hideUnset = false;
  }
  if (hidden == pieceHidden && sets == faceSets && hideUnset == hideUnsetFaces) return;
  auto e = std::make_unique<LambdaEntry>();
  e->label = label;
  auto oldHidden = pieceHidden;
  auto oldSets = faceSets;
  bool oldUnset = hideUnsetFaces;
  // (the face sets' visibility only: which face is in which set is left as it is then)
  auto restore = [](Document& d, const std::vector<uint8_t>& h, const std::vector<FaceSet>& s, bool unset) {
    d.pieceHidden = h;
    for (auto& fs : d.faceSets)
      for (auto& o : s)
        if (o.id == fs.id) fs.hidden = o.hidden;
    d.hideUnsetFaces = unset;
    d.updateHiddenFaces();
  };
  e->onUndo = [restore, oldHidden, oldSets, oldUnset](Document& d) { restore(d, oldHidden, oldSets, oldUnset); };
  e->onRedo = [restore, hidden, sets, hideUnset](Document& d) { restore(d, hidden, sets, hideUnset); };
  pieceHidden = std::move(hidden);
  faceSets = std::move(sets);
  hideUnsetFaces = hideUnset;
  updateHiddenFaces();
  dirty = true;
  undo.push(std::move(e));
}

}  // namespace patina

namespace patina {

namespace {
json prop(const char* type, const char* desc) { return {{"type", type}, {"description", desc}}; }
json num(const char* type, const char* desc, double lo, double hi) {
  return {{"type", type}, {"description", desc}, {"minimum", (int64_t)lo}, {"maximum", (int64_t)hi}};
}
json propEnum(std::vector<std::string> values, const char* desc) {
  return {{"type", "string"}, {"enum", values}, {"description", desc}};
}
json schema(json props) { return {{"type", "object"}, {"properties", props}, {"additionalProperties", false}}; }

const char* kPieceNames[] = {"island", "connected", "material", "face_set"};

Document::PieceBy pieceByArg(const json& a, int fallback) {
  std::string by = a.value("by", std::string(kPieceNames[std::clamp(fallback, 0, 3)]));
  for (int i = 0; i < 4; ++i)
    if (by == kPieceNames[i]) return (Document::PieceBy)i;
  throw CommandError("by: island, connected, material or face_set");
}
}  // namespace

json Engine::soloJson() const {
  size_t hidden = doc.pieceHiddenCount(), all = doc.mesh.faceCount();
  return json{{"picking", soloPicking}, {"by", kPieceNames[std::clamp(soloBy, 0, 3)]}, {"hidden_faces", hidden},
              {"shown_faces", all - std::min(all, hidden)}, {"faces", all}};
}

void Engine::registerSoloCommands() {
  auto& R = commands;
  R.add({"solo",
         "(1.2.19) Solo: shows one piece of the active object alone (and paints only it), as Blender's Shift+H / H / "
         "Alt+H. A piece is a UV island (by: island, the default), a connected part of the mesh (connected), a "
         "material slot (material) or a face set (face_set; the faces in none are one piece). Give the piece by a "
         "face index (face) or a point in the 3D view ([x, y], 0..1 from the top left; it must be on the model). "
         "action: only (show the piece alone; toggle: true shows everything again when it is already shown alone), "
         "add (show it too), hide (hide it), show_all (every piece and every face set shown again), invert (the "
         "hidden pieces shown, the shown hidden). Hidden faces leave the 3D view, the brush, fills, picking and the "
         "UV editor; renders and exports keep them, and so does the paint. Faces whose UVs share texels with a shown "
         "piece (stacked islands) still get its paint there: the texels are the same. One undo step; kept in the "
         "project. Returns the faces of the piece and how many faces are shown and hidden.",
         schema({{"action", propEnum({"only", "add", "hide", "show_all", "invert"}, "What to do")},
                 {"by", propEnum({"island", "connected", "material", "face_set"}, "What a piece is (default: the window's pick, island at first)")},
                 {"face", num("integer", "A face of the piece", 0, 1e9)},
                 {"point", prop("array", "A point on the model in the 3D view, [x, y] 0..1 from the top left")},
                 {"toggle", prop("boolean", "only: show everything again when the piece is already shown alone")},
                 {"picking", prop("boolean", "The window's Solo picker on (clicks pick pieces) or off (no other action needed)")}}),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           Document& d = e.doc;
           if (!d.hasMesh()) throw CommandError("no model open");
           if (a.contains("by")) e.soloBy = (int)pieceByArg(a, e.soloBy);
           if (a.contains("picking")) e.soloPicking = a["picking"].get<bool>();
           if (!a.contains("action")) return e.soloJson();
           std::string act = a["action"];
           uint32_t F = d.mesh.faceCount();
           std::vector<uint8_t> hidden = d.pieceHidden;
           if (hidden.size() != F) hidden.assign(F, 0);
           json r;
           if (act == "show_all") {
             d.setPieceHidden({}, "Show everything", true);
           } else if (act == "invert") {
             if (!d.pieceHiddenCount()) throw CommandError("nothing is hidden by Solo");
             for (auto& h : hidden) h = !h;
             d.setPieceHidden(std::move(hidden), "Solo: invert");
           } else if (act == "only" || act == "add" || act == "hide") {
             uint32_t face = 0;
             if (a.contains("face")) {
               face = a["face"].get<uint32_t>();
               if (face >= F) throw CommandError("face: out of range");
             } else if (a.contains("point")) {
               if (!a["point"].is_array() || a["point"].size() < 2) throw CommandError("point is [x, y]");
               RayHit h = e.pick(vec2(a["point"][0].get<float>(), a["point"][1].get<float>()) * e.viewportSize);
               if (!h.hit) throw CommandError("the point is not on the model (the active object)");
               face = h.face;
             } else {
               throw CommandError("give the piece: face or point");
             }
             auto by = pieceByArg(a, e.soloBy);
             auto piece = d.pieceFaces(face, by);
             r["piece_faces"] = piece.size();
             if (act == "only") {
               std::vector<uint8_t> only(F, 1);
               for (uint32_t f : piece) only[f] = 0;
               // (again on the piece shown alone, as Blender's Shift+H: everything comes back)
               if (a.value("toggle", false) && only == hidden) d.setPieceHidden({}, "Show everything", true);
               else d.setPieceHidden(std::move(only), "Solo a piece");
             } else if (act == "add") {
               for (uint32_t f : piece) hidden[f] = 0;
               d.setPieceHidden(std::move(hidden), "Solo: show a piece");
             } else {
               for (uint32_t f : piece) hidden[f] = 1;
               if (std::find(hidden.begin(), hidden.end(), uint8_t(0)) == hidden.end())
                 throw CommandError("that would hide every face: show_all, or only to show a piece alone");
               d.setPieceHidden(std::move(hidden), "Solo: hide a piece");
             }
           } else {
             throw CommandError("action: only, add, hide, show_all or invert");
           }
           json s = e.soloJson();
           for (auto& [k, v] : r.items()) s[k] = v;
           return s;
         }});
}

}  // namespace patina
