#include "api/openai_types.h"

#include <nlohmann/json.hpp>

#include <stdexcept>

#include "api/api_error.h"

namespace hoodie::api {
namespace {

using nlohmann::json;

json message_to_json(const Message& message) {
  json out;
  out["role"] = message.role;
  // Assistant messages with tool calls may legitimately have null content.
  if (!message.content.empty() || message.tool_calls.empty()) {
    out["content"] = message.content;
  } else {
    out["content"] = nullptr;
  }
  if (!message.tool_call_id.empty()) {
    out["tool_call_id"] = message.tool_call_id;
  }
  if (!message.tool_calls.empty()) {
    json calls = json::array();
    for (const ToolCall& call : message.tool_calls) {
      calls.push_back({{"id", call.id},
                       {"type", "function"},
                       {"function", {{"name", call.name}, {"arguments", call.arguments}}}});
    }
    out["tool_calls"] = std::move(calls);
  }
  return out;
}

Message message_from_json(const json& in) {
  Message message;
  if (in.contains("role") && in.at("role").is_string()) message.role = in.at("role").get<std::string>();
  if (in.contains("content")) {
    const json& content = in.at("content");
    if (content.is_string()) {
      message.content = content.get<std::string>();
    } else if (content.is_array()) {
      // Some servers return content parts; concatenate text parts.
      for (const json& part : content) {
        if (part.is_object() && part.contains("text") && part.at("text").is_string()) {
          message.content += part.at("text").get<std::string>();
        }
      }
    }
  }
  if (in.contains("tool_call_id") && in.at("tool_call_id").is_string()) {
    message.tool_call_id = in.at("tool_call_id").get<std::string>();
  }
  if (in.contains("tool_calls") && in.at("tool_calls").is_array()) {
    for (const json& call : in.at("tool_calls")) {
      ToolCall parsed;
      if (call.contains("id") && call.at("id").is_string()) {
        parsed.id = call.at("id").get<std::string>();
      }
      if (call.contains("function") && call.at("function").is_object()) {
        const json& fn = call.at("function");
        if (fn.contains("name") && fn.at("name").is_string()) {
          parsed.name = fn.at("name").get<std::string>();
        }
        if (fn.contains("arguments")) {
          if (fn.at("arguments").is_string()) {
            parsed.arguments = fn.at("arguments").get<std::string>();
          } else {
            parsed.arguments = fn.at("arguments").dump();  // occasionally an object
          }
        }
      }
      message.tool_calls.push_back(std::move(parsed));
    }
  }
  return message;
}

}  // namespace

Message Message::system(std::string text) { return Message{"system", std::move(text), {}, {}}; }
Message Message::user(std::string text) { return Message{"user", std::move(text), {}, {}}; }
Message Message::assistant(std::string text) {
  return Message{"assistant", std::move(text), {}, {}};
}
Message Message::tool(std::string call_id, std::string text) {
  return Message{"tool", std::move(text), std::move(call_id), {}};
}

std::string messages_to_json(const std::vector<Message>& messages) {
  json array = json::array();
  for (const Message& message : messages) array.push_back(message_to_json(message));
  return array.dump();
}

ChatResult parse_chat_response(const std::string& json_text) {
  json root;
  try {
    root = json::parse(json_text);
  } catch (const json::exception& e) {
    throw ApiError(0, std::string("chat: invalid JSON response: ") + e.what(), json_text);
  }

  if (root.contains("error")) {
    throw ApiError(0, "chat: server error: " + extract_error_message(json_text), json_text);
  }
  if (!root.contains("choices") || !root.at("choices").is_array() || root.at("choices").empty()) {
    throw ApiError(0, "chat: response has no choices", json_text);
  }

  const json& choice = root.at("choices").at(0);
  ChatResult result;
  if (choice.contains("finish_reason") && choice.at("finish_reason").is_string()) {
    result.finish_reason = choice.at("finish_reason").get<std::string>();
  }
  if (!choice.contains("message")) {
    throw ApiError(0, "chat: choice has no message", json_text);
  }
  result.message = message_from_json(choice.at("message"));

  if (root.contains("usage") && root.at("usage").is_object()) {
    const json& usage = root.at("usage");
    if (usage.contains("prompt_tokens") && usage.at("prompt_tokens").is_number()) {
      result.prompt_tokens = usage.at("prompt_tokens").get<int>();
    }
    if (usage.contains("completion_tokens") && usage.at("completion_tokens").is_number()) {
      result.completion_tokens = usage.at("completion_tokens").get<int>();
    }
  }
  return result;
}

std::string parse_transcription_response(const std::string& json_text) {
  json root;
  try {
    root = json::parse(json_text);
  } catch (const json::exception& e) {
    throw ApiError(0, std::string("asr: invalid JSON response: ") + e.what(), json_text);
  }
  if (root.contains("error")) {
    throw ApiError(0, "asr: server error: " + extract_error_message(json_text), json_text);
  }
  if (!root.contains("text") || !root.at("text").is_string()) {
    throw ApiError(0, "asr: response has no text field", json_text);
  }
  return root.at("text").get<std::string>();
}

std::string extract_error_message(const std::string& json_text) {
  json root;
  try {
    root = json::parse(json_text);
  } catch (const json::exception&) {
    return json_text;
  }
  if (root.contains("error")) {
    const json& error = root.at("error");
    if (error.is_string()) return error.get<std::string>();
    if (error.is_object() && error.contains("message") && error.at("message").is_string()) {
      return error.at("message").get<std::string>();
    }
  }
  if (root.contains("message") && root.at("message").is_string()) {
    return root.at("message").get<std::string>();
  }
  return json_text;
}

}  // namespace hoodie::api
