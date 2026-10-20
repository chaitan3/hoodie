#pragma once

#include <functional>
#include <string>

#include "audio/audio_format.h"
#include "net/http.h"
#include "util/cancel.h"

namespace hoodie::api {

// Streaming-first synthesis interface. synthesize() delivers one or more audio
// chunks, then on_done; the buffered floor delivers exactly one chunk holding
// the whole clip.
class TtsClient {
 public:
  virtual ~TtsClient() = default;

  virtual bool streaming() const = 0;

  virtual void synthesize(const std::string& text, const util::CancelToken& cancel,
                          std::function<void(const audio::AudioFrame&)> on_chunk,
                          std::function<void()> on_done) = 0;

  virtual std::string description() const = 0;
};

// Buffered floor: posts the whole text, decodes the whole body, then emits it as
// a single chunk. Matches the previous synthesize() behavior exactly.
class BufferedTtsClient : public TtsClient {
 public:
  BufferedTtsClient(net::HttpClient& http, std::string url, std::string model, std::string api_key,
                    std::string response_format);

  bool streaming() const override;
  void synthesize(const std::string& text, const util::CancelToken& cancel,
                  std::function<void(const audio::AudioFrame&)> on_chunk,
                  std::function<void()> on_done) override;
  std::string description() const override;

 private:
  net::HttpClient& http_;
  std::string url_;
  std::string model_;
  std::string api_key_;
  std::string response_format_;
};

// Convenience for callers that only want the complete clip (CLI/diagnostics).
audio::AudioFrame synthesize_blocking(TtsClient& client, const std::string& text,
                                      const util::CancelToken& cancel = {});

}  // namespace hoodie::api
