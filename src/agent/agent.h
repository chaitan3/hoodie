#pragma once

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
  struct Result {
    std::string text;
    int steps = 0;
    std::vector<api::Message> messages;
  };

  Agent(api::LlmClient& llm, ToolRegistry tools, AgentOptions options)
      : llm_(llm), tools_(std::move(tools)), options_(std::move(options)) {}

  Result run(const std::string& input, ToolContext& context,
             const util::CancelToken& cancel = {});

 private:
  api::LlmClient& llm_;
  ToolRegistry tools_;
  AgentOptions options_;
};

}  // namespace hoodie::agent
