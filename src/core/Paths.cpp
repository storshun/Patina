#include "core/Paths.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>

namespace patina::paths {

static fs::path envPath(const char* var) {
  const char* v = std::getenv(var);
  return v && *v ? fs::path(v) : fs::path();
}

#ifdef _WIN32
static fs::path home() {
  auto h = envPath("USERPROFILE");
  return h.empty() ? fs::temp_directory_path() : h;
}
// Windows: %LOCALAPPDATA%\Patina (data, cache) and %APPDATA%\Patina (settings)
static fs::path localAppData() {
  auto p = envPath("LOCALAPPDATA");
  return (p.empty() ? home() / "AppData" / "Local" : p) / "Patina";
}

fs::path exeDir() {
  wchar_t buf[32768];
  DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof buf / sizeof *buf));
  if (n == 0) return fs::current_path();
  return fs::path(std::wstring(buf, n)).parent_path();
}
fs::path exePath() {
  wchar_t buf[32768];
  DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof buf / sizeof *buf));
  return n ? fs::path(std::wstring(buf, n)) : fs::path("patina.exe");
}
#else
static fs::path home() {
  auto h = envPath("HOME");
  return h.empty() ? fs::path("/tmp") : h;
}

fs::path exeDir() {
  char buf[4096];
  ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
  if (n <= 0) return fs::current_path();
  buf[n] = 0;
  return fs::path(buf).parent_path();
}
fs::path exePath() {
  char buf[4096];
  ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
  if (n <= 0) return fs::path("patina");
  buf[n] = 0;
  return fs::path(buf);
}
#endif

fs::path homeDir() { return home(); }

fs::path dataDir() {
  static fs::path cached;
  if (!cached.empty()) return cached;
  std::vector<fs::path> candidates;
  if (auto p = envPath("PATINA_DATA"); !p.empty()) candidates.push_back(p);
  fs::path e = exeDir();
  candidates.push_back(e);                 // Windows zip / portable layout: materials next to the exe
  candidates.push_back(e / "../share/patina");
  candidates.push_back(e / "..");      // build/ inside the source tree
  candidates.push_back(e / "../..");
#ifdef PATINA_SOURCE_DIR
  candidates.push_back(PATINA_SOURCE_DIR);
#endif
#ifdef PATINA_INSTALL_DATADIR
  candidates.push_back(PATINA_INSTALL_DATADIR);
#endif
  for (auto& c : candidates) {
    std::error_code ec;
    if (fs::exists(c / "materials", ec)) {
      cached = fs::weakly_canonical(c, ec);
      return cached;
    }
  }
  cached = e;
  return cached;
}

#ifdef _WIN32
fs::path userDataDir() { return ensureDir(localAppData() / "user"); }
fs::path configDir() {
  auto p = envPath("APPDATA");
  return ensureDir((p.empty() ? home() / "AppData" / "Roaming" : p) / "Patina");
}
fs::path cacheDir() { return ensureDir(localAppData() / "cache"); }
#else
fs::path userDataDir() {
  auto x = envPath("XDG_DATA_HOME");
  return ensureDir((x.empty() ? home() / ".local/share" : x) / "patina" / "user");
}

fs::path configDir() {
  auto x = envPath("XDG_CONFIG_HOME");
  return ensureDir((x.empty() ? home() / ".config" : x) / "patina");
}

fs::path cacheDir() {
  auto x = envPath("XDG_CACHE_HOME");
  return ensureDir((x.empty() ? home() / ".cache" : x) / "patina");
}
#endif

fs::path bridgeDir() {
  auto b = envPath("PATINA_BRIDGE_DIR");
  return ensureDir(b.empty() ? cacheDir() / "bridge" : b);
}

fs::path ensureDir(const fs::path& p) {
  std::error_code ec;
  fs::create_directories(p, ec);
  return p;
}

std::string sanitizeFileName(const std::string& s) {
  std::string o;
  for (unsigned char c : s) {
    if (c < 32 || std::string("\\/:*?\"<>|").find((char)c) != std::string::npos) o.push_back('_');
    else o.push_back((char)c);
  }
  if (o.empty()) o = "untitled";
  return o;
}

std::string readFile(const fs::path& p, bool* ok) {
  std::ifstream f(p, std::ios::binary);
  if (ok) *ok = (bool)f;
  if (!f) return {};
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

bool writeFileAtomic(const fs::path& p, const std::string& data) {
  fs::path tmp = p;
  tmp += ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary);
    if (!f) return false;
    f.write(data.data(), data.size());
    if (!f) return false;
  }
  std::error_code ec;
  fs::rename(tmp, p, ec);  // replaces atomically on POSIX; MoveFileEx(REPLACE_EXISTING) on Windows
  if (ec) fs::remove(tmp, ec);
  return !ec;
}

}  // namespace patina::paths
