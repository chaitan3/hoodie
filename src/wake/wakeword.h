#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

#include "ml/onnx.h"

namespace hoodie::wake {

struct WakeWordConfig {
  std::string melspectrogram_model = "models/melspectrogram.onnx";
  std::string embedding_model = "models/embedding_model.onnx";
  std::string keyword_model = "models/alexa_v0.1.onnx";
  std::string keyword = "alexa";
  float threshold = 0.5f;
  int warmup_frames = 5;  // predictions forced to zero for the first N 80 ms frames
};

// openWakeWord streaming detector: melspectrogram -> speech embedding -> keyword.
// Input must be 16 kHz mono int16. Mirrors the reference streaming pipeline
// (80 ms / 1280-sample steps) so it can be validated against the Python model.
class WakeWordDetector {
 public:
  explicit WakeWordDetector(const WakeWordConfig& config);

  void reset();

  // Feed 16 kHz mono int16 samples. Returns the keyword score when one or more
  // complete 80 ms frames were processed, otherwise std::nullopt.
  std::optional<float> process(const int16_t* samples, std::size_t frames);

  // Process a whole buffer and return the maximum score seen.
  float process_all(const int16_t* samples, std::size_t frames);

  float threshold() const { return threshold_; }
  const std::string& keyword() const { return keyword_; }

 private:
  float process_chunk(const int16_t* chunk);  // exactly kChunk samples

  static constexpr std::size_t kChunk = 1280;        // 80 ms @ 16 kHz
  static constexpr std::size_t kMelContext = 480;    // 3 extra mel frames of context
  static constexpr std::size_t kEmbeddingWindow = 76;
  static constexpr std::size_t kMaxMelFrames = 970;  // 10 s
  static constexpr std::size_t kMaxFeatureFrames = 120;
  static constexpr std::size_t kFeatureDim = 96;

  ml::OnnxSession mel_model_;
  ml::OnnxSession embedding_model_;
  ml::OnnxSession keyword_model_;

  std::deque<int16_t> raw_;       // rolling raw audio (~10 s); indexed, never pointer-walked
  std::vector<float> melspec_;    // mel_frames * mel_bins
  std::size_t mel_frames_ = 0;
  std::size_t mel_bins_ = 32;
  std::vector<float> features_;   // feature_frames * feature_dim
  std::size_t feature_frames_ = 0;
  int feature_input_frames_ = 16;
  int output_classes_ = 1;

  std::vector<int16_t> pending_;  // must stay contiguous for chunk extraction
  long frame_count_ = 0;

  std::string keyword_;
  float threshold_ = 0.5f;
  int warmup_frames_ = 5;
};

}  // namespace hoodie::wake
