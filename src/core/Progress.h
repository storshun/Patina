#pragma once
// Process-wide progress of the long operation running on the engine thread (bakes,
// renders, exports), read by the HTTP job API from other threads. Operations report a
// fraction and poll cancelled(); everything is lock-free except the label.
#include <string>

namespace patina::progress {

void begin(const std::string& label);  // fraction = 0
void set(float fraction);              // 0..1 of the current operation
void end();                            // no operation running
float current();                       // -1 when nothing reports progress
std::string label();
// Cancel: the job API asks, operations that check cancelled() stop early.
void requestCancel();
void clearCancel();
bool cancelled();

struct Scope {
  explicit Scope(const std::string& label) { begin(label); }
  ~Scope() { end(); }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;
};

}  // namespace patina::progress
