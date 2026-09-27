#pragma once

#include <onnxruntime/onnxruntime_cxx_api.h>

#include <cstdint>
#include <string>
#include <vector>

namespace hoodie::ml {

// Shared process-wide ONNX Runtime environment (created on first use).
Ort::Env& env();
Ort::MemoryInfo& cpu_memory();

// Thin RAII wrapper around a single ONNX session with named I/O.
class OnnxSession {
 public:
  explicit OnnxSession(const std::string& path, int intra_op_threads = 1);

  OnnxSession(const OnnxSession&) = delete;
  OnnxSession& operator=(const OnnxSession&) = delete;

  std::vector<Ort::Value> run(const std::vector<const char*>& input_names,
                              const std::vector<Ort::Value>& inputs,
                              const std::vector<const char*>& output_names);

  std::size_t input_count() const { return input_names_.size(); }
  std::size_t output_count() const { return output_names_.size(); }
  const std::string& input_name(std::size_t i) const { return input_names_.at(i); }
  const std::string& output_name(std::size_t i) const { return output_names_.at(i); }
  std::vector<int64_t> input_shape(std::size_t i) const;
  std::vector<int64_t> output_shape(std::size_t i) const;

 private:
  Ort::Session session_;
  std::vector<std::string> input_names_;
  std::vector<std::string> output_names_;
};

Ort::Value make_float_tensor(float* data, std::size_t count, const std::vector<int64_t>& shape);
Ort::Value make_int64_scalar(int64_t* value);

}  // namespace hoodie::ml
