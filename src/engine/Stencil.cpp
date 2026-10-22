#include "engine/Stencil.h"

#include <cmath>

namespace patina {

bool Stencil::setImage(Image img) {
  if (img.width <= 0 || img.height <= 0) return false;
  if (img.channels != 4) {
    Image rgba;
    rgba.resize(img.width, img.height, 4);
    for (size_t i = 0; i < size_t(img.width) * img.height; ++i) {
      const float* s = &img.pixels[i * img.channels];
      float* d = &rgba.pixels[i * 4];
      if (img.channels == 1) d[0] = d[1] = d[2] = s[0], d[3] = 1.f;
      else if (img.channels == 2) d[0] = d[1] = d[2] = s[0], d[3] = s[1];
      else d[0] = s[0], d[1] = s[1], d[2] = s[2], d[3] = 1.f;
    }
    img = std::move(rgba);
  }
  // keep GPU memory reasonable: halve (box filter) until it fits 4096
  while (std::max(img.width, img.height) > 4096) {
    Image h;
    h.resize(std::max(1, img.width / 2), std::max(1, img.height / 2), 4);
    for (int y = 0; y < h.height; ++y)
      for (int x = 0; x < h.width; ++x)
        for (int c = 0; c < 4; ++c) {
          float s = 0;
          for (int k = 0; k < 4; ++k)
            s += img.at(std::min(img.width - 1, 2 * x + (k & 1)), std::min(img.height - 1, 2 * y + (k >> 1)))[c];
          h.at(x, y)[c] = s * 0.25f;
        }
    img = std::move(h);
  }
  for (float& v : img.pixels) v = std::clamp(v, 0.f, 1.f);
  image = std::move(img);
  tex.create(image.width, image.height, GL_RGBA8, 0);
  tex.setFilter(GL_LINEAR_MIPMAP_LINEAR, GL_LINEAR);
  tex.setWrap(GL_REPEAT, GL_REPEAT);
  tex.upload(image);
  return true;
}

bool Stencil::loadFile(const std::string& path, std::string* err) {
  Image img;
  if (!loadImage(path, img)) {
    if (err) *err = "cannot read image " + path;
    return false;
  }
  if (!setImage(std::move(img))) {
    if (err) *err = "empty image " + path;
    return false;
  }
  source = path;
  text.clear();
  return true;
}

bool Stencil::setText(const std::string& t, const TextStyle& style, std::string* err) {
  Image img;
  if (!renderText(t, style, img, err)) return false;
  if (!setImage(std::move(img))) return false;
  text = t;
  textStyle = style;
  source.clear();
  return true;
}

void Stencil::clear() {
  image = Image();
  tex = Texture2D();
  source.clear();
  text.clear();
  enabled = false;
}

glm::mat3 Stencil::screenToStencil(vec2 vp) const {
  float h = std::max(scale * vp.y, 1e-3f), w = std::max(h * aspect(), 1e-3f);
  vec2 c = center * vp;
  float a = glm::radians(rotationDeg), cs = std::cos(a), sn = std::sin(a);
  // local = R(-a) (s - c): x' = cs*dx + sn*dy, y' = -sn*dx + cs*dy; then / size + 0.5
  glm::mat3 m(1.f);
  m[0] = vec3(cs / w, -sn / h, 0);
  m[1] = vec3(sn / w, cs / h, 0);
  m[2] = vec3((-cs * c.x - sn * c.y) / w + 0.5f, (sn * c.x - cs * c.y) / h + 0.5f, 1);
  return m;
}

void Stencil::corners(vec2 vp, vec2 out[4]) const {
  float h = scale * vp.y, w = h * aspect();
  vec2 c = center * vp;
  float a = glm::radians(rotationDeg), cs = std::cos(a), sn = std::sin(a);
  const vec2 local[4] = {{-0.5f, -0.5f}, {0.5f, -0.5f}, {0.5f, 0.5f}, {-0.5f, 0.5f}};
  for (int i = 0; i < 4; ++i) {
    vec2 l = local[i] * vec2(w, h);
    out[i] = c + vec2(cs * l.x - sn * l.y, sn * l.x + cs * l.y);
  }
}

void Stencil::fit() {
  center = vec2(0.5f);
  scale = 0.9f;
}

nlohmann::json Stencil::toJson() const {
  nlohmann::json j{{"enabled", enabled},
                   {"mode", mode == Decal ? "decal" : "mask"},
                   {"invert", invert},
                   {"tile", tile},
                   {"position", {center.x, center.y}},
                   {"scale", scale},
                   {"rotation", rotationDeg},
                   {"overlay_opacity", overlayOpacity},
                   {"show_overlay", showOverlay},
                   {"has_image", valid()},
                   {"size", {image.width, image.height}}};
  if (!source.empty()) j["path"] = source;
  if (!text.empty()) {
    j["text"] = text;
    j["font"] = textStyle.font;
    j["font_size"] = textStyle.size;
    j["color"] = {textStyle.color.x, textStyle.color.y, textStyle.color.z};
    j["outline"] = textStyle.outline;
    j["outline_color"] = {textStyle.outlineColor.x, textStyle.outlineColor.y, textStyle.outlineColor.z};
    j["align"] = textStyle.align == 0 ? "left" : textStyle.align == 2 ? "right" : "center";
  }
  return j;
}

}  // namespace patina
