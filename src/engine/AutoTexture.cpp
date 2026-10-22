#include "engine/AutoTexture.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>

namespace patina {

std::vector<std::string> nameTokens(const std::string& s) {
  std::vector<std::string> out;
  std::string cur;
  auto flush = [&] {
    if (!cur.empty()) out.push_back(cur), cur.clear();
  };
  for (size_t i = 0; i < s.size(); ++i) {
    unsigned char c = (unsigned char)s[i];
    if (std::isalpha(c)) {
      // camelCase: a capital after a lower-case letter starts a word
      if (std::isupper(c) && i > 0 && std::islower((unsigned char)s[i - 1])) flush();
      if (!cur.empty() && std::isdigit((unsigned char)cur.back())) flush();
      cur += (char)std::tolower(c);
    } else if (std::isdigit(c)) {
      if (!cur.empty() && !std::isdigit((unsigned char)cur.back())) flush();
      cur += (char)c;
    } else {
      flush();
    }
  }
  flush();
  return out;
}

static std::string singular(const std::string& t) {
  if (t.size() > 4 && t.compare(t.size() - 3, 3, "ies") == 0) return t.substr(0, t.size() - 3) + "y";
  if (t.size() > 3 && t.back() == 's' && t[t.size() - 2] != 's') return t.substr(0, t.size() - 1);
  return t;
}

// what a word in a slot name suggests, as words found in material ids, names, tags or categories
static const std::map<std::string, std::vector<std::string>>& synonyms() {
  static const std::map<std::string, std::vector<std::string>> m = [] {
    std::map<std::string, std::vector<std::string>> s;
    auto add = [&](std::initializer_list<const char*> words, std::initializer_list<const char*> to) {
      for (const char* w : words)
        for (const char* t : to) s[w].push_back(t);
    };
    add({"body", "face", "head", "flesh", "arm", "leg", "hand", "torso", "neck", "ear", "foot", "feet", "nose",
         "lip", "lips", "chest", "belly", "finger", "toe", "character", "char", "human", "skin"},
        {"skin"});
    add({"hair", "brow", "eyebrow", "lash", "eyelash", "beard", "mustache", "moustache", "fur", "ponytail", "bang",
         "bangs", "braid", "wig", "scalp"},
        {"hair"});
    add({"shirt", "tshirt", "blouse", "tunic", "robe", "apron", "curtain", "bandage", "cloth", "clothes", "clothing",
         "fabric", "garment", "outfit"},
        {"linen"});
    add({"shirt", "tshirt", "top", "blouse", "dress", "clothes", "clothing", "cloth", "fabric", "sleeve", "hoodie",
         "sock", "cape", "scarf", "robe", "tunic", "collar", "outfit", "garment", "costume", "suit", "vest", "apron",
         "curtain", "flag", "bandage", "ribbon", "bow", "tie", "glove"},
        {"cloth", "fabric", "clothing"});
    add({"pant", "pants", "trouser", "trousers", "short", "shorts", "chino"}, {"gabardine", "fabric", "clothing"});
    add({"jean", "jeans", "denim"}, {"denim"});
    add({"skirt", "kilt", "tartan"}, {"skirt", "plaid"});
    add({"uniform", "school", "blazer"}, {"uniform"});
    add({"jacket", "coat", "parka", "anorak"}, {"puffer", "gabardine", "clothing"});
    add({"sweater", "jumper", "knit", "cardigan", "beanie", "knitted"}, {"knit", "wool"});
    add({"shoe", "boot", "belt", "strap", "holster", "bag", "saddle", "wallet", "harness", "sandal", "heel", "glove"},
        {"leather"});
    add({"sofa", "couch", "seat", "cushion", "armchair", "upholstery"}, {"upholstery", "leather"});
    add({"sole", "tire", "tyre", "wheel", "rubber", "grip", "handle", "hose", "gasket"}, {"rubber", "grip"});
    add({"metal", "metallic", "met"}, {"metal", "steel"});
    add({"gun", "weapon", "blade", "sword", "knife", "barrel", "pistol", "rifle", "axe", "dagger", "bolt", "screw",
         "nut", "bracket", "pipe", "machine", "engine"},
        {"steel", "gunmetal", "metal"});
    add({"armor", "armour", "plate", "helmet", "gauntlet", "pauldron", "greave", "chainmail", "mail", "shield"},
        {"armor", "steel", "hard"});
    add({"buckle", "zipper", "zip", "button", "rivet", "jewelry", "jewellery", "ring", "necklace", "earring",
         "bracelet", "crown", "trim", "ornament", "coin", "medal", "badge"},
        {"brass", "gold", "silver"});
    add({"aluminium"}, {"aluminum", "aluminium"});
    add({"wood", "wooden", "timber", "plank", "crate", "table", "log", "branch", "trunk", "barrel", "chair", "door",
         "shelf", "furniture", "beam", "fence", "stick", "staff"},
        {"wood", "oak", "planks"});
    add({"floor", "flooring"}, {"floor", "floorboards"});
    add({"tree", "bark"}, {"bark"});
    add({"stone", "rock", "cliff", "boulder", "pebble", "statue", "pillar", "column"}, {"stone", "granite"});
    add({"brick"}, {"bricks"});
    add({"concrete", "cement"}, {"concrete"});
    add({"tile", "tiles"}, {"tiles"});
    add({"wall"}, {"wall", "plaster"});
    add({"road", "street", "pavement", "tarmac"}, {"asphalt", "road"});
    add({"ground", "terrain", "dirt", "soil", "mud", "earth"}, {"dirt", "mud", "ground"});
    add({"grass", "lawn", "meadow"}, {"grass"});
    add({"light", "lamp", "glow", "emissive", "emission", "neon", "led", "screen", "monitor", "display", "bulb"},
        {"glow", "emissive"});
    add({"car", "vehicle", "paint", "painted", "chassis", "hood", "bonnet", "fender"}, {"paint", "car"});
    add({"scale", "dragon", "snake", "fish", "reptile", "lizard"}, {"scales"});
    add({"hull", "panel", "scifi", "mech", "robot", "spaceship", "ship", "android", "cyber"},
        {"panels", "hull", "hard"});
    add({"silk", "satin"}, {"satin", "silk"});
    add({"nylon", "puffer", "down"}, {"puffer"});
    add({"camo", "camouflage", "military"}, {"camo"});
    add({"carbon"}, {"carbon"});
    add({"plastic", "case", "casing", "shell", "toy"}, {"plastic"});
    return s;
  }();
  return m;
}

// words that say nothing about the surface
static bool stopWord(const std::string& t) {
  static const std::set<std::string> s = {
      "mat", "material", "materials", "m", "mtl", "mt", "lod", "low", "high", "poly", "geo", "mesh", "obj", "object",
      "left", "right", "l", "r", "lt", "rt", "default", "new", "base", "main", "the", "of", "and", "slot", "surface",
      "shader", "bsdf", "principled", "texture", "tex", "set", "part", "piece", "a", "b", "c", "x", "y", "z", "mi"};
  if (s.count(t)) return true;
  return std::all_of(t.begin(), t.end(), [](char c) { return std::isdigit((unsigned char)c); });
}

// parts of a model that want painting by hand rather than a library material
static bool handPainted(const std::string& t) {
  static const std::set<std::string> s = {"eye",  "eyes", "iris",   "pupil", "sclera", "cornea", "teeth", "tooth",
                                          "tongue", "mouth", "nail", "nails", "glass", "lens",   "visor",  "window",
                                          "decal", "logo", "text", "label", "tattoo", "makeup", "blush", "eyeshadow"};
  return s.count(t) > 0;
}

static bool isStylizedTag(const std::string& t) {
  return t == "toon" || t == "cel" || t == "stylised" || t == "stylized" || t == "anime" || t == "hand-painted" ||
         t == "hand" || t == "painted";
}

float scoreMaterialForName(const std::string& slotName, const MaterialDef& def, const std::string& styleIn) {
  std::set<std::string> idName, tags;
  for (auto& t : nameTokens(def.id))
    if (t != "patina") idName.insert(t), idName.insert(singular(t));
  for (auto& t : nameTokens(def.name)) idName.insert(t), idName.insert(singular(t));
  bool stylized = false;
  for (auto& tag : def.tags) {
    std::string lt;
    for (char ch : tag) lt += (char)std::tolower((unsigned char)ch);
    tags.insert(lt);
    stylized |= isStylizedTag(lt);
    for (auto& t : nameTokens(tag)) tags.insert(t), tags.insert(singular(t));
  }
  std::string cat;
  for (char ch : def.category) cat += (char)std::tolower((unsigned char)ch);
  std::string catS = singular(cat);
  if (cat == "stylised" || cat == "stylized") stylized = true;

  std::vector<std::string> words;
  std::string style = styleIn == "stylised" ? "stylized" : styleIn;  // either spelling
  for (auto& t : nameTokens(slotName)) {
    if (stopWord(t)) continue;
    if (handPainted(t) || handPainted(singular(t))) return 0.f;  // leave these to the artist
    if (t == "toon" || t == "anime" || t == "cel" || t == "stylised" || t == "stylized") {
      style = "stylized";  // a style, not a surface
      continue;
    }
    words.push_back(t);
  }
  if (words.empty()) return 0.f;
  // "Gun_Metal" is also "gunmetal"
  for (size_t i = 0, n = words.size(); i + 1 < n; ++i) words.push_back(words[i] + words[i + 1]);
  float total = 0.f;
  std::set<std::string> seen;
  for (auto& w0 : words) {
    std::string w = singular(w0);
    if (!seen.insert(w).second) continue;
    std::vector<std::pair<std::string, float>> cands = {{w0, 1.f}, {w, 1.f}};
    auto& syn = synonyms();
    for (const std::string& k : {w0, w}) {
      auto it = syn.find(k);
      if (it != syn.end())  // the first synonym is the most specific
        for (size_t i = 0; i < it->second.size(); ++i) cands.push_back({it->second[i], 0.95f - 0.02f * float(i)});
    }
    float best = 0.f;
    for (auto& [k, wt] : cands) {
      float s = idName.count(k) ? 3.f : tags.count(k) ? 2.f : (cat == k || catS == k) ? 1.5f : 0.f;
      best = std::max(best, s * wt);
    }
    total += best;
  }
  if (total <= 0.f) return 0.f;
  if (style == "stylized") total += stylized ? 1.f : -0.5f;
  else if (style == "realistic") total += stylized ? -1.5f : 0.f;
  else if (stylized) total -= 0.5f;
  if (cat == "basic") total -= 1.f;  // flat colors only when the name is nothing but a color
  return std::max(total, 0.f);
}

std::vector<AutoTextureMatch> matchSlotMaterials(const Mesh& m, const MaterialLibrary& lib, const std::string& style) {
  uint32_t slots = 0;
  for (uint32_t s : m.faceMaterial) slots = std::max(slots, s + 1);
  slots = std::max<uint32_t>(slots, (uint32_t)m.materialNames.size());
  std::vector<int> faces(slots, 0);
  for (uint32_t f = 0; f < m.faceCount() && f < m.faceMaterial.size(); ++f) faces[m.faceMaterial[f]]++;
  std::vector<AutoTextureMatch> out;
  for (uint32_t s = 0; s < slots; ++s) {
    if (!faces[s]) continue;
    AutoTextureMatch r;
    r.slot = s;
    r.slotName = m.slotName(s);
    r.faces = faces[s];
    std::vector<std::pair<float, const MaterialDef*>> ranked;
    for (auto& e : lib.entries()) {
      if (e->transient) continue;
      float sc = scoreMaterialForName(r.slotName, e->def, style);
      if (sc > 0) ranked.push_back({sc, &e->def});
    }
    std::stable_sort(ranked.begin(), ranked.end(), [](auto& a, auto& b) { return a.first > b.first; });
    if (!ranked.empty() && ranked[0].first >= 2.5f) {
      r.materialId = ranked[0].second->id;
      r.materialName = ranked[0].second->name;
      r.score = ranked[0].first;
    }
    for (size_t i = r.materialId.empty() ? 0 : 1; i < ranked.size() && r.alternatives.size() < 4; ++i)
      r.alternatives.push_back(ranked[i].second->id);
    out.push_back(std::move(r));
  }
  return out;
}

}  // namespace patina
