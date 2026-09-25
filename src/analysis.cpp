#include "analysis.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <random>
#include <stdexcept>

#include <fftw3.h>

#include "dsp/biquad.hpp"

namespace rc {

static std::mutex g_plan_mutex;

size_t next_pow2(size_t n) {
  size_t p = 1;
  while (p < n) p <<= 1;
  return p;
}

std::vector<cplx> rfft(const std::vector<double>& x, size_t n) {
  double* in = fftw_alloc_real(n);
  fftw_complex* out = fftw_alloc_complex(n / 2 + 1);
  fftw_plan plan;
  {
    std::lock_guard<std::mutex> lock(g_plan_mutex);
    plan = fftw_plan_dft_r2c_1d(int(n), in, out, FFTW_ESTIMATE);
  }
  size_t m = std::min(n, x.size());
  std::copy(x.begin(), x.begin() + long(m), in);
  std::fill(in + m, in + n, 0.0);
  fftw_execute(plan);
  std::vector<cplx> X(n / 2 + 1);
  for (size_t k = 0; k < X.size(); ++k) X[k] = {out[k][0], out[k][1]};
  {
    std::lock_guard<std::mutex> lock(g_plan_mutex);
    fftw_destroy_plan(plan);
  }
  fftw_free(in);
  fftw_free(out);
  return X;
}

std::vector<double> irfft(const std::vector<cplx>& X, size_t n) {
  fftw_complex* in = fftw_alloc_complex(n / 2 + 1);
  double* out = fftw_alloc_real(n);
  fftw_plan plan;
  {
    std::lock_guard<std::mutex> lock(g_plan_mutex);
    plan = fftw_plan_dft_c2r_1d(int(n), in, out, FFTW_ESTIMATE);
  }
  for (size_t k = 0; k < n / 2 + 1; ++k) {
    cplx v = k < X.size() ? X[k] : cplx(0);
    in[k][0] = v.real();
    in[k][1] = v.imag();
  }
  fftw_execute(plan);
  std::vector<double> x(out, out + n);
  for (double& v : x) v /= double(n);
  {
    std::lock_guard<std::mutex> lock(g_plan_mutex);
    fftw_destroy_plan(plan);
  }
  fftw_free(in);
  fftw_free(out);
  return x;
}

std::vector<cplx> cfft(const std::vector<cplx>& x, bool inverse) {
  size_t n = x.size();
  fftw_complex* buf = fftw_alloc_complex(n);
  fftw_plan plan;
  {
    std::lock_guard<std::mutex> lock(g_plan_mutex);
    plan = fftw_plan_dft_1d(int(n), buf, buf, inverse ? FFTW_BACKWARD : FFTW_FORWARD, FFTW_ESTIMATE);
  }
  for (size_t i = 0; i < n; ++i) {
    buf[i][0] = x[i].real();
    buf[i][1] = x[i].imag();
  }
  fftw_execute(plan);
  std::vector<cplx> y(n);
  for (size_t i = 0; i < n; ++i) y[i] = {buf[i][0], buf[i][1]};
  {
    std::lock_guard<std::mutex> lock(g_plan_mutex);
    fftw_destroy_plan(plan);
  }
  fftw_free(buf);
  return y;
}

// ------------------------------------------------------------- signals

Sweep make_sweep(double f1, double f2, double seconds, int fs) {
  Sweep s;
  s.f1 = f1;
  s.f2 = f2;
  s.fs = fs;
  size_t n = size_t(seconds * fs);
  s.signal.resize(n);
  const double L = std::log(f2 / f1);
  const double T = seconds;
  for (size_t i = 0; i < n; ++i) {
    double t = double(i) / fs;
    s.signal[i] = std::sin(2 * M_PI * f1 * T / L * (std::exp(t * L / T) - 1));
  }
  // Fades keep the start/stop transients out of the measurement band.
  size_t fin = std::min(n / 4, size_t(0.05 * fs)), fout = std::min(n / 4, size_t(0.005 * fs));
  for (size_t i = 0; i < fin; ++i) s.signal[i] *= 0.5 - 0.5 * std::cos(M_PI * double(i) / double(fin));
  for (size_t i = 0; i < fout; ++i) s.signal[n - 1 - i] *= 0.5 - 0.5 * std::cos(M_PI * double(i) / double(fout));
  return s;
}

std::vector<double> deconvolve(const std::vector<double>& recorded, const Sweep& sweep, size_t ir_len) {
  size_t N = next_pow2(recorded.size() + sweep.signal.size());
  auto X = rfft(sweep.signal, N);
  auto Y = rfft(recorded, N);
  double maxX2 = 0;
  for (const auto& v : X) maxX2 = std::max(maxX2, std::norm(v));
  std::vector<cplx> H(X.size());
  for (size_t k = 0; k < X.size(); ++k) {
    double f = double(k) * sweep.fs / double(N);
    // -60 dB regularisation in band, rising to 0 dB half an octave outside
    // it, so out-of-band noise is not amplified.
    double outside = 0;
    if (f < sweep.f1) outside = f > 0 ? std::log2(sweep.f1 / f) : 10;
    if (f > sweep.f2) outside = std::log2(f / sweep.f2);
    double r = std::min(1.0, outside / 0.5);
    double eps = maxX2 * std::pow(10.0, -6.0 * (1.0 - r));
    H[k] = Y[k] * std::conj(X[k]) / (std::norm(X[k]) + eps);
  }
  auto ir = irfft(H, N);
  ir.resize(std::min(ir_len, N));
  return ir;
}

std::vector<double> pink_noise(size_t n, double f_lo, double f_hi, int fs, unsigned seed) {
  std::mt19937 rng(seed);
  std::uniform_real_distribution<double> phase(0, 2 * M_PI);
  std::vector<cplx> X(n / 2 + 1);
  for (size_t k = 1; k < X.size(); ++k) {
    double f = double(k) * fs / double(n);
    if (f < f_lo || f > f_hi) continue;
    X[k] = std::polar(1.0 / std::sqrt(f), phase(rng));
  }
  auto x = irfft(X, n);
  double sq = 0;
  for (double v : x) sq += v * v;
  double g = 1.0 / std::sqrt(sq / double(n) + 1e-300);
  for (double& v : x) v *= g;
  return x;
}

double band_level_dbfs(const std::vector<double>& x, double f_lo, double f_hi, int fs) {
  if (x.empty()) return -200;
  size_t N = next_pow2(x.size());
  auto X = rfft(x, N);
  double p = 0;
  for (size_t k = 0; k < X.size(); ++k) {
    double f = double(k) * fs / double(N);
    if (f < f_lo || f > f_hi) continue;
    double w = (k == 0 || k == N / 2) ? 1.0 : 2.0;
    p += w * std::norm(X[k]);
  }
  double ms = p / double(N) / double(x.size());
  return 10 * std::log10(ms + 1e-30) + 3.0103;
}

double peak_dbfs(const std::vector<double>& x) {
  double pk = 0;
  for (double v : x) pk = std::max(pk, std::fabs(v));
  return 20 * std::log10(pk + 1e-30);
}

// ------------------------------------------------------------ microphone

double MicCal::at(double freq) const {
  if (f.empty()) return 0;
  return interp_log(f, db, freq);
}

MicCal load_mic_cal(const std::string& path) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("cannot open mic calibration " + path);
  MicCal cal;
  cal.path = path;
  std::string line;
  while (std::getline(in, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    if (line.empty()) continue;
    if (line[0] == '"' || line[0] == '*' || std::isalpha(static_cast<unsigned char>(line[0]))) {
      auto s = line.find("Sens Factor");
      if (s != std::string::npos) {
        auto eq = line.find('=', s);
        if (eq != std::string::npos) cal.sens_db = std::strtod(line.c_str() + eq + 1, nullptr);
      }
      auto sn = line.find("SERNO:");
      if (sn != std::string::npos) {
        std::string rest = line.substr(sn + 6);
        rest.erase(std::remove_if(rest.begin(), rest.end(), [](char c) { return c == '"' || c == ' '; }), rest.end());
        cal.serial = rest;
      }
      continue;
    }
    char* end = nullptr;
    double f = std::strtod(line.c_str(), &end);
    if (end == line.c_str()) continue;
    char* end2 = nullptr;
    double d = std::strtod(end, &end2);
    if (end2 == end || f <= 0) continue;
    cal.f.push_back(f);
    cal.db.push_back(d);
  }
  if (cal.f.size() < 10) throw std::runtime_error("mic calibration " + path + " has no data");
  return cal;
}

// ------------------------------------------------------------ frequency domain

std::vector<double> log_grid(double f_lo, double f_hi, int per_octave) {
  std::vector<double> g;
  int n = int(std::floor(std::log2(f_hi / f_lo) * per_octave + 1e-9));
  for (int i = 0; i <= n; ++i) g.push_back(f_lo * std::pow(2.0, double(i) / per_octave));
  return g;
}

double interp_log(const std::vector<double>& grid, const std::vector<double>& y, double f) {
  if (f <= grid.front()) return y.front();
  if (f >= grid.back()) return y.back();
  auto it = std::upper_bound(grid.begin(), grid.end(), f);
  size_t i = size_t(it - grid.begin());
  double a = std::log(f / grid[i - 1]) / std::log(grid[i] / grid[i - 1]);
  return y[i - 1] * (1 - a) + y[i] * a;
}

double band_taper(double f, double f_lo, double f_hi) {
  // 1 in band, half-octave raised-cosine skirts.
  auto skirt = [](double oct) { return oct >= 0.5 ? 0.0 : 0.5 + 0.5 * std::cos(M_PI * oct / 0.5); };
  if (f <= 0) return 0;
  if (f < f_lo) return skirt(std::log2(f_lo / f));
  if (f > f_hi) return skirt(std::log2(f / f_hi));
  return 1;
}

double arrival_time(const std::vector<double>& ir, double f_lo, double f_hi, int fs) {
  size_t N = next_pow2(ir.size());
  auto X = rfft(ir, N);
  std::vector<cplx> Z(N, 0.0);
  for (size_t k = 1; k < N / 2; ++k) Z[k] = 2.0 * X[k] * band_taper(double(k) * fs / double(N), f_lo, f_hi);
  auto z = cfft(Z, true);
  size_t best = 0;
  double bv = -1;
  for (size_t i = 0; i < N; ++i) {
    double v = std::abs(z[i]);
    if (v > bv) {
      bv = v;
      best = i;
    }
  }
  // Parabolic refinement for sub-sample precision.
  if (best > 0 && best + 1 < N) {
    double a = std::abs(z[best - 1]), b = std::abs(z[best]), c = std::abs(z[best + 1]);
    double den = a - 2 * b + c;
    if (std::fabs(den) > 1e-30) return double(best) + 0.5 * (a - c) / den;
  }
  return double(best);
}

std::vector<double> fdw_power(const std::vector<double>& ir, double peak, int fs, size_t nfft, double pre_ms,
                              double cycles, double min_ms, double max_ms) {
  std::vector<double> windows;
  for (double t = max_ms; t > min_ms * 1.01; t /= 2) windows.push_back(t);
  windows.push_back(min_ms);

  const long pk = long(std::lround(peak));
  const long pre = std::max(1L, long(pre_ms * fs / 1000));
  std::vector<std::vector<double>> P;
  for (double T : windows) {
    long len = long(T * fs / 1000);
    std::vector<double> seg(size_t(pre + len), 0.0);
    for (long i = 0; i < pre + len; ++i) {
      long src = pk - pre + i;
      if (src < 0 || src >= long(ir.size())) continue;
      double w;
      if (i < pre)
        w = 0.5 - 0.5 * std::cos(M_PI * double(i) / double(pre));
      else if (i - pre < len / 2)
        w = 1;
      else
        w = 0.5 + 0.5 * std::cos(M_PI * double(i - pre - len / 2) / double(len - len / 2));
      seg[size_t(i)] = ir[size_t(src)] * w;
    }
    auto X = rfft(seg, nfft);
    std::vector<double> p(X.size());
    for (size_t k = 0; k < X.size(); ++k) p[k] = std::norm(X[k]) + 1e-30;
    P.push_back(std::move(p));
  }

  std::vector<double> out(nfft / 2 + 1);
  for (size_t k = 0; k < out.size(); ++k) {
    double f = double(k) * fs / double(nfft);
    double want = f > 0 ? std::clamp(cycles / f * 1000, min_ms, max_ms) : max_ms;
    size_t i = 0;
    while (i + 1 < windows.size() && windows[i + 1] >= want) ++i;
    if (i + 1 >= windows.size()) {
      out[k] = P[i][k];
      continue;
    }
    double a = std::log(windows[i] / want) / std::log(windows[i] / windows[i + 1]);
    out[k] = std::exp((1 - a) * std::log(P[i][k]) + a * std::log(P[i + 1][k]));
  }
  return out;
}

static double smoothing_octaves(double f) {
  static const double kf[] = {10, 250, 1000, 3000, 8000};
  static const double ko[] = {1.0 / 24, 1.0 / 24, 1.0 / 12, 1.0 / 6, 1.0 / 3};
  const int n = 5;
  if (f <= kf[0]) return ko[0];
  if (f >= kf[n - 1]) return ko[n - 1];
  int i = 1;
  while (kf[i] < f) ++i;
  double a = std::log(f / kf[i - 1]) / std::log(kf[i] / kf[i - 1]);
  return ko[i - 1] * (1 - a) + ko[i] * a;
}

std::vector<double> smooth_to_grid(const std::vector<double>& power, double bin_hz, const std::vector<double>& grid) {
  std::vector<double> prefix(power.size() + 1, 0.0);
  for (size_t k = 0; k < power.size(); ++k) prefix[k + 1] = prefix[k] + power[k];
  std::vector<double> out;
  out.reserve(grid.size());
  for (double f : grid) {
    double b = smoothing_octaves(f);
    double klo = f * std::pow(2.0, -b / 2) / bin_hz, khi = f * std::pow(2.0, b / 2) / bin_hz;
    double p;
    if (khi - klo < 2) {
      double kf = f / bin_hz;
      size_t k0 = std::min(size_t(kf), power.size() - 2);
      double a = kf - double(k0);
      p = power[k0] * (1 - a) + power[k0 + 1] * a;
    } else {
      size_t a = size_t(std::ceil(klo)), b2 = std::min(size_t(std::floor(khi)), power.size() - 1);
      p = (prefix[b2 + 1] - prefix[a]) / double(b2 + 1 - a);
    }
    out.push_back(10 * std::log10(p + 1e-30));
  }
  return out;
}

std::vector<double> smooth_grid(const std::vector<double>& grid, const std::vector<double>& db, double octaves) {
  if (grid.size() < 2) return db;
  double step = std::log2(grid[1] / grid[0]);
  long h = std::max(0L, long(std::lround(octaves / 2 / step)));
  std::vector<double> out(db.size());
  for (long i = 0; i < long(db.size()); ++i) {
    double p = 0;
    long n = 0;
    for (long j = std::max(0L, i - h); j <= std::min(long(db.size()) - 1, i + h); ++j, ++n) p += std::pow(10, db[size_t(j)] / 10);
    out[size_t(i)] = 10 * std::log10(p / double(n));
  }
  return out;
}

std::vector<cplx> windowed_response(const std::vector<double>& ir, long start, size_t len, const std::vector<double>& freqs,
                                    int fs) {
  std::vector<double> seg(len, 0.0);
  size_t rise = std::min(len / 10, size_t(0.010 * fs));
  size_t fall = len * 3 / 10;
  for (size_t i = 0; i < len; ++i) {
    long src = start + long(i);
    if (src < 0 || src >= long(ir.size())) continue;
    double w = 1;
    if (i < rise) w = 0.5 - 0.5 * std::cos(M_PI * double(i) / double(rise));
    if (i >= len - fall) w = 0.5 + 0.5 * std::cos(M_PI * double(i - (len - fall)) / double(fall));
    seg[i] = ir[size_t(src)] * w;
  }
  std::vector<cplx> out;
  out.reserve(freqs.size());
  for (double f : freqs) {
    // Direct DTFT with a rotating phasor; only a few hundred bass
    // frequencies are needed, so this beats an FFT plus interpolation.
    cplx rot = std::polar(1.0, -2 * M_PI * f / fs), ph = 1, acc = 0;
    for (size_t i = 0; i < len; ++i) {
      acc += seg[i] * ph;
      ph *= rot;
      if ((i & 1023) == 1023) ph /= std::abs(ph);
    }
    out.push_back(acc);
  }
  return out;
}

// ------------------------------------------------------------ design

double target_db(const TargetCurve& t, double f) {
  // Shelves evaluated as the same biquads the engine's tone controls use.
  const double fs = 192000;  // high rate: no cramping near 20 kHz
  double bass = 20 * std::log10(std::abs(BiquadCoeffs::lowshelf(t.bass_corner_hz, t.bass_boost_db, fs).response(f, fs)));
  double treble = 20 * std::log10(std::abs(BiquadCoeffs::highshelf(t.treble_hz, t.treble_db, fs).response(f, fs)));
  double tilt = f > t.tilt_start_hz ? t.tilt_db_per_oct * std::log2(f / t.tilt_start_hz) : 0;
  return bass + treble + tilt;
}

std::vector<float> minphase_fir(const std::vector<double>& grid, const std::vector<double>& db, int taps, int fs) {
  size_t N = next_pow2(size_t(taps) * 4);
  // Real cepstrum of the log magnitude, folded onto positive time: the
  // homomorphic minimum-phase construction.
  std::vector<cplx> logmag(N);
  for (size_t k = 0; k <= N / 2; ++k) {
    double f = double(k) * fs / double(N);
    double d = interp_log(grid, db, std::max(f, grid.front()));
    logmag[k] = d / 20.0 * std::log(10.0);
    if (k > 0 && k < N / 2) logmag[N - k] = logmag[k];
  }
  auto cep = cfft(logmag, true);
  for (auto& v : cep) v /= double(N);
  std::vector<cplx> fold(N, 0.0);
  fold[0] = cep[0].real();
  for (size_t n = 1; n < N / 2; ++n) fold[n] = 2.0 * cep[n].real();
  fold[N / 2] = cep[N / 2].real();
  auto spec = cfft(fold, false);
  for (auto& v : spec) v = std::exp(v);
  auto h = cfft(spec, true);

  std::vector<float> out(static_cast<size_t>(taps));
  size_t fade = size_t(taps) / 10;
  for (size_t i = 0; i < size_t(taps); ++i) {
    double w = 1;
    if (i >= size_t(taps) - fade) w = 0.5 + 0.5 * std::cos(M_PI * double(i - (size_t(taps) - fade)) / double(fade));
    out[i] = float(h[i].real() / double(N) * w);
  }
  return out;
}

std::vector<cplx> fir_response(const std::vector<float>& h, const std::vector<double>& freqs, int fs) {
  std::vector<cplx> out;
  out.reserve(freqs.size());
  for (double f : freqs) {
    cplx rot = std::polar(1.0, -2 * M_PI * f / fs), ph = 1, acc = 0;
    for (size_t i = 0; i < h.size(); ++i) {
      acc += double(h[i]) * ph;
      ph *= rot;
      if ((i & 1023) == 1023) ph /= std::abs(ph);
    }
    out.push_back(acc);
  }
  return out;
}

double fir_peak_db(const std::vector<float>& h) {
  std::vector<double> x(h.begin(), h.end());
  auto X = rfft(x, next_pow2(h.size() * 4));
  double mx = 0;
  for (const auto& v : X) mx = std::max(mx, std::abs(v));
  return 20 * std::log10(mx + 1e-30);
}

}  // namespace rc
