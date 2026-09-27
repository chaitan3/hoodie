#include "api/llm_client.h"

#include <nlohmann/json.hpp>

#include <utility>

#include "api/api_util.h"

namespace hoodie::api {

BufferedLlmClient::BufferedLlmClient(net::HttpClient& http, std::string url, std::string model,
                                     std::string api_key)
    : http_(http), url_(std::move(url)), model_(std::move(model)), api_key_(std::move(api_key)) {}

ChatResult BufferedLlmClient::chat(const ChatRequest& request, const util::CancelToken& cancel) {
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
  return parse_chat_response(result.body_string());
}

std::string BufferedLlmClient::description() const {
  return "buffered LLM (" + model_ + " @ " + url_ + ")";
}

}  // namespace hoodie::api
