#include "design.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <stdexcept>

#include "dsp/biquad.hpp"
#include "wav.hpp"

namespace rc {

namespace fs = std::filesystem;

std::string response_path() { return data_dir() + "/response.json"; }

static std::string ir_file(const std::string& dir, size_t pos, int chan) {
  return dir + "/pos" + std::to_string(pos + 1) + "_" + kChanNames[chan] + ".wav";
}

MeasurementSet load_measurements(const std::string& dir) {
  MeasurementSet m;
  m.dir = dir;
  m.meta = Json::parse(read_file(dir + "/meta.json"));
  m.fs = m.meta.get("fs").as_int(kSampleRate);
  if (m.fs != kSampleRate) throw std::runtime_error("measurements must be at 48 kHz");
  int positions = m.meta.get("positions").as_int(0);
  if (positions < 1) throw std::runtime_error(dir + ": no positions");
  for (int p = 0; p < positions; ++p) {
    std::vector<std::vector<double>> chans;
    for (int c = 0; c < kNumChans; ++c) {
      auto f = read_wav_mono(ir_file(dir, size_t(p), c), m.fs);
      chans.emplace_back(f.begin(), f.end());
    }
    m.irs.push_back(std::move(chans));
  }
  return m;
}

void save_measurements(const MeasurementSet& m) {
  ensure_dir(m.dir);
  Json meta = m.meta;
  meta["fs"] = m.fs;
  meta["positions"] = int(m.irs.size());
  write_file_atomic(m.dir + "/meta.json", meta.dump(2) + "\n");
  for (size_t p = 0; p < m.irs.size(); ++p)
    for (int c = 0; c < kNumChans; ++c) {
      std::vector<float> f(m.irs[p][size_t(c)].begin(), m.irs[p][size_t(c)].end());
      write_wav_mono(ir_file(m.dir, p, c), f, m.fs);
    }
}

namespace {

double mean_in_band(const std::vector<double>& grid, const std::vector<double>& y, double a, double b) {
  double s = 0;
  int n = 0;
  for (size_t i = 0; i < grid.size(); ++i)
    if (grid[i] >= a && grid[i] <= b) {
      s += y[i];
      ++n;
    }
  return n ? s / n : 0;
}

double median_in_band(const std::vector<double>& grid, const std::vector<double>& y, double a, double b) {
  std::vector<double> v;
  for (size_t i = 0; i < grid.size(); ++i)
    if (grid[i] >= a && grid[i] <= b) v.push_back(y[i]);
  if (v.empty()) return 0;
  std::nth_element(v.begin(), v.begin() + long(v.size() / 2), v.end());
  return v[v.size() / 2];
}

// Scans inward from one end of [f_lo, f_hi] (dir +1: upward from f_lo,
// dir -1: downward from f_hi) and returns the first frequency where y
// reaches `threshold`. Scanning from outside in means a room null inside
// the passband can't be mistaken for the speaker's roll-off.
double find_edge(const std::vector<double>& grid, const std::vector<double>& y, double f_lo, double f_hi, int dir,
                 double threshold) {
  if (dir > 0) {
    for (size_t i = 0; i < grid.size(); ++i)
      if (grid[i] >= f_lo && grid[i] <= f_hi && y[i] >= threshold) return grid[i];
    return f_hi;
  }
  for (size_t i = grid.size(); i-- > 0;)
    if (grid[i] >= f_lo && grid[i] <= f_hi && y[i] >= threshold) return grid[i];
  return f_lo;
}

std::string fmt(const char* f, double a) {
  char buf[128];
  snprintf(buf, sizeof buf, f, a);
  return buf;
}

Json round_vec(const std::vector<double>& v, double offset = 0) {
  Json a = Json::array();
  for (double x : v) a.push(std::round((x + offset) * 100) / 100);
  return a;
}

}  // namespace

DesignResult design_filters(const Config& in, const MeasurementSet& m, const MicCal& cal) {
  DesignResult r;
  r.cfg = in;
  Config& cfg = r.cfg;
  const int fs = m.fs;
  const size_t P = m.irs.size();
  const size_t nfft = kIrLength;
  const double bin = double(fs) / double(nfft);
  const auto grid = log_grid(10, 24000, 48);
  const double xo = cfg.crossover_hz;
  auto weight = [&](size_t p) { return (p == 0 && P >= 3) ? 2.0 : 1.0; };

  // ---- arrival times and spatially averaged magnitude responses
  std::vector<std::vector<double>> arrival(P, std::vector<double>(kNumChans));
  std::vector<double> M[kNumChans], Mc[kNumChans], Moct[kNumChans];
  std::vector<std::vector<double>> per_pos[kNumChans];
  for (int c = 0; c < kNumChans; ++c) {
    const bool is_sub = c == kSub;
    std::vector<double> acc(grid.size(), 0.0);
    double wsum = 0;
    for (size_t p = 0; p < P; ++p) {
      const auto& ir = m.irs[p][size_t(c)];
      arrival[p][size_t(c)] = is_sub ? arrival_time(ir, 30, 120, fs) : arrival_time(ir, 200, 5000, fs);
      auto pw = fdw_power(ir, arrival[p][size_t(c)], fs, nfft, is_sub ? 50 : 3, 15, 5, 500);
      auto db = smooth_to_grid(pw, bin, grid);
      per_pos[c].push_back(smooth_grid(grid, db, 1.0 / 6));
      for (size_t i = 0; i < grid.size(); ++i) acc[i] += weight(p) * std::pow(10, (db[i] - cal.at(grid[i])) / 10);
      wsum += weight(p);
    }
    M[c].resize(grid.size());
    for (size_t i = 0; i < grid.size(); ++i) M[c][i] = 10 * std::log10(acc[i] / wsum + 1e-30);
    Mc[c] = smooth_grid(grid, M[c], 1.0 / 3);
    Moct[c] = smooth_grid(grid, M[c], 1.0);
  }

  std::vector<double> T(grid.size());
  for (size_t i = 0; i < grid.size(); ++i) T[i] = target_db(cfg.target, grid[i]);
  auto minus_target = [&](const std::vector<double>& y) {
    std::vector<double> d(y.size());
    for (size_t i = 0; i < y.size(); ++i) d[i] = y[i] - T[i];
    return d;
  };

  // ---- levels: mains matched on 300 Hz-3 kHz, sub matched in its passband
  const double lvlL = mean_in_band(grid, minus_target(M[kLeft]), 300, 3000);
  const double lvlR = mean_in_band(grid, minus_target(M[kRight]), 300, 3000);
  const double ref = (lvlL + lvlR) / 2;
  double trim[kNumChans];
  trim[kLeft] = ref - lvlL;
  trim[kRight] = ref - lvlR;

  // Speaker limits: where the third-octave response first comes within 6 dB
  // of its typical (median) level, scanning in from the band edges.
  auto Dsub = minus_target(Mc[kSub]);
  const double sub_nominal = median_in_band(grid, Dsub, 35, 100);
  const double sub_lo = std::max(12.0, find_edge(grid, Dsub, 12, 200, +1, sub_nominal - 6));
  const double sub_hi = std::min(400.0, find_edge(grid, Dsub, 40, 400, -1, sub_nominal - 6));
  const double lvlS = mean_in_band(grid, Dsub, std::max(sub_lo * 1.3, 25.0), std::min(xo, sub_hi / 1.3));
  trim[kSub] = ref - lvlS;

  std::vector<double> Dmain(grid.size());
  for (size_t i = 0; i < grid.size(); ++i) Dmain[i] = (Mc[kLeft][i] + Mc[kRight][i]) / 2 - T[i] - ref;
  const double main_lo = find_edge(grid, Dmain, 15, 300, +1, -6);

  // The target follows the system's natural low-end roll-off (2nd order
  // below it), like Dirac's automatic target: no boost is spent trying to
  // make the sub play below what it can.
  const double sys_lo = cfg.bass_management ? sub_lo : main_lo;
  for (size_t i = 0; i < grid.size(); ++i) {
    double x4 = std::pow(grid[i] / sys_lo, 4);
    T[i] += 10 * std::log10(x4 / (1 + x4));
  }

  // ---- correction curves
  double lo[kNumChans], hi[kNumChans];
  lo[kLeft] = lo[kRight] = cfg.target.low_hz > 0 ? cfg.target.low_hz
                                                : std::max(main_lo, cfg.bass_management ? xo / 2 : 0.0);
  hi[kLeft] = hi[kRight] = cfg.target.high_hz;
  lo[kSub] = std::max({sub_lo, cfg.target.low_hz, 15.0});
  hi[kSub] = std::min(sub_hi, std::max(2 * xo, 160.0));

  // Below the transition frequency: full-resolution correction of room
  // modes. Above twice that: only a broad (octave-smoothed, +/-hf_max_db)
  // tonal correction, since a mic there measures reflections the ear
  // largely ignores and narrow EQ makes speakers sound hollow (Toole;
  // Dirac ART works only up to 150 Hz for the same reason).
  const double f_full = cfg.target.transition_hz, f_broad = 2 * cfg.target.transition_hz;
  auto full_weight = [&](double f) {
    if (f <= f_full) return 1.0;
    if (f >= f_broad) return 0.0;
    double x = std::log(f / f_full) / std::log(f_broad / f_full);
    return 0.5 + 0.5 * std::cos(M_PI * x);
  };
  std::vector<double> C[kNumChans];
  for (int c = 0; c < kNumChans; ++c) {
    C[c].resize(grid.size());
    for (size_t i = 0; i < grid.size(); ++i) {
      double want_fine = T[i] + ref - (M[c][i] + trim[c]);
      double want_coarse = T[i] + ref - (Mc[c][i] + trim[c]);
      double v;
      if (want_fine > 0) {
        // Only fill a dip as far as the surrounding third-octave is below
        // target: narrow nulls are cancellations that boost cannot fix.
        v = std::min({want_fine, std::max(want_coarse, 0.0) + 1.0, cfg.target.max_boost_db});
        // With several positions, only boost dips that stay put: a null
        // that moves with the mic is corrected at one seat and made worse
        // at the next (Johansson, Dirac).
        if (per_pos[c].size() >= 2) {
          double mean = 0, sq = 0;
          for (const auto& pp : per_pos[c]) mean += pp[i];
          mean /= double(per_pos[c].size());
          for (const auto& pp : per_pos[c]) sq += (pp[i] - mean) * (pp[i] - mean);
          double spread = std::sqrt(sq / double(per_pos[c].size()));
          v *= std::clamp((6.0 - spread) / 3.0, 0.0, 1.0);
        }
      } else {
        v = std::max(want_fine, -cfg.target.max_cut_db);
      }
      double broad = std::clamp(T[i] + ref - (Moct[c][i] + trim[c]), -cfg.target.hf_max_db, cfg.target.hf_max_db / 2);
      double w = full_weight(grid[i]);
      C[c][i] = (w * v + (1 - w) * broad) * band_taper(grid[i], lo[c], hi[c]);
    }
    r.filters[c] = minphase_fir(grid, C[c], kFilterTaps, fs);
  }

  // ---- mains time alignment (main listening position)
  const double tL = arrival[0][kLeft], tR = arrival[0][kRight];
  double delay_ms[kNumChans];
  delay_ms[kLeft] = (std::max(tL, tR) - tL) * 1000.0 / fs;
  delay_ms[kRight] = (std::max(tL, tR) - tR) * 1000.0 / fs;

  // A big left/right difference almost always means the mic was off-center
  // rather than an asymmetric room; applying it in full would pull the
  // stereo image to one side at the real seat. Cap it and say so.
  const double kMaxBalanceDb = 2.0, kMaxSkewMs = 1.0;
  const double raw_balance = trim[kLeft] - trim[kRight], raw_skew = delay_ms[kLeft] - delay_ms[kRight];
  if (std::fabs(raw_balance) > 2 * kMaxBalanceDb || std::fabs(raw_skew) > kMaxSkewMs) {
    r.warnings.push_back(fmt("Left/right differ by %.1f dB", raw_balance) + fmt(" and %.2f ms at the mic;", raw_skew) +
                         " limited to ±2 dB and 1 ms. Was the mic centred between the speakers?");
  }
  const double bal = std::clamp(raw_balance, -2 * kMaxBalanceDb, 2 * kMaxBalanceDb);
  const double mid = (trim[kLeft] + trim[kRight]) / 2;
  trim[kLeft] = mid + bal / 2;
  trim[kRight] = mid - bal / 2;
  const double skew = std::clamp(raw_skew, -kMaxSkewMs, kMaxSkewMs);
  delay_ms[kLeft] = std::max(0.0, skew);
  delay_ms[kRight] = std::max(0.0, -skew);

  // ---- sub alignment: pick the delay and polarity that make the sub and
  // the mains add up best through the crossover region, over all positions.
  const auto F = log_grid(15, 500, 48);
  std::vector<std::vector<cplx>> H(P * kNumChans);
  for (size_t p = 0; p < P; ++p) {
    double first = std::min({arrival[p][kLeft], arrival[p][kRight], arrival[p][kSub]});
    long start = long(first) - long(0.030 * fs);
    for (int c = 0; c < kNumChans; ++c)
      H[p * kNumChans + size_t(c)] = windowed_response(m.irs[p][size_t(c)], start, size_t(0.5 * fs), F, fs);
  }
  std::vector<cplx> CF[kNumChans];
  for (int c = 0; c < kNumChans; ++c) CF[c] = fir_response(r.filters[c], F, fs);

  const bool hp_on = cfg.bass_management && cfg.mains_highpass;
  auto mains_at = [&](size_t p, size_t k, bool corrected) {
    double w = 2 * M_PI * F[k];
    cplx sum = 0;
    for (int c : {int(kLeft), int(kRight)}) {
      cplx h = H[p * kNumChans + size_t(c)][k];
      if (corrected)
        h *= CF[c][k] * std::pow(10.0, trim[c] / 20) * std::polar(1.0, -w * delay_ms[c] / 1000);
      sum += h;
    }
    if (corrected && hp_on) sum *= LinkwitzRiley::response(cfg.crossover_slope, true, F[k], xo, fs);
    return sum;
  };
  auto sub_at = [&](size_t p, size_t k) {
    // x2: the sub receives L+R.
    return H[p * kNumChans + kSub][k] * CF[kSub][k] * std::pow(10.0, trim[kSub] / 20) * 2.0 *
           LinkwitzRiley::response(cfg.crossover_slope, false, F[k], xo, fs);
  };

  std::vector<size_t> band;
  for (size_t k = 0; k < F.size(); ++k)
    if (F[k] >= xo / 2 && F[k] <= xo * 2) band.push_back(k);
  std::vector<cplx> Mn(P * F.size()), Sb(P * F.size());
  for (size_t p = 0; p < P; ++p)
    for (size_t k = 0; k < F.size(); ++k) {
      Mn[p * F.size() + k] = mains_at(p, k, true);
      Sb[p * F.size() + k] = sub_at(p, k);
    }
  auto score = [&](double d_ms, double pol) {
    double num = 0, den = 0;
    for (size_t p = 0; p < P; ++p)
      for (size_t k : band) {
        cplx mm = Mn[p * F.size() + k];
        cplx ss = Sb[p * F.size() + k] * pol * std::polar(1.0, -2 * M_PI * F[k] * d_ms / 1000);
        num += weight(p) * std::norm(mm + ss);
        den += weight(p) * std::pow(std::abs(mm) + std::abs(ss), 2);
      }
    return den > 0 ? num / den : 0;
  };
  double best_d = 0, best_pol = 1, best_score = -1;
  for (double pol : {1.0, -1.0})
    for (double d = -20; d <= 20.0001; d += 0.05) {
      // Tiny bias toward small delays breaks ties between solutions a whole
      // crossover period apart.
      double s = score(d, pol) - 0.0004 * std::fabs(d);
      if (s > best_score) {
        best_score = s;
        best_d = d;
        best_pol = pol;
      }
    }
  const double score_before = score(0, 1), score_after = score(best_d, best_pol);
  delay_ms[kSub] = best_d;

  for (int c = 0; c < kNumChans; ++c) {
    cfg.ch[c].trim_db = trim[c];
    cfg.ch[c].delay_ms = delay_ms[c];
    cfg.ch[c].filter_peak_db = fir_peak_db(r.filters[c]);
  }
  cfg.ch[kLeft].invert = cfg.ch[kRight].invert = false;
  cfg.ch[kSub].invert = best_pol < 0;
  cfg.measurement = m.dir;

  // ---- report for the plugin graph. Levels are relative: the reference
  // level (mains 300 Hz-3 kHz) is drawn at 75 dB.
  const double off = 75 - ref;
  const auto disp = log_grid(15, 20000, 12);
  auto on_disp = [&](const std::vector<double>& y) {
    std::vector<double> o;
    for (double f : disp) o.push_back(interp_log(grid, y, f));
    return o;
  };
  Json rep = Json::object();
  rep["created"] = int64_t(time(nullptr));
  rep["measurement"] = m.dir;
  rep["freqs"] = round_vec(disp);
  std::vector<double> tgt(grid.size());
  for (size_t i = 0; i < grid.size(); ++i) tgt[i] = T[i] + ref;
  rep["target"] = round_vec(on_disp(tgt), off);
  Json chans = Json::object();
  for (int c = 0; c < kNumChans; ++c) {
    std::vector<double> after(grid.size());
    for (size_t i = 0; i < grid.size(); ++i) after[i] = M[c][i] + trim[c] + C[c][i];
    Json j = Json::object();
    j["measured"] = round_vec(on_disp(M[c]), off);
    j["corrected"] = round_vec(on_disp(after), off);
    j["correction"] = round_vec(on_disp(C[c]));
    j["range"] = Json(std::vector<double>{lo[c], hi[c]});
    chans[kChanNames[c]] = j;
  }
  rep["channels"] = chans;

  // System (mono signal into both inputs) before vs after, bass region.
  std::vector<double> sys_before, sys_after;
  for (size_t k = 0; k < F.size(); ++k) {
    double b = 0, a = 0, ws = 0;
    for (size_t p = 0; p < P; ++p) {
      b += weight(p) * std::norm(mains_at(p, k, false));
      cplx s = Sb[p * F.size() + k] * best_pol * std::polar(1.0, -2 * M_PI * F[k] * best_d / 1000);
      a += weight(p) * std::norm(Mn[p * F.size() + k] + (cfg.bass_management ? s : cplx(0)));
      ws += weight(p);
    }
    // -6 dB: two speakers playing the same signal, back on the per-channel scale.
    sys_before.push_back(10 * std::log10(b / ws + 1e-30) - 6.02 - cal.at(F[k]));
    sys_after.push_back(10 * std::log10(a / ws + 1e-30) - 6.02 - cal.at(F[k]));
  }
  Json sys = Json::object();
  sys["freqs"] = round_vec(F);
  sys["before"] = round_vec(sys_before, off);
  sys["after"] = round_vec(sys_after, off);
  rep["system"] = sys;

  Json sum = Json::object();
  sum["trim_db"] = Json(std::vector<double>{trim[0], trim[1], trim[2]});
  sum["delay_ms"] = Json(std::vector<double>{delay_ms[0], delay_ms[1], delay_ms[2]});
  sum["sub_invert"] = best_pol < 0;
  sum["crossover_sum_before"] = score_before;
  sum["crossover_sum_after"] = score_after;
  sum["sub_range_hz"] = Json(std::vector<double>{sub_lo, sub_hi});
  sum["mains_low_hz"] = main_lo;
  sum["positions"] = int(P);
  // Predicted bass accuracy at the listening area: RMS distance of the
  // corrected system from the target over the sub's usable range to 250 Hz.
  // Used to pick the crossover.
  {
    double sq = 0;
    int n = 0;
    for (size_t k = 0; k < F.size(); ++k) {
      if (F[k] < std::max(sub_lo * 1.2, 25.0) || F[k] > 250) continue;
      double t = interp_log(grid, T, F[k]) + ref;
      sq += std::pow(sys_after[k] - t, 2);
      ++n;
    }
    r.bass_deviation_db = n ? std::sqrt(sq / n) : 99;
    sum["bass_deviation_db"] = r.bass_deviation_db;
  }
  sum["crossover_hz"] = xo;
  rep["summary"] = sum;
  r.report = rep;

  // ---- human-readable notes
  double dist_cm = std::fabs(tL - tR) / fs * 343.0 * 100;
  r.notes.push_back(fmt("Mains level trim:    L %+.1f dB", trim[kLeft]) + fmt("   R %+.1f dB", trim[kRight]));
  r.notes.push_back(fmt("Mains delay:         L %.2f ms", delay_ms[kLeft]) + fmt("   R %.2f ms", delay_ms[kRight]) +
                    fmt("   (%.0f cm path difference)", dist_cm));
  r.notes.push_back(fmt("Mains -6 dB point:   %.0f Hz", main_lo));
  r.notes.push_back(fmt("Sub usable range:    %.0f", sub_lo) + fmt("-%.0f Hz", sub_hi));
  r.notes.push_back(fmt("Sub level trim:      %+.1f dB", trim[kSub]));
  r.notes.push_back(fmt("Sub delay:           %+.2f ms", best_d) + (best_pol < 0 ? "   polarity INVERTED" : "   polarity normal"));
  r.notes.push_back(fmt("Crossover summation: %.0f%%", score_before * 100) + fmt(" -> %.0f%% of ideal", score_after * 100));
  for (int c = 0; c < kNumChans; ++c)
    r.notes.push_back(std::string("FIR ") + kChanNames[c] + fmt(": peak gain %+.1f dB", cfg.ch[c].filter_peak_db));

  if (trim[kSub] > 8)
    r.warnings.push_back(fmt("The sub needs %+.0f dB of digital gain. Turn the sub's volume knob up and recalibrate.", trim[kSub]));
  if (trim[kSub] < -12)
    r.warnings.push_back(fmt("The sub is %.0f dB too loud. Turn its volume knob down and recalibrate.", -trim[kSub]));
  if (score_after < 0.7)
    r.warnings.push_back("Sub and mains do not sum well through the crossover even after alignment; "
                         "try moving the sub or a different crossover frequency.");
  return r;
}

DesignResult design_best(const Config& in, const MeasurementSet& m, const MicCal& cal) {
  if (!in.target.auto_crossover || !in.bass_management) return design_filters(in, m, cal);
  // Like Dirac's Bass Control: try each crossover and keep the one whose
  // predicted bass is closest to target in this room. A pull toward 80 Hz
  // (the THX default; 0.6 dB per 20 Hz) keeps it there unless another
  // crossover is clearly better.
  DesignResult best;
  double best_score = 1e9;
  std::string table;
  for (double xo : {60.0, 70.0, 80.0, 90.0, 100.0, 110.0, 120.0}) {
    Config c = in;
    c.crossover_hz = xo;
    DesignResult r = design_filters(c, m, cal);
    // Don't hand the mains bass they can't play.
    double mains_lo = r.report.get("summary").get("mains_low_hz").as_num(40);
    if (xo < mains_lo * 1.5) continue;
    double score = r.bass_deviation_db + 0.03 * std::fabs(xo - 80);
    char buf[64];
    snprintf(buf, sizeof buf, "%s%.0f Hz %.1f dB", table.empty() ? "" : ", ", xo, r.bass_deviation_db);
    table += buf;
    if (score < best_score) {
      best_score = score;
      best = std::move(r);
    }
  }
  if (best_score >= 1e9) return design_filters(in, m, cal);
  char buf[96];
  snprintf(buf, sizeof buf, "Crossover:           %.0f Hz (auto; bass deviation per crossover: ", best.cfg.crossover_hz);
  best.notes.insert(best.notes.begin(), std::string(buf) + table + ")");
  return best;
}

void install_design(DesignResult& r) {
  std::string dir = data_dir() + "/filters";
  ensure_dir(dir);
  for (int c = 0; c < kNumChans; ++c) {
    std::string path = dir + "/" + kChanNames[c] + ".wav";
    std::string tmp = path + ".tmp.wav";
    write_wav_mono(tmp, r.filters[c], kSampleRate);
    fs::rename(tmp, path);
    r.cfg.ch[c].filter = path;
  }
  write_file_atomic(response_path(), r.report.dump() + "\n");
}

}  // namespace rc
