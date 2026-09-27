#include "wake/vad.h"

#include <algorithm>
#include <stdexcept>

namespace hoodie::wake {

SileroVad::SileroVad(const std::string& model_path, int intra_op_threads)
    : session_(model_path, intra_op_threads) {
  reset();
}

void SileroVad::reset() {
  h_.assign(2 * kStateSize, 0.0f);
  c_.assign(2 * kStateSize, 0.0f);
  pending_.clear();
}

float SileroVad::process_frame(const int16_t* frame, std::size_t count) {
  std::vector<float> input(count);
  for (std::size_t i = 0; i < count; ++i) {
    input[i] = static_cast<float>(frame[i]) / 32767.0f;
  }

  std::vector<Ort::Value> inputs;
  std::vector<const char*> input_names;
  const std::vector<int64_t> input_shape{1, static_cast<int64_t>(count)};
  const std::vector<int64_t> state_shape{2, 1, kStateSize};

  // Build inputs in the model's declared order, matching names case-insensitively.
  for (std::size_t i = 0; i < session_.input_count(); ++i) {
    std::string name = session_.input_name(i);
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (name == "input") {
      inputs.push_back(ml::make_float_tensor(input.data(), input.size(), input_shape));
    } else if (name == "h") {
      inputs.push_back(ml::make_float_tensor(h_.data(), h_.size(), state_shape));
    } else if (name == "c") {
      inputs.push_back(ml::make_float_tensor(c_.data(), c_.size(), state_shape));
    } else if (name == "sr") {
      inputs.push_back(ml::make_int64_scalar(&sample_rate_));
    } else {
      throw std::runtime_error("vad: unexpected model input '" + session_.input_name(i) + "'");
    }
    input_names.push_back(session_.input_name(i).c_str());
  }

  std::vector<const char*> output_names;
  for (std::size_t i = 0; i < session_.output_count(); ++i) {
    output_names.push_back(session_.output_name(i).c_str());
  }

  std::vector<Ort::Value> outputs = session_.run(input_names, inputs, output_names);
  if (outputs.size() < 3) {
    throw std::runtime_error("vad: expected 3 outputs from Silero model");
  }

  const float score = outputs[0].GetTensorData<float>()[0];

  const float* new_h = outputs[1].GetTensorData<float>();
  const float* new_c = outputs[2].GetTensorData<float>();
  std::copy(new_h, new_h + h_.size(), h_.begin());
  std::copy(new_c, new_c + c_.size(), c_.begin());
  return score;
}

std::vector<float> SileroVad::process(const int16_t* samples, std::size_t frames) {
  pending_.insert(pending_.end(), samples, samples + frames);
  std::vector<float> scores;
  while (pending_.size() >= static_cast<std::size_t>(kFrameSamples)) {
    scores.push_back(process_frame(pending_.data(), kFrameSamples));
    pending_.erase(pending_.begin(),
                   pending_.begin() + static_cast<std::ptrdiff_t>(kFrameSamples));
  }
  return scores;
}

}  // namespace hoodie::wake
