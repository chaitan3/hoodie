#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace hoodie::audio {

constexpr int kSampleRate16k = 16000;  // wake word / VAD
constexpr int kSampleRate24k = 24000;  // OpenAI realtime / TTS PCM
constexpr int kSampleRate48k = 48000;  // typical device capture

// Canonical in-process audio buffer: interleaved signed 16-bit PCM.
struct AudioFrame {
  std::vector<int16_t> samples;  // interleaved
  int sample_rate = kSampleRate16k;
  int channels = 1;

  std::size_t frames() const {
    return channels > 0 ? samples.size() / static_cast<std::size_t>(channels) : 0;
  }
  bool empty() const { return samples.empty(); }
  double duration_seconds() const {
    return sample_rate > 0 ? static_cast<double>(frames()) / sample_rate : 0.0;
  }
  void clear() { samples.clear(); }
};

// Average all channels down to mono.
inline AudioFrame to_mono(const AudioFrame& in) {
  if (in.channels <= 1) return in;
  AudioFrame out;
  out.sample_rate = in.sample_rate;
  out.channels = 1;
  const auto ch = static_cast<std::size_t>(in.channels);
  const std::size_t n = in.frames();
  out.samples.resize(n);
  for (std::size_t i = 0; i < n; ++i) {
    int sum = 0;
    for (std::size_t c = 0; c < ch; ++c) sum += in.samples[i * ch + c];
    out.samples[i] = static_cast<int16_t>(sum / static_cast<int>(ch));
  }
  return out;
}

}  // namespace hoodie::audio
