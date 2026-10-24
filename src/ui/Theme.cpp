#include "ui/Theme.h"

#include <algorithm>

namespace patina::ui {

namespace {

constexpr ImVec4 rgb(int r, int g, int b, float a = 1.f) { return ImVec4(r / 255.f, g / 255.f, b / 255.f, a); }
constexpr ImVec4 kDefault(0.f, 0.f, 0.f, -1.f);

const Theme kThemes[] = {
    {
        "patina",
        /* menuBar */ rgb(26, 27, 29), /* window */ rgb(33, 34, 37), /* child */ rgb(30, 31, 34), /* popup */ rgb(28, 29, 32, 0.98f),
        /* border */ rgb(52, 54, 58), /* frame */ rgb(46, 48, 52), rgb(58, 60, 66), rgb(66, 68, 74),
        /* button */ rgb(52, 54, 59), rgb(68, 70, 77), /* header */ rgb(60, 62, 68), rgb(72, 74, 81),
        /* tab */ rgb(36, 37, 40), rgb(72, 74, 81), rgb(50, 52, 57), rgb(30, 31, 34), rgb(42, 44, 48),
        /* scrollbar */ rgb(28, 29, 31), kDefault, /* text */ kDefault, kDefault,
        /* accent */ rgb(232, 133, 44), rgb(200, 112, 36), rgb(88, 62, 40), rgb(245, 160, 76),
        /* onAccent */ rgb(255, 246, 234), rgb(58, 34, 16), /* iconBase */ rgb(216, 212, 204),
        /* plate */ rgb(38, 38, 42, 0.94f), /* bar */ rgb(22, 22, 25), /* uvBackdrop */ ImVec4(0.075f, 0.078f, 0.085f, 1.f), ImVec4(0.9f, 0.6f, 0.2f, 1.f), 3.f,
    },
    {
        // VividCyanDarkColorscheme (Window, View, Button, Selection and DecorationFocus), as the Blender theme has it
        "vivid_dark",
        /* menuBar */ rgb(18, 26, 40), /* window */ rgb(24, 32, 48), /* child */ rgb(21, 29, 43), /* popup */ rgb(22, 30, 46, 0.98f),
        /* border */ rgb(42, 53, 80), /* frame */ rgb(34, 44, 68), rgb(44, 56, 84), rgb(51, 64, 95),
        /* button */ rgb(38, 49, 75), rgb(47, 60, 92), /* header */ rgb(38, 49, 75), rgb(47, 60, 92),
        /* tab */ rgb(20, 27, 41), rgb(47, 60, 92), rgb(31, 40, 64), rgb(18, 26, 40), rgb(26, 35, 53),
        /* scrollbar */ rgb(20, 27, 41), rgb(51, 64, 95), /* text */ rgb(211, 218, 227), rgb(139, 145, 157),
        /* accent */ rgb(0, 161, 197), rgb(0, 124, 152), rgb(18, 56, 74), rgb(95, 212, 238),
        /* onAccent */ rgb(255, 255, 255), rgb(6, 28, 38), /* iconBase */ rgb(211, 218, 227),
        /* plate */ rgb(28, 37, 56, 0.94f), /* bar */ rgb(14, 21, 32), /* uvBackdrop */ rgb(15, 21, 33), rgb(255, 185, 138), 5.f,
    },
};
static_assert(sizeof kThemes / sizeof kThemes[0] == (size_t)ThemeId::Count);

ThemeId current = ThemeId::Patina;

}  // namespace

const Theme& theme() { return kThemes[(int)current]; }
ThemeId themeId() { return current; }
void setTheme(ThemeId id) { current = id < ThemeId::Count ? id : ThemeId::Patina; }

ThemeId themeFromId(const std::string& id) {
  for (int i = 0; i < (int)ThemeId::Count; ++i)
    if (id == kThemes[i].id) return (ThemeId)i;
  return ThemeId::Patina;
}

ImU32 themeColor(const ImVec4& c, float alpha) {
  return ImGui::ColorConvertFloat4ToU32(ImVec4(c.x, c.y, c.z, std::clamp(c.w * alpha, 0.f, 1.f)));
}

}  // namespace patina::ui
