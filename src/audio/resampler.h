#pragma once

#include <cstdint>
#include <vector>

#include "audio/audio_format.h"

namespace hoodie::audio {

// Streaming sample-rate converter backed by soxr (SOXR_HQ).
//
// Stateful across calls, so it is suitable for resampling a continuous capture
// stream. Input and output are interleaved int16 PCM. Not thread-safe.
class Resampler {
 public:
  Resampler(int input_rate, int output_rate, int channels = 1);
  ~Resampler();

  Resampler(const Resampler&) = delete;
  Resampler& operator=(const Resampler&) = delete;

  // Append resampled output (interleaved) to `out`.
  void process(const int16_t* input, std::size_t input_frames, std::vector<int16_t>& out);

  // Drain the converter's internal delay at end-of-stream.
  void flush(std::vector<int16_t>& out);

  int input_rate() const { return input_rate_; }
  int output_rate() const { return output_rate_; }
  int channels() const { return channels_; }

 private:
  void* handle_ = nullptr;  // soxr_t
  int input_rate_ = 0;
  int output_rate_ = 0;
  int channels_ = 1;
  double ratio_ = 1.0;
};

// Convenience: mix to mono and resample a whole buffer in one call.
AudioFrame resample_frame(const AudioFrame& input, int output_rate);

}  // namespace hoodie::audio
