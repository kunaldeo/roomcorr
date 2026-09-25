#include "config.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unistd.h>

namespace rc {

const char* const kChanNames[kNumChans] = {"left", "right", "sub"};

namespace fs = std::filesystem;

static std::string home() {
  const char* h = getenv("HOME");
  return h ? h : "/tmp";
}

std::string config_dir() {
  const char* x = getenv("XDG_CONFIG_HOME");
  return (x && *x ? std::string(x) : home() + "/.config") + "/roomcorr";
}

std::string data_dir() {
  const char* x = getenv("XDG_DATA_HOME");
  return (x && *x ? std::string(x) : home() + "/.local/share") + "/roomcorr";
}

std::string config_path() { return config_dir() + "/config.json"; }

std::string socket_path() {
  if (const char* o = getenv("ROOMCORR_SOCKET"); o && *o) return o;
  const char* x = getenv("XDG_RUNTIME_DIR");
  return (x && *x ? std::string(x) : "/tmp") + "/roomcorr.sock";
}

void ensure_dir(const std::string& path) { fs::create_directories(path); }

std::string read_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) throw std::runtime_error("cannot read " + path);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

void write_file_atomic(const std::string& path, const std::string& data) {
  ensure_dir(fs::path(path).parent_path());
  std::string tmp = path + ".tmp." + std::to_string(getpid());
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) throw std::runtime_error("cannot write " + tmp);
    f << data;
  }
  fs::rename(tmp, path);
}

// ------------------------------------------------------------------ JSON I/O

static Json channel_to_json(const ChannelConfig& c) {
  Json j = Json::object();
  j["trim_db"] = c.trim_db;
  j["delay_ms"] = c.delay_ms;
  j["invert"] = c.invert;
  j["filter"] = c.filter;
  j["filter_peak_db"] = c.filter_peak_db;
  return j;
}

static void channel_from_json(ChannelConfig& c, const Json& j) {
  c.trim_db = j.get("trim_db").as_num(c.trim_db);
  c.delay_ms = j.get("delay_ms").as_num(c.delay_ms);
  c.invert = j.get("invert").as_bool(c.invert);
  c.filter = j.get("filter").as_str(c.filter);
  c.filter_peak_db = j.get("filter_peak_db").as_num(c.filter_peak_db);
}

Json Config::to_json() const {
  Json j = Json::object();
  j["output_device"] = output_device;
  j["sub_outputs"] = Json(sub_outputs);
  j["enabled"] = enabled;
  j["room_eq"] = room_eq;
  j["bass_management"] = bass_management;
  j["mains_highpass"] = mains_highpass;
  j["mute"] = mute;
  j["limiter"] = limiter;
  j["crossover_hz"] = crossover_hz;
  j["crossover_slope"] = crossover_slope;
  j["sub_gain_db"] = sub_gain_db;
  j["bass_db"] = bass_db;
  j["treble_db"] = treble_db;
  j["headroom_db"] = headroom_db;
  Json chans = Json::object();
  for (int i = 0; i < kNumChans; ++i) chans[kChanNames[i]] = channel_to_json(ch[i]);
  j["channels"] = chans;
  j["mic_cal"] = mic_cal;
  j["mic_match"] = mic_match;
  j["positions"] = positions;
  Json t = Json::object();
  t["bass_boost_db"] = target.bass_boost_db;
  t["bass_corner_hz"] = target.bass_corner_hz;
  t["tilt_db_per_oct"] = target.tilt_db_per_oct;
  t["tilt_start_hz"] = target.tilt_start_hz;
  t["max_boost_db"] = target.max_boost_db;
  t["max_cut_db"] = target.max_cut_db;
  t["low_hz"] = target.low_hz;
  t["high_hz"] = target.high_hz;
  j["target"] = t;
  j["measurement"] = measurement;
  return j;
}

Config Config::from_json(const Json& j) {
  Config c;
  c.output_device = j.get("output_device").as_str(c.output_device);
  if (j.get("sub_outputs").is_array()) c.sub_outputs = j.get("sub_outputs").str_vector();
  c.enabled = j.get("enabled").as_bool(c.enabled);
  c.room_eq = j.get("room_eq").as_bool(c.room_eq);
  c.bass_management = j.get("bass_management").as_bool(c.bass_management);
  c.mains_highpass = j.get("mains_highpass").as_bool(c.mains_highpass);
  c.mute = j.get("mute").as_bool(c.mute);
  c.limiter = j.get("limiter").as_bool(c.limiter);
  c.crossover_hz = j.get("crossover_hz").as_num(c.crossover_hz);
  c.crossover_slope = j.get("crossover_slope").as_int(c.crossover_slope);
  c.sub_gain_db = j.get("sub_gain_db").as_num(c.sub_gain_db);
  c.bass_db = j.get("bass_db").as_num(c.bass_db);
  c.treble_db = j.get("treble_db").as_num(c.treble_db);
  c.headroom_db = j.get("headroom_db").as_num(c.headroom_db);
  for (int i = 0; i < kNumChans; ++i) channel_from_json(c.ch[i], j.get("channels").get(kChanNames[i]));
  c.mic_cal = j.get("mic_cal").as_str(c.mic_cal);
  c.mic_match = j.get("mic_match").as_str(c.mic_match);
  c.positions = j.get("positions").as_int(c.positions);
  const Json& t = j.get("target");
  c.target.bass_boost_db = t.get("bass_boost_db").as_num(c.target.bass_boost_db);
  c.target.bass_corner_hz = t.get("bass_corner_hz").as_num(c.target.bass_corner_hz);
  c.target.tilt_db_per_oct = t.get("tilt_db_per_oct").as_num(c.target.tilt_db_per_oct);
  c.target.tilt_start_hz = t.get("tilt_start_hz").as_num(c.target.tilt_start_hz);
  c.target.max_boost_db = t.get("max_boost_db").as_num(c.target.max_boost_db);
  c.target.max_cut_db = t.get("max_cut_db").as_num(c.target.max_cut_db);
  c.target.low_hz = t.get("low_hz").as_num(c.target.low_hz);
  c.target.high_hz = t.get("high_hz").as_num(c.target.high_hz);
  c.measurement = j.get("measurement").as_str(c.measurement);

  // Clamp anything the engine would choke on.
  c.crossover_hz = std::clamp(c.crossover_hz, 30.0, 250.0);
  if (c.crossover_slope != 48) c.crossover_slope = 24;
  c.sub_gain_db = std::clamp(c.sub_gain_db, -20.0, 15.0);
  c.bass_db = std::clamp(c.bass_db, -12.0, 12.0);
  c.treble_db = std::clamp(c.treble_db, -12.0, 12.0);
  c.headroom_db = std::clamp(c.headroom_db, 0.0, 30.0);
  return c;
}

bool Config::set(const std::string& key, const Json& value) {
  // Round-trip through JSON so every key goes through the same parsing and
  // clamping as the config file.
  Json j = to_json();
  auto dot = key.find('.');
  if (dot == std::string::npos) {
    if (!j.has(key)) return false;
    j[key] = value;
  } else {
    std::string outer = key.substr(0, dot), inner = key.substr(dot + 1);
    if (outer == "channels") {
      auto dot2 = inner.find('.');
      if (dot2 == std::string::npos) return false;
      std::string chan = inner.substr(0, dot2), field = inner.substr(dot2 + 1);
      if (!j["channels"].has(chan) || !j["channels"][chan].has(field)) return false;
      j["channels"][chan][field] = value;
    } else {
      if (!j.has(outer) || !j[outer].has(inner)) return false;
      j[outer][inner] = value;
    }
  }
  *this = from_json(j);
  return true;
}

double Config::auto_preamp_db() const {
  double worst = 0;
  for (int i = 0; i < kNumChans; ++i) {
    // Independent of room_eq/enabled so toggling them stays level-matched.
    // Tone and sub-gain boosts are left to the limiter: folding them in here
    // would turn the whole system down whenever the bass slider goes up.
    double g = ch[i].trim_db + (ch[i].filter.empty() ? 0 : ch[i].filter_peak_db);
    worst = std::max(worst, g);
  }
  return -worst - headroom_db;
}

Config load_config() {
  try {
    return Config::from_json(Json::parse(read_file(config_path())));
  } catch (const std::exception&) {
    return Config{};
  }
}

void save_config(const Config& c) { write_file_atomic(config_path(), c.to_json().dump(2) + "\n"); }

}  // namespace rc
