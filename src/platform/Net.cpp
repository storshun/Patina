#include "platform/Net.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cerrno>
#include <cstring>

#include "core/Log.h"

namespace patina::net {

// ---- tiny socket portability layer (POSIX / Winsock)
#ifdef _WIN32
using sock = SOCKET;
static const int kShutBoth = SD_BOTH;
static int lastErr() { return WSAGetLastError(); }
static std::string errStr(int e) {
  char buf[256] = {0};
  FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, (DWORD)e, 0, buf, sizeof buf, nullptr);
  std::string s = buf;
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
  return s.empty() ? "winsock error " + std::to_string(e) : s;
}
static void closeSock(std::intptr_t fd) { ::closesocket((sock)fd); }
static int pollOne(std::intptr_t fd, short events, int ms) {
  WSAPOLLFD p{(sock)fd, events, 0};
  return ::WSAPoll(&p, 1, ms);
}
static void setNonBlocking(std::intptr_t fd, bool on) {
  u_long v = on ? 1 : 0;
  ioctlsocket((sock)fd, FIONBIO, &v);
}
static bool inProgress(int e) { return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS; }
struct WsaInit {
  WsaInit() {
    WSADATA d;
    WSAStartup(MAKEWORD(2, 2), &d);
  }
};
static WsaInit g_wsa;
static std::intptr_t openSocket() {
  sock s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  return s == INVALID_SOCKET ? -1 : (std::intptr_t)s;
}
static std::intptr_t acceptSock(std::intptr_t l) {
  sock s = ::accept((sock)l, nullptr, nullptr);
  return s == INVALID_SOCKET ? -1 : (std::intptr_t)s;
}
static const int kSendFlags = 0;
#else
static const int kShutBoth = SHUT_RDWR;
static int lastErr() { return errno; }
static std::string errStr(int e) { return strerror(e); }
static void closeSock(std::intptr_t fd) { ::close((int)fd); }
static int pollOne(std::intptr_t fd, short events, int ms) {
  pollfd p{(int)fd, events, 0};
  return ::poll(&p, 1, ms);
}
static void setNonBlocking(std::intptr_t fd, bool on) {
  int flags = fcntl((int)fd, F_GETFL, 0);
  fcntl((int)fd, F_SETFL, on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
}
static bool inProgress(int e) { return e == EINPROGRESS; }
static std::intptr_t openSocket() { return ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0); }
static std::intptr_t acceptSock(std::intptr_t l) { return ::accept4((int)l, nullptr, nullptr, SOCK_CLOEXEC); }
static const int kSendFlags = MSG_NOSIGNAL;
#endif

static bool sendAll(std::intptr_t fd, const std::string& s) {
  size_t off = 0;
  while (off < s.size()) {
    auto n = ::send(fd, s.data() + off, (int)std::min<size_t>(s.size() - off, 1 << 30), kSendFlags);
    if (n < 0) {
      if (lastErr() == EINTR) continue;
      return false;
    }
    off += size_t(n);
  }
  return true;
}

bool JsonLineServer::start(int port, Handler handler, std::string* err) {
  stop();
  handler_ = std::move(handler);
  listenFd_ = openSocket();
  if (listenFd_ < 0) {
    if (err) *err = errStr(lastErr());
    return false;
  }
  int one = 1;
#ifdef _WIN32
  // Windows SO_REUSEADDR allows port stealing; exclusive use is the safe equivalent
  setsockopt(listenFd_, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&one, sizeof one);
#else
  setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#endif
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::bind(listenFd_, (sockaddr*)&addr, sizeof addr) < 0 || ::listen(listenFd_, 16) < 0) {
    if (err) *err = std::string("port ") + std::to_string(port) + ": " + errStr(lastErr());
    closeSock(listenFd_);
    listenFd_ = -1;
    return false;
  }
  socklen_t len = (socklen_t)sizeof addr;
  getsockname(listenFd_, (sockaddr*)&addr, &len);
  port_ = ntohs(addr.sin_port);
  running_ = true;
  acceptThread_ = std::thread([this] { acceptLoop(); });
  PLOG_I("Command server listening on 127.0.0.1:%d", port_);
  return true;
}

void JsonLineServer::stop() {
  if (!running_ && listenFd_ < 0) return;
  running_ = false;
  if (listenFd_ >= 0) {
    ::shutdown(listenFd_, kShutBoth);
    closeSock(listenFd_);
    listenFd_ = -1;
  }
  if (acceptThread_.joinable()) acceptThread_.join();
  {
    std::lock_guard<std::mutex> lk(fdMutex_);
    for (auto fd : clientFds_) ::shutdown(fd, kShutBoth);
  }
  for (auto& t : clientThreads_)
    if (t.joinable()) t.join();
  clientThreads_.clear();
  clientFds_.clear();
}

void JsonLineServer::acceptLoop() {
  while (running_) {
    int r = pollOne(listenFd_, POLLIN, 200);
    if (r <= 0) continue;
    std::intptr_t fd = acceptSock(listenFd_);
    if (fd < 0) continue;
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof one);
    std::lock_guard<std::mutex> lk(fdMutex_);
    clientFds_.push_back(fd);
    clientThreads_.emplace_back([this, fd] { serve(fd); });
  }
}

void JsonLineServer::serve(std::intptr_t fd) {
  connections_++;
  std::string buf;
  char chunk[65536];
  while (running_) {
    int r = pollOne(fd, POLLIN, 250);
    if (r < 0) break;
    if (r == 0) continue;
    auto n = ::recv(fd, chunk, (int)sizeof chunk, 0);
    if (n <= 0) break;
    buf.append(chunk, size_t(n));
    size_t nl;
    while ((nl = buf.find('\n')) != std::string::npos) {
      std::string line = buf.substr(0, nl);
      buf.erase(0, nl + 1);
      if (line.find_first_not_of(" \r\t") == std::string::npos) continue;
      json resp;
      json req;
      try {
        req = json::parse(line);
        resp = handler_(req);
      } catch (std::exception& e) {
        resp = {{"ok", false}, {"error", std::string("bad request: ") + e.what()}};
      }
      resp["id"] = req.is_object() && req.contains("id") ? req["id"] : json(nullptr);
      if (!sendAll(fd, resp.dump(-1, ' ', false, json::error_handler_t::replace) + "\n")) break;
    }
  }
  {
    std::lock_guard<std::mutex> lk(fdMutex_);
    clientFds_.erase(std::remove(clientFds_.begin(), clientFds_.end(), fd), clientFds_.end());
  }
  closeSock(fd);
  connections_--;
}

// ------------------------------------------------------------------ client

bool JsonLineClient::connect(const std::string& host, int port, int timeoutMs, std::string* err) {
  close();
  fd_ = openSocket();
  if (fd_ < 0) return false;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons((uint16_t)port);
  inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
  setNonBlocking(fd_, true);
  int r = ::connect(fd_, (sockaddr*)&addr, sizeof addr);
  if (r < 0 && !inProgress(lastErr())) {
    if (err) *err = errStr(lastErr());
    close();
    return false;
  }
  if (r < 0) {
    if (pollOne(fd_, POLLOUT, timeoutMs) <= 0) {
      if (err) *err = "connection timed out";
      close();
      return false;
    }
    int soerr = 0;
    socklen_t l = (socklen_t)sizeof soerr;
    getsockopt(fd_, SOL_SOCKET, SO_ERROR, (char*)&soerr, &l);
    if (soerr) {
      if (err) *err = errStr(soerr);
      close();
      return false;
    }
  }
  setNonBlocking(fd_, false);
  int one = 1;
  setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof one);
  buffer_.clear();
  return true;
}

void JsonLineClient::close() {
  if (fd_ >= 0) closeSock(fd_);
  fd_ = -1;
}

bool JsonLineClient::readLine(std::string& line, int timeoutMs) {
  char chunk[65536];
  while (true) {
    size_t nl = buffer_.find('\n');
    if (nl != std::string::npos) {
      line = buffer_.substr(0, nl);
      buffer_.erase(0, nl + 1);
      return true;
    }
    int r = pollOne(fd_, POLLIN, timeoutMs);
    if (r <= 0) return false;
    auto n = ::recv(fd_, chunk, (int)sizeof chunk, 0);
    if (n <= 0) return false;
    buffer_.append(chunk, size_t(n));
  }
}

bool JsonLineClient::call(const std::string& cmd, const json& args, json& response, int timeoutMs, std::string* err) {
  if (fd_ < 0) {
    if (err) *err = "not connected";
    return false;
  }
  int id = nextId_++;
  json req = {{"id", id}, {"cmd", cmd}, {"args", args}};
  if (!sendAll(fd_, req.dump() + "\n")) {
    if (err) *err = "send failed";
    close();
    return false;
  }
  std::string line;
  while (readLine(line, timeoutMs)) {
    try {
      json r = json::parse(line);
      if (r.value("id", json()) == json(id)) {
        response = r;
        return true;
      }
    } catch (...) {
    }
  }
  if (err) *err = "no response (timeout or connection closed)";
  close();
  return false;
}

void callAsync(int port, const std::string& cmd, const json& args, std::function<void(bool, const json&)> done) {
  std::thread([=] {
    JsonLineClient c;
    json resp;
    std::string err;
    bool ok = c.connect("127.0.0.1", port, 1500, &err) && c.call(cmd, args, resp, 20000, &err);
    if (!ok) resp = {{"ok", false}, {"error", err}};
    if (done) done(ok && resp.value("ok", false), resp);
  }).detach();
}

}  // namespace patina::net
