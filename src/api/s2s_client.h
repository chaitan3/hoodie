#pragma once

#include <functional>
#include <string>

#include "audio/audio_format.h"
#include "net/http.h"
#include "util/cancel.h"

namespace hoodie::api {

struct S2sTextDelta {
  std::string text;
  bool is_final = false;
};

// End-to-end speech-to-speech client. The input is one completed utterance;
// the implementation streams text and 24 kHz audio back as it arrives.
class S2sClient {
 public:
  virtual ~S2sClient() = default;

  virtual void run(const audio::AudioFrame& audio, const util::CancelToken& cancel,
                   std::function<void(const S2sTextDelta&)> on_text,
                   std::function<void(const audio::AudioFrame&)> on_audio,
                   std::function<void()> on_done) = 0;

  virtual std::string description() const = 0;
};

// audio.cpp's LFM3-Audio S2S route. Raw PCM is uploaded with chunked
// Transfer-Encoding and the response is SSE with interleaved text and
// base64-encoded 24 kHz PCM audio.
class AudioCppS2sClient : public S2sClient {
 public:
  AudioCppS2sClient(net::HttpClient& http, std::string url, std::string model,
                    std::string api_key);

  void run(const audio::AudioFrame& audio, const util::CancelToken& cancel,
           std::function<void(const S2sTextDelta&)> on_text,
           std::function<void(const audio::AudioFrame&)> on_audio,
           std::function<void()> on_done) override;

  std::string description() const override;

 private:
  net::HttpClient& http_;
  std::string url_;
  std::string model_;
  std::string api_key_;
};

}  // namespace hoodie::api
