#include "ml/onnx.h"

#include <spdlog/spdlog.h>

#include <stdexcept>

namespace hoodie::ml {
namespace {

Ort::Session create_session(const std::string& path, int intra_op_threads) {
  Ort::SessionOptions options;
  options.SetIntraOpNumThreads(intra_op_threads);
  options.SetInterOpNumThreads(1);
  options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
  return Ort::Session(env(), path.c_str(), options);
}

}  // namespace

Ort::Env& env() {
  static Ort::Env instance(ORT_LOGGING_LEVEL_WARNING, "hoodie");
  return instance;
}

Ort::MemoryInfo& cpu_memory() {
  static Ort::MemoryInfo instance =
      Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  return instance;
}

OnnxSession::OnnxSession(const std::string& path, int intra_op_threads)
    : session_(create_session(path, intra_op_threads)) {
  Ort::AllocatorWithDefaultOptions allocator;
  for (std::size_t i = 0; i < session_.GetInputCount(); ++i) {
    input_names_.push_back(session_.GetInputNameAllocated(i, allocator).get());
  }
  for (std::size_t i = 0; i < session_.GetOutputCount(); ++i) {
    output_names_.push_back(session_.GetOutputNameAllocated(i, allocator).get());
  }

  std::string inputs;
  for (const auto& name : input_names_) inputs += (inputs.empty() ? "" : ", ") + name;
  std::string outputs;
  for (const auto& name : output_names_) outputs += (outputs.empty() ? "" : ", ") + name;
  spdlog::debug("onnx: loaded {} [in: {} | out: {}]", path, inputs, outputs);
  for (std::size_t i = 0; i < input_names_.size(); ++i) {
    std::string dims;
    for (int64_t d : input_shape(i)) dims += (dims.empty() ? "" : "x") + std::to_string(d);
    spdlog::debug("onnx:   input[{}] {} = [{}]", i, input_names_[i], dims);
  }
  for (std::size_t i = 0; i < output_names_.size(); ++i) {
    std::string dims;
    for (int64_t d : output_shape(i)) dims += (dims.empty() ? "" : "x") + std::to_string(d);
    spdlog::debug("onnx:   output[{}] {} = [{}]", i, output_names_[i], dims);
  }
}

std::vector<Ort::Value> OnnxSession::run(const std::vector<const char*>& input_names,
                                         const std::vector<Ort::Value>& inputs,
                                         const std::vector<const char*>& output_names) {
  if (input_names.size() != inputs.size()) {
    throw std::runtime_error("onnx: input name/value count mismatch");
  }
  return session_.Run(Ort::RunOptions{nullptr}, input_names.data(), inputs.data(), inputs.size(),
                      output_names.data(), output_names.size());
}

std::vector<int64_t> OnnxSession::input_shape(std::size_t i) const {
  return session_.GetInputTypeInfo(i).GetTensorTypeAndShapeInfo().GetShape();
}

std::vector<int64_t> OnnxSession::output_shape(std::size_t i) const {
  return session_.GetOutputTypeInfo(i).GetTensorTypeAndShapeInfo().GetShape();
}

Ort::Value make_float_tensor(float* data, std::size_t count, const std::vector<int64_t>& shape) {
  return Ort::Value::CreateTensor<float>(cpu_memory(), data, count, shape.data(), shape.size());
}

Ort::Value make_int64_scalar(int64_t* value) {
  return Ort::Value::CreateTensor<int64_t>(cpu_memory(), value, 1, nullptr, 0);
}

}  // namespace hoodie::ml
