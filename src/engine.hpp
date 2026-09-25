// The realtime DSP graph, independent of PipeWire so it can be tested offline.
//
// Input is 5.1 (stereo sources simply leave FC/LFE/RL/RR silent). Like an
// AV receiver with "small" mains and one sub:
//   FC, RL, RR fold into L/R (ITU downmix, -3 dB)
//   L,R ─ preamp ─ tone ─┬─ HPF ─ FIR[left/right] ─ trim ─ delay ─┐
//                        └─ LPF(L+R) + LFE·(+10 dB) ─ FIR[sub] ─ trim/pol ─ delay ─┤─ bypass ─ limiter ─ out
//
// Audio arrives in arbitrary chunk sizes; internally everything runs in
// fixed blocks of kBlock samples, which is also the engine's latency.
#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include "config.hpp"
#include "dsp/biquad.hpp"
#include "dsp/convolver.hpp"

namespace rc {

// Output positions sent to the X4 in its 5.1 profile.
enum OutPos { kOutFL = 0, kOutFR, kOutFC, kOutLFE, kOutRL, kOutRR, kNumOut };
extern const char* const kOutPosNames[kNumOut];

struct Meter {
  float peak = 0;  // linear, max since last read
  float rms = 0;   // linear, over the last read interval
};

struct EngineStats {
  Meter in[2];
  Meter out[kNumChans];
  float limiter_gr_db = 0;   // largest gain reduction since last read
  uint64_t clipped = 0;      // samples that hit the limiter since start
  float load = 0;            // DSP time / audio time, max since last read
};

// Everything the audio thread needs, as plain values.
struct EngineParams {
  bool enabled = true, room_eq = true, bass_management = true, mains_highpass = true, mute = false,
       limiter = true;
  double crossover_hz = 80;
  int crossover_slope = 24;
  double preamp_db = 0, bass_db = 0, treble_db = 0;
  double gain_db[kNumChans] = {0, 0, 0};
  bool invert[kNumChans] = {false, false, false};
  int delay_samples[kNumChans] = {0, 0, 0};
  bool sub_to[kNumOut] = {false, false, true, true, false, false};

  static EngineParams from_config(const Config& c);
};

class Engine {
public:
  static constexpr int kBlock = 256;
  static constexpr int kMaxTaps = 262144;
  static constexpr int kMaxDelay = 4096;  // 85 ms

  Engine();

  // Non-realtime.
  void set_params(const EngineParams& p);
  void set_filter(int chan, const std::vector<float>& ir);  // empty = unity
  void set_eq_filters_enabled(bool on);  // swaps FIRs for unity and back
  void collect_garbage();
  EngineStats read_stats();  // resets the peak-hold values

  // Spectrum tap: the most recent kTapSize samples of the input ((L+R)/2,
  // before any processing) and of each output, for the live analyzer.
  enum Tap { kTapIn = 0, kTapLeft, kTapRight, kTapSub, kNumTaps };
  static constexpr int kTapSize = 16384;  // power of two
  // Non-realtime: copies the newest n (<= kTapSize) samples of a tap.
  void read_tap(int tap, float* dst, int n) const;

  // Realtime. in and out: kNumOut planar channels (FL FR FC LFE RL RR);
  // null input pointers are silence.
  void process(const float* const* in, float* const* out, int n);

private:
  void process_block();
  void apply_params(const EngineParams& p);

  // Parameter handoff: writer locks, audio thread try_locks.
  std::mutex param_mutex_;
  EngineParams pending_;
  std::atomic<bool> params_dirty_{false};
  EngineParams p_;  // audio thread's copy

  // Unity-or-correction state for each FIR, owned by the control thread.
  std::vector<float> filters_[kNumChans];
  bool eq_on_ = true;

  // Block FIFO
  int fifo_pos_ = 0;
  float in_blk_[kNumOut][kBlock] = {};
  float out_blk_[kNumOut][kBlock] = {};

  // DSP state
  Biquad low_shelf_[2], high_shelf_[2];
  LinkwitzRiley hp_[2], lp_, lfe_lp_;
  std::unique_ptr<Convolver> conv_[kNumChans];
  std::vector<float> delay_buf_[kNumChans];
  int delay_pos_ = 0;
  float bm_[kNumChans][kBlock] = {};  // per-output working buffers

  // Smoothed gains (per sample one-pole)
  double g_pre_ = 1, g_ch_[kNumChans] = {1, 1, 1}, g_mix_ = 1, g_mute_ = 1;
  double lim_gain_ = 1;
  bool snap_ = true;  // jump straight to the first parameters instead of ramping
  double smooth_coef_, release_coef_;

  // Coefficients currently applied, to detect changes.
  double cur_xover_ = -1, cur_bass_ = 1e9, cur_treble_ = 1e9;
  int cur_slope_ = -1;

  std::vector<float> tap_[kNumTaps];
  std::atomic<uint32_t> tap_pos_{0};

  // Stats (audio thread writes, control thread reads)
  std::atomic<float> in_peak_[2]{}, out_peak_[kNumChans]{};
  std::atomic<double> in_sq_[2]{}, out_sq_[kNumChans]{};
  std::atomic<uint64_t> sq_count_{0};
  std::atomic<float> gr_max_{0};
  std::atomic<uint64_t> clipped_{0};
  std::atomic<float> load_{0};
};

}  // namespace rc
