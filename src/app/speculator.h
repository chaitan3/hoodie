#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include "agent/tool_registry.h"
#include "api/llm_client.h"
#include "util/cancel.h"

namespace hoodie::app {

// Runs one LLM request on a partial transcript, buffering content and noting
// tool calls without executing them or emitting audio. The caller confirms
// against the final transcript and either consumes the text or discards it.
class Speculator {
 public:
  Speculator(api::LlmClient& llm, agent::ToolRegistry tools, std::string system_prompt);
  ~Speculator();

  Speculator(const Speculator&) = delete;
  Speculator& operator=(const Speculator&) = delete;

  void start(const std::string& transcript, const util::CancelToken& cancel);
  void stop();

  const std::string& transcript() const { return transcript_; }

  // Wait up to timeout for the request to finish; true if it finished.
  bool wait(std::chrono::milliseconds timeout);

  bool done() const;
  bool has_tools() const;
  bool failed() const;
  std::string text() const;

 private:
  void worker_loop();

  api::LlmClient& llm_;
  agent::ToolRegistry tools_;
  std::string system_prompt_;
  std::string transcript_;
  util::CancelToken request_cancel_;
  std::thread worker_;

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  bool done_ = false;
  bool has_tools_ = false;
  bool failed_ = false;
  std::string text_;
};

}  // namespace hoodie::app
