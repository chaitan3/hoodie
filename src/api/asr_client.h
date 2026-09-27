#pragma once

#include <string>

#include "audio/audio_format.h"
#include "net/http.h"
#include "util/cancel.h"

namespace hoodie::api {

// Transcribes a complete utterance. Streaming implementations will expose
// incremental results via the same abstraction later (see MIGRATION.md §6).
class AsrClient {
 public:
  virtual ~AsrClient() = default;

  virtual std::string transcribe(const audio::AudioFrame& audio,
                                 const util::CancelToken& cancel = {}) = 0;
  virtual std::string description() const = 0;
};

// Buffered floor: posts a complete WAV to /v1/audio/transcriptions and reads
// the JSON {"text": ...} response. Matches the current Python client exactly.
class BufferedAsrClient : public AsrClient {
 public:
  BufferedAsrClient(net::HttpClient& http, std::string url, std::string model,
                    std::string api_key);

  std::string transcribe(const audio::AudioFrame& audio,
                         const util::CancelToken& cancel = {}) override;
  std::string description() const override;

 private:
  net::HttpClient& http_;
  std::string url_;
  std::string model_;
  std::string api_key_;
};

}  // namespace hoodie::api
