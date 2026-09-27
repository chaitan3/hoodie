#pragma once

#include <portaudio.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>

#include "audio/audio_format.h"
#include "audio/ring_buffer.h"
#include "audio/resampler.h"

namespace hoodie::audio {

// Streaming PCM playback via PortAudio.
//
// enqueue() may be called from any single thread; it resamples/mixes to the
// output stream's format and blocks while the ring buffer is full. The
// PortAudio callback drains the ring buffer and fills silence on underrun.
class AudioPlayer {
 public:
  struct Options {
    std::string device;           // "" = system default
    int sample_rate = 0;          // 0 = device default
    int channels = 1;
    std::size_t buffer_samples = 1u << 16;  // ring capacity in int16 samples
  };

  explicit AudioPlayer(Options options);
  ~AudioPlayer();

  AudioPlayer(const AudioPlayer&) = delete;
  AudioPlayer& operator=(const AudioPlayer&) = delete;

  void start();
  void stop();

  // Resample/mix the frame to the output format and queue it for playback.
  void enqueue(const AudioFrame& frame);

  // Wait until the ring buffer is empty and the device has had time to drain.
  bool wait_drained(std::chrono::milliseconds settle = std::chrono::milliseconds(300),
                    std::chrono::milliseconds timeout = std::chrono::seconds(60));

  bool running() const { return running_.load(std::memory_order_acquire); }
  int sample_rate() const { return sample_rate_; }
  int channels() const { return channels_; }
  const std::string& device_name() const { return device_name_; }
  std::size_t underruns() const { return underruns_.load(std::memory_order_relaxed); }

 private:
  static int pa_callback(const void* input, void* output, unsigned long frame_count,
                         const PaStreamCallbackTimeInfo* time_info,
                         PaStreamCallbackFlags status_flags, void* user_data);

  void ensure_resampler(int input_rate);

  Options options_;
  PaStream* stream_ = nullptr;
  std::atomic<bool> running_{false};
  std::atomic<std::size_t> underruns_{0};
  SpscRingBuffer<int16_t> ring_;
  std::unique_ptr<Resampler> resampler_;
  int resampler_input_rate_ = 0;
  int sample_rate_ = 0;
  int channels_ = 1;
  std::string device_name_;
};

}  // namespace hoodie::audio
