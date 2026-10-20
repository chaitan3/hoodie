#include "api/clients.h"

#include <spdlog/spdlog.h>

#include <memory>
#include <string>

#include "api/asr_client.h"
#include "api/llm_client.h"
#include "api/tts_client.h"

namespace hoodie::api {

ClientSet make_clients(const Config& config, net::HttpClient& http) {
  ClientSet clients;
  const StreamingConfig& streaming = config.streaming;

  const std::string audio_base =
      streaming.audio_base_url.empty() ? config.server.base_url : streaming.audio_base_url;
  const std::string llm_base =
      streaming.llm_base_url.empty() ? config.server.base_url : streaming.llm_base_url;
  const std::string asr_model =
      streaming.asr_model.empty() ? config.server.asr_model : streaming.asr_model;
  const std::string tts_model =
      streaming.tts_model.empty() ? config.server.tts_model : streaming.tts_model;

  // ASR: live ingest with an automatic buffered fallback.
  if (streaming.asr_mode == "live") {
    auto live = std::make_unique<LiveAsrClient>(
        http, join_url(audio_base, streaming.asr_live_path), asr_model, config.server.api_key,
        /*sample_rate=*/16000, /*channels=*/1);
    auto fallback = std::make_unique<BufferedAsrClient>(
        http, config.resolve_url(config.server.asr_path), asr_model, config.server.api_key);
    clients.asr = std::make_unique<FallbackAsrClient>(std::move(live), std::move(fallback));
    spdlog::info("asr: live (buffered fallback) selected");
  } else {
    clients.asr = std::make_unique<BufferedAsrClient>(
        http, config.resolve_url(config.server.asr_path), asr_model, config.server.api_key);
    spdlog::info("asr: buffered selected");
  }

  // TTS: audio.cpp raw-PCM stream, or the buffered OpenAI-compatible floor.
  if (streaming.tts_mode == "stream") {
    clients.tts = std::make_unique<StreamPcmTtsClient>(
        http, join_url(audio_base, streaming.tts_path), tts_model, config.server.api_key,
        streaming.tts_pcm_rate, streaming.tts_pcm_channels);
    spdlog::info("tts: streaming raw PCM selected");
  } else {
    clients.tts = std::make_unique<BufferedTtsClient>(
        http, config.resolve_url(config.server.tts_path), config.server.tts_model,
        config.server.api_key, config.capabilities.tts_format);
    spdlog::info("tts: buffered selected");
  }

  // LLM: llama.cpp SSE, or the buffered OpenAI-compatible floor.
  if (streaming.llm_mode == "stream") {
    clients.llm = std::make_unique<SseLlmClient>(
        http, join_url(llm_base, streaming.llm_path), config.server.llm_model,
        config.server.api_key, streaming.llm_cache_prompt);
    spdlog::info("llm: SSE streaming selected");
  } else {
    clients.llm = std::make_unique<BufferedLlmClient>(
        http, config.resolve_url(config.server.llm_path), config.server.llm_model,
        config.server.api_key);
    spdlog::info("llm: buffered selected");
  }

  spdlog::info("clients: {}", clients.asr->description());
  spdlog::info("clients: {}", clients.tts->description());
  spdlog::info("clients: {}", clients.llm->description());
  return clients;
}

}  // namespace hoodie::api
