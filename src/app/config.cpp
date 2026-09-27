#include "app/config.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <fstream>
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
  const json& caps = sub(root, "capabilities");
  const json& wake = sub(root, "wakeword");
  const json& audio = sub(root, "audio");

  cfg.server.base_url = get_string(server, "base_url", cfg.server.base_url);
  cfg.server.api_key = get_string(server, "api_key", cfg.server.api_key);
  cfg.server.asr_path = get_string(server, "asr_path", cfg.server.asr_path);
  cfg.server.tts_path = get_string(server, "tts_path", cfg.server.tts_path);
  cfg.server.llm_path = get_string(server, "llm_path", cfg.server.llm_path);
  cfg.server.asr_model = get_string(server, "asr_model", cfg.server.asr_model);
  cfg.server.tts_model = get_string(server, "tts_model", cfg.server.tts_model);
  cfg.server.llm_model = get_string(server, "llm_model", cfg.server.llm_model);

  cfg.capabilities.llm_stream = get_bool(caps, "llm_stream", cfg.capabilities.llm_stream);
  cfg.capabilities.tts_stream = get_bool(caps, "tts_stream", cfg.capabilities.tts_stream);
  cfg.capabilities.tts_format = get_string(caps, "tts_format", cfg.capabilities.tts_format);
  cfg.capabilities.asr_stream = get_bool(caps, "asr_stream", cfg.capabilities.asr_stream);
  cfg.capabilities.asr_realtime = get_bool(caps, "asr_realtime", cfg.capabilities.asr_realtime);

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

  // Environment overrides win over the file, so secrets can stay out of it.
  apply_env_override("HOODIE_API_KEY", cfg.server.api_key);
  apply_env_override("HOODIE_BASE_URL", cfg.server.base_url);

  if (cfg.server.base_url.empty()) {
    throw std::runtime_error("config: server.base_url must not be empty");
  }
  if (cfg.audio.capture_sample_rate <= 0) {
    throw std::runtime_error("config: audio.capture_sample_rate must be positive");
  }
  return cfg;
}

Config Config::load_or_default(const std::string& path) {
  std::ifstream probe(path);
  if (!probe) {
    Config cfg;
    apply_env_override("HOODIE_API_KEY", cfg.server.api_key);
    apply_env_override("HOODIE_BASE_URL", cfg.server.base_url);
    return cfg;
  }
  probe.close();
  return load(path);
}

std::string Config::resolve_url(const std::string& path) const {
  if (server.base_url.empty()) return path;
  if (path.empty()) return server.base_url;
  const bool base_slash = server.base_url.back() == '/';
  const bool path_slash = path.front() == '/';
  if (base_slash && path_slash) return server.base_url + path.substr(1);
  if (!base_slash && !path_slash) return server.base_url + "/" + path;
  return server.base_url + path;
}

std::string Config::summary() const {
  std::ostringstream out;
  out << "config:\n"
      << "  server.base_url      = " << server.base_url << "\n"
      << "  server.api_key       = " << redact(server.api_key) << "\n"
      << "  server.asr_path      = " << server.asr_path << "  (model=" << server.asr_model << ")\n"
      << "  server.tts_path      = " << server.tts_path << "  (model=" << server.tts_model << ")\n"
      << "  server.llm_path      = " << server.llm_path << "  (model=" << server.llm_model << ")\n"
      << "  capabilities.llm_stream    = " << (capabilities.llm_stream ? "true" : "false") << "\n"
      << "  capabilities.tts_stream    = " << (capabilities.tts_stream ? "true" : "false")
      << "  (format=" << capabilities.tts_format << ")\n"
      << "  capabilities.asr_stream    = " << (capabilities.asr_stream ? "true" : "false") << "\n"
      << "  capabilities.asr_realtime  = " << (capabilities.asr_realtime ? "true" : "false") << "\n"
      << "  wakeword.keyword     = " << wakeword.keyword
      << "  (threshold=" << wakeword.threshold
      << ", gate=" << (wakeword.require_real_context ? "on" : "off") << ")\n"
      << "  audio.input_device   = " << (audio.input_device.empty() ? "<default>" : audio.input_device)
      << "\n"
      << "  audio.output_device  = "
      << (audio.output_device.empty() ? "<default>" : audio.output_device) << "\n"
      << "  audio.capture_rate   = " << audio.capture_sample_rate << "\n";
  return out.str();
}

}  // namespace hoodie
