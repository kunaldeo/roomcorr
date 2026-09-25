// Persistent settings shared by the daemon, the calibrator and the control
// protocol. Stored as JSON in ~/.config/roomcorr/config.json.
#pragma once

#include <string>
#include <vector>

#include "json.hpp"

namespace rc {

constexpr int kSampleRate = 48000;

// Output channels after bass management.
enum Chan { kLeft = 0, kRight = 1, kSub = 2, kNumChans = 3 };
extern const char* const kChanNames[kNumChans];  // "left", "right", "sub"

struct ChannelConfig {
  double trim_db = 0;     // level match from calibration
  double delay_ms = 0;    // relative; the engine subtracts the minimum
  bool invert = false;    // polarity
  std::string filter;     // FIR (mono float WAV at 48 kHz); empty = none
  double filter_peak_db = 0;  // max boost of the FIR, for headroom
};

// House curve. Defaults follow the Harman in-room target (Olive, Welti &
// McMullin 2013): a 2nd-order +6.6 dB bass shelf at 105 Hz and a -2.4 dB
// treble shelf above ~2.5 kHz, which listeners consistently preferred.
struct TargetCurve {
  double bass_boost_db = 6.5;   // low-shelf lift
  double bass_corner_hz = 105;  // shelf midpoint
  double treble_db = -2.5;      // high-shelf (negative = gentle roll-off)
  double treble_hz = 2500;
  double tilt_db_per_oct = 0;   // optional extra tilt above tilt_start_hz
  double tilt_start_hz = 1000;
  double max_boost_db = 4.0;    // never fill a dip by more than this
  double max_cut_db = 15.0;
  double low_hz = 0;            // 0 = derived from each speaker's roll-off
  double high_hz = 20000;       // correction upper limit
  // Above the room's transition frequency a single mic mostly measures
  // reflections, and narrow EQ there makes speakers sound hollow (Toole).
  // Full-resolution correction below `transition_hz`, only broad
  // (octave-smoothed, +/- hf_max_db) correction above twice that.
  double transition_hz = 250;
  double hf_max_db = 2.0;
  bool auto_crossover = true;   // pick the crossover that sums best in the room
};

struct Config {
  // Routing
  std::string output_device;  // PipeWire node.name of the 5.1 X4 sink
  std::vector<std::string> sub_outputs = {"FC", "LFE"};  // X4 C/Sub jack: tip=FC, ring=LFE

  // Processing switches
  bool enabled = true;          // false = level-matched plain stereo bypass
  bool room_eq = true;          // FIR correction
  bool bass_management = true;  // route bass to the sub
  bool mains_highpass = true;   // false = "large" mains, sub only adds
  bool mute = false;
  bool limiter = true;

  // Bass management
  double crossover_hz = 80;
  int crossover_slope = 24;  // 24 (LR4) or 48 (LR8) dB/oct

  // User adjustments on top of calibration
  double sub_gain_db = 0;
  double bass_db = 0;     // low shelf @ 100 Hz
  double treble_db = 0;   // high shelf @ 8 kHz
  double headroom_db = 0; // extra attenuation beyond the automatic amount

  ChannelConfig ch[kNumChans];

  // Calibration
  std::string mic_cal;  // UMIK-1 calibration file
  std::string mic_match = "UMIK";
  int positions = 5;
  TargetCurve target;
  std::string measurement;  // directory of the measurement set in use

  Json to_json() const;
  static Config from_json(const Json& j);

  // Applies one "key": value pair from the control protocol. Returns false
  // for an unknown key. Nested keys use dots: "target.bass_boost_db".
  bool set(const std::string& key, const Json& value);

  // Automatic preamp: enough attenuation that the correction filters plus
  // positive trims cannot push a full-scale signal into clipping.
  double auto_preamp_db() const;
};

std::string config_dir();     // ~/.config/roomcorr
std::string data_dir();       // ~/.local/share/roomcorr
std::string config_path();    // ~/.config/roomcorr/config.json
std::string socket_path();    // $XDG_RUNTIME_DIR/roomcorr.sock
void ensure_dir(const std::string& path);

Config load_config();  // defaults if the file is missing
void save_config(const Config& c);

std::string read_file(const std::string& path);
void write_file_atomic(const std::string& path, const std::string& data);

}  // namespace rc
