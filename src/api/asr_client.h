#pragma once

#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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

// Live ingest: chunked PCM up, SSE down, on one full-duplex HTTP connection.
// push() enqueues 16 kHz mono PCM; a worker thread uploads it while parsing
// transcript.text.delta / .done events.
class LiveAsrClient : public AsrClient {
 public:
  LiveAsrClient(net::HttpClient& http, std::string url, std::string model, std::string api_key,
                int sample_rate = 16000, int channels = 1);
  ~LiveAsrClient() override;

  AsrCapabilities capabilities() const override;
  void start(const util::CancelToken& cancel,
             std::function<void(const AsrDelta&)> on_delta) override;
  void push(const audio::AudioFrame& audio) override;
  void commit() override;
  void stop() override;
  std::string description() const override;

 private:
  void worker_loop();
  std::size_t read_pcm(uint8_t* buffer, std::size_t capacity);
  void handle_event(const std::string& event_name, const std::string& data);

  net::HttpClient& http_;
  std::string url_;
  std::string model_;
  std::string api_key_;
  int sample_rate_ = 16000;
  int channels_ = 1;

  util::CancelToken cancel_;
  util::CancelToken request_cancel_;
  std::function<void(const AsrDelta&)> on_delta_;

  std::thread worker_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<uint8_t> pcm_;
  bool input_done_ = false;
  bool stopped_ = false;
  bool final_delivered_ = false;
  std::exception_ptr error_;
  std::string stream_error_;
  std::string partial_text_;  // cumulative partial transcript
};

// Live ASR with an automatic buffered fallback. Every pushed frame is retained
// so a failure before the final transcript can replay the same utterance
// through the fallback (safe: nothing downstream has run yet).
class FallbackAsrClient : public AsrClient {
 public:
  FallbackAsrClient(std::unique_ptr<AsrClient> primary, std::unique_ptr<AsrClient> fallback);

  AsrCapabilities capabilities() const override;
  void start(const util::CancelToken& cancel,
             std::function<void(const AsrDelta&)> on_delta) override;
  void push(const audio::AudioFrame& audio) override;
  void commit() override;
  void stop() override;
  std::string description() const override;

 private:
  void switch_to_fallback();
  void forward_delta(const AsrDelta& delta);

  std::unique_ptr<AsrClient> primary_;
  std::unique_ptr<AsrClient> fallback_;
  bool use_fallback_ = false;
  bool fallback_started_ = false;
  bool final_delivered_ = false;
  util::CancelToken cancel_;
  std::function<void(const AsrDelta&)> on_delta_;
  std::vector<audio::AudioFrame> buffered_;
};

// Convenience for callers that only want the final text (CLI/diagnostics).
std::string transcribe_blocking(AsrClient& client, const audio::AudioFrame& audio,
                                const util::CancelToken& cancel = {});

}  // namespace hoodie::api
