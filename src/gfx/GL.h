#pragma once
#include <glad/gl.h>

#include <string>

namespace patina::gl {

// Installs a KHR_debug callback that routes driver messages into the log.
void installDebugCallback();
// Logs and returns true if glGetError reports an error (debug helper).
bool checkError(const char* where);
std::string rendererString();

// Scoped state helpers ------------------------------------------------------
struct ViewportScope {
  int prev[4];
  ViewportScope(int x, int y, int w, int h) {
    glGetIntegerv(GL_VIEWPORT, prev);
    glViewport(x, y, w, h);
  }
  ~ViewportScope() { glViewport(prev[0], prev[1], prev[2], prev[3]); }
};

struct FramebufferScope {
  int prev;
  explicit FramebufferScope(unsigned fbo) {
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  }
  ~FramebufferScope() { glBindFramebuffer(GL_FRAMEBUFFER, prev); }
};

}  // namespace patina::gl
