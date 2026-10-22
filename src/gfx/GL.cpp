#include "gfx/GL.h"

#include "core/Log.h"

namespace patina::gl {

static void GLAD_API_PTR debugCb(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei, const GLchar* msg,
                                 const void*) {
  if (severity == GL_DEBUG_SEVERITY_NOTIFICATION) return;
  if (type == GL_DEBUG_TYPE_ERROR) PLOG_E("GL: %s", msg);
  else if (severity == GL_DEBUG_SEVERITY_HIGH || severity == GL_DEBUG_SEVERITY_MEDIUM) PLOG_W("GL: %s", msg);
  else PLOG_D("GL: %s", msg);
}

void installDebugCallback() {
  if (!GLAD_GL_KHR_debug && !GLAD_GL_VERSION_4_3) return;
  glEnable(GL_DEBUG_OUTPUT);
  glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
  glDebugMessageCallback(debugCb, nullptr);
}

bool checkError(const char* where) {
  GLenum e = glGetError();
  if (e == GL_NO_ERROR) return false;
  PLOG_E("GL error 0x%x at %s", e, where);
  return true;
}

std::string rendererString() {
  auto s = [](GLenum e) {
    const char* p = (const char*)glGetString(e);
    return std::string(p ? p : "?");
  };
  return s(GL_RENDERER) + " | " + s(GL_VERSION);
}

}  // namespace patina::gl
