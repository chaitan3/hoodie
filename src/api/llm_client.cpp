#include "api/llm_client.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "api/api_error.h"
#include "api/api_util.h"
#include "net/sse.h"

namespace hoodie::api {
namespace {

nlohmann::json build_chat_body(const ChatRequest& request, const std::string& default_model) {
  nlohmann::json body;
  body["model"] = request.model.empty() ? default_model : request.model;

  nlohmann::json messages = nlohmann::json::array();
  for (const Message& message : request.messages) {
    nlohmann::json entry;
    entry["role"] = message.role;
    entry["content"] = message.content;
    if (!message.tool_call_id.empty()) entry["tool_call_id"] = message.tool_call_id;
    if (!message.tool_calls.empty()) {
      entry["tool_calls"] =
          nlohmann::json::parse(messages_to_json({message})).at(0).at("tool_calls");
    }
    messages.push_back(std::move(entry));
  }
  body["messages"] = std::move(messages);

  if (!request.tools_json.empty()) {
    body["tools"] = nlohmann::json::parse(request.tools_json);
  }
  if (request.speculative) body["hoodie_speculative"] = true;
  return body;
}

}  // namespace

BufferedLlmClient::BufferedLlmClient(net::HttpClient& http, std::string url, std::string model,
                                     std::string api_key)
    : http_(http), url_(std::move(url)), model_(std::move(model)), api_key_(std::move(api_key)) {}

bool BufferedLlmClient::streaming() const { return false; }

void BufferedLlmClient::chat(const ChatRequest& request, const util::CancelToken& cancel,
                             const std::function<void(const LlmEvent&)>& on_event) {
  const nlohmann::json body = build_chat_body(request, model_);
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

SseLlmClient::SseLlmClient(net::HttpClient& http, std::string url, std::string model,
                           std::string api_key, bool cache_prompt)
    : http_(http),
      url_(std::move(url)),
      model_(std::move(model)),
      api_key_(std::move(api_key)),
      cache_prompt_(cache_prompt) {}

bool SseLlmClient::streaming() const { return true; }

void SseLlmClient::chat(const ChatRequest& request, const util::CancelToken& cancel,
                        const std::function<void(const LlmEvent&)>& on_event) {
  nlohmann::json body = build_chat_body(request, model_);
  body["stream"] = true;
  if (cache_prompt_) body["cache_prompt"] = true;

  bool done = false;
  std::string finish_reason = "stop";
  std::string stream_error;
  std::map<int, ToolCall> tool_calls;

  const auto finish = [&](const std::string& reason) {
    if (done) return;
    done = true;
    if (!on_event) return;
    for (auto& entry : tool_calls) {
      on_event(LlmEvent{LlmEvent::Kind::ToolCall, std::string{}, entry.second, std::string{}});
    }
    tool_calls.clear();
    on_event(LlmEvent{LlmEvent::Kind::Done, std::string{}, ToolCall{}, reason});
  };

  const std::function<void(const net::SseEvent&)> handle_event = [&](const net::SseEvent& event) {
    if (done || !stream_error.empty()) return;
    const std::string& payload = event.data;
    if (payload == "[DONE]") {
      finish(finish_reason.empty() ? "stop" : finish_reason);
      return;
    }
    if (payload.empty()) return;

    nlohmann::json chunk;
    try {
      chunk = nlohmann::json::parse(payload);
    } catch (const std::exception& e) {
      spdlog::warn("llm: ignoring malformed SSE payload: {}", e.what());
      return;
    }

    if (chunk.contains("error")) {
      stream_error = chunk["error"].is_string() ? chunk["error"].get<std::string>()
                                                : chunk["error"].dump();
      return;
    }
    if (!chunk.contains("choices") || !chunk["choices"].is_array() ||
        chunk["choices"].empty()) {
      return;
    }

    const auto& choice = chunk["choices"][0];
    if (choice.contains("delta") && choice["delta"].is_object()) {
      const auto& delta = choice["delta"];
      if (delta.contains("content") && delta["content"].is_string()) {
        const std::string text = delta["content"].get<std::string>();
        if (!text.empty() && on_event) {
          on_event(LlmEvent{LlmEvent::Kind::Token, text, ToolCall{}, std::string{}});
        }
      }
      if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
        for (const auto& entry : delta["tool_calls"]) {
          const int index = entry.value("index", 0);
          ToolCall& pending = tool_calls[index];
          if (entry.contains("id") && entry["id"].is_string()) {
            pending.id = entry["id"].get<std::string>();
          }
          if (entry.contains("function") && entry["function"].is_object()) {
            const auto& function = entry["function"];
            if (function.contains("name") && function["name"].is_string()) {
              pending.name = function["name"].get<std::string>();
            }
            if (function.contains("arguments") && function["arguments"].is_string()) {
              pending.arguments += function["arguments"].get<std::string>();
            }
          }
        }
      }
    }
    if (choice.contains("finish_reason") && choice["finish_reason"].is_string()) {
      finish_reason = choice["finish_reason"].get<std::string>();
      finish(finish_reason);
    }
  };

  net::SseParser parser;
  const net::HttpResult result = http_.post_json_stream(
      url_, json_headers(api_key_), body.dump(),
      [&](const uint8_t* data, std::size_t size) { parser.feed(data, size, handle_event); },
      cancel);
  parser.finish(handle_event);

  if (!stream_error.empty()) {
    throw ApiError(result.status, "llm stream: " + stream_error);
  }
  if (!result.ok()) {
    throw_if_http_error(result, "llm");
  }
  finish("stop");  // in case the server closed without [DONE]
}

std::string SseLlmClient::description() const {
  return "SSE LLM (" + model_ + " @ " + url_ + ")";
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
