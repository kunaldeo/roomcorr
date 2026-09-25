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

}  // namespace

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
