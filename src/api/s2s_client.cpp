#include "api/s2s_client.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "api/api_error.h"
#include "api/api_util.h"
#include "audio/resampler.h"
#include "net/sse.h"

namespace hoodie::api {
namespace {

constexpr int kInputSampleRate = 16000;
constexpr int kOutputSampleRate = 24000;

int base64_value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+' || c == '-') return 62;
  if (c == '/' || c == '_') return 63;
  return -1;
}

std::vector<uint8_t> decode_base64(const std::string& input) {
  std::vector<uint8_t> out;
  int accumulator = 0;
  int bits = 0;
  for (unsigned char c : input) {
    if (c == '=') break;
    const int value = base64_value(static_cast<char>(c));
    if (value < 0) continue;  // tolerate whitespace and newlines
    accumulator = (accumulator << 6) | value;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<uint8_t>((accumulator >> bits) & 0xff));
    }
  }
  return out;
}

std::string strip_data_uri_prefix(std::string data) {
  const std::string marker = ";base64,";
  const std::size_t pos = data.find(marker);
  if (pos != std::string::npos) data.erase(0, pos + marker.size());
  return data;
}

}  // namespace

AudioCppS2sClient::AudioCppS2sClient(net::HttpClient& http, std::string url, std::string model,
                                     std::string api_key)
    : http_(http), url_(std::move(url)), model_(std::move(model)), api_key_(std::move(api_key)) {
  const char* separator = url_.find('?') == std::string::npos ? "?" : "&";
  url_ += std::string(separator) + "model=" + model_ +
          "&sample_rate=" + std::to_string(kInputSampleRate) +
          "&channels=1&sample_format=s16le&return_text=true";
}

void AudioCppS2sClient::run(const audio::AudioFrame& audio, const util::CancelToken& cancel,
                            std::function<void(const S2sTextDelta&)> on_text,
                            std::function<void(const audio::AudioFrame&)> on_audio,
                            std::function<void()> on_done) {
  audio::AudioFrame mono = audio::to_mono(audio);
  if (mono.sample_rate != kInputSampleRate) {
    mono = audio::resample_frame(mono, kInputSampleRate);
  }
  if (mono.empty()) throw std::runtime_error("s2s: empty input audio");

  const auto* bytes = reinterpret_cast<const uint8_t*>(mono.samples.data());
  const std::size_t byte_count = mono.samples.size() * sizeof(int16_t);
  std::size_t offset = 0;
  std::string stream_error;

  net::SseParser parser;
  const auto handle_event = [&](const net::SseEvent& event) {
    if (event.data.empty()) return;
    if (event.data == "[DONE]") return;
    if (!stream_error.empty()) return;

    nlohmann::json payload;
    try {
      payload = nlohmann::json::parse(event.data);
    } catch (const std::exception& e) {
      spdlog::warn("s2s: ignoring malformed SSE payload: {}", e.what());
      return;
    }
    if (payload.contains("error")) {
      if (payload["error"].is_string()) {
        stream_error = payload["error"].get<std::string>();
      } else if (payload["error"].is_object() && payload["error"].contains("message") &&
                 payload["error"]["message"].is_string()) {
        stream_error = payload["error"]["message"].get<std::string>();
      } else {
        stream_error = payload["error"].dump();
      }
      return;
    }

    std::string type = event.event;
    if (payload.contains("type") && payload["type"].is_string()) {
      type = payload["type"].get<std::string>();
    }

    try {
      if (type == "speech.text.delta") {
        std::string delta;
        if (payload.contains("delta") && payload["delta"].is_string()) {
          delta = payload["delta"].get<std::string>();
        } else if (payload.contains("text") && payload["text"].is_string()) {
          delta = payload["text"].get<std::string>();
        }
        if (!delta.empty() && on_text) on_text(S2sTextDelta{delta, /*is_final=*/false});
      } else if (type == "speech.text.done") {
        std::string text;
        if (payload.contains("text") && payload["text"].is_string()) {
          text = payload["text"].get<std::string>();
        }
        if (!text.empty() && on_text) on_text(S2sTextDelta{text, /*is_final=*/true});
      } else if (type == "speech.audio.delta") {
        if (!payload.contains("audio") || !payload["audio"].is_string()) return;
        const std::vector<uint8_t> pcm =
            decode_base64(strip_data_uri_prefix(payload["audio"].get<std::string>()));
        if (pcm.empty()) return;
        const std::size_t samples = pcm.size() / sizeof(int16_t);
        if (samples == 0) return;
        audio::AudioFrame frame;
        frame.sample_rate = kOutputSampleRate;
        frame.channels = 1;
        frame.samples.resize(samples);
        std::memcpy(frame.samples.data(), pcm.data(), samples * sizeof(int16_t));
        if (on_audio) on_audio(frame);
      }
    } catch (const std::exception& e) {
      stream_error = std::string("s2s: callback failed: ") + e.what();
    }
  };

  net::Headers headers = bearer_headers(api_key_);
  headers.emplace_back("Expect", "");
  headers.emplace_back("Content-Type", "application/octet-stream");

  const net::HttpResult result = http_.post_duplex(
      url_, headers,
      [&](uint8_t* buffer, std::size_t capacity) {
        if (offset >= byte_count) return std::size_t{0};
        const std::size_t count = std::min(capacity, byte_count - offset);
        std::memcpy(buffer, bytes + offset, count);
        offset += count;
        return count;
      },
      [&](const uint8_t* data, std::size_t size) {
        parser.feed(data, size, [&](const net::SseEvent& event) { handle_event(event); });
      },
      cancel);
  parser.finish([&](const net::SseEvent& event) { handle_event(event); });

  throw_if_http_error(result, "s2s");
  if (!stream_error.empty()) throw ApiError(0, "s2s: " + stream_error);
  if (on_done) on_done();
}

std::string AudioCppS2sClient::description() const {
  return "LFM3 S2S (" + model_ + " @ " + url_ + ")";
}

}  // namespace hoodie::api
