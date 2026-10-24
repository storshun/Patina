// Patina entry point.
//   patina [file]                 desktop application (GUI)
//   patina --headless             automation server without a window (EGL)
//   patina --mcp                  MCP server on stdio (proxies to a running window,
//                                 or runs a headless engine in-process)
#include <signal.h>
#include <cstdlib>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#include <shellapi.h>
#else
#include <fcntl.h>
#include <spawn.h>
#include <unistd.h>
extern char** environ;
#endif

#include <atomic>
#include <functional>
#include <set>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <mutex>
#include <vector>

#include "AppArgs.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "engine/Engine.h"
#include "engine/Recipes.h"
#include "mcp/ClaudeSetup.h"
#include "mcp/McpServer.h"
#include "platform/HeadlessContext.h"
#include "platform/HttpApi.h"
#include "platform/Net.h"

#ifdef _WIN32
// (0.22.2) laptops with two GPUs: the drivers give a program the fast one only when it asks like this
// (NVIDIA Optimus, AMD PowerXpress); without it Patina could run on the slower built-in one
extern "C" {
__declspec(dllexport) unsigned long NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

namespace patina {
int runGui(const Args& args);  // ui/App.cpp
}  // namespace patina

using namespace patina;
namespace fs = std::filesystem;

static void usage() {
  std::fprintf(stderr,
               "Patina %s - texture painter\n\n"
               "Usage: patina [options] [mesh.obj|.glb|.fbx|.stl|.3mf|.ply|project.patina]\n"
               "       patina run job.json [--set name=value ...] [--report out.json]\n"
               "       patina call <command> ['{\"json\": \"arguments\"}']\n\n"
               "  --headless          run without a window (automation server, EGL)\n"
               "  --mcp               Model Context Protocol server on stdin/stdout\n"
               "  --setup-claude      add Patina to the Claude desktop app's MCP servers, then exit\n"
               "  --port N            command server port (default 7860, env PATINA_PORT)\n"
               "  --no-server         do not open the command server\n"
               "  --resolution N      default texture resolution (default 2048)\n"
               "  --verbose           debug logging\n"
               "  --version           print the version\n\n"
               "HTTP API (for other applications; see the manual, 'HTTP API and batch mode'):\n"
               "  --http              start the HTTP API (default port 7870, this machine only)\n"
               "  --http-port N       its port (implies --http)\n"
               "  --lan               answer the local network too (private addresses only, token required)\n"
               "  --bind ADDR         listen on one interface address (implies --lan rules)\n"
               "  --allow CIDR        accept only these ranges besides this machine (repeatable)\n"
               "  --token T           API token (env PATINA_TOKEN; generated for --lan if not given)\n"
               "  --no-token          allow --lan without a token (trusted networks only)\n"
               "  --workspace DIR     folder for uploads and outputs; relative paths resolve here\n"
               "  --sandbox           refuse paths outside the workspace\n"
               "  --max-upload-mb N   largest upload (default 4096)\n"
               "  --cors-origin URL   let a web page from URL call the API (repeatable)\n\n"
               "Batch mode:\n"
               "  run job.json        run the steps of a job file headless, then exit (0 = all steps ok)\n"
               "  --set name=value    replaces ${name} in the job file (repeatable)\n"
               "  --report FILE       write the results as JSON (default: stdout)\n"
               "  --progress FILE     batches: a JSON line per finished file (FILE.cancel stops the batch)\n"
               "  --quiet             no progress lines on stderr\n"
               "  call CMD [JSON]     one command: sent to a running Patina if there is one, else run headless\n",
               PATINA_VERSION);
}

static Args parseArgs(int argc, char** argv) {
  Args a;
  if (const char* p = std::getenv("PATINA_PORT")) a.port = std::atoi(p);
  if (const char* t = std::getenv("PATINA_TOKEN")) a.token = t;
  int first = 1;
  if (argc > 1 && (std::string(argv[1]) == "run" || std::string(argv[1]) == "call")) {
    a.mode = argv[1];
    first = 2;
  }
  for (int i = first; i < argc; ++i) {
    std::string s = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing value for %s\n", s.c_str());
        std::exit(2);
      }
      return argv[++i];
    };
    if (s == "--headless") a.headless = true;
    else if (s == "--mcp") a.mcp = true;
    else if (s == "--setup-claude") {
      auto r = mcp::setupClaudeDesktop();
      std::printf("%s\n", r.message.c_str());
      for (auto& f : r.files) std::printf("  %s\n", f.string().c_str());
      std::printf("  command: %s --mcp\n", r.command.c_str());
      std::exit(r.ok ? 0 : 1);
    }
    else if (s == "--port") a.port = std::stoi(next());
    else if (s == "--no-server") a.server = false;
    else if (s == "--resolution") a.resolution = std::stoi(next());
    else if (s == "--verbose" || s == "-v") a.verbose = true;
    else if (s == "--http") a.http = a.httpGiven = true;
    else if (s == "--http-port") a.httpPort = std::stoi(next()), a.http = a.httpGiven = true;
    else if (s == "--lan") a.lan = true, a.http = a.httpGiven = true;
    else if (s == "--bind") a.bind = next(), a.http = a.httpGiven = true;
    else if (s == "--allow") a.allow.push_back(next());
    else if (s == "--token") a.token = next();
    else if (s == "--no-token") a.noToken = true;
    else if (s == "--workspace") a.workspace = next();
    else if (s == "--sandbox") a.sandbox = true;
    else if (s == "--max-upload-mb") a.maxUploadMB = std::stoull(next());
    else if (s == "--cors-origin") a.corsOrigins.push_back(next());
    else if (s == "--set") {
      std::string kv = next();
      auto eq = kv.find('=');
      if (eq == std::string::npos) {
        std::fprintf(stderr, "--set needs name=value\n");
        std::exit(2);
      }
      a.vars[kv.substr(0, eq)] = kv.substr(eq + 1);
    } else if (s == "--report") a.report = next();
    else if (s == "--progress") a.progress = next();
    else if (s == "--quiet" || s == "-q") a.quiet = true;
    else if (s == "--version") {
      std::printf("patina %s\n", PATINA_VERSION);
      std::exit(0);
    } else if (s == "--help" || s == "-h") {
      usage();
      std::exit(0);
    } else if (!s.empty() && s[0] == '-') {
      std::fprintf(stderr, "unknown option %s\n", s.c_str());
      usage();
      std::exit(2);
    } else if (a.mode == "run" && a.jobFile.empty()) {
      a.jobFile = s;
    } else if (a.mode == "call" && a.callCmd.empty()) {
      a.callCmd = s;
    } else if (a.mode == "call" && a.callArgs.empty()) {
      a.callArgs = s;
    } else {
      a.file = s;
    }
  }
  if (a.mode == "run" && a.jobFile.empty()) {
    std::fprintf(stderr, "patina run: give a job file (patina run job.json)\n");
    std::exit(2);
  }
  if (a.mode == "call" && a.callCmd.empty()) {
    std::fprintf(stderr, "patina call: give a command (patina call get_state)\n");
    std::exit(2);
  }
  return a;
}

static std::atomic<bool> g_quit{false};
static std::atomic<bool> g_stdinDone{false};  // (1.2.7) `--mcp`: the client closed stdin
// (1.2.7) `--mcp` while its own engine starts: nothing to wind down yet, and the start may wait in the graphics driver
// for a long time (it never looks at g_quit there), so a stop signal ends it at once
static std::atomic<bool> g_mcpStarting{false};
static void onSignal(int) {
  if (g_mcpStarting) std::_Exit(0);
  g_quit = true;
}

// Runs the engine on this thread with an offscreen context until g_quit.
static int runHeadless(const Args& a, std::function<void(Engine&)> started = {}) {
  HeadlessContext ctx;
  std::string err;
  if (!ctx.create(&err)) {
    PLOG_E("%s", err.c_str());
    return 1;
  }
  Engine engine;
  engine.defaultResolution = a.resolution;
  engine.init(true);
  if (!a.file.empty()) {
    if (!engine.openFile(a.file, &err)) PLOG_E("%s", err.c_str());
  }
  net::JsonLineServer server;
  if (a.server) {
    bool ok = server.start(a.port, [&engine](const json& req) {
      auto f = engine.post(req);
      if (f.wait_for(std::chrono::seconds(300)) != std::future_status::ready)
        return json{{"ok", false}, {"error", "timed out"}};
      return f.get();
    }, &err);
    if (ok) engine.serverPort = server.port();
    else PLOG_W("Command server not started: %s", err.c_str());
  }
  // internal command used by the MCP "open_gui_window" hand-off
  engine.commands.add({"__stop_server", "", json{{"type", "object"}, {"properties", json::object()}}, [&server](CommandContext&, const json&) {
                         server.stop();
                         return json{{"stopped", true}};
                       }, false});
  net::HttpApi http;
  if (a.http) {
    auto exec = [&engine](const json& req, int timeoutS) {
      auto f = engine.post(req);
      if (f.wait_for(std::chrono::seconds(timeoutS)) != std::future_status::ready)
        return json{{"ok", false}, {"error", "timed out"}};
      return f.get();
    };
    auto describe = [&engine] { return engine.commands.describe(true); };
    if (!http.start(a.httpOptions(), exec, describe, &err)) {
      PLOG_E("HTTP API: %s", err.c_str());
      engine.shutdown();
      return 1;
    }
    std::fprintf(stderr, "Patina %s HTTP API: %s%s%s\n", PATINA_VERSION, http.url().c_str(),
                 http.token().empty() ? "" : "  token: ", http.token().c_str());
  }
  if (started) started(engine);
  while (!g_quit) {
    engine.waitForWork(50);
    engine.tick();
  }
  http.stop();
  server.stop();
  engine.shutdown();
  return 0;
}

// ------------------------------------------------------------------ batch mode

static bool isPathKey(const std::string& k) {
  auto ends = [&](const char* suf) {
    size_t n = std::strlen(suf);
    return k.size() >= n && k.compare(k.size() - n, n, suf) == 0;
  };
  return k == "path" || k == "directory" || k == "folder" || ends("_path") || ends("_dir") || ends("_directory");
}

// ${name} -> value in every string of the job
static json substitute(const json& j, const std::map<std::string, std::string>& vars, std::set<std::string>& missing) {
  return substituteVars(j, vars, missing);
}

// 0.30: a batch: the same steps for every file ("per_file", with ${file}, ${name}, ${dir}, ${out}...). A file whose
// step fails is left (the rest of its steps skipped) and the next one goes on; --progress gets a JSON line per file.
static int runFiles(Args a, const json& job) {
  std::error_code ec;
  fs::path jobDir = fs::absolute(a.jobFile, ec).parent_path();
  std::map<std::string, std::string> base;
  if (job.contains("vars") && job["vars"].is_object())
    for (auto it = job["vars"].begin(); it != job["vars"].end(); ++it)
      base[it.key()] = it.value().is_string() ? it.value().get<std::string>() : it.value().dump();
  for (auto& [k, v] : a.vars) base[k] = v;
  base["job_dir"] = jobDir.string();
  if (!a.verbose) Log::setMinLevel(LogLevel::Warn);
  HeadlessContext ctx;
  std::string err;
  if (!ctx.create(&err)) {
    std::fprintf(stderr, "patina run: no GPU context: %s\n", err.c_str());
    return 3;
  }
  Engine engine;
  engine.defaultResolution = a.resolution;
  engine.init(true);
  json files = job["files"];
  json report = {{"job", a.jobFile}, {"name", job.value("name", std::string())}, {"files", json::array()}};
  auto t0 = std::chrono::steady_clock::now();
  auto secs = [](auto from) { return std::chrono::duration<double>(std::chrono::steady_clock::now() - from).count(); };
  int failed = 0;
  bool cancelled = false;
  for (size_t i = 0; i < files.size(); ++i) {
    if (!a.progress.empty() && fs::exists(a.progress + ".cancel", ec)) {
      cancelled = true;
      break;
    }
    fs::path file = files[i].get<std::string>();
    auto vars = base;
    vars["file"] = file.string();
    vars["name"] = file.stem().string();
    vars["dir"] = file.parent_path().string();
    vars["index"] = std::to_string(i + 1);
    if (!vars.count("out")) vars["out"] = file.parent_path().string();
    std::set<std::string> missing;
    json steps = substitute(job["per_file"], vars, missing);
    json entry = {{"file", file.string()}, {"index", i + 1}, {"steps", json::array()}};
    auto tf = std::chrono::steady_clock::now();
    bool ok = missing.empty();
    if (!ok) entry["error"] = "no value for " + *missing.begin();
    if (!a.quiet) std::fprintf(stderr, "[%zu/%zu] %s ...", i + 1, files.size(), file.filename().string().c_str());
    for (size_t k = 0; ok && k < steps.size(); ++k) {
      std::string cmd = steps[k].value("cmd", std::string());
      json st = {{"cmd", cmd}};
      try {
        json r = engine.execute(cmd, steps[k].value("args", json::object()));
        engine.tick();
        st["ok"] = true;
        st["result"] = r;
      } catch (std::exception& e) {
        st["ok"] = false;
        st["error"] = e.what();
        entry["error"] = cmd + ": " + e.what();
        ok = false;
      }
      entry["steps"].push_back(st);
    }
    entry["ok"] = ok;
    entry["seconds"] = secs(tf);
    if (!ok) ++failed;
    if (!a.quiet) std::fprintf(stderr, ok ? " ok\n" : " FAILED\n");
    report["files"].push_back(entry);
    if (!a.progress.empty()) {
      std::ofstream f(fs::path(a.progress), std::ios::app | std::ios::binary);
      json line = entry;
      for (auto& st : line["steps"]) st.erase("result");  // the progress line stays small
      f << line.dump() << "\n";
    }
  }
  report["ok"] = failed == 0 && !cancelled;
  report["failed"] = failed;
  report["cancelled"] = cancelled;
  report["seconds"] = secs(t0);
  engine.shutdown();
  std::string out = report.dump(2);
  if (!a.report.empty()) paths::writeFileAtomic(a.report, out + "\n");
  else std::printf("%s\n", out.c_str());
  return failed || cancelled ? 1 : 0;
}

static int runBatch(Args a) {
  std::string text;
  bool ok = false;
  text = paths::readFile(a.jobFile, &ok);
  if (!ok) {
    std::fprintf(stderr, "patina run: cannot read %s\n", a.jobFile.c_str());
    return 2;
  }
  json job;
  try {
    job = json::parse(text);
  } catch (std::exception& e) {
    std::fprintf(stderr, "patina run: %s is not valid JSON: %s\n", a.jobFile.c_str(), e.what());
    return 2;
  }
  if (job.is_array()) job = json{{"steps", job}};
  if (job.is_object() && job.contains("files") && job.contains("per_file")) return runFiles(a, job);
  if (!job.is_object() || !job.contains("steps") || !job["steps"].is_array()) {
    std::fprintf(stderr, "patina run: the job needs \"steps\": [{\"cmd\": ..., \"args\": {...}}]\n");
    return 2;
  }
  std::error_code ec;
  fs::path jobDir = fs::absolute(a.jobFile, ec).parent_path();
  std::map<std::string, std::string> vars;
  if (job.contains("vars") && job["vars"].is_object())
    for (auto it = job["vars"].begin(); it != job["vars"].end(); ++it)
      vars[it.key()] = it.value().is_string() ? it.value().get<std::string>() : it.value().dump();
  for (auto& [k, v] : a.vars) vars[k] = v;
  vars["job_dir"] = jobDir.string();
  vars["cwd"] = fs::current_path(ec).string();
  std::set<std::string> missing;
  json steps = substitute(job["steps"], vars, missing);
  if (!missing.empty()) {
    std::string names;
    for (auto& m : missing) names += (names.empty() ? "" : ", ") + m;
    std::fprintf(stderr, "patina run: no value for %s (use --set name=value or \"vars\" in the job)\n", names.c_str());
    return 2;
  }
  bool stopOnError = job.value("stop_on_error", true);
  if (!a.verbose) Log::setMinLevel(LogLevel::Warn);  // progress lines only; --verbose shows the log

  HeadlessContext ctx;
  std::string err;
  if (!ctx.create(&err)) {
    std::fprintf(stderr, "patina run: no GPU context: %s\n", err.c_str());
    return 3;
  }
  Engine engine;
  engine.defaultResolution = a.resolution;
  engine.init(true);
  json report = {{"job", a.jobFile}, {"name", job.value("name", std::string())}, {"steps", json::array()}};
  auto t0 = std::chrono::steady_clock::now();
  auto secs = [](auto from) { return std::chrono::duration<double>(std::chrono::steady_clock::now() - from).count(); };
  int failures = 0;
  size_t n = steps.size();
  for (size_t i = 0; i < n; ++i) {
    json st = steps[i];
    std::string cmd = st.value("cmd", std::string());
    json args = st.value("args", json::object());
    if (args.is_object())
      for (auto it = args.begin(); it != args.end(); ++it)
        if (it.value().is_string() && isPathKey(it.key())) {
          fs::path p(it.value().get<std::string>());
          if (!p.empty() && p.is_relative()) it.value() = (jobDir / p).lexically_normal().string();
        }
    if (!a.quiet) std::fprintf(stderr, "[%zu/%zu] %s ...", i + 1, n, cmd.c_str());
    auto ts = std::chrono::steady_clock::now();
    json entry = {{"cmd", cmd}};
    try {
      if (cmd.empty()) throw CommandError("a step without \"cmd\"");
      json r = engine.execute(cmd, args);
      engine.tick();
      entry["ok"] = true;
      entry["result"] = r;
    } catch (std::exception& e) {
      entry["ok"] = false;
      entry["error"] = e.what();
      ++failures;
    }
    entry["seconds"] = secs(ts);
    if (!a.quiet) {
      if (entry["ok"].get<bool>()) std::fprintf(stderr, " ok (%.1f s)\n", entry["seconds"].get<double>());
      else std::fprintf(stderr, " FAILED: %s\n", entry["error"].get<std::string>().c_str());
    }
    report["steps"].push_back(entry);
    if (!entry["ok"].get<bool>() && stopOnError) break;
  }
  report["ok"] = failures == 0;
  report["seconds"] = secs(t0);
  engine.shutdown();
  std::string out = report.dump(2);
  if (!a.report.empty()) {
    if (!paths::writeFileAtomic(a.report, out + "\n")) {
      std::fprintf(stderr, "patina run: could not write %s\n", a.report.c_str());
      return 1;
    }
  } else {
    std::printf("%s\n", out.c_str());
  }
  if (!a.quiet) std::fprintf(stderr, "%s in %.1f s\n", failures ? "Job failed" : "Job done", report["seconds"].get<double>());
  return failures ? 1 : 0;
}

static int runCall(Args a) {
  json args = json::object();
  if (!a.callArgs.empty()) {
    try {
      args = json::parse(a.callArgs);
    } catch (std::exception& e) {
      std::fprintf(stderr, "patina call: the arguments are not JSON: %s\n", e.what());
      return 2;
    }
  }
  // a running Patina (window or headless) first
  {
    net::JsonLineClient c;
    std::string err;
    if (c.connect("127.0.0.1", a.port, 500, &err)) {
      json r;
      if (c.call(a.callCmd, args, r, 4 * 3600 * 1000, &err)) {
        std::printf("%s\n", (r.value("ok", false) ? r.value("result", json()) : r).dump(2).c_str());
        return r.value("ok", false) ? 0 : 1;
      }
    }
  }
  Log::setMinLevel(LogLevel::Warn);
  HeadlessContext ctx;
  std::string err;
  if (!ctx.create(&err)) {
    std::fprintf(stderr, "patina call: no GPU context: %s\n", err.c_str());
    return 3;
  }
  Engine engine;
  engine.defaultResolution = a.resolution;
  engine.init(true);
  int rc = 0;
  try {
    json r = engine.execute(a.callCmd, args);
    std::printf("%s\n", r.dump(2).c_str());
  } catch (std::exception& e) {
    std::fprintf(stderr, "%s\n", e.what());
    rc = 1;
  }
  engine.shutdown();
  return rc;
}

#ifdef _WIN32
static std::wstring widen(const std::string& u8) {
  if (u8.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, u8.data(), (int)u8.size(), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, u8.data(), (int)u8.size(), w.data(), n);
  return w;
}

static bool spawnGui(const std::string& project, int port) {
  // the window build (patina.exe, GUI subsystem) next to this console build (patina-cli.exe)
  fs::path exe = paths::exeDir() / "patina.exe";
  std::wstring cmd = L"\"" + exe.wstring() + L"\"";
  if (!project.empty()) cmd += L" \"" + widen(project) + L"\"";
  cmd += L" --port " + std::to_wstring(port);
  STARTUPINFOW si{};
  si.cb = sizeof si;
  PROCESS_INFORMATION pi{};
  // no inherited handles: the MCP stdio pipes must stay ours
  BOOL ok = CreateProcessW(exe.wstring().c_str(), cmd.data(), nullptr, nullptr, FALSE,
                           DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &si, &pi);
  if (!ok) return false;
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
}
#else
static bool spawnGui(const std::string& project, int port) {
  std::string exe = (paths::exeDir() / "patina").string();
  std::string portStr = std::to_string(port);
  std::vector<char*> argv = {exe.data()};
  if (!project.empty()) argv.push_back(const_cast<char*>(project.c_str()));
  argv.push_back(const_cast<char*>("--port"));
  argv.push_back(portStr.data());
  argv.push_back(nullptr);
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  // keep the MCP stdio channel clean: GUI stdout -> /dev/null
  posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
  posix_spawnattr_t attr;
  posix_spawnattr_init(&attr);
  posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);
  pid_t pid;
  int rc = posix_spawn(&pid, exe.c_str(), &fa, &attr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&fa);
  posix_spawnattr_destroy(&attr);
  return rc == 0;
}
#endif

// (1.2.7) the end of `--mcp`: the client closed stdin (the stdio thread is done: join it), or a signal set g_quit
// while that thread still waits in a read that nothing can wake. Then leave without it: it waited forever
// before, and only a SIGKILL ended it.
static void endMcp(std::thread& io, int rc = 0) {
  if (!io.joinable()) return;
  if (g_stdinDone) {
    io.join();
    return;
  }
  PLOG_I("MCP: stopped by a signal");
  std::cout.flush();
  std::fflush(stdout);
  std::fflush(stderr);
  std::_Exit(rc);
}

static int runMcp(Args a) {
  // Proxy mode if a Patina window (or headless server) is already running.
  struct Proxy {
    std::mutex m;
    net::JsonLineClient client;
    int port = 0;
    bool active = false;
    json call(const std::string& cmd, const json& args) {
      std::lock_guard<std::mutex> lk(m);
      json resp;
      std::string err;
      for (int attempt = 0; attempt < 2; ++attempt) {
        if (!client.connected() && !client.connect("127.0.0.1", port, 2000, &err)) break;
        if (client.call(cmd, args, resp, 300000, &err)) return resp;
      }
      return {{"ok", false}, {"error", "Patina window not reachable on port " + std::to_string(port) + ": " + err}};
    }
    // (1.2.5) a Patina window opened since: from now on the tools drive it (checked at most every 2 s)
    std::chrono::steady_clock::time_point lastProbe{};
    bool probe() {
      std::lock_guard<std::mutex> lk(m);
      if (active) return true;
      auto now = std::chrono::steady_clock::now();
      if (now - lastProbe < std::chrono::seconds(2)) return false;
      lastProbe = now;
      std::string err;
      json r;
      if (client.connect("127.0.0.1", port, 300, &err) && client.call("ping", json::object(), r, 3000, &err) &&
          r.value("ok", false)) {
        active = true;
        PLOG_I("MCP: a Patina window is running on port %d now: the tools drive it (live mode)", port);
      }
      return active;
    }
  };
  auto proxy = std::make_shared<Proxy>();
  proxy->port = a.port;
  {
    std::string err;
    if (proxy->client.connect("127.0.0.1", a.port, 500, &err)) {
      json r;
      if (proxy->client.call("ping", json::object(), r, 3000, &err) && r.value("ok", false)) proxy->active = true;
    }
  }
  auto makeServer = [&](McpServer::Backend localBackend, std::function<json()> localLister) {
    auto backend = [proxy, localBackend](const std::string& cmd, const json& args) -> json {
      if (proxy->active || proxy->probe()) return proxy->call(cmd, args);
      return localBackend(cmd, args);
    };
    auto lister = [proxy, localLister]() -> json {
      if (proxy->active || proxy->probe()) {
        json r = proxy->call("list_commands", json::object());
        if (r.value("ok", false)) {
          json out = json::array();
          for (auto& t : r["result"]) {
            static const std::set<std::string> hidden = {"list_commands", "bridge_open_mesh", "bridge_update_mesh",
                                                         "bridge_export", "__stop_server"};
            if (!hidden.count(t.value("name", std::string()))) out.push_back(t);
          }
          return out;
        }
      }
      return localLister();
    };
    return std::make_unique<McpServer>(backend, lister);
  };

  if (proxy->active) {
    PLOG_I("MCP: connected to the running Patina window on port %d (live mode)", a.port);
    auto server = makeServer([](const std::string&, const json&) { return json{{"ok", false}, {"error", "no backend"}}; },
                             [] { return json::array(); });
    server->extraTools.push_back({{"name", "open_gui_window"},
                                  {"description", "Opens the Patina desktop window (already open in live mode)."},
                                  {"inputSchema", {{"type", "object"}, {"properties", json::object()}}}});
    server->localTool = [](const std::string& n, const json&, bool& handled) -> json {
      handled = n == "open_gui_window";
      return {{"ok", true}, {"result", {{"note", "connected to a live Patina window already"}}}};
    };
    // (1.2.7) stdio on a worker thread, so SIGTERM/SIGINT end it too, not only the client closing stdin
    std::thread io([&server] {
      server->run(std::cin, std::cout);
      g_stdinDone = true;
      g_quit = true;
    });
    while (!g_quit) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    endMcp(io);
    return 0;
  }

  // Local headless engine: GL on the main thread, MCP stdio on a worker thread.
  PLOG_I("MCP: no Patina window found on port %d - starting a headless engine", a.port);
  Engine* enginePtr = nullptr;
  std::thread mcpThread;
  std::atomic<bool> ready{false};
  // (1.2.5) without the command server: it talks over stdio only. On the port it kept a Patina window opened
  // later off it (that window moved to 7861, Blender's port), and Blender's add-on reached this hidden engine.
  Args local = a;
  local.server = false;
  g_mcpStarting = true;
  int rc = runHeadless(local, [&](Engine& engine) {
    g_mcpStarting = false;
    enginePtr = &engine;
    ready = true;
    mcpThread = std::thread([&engine, proxy, makeServer, a]() mutable {
      auto local = [&engine](const std::string& cmd, const json& args) -> json {
        auto f = engine.post({{"cmd", cmd}, {"args", args}});
        return f.get();
      };
      auto lister = [&engine]() -> json {
        json all = engine.commands.describe(true);
        return all;
      };
      auto server = makeServer(local, lister);
      server->extraTools.push_back(
          {{"name", "open_gui_window"},
           {"description",
            "Opens the Patina desktop window with the current project so the user (and you, via "
            "screenshot_window) can watch. After this call all tools act on the live window."},
           {"inputSchema", {{"type", "object"}, {"properties", json::object()}}}});
      server->localTool = [&engine, proxy, a](const std::string& n, const json&, bool& handled) -> json {
        if (n != "open_gui_window") return nullptr;
        handled = true;
        if (proxy->active) return {{"ok", true}, {"result", {{"note", "already live"}}}};
        std::string handoff = (paths::cacheDir() / "mcp_handoff.patina").string();
        json saved = engine.post({{"cmd", "save_project"}, {"args", {{"path", handoff}}}}).get();
        engine.post({{"cmd", "__stop_server"}, {"args", json::object()}}).get();
        if (!spawnGui(saved.value("ok", false) ? handoff : "", a.port))
          return {{"ok", false}, {"error", "could not start the patina GUI"}};
        for (int i = 0; i < 100; ++i) {
          std::this_thread::sleep_for(std::chrono::milliseconds(150));
          std::string err;
          net::JsonLineClient probe;
          if (probe.connect("127.0.0.1", a.port, 300, &err)) {
            json r;
            if (probe.call("ping", json::object(), r, 3000, &err) && r.value("ok", false)) {
              proxy->active = true;
              return {{"ok", true}, {"result", {{"note", "Patina window opened; tools now drive the live window"}}}};
            }
          }
        }
        return {{"ok", false}, {"error", "the window did not come up within 15 s (is a display available?)"}};
      };
      server->run(std::cin, std::cout);
      g_stdinDone = true;
      g_quit = true;
    });
  });
  endMcp(mcpThread, rc);
  (void)enginePtr;
  return rc;
}

#ifdef _WIN32
// argv in UTF-8 regardless of the console code page
static std::vector<std::string> utf8Args() {
  int n = 0;
  LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n);
  std::vector<std::string> out;
  for (int i = 0; i < n; ++i) {
    int len = WideCharToMultiByte(CP_UTF8, 0, w[i], -1, nullptr, 0, nullptr, nullptr);
    std::string s(len > 0 ? len - 1 : 0, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w[i], -1, s.data(), len, nullptr, nullptr);
    out.push_back(s);
  }
  LocalFree(w);
  return out;
}
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
  std::vector<std::string> u8 = utf8Args();
  std::vector<char*> u8argv;
  for (auto& s : u8) u8argv.push_back(s.data());
  u8argv.push_back(nullptr);
  argc = (int)u8.size();
  argv = u8argv.data();
  // MCP speaks newline-delimited JSON: no CRLF translation on stdio
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
  SetConsoleOutputCP(CP_UTF8);
#endif
  Args a = parseArgs(argc, argv);
  if (a.verbose) Log::setMinLevel(LogLevel::Debug);
#ifndef _WIN32
  signal(SIGPIPE, SIG_IGN);
#endif
#ifdef PATINA_HAS_GUI
  // GUI: SDL installs SIGINT/SIGTERM handlers that post a quit event
  if (!a.headless && !a.mcp && a.mode.empty()) return runGui(a);
#endif
  signal(SIGINT, onSignal);
  signal(SIGTERM, onSignal);
#ifdef _WIN32
  SetConsoleCtrlHandler([](DWORD) -> BOOL { g_quit = true; return TRUE; }, TRUE);
#endif
  if (a.mode == "run") return runBatch(a);
  if (a.mode == "call") return runCall(a);
  if (a.mcp) return runMcp(a);
  return runHeadless(a);
}
