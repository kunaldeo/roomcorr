// Plays a multichannel signal to one PipeWire node while recording the
// measurement microphone, with a fixed (if unknown) offset between the two.
//
// Everything played in one run shares that offset, so the relative arrival
// times of different speakers measured in one run are exact. That is why a
// whole position (left, right, sub) is measured as a single run.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace rc {

struct MeasureRequest {
  std::string play_target;                 // node.name to play into
  std::vector<std::string> positions;      // channel positions, e.g. FL FR FC LFE RL RR
  std::vector<std::vector<float>> signal;  // [channel][frame]
  std::string capture_target;              // mic node.name
  double tail_seconds = 1.0;               // keep recording after the signal ends
};

struct MeasureResult {
  std::vector<double> recording;  // starts at the signal's first sample (plus latency)
  double capture_peak_dbfs = -200;
};

MeasureResult run_measurement(const MeasureRequest& req);  // throws

// Live mode: loops `signal` (per channel, seamless loop) on the outputs and
// calls `tick` every interval_ms with the last `window_s` seconds of mic
// signal, until tick returns false. Fades in and out so the sub doesn't
// thump. Used for the live subwoofer level adjustment.
void run_live(const MeasureRequest& req, int interval_ms, double window_s,
              const std::function<bool(const std::vector<double>& recent)>& tick);  // throws

}  // namespace rc
