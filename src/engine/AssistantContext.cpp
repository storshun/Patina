#include "engine/AssistantContext.h"

#include <algorithm>
#include <cctype>
#include <cmath>

#include "engine/AutoTexture.h"
#include "engine/Engine.h"

namespace patina {

namespace {

std::string fmt(float v, int decimals = 2) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.*f", decimals, v);
  return buf;
}

std::string pct(size_t n, size_t total) {
  if (!total) return "0%";
  long p = std::lround(100.0 * n / total);
  return (p == 0 && n ? std::string("<1") : std::to_string(p)) + "%";
}

std::string maskSummary(const Layer& l) {
  if (!l.maskEffects.empty()) {
    std::string s;
    for (auto& fx : l.maskEffects) {
      if (!fx.enabled) continue;
      if (!s.empty()) s += "+";
      if (fx.type == MaskEffect::Generator) s += maskGeneratorName(fx.gen.type);
      else s += maskEffectName(fx.type);
    }
    return s.empty() ? "" : "mask " + s;
  }
  std::string s;
  if (l.mask) s = "painted";
  if (l.generator.type != MaskGenerator::None) s += (s.empty() ? "" : "+") + std::string(maskGeneratorName(l.generator.type));
  return s.empty() ? "" : "mask " + s;
}

// Words for a whole look ("battle-worn", "abandoned") or for painting by hand name no material,
// but they mean the weathering ones: the shortlist gets those too.
std::string lookMaterialWords(const std::string& request) {
  static const std::vector<std::pair<std::vector<const char*>, const char*>> table = {
      {{"weathered", "aged", "old", "weather", "timeworn", "time"}, "dirt dust grime"},
      {{"battle", "battered", "beaten", "war", "damaged"}, "scratched steel dirt soot"},
      {{"rusty", "rusted", "corroded", "corrosion"}, "rust grime"},
      {{"abandoned", "derelict", "neglected", "forgotten", "ruined"}, "dust grime moss"},
      {{"muddy", "splattered", "caked", "mud"}, "mud dirt"},
      {{"burnt", "burned", "scorched", "charred", "fire", "singed", "smoky"}, "soot"},
      {{"snowy", "wintry", "snowed", "frosty", "frost", "winter", "icy"}, "snow"},
      {{"overgrown", "mossy", "nature", "forest", "damp"}, "moss dirt"},
      {{"dusty", "attic"}, "dust"},
      {{"grimy", "filthy", "greasy", "dirty"}, "grime dirt dust"},
      {{"scratches", "scratched", "scratch"}, "scratched steel"},
  };
  std::string words, w;
  std::vector<std::string> tokens;
  for (char ch : request + " ") {
    if (std::isalpha((unsigned char)ch)) w += (char)std::tolower((unsigned char)ch);
    else if (!w.empty()) tokens.push_back(w), w.clear();
  }
  for (auto& [keys, extra] : table)
    for (const char* k : keys)
      if (std::find(tokens.begin(), tokens.end(), k) != tokens.end()) {
        words += std::string(" ") + extra;
        break;
      }
  return words;
}

}  // namespace

const char* assistantSystemPrompt() {
  return "You are the Patina Assistant, inside Patina, a 3D texture painting program. Turn the user's request "
         "into a plan of Patina commands for the project described.\n"
         "Answer with one JSON object and nothing else: {\"say\": \"one short sentence on what the plan does\", "
         "\"steps\": [{\"cmd\": \"command\", \"args\": {}}]}\n"
         "Use only the part ids, layer indices and material ids the project lists. A step can use an earlier "
         "step's result: \"$0.index\" is step 0's index.\n"
         "If Patina can't do it, give no steps: say why in one sentence and suggest up to three close requests it "
         "can do, {\"say\": \"...\", \"suggest\": [\"...\"]}. A question about the project gets a short answer in say.\n"
         "If the request is unclear or names a part that isn't listed, answer {\"ask\": \"a short question\"} and name "
         "the parts there are.\n"
         "Amounts: slightly 0.15-0.3, a bit 0.3-0.45, noticeably 0.45-0.65, strongly 0.65-0.9.";
}

nlohmann::json buildAssistantContext(Engine& e, const std::string& request, const AssistantContextOptions& opt) {
  using json = nlohmann::json;
  Document& d = e.doc;
  std::string t;
  json partsOut = json::array(), matsOut = json::array();
  if (!d.hasMesh()) {
    t = "Model: none open\n";
  } else {
    const Mesh& m = d.mesh;
    vec3 size = m.bounds.hi - m.bounds.lo;
    const SymmetryReport& sym = d.symmetry();
    bool symmetric = sym.unmatched * 20 < std::max(1, sym.vertices) && sym.p95 <= sym.size * 0.01f;
    t += "Model: " + m.name + " | " + std::to_string(m.faceCount()) + " faces | Y up | size " + fmt(size.x) + " x " +
         fmt(size.y) + " x " + fmt(size.z) + " | " + (symmetric ? "roughly symmetric (X)" : "not symmetric") + "\n";
    std::string tiles = "off";
    if (m.tiles.active()) {
      tiles = std::string((m.tiles.mode & TileBySlot) ? "texture sets" : "") +
              ((m.tiles.mode & TileBySlot) && (m.tiles.mode & TileByUdim) ? " + " : "") +
              ((m.tiles.mode & TileByUdim) ? "UDIM tiles" : "") + ":";
      for (auto& ti : m.tiles.tiles) tiles += " " + ti.name;
    }
    t += "Texture: " + std::to_string(d.tileRes()) + " px | " + tiles + "\n";
    // parts
    const parts::PartsReport& P = e.meshParts();
    t += "Parts:\n";
    int shown = 0;
    for (auto& p : P.parts) {
      if (shown++ >= opt.maxParts) break;
      t += "- " + p.id + " | " + p.label + " | " + pct(p.faces.size(), m.faceCount()) +
           (p.fallback ? " | a named part covers this, prefer it" : "") + "\n";
      partsOut.push_back({{"id", p.id}, {"label", p.label}, {"faces", p.faces.size()}});
    }
    if (P.parts.empty()) t += "- (one piece, one material slot)\n";
    if (!P.neck.found) t += "Head: not found (" + P.neck.why + ")\n";
    auto sel = d.selectedFaces();
    if (!sel.empty()) {
      t += "Selection: " + std::to_string(sel.size()) + " faces (part id: selection)\n";
      partsOut.push_back({{"id", "selection"}, {"label", "the selected faces"}, {"faces", sel.size()}});
    }
    // layers, top of the stack first
    int listed = 0, stack = 0;
    for (auto& lp : d.layers) stack += !lp->isBaked();
    t += "Layers (top first):";
    if (!stack) t += " none";
    t += "\n";
    for (int i = (int)d.layers.size() - 1; i >= 0 && listed < opt.maxLayers; --i) {
      const Layer& l = *d.layers[i];
      if (l.isBaked()) continue;  // (0.41) baked map layers: not part of what the model was trained on
      ++listed;
      static const char* types[] = {"paint", "fill", "adjust", "folder"};
      std::string line = "- " + std::to_string(i) + " \"" + l.name + "\" " + types[(int)l.type];
      if (l.type == LayerType::Fill) line += " " + (l.material.id.empty() ? std::string("flat colour") : l.material.id);  // the model was trained on this wording
      if (int depth = d.depthOf(i); depth > 0) line += " | in folder";
      std::string mask = maskSummary(l);
      if (!mask.empty()) line += " | " + mask;
      if (l.blend != BlendMode::Normal) line += " | " + std::string(blendModeName(l.blend));
      if (l.opacity < 0.999f) line += " | opacity " + fmt(l.opacity);
      if (!l.visible) line += " | hidden";
      t += line + "\n";
    }
    if (stack > listed) t += "- (" + std::to_string(stack - listed) + " more below)\n";
    if (d.selected >= 0) t += "Selected layer: " + std::to_string(d.selected) + "\n";
  }
  // what the last requests made, for follow-ups
  int shownTurns = 0;
  for (auto it = e.assistantTurns.rbegin(); it != e.assistantTurns.rend() && shownTurns < 2; ++it) {
    if (it->serial && !d.undo.isApplied(it->serial)) continue;  // undone since
    std::string made;
    for (uint32_t uid : it->layers) {
      int i = d.indexOf(uid);
      if (i < 0) continue;
      made += (made.empty() ? "" : ", ") + std::string("layer ") + std::to_string(i) + " \"" + d.layers[i]->name + "\"";
    }
    t += std::string(shownTurns ? "Before that: \"" : "Last request: \"") + it->request + "\"" +
         (made.empty() ? "" : " -> made " + made) + "\n";
    ++shownTurns;
  }
  // how painting is set up, so "a bigger brush" or "softer" can be relative
  {
    const Painter& P = e.painter;
    json b = P.brush.toJson();
    static const char* tools[] = {"paint", "erase", "fill", "picker", "blur", "clone", "smudge"};
    json mat = P.material.toJson();
    std::string paints = mat.value("id", std::string());
    if (paints.empty() && mat.contains("flat")) {
      auto c = mat["flat"].value("color", json::array({0.8, 0.8, 0.8}));
      char hex[16];
      std::snprintf(hex, sizeof hex, "#%02x%02x%02x", (int)std::lround(std::clamp(c[0].get<double>(), 0.0, 1.0) * 255),
                    (int)std::lround(std::clamp(c[1].get<double>(), 0.0, 1.0) * 255),
                    (int)std::lround(std::clamp(c[2].get<double>(), 0.0, 1.0) * 255));
      paints = std::string("flat colour ") + hex;
    }
    std::string sym;
    for (int a = 0; a < 3; ++a)
      if (P.symmetry[a]) sym += "xyz"[a];
    t += "Painting: tool " + std::string(tools[(int)P.tool]) + " | brush " + e.selectedBrushId + ", size " +
         fmt(b.value("size", 0.f), 0) + ", hardness " + fmt(b.value("hardness", 0.f)) + ", flow " + fmt(b.value("flow", 1.f)) +
         ", opacity " + fmt(b.value("opacity", 1.f)) + (b.value("lazy_radius", 0.f) > 0 ? ", lazy " + fmt(b.value("lazy_radius", 0.f), 0) : "") +
         (P.lockIsland ? ", island lock" : "") + " | symmetry " + (sym.empty() ? "off" : sym) + " | paints " + paints + "\n";
  }
  // library materials that fit the request (and the slot names)
  std::vector<std::pair<float, const MaterialLibrary::Entry*>> scored;
  std::string query = request + lookMaterialWords(request);
  for (auto& me : e.materials.entries()) {
    float s = request.empty() ? 0.f : scoreMaterialForName(query, me->def);
    if (s > 0) scored.push_back({s, me.get()});
  }
  std::stable_sort(scored.begin(), scored.end(), [](auto& a, auto& b) { return a.first > b.first; });
  if ((int)scored.size() > opt.maxMaterials) scored.resize(opt.maxMaterials);
  t += "Materials:";
  if (scored.empty()) t += " (none match the request)";
  t += "\n";
  for (auto& [s, me] : scored) {
    t += "- " + me->def.id + " | " + me->def.name + "\n";
    matsOut.push_back(me->def.id);
  }
  return json{{"text", t}, {"parts", partsOut}, {"materials", matsOut}, {"tokens", int(t.size() / 3.6)},
              {"system", assistantSystemPrompt()}};
}

}  // namespace patina
