#include "api/asr_client.h"

#include <utility>
#include <vector>

#include "api/api_util.h"
#include "audio/wav.h"

namespace hoodie::api {

BufferedAsrClient::BufferedAsrClient(net::HttpClient& http, std::string url, std::string model,
                                     std::string api_key)
    : http_(http), url_(std::move(url)), model_(std::move(model)), api_key_(std::move(api_key)) {}

std::string BufferedAsrClient::transcribe(const audio::AudioFrame& audio,
                                          const util::CancelToken& cancel) {
  const std::vector<uint8_t> wav = audio::encode_wav_pcm16(audio);

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
      http_.post_multipart(url_, bearer_headers(api_key_), fields, cancel);
  throw_if_http_error(result, "asr");
  return parse_transcription_response(result.body_string());
}

std::string BufferedAsrClient::description() const {
  return "buffered ASR (" + model_ + " @ " + url_ + ")";
}

}  // namespace hoodie::api
