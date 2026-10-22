#pragma once
#include <string>

namespace patina {

// Offscreen OpenGL 4.5 core context through EGL (no window system needed).
// Tries, in order: EGL device platform (NVIDIA / Mesa DRM render nodes),
// Mesa surfaceless platform, then the default display.
class HeadlessContext {
 public:
  ~HeadlessContext();
  bool create(std::string* err = nullptr);
  void destroy();
  bool makeCurrent();
  std::string description() const { return desc_; }

 private:
  void* display_ = nullptr;
  void* context_ = nullptr;
  std::string desc_;
};

}  // namespace patina
