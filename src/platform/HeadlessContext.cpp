#include "platform/HeadlessContext.h"

#include <glad/egl.h>
#include <glad/gl.h>

#include <vector>

#include "core/Log.h"
#include "gfx/GL.h"

namespace patina {

HeadlessContext::~HeadlessContext() { destroy(); }

static bool tryDisplay(EGLDisplay dpy, EGLContext& ctxOut) {
  if (dpy == EGL_NO_DISPLAY) return false;
  EGLint major = 0, minor = 0;
  if (!eglInitialize(dpy, &major, &minor)) return false;
  // reload with the display to pick up client + display extensions
  gladLoaderLoadEGL(dpy);
  if (!eglBindAPI(EGL_OPENGL_API)) return false;
  EGLConfig cfg = nullptr;
  EGLint n = 0;
  const EGLint cfgAttr[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
  if (!eglChooseConfig(dpy, cfgAttr, &cfg, 1, &n) || n == 0) {
    // surfaceless displays may expose no pbuffer configs
    const EGLint any[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE};
    if (!eglChooseConfig(dpy, any, &cfg, 1, &n) || n == 0) cfg = nullptr;
  }
  const EGLint ctxAttr[] = {EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 5,
                            EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
  EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxAttr);
  if (ctx == EGL_NO_CONTEXT) {
    eglTerminate(dpy);
    return false;
  }
  if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
    eglDestroyContext(dpy, ctx);
    eglTerminate(dpy);
    return false;
  }
  ctxOut = ctx;
  return true;
}

bool HeadlessContext::create(std::string* err) {
  auto fail = [&](const std::string& m) {
    if (err) *err = m;
    PLOG_E("%s", m.c_str());
    return false;
  };
  if (!gladLoaderLoadEGL(EGL_NO_DISPLAY)) return fail("Could not load libEGL.so.1 (install mesa-libEGL)");

  std::vector<std::pair<std::string, EGLDisplay>> candidates;
  if (eglQueryDevicesEXT && eglGetPlatformDisplayEXT) {
    EGLDeviceEXT devs[16];
    EGLint nd = 0;
    if (eglQueryDevicesEXT(16, devs, &nd))
      for (int i = 0; i < nd; ++i)
        candidates.push_back({"egl-device-" + std::to_string(i),
                              eglGetPlatformDisplayEXT(EGL_PLATFORM_DEVICE_EXT, devs[i], nullptr)});
  }
  if (eglGetPlatformDisplayEXT)
    candidates.push_back({"surfaceless", eglGetPlatformDisplayEXT(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr)});
  candidates.push_back({"default", eglGetDisplay(EGL_DEFAULT_DISPLAY)});

  for (auto& [name, dpy] : candidates) {
    EGLContext ctx = EGL_NO_CONTEXT;
    if (tryDisplay(dpy, ctx)) {
      display_ = dpy;
      context_ = ctx;
      if (!gladLoadGL((GLADloadfunc)eglGetProcAddress)) return fail("Failed to load OpenGL functions");
      if (!GLAD_GL_VERSION_4_5) return fail("OpenGL 4.5 is required");
      desc_ = name + ": " + gl::rendererString();
      PLOG_I("Headless GL context (%s)", desc_.c_str());
      return true;
    }
  }
  return fail("Could not create a headless OpenGL 4.5 context through EGL");
}

bool HeadlessContext::makeCurrent() {
  return display_ && eglMakeCurrent((EGLDisplay)display_, EGL_NO_SURFACE, EGL_NO_SURFACE, (EGLContext)context_);
}

void HeadlessContext::destroy() {
  if (display_) {
    eglMakeCurrent((EGLDisplay)display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (context_) eglDestroyContext((EGLDisplay)display_, (EGLContext)context_);
    eglTerminate((EGLDisplay)display_);
  }
  display_ = context_ = nullptr;
}

}  // namespace patina
