// Uniformly partitioned overlap-save FFT convolution (UPOLS).
//
// Latency is exactly one block. Filters can be swapped from a non-realtime
// thread at any time; the audio thread picks the new filter up at the next
// block and crossfades from the old one over that block, so toggling room EQ
// or loading a new calibration never clicks.
#pragma once

#include <atomic>
#include <memory>
#include <vector>

#include <fftw3.h>

namespace rc {

class Convolver {
public:
  // block: samples per process() call. max_taps: longest filter accepted.
  Convolver(int block, int max_taps);
  ~Convolver();
  Convolver(const Convolver&) = delete;
  Convolver& operator=(const Convolver&) = delete;

  // Non-realtime. An empty vector means a unit impulse (pass-through).
  // Filters longer than max_taps are truncated.
  void set_filter(const std::vector<float>& ir);

  // Non-realtime: frees filters the audio thread has retired. Call now and
  // then; set_filter also calls it.
  void collect();

  // Realtime: in and out are `block` samples and may alias.
  void process(const float* in, float* out);

  int block() const { return B_; }

private:
  struct Spectrum {
    int parts = 0;
    std::vector<float> re, im;  // parts * bins, partition-major
  };

  std::unique_ptr<Spectrum> make_spectrum(const std::vector<float>& ir);
  void accumulate(const Spectrum& h, float* acc_re, float* acc_im);
  void inverse(const float* acc_re, const float* acc_im, float* out);

  int B_, N_, bins_, max_parts_;
  fftwf_plan fwd_ = nullptr, inv_ = nullptr;
  float* time_ = nullptr;            // N samples: previous block | current block
  fftwf_complex* freq_ = nullptr;    // bins
  float* ifft_out_ = nullptr;        // N samples
  std::vector<float> fdl_re_, fdl_im_;  // frequency-domain delay line, max_parts * bins
  int fdl_pos_ = 0;
  std::vector<float> acc_re_, acc_im_, acc2_re_, acc2_im_, tmp_out_;

  Spectrum* current_ = nullptr;                 // owned by the audio thread
  std::atomic<Spectrum*> pending_{nullptr};     // posted by set_filter
  std::atomic<Spectrum*> retired_{nullptr};     // handed back for freeing
};

}  // namespace rc
