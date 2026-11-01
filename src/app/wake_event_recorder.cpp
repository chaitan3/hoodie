#include "app/wake_event_recorder.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>

#include "audio/audio_format.h"
#include "audio/wav.h"

namespace hoodie::app {
namespace {

constexpr std::size_t kDetectorSamples = 1280;  // 80 ms at 16 kHz
constexpr int kSamplesPerMs = 16;               // 16 kHz

std::string utc_timestamp() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t value = std::chrono::system_clock::to_time_t(now);
  std::tm utc{};
  gmtime_r(&value, &utc);
  char buffer[32];
  std::strftime(buffer, sizeof(buffer), "%Y%m%dT%H%M%SZ", &utc);
  return buffer;
}

std::size_t ms_to_samples(int ms) {
  return std::max(0, ms) * static_cast<std::size_t>(kSamplesPerMs);
}

}  // namespace

WakeEventRecorder::WakeEventRecorder(WakeEventRecorderOptions options)
    : options_(std::move(options)) {
  if (options_.pre_roll_ms < 0) options_.pre_roll_ms = 0;
  if (options_.post_roll_ms < 0) options_.post_roll_ms = 0;

  if (!options_.directory.empty()) {
    std::error_code error;
    std::filesystem::create_directories(options_.directory, error);
    if (error) {
      spdlog::error("wake-events: cannot create '{}': {}", options_.directory,
                    error.message());
      options_.directory.clear();
    }
  }

  worker_ = std::thread([this] { worker_loop(); });
}

WakeEventRecorder::~WakeEventRecorder() {
  finalize_active();
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    stopping_ = true;
  }
  queue_cv_.notify_all();
  if (worker_.joinable()) worker_.join();
}

void WakeEventRecorder::set_capture_info(int sample_rate, int channels, std::string device) {
  options_.capture_sample_rate = sample_rate;
  options_.capture_channels = channels;
  options_.input_device = std::move(device);
}

void WakeEventRecorder::push_audio(const int16_t* samples, std::size_t frames) {
  if (frames == 0) return;

  total_samples_ += frames;
  audio_history_.insert(audio_history_.end(), samples, samples + frames);
  const std::size_t pre_roll_samples = ms_to_samples(options_.pre_roll_ms);
  while (audio_history_.size() > pre_roll_samples) audio_history_.pop_front();

  if (!active_) return;
  active_->audio.insert(active_->audio.end(), samples, samples + frames);
  if (active_->post_remaining > frames) {
    active_->post_remaining -= frames;
  } else {
    active_->post_remaining = 0;
    active_->complete = true;
    finalize_active();
  }
}

void WakeEventRecorder::push_score(float score, bool armed, int consecutive) {
  ++score_frame_;
  Score sample;
  sample.frame = score_frame_;
  sample.score = score;
  sample.armed = armed;
  sample.consecutive = consecutive;

  score_history_.push_back(sample);
  const std::size_t max_scores =
      ms_to_samples(options_.pre_roll_ms) / kDetectorSamples + 2;
  while (score_history_.size() > max_scores) score_history_.pop_front();

  if (active_) active_->scores.push_back(sample);
}

void WakeEventRecorder::on_trigger(float score) {
  if (options_.directory.empty() || active_) return;

  Event event;
  event.id = next_event_id();
  event.created_at = utc_timestamp();
  event.trigger_score = score;
  event.trigger_frame = score_frame_;
  event.audio.assign(audio_history_.begin(), audio_history_.end());
  event.scores.assign(score_history_.begin(), score_history_.end());
  event.pre_roll_samples = event.audio.size();
  event.audio_start_sample =
      total_samples_ >= event.pre_roll_samples ? total_samples_ - event.pre_roll_samples : 0;
  event.trigger_sample =
      event.trigger_frame > 0 ? (event.trigger_frame - 1) * kDetectorSamples : 0;
  event.post_remaining = ms_to_samples(options_.post_roll_ms);

  active_ = std::make_unique<Event>(std::move(event));
  if (active_->post_remaining == 0) {
    active_->complete = true;
    finalize_active();
  }
}

void WakeEventRecorder::flush() { finalize_active(); }

void WakeEventRecorder::finalize_active() {
  if (!active_) return;
  enqueue(std::move(*active_));
  active_.reset();
}

void WakeEventRecorder::enqueue(Event event) {
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if (queue_.size() >= kMaxQueuedEvents) {
      spdlog::warn("wake-events: queue full; dropping oldest event");
      queue_.pop_front();
    }
    queue_.push_back(std::move(event));
  }
  queue_cv_.notify_one();
}

void WakeEventRecorder::worker_loop() {
  while (true) {
    Event event;
    {
      std::unique_lock<std::mutex> lock(queue_mutex_);
      queue_cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
      if (stopping_ && queue_.empty()) return;
      event = std::move(queue_.front());
      queue_.pop_front();
    }
    try {
      write_event(event);
    } catch (const std::exception& error) {
      spdlog::error("wake-events: failed to write '{}': {}", event.id, error.what());
    } catch (...) {
      spdlog::error("wake-events: failed to write '{}'", event.id);
    }
  }
}

void WakeEventRecorder::write_event(const Event& event) {
  if (options_.directory.empty()) return;

  const std::filesystem::path root(options_.directory);
  const std::filesystem::path directory = root / event.id;
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error) {
    spdlog::error("wake-events: cannot create '{}': {}", directory.string(),
                  error.message());
    return;
  }

  const std::filesystem::path audio_path = directory / "audio.wav";
  audio::AudioFrame frame;
  frame.sample_rate = audio::kSampleRate16k;
  frame.channels = 1;
  frame.samples = event.audio;
  audio::write_audio_file(audio_path.string(), frame);

  const double trigger_time =
      event.trigger_frame > 0
          ? static_cast<double>(event.trigger_frame - 1) * kDetectorSamples /
                audio::kSampleRate16k
          : 0.0;
  const double trigger_offset =
      event.trigger_sample >= event.audio_start_sample
          ? static_cast<double>(event.trigger_sample - event.audio_start_sample) /
                audio::kSampleRate16k
          : 0.0;

  nlohmann::json root_json;
  root_json["schema"] = 1;
  root_json["id"] = event.id;
  root_json["created_at"] = event.created_at;
  root_json["accepted"] = true;
  root_json["post_roll_complete"] = event.complete;
  root_json["trigger"] = {
      {"score", event.trigger_score},
      {"frame", event.trigger_frame},
      {"time_s", trigger_time},
      {"offset_s", trigger_offset},
      {"threshold", options_.threshold},
      {"min_consecutive_frames", options_.min_consecutive_frames},
  };
  root_json["audio"] = {
      {"file", "audio.wav"},
      {"sample_rate", audio::kSampleRate16k},
      {"channels", 1},
      {"samples", event.audio.size()},
      {"duration_s", static_cast<double>(event.audio.size()) / audio::kSampleRate16k},
      {"pre_roll_s", static_cast<double>(event.pre_roll_samples) / audio::kSampleRate16k},
      {"trigger_offset_s", trigger_offset},
  };
  root_json["wakeword"] = {
      {"keyword", options_.keyword},
      {"threshold", options_.threshold},
      {"min_consecutive_frames", options_.min_consecutive_frames},
      {"warmup_frames", options_.warmup_frames},
      {"require_real_context", options_.require_real_context},
      {"melspectrogram_model", options_.melspectrogram_model},
      {"embedding_model", options_.embedding_model},
      {"keyword_model", options_.keyword_model},
  };
  root_json["capture"] = {
      {"input_device", options_.input_device},
      {"sample_rate", options_.capture_sample_rate},
      {"channels", options_.capture_channels},
  };

  nlohmann::json scores = nlohmann::json::array();
  for (const Score& score : event.scores) {
    scores.push_back({
        {"frame", score.frame},
        {"t", score.frame > 0
                  ? static_cast<double>(score.frame - 1) * kDetectorSamples /
                        audio::kSampleRate16k
                  : 0.0},
        {"score", score.score},
        {"armed", score.armed},
        {"consecutive", score.consecutive},
    });
  }
  root_json["scores"] = std::move(scores);

  const std::filesystem::path json_path = directory / "event.json";
  const std::filesystem::path temporary = directory / "event.json.tmp";
  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out) {
      spdlog::error("wake-events: cannot write '{}'", temporary.string());
      return;
    }
    out << root_json.dump(2) << '\n';
    if (!out) {
      spdlog::error("wake-events: failed writing '{}'", temporary.string());
      return;
    }
  }
  std::filesystem::rename(temporary, json_path, error);
  if (error) {
    spdlog::error("wake-events: cannot finalize '{}': {}", json_path.string(),
                  error.message());
    return;
  }

  spdlog::info("wake-events: saved {} (score {:.2f}, {:.2f}s)",
               event.id, event.trigger_score,
               static_cast<double>(event.audio.size()) / audio::kSampleRate16k);
  prune();
}

void WakeEventRecorder::prune() {
  if (options_.max_events <= 0 || options_.directory.empty()) return;

  std::vector<std::filesystem::path> events;
  std::error_code error;
  for (const auto& entry :
       std::filesystem::directory_iterator(options_.directory, error)) {
    if (error) break;
    if (!entry.is_directory()) continue;
    const std::string name = entry.path().filename().string();
    if (name.rfind("wake-", 0) == 0) events.push_back(entry.path());
  }
  if (error) {
    spdlog::warn("wake-events: cannot scan '{}': {}", options_.directory,
                 error.message());
    return;
  }

  std::sort(events.begin(), events.end());
  const std::size_t limit = static_cast<std::size_t>(options_.max_events);
  for (std::size_t i = 0; i + limit < events.size(); ++i) {
    std::filesystem::remove_all(events[i], error);
    if (error) {
      spdlog::warn("wake-events: cannot remove '{}': {}", events[i].string(),
                   error.message());
      error.clear();
    }
  }
}

std::string WakeEventRecorder::next_event_id() {
  const std::string base = "wake-" + utc_timestamp();
  for (;;) {
    ++sequence_;
    const std::string id = base + "-" + std::to_string(sequence_);
    std::error_code error;
    if (!std::filesystem::exists(std::filesystem::path(options_.directory) / id, error)) {
      return id;
    }
  }
}

}  // namespace hoodie::app
