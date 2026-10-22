#include "mcp/ClaudeSetup.h"

#include <cstdlib>
#include <nlohmann/json.hpp>

#include "core/Log.h"
#include "core/Paths.h"

namespace patina::mcp {

namespace fs = std::filesystem;
using json = nlohmann::ordered_json;

static fs::path env(const char* v) {
  const char* s = std::getenv(v);
  return s && *s ? fs::path(s) : fs::path();
}

std::vector<fs::path> claudeDesktopConfigs() {
  std::vector<fs::path> dirs;
#ifdef _WIN32
  fs::path roaming = env("APPDATA");
  if (roaming.empty()) roaming = paths::homeDir() / "AppData" / "Roaming";
  dirs.push_back(roaming / "Claude");
  // the Microsoft Store build keeps its settings in its package folder
  fs::path packages = env("LOCALAPPDATA");
  if (packages.empty()) packages = paths::homeDir() / "AppData" / "Local";
  packages /= "Packages";
  std::error_code ec;
  if (fs::is_directory(packages, ec))
    for (auto& e : fs::directory_iterator(packages, ec)) {
      std::string n = e.path().filename().string();
      if (n.rfind("Claude_", 0) == 0 || n.rfind("AnthropicPBC.Claude", 0) == 0)
        dirs.push_back(e.path() / "LocalCache" / "Roaming" / "Claude");
    }
#elif defined(__APPLE__)
  dirs.push_back(paths::homeDir() / "Library" / "Application Support" / "Claude");
#else
  fs::path x = env("XDG_CONFIG_HOME");
  dirs.push_back((x.empty() ? paths::homeDir() / ".config" : x) / "Claude");
#endif
  std::vector<fs::path> out, missing;
  for (auto& d : dirs) {
    std::error_code ec;
    (fs::is_directory(d, ec) ? out : missing).push_back(d / "claude_desktop_config.json");
  }
  // where the app has never run: the standard place (created)
  if (out.empty() && !missing.empty()) out.push_back(missing.front());
  return out;
}

fs::path mcpCommand() {
  fs::path exe = paths::exePath();
#ifdef _WIN32
  // patina.exe is a windowed program without a console: Claude needs the console twin for stdio
  fs::path cli = exe.parent_path() / "patina-cli.exe";
  std::error_code ec;
  if (fs::exists(cli, ec)) return cli;
#endif
  return exe;
}

static bool entryIs(const json& cfg, const std::string& cmd) {
  if (!cfg.is_object() || !cfg.contains("mcpServers") || !cfg["mcpServers"].is_object()) return false;
  const json& s = cfg["mcpServers"];
  if (!s.contains("patina") || !s["patina"].is_object()) return false;
  const json& p = s["patina"];
  return p.value("command", std::string()) == cmd && p.contains("args") && p["args"] == json::array({"--mcp"});
}

ClaudeSetupResult setupClaudeDesktop(const fs::path& configOverride) {
  ClaudeSetupResult r;
  r.command = mcpCommand().string();
  std::vector<fs::path> files = configOverride.empty() ? claudeDesktopConfigs() : std::vector<fs::path>{configOverride};
  std::string problems;
  for (const fs::path& file : files) {
    json cfg = json::object();
    std::error_code ec;
    bool exists = fs::exists(file, ec);
    std::string old;
    if (exists) {
      bool ok = false;
      old = paths::readFile(file, &ok);
      if (!ok) {
        problems += "Could not read " + file.string() + ". ";
        continue;
      }
      if (!old.empty()) {
        try {
          cfg = json::parse(old);
        } catch (const std::exception& e) {
          // never overwrite a file we can't understand: the user's other servers are in it
          problems += file.string() + " is not valid JSON (" + e.what() + "); fix or remove it first. ";
          continue;
        }
      }
      if (!cfg.is_object()) {
        problems += file.string() + " does not hold a JSON object. ";
        continue;
      }
    }
    if (entryIs(cfg, r.command)) {
      r.files.push_back(file);
      continue;
    }
    if (!cfg.contains("mcpServers") || !cfg["mcpServers"].is_object()) cfg["mcpServers"] = json::object();
    cfg["mcpServers"]["patina"] = {{"command", r.command}, {"args", json::array({"--mcp"})}};
    fs::create_directories(file.parent_path(), ec);
    if (exists && !old.empty()) paths::writeFileAtomic(fs::path(file.string() + ".bak"), old);
    if (!paths::writeFileAtomic(file, cfg.dump(2) + "\n")) {
      problems += "Could not write " + file.string() + ". ";
      continue;
    }
    r.files.push_back(file);
    r.changed = true;
    PLOG_I("Claude desktop app: Patina added to %s", file.string().c_str());
  }
  r.ok = !r.files.empty() && problems.empty();
  if (r.files.empty()) {
    r.message = problems.empty() ? "No Claude desktop configuration found." : problems;
  } else if (r.changed) {
    r.message = "Patina is now in the Claude app's MCP servers (" + r.files.front().string() +
                "). Quit the Claude app completely and open it again; then ask Claude to use Patina. " + problems;
  } else {
    r.message = "The Claude app is already set up to start this Patina (" + r.files.front().string() +
                "). If Claude doesn't see it, quit the app completely and open it again. " + problems;
  }
  return r;
}

bool claudeDesktopHasPatina() {
  std::string cmd = mcpCommand().string();
  for (const fs::path& f : claudeDesktopConfigs()) {
    bool ok = false;
    std::string s = paths::readFile(f, &ok);
    if (!ok || s.empty()) continue;
    try {
      if (entryIs(json::parse(s), cmd)) return true;
    } catch (...) {
    }
  }
  return false;
}

}  // namespace patina::mcp
