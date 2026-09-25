// Newline-delimited JSON over a unix socket, driven by the PipeWire main
// loop. Used by `roomcorr ctl` and the Omarchy plugin.
//
// Requests:  {"cmd":"get"} {"cmd":"status"} {"cmd":"set","values":{...}}
//            {"cmd":"subscribe","interval_ms":100} {"cmd":"reload"}
// Replies and pushes carry "type": "state" | "status" | "error" | "ok".
#pragma once

#include <functional>
#include <map>
#include <string>

#include <pipewire/pipewire.h>

#include "json.hpp"

namespace rc {

class ControlServer {
public:
  // Returns the reply for a request (or null for no reply).
  using Handler = std::function<Json(int client, const Json& request)>;

  ~ControlServer() { stop(); }
  bool start(pw_loop* loop, const std::string& path, Handler handler);

  // Must run before the loop is destroyed.
  void stop();

  void send(int client, const Json& msg);
  void broadcast_subscribers(const Json& msg);
  void subscribe(int client, int interval_ms);

  // Calls make_status for each subscriber whose interval has elapsed.
  void tick(int64_t now_ms, const std::function<Json()>& make_status);

private:
  struct Client {
    int fd = -1;
    spa_source* source = nullptr;
    std::string inbuf;
    int interval_ms = 0;  // 0 = not subscribed
    int64_t last_push = 0;
  };

  static void on_accept(void* data, int fd, uint32_t mask);
  static void on_client(void* data, int fd, uint32_t mask);
  void drop(int fd);

  pw_loop* loop_ = nullptr;
  int listen_fd_ = -1;
  spa_source* listen_source_ = nullptr;
  std::string path_;
  Handler handler_;
  std::map<int, Client> clients_;
};

// Client side, for `roomcorr ctl` and the calibrator.
// Sends one request and returns the first reply; throws if the daemon is
// not running.
Json control_request(const Json& request, int timeout_ms = 2000);

}  // namespace rc
