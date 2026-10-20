#include "api/asr_client.h"

#include <stdexcept>
#include <utility>
#include <vector>

#include "api/api_util.h"
#include "api/openai_types.h"
#include "audio/wav.h"

namespace hoodie::api {

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
