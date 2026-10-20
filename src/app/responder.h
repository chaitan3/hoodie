#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "agent/sentence_splitter.h"
#include "api/tts_client.h"
#include "audio/audio_format.h"
#include "audio/player.h"
#include "util/cancel.h"

namespace hoodie::app {

// Streams LLM text through the sentence splitter into TTS, and TTS chunks into
// the player (or an output WAV). A single TTS worker preserves sentence order.
class Responder {
 public:
  Responder(api::TtsClient& tts, std::string out_file, std::string output_device);
  ~Responder();

  Responder(const Responder&) = delete;
  Responder& operator=(const Responder&) = delete;

  // on_first_audio runs once, when the first chunk is ready to play/write.
  void start(const util::CancelToken& cancel, std::function<void()> on_first_audio);
  void push_text(const std::string& token);
  void finish();
  void abort();

  bool failed() const;
  std::string error() const;

 private:
  void enqueue(std::string sentence);
  void worker_loop();
  void handle_chunk(const audio::AudioFrame& chunk);

  api::TtsClient& tts_;
  std::string out_file_;
  std::string output_device_;
  util::CancelToken cancel_;
  agent::SentenceSplitter splitter_;
  std::function<void()> on_first_audio_;

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::string> queue_;
  bool finished_ = false;
  bool stopped_ = false;
  bool failed_ = false;
  std::string error_;
  std::thread worker_;

  std::unique_ptr<audio::AudioPlayer> player_;
  audio::AudioFrame collected_;
};

}  // namespace hoodie::app
