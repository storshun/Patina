#include "platform/HttpApi.h"

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#endif
#include <httplib.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <thread>

#include "core/Log.h"
#include "core/Paths.h"
#include "core/Progress.h"

namespace patina::net {
namespace fs = std::filesystem;

// ------------------------------------------------------------------ addresses

namespace {

// 16-byte address; IPv4 is stored IPv4-mapped (::ffff:a.b.c.d)
bool parseAddr(std::string s, std::array<uint8_t, 16>& out) {
  if (!s.empty() && s.front() == '[') s = s.substr(1, s.find(']') - 1);
  if (auto pct = s.find('%'); pct != std::string::npos) s = s.substr(0, pct);  // fe80::1%eth0
  in_addr a4{};
  if (inet_pton(AF_INET, s.c_str(), &a4) == 1) {
    out.fill(0);
    out[10] = out[11] = 0xff;
    std::memcpy(&out[12], &a4, 4);
    return true;
  }
  in6_addr a6{};
  if (inet_pton(AF_INET6, s.c_str(), &a6) == 1) {
    std::memcpy(out.data(), &a6, 16);
    return true;
  }
  return false;
}

bool inCidr(const std::array<uint8_t, 16>& a, const std::string& cidr) {
  std::string net = cidr;
  int bits = -1;
  if (auto sl = cidr.find('/'); sl != std::string::npos) {
    net = cidr.substr(0, sl);
    bits = std::atoi(cidr.c_str() + sl + 1);
  }
  std::array<uint8_t, 16> n;
  if (!parseAddr(net, n)) return false;
  bool v4 = net.find(':') == std::string::npos;
  if (bits < 0) bits = v4 ? 32 : 128;
  if (v4) bits += 96;
  bits = std::clamp(bits, 0, 128);
  for (int i = 0; i < bits; ++i) {
    int byte = i / 8, bit = 7 - i % 8;
    if (((a[byte] >> bit) & 1) != ((n[byte] >> bit) & 1)) return false;
  }
  return true;
}

std::string randomHex(int bytes) {
  std::random_device rd;
  std::string s;
  static const char* hx = "0123456789abcdef";
  for (int i = 0; i < bytes; ++i) {
    unsigned v = rd() & 0xff;
    s += hx[v >> 4];
    s += hx[v & 15];
  }
  return s;
}

bool constantTimeEq(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  unsigned char d = 0;
  for (size_t i = 0; i < a.size(); ++i) d |= (unsigned char)(a[i] ^ b[i]);
  return d == 0;
}

double now() {
  using namespace std::chrono;
  return duration<double>(system_clock::now().time_since_epoch()).count();
}

// Commands that only read state: they run even while a job holds the engine.
const std::set<std::string>& readOnlyCommands() {
  static const std::set<std::string> s = {"ping", "get_state", "get_log", "list_commands", "list_materials",
                                          "list_brushes", "list_layers", "list_smart_materials", "uv_info",
                                          "history", "get_material"};
  return s;
}

bool isPathKey(const std::string& k) {
  auto ends = [&](const char* suf) {
    size_t n = std::strlen(suf);
    return k.size() >= n && k.compare(k.size() - n, n, suf) == 0;
  };
  return k == "path" || k == "directory" || k == "folder" || ends("_path") || ends("_dir") || ends("_directory");
}

}  // namespace

const std::vector<std::string>& privateRanges() {
  static const std::vector<std::string> r = {"127.0.0.0/8",    "10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16",
                                             "169.254.0.0/16", "::1/128",    "fc00::/7",      "fe80::/10"};
  return r;
}

bool addressInRanges(const std::string& addr, const std::vector<std::string>& cidrs) {
  std::array<uint8_t, 16> a;
  if (!parseAddr(addr, a)) return false;
  for (auto& c : cidrs)
    if (inCidr(a, c)) return true;
  return false;
}

// ------------------------------------------------------------------ implementation

struct Job {
  std::string id, name;
  json steps = json::array();
  bool stopOnError = true;
  std::string status = "queued";  // queued running done failed cancelled
  json results = json::array();
  std::string error;
  double created = 0, started = 0, finished = 0;
  int currentStep = -1;
  std::atomic<bool> cancel{false};
};

struct HttpApi::Impl {
  HttpApiOptions opt;
  Exec exec;
  Describe describe;
  httplib::Server svr;
  std::thread listenThread, runnerThread;
  std::atomic<bool> up{false};
  int port = 0;
  std::string host;
  std::string tokenFile;
  fs::path ws;
  std::vector<std::string> allowed;
  double startedAt = 0;

  // jobs
  mutable std::mutex jm;
  std::condition_variable jcv;       // runner wakes
  std::condition_variable doneCv;    // long-poll waiters
  std::map<std::string, std::shared_ptr<Job>> jobs;
  std::deque<std::string> order;     // creation order (retention)
  std::deque<std::shared_ptr<Job>> queue;
  std::shared_ptr<Job> running;
  bool quit = false;
  int counter = 0;
  // the engine is used by one job, or one non-read-only request, at a time
  std::timed_mutex engineMutex;

  // ---- helpers
  static void reply(httplib::Response& res, int status, const json& j) {
    res.status = status;
    res.set_content(j.dump(), "application/json");
  }
  static void fail(httplib::Response& res, int status, const std::string& msg) {
    reply(res, status, json{{"ok", false}, {"error", msg}});
  }

  // workspace-relative resolution; throws std::runtime_error when sandboxed and outside
  std::string resolve(const std::string& p) const {
    fs::path in(p);
    fs::path abs = in.is_absolute() ? in : ws / in;
    abs = abs.lexically_normal();
    if (opt.sandbox) {
      auto rel = abs.lexically_relative(ws);
      bool inside = abs == ws || (!rel.empty() && rel.string().rfind("..", 0) != 0);
      if (!inside) throw std::runtime_error("path outside the workspace (the server runs with --sandbox): " + p);
    }
    return abs.string();
  }
  // every file name in the arguments, nested ones too (brush tip images, material maps)
  void rewriteTree(json& j, bool allStrings) const {
    if (j.is_object()) {
      for (auto it = j.begin(); it != j.end(); ++it) {
        json& v = it.value();
        if (v.is_string()) {
          if (allStrings || isPathKey(it.key()) || it.key() == "image") {
            std::string s = v;
            if (!s.empty()) v = resolve(s);
          }
        } else {
          rewriteTree(v, it.key() == "maps");
        }
      }
    } else if (j.is_array()) {
      for (auto& v : j)
        if (!v.is_string()) rewriteTree(v, false);
    }
  }
  void rewritePaths(const std::string& cmd, json& args) const {
    if (!args.is_object()) return;
    rewriteTree(args, false);
    if (cmd == "apply_smart_material" && args.contains("id") && args["id"].is_string()) {
      std::string v = args["id"];
      if (v.size() > 5 && v.compare(v.size() - 5, 5, ".json") == 0) args["id"] = resolve(v);
    }
  }
  // file endpoint paths are always confined to the workspace
  bool wsPath(const std::string& rel, fs::path& out) const {
    fs::path p = (ws / fs::path(rel)).lexically_normal();
    auto r = p.lexically_relative(ws);
    if (r.empty() && p != ws) return false;
    if (!r.empty() && r.string().rfind("..", 0) == 0) return false;
    out = p;
    return true;
  }

  json jobJson(const Job& j, bool withResults = true) const {
    json o = {{"id", j.id},
              {"name", j.name},
              {"status", j.status},
              {"created", j.created},
              {"started", j.started > 0 ? json(j.started) : json(nullptr)},
              {"finished", j.finished > 0 ? json(j.finished) : json(nullptr)},
              {"steps", j.steps.size()},
              {"current_step", j.currentStep},
              {"url", "/api/v1/jobs/" + j.id}};
    if (j.status == "running") {
      float p = progress::current();
      size_t n = std::max<size_t>(1, j.steps.size());
      o["progress"] = (double(std::max(0, j.currentStep)) + (p >= 0 ? p : 0.0)) / double(n);
      std::string lbl = progress::label();
      o["label"] = lbl.empty() && j.currentStep >= 0 && j.currentStep < (int)j.steps.size()
                       ? j.steps[j.currentStep].value("cmd", std::string())
                       : lbl;
    } else {
      o["progress"] = j.status == "done" ? 1.0 : 0.0;
    }
    if (!j.error.empty()) o["error"] = j.error;
    if (withResults) o["results"] = j.results;
    return o;
  }

  void runJobs() {
    for (;;) {
      std::shared_ptr<Job> j;
      {
        std::unique_lock<std::mutex> lk(jm);
        jcv.wait(lk, [&] { return quit || !queue.empty(); });
        if (quit) return;
        j = queue.front();
        queue.pop_front();
        if (j->cancel) continue;
        running = j;
        j->status = "running";
        j->started = now();
      }
      std::unique_lock<std::timed_mutex> eng(engineMutex);
      progress::clearCancel();
      bool failed = false;
      for (size_t i = 0; i < j->steps.size(); ++i) {
        {
          std::lock_guard<std::mutex> lk(jm);
          j->currentStep = (int)i;
        }
        if (j->cancel) break;
        json step = j->steps[i];
        std::string cmd = step.value("cmd", std::string());
        json args = step.value("args", json::object());
        json r;
        double t0 = now();
        try {
          rewritePaths(cmd, args);
          r = exec({{"cmd", cmd}, {"args", args}}, 4 * 3600);
        } catch (std::exception& e) {
          r = {{"ok", false}, {"error", e.what()}};
        }
        json entry = {{"cmd", cmd}, {"ok", r.value("ok", false)}, {"seconds", now() - t0}};
        if (r.value("ok", false)) entry["result"] = r.value("result", json());
        else entry["error"] = r.value("error", std::string("failed"));
        {
          std::lock_guard<std::mutex> lk(jm);
          j->results.push_back(entry);
          if (!entry["ok"].get<bool>()) {
            if (j->error.empty()) j->error = "step " + std::to_string(i + 1) + " (" + cmd + "): " + entry["error"].get<std::string>();
            if (j->stopOnError) failed = true;
          }
        }
        if (failed) break;
      }
      progress::clearCancel();
      eng.unlock();
      {
        std::lock_guard<std::mutex> lk(jm);
        j->status = j->cancel ? "cancelled" : failed || !j->error.empty() ? "failed" : "done";
        j->finished = now();
        running.reset();
        PLOG_I("API job %s (%s): %s in %.1f s", j->id.c_str(), j->name.c_str(), j->status.c_str(),
               j->finished - j->started);
      }
      doneCv.notify_all();
    }
  }

  std::shared_ptr<Job> addJob(const json& body, std::string* err) {
    auto j = std::make_shared<Job>();
    if (body.contains("steps")) j->steps = body["steps"];
    else if (body.contains("cmd")) j->steps = json::array({{{"cmd", body["cmd"]}, {"args", body.value("args", json::object())}}});
    if (!j->steps.is_array() || j->steps.empty()) {
      *err = "a job needs \"steps\": [{\"cmd\": ..., \"args\": {...}}, ...] (or \"cmd\" and \"args\")";
      return nullptr;
    }
    for (auto& s : j->steps) {
      if (!s.is_object() || !s.contains("cmd") || !s["cmd"].is_string()) {
        *err = "every step needs a \"cmd\"";
        return nullptr;
      }
      if (s.contains("args") && !s["args"].is_object()) {
        *err = "\"args\" must be an object (step " + s["cmd"].get<std::string>() + ")";
        return nullptr;
      }
    }
    j->name = body.value("name", std::string());
    j->stopOnError = body.value("stop_on_error", true);
    j->created = now();
    std::lock_guard<std::mutex> lk(jm);
    j->id = "j" + std::to_string(++counter) + "-" + randomHex(3);
    jobs[j->id] = j;
    order.push_back(j->id);
    while (order.size() > 500) {
      auto it = jobs.find(order.front());
      if (it != jobs.end() && (it->second->status == "queued" || it->second->status == "running")) break;
      jobs.erase(order.front());
      order.pop_front();
    }
    queue.push_back(j);
    jcv.notify_all();
    return j;
  }

  json openapi() const {
    json cmds = describe();
    json paths = json::object();
    auto okResp = [](const std::string& d) {
      return json{{"200", {{"description", d}, {"content", {{"application/json", {{"schema", {{"$ref", "#/components/schemas/Result"}}}}}}}}},
                  {"400", {{"description", "the command failed (see error)"}}},
                  {"401", {{"description", "missing or wrong token"}}},
                  {"409", {{"description", "busy: a job is using the engine"}}}};
    };
    for (auto& c : cmds) {
      std::string n = c.value("name", std::string());
      json schema = c.value("inputSchema", json::object());
      paths["/api/v1/commands/" + n] = {
          {"post",
           {{"operationId", n},
            {"summary", c.value("description", std::string()).substr(0, 120)},
            {"description", c.value("description", std::string())},
            {"tags", {"commands"}},
            {"requestBody", {{"required", false}, {"content", {{"application/json", {{"schema", schema}}}}}}},
            {"responses", okResp("result of the command")}}}};
    }
    paths["/api/v1/health"] = {{"get", {{"operationId", "health"}, {"tags", {"server"}}, {"security", json::array()},
                                        {"responses", {{"200", {{"description", "server is up"}}}}}}}};
    paths["/api/v1/info"] = {{"get", {{"operationId", "info"}, {"tags", {"server"}}, {"responses", {{"200", {{"description", "server information"}}}}}}}};
    paths["/api/v1/jobs"] = {
        {"get", {{"operationId", "list_jobs"}, {"tags", {"jobs"}}, {"responses", {{"200", {{"description", "jobs, newest first"}}}}}}},
        {"post",
         {{"operationId", "create_job"},
          {"tags", {"jobs"}},
          {"requestBody", {{"required", true}, {"content", {{"application/json", {{"schema", {{"$ref", "#/components/schemas/JobRequest"}}}}}}}}},
          {"responses", {{"202", {{"description", "queued"}}}, {"400", {{"description", "bad job"}}}}}}}};
    paths["/api/v1/jobs/{id}"] = {
        {"parameters", {{{"name", "id"}, {"in", "path"}, {"required", true}, {"schema", {{"type", "string"}}}},
                        {{"name", "wait"}, {"in", "query"}, {"schema", {{"type", "number"}}}, {"description", "seconds to wait for the job to finish"}}}},
        {"get", {{"operationId", "get_job"}, {"tags", {"jobs"}}, {"responses", {{"200", {{"description", "job status"}}}, {"404", {{"description", "no such job"}}}}}}},
        {"delete", {{"operationId", "cancel_job"}, {"tags", {"jobs"}}, {"responses", {{"200", {{"description", "cancel requested"}}}}}}}};
    paths["/api/v1/files/{path}"] = {
        {"parameters", {{{"name", "path"}, {"in", "path"}, {"required", true}, {"schema", {{"type", "string"}}}}}},
        {"get", {{"operationId", "download_file"}, {"tags", {"files"}}, {"responses", {{"200", {{"description", "file content"}}}}}}},
        {"put", {{"operationId", "upload_file"}, {"tags", {"files"}},
                 {"requestBody", {{"content", {{"application/octet-stream", {{"schema", {{"type", "string"}, {"format", "binary"}}}}}}}}},
                 {"responses", {{"201", {{"description", "stored"}}}}}}},
        {"delete", {{"operationId", "delete_file"}, {"tags", {"files"}}, {"responses", {{"200", {{"description", "deleted"}}}}}}}};
    paths["/api/v1/files"] = {{"get", {{"operationId", "list_files"}, {"tags", {"files"}}, {"responses", {{"200", {{"description", "workspace listing"}}}}}}}};
    return {{"openapi", "3.0.3"},
            {"info", {{"title", "Patina API"}, {"version", PATINA_VERSION},
                      {"description", "Texture painting, baking and export as a service. Every command of the "
                                      "Patina automation API is a POST endpoint; long work goes through jobs."}}},
            {"servers", {{{"url", "/"}}}},
            {"security", {{{"bearer", json::array()}}}},
            {"components",
             {{"securitySchemes", {{"bearer", {{"type", "http"}, {"scheme", "bearer"}}}}},
              {"schemas",
               {{"Result", {{"type", "object"}, {"properties", {{"ok", {{"type", "boolean"}}}, {"result", json::object()}, {"error", {{"type", "string"}}}}}}},
                {"JobRequest",
                 {{"type", "object"},
                  {"properties",
                   {{"name", {{"type", "string"}}},
                    {"stop_on_error", {{"type", "boolean"}, {"default", true}}},
                    {"steps", {{"type", "array"}, {"items", {{"type", "object"}, {"properties", {{"cmd", {{"type", "string"}}}, {"args", {{"type", "object"}}}}}, {"required", {"cmd"}}}}}}}}}}}}}},
            {"paths", paths}};
  }

  void routes() {
    svr.set_pre_routing_handler([this](const httplib::Request& req, httplib::Response& res) {
      // 1. only the local network (or the configured ranges)
      if (!addressInRanges(req.remote_addr, allowed)) {
        PLOG_W("API: refused %s %s from %s (not a local-network address)", req.method.c_str(), req.path.c_str(),
               req.remote_addr.c_str());
        fail(res, 403, "Patina only answers requests from the local network");
        return httplib::Server::HandlerResponse::Handled;
      }
      // 2. browsers: only configured origins (stops web pages from driving the API)
      std::string origin = req.get_header_value("Origin");
      if (!origin.empty()) {
        bool ok = std::find(opt.corsOrigins.begin(), opt.corsOrigins.end(), origin) != opt.corsOrigins.end() ||
                  std::find(opt.corsOrigins.begin(), opt.corsOrigins.end(), "*") != opt.corsOrigins.end();
        if (!ok) {
          fail(res, 403, "requests from web pages are not allowed (start Patina with --cors-origin " + origin + ")");
          return httplib::Server::HandlerResponse::Handled;
        }
        res.set_header("Access-Control-Allow-Origin", origin);
        res.set_header("Access-Control-Allow-Headers", "Authorization, Content-Type, X-Patina-Token");
        res.set_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
        if (req.method == "OPTIONS") {
          res.status = 204;
          return httplib::Server::HandlerResponse::Handled;
        }
      }
      // 3. on localhost, the Host header must name this machine (DNS rebinding)
      if (opt.bind.empty() && !opt.lan) {
        std::string h = req.get_header_value("Host");
        std::string hn = h;
        if (!h.empty() && h[0] == '[') hn = h.substr(0, h.find(']') + 1);
        else if (auto c = h.rfind(':'); c != std::string::npos) hn = h.substr(0, c);
        if (!h.empty() && hn != "localhost" && hn != "127.0.0.1" && hn != "[::1]") {
          fail(res, 403, "unexpected Host header");
          return httplib::Server::HandlerResponse::Handled;
        }
      }
      // 4. token
      if (!opt.token.empty() && req.path != "/api/v1/health") {
        std::string t = req.get_header_value("X-Patina-Token");
        std::string auth = req.get_header_value("Authorization");
        if (t.empty() && auth.rfind("Bearer ", 0) == 0) t = auth.substr(7);
        if (!constantTimeEq(t, opt.token)) {
          fail(res, 401, "missing or wrong token (Authorization: Bearer <token>)");
          return httplib::Server::HandlerResponse::Handled;
        }
      }
      return httplib::Server::HandlerResponse::Unhandled;
    });

    svr.Get("/api/v1/health", [this](const httplib::Request&, httplib::Response& res) {
      std::lock_guard<std::mutex> lk(jm);
      reply(res, 200, {{"ok", true}, {"name", "patina"}, {"version", PATINA_VERSION}, {"busy", running != nullptr},
                       {"queued", queue.size()}, {"uptime", now() - startedAt}});
    });
    svr.Get("/api/v1/info", [this](const httplib::Request&, httplib::Response& res) {
      json st = exec({{"cmd", "get_state"}, {"args", json::object()}}, 30);
      reply(res, 200, {{"ok", true},
                       {"version", PATINA_VERSION},
                       {"workspace", ws.string()},
                       {"sandbox", opt.sandbox},
                       {"network", opt.lan || !opt.bind.empty() ? "local network" : "this machine"},
                       {"max_upload_mb", opt.maxUploadMB},
                       {"commands", describe().size()},
                       {"state", st.value("result", json())}});
    });
    svr.Get("/api/v1/commands", [this](const httplib::Request&, httplib::Response& res) {
      reply(res, 200, {{"ok", true}, {"result", describe()}});
    });
    svr.Get("/api/v1/openapi.json", [this](const httplib::Request&, httplib::Response& res) { reply(res, 200, openapi()); });

    svr.Post(R"(/api/v1/commands/([A-Za-z0-9_]+))", [this](const httplib::Request& req, httplib::Response& res) {
      std::string cmd = req.matches[1];
      bool known = false;
      for (auto& c : describe())
        if (c.value("name", std::string()) == cmd) known = true;
      if (!known) return fail(res, 404, "unknown command '" + cmd + "' (GET /api/v1/commands lists them)");
      json args = json::object();
      if (!req.body.empty()) {
        try {
          args = json::parse(req.body);
        } catch (std::exception& e) {
          return fail(res, 400, std::string("body is not JSON: ") + e.what());
        }
        if (!args.is_object()) return fail(res, 400, "the body must be a JSON object of arguments");
      }
      try {
        rewritePaths(cmd, args);
      } catch (std::exception& e) {
        return fail(res, 403, e.what());
      }
      int timeout = req.has_param("timeout") ? std::max(1, std::atoi(req.get_param_value("timeout").c_str())) : 600;
      std::unique_lock<std::timed_mutex> eng(engineMutex, std::defer_lock);
      if (!readOnlyCommands().count(cmd)) {
        int busyWait = req.has_param("busy_wait") ? std::max(0, std::atoi(req.get_param_value("busy_wait").c_str())) : 30;
        if (!eng.try_lock_for(std::chrono::seconds(busyWait))) {
          std::lock_guard<std::mutex> lk(jm);
          res.set_header("Retry-After", "10");
          return reply(res, 409, {{"ok", false}, {"error", "busy: a job is using the engine"},
                                  {"job", running ? json(running->id) : json(nullptr)}});
        }
      }
      json r = exec({{"cmd", cmd}, {"args", args}}, timeout);
      reply(res, r.value("ok", false) ? 200 : (r.value("error", std::string()) == "timed out" ? 504 : 400), r);
    });

    svr.Post("/api/v1/jobs", [this](const httplib::Request& req, httplib::Response& res) {
      json body;
      try {
        body = json::parse(req.body);
      } catch (std::exception& e) {
        return fail(res, 400, std::string("body is not JSON: ") + e.what());
      }
      std::string err;
      auto j = addJob(body, &err);
      if (!j) return fail(res, 400, err);
      std::lock_guard<std::mutex> lk(jm);
      json o = jobJson(*j, false);
      o["ok"] = true;
      o["position"] = queue.size();
      res.set_header("Location", "/api/v1/jobs/" + j->id);
      reply(res, 202, o);
    });
    svr.Get("/api/v1/jobs", [this](const httplib::Request& req, httplib::Response& res) {
      std::lock_guard<std::mutex> lk(jm);
      json list = json::array();
      for (auto it = order.rbegin(); it != order.rend(); ++it)
        if (auto f = jobs.find(*it); f != jobs.end()) list.push_back(jobJson(*f->second, false));
      reply(res, 200, {{"ok", true}, {"jobs", list}});
    });
    svr.Get(R"(/api/v1/jobs/([A-Za-z0-9\-]+))", [this](const httplib::Request& req, httplib::Response& res) {
      std::string id = req.matches[1];
      double wait = req.has_param("wait") ? std::clamp(std::atof(req.get_param_value("wait").c_str()), 0.0, 600.0) : 0.0;
      std::unique_lock<std::mutex> lk(jm);
      auto it = jobs.find(id);
      if (it == jobs.end()) return fail(res, 404, "no job '" + id + "'");
      auto j = it->second;
      if (wait > 0)
        doneCv.wait_for(lk, std::chrono::duration<double>(wait),
                        [&] { return quit || (j->status != "queued" && j->status != "running"); });
      json o = jobJson(*j);
      o["ok"] = true;
      reply(res, 200, o);
    });
    svr.Delete(R"(/api/v1/jobs/([A-Za-z0-9\-]+))", [this](const httplib::Request& req, httplib::Response& res) {
      std::string id = req.matches[1];
      std::lock_guard<std::mutex> lk(jm);
      auto it = jobs.find(id);
      if (it == jobs.end()) return fail(res, 404, "no job '" + id + "'");
      auto j = it->second;
      if (j->status == "queued") {
        j->cancel = true;
        j->status = "cancelled";
        j->finished = now();
        queue.erase(std::remove(queue.begin(), queue.end(), j), queue.end());
        doneCv.notify_all();
      } else if (j->status == "running") {
        j->cancel = true;
        progress::requestCancel();  // bakes and renders stop early; other steps finish first
      }
      json o = jobJson(*j, false);
      o["ok"] = true;
      reply(res, 200, o);
    });

    // ---- files
    svr.Get("/api/v1/files", [this](const httplib::Request& req, httplib::Response& res) {
      fs::path dir;
      if (!wsPath(req.has_param("dir") ? req.get_param_value("dir") : "", dir)) return fail(res, 403, "outside the workspace");
      bool rec = req.has_param("recursive") && req.get_param_value("recursive") != "0";
      json list = json::array();
      std::error_code ec;
      auto add = [&](const fs::directory_entry& e) {
        json o = {{"path", fs::relative(e.path(), ws, ec).generic_string()}, {"dir", e.is_directory()}};
        if (e.is_regular_file()) o["size"] = (uint64_t)e.file_size(ec);
        list.push_back(o);
      };
      if (rec) {
        for (auto& e : fs::recursive_directory_iterator(dir, ec)) add(e);
      } else {
        for (auto& e : fs::directory_iterator(dir, ec)) add(e);
      }
      reply(res, 200, {{"ok", true}, {"workspace", ws.string()}, {"files", list}});
    });
    svr.Get(R"(/api/v1/files/(.+))", [this](const httplib::Request& req, httplib::Response& res) {
      fs::path p;
      if (!wsPath(req.matches[1], p)) return fail(res, 403, "outside the workspace");
      std::error_code ec;
      if (!fs::is_regular_file(p, ec)) return fail(res, 404, "no file " + std::string(req.matches[1]));
      uint64_t size = fs::file_size(p, ec);
      auto in = std::make_shared<std::ifstream>(p, std::ios::binary);
      std::string ext = p.extension().string();
      std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
      const char* mime = ext == ".png"    ? "image/png"
                         : ext == ".jpg"  ? "image/jpeg"
                         : ext == ".json" ? "application/json"
                         : ext == ".glb"  ? "model/gltf-binary"
                         : ext == ".exr"  ? "image/x-exr"
                                          : "application/octet-stream";
      res.set_content_provider(size, mime, [in](size_t offset, size_t length, httplib::DataSink& sink) {
        std::vector<char> buf(std::min<size_t>(length, 1 << 20));
        in->seekg((std::streamoff)offset);
        in->read(buf.data(), (std::streamsize)buf.size());
        std::streamsize got = in->gcount();
        if (got <= 0) return false;
        sink.write(buf.data(), (size_t)got);
        return true;
      });
    });
    svr.Put(R"(/api/v1/files/(.+))", [this](const httplib::Request& req, httplib::Response& res,
                                             const httplib::ContentReader& reader) {
      fs::path p;
      if (!wsPath(req.matches[1], p) || p == ws) return fail(res, 403, "outside the workspace");
      std::error_code ec;
      fs::create_directories(p.parent_path(), ec);
      fs::path tmp = p;
      tmp += ".part-" + randomHex(4);
      std::ofstream out(tmp, std::ios::binary);
      if (!out) return fail(res, 500, "cannot write " + p.string());
      uint64_t total = 0, limit = opt.maxUploadMB * 1024ull * 1024ull;
      bool tooBig = false;
      reader([&](const char* data, size_t len) {
        total += len;
        if (total > limit) {
          tooBig = true;
          return false;
        }
        out.write(data, (std::streamsize)len);
        return (bool)out;
      });
      out.close();
      if (tooBig || !out) {
        fs::remove(tmp, ec);
        return fail(res, tooBig ? 413 : 500, tooBig ? "file larger than the upload limit" : "write failed");
      }
      fs::rename(tmp, p, ec);
      if (ec) {
        fs::remove(p, ec);
        fs::rename(tmp, p, ec);
      }
      if (ec) return fail(res, 500, "could not store the file: " + ec.message());
      reply(res, 201, {{"ok", true}, {"path", p.string()}, {"relative", fs::relative(p, ws, ec).generic_string()},
                       {"size", total}});
    });
    svr.Delete(R"(/api/v1/files/(.+))", [this](const httplib::Request& req, httplib::Response& res) {
      fs::path p;
      if (!wsPath(req.matches[1], p) || p == ws) return fail(res, 403, "outside the workspace");
      std::error_code ec;
      uintmax_t n = fs::remove_all(p, ec);
      if (ec) return fail(res, 500, ec.message());
      reply(res, 200, {{"ok", true}, {"removed", (uint64_t)n}});
    });

    svr.set_error_handler([](const httplib::Request&, httplib::Response& res) {
      if (res.body.empty()) fail(res, res.status, res.status == 404 ? "no such endpoint (see /api/v1/openapi.json)" : "error");
    });
    svr.set_exception_handler([](const httplib::Request&, httplib::Response& res, std::exception_ptr ep) {
      std::string what = "internal error";
      try {
        if (ep) std::rethrow_exception(ep);
      } catch (std::exception& e) {
        what = e.what();
      } catch (...) {
      }
      fail(res, 500, what);
    });
  }
};

HttpApi::HttpApi() : d_(std::make_unique<Impl>()) {}
HttpApi::~HttpApi() { stop(); }

bool HttpApi::start(const HttpApiOptions& o, Exec exec, Describe describe, std::string* err) {
  if (d_->up) return true;
  d_->opt = o;
  d_->exec = std::move(exec);
  d_->describe = std::move(describe);
  // reach
  if (!o.bind.empty()) d_->host = o.bind;
  else d_->host = o.lan ? "0.0.0.0" : "127.0.0.1";
  d_->allowed = {"127.0.0.0/8", "::1/128"};
  if (o.lan || !o.bind.empty()) {
    if (o.allow.empty()) d_->allowed = privateRanges();
    else d_->allowed.insert(d_->allowed.end(), o.allow.begin(), o.allow.end());
  }
  for (auto& c : d_->allowed) {
    std::array<uint8_t, 16> tmp;
    std::string net = c.substr(0, c.find('/'));
    if (!parseAddr(net, tmp)) {
      if (err) *err = "not an address range: " + c;
      return false;
    }
  }
  // token: required on the network; generated once and kept in the settings folder
  if (d_->opt.token.empty() && (o.lan || !o.bind.empty()) && o.requireToken) {
    fs::path f = paths::configDir() / "api_token";
    bool ok = false;
    std::string t = paths::readFile(f, &ok);
    while (!t.empty() && (t.back() == '\n' || t.back() == '\r' || t.back() == ' ')) t.pop_back();
    if (!ok || t.size() < 16) {
      t = randomHex(24);
      paths::writeFileAtomic(paths::ensureDir(paths::configDir()) / "api_token", t + "\n");
#ifndef _WIN32
      std::error_code ec;
      fs::permissions(f, fs::perms::owner_read | fs::perms::owner_write, ec);
#endif
    }
    d_->opt.token = t;
    d_->tokenFile = f.string();
  }
  // workspace
  std::error_code ec;
  fs::path ws = o.workspace.empty() ? paths::cacheDir() / "api" : fs::path(o.workspace);
  fs::create_directories(ws, ec);
  ws = fs::weakly_canonical(fs::absolute(ws, ec), ec);
  d_->ws = ws;

  d_->svr.new_task_queue = [] { return new httplib::ThreadPool(24); };
  d_->svr.set_payload_max_length((size_t)std::min<uint64_t>(o.maxUploadMB * 1024ull * 1024ull, SIZE_MAX / 2));
  d_->svr.set_read_timeout(60, 0);
  d_->svr.set_write_timeout(60, 0);
  d_->routes();
  if (!d_->svr.bind_to_port(d_->host, o.port)) {
    if (err) *err = "cannot listen on " + d_->host + ":" + std::to_string(o.port) + " (in use?)";
    return false;
  }
  d_->port = o.port;
  d_->startedAt = now();
  d_->up = true;
  d_->quit = false;
  d_->listenThread = std::thread([this] { d_->svr.listen_after_bind(); });
  d_->runnerThread = std::thread([this] { d_->runJobs(); });
  PLOG_I("HTTP API on %s (%s)%s", url().c_str(),
         o.lan || !o.bind.empty() ? "local network only" : "this machine only",
         d_->opt.token.empty() ? "" : (", token " + d_->opt.token).c_str());
  if (!d_->tokenFile.empty()) PLOG_I("HTTP API token stored in %s", d_->tokenFile.c_str());
  return true;
}

void HttpApi::stop() {
  if (!d_ || !d_->up) return;
  d_->up = false;
  {
    std::lock_guard<std::mutex> lk(d_->jm);
    d_->quit = true;
    if (d_->running) d_->running->cancel = true;
  }
  progress::requestCancel();
  d_->jcv.notify_all();
  d_->doneCv.notify_all();
  d_->svr.stop();
  if (d_->listenThread.joinable()) d_->listenThread.join();
  if (d_->runnerThread.joinable()) d_->runnerThread.join();
  progress::clearCancel();
}

bool HttpApi::running() const { return d_->up; }
int HttpApi::port() const { return d_->port; }
// The address other machines reach this one on (the interface of the default route).
static std::string primaryAddress() {
#ifdef _WIN32
  SOCKET fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd == INVALID_SOCKET) return "";
#else
  int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) return "";
#endif
  sockaddr_in to{};
  to.sin_family = AF_INET;
  to.sin_port = htons(9);
  inet_pton(AF_INET, "192.0.2.1", &to.sin_addr);  // UDP connect sends nothing
  std::string out;
  if (::connect(fd, (sockaddr*)&to, sizeof to) == 0) {
    sockaddr_in me{};
    socklen_t len = sizeof me;
    if (::getsockname(fd, (sockaddr*)&me, &len) == 0) {
      char buf[64] = "";
      inet_ntop(AF_INET, &me.sin_addr, buf, sizeof buf);
      out = buf;
    }
  }
#ifdef _WIN32
  closesocket(fd);
#else
  ::close(fd);
#endif
  return out == "0.0.0.0" ? "" : out;
}

std::string HttpApi::url() const {
  std::string h = d_->host;
  if (h == "0.0.0.0") {
    h = primaryAddress();
    if (h.empty()) h = "<this machine's address>";
  }
  return "http://" + h + ":" + std::to_string(d_->port) + "/api/v1/";
}
std::string HttpApi::token() const { return d_->opt.token; }
std::string HttpApi::workspace() const { return d_->ws.string(); }
std::string HttpApi::tokenFile() const { return d_->tokenFile; }
int HttpApi::jobsQueued() const {
  std::lock_guard<std::mutex> lk(d_->jm);
  return (int)d_->queue.size();
}
json HttpApi::status() const {
  std::lock_guard<std::mutex> lk(d_->jm);
  json o = {{"running", d_->up.load()}, {"url", url()}, {"queued", d_->queue.size()}};
  if (d_->running) o["job"] = d_->jobJson(*d_->running, false);
  return o;
}

}  // namespace patina::net
