// 1/6-octave spectrum of the engine's taps, for the Studio's live analyzer.
#pragma once

#include <vector>

#include <fftw3.h>

#include "engine.hpp"

namespace rc {

class SpectrumAnalyzer {
public:
  static constexpr int kSize = 8192;  // 5.9 Hz bins, 170 ms window

  SpectrumAnalyzer();
  ~SpectrumAnalyzer();

  const std::vector<double>& freqs() const { return freqs_; }

  // Band levels in dBFS (AES17: a full-scale sine reads 0) for one tap.
  std::vector<double> analyze(const Engine& engine, int tap);

private:
  std::vector<double> freqs_;
  std::vector<int> lo_, hi_;  // bin range per band
  std::vector<float> window_;
  double norm_ = 1;
  float* in_ = nullptr;
  fftwf_complex* out_ = nullptr;
  fftwf_plan plan_ = nullptr;
};

}  // namespace rc
