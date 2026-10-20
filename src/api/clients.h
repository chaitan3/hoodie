#pragma once

#include <memory>

#include "api/asr_client.h"
#include "api/llm_client.h"
#include "api/tts_client.h"
#include "app/config.h"
#include "net/http.h"

namespace hoodie::api {

struct ClientSet {
  std::unique_ptr<AsrClient> asr;
  std::unique_ptr<TtsClient> tts;
  std::unique_ptr<LlmClient> llm;
};

// Build the per-service clients from configuration. Each service selects its
// streaming or buffered implementation from streaming.*_mode at startup; live
// ASR also installs a buffered replay fallback. See ARCHITECTURE.md section 6.
ClientSet make_clients(const Config& config, net::HttpClient& http);

}  // namespace hoodie::api
