// Turns a measurement set into correction filters, trims, delays and sub
// alignment. Pure computation: no audio I/O, so it can be re-run with a new
// target curve without re-measuring.
#pragma once

#include <string>
#include <vector>

#include "analysis.hpp"
#include "config.hpp"
#include "json.hpp"

namespace rc {

// One calibration session: impulse responses for every (position, channel),
// all sharing a time reference within a position.
struct MeasurementSet {
  std::string dir;
  int fs = kSampleRate;
  std::vector<std::vector<std::vector<double>>> irs;  // [position][chan]
  Json meta;
};

constexpr int kIrLength = 1 << 17;  // 2.7 s at 48 kHz
constexpr int kFilterTaps = 32768;  // 0.68 s: 1.5 Hz resolution for the sub

MeasurementSet load_measurements(const std::string& dir);  // throws
void save_measurements(const MeasurementSet& m);          // writes dir/*

struct DesignResult {
  Config cfg;  // input config with trims, delays, polarity, filters updated
  std::vector<float> filters[kNumChans];
  Json report;                     // response.json for the plugin graph
  std::vector<std::string> notes;  // human-readable summary
  std::vector<std::string> warnings;
  double bass_deviation_db = 99;  // predicted, 25-250 Hz, RMS from target
};

DesignResult design_filters(const Config& in, const MeasurementSet& m, const MicCal& cal);

// design_filters at the best crossover when target.auto_crossover is set.
DesignResult design_best(const Config& in, const MeasurementSet& m, const MicCal& cal);

// Writes filters to data_dir()/filters and the report to
// data_dir()/response.json, and points result.cfg at them.
void install_design(DesignResult& result);

std::string response_path();  // data_dir()/response.json

}  // namespace rc
