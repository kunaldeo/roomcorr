// Interactive calibration wizard plus the `design`, `verify` and `setup`
// commands. Runs in a terminal; the Omarchy plugin launches it in one.
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "analysis.hpp"
#include "config.hpp"
#include "control.hpp"
#include "design.hpp"
#include "devices.hpp"
#include "measure.hpp"

namespace rc {

namespace fs = std::filesystem;

int run_verify(int argc, char** argv);

namespace {

// ------------------------------------------------------------------ terminal

const bool kColor = isatty(1);
std::string bold(const std::string& s) { return kColor ? "\033[1m" + s + "\033[0m" : s; }
std::string dim(const std::string& s) { return kColor ? "\033[2m" + s + "\033[0m" : s; }
std::string green(const std::string& s) { return kColor ? "\033[32m" + s + "\033[0m" : s; }
std::string yellow(const std::string& s) { return kColor ? "\033[33m" + s + "\033[0m" : s; }
std::string red(const std::string& s) { return kColor ? "\033[31m" + s + "\033[0m" : s; }
std::string cyan(const std::string& s) { return kColor ? "\033[36m" + s + "\033[0m" : s; }

std::string f1(const char* fmt, double v) {
  char buf[64];
  snprintf(buf, sizeof buf, fmt, v);
  return buf;
}

void step(int n, const std::string& title) { printf("\n%s %s\n", cyan(bold("[" + std::to_string(n) + "]")).c_str(), bold(title).c_str()); }

// --yes: answer every prompt automatically (for unattended runs).
bool g_auto = false;

std::string ask(const std::string& prompt) {
  printf("%s ", prompt.c_str());
  if (g_auto) {
    printf("%s\n", dim("(auto)").c_str());
    return "";
  }
  fflush(stdout);
  std::string line;
  if (!std::getline(std::cin, line)) throw std::runtime_error("aborted");
  return line;
}

void press_enter(const std::string& what = "Press Enter to continue") { ask(dim("› " + what + "...")); }

// auto_answer: what --yes picks (retry prompts say no, so they can't loop).
bool yes(const std::string& prompt, bool def = true, int auto_answer = -1) {
  if (g_auto) {
    bool a = auto_answer < 0 ? def : auto_answer == 1;
    printf("%s %s\n", prompt.c_str(), dim(a ? "(auto: yes)" : "(auto: no)").c_str());
    return a;
  }
  std::string a = ask(prompt + (def ? " [Y/n]" : " [y/N]"));
  if (a.empty()) return def;
  return a[0] == 'y' || a[0] == 'Y';
}

// ------------------------------------------------------------------ restore on exit

// The daemon is muted while we measure; make sure it is unmuted again even
// if the wizard is interrupted.
bool g_daemon_muted = false;
std::string g_restore_sink;
double g_restore_volume = -1;

void restore() {
  if (g_daemon_muted) {
    try {
      Json r = Json::object();
      r["cmd"] = "set";
      r["values"]["mute"] = false;
      control_request(r);
    } catch (...) {
    }
    g_daemon_muted = false;
  }
  if (!g_restore_sink.empty() && g_restore_volume >= 0) {
    set_sink_volume(g_restore_sink, g_restore_volume);
    g_restore_sink.clear();
  }
}

void on_interrupt(int) {
  restore();
  printf("\n%s\n", yellow("Interrupted; audio restored.").c_str());
  _exit(130);
}

bool daemon_set(const Json& values) {
  try {
    Json r = Json::object();
    r["cmd"] = "set";
    r["values"] = values;
    return control_request(r).get("type").as_str() == "state";
  } catch (...) {
    return false;
  }
}

bool daemon_reload() {
  try {
    Json r = Json::object();
    r["cmd"] = "reload";
    control_request(r);
    return true;
  } catch (...) {
    return false;
  }
}

// ------------------------------------------------------------------ signals

const std::vector<std::string> kX4Positions = {"FL", "FR", "FC", "LFE", "RL", "RR"};

int pos_index(const std::string& p) {
  for (size_t i = 0; i < kX4Positions.size(); ++i)
    if (kX4Positions[i] == p) return int(i);
  return -1;
}

// Pink noise burst on the given outputs at `level_dbfs` (AES17), returns the
// mic level in [f_lo, f_hi] as dBFS.
struct Burst {
  double mic_dbfs;
  double capture_peak;
};

Burst noise_burst(const std::string& target, const std::string& mic, const std::vector<std::string>& outs,
                  double level_dbfs, double f_lo, double f_hi, double seconds = 1.5) {
  const size_t n = size_t(seconds * kSampleRate);
  auto pn = pink_noise(1 << 16, f_lo, f_hi, kSampleRate, 7);
  const double rms = std::pow(10.0, (level_dbfs - 3.0103) / 20);
  std::vector<float> sig(n);
  size_t ramp = size_t(0.05 * kSampleRate);
  for (size_t i = 0; i < n; ++i) {
    double w = std::min({1.0, double(i) / double(ramp), double(n - i) / double(ramp)});
    sig[i] = float(pn[i % pn.size()] * rms * w);
  }
  MeasureRequest req;
  req.play_target = target;
  req.positions = kX4Positions;
  req.capture_target = mic;
  req.tail_seconds = 0.2;
  for (const auto& p : kX4Positions) {
    bool on = std::find(outs.begin(), outs.end(), p) != outs.end();
    req.signal.push_back(on ? sig : std::vector<float>(n, 0.f));
  }
  auto r = run_measurement(req);
  // Skip the first 0.4 s: acoustic latency, ramp, room build-up.
  size_t from = std::min(r.recording.size(), size_t(0.4 * kSampleRate));
  size_t to = std::min(r.recording.size(), n);
  std::vector<double> seg(r.recording.begin() + long(from), r.recording.begin() + long(std::max(from, to)));
  return {band_level_dbfs(seg, f_lo, f_hi, kSampleRate), r.capture_peak_dbfs};
}

double silence_level(const std::string& target, const std::string& mic, double f_lo, double f_hi) {
  return noise_burst(target, mic, {}, -200, f_lo, f_hi, 1.0).mic_dbfs;
}

// Raises the test level until the mic reads target_spl (or max_level).
double auto_level(const std::string& target, const std::string& mic, const MicCal& cal,
                  const std::vector<std::string>& outs, double f_lo, double f_hi, double target_spl, double floor_dbfs,
                  double* spl_out) {
  double level = -45, max_level = -10;
  double spl = 0;
  for (int i = 0; i < 8; ++i) {
    Burst b = noise_burst(target, mic, outs, level, f_lo, f_hi);
    spl = cal.spl(b.mic_dbfs);
    printf("    test signal %s dBFS  →  %s dB SPL  %s\n", f1("%6.1f", level).c_str(), f1("%5.1f", spl).c_str(),
           dim("(" + f1("%.0f", b.mic_dbfs - floor_dbfs) + " dB above room noise)").c_str());
    if (b.capture_peak > -2) {
      printf("    %s\n", yellow("microphone is close to clipping; backing off").c_str());
      level -= 6;
      break;
    }
    // Loud enough once we hit the target SPL, or once the signal is 45 dB
    // over the room noise (plenty for sweeps, which add ~30 dB of
    // processing gain on top).
    const double snr = b.mic_dbfs - floor_dbfs;
    if (spl >= target_spl - 1.5 || snr >= 45 || level >= max_level) break;
    double stepdb = std::clamp(std::min(target_spl - spl, 45 - snr), 2.0, 12.0);
    level = std::min(max_level, level + stepdb);
  }
  if (spl_out) *spl_out = spl;
  return level;
}

struct Sequence {
  std::vector<std::vector<float>> signal;  // X4 positions
  size_t offset[kNumChans];
  Sweep sweep[kNumChans];  // scaled by amplitude
  size_t seg_len[kNumChans];
};

// One position: left sweep, right sweep, sub sweep, back to back, in one run.
Sequence build_sequence(double main_amp, double sub_amp, const std::vector<std::string>& sub_outs) {
  Sequence s;
  const double gap = 2.2;
  s.sweep[kLeft] = s.sweep[kRight] = make_sweep(15, 22000, 6.0, kSampleRate);
  s.sweep[kSub] = make_sweep(10, 1000, 6.0, kSampleRate);
  for (auto& v : s.sweep[kLeft].signal) v *= main_amp;
  for (auto& v : s.sweep[kRight].signal) v *= main_amp;
  for (auto& v : s.sweep[kSub].signal) v *= sub_amp;
  size_t pos = size_t(0.3 * kSampleRate);
  for (int c = 0; c < kNumChans; ++c) {
    s.offset[c] = pos;
    s.seg_len[c] = s.sweep[c].signal.size() + size_t(gap * kSampleRate);
    pos += s.seg_len[c];
  }
  s.signal.assign(kX4Positions.size(), std::vector<float>(pos, 0.f));
  auto put = [&](const std::string& out, int c) {
    int o = pos_index(out);
    if (o < 0) return;
    for (size_t i = 0; i < s.sweep[c].signal.size(); ++i) s.signal[size_t(o)][s.offset[c] + i] = float(s.sweep[c].signal[i]);
  };
  put("FL", kLeft);
  put("FR", kRight);
  for (const auto& o : sub_outs) put(o, kSub);
  return s;
}

double ir_snr_db(const std::vector<double>& ir) {
  double pk = 0;
  for (double v : ir) pk = std::max(pk, std::fabs(v));
  size_t a = size_t(1.8 * kSampleRate), b = std::min(ir.size(), size_t(2.6 * kSampleRate));
  double sq = 0;
  for (size_t i = a; i < b; ++i) sq += ir[i] * ir[i];
  double noise = std::sqrt(sq / double(std::max<size_t>(1, b - a)));
  return 20 * std::log10(pk / (noise + 1e-30));
}

const char* kPositionHints[] = {
    "MAIN listening position: where your head is, at ear height",
    "about 30 cm to the LEFT of the main position",
    "about 30 cm to the RIGHT of the main position",
    "about 30 cm FORWARD (toward the speakers)",
    "about 30 cm BACK",
    "about 20 cm LEFT and 15 cm UP",
    "about 20 cm RIGHT and 15 cm UP",
    "about 20 cm LEFT and 20 cm FORWARD",
    "about 20 cm RIGHT and 20 cm BACK",
};

std::string timestamp() {
  char buf[32];
  time_t t = time(nullptr);
  strftime(buf, sizeof buf, "%Y%m%d-%H%M%S", localtime(&t));
  return buf;
}

MicCal load_cal_or_die(Config& cfg) {
  if (cfg.mic_cal.empty() || !fs::exists(cfg.mic_cal)) {
    auto found = find_mic_cal_file();
    if (!found) throw std::runtime_error("no UMIK-1 calibration file; pass --mic-cal FILE");
    cfg.mic_cal = *found;
  }
  // Keep a private copy so a cleaned-out Downloads folder doesn't break us.
  std::string dest = config_dir() + "/" + fs::path(cfg.mic_cal).filename().string();
  if (fs::absolute(cfg.mic_cal) != fs::absolute(dest)) {
    ensure_dir(config_dir());
    fs::copy_file(cfg.mic_cal, dest, fs::copy_options::overwrite_existing);
    cfg.mic_cal = dest;
  }
  return load_mic_cal(cfg.mic_cal);
}

void print_design(const DesignResult& r) {
  for (const auto& n : r.notes) printf("    %s\n", n.c_str());
  for (const auto& w : r.warnings) printf("    %s %s\n", yellow("!").c_str(), w.c_str());
}

}  // namespace

// ------------------------------------------------------------------ calibrate

int run_calibrate(int argc, char** argv) {
  Config cfg = load_config();
  int positions = cfg.positions;
  for (int i = 0; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--positions" && i + 1 < argc) positions = std::clamp(atoi(argv[++i]), 1, 9);
    else if (a == "--mic-cal" && i + 1 < argc) cfg.mic_cal = argv[++i];
    else if (a == "--yes" || a == "-y") g_auto = true;
  }

  setvbuf(stdout, nullptr, _IOLBF, 0);
  printf("%s\n", bold("Room Correction — calibration").c_str());
  printf("%s\n", dim("Measures your speakers and sub with the UMIK-1, then builds correction filters.").c_str());

  step(1, "Devices");
  if (cfg.output_device.empty() || !sink_exists(cfg.output_device)) {
    printf("  %s\n", red("The X4 5.1 output isn't configured. Run `roomcorr setup` first.").c_str());
    return 1;
  }
  auto mic = find_mic(cfg);
  if (!mic) {
    printf("  %s\n", red("No UMIK-1 found. Plug it in and try again.").c_str());
    return 1;
  }
  MicCal cal = load_cal_or_die(cfg);
  printf("  Output      %s\n", cfg.output_device.c_str());
  printf("  Microphone  %s\n", mic->c_str());
  printf("  Mic cal     %s  (serial %s, sensitivity %+.3f dB, %zu points)\n", fs::path(cal.path).filename().c_str(),
         cal.serial.c_str(), cal.sens_db, cal.f.size());
  run_cmd("pactl set-source-volume " + shell_quote(*mic) + " 100% >/dev/null 2>&1");
  run_cmd("pactl set-source-mute " + shell_quote(*mic) + " 0 >/dev/null 2>&1");

  signal(SIGINT, on_interrupt);
  signal(SIGTERM, on_interrupt);
  Json mute = Json::object();
  mute["mute"] = true;
  if (daemon_set(mute)) {
    g_daemon_muted = true;
    printf("  %s\n", dim("Room Correction output muted while measuring.").c_str());
  }

  step(2, "Before we start");
  printf("  • Set the %s volume to where you normally listen (fairly loud).\n", bold("Marantz").c_str());
  printf("    %s the calibration assumes it stays there. Use the Room Correction volume from now on.\n",
         yellow("Don't touch it afterwards:").c_str());
  printf("  • On the %s: volume knob about halfway, crossover/low-pass knob to its %s (or LFE), phase 0°.\n",
         bold("Polk sub").c_str(), bold("maximum").c_str());
  printf("  • Stop other audio. Keep the room quiet during the sweeps.\n");
  printf("  • Put the mic at the %s, ear height, pointing %s at the ceiling (90° calibration).\n",
         bold("main listening position").c_str(), bold("straight up").c_str());
  press_enter();

  const std::string& out = cfg.output_device;
  step(3, "Room noise");
  double floor_main = silence_level(out, *mic, 200, 4000), floor_sub = silence_level(out, *mic, 30, 90);
  printf("  Noise floor ≈ %.0f dB SPL (mid band), %.0f dB SPL (sub band)\n", cal.spl(floor_main), cal.spl(floor_sub));

  step(4, "Find the subwoofer");
  std::vector<std::string> sub_outs;
  // Subs with auto-standby (the Polk has one) need a few seconds of bass
  // before they switch on; short test bursts alone go unheard.
  printf("    %s\n", dim("waking the sub from standby...").c_str());
  noise_burst(out, *mic, {"FC", "LFE"}, -30, 30, 90, 4.0);
  for (double level : {-35.0, -25.0}) {
    for (const char* p : {"FC", "LFE"}) {
      Burst b = noise_burst(out, *mic, {p}, level, 30, 90);
      bool hit = b.mic_dbfs - floor_sub > 10;
      printf("    C/Sub jack, %s wire (%s): %s\n", std::string(p) == "FC" ? "tip" : "ring", p,
             hit ? green("sub heard (" + f1("%+.0f", b.mic_dbfs - floor_sub) + " dB)").c_str()
                 : dim("nothing (" + f1("%+.0f", b.mic_dbfs - floor_sub) + " dB)").c_str());
      if (hit) sub_outs.push_back(p);
    }
    if (!sub_outs.empty()) break;
    printf("    %s\n", dim("trying louder...").c_str());
  }
  if (sub_outs.empty()) {
    printf("  %s\n", red("The sub wasn't detected. Check it's powered on, its volume is up and the cable is in the C/Sub jack.").c_str());
    restore();
    return 1;
  }
  cfg.sub_outputs = sub_outs;

  step(5, "Levels");
  printf("  Mains (left):\n");
  double main_spl = 0, sub_spl = 0;
  double main_level = auto_level(out, *mic, cal, {"FL"}, 200, 4000, 75, floor_main, &main_spl);
  printf("  Subwoofer:\n");
  double sub_level = auto_level(out, *mic, cal, sub_outs, 30, 90, 75, floor_sub, &sub_spl);
  // Sensitivity difference: how much louder the sub plays than a main
  // speaker for the same signal.
  while (true) {
    double diff = (sub_spl - sub_level) - (main_spl - main_level);
    printf("  Sub vs main speaker at the same signal level: %s dB\n", f1("%+.1f", diff).c_str());
    if (diff >= -3 && diff <= 9) {
      printf("  %s\n", green("Sub volume knob is in a good range.").c_str());
      break;
    }
    printf("  %s\n", yellow(diff < -3 ? "The sub is quiet: turn its volume knob UP a little."
                                      : "The sub is very loud: turn its volume knob DOWN a little.")
                         .c_str());
    if (g_auto) break;
    std::string a = ask(dim("› Adjust the knob, then Enter to re-check (or 's' to skip):"));
    if (!a.empty() && (a[0] == 's' || a[0] == 'S')) break;
    Burst b = noise_burst(out, *mic, sub_outs, sub_level, 30, 90);
    sub_spl = cal.spl(b.mic_dbfs);
  }
  const double main_amp = std::min(0.5, std::pow(10.0, main_level / 20));
  const double sub_amp = std::min(0.5, std::pow(10.0, sub_level / 20));

  step(6, "Measure");
  printf("  %d positions, about 25 s each. Sweeps play left, right, then sub.\n", positions);
  printf("  %s\n", dim("More positions = a correction that works for more than one head position.").c_str());
  MeasurementSet set;
  set.dir = data_dir() + "/measurements/" + timestamp();
  Sequence seq = build_sequence(main_amp, sub_amp, sub_outs);
  for (int p = 0; p < positions;) {
    printf("\n  %s %s\n", bold("Position " + std::to_string(p + 1) + "/" + std::to_string(positions) + ":").c_str(),
           kPositionHints[p]);
    press_enter("Place the mic (pointing up), then press Enter; stay quiet");
    MeasureRequest req;
    req.play_target = out;
    req.positions = kX4Positions;
    req.signal = seq.signal;
    req.capture_target = *mic;
    req.tail_seconds = 0.5;
    printf("    measuring...");
    fflush(stdout);
    MeasureResult res;
    try {
      res = run_measurement(req);
    } catch (const std::exception& e) {
      printf("\r    %s\n", red(e.what()).c_str());
      if (yes("    Retry?", true, 0)) continue;
      restore();
      return 1;
    }
    std::vector<std::vector<double>> irs;
    std::string line;
    bool bad = res.capture_peak_dbfs > -1;
    for (int c = 0; c < kNumChans; ++c) {
      size_t a = seq.offset[c], b = std::min(res.recording.size(), a + seq.seg_len[c]);
      std::vector<double> seg(res.recording.begin() + long(a), res.recording.begin() + long(b));
      irs.push_back(deconvolve(seg, seq.sweep[c], kIrLength));
      double snr = ir_snr_db(irs.back());
      bool ok = snr > 35;
      bad = bad || snr < 25;
      line += std::string(kChanNames[c]) + " " + (ok ? green(f1("%.0f dB", snr)) : yellow(f1("%.0f dB", snr))) + "   ";
    }
    printf("\r    SNR  %s\n", line.c_str());
    if (res.capture_peak_dbfs > -1) printf("    %s\n", yellow("The microphone clipped.").c_str());
    if (bad) {
      printf("    %s\n", yellow("This measurement looks unreliable (noise or clipping).").c_str());
      if (yes("    Measure this position again?", true, 0)) continue;
    }
    set.irs.push_back(std::move(irs));
    ++p;
  }

  set.meta = Json::object();
  set.meta["created"] = timestamp();
  set.meta["mic_cal"] = cfg.mic_cal;
  set.meta["mic"] = *mic;
  set.meta["output"] = out;
  set.meta["sub_outputs"] = Json(sub_outs);
  set.meta["main_level_dbfs"] = main_level;
  set.meta["sub_level_dbfs"] = sub_level;
  set.meta["main_sweep_amp"] = main_amp;
  set.meta["noise_floor_spl"] = cal.spl(floor_main);
  save_measurements(set);
  printf("\n  Saved measurements to %s\n", dim(set.dir).c_str());

  step(7, "Design filters");
  cfg.positions = positions;
  DesignResult r = design_filters(cfg, set, cal);
  install_design(r);
  print_design(r);
  save_config(r.cfg);
  if (daemon_reload())
    printf("  %s\n", green("Filters loaded into the running engine.").c_str());
  else
    printf("  %s\n", yellow("The roomcorr daemon isn't running; start it with `systemctl --user start roomcorr`.").c_str());
  restore();

  printf("\n  Verification plays sweeps through the corrected system to confirm the result.\n");
  if (yes("  Run verification now (mic at the main position)?")) {
    run_verify(0, nullptr);
  }
  printf("\n%s\n", green(bold("Done. Enjoy the music.")).c_str());
  return 0;
}

// ------------------------------------------------------------------ probe

// roomcorr probe OUT[,OUT...] [LEVEL_DBFS] [F_LO F_HI]
// Plays a pink-noise burst on the given X4 outputs and reports what the mic
// hears, relative to silence. For checking wiring and levels.
int run_probe(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IOLBF, 0);
  Config cfg = load_config();
  if (argc < 1) {
    fprintf(stderr, "usage: roomcorr probe FL|FR|FC|LFE|RL|RR[,...] [level_dbfs=-40] [f_lo f_hi]\n");
    return 2;
  }
  std::vector<std::string> outs;
  std::string list = argv[0];
  for (size_t a = 0; a <= list.size();) {
    size_t b = list.find(',', a);
    if (b == std::string::npos) b = list.size();
    if (b > a) outs.push_back(list.substr(a, b - a));
    a = b + 1;
  }
  double level = argc > 1 ? atof(argv[1]) : -40;
  double lo = argc > 3 ? atof(argv[2]) : 100, hi = argc > 3 ? atof(argv[3]) : 10000;
  if (level > -10) {
    fprintf(stderr, "refusing levels above -10 dBFS\n");
    return 2;
  }
  auto mic = find_mic(cfg);
  if (!mic || cfg.output_device.empty()) {
    fprintf(stderr, "needs the mic and `roomcorr setup`\n");
    return 1;
  }
  MicCal cal = load_mic_cal(cfg.mic_cal);
  double floor = silence_level(cfg.output_device, *mic, lo, hi);
  Burst b = noise_burst(cfg.output_device, *mic, outs, level, lo, hi);
  printf("%s at %.0f dBFS, %.0f-%.0f Hz: mic %.1f dBFS (≈%.0f dB SPL), %+.1f dB over silence, capture peak %.1f dBFS\n",
         list.c_str(), level, lo, hi, b.mic_dbfs, cal.spl(b.mic_dbfs), b.mic_dbfs - floor, b.capture_peak);
  return 0;
}

// ------------------------------------------------------------------ design

int run_design(int argc, char** argv) {
  Config cfg = load_config();
  std::string dir = cfg.measurement;
  for (int i = 0; i < argc; ++i)
    if (std::string(argv[i]) == "--measurement" && i + 1 < argc) dir = argv[++i];
  if (dir.empty()) {
    fprintf(stderr, "no measurement set; run `roomcorr calibrate` first\n");
    return 1;
  }
  MeasurementSet set = load_measurements(dir);
  MicCal cal = load_mic_cal(cfg.mic_cal);
  DesignResult r = design_filters(cfg, set, cal);
  install_design(r);
  print_design(r);
  save_config(r.cfg);
  daemon_reload();
  return 0;
}

// ------------------------------------------------------------------ verify

int run_verify(int, char**) {
  Config cfg = load_config();
  auto mic = find_mic(cfg);
  if (!mic) {
    printf("%s\n", red("No UMIK-1 found.").c_str());
    return 1;
  }
  Json state;
  try {
    Json r = Json::object();
    r["cmd"] = "get";
    state = control_request(r);
  } catch (const std::exception& e) {
    printf("%s\n", red(std::string("verify needs the running daemon: ") + e.what()).c_str());
    return 1;
  }
  const std::string sink = state.get("sink").as_str("roomcorr_sink");
  MicCal cal = load_mic_cal(cfg.mic_cal);
  double amp = 0.1;
  try {
    amp = load_measurements(cfg.measurement).meta.get("main_sweep_amp").as_num(0.1);
  } catch (...) {
  }

  printf("%s\n", bold("Verifying the corrected system").c_str());
  signal(SIGINT, on_interrupt);
  Json v = Json::object();
  v["mute"] = false;
  v["enabled"] = true;
  v["room_eq"] = true;
  daemon_set(v);
  // Sweep through our sink at 100% so it plays at the calibration level.
  g_restore_sink = sink;
  g_restore_volume = sink_volume(sink);
  set_sink_volume(sink, 1.0);

  Sweep sw = make_sweep(15, 22000, 6.0, kSampleRate);
  for (auto& x : sw.signal) x *= amp;
  const size_t seg = sw.signal.size() + size_t(2.2 * kSampleRate), pre = size_t(0.3 * kSampleRate);
  // left only, right only, both (mono)
  const int kSegs = 3;
  std::vector<std::vector<float>> sig(2, std::vector<float>(pre + kSegs * seg, 0.f));
  for (size_t i = 0; i < sw.signal.size(); ++i) {
    sig[0][pre + i] = float(sw.signal[i]);
    sig[1][pre + seg + i] = float(sw.signal[i]);
    sig[0][pre + 2 * seg + i] = sig[1][pre + 2 * seg + i] = float(sw.signal[i]);
  }
  MeasureRequest req;
  req.play_target = sink;
  req.positions = {"FL", "FR"};
  req.signal = sig;
  req.capture_target = *mic;
  req.tail_seconds = 0.5;
  printf("  measuring (~26 s)...\n");
  MeasureResult res;
  try {
    res = run_measurement(req);
  } catch (const std::exception& e) {
    restore();
    printf("%s\n", red(e.what()).c_str());
    return 1;
  }
  restore();

  Json report;
  try {
    report = Json::parse(read_file(response_path()));
  } catch (...) {
    report = Json::object();
  }
  auto disp = log_grid(15, 20000, 12);
  auto grid = log_grid(10, 24000, 48);
  std::vector<std::vector<double>> curves;
  for (int s = 0; s < kSegs; ++s) {
    size_t a = pre + size_t(s) * seg;
    std::vector<double> part(res.recording.begin() + long(a),
                             res.recording.begin() + long(std::min(res.recording.size(), a + seg)));
    auto ir = deconvolve(part, sw, kIrLength);
    double t = arrival_time(ir, 200, 5000, kSampleRate);
    auto pw = fdw_power(ir, t, kSampleRate, kIrLength, 3, 15, 5, 500);
    auto db = smooth_to_grid(pw, double(kSampleRate) / kIrLength, grid);
    std::vector<double> d;
    for (double f : disp) d.push_back(interp_log(grid, db, f) - cal.at(f) - (s == 2 ? 6.02 : 0));
    curves.push_back(d);
  }
  // Put the curves on the report's scale: match the target at 300 Hz-3 kHz.
  auto tf = report.get("freqs").num_vector(), tv = report.get("target").num_vector();
  double off = 0;
  if (!tf.empty()) {
    double sum = 0;
    int n = 0;
    for (size_t i = 0; i < disp.size(); ++i)
      if (disp[i] >= 300 && disp[i] <= 3000) {
        sum += interp_log(tf, tv, disp[i]) - (curves[0][i] + curves[1][i]) / 2;
        ++n;
      }
    off = n ? sum / n : 0;
  } else {
    off = 75 - curves[0][disp.size() / 2];
  }
  Json vj = Json::object();
  vj["created"] = int64_t(time(nullptr));
  vj["freqs"] = Json(disp);
  const char* names[] = {"left", "right", "both"};
  for (int s = 0; s < kSegs; ++s) {
    Json a = Json::array();
    for (double x : curves[size_t(s)]) a.push(std::round((x + off) * 100) / 100);
    vj[names[s]] = a;
  }
  report["verified"] = vj;
  write_file_atomic(response_path(), report.dump() + "\n");

  // Deviation from target, bass and overall, for the terminal.
  if (!tf.empty()) {
    auto dev = [&](int s, double lo, double hi) {
      double sq = 0;
      int n = 0;
      for (size_t i = 0; i < disp.size(); ++i)
        if (disp[i] >= lo && disp[i] <= hi) {
          sq += std::pow(curves[size_t(s)][i] + off - interp_log(tf, tv, disp[i]), 2);
          ++n;
        }
      return n ? std::sqrt(sq / n) : 0;
    };
    printf("  Deviation from target (RMS):\n");
    printf("    both speakers + sub   20-200 Hz %s dB   200 Hz-10 kHz %s dB\n", f1("%.1f", dev(2, 20, 200)).c_str(),
           f1("%.1f", dev(2, 200, 10000)).c_str());
    printf("    left                  20-200 Hz %s dB   200 Hz-10 kHz %s dB\n", f1("%.1f", dev(0, 20, 200)).c_str(),
           f1("%.1f", dev(0, 200, 10000)).c_str());
    printf("    right                 20-200 Hz %s dB   200 Hz-10 kHz %s dB\n", f1("%.1f", dev(1, 20, 200)).c_str(),
           f1("%.1f", dev(1, 200, 10000)).c_str());
  }
  printf("  %s\n", green("Saved; the Room Correction panel shows the measured curves.").c_str());
  return 0;
}

// ------------------------------------------------------------------ setup

int run_setup(int, char**) {
  Config cfg = load_config();
  printf("%s\n", bold("Room Correction — setup").c_str());
  auto card = find_x4_card();
  if (!card) {
    printf("%s\n", red("Sound Blaster X4 not found.").c_str());
    return 1;
  }
  const std::string suffix = card->substr(std::string("alsa_card.").size());
  const std::string stereo_sink = "alsa_output." + suffix + ".analog-stereo";
  const std::string sink51 = "alsa_output." + suffix + ".analog-surround-51";
  double prev_volume = sink_volume(stereo_sink);

  printf("  Switching the X4 to its 5.1 profile (C/Sub jack becomes active)...\n");
  run_cmd("pactl set-card-profile " + shell_quote(*card) + " output:analog-surround-51+input:analog-stereo");
  for (int i = 0; i < 50 && !sink_exists(sink51); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
  if (!sink_exists(sink51)) {
    printf("%s\n", red("The 5.1 output did not appear.").c_str());
    return 1;
  }
  // Keep the hardware level where it was: if the engine ever stops, audio
  // falls back to this sink and must not suddenly be louder.
  if (prev_volume > 0) set_sink_volume(sink51, prev_volume);
  cfg.output_device = sink51;
  printf("  Output: %s\n", sink51.c_str());

  if (auto calfile = find_mic_cal_file()) {
    cfg.mic_cal = *calfile;
    try {
      load_cal_or_die(cfg);
      printf("  Mic calibration: %s\n", cfg.mic_cal.c_str());
    } catch (const std::exception& e) {
      printf("  %s\n", yellow(e.what()).c_str());
    }
  }
  save_config(cfg);

  if (daemon_reload()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    // The X4 keeps its old hardware level, so the engine starts at 100%.
    if (prev_volume > 0) set_sink_volume("roomcorr_sink", 1.0);
    run_cmd("pactl set-default-sink roomcorr_sink");
    printf("  %s\n", green("Room Correction is now the default output.").c_str());
  } else {
    printf("  %s\n", yellow("Start the engine (systemctl --user enable --now roomcorr) and run setup again.").c_str());
  }
  return 0;
}

}  // namespace rc
