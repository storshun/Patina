#pragma once
#include <filesystem>
#include <string>

namespace patina::paths {

namespace fs = std::filesystem;

fs::path exeDir();
fs::path exePath();     // this program
fs::path homeDir();      // $HOME, %USERPROFILE% on Windows
// Read-only data shipped with Patina (materials, brushes). Search order:
// $PATINA_DATA, <exe>/../share/patina, <exe>/../../ (build tree -> source),
// compile-time source dir, compile-time install dir.
fs::path dataDir();
fs::path userDataDir();   // $XDG_DATA_HOME/patina/user (~/.local/share/patina/user; kept apart from an
                          // install into ~/.local, whose read-only data lives in ~/.local/share/patina)
fs::path configDir();     // $XDG_CONFIG_HOME/patina
fs::path cacheDir();      // $XDG_CACHE_HOME/patina
fs::path bridgeDir();     // $PATINA_BRIDGE_DIR or cacheDir()/bridge
fs::path ensureDir(const fs::path& p);
std::string sanitizeFileName(const std::string& s);
std::string readFile(const fs::path& p, bool* ok = nullptr);
bool writeFileAtomic(const fs::path& p, const std::string& data);

}  // namespace patina::paths
