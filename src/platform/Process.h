#pragma once
// 0.30: a child process (the batch runner: a hidden `patina run`), started detached from the window's console.
#include <cstdint>
#include <string>
#include <vector>

namespace patina {

class ChildProcess {
 public:
  ChildProcess() = default;
  ~ChildProcess();
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;
  // Starts `exe args...` (no window, output to nothing). False with the reason in err.
  bool start(const std::string& exe, const std::vector<std::string>& args, std::string* err);
  bool running();       // not finished yet
  int exitCode() const { return exit_; }  // once finished
  void kill();
  bool started() const { return started_; }

 private:
  bool started_ = false, done_ = false;
  int exit_ = -1;
#ifdef _WIN32
  void* handle_ = nullptr;
#else
  int pid_ = -1;
#endif
};

// This Patina's own executable for headless work (patina-cli.exe next to patina.exe on Windows)
std::string selfExecutable();

}  // namespace patina
