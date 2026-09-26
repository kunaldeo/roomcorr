// PipeWire front end: a stereo virtual sink ("Room Correction") whose audio
// runs through the Engine and out to the Sound Blaster X4's 5.1 sink.
//
// Two streams in one node group, the same pattern as PipeWire's own
// loopback/filter-chain modules: the capture side (our sink) triggers the
// playback side, whose process callback moves one quantum through the DSP.
#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <csignal>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <memory>

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/result.h>

#include "config.hpp"
#include "control.hpp"
#include "engine.hpp"
#include "spectrum.hpp"
#include "wav.hpp"

namespace rc {

namespace {

constexpr uint32_t kMaxFrames = 8192;
// ROOMCORR_INSTANCE gives test daemons their own node names so they never
// collide with (or play through) the real one.
std::string instance_suffix() {
  const char* s = getenv("ROOMCORR_INSTANCE");
  return s && *s ? std::string("_") + s : std::string();
}
const std::string kSinkNameStr = "roomcorr_sink" + instance_suffix();
const std::string kOutputNameStr = "roomcorr_output" + instance_suffix();
const char* const kSinkName = kSinkNameStr.c_str();
const char* const kOutputName = kOutputNameStr.c_str();

int64_t now_ms() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return int64_t(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

double lin_to_db(double v) { return v > 1e-6 ? 20 * std::log10(v) : -120.0; }

// Creates the shared-audio object that other programs read (see
// include/roomcorr/shared_audio.h). Returns null if shared memory isn't
// available; the engine then simply doesn't publish.
rc_shared_audio_header* create_shared_audio(const std::string& name, size_t& bytes) {
  constexpr uint32_t kCapacity = 1u << 16;  // 1.37 s at 48 kHz
  constexpr uint32_t kHeader = 256;
  static_assert(sizeof(rc_shared_audio_header) <= kHeader, "header grew");
  bytes = kHeader + size_t(RC_AUDIO_CHANNELS) * kCapacity * sizeof(float);
  shm_unlink(name.c_str());  // a stale object from a crashed run
  int fd = shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0644);
  if (fd < 0) return nullptr;
  if (ftruncate(fd, off_t(bytes)) != 0) {
    close(fd);
    shm_unlink(name.c_str());
    return nullptr;
  }
  void* mem = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  if (mem == MAP_FAILED) {
    shm_unlink(name.c_str());
    return nullptr;
  }
  std::memset(mem, 0, bytes);
  auto* h = static_cast<rc_shared_audio_header*>(mem);
  std::memcpy(h->magic, RC_SHARED_AUDIO_MAGIC, sizeof(h->magic));
  h->version = RC_SHARED_AUDIO_VERSION;
  h->header_size = kHeader;
  h->sample_rate = kSampleRate;
  h->channels = RC_AUDIO_CHANNELS;
  h->capacity = kCapacity;
  const char* names[RC_AUDIO_CHANNELS] = {"in_l", "in_r", "out_l", "out_r", "out_sub"};
  for (int c = 0; c < RC_AUDIO_CHANNELS; ++c) std::strncpy(h->channel_names[c], names[c], 7);
  return h;
}

class Daemon {
public:
  int run();

private:
  static void on_capture_process(void* data);
  static void on_playback_process(void* data);
  static void on_capture_state(void* data, pw_stream_state old, pw_stream_state state, const char* error);
  static void on_playback_state(void* data, pw_stream_state old, pw_stream_state state, const char* error);
  static void on_core_error(void* data, uint32_t id, int seq, int res, const char* message);
  static void on_timer(void* data, uint64_t expirations);
  static void on_signal(void* data, int signal_number);

  bool create_capture();
  bool create_playback();
  void apply_config(const Config& old, bool force_filters);
  void load_filters();
  Json handle(int client, const Json& req);
  Json state_json();
  Json status_json(bool spectrum);

  pw_main_loop* main_loop_ = nullptr;
  pw_context* context_ = nullptr;
  pw_core* core_ = nullptr;
  spa_hook core_listener_{};
  pw_stream* capture_ = nullptr;
  pw_stream* playback_ = nullptr;
  spa_hook capture_listener_{}, playback_listener_{};
  spa_source* timer_ = nullptr;

  Engine engine_;
  SpectrumAnalyzer analyzer_;
  rc_shared_audio_header* shared_ = nullptr;
  size_t shared_bytes_ = 0;
  std::string shared_name_;
  Config cfg_;
  ControlServer control_;
  std::string capture_state_ = "unconnected", playback_state_ = "unconnected";
  std::string filter_status_ = "none";
  int64_t save_due_ = 0;
  int64_t quiet_until_ = 0;  // measurement mute lease (not saved)
  void push_params();
  int exit_code_ = 0;

  float tmp_in_[kNumOut][kMaxFrames] = {};
};

// ------------------------------------------------------------------ realtime

void Daemon::on_capture_process(void* data) {
  auto* self = static_cast<Daemon*>(data);
  if (pw_stream_trigger_process(self->playback_) < 0) {
    // Playback isn't ready: recycle input so the sink keeps running.
    pw_buffer* b;
    while ((b = pw_stream_dequeue_buffer(self->capture_))) pw_stream_queue_buffer(self->capture_, b);
  }
}

void Daemon::on_playback_process(void* data) {
  auto* self = static_cast<Daemon*>(data);
  // Take the newest input buffer and recycle any older ones.
  pw_buffer* in = nullptr;
  pw_buffer* t;
  while ((t = pw_stream_dequeue_buffer(self->capture_))) {
    if (in) pw_stream_queue_buffer(self->capture_, in);
    in = t;
  }
  pw_buffer* out = pw_stream_dequeue_buffer(self->playback_);
  if (!out) {
    if (in) pw_stream_queue_buffer(self->capture_, in);
    return;
  }

  spa_buffer* ob = out->buffer;
  uint32_t n = kMaxFrames;
  for (uint32_t i = 0; i < ob->n_datas; ++i) n = std::min(n, ob->datas[i].maxsize / uint32_t(sizeof(float)));
  if (out->requested > 0) n = std::min<uint32_t>(n, uint32_t(out->requested));

  const float* ip[kNumOut];
  for (int c = 0; c < kNumOut; ++c) {
    uint32_t have = 0;
    if (in && uint32_t(c) < in->buffer->n_datas && in->buffer->datas[c].data) {
      spa_data& d = in->buffer->datas[c];
      uint32_t off = std::min(d.chunk->offset, d.maxsize);
      have = std::min(n, std::min(d.chunk->size, d.maxsize - off) / uint32_t(sizeof(float)));
      memcpy(self->tmp_in_[c], SPA_PTROFF(d.data, off, float), have * sizeof(float));
    }
    if (have < n) memset(self->tmp_in_[c] + have, 0, (n - have) * sizeof(float));
    ip[c] = self->tmp_in_[c];
  }
  float* op[kNumOut] = {};
  for (uint32_t i = 0; i < ob->n_datas && i < uint32_t(kNumOut); ++i) op[i] = static_cast<float*>(ob->datas[i].data);

  self->engine_.process(ip, op, int(n));
  for (uint32_t i = 0; i < ob->n_datas; ++i) {
    ob->datas[i].chunk->offset = 0;
    ob->datas[i].chunk->stride = sizeof(float);
    ob->datas[i].chunk->size = n * sizeof(float);
  }
  pw_stream_queue_buffer(self->playback_, out);
  if (in) pw_stream_queue_buffer(self->capture_, in);
}

// ------------------------------------------------------------------ streams

static const spa_pod* audio_format(spa_pod_builder* b, uint32_t channels, const uint32_t* positions) {
  spa_audio_info_raw info{};
  info.format = SPA_AUDIO_FORMAT_F32P;
  info.rate = kSampleRate;
  info.channels = channels;
  for (uint32_t i = 0; i < channels; ++i) info.position[i] = positions[i];
  return spa_format_audio_raw_build(b, SPA_PARAM_EnumFormat, &info);
}

void Daemon::on_capture_state(void* data, pw_stream_state, pw_stream_state state, const char* error) {
  auto* self = static_cast<Daemon*>(data);
  self->capture_state_ = pw_stream_state_as_string(state);
  if (error) fprintf(stderr, "roomcorr: sink stream: %s\n", error);
}

void Daemon::on_playback_state(void* data, pw_stream_state, pw_stream_state state, const char* error) {
  auto* self = static_cast<Daemon*>(data);
  self->playback_state_ = pw_stream_state_as_string(state);
  if (error) fprintf(stderr, "roomcorr: output stream: %s\n", error);
}

bool Daemon::create_capture() {
  pw_properties* props = pw_properties_new(
      PW_KEY_MEDIA_CLASS, "Audio/Sink", PW_KEY_NODE_NAME, kSinkName, PW_KEY_NODE_DESCRIPTION, "Room Correction",
      PW_KEY_NODE_GROUP, "roomcorr", "node.link-group", "roomcorr", SPA_KEY_AUDIO_POSITION, "[ FL, FR, FC, LFE, RL, RR ]", PW_KEY_NODE_VIRTUAL, "true",
      // As in PipeWire's loopback: both halves run in the same graph cycle,
      // so the adapters must not rate-match (it drops samples every cycle).
      "resample.disable", "true", "resample.prefill", "true",
      "device.icon-name", "audio-speakers", nullptr);
  capture_ = pw_stream_new(core_, "roomcorr sink", props);
  if (!capture_) return false;

  static pw_stream_events events{};
  events.version = PW_VERSION_STREAM_EVENTS;
  events.process = on_capture_process;
  events.state_changed = on_capture_state;
  pw_stream_add_listener(capture_, &capture_listener_, &events, this);

  uint8_t buf[1024];
  spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
  // 5.1 input so films and games keep their LFE channel; stereo streams
  // link to FL/FR only.
  const uint32_t pos[6] = {SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR, SPA_AUDIO_CHANNEL_FC,
                           SPA_AUDIO_CHANNEL_LFE, SPA_AUDIO_CHANNEL_RL, SPA_AUDIO_CHANNEL_RR};
  const spa_pod* params[1] = {audio_format(&b, 6, pos)};
  // ASYNC: our process() only triggers the playback side, which dequeues
  // this stream's buffers from its own callback. Without the flag PipeWire
  // assumes a buffer is consumed inside process() and the pair runs only
  // every other graph cycle, dropping half the audio.
  return pw_stream_connect(capture_, PW_DIRECTION_INPUT, PW_ID_ANY,
                           pw_stream_flags(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
                                           PW_STREAM_FLAG_RT_PROCESS | PW_STREAM_FLAG_ASYNC),
                           params, 1) == 0;
}

bool Daemon::create_playback() {
  pw_properties* props = pw_properties_new(
      PW_KEY_NODE_NAME, kOutputName, PW_KEY_NODE_DESCRIPTION, "Room Correction Output", PW_KEY_MEDIA_TYPE, "Audio",
      PW_KEY_MEDIA_CATEGORY, "Playback", PW_KEY_NODE_GROUP, "roomcorr", "node.link-group", "roomcorr",
      SPA_KEY_AUDIO_POSITION, "[ FL, FR, FC, LFE, RL, RR ]", PW_KEY_STREAM_DONT_REMIX, "true",
      PW_KEY_NODE_VIRTUAL, "true", "resample.disable", "true", "resample.prefill", "true",
      // Only ever play into the X4. Falling back to the default sink would
      // be our own sink: a feedback loop.
      "node.dont-fallback", "true", nullptr);
  if (!cfg_.output_device.empty()) pw_properties_set(props, PW_KEY_TARGET_OBJECT, cfg_.output_device.c_str());
  playback_ = pw_stream_new(core_, "roomcorr output", props);
  if (!playback_) return false;

  static pw_stream_events events{};
  events.version = PW_VERSION_STREAM_EVENTS;
  events.process = on_playback_process;
  events.state_changed = on_playback_state;
  pw_stream_add_listener(playback_, &playback_listener_, &events, this);

  uint8_t buf[1024];
  spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
  const uint32_t pos[6] = {SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR, SPA_AUDIO_CHANNEL_FC,
                           SPA_AUDIO_CHANNEL_LFE, SPA_AUDIO_CHANNEL_RL, SPA_AUDIO_CHANNEL_RR};
  const spa_pod* params[1] = {audio_format(&b, 6, pos)};
  return pw_stream_connect(playback_, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                           pw_stream_flags(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
                                           PW_STREAM_FLAG_RT_PROCESS | PW_STREAM_FLAG_TRIGGER),
                           params, 1) == 0;
}

// ------------------------------------------------------------------ config

void Daemon::load_filters() {
  int loaded = 0;
  std::string errors;
  for (int c = 0; c < kNumChans; ++c) {
    std::vector<float> ir;
    if (!cfg_.ch[c].filter.empty()) {
      try {
        ir = read_wav_mono(cfg_.ch[c].filter, kSampleRate);
        if (int(ir.size()) > Engine::kMaxTaps) ir.resize(Engine::kMaxTaps);
        ++loaded;
      } catch (const std::exception& e) {
        errors += std::string(errors.empty() ? "" : "; ") + e.what();
        ir.clear();
      }
    }
    engine_.set_filter(c, ir);
  }
  filter_status_ = !errors.empty() ? "error: " + errors : loaded == kNumChans ? "loaded" : loaded ? "partial" : "none";
  fprintf(stderr, "roomcorr: filters %s\n", filter_status_.c_str());
}

void Daemon::push_params() {
  EngineParams p = EngineParams::from_config(cfg_);
  if (quiet_until_ > now_ms()) p.mute = true;
  engine_.set_params(p);
}

void Daemon::apply_config(const Config& old, bool force_filters) {
  push_params();
  bool filters_changed = force_filters;
  for (int c = 0; c < kNumChans; ++c) filters_changed = filters_changed || old.ch[c].filter != cfg_.ch[c].filter;
  if (filters_changed) load_filters();
  engine_.set_eq_filters_enabled(cfg_.room_eq);

  if (old.output_device != cfg_.output_device && playback_) {
    // Retarget the output: reconnecting with the new target.object.
    pw_stream_disconnect(playback_);
    pw_stream_destroy(playback_);
    playback_ = nullptr;
    create_playback();
  }
}

Json Daemon::state_json() {
  Json j = Json::object();
  j["type"] = "state";
  j["config"] = cfg_.to_json();
  j["preamp_db"] = cfg_.auto_preamp_db();
  j["filters"] = filter_status_;
  j["quiet"] = quiet_until_ > now_ms();
  j["latency_ms"] = 1000.0 * Engine::kBlock / kSampleRate;
  j["sink"] = kSinkName;
  j["spectrum_freqs"] = Json(analyzer_.freqs());
  return j;
}

Json Daemon::status_json(bool spectrum) {
  EngineStats s = engine_.read_stats();
  auto meter = [](const Meter& m) {
    Json j = Json::object();
    j["peak"] = std::round(lin_to_db(m.peak) * 10) / 10;
    j["rms"] = std::round(lin_to_db(m.rms) * 10) / 10;
    return j;
  };
  Json j = Json::object();
  j["type"] = "status";
  j["in"] = Json(Json::Array{meter(s.in[0]), meter(s.in[1])});
  j["out"] = Json(Json::Array{meter(s.out[kLeft]), meter(s.out[kRight]), meter(s.out[kSub])});
  j["limiter_db"] = std::round(s.limiter_gr_db * 10) / 10;
  j["limited"] = s.clipped;
  j["load"] = std::round(s.load * 1000) / 1000;
  j["tail_misses"] = s.tail_misses;
  j["idle_channels"] = s.idle_channels;
  j["sink_state"] = capture_state_;
  j["output_state"] = playback_state_;
  if (spectrum) {
    Json sp = Json::object();
    sp["in"] = Json(analyzer_.analyze(engine_, Engine::kTapIn));
    sp["left"] = Json(analyzer_.analyze(engine_, Engine::kTapLeft));
    sp["right"] = Json(analyzer_.analyze(engine_, Engine::kTapRight));
    sp["sub"] = Json(analyzer_.analyze(engine_, Engine::kTapSub));
    j["spectrum"] = sp;
  }
  return j;
}

Json Daemon::handle(int client, const Json& req) {
  std::string cmd = req.get("cmd").as_str();
  Json ok = Json::object();
  ok["type"] = "ok";
  if (cmd == "ping") return ok;
  if (cmd == "get") return state_json();
  if (cmd == "status") return status_json(req.get("spectrum").as_bool(false));
  if (cmd == "subscribe") {
    control_.subscribe(client, req.get("interval_ms").as_int(100), req.get("spectrum").as_bool(false));
    return state_json();
  }
  if (cmd == "set") {
    Config old = cfg_;
    Json unknown = Json::array();
    for (const auto& [k, v] : req.get("values").obj())
      if (!cfg_.set(k, v)) unknown.push(k);
    apply_config(old, false);
    save_due_ = now_ms() + 1000;
    control_.broadcast_subscribers(state_json());
    if (unknown.size()) {
      Json e = Json::object();
      e["type"] = "error";
      e["error"] = "unknown keys";
      e["keys"] = unknown;
      return e;
    }
    return state_json();
  }
  if (cmd == "save") {
    save_due_ = 0;
    save_config(cfg_);
    return state_json();
  }
  if (cmd == "quiet") {
    // Temporary mute for measurements. It expires on its own, so a
    // calibration that crashes can't leave the speakers silent.
    int seconds = std::clamp(req.get("seconds").as_int(0), 0, 600);
    quiet_until_ = seconds ? now_ms() + seconds * 1000 : 0;
    push_params();
    Json r = state_json();
    r["quiet"] = seconds > 0;
    return r;
  }
  if (cmd == "reload") {
    Config old = cfg_;
    cfg_ = load_config();
    apply_config(old, true);
    control_.broadcast_subscribers(state_json());
    return state_json();
  }
  Json e = Json::object();
  e["type"] = "error";
  e["error"] = "unknown command: " + cmd;
  return e;
}

// ------------------------------------------------------------------ loop

void Daemon::on_timer(void* data, uint64_t) {
  auto* self = static_cast<Daemon*>(data);
  int64_t now = now_ms();
  self->control_.tick(now, [self](bool spectrum) { return self->status_json(spectrum); });
  self->engine_.collect_garbage();
  if (self->quiet_until_ && now >= self->quiet_until_) {
    self->quiet_until_ = 0;
    self->push_params();
  }
  if (self->save_due_ && now >= self->save_due_) {
    self->save_due_ = 0;
    try {
      save_config(self->cfg_);
    } catch (const std::exception& e) {
      fprintf(stderr, "roomcorr: saving config: %s\n", e.what());
    }
  }
}

void Daemon::on_core_error(void* data, uint32_t id, int seq, int res, const char* message) {
  auto* self = static_cast<Daemon*>(data);
  fprintf(stderr, "roomcorr: pipewire error id:%u res:%d (%s): %s\n", id, res, spa_strerror(res), message);
  if (id == PW_ID_CORE && res == -EPIPE) {
    // PipeWire went away; exit and let systemd restart us.
    self->exit_code_ = 1;
    pw_main_loop_quit(self->main_loop_);
  }
}

void Daemon::on_signal(void* data, int) {
  auto* self = static_cast<Daemon*>(data);
  pw_main_loop_quit(self->main_loop_);
}

int Daemon::run() {
  cfg_ = load_config();
  if (cfg_.output_device.empty())
    fprintf(stderr, "roomcorr: no output_device configured; run `roomcorr setup`. Output will follow WirePlumber.\n");

  main_loop_ = pw_main_loop_new(nullptr);
  pw_loop* loop = pw_main_loop_get_loop(main_loop_);
  pw_loop_add_signal(loop, SIGINT, on_signal, this);
  pw_loop_add_signal(loop, SIGTERM, on_signal, this);
  context_ = pw_context_new(loop, nullptr, 0);
  core_ = pw_context_connect(context_, nullptr, 0);
  if (!core_) {
    fprintf(stderr, "roomcorr: cannot connect to PipeWire\n");
    return 1;
  }
  static pw_core_events core_events{};
  core_events.version = PW_VERSION_CORE_EVENTS;
  core_events.error = on_core_error;
  pw_core_add_listener(core_, &core_listener_, &core_events, this);

  Config none;
  none.output_device = cfg_.output_device;
  apply_config(none, true);

  shared_name_ = std::string(RC_SHARED_AUDIO_NAME) + (instance_suffix().empty() ? "" : "-" + instance_suffix().substr(1));
  shared_ = create_shared_audio(shared_name_, shared_bytes_);
  if (shared_) {
    engine_.set_shared_audio(shared_);
    fprintf(stderr, "roomcorr: sharing live audio at /dev/shm%s\n", shared_name_.c_str());
  } else {
    fprintf(stderr, "roomcorr: shared audio unavailable: %s\n", strerror(errno));
  }

  if (!create_playback() || !create_capture()) {
    fprintf(stderr, "roomcorr: cannot create streams\n");
    return 1;
  }
  if (!control_.start(loop, socket_path(), [this](int c, const Json& r) { return handle(c, r); }))
    fprintf(stderr, "roomcorr: cannot open control socket %s\n", socket_path().c_str());

  timer_ = pw_loop_add_timer(loop, on_timer, this);
  timespec interval{0, 25 * 1000000};
  pw_loop_update_timer(loop, timer_, &interval, &interval, false);

  fprintf(stderr, "roomcorr: running; sink '%s' -> %s\n", kSinkName,
          cfg_.output_device.empty() ? "(default)" : cfg_.output_device.c_str());
  pw_main_loop_run(main_loop_);

  if (save_due_) save_config(cfg_);
  control_.stop();
  if (timer_) pw_loop_destroy_source(loop, timer_);
  if (capture_) pw_stream_destroy(capture_);
  if (playback_) pw_stream_destroy(playback_);
  pw_core_disconnect(core_);
  if (shared_) {
    // Streams are gone, so the audio thread no longer writes.
    munmap(shared_, shared_bytes_);
    shm_unlink(shared_name_.c_str());
  }
  pw_context_destroy(context_);
  pw_main_loop_destroy(main_loop_);
  return exit_code_;
}

}  // namespace

int run_daemon() {
  pw_init(nullptr, nullptr);
  auto daemon = std::make_unique<Daemon>();
  int rc = daemon->run();
  pw_deinit();
  return rc;
}

}  // namespace rc
