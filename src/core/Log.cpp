#include "core/Log.h"

#include <atomic>
#include <chrono>
#include <cstdio>

namespace patina {

namespace {
std::mutex g_mutex;
std::deque<LogEntry> g_entries;
std::atomic<uint64_t> g_counter{0};
std::atomic<bool> g_quiet{false};
std::atomic<int> g_minLevel{(int)LogLevel::Info};
const auto g_start = std::chrono::steady_clock::now();
}  // namespace

double nowSeconds() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
}

void Log::write(LogLevel lvl, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  writev(lvl, fmt, ap);
  va_end(ap);
}

void Log::writev(LogLevel lvl, const char* fmt, va_list ap) {
  char buf[4096];
  vsnprintf(buf, sizeof buf, fmt, ap);
  if ((int)lvl >= g_minLevel.load() && !g_quiet.load()) {
    static const char* tags[] = {"debug", "info", "warn", "error"};
    // stderr only: stdout is reserved for the MCP protocol in --mcp mode.
    fprintf(stderr, "[patina %s] %s\n", tags[(int)lvl], buf);
  }
  std::lock_guard<std::mutex> lk(g_mutex);
  g_entries.push_back({lvl, nowSeconds(), buf});
  while (g_entries.size() > 2000) g_entries.pop_front();
  g_counter++;
}

std::vector<LogEntry> Log::snapshot(size_t maxEntries) {
  std::lock_guard<std::mutex> lk(g_mutex);
  size_t n = std::min(maxEntries, g_entries.size());
  return std::vector<LogEntry>(g_entries.end() - n, g_entries.end());
}

uint64_t Log::counter() { return g_counter.load(); }
void Log::clear() {
  std::lock_guard<std::mutex> lk(g_mutex);
  g_entries.clear();
  g_counter++;
}
void Log::setQuiet(bool q) { g_quiet = q; }
void Log::setMinLevel(LogLevel l) { g_minLevel = (int)l; }

}  // namespace patina
