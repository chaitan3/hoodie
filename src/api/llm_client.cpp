#include "api/llm_client.h"

#include <nlohmann/json.hpp>

#include <utility>

#include "api/api_util.h"

namespace hoodie::api {

BufferedLlmClient::BufferedLlmClient(net::HttpClient& http, std::string url, std::string model,
                                     std::string api_key)
    : http_(http), url_(std::move(url)), model_(std::move(model)), api_key_(std::move(api_key)) {}

bool BufferedLlmClient::streaming() const { return false; }

void BufferedLlmClient::chat(const ChatRequest& request, const util::CancelToken& cancel,
                             const std::function<void(const LlmEvent&)>& on_event) {
  nlohmann::json body;
  body["model"] = request.model.empty() ? model_ : request.model;

  nlohmann::json messages = nlohmann::json::array();
  for (const Message& message : request.messages) {
    nlohmann::json entry;
    entry["role"] = message.role;
    entry["content"] = message.content;
    if (!message.tool_call_id.empty()) entry["tool_call_id"] = message.tool_call_id;
    if (!message.tool_calls.empty()) {
      entry["tool_calls"] = nlohmann::json::parse(messages_to_json({message})).at(0).at("tool_calls");
    }
    messages.push_back(std::move(entry));
  }
  body["messages"] = std::move(messages);

  if (!request.tools_json.empty()) {
    body["tools"] = nlohmann::json::parse(request.tools_json);
  }

  const net::HttpResult result =
      http_.post_json(url_, json_headers(api_key_), body.dump(), cancel);
  throw_if_http_error(result, "llm");
  const ChatResult response = parse_chat_response(result.body_string());

  if (!on_event) return;
  if (!response.message.content.empty()) {
    on_event(LlmEvent{LlmEvent::Kind::Token, response.message.content, ToolCall{},
                      std::string{}});
  }
  for (const ToolCall& call : response.message.tool_calls) {
    on_event(LlmEvent{LlmEvent::Kind::ToolCall, std::string{}, call, std::string{}});
  }
  on_event(LlmEvent{LlmEvent::Kind::Done, std::string{}, ToolCall{}, response.finish_reason});
}

std::string BufferedLlmClient::description() const {
  return "buffered LLM (" + model_ + " @ " + url_ + ")";
}

ChatResult chat_blocking(LlmClient& client, const ChatRequest& request,
                         const util::CancelToken& cancel) {
  ChatResult result;
  result.message.role = "assistant";
  client.chat(request, cancel, [&](const LlmEvent& event) {
    switch (event.kind) {
      case LlmEvent::Kind::Token:
        result.message.content += event.text;
        break;
      case LlmEvent::Kind::ToolCall:
        result.message.tool_calls.push_back(event.tool_call);
        break;
      case LlmEvent::Kind::Done:
        result.finish_reason = event.finish_reason;
        break;
    }
  });
  return result;
}

}  // namespace hoodie::api
