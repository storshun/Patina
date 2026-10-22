#pragma once
#include <functional>
#include <iosfwd>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>

namespace patina {

using json = nlohmann::json;

// Model Context Protocol server over stdio (JSON-RPC 2.0, one message per line).
// Tools are the Patina command registry; the backend executes a command and
// returns {"ok":bool,"result":...,"error":...}.
class McpServer {
 public:
  using Backend = std::function<json(const std::string& cmd, const json& args)>;
  using ToolLister = std::function<json()>;  // array of {name, description, inputSchema}

  McpServer(Backend backend, ToolLister lister) : backend_(std::move(backend)), lister_(std::move(lister)) {}
  // Extra tools handled locally (e.g. open_gui_window). Return nullopt-like null to fall through.
  std::function<json(const std::string& name, const json& args, bool& handled)> localTool;
  json extraTools = json::array();

  // Blocking loop until stdin closes.
  void run(std::istream& in, std::ostream& out);
  json handle(const json& msg);  // returns null for notifications

  static json toolResult(const json& backendResponse);
  static const char* instructions();
  // (0.34) guides (patina_guide tool, patina://guide/<id> resources); "parameters" is written from the tool list
  std::string guideText(const std::string& id) const;
  static json guideTool();

 private:
  Backend backend_;
  ToolLister lister_;
  std::mutex outMutex_;
};

}  // namespace patina
