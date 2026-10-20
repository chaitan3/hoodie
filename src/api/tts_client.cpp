#include "api/tts_client.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "api/api_util.h"
#include "audio/wav.h"

namespace hoodie::api {
namespace {

std::string to_lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

constexpr int kOpenAiPcmSampleRate = 24000;
constexpr std::size_t kSampleBytes = sizeof(int16_t);

}  // namespace

BufferedTtsClient::BufferedTtsClient(net::HttpClient& http, std::string url, std::string model,
                                     std::string api_key, std::string response_format)
    : http_(http),
      url_(std::move(url)),
      model_(std::move(model)),
      api_key_(std::move(api_key)),
      response_format_(std::move(response_format)) {}

bool BufferedTtsClient::streaming() const { return false; }

void BufferedTtsClient::synthesize(const std::string& text, const util::CancelToken& cancel,
                                   std::function<void(const audio::AudioFrame&)> on_chunk,
                                   std::function<void()> on_done) {
  nlohmann::json body{{"model", model_}, {"input", text}};
  if (!response_format_.empty()) body["response_format"] = response_format_;

  const net::HttpResult result =
      http_.post_json(url_, json_headers(api_key_), body.dump(), cancel);
  throw_if_http_error(result, "tts");
  if (result.body.empty()) {
    throw ApiError(result.status, "tts: empty audio response");
  }

  const std::string content_type = to_lower(result.content_type);
  audio::AudioFrame frame;
  if (content_type.find("pcm") != std::string::npos ||
      content_type.find("l16") != std::string::npos ||
      content_type.find("audio/raw") != std::string::npos) {
    frame = audio::decode_pcm16(result.body.data(), result.body.size(), kOpenAiPcmSampleRate, 1);
  } else {
    frame = audio::decode_audio_memory(result.body.data(), result.body.size(),
                                       result.content_type);
  }

  if (on_chunk) on_chunk(frame);
  if (on_done) on_done();
}

std::string BufferedTtsClient::description() const {
  return "buffered TTS (" + model_ + " @ " + url_ +
         (response_format_.empty() ? "" : ", format=" + response_format_) + ")";
}

StreamPcmTtsClient::StreamPcmTtsClient(net::HttpClient& http, std::string url, std::string model,
                                       std::string api_key, int pcm_rate, int pcm_channels)
    : http_(http),
      url_(std::move(url)),
      model_(std::move(model)),
      api_key_(std::move(api_key)),
      pcm_rate_(pcm_rate),
      pcm_channels_(pcm_channels) {}

bool StreamPcmTtsClient::streaming() const { return true; }

void StreamPcmTtsClient::synthesize(const std::string& text, const util::CancelToken& cancel,
                                    std::function<void(const audio::AudioFrame&)> on_chunk,
                                    std::function<void()> on_done) {
  const nlohmann::json body{{"model", model_},
                            {"input", text},
                            {"response_format", "pcm"},
                            {"stream_format", "audio"}};

  std::vector<uint8_t> pending;
  const net::HttpResult result = http_.post_json_stream(
      url_, json_headers(api_key_), body.dump(),
      [&](const uint8_t* data, std::size_t size) {
        pending.insert(pending.end(), data, data + size);
        const std::size_t samples = pending.size() / kSampleBytes;
        if (samples == 0) return;

        audio::AudioFrame frame;
        frame.sample_rate = pcm_rate_;
        frame.channels = pcm_channels_;
        frame.samples.resize(samples);
        std::memcpy(frame.samples.data(), pending.data(), samples * kSampleBytes);
        pending.erase(pending.begin(),
                      pending.begin() + static_cast<std::ptrdiff_t>(samples * kSampleBytes));
        if (on_chunk) on_chunk(frame);
      },
      cancel);

  throw_if_http_error(result, "tts");
  if (!pending.empty()) {
    spdlog::warn("tts: dropping {} trailing byte(s) of an incomplete sample", pending.size());
  }
  if (on_done) on_done();
}

std::string StreamPcmTtsClient::description() const {
  return "streaming PCM TTS (" + model_ + " @ " + url_ + ", " + std::to_string(pcm_rate_) +
         " Hz, " + std::to_string(pcm_channels_) + " ch)";
}

audio::AudioFrame synthesize_blocking(TtsClient& client, const std::string& text,
                                      const util::CancelToken& cancel) {
  audio::AudioFrame out;
  client.synthesize(
      text, cancel,
      [&](const audio::AudioFrame& chunk) {
        if (out.samples.empty()) {
          out.sample_rate = chunk.sample_rate;
          out.channels = chunk.channels;
        }
        out.samples.insert(out.samples.end(), chunk.samples.begin(), chunk.samples.end());
      },
      [] {});
  return out;
}

}  // namespace hoodie::api
