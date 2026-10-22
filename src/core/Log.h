#pragma once
#include <cstdarg>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace patina {

enum class LogLevel { Debug, Info, Warn, Error };

struct LogEntry {
  LogLevel level;
  double time;
  std::string text;
};

// Thread-safe logger with a ring buffer the UI console can display.
class Log {
 public:
  static void write(LogLevel lvl, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
  static void writev(LogLevel lvl, const char* fmt, va_list ap);
  static std::vector<LogEntry> snapshot(size_t maxEntries = 500);
  static uint64_t counter();  // increments on each write (cheap change detection)
  static void clear();        // (0.36) the console's Clear
  static void setQuiet(bool q);  // suppress stderr (used by MCP stdio mode? no: stderr is safe)
  static void setMinLevel(LogLevel l);
};

#define PLOG_D(...) ::patina::Log::write(::patina::LogLevel::Debug, __VA_ARGS__)
#define PLOG_I(...) ::patina::Log::write(::patina::LogLevel::Info, __VA_ARGS__)
#define PLOG_W(...) ::patina::Log::write(::patina::LogLevel::Warn, __VA_ARGS__)
#define PLOG_E(...) ::patina::Log::write(::patina::LogLevel::Error, __VA_ARGS__)

double nowSeconds();

}  // namespace patina
