#pragma once

#include <functional>
#include <string>

#include "audio/audio_format.h"
#include "net/http.h"
#include "util/cancel.h"

namespace hoodie::api {

struct AsrDelta {
  std::string text;
  bool is_final = false;
};

struct AsrCapabilities {
  bool partials = false;     // text can arrive before the utterance ends
  bool live_ingest = false;  // push() streams audio while the user is speaking
};

// Streaming-first transcription interface.
//
// Contract: push() may be called any number of times; commit() ends the
// utterance and must not return until the final delta has been delivered.
// Buffered adapters accumulate push() and emit a single final delta; live
// adapters stream audio and emit partials while the user speaks.
class AsrClient {
 public:
  virtual ~AsrClient() = default;

  virtual AsrCapabilities capabilities() const = 0;

  virtual void start(const util::CancelToken& cancel,
                     std::function<void(const AsrDelta&)> on_delta) = 0;
  virtual void push(const audio::AudioFrame& audio) = 0;
  virtual void commit() = 0;
  virtual void stop() = 0;

  virtual std::string description() const = 0;
};

// Buffered floor: buffers push() frames, posts one WAV on commit(), and emits a
// single final delta. Reproduces the previous transcribe() behavior exactly.
class BufferedAsrClient : public AsrClient {
 public:
  BufferedAsrClient(net::HttpClient& http, std::string url, std::string model,
                    std::string api_key);

  AsrCapabilities capabilities() const override;
  void start(const util::CancelToken& cancel,
             std::function<void(const AsrDelta&)> on_delta) override;
  void push(const audio::AudioFrame& audio) override;
  void commit() override;
  void stop() override;
  std::string description() const override;

 private:
  net::HttpClient& http_;
  std::string url_;
  std::string model_;
  std::string api_key_;

  util::CancelToken cancel_;
  std::function<void(const AsrDelta&)> on_delta_;
  audio::AudioFrame utterance_;
};

// Convenience for callers that only want the final text (CLI/diagnostics).
std::string transcribe_blocking(AsrClient& client, const audio::AudioFrame& audio,
                                const util::CancelToken& cancel = {});

}  // namespace hoodie::api
