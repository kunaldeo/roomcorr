// Biquad sections (RBJ cookbook) and Linkwitz-Riley crossovers.
// State is kept in double: at 48 kHz a 30 Hz filter has poles very close to
// the unit circle and single precision adds audible noise there.
#pragma once

#include <cmath>
#include <complex>

namespace rc {

struct BiquadCoeffs {
  double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;

  static BiquadCoeffs lowpass(double f, double q, double fs) {
    double w = 2 * M_PI * f / fs, c = std::cos(w), al = std::sin(w) / (2 * q), a0 = 1 + al;
    return {(1 - c) / 2 / a0, (1 - c) / a0, (1 - c) / 2 / a0, -2 * c / a0, (1 - al) / a0};
  }
  static BiquadCoeffs highpass(double f, double q, double fs) {
    double w = 2 * M_PI * f / fs, c = std::cos(w), al = std::sin(w) / (2 * q), a0 = 1 + al;
    return {(1 + c) / 2 / a0, -(1 + c) / a0, (1 + c) / 2 / a0, -2 * c / a0, (1 - al) / a0};
  }
  // Shelves use slope S = 1 (the gentlest monotonic shelf).
  static BiquadCoeffs lowshelf(double f, double gain_db, double fs) {
    double A = std::pow(10, gain_db / 40), w = 2 * M_PI * f / fs, c = std::cos(w), s = std::sin(w);
    double al = s / 2 * std::sqrt(2.0), sq = 2 * std::sqrt(A) * al;
    double a0 = (A + 1) + (A - 1) * c + sq;
    return {A * ((A + 1) - (A - 1) * c + sq) / a0, 2 * A * ((A - 1) - (A + 1) * c) / a0,
            A * ((A + 1) - (A - 1) * c - sq) / a0, -2 * ((A - 1) + (A + 1) * c) / a0,
            ((A + 1) + (A - 1) * c - sq) / a0};
  }
  static BiquadCoeffs highshelf(double f, double gain_db, double fs) {
    double A = std::pow(10, gain_db / 40), w = 2 * M_PI * f / fs, c = std::cos(w), s = std::sin(w);
    double al = s / 2 * std::sqrt(2.0), sq = 2 * std::sqrt(A) * al;
    double a0 = (A + 1) - (A - 1) * c + sq;
    return {A * ((A + 1) + (A - 1) * c + sq) / a0, -2 * A * ((A - 1) + (A + 1) * c) / a0,
            A * ((A + 1) + (A - 1) * c - sq) / a0, 2 * ((A - 1) - (A + 1) * c) / a0,
            ((A + 1) - (A - 1) * c - sq) / a0};
  }

  std::complex<double> response(double f, double fs) const {
    std::complex<double> z1 = std::polar(1.0, -2 * M_PI * f / fs), z2 = z1 * z1;
    return (b0 + b1 * z1 + b2 * z2) / (1.0 + a1 * z1 + a2 * z2);
  }
};

struct Biquad {
  BiquadCoeffs k;
  double s1 = 0, s2 = 0;  // transposed direct form II

  inline double tick(double x) {
    double y = k.b0 * x + s1;
    s1 = k.b1 * x - k.a1 * y + s2;
    s2 = k.b2 * x - k.a2 * y;
    return y;
  }
  void reset() { s1 = s2 = 0; }
};

// Linkwitz-Riley crossover half: LR4 is two identical 2nd-order Butterworth
// sections, LR8 two identical 4th-order Butterworth filters (4 sections).
// Low and high halves at the same frequency sum to an all-pass.
struct LinkwitzRiley {
  static constexpr int kMaxSections = 4;
  Biquad sec[kMaxSections];
  int n = 0;

  static int sections_for(int slope_db) { return slope_db >= 48 ? 4 : 2; }

  static void design(BiquadCoeffs* out, int slope_db, bool high, double f, double fs) {
    if (slope_db >= 48) {
      // 4th-order Butterworth Qs.
      const double qs[2] = {0.54119610, 1.30656296};
      for (int i = 0; i < 4; ++i) {
        double q = qs[i % 2];
        out[i] = high ? BiquadCoeffs::highpass(f, q, fs) : BiquadCoeffs::lowpass(f, q, fs);
      }
    } else {
      for (int i = 0; i < 2; ++i)
        out[i] = high ? BiquadCoeffs::highpass(f, M_SQRT1_2, fs) : BiquadCoeffs::lowpass(f, M_SQRT1_2, fs);
    }
  }

  void configure(int slope_db, bool high, double f, double fs) {
    BiquadCoeffs k[kMaxSections];
    int m = sections_for(slope_db);
    design(k, slope_db, high, f, fs);
    if (m != n) {
      for (auto& s : sec) s.reset();
      n = m;
    }
    // Keep state across frequency changes so moving the crossover slider
    // doesn't click.
    for (int i = 0; i < n; ++i) sec[i].k = k[i];
  }

  inline double tick(double x) {
    for (int i = 0; i < n; ++i) x = sec[i].tick(x);
    return x;
  }

  static std::complex<double> response(int slope_db, bool high, double f, double fc, double fs) {
    BiquadCoeffs k[kMaxSections];
    int m = sections_for(slope_db);
    design(k, slope_db, high, fc, fs);
    std::complex<double> h = 1;
    for (int i = 0; i < m; ++i) h *= k[i].response(f, fs);
    return h;
  }
};

}  // namespace rc
