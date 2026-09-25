// Real-time-paced CPU benchmark of the engine: three 262,144-tap filters,
// 1024-frame quanta at 48 kHz, sleeping between quanta like PipeWire does.
// Run: build/roomcorr_bench
#include "engine.hpp"
#include <chrono>
#include <cstdio>
#include <random>
#include <thread>
#include <ctime>
using namespace rc;
static double cpu(clockid_t c) { timespec t; clock_gettime(c, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
int main() {
  std::mt19937 rng(1); std::normal_distribution<float> nd;
  Engine e; Config cfg; e.set_params(EngineParams::from_config(cfg));
  std::vector<float> h(262144); for (size_t i = 0; i < h.size(); ++i) h[i] = nd(rng) * 1e-3f * std::exp(-float(i) / 40000.f);
  for (int c = 0; c < 3; ++c) e.set_filter(c, h);
  const int Q = 1024; const double period = double(Q) / 48000;
  std::vector<float> in(Q), silent(Q, 0.f), out[6]; for (auto& o : out) o.resize(Q);
  float* op[6]; for (int i = 0; i < 6; ++i) op[i] = out[i].data();
  auto run = [&](const char* label, bool sound, int quanta) {
    double rt0 = cpu(CLOCK_THREAD_CPUTIME_ID), p0 = cpu(CLOCK_PROCESS_CPUTIME_ID), worst = 0;
    auto next = std::chrono::steady_clock::now();
    for (int q = 0; q < quanta; ++q) {
      for (auto& v : in) v = sound ? nd(rng) * 0.1f : 0.f;
      const float* ip[6] = {in.data(), in.data(), nullptr, nullptr, nullptr, nullptr};
      double a = cpu(CLOCK_THREAD_CPUTIME_ID);
      e.process(ip, op, Q);
      worst = std::max(worst, cpu(CLOCK_THREAD_CPUTIME_ID) - a);
      next += std::chrono::microseconds(int(period * 1e6));
      std::this_thread::sleep_until(next);
    }
    double secs = quanta * period;
    double rt = cpu(CLOCK_THREAD_CPUTIME_ID) - rt0, all = cpu(CLOCK_PROCESS_CPUTIME_ID) - p0;
    printf("%-22s realtime thread %5.2f%% (worst block %4.2f%%) | worker threads %5.2f%% | total %5.2f%% of one core\n",
           label, 100 * rt / secs, 100 * worst / period, 100 * (all - rt) / secs, 100 * all / secs);
  };
  run("warm-up", true, 60);
  run("playing", true, 280);
  run("silence (first 7 s)", false, 330);
  run("silence (idle)", false, 140);
  run("playing again", true, 140);
}
