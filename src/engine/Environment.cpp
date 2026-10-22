#include "engine/Environment.h"

#include "core/Image.h"
#include "core/Log.h"
#include "gfx/Shader.h"

namespace patina {

namespace {
constexpr int kSourceSize = 256;
constexpr int kSpecSize = 128;
constexpr int kSpecLevels = 6;

vec3 faceDir(int face, float u, float v) {
  float sc = u * 2 - 1, tc = v * 2 - 1;
  vec3 d;
  switch (face) {
    case 0: d = {1, -tc, -sc}; break;
    case 1: d = {-1, -tc, sc}; break;
    case 2: d = {sc, 1, tc}; break;
    case 3: d = {sc, -1, -tc}; break;
    case 4: d = {sc, -tc, 1}; break;
    default: d = {-sc, -tc, -1}; break;
  }
  return glm::normalize(d);
}

void renderFaces(TextureCube& cube, int level, Shader& s) {
  Framebuffer fbo;
  int size = std::max(1, cube.size() >> level);
  gl::ViewportScope vp(0, 0, size, size);
  for (int f = 0; f < 6; ++f) {
    fbo.attachLayer(0, cube.id(), level, f);
    fbo.setDrawBuffers(1);
    gl::FramebufferScope fs(fbo.id());
    s.use();
    s.set("uFace", f);
    drawFullscreenTriangle();
  }
}
}  // namespace

mat3 Environment::rotationMatrix() const {
  return mat3(glm::rotate(mat4(1), rotation, vec3(0, 1, 0)));
}

void Environment::ensureLut() {
  if (lut_.valid()) return;
  lut_.create(128, 128, GL_RG16F);
  Framebuffer fbo;
  fbo.attach(0, lut_);
  fbo.setDrawBuffers(1);
  gl::FramebufferScope fs(fbo.id());
  gl::ViewportScope vp(0, 0, 128, 128);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  Shader& s = ShaderCache::get("fullscreen.vs.glsl", "env.fs.glsl", {"ENV_BRDF"});
  s.use();
  drawFullscreenTriangle();
}

bool Environment::initStudio() {
  ensureLut();
  TextureCube src;
  src.create(kSourceSize, GL_RGBA16F, 1 + (int)std::log2(kSourceSize));
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  Shader& s = ShaderCache::get("fullscreen.vs.glsl", "env.fs.glsl", {"ENV_STUDIO"});
  if (!s.valid()) return false;
  renderFaces(src, 0, s);
  name = "Studio";
  return buildFromCube(src);
}

bool Environment::loadHDR(const std::string& path) {
  Image img;
  if (!loadImage(path, img, 3)) return false;
  ensureLut();
  Texture2D eq(img.width, img.height, GL_RGB16F, 1);
  eq.upload(img);
  eq.setWrap(GL_REPEAT, GL_CLAMP_TO_EDGE);
  TextureCube src;
  src.create(kSourceSize, GL_RGBA16F, 1 + (int)std::log2(kSourceSize));
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  Shader& s = ShaderCache::get("fullscreen.vs.glsl", "env.fs.glsl", {"ENV_EQUIRECT"});
  s.tex("uEquirect", 0, eq.id());
  renderFaces(src, 0, s);
  auto slash = path.find_last_of('/');
  name = path.substr(slash == std::string::npos ? 0 : slash + 1);
  PLOG_I("Loaded environment %s", path.c_str());
  return buildFromCube(src);
}

bool Environment::buildFromCube(TextureCube& src) {
  static uint64_t builds = 0;
  serial_ = ++builds;
  glGenerateTextureMipmap(src.id());
  spec_.create(kSpecSize, GL_RGBA16F, kSpecLevels);
  Shader& s = ShaderCache::get("fullscreen.vs.glsl", "env.fs.glsl", {"ENV_PREFILTER"});
  if (!s.valid()) return false;
  s.tex("uSource", 0, src.id(), GL_TEXTURE_CUBE_MAP);
  s.set("uSourceSize", float(kSourceSize));
  for (int l = 0; l < kSpecLevels; ++l) {
    s.set("uRoughness", float(l) / float(kSpecLevels - 1));
    renderFaces(spec_, l, s);
  }
  glTextureParameteri(spec_.id(), GL_TEXTURE_CUBE_MAP_SEAMLESS, GL_TRUE);
  glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
  computeSH(src);
  return true;
}

void Environment::computeSH(TextureCube& src) {
  // Read a small mip and project onto 9 SH coefficients (radiance).
  int level = std::max(0, src.levels() - 1 - 4);  // 16x16 faces
  int size = std::max(1, src.size() >> level);
  std::vector<float> face(size_t(size) * size * 4);
  vec3 c[9] = {};
  float wsum = 0;
  for (int f = 0; f < 6; ++f) {
    glGetTextureSubImage(src.id(), level, 0, 0, f, size, size, 1, GL_RGBA, GL_FLOAT, (GLsizei)(face.size() * 4),
                         face.data());
    for (int y = 0; y < size; ++y)
      for (int x = 0; x < size; ++x) {
        float u = (x + 0.5f) / size, v = (y + 0.5f) / size;
        vec3 d = faceDir(f, u, v);
        float uu = u * 2 - 1, vv = v * 2 - 1;
        float w = 1.f / std::pow(1 + uu * uu + vv * vv, 1.5f);  // solid angle
        vec3 L(face[(y * size + x) * 4], face[(y * size + x) * 4 + 1], face[(y * size + x) * 4 + 2]);
        float b[9] = {0.282095f,
                      0.488603f * d.y,
                      0.488603f * d.z,
                      0.488603f * d.x,
                      1.092548f * d.x * d.y,
                      1.092548f * d.y * d.z,
                      0.315392f * (3 * d.z * d.z - 1),
                      1.092548f * d.x * d.z,
                      0.546274f * (d.x * d.x - d.y * d.y)};
        for (int i = 0; i < 9; ++i) c[i] += L * b[i] * w;
        wsum += w;
      }
  }
  float norm = 4.f * kPi / wsum;
  for (int i = 0; i < 9; ++i) sh_[i] = vec4(c[i] * norm, 0);
}

void Environment::bind(const Shader& s, int unitSpec, int unitLut) const {
  s.tex("uEnvSpec", unitSpec, spec_.id(), GL_TEXTURE_CUBE_MAP);
  s.tex("uBrdfLut", unitLut, lut_.id());
  s.setArray("uSH", sh_, 9);
  s.set("uEnvMips", mipCount());
}

}  // namespace patina
