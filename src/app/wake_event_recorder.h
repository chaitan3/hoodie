#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hoodie::app {

struct WakeEventRecorderOptions {
  std::string directory;
  int pre_roll_ms = 3000;
  int post_roll_ms = 5000;
  int max_events = 100;
  std::string keyword;
  float threshold = 0.75f;
  int min_consecutive_frames = 2;
  int warmup_frames = 5;
  bool require_real_context = true;
  std::string melspectrogram_model;
  std::string embedding_model;
  std::string keyword_model;
  std::string input_device;
  int capture_sample_rate = 48000;
  int capture_channels = 1;
};

// Saves accepted wake detections for offline analysis. The recorder keeps a
// rolling 16 kHz pre-roll, then captures a configurable post-roll. Files are
// written by a worker thread so disk I/O never blocks the audio loop.
class WakeEventRecorder {
 public:
  explicit WakeEventRecorder(WakeEventRecorderOptions options);
  ~WakeEventRecorder();

  WakeEventRecorder(const WakeEventRecorder&) = delete;
  WakeEventRecorder& operator=(const WakeEventRecorder&) = delete;

  // Update the capture metadata after the real device/file is known.
  void set_capture_info(int sample_rate, int channels, std::string device);

  // Feed the same 16 kHz mono audio that was passed to the wake detector.
  void push_audio(const int16_t* samples, std::size_t frames);

  // Feed one completed 80 ms detector score. `armed` and `consecutive` describe
  // the gate state after this score.
  void push_score(float score, bool armed, int consecutive);

  // Begin recording an accepted detection. The pre-roll already contains the
  // audio that produced the trigger.
  void on_trigger(float score);

  // Finalize a partial event, for example during shutdown.
  void flush();

 private:
  struct Score {
    std::uint64_t frame = 0;
    float score = 0.0f;
    bool armed = true;
    int consecutive = 0;
  };

  struct Event {
    std::string id;
    std::string created_at;
    float trigger_score = 0.0f;
    std::uint64_t trigger_frame = 0;
    std::uint64_t audio_start_sample = 0;
    std::uint64_t trigger_sample = 0;
    std::size_t pre_roll_samples = 0;
    std::size_t post_remaining = 0;
    bool complete = false;
    std::vector<int16_t> audio;
    std::vector<Score> scores;
  };

  void finalize_active();
  void enqueue(Event event);
  void worker_loop();
  void write_event(const Event& event);
  void prune();
  std::string next_event_id();

  WakeEventRecorderOptions options_;
  std::deque<int16_t> audio_history_;
  std::deque<Score> score_history_;
  std::uint64_t total_samples_ = 0;
  std::uint64_t score_frame_ = 0;
  std::uint64_t sequence_ = 0;
  std::unique_ptr<Event> active_;

  static constexpr std::size_t kMaxQueuedEvents = 8;

  std::thread worker_;
  std::mutex queue_mutex_;
  std::condition_variable queue_cv_;
  std::deque<Event> queue_;
  bool stopping_ = false;
};

}  // namespace hoodie::app
