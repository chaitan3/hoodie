#pragma once

namespace hoodie::wake {

// Trigger policy for the openWakeWord score stream.
//
// A single 80 ms frame can score highly on a TV syllable or music transient,
// so the live loop requires several consecutive frames at or above the
// threshold. Once accepted the gate latches until the score falls well below
// the threshold; this is the same hysteresis that keeps the detector's stale
// context from re-triggering immediately after a reply.
struct WakeWordGateOptions {
  float threshold = 0.75f;
  int min_consecutive_frames = 2;
  float release_ratio = 0.5f;
};

class WakeWordGate {
 public:
  explicit WakeWordGate(WakeWordGateOptions options = {})
      : options_(options) {
    if (options_.min_consecutive_frames < 1) options_.min_consecutive_frames = 1;
  }

  // Returns true exactly once per accepted run of high-scoring frames.
  bool update(float score) {
    if (!armed_) {
      if (score < options_.threshold * options_.release_ratio) armed_ = true;
      return false;
    }

    if (score >= options_.threshold) {
      ++consecutive_;
      if (consecutive_ >= options_.min_consecutive_frames) {
        consecutive_ = 0;
        armed_ = false;
        return true;
      }
    } else {
      consecutive_ = 0;
    }
    return false;
  }

  void reset() {
    consecutive_ = 0;
    armed_ = true;
  }

  bool armed() const { return armed_; }
  int consecutive_frames() const { return consecutive_; }
  int min_consecutive_frames() const { return options_.min_consecutive_frames; }
  float threshold() const { return options_.threshold; }

 private:
  WakeWordGateOptions options_;
  int consecutive_ = 0;
  bool armed_ = true;
};

}  // namespace hoodie::wake
