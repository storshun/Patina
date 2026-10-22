#pragma once
// Drawing tablet input (Wacom, Huion, XP-Pen... through SDL3: Wayland tablet protocol and
// XInput2 on Linux, Windows Ink on Windows).
//
// SDL delivers pen events at the tablet's rate (often 130-266 Hz) and also synthesizes
// mouse events from them for the UI. PenInput keeps every pen and mouse motion sample of
// a frame, so strokes get the tablet's full resolution of position and pressure instead
// of one sample per frame, and it tells pen and mouse input apart by the event source.
#include <SDL3/SDL.h>

#include <cstdint>
#include <deque>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "core/Math.h"

namespace patina {

struct PointerSample {
  vec2 pos{0};          // window coordinates (same space as ImGui's mouse position)
  float pressure = 1.f; // after the pressure curve (1 for the mouse)
  float rawPressure = 1.f;
  vec2 tilt{0};         // degrees, -90..90 (x: left-right, y: top-down)
  bool down = false;    // pen tip on the tablet / left mouse button held
  bool eraser = false;  // the pen's eraser end
  bool pen = false;
  uint64_t ns = 0;      // SDL timestamp
};

enum class PenButtonAction { RightClick = 0, MiddleClick, SwapColors, Undo, Nothing, Count };
const char* penButtonActionName(PenButtonAction a);

class PenInput {
 public:
  // ---- settings (saved with the session)
  float softness = 0.f;      // -1 firm .. 0 linear .. +1 soft (light touches count more)
  float minPressure = 0.02f; // raw pressure below this counts as 0 (noise at the tip)
  float fullPressure = 1.f;  // raw pressure that already gives full effect
  bool eraserTipErases = true;
  PenButtonAction button[2] = {PenButtonAction::RightClick, PenButtonAction::MiddleClick};

  // Called for every SDL event before ImGui sees it. Returns false to drop the event
  // (a remapped pen button whose action Patina handles itself).
  bool handleEvent(SDL_Event& ev);
  // After polling: the samples since the previous frame become this frame's samples.
  void beginFrame();
  const std::vector<PointerSample>& frameSamples() const { return frame_; }

  float map(float raw) const;  // pressure curve
  // Current state
  bool lastWasPen() const { return lastWasPen_; }
  bool penDown() const { return down_; }
  bool penInProximity() const { return inProximity_; }
  bool eraser() const { return eraser_; }
  float rawPressure() const { return pressure_; }
  float pressure() const { return lastWasPen_ ? map(pressure_) : 1.f; }
  vec2 tilt() const { return tilt_; }
  bool seenPen() const { return penEvents_ > 0; }

  // Actions requested by pen buttons this frame (handled by the app)
  bool takeSwapColors() { bool v = swapReq_; swapReq_ = false; return v; }
  bool takeUndo() { bool v = undoReq_; undoReq_ = false; return v; }

  // Diagnostics for the test window and the pen_info command
  nlohmann::json diagnostics() const;
  void resetStats();
  const std::deque<std::pair<double, float>>& pressureHistory() const { return history_; }  // (seconds, raw)
  float motionRateHz() const;

  // Test harness: queue synthetic tablet input, fed to SDL a few samples per frame
  struct Injected {
    vec2 pos;
    float pressure;
    vec2 tilt;
    bool down, eraser;
    int button;  // 0 none, 1 / 2 barrel button held
    bool mouse;  // a plain mouse sample instead of the pen
  };
  void inject(const std::vector<Injected>& samples) { injectQueue_.insert(injectQueue_.end(), samples.begin(), samples.end()); }
  void pumpInjected(SDL_Window* window, int perFrame = 3);
  bool injecting() const { return !injectQueue_.empty(); }

 private:
  void push(const PointerSample& s);
  std::vector<PointerSample> pending_, frame_;
  bool lastWasPen_ = false, down_ = false, inProximity_ = false, eraser_ = false;
  float pressure_ = 0.f;
  vec2 tilt_{0};
  vec2 lastPenPos_{0};
  uint32_t buttons_ = 0;
  bool swapReq_ = false, undoReq_ = false;
  // stats
  uint64_t penEvents_ = 0, motionEvents_ = 0, axisEvents_ = 0, downEvents_ = 0, buttonEvents_ = 0;
  float minRaw_ = 1.f, maxRaw_ = 0.f, maxTilt_ = 0.f;
  bool eraserSeen_ = false;
  std::vector<uint32_t> penIds_;
  std::deque<double> motionTimes_;
  std::deque<std::pair<double, float>> history_;
  // injection
  std::deque<Injected> injectQueue_;
  bool injDown_ = false, injEraser_ = false;
  int injButton_ = 0;
};

}  // namespace patina
