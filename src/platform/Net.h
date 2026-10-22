#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

namespace patina::net {

using json = nlohmann::json;

// Newline-delimited JSON over TCP, bound to 127.0.0.1 only.
// The handler runs on a connection thread and returns the response object.
class JsonLineServer {
 public:
  using Handler = std::function<json(const json& request)>;
  ~JsonLineServer() { stop(); }
  bool start(int port, Handler handler, std::string* err = nullptr);
  void stop();
  int port() const { return port_; }
  bool running() const { return running_; }
  int connectionCount() const { return connections_.load(); }

 private:
  void acceptLoop();
  void serve(std::intptr_t fd);
  std::intptr_t listenFd_ = -1;  // socket handle (SOCKET on Windows)
  int port_ = 0;
  Handler handler_;
  std::atomic<bool> running_{false};
  std::atomic<int> connections_{0};
  std::thread acceptThread_;
  std::mutex fdMutex_;
  std::vector<std::intptr_t> clientFds_;
  std::vector<std::thread> clientThreads_;
};

// Blocking client used by the MCP proxy and to push notifications to Blender.
class JsonLineClient {
 public:
  ~JsonLineClient() { close(); }
  bool connect(const std::string& host, int port, int timeoutMs = 2000, std::string* err = nullptr);
  void close();
  bool connected() const { return fd_ >= 0; }
  // Sends {"id":n,"cmd":cmd,"args":args} and waits for the matching response.
  bool call(const std::string& cmd, const json& args, json& response, int timeoutMs = 60000, std::string* err = nullptr);

 private:
  bool readLine(std::string& line, int timeoutMs);
  std::intptr_t fd_ = -1;
  int nextId_ = 1;
  std::string buffer_;
};

// Fire-and-forget request on a background thread (used for Blender pushes).
void callAsync(int port, const std::string& cmd, const json& args, std::function<void(bool ok, const json&)> done = {});

}  // namespace patina::net
