#include "ui/Icons.h"

#include <imgui_internal.h>

#include <algorithm>
#include <vector>

#include "core/VectorIcon.h"
#include "ui/Theme.h"

namespace patina::ui {

static ImU32 withAlpha(ImU32 c, float a) {
  int al = int(((c >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(a, 0.f, 1.f) + 0.5f);
  return (c & ~IM_COL32_A_MASK) | (ImU32(al) << IM_COL32_A_SHIFT);
}

IconColors iconColors(IconState s) {
  const Theme& t = theme();  // (1.2.7) the accent is the theme's
  switch (s) {
    case IconState::Chosen: return {themeColor(t.onAccent), themeColor(t.onAccentMark)};
    case IconState::On: return {themeColor(t.onAccent), themeColor(t.accentLight)};
    case IconState::Disabled: return {themeColor(t.iconBase), themeColor(t.accent), 0.38f};
    default: return {themeColor(t.iconBase), themeColor(t.accent)};
  }
}

bool drawIcon(ImDrawList* dl, const char* name, ImVec2 pos, float size, const IconColors& c) {
  const vicon::Icon* ic = vicon::find(name);
  if (!ic) return false;
  const float s = size / 24.f;
  auto color = [&](vicon::Paint p, uint32_t rgb, float a) -> ImU32 {
    ImU32 col = p == vicon::Paint::Current  ? c.base
              : p == vicon::Paint::Accent   ? c.accent
                                            : IM_COL32(rgb >> 16 & 0xFF, rgb >> 8 & 0xFF, rgb & 0xFF, 255);
    return withAlpha(col, a * c.alpha);
  };
  std::vector<ImVec2> pts;
  auto map = [&](const std::vector<vec2>& in) {
    pts.resize(in.size());
    for (size_t i = 0; i < in.size(); ++i) pts[i] = ImVec2(pos.x + in[i].x * s, pos.y + in[i].y * s);
  };
  const ImVec2 uv = dl->_Data->TexUvWhitePixel;
  for (const auto& sh : ic->shapes) {
    if (sh.fill != vicon::Paint::None) {
      ImU32 fc = color(sh.fill, sh.fillRGB, sh.fillAlpha);
      for (size_t f = 0; f < sh.fills.size(); ++f) {
        map(sh.fills[f]);
        if (sh.fillConvex[f]) {
          dl->AddConvexPolyFilled(pts.data(), (int)pts.size(), fc);
        } else {
          const auto& tri = sh.fillTris[f];
          dl->PrimReserve((int)tri.size(), (int)tri.size());
          for (uint16_t k : tri) {
            dl->_IdxWritePtr[0] = (ImDrawIdx)dl->_VtxCurrentIdx;
            dl->_IdxWritePtr++;
            dl->PrimWriteVtx(pts[k], uv, fc);
          }
          // a thin line round it for a soft edge (when no stroke covers it)
          if (sh.stroke == vicon::Paint::None) dl->AddPolyline(pts.data(), (int)pts.size(), fc, ImDrawFlags_Closed, 1.f);
        }
      }
    }
    if (sh.stroke != vicon::Paint::None) {
      ImU32 sc = color(sh.stroke, sh.strokeRGB, sh.strokeAlpha);
      float w = std::max(0.75f, sh.strokeWidth * s);
      for (size_t p = 0; p < sh.pieces.size(); ++p) {
        map(sh.pieces[p]);
        dl->AddPolyline(pts.data(), (int)pts.size(), sc, sh.pieceClosed[p] ? ImDrawFlags_Closed : ImDrawFlags_None, w);
      }
      // round ends and corners
      if (w > 1.2f)
        for (const vec2& j : sh.joints) dl->AddCircleFilled(ImVec2(pos.x + j.x * s, pos.y + j.y * s), w * 0.5f, sc, 10);
    }
  }
  return true;
}

void icon(const char* name, float size, IconState s) {
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::Dummy(ImVec2(size, size));
  drawIcon(ImGui::GetWindowDrawList(), name, p, size, iconColors(s));
}

}  // namespace patina::ui
