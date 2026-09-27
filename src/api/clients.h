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

// Build the per-service clients from configuration. Streaming tiers are
// selected when implemented; until then the buffered floor is used and the
// fallback is logged (see MIGRATION.md §6.1).
ClientSet make_clients(const Config& config, net::HttpClient& http);

}  // namespace hoodie::api
