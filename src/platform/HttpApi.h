#pragma once
// HTTP/JSON API for other applications (pipelines, render farms, asset servers).
//
//   GET    /api/v1/health                 no token needed: version, busy
//   GET    /api/v1/info                   version, GPU, workspace, limits
//   GET    /api/v1/commands               every command with its JSON Schema
//   GET    /api/v1/openapi.json           OpenAPI 3 description (for client generators)
//   POST   /api/v1/commands/{name}        run one command now, body = arguments
//   POST   /api/v1/jobs                   queue a job: {"steps":[{"cmd":..,"args":..}],"name":..}
//   GET    /api/v1/jobs[/{id}][?wait=s]   status, progress, results (wait = long poll)
//   DELETE /api/v1/jobs/{id}              cancel (queued: dropped, running: stops at a safe point)
//   GET    /api/v1/files[?dir=]           list the workspace
//   PUT    /api/v1/files/{path}           upload into the workspace (raw body)
//   GET    /api/v1/files/{path}           download from the workspace
//   DELETE /api/v1/files/{path}
//
// Reach: 127.0.0.1 by default. With `lan`, the server listens on the network but only
// answers clients with private (RFC 1918 / link-local / ULA) or loopback addresses, or the
// ranges in `allow`; requests from public addresses are refused. A token is required
// on the network (generated and stored in the settings folder if none is given).
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace patina::net {

using json = nlohmann::json;

struct HttpApiOptions {
  int port = 7870;
  bool lan = false;                 // listen on the network (private clients only)
  std::string bind;                 // explicit interface address (overrides lan / localhost)
  std::vector<std::string> allow;   // CIDR ranges allowed instead of the private ranges
  std::string token;                // required on every request but /health when not empty
  bool requireToken = true;         // lan without a token: generate one
  std::string workspace;            // uploads / outputs; relative paths in arguments resolve here
  bool sandbox = false;             // paths in arguments must stay inside the workspace
  uint64_t maxUploadMB = 4096;
  std::vector<std::string> corsOrigins;  // browser origins allowed to call the API (default none)
};

// Is `addr` (IPv4 or IPv6 text) inside one of the CIDR ranges? Exposed for tests.
bool addressInRanges(const std::string& addr, const std::vector<std::string>& cidrs);
const std::vector<std::string>& privateRanges();

class HttpApi {
 public:
  // exec: runs {"cmd","args"} on the engine thread and returns {"ok",..,"result"|"error"}.
  // describe: the command list (name, description, inputSchema).
  using Exec = std::function<json(const json& request, int timeoutSeconds)>;
  using Describe = std::function<json()>;
  HttpApi();
  ~HttpApi();
  bool start(const HttpApiOptions& opt, Exec exec, Describe describe, std::string* err = nullptr);
  void stop();
  bool running() const;
  int port() const;
  std::string url() const;          // how to reach it (for the log / UI)
  std::string token() const;
  std::string workspace() const;
  std::string tokenFile() const;    // where a generated token was stored ("" if given)
  int jobsQueued() const;
  json status() const;              // for the UI: running, url, jobs, current job

 private:
  struct Impl;
  std::unique_ptr<Impl> d_;
};

}  // namespace patina::net
