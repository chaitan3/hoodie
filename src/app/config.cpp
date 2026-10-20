#include "app/config.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <stdexcept>
#include <string>

namespace hoodie {
namespace {

using nlohmann::json;

std::string get_string(const json& obj, const char* key, const std::string& fallback) {
  if (!obj.contains(key) || obj.at(key).is_null()) return fallback;
  if (!obj.at(key).is_string()) {
    throw std::runtime_error(std::string("config: '") + key + "' must be a string");
  }
  return obj.at(key).get<std::string>();
}

bool get_bool(const json& obj, const char* key, bool fallback) {
  if (!obj.contains(key) || obj.at(key).is_null()) return fallback;
  if (!obj.at(key).is_boolean()) {
    throw std::runtime_error(std::string("config: '") + key + "' must be a boolean");
  }
  return obj.at(key).get<bool>();
}

double get_double(const json& obj, const char* key, double fallback) {
  if (!obj.contains(key) || obj.at(key).is_null()) return fallback;
  if (!obj.at(key).is_number()) {
    throw std::runtime_error(std::string("config: '") + key + "' must be a number");
  }
  return obj.at(key).get<double>();
}

int get_int(const json& obj, const char* key, int fallback) {
  if (!obj.contains(key) || obj.at(key).is_null()) return fallback;
  if (!obj.at(key).is_number_integer()) {
    throw std::runtime_error(std::string("config: '") + key + "' must be an integer");
  }
  return obj.at(key).get<int>();
}

const json& sub(const json& root, const char* key) {
  static const json empty = json::object();
  if (!root.contains(key) || root.at(key).is_null()) return empty;
  if (!root.at(key).is_object()) {
    throw std::runtime_error(std::string("config: '") + key + "' must be an object");
  }
  return root.at(key);
}

void apply_env_override(const char* name, std::string& target) {
  if (const char* value = std::getenv(name); value != nullptr && *value != '\0') {
    target = value;
  }
}

void apply_env_overrides(Config& cfg) {
  apply_env_override("HOODIE_BASE_URL", cfg.server.base_url);
  apply_env_override("HOODIE_AUDIO_BASE_URL", cfg.server.audio_base_url);
  apply_env_override("HOODIE_LLM_BASE_URL", cfg.server.llm_base_url);
  apply_env_override("HOODIE_API_KEY", cfg.server.api_key);
  apply_env_override("HOODIE_AUDIO_API_KEY", cfg.server.audio_api_key);
  apply_env_override("HOODIE_LLM_API_KEY", cfg.server.llm_api_key);
}

std::string redact(const std::string& key) {
  if (key.empty()) return "<unset>";
  if (key.size() <= 6) return "***";
  return key.substr(0, 4) + "..." + key.substr(key.size() - 2);
}

}  // namespace

Config Config::load(const std::string& path) {
  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error("cannot open config file: " + path);
  }

  json root;
  try {
    in >> root;
  } catch (const json::exception& e) {
    throw std::runtime_error("invalid JSON in " + path + ": " + e.what());
  }
  if (!root.is_object()) {
    throw std::runtime_error("config root must be a JSON object: " + path);
  }

  Config cfg;
  const json& server = sub(root, "server");
  const json& wake = sub(root, "wakeword");
  const json& audio = sub(root, "audio");
  const json& assistant = sub(root, "assistant");
  const json& streaming = sub(root, "streaming");

  cfg.server.base_url = get_string(server, "base_url", cfg.server.base_url);
  cfg.server.audio_base_url = get_string(server, "audio_base_url", cfg.server.audio_base_url);
  cfg.server.llm_base_url = get_string(server, "llm_base_url", cfg.server.llm_base_url);
  cfg.server.api_key = get_string(server, "api_key", cfg.server.api_key);
  cfg.server.audio_api_key = get_string(server, "audio_api_key", cfg.server.audio_api_key);
  cfg.server.llm_api_key = get_string(server, "llm_api_key", cfg.server.llm_api_key);
  cfg.server.asr_model = get_string(server, "asr_model", cfg.server.asr_model);
  cfg.server.tts_model = get_string(server, "tts_model", cfg.server.tts_model);
  cfg.server.llm_model = get_string(server, "llm_model", cfg.server.llm_model);
  cfg.server.tts_pcm_rate = get_int(server, "tts_pcm_rate", cfg.server.tts_pcm_rate);
  cfg.server.tts_pcm_channels = get_int(server, "tts_pcm_channels", cfg.server.tts_pcm_channels);
  cfg.server.llm_cache_prompt =
      get_bool(server, "llm_cache_prompt", cfg.server.llm_cache_prompt);

  cfg.wakeword.keyword = get_string(wake, "keyword", cfg.wakeword.keyword);
  cfg.wakeword.threshold = get_double(wake, "threshold", cfg.wakeword.threshold);
  cfg.wakeword.melspectrogram_model =
      get_string(wake, "melspectrogram_model", cfg.wakeword.melspectrogram_model);
  cfg.wakeword.embedding_model =
      get_string(wake, "embedding_model", cfg.wakeword.embedding_model);
  cfg.wakeword.keyword_model = get_string(wake, "keyword_model", cfg.wakeword.keyword_model);
  cfg.wakeword.vad_model = get_string(wake, "vad_model", cfg.wakeword.vad_model);
  cfg.wakeword.warmup_frames = get_int(wake, "warmup_frames", cfg.wakeword.warmup_frames);
  cfg.wakeword.require_real_context =
      get_bool(wake, "require_real_context", cfg.wakeword.require_real_context);

  cfg.audio.input_device = get_string(audio, "input_device", cfg.audio.input_device);
  cfg.audio.output_device = get_string(audio, "output_device", cfg.audio.output_device);
  cfg.audio.capture_sample_rate =
      get_int(audio, "capture_sample_rate", cfg.audio.capture_sample_rate);

  cfg.assistant.min_utterance_ms =
      get_int(assistant, "min_utterance_ms", cfg.assistant.min_utterance_ms);
  cfg.assistant.listen_timeout_s =
      get_double(assistant, "listen_timeout_s", cfg.assistant.listen_timeout_s);
  cfg.assistant.speculate = get_bool(assistant, "speculate", cfg.assistant.speculate);
  cfg.assistant.speculate_stable_ms =
      get_int(assistant, "speculate_stable_ms", cfg.assistant.speculate_stable_ms);
  cfg.assistant.speculate_silence_ms =
      get_int(assistant, "speculate_silence_ms", cfg.assistant.speculate_silence_ms);
  cfg.assistant.speculate_min_words =
      get_int(assistant, "speculate_min_words", cfg.assistant.speculate_min_words);

  cfg.streaming.asr_mode = get_string(streaming, "asr_mode", cfg.streaming.asr_mode);
  cfg.streaming.tts_mode = get_string(streaming, "tts_mode", cfg.streaming.tts_mode);
  cfg.streaming.llm_mode = get_string(streaming, "llm_mode", cfg.streaming.llm_mode);

  apply_env_overrides(cfg);

  // Every service must resolve to a host: base_url is the default, with the
  // per-service overrides taking precedence.
  if (cfg.server.audio_base().empty()) {
    throw std::runtime_error(
        "config: server.base_url or server.audio_base_url must not be empty");
  }
  if (cfg.server.llm_base().empty()) {
    throw std::runtime_error("config: server.base_url or server.llm_base_url must not be empty");
  }
  if (cfg.audio.capture_sample_rate <= 0) {
    throw std::runtime_error("config: audio.capture_sample_rate must be positive");
  }
  if (cfg.server.tts_pcm_rate <= 0 || cfg.server.tts_pcm_channels <= 0) {
    throw std::runtime_error("config: server.tts_pcm_rate/channels must be positive");
  }
  if (cfg.assistant.min_utterance_ms < 0) {
    throw std::runtime_error("config: assistant.min_utterance_ms must not be negative");
  }
  if (cfg.assistant.listen_timeout_s <= 0.0) {
    throw std::runtime_error("config: assistant.listen_timeout_s must be positive");
  }
  if (cfg.assistant.speculate_stable_ms < 0 || cfg.assistant.speculate_silence_ms < 0 ||
      cfg.assistant.speculate_min_words < 0) {
    throw std::runtime_error("config: assistant speculation values must not be negative");
  }
  const auto valid_mode = [](const std::string& mode, std::initializer_list<const char*> allowed,
                             const char* key) {
    for (const char* candidate : allowed) {
      if (mode == candidate) return;
    }
    throw std::runtime_error(std::string("config: ") + key + " has invalid value '" + mode + "'");
  };
  valid_mode(cfg.streaming.asr_mode, {"buffered", "live"}, "streaming.asr_mode");
  valid_mode(cfg.streaming.tts_mode, {"buffered", "stream"}, "streaming.tts_mode");
  valid_mode(cfg.streaming.llm_mode, {"buffered", "stream"}, "streaming.llm_mode");
  return cfg;
}

Config Config::load_or_default(const std::string& path) {
  std::ifstream probe(path);
  if (!probe) {
    Config cfg;
    apply_env_overrides(cfg);
    return cfg;
  }
  probe.close();
  return load(path);
}

std::string join_url(const std::string& base_url, const std::string& path) {
  if (base_url.empty()) return path;
  if (path.empty()) return base_url;
  const bool base_slash = base_url.back() == '/';
  const bool path_slash = path.front() == '/';
  if (base_slash && path_slash) return base_url + path.substr(1);
  if (!base_slash && !path_slash) return base_url + "/" + path;
  return base_url + path;
}

std::string Config::summary() const {
  std::ostringstream out;
  out << "config:\n"
      << "  server.base_url       = " << (server.base_url.empty() ? "<unset>" : server.base_url)
      << "\n"
      << "  server.audio_base_url = "
      << (server.audio_base_url.empty() ? "<base_url>" : server.audio_base_url) << "\n"
      << "  server.llm_base_url   = "
      << (server.llm_base_url.empty() ? "<base_url>" : server.llm_base_url) << "\n"
      << "  server.api_key        = " << redact(server.api_key) << "\n"
      << "  server.audio_api_key  = "
      << (server.audio_api_key.empty() ? "<api_key>" : redact(server.audio_api_key)) << "\n"
      << "  server.llm_api_key    = "
      << (server.llm_api_key.empty() ? "<api_key>" : redact(server.llm_api_key)) << "\n"
      << "  server.asr_model      = " << server.asr_model << "\n"
      << "  server.tts_model      = " << server.tts_model << "\n"
      << "  server.llm_model      = " << server.llm_model << "\n"
      << "  server.tts_pcm        = " << server.tts_pcm_rate << " Hz, "
      << server.tts_pcm_channels << " ch\n"
      << "  server.llm_cache_prompt = " << (server.llm_cache_prompt ? "true" : "false") << "\n"
      << "  streaming.asr_mode    = " << streaming.asr_mode << "\n"
      << "  streaming.tts_mode    = " << streaming.tts_mode << "\n"
      << "  streaming.llm_mode    = " << streaming.llm_mode << "\n"
      << "  wakeword.keyword      = " << wakeword.keyword
      << "  (threshold=" << wakeword.threshold
      << ", gate=" << (wakeword.require_real_context ? "on" : "off") << ")\n"
      << "  audio.input_device    = "
      << (audio.input_device.empty() ? "<default>" : audio.input_device) << "\n"
      << "  audio.output_device   = "
      << (audio.output_device.empty() ? "<default>" : audio.output_device) << "\n"
      << "  audio.capture_rate    = " << audio.capture_sample_rate << "\n"
      << "  assistant.min_utterance_ms = " << assistant.min_utterance_ms << "\n"
      << "  assistant.listen_timeout_s = " << assistant.listen_timeout_s << "\n"
      << "  assistant.speculate   = " << (assistant.speculate ? "on" : "off")
      << "  (stable=" << assistant.speculate_stable_ms
      << "ms, silence=" << assistant.speculate_silence_ms
      << "ms, min_words=" << assistant.speculate_min_words << ")\n";
  return out.str();
}

}  // namespace hoodie
