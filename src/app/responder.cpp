#include "app/responder.h"

#include <spdlog/spdlog.h>

#include <exception>
#include <utility>

#include "agent/text_normalize.h"
#include "audio/wav.h"

namespace hoodie::app {

Responder::Responder(api::TtsClient& tts, std::string out_file, std::string output_device)
    : tts_(tts), out_file_(std::move(out_file)), output_device_(std::move(output_device)) {}

Responder::~Responder() { abort(); }

void Responder::start(const util::CancelToken& cancel,
                      std::function<void()> on_first_audio) {
  cancel_ = cancel;
  on_first_audio_ = std::move(on_first_audio);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    finished_ = false;
    stopped_ = false;
    failed_ = false;
    error_.clear();
  }
  worker_ = std::thread([this] { worker_loop(); });
}

void Responder::push_text(const std::string& token) {
  splitter_.push(token, [this](const std::string& chunk) { enqueue(chunk); });
}

void Responder::finish() {
  splitter_.flush([this](const std::string& chunk) { enqueue(chunk); });
  {
    std::lock_guard<std::mutex> lock(mutex_);
    finished_ = true;
  }
  cv_.notify_all();

  if (worker_.joinable()) worker_.join();

  if (player_) {
    player_->wait_drained();
    player_->stop();
    player_.reset();
  }

  if (!out_file_.empty() && !collected_.samples.empty()) {
    audio::write_audio_file(out_file_, collected_);
    spdlog::info("wrote {} ({:.2f} s, {} Hz, {} ch)", out_file_, collected_.duration_seconds(),
                 collected_.sample_rate, collected_.channels);
  }
}

void Responder::abort() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopped_ = true;
    queue_.clear();
  }
  cv_.notify_all();
  if (worker_.joinable()) worker_.join();
  if (player_) {
    player_->stop();
    player_.reset();
  }
}

bool Responder::failed() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return failed_;
}

std::string Responder::error() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return error_;
}

void Responder::enqueue(std::string sentence) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_ || finished_) return;
    queue_.push_back(std::move(sentence));
  }
  cv_.notify_one();
}

void Responder::worker_loop() {
  for (;;) {
    std::string sentence;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [this] { return stopped_ || finished_ || !queue_.empty(); });
      if (stopped_) return;
      if (queue_.empty() && finished_) return;
      sentence = std::move(queue_.front());
      queue_.pop_front();
    }

    if (cancel_.cancelled()) return;

    try {
      const std::string spoken = agent::normalize_for_tts(sentence);
      if (spoken.empty()) continue;
      tts_.synthesize(
          spoken, cancel_,
          [this](const audio::AudioFrame& chunk) { handle_chunk(chunk); }, [] {});
    } catch (const std::exception& e) {
      std::lock_guard<std::mutex> lock(mutex_);
      failed_ = true;
      error_ = e.what();
      stopped_ = true;
      cv_.notify_all();
      return;
    }
  }
}

void Responder::handle_chunk(const audio::AudioFrame& chunk) {
  if (cancel_.cancelled()) return;

  std::function<void()> first_audio;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_) return;
    if (on_first_audio_) {
      first_audio = std::move(on_first_audio_);
      on_first_audio_ = nullptr;
    }
  }
  if (first_audio) {
    try {
      first_audio();
    } catch (const std::exception& e) {
      spdlog::warn("responder: first-audio callback failed: {}", e.what());
    }
  }

  if (!out_file_.empty()) {
    if (collected_.samples.empty()) {
      collected_.sample_rate = chunk.sample_rate;
      collected_.channels = chunk.channels;
    }
    collected_.samples.insert(collected_.samples.end(), chunk.samples.begin(),
                              chunk.samples.end());
    return;
  }

  // Never throw through the HTTP write callback that delivered this chunk.
  try {
    if (!player_) {
      audio::AudioPlayer::Options options;
      options.device = output_device_;
      options.sample_rate = 0;  // device default
      options.channels = 1;
      player_ = std::make_unique<audio::AudioPlayer>(options);
      player_->start();
    }
    player_->enqueue(chunk);
  } catch (const std::exception& e) {
    std::lock_guard<std::mutex> lock(mutex_);
    failed_ = true;
    error_ = e.what();
    stopped_ = true;
    cv_.notify_all();
  }
}

}  // namespace hoodie::app
