#include "api/clients.h"

#include <spdlog/spdlog.h>

#include <string>

namespace hoodie::api {

ClientSet make_clients(const Config& config, net::HttpClient& http) {
  ClientSet clients;

  const std::string asr_url = config.resolve_url(config.server.asr_path);
  const std::string tts_url = config.resolve_url(config.server.tts_path);
  const std::string llm_url = config.resolve_url(config.server.llm_path);

  if (config.capabilities.asr_stream) {
    spdlog::info("asr: streaming requested; SSE client not implemented yet, using buffered fallback");
  }
  clients.asr = std::make_unique<BufferedAsrClient>(http, asr_url, config.server.asr_model,
                                                    config.server.api_key);

  if (config.capabilities.tts_stream) {
    spdlog::info("tts: streaming requested; chunked client not implemented yet, using buffered fallback");
  }
  clients.tts = std::make_unique<BufferedTtsClient>(
      http, tts_url, config.server.tts_model, config.server.api_key, config.capabilities.tts_format);

  if (config.capabilities.llm_stream) {
    spdlog::info("llm: streaming requested; SSE client not implemented yet, using buffered fallback");
  }
  clients.llm = std::make_unique<BufferedLlmClient>(http, llm_url, config.server.llm_model,
                                                    config.server.api_key);

  spdlog::info("clients: {}", clients.asr->description());
  spdlog::info("clients: {}", clients.tts->description());
  spdlog::info("clients: {}", clients.llm->description());
  return clients;
}

}  // namespace hoodie::api
