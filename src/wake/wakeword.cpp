#include "wake/wakeword.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <stdexcept>

namespace hoodie::wake {

WakeWordDetector::WakeWordDetector(const WakeWordConfig& config)
    : mel_model_(config.melspectrogram_model, 1),
      embedding_model_(config.embedding_model, 1),
      keyword_model_(config.keyword_model, 1),
      keyword_(config.keyword),
      threshold_(config.threshold),
      warmup_frames_(config.warmup_frames) {
  const std::vector<int64_t> keyword_input = keyword_model_.input_shape(0);
  const std::vector<int64_t> keyword_output = keyword_model_.output_shape(0);
  if (keyword_input.size() >= 2 && keyword_input[1] > 0) {
    feature_input_frames_ = static_cast<int>(keyword_input[1]);
  }
  if (!keyword_output.empty() && keyword_output.back() > 0) {
    output_classes_ = static_cast<int>(keyword_output.back());
  }
  reset();
}

void WakeWordDetector::reset() {
  raw_.clear();
  pending_.clear();
  melspec_.assign(kEmbeddingWindow * mel_bins_, 1.0f);  // reference pads with ones
  mel_frames_ = kEmbeddingWindow;
  features_.assign(static_cast<std::size_t>(feature_input_frames_) * kFeatureDim, 0.0f);
  feature_frames_ = static_cast<std::size_t>(feature_input_frames_);
  frame_count_ = 0;
}

std::optional<float> WakeWordDetector::process(const int16_t* samples, std::size_t frames) {
  pending_.insert(pending_.end(), samples, samples + frames);
  std::optional<float> last;
  while (pending_.size() >= kChunk) {
    last = process_chunk(pending_.data());
    pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(kChunk));
  }
  return last;
}

float WakeWordDetector::process_all(const int16_t* samples, std::size_t frames) {
  float best = 0.0f;
  const auto score = process(samples, frames);
  if (score.has_value()) best = std::max(best, *score);
  while (pending_.size() >= kChunk) {
    best = std::max(best, process_chunk(pending_.data()));
    pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(kChunk));
  }
  return best;
}

float WakeWordDetector::process_chunk(const int16_t* chunk) {
  // 1. Append raw audio (keep ~10 s) and build the melspectrogram input window.
  raw_.insert(raw_.end(), chunk, chunk + kChunk);
  constexpr std::size_t kMaxRaw = 16000 * 10;
  while (raw_.size() > kMaxRaw) raw_.pop_front();

  const std::size_t window = std::min(raw_.size(), kChunk + kMelContext);
  std::vector<float> audio(window);
  const std::size_t offset = raw_.size() - window;
  for (std::size_t i = 0; i < window; ++i) {
    audio[i] = static_cast<float>(raw_[offset + i]);
  }

  // 2. Melspectrogram: output [1, 1, frames, mel_bins], transformed as x/10 + 2.
  {
    const std::vector<int64_t> in_shape{1, static_cast<int64_t>(window)};
    std::vector<Ort::Value> inputs;
    inputs.push_back(ml::make_float_tensor(audio.data(), audio.size(), in_shape));
    const char* in_names[] = {mel_model_.input_name(0).c_str()};
    const char* out_names[] = {mel_model_.output_name(0).c_str()};
    std::vector<Ort::Value> outputs = mel_model_.run(
        std::vector<const char*>(in_names, in_names + 1), inputs,
        std::vector<const char*>(out_names, out_names + 1));

    const std::vector<int64_t> shape = outputs[0].GetTensorTypeAndShapeInfo().GetShape();
    if (shape.size() < 2) {
      throw std::runtime_error("wakeword: unexpected melspectrogram output rank");
    }
    const std::size_t frames = static_cast<std::size_t>(shape[shape.size() - 2]);
    mel_bins_ = static_cast<std::size_t>(shape[shape.size() - 1]);

    const float* data = outputs[0].GetTensorData<float>();
    const std::size_t total = frames * mel_bins_;
    const std::size_t base = melspec_.size();
    melspec_.resize(base + total);
    for (std::size_t i = 0; i < total; ++i) melspec_[base + i] = data[i] / 10.0f + 2.0f;
    mel_frames_ += frames;
  }

  if (mel_frames_ > kMaxMelFrames) {
    const std::size_t drop = mel_frames_ - kMaxMelFrames;
    melspec_.erase(melspec_.begin(), melspec_.begin() + static_cast<std::ptrdiff_t>(drop * mel_bins_));
    mel_frames_ = kMaxMelFrames;
  }

  // 3. Embedding from the last 76 mel frames -> 96-dim feature frame.
  if (mel_frames_ >= kEmbeddingWindow) {
    const std::size_t start = (mel_frames_ - kEmbeddingWindow) * mel_bins_;
    std::vector<float> window_data(
        melspec_.begin() + static_cast<std::ptrdiff_t>(start),
        melspec_.begin() + static_cast<std::ptrdiff_t>(start + kEmbeddingWindow * mel_bins_));
    const std::vector<int64_t> in_shape{1, static_cast<int64_t>(kEmbeddingWindow),
                                        static_cast<int64_t>(mel_bins_), 1};
    std::vector<Ort::Value> inputs;
    inputs.push_back(ml::make_float_tensor(window_data.data(), window_data.size(), in_shape));
    const char* in_names[] = {embedding_model_.input_name(0).c_str()};
    const char* out_names[] = {embedding_model_.output_name(0).c_str()};
    std::vector<Ort::Value> outputs = embedding_model_.run(
        std::vector<const char*>(in_names, in_names + 1), inputs,
        std::vector<const char*>(out_names, out_names + 1));

    const std::size_t count = outputs[0].GetTensorTypeAndShapeInfo().GetElementCount();
    const float* data = outputs[0].GetTensorData<float>();
    features_.insert(features_.end(), data, data + count);
    feature_frames_ += count / kFeatureDim;
  }

  if (feature_frames_ > kMaxFeatureFrames) {
    const std::size_t drop = feature_frames_ - kMaxFeatureFrames;
    features_.erase(features_.begin(),
                    features_.begin() + static_cast<std::ptrdiff_t>(drop * kFeatureDim));
    feature_frames_ = kMaxFeatureFrames;
  }

  // 4. Keyword model on the last feature_input_frames_ embeddings.
  float score = 0.0f;
  if (feature_frames_ >= static_cast<std::size_t>(feature_input_frames_)) {
    const std::size_t start =
        (feature_frames_ - static_cast<std::size_t>(feature_input_frames_)) * kFeatureDim;
    std::vector<float> window_data(features_.begin() + static_cast<std::ptrdiff_t>(start),
                                   features_.end());
    const std::vector<int64_t> in_shape{1, feature_input_frames_,
                                        static_cast<int64_t>(kFeatureDim)};
    std::vector<Ort::Value> inputs;
    inputs.push_back(ml::make_float_tensor(window_data.data(), window_data.size(), in_shape));
    const char* in_names[] = {keyword_model_.input_name(0).c_str()};
    const char* out_names[] = {keyword_model_.output_name(0).c_str()};
    std::vector<Ort::Value> outputs = keyword_model_.run(
        std::vector<const char*>(in_names, in_names + 1), inputs,
        std::vector<const char*>(out_names, out_names + 1));

    const float* data = outputs[0].GetTensorData<float>();
    if (output_classes_ <= 1) {
      score = data[0];
    } else {
      score = *std::max_element(data, data + output_classes_);
    }
  }

  ++frame_count_;
  if (frame_count_ <= warmup_frames_) score = 0.0f;
  return score;
}

}  // namespace hoodie::wake
