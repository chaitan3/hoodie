#pragma once

#include <functional>
#include <string>

#include "api/openai_types.h"
#include "net/http.h"
#include "util/cancel.h"

namespace hoodie::api {

struct LlmEvent {
  enum class Kind { Token, ToolCall, Done };

  Kind kind = Kind::Done;
  std::string text;           // Kind::Token
  ToolCall tool_call;         // Kind::ToolCall
  std::string finish_reason;  // Kind::Done
};

// Streaming-first chat interface. The buffered floor emits the whole content as
// one Token event, then any ToolCall events, then Done; streaming adapters emit
// tokens as they arrive.
class LlmClient {
 public:
  virtual ~LlmClient() = default;

  virtual bool streaming() const = 0;

  virtual void chat(const ChatRequest& request, const util::CancelToken& cancel,
                    const std::function<void(const LlmEvent&)>& on_event) = 0;

  virtual std::string description() const = 0;
};

// Buffered floor: POST /chat/completions without stream, then replay the parsed
// message as events. Matches the previous chat() behavior exactly.
class BufferedLlmClient : public LlmClient {
 public:
  BufferedLlmClient(net::HttpClient& http, std::string url, std::string model,
                    std::string api_key);

  bool streaming() const override;
  void chat(const ChatRequest& request, const util::CancelToken& cancel,
            const std::function<void(const LlmEvent&)>& on_event) override;
  std::string description() const override;

 private:
  net::HttpClient& http_;
  std::string url_;
  std::string model_;
  std::string api_key_;
};

// Convenience: collect events into the old ChatResult shape (CLI/diagnostics).
ChatResult chat_blocking(LlmClient& client, const ChatRequest& request,
                         const util::CancelToken& cancel = {});

}  // namespace hoodie::api
