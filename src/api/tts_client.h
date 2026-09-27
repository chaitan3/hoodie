#pragma once

#include <string>

#include "audio/audio_format.h"
#include "net/http.h"
#include "util/cancel.h"

namespace hoodie::api {

// Synthesises speech. Streaming implementations will deliver audio chunks via
// a callback later; this abstraction returns the full decoded clip.
class TtsClient {
 public:
  virtual ~TtsClient() = default;

  virtual audio::AudioFrame synthesize(const std::string& text,
                                       const util::CancelToken& cancel = {}) = 0;
  virtual std::string description() const = 0;
};

// Buffered floor: posts to /v1/audio/speech and decodes the whole response.
// Requests `response_format` when configured (default "pcm"); falls back to
// container auto-detection so an mp3/wav default still works.
class BufferedTtsClient : public TtsClient {
 public:
  BufferedTtsClient(net::HttpClient& http, std::string url, std::string model, std::string api_key,
                    std::string response_format);

  audio::AudioFrame synthesize(const std::string& text,
                               const util::CancelToken& cancel = {}) override;
  std::string description() const override;

 private:
  net::HttpClient& http_;
  std::string url_;
  std::string model_;
  std::string api_key_;
  std::string response_format_;
};

}  // namespace hoodie::api
