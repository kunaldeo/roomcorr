// Offline checks for the DSP core. Run: build/roomcorr_tests
#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "analysis.hpp"
#include "design.hpp"
#include "dsp/biquad.hpp"
#include "dsp/convolver.hpp"
#include "engine.hpp"
#include "json.hpp"

using namespace rc;

static int failures = 0;

#define CHECK(cond, ...)                         \
  do {                                           \
    if (!(cond)) {                               \
      ++failures;                                \
      printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
      printf(__VA_ARGS__);                       \
      printf("\n");                              \
    }                                            \
  } while (0)

static double db(double x) { return 20 * std::log10(std::fabs(x) + 1e-30); }

static void test_json() {
  printf("json\n");
  Json j = Json::parse(R"({"a": [1, 2.5, -3e2], "b": {"c": "x\"yé"}, "d": true, "e": null})");
  CHECK(j.get("a").arr().size() == 3, "array size");
  CHECK(j.get("a").arr()[2].as_num() == -300, "number");
  CHECK(j.get("b").get("c").as_str() == "x\"y\xc3\xa9", "string escapes");
  Json k = Json::parse(j.dump(2));
  CHECK(k.dump() == j.dump(), "round trip");
  Config c;
  c.sub_gain_db = 3;
  CHECK(c.set("target.bass_boost_db", Json(6.0)) && c.target.bass_boost_db == 6, "nested set");
  CHECK(c.set("channels.sub.invert", Json(true)) && c.ch[kSub].invert, "channel set");
  CHECK(!c.set("nope", Json(1)), "unknown key");
  CHECK(c.set("crossover_hz", Json(5.0)) && c.crossover_hz == 30, "clamp");
}

static void test_convolver() {
  printf("convolver\n");
  std::mt19937 rng(1);
  std::normal_distribution<float> nd;
  const int B = 256;
  std::vector<float> h(3000), x(B * 20);
  for (auto& v : h) v = nd(rng) * 0.1f;
  for (auto& v : x) v = nd(rng);
  Convolver conv(B, 8192);
  conv.set_filter(h);
  std::vector<float> y(x.size());
  // The first block crossfades from unity to h; start comparing after it.
  for (size_t b = 0; b < x.size() / B; ++b) conv.process(&x[b * B], &y[b * B]);
  double err = 0;
  for (size_t n = B; n < x.size(); ++n) {
    double ref = 0;
    for (size_t k = 0; k < h.size() && k <= n; ++k) ref += double(h[k]) * x[n - k];
    err = std::max(err, std::fabs(ref - y[n]));
  }
  CHECK(err < 1e-4, "max error %g", err);

  // Unity filter passes through with zero added latency.
  Convolver id(B, 1024);
  std::vector<float> z(B * 2);
  id.process(&x[0], &z[0]);
  id.process(&x[B], &z[B]);
  double e2 = 0;
  for (int n = 0; n < 2 * B; ++n) e2 = std::max(e2, double(std::fabs(z[n] - x[n])));
  CHECK(e2 < 1e-5, "identity error %g", e2);
}

// Pushes an impulse into the left input and returns the six outputs.
static std::vector<std::vector<float>> engine_impulse(Engine& e, int len, int in_chan = 0) {
  std::vector<float> in[kNumOut];
  for (auto& v : in) v.assign(size_t(len), 0.f);
  in[in_chan][0] = 1.f;
  std::vector<std::vector<float>> out(kNumOut, std::vector<float>(len));
  const float* ip[kNumOut];
  for (int c = 0; c < kNumOut; ++c) ip[c] = in[c].data();
  float* op[kNumOut];
  for (int o = 0; o < kNumOut; ++o) op[o] = out[o].data();
  // Odd chunk sizes exercise the block FIFO.
  int pos = 0, chunk = 113;
  while (pos < len) {
    int n = std::min(chunk, len - pos);
    const float* ic[kNumOut];
    for (int c = 0; c < kNumOut; ++c) ic[c] = ip[c] + pos;
    float* oc[kNumOut];
    for (int o = 0; o < kNumOut; ++o) oc[o] = op[o] + pos;
    e.process(ic, oc, n);
    pos += n;
    chunk = chunk == 113 ? 1024 : 113;
  }
  return out;
}

static void test_engine_crossover() {
  printf("engine bass management\n");
  Engine e;
  Config c;
  c.sub_outputs = {"FC"};
  c.limiter = false;
  EngineParams p = EngineParams::from_config(c);
  p.preamp_db = 0;
  e.set_params(p);
  const int len = 1 << 16;
  auto out = engine_impulse(e, len);

  // Latency is exactly one block.
  int first = 0;
  while (first < len && out[kOutFL][size_t(first)] == 0.f && out[kOutFC][size_t(first)] == 0.f) ++first;
  CHECK(first == Engine::kBlock, "latency %d", first);

  // Main + sub of an LR4 crossover sums to an all-pass: flat magnitude.
  std::vector<double> sum(len), sub(len), main(len);
  for (int n = 0; n < len; ++n) {
    sum[size_t(n)] = double(out[kOutFL][size_t(n)]) + out[kOutFC][size_t(n)];
    sub[size_t(n)] = out[kOutFC][size_t(n)];
    main[size_t(n)] = out[kOutFL][size_t(n)];
  }
  auto S = rfft(sum, len), SB = rfft(sub, len), MN = rfft(main, len);
  double worst = 0;
  for (double f : {20.0, 50.0, 80.0, 120.0, 1000.0, 10000.0}) {
    size_t k = size_t(f * len / kSampleRate);
    worst = std::max(worst, std::fabs(db(std::abs(S[k]))));
  }
  CHECK(worst < 0.05, "LR4 sum deviates %.3f dB", worst);
  size_t k80 = size_t(80.0 * len / kSampleRate);
  CHECK(std::fabs(db(std::abs(SB[k80])) + 6.02) < 0.3, "sub at crossover %.2f dB", db(std::abs(SB[k80])));
  CHECK(std::fabs(db(std::abs(MN[k80])) + 6.02) < 0.3, "main at crossover %.2f dB", db(std::abs(MN[k80])));
  size_t k20 = size_t(20.0 * len / kSampleRate), k1k = size_t(1000.0 * len / kSampleRate);
  CHECK(db(std::abs(SB[k1k])) < -60, "sub leaks at 1 kHz: %.1f dB", db(std::abs(SB[k1k])));
  CHECK(std::fabs(db(std::abs(SB[k20]))) < 0.2, "sub at 20 Hz %.2f dB", db(std::abs(SB[k20])));

  // Delay + trim + polarity on the sub.
  c.ch[kSub].delay_ms = 2.0;
  c.ch[kSub].invert = true;
  c.ch[kSub].trim_db = -6;
  p = EngineParams::from_config(c);
  p.preamp_db = 0;
  Engine e2;
  e2.set_params(p);
  auto out2 = engine_impulse(e2, len);
  double pk = 0;
  int at = 0;
  for (int n = 0; n < len; ++n)
    if (std::fabs(out2[kOutFC][size_t(n)]) > pk) {
      pk = std::fabs(out2[kOutFC][size_t(n)]);
      at = n;
    }
  double pk1 = 0;
  int at1 = 0;
  for (int n = 0; n < len; ++n)
    if (std::fabs(out[kOutFC][size_t(n)]) > pk1) {
      pk1 = std::fabs(out[kOutFC][size_t(n)]);
      at1 = n;
    }
  CHECK(at - at1 == 96, "sub delay %d samples, want 96", at - at1);
  CHECK(std::fabs(db(pk / pk1) + 6) < 0.1, "sub trim %.2f dB", db(pk / pk1));
  CHECK(out2[kOutFC][size_t(at)] * out[kOutFC][size_t(at1)] < 0, "polarity not inverted");
  CHECK(out2[kOutLFE][size_t(at)] == 0.f, "LFE should be silent when not routed");

  // Bypass: plain stereo, sub silent.
  c = Config{};
  c.enabled = false;
  c.limiter = false;
  p = EngineParams::from_config(c);
  p.preamp_db = 0;
  Engine e3;
  e3.set_params(p);
  // Let the bypass crossfade settle before the impulse.
  std::vector<float> zeros(48000, 0.f);
  std::vector<std::vector<float>> sink(kNumOut, std::vector<float>(48000));
  const float* zi[kNumOut] = {zeros.data(), zeros.data(), nullptr, nullptr, nullptr, nullptr};
  float* so[kNumOut];
  for (int o = 0; o < kNumOut; ++o) so[o] = sink[o].data();
  e3.process(zi, so, 48000);
  auto out3 = engine_impulse(e3, 4096);
  CHECK(std::fabs(out3[kOutFL][Engine::kBlock] - 1.f) < 1e-3, "bypass passes impulse: %f", out3[kOutFL][Engine::kBlock]);
  double subsum = 0;
  for (float v : out3[kOutFC]) subsum += std::fabs(v);
  CHECK(subsum < 1e-3, "bypass sub not silent: %g", subsum);
}

static void test_lfe() {
  printf("5.1 input: LFE and centre\n");
  Config c;
  c.sub_outputs = {"LFE"};
  c.limiter = false;
  EngineParams p = EngineParams::from_config(c);
  p.preamp_db = 0;
  Engine e;
  e.set_params(p);
  const int len = 1 << 16;
  auto out = engine_impulse(e, len, kOutLFE);
  std::vector<double> sub(out[kOutLFE].begin(), out[kOutLFE].end()), fl(out[kOutFL].begin(), out[kOutFL].end());
  auto S = rfft(sub, len);
  size_t k40 = size_t(40.0 * len / kSampleRate);
  // LFE plays +10 dB in band; LR4 at 120 Hz is -0.03 dB and the 80 Hz
  // crossover LPF doesn't touch it.
  CHECK(std::fabs(db(std::abs(S[k40])) - 10.0) < 0.2, "LFE gain at 40 Hz %.2f dB", db(std::abs(S[k40])));
  double leak = 0;
  for (float v : fl) leak += std::fabs(v);
  CHECK(leak < 1e-6, "LFE leaked into the mains");

  Engine e2;
  e2.set_params(p);
  auto oc = engine_impulse(e2, len, kOutFC);
  std::vector<double> l(oc[kOutFL].begin(), oc[kOutFL].end()), r(oc[kOutFR].begin(), oc[kOutFR].end());
  auto L = rfft(l, len), R = rfft(r, len);
  size_t k1k = size_t(1000.0 * len / kSampleRate);
  CHECK(std::fabs(db(std::abs(L[k1k])) + 3.01) < 0.1 && std::fabs(db(std::abs(R[k1k])) + 3.01) < 0.1,
        "centre fold-down L %.2f R %.2f dB", db(std::abs(L[k1k])), db(std::abs(R[k1k])));
}

static void test_minphase() {
  printf("minimum-phase FIR\n");
  auto grid = log_grid(10, 24000, 48);
  std::vector<double> want(grid.size());
  for (size_t i = 0; i < grid.size(); ++i) {
    double f = grid[i];
    want[i] = 4 * std::exp(-std::pow(std::log2(f / 50), 2) * 20) - 8 * std::exp(-std::pow(std::log2(f / 110), 2) * 30) +
              (f > 2000 ? -2 : 0) * band_taper(f, 2000, 30000);
  }
  auto h = minphase_fir(grid, want, kFilterTaps, kSampleRate);
  std::vector<double> check;
  for (double f = 20; f < 20000; f *= 1.1) check.push_back(f);
  auto H = fir_response(h, check, kSampleRate);
  double worst = 0;
  for (size_t i = 0; i < check.size(); ++i) worst = std::max(worst, std::fabs(db(std::abs(H[i])) - interp_log(grid, want, check[i])));
  CHECK(worst < 0.3, "min-phase magnitude error %.2f dB", worst);
  // Minimum phase: energy is front-loaded.
  double e_first = 0, e_all = 0;
  for (size_t i = 0; i < h.size(); ++i) {
    e_all += double(h[i]) * h[i];
    if (i < 4800) e_first += double(h[i]) * h[i];
  }
  CHECK(e_first / e_all > 0.99, "energy in first 100 ms: %.3f", e_first / e_all);
}

// Simple synthetic "speaker in a room": a delayed impulse through a filter
// chain, plus an optional reflection.
static std::vector<double> synth_ir(int delay, const std::vector<BiquadCoeffs>& chain, int len, double refl_gain = 0,
                                    int refl_delay = 0) {
  std::vector<double> x(size_t(len), 0.0);
  x[size_t(delay)] = 1;
  if (refl_gain != 0) x[size_t(delay + refl_delay)] += refl_gain;
  for (const auto& k : chain) {
    Biquad b{k};
    for (auto& v : x) v = b.tick(v);
  }
  return x;
}

static std::vector<double> convolve(const std::vector<double>& a, const std::vector<double>& b) {
  size_t n = next_pow2(a.size() + b.size());
  auto A = rfft(a, n), B = rfft(b, n);
  for (size_t k = 0; k < A.size(); ++k) A[k] *= B[k];
  auto y = irfft(A, n);
  y.resize(a.size() + b.size() - 1);
  return y;
}

static void test_deconvolution() {
  printf("sweep deconvolution\n");
  const int fs = kSampleRate;
  Sweep s = make_sweep(15, 22000, 3.0, fs);
  auto sys = synth_ir(1234, {BiquadCoeffs::highpass(60, 0.7, fs), BiquadCoeffs::lowshelf(200, 4, fs)}, 20000);
  auto rec = convolve(s.signal, sys);
  std::mt19937 rng(2);
  std::normal_distribution<double> nd(0, 1e-4);
  for (auto& v : rec) v += nd(rng);
  auto ir = deconvolve(rec, s, kIrLength);
  double t = arrival_time(ir, 200, 5000, fs);
  CHECK(std::fabs(t - 1234) < 1.0, "arrival %.2f, want 1234", t);
  auto H = rfft(ir, kIrLength), Href = rfft(sys, kIrLength);
  double worst = 0;
  for (double f : {30.0, 60.0, 200.0, 1000.0, 10000.0}) {
    size_t k = size_t(f * kIrLength / fs);
    worst = std::max(worst, std::fabs(db(std::abs(H[k])) - db(std::abs(Href[k]))));
  }
  CHECK(worst < 0.3, "deconvolved magnitude error %.2f dB", worst);
}

static void test_design() {
  printf("synthetic room calibration\n");
  const int fs = kSampleRate;
  const int lat = 2000;  // common system latency
  MeasurementSet m;
  m.dir = "/tmp/roomcorr-test";
  std::mt19937 rng(3);
  std::uniform_int_distribution<int> jitter(-20, 20);
  for (int p = 0; p < 5; ++p) {
    // Right speaker 30 cm further (42 samples); sub 1.4 m further plus 5 ms
    // of internal DSP latency; a 45 Hz room mode on the sub, a 150 Hz dip
    // and a bass bump on the mains.
    int j = jitter(rng);
    std::vector<std::vector<double>> chans;
    auto mains = [&](int d, double refl) {
      return synth_ir(lat + d + j,
                      {BiquadCoeffs::highpass(55, 0.9, fs), BiquadCoeffs::highpass(55, 0.9, fs),
                       BiquadCoeffs{} /*placeholder*/, BiquadCoeffs::lowshelf(250, 3, fs)},
                      kIrLength, refl, 300 + 7 * p);
    };
    chans.push_back(mains(0, 0.3));
    chans.push_back(mains(42, 0.3));
    // Room mode: resonant peak at 45 Hz, +9 dB.
    double w = 2 * M_PI * 45 / fs, A = std::pow(10, 9 / 40.0), al = std::sin(w) / (2 * 6);
    double a0 = 1 + al / A;
    BiquadCoeffs mode{(1 + al * A) / a0, -2 * std::cos(w) / a0, (1 - al * A) / a0, -2 * std::cos(w) / a0, (1 - al / A) / a0};
    chans.push_back(synth_ir(lat + 196 + 240 + j,
                             {BiquadCoeffs::lowpass(150, 0.7, fs), BiquadCoeffs::lowpass(150, 0.7, fs),
                              BiquadCoeffs::highpass(25, 0.7, fs), mode},
                             kIrLength, 0.2, 500));
    // Sub 6 dB louder than it should be.
    for (auto& v : chans[2]) v *= 2;
    m.irs.push_back(std::move(chans));
  }
  MicCal cal;  // flat mic
  Config c;
  auto r = design_filters(c, m, cal);
  for (auto& n : r.notes) printf("    %s\n", n.c_str());
  for (auto& w2 : r.warnings) printf("    warning: %s\n", w2.c_str());
  CHECK(std::fabs(r.cfg.ch[kRight].delay_ms) < 0.05, "right delay %.3f", r.cfg.ch[kRight].delay_ms);
  CHECK(std::fabs(r.cfg.ch[kLeft].delay_ms - 42.0 / 48) < 0.05, "left delay %.3f want 0.875", r.cfg.ch[kLeft].delay_ms);
  // The sub is 6 dB hot and the Harman target asks for ~6 dB of bass lift
  // in its band, so almost no trim is needed.
  CHECK(r.cfg.ch[kSub].trim_db < 2 && r.cfg.ch[kSub].trim_db > -4, "sub trim %.2f", r.cfg.ch[kSub].trim_db);
  CHECK(r.report.get("summary").get("crossover_sum_after").as_num() > 0.9, "crossover summation %.2f",
        r.report.get("summary").get("crossover_sum_after").as_num());

  // Corrected system response in the bass should follow the target far
  // better than the raw one.
  const Json& sys = r.report.get("system");
  auto f = sys.get("freqs").num_vector(), b = sys.get("before").num_vector(), a = sys.get("after").num_vector();
  auto tf = r.report.get("freqs").num_vector(), tv = r.report.get("target").num_vector();
  double dev_b = 0, dev_a = 0;
  int n = 0;
  for (size_t i = 0; i < f.size(); ++i) {
    if (f[i] < 30 || f[i] > 300) continue;
    double t = interp_log(tf, tv, f[i]);
    dev_b += std::pow(b[i] - t, 2);
    dev_a += std::pow(a[i] - t, 2);
    ++n;
  }
  dev_b = std::sqrt(dev_b / n);
  dev_a = std::sqrt(dev_a / n);
  printf("    bass RMS deviation from target: before %.1f dB, after %.1f dB\n", dev_b, dev_a);
  CHECK(dev_a < 2.0 && dev_a < dev_b / 2, "bass deviation after %.2f dB (before %.2f)", dev_a, dev_b);
}

int main() {
  test_json();
  test_convolver();
  test_engine_crossover();
  test_lfe();
  test_minphase();
  test_deconvolution();
  test_design();
  printf(failures ? "%d FAILURES\n" : "all tests passed\n", failures);
  return failures ? 1 : 0;
}
