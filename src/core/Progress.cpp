#include "core/Progress.h"

#include <atomic>
#include <mutex>

namespace patina::progress {
namespace {
std::atomic<float> g_fraction{-1.f};
std::atomic<bool> g_cancel{false};
std::atomic<int> g_depth{0};
std::mutex g_m;
std::string g_label;
}  // namespace

void begin(const std::string& l) {
  // nested operations (a bake inside a resolution change) keep the outer label
  if (g_depth++ == 0) {
    std::lock_guard<std::mutex> lk(g_m);
    g_label = l;
    g_fraction = 0.f;
  }
}
void set(float f) {
  if (g_depth.load() == 1) g_fraction = f < 0.f ? 0.f : f > 1.f ? 1.f : f;
}
void end() {
  if (--g_depth <= 0) {
    g_depth = 0;
    g_fraction = -1.f;
    std::lock_guard<std::mutex> lk(g_m);
    g_label.clear();
  }
}
float current() { return g_fraction.load(); }
std::string label() {
  std::lock_guard<std::mutex> lk(g_m);
  return g_label;
}
void requestCancel() { g_cancel = true; }
void clearCancel() { g_cancel = false; }
bool cancelled() { return g_cancel.load(); }

}  // namespace patina::progress
