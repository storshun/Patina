#pragma once
// (0.36) Toolbar icons: a small SVG reader for the line icons in icons/*.svg (24x24, strokes with round
// ends, a few fills). It turns each icon into polylines and fill outlines in icon units, ready for the UI
// to draw at any size. Colors are kept as roles so the UI can recolor an icon for its state:
// currentColor is the icon's own color, Patina's copper (#E8852C) is the accent, any other color stays.
// Supported: svg, g, path (M L H V C S Q T A Z, absolute and relative), circle, ellipse, rect (rx),
// line, polyline, polygon; fill, stroke, stroke-width, stroke-opacity, fill-opacity, opacity,
// stroke-dasharray and transform (translate rotate scale matrix). Anything else is reported.
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/Math.h"

namespace patina::vicon {

enum class Paint : uint8_t { None, Current, Accent, Literal };

struct Shape {
  Paint stroke = Paint::None, fill = Paint::None;
  uint32_t strokeRGB = 0, fillRGB = 0;  // 0xRRGGBB for Literal
  float strokeWidth = 1.f, strokeAlpha = 1.f, fillAlpha = 1.f;
  // stroke pieces: polylines broken at sharp corners and dashes, each drawn with round ends
  std::vector<std::vector<vec2>> pieces;
  std::vector<uint8_t> pieceClosed;   // a smooth closed loop (a circle): no ends
  std::vector<vec2> joints;           // corners and ends: a round dot of the stroke's width each
  // fills: closed outlines; convex ones are drawn as they are, others as triangles
  std::vector<std::vector<vec2>> fills;
  std::vector<uint8_t> fillConvex;
  std::vector<std::vector<uint16_t>> fillTris;  // (concave) indices into fills[i], three per triangle
};

struct Icon {
  std::vector<Shape> shapes;
  std::vector<std::string> problems;  // what was not understood (empty for a good icon)
};

Icon parse(std::string_view svg);

// the icons built in (icons/*.svg, embedded at build time)
struct Embedded {
  const char* name;
  const char* svg;
};
const std::vector<Embedded>& embedded();
// a built-in icon by name (parsed once); nullptr if there is none
const Icon* find(std::string_view name);

}  // namespace patina::vicon
