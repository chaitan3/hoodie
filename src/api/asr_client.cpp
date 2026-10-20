#include "api/asr_client.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdint>
#include <exception>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "api/api_error.h"
#include "api/api_util.h"
#include "api/openai_types.h"
#include "audio/resampler.h"
#include "audio/wav.h"
#include "net/sse.h"

namespace hoodie::api {

// ---------------------------------------------------------------------------
// Buffered
// ---------------------------------------------------------------------------

BufferedAsrClient::BufferedAsrClient(net::HttpClient& http, std::string url, std::string model,
                                     std::string api_key)
    : http_(http), url_(std::move(url)), model_(std::move(model)), api_key_(std::move(api_key)) {}

AsrCapabilities BufferedAsrClient::capabilities() const {
  return AsrCapabilities{/*partials=*/false, /*live_ingest=*/false};
}

void BufferedAsrClient::start(const util::CancelToken& cancel,
                              std::function<void(const AsrDelta&)> on_delta) {
  cancel_ = cancel;
  on_delta_ = std::move(on_delta);
  utterance_ = audio::AudioFrame{};
}

void BufferedAsrClient::push(const audio::AudioFrame& audio) {
  if (audio.empty()) return;
  if (utterance_.empty()) {
    utterance_.sample_rate = audio.sample_rate;
    utterance_.channels = audio.channels;
  } else if (utterance_.sample_rate != audio.sample_rate ||
             utterance_.channels != audio.channels) {
    throw std::runtime_error("asr: push() audio format changed mid-utterance");
  }
  utterance_.samples.insert(utterance_.samples.end(), audio.samples.begin(), audio.samples.end());
}

void BufferedAsrClient::commit() {
  if (!on_delta_) return;
  if (utterance_.empty()) {
    on_delta_(AsrDelta{"", /*is_final=*/true});
    return;
  }

  const std::vector<uint8_t> wav = audio::encode_wav_pcm16(utterance_);

  std::vector<net::MultipartField> fields;
  net::MultipartField model_field;
  model_field.name = "model";
  model_field.value = model_;
  fields.push_back(std::move(model_field));

  net::MultipartField file_field;
  file_field.name = "file";
  file_field.filename = "audio.wav";
  file_field.content_type = "audio/wav";
  file_field.data = &wav;
  fields.push_back(std::move(file_field));

  const net::HttpResult result =
      http_.post_multipart(url_, bearer_headers(api_key_), fields, cancel_);
  throw_if_http_error(result, "asr");
  on_delta_(AsrDelta{parse_transcription_response(result.body_string()), /*is_final=*/true});
}

void BufferedAsrClient::stop() {
  utterance_ = audio::AudioFrame{};
  on_delta_ = nullptr;
  cancel_ = util::CancelToken{};
}

std::string BufferedAsrClient::description() const {
  return "buffered ASR (" + model_ + " @ " + url_ + ")";
}

// ---------------------------------------------------------------------------
// Live ingest
// ---------------------------------------------------------------------------

LiveAsrClient::LiveAsrClient(net::HttpClient& http, std::string url, std::string model,
                             std::string api_key, int sample_rate, int channels)
    : http_(http),
      url_(std::move(url)),
      model_(std::move(model)),
      api_key_(std::move(api_key)),
      sample_rate_(sample_rate),
      channels_(channels) {
  const char* separator = url_.find('?') == std::string::npos ? "?" : "&";
  url_ += std::string(separator) + "model=" + model_ +
          "&sample_rate=" + std::to_string(sample_rate_) +
          "&channels=" + std::to_string(channels_) + "&sample_format=s16le";
}

LiveAsrClient::~LiveAsrClient() { stop(); }

AsrCapabilities LiveAsrClient::capabilities() const {
  return AsrCapabilities{/*partials=*/true, /*live_ingest=*/true};
}

void LiveAsrClient::start(const util::CancelToken& cancel,
                          std::function<void(const AsrDelta&)> on_delta) {
  stop();  // retire any previous utterance
  cancel_ = cancel;
  request_cancel_ = util::CancelToken{};
  on_delta_ = std::move(on_delta);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    pcm_.clear();
    input_done_ = false;
    stopped_ = false;
    final_delivered_ = false;
    error_ = nullptr;
    stream_error_.clear();
    partial_text_.clear();
  }
  worker_ = std::thread([this] { worker_loop(); });
}

void LiveAsrClient::push(const audio::AudioFrame& audio) {
  if (audio.empty()) return;

  audio::AudioFrame mono = audio::to_mono(audio);
  if (mono.sample_rate != sample_rate_) {
    mono = audio::resample_frame(mono, sample_rate_);
  }
  if (mono.empty()) return;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_ || input_done_) return;
    const auto* bytes = reinterpret_cast<const uint8_t*>(mono.samples.data());
    pcm_.insert(pcm_.end(), bytes,
                bytes + mono.samples.size() * static_cast<std::size_t>(sizeof(int16_t)));
  }
  cv_.notify_one();
}

void LiveAsrClient::commit() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    input_done_ = true;
  }
  cv_.notify_all();

  if (worker_.joinable()) worker_.join();

  if (error_) std::rethrow_exception(error_);
  if (!stream_error_.empty()) {
    throw ApiError(0, "asr live: " + stream_error_);
  }
  if (!final_delivered_ && on_delta_) {
    on_delta_(AsrDelta{"", /*is_final=*/true});
    final_delivered_ = true;
  }
}

void LiveAsrClient::stop() {
  request_cancel_.cancel();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopped_ = true;
    input_done_ = true;
    pcm_.clear();
  }
  cv_.notify_all();

  if (worker_.joinable()) worker_.join();

  on_delta_ = nullptr;
  cancel_ = util::CancelToken{};
  request_cancel_ = util::CancelToken{};
}

std::size_t LiveAsrClient::read_pcm(uint8_t* buffer, std::size_t capacity) {
  std::unique_lock<std::mutex> lock(mutex_);
  cv_.wait(lock, [this] { return stopped_ || input_done_ || !pcm_.empty(); });
  if (stopped_ || pcm_.empty()) return 0;

  const std::size_t count = std::min(capacity, pcm_.size());
  std::copy(pcm_.begin(),
            pcm_.begin() + static_cast<std::ptrdiff_t>(count), buffer);
  pcm_.erase(pcm_.begin(), pcm_.begin() + static_cast<std::ptrdiff_t>(count));
  return count;
}

void LiveAsrClient::handle_event(const std::string& event_name, const std::string& data) {
  if (data.empty()) return;

  if (data == "[DONE]") {
    if (!final_delivered_ && on_delta_) {
      on_delta_(AsrDelta{"", /*is_final=*/true});
      final_delivered_ = true;
    }
    return;
  }
  if (!stream_error_.empty()) return;

  nlohmann::json payload;
  try {
    payload = nlohmann::json::parse(data);
  } catch (const std::exception& e) {
    spdlog::warn("asr live: ignoring malformed SSE payload: {}", e.what());
    return;
  }

  if (payload.contains("error")) {
    stream_error_ = payload["error"].is_string() ? payload["error"].get<std::string>()
                                                 : payload["error"].dump();
    return;
  }

  std::string type = event_name;
  if (payload.contains("type") && payload["type"].is_string()) {
    type = payload["type"].get<std::string>();
  }

  if (type == "transcript.text.delta") {
    std::string delta;
    if (payload.contains("delta") && payload["delta"].is_string()) {
      delta = payload["delta"].get<std::string>();
    } else if (payload.contains("text") && payload["text"].is_string()) {
      delta = payload["text"].get<std::string>();
    }
    if (!delta.empty()) {
      partial_text_ += delta;
      if (on_delta_) on_delta_(AsrDelta{partial_text_, /*is_final=*/false});
    }
  } else if (type == "transcript.text.done") {
    std::string text;
    if (payload.contains("text") && payload["text"].is_string()) {
      text = payload["text"].get<std::string>();
    } else if (payload.contains("transcript") && payload["transcript"].is_string()) {
      text = payload["transcript"].get<std::string>();
    }
    if (on_delta_) on_delta_(AsrDelta{text, /*is_final=*/true});
    final_delivered_ = true;
  }
}

void LiveAsrClient::worker_loop() {
  net::Headers headers = bearer_headers(api_key_);
  headers.emplace_back("Expect", "");
  headers.emplace_back("Content-Type", "application/octet-stream");

  net::SseParser parser;
  const auto on_event = [this](const net::SseEvent& event) {
    handle_event(event.event, event.data);
  };
  const net::HttpResult result = http_.post_duplex(
      url_, headers,
      [this](uint8_t* buffer, std::size_t capacity) { return read_pcm(buffer, capacity); },
      [&](const uint8_t* data, std::size_t size) { parser.feed(data, size, on_event); },
      request_cancel_);
  parser.finish(on_event);

  if (result.cancelled) return;
  if (!result.ok()) {
    std::string detail;
    if (result.status != 0) detail = "HTTP " + std::to_string(result.status);
    if (!result.error.empty()) {
      if (!detail.empty()) detail += ": ";
      detail += result.error;
    }
    if (detail.empty()) detail = "request failed";
    std::string body = result.body_string();
    for (char& c : body) {
      if (c == '\n' || c == '\r') c = ' ';
    }
    if (!body.empty()) {
      detail += " - ";
      detail += body.substr(0, 512);
    }
    if (body.find("incrementally delivered body") != std::string::npos) {
      detail +=
          " (a reverse proxy is buffering the live upload; use a direct audio.cpp URL "
          "or disable proxy request buffering)";
    }
    std::lock_guard<std::mutex> lock(mutex_);
    error_ = std::make_exception_ptr(
        ApiError(result.status,
                 "asr live request failed at " + url_ + " (" + detail + ")",
                 result.body_string()));
  }
}

std::string LiveAsrClient::description() const {
  return "live ASR (" + model_ + " @ " + url_ + ")";
}

// ---------------------------------------------------------------------------
// Live with buffered fallback
// ---------------------------------------------------------------------------

FallbackAsrClient::FallbackAsrClient(std::unique_ptr<AsrClient> primary,
                                     std::unique_ptr<AsrClient> fallback)
    : primary_(std::move(primary)), fallback_(std::move(fallback)) {}

AsrCapabilities FallbackAsrClient::capabilities() const {
  return use_fallback_ ? fallback_->capabilities() : primary_->capabilities();
}

void FallbackAsrClient::forward_delta(const AsrDelta& delta) {
  if (delta.is_final) final_delivered_ = true;
  if (on_delta_) on_delta_(delta);
}

void FallbackAsrClient::start(const util::CancelToken& cancel,
                              std::function<void(const AsrDelta&)> on_delta) {
  cancel_ = cancel;
  on_delta_ = std::move(on_delta);
  buffered_.clear();
  final_delivered_ = false;
  fallback_started_ = false;

  const auto forward = [this](const AsrDelta& delta) { forward_delta(delta); };
  if (use_fallback_) {
    fallback_->start(cancel_, forward);
    fallback_started_ = true;
    return;
  }
  try {
    primary_->start(cancel_, forward);
  } catch (const std::exception& e) {
    spdlog::warn("asr: live start failed ({}); using buffered fallback", e.what());
    use_fallback_ = true;
    fallback_->start(cancel_, forward);
    fallback_started_ = true;
  }
}

void FallbackAsrClient::push(const audio::AudioFrame& audio) {
  buffered_.push_back(audio);
  if (use_fallback_) {
    if (fallback_started_) fallback_->push(audio);
    return;
  }
  try {
    primary_->push(audio);
  } catch (const std::exception& e) {
    spdlog::warn("asr: live push failed ({}); buffering for fallback", e.what());
    use_fallback_ = true;
  }
}

void FallbackAsrClient::switch_to_fallback() {
  use_fallback_ = true;
  if (fallback_started_) return;
  const auto forward = [this](const AsrDelta& delta) { forward_delta(delta); };
  fallback_->start(cancel_, forward);
  fallback_started_ = true;
  for (const audio::AudioFrame& frame : buffered_) fallback_->push(frame);
}

void FallbackAsrClient::commit() {
  if (use_fallback_) {
    switch_to_fallback();
    fallback_->commit();
    return;
  }
  try {
    primary_->commit();
  } catch (const std::exception& e) {
    if (final_delivered_) throw;  // a final was already delivered; don't duplicate
    spdlog::warn("asr: live commit failed ({}); replaying through buffered fallback", e.what());
    switch_to_fallback();
    fallback_->commit();
  }
}

void FallbackAsrClient::stop() {
  primary_->stop();
  if (fallback_started_) fallback_->stop();
  buffered_.clear();
  on_delta_ = nullptr;
}

std::string FallbackAsrClient::description() const {
  return primary_->description() + " with buffered fallback";
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::string transcribe_blocking(AsrClient& client, const audio::AudioFrame& audio,
                                const util::CancelToken& cancel) {
  std::string text;
  client.start(cancel, [&](const AsrDelta& delta) {
    if (delta.is_final) text = delta.text;
  });
  client.push(audio);
  client.commit();
  return text;
}

}  // namespace hoodie::api
