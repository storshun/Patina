#pragma once
#include <array>
#include <string>

#include "core/I18n.h"
#include "core/Math.h"
#include "gfx/GL.h"

namespace patina {

// Material channels painted by layers. 0.26 added opacity, sheen, clearcoat and subsurface (one value each;
// composited together into one RGBA "extra" map: r opacity, g sheen, b coat, a subsurface).
enum class Channel : int { Color = 0, Roughness, Metallic, Height, Normal, Emissive, Opacity, Sheen, Coat, Subsurface, Count };
constexpr int kChannelCount = (int)Channel::Count;

struct ChannelInfo {
  const char* key;       // "basecolor"
  const char* label;     // "Base Color"
  GLenum layerFormat;    // per-layer storage (value + coverage alpha)
  int bit;               // bit in channel masks
};

inline const ChannelInfo& channelInfo(Channel c) {
  static const std::array<ChannelInfo, kChannelCount> info = {{
      {"basecolor", N_("Base Color"), GL_RGBA8, 1},
      {"roughness", N_("Roughness"), GL_RG8, 2},
      {"metallic", N_("Metallic"), GL_RG8, 4},
      {"height", N_("Height"), GL_RG16F, 8},
      {"normal", N_("Normal"), GL_RGBA8, 16},
      {"emissive", N_("Emissive"), GL_RGBA8, 32},
      {"opacity", N_("Opacity"), GL_RG8, 64},
      {"sheen", N_("Sheen"), GL_RG8, 128},
      {"coat", N_("Clearcoat"), GL_RG8, 256},
      {"subsurface", N_("Subsurface"), GL_RG8, 512},
  }};
  return info[(int)c];
}

inline int channelFromKey(const std::string& k) {
  for (int i = 0; i < kChannelCount; ++i)
    if (k == channelInfo((Channel)i).key) return i;
  if (k == "color" || k == "albedo" || k == "base_color") return 0;
  if (k == "emission") return (int)Channel::Emissive;
  if (k == "alpha" || k == "transparency") return (int)Channel::Opacity;
  if (k == "clearcoat") return (int)Channel::Coat;
  if (k == "sss") return (int)Channel::Subsurface;
  return -1;
}

constexpr int kAllChannels = 1023;
constexpr int kOldAllChannels = 63;  // every channel before 0.26
constexpr int kExtraChannels = 64 | 128 | 256 | 512;
// The first channel kept in the "extra" map, and whether a channel is one of them.
constexpr int kFirstExtraChannel = (int)Channel::Opacity;
inline bool isExtraChannel(int c) { return c >= kFirstExtraChannel && c < kChannelCount; }
// Channel masks saved before 0.26 said "all" as 63; they mean every channel now.
inline int upgradeChannelMask(int m) { return m == kOldAllChannels ? kAllChannels : m; }

// Flat values a brush or flat material paints.
struct FlatValues {
  vec3 color{0.8f, 0.8f, 0.8f};  // sRGB
  float roughness = 0.5f;
  float metallic = 0.f;
  float height = 0.5f;
  vec3 emissive{0.f};  // sRGB
  // 0.26: 1 = opaque; sheen (velvet), clearcoat (lacquer) and subsurface (skin, wax) amounts
  float opacity = 1.f;
  float sheen = 0.f;
  float coat = 0.f;
  float subsurface = 0.f;
  vec4 extra() const { return vec4(opacity, sheen, coat, subsurface); }
};

}  // namespace patina
