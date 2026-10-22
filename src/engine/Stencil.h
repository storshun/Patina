#pragma once
#include <string>

#include "core/Image.h"
#include "core/Math.h"
#include "core/TextRender.h"
#include "gfx/Texture.h"
#include <nlohmann/json.hpp>

namespace patina {

// A picture laid over the 3D view that strokes paint through (like a physical stencil),
// or whose colors they paint (a decal). It stays fixed on the screen while the camera
// moves, so it can be lined up with the model before painting. Text becomes a stencil too.
struct Stencil {
  enum Mode { Mask = 0, Decal = 1 };
  bool enabled = false;
  int mode = Mask;
  bool invert = false;
  bool tile = false;
  vec2 center{0.5f};          // in the viewport, fractions (0,0 = top left)
  float scale = 0.6f;         // height as a fraction of the viewport height
  float rotationDeg = 0.f;
  float overlayOpacity = 0.4f;
  bool showOverlay = true;
  std::string source;         // the image file, empty for text
  std::string text;           // set when the stencil was made from text
  TextStyle textStyle;
  Image image;                // RGBA, row 0 = top
  Texture2D tex;

  bool valid() const { return tex.valid() && image.width > 0; }
  float aspect() const { return image.height > 0 ? float(image.width) / float(image.height) : 1.f; }
  bool loadFile(const std::string& path, std::string* err = nullptr);
  bool setText(const std::string& text, const TextStyle& style, std::string* err = nullptr);
  bool setImage(Image img);  // takes RGBA (any size; scaled down past 4096)
  void clear();
  // viewport pixels (origin top left) -> stencil coordinates (0..1 inside, v down)
  glm::mat3 screenToStencil(vec2 viewport) const;
  // corners in viewport pixels: top left, top right, bottom right, bottom left
  void corners(vec2 viewport, vec2 out[4]) const;
  // fit the whole stencil into the view (keeps rotation)
  void fit();
  nlohmann::json toJson() const;
};

}  // namespace patina
