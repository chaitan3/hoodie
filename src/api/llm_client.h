#pragma once

#include <string>

#include "api/openai_types.h"
#include "net/http.h"
#include "util/cancel.h"

namespace hoodie::api {

// Chat completion client. The tool-calling loop lives in the agent (Phase 4);
// this layer only performs one request/response.
class LlmClient {
 public:
  virtual ~LlmClient() = default;

  virtual ChatResult chat(const ChatRequest& request, const util::CancelToken& cancel = {}) = 0;
  virtual std::string description() const = 0;
};

// Buffered floor: POST /chat/completions without `stream`, returning the whole
// assistant message (including tool_calls when present).
class BufferedLlmClient : public LlmClient {
 public:
  BufferedLlmClient(net::HttpClient& http, std::string url, std::string model,
                    std::string api_key);

  ChatResult chat(const ChatRequest& request, const util::CancelToken& cancel = {}) override;
  std::string description() const override;

 private:
  net::HttpClient& http_;
  std::string url_;
  std::string model_;
  std::string api_key_;
};

}  // namespace hoodie::api
