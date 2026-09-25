// Measurement signal processing: sweeps, deconvolution, windowing,
// smoothing, microphone calibration, target curves and minimum-phase FIR
// design. Double precision throughout; none of this runs in realtime.
#pragma once

#include <complex>
#include <string>
#include <vector>

#include "config.hpp"

namespace rc {

using cplx = std::complex<double>;

// ------------------------------------------------------------------ FFT

size_t next_pow2(size_t n);
std::vector<cplx> rfft(const std::vector<double>& x, size_t n);    // n/2+1 bins
std::vector<double> irfft(const std::vector<cplx>& X, size_t n);   // normalised
std::vector<cplx> cfft(const std::vector<cplx>& x, bool inverse);  // unnormalised

// ------------------------------------------------------------- signals

struct Sweep {
  std::vector<double> signal;  // unit amplitude, faded
  double f1 = 0, f2 = 0;
  int fs = kSampleRate;
};

// Exponential (Farina) sine sweep.
Sweep make_sweep(double f1, double f2, double seconds, int fs);

// Recovers the impulse response from a recording of `sweep` by regularised
// spectral division. The result starts at the sweep's start sample, so any
// system latency shows up as a delay of the IR peak.
std::vector<double> deconvolve(const std::vector<double>& recorded, const Sweep& sweep, size_t ir_len);

// Band-limited pink noise with unit RMS, periodic in n (loops seamlessly).
std::vector<double> pink_noise(size_t n, double f_lo, double f_hi, int fs, unsigned seed);

// RMS of x restricted to [f_lo, f_hi], in dBFS (AES17: full-scale sine = 0).
double band_level_dbfs(const std::vector<double>& x, double f_lo, double f_hi, int fs);
double peak_dbfs(const std::vector<double>& x);

// ------------------------------------------------------------ microphone

struct MicCal {
  std::string path, serial;
  double sens_db = 0;          // "Sens Factor"
  std::vector<double> f, db;   // mic deviation; subtract from measurements
  double at(double freq) const;
  // Approximate SPL for a given dBFS (AES17) reading, from the UMIK-1's
  // nominal -18 dBFS @ 94 dB SPL and the sensitivity factor.
  double spl(double dbfs) const { return dbfs + 112.0 - sens_db; }
};

MicCal load_mic_cal(const std::string& path);  // throws

// ------------------------------------------------------------ frequency domain

// Log-spaced frequencies from f_lo to f_hi, `per_octave` points per octave.
std::vector<double> log_grid(double f_lo, double f_hi, int per_octave);

// Arrival time (in samples, fractional) of the energy in [f_lo, f_hi]:
// the peak of the band-passed Hilbert envelope.
double arrival_time(const std::vector<double>& ir, double f_lo, double f_hi, int fs);

// Frequency-dependent windowed power spectrum (like REW's FDW): each
// frequency sees a right-side window of `cycles` periods, clamped to
// [min_ms, max_ms]. Long windows in the bass capture room modes (what we
// want to correct); short windows in the treble keep late reflections out.
// Returned on the bins of an nfft-point FFT.
std::vector<double> fdw_power(const std::vector<double>& ir, double peak, int fs, size_t nfft, double pre_ms,
                              double cycles, double min_ms, double max_ms);

// Variable fractional-octave smoothing of a linear-bin power spectrum onto
// `grid`, result in dB. Resolution is 1/24 octave in the bass, easing to
// 1/3 octave in the treble (roughly what the ear resolves).
std::vector<double> smooth_to_grid(const std::vector<double>& power, double bin_hz, const std::vector<double>& grid);

// Constant-fraction smoothing of a curve already on a log grid (power domain).
std::vector<double> smooth_grid(const std::vector<double>& grid, const std::vector<double>& db, double octaves);

// 1 inside [f_lo, f_hi], falling to 0 over half an octave outside.
double band_taper(double f, double f_lo, double f_hi);

// Linear interpolation of a dB curve on a log grid (clamped at the ends).
double interp_log(const std::vector<double>& grid, const std::vector<double>& y, double f);

// Complex response of an IR at arbitrary frequencies, using the given
// window (start sample, length) applied identically to every IR that must
// keep relative phase.
std::vector<cplx> windowed_response(const std::vector<double>& ir, long start, size_t len, const std::vector<double>& freqs,
                                    int fs);

// ------------------------------------------------------------ design

double target_db(const TargetCurve& t, double f);

// Minimum-phase FIR whose magnitude follows `db` (on the log grid `grid`).
std::vector<float> minphase_fir(const std::vector<double>& grid, const std::vector<double>& db, int taps, int fs);

// Complex frequency response of an FIR at the given frequencies.
std::vector<cplx> fir_response(const std::vector<float>& h, const std::vector<double>& freqs, int fs);

// Largest gain of the FIR in dB (over the whole band).
double fir_peak_db(const std::vector<float>& h);

}  // namespace rc
