#include "engine/Regions.h"

#include <algorithm>

#include "engine/Engine.h"

namespace patina::regions {

namespace {

json propColor(const char* desc) {
  return {{"type", "array"}, {"items", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}}, {"minItems", 3},
          {"maxItems", 3}, {"description", desc}};
}

const std::vector<std::string> kMaskTypes = {"cavity", "edges",      "occlusion",     "facing_up", "facing_down", "facing",
                                             "gradient_up", "gradient_down", "noise", "thin", "gradient", "drips", "pattern"};

}  // namespace

json regionSchema() {
  return {{"type", "object"},
          {"description", "Where the layer shows: a part id from mesh_parts (slot:<name>, piece:<n>, group:<name>, "
                          "region:head, region:body, selection), several parts, and parts to leave out"},
          {"properties",
           {{"part", {{"type", "string"}}},
            {"parts", {{"type", "array"}, {"items", {{"type", "string"}}}, {"minItems", 1}}},
            {"except", {{"type", "array"}, {"items", {{"type", "string"}}}}}}},
          {"additionalProperties", false}};
}

json maskSchema() {
  return {{"type", "array"},
          {"description",
           "Smart mask stack, multiplied together: cavity (creases, pores), edges (worn edges), occlusion (sheltered "
           "places), facing_up (where dust settles), facing_down, facing (direction), gradient_up (stronger toward the "
           "top), gradient_down, gradient (along direction), noise (breakup; kind clouds, cells, cracks, ridges or "
           "streaks), thin (thin parts), drips (streaks running down the sides), pattern (a library material's height or "
           "brightness: material, source). level = threshold 0..1 (higher = less coverage), contrast 0 soft .. 1 hard"},
          {"items",
           {{"type", "object"},
            {"properties",
             {{"type", {{"type", "string"}, {"enum", kMaskTypes}}},
              {"level", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}},
              {"contrast", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}},
              {"noise", {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}},
              {"scale", {{"type", "number"}, {"minimum", 0.05}, {"maximum", 20}}},
              {"invert", {{"type", "boolean"}}},
              {"direction", propColor("facing, gradient: [x, y, z]")},
              {"kind", {{"type", "string"}, {"enum", {"clouds", "cells", "cracks", "ridges", "streaks"}}}},
              {"material", {{"type", "string"}, {"description", "pattern: a library material id"}}},
              {"source", {{"type", "string"}, {"enum", {"height", "brightness", "roughness", "metallic"}}}}}},
            {"required", {"type"}},
            {"additionalProperties", false}}},
          {"maxItems", 4}};
}

namespace {

uint32_t slotOfPart(Engine& e, const std::string& id, bool* ok) {
  *ok = false;
  if (id.rfind("slot:", 0) != 0) return 0;
  for (auto& p : e.meshParts().parts)
    if (p.id == id && !p.faces.empty()) {
      *ok = true;
      uint32_t f = p.faces.front();
      return f < e.doc.mesh.faceMaterial.size() ? e.doc.mesh.faceMaterial[f] : 0;
    }
  return 0;
}

}  // namespace

Region regionArg(Engine& e, const json& a) {
  Region r;
  if (!a.contains("region") || a["region"].is_null()) return r;
  const json& j = a["region"];
  std::vector<std::string> in, out;
  if (j.is_string()) in.push_back(j.get<std::string>());
  else if (j.is_object()) {
    if (j.contains("part")) in.push_back(j["part"].get<std::string>());
    for (auto& p : j.value("parts", json::array())) in.push_back(p.get<std::string>());
    for (auto& p : j.value("except", json::array())) out.push_back(p.get<std::string>());
  } else {
    throw CommandError("region must be a part id or {part | parts, except}");
  }
  if (in.empty() && out.empty()) return r;
  r.given = true;
  r.ids = in;
  uint32_t F = e.doc.mesh.faceCount();
  std::vector<uint8_t> keep(F, in.empty() ? 1 : 0);
  r.slotsOnly = out.empty() && !in.empty();
  for (auto& id : in) {
    for (uint32_t f : e.partFaces(id)) keep[f] = 1;
    bool ok = false;
    uint32_t s = slotOfPart(e, id, &ok);
    if (ok) r.slots.push_back(s);
    else r.slotsOnly = false;
  }
  for (auto& id : out)
    for (uint32_t f : e.partFaces(id)) keep[f] = 0;
  for (uint32_t f = 0; f < F; ++f)
    if (keep[f]) r.faces.push_back(f);
  if (r.faces.empty()) throw CommandError("the region has no faces left");
  return r;
}

namespace {

MaskEffect generatorEffect(const json& spec, bool first) {
  std::string t = spec.at("type");
  MaskEffect fx;
  fx.type = MaskEffect::Generator;
  fx.blend = first ? MaskEffect::Normal : MaskEffect::Multiply;
  MaskGenerator& g = fx.gen;
  g.invert = spec.value("invert", false);
  if (t == "cavity") g.type = MaskGenerator::Cavity, g.level = 0.55f, g.contrast = 0.6f;
  else if (t == "edges") g.type = MaskGenerator::Edges, g.level = 0.5f, g.contrast = 0.6f;
  else if (t == "occlusion") g.type = MaskGenerator::Occlusion, g.level = 0.5f, g.contrast = 0.5f;
  else if (t == "facing_up" || t == "facing_down" || t == "facing") {
    g.type = MaskGenerator::Facing, g.level = 0.5f, g.contrast = 0.5f;
    g.direction = t == "facing_down" ? vec3(0, -1, 0) : vec3(0, 1, 0);
  } else if (t == "gradient_up" || t == "gradient_down") {
    g.type = MaskGenerator::GradientY, g.level = 0.5f, g.contrast = 0.3f;
    if (t == "gradient_down") g.invert = !g.invert;
  } else if (t == "noise") {
    g.type = MaskGenerator::Noise, g.level = 0.5f, g.contrast = 0.5f;
    g.noiseType = maskNoiseTypeFromName(spec.value("kind", std::string("clouds")));
  } else if (t == "thin") g.type = MaskGenerator::Thin, g.level = 0.5f, g.contrast = 0.6f;
  else if (t == "gradient") g.type = MaskGenerator::Gradient, g.level = 0.5f, g.contrast = 0.3f;
  else if (t == "drips") g.type = MaskGenerator::Drips, g.level = 0.4f, g.contrast = 0.6f;
  else if (t == "pattern") {
    fx.type = MaskEffect::Pattern;
    fx.pattern.id = spec.value("material", std::string());
    if (fx.pattern.id.empty()) throw CommandError("a pattern mask needs a material");
    std::string src = spec.value("source", std::string("height"));
    fx.source = src == "brightness" ? 1 : src == "roughness" ? 2 : src == "metallic" ? 3 : 0;
    fx.level = std::clamp(spec.value("level", 0.5f), 0.f, 1.f);
    fx.contrast = std::clamp(spec.value("contrast", 0.6f), 0.f, 1.f);
    fx.invert = spec.value("invert", false);
    fx.pattern.scale = std::clamp(spec.value("scale", 1.f), 0.05f, 20.f);
    return fx;
  } else throw CommandError("unknown mask type '" + t + "'");
  g.level = std::clamp(spec.value("level", g.level), 0.f, 1.f);
  g.contrast = std::clamp(spec.value("contrast", g.contrast), 0.f, 1.f);
  g.noise = std::clamp(spec.value("noise", g.noise), 0.f, 1.f);
  g.noiseScale = std::clamp(spec.value("scale", g.noiseScale), 0.05f, 20.f);
  if (spec.contains("direction") && spec["direction"].is_array() && spec["direction"].size() == 3) {
    vec3 dir(spec["direction"][0].get<float>(), spec["direction"][1].get<float>(), spec["direction"][2].get<float>());
    if (glm::length(dir) > 1e-6f) g.direction = glm::normalize(dir);
  }
  return fx;
}

// the leading effects that make a layer's region (material slots or the painted mask)
bool isRegionEffect(const MaskEffect& fx) {
  return fx.type == MaskEffect::Paint || (fx.type == MaskEffect::Generator && fx.gen.type == MaskGenerator::MaterialSlot);
}

}  // namespace

void applyRegionAndMasks(Engine& e, int index, const Region& r, const json& masks, bool keepRegion, bool keepMasks) {
  Document& d = e.doc;
  Layer& layer = *d.layers[index];
  if (layer.maskEffects.empty() && (layer.generator.type != MaskGenerator::None || layer.mask)) layer.convertMaskToStack();
  std::vector<MaskEffect> oldRegion, oldMasks;
  for (auto& fx : layer.maskEffects) (isRegionEffect(fx) && oldMasks.empty() ? oldRegion : oldMasks).push_back(fx);
  std::vector<MaskEffect> stack;
  if (r.given) {
    if (r.slotsOnly) {
      for (size_t i = 0; i < r.slots.size(); ++i) {
        MaskEffect fx;
        fx.type = MaskEffect::Generator;
        fx.blend = i == 0 ? MaskEffect::Normal : MaskEffect::Lighten;
        fx.gen.type = MaskGenerator::MaterialSlot;
        fx.gen.slot = (int)r.slots[i];
        stack.push_back(fx);
      }
    } else {
      d.maskFromFaces(index, r.faces, "Mask from part");
      MaskEffect fx;
      fx.type = MaskEffect::Paint;
      stack.push_back(fx);
    }
  } else if (keepRegion) {
    stack = oldRegion;
  }
  if (masks.is_array() && !(keepMasks && masks.empty()))
    for (auto& m : masks) {
      stack.push_back(generatorEffect(m, stack.empty()));
      if (stack.back().type == MaskEffect::Pattern && !e.materials.find(stack.back().pattern.id))
        throw CommandError("no material '" + stack.back().pattern.id + "' in the library (see list_materials)");
    }
  else if (keepMasks)
    for (auto fx : oldMasks) {
      if (stack.empty() && fx.blend == MaskEffect::Multiply) fx.blend = MaskEffect::Normal;
      else if (!stack.empty() && fx.blend == MaskEffect::Normal) fx.blend = MaskEffect::Multiply;
      stack.push_back(fx);
    }
  if (stack.empty() && !keepRegion && !keepMasks) return;
  Layer& l = *d.layers[index];
  l.maskEffects = stack;
  d.ensureBakedFor(l);
  d.markComposite();
}

}  // namespace patina::regions
