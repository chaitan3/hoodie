#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ml/onnx.h"

namespace hoodie::wake {

// Voice activity detection using Silero's ONNX model (as bundled by
// openWakeWord). Input is 16 kHz mono int16; internally processed in 640-sample
// (40 ms) frames with persistent recurrent state.
class SileroVad {
 public:
  explicit SileroVad(const std::string& model_path = "models/silero_vad.onnx",
                     int intra_op_threads = 1);

  void reset();

  // Feed 16 kHz mono int16 audio; returns one probability per complete 640-
  // sample frame consumed. Leftover samples are buffered for the next call.
  std::vector<float> process(const int16_t* samples, std::size_t frames);

  static constexpr int kFrameSamples = 640;  // 40 ms @ 16 kHz
  static constexpr int kStateSize = 64;

 private:
  float process_frame(const int16_t* frame, std::size_t count);

  ml::OnnxSession session_;
  std::vector<float> h_;
  std::vector<float> c_;
  std::vector<int16_t> pending_;
  int64_t sample_rate_ = 16000;
};

}  // namespace hoodie::wake
