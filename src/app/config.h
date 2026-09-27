#pragma once

#include <string>

namespace hoodie {

// Remote OpenAI-compatible server. Note the current server uses inconsistent
// paths: ASR/TTS are under /v1/... but the LLM endpoint is /chat/completions
// with no /v1 prefix. Paths are therefore configured individually.
struct ServerConfig {
  std::string base_url;
  std::string api_key;

  std::string asr_path = "/v1/audio/transcriptions";
  std::string tts_path = "/v1/audio/speech";
  std::string llm_path = "/chat/completions";

  std::string asr_model = "small";
  std::string tts_model = "tts-1";
  std::string llm_model = "qwen3.5-4b";
};

// Which transport each service uses. Streaming is the canonical interface; when
// a flag is false the corresponding buffered adapter is selected. Selected once
// at startup, never switched mid-request (see MIGRATION.md §6.1).
struct Capabilities {
  bool llm_stream = true;
  bool tts_stream = true;
  std::string tts_format = "pcm";  // requested when tts_stream is true
  bool asr_stream = true;          // SSE file-based transcription
  bool asr_realtime = false;       // future optional tier
};

struct WakeWordConfig {
  std::string keyword = "alexa";
  double threshold = 0.5;
  std::string melspectrogram_model = "models/melspectrogram.onnx";
  std::string embedding_model = "models/embedding_model.onnx";
  std::string keyword_model = "models/alexa_v0.1.onnx";
  std::string vad_model = "models/silero_vad.onnx";
  int warmup_frames = 5;
};

struct AudioConfig {
  std::string input_device;   // empty = system default
  std::string output_device;  // empty = system default
  int capture_sample_rate = 48000;
};

struct Config {
  ServerConfig server;
  Capabilities capabilities;
  WakeWordConfig wakeword;
  AudioConfig audio;

  // Load and validate configuration. Throws std::runtime_error on failure.
  // Applies HOODIE_API_KEY / HOODIE_BASE_URL environment overrides.
  static Config load(const std::string& path);

  // Like load(), but returns defaulted config when the file does not exist.
  // Still throws on malformed contents. Intended for diagnostic commands.
  static Config load_or_default(const std::string& path);

  // base_url + path, joining with exactly one slash.
  std::string resolve_url(const std::string& path) const;

  // Human-readable dump with the API key redacted.
  std::string summary() const;
};

}  // namespace hoodie
