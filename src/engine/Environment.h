#pragma once
#include <string>

#include "gfx/Texture.h"

namespace patina {

// Image-based lighting: prefiltered specular cube, SH9 irradiance, BRDF LUT.
class Environment {
 public:
  bool initStudio();                          // procedural studio lighting
  bool loadHDR(const std::string& equirectPath);
  void bind(const class Shader& s, int unitSpec, int unitLut) const;  // sets uEnvSpec, uBrdfLut, uSH, uEnvMips

  GLuint specular() const { return spec_.id(); }
  GLuint brdfLut() const { return lut_.id(); }
  const vec4* sh() const { return sh_; }
  float mipCount() const { return float(spec_.levels() - 1); }
  float intensity = 1.f;
  float rotation = 0.f;  // radians around Y
  float exposure = 1.f;
  std::string name = "Studio";
  mat3 rotationMatrix() const;
  uint64_t serial() const { return serial_; }  // (0.39) changes with every new environment (the path tracer's CDFs)

 private:
  bool buildFromCube(TextureCube& source);
  void computeSH(TextureCube& source);
  void ensureLut();
  TextureCube spec_;
  Texture2D lut_;
  vec4 sh_[9] = {};
  uint64_t serial_ = 0;
};

}  // namespace patina
