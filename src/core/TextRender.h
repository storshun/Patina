#pragma once
#include <string>
#include <vector>

#include "core/Image.h"
#include "core/Math.h"

namespace patina {

// Text rendered into an RGBA image (straight alpha, row 0 = top) for the text tool:
// the image becomes the stencil, so text is placed and painted like any stencil.
struct TextStyle {
  std::string font = "sans";  // bundled name (sans, sans-bold, serif, mono) or a .ttf / .otf path
  float size = 128.f;         // pixel height of a line
  vec3 color{1.f};            // sRGB
  float outline = 0.f;        // outline width in pixels (0 = none)
  vec3 outlineColor{0.f};
  int align = 1;              // 0 left, 1 center, 2 right
  float lineSpacing = 1.15f;  // times the font's line height
  float letterSpacing = 0.f;  // extra pixels between letters
};

struct FontInfo {
  std::string name;  // what TextStyle::font accepts
  std::string path;
  bool bundled = false;
};

// Bundled fonts first, then common system fonts that exist on this machine.
std::vector<FontInfo> availableFonts();
std::string resolveFont(const std::string& nameOrPath);  // "" if not found

bool renderText(const std::string& utf8, const TextStyle& style, Image& out, std::string* err = nullptr);

// (0.36) Whether a font file can be used for the interface (it parses: TrueType, or OpenType with CFF outlines,
// not CFF2), and how many of `codepoints` it has a glyph for. -1: it can't be used.
int fontCoverage(const std::string& path, const std::vector<uint32_t>& codepoints, int index = 0,
                 std::vector<uint8_t>* has = nullptr);
std::vector<uint32_t> utf8Codepoints(const std::string& utf8);  // each character once, sorted

}  // namespace patina
