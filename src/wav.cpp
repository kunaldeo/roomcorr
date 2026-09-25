#include "wav.hpp"

#include <sndfile.h>
#include <stdexcept>

namespace rc {

std::vector<float> read_wav_mono(const std::string& path, int expected_rate) {
  SF_INFO info{};
  SNDFILE* f = sf_open(path.c_str(), SFM_READ, &info);
  if (!f) throw std::runtime_error("cannot open " + path + ": " + sf_strerror(nullptr));
  if (expected_rate && info.samplerate != expected_rate) {
    sf_close(f);
    throw std::runtime_error(path + ": sample rate " + std::to_string(info.samplerate) + ", expected " +
                             std::to_string(expected_rate));
  }
  std::vector<float> inter(size_t(info.frames) * size_t(info.channels));
  sf_count_t got = sf_readf_float(f, inter.data(), info.frames);
  sf_close(f);
  std::vector<float> out(static_cast<size_t>(got));
  for (sf_count_t i = 0; i < got; ++i) out[size_t(i)] = inter[size_t(i) * size_t(info.channels)];
  return out;
}

void write_wav_mono(const std::string& path, const std::vector<float>& data, int rate) {
  SF_INFO info{};
  info.samplerate = rate;
  info.channels = 1;
  info.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;
  SNDFILE* f = sf_open(path.c_str(), SFM_WRITE, &info);
  if (!f) throw std::runtime_error("cannot write " + path + ": " + sf_strerror(nullptr));
  sf_writef_float(f, data.data(), sf_count_t(data.size()));
  sf_close(f);
}

}  // namespace rc
