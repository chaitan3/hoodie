#include "audio/capture.h"

#include <portaudio.h>
#include <spdlog/spdlog.h>

#include <stdexcept>
#include <string>

#include "audio/devices.h"

namespace hoodie::audio {

AudioCapture::AudioCapture(Options options) : options_(std::move(options)) {}

AudioCapture::~AudioCapture() { stop(); }

int AudioCapture::pa_callback(const void* input, void* /*output*/, unsigned long frame_count,
                              const PaStreamCallbackTimeInfo* /*time_info*/,
                              PaStreamCallbackFlags status_flags, void* user_data) {
  auto* self = static_cast<AudioCapture*>(user_data);
  if ((status_flags & paInputOverflow) != 0) {
    spdlog::warn("capture: input overflow");
  }
  if (self->callback_ && input != nullptr) {
    self->callback_(static_cast<const int16_t*>(input), frame_count, self->channels_);
  }
  return paContinue;
}

void AudioCapture::start(Callback callback) {
  if (running_.load(std::memory_order_acquire)) {
    throw std::runtime_error("capture: already running");
  }
  callback_ = std::move(callback);

  std::string error;
  const auto device = resolve_device(options_.device, /*want_input=*/true, &error);
  if (!device.has_value()) {
    throw std::runtime_error("capture: " + error);
  }

  const PaDeviceInfo* info = Pa_GetDeviceInfo(static_cast<PaDeviceIndex>(*device));
  if (info == nullptr) {
    throw std::runtime_error("capture: device info unavailable");
  }
  device_name_ = info->name != nullptr ? info->name : "<unnamed>";

  channels_ = options_.channels;
  if (info->maxInputChannels > 0 && channels_ > info->maxInputChannels) {
    spdlog::warn("capture: requested {} channels, device supports {}; clamping", channels_,
                 info->maxInputChannels);
    channels_ = info->maxInputChannels;
  }
  sample_rate_ = options_.sample_rate;

  auto is_supported = [&](int channels) {
    PaStreamParameters params{};
    params.device = *device;
    params.channelCount = channels;
    params.sampleFormat = paInt16;
    params.suggestedLatency = info->defaultLowInputLatency;
    params.hostApiSpecificStreamInfo = nullptr;
    return Pa_IsFormatSupported(&params, nullptr, sample_rate_) == paFormatIsSupported;
  };

  // Some codecs (e.g. I2S + ES8388) only offer stereo capture. Requesting mono
  // would fail Pa_IsFormatSupported, so fall back to the device's channel count
  // and let the consumer downmix.
  if (!is_supported(channels_)) {
    const int fallback = info->maxInputChannels;
    if (fallback > 0 && fallback != channels_ && is_supported(fallback)) {
      spdlog::warn("capture: '{}' does not support {} ch; using {} ch (will mix to mono)",
                   device_name_, channels_, fallback);
      channels_ = fallback;
    } else {
      throw std::runtime_error("capture: device '" + device_name_ + "' does not support " +
                               std::to_string(sample_rate_) + " Hz / " +
                               std::to_string(channels_) + " ch");
    }
  }

  PaStreamParameters input_params{};
  input_params.device = *device;
  input_params.channelCount = channels_;
  input_params.sampleFormat = paInt16;
  input_params.suggestedLatency = info->defaultLowInputLatency;
  input_params.hostApiSpecificStreamInfo = nullptr;

  PaStream* stream = nullptr;
  const PaError err =
      Pa_OpenStream(&stream, &input_params, nullptr, sample_rate_, options_.frames_per_buffer,
                    paClipOff, &AudioCapture::pa_callback, this);
  if (err != paNoError) {
    throw std::runtime_error(std::string("capture: Pa_OpenStream failed: ") + Pa_GetErrorText(err));
  }
  stream_ = stream;

  const PaError start_err = Pa_StartStream(stream);
  if (start_err != paNoError) {
    Pa_CloseStream(stream);
    stream_ = nullptr;
    throw std::runtime_error(std::string("capture: Pa_StartStream failed: ") +
                             Pa_GetErrorText(start_err));
  }

  running_.store(true, std::memory_order_release);
  spdlog::info("capture: started '{}' {} Hz, {} ch", device_name_, sample_rate_, channels_);
}

void AudioCapture::stop() {
  if (!running_.exchange(false, std::memory_order_acq_rel)) {
    return;
  }
  auto* stream = stream_;
  if (stream != nullptr) {
    Pa_StopStream(stream);
    Pa_CloseStream(stream);
    stream_ = nullptr;
  }
  callback_ = nullptr;
  spdlog::info("capture: stopped");
}

}  // namespace hoodie::audio
