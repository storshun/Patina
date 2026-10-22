#include "gfx/Texture.h"

#include <cstring>

#include "core/Log.h"

namespace patina {

Texture2D& Texture2D::operator=(Texture2D&& o) noexcept {
  if (this != &o) {
    destroy();
    id_ = o.id_;
    w_ = o.w_;
    h_ = o.h_;
    levels_ = o.levels_;
    fmt_ = o.fmt_;
    o.id_ = 0;
  }
  return *this;
}

void Texture2D::create(int w, int h, GLenum internalFormat, int levels) {
  destroy();
  w_ = std::max(1, w);
  h_ = std::max(1, h);
  fmt_ = internalFormat;
  if (levels <= 0) {
    levels = 1;
    int s = std::max(w_, h_);
    while (s > 1) s >>= 1, ++levels;
  }
  levels_ = levels;
  glCreateTextures(GL_TEXTURE_2D, 1, &id_);
  glTextureStorage2D(id_, levels_, fmt_, w_, h_);
  bool isDepth = fmt_ == GL_DEPTH_COMPONENT32F || fmt_ == GL_DEPTH_COMPONENT24 || fmt_ == GL_DEPTH24_STENCIL8;
  setFilter(levels_ > 1 ? GL_LINEAR_MIPMAP_LINEAR : (isDepth ? GL_NEAREST : GL_LINEAR), isDepth ? GL_NEAREST : GL_LINEAR);
  setWrap(GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE);
  if (fmt_ == GL_R8 || fmt_ == GL_R16F || fmt_ == GL_R32F || fmt_ == GL_R16) {
    // single-channel textures read as gray when displayed (UI previews, masks)
    GLint sw[4] = {GL_RED, GL_RED, GL_RED, GL_ONE};
    glTextureParameteriv(id_, GL_TEXTURE_SWIZZLE_RGBA, sw);
  }
}

void Texture2D::destroy() {
  if (id_) glDeleteTextures(1, &id_);
  id_ = 0;
}

void Texture2D::setFilter(GLenum minF, GLenum magF) {
  glTextureParameteri(id_, GL_TEXTURE_MIN_FILTER, minF);
  glTextureParameteri(id_, GL_TEXTURE_MAG_FILTER, magF);
}

void Texture2D::setWrap(GLenum s, GLenum t) {
  glTextureParameteri(id_, GL_TEXTURE_WRAP_S, s);
  glTextureParameteri(id_, GL_TEXTURE_WRAP_T, t);
}

void Texture2D::generateMips() {
  if (levels_ > 1) glGenerateTextureMipmap(id_);
}

int Texture2D::channelsOf(GLenum f) {
  switch (f) {
    case GL_R8: case GL_R16: case GL_R16F: case GL_R32F: return 1;
    case GL_RG8: case GL_RG16: case GL_RG16F: case GL_RG32F: return 2;
    case GL_RGB8: case GL_RGB16F: case GL_RGB32F: case GL_SRGB8: case GL_R11F_G11F_B10F: return 3;
    default: return 4;
  }
}

static GLenum pixelFormat(int ch) {
  return ch == 1 ? GL_RED : ch == 2 ? GL_RG : ch == 3 ? GL_RGB : GL_RGBA;
}

void Texture2D::clear(const vec4& v) {
  glClearTexImage(id_, 0, GL_RGBA, GL_FLOAT, &v.x);
}

void Texture2D::upload(const Image& img) {
  if (img.width != w_ || img.height != h_) {
    PLOG_E("Texture upload size mismatch (%dx%d vs %dx%d)", img.width, img.height, w_, h_);
    return;
  }
  // flip rows: Image row 0 = top, GL row 0 = bottom
  size_t row = size_t(img.width) * img.channels;
  std::vector<float> flipped(img.pixels.size());
  for (int y = 0; y < img.height; ++y)
    std::memcpy(&flipped[size_t(y) * row], &img.pixels[size_t(img.height - 1 - y) * row], row * sizeof(float));
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTextureSubImage2D(id_, 0, 0, 0, w_, h_, pixelFormat(img.channels), GL_FLOAT, flipped.data());
  generateMips();
}

void Texture2D::uploadRaw(GLenum fmt, GLenum type, const void* data) {
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTextureSubImage2D(id_, 0, 0, 0, w_, h_, fmt, type, data);
  generateMips();
}

Image Texture2D::download(int channels, int level) const {
  Image img;
  int w = std::max(1, w_ >> level), h = std::max(1, h_ >> level);
  img.resize(w, h, channels);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glGetTextureImage(id_, level, pixelFormat(channels), GL_FLOAT, (GLsizei)(img.pixels.size() * sizeof(float)),
                    img.pixels.data());
  img.flipVertical();
  return img;
}

void Texture2D::downloadRaw(GLenum fmt, GLenum type, void* dst, size_t bytes, int level) const {
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glGetTextureImage(id_, level, fmt, type, (GLsizei)bytes, dst);
}

void Texture2D::copyFrom(const Texture2D& src) {
  glCopyImageSubData(src.id(), GL_TEXTURE_2D, 0, 0, 0, 0, id_, GL_TEXTURE_2D, 0, 0, 0, 0, std::min(w_, src.w_),
                     std::min(h_, src.h_), 1);
}

void TextureCube::create(int size, GLenum internalFormat, int levels) {
  destroy();
  size_ = size;
  levels_ = levels;
  glCreateTextures(GL_TEXTURE_CUBE_MAP, 1, &id_);
  glTextureStorage2D(id_, levels, internalFormat, size, size);
  glTextureParameteri(id_, GL_TEXTURE_MIN_FILTER, levels > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
  glTextureParameteri(id_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTextureParameteri(id_, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTextureParameteri(id_, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTextureParameteri(id_, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
}

void TextureCube::destroy() {
  if (id_) glDeleteTextures(1, &id_);
  id_ = 0;
}

void Framebuffer::create() {
  if (!fbo_) glCreateFramebuffers(1, &fbo_);
}

void Framebuffer::destroy() {
  if (fbo_) glDeleteFramebuffers(1, &fbo_);
  fbo_ = 0;
}

void Framebuffer::attach(int slot, const Texture2D& tex, int level) {
  create();
  glNamedFramebufferTexture(fbo_, GL_COLOR_ATTACHMENT0 + slot, tex.id(), level);
}

void Framebuffer::attachLayer(int slot, GLuint tex, int level, int layer) {
  create();
  glNamedFramebufferTextureLayer(fbo_, GL_COLOR_ATTACHMENT0 + slot, tex, level, layer);
}

void Framebuffer::attachDepth(const Texture2D& tex) {
  create();
  glNamedFramebufferTexture(fbo_, GL_DEPTH_ATTACHMENT, tex.id(), 0);
}

void Framebuffer::detachAll() {
  create();
  for (int i = 0; i < 8; ++i) glNamedFramebufferTexture(fbo_, GL_COLOR_ATTACHMENT0 + i, 0, 0);
  glNamedFramebufferTexture(fbo_, GL_DEPTH_ATTACHMENT, 0, 0);
}

void Framebuffer::setDrawBuffers(int count) {
  GLenum bufs[8];
  for (int i = 0; i < count; ++i) bufs[i] = GL_COLOR_ATTACHMENT0 + i;
  glNamedFramebufferDrawBuffers(fbo_, count, bufs);
}

bool Framebuffer::complete() const {
  GLenum s = glCheckNamedFramebufferStatus(fbo_, GL_FRAMEBUFFER);
  if (s != GL_FRAMEBUFFER_COMPLETE) PLOG_E("Framebuffer incomplete: 0x%x", s);
  return s == GL_FRAMEBUFFER_COMPLETE;
}

void RenderTarget::ensure(int w, int h, GLenum colorFormat) {
  w = std::max(1, w);
  h = std::max(1, h);
  if (color.valid() && color.width() == w && color.height() == h && color.format() == colorFormat) return;
  color.create(w, h, colorFormat);
  depth.create(w, h, GL_DEPTH_COMPONENT32F);
  fbo.create();
  fbo.attach(0, color);
  fbo.attachDepth(depth);
  fbo.setDrawBuffers(1);
  fbo.complete();
}

void drawFullscreenTriangle() {
  static GLuint vao = 0;
  if (!vao) glCreateVertexArrays(1, &vao);
  glBindVertexArray(vao);
  glDrawArrays(GL_TRIANGLES, 0, 3);
}

}  // namespace patina
