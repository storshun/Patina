#include "platform/Process.h"

#include <cstdlib>
#include <filesystem>

#include "core/Paths.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace patina {

namespace fs = std::filesystem;

std::string selfExecutable() {
  if (const char* e = std::getenv("PATINA_EXE"); e && *e) return e;  // tests: the patina binary to use
  fs::path dir = paths::exeDir();
#ifdef _WIN32
  std::error_code ec;
  if (fs::exists(dir / "patina-cli.exe", ec)) return (dir / "patina-cli.exe").string();
  return (dir / "patina.exe").string();
#else
  return (dir / "patina").string();
#endif
}

ChildProcess::~ChildProcess() {
#ifdef _WIN32
  if (handle_) CloseHandle(handle_);
#else
  if (started_ && !done_) running();  // reap if it already ended
#endif
}

#ifdef _WIN32
static std::wstring widen(const std::string& u8) {
  if (u8.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, u8.data(), (int)u8.size(), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, u8.data(), (int)u8.size(), w.data(), n);
  return w;
}
static std::wstring quote(const std::wstring& a) {
  std::wstring q = L"\"";
  for (wchar_t c : a) {
    if (c == L'"') q += L'\\';
    q += c;
  }
  return q + L"\"";
}

bool ChildProcess::start(const std::string& exe, const std::vector<std::string>& args, std::string* err) {
  std::wstring cmd = quote(widen(exe));
  for (auto& a : args) cmd += L" " + quote(widen(a));
  STARTUPINFOW si{};
  si.cb = sizeof si;
  PROCESS_INFORMATION pi{};
  BOOL ok = CreateProcessW(widen(exe).c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                           &si, &pi);
  if (!ok) {
    if (err) *err = "could not start " + exe;
    return false;
  }
  CloseHandle(pi.hThread);
  handle_ = pi.hProcess;
  started_ = true;
  return true;
}

bool ChildProcess::running() {
  if (!started_ || done_) return false;
  DWORD code = 0;
  if (GetExitCodeProcess((HANDLE)handle_, &code) && code == STILL_ACTIVE) return true;
  exit_ = (int)code;
  done_ = true;
  return false;
}

void ChildProcess::kill() {
  if (started_ && !done_ && handle_) TerminateProcess((HANDLE)handle_, 1);
}
#else
bool ChildProcess::start(const std::string& exe, const std::vector<std::string>& args, std::string* err) {
  std::vector<std::string> store = {exe};
  store.insert(store.end(), args.begin(), args.end());
  std::vector<char*> argv;
  for (auto& s : store) argv.push_back(s.data());
  argv.push_back(nullptr);
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
  posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
  pid_t pid;
  int rc = posix_spawn(&pid, exe.c_str(), &fa, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&fa);
  if (rc != 0) {
    if (err) *err = "could not start " + exe;
    return false;
  }
  pid_ = pid;
  started_ = true;
  return true;
}

bool ChildProcess::running() {
  if (!started_ || done_) return false;
  int status = 0;
  pid_t r = waitpid(pid_, &status, WNOHANG);
  if (r == 0) return true;
  exit_ = r == pid_ && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  done_ = true;
  return false;
}

void ChildProcess::kill() {
  if (started_ && !done_ && pid_ > 0) ::kill(pid_, SIGTERM);
}
#endif

}  // namespace patina
