#pragma once

#include <functional>
#include <string>
#include <vector>

#include "agent/tool_registry.h"
#include "api/llm_client.h"
#include "api/openai_types.h"
#include "util/cancel.h"

namespace hoodie::agent {

struct AgentOptions {
  std::string system_prompt;
  std::string model;   // empty = client default
  int max_steps = 6;   // max tool-calling rounds before giving up
};

// Minimal tool-calling agent loop over an OpenAI-compatible chat client.
class Agent {
 public:
  using TextCallback = std::function<void(const std::string&)>;

  struct Result {
    std::string text;
    int steps = 0;
    std::vector<api::Message> messages;
  };

  Agent(api::LlmClient& llm, ToolRegistry tools, AgentOptions options)
      : llm_(llm), tools_(std::move(tools)), options_(std::move(options)) {}

  // on_text receives content tokens as they arrive (buffered clients deliver the
  // whole reply as one token). Empty callback = collect only.
  Result run(const std::string& input, ToolContext& context,
             const util::CancelToken& cancel = {}, const TextCallback& on_text = {});

 private:
  api::LlmClient& llm_;
  ToolRegistry tools_;
  AgentOptions options_;
};

}  // namespace hoodie::agent
