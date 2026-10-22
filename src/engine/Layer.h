#pragma once
#include <array>
#include <memory>
#include <string>
#include <vector>

#include "engine/Channels.h"
#include "engine/MaterialLibrary.h"
#include "gfx/Texture.h"

namespace patina {

enum class LayerType { Paint = 0, Fill = 1, Adjust = 2, Folder = 3, Baked = 4 };

// (0.41) Baked mesh maps that have a layer of their own in the stack (Layer::bakedMap): the layer holds fixes painted
// over the bake and adjustments (its mask effects); what it makes is the map everything else reads (smart masks, the
// 3D view, exports). It adds nothing to the material channels.
enum class BakedMap { AO = 0, Curvature, Thickness, Count };
constexpr int kBakedMapCount = (int)BakedMap::Count;
const char* bakedMapKey(int k);    // "ao", "curvature", "thickness"
const char* bakedMapLabel(int k);  // "Ambient occlusion"... (N_, translate when shown)
int bakedMapFromKey(const std::string& s);  // -1: none
float bakedMapNeutral(int k);       // the value that changes nothing: AO 1, curvature 0.5, thickness 1

enum class BlendMode { Normal = 0, Multiply, Screen, Overlay, Add, Subtract, Darken, Lighten, AddSigned, SoftLight, Count };
const char* blendModeName(BlendMode m);
BlendMode blendModeFromName(const std::string& s);

// Procedural mask driven by baked mesh maps ("smart mask").
struct MaskGenerator {
  enum Type { None = 0, Edges, Cavity, Occlusion, GradientY, Noise, Facing, MaterialSlot, Thin, Gradient, Drips, Count };
  int type = None;
  float level = 0.5f;     // threshold
  float contrast = 0.6f;  // 0 soft .. 1 hard
  float noise = 0.f;      // noise breakup amount
  float noiseScale = 1.f;
  bool invert = false;
  vec3 direction{0, 1, 0};  // Facing: surfaces whose normal points this way; Gradient: dark to light this way
  int noiseType = 0;        // Noise: 0 clouds, 1 cells, 2 cracks, 3 ridges, 4 streaks
  int slot = 0;             // MaterialSlot: the material slot (Blender material index) to select
  json toJson() const;
  void fromJson(const json& j);
};
const char* maskGeneratorName(int t);

// One step of a layer's mask effect stack. The stack starts white; every enabled effect
// computes a value and blends it onto the result so far (or filters it), with its
// own opacity. A layer with a stack ignores its single `generator`.
struct MaskEffect {
  enum Type { Paint = 0, Generator, Fill, Noise, Levels, Blur, Invert, Grow, Edges, Pattern, Count };
  enum Blend { Normal = 0, Multiply, Add, Subtract, Lighten, Darken, Screen, BlendCount };
  int type = Paint;
  bool enabled = true;
  int blend = Normal;
  float opacity = 1.f;
  MaskGenerator gen;        // Generator
  float value = 1.f;        // Fill
  float scale = 1.f;        // Noise: size (relative to the object)
  float seed = 0.f;         // Noise
  float contrast = 0.5f;    // Noise: 0 soft .. 1 hard
  float inLow = 0.f, inHigh = 1.f, gamma = 1.f, outLow = 0.f, outHigh = 1.f;  // Levels
  float radius = 4.f;       // Blur, Edges: texels at 1K (scaled with the resolution)
  int noiseType = 0;        // Noise: 0 clouds, 1 cells, 2 cracks, 3 ridges, 4 streaks
  float amount = 3.f;       // Grow: texels at 1K, more than 0 grows, less shrinks
  MaterialRef pattern;      // Pattern: a library material, projected like a fill layer's
  int source = 0;           // Pattern: 0 height, 1 brightness, 2 roughness, 3 metallic
  float level = 0.5f;       // Pattern: threshold (with contrast; contrast 0 keeps the values as they are)
  bool invert = false;      // Pattern
  bool isFilter() const { return type == Levels || type == Blur || type == Invert || type == Grow || type == Edges; }
  json toJson() const;
  void fromJson(const json& j);
};
const char* maskEffectName(int t);
const char* maskNoiseTypeName(int t);
int maskNoiseTypeFromName(const std::string& s);
const char* maskBlendName(int b);
int maskEffectFromName(const std::string& s);
int maskBlendFromName(const std::string& s);

// Adjustment applied to everything below the layer (through its mask/opacity).
struct AdjustParams {
  float hue = 0.f;         // -0.5 .. 0.5 (turns)
  float saturation = 1.f;  // multiplier
  float brightness = 1.f;  // multiplier
  float contrast = 1.f;    // around mid gray
  float roughness = 0.f;   // offsets
  float metallic = 0.f;
  float height = 0.f;
  // (0.33) develop, as a photo editor's basic controls: in linear light first, then the settings above. All 0 = no change.
  float exposure = 0.f;     // stops
  float temperature = 0.f;  // -1 cooler (blue) .. 1 warmer (yellow)
  float tint = 0.f;         // -1 green .. 1 magenta
  float highlights = 0.f;   // -1 .. 1: the bright parts down or up
  float shadows = 0.f;      // -1 .. 1: the dark parts down or up
  float vibrance = 0.f;     // -1 .. 1: saturation, mostly of the dull colors
  float delight = 0.f;      // 0 .. 1: takes shadows baked into the color out, using the ambient occlusion
  bool develops() const {
    return exposure != 0.f || temperature != 0.f || tint != 0.f || highlights != 0.f || shadows != 0.f ||
           vibrance != 0.f || delight != 0.f;
  }
  json toJson() const;
  void fromJson(const json& j);
};

struct Layer {
  uint32_t uid = 0;
  std::string name = "Layer";
  LayerType type = LayerType::Paint;
  bool visible = true;
  float opacity = 1.f;
  BlendMode blend = BlendMode::Normal;
  int channelMask = kAllChannels;
  std::array<float, kChannelCount> channelOpacity{1, 1, 1, 1, 1, 1, 1, 1, 1, 1};

  // Paint layers: per-channel textures (value + coverage alpha), allocated on first use.
  std::array<std::unique_ptr<Texture2D>, kChannelCount> channels;
  // Fill layers
  MaterialRef material;
  // Adjustment layers
  AdjustParams adjust;
  // Optional painted mask (R8). Null = fully opaque.
  std::unique_ptr<Texture2D> mask;
  MaskGenerator generator;
  std::vector<MaskEffect> maskEffects;  // empty: mask = painted mask x generator (the simple form)
  // Turns the simple form into a stack: [Paint, Generator (multiply)] with the same result.
  void convertMaskToStack();
  // Mesh maps the mask needs (its generator and every generator effect of the stack)
  bool needsCurvature() const;
  bool needsAO() const;
  bool needsThickness() const;
  // Folders: children name their folder in `parent` (0 = top level). Sibling order is
  // the order in the flat layer vector. A folder composites its children on top of
  // what lies below, then blends the result in through its own mask / opacity.
  uint32_t parent = 0;
  int colorTag = 0;       // 0 none, 1..7 swatch colors in the layer list
  // Instances show another layer's content (paint, fill material or adjustment) with
  // their own opacity, blend mode, channels and mask. Painting an instance paints the
  // source; every instance follows.
  uint32_t instanceOf = 0;
  bool expanded = true;   // folder open in the layer list (UI state, not undone)
  // (0.41) baked map layers: which map, and how strong it is in the end (0 = the neutral value, 1 = as it is, up to
  // 2). The fixes painted over the bake are `mask` (RG8: the value painted, how much of it covers the bake); the
  // mask effects adjust the bake with the fixes, from the bottom up. Opacity and the eye: how much of all that applies
  // (hidden = the map as it was baked).
  int bakedMap = 0;
  float bakedStrength = 1.f;
  bool isBaked() const { return type == LayerType::Baked; }

  bool masksOnly() const { return type != LayerType::Paint; }  // painting edits the mask (a baked map: its fixes)
  bool isFolder() const { return type == LayerType::Folder; }
  bool hasChannel(int c) const { return channels[c] && channels[c]->valid(); }
  Texture2D& ensureChannel(int c, ivec2 res);
  Texture2D& ensureMask(ivec2 res, float initValue = 1.f);  // (a baked map layer: its fixes, none yet)
  void clearChannel(int c);
  size_t gpuBytes() const;

  json toJson() const;  // metadata only (textures are serialized by the project writer)
  void fromJson(const json& j);
};

}  // namespace patina
