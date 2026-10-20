#include "api/clients.h"

#include <spdlog/spdlog.h>

#include <memory>
#include <string>

#include "api/asr_client.h"
#include "api/llm_client.h"
#include "api/tts_client.h"

namespace hoodie::api {
namespace {

// Paths are a property of the protocol, not the deployment. Only ASR has a
// mode-specific path (live ingest); TTS/LLM use the same path for buffered and
// streaming, with different request bodies.
constexpr const char* kAsrPath = "/v1/audio/transcriptions";
constexpr const char* kAsrLivePath = "/v1/audio/transcriptions/live";
constexpr const char* kTtsPath = "/v1/audio/speech";
constexpr const char* kLlmPath = "/v1/chat/completions";

}  // namespace

ClientSet make_clients(const Config& config, net::HttpClient& http) {
  ClientSet clients;
  const ServerConfig& server = config.server;
  const StreamingConfig& modes = config.streaming;

  const std::string audio_base = server.audio_base();
  const std::string llm_base = server.llm_base();
  const std::string audio_key = server.audio_key();
  const std::string llm_key = server.llm_key();

  // ASR: live ingest with an automatic buffered fallback on the same host.
  if (modes.asr_mode == "live") {
    auto live = std::make_unique<LiveAsrClient>(
        http, join_url(audio_base, kAsrLivePath), server.asr_model, audio_key,
        /*sample_rate=*/16000, /*channels=*/1);
    auto fallback = std::make_unique<BufferedAsrClient>(
        http, join_url(audio_base, kAsrPath), server.asr_model, audio_key);
    clients.asr = std::make_unique<FallbackAsrClient>(std::move(live), std::move(fallback));
    spdlog::info("asr: live (buffered fallback) selected");
  } else {
    clients.asr = std::make_unique<BufferedAsrClient>(
        http, join_url(audio_base, kAsrPath), server.asr_model, audio_key);
    spdlog::info("asr: buffered selected");
  }

  // TTS: raw-PCM stream or the buffered floor, same host/path.
  if (modes.tts_mode == "stream") {
    clients.tts = std::make_unique<StreamPcmTtsClient>(
        http, join_url(audio_base, kTtsPath), server.tts_model, audio_key,
        server.tts_pcm_rate, server.tts_pcm_channels);
    spdlog::info("tts: streaming raw PCM selected");
  } else {
    clients.tts = std::make_unique<BufferedTtsClient>(
        http, join_url(audio_base, kTtsPath), server.tts_model, audio_key, "pcm");
    spdlog::info("tts: buffered selected");
  }

  // LLM: SSE stream or the buffered floor, same host/path.
  if (modes.llm_mode == "stream") {
    clients.llm = std::make_unique<SseLlmClient>(
        http, join_url(llm_base, kLlmPath), server.llm_model, llm_key,
        server.llm_cache_prompt);
    spdlog::info("llm: SSE streaming selected");
  } else {
    clients.llm = std::make_unique<BufferedLlmClient>(
        http, join_url(llm_base, kLlmPath), server.llm_model, llm_key);
    spdlog::info("llm: buffered selected");
  }

  spdlog::info("clients: {}", clients.asr->description());
  spdlog::info("clients: {}", clients.tts->description());
  spdlog::info("clients: {}", clients.llm->description());
  return clients;
}

}  // namespace hoodie::api
