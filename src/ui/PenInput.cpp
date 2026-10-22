#include "ui/PenInput.h"

#include <algorithm>
#include <cmath>

#include "core/I18n.h"

namespace patina {

const char* penButtonActionName(PenButtonAction a) {
  switch (a) {
    case PenButtonAction::RightClick: return "Right click";
    case PenButtonAction::MiddleClick: return "Middle click (orbit / pan)";
    case PenButtonAction::SwapColors: return "Swap colors";
    case PenButtonAction::Undo: return "Undo";
    case PenButtonAction::Nothing: return "Nothing";
    default: return "?";
  }
}

static double secondsOf(uint64_t ns) { return double(ns) * 1e-9; }

float PenInput::map(float raw) const {
  float span = std::max(fullPressure - minPressure, 1e-3f);
  float t = std::clamp((raw - minPressure) / span, 0.f, 1.f);
  float gamma = std::exp2(-std::clamp(softness, -1.f, 1.f) * 1.5f);
  return std::pow(t, gamma);
}

void PenInput::push(const PointerSample& s) {
  pending_.push_back(s);
  if (pending_.size() > 4096) pending_.erase(pending_.begin(), pending_.begin() + 1024);  // never grows unbounded
}

bool PenInput::handleEvent(SDL_Event& ev) {
  auto penSample = [&](float x, float y, uint64_t ts) {
    PointerSample s;
    s.pos = vec2(x, y);
    s.rawPressure = pressure_;
    s.pressure = down_ ? map(pressure_) : 0.f;
    s.tilt = tilt_;
    s.down = down_;
    s.eraser = eraser_;
    s.pen = true;
    s.ns = ts;
    lastPenPos_ = s.pos;
    push(s);
  };
  auto notePen = [&](uint32_t id) {
    ++penEvents_;
    lastWasPen_ = true;
    if (std::find(penIds_.begin(), penIds_.end(), id) == penIds_.end() && penIds_.size() < 16) penIds_.push_back(id);
  };
  switch (ev.type) {
    case SDL_EVENT_PEN_PROXIMITY_IN:
      notePen(ev.pproximity.which);
      inProximity_ = true;
      break;
    case SDL_EVENT_PEN_PROXIMITY_OUT:
      notePen(ev.pproximity.which);
      inProximity_ = false;
      down_ = false;
      break;
    case SDL_EVENT_PEN_DOWN:
    case SDL_EVENT_PEN_UP:
      notePen(ev.ptouch.which);
      ++downEvents_;
      down_ = ev.type == SDL_EVENT_PEN_DOWN;
      eraser_ = ev.ptouch.eraser;
      eraserSeen_ |= eraser_;
      inProximity_ = true;
      penSample(ev.ptouch.x, ev.ptouch.y, ev.ptouch.timestamp);
      break;
    case SDL_EVENT_PEN_MOTION: {
      notePen(ev.pmotion.which);
      ++motionEvents_;
      inProximity_ = true;
      down_ = (ev.pmotion.pen_state & SDL_PEN_INPUT_DOWN) != 0;
      eraser_ = (ev.pmotion.pen_state & SDL_PEN_INPUT_ERASER_TIP) != 0;
      eraserSeen_ |= eraser_;
      double t = secondsOf(ev.pmotion.timestamp);
      motionTimes_.push_back(t);
      while (!motionTimes_.empty() && motionTimes_.front() < t - 1.0) motionTimes_.pop_front();
      penSample(ev.pmotion.x, ev.pmotion.y, ev.pmotion.timestamp);
      break;
    }
    case SDL_EVENT_PEN_AXIS: {
      notePen(ev.paxis.which);
      ++axisEvents_;
      if (ev.paxis.axis == SDL_PEN_AXIS_PRESSURE) {
        pressure_ = std::clamp(ev.paxis.value, 0.f, 1.f);
        minRaw_ = std::min(minRaw_, pressure_);
        maxRaw_ = std::max(maxRaw_, pressure_);
        double t = secondsOf(ev.paxis.timestamp);
        history_.push_back({t, pressure_});
        while (!history_.empty() && history_.front().first < t - 5.0) history_.pop_front();
        // a pressure change without movement still matters (pressing harder in place)
        if (down_) penSample(ev.paxis.x, ev.paxis.y, ev.paxis.timestamp);
      } else if (ev.paxis.axis == SDL_PEN_AXIS_XTILT) {
        tilt_.x = ev.paxis.value;
        maxTilt_ = std::max(maxTilt_, std::abs(tilt_.x));
      } else if (ev.paxis.axis == SDL_PEN_AXIS_YTILT) {
        tilt_.y = ev.paxis.value;
        maxTilt_ = std::max(maxTilt_, std::abs(tilt_.y));
      }
      break;
    }
    case SDL_EVENT_PEN_BUTTON_DOWN:
    case SDL_EVENT_PEN_BUTTON_UP:
      notePen(ev.pbutton.which);
      ++buttonEvents_;
      if (ev.pbutton.button < 32) {
        if (ev.type == SDL_EVENT_PEN_BUTTON_DOWN) buttons_ |= 1u << ev.pbutton.button;
        else buttons_ &= ~(1u << ev.pbutton.button);
      }
      break;
    case SDL_EVENT_MOUSE_MOTION:
      if (ev.motion.which == SDL_PEN_MOUSEID) break;  // the pen's own samples are already queued
      if (ev.motion.which == SDL_TOUCH_MOUSEID) break;
      {
        lastWasPen_ = false;
        PointerSample s;
        s.pos = vec2(ev.motion.x, ev.motion.y);
        s.down = (ev.motion.state & SDL_BUTTON_LMASK) != 0;
        s.ns = ev.motion.timestamp;
        push(s);
      }
      break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
      bool isDown = ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
      if (ev.button.which == SDL_PEN_MOUSEID) {
        // SDL turns the first barrel button into a right click and the second into a
        // middle click; map them to what the artist chose
        int b = ev.button.button == SDL_BUTTON_RIGHT ? 0 : ev.button.button == SDL_BUTTON_MIDDLE ? 1 : -1;
        if (b < 0) break;
        switch (button[b]) {
          case PenButtonAction::RightClick: ev.button.button = SDL_BUTTON_RIGHT; break;
          case PenButtonAction::MiddleClick: ev.button.button = SDL_BUTTON_MIDDLE; break;
          case PenButtonAction::SwapColors:
            if (isDown) swapReq_ = true;
            return false;
          case PenButtonAction::Undo:
            if (isDown) undoReq_ = true;
            return false;
          default: return false;
        }
        break;
      }
      if (ev.button.which == SDL_TOUCH_MOUSEID) break;
      lastWasPen_ = false;
      if (ev.button.button == SDL_BUTTON_LEFT) {
        PointerSample s;
        s.pos = vec2(ev.button.x, ev.button.y);
        s.down = isDown;
        s.ns = ev.button.timestamp;
        push(s);
      }
      break;
    }
    default: break;
  }
  return true;
}

void PenInput::beginFrame() {
  frame_.swap(pending_);
  pending_.clear();
}

float PenInput::motionRateHz() const {
  if (motionTimes_.size() < 2) return 0.f;
  double span = motionTimes_.back() - motionTimes_.front();
  return span > 0.05 ? float((motionTimes_.size() - 1) / span) : 0.f;
}

void PenInput::resetStats() {
  penEvents_ = motionEvents_ = axisEvents_ = downEvents_ = buttonEvents_ = 0;
  minRaw_ = 1.f;
  maxRaw_ = 0.f;
  maxTilt_ = 0.f;
  eraserSeen_ = false;
  penIds_.clear();
  motionTimes_.clear();
  history_.clear();
}

nlohmann::json PenInput::diagnostics() const {
  using nlohmann::json;
  const char* drv = SDL_GetCurrentVideoDriver();
  int v = SDL_GetVersion();
  json j = {{"platform", SDL_GetPlatform()},
            {"video_driver", drv ? drv : ""},
            {"sdl", std::to_string(SDL_VERSIONNUM_MAJOR(v)) + "." + std::to_string(SDL_VERSIONNUM_MINOR(v)) + "." +
                        std::to_string(SDL_VERSIONNUM_MICRO(v))},
            {"pen_seen", penEvents_ > 0},
            {"pens", penIds_},
            {"events", {{"total", penEvents_}, {"motion", motionEvents_}, {"axis", axisEvents_}, {"touch", downEvents_},
                        {"buttons", buttonEvents_}}},
            {"motion_rate_hz", motionRateHz()},
            {"pressure_seen", {{"min", penEvents_ ? minRaw_ : 0.f}, {"max", maxRaw_}}},
            {"tilt_seen_degrees", maxTilt_},
            {"eraser_seen", eraserSeen_},
            {"state", {{"in_proximity", inProximity_}, {"down", down_}, {"eraser", eraser_}, {"pressure", pressure_},
                       {"mapped", map(pressure_)}, {"tilt", {tilt_.x, tilt_.y}}, {"buttons", buttons_},
                       {"last_input", lastWasPen_ ? "pen" : "mouse"}}},
            {"settings", {{"softness", softness}, {"min_pressure", minPressure}, {"full_pressure", fullPressure},
                          {"eraser_tip_erases", eraserTipErases},
                          {"button1", penButtonActionName(button[0])}, {"button2", penButtonActionName(button[1])}}}};
  // plain-language findings (English in the report and for MCP; the pen test window translates them)
  json f = json::array();
  if (!penEvents_) {
    f.push_back(N_("No pen events yet: move the pen over the window. If nothing arrives, the tablet driver is not "
                   "delivering tablet events to applications (see the manual's pen tablet section)."));
  } else {
    if (maxRaw_ <= 0.f || (maxRaw_ >= 0.999f && minRaw_ >= 0.999f))
      f.push_back(N_("Pen seen but no pressure range: the driver reports the pen as a mouse or pressure is off."));
    else if (maxRaw_ < 0.6f)
      f.push_back(N_("The highest pressure seen is low; press harder once, or lower 'Full pressure at'."));
    else
      f.push_back(N_("Pressure works."));
    if (motionRateHz() > 0 && motionRateHz() < 90)
      f.push_back(N_("Fewer than 90 pen samples per second: the tablet may be in a power-saving or mouse mode."));
    if (maxTilt_ > 1.f) f.push_back(N_("Tilt works."));
    if (eraserSeen_) f.push_back(N_("Eraser end works."));
  }
  j["findings"] = f;
  return j;
}

void PenInput::pumpInjected(SDL_Window* window, int perFrame) {
  if (injectQueue_.empty() || !window) return;
  SDL_WindowID wid = SDL_GetWindowID(window);
  uint64_t now = SDL_GetTicksNS();
  auto push = [&](SDL_Event e) { SDL_PushEvent(&e); };
  for (int n = 0; n < perFrame && !injectQueue_.empty(); ++n) {
    Injected s = injectQueue_.front();
    // a new contact: put the real pointer there first (the UI polls it while no button
    // is held) and send the touch on the next frame, like a real pen landing
    if (s.down && !injDown_ && n > 0) return;
    if (s.down && !injDown_) {
      static vec2 warped{-1e9f};
      if (glm::length(warped - s.pos) > 0.5f) {
        SDL_WarpMouseInWindow(window, s.pos.x, s.pos.y);
        warped = s.pos;
        return;
      }
      warped = vec2(-1e9f);
    }
    injectQueue_.pop_front();
    if (s.mouse) {
      SDL_Event e{};
      e.motion.type = SDL_EVENT_MOUSE_MOTION;
      e.motion.timestamp = now;
      e.motion.windowID = wid;
      e.motion.which = 1;
      e.motion.x = s.pos.x;
      e.motion.y = s.pos.y;
      e.motion.state = s.down ? SDL_BUTTON_LMASK : 0;
      push(e);
      if (s.down != injDown_) {
        SDL_Event b{};
        b.button.type = s.down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
        b.button.timestamp = now;
        b.button.windowID = wid;
        b.button.which = 1;
        b.button.button = SDL_BUTTON_LEFT;
        b.button.down = s.down;
        b.button.clicks = 1;
        b.button.x = s.pos.x;
        b.button.y = s.pos.y;
        push(b);
        injDown_ = s.down;
      }
      continue;
    }
    SDL_PenInputFlags state = (s.down ? SDL_PEN_INPUT_DOWN : 0) | (s.eraser ? SDL_PEN_INPUT_ERASER_TIP : 0);
    auto axis = [&](SDL_PenAxis a, float v) {
      SDL_Event e{};
      e.paxis.type = SDL_EVENT_PEN_AXIS;
      e.paxis.timestamp = now;
      e.paxis.windowID = wid;
      e.paxis.which = 1;
      e.paxis.pen_state = state;
      e.paxis.x = s.pos.x;
      e.paxis.y = s.pos.y;
      e.paxis.axis = a;
      e.paxis.value = v;
      push(e);
    };
    axis(SDL_PEN_AXIS_PRESSURE, s.down ? s.pressure : 0.f);
    axis(SDL_PEN_AXIS_XTILT, s.tilt.x);
    axis(SDL_PEN_AXIS_YTILT, s.tilt.y);
    if (s.down != injDown_ || s.eraser != injEraser_) {
      SDL_Event e{};
      e.ptouch.type = s.down ? SDL_EVENT_PEN_DOWN : SDL_EVENT_PEN_UP;
      e.ptouch.timestamp = now;
      e.ptouch.windowID = wid;
      e.ptouch.which = 1;
      e.ptouch.pen_state = state;
      e.ptouch.x = s.pos.x;
      e.ptouch.y = s.pos.y;
      e.ptouch.eraser = s.eraser;
      e.ptouch.down = s.down;
      push(e);
      // SDL's mouse emulation of the tip
      SDL_Event m{};
      m.motion.type = SDL_EVENT_MOUSE_MOTION;
      m.motion.timestamp = now;
      m.motion.windowID = wid;
      m.motion.which = SDL_PEN_MOUSEID;
      m.motion.x = s.pos.x;
      m.motion.y = s.pos.y;
      push(m);
      SDL_Event b{};
      b.button.type = s.down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
      b.button.timestamp = now;
      b.button.windowID = wid;
      b.button.which = SDL_PEN_MOUSEID;
      b.button.button = SDL_BUTTON_LEFT;
      b.button.down = s.down;
      b.button.clicks = 1;
      b.button.x = s.pos.x;
      b.button.y = s.pos.y;
      push(b);
      injDown_ = s.down;
      injEraser_ = s.eraser;
    }
    {
      SDL_Event e{};
      e.pmotion.type = SDL_EVENT_PEN_MOTION;
      e.pmotion.timestamp = now;
      e.pmotion.windowID = wid;
      e.pmotion.which = 1;
      e.pmotion.pen_state = state;
      e.pmotion.x = s.pos.x;
      e.pmotion.y = s.pos.y;
      push(e);
      SDL_Event m{};
      m.motion.type = SDL_EVENT_MOUSE_MOTION;
      m.motion.timestamp = now;
      m.motion.windowID = wid;
      m.motion.which = SDL_PEN_MOUSEID;
      m.motion.x = s.pos.x;
      m.motion.y = s.pos.y;
      m.motion.state = s.down ? SDL_BUTTON_LMASK : 0;
      push(m);
    }
    if (s.button != injButton_) {
      // barrel buttons: SDL sends pen button events and a right (1) / middle (2) click
      for (int b = 1; b <= 2; ++b) {
        bool was = injButton_ == b, is = s.button == b;
        if (was == is) continue;
        SDL_Event e{};
        e.pbutton.type = is ? SDL_EVENT_PEN_BUTTON_DOWN : SDL_EVENT_PEN_BUTTON_UP;
        e.pbutton.timestamp = now;
        e.pbutton.windowID = wid;
        e.pbutton.which = 1;
        e.pbutton.button = (Uint8)b;
        e.pbutton.down = is;
        e.pbutton.x = s.pos.x;
        e.pbutton.y = s.pos.y;
        push(e);
        SDL_Event m{};
        m.button.type = is ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
        m.button.timestamp = now;
        m.button.windowID = wid;
        m.button.which = SDL_PEN_MOUSEID;
        m.button.button = b == 1 ? SDL_BUTTON_RIGHT : SDL_BUTTON_MIDDLE;
        m.button.down = is;
        m.button.clicks = 1;
        m.button.x = s.pos.x;
        m.button.y = s.pos.y;
        push(m);
      }
      injButton_ = s.button;
    }
  }
}

}  // namespace patina
