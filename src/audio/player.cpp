#include "audio/player.h"

#include <portaudio.h>
#include <spdlog/spdlog.h>

#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "audio/devices.h"

namespace hoodie::audio {

namespace {
constexpr auto kEnqueuePollInterval = std::chrono::milliseconds(2);
constexpr auto kDrainPollInterval = std::chrono::milliseconds(5);
}  // namespace

AudioPlayer::AudioPlayer(Options options)
    : options_(std::move(options)), ring_(options_.buffer_samples) {}

AudioPlayer::~AudioPlayer() { stop(); }

int AudioPlayer::pa_callback(const void* /*input*/, void* output, unsigned long frame_count,
                             const PaStreamCallbackTimeInfo* /*time_info*/,
                             PaStreamCallbackFlags /*status_flags*/, void* user_data) {
  auto* self = static_cast<AudioPlayer*>(user_data);
  auto* out = static_cast<int16_t*>(output);
  const std::size_t want = frame_count * static_cast<std::size_t>(self->channels_);
  const std::size_t got = self->ring_.read(out, want);
  if (got < want) {
    std::memset(out + got, 0, (want - got) * sizeof(int16_t));
    if (self->running_.load(std::memory_order_acquire)) {
      self->underruns_.fetch_add(1, std::memory_order_relaxed);
    }
  }
  return paContinue;
}

void AudioPlayer::ensure_resampler(int input_rate) {
  if (resampler_ != nullptr && resampler_input_rate_ == input_rate) return;
  resampler_ = std::make_unique<Resampler>(input_rate, sample_rate_, /*channels=*/1);
  resampler_input_rate_ = input_rate;
}

void AudioPlayer::start() {
  if (running_.load(std::memory_order_acquire)) {
    throw std::runtime_error("player: already running");
  }

  std::string error;
  const auto device = resolve_device(options_.device, /*want_input=*/false, &error);
  if (!device.has_value()) {
    throw std::runtime_error("player: " + error);
  }

  const PaDeviceInfo* info = Pa_GetDeviceInfo(static_cast<PaDeviceIndex>(*device));
  if (info == nullptr) {
    throw std::runtime_error("player: device info unavailable");
  }
  device_name_ = info->name != nullptr ? info->name : "<unnamed>";

  channels_ = options_.channels;
  if (info->maxOutputChannels > 0 && channels_ > info->maxOutputChannels) {
    channels_ = info->maxOutputChannels;
  }
  sample_rate_ = options_.sample_rate > 0 ? options_.sample_rate
                                          : static_cast<int>(info->defaultSampleRate);

  PaStreamParameters output_params{};
  output_params.device = *device;
  output_params.channelCount = channels_;
  output_params.sampleFormat = paInt16;
  output_params.suggestedLatency = info->defaultLowOutputLatency;
  output_params.hostApiSpecificStreamInfo = nullptr;

  if (Pa_IsFormatSupported(nullptr, &output_params, sample_rate_) != paFormatIsSupported) {
    throw std::runtime_error("player: device '" + device_name_ + "' does not support " +
                             std::to_string(sample_rate_) + " Hz / " +
                             std::to_string(channels_) + " ch");
  }

  PaStream* stream = nullptr;
  const PaError err =
      Pa_OpenStream(&stream, nullptr, &output_params, sample_rate_, 0, paClipOff,
                    &AudioPlayer::pa_callback, this);
  if (err != paNoError) {
    throw std::runtime_error(std::string("player: Pa_OpenStream failed: ") + Pa_GetErrorText(err));
  }
  stream_ = stream;

  const PaError start_err = Pa_StartStream(stream);
  if (start_err != paNoError) {
    Pa_CloseStream(stream);
    stream_ = nullptr;
    throw std::runtime_error(std::string("player: Pa_StartStream failed: ") +
                             Pa_GetErrorText(start_err));
  }

  running_.store(true, std::memory_order_release);
  spdlog::info("player: started '{}' {} Hz, {} ch", device_name_, sample_rate_, channels_);
}

void AudioPlayer::stop() {
  if (!running_.exchange(false, std::memory_order_acq_rel)) {
    return;
  }
  if (stream_ != nullptr) {
    Pa_StopStream(stream_);
    Pa_CloseStream(stream_);
    stream_ = nullptr;
  }
  ring_.clear();
  resampler_.reset();
  resampler_input_rate_ = 0;
  spdlog::info("player: stopped (underruns={})", underruns_.load(std::memory_order_relaxed));
}

void AudioPlayer::enqueue(const AudioFrame& frame) {
  if (frame.empty() || !running_.load(std::memory_order_acquire)) return;

  AudioFrame source = frame.channels > 1 ? to_mono(frame) : frame;

  const int16_t* data = source.samples.data();
  std::size_t count = source.samples.size();
  std::vector<int16_t> resampled;

  if (source.sample_rate != sample_rate_) {
    ensure_resampler(source.sample_rate);
    resampler_->process(source.samples.data(), source.frames(), resampled);
    data = resampled.data();
    count = resampled.size();
  }

  std::size_t offset = 0;
  while (offset < count && running_.load(std::memory_order_acquire)) {
    offset += ring_.write(data + offset, count - offset);
    if (offset < count) {
      std::this_thread::sleep_for(kEnqueuePollInterval);
    }
  }
}

bool AudioPlayer::wait_drained(std::chrono::milliseconds settle, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (ring_.empty()) {
      std::this_thread::sleep_for(settle);
      return true;
    }
    std::this_thread::sleep_for(kDrainPollInterval);
  }
  return false;
}

}  // namespace hoodie::audio
