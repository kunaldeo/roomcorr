// Two-stage partitioned overlap-save FFT convolution.
//
// The first 2*T taps of the filter (T = kTailBlock) are convolved on the
// audio thread in `block`-sized partitions: latency is one block. The rest
// of the filter (the long, quiet tail of a 262k-tap room correction filter)
// is convolved by a worker thread in T-sized partitions. Because the tail
// starts 2*T samples into the filter, the worker's result for each T-block
// of input isn't needed until T samples after that block is complete, so
// it has a whole T-block (170 ms at 48 kHz) of slack. The output is the same
// as a single uniform convolution; the audio thread just does ~10x less work.
//
// Filters can be swapped from a non-realtime thread at any time. The switch
// happens at a T-block boundary: the worker crossfades the tail over that
// block while the audio thread crossfades the head, so it never clicks.
//
// When the input has been silent for longer than the filter's memory, the
// convolver clears its state once and skips all work until sound returns.
#pragma once

#include <atomic>
#include <memory>
#include <semaphore>
#include <thread>
#include <vector>

#include <fftw3.h>

namespace rc {

class Convolver {
public:
  static constexpr int kTailBlock = 8192;  // T

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
  bool idle() const { return idle_; }
  // Times the worker was late with a tail block (should stay 0).
  uint64_t tail_misses() const { return misses_.load(std::memory_order_relaxed); }
  // Tests only: make process() wait for the worker instead of skipping a
  // late tail block, so results are deterministic without realtime pacing.
  void set_synchronous_for_tests(bool on) { synchronous_ = on; }

private:
  // One uniformly partitioned overlap-save stage (FFT size 2*block).
  struct Stage {
    int block = 0, bins = 0, max_parts = 0;
    fftwf_plan fwd = nullptr, inv = nullptr;
    float* time = nullptr;          // 2*block: previous | current
    fftwf_complex* freq = nullptr;  // bins
    float* ifft_out = nullptr;      // 2*block
    std::vector<float> fdl_re, fdl_im;  // max_parts * bins
    int fdl_pos = 0;
    std::vector<float> acc_re, acc_im;

    void init(int block, int max_parts, unsigned plan_flags);
    void destroy();
    void push(const float* in);  // slide input window, transform into the delay line
    void accumulate(const float* hre, const float* him, int parts);
    void inverse(float* out);    // acc -> `block` output samples
    void reset();
  };

  struct Spectrum {
    int head_parts = 0, tail_parts = 0;
    std::vector<float> hre, him;  // head_parts * (B+1)
    std::vector<float> tre, tim;  // tail_parts * (T+1)
  };

  struct Job {
    int64_t index = -1;
    const Spectrum* spec = nullptr;
    const Spectrum* prev = nullptr;  // crossfade from this one (filter switch)
    bool reset = false;              // clear tail state first (after idle)
  };

  std::unique_ptr<Spectrum> make_spectrum(const std::vector<float>& ir);
  void worker_loop();
  void enter_idle();
  void advance_timeline(const float* in);  // tail input + job posting
  void maybe_retire();

  const int B_, T_, max_taps_, head_taps_;
  const int max_head_parts_, max_tail_parts_;
  Stage head_, tail_;  // tail_ belongs to the worker
  std::vector<float> head_tmp_, tail_tmp_;

  // Audio-thread state
  Spectrum* current_ = nullptr;     // filter the head is using
  Spectrum* next_ = nullptr;        // switch in progress: tail already uses it
  int64_t switch_block_ = -1;       // T-block at which the head follows
  Spectrum* to_retire_ = nullptr;   // old filter, freed once the worker is past it
  int64_t retire_after_ = -1;
  int64_t tblock_ = 0;              // current T-block on the output timeline
  int tpos_ = 0;                    // samples into it
  int64_t last_posted_ = -1;
  bool idle_ = false;
  int64_t silent_ = 0;
  bool tail_reset_pending_ = false;
  bool synchronous_ = false;

  // Shared with the worker. Job k reads tail_in_[k&1] and writes
  // tail_out_[k&1]; the audio thread plays job k during T-block k+2.
  std::vector<float> tail_in_[2], tail_out_[2];
  Job jobs_[2];
  std::atomic<int64_t> posted_[2];
  std::atomic<int64_t> done_{-1};
  int64_t queue_[4] = {};
  std::atomic<uint32_t> q_write_{0};
  uint32_t q_read_ = 0;
  std::counting_semaphore<64> jobs_ready_{0};
  std::atomic<bool> stop_{false};
  std::thread worker_;

  std::atomic<Spectrum*> pending_{nullptr};  // posted by set_filter
  std::atomic<Spectrum*> retired_{nullptr};  // handed back for freeing
  std::atomic<uint64_t> misses_{0};
};

}  // namespace rc
