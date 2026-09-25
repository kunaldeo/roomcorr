// Float WAV I/O via libsndfile. Filters and impulse responses are stored as
// 32-bit float WAVs so they can be inspected in REW or Audacity.
#pragma once

#include <string>
#include <vector>

namespace rc {

// Reads the first channel. Throws on error or when the rate differs from
// expected_rate (if non-zero).
std::vector<float> read_wav_mono(const std::string& path, int expected_rate = 0);
void write_wav_mono(const std::string& path, const std::vector<float>& data, int rate);

}  // namespace rc
