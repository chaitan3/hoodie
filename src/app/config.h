#pragma once

#include <string>

namespace hoodie {

// Hosts, keys and models for the remote services. Paths are derived from the
// transport mode selected in StreamingConfig, so there is exactly one place to
// configure each service's host/model/secret.
struct ServerConfig {
  std::string base_url;        // default host for every service
  std::string audio_base_url;  // optional override for ASR + TTS
  std::string llm_base_url;    // optional override for LLM

  std::string api_key;       // default API key
  std::string audio_api_key; // optional override for ASR + TTS
  std::string llm_api_key;   // optional override for LLM

  std::string asr_model = "small";
  std::string tts_model = "tts-1";
  std::string llm_model = "qwen3.5-4b";

  int tts_pcm_rate = 24000;    // raw streamed PCM has no header
  int tts_pcm_channels = 1;
  bool llm_cache_prompt = true;  // llama.cpp cache_prompt extension

  std::string audio_base() const {
    return audio_base_url.empty() ? base_url : audio_base_url;
  }
  std::string llm_base() const { return llm_base_url.empty() ? base_url : llm_base_url; }
  std::string audio_key() const { return audio_api_key.empty() ? api_key : audio_api_key; }
  std::string llm_key() const { return llm_api_key.empty() ? api_key : llm_api_key; }
};

struct WakeWordConfig {
  std::string keyword = "alexa";
  double threshold = 0.5;
  std::string melspectrogram_model = "models/melspectrogram.onnx";
  std::string embedding_model = "models/embedding_model.onnx";
  std::string keyword_model = "models/alexa_v0.1.onnx";
  std::string vad_model = "models/silero_vad.onnx";
  int warmup_frames = 5;
  bool require_real_context = true;
};

struct AudioConfig {
  std::string input_device;   // empty = system default
  std::string output_device;  // empty = system default
  int capture_sample_rate = 48000;
};

// Voice-loop tuning for the orchestrator (src/app/assistant.cpp).
struct AssistantConfig {
  // Ignore the endpointer's end-of-speech until the utterance holds at least
  // this much audio. The wake-word tail otherwise trips a SpeechStart and a
  // short pause ends the utterance before the command. 0 disables the gate.
  int min_utterance_ms = 1500;
  double listen_timeout_s = 8.0;  // max silence after wake before giving up

  // Speculative LLM start on a stable ASR partial. Off by default; never
  // executes tools or plays audio before the final transcript is confirmed.
  bool speculate = false;
  int speculate_stable_ms = 400;   // partial unchanged for this long
  int speculate_silence_ms = 250;  // VAD silence before speculating
  int speculate_min_words = 2;     // ignore very short partials
};

// Transport per service. Selected once at startup; never switched mid-request.
struct StreamingConfig {
  std::string asr_mode = "buffered";  // buffered | live
  std::string tts_mode = "buffered";  // buffered | stream
  std::string llm_mode = "buffered";  // buffered | stream
};

struct Config {
  ServerConfig server;
  StreamingConfig streaming;
  WakeWordConfig wakeword;
  AudioConfig audio;
  AssistantConfig assistant;

  // Load and validate configuration. Throws std::runtime_error on failure.
  // Applies HOODIE_* environment overrides.
  static Config load(const std::string& path);

  // Like load(), but returns defaulted config when the file does not exist.
  // Still throws on malformed contents. Intended for diagnostic commands.
  static Config load_or_default(const std::string& path);

  // Human-readable dump with API keys redacted.
  std::string summary() const;
};

// Join a base URL and a path with exactly one slash.
std::string join_url(const std::string& base_url, const std::string& path);

}  // namespace hoodie
