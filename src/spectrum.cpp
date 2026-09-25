#include "spectrum.hpp"

#include <algorithm>
#include <cmath>

namespace rc {

SpectrumAnalyzer::SpectrumAnalyzer() {
  in_ = fftwf_alloc_real(kSize);
  out_ = fftwf_alloc_complex(kSize / 2 + 1);
  plan_ = fftwf_plan_dft_r2c_1d(kSize, in_, out_, FFTW_MEASURE);
  window_.resize(kSize);
  double s2 = 0;
  for (int i = 0; i < kSize; ++i) {
    window_[size_t(i)] = float(0.5 - 0.5 * std::cos(2 * M_PI * i / kSize));
    s2 += double(window_[size_t(i)]) * window_[size_t(i)];
  }
  // One-sided power per bin -> mean square of that component (Parseval).
  norm_ = 2.0 / (double(kSize) * s2);
  const double bin = double(kSampleRate) / kSize;
  for (double f = 20; f <= 20001; f *= std::pow(2.0, 1.0 / 6)) {
    freqs_.push_back(f);
    double a = f * std::pow(2.0, -1.0 / 12) / bin, b = f * std::pow(2.0, 1.0 / 12) / bin;
    int lo = std::max(1, int(std::ceil(a))), hi = std::min(kSize / 2, int(std::floor(b)));
    // In the deep bass a band is narrower than the Hann window's main lobe
    // (3 bins); take the lobe around the centre so a tone reads its level.
    if (hi - lo < 2) {
      int c = std::clamp(int(std::lround(f / bin)), 2, kSize / 2 - 1);
      lo = c - 1;
      hi = c + 1;
    }
    lo_.push_back(lo);
    hi_.push_back(hi);
  }
}

SpectrumAnalyzer::~SpectrumAnalyzer() {
  fftwf_destroy_plan(plan_);
  fftwf_free(in_);
  fftwf_free(out_);
}

std::vector<double> SpectrumAnalyzer::analyze(const Engine& engine, int tap) {
  engine.read_tap(tap, in_, kSize);
  for (int i = 0; i < kSize; ++i) in_[i] *= window_[size_t(i)];
  fftwf_execute(plan_);
  std::vector<double> out(freqs_.size());
  for (size_t b = 0; b < freqs_.size(); ++b) {
    double p = 0;
    for (int k = lo_[b]; k <= hi_[b]; ++k) p += double(out_[k][0]) * out_[k][0] + double(out_[k][1]) * out_[k][1];
    double db = 10 * std::log10(p * norm_ + 1e-14) + 3.0103;
    out[b] = std::round(std::max(-100.0, db) * 10) / 10;
  }
  return out;
}

}  // namespace rc
