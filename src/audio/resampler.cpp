#include "audio/resampler.h"

#include <soxr.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace hoodie::audio {
namespace {

int16_t to_int16(float value) {
  return static_cast<int16_t>(std::lround(std::clamp(value, -32768.0f, 32767.0f)));
}

}  // namespace

Resampler::Resampler(int input_rate, int output_rate, int channels)
    : input_rate_(input_rate),
      output_rate_(output_rate),
      channels_(channels),
      ratio_(static_cast<double>(output_rate) / static_cast<double>(input_rate)) {
  if (input_rate <= 0 || output_rate <= 0) {
    throw std::runtime_error("resampler: sample rates must be positive");
  }
  if (channels <= 0) {
    throw std::runtime_error("resampler: channels must be positive");
  }

  soxr_error_t error = nullptr;
  // Float32 I/O instead of SOXR_INT16_I: the integer path applies dithering
  // (observably non-deterministic across runs here) and float I/O avoids it.
  // Single-threaded runtime keeps results reproducible and suits small chunks.
  const soxr_io_spec_t io = soxr_io_spec(SOXR_FLOAT32_I, SOXR_FLOAT32_I);
  const soxr_quality_spec_t quality = soxr_quality_spec(SOXR_HQ, 0);
  const soxr_runtime_spec_t runtime = soxr_runtime_spec(1);
  handle_ = soxr_create(input_rate, output_rate, static_cast<unsigned>(channels), &error, &io,
                        &quality, &runtime);
  if (handle_ == nullptr) {
    throw std::runtime_error(std::string("resampler: soxr_create failed: ") +
                             (error != nullptr ? error : "unknown error"));
  }
}

Resampler::~Resampler() {
  if (handle_ != nullptr) soxr_delete(static_cast<soxr_t>(handle_));
}

void Resampler::process(const int16_t* input, std::size_t input_frames,
                        std::vector<int16_t>& out) {
  if (handle_ == nullptr || input_frames == 0) return;
  auto* soxr_handle = static_cast<soxr_t>(handle_);

  std::vector<float> float_in(input_frames * static_cast<std::size_t>(channels_));
  for (std::size_t i = 0; i < float_in.size(); ++i) float_in[i] = static_cast<float>(input[i]);

  std::size_t consumed = 0;
  while (consumed < input_frames) {
    const std::size_t out_frames =
        static_cast<std::size_t>(static_cast<double>(input_frames - consumed) * ratio_) + 1024;
    std::vector<float> float_out(out_frames * static_cast<std::size_t>(channels_));

    std::size_t in_done = 0;
    std::size_t out_done = 0;
    soxr_process(soxr_handle, float_in.data() + consumed * static_cast<std::size_t>(channels_),
                 input_frames - consumed, &in_done, float_out.data(), out_frames, &out_done);

    const std::size_t produced = out_done * static_cast<std::size_t>(channels_);
    out.reserve(out.size() + produced);
    for (std::size_t i = 0; i < produced; ++i) out.push_back(to_int16(float_out[i]));

    consumed += in_done;
    if (in_done == 0 && out_done == 0) break;  // no progress; avoid spinning
  }
}

void Resampler::flush(std::vector<int16_t>& out) {
  if (handle_ == nullptr) return;
  auto* soxr_handle = static_cast<soxr_t>(handle_);
  for (;;) {
    constexpr std::size_t kOutFrames = 4096;
    std::vector<float> float_out(kOutFrames * static_cast<std::size_t>(channels_));

    std::size_t in_done = 0;
    std::size_t out_done = 0;
    soxr_process(soxr_handle, nullptr, 0, &in_done, float_out.data(), kOutFrames, &out_done);

    const std::size_t produced = out_done * static_cast<std::size_t>(channels_);
    out.reserve(out.size() + produced);
    for (std::size_t i = 0; i < produced; ++i) out.push_back(to_int16(float_out[i]));
    if (out_done == 0) break;
  }
}

AudioFrame resample_frame(const AudioFrame& input, int output_rate) {
  AudioFrame mono = to_mono(input);
  if (mono.sample_rate == output_rate || mono.empty()) return mono;

  Resampler resampler(mono.sample_rate, output_rate, /*channels=*/1);
  AudioFrame out;
  out.sample_rate = output_rate;
  out.channels = 1;
  resampler.process(mono.samples.data(), mono.frames(), out.samples);
  resampler.flush(out.samples);
  return out;
}

}  // namespace hoodie::audio
