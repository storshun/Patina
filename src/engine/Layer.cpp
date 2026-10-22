#include "engine/Layer.h"

namespace patina {

static const char* kBlendNames[] = {"normal", "multiply", "screen", "overlay", "add",
                                    "subtract", "darken", "lighten", "add_signed", "soft_light"};

const char* blendModeName(BlendMode m) { return kBlendNames[std::clamp((int)m, 0, (int)BlendMode::Count - 1)]; }

BlendMode blendModeFromName(const std::string& s) {
  for (int i = 0; i < (int)BlendMode::Count; ++i)
    if (s == kBlendNames[i]) return (BlendMode)i;
  return BlendMode::Normal;
}

static const char* kGenNames[] = {"none",   "edges", "cavity", "occlusion", "gradient_y", "noise",
                                  "facing", "material", "thin", "gradient", "drips"};
static const char* kNoiseTypes[] = {"clouds", "cells", "cracks", "ridges", "streaks"};
const char* maskNoiseTypeName(int t) { return kNoiseTypes[std::clamp(t, 0, 4)]; }
int maskNoiseTypeFromName(const std::string& s) {
  for (int i = 0; i < 5; ++i)
    if (s == kNoiseTypes[i]) return i;
  return 0;
}
static int noiseTypeFrom(const json& j, int d) {
  if (!j.contains("noise_type")) return d;
  if (j["noise_type"].is_number()) return std::clamp(j["noise_type"].get<int>(), 0, 4);
  return maskNoiseTypeFromName(j["noise_type"].get<std::string>());
}
static constexpr int kGenCount = MaskGenerator::Count;
const char* maskGeneratorName(int t) { return kGenNames[std::clamp(t, 0, kGenCount - 1)]; }

json MaskGenerator::toJson() const {
  json j = {{"type", kGenNames[std::clamp(type, 0, kGenCount - 1)]}, {"level", level}, {"contrast", contrast},
            {"noise", noise}, {"noise_scale", noiseScale}, {"invert", invert}};
  if (type == Facing || type == Gradient) j["direction"] = {direction.x, direction.y, direction.z};
  if (type == Noise) j["noise_type"] = maskNoiseTypeName(noiseType);
  if (type == MaterialSlot) j["slot"] = slot;
  return j;
}

void MaskGenerator::fromJson(const json& j) {
  if (j.contains("type")) {
    if (j["type"].is_number()) type = j["type"];
    else {
      std::string t = j["type"];
      for (int i = 0; i < kGenCount; ++i)
        if (t == kGenNames[i]) type = i;
    }
  }
  level = j.value("level", level);
  contrast = j.value("contrast", contrast);
  noise = j.value("noise", noise);
  noiseScale = j.value("noise_scale", noiseScale);
  invert = j.value("invert", invert);
  if (j.contains("direction") && j["direction"].is_array() && j["direction"].size() == 3) {
    vec3 d(j["direction"][0].get<float>(), j["direction"][1].get<float>(), j["direction"][2].get<float>());
    if (glm::length(d) > 1e-6f) direction = glm::normalize(d);
  }
  slot = j.value("slot", slot);
  noiseType = noiseTypeFrom(j, noiseType);
}

static const char* kEffNames[] = {"paint", "generator", "fill", "noise", "levels", "blur", "invert", "grow", "edges", "pattern"};
static const char* kPatSources[] = {"height", "brightness", "roughness", "metallic"};
static const char* kEffBlend[] = {"normal", "multiply", "add", "subtract", "lighten", "darken", "screen"};
const char* maskEffectName(int t) { return kEffNames[std::clamp(t, 0, (int)MaskEffect::Count - 1)]; }
const char* maskBlendName(int b) { return kEffBlend[std::clamp(b, 0, (int)MaskEffect::BlendCount - 1)]; }
int maskEffectFromName(const std::string& s) {
  for (int i = 0; i < MaskEffect::Count; ++i)
    if (s == kEffNames[i]) return i;
  return -1;
}
int maskBlendFromName(const std::string& s) {
  for (int i = 0; i < MaskEffect::BlendCount; ++i)
    if (s == kEffBlend[i]) return i;
  return 0;
}

json MaskEffect::toJson() const {
  json j = {{"type", maskEffectName(type)}, {"enabled", enabled}, {"blend", maskBlendName(blend)}, {"opacity", opacity}};
  switch (type) {
    case Generator: j["generator"] = gen.toJson(); break;
    case Fill: j["value"] = value; break;
    case Noise:
      j["scale"] = scale, j["seed"] = seed, j["contrast"] = contrast, j["noise_type"] = maskNoiseTypeName(noiseType);
      break;
    case Grow: j["amount"] = amount; break;
    case Edges: j["radius"] = radius; break;
    case Pattern:
      j["material"] = pattern.toJson(), j["source"] = kPatSources[std::clamp(source, 0, 3)], j["level"] = level;
      j["contrast"] = contrast, j["invert"] = invert;
      break;
    case Levels:
      j["in_low"] = inLow, j["in_high"] = inHigh, j["gamma"] = gamma, j["out_low"] = outLow, j["out_high"] = outHigh;
      break;
    case Blur: j["radius"] = radius; break;
    default: break;
  }
  return j;
}

void MaskEffect::fromJson(const json& j) {
  if (j.contains("type")) {
    if (j["type"].is_number()) type = std::clamp(j["type"].get<int>(), 0, (int)Count - 1);
    else if (int t = maskEffectFromName(j["type"].get<std::string>()); t >= 0) type = t;
  }
  enabled = j.value("enabled", enabled);
  if (j.contains("blend")) blend = j["blend"].is_number() ? std::clamp(j["blend"].get<int>(), 0, (int)BlendCount - 1)
                                                          : maskBlendFromName(j["blend"].get<std::string>());
  opacity = std::clamp(j.value("opacity", opacity), 0.f, 1.f);
  if (j.contains("generator") && j["generator"].is_object()) gen.fromJson(j["generator"]);
  value = j.value("value", value);
  scale = j.value("scale", scale);
  seed = j.value("seed", seed);
  contrast = j.value("contrast", contrast);
  inLow = j.value("in_low", inLow);
  inHigh = j.value("in_high", inHigh);
  gamma = std::max(0.01f, j.value("gamma", gamma));
  outLow = j.value("out_low", outLow);
  outHigh = j.value("out_high", outHigh);
  radius = std::clamp(j.value("radius", radius), 0.f, 256.f);
  noiseType = noiseTypeFrom(j, noiseType);
  amount = std::clamp(j.value("amount", amount), -128.f, 128.f);
  if (j.contains("material") && j["material"].is_object()) pattern.fromJson(j["material"]);
  else if (j.contains("material") && j["material"].is_string()) pattern.id = j["material"].get<std::string>();
  if (j.contains("source")) {
    if (j["source"].is_number()) source = std::clamp(j["source"].get<int>(), 0, 3);
    else
      for (int i = 0; i < 4; ++i)
        if (j["source"].get<std::string>() == kPatSources[i]) source = i;
  }
  level = j.value("level", level);
  invert = j.value("invert", invert);
}

void Layer::convertMaskToStack() {
  if (!maskEffects.empty() || isBaked()) return;  // (a baked map layer's stack starts from the bake with its fixes)
  MaskEffect paint;
  paint.type = MaskEffect::Paint;
  maskEffects.push_back(paint);
  if (generator.type != MaskGenerator::None) {
    MaskEffect g;
    g.type = MaskEffect::Generator;
    g.blend = MaskEffect::Multiply;
    g.gen = generator;
    maskEffects.push_back(g);
    generator.type = MaskGenerator::None;
  }
}

static bool genNeedsCurv(int g) { return g == MaskGenerator::Edges || g == MaskGenerator::Cavity; }
static bool genNeedsAO(int g) { return g == MaskGenerator::Cavity || g == MaskGenerator::Occlusion; }
bool Layer::needsCurvature() const {
  if (genNeedsCurv(generator.type)) return true;
  for (auto& e : maskEffects)
    if (e.enabled && e.type == MaskEffect::Generator && genNeedsCurv(e.gen.type)) return true;
  return false;
}
bool Layer::needsAO() const {
  if (genNeedsAO(generator.type)) return true;
  if (type == LayerType::Adjust && adjust.delight > 0.f) return true;
  for (auto& e : maskEffects)
    if (e.enabled && e.type == MaskEffect::Generator && genNeedsAO(e.gen.type)) return true;
  return false;
}

bool Layer::needsThickness() const {
  if (generator.type == MaskGenerator::Thin) return true;
  for (auto& e : maskEffects)
    if (e.enabled && e.type == MaskEffect::Generator && e.gen.type == MaskGenerator::Thin) return true;
  return false;
}

json AdjustParams::toJson() const {
  return {{"hue", hue}, {"saturation", saturation}, {"brightness", brightness}, {"contrast", contrast},
          {"roughness", roughness}, {"metallic", metallic}, {"height", height}, {"exposure", exposure},
          {"temperature", temperature}, {"tint", tint}, {"highlights", highlights}, {"shadows", shadows},
          {"vibrance", vibrance}, {"delight", delight}};
}

void AdjustParams::fromJson(const json& j) {
  hue = j.value("hue", hue);
  saturation = j.value("saturation", saturation);
  brightness = j.value("brightness", brightness);
  contrast = j.value("contrast", contrast);
  roughness = j.value("roughness", roughness);
  metallic = j.value("metallic", metallic);
  height = j.value("height", height);
  exposure = std::clamp(j.value("exposure", exposure), -5.f, 5.f);
  temperature = std::clamp(j.value("temperature", temperature), -1.f, 1.f);
  tint = std::clamp(j.value("tint", tint), -1.f, 1.f);
  highlights = std::clamp(j.value("highlights", highlights), -1.f, 1.f);
  shadows = std::clamp(j.value("shadows", shadows), -1.f, 1.f);
  vibrance = std::clamp(j.value("vibrance", vibrance), -1.f, 1.f);
  delight = std::clamp(j.value("delight", delight), 0.f, 1.f);
}

Texture2D& Layer::ensureChannel(int c, ivec2 res) {
  auto& t = channels[c];
  if (!t || !t->valid() || t->size() != res) {
    t = std::make_unique<Texture2D>(res.x, res.y, channelInfo((Channel)c).layerFormat, 1);
    t->clear(vec4(0));
  }
  return *t;
}

Texture2D& Layer::ensureMask(ivec2 res, float v) {
  GLenum fmt = isBaked() ? GL_RG8 : GL_R8;
  if (!mask || !mask->valid() || mask->size() != res || mask->format() != fmt) {
    mask = std::make_unique<Texture2D>(res.x, res.y, fmt, 1);
    mask->clear(isBaked() ? vec4(0) : vec4(v));  // (fixes: none painted yet)
  }
  return *mask;
}

static const char* kBakedKeys[] = {"ao", "curvature", "thickness"};
static const char* kBakedLabels[] = {N_("Ambient occlusion"), N_("Curvature"), N_("Thickness")};
static_assert(sizeof(kBakedKeys) / sizeof(kBakedKeys[0]) == kBakedMapCount);
const char* bakedMapKey(int k) { return kBakedKeys[std::clamp(k, 0, kBakedMapCount - 1)]; }
const char* bakedMapLabel(int k) { return kBakedLabels[std::clamp(k, 0, kBakedMapCount - 1)]; }
int bakedMapFromKey(const std::string& s) {
  for (int k = 0; k < kBakedMapCount; ++k)
    if (s == kBakedKeys[k]) return k;
  if (s == "occlusion" || s == "ambient_occlusion") return 0;
  return -1;
}
float bakedMapNeutral(int k) { return k == (int)BakedMap::Curvature ? 0.5f : 1.f; }

void Layer::clearChannel(int c) {
  if (channels[c]) channels[c]->clear(vec4(0));
}

size_t Layer::gpuBytes() const {
  size_t b = 0;
  static const int bpp[] = {4, 2, 2, 4, 4, 4, 2, 2, 2, 2};
  static_assert(sizeof(bpp) / sizeof(bpp[0]) == kChannelCount);
  for (int c = 0; c < kChannelCount; ++c)
    if (hasChannel(c)) b += size_t(channels[c]->width()) * channels[c]->height() * bpp[c];
  if (mask) b += size_t(mask->width()) * mask->height() * (isBaked() ? 2 : 1);
  return b;
}

json Layer::toJson() const {
  json co = json::array();
  for (float v : channelOpacity) co.push_back(v);
  json j = {{"uid", uid},
            {"name", name},
            {"type", type == LayerType::Paint ? "paint" : type == LayerType::Fill ? "fill"
                     : type == LayerType::Folder ? "folder" : type == LayerType::Baked ? "baked" : "adjust"},
            {"parent", parent},
            {"color_tag", colorTag},
            {"expanded", expanded},
            {"visible", visible},
            {"opacity", opacity},
            {"blend", blendModeName(blend)},
            {"channel_mask", channelMask},
            {"channels_v", 2},  // 0.26: the mask counts ten channels (an older 63 meant all of them)
            {"channel_opacity", co},
            {"generator", generator.toJson()},
            {"has_mask", (bool)mask}};
  if (instanceOf) j["instance_of"] = instanceOf;
  if (!maskEffects.empty()) {
    json fx = json::array();
    for (auto& e : maskEffects) fx.push_back(e.toJson());
    j["mask_effects"] = fx;
  }
  if (type == LayerType::Fill) j["material"] = material.toJson();
  if (type == LayerType::Adjust) j["adjust"] = adjust.toJson();
  if (type == LayerType::Baked) j["baked_map"] = bakedMapKey(bakedMap), j["strength"] = bakedStrength;
  return j;
}

void Layer::fromJson(const json& j) {
  uid = j.value("uid", uid);
  name = j.value("name", name);
  if (j.contains("type"))
    type = j["type"] == "fill"     ? LayerType::Fill
         : j["type"] == "adjust" ? LayerType::Adjust
         : j["type"] == "folder" ? LayerType::Folder
         : j["type"] == "baked"  ? LayerType::Baked
                                 : LayerType::Paint;
  parent = j.value("parent", parent);
  colorTag = j.value("color_tag", colorTag);
  expanded = j.value("expanded", expanded);
  visible = j.value("visible", visible);
  opacity = j.value("opacity", opacity);
  if (j.contains("blend")) blend = blendModeFromName(j["blend"]);
  channelMask = j.value("channel_mask", channelMask);
  if (j.contains("channel_mask") && j.value("channels_v", 1) < 2) channelMask = upgradeChannelMask(channelMask);
  if (j.contains("channel_opacity"))
    for (size_t i = 0; i < std::min<size_t>(kChannelCount, j["channel_opacity"].size()); ++i)
      channelOpacity[i] = j["channel_opacity"][i];
  if (j.contains("generator")) generator.fromJson(j["generator"]);
  instanceOf = j.value("instance_of", 0u);
  // the stack is part of the layer's state: absent means none (undo restores "no stack")
  maskEffects.clear();
  if (j.contains("mask_effects") && j["mask_effects"].is_array())
    for (auto& e : j["mask_effects"]) {
      MaskEffect fx;
      fx.fromJson(e);
      maskEffects.push_back(fx);
    }
  if (j.contains("material")) material.fromJson(j["material"]);
  if (j.contains("adjust")) adjust.fromJson(j["adjust"]);
  if (j.contains("baked_map")) bakedMap = std::max(0, bakedMapFromKey(j.value("baked_map", std::string("ao"))));
  bakedStrength = std::clamp(j.value("strength", isBaked() ? bakedStrength : 1.f), 0.f, 2.f);
}

}  // namespace patina
