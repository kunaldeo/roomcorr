#include "control.hpp"

#include <cerrno>
#include <cstring>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "config.hpp"

namespace rc {

void ControlServer::stop() {
  for (auto& [fd, c] : clients_) {
    if (c.source) pw_loop_destroy_source(loop_, c.source);
    close(fd);
  }
  clients_.clear();
  if (listen_source_) pw_loop_destroy_source(loop_, listen_source_);
  listen_source_ = nullptr;
  if (listen_fd_ >= 0) {
    close(listen_fd_);
    unlink(path_.c_str());
    listen_fd_ = -1;
  }
}

bool ControlServer::start(pw_loop* loop, const std::string& path, Handler handler) {
  loop_ = loop;
  path_ = path;
  handler_ = std::move(handler);
  listen_fd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (listen_fd_ < 0) return false;
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (path.size() >= sizeof addr.sun_path) return false;
  strcpy(addr.sun_path, path.c_str());
  unlink(path.c_str());
  if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) return false;
  chmod(path.c_str(), 0600);
  if (listen(listen_fd_, 8) < 0) return false;
  listen_source_ = pw_loop_add_io(loop_, listen_fd_, SPA_IO_IN, false, on_accept, this);
  return listen_source_ != nullptr;
}

void ControlServer::on_accept(void* data, int fd, uint32_t mask) {
  auto* self = static_cast<ControlServer*>(data);
  while (true) {
    int cfd = accept4(fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (cfd < 0) break;
    Client c;
    c.fd = cfd;
    // Each client source gets the server as data; the fd identifies it.
    c.source = pw_loop_add_io(self->loop_, cfd, SPA_IO_IN | SPA_IO_HUP | SPA_IO_ERR, false, on_client, self);
    self->clients_[cfd] = std::move(c);
  }
}

void ControlServer::on_client(void* data, int fd, uint32_t mask) {
  auto* self = static_cast<ControlServer*>(data);
  auto it = self->clients_.find(fd);
  if (it == self->clients_.end()) return;
  char buf[4096];
  while (true) {
    ssize_t n = read(fd, buf, sizeof buf);
    if (n > 0) {
      it->second.inbuf.append(buf, size_t(n));
      if (it->second.inbuf.size() > (1 << 20)) {
        self->drop(fd);
        return;
      }
      continue;
    }
    if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
      self->drop(fd);
      return;
    }
    if (errno == EINTR) continue;
    break;
  }
  std::string& in = it->second.inbuf;
  size_t nl;
  while ((nl = in.find('\n')) != std::string::npos) {
    std::string line = in.substr(0, nl);
    in.erase(0, nl + 1);
    if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
    Json reply;
    try {
      reply = self->handler_(fd, Json::parse(line));
    } catch (const std::exception& e) {
      reply = Json::object();
      reply["type"] = "error";
      reply["error"] = e.what();
    }
    if (!reply.is_null()) self->send(fd, reply);
    // The handler may have dropped this client via send() failing.
    it = self->clients_.find(fd);
    if (it == self->clients_.end()) return;
  }
}

void ControlServer::send(int client, const Json& msg) {
  auto it = clients_.find(client);
  if (it == clients_.end()) return;
  std::string line = msg.dump() + "\n";
  const char* p = line.data();
  size_t left = line.size();
  while (left > 0) {
    ssize_t n = ::send(client, p, left, MSG_NOSIGNAL);
    if (n > 0) {
      p += n;
      left -= size_t(n);
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      // A client that stops reading loses messages rather than stalling
      // the daemon; meters are refreshed constantly anyway.
      pollfd pfd{client, POLLOUT, 0};
      if (poll(&pfd, 1, 20) > 0) continue;
    }
    drop(client);
    return;
  }
}

void ControlServer::broadcast_subscribers(const Json& msg) {
  std::vector<int> fds;
  for (auto& [fd, c] : clients_)
    if (c.interval_ms > 0) fds.push_back(fd);
  for (int fd : fds) send(fd, msg);
}

void ControlServer::subscribe(int client, int interval_ms) {
  auto it = clients_.find(client);
  if (it != clients_.end()) it->second.interval_ms = std::max(20, interval_ms);
}

void ControlServer::tick(int64_t now_ms, const std::function<Json()>& make_status) {
  std::vector<int> due;
  for (auto& [fd, c] : clients_)
    if (c.interval_ms > 0 && now_ms - c.last_push >= c.interval_ms) {
      due.push_back(fd);
      c.last_push = now_ms;
    }
  if (due.empty()) return;
  Json status = make_status();
  for (int fd : due) send(fd, status);
}

void ControlServer::drop(int fd) {
  auto it = clients_.find(fd);
  if (it == clients_.end()) return;
  if (it->second.source) pw_loop_destroy_source(loop_, it->second.source);
  close(fd);
  clients_.erase(it);
}

// ------------------------------------------------------------------ client

Json control_request(const Json& request, int timeout_ms) {
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) throw std::runtime_error("socket failed");
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::string path = socket_path();
  strncpy(addr.sun_path, path.c_str(), sizeof addr.sun_path - 1);
  if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0) {
    close(fd);
    throw std::runtime_error("roomcorr daemon is not running (" + path + ")");
  }
  std::string line = request.dump() + "\n";
  if (::send(fd, line.data(), line.size(), MSG_NOSIGNAL) != ssize_t(line.size())) {
    close(fd);
    throw std::runtime_error("send failed");
  }
  std::string in;
  char buf[65536];
  while (in.find('\n') == std::string::npos) {
    pollfd pfd{fd, POLLIN, 0};
    if (poll(&pfd, 1, timeout_ms) <= 0) {
      close(fd);
      throw std::runtime_error("timed out waiting for the daemon");
    }
    ssize_t n = read(fd, buf, sizeof buf);
    if (n <= 0) {
      close(fd);
      throw std::runtime_error("daemon closed the connection");
    }
    in.append(buf, size_t(n));
  }
  close(fd);
  return Json::parse(in.substr(0, in.find('\n')));
}

}  // namespace rc
