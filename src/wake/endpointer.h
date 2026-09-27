#pragma once

#include <cstddef>
#include <functional>

namespace hoodie::wake {

// Frame-level endpointer that turns a stream of VAD probabilities into speech
// start/end events. Replaces the inverted logic in the original Python
// (which stopped on speech onset instead of after silence).
//
// Frame duration is implicit: thresholds are expressed in frames, so the caller
// supplies frames of a known length (SileroVad uses 40 ms frames).
class Endpointer {
 public:
  enum class Event { SpeechStart, SpeechEnd };

  struct Options {
    float threshold = 0.5f;
    int min_speech_frames = 5;      // consecutive speech frames to confirm start
    int min_silence_frames = 15;    // consecutive silence frames to confirm end (600 ms @ 40 ms)
    int max_speech_frames = 375;    // force end after this many frames (15 s @ 40 ms)
  };

  using Callback = std::function<void(Event event, std::size_t frame_index)>;

  Endpointer(Options options, Callback callback)
      : options_(options), callback_(std::move(callback)) {}

  void push(float speech_probability) {
    const bool is_speech = speech_probability >= options_.threshold;
    if (!in_speech_) {
      if (is_speech) {
        if (++speech_run_ >= options_.min_speech_frames) {
          in_speech_ = true;
          speech_length_ = 0;
          silence_run_ = 0;
          callback_(Event::SpeechStart, frame_index_);
        }
      } else {
        speech_run_ = 0;
      }
    } else {
      ++speech_length_;
      if (is_speech) {
        silence_run_ = 0;
      } else {
        ++silence_run_;
      }
      if (silence_run_ >= options_.min_silence_frames ||
          speech_length_ >= options_.max_speech_frames) {
        in_speech_ = false;
        speech_run_ = 0;
        silence_run_ = 0;
        callback_(Event::SpeechEnd, frame_index_);
      }
    }
    ++frame_index_;
  }

  void reset() {
    in_speech_ = false;
    speech_run_ = 0;
    silence_run_ = 0;
    speech_length_ = 0;
    frame_index_ = 0;
  }

  bool in_speech() const { return in_speech_; }

 private:
  Options options_;
  Callback callback_;
  bool in_speech_ = false;
  int speech_run_ = 0;
  int silence_run_ = 0;
  int speech_length_ = 0;
  std::size_t frame_index_ = 0;
};

}  // namespace hoodie::wake
