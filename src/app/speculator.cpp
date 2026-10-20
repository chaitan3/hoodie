#include "app/speculator.h"

#include <exception>
#include <utility>

namespace hoodie::app {

Speculator::Speculator(api::LlmClient& llm, agent::ToolRegistry tools, std::string system_prompt)
    : llm_(llm), tools_(std::move(tools)), system_prompt_(std::move(system_prompt)) {}

Speculator::~Speculator() { stop(); }

void Speculator::start(const std::string& transcript, const util::CancelToken& cancel) {
  stop();
  transcript_ = transcript;
  request_cancel_ = util::CancelToken{};

  {
    std::lock_guard<std::mutex> lock(mutex_);
    done_ = false;
    has_tools_ = false;
    failed_ = false;
    text_.clear();
  }

  if (cancel.cancelled()) {
    std::lock_guard<std::mutex> lock(mutex_);
    done_ = true;
    failed_ = true;
    return;
  }
  worker_ = std::thread([this] { worker_loop(); });
}

void Speculator::stop() {
  request_cancel_.cancel();
  if (worker_.joinable()) worker_.join();
  request_cancel_ = util::CancelToken{};
}

bool Speculator::wait(std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(mutex_);
  return cv_.wait_for(lock, timeout, [this] { return done_; });
}

bool Speculator::done() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return done_;
}

bool Speculator::has_tools() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return has_tools_;
}

bool Speculator::failed() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return failed_;
}

std::string Speculator::text() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return text_;
}

void Speculator::worker_loop() {
  api::ChatRequest request;
  request.messages.push_back(api::Message::system(system_prompt_));
  request.messages.push_back(api::Message::user(transcript_));
  if (!tools_.empty()) request.tools_json = tools_.tools_json();
  request.speculative = true;

  try {
    llm_.chat(request, request_cancel_, [this](const api::LlmEvent& event) {
      std::lock_guard<std::mutex> lock(mutex_);
      switch (event.kind) {
        case api::LlmEvent::Kind::Token:
          text_ += event.text;
          break;
        case api::LlmEvent::Kind::ToolCall:
          has_tools_ = true;
          break;
        case api::LlmEvent::Kind::Done:
          done_ = true;
          cv_.notify_all();
          break;
      }
    });
  } catch (const std::exception&) {
    std::lock_guard<std::mutex> lock(mutex_);
    failed_ = true;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (!done_) {
    done_ = true;
    cv_.notify_all();
  }
}

}  // namespace hoodie::app
