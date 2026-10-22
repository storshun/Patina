#pragma once
#include <memory>
#include <vector>

#include "core/Image.h"
#include "core/Math.h"
#include "gfx/GL.h"

namespace patina {

// RAII OpenGL 2D texture (immutable storage, DSA).
class Texture2D {
 public:
  Texture2D() = default;
  Texture2D(int w, int h, GLenum internalFormat, int levels = 1) { create(w, h, internalFormat, levels); }
  ~Texture2D() { destroy(); }
  Texture2D(const Texture2D&) = delete;
  Texture2D& operator=(const Texture2D&) = delete;
  Texture2D(Texture2D&& o) noexcept { *this = std::move(o); }
  Texture2D& operator=(Texture2D&& o) noexcept;

  void create(int w, int h, GLenum internalFormat, int levels = 1);
  void destroy();
  bool valid() const { return id_ != 0; }
  GLuint id() const { return id_; }
  int width() const { return w_; }
  int height() const { return h_; }
  ivec2 size() const { return {w_, h_}; }
  GLenum format() const { return fmt_; }
  int levels() const { return levels_; }

  void setFilter(GLenum minF, GLenum magF);
  void setWrap(GLenum s, GLenum t);
  void generateMips();
  void clear(const vec4& v);
  // Upload an image (row 0 = top in the Image, flipped to GL's bottom-up).
  void upload(const Image& img);
  void uploadRaw(GLenum fmt, GLenum type, const void* data);
  // Read back as float image (row 0 = top), `channels` 1..4.
  Image download(int channels = 4, int level = 0) const;
  void downloadRaw(GLenum fmt, GLenum type, void* dst, size_t bytes, int level = 0) const;
  void copyFrom(const Texture2D& src);  // same size + format
  static int channelsOf(GLenum internalFormat);

 private:
  GLuint id_ = 0;
  int w_ = 0, h_ = 0, levels_ = 1;
  GLenum fmt_ = 0;
};

class TextureCube {
 public:
  ~TextureCube() { destroy(); }
  void create(int size, GLenum internalFormat, int levels);
  void destroy();
  GLuint id() const { return id_; }
  int size() const { return size_; }
  int levels() const { return levels_; }

 private:
  GLuint id_ = 0;
  int size_ = 0, levels_ = 1;
};

// Framebuffer with up to 8 color attachments and an optional depth texture.
class Framebuffer {
 public:
  Framebuffer() = default;
  ~Framebuffer() { destroy(); }
  Framebuffer(const Framebuffer&) = delete;
  Framebuffer& operator=(const Framebuffer&) = delete;

  void create();
  void destroy();
  GLuint id() const { return fbo_; }
  void attach(int slot, const Texture2D& tex, int level = 0);
  void attachLayer(int slot, GLuint tex, int level, int layer);  // cube face / array layer
  void attachDepth(const Texture2D& tex);
  void detachAll();
  void setDrawBuffers(int count);
  bool complete() const;
  void bind() const { glBindFramebuffer(GL_FRAMEBUFFER, fbo_); }

 private:
  GLuint fbo_ = 0;
};

// A color + depth offscreen target that resizes on demand (viewports, thumbnails).
struct RenderTarget {
  Texture2D color;
  Texture2D depth;
  Framebuffer fbo;
  int samples = 1;
  void ensure(int w, int h, GLenum colorFormat = GL_RGBA8);
  int width() const { return color.width(); }
  int height() const { return color.height(); }
};

// Fullscreen triangle helper (no vertex buffers; uses gl_VertexID).
void drawFullscreenTriangle();

}  // namespace patina
