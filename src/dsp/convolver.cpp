#include "dsp/convolver.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

namespace rc {

// FFTW planning is not thread-safe; plan creation happens rarely, so one
// global lock is fine.
static std::mutex g_fftw_plan_mutex;

// Input below this (-160 dBFS) counts as silence for the idle skip.
static constexpr float kSilence = 1e-8f;

// ------------------------------------------------------------------ Stage

void Convolver::Stage::init(int blk, int parts, unsigned plan_flags) {
  block = blk;
  bins = blk + 1;
  max_parts = std::max(1, parts);
  time = fftwf_alloc_real(size_t(2 * blk));
  ifft_out = fftwf_alloc_real(size_t(2 * blk));
  freq = fftwf_alloc_complex(size_t(bins));
  {
    std::lock_guard<std::mutex> lock(g_fftw_plan_mutex);
    fwd = fftwf_plan_dft_r2c_1d(2 * blk, time, freq, plan_flags);
    inv = fftwf_plan_dft_c2r_1d(2 * blk, freq, ifft_out, plan_flags);
  }
  fdl_re.assign(size_t(max_parts) * bins, 0.f);
  fdl_im.assign(size_t(max_parts) * bins, 0.f);
  acc_re.assign(size_t(bins), 0.f);
  acc_im.assign(size_t(bins), 0.f);
  reset();
}

void Convolver::Stage::destroy() {
  if (!time) return;
  std::lock_guard<std::mutex> lock(g_fftw_plan_mutex);
  fftwf_destroy_plan(fwd);
  fftwf_destroy_plan(inv);
  fftwf_free(time);
  fftwf_free(ifft_out);
  fftwf_free(freq);
  time = nullptr;
}

void Convolver::Stage::reset() {
  std::fill(time, time + 2 * block, 0.f);
  std::fill(fdl_re.begin(), fdl_re.end(), 0.f);
  std::fill(fdl_im.begin(), fdl_im.end(), 0.f);
  fdl_pos = 0;
}

void Convolver::Stage::push(const float* in) {
  std::memmove(time, time + block, sizeof(float) * size_t(block));
  std::memcpy(time + block, in, sizeof(float) * size_t(block));
  fftwf_execute(fwd);
  fdl_pos = (fdl_pos + 1) % max_parts;
  float* xr = &fdl_re[size_t(fdl_pos) * bins];
  float* xi = &fdl_im[size_t(fdl_pos) * bins];
  for (int k = 0; k < bins; ++k) {
    xr[k] = freq[k][0];
    xi[k] = freq[k][1];
  }
}

void Convolver::Stage::accumulate(const float* hre, const float* him, int parts) {
  float* __restrict ar = acc_re.data();
  float* __restrict ai = acc_im.data();
  std::fill(ar, ar + bins, 0.f);
  std::fill(ai, ai + bins, 0.f);
  for (int p = 0; p < parts; ++p) {
    // Partition p pairs with the input spectrum from p blocks ago.
    int slot = fdl_pos - p;
    if (slot < 0) slot += max_parts;
    const float* __restrict xr = &fdl_re[size_t(slot) * bins];
    const float* __restrict xi = &fdl_im[size_t(slot) * bins];
    const float* __restrict hr = hre + size_t(p) * bins;
    const float* __restrict hi = him + size_t(p) * bins;
    for (int k = 0; k < bins; ++k) {
      ar[k] += xr[k] * hr[k] - xi[k] * hi[k];
      ai[k] += xr[k] * hi[k] + xi[k] * hr[k];
    }
  }
}

void Convolver::Stage::inverse(float* out) {
  for (int k = 0; k < bins; ++k) {
    freq[k][0] = acc_re[size_t(k)];
    freq[k][1] = acc_im[size_t(k)];
  }
  fftwf_execute(inv);
  // Overlap-save: the second half is the valid linear-convolution output.
  std::memcpy(out, ifft_out + block, sizeof(float) * size_t(block));
}

// ------------------------------------------------------------------ Convolver

Convolver::Convolver(int block, int max_taps)
    : B_(block),
      T_(kTailBlock),
      max_taps_(max_taps),
      head_taps_(std::min(max_taps, 2 * kTailBlock)),
      max_head_parts_((std::min(max_taps, 2 * kTailBlock) + block - 1) / block),
      max_tail_parts_(max_taps > 2 * kTailBlock ? (max_taps - 2 * kTailBlock + kTailBlock - 1) / kTailBlock : 0) {
  head_.init(B_, max_head_parts_, FFTW_MEASURE);
  head_tmp_.assign(size_t(B_), 0.f);
  for (auto& p : posted_) p.store(-1);
  if (max_tail_parts_ > 0) {
    // The tail isn't realtime-critical: a quick plan is plenty.
    tail_.init(T_, max_tail_parts_, FFTW_ESTIMATE);
    tail_tmp_.assign(size_t(T_), 0.f);
    for (int i = 0; i < 2; ++i) {
      tail_in_[i].assign(size_t(T_), 0.f);
      tail_out_[i].assign(size_t(T_), 0.f);
    }
    worker_ = std::thread([this] { worker_loop(); });
  }
  current_ = make_spectrum({}).release();
}

Convolver::~Convolver() {
  if (worker_.joinable()) {
    stop_.store(true);
    jobs_ready_.release();
    worker_.join();
  }
  delete current_;
  if (next_ != current_) delete next_;
  delete to_retire_;
  delete pending_.exchange(nullptr);
  delete retired_.exchange(nullptr);
  head_.destroy();
  tail_.destroy();
}

std::unique_ptr<Convolver::Spectrum> Convolver::make_spectrum(const std::vector<float>& ir_in) {
  std::vector<float> ir = ir_in;
  if (ir.empty()) ir = {1.f};
  if (int(ir.size()) > max_taps_) ir.resize(size_t(max_taps_));

  auto s = std::make_unique<Spectrum>();
  const int n_head = std::min(int(ir.size()), head_taps_);
  const int n_tail = int(ir.size()) - n_head;
  s->head_parts = (n_head + B_ - 1) / B_;
  s->tail_parts = n_tail > 0 ? (n_tail + T_ - 1) / T_ : 0;

  // Transforms each `blk`-sized partition of ir[from, from+count) into
  // (re, im); the 1/N inverse-FFT normalisation is folded in.
  auto transform = [&](int from, int count, int blk, int parts, std::vector<float>& re, std::vector<float>& im) {
    const int n = 2 * blk, bins = blk + 1;
    re.assign(size_t(parts) * bins, 0.f);
    im.assign(size_t(parts) * bins, 0.f);
    float* t = fftwf_alloc_real(size_t(n));
    fftwf_complex* f = fftwf_alloc_complex(size_t(bins));
    fftwf_plan plan;
    {
      std::lock_guard<std::mutex> lock(g_fftw_plan_mutex);
      plan = fftwf_plan_dft_r2c_1d(n, t, f, FFTW_ESTIMATE);
    }
    const float scale = 1.f / float(n);
    for (int p = 0; p < parts; ++p) {
      std::fill(t, t + n, 0.f);
      int off = p * blk, m = std::min(blk, count - off);
      for (int i = 0; i < m; ++i) t[i] = ir[size_t(from + off + i)] * scale;
      fftwf_execute(plan);
      for (int k = 0; k < bins; ++k) {
        re[size_t(p) * bins + k] = f[k][0];
        im[size_t(p) * bins + k] = f[k][1];
      }
    }
    {
      std::lock_guard<std::mutex> lock(g_fftw_plan_mutex);
      fftwf_destroy_plan(plan);
    }
    fftwf_free(t);
    fftwf_free(f);
  };
  transform(0, n_head, B_, s->head_parts, s->hre, s->him);
  if (s->tail_parts) transform(n_head, n_tail, T_, s->tail_parts, s->tre, s->tim);
  return s;
}

void Convolver::collect() { delete retired_.exchange(nullptr); }

void Convolver::set_filter(const std::vector<float>& ir) {
  collect();
  Spectrum* s = make_spectrum(ir).release();
  // If the previous filter was never picked up, the audio thread never saw
  // it, so it is ours to free.
  delete pending_.exchange(s);
}

// ------------------------------------------------------------------ worker

void Convolver::worker_loop() {
  while (true) {
    jobs_ready_.acquire();
    if (stop_.load()) return;
    const int64_t idx = queue_[q_read_ & 3];
    ++q_read_;
    const int slot = int(idx & 1);
    const Job job = jobs_[slot];
    if (job.reset) tail_.reset();
    tail_.push(tail_in_[slot].data());
    float* out = tail_out_[slot].data();
    if (job.spec->tail_parts > 0) {
      tail_.accumulate(job.spec->tre.data(), job.spec->tim.data(), job.spec->tail_parts);
      tail_.inverse(out);
    } else {
      std::fill(out, out + T_, 0.f);
    }
    if (job.prev) {
      // Filter switch: fade the tail from the old filter to the new one
      // across this block.
      if (job.prev->tail_parts > 0) {
        tail_.accumulate(job.prev->tre.data(), job.prev->tim.data(), job.prev->tail_parts);
        tail_.inverse(tail_tmp_.data());
      } else {
        std::fill(tail_tmp_.begin(), tail_tmp_.end(), 0.f);
      }
      for (int i = 0; i < T_; ++i) {
        float w = float(i + 1) / float(T_);
        out[i] = tail_tmp_[size_t(i)] * (1.f - w) + out[i] * w;
      }
    }
    done_.store(idx, std::memory_order_release);
  }
}

// ------------------------------------------------------------------ realtime

void Convolver::maybe_retire() {
  if (to_retire_ && done_.load(std::memory_order_acquire) >= retire_after_ && !retired_.load(std::memory_order_acquire)) {
    retired_.store(to_retire_, std::memory_order_release);
    to_retire_ = nullptr;
  }
}

void Convolver::enter_idle() {
  idle_ = true;
  head_.reset();
  if (max_tail_parts_ > 0) {
    for (auto& b : tail_in_) std::fill(b.begin(), b.end(), 0.f);
    tail_reset_pending_ = true;
  }
}

void Convolver::advance_timeline(const float* in) {
  if (max_tail_parts_ == 0) return;
  if (in) std::memcpy(&tail_in_[tblock_ & 1][size_t(tpos_)], in, sizeof(float) * size_t(B_));
  tpos_ += B_;
  if (tpos_ < T_) return;

  // Input T-block `tblock_` is complete: hand it to the worker (not while
  // idle: the input is silence and the tail state is being reset).
  // If the worker ever falls two jobs behind (a badly overloaded machine),
  // drop this job rather than queue more, and restart the tail cleanly.
  const bool overloaded = last_posted_ - done_.load(std::memory_order_acquire) >= 2;
  if (!idle_ && overloaded) {
    misses_.fetch_add(1, std::memory_order_relaxed);
    tail_reset_pending_ = true;
  }
  if (!idle_ && !overloaded) {
    const int slot = int(tblock_ & 1);
    Job& job = jobs_[slot];
    job.index = tblock_;
    job.reset = tail_reset_pending_;
    tail_reset_pending_ = false;
    job.prev = nullptr;
    if (!next_ && !to_retire_ && pending_.load(std::memory_order_acquire) &&
        !retired_.load(std::memory_order_acquire)) {
      // Start a filter switch: the tail switches with this job (whose
      // output plays from T-block tblock_+2), the head follows then.
      next_ = pending_.exchange(nullptr, std::memory_order_acq_rel);
      if (next_) {
        job.prev = current_;
        switch_block_ = tblock_ + 2;
      }
    }
    job.spec = next_ ? next_ : current_;
    posted_[slot].store(tblock_, std::memory_order_release);
    queue_[q_write_.load(std::memory_order_relaxed) & 3] = tblock_;
    q_write_.fetch_add(1, std::memory_order_release);
    last_posted_ = tblock_;
    jobs_ready_.release();
  }
  ++tblock_;
  tpos_ = 0;
}

void Convolver::process(const float* in, float* out) {
  maybe_retire();

  // ---- idle skip
  float peak = 0;
  for (int i = 0; i < B_; ++i) peak = std::max(peak, std::fabs(in[i]));
  if (peak > kSilence) {
    silent_ = 0;
    idle_ = false;
  } else if (!idle_) {
    silent_ += B_;
    if (silent_ > int64_t(max_taps_) + 3 * int64_t(T_) && !next_) enter_idle();
  }
  if (idle_) {
    // Nothing is sounding: apply a pending filter right away (no crossfade
    // needed) and output silence without computing anything.
    if (!next_ && !to_retire_ && pending_.load(std::memory_order_acquire) && !retired_.load(std::memory_order_acquire)) {
      Spectrum* s = pending_.exchange(nullptr, std::memory_order_acq_rel);
      if (s) {
        to_retire_ = current_;
        retire_after_ = last_posted_;
        current_ = s;
      }
    }
    std::memset(out, 0, sizeof(float) * size_t(B_));
    advance_timeline(nullptr);
    return;
  }

  // Keep the input: `in` and `out` may alias.
  float* head_out = out;
  head_.push(in);
  if (max_tail_parts_ > 0) std::memcpy(&tail_in_[tblock_ & 1][size_t(tpos_)], in, sizeof(float) * size_t(B_));

  // ---- head (first 2T taps)
  const bool switch_now = next_ && tblock_ == switch_block_ && tpos_ == 0;
  const bool head_only_switch = max_tail_parts_ == 0 && !next_ && pending_.load(std::memory_order_acquire) &&
                                !to_retire_ && !retired_.load(std::memory_order_acquire);
  if (switch_now || head_only_switch) {
    Spectrum* incoming = switch_now ? next_ : pending_.exchange(nullptr, std::memory_order_acq_rel);
    head_.accumulate(current_->hre.data(), current_->him.data(), current_->head_parts);
    head_.inverse(head_tmp_.data());
    head_.accumulate(incoming->hre.data(), incoming->him.data(), incoming->head_parts);
    head_.inverse(head_out);
    for (int i = 0; i < B_; ++i) {
      float w = float(i + 1) / float(B_);
      head_out[i] = head_tmp_[size_t(i)] * (1.f - w) + head_out[i] * w;
    }
    to_retire_ = current_;
    retire_after_ = switch_now ? switch_block_ - 2 : last_posted_;
    current_ = incoming;
    next_ = nullptr;
  } else {
    head_.accumulate(current_->hre.data(), current_->him.data(), current_->head_parts);
    head_.inverse(head_out);
  }

  // ---- tail: the worker's result for input T-block tblock_-2
  if (max_tail_parts_ > 0) {
    const int64_t want = tblock_ - 2;
    const int slot = int(want & 1);
    if (want >= 0 && posted_[slot].load(std::memory_order_acquire) == want) {
      if (synchronous_)
        while (done_.load(std::memory_order_acquire) < want) std::this_thread::yield();
      if (done_.load(std::memory_order_acquire) >= want) {
        const float* t = &tail_out_[slot][size_t(tpos_)];
        for (int i = 0; i < B_; ++i) head_out[i] += t[i];
      } else {
        misses_.fetch_add(1, std::memory_order_relaxed);
      }
    }
    // Input was already copied above; just move the timeline on.
    advance_timeline(nullptr);
  }
}

}  // namespace rc
