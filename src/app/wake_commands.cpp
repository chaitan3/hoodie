#include "app/wake_commands.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "audio/resampler.h"
#include "audio/wav.h"
#include "wake/endpointer.h"
#include "wake/vad.h"
#include "wake/wakeword.h"

namespace hoodie::app {
namespace {

audio::AudioFrame load_16k_mono(const std::string& path) {
  const audio::AudioFrame raw = audio::read_audio_file(path);
  return audio::resample_frame(raw, audio::kSampleRate16k);
}

}  // namespace

int run_wakeword_check(const Config& config, const std::string& audio_file,
                       float threshold_override) {
  const audio::AudioFrame audio = load_16k_mono(audio_file);
  if (audio.empty()) {
    spdlog::error("wakeword: no audio in {}", audio_file);
    return 1;
  }

  wake::WakeWordConfig detector_config;
  detector_config.melspectrogram_model = config.wakeword.melspectrogram_model;
  detector_config.embedding_model = config.wakeword.embedding_model;
  detector_config.keyword_model = config.wakeword.keyword_model;
  detector_config.keyword = config.wakeword.keyword;
  detector_config.threshold = threshold_override > 0 ? threshold_override
                                                     : static_cast<float>(config.wakeword.threshold);
  detector_config.warmup_frames = config.wakeword.warmup_frames;
  detector_config.require_real_context = config.wakeword.require_real_context;

  wake::WakeWordDetector detector(detector_config);

  constexpr std::size_t kChunk = 1280;
  float best_score = 0.0f;
  double best_time = 0.0;
  std::size_t frame_index = 0;
  for (std::size_t offset = 0; offset + kChunk <= audio.samples.size(); offset += kChunk) {
    const auto score = detector.process(audio.samples.data() + offset, kChunk);
    const double time = static_cast<double>(offset) / audio.sample_rate;
    if (score.has_value()) {
      spdlog::debug("wakeword: t={:.2f}s score={:.3f}", time, *score);
      if (*score > best_score) {
        best_score = *score;
        best_time = time;
      }
    }
    ++frame_index;
  }

  const bool detected = best_score >= detector.threshold();
  std::cout << config.wakeword.keyword << ": max=" << best_score << " at=" << best_time
            << "s detected=" << (detected ? "yes" : "no") << "\n";
  spdlog::info("wakeword: {} frames, threshold {:.2f}", frame_index, detector.threshold());
  return 0;
}

int run_vad_check(const Config& config, const std::string& audio_file, float threshold_override) {
  const audio::AudioFrame audio = load_16k_mono(audio_file);
  if (audio.empty()) {
    spdlog::error("vad: no audio in {}", audio_file);
    return 1;
  }

  const float threshold = threshold_override > 0 ? threshold_override : 0.5f;
  wake::SileroVad vad(config.wakeword.vad_model);
  const std::vector<float> scores = vad.process(audio.samples.data(), audio.samples.size());

  std::size_t speech_frames = 0;
  float max_score = 0.0f;
  int segments = 0;
  bool in_segment = false;
  const double frame_seconds =
      static_cast<double>(wake::SileroVad::kFrameSamples) / audio.sample_rate;
  for (float score : scores) {
    max_score = std::max(max_score, score);
    if (score >= threshold) {
      ++speech_frames;
      if (!in_segment) {
        ++segments;
        in_segment = true;
      }
    } else {
      in_segment = false;
    }
  }

  const double ratio = scores.empty() ? 0.0 : static_cast<double>(speech_frames) / scores.size();
  std::cout << "vad: frames=" << scores.size() << " frame_ms=" << (frame_seconds * 1000.0)
            << " max=" << max_score << " speech_ratio=" << ratio << " segments=" << segments
            << " detected=" << (speech_frames > 0 ? "yes" : "no") << "\n";
  return 0;
}

int run_endpoint_check(const Config& config, const std::string& audio_file) {
  const audio::AudioFrame audio = load_16k_mono(audio_file);
  if (audio.empty()) {
    spdlog::error("endpoint: no audio in {}", audio_file);
    return 1;
  }

  wake::SileroVad vad(config.wakeword.vad_model);
  const std::vector<float> scores = vad.process(audio.samples.data(), audio.samples.size());
  const double frame_seconds =
      static_cast<double>(wake::SileroVad::kFrameSamples) / audio.sample_rate;

  std::vector<std::pair<std::size_t, std::size_t>> segments;
  std::optional<std::size_t> segment_start;
  wake::Endpointer::Options options;
  options.min_speech_frames = 3;    // 120 ms
  options.min_silence_frames = 15;  // 600 ms
  wake::Endpointer endpointer(options, [&](wake::Endpointer::Event event, std::size_t index) {
    if (event == wake::Endpointer::Event::SpeechStart) {
      segment_start = index;
    } else if (segment_start.has_value()) {
      segments.emplace_back(*segment_start, index);
      segment_start.reset();
    }
  });

  for (float score : scores) endpointer.push(score);
  if (segment_start.has_value()) segments.emplace_back(*segment_start, scores.size());

  double speech_seconds = 0.0;
  std::cout << "endpoint: segments=" << segments.size();
  for (const auto& segment : segments) {
    const double start = static_cast<double>(segment.first) * frame_seconds;
    const double end = static_cast<double>(segment.second) * frame_seconds;
    speech_seconds += end - start;
    std::cout << " [" << start << "," << end << "]";
  }
  std::cout << " speech_s=" << speech_seconds << "\n";
  return 0;
}

// Diagnostic: run each model three times on a fixed input to verify that ONNX
// Runtime is deterministic in this environment.
int run_onnx_probe(const Config& config) {
  ml::OnnxSession mel(config.wakeword.melspectrogram_model, 1);
  ml::OnnxSession emb(config.wakeword.embedding_model, 1);
  ml::OnnxSession key(config.wakeword.keyword_model, 1);

  const auto probe = [](const char* label, ml::OnnxSession& model, std::vector<float> input,
                        const std::vector<int64_t>& shape) {
    for (int run = 0; run < 3; ++run) {
      std::vector<Ort::Value> inputs;
      inputs.push_back(ml::make_float_tensor(input.data(), input.size(), shape));
      const char* in_names[] = {model.input_name(0).c_str()};
      const char* out_names[] = {model.output_name(0).c_str()};
      std::vector<Ort::Value> outputs = model.run(std::vector<const char*>(in_names, in_names + 1),
                                                  inputs,
                                                  std::vector<const char*>(out_names, out_names + 1));
      const std::size_t count = outputs[0].GetTensorTypeAndShapeInfo().GetElementCount();
      const float* data = outputs[0].GetTensorData<float>();
      double sum = 0.0;
      for (std::size_t i = 0; i < count; ++i) sum += data[i];
      spdlog::info("probe {} run {}: n={} first={:.6f} sum={:.5f}", label, run, count,
                   count > 0 ? data[0] : 0.0f, sum);
    }
  };

  std::vector<float> mel_in(1280);
  std::vector<float> emb_in(76 * 32);
  std::vector<float> key_in(16 * 96);
  for (std::size_t i = 0; i < mel_in.size(); ++i) mel_in[i] = static_cast<float>(i % 97) - 48.0f;
  for (std::size_t i = 0; i < emb_in.size(); ++i) emb_in[i] = static_cast<float>(i % 13) * 0.25f;
  for (std::size_t i = 0; i < key_in.size(); ++i) key_in[i] = static_cast<float>(i % 7) * 0.1f;

  probe("mel", mel, std::move(mel_in), {1, 1280});
  probe("emb", emb, std::move(emb_in), {1, 76, 32, 1});
  probe("key", key, std::move(key_in), {1, 16, 96});
  return 0;
}

}  // namespace hoodie::app
