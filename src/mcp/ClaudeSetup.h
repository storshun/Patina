#pragma once
// Adds Patina to the Claude desktop app's MCP servers (claude_desktop_config.json), so Claude in the
// app can drive Patina without editing the file by hand.
#include <filesystem>
#include <string>
#include <vector>

namespace patina::mcp {

struct ClaudeSetupResult {
  bool ok = false;
  bool changed = false;                       // false: it was already set up this way
  std::vector<std::filesystem::path> files;   // config files written (or already right)
  std::string command;                        // the command Claude will start
  std::string message;                        // for the user
};

// Where the Claude desktop app keeps its configuration on this system (existing ones first).
std::vector<std::filesystem::path> claudeDesktopConfigs();
// The program the app should start: this one (patina-cli.exe next to patina.exe on Windows).
std::filesystem::path mcpCommand();
// Writes "mcpServers": {"patina": {"command": <mcpCommand>, "args": ["--mcp"]}} into the config,
// keeping everything else; the previous file is kept as claude_desktop_config.json.bak.
// `configOverride`: write this file instead (tests).
ClaudeSetupResult setupClaudeDesktop(const std::filesystem::path& configOverride = {});
// Whether the config already starts this Patina.
bool claudeDesktopHasPatina();

}  // namespace patina::mcp
