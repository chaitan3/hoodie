#pragma once

#include <portaudio.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace hoodie::audio {

// Microphone capture via PortAudio. The callback runs on PortAudio's own
// thread and must be real-time friendly (no blocking, no allocation).
class AudioCapture {
 public:
  struct Options {
    std::string device;           // "" = system default
    int sample_rate = 48000;
    int channels = 1;
    int frames_per_buffer = 0;    // 0 = let PortAudio choose
  };

  using Callback = std::function<void(const int16_t* samples, std::size_t frames, int channels)>;

  explicit AudioCapture(Options options);
  ~AudioCapture();

  AudioCapture(const AudioCapture&) = delete;
  AudioCapture& operator=(const AudioCapture&) = delete;

  // Opens and starts the stream. Throws std::runtime_error on failure.
  void start(Callback callback);
  void stop();

  bool running() const { return running_.load(std::memory_order_acquire); }
  int sample_rate() const { return sample_rate_; }
  int channels() const { return channels_; }
  const std::string& device_name() const { return device_name_; }

 private:
  static int pa_callback(const void* input, void* output, unsigned long frame_count,
                         const PaStreamCallbackTimeInfo* time_info,
                         PaStreamCallbackFlags status_flags, void* user_data);

  Options options_;
  Callback callback_;
  PaStream* stream_ = nullptr;
  std::atomic<bool> running_{false};
  int sample_rate_ = 0;
  int channels_ = 1;
  std::string device_name_;
};

}  // namespace hoodie::audio
