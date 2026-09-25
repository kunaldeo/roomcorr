#include "measure.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <thread>

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

#include "config.hpp"

namespace rc {

namespace {

uint32_t channel_id(const std::string& name) {
  static const std::pair<const char*, uint32_t> map[] = {
      {"FL", SPA_AUDIO_CHANNEL_FL},   {"FR", SPA_AUDIO_CHANNEL_FR}, {"FC", SPA_AUDIO_CHANNEL_FC},
      {"LFE", SPA_AUDIO_CHANNEL_LFE}, {"RL", SPA_AUDIO_CHANNEL_RL}, {"RR", SPA_AUDIO_CHANNEL_RR},
      {"SL", SPA_AUDIO_CHANNEL_SL},   {"SR", SPA_AUDIO_CHANNEL_SR}, {"MONO", SPA_AUDIO_CHANNEL_MONO}};
  for (auto& [n, id] : map)
    if (name == n) return id;
  throw std::runtime_error("unknown channel position " + name);
}

const spa_pod* format(spa_pod_builder* b, const std::vector<uint32_t>& pos) {
  spa_audio_info_raw info{};
  info.format = SPA_AUDIO_FORMAT_F32P;
  info.rate = kSampleRate;
  info.channels = uint32_t(pos.size());
  for (size_t i = 0; i < pos.size(); ++i) info.position[i] = pos[i];
  return spa_format_audio_raw_build(b, SPA_PARAM_EnumFormat, &info);
}

struct Session {
  const MeasureRequest* req = nullptr;
  pw_thread_loop* loop = nullptr;
  pw_stream* play = nullptr;
  pw_stream* cap = nullptr;

  std::atomic<int> play_state{PW_STREAM_STATE_UNCONNECTED}, cap_state{PW_STREAM_STATE_UNCONNECTED};
  std::atomic<bool> go{false};
  std::atomic<int64_t> start_index{-1};  // capture frame at which playback began
  std::atomic<int64_t> captured{0};
  size_t play_pos = 0;
  size_t total = 0;
  std::vector<float> rec;  // preallocated
  std::atomic<float> peak{0};
  std::string error;
};

void on_play_process(void* data) {
  auto* s = static_cast<Session*>(data);
  pw_buffer* b = pw_stream_dequeue_buffer(s->play);
  if (!b) return;
  spa_buffer* buf = b->buffer;
  uint32_t n = UINT32_MAX;
  for (uint32_t i = 0; i < buf->n_datas; ++i) n = std::min(n, buf->datas[i].maxsize / uint32_t(sizeof(float)));
  if (b->requested) n = std::min<uint32_t>(n, uint32_t(b->requested));

  bool running = s->go.load(std::memory_order_acquire);
  if (running && s->start_index.load(std::memory_order_relaxed) < 0) {
    // Both streams run in the same graph cycle, so the capture position
    // right now is a fixed offset from our playback position.
    s->start_index.store(s->captured.load(std::memory_order_acquire), std::memory_order_release);
  }
  for (uint32_t c = 0; c < buf->n_datas; ++c) {
    float* dst = static_cast<float*>(buf->datas[c].data);
    if (!dst) continue;
    const auto& src = s->req->signal[std::min<size_t>(c, s->req->signal.size() - 1)];
    for (uint32_t i = 0; i < n; ++i) {
      size_t k = s->play_pos + i;
      dst[i] = running && c < s->req->signal.size() && k < src.size() ? src[k] : 0.f;
    }
    buf->datas[c].chunk->offset = 0;
    buf->datas[c].chunk->stride = sizeof(float);
    buf->datas[c].chunk->size = n * sizeof(float);
  }
  if (running) s->play_pos += n;
  pw_stream_queue_buffer(s->play, b);
}

void on_cap_process(void* data) {
  auto* s = static_cast<Session*>(data);
  pw_buffer* b = pw_stream_dequeue_buffer(s->cap);
  if (!b) return;
  spa_data& d = b->buffer->datas[0];
  if (d.data) {
    uint32_t off = std::min(d.chunk->offset, d.maxsize);
    uint32_t n = std::min(d.chunk->size, d.maxsize - off) / uint32_t(sizeof(float));
    const float* src = SPA_PTROFF(d.data, off, const float);
    int64_t at = s->captured.load(std::memory_order_relaxed);
    float pk = s->peak.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < n; ++i) {
      if (size_t(at) + i < s->rec.size()) s->rec[size_t(at) + i] = src[i];
      pk = std::max(pk, std::fabs(src[i]));
    }
    s->peak.store(pk, std::memory_order_relaxed);
    s->captured.store(at + n, std::memory_order_release);
  }
  pw_stream_queue_buffer(s->cap, b);
}

void on_play_state(void* data, pw_stream_state, pw_stream_state st, const char* err) {
  auto* s = static_cast<Session*>(data);
  s->play_state = st;
  if (err) s->error = std::string("playback: ") + err;
}

void on_cap_state(void* data, pw_stream_state, pw_stream_state st, const char* err) {
  auto* s = static_cast<Session*>(data);
  s->cap_state = st;
  if (err) s->error = std::string("capture: ") + err;
}

// ---- live mode

struct LiveSession {
  const MeasureRequest* req = nullptr;
  pw_stream* play = nullptr;
  pw_stream* cap = nullptr;
  std::atomic<int> play_state{PW_STREAM_STATE_UNCONNECTED}, cap_state{PW_STREAM_STATE_UNCONNECTED};
  size_t pos = 0;
  double gain = 0;                 // fade envelope, audio thread only
  std::atomic<bool> stopping{false};
  std::atomic<bool> faded_out{false};
  std::vector<float> ring;         // capture ring, power-of-two size
  std::atomic<uint64_t> written{0};
  std::string error;
};

void live_play(void* data) {
  auto* s = static_cast<LiveSession*>(data);
  pw_buffer* b = pw_stream_dequeue_buffer(s->play);
  if (!b) return;
  spa_buffer* buf = b->buffer;
  uint32_t n = UINT32_MAX;
  for (uint32_t i = 0; i < buf->n_datas; ++i) n = std::min(n, buf->datas[i].maxsize / uint32_t(sizeof(float)));
  if (b->requested) n = std::min<uint32_t>(n, uint32_t(b->requested));
  const double step = 1.0 / (0.08 * kSampleRate);  // 80 ms fades
  const bool stop = s->stopping.load(std::memory_order_acquire);
  double g = s->gain;
  const size_t len = s->req->signal[0].size();
  for (uint32_t i = 0; i < n; ++i) {
    g = stop ? std::max(0.0, g - step) : std::min(1.0, g + step);
    size_t k = (s->pos + i) % len;
    for (uint32_t c = 0; c < buf->n_datas; ++c) {
      float* dst = static_cast<float*>(buf->datas[c].data);
      if (!dst) continue;
      dst[i] = c < s->req->signal.size() ? float(s->req->signal[c][k] * g) : 0.f;
    }
  }
  s->gain = g;
  s->pos = (s->pos + n) % len;
  if (stop && g <= 0) s->faded_out.store(true, std::memory_order_release);
  for (uint32_t c = 0; c < buf->n_datas; ++c) {
    buf->datas[c].chunk->offset = 0;
    buf->datas[c].chunk->stride = sizeof(float);
    buf->datas[c].chunk->size = n * sizeof(float);
  }
  pw_stream_queue_buffer(s->play, b);
}

void live_cap(void* data) {
  auto* s = static_cast<LiveSession*>(data);
  pw_buffer* b = pw_stream_dequeue_buffer(s->cap);
  if (!b) return;
  spa_data& d = b->buffer->datas[0];
  if (d.data) {
    uint32_t off = std::min(d.chunk->offset, d.maxsize);
    uint32_t n = std::min(d.chunk->size, d.maxsize - off) / uint32_t(sizeof(float));
    const float* src = SPA_PTROFF(d.data, off, const float);
    uint64_t w = s->written.load(std::memory_order_relaxed);
    const size_t mask = s->ring.size() - 1;
    for (uint32_t i = 0; i < n; ++i) s->ring[(w + i) & mask] = src[i];
    s->written.store(w + n, std::memory_order_release);
  }
  pw_stream_queue_buffer(s->cap, b);
}

void live_play_state(void* data, pw_stream_state, pw_stream_state st, const char* err) {
  auto* s = static_cast<LiveSession*>(data);
  s->play_state = st;
  if (err) s->error = std::string("playback: ") + err;
}

void live_cap_state(void* data, pw_stream_state, pw_stream_state st, const char* err) {
  auto* s = static_cast<LiveSession*>(data);
  s->cap_state = st;
  if (err) s->error = std::string("capture: ") + err;
}

}  // namespace

void run_live(const MeasureRequest& req, int interval_ms, double window_s,
              const std::function<bool(const std::vector<double>& recent)>& tick) {
  if (req.signal.empty() || req.signal.size() != req.positions.size() || req.signal[0].empty())
    throw std::runtime_error("live: signal/positions mismatch");
  pw_init(nullptr, nullptr);
  LiveSession s;
  s.req = &req;
  s.ring.assign(1 << 18, 0.f);  // 5.4 s
  const size_t window = std::min(s.ring.size(), size_t(window_s * kSampleRate));

  pw_thread_loop* loop = pw_thread_loop_new("roomcorr-live", nullptr);
  pw_thread_loop_lock(loop);
  pw_thread_loop_start(loop);

  std::vector<uint32_t> play_pos;
  std::string pos_str = "[ ";
  for (const auto& p : req.positions) {
    play_pos.push_back(channel_id(p));
    pos_str += p + " ";
  }
  pos_str += "]";

  static pw_stream_events pe{}, ce{};
  pe.version = ce.version = PW_VERSION_STREAM_EVENTS;
  pe.process = live_play;
  pe.state_changed = live_play_state;
  ce.process = live_cap;
  ce.state_changed = live_cap_state;
  pw_properties* pp = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback", PW_KEY_MEDIA_ROLE,
                                        "Production", PW_KEY_NODE_NAME, "roomcorr_live_out", PW_KEY_NODE_DESCRIPTION,
                                        "Room Correction Level Test", PW_KEY_TARGET_OBJECT, req.play_target.c_str(),
                                        PW_KEY_STREAM_DONT_REMIX, "true", SPA_KEY_AUDIO_POSITION, pos_str.c_str(),
                                        "node.dont-fallback", "true", nullptr);
  s.play = pw_stream_new_simple(pw_thread_loop_get_loop(loop), "roomcorr live out", pp, &pe, &s);
  pw_properties* cp = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE,
                                        "Production", PW_KEY_NODE_NAME, "roomcorr_live_in", PW_KEY_TARGET_OBJECT,
                                        req.capture_target.c_str(), SPA_KEY_AUDIO_POSITION, "[ MONO ]",
                                        "node.dont-fallback", "true", nullptr);
  s.cap = pw_stream_new_simple(pw_thread_loop_get_loop(loop), "roomcorr live in", cp, &ce, &s);

  uint8_t b1[1024], b2[1024];
  spa_pod_builder pb1 = SPA_POD_BUILDER_INIT(b1, sizeof b1), pb2 = SPA_POD_BUILDER_INIT(b2, sizeof b2);
  const spa_pod* pparams[1] = {format(&pb1, play_pos)};
  const spa_pod* cparams[1] = {format(&pb2, {SPA_AUDIO_CHANNEL_MONO})};
  auto flags = pw_stream_flags(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS |
                               PW_STREAM_FLAG_DONT_RECONNECT);
  pw_stream_connect(s.cap, PW_DIRECTION_INPUT, PW_ID_ANY, flags, cparams, 1);
  pw_stream_connect(s.play, PW_DIRECTION_OUTPUT, PW_ID_ANY, flags, pparams, 1);
  pw_thread_loop_unlock(loop);

  auto cleanup = [&] {
    pw_thread_loop_lock(loop);
    pw_stream_destroy(s.play);
    pw_stream_destroy(s.cap);
    pw_thread_loop_unlock(loop);
    pw_thread_loop_stop(loop);
    pw_thread_loop_destroy(loop);
  };
  using namespace std::chrono;
  auto until = steady_clock::now() + seconds(5);
  while (!(s.play_state == PW_STREAM_STATE_STREAMING && s.cap_state == PW_STREAM_STATE_STREAMING)) {
    if (steady_clock::now() > until || s.play_state == PW_STREAM_STATE_ERROR || s.cap_state == PW_STREAM_STATE_ERROR) {
      std::string err = s.error.empty() ? "streams did not start" : s.error;
      cleanup();
      throw std::runtime_error("live: " + err);
    }
    std::this_thread::sleep_for(milliseconds(10));
  }

  std::vector<double> recent(window);
  bool keep_going = true;
  try {
    while (keep_going) {
      std::this_thread::sleep_for(milliseconds(interval_ms));
      uint64_t w = s.written.load(std::memory_order_acquire);
      if (w < window) continue;  // still filling
      const size_t mask = s.ring.size() - 1;
      for (size_t i = 0; i < window; ++i) recent[i] = s.ring[(w - window + i) & mask];
      keep_going = tick(recent);
    }
  } catch (...) {
    s.stopping = true;
    cleanup();
    throw;
  }
  // Fade out before tearing the streams down.
  s.stopping = true;
  auto fade_until = steady_clock::now() + milliseconds(500);
  while (!s.faded_out && steady_clock::now() < fade_until) std::this_thread::sleep_for(milliseconds(10));
  cleanup();
}

MeasureResult run_measurement(const MeasureRequest& req) {
  if (req.signal.empty() || req.signal.size() != req.positions.size())
    throw std::runtime_error("measurement: signal/positions mismatch");
  pw_init(nullptr, nullptr);

  Session s;
  s.req = &req;
  s.total = req.signal[0].size();
  const size_t tail = size_t(req.tail_seconds * kSampleRate);
  // Room for the signal, the tail and a generous amount of pre-roll.
  s.rec.assign(s.total + tail + size_t(20 * kSampleRate), 0.f);

  s.loop = pw_thread_loop_new("roomcorr-measure", nullptr);
  pw_thread_loop_lock(s.loop);
  pw_thread_loop_start(s.loop);

  std::vector<uint32_t> play_pos;
  for (const auto& p : req.positions) play_pos.push_back(channel_id(p));
  std::string pos_str = "[ ";
  for (const auto& p : req.positions) pos_str += p + " ";
  pos_str += "]";

  static pw_stream_events play_events{}, cap_events{};
  play_events.version = cap_events.version = PW_VERSION_STREAM_EVENTS;
  play_events.process = on_play_process;
  play_events.state_changed = on_play_state;
  cap_events.process = on_cap_process;
  cap_events.state_changed = on_cap_state;

  pw_properties* pp = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback", PW_KEY_MEDIA_ROLE,
                                        "Production", PW_KEY_NODE_NAME, "roomcorr_measure_out", PW_KEY_NODE_DESCRIPTION,
                                        "Room Correction Measurement", PW_KEY_TARGET_OBJECT, req.play_target.c_str(),
                                        PW_KEY_STREAM_DONT_REMIX, "true", SPA_KEY_AUDIO_POSITION, pos_str.c_str(),
                                        "node.dont-fallback", "true", nullptr);
  s.play = pw_stream_new_simple(pw_thread_loop_get_loop(s.loop), "roomcorr measure out", pp, &play_events, &s);

  pw_properties* cp = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE,
                                        "Production", PW_KEY_NODE_NAME, "roomcorr_measure_in", PW_KEY_NODE_DESCRIPTION,
                                        "Room Correction Microphone", PW_KEY_TARGET_OBJECT, req.capture_target.c_str(),
                                        SPA_KEY_AUDIO_POSITION, "[ MONO ]", "node.dont-fallback", "true", nullptr);
  s.cap = pw_stream_new_simple(pw_thread_loop_get_loop(s.loop), "roomcorr measure in", cp, &cap_events, &s);


  uint8_t buf1[1024], buf2[1024];
  spa_pod_builder b1 = SPA_POD_BUILDER_INIT(buf1, sizeof buf1), b2 = SPA_POD_BUILDER_INIT(buf2, sizeof buf2);
  const spa_pod* pparams[1] = {format(&b1, play_pos)};
  const spa_pod* cparams[1] = {format(&b2, {SPA_AUDIO_CHANNEL_MONO})};
  auto flags = pw_stream_flags(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS |
                               PW_STREAM_FLAG_DONT_RECONNECT);
  pw_stream_connect(s.cap, PW_DIRECTION_INPUT, PW_ID_ANY, flags, cparams, 1);
  pw_stream_connect(s.play, PW_DIRECTION_OUTPUT, PW_ID_ANY, flags, pparams, 1);
  pw_thread_loop_unlock(s.loop);

  auto cleanup = [&] {
    pw_thread_loop_lock(s.loop);
    pw_stream_destroy(s.play);
    pw_stream_destroy(s.cap);
    pw_thread_loop_unlock(s.loop);
    pw_thread_loop_stop(s.loop);
    pw_thread_loop_destroy(s.loop);
  };
  using namespace std::chrono;
  auto wait_for = [&](auto pred, double seconds) {
    auto until = steady_clock::now() + duration<double>(seconds);
    while (!pred()) {
      if (steady_clock::now() > until) return false;
      if (s.play_state == PW_STREAM_STATE_ERROR || s.cap_state == PW_STREAM_STATE_ERROR) return false;
      std::this_thread::sleep_for(milliseconds(10));
    }
    return true;
  };

  if (!wait_for([&] { return s.play_state == PW_STREAM_STATE_STREAMING && s.cap_state == PW_STREAM_STATE_STREAMING; }, 5)) {
    std::string err = s.error.empty() ? "streams did not start (is the device available?)" : s.error;
    cleanup();
    throw std::runtime_error("measurement: " + err);
  }
  // Let both devices settle (the mic's resampler locks in) before starting.
  std::this_thread::sleep_for(milliseconds(500));
  s.go.store(true, std::memory_order_release);

  const double timeout = double(s.total + tail) / kSampleRate + 10;
  bool done = wait_for(
      [&] {
        int64_t st = s.start_index.load();
        return st >= 0 && s.captured.load() >= st + int64_t(s.total + tail);
      },
      timeout);
  int64_t st = s.start_index.load();
  int64_t got = s.captured.load();
  cleanup();
  if (!done) throw std::runtime_error("measurement: timed out (captured " + std::to_string(got) + " frames)");

  MeasureResult r;
  r.recording.assign(s.rec.begin() + st, s.rec.begin() + st + long(s.total + tail));
  r.capture_peak_dbfs = 20 * std::log10(double(s.peak.load()) + 1e-30);
  return r;
}

}  // namespace rc
