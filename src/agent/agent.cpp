#include "agent/agent.h"

#include <spdlog/spdlog.h>

#include <exception>
#include <utility>

namespace hoodie::agent {

Agent::Result Agent::run(const std::string& input, ToolContext& context,
                         const util::CancelToken& cancel) {
  std::vector<api::Message> messages;
  messages.push_back(api::Message::system(options_.system_prompt));
  messages.push_back(api::Message::user(input));

  Result result;
  result.steps = 0;

  for (int step = 1; step <= options_.max_steps; ++step) {
    api::ChatRequest request;
    request.model = options_.model;
    request.messages = messages;
    if (!tools_.empty()) request.tools_json = tools_.tools_json();

    const api::ChatResult response = llm_.chat(request, cancel);
    messages.push_back(response.message);

    if (response.message.tool_calls.empty()) {
      result.text = response.message.content;
      result.steps = step;
      result.messages = std::move(messages);
      return result;
    }

    for (const api::ToolCall& call : response.message.tool_calls) {
      spdlog::info("agent: tool {} args={}", call.name, call.arguments);
      std::string output;
      try {
        output = tools_.invoke(call.name, call.arguments, context);
      } catch (const std::exception& e) {
        output = std::string("tool error: ") + e.what();
      }
      spdlog::info("agent: tool {} -> {}", call.name, output);
      messages.push_back(api::Message::tool(call.id, output));
    }
  }

  spdlog::warn("agent: reached max steps ({})", options_.max_steps);
  result.steps = options_.max_steps;
  result.text = messages.empty() ? "" : messages.back().content;
  result.messages = std::move(messages);
  return result;
}

}  // namespace hoodie::agent
