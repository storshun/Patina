// Windows: offscreen OpenGL 4.5 core context through WGL on a hidden window.
// (EGL is not part of Windows; the window is never shown and all rendering
// goes to framebuffer objects, exactly like the EGL path on Linux.)
#include "platform/HeadlessContext.h"

#include <windows.h>

#include <glad/gl.h>

#include "core/Log.h"

namespace patina {

namespace {
// WGL_ARB_create_context / WGL_ARB_create_context_profile
constexpr int WGL_CONTEXT_MAJOR_VERSION_ARB = 0x2091;
constexpr int WGL_CONTEXT_MINOR_VERSION_ARB = 0x2092;
constexpr int WGL_CONTEXT_FLAGS_ARB = 0x2094;
constexpr int WGL_CONTEXT_PROFILE_MASK_ARB = 0x9126;
constexpr int WGL_CONTEXT_CORE_PROFILE_BIT_ARB = 0x00000001;
using PFNWGLCREATECONTEXTATTRIBSARB = HGLRC(WINAPI*)(HDC, HGLRC, const int*);

HMODULE g_opengl32 = nullptr;

GLADapiproc loadProc(const char* name) {
  PROC p = wglGetProcAddress(name);
  // wglGetProcAddress returns small sentinel values for unsupported / GL 1.1 entry points
  if (p == nullptr || p == (PROC)1 || p == (PROC)2 || p == (PROC)3 || p == (PROC)-1) {
    if (!g_opengl32) g_opengl32 = LoadLibraryA("opengl32.dll");
    p = g_opengl32 ? GetProcAddress(g_opengl32, name) : nullptr;
  }
  return (GLADapiproc)p;
}

struct WinState {
  HWND hwnd = nullptr;
  HDC dc = nullptr;
  HGLRC rc = nullptr;
};

const wchar_t* kClass = L"PatinaHeadlessGL";

bool setPixelFormat(HDC dc) {
  PIXELFORMATDESCRIPTOR pfd{};
  pfd.nSize = sizeof pfd;
  pfd.nVersion = 1;
  pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
  pfd.iPixelType = PFD_TYPE_RGBA;
  pfd.cColorBits = 32;
  pfd.cDepthBits = 24;
  pfd.cStencilBits = 8;
  int fmt = ChoosePixelFormat(dc, &pfd);
  return fmt != 0 && SetPixelFormat(dc, fmt, &pfd);
}
}  // namespace

HeadlessContext::~HeadlessContext() { destroy(); }

bool HeadlessContext::create(std::string* err) {
  auto fail = [&](const std::string& m) {
    if (err) *err = m;
    destroy();
    return false;
  };
  HINSTANCE inst = GetModuleHandleW(nullptr);
  WNDCLASSW wc{};
  wc.style = CS_OWNDC;
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = inst;
  wc.lpszClassName = kClass;
  RegisterClassW(&wc);  // fails harmlessly when already registered
  auto* st = new WinState();
  display_ = st;
  st->hwnd = CreateWindowExW(0, kClass, L"Patina (headless)", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr,
                             inst, nullptr);
  if (!st->hwnd) return fail("CreateWindow failed");
  st->dc = GetDC(st->hwnd);
  if (!setPixelFormat(st->dc)) return fail("No OpenGL pixel format (is a GPU driver installed?)");
  // legacy context first, to get wglCreateContextAttribsARB
  HGLRC legacy = wglCreateContext(st->dc);
  if (!legacy || !wglMakeCurrent(st->dc, legacy)) return fail("wglCreateContext failed");
  auto createAttribs = (PFNWGLCREATECONTEXTATTRIBSARB)wglGetProcAddress("wglCreateContextAttribsARB");
  if (!createAttribs) {
    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(legacy);
    return fail("WGL_ARB_create_context is not supported by this driver");
  }
  const int attr[] = {WGL_CONTEXT_MAJOR_VERSION_ARB, 4, WGL_CONTEXT_MINOR_VERSION_ARB, 5,
                      WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB, WGL_CONTEXT_FLAGS_ARB, 0, 0};
  st->rc = createAttribs(st->dc, nullptr, attr);
  wglMakeCurrent(nullptr, nullptr);
  wglDeleteContext(legacy);
  if (!st->rc) return fail("This GPU driver does not provide OpenGL 4.5 core");
  context_ = st->rc;
  if (!wglMakeCurrent(st->dc, st->rc)) return fail("wglMakeCurrent failed");
  if (!gladLoadGL(loadProc)) return fail("Failed to load OpenGL functions");
  const char* renderer = (const char*)glGetString(GL_RENDERER);
  const char* version = (const char*)glGetString(GL_VERSION);
  desc_ = std::string("wgl: ") + (renderer ? renderer : "?") + " | " + (version ? version : "?");
  PLOG_I("Headless GL context (%s)", desc_.c_str());
  return true;
}

void HeadlessContext::destroy() {
  auto* st = (WinState*)display_;
  if (!st) return;
  if (st->rc) {
    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(st->rc);
  }
  if (st->dc && st->hwnd) ReleaseDC(st->hwnd, st->dc);
  if (st->hwnd) DestroyWindow(st->hwnd);
  delete st;
  display_ = nullptr;
  context_ = nullptr;
}

bool HeadlessContext::makeCurrent() {
  auto* st = (WinState*)display_;
  return st && st->rc && wglMakeCurrent(st->dc, st->rc);
}

}  // namespace patina
