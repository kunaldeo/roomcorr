#include "devices.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

namespace rc {

namespace fs = std::filesystem;

std::string shell_quote(const std::string& s) {
  std::string out = "'";
  for (char c : s) out += c == '\'' ? std::string("'\\''") : std::string(1, c);
  return out + "'";
}

std::string run_capture(const std::string& cmd) {
  std::string out;
  FILE* p = popen(cmd.c_str(), "r");
  if (!p) return out;
  std::array<char, 4096> buf;
  size_t n;
  while ((n = fread(buf.data(), 1, buf.size(), p)) > 0) out.append(buf.data(), n);
  pclose(p);
  return out;
}

int run_cmd(const std::string& cmd) {
  int r = system(cmd.c_str());
  return WIFEXITED(r) ? WEXITSTATUS(r) : -1;
}

Json pactl_list(const std::string& what) {
  std::string out = run_capture("pactl -f json list " + what + " 2>/dev/null");
  try {
    return Json::parse(out);
  } catch (const std::exception&) {
    return Json::array();
  }
}

static std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return s;
}

std::optional<std::string> find_mic(const Config& cfg) {
  std::string match = lower(cfg.mic_match);
  const Json list = pactl_list("sources");
  for (const auto& s : list.arr()) {
    std::string name = s.get("name").as_str();
    if (name.find(".monitor") != std::string::npos) continue;
    std::string hay = lower(name + " " + s.get("description").as_str());
    if (hay.find(match) != std::string::npos || hay.find("minidsp") != std::string::npos) return name;
  }
  return std::nullopt;
}

std::optional<std::string> find_x4_card() {
  const Json list = pactl_list("cards");
  for (const auto& c : list.arr()) {
    std::string name = c.get("name").as_str();
    if (name.find("Sound_Blaster_X4") != std::string::npos) return name;
  }
  return std::nullopt;
}

bool sink_exists(const std::string& name) {
  const Json list = pactl_list("sinks");
  for (const auto& s : list.arr())
    if (s.get("name").as_str() == name) return true;
  return false;
}

double sink_volume(const std::string& name) {
  const Json list = pactl_list("sinks");
  for (const auto& s : list.arr()) {
    if (s.get("name").as_str() != name) continue;
    for (const auto& [ch, v] : s.get("volume").obj()) return v.get("value").as_num(65536) / 65536.0;
  }
  return -1;
}

void set_sink_volume(const std::string& name, double fraction) {
  char pct[32];
  snprintf(pct, sizeof pct, "%.0f%%", std::clamp(fraction, 0.0, 1.5) * 100);
  run_cmd("pactl set-sink-volume " + shell_quote(name) + " " + pct);
}

std::optional<std::string> find_mic_cal_file() {
  const char* home = getenv("HOME");
  std::vector<std::string> dirs = {config_dir(), home ? std::string(home) + "/Downloads" : ""};
  for (const auto& d : dirs) {
    if (d.empty() || !fs::is_directory(d)) continue;
    for (const auto& e : fs::directory_iterator(d)) {
      std::string n = e.path().filename().string();
      if (n.find("90deg") != std::string::npos && e.path().extension() == ".txt") return e.path().string();
    }
  }
  return std::nullopt;
}

}  // namespace rc
