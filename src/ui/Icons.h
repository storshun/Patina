#pragma once
// (0.36) Drawing the toolbar icons (icons/*.svg, read by core/VectorIcon) with ImGui.
#include <imgui.h>

namespace patina::ui {

enum class IconState { Normal, Chosen, On, Disabled };  // Chosen: the tool in use; On: a switch that is on

struct IconColors {
  ImU32 base, accent;  // currentColor and the copper accent
  float alpha = 1.f;   // (other colors, such as the axis letters, keep their color)
};
IconColors iconColors(IconState s);

// draws icons/<name>.svg with its top-left corner at `pos`, `size` pixels square; false if there is no such icon
bool drawIcon(ImDrawList* dl, const char* name, ImVec2 pos, float size, const IconColors& c);
// an icon in the layout, like ImGui::Image
void icon(const char* name, float size, IconState s = IconState::Normal);

}  // namespace patina::ui
