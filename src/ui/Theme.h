#pragma once
// (1.2.7) The interface's colors: Patina's own, or Vivid Dark (KDE Plasma's Vivid-Dark-Global-6, the palette of the
// "Vivid Dark" Blender theme too). Chosen in Edit > Preferences > Display, kept in session.json. setupStyle makes the
// style from it; what panels draw by hand (the chosen tool, icons, pies, drop marks) asks it for the accent.

#include <imgui.h>

#include <string>

namespace patina::ui {

enum class ThemeId { Patina = 0, VividDark, Count };

struct Theme {
  const char* id;  // in session.json
  // the style's colors; w < 0: ImGui's dark default
  ImVec4 menuBar, window, child, popup, border, frame, frameHovered, frameActive, button, buttonHovered, header,
      headerHovered, tab, tabHovered, tabSelected, tabDimmed, tabDimmedSelected, scrollbarBg, scrollbarGrab, text,
      textDisabled;
  ImVec4 accent;       // focus, check marks, highlights drawn by hand (Patina: copper)
  ImVec4 accentDeep;   // the chosen tool's button, slider grabs, a pressed button (light text on it)
  ImVec4 accentDim;    // a toggle that is on, the pie item that is on
  ImVec4 accentLight;  // an icon's accent on a toggle that is on
  ImVec4 onAccent;     // text and icons on accentDeep
  ImVec4 onAccentMark; // an icon's accent on accentDeep
  ImVec4 iconBase;     // an icon's own color
  ImVec4 plate;        // dark plates drawn over the 3D view (pie items, labels)
  ImVec4 bar;          // the Guided layout's step bar
  ImVec4 uvBackdrop;   // the UV editor round the texture
  ImVec4 uvBorder;     // ... and the texture's border
  float rounding;      // frames, tabs, grabs, at UI scale 1 (windows and popups a little more)
};

const Theme& theme();
ThemeId themeId();
void setTheme(ThemeId id);
ThemeId themeFromId(const std::string& id);  // (unknown: Patina)
ImU32 themeColor(const ImVec4& c, float alpha = 1.f);  // c, its alpha times `alpha`

}  // namespace patina::ui
