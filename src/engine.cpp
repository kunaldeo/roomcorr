#include "engine.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>

namespace rc {

const char* const kOutPosNames[kNumOut] = {"FL", "FR", "FC", "LFE", "RL", "RR"};

static constexpr int kDelayMask = 8191;  // ring size 8192 > kMaxDelay + kBlock
static constexpr double kLimitThreshold = 0.966;  // -0.3 dBFS

static inline double db_to_lin(double db) { return std::pow(10.0, db / 20.0); }

static inline void store_max(std::atomic<float>& a, float v) {
  if (v > a.load(std::memory_order_relaxed)) a.store(v, std::memory_order_relaxed);
}

static inline void add_relaxed(std::atomic<double>& a, double v) {
  a.store(a.load(std::memory_order_relaxed) + v, std::memory_order_relaxed);
}

EngineParams EngineParams::from_config(const Config& c) {
  EngineParams p;
  p.enabled = c.enabled;
  p.room_eq = c.room_eq;
  p.bass_management = c.bass_management;
  p.mains_highpass = c.mains_highpass;
  p.mute = c.mute;
  p.limiter = c.limiter;
  p.crossover_hz = c.crossover_hz;
  p.crossover_slope = c.crossover_slope;
  p.preamp_db = c.auto_preamp_db();
  p.bass_db = c.bass_db;
  p.treble_db = c.treble_db;
  double min_delay = 1e9;
  for (int i = 0; i < kNumChans; ++i) min_delay = std::min(min_delay, c.ch[i].delay_ms);
  for (int i = 0; i < kNumChans; ++i) {
    p.gain_db[i] = c.ch[i].trim_db + (i == kSub ? c.sub_gain_db : 0);
    p.invert[i] = c.ch[i].invert;
    int d = int(std::lround((c.ch[i].delay_ms - min_delay) * kSampleRate / 1000.0));
    p.delay_samples[i] = std::clamp(d, 0, Engine::kMaxDelay);
  }
  for (int o = 0; o < kNumOut; ++o) p.sub_to[o] = false;
  for (const auto& name : c.sub_outputs)
    for (int o = 0; o < kNumOut; ++o)
      if (name == kOutPosNames[o] && o != kOutFL && o != kOutFR) p.sub_to[o] = true;
  return p;
}

Engine::Engine() {
  for (auto& c : conv_) c = std::make_unique<Convolver>(kBlock, kMaxTaps);
  for (auto& d : delay_buf_) d.assign(kDelayMask + 1, 0.f);
  // The LFE channel is band-limited to 120 Hz by convention; filter it
  // there regardless of the crossover.
  lfe_lp_.configure(24, false, 120, kSampleRate);
  smooth_coef_ = 1.0 - std::exp(-1.0 / (0.010 * kSampleRate));   // 10 ms
  release_coef_ = 1.0 - std::exp(-1.0 / (0.150 * kSampleRate));  // 150 ms
  apply_params(p_);
}

void Engine::set_params(const EngineParams& p) {
  std::lock_guard<std::mutex> lock(param_mutex_);
  pending_ = p;
  params_dirty_.store(true, std::memory_order_release);
}

void Engine::set_filter(int chan, const std::vector<float>& ir) {
  filters_[chan] = ir;
  if (eq_on_) conv_[chan]->set_filter(ir);
}

void Engine::set_eq_filters_enabled(bool on) {
  if (on == eq_on_) return;
  eq_on_ = on;
  for (int i = 0; i < kNumChans; ++i) conv_[i]->set_filter(on ? filters_[i] : std::vector<float>{});
}

void Engine::collect_garbage() {
  for (auto& c : conv_) c->collect();
}

EngineStats Engine::read_stats() {
  EngineStats s;
  uint64_t n = std::max<uint64_t>(1, sq_count_.exchange(0));
  for (int i = 0; i < 2; ++i) {
    s.in[i].peak = in_peak_[i].exchange(0);
    s.in[i].rms = float(std::sqrt(in_sq_[i].exchange(0) / double(n)));
  }
  for (int i = 0; i < kNumChans; ++i) {
    s.out[i].peak = out_peak_[i].exchange(0);
    s.out[i].rms = float(std::sqrt(out_sq_[i].exchange(0) / double(n)));
  }
  s.limiter_gr_db = gr_max_.exchange(0);
  s.clipped = clipped_.load();
  s.load = load_.exchange(0);
  return s;
}

void Engine::apply_params(const EngineParams& p) {
  p_ = p;
  const double fs = kSampleRate;
  if (snap_) {
    g_pre_ = db_to_lin(p.preamp_db);
    for (int c = 0; c < kNumChans; ++c) g_ch_[c] = db_to_lin(p.gain_db[c]) * (p.invert[c] ? -1.0 : 1.0);
    g_mix_ = p.enabled ? 1.0 : 0.0;
    g_mute_ = p.mute ? 0.0 : 1.0;
  }
  if (p.crossover_hz != cur_xover_ || p.crossover_slope != cur_slope_) {
    for (auto& h : hp_) h.configure(p.crossover_slope, true, p.crossover_hz, fs);
    lp_.configure(p.crossover_slope, false, p.crossover_hz, fs);
    cur_xover_ = p.crossover_hz;
    cur_slope_ = p.crossover_slope;
  }
  if (p.bass_db != cur_bass_) {
    for (auto& b : low_shelf_) b.k = BiquadCoeffs::lowshelf(100, p.bass_db, fs);
    cur_bass_ = p.bass_db;
  }
  if (p.treble_db != cur_treble_) {
    for (auto& b : high_shelf_) b.k = BiquadCoeffs::highshelf(8000, p.treble_db, fs);
    cur_treble_ = p.treble_db;
  }
}

void Engine::process(const float* const* in, float* const* out, int n) {
  timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);

  if (params_dirty_.load(std::memory_order_acquire)) {
    std::unique_lock<std::mutex> lock(param_mutex_, std::try_to_lock);
    if (lock.owns_lock()) {
      params_dirty_.store(false, std::memory_order_relaxed);
      apply_params(pending_);
      snap_ = false;
    }
  }

  int i = 0;
  while (i < n) {
    int k = std::min(n - i, kBlock - fifo_pos_);
    for (int c = 0; c < kNumOut; ++c) {
      if (in[c])
        std::memcpy(&in_blk_[c][fifo_pos_], in[c] + i, sizeof(float) * size_t(k));
      else
        std::memset(&in_blk_[c][fifo_pos_], 0, sizeof(float) * size_t(k));
    }
    for (int o = 0; o < kNumOut; ++o)
      if (out[o]) std::memcpy(out[o] + i, &out_blk_[o][fifo_pos_], sizeof(float) * size_t(k));
    fifo_pos_ += k;
    i += k;
    if (fifo_pos_ == kBlock) {
      process_block();
      fifo_pos_ = 0;
    }
  }

  clock_gettime(CLOCK_MONOTONIC, &t1);
  double used = double(t1.tv_sec - t0.tv_sec) + double(t1.tv_nsec - t0.tv_nsec) * 1e-9;
  if (n > 0) store_max(load_, float(used / (double(n) / kSampleRate)));
}

void Engine::process_block() {
  const int B = kBlock;
  const EngineParams& p = p_;

  // ---- 5.1 -> L/R fold-down (ITU-R BS.775): centre and surrounds at -3 dB.
  // A stereo source leaves those channels at zero, so this is a no-op then.
  constexpr float kFold = 0.70710678f;
  for (int s = 0; s < B; ++s) {
    float c = in_blk_[kOutFC][s] * kFold;
    in_blk_[kOutFL][s] += c + in_blk_[kOutRL][s] * kFold;
    in_blk_[kOutFR][s] += c + in_blk_[kOutRR][s] * kFold;
  }

  // ---- input meters
  for (int c = 0; c < 2; ++c) {
    float pk = 0;
    double sq = 0;
    for (int s = 0; s < B; ++s) {
      float v = in_blk_[c][s];
      pk = std::max(pk, std::fabs(v));
      sq += double(v) * v;
    }
    store_max(in_peak_[c], pk);
    add_relaxed(in_sq_[c], sq);
  }

  // ---- preamp, tone, bass management (per sample, double precision)
  const double pre_target = db_to_lin(p.preamp_db);
  float* mL = bm_[kLeft];
  float* mR = bm_[kRight];
  float* sub = bm_[kSub];
  double pre[kBlock];
  for (int s = 0; s < B; ++s) {
    g_pre_ += (pre_target - g_pre_) * smooth_coef_;
    pre[s] = g_pre_;
    double l = in_blk_[0][s] * g_pre_;
    double r = in_blk_[1][s] * g_pre_;
    l = high_shelf_[0].tick(low_shelf_[0].tick(l));
    r = high_shelf_[1].tick(low_shelf_[1].tick(r));
    if (p.bass_management) {
      // Redirected bass: both channels at unity, as an AV receiver does.
      // Calibration matches the sub's SPL to a single main speaker, so a mono
      // bass note lands at the same level it would from the pair of mains.
      // Plus the LFE channel at its standard +10 dB in-band gain.
      sub[s] = float(lp_.tick(l + r) + lfe_lp_.tick(in_blk_[kOutLFE][s] * g_pre_) * 3.16227766);
      if (p.mains_highpass) {
        l = hp_[0].tick(l);
        r = hp_[1].tick(r);
      }
    } else {
      sub[s] = 0.f;
    }
    mL[s] = float(l);
    mR[s] = float(r);
  }

  // ---- room correction FIRs (unity filters when EQ is off)
  for (int c = 0; c < kNumChans; ++c) conv_[c]->process(bm_[c], bm_[c]);

  // ---- trims, polarity, delays
  for (int c = 0; c < kNumChans; ++c) {
    double target = db_to_lin(p.gain_db[c]) * (p.invert[c] ? -1.0 : 1.0);
    double g = g_ch_[c];
    std::vector<float>& dl = delay_buf_[c];
    int d = p.delay_samples[c];
    float* x = bm_[c];
    for (int s = 0; s < B; ++s) {
      g += (target - g) * smooth_coef_;
      int w = (delay_pos_ + s) & kDelayMask;
      dl[size_t(w)] = float(x[s] * g);
      x[s] = dl[size_t((w - d) & kDelayMask)];
    }
    g_ch_[c] = g;
  }
  delay_pos_ = (delay_pos_ + B) & kDelayMask;

  // ---- bypass crossfade, mute, limiter, output
  const double mix_target = p.enabled ? 1.0 : 0.0;
  const double mute_target = p.mute ? 0.0 : 1.0;
  double pk[kNumChans] = {0, 0, 0}, sq[kNumChans] = {0, 0, 0};
  float gr_worst = 0;
  uint64_t clips = 0;
  for (int s = 0; s < B; ++s) {
    g_mix_ += (mix_target - g_mix_) * smooth_coef_;
    g_mute_ += (mute_target - g_mute_) * smooth_coef_;
    // Bypass is plain stereo through the same preamp, so A/B is level-matched.
    double v[kNumChans] = {
        (mL[s] * g_mix_ + in_blk_[0][s] * pre[s] * (1 - g_mix_)) * g_mute_,
        (mR[s] * g_mix_ + in_blk_[1][s] * pre[s] * (1 - g_mix_)) * g_mute_,
        sub[s] * g_mix_ * g_mute_,
    };
    double peak = std::max({std::fabs(v[0]), std::fabs(v[1]), std::fabs(v[2])});
    if (p.limiter) {
      // Linked peak limiter with instant attack: one gain for all channels
      // keeps the image and the sub/main balance intact while limiting.
      if (peak * lim_gain_ > kLimitThreshold) {
        lim_gain_ = kLimitThreshold / peak;
        ++clips;
      } else {
        lim_gain_ += (1.0 - lim_gain_) * release_coef_;
      }
      for (double& x : v) x *= lim_gain_;
      gr_worst = std::max(gr_worst, float(-20.0 * std::log10(lim_gain_)));
    } else {
      for (double& x : v) {
        if (std::fabs(x) > 1.0) ++clips;
        x = std::clamp(x, -1.0, 1.0);
      }
    }
    for (int c = 0; c < kNumChans; ++c) {
      pk[c] = std::max(pk[c], std::fabs(v[c]));
      sq[c] += v[c] * v[c];
    }
    out_blk_[kOutFL][s] = float(v[kLeft]);
    out_blk_[kOutFR][s] = float(v[kRight]);
    for (int o = kOutFC; o < kNumOut; ++o) out_blk_[o][s] = p.sub_to[o] ? float(v[kSub]) : 0.f;
  }

  for (int c = 0; c < kNumChans; ++c) {
    store_max(out_peak_[c], float(pk[c]));
    add_relaxed(out_sq_[c], sq[c]);
  }
  sq_count_.store(sq_count_.load(std::memory_order_relaxed) + B, std::memory_order_relaxed);
  store_max(gr_max_, gr_worst);
  if (clips) clipped_.fetch_add(clips, std::memory_order_relaxed);
}

}  // namespace rc
