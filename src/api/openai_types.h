#pragma once

#include <string>
#include <vector>

namespace hoodie::api {

struct ToolCall {
  std::string id;
  std::string name;
  std::string arguments;  // raw JSON string as returned by the server
};

struct Message {
  std::string role;                 // system | user | assistant | tool
  std::string content;
  std::string tool_call_id;         // role == "tool"
  std::vector<ToolCall> tool_calls; // role == "assistant" (function calling)

  static Message system(std::string text);
  static Message user(std::string text);
  static Message assistant(std::string text);
  static Message tool(std::string call_id, std::string text);
};

struct ChatRequest {
  std::string model;
  std::vector<Message> messages;
  std::string tools_json;  // optional raw JSON array of tool schemas
  bool stream = false;
};

struct ChatResult {
  Message message;
  std::string finish_reason;  // stop | length | tool_calls | ...
  int prompt_tokens = 0;
  int completion_tokens = 0;
};

// Serialise messages to the OpenAI "messages" array JSON.
std::string messages_to_json(const std::vector<Message>& messages);

// Parse a buffered /chat/completions response. Throws ApiError on malformed
// input or an embedded error object.
ChatResult parse_chat_response(const std::string& json);

// Parse a buffered /audio/transcriptions response ({"text": "..."}).
std::string parse_transcription_response(const std::string& json);

// Best-effort extraction of an error message from an error response body.
std::string extract_error_message(const std::string& json);

}  // namespace hoodie::api
