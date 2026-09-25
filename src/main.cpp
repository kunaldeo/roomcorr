#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <unistd.h>

#include "config.hpp"
#include "control.hpp"
#include "json.hpp"

namespace rc {
int run_daemon();
int run_calibrate(int argc, char** argv);
int run_design(int argc, char** argv);
int run_verify(int argc, char** argv);
int run_setup(int argc, char** argv);
int run_probe(int argc, char** argv);
}  // namespace rc

using namespace rc;

static void usage() {
  printf(
      "roomcorr — room correction and bass management for PipeWire\n\n"
      "  roomcorr daemon                 run the DSP engine (normally via systemd)\n"
      "  roomcorr studio [live|response|alignment|calibrate]\n"
      "                                  open Room Correction Studio\n"
      "  roomcorr setup                  switch the X4 to 5.1 and make Room Correction the default output\n"
      "  roomcorr calibrate [--positions N] [--mic-cal FILE] [--yes]\n"
      "                                  measure with the UMIK-1 and build filters\n"
      "  roomcorr verify                 measure the corrected system\n"
      "  roomcorr probe OUTS [DBFS]      noise burst on X4 outputs (FL,FR,FC,LFE,...), report mic level\n"
      "  roomcorr design [--measurement DIR]\n"
      "                                  rebuild filters from saved measurements (e.g. after a target change)\n"
      "  roomcorr ctl get|status|reload|watch\n"
      "  roomcorr ctl set KEY=VALUE ...  e.g. sub_gain_db=2 crossover_hz=90 target.bass_boost_db=5\n"
      "  roomcorr ctl toggle KEY         e.g. enabled, room_eq, bass_management, mute\n");
}

static Json parse_value(const std::string& v) {
  if (v == "true" || v == "on") return Json(true);
  if (v == "false" || v == "off") return Json(false);
  char* end = nullptr;
  double d = strtod(v.c_str(), &end);
  if (end && *end == 0 && !v.empty()) return Json(d);
  if (!v.empty() && (v[0] == '[' || v[0] == '{')) return Json::parse(v);
  return Json(v);
}

// Opens Room Correction Studio (a Quickshell app). One instance: a second
// launch just exits. The optional tab name is passed through the env.
static int run_studio(int argc, char** argv) {
  const char* home = getenv("HOME");
  std::string dir = std::string(home ? home : "") + "/.local/share/roomcorr/studio";
  if (const char* d = getenv("ROOMCORR_STUDIO_DIR")) dir = d;
  if (argc > 0) setenv("ROOMCORR_STUDIO_TAB", argv[0], 1);
  execlp("qs", "qs", "-n", "-p", dir.c_str(), (char*)nullptr);
  perror("roomcorr: cannot start quickshell (qs)");
  return 1;
}

static int run_ctl(int argc, char** argv) {
  if (argc < 1) {
    usage();
    return 2;
  }
  std::string sub = argv[0];
  Json req = Json::object();
  if (sub == "get" || sub == "status" || sub == "reload") {
    req["cmd"] = sub;
  } else if (sub == "set") {
    req["cmd"] = "set";
    req["values"] = Json::object();
    for (int i = 1; i < argc; ++i) {
      std::string a = argv[i];
      auto eq = a.find('=');
      if (eq == std::string::npos) {
        fprintf(stderr, "expected KEY=VALUE, got %s\n", a.c_str());
        return 2;
      }
      req["values"][a.substr(0, eq)] = parse_value(a.substr(eq + 1));
    }
  } else if (sub == "toggle" && argc >= 2) {
    Json get = Json::object();
    get["cmd"] = "get";
    Json state = control_request(get);
    const Json& cur = state.get("config").get(argv[1]);
    if (!cur.is_bool()) {
      fprintf(stderr, "%s is not a boolean setting\n", argv[1]);
      return 2;
    }
    req["cmd"] = "set";
    req["values"][argv[1]] = !cur.as_bool();
  } else if (sub == "watch") {
    // Poll rather than subscribe: keeps the client trivial.
    while (true) {
      Json r = Json::object();
      r["cmd"] = "status";
      printf("%s\n", control_request(r).dump().c_str());
      fflush(stdout);
      usleep(200000);
    }
  } else {
    usage();
    return 2;
  }
  Json reply = control_request(req);
  printf("%s\n", reply.dump(2).c_str());
  return reply.get("type").as_str() == "error" ? 1 : 0;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    usage();
    return 2;
  }
  std::string cmd = argv[1];
  try {
    if (cmd == "daemon") return run_daemon();
    if (cmd == "calibrate") return run_calibrate(argc - 2, argv + 2);
    if (cmd == "design") return run_design(argc - 2, argv + 2);
    if (cmd == "verify") return run_verify(argc - 2, argv + 2);
    if (cmd == "setup") return run_setup(argc - 2, argv + 2);
    if (cmd == "ctl") return run_ctl(argc - 2, argv + 2);
    if (cmd == "probe") return run_probe(argc - 2, argv + 2);
    if (cmd == "studio") return run_studio(argc - 2, argv + 2);
    if (cmd == "-h" || cmd == "--help" || cmd == "help") {
      usage();
      return 0;
    }
  } catch (const std::exception& e) {
    fprintf(stderr, "roomcorr: %s\n", e.what());
    return 1;
  }
  usage();
  return 2;
}
