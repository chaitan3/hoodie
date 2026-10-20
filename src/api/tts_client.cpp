#include "api/tts_client.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>

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
