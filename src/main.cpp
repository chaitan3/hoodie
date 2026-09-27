#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#include "app/api_commands.h"
#include "app/assistant.h"
#include "api/api_error.h"
#include "agent/text_normalize.h"
#include "app/config.h"
#include "app/wake_commands.h"
#include "audio/audio_format.h"
#include "audio/capture.h"
#include "audio/devices.h"
#include "audio/player.h"
#include "audio/ring_buffer.h"
#include "audio/wav.h"
#include "util/logging.h"

namespace {

constexpr const char* kVersion = "0.1.0";

struct CliOptions {
  std::string config_path;
  bool list_devices = false;
  bool selftest = false;
  std::string play_file;
  std::string record_file;
  double record_seconds = 5.0;
  std::string input_device;
  std::string output_device;
  std::string transcribe_file;
  std::string ask_text;
  std::string agent_text;
  std::string norm_text;
  std::string say_text;
  std::string roundtrip_file;
  std::string out_file;
  std::string system_prompt;
  std::string wakeword_file;
  std::string vad_file;
  std::string endpoint_file;
  std::string tool_name;
  std::string tool_args;
  bool onnx_probe = false;
  bool listen = false;
  bool listen_once = false;
  std::string listen_wav;
  std::string save_utterances_dir;
  float threshold_override = 0.0f;
};

bool has_api_commands(const CliOptions& options) {
  return !options.transcribe_file.empty() || !options.ask_text.empty() ||
         !options.agent_text.empty() || !options.say_text.empty() ||
         !options.roundtrip_file.empty() || !options.tool_name.empty();
}

bool has_wake_commands(const CliOptions& options) {
  return !options.wakeword_file.empty() || !options.vad_file.empty() ||
         !options.endpoint_file.empty() || options.onnx_probe;
}

void print_usage(const char* argv0) {
  std::cout
      << "hoodie " << kVersion << " - voice assistant (C++)\n\n"
      << "Usage: " << argv0 << " [options]\n\n"
      << "General:\n"
      << "  -c, --config <path>      Configuration file (default: config/hoodie.json,\n"
      << "                           or $HOODIE_CONFIG if set)\n"
      << "  -h, --help               Show this help and exit\n"
      << "  -V, --version            Show version and exit\n\n"
      << "Audio diagnostics:\n"
      << "      --list-devices       List PortAudio devices and exit\n"
      << "      --selftest           Run hardware-free DSP/WAV checks and exit\n"
      << "      --record <file>      Record mic audio to a WAV file, then exit\n"
      << "      --seconds <n>        Recording length in seconds (default 5)\n"
      << "      --play <file>        Play an audio file, then exit\n"
      << "      --input-device <d>   Input device (name, index, or 'default')\n"
      << "      --output-device <d>  Output device (name, index, or 'default')\n\n"
      << "Assistant:\n"
      << "      --listen             Run the voice loop (wake -> ASR -> agent -> TTS)\n"
      << "      --listen-wav <file>  Replay a WAV through the voice loop (no mic)\n"
      << "      --once               Handle a single turn, then exit\n"
      << "      --save-utterances <d>  Save each utterance WAV sent to ASR in dir <d>\n"
      << "      --out <file>         Write the reply audio to a WAV instead of playing\n\n"
      << "Server round trips:\n"
      << "      --transcribe <file>  Transcribe an audio file, print the text\n"
      << "      --ask <text>         Send a prompt to the LLM, print the reply\n"
      << "      --agent <text>       Run the tool-calling agent, speak the reply\n"
      << "      --say <text>         Synthesise speech (play, or --out <file>)\n"
      << "      --roundtrip <file>   ASR -> agent -> TTS on an audio file\n"
      << "      --out <file>         Write TTS output to a WAV instead of playing\n"
      << "      --system <text>      Override the LLM system prompt\n"
      << "      --norm <text>        Print TTS-normalized text and exit\n"
      << "      --tool <name>        Invoke a built-in tool directly\n"
      << "      --args <json>        Arguments for --tool (default {})\n\n"
      << "Wake word / VAD:\n"
      << "      --wakeword <file>    Run the wake-word detector over an audio file\n"
      << "      --vad <file>         Run Silero VAD over an audio file\n"
      << "      --endpoint <file>    Run VAD + endpointer, print speech segments\n"
      << "      --threshold <x>      Override the detection threshold (0..1)\n\n"
      << "Environment:\n"
      << "  HOODIE_CONFIG     Config file path\n"
      << "  HOODIE_LOG_LEVEL  trace|debug|info|warn|error|critical|off\n"
      << "  HOODIE_API_KEY    Overrides server.api_key\n"
      << "  HOODIE_BASE_URL   Overrides server.base_url\n";
}

std::string default_config_path() {
  if (const char* env = std::getenv("HOODIE_CONFIG"); env != nullptr && *env != '\0') {
    return env;
  }
  return "config/hoodie.json";
}

std::string default_log_level() {
  if (const char* env = std::getenv("HOODIE_LOG_LEVEL"); env != nullptr && *env != '\0') {
    return env;
  }
  return "info";
}

hoodie::Config load_config(const std::string& path) {
  if (!std::ifstream(path).good()) {
    spdlog::warn("config file '{}' not found; using defaults", path);
  }
  return hoodie::Config::load_or_default(path);
}

// Returns true on success. On failure, `error` is set and false is returned.
bool parse_args(int argc, char** argv, CliOptions& options, std::string& error) {
  options.config_path = default_config_path();
  const auto need_value = [&](int& i, const std::string& flag, std::string& out) -> bool {
    if (i + 1 >= argc) {
      error = flag + " requires a value";
      return false;
    }
    out = argv[++i];
    return true;
  };

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      print_usage(argv[0]);
      std::exit(0);
    } else if (arg == "-V" || arg == "--version") {
      std::cout << "hoodie " << kVersion << "\n";
      std::exit(0);
    } else if (arg == "-c" || arg == "--config") {
      if (!need_value(i, arg, options.config_path)) return false;
    } else if (arg == "--list-devices") {
      options.list_devices = true;
    } else if (arg == "--selftest") {
      options.selftest = true;
    } else if (arg == "--play") {
      if (!need_value(i, arg, options.play_file)) return false;
    } else if (arg == "--record") {
      if (!need_value(i, arg, options.record_file)) return false;
    } else if (arg == "--seconds") {
      std::string value;
      if (!need_value(i, arg, value)) return false;
      try {
        options.record_seconds = std::stod(value);
      } catch (const std::exception&) {
        error = "invalid --seconds value: " + value;
        return false;
      }
    } else if (arg == "--input-device") {
      if (!need_value(i, arg, options.input_device)) return false;
    } else if (arg == "--output-device") {
      if (!need_value(i, arg, options.output_device)) return false;
    } else if (arg == "--transcribe") {
      if (!need_value(i, arg, options.transcribe_file)) return false;
    } else if (arg == "--ask") {
      if (!need_value(i, arg, options.ask_text)) return false;
    } else if (arg == "--agent") {
      if (!need_value(i, arg, options.agent_text)) return false;
    } else if (arg == "--norm") {
      if (!need_value(i, arg, options.norm_text)) return false;
    } else if (arg == "--tool") {
      if (!need_value(i, arg, options.tool_name)) return false;
    } else if (arg == "--args") {
      if (!need_value(i, arg, options.tool_args)) return false;
    } else if (arg == "--say") {
      if (!need_value(i, arg, options.say_text)) return false;
    } else if (arg == "--roundtrip") {
      if (!need_value(i, arg, options.roundtrip_file)) return false;
    } else if (arg == "--out") {
      if (!need_value(i, arg, options.out_file)) return false;
    } else if (arg == "--system") {
      if (!need_value(i, arg, options.system_prompt)) return false;
    } else if (arg == "--wakeword") {
      if (!need_value(i, arg, options.wakeword_file)) return false;
    } else if (arg == "--vad") {
      if (!need_value(i, arg, options.vad_file)) return false;
    } else if (arg == "--endpoint") {
      if (!need_value(i, arg, options.endpoint_file)) return false;
    } else if (arg == "--onnx-probe") {
      options.onnx_probe = true;
    } else if (arg == "--listen") {
      options.listen = true;
    } else if (arg == "--once") {
      options.listen_once = true;
    } else if (arg == "--listen-wav") {
      if (!need_value(i, arg, options.listen_wav)) return false;
    } else if (arg == "--save-utterances") {
      if (!need_value(i, arg, options.save_utterances_dir)) return false;
    } else if (arg == "--threshold") {
      std::string value;
      if (!need_value(i, arg, value)) return false;
      try {
        options.threshold_override = std::stof(value);
      } catch (const std::exception&) {
        error = "invalid --threshold value: " + value;
        return false;
      }
    } else {
      error = "unknown argument '" + arg + "'";
      return false;
    }
  }
  return true;
}

int run_record(const CliOptions& options, const hoodie::Config& config) {
  using namespace hoodie::audio;

  AudioCapture::Options capture_options;
  capture_options.device =
      options.input_device.empty() ? config.audio.input_device : options.input_device;
  capture_options.sample_rate = config.audio.capture_sample_rate;
  capture_options.channels = 1;

  AudioCapture capture(capture_options);

  AudioFrame recording;
  recording.sample_rate = config.audio.capture_sample_rate;
  recording.channels = 1;

  std::mutex mutex;
  capture.start([&](const int16_t* samples, std::size_t frames, int channels) {
    std::lock_guard<std::mutex> lock(mutex);
    recording.samples.insert(recording.samples.end(), samples,
                             samples + frames * static_cast<std::size_t>(channels));
  });
  recording.channels = capture.channels();
  recording.sample_rate = capture.sample_rate();

  spdlog::info("recording {} s...", options.record_seconds);
  std::this_thread::sleep_for(std::chrono::duration<double>(options.record_seconds));
  capture.stop();

  {
    std::lock_guard<std::mutex> lock(mutex);
    spdlog::info("recorded {} frames ({:.2f} s)", recording.frames(),
                 recording.duration_seconds());
    write_audio_file(options.record_file, recording);
  }
  spdlog::info("wrote {}", options.record_file);
  return 0;
}

int run_play(const CliOptions& options, const hoodie::Config& config) {
  using namespace hoodie::audio;

  const AudioFrame frame = read_audio_file(options.play_file);
  spdlog::info("playing '{}' ({} Hz, {} ch, {:.2f} s)", options.play_file, frame.sample_rate,
               frame.channels, frame.duration_seconds());

  AudioPlayer::Options player_options;
  player_options.device =
      options.output_device.empty() ? config.audio.output_device : options.output_device;
  player_options.sample_rate = 0;  // device default
  player_options.channels = 1;

  AudioPlayer player(player_options);
  player.start();
  player.enqueue(frame);
  const bool drained = player.wait_drained();
  player.stop();
  if (!drained) {
    spdlog::warn("playback did not drain within timeout");
    return 1;
  }
  return 0;
}

int run_selftest() {
  using namespace hoodie::audio;
  constexpr double kPi = 3.14159265358979323846;
  int failures = 0;
  const auto check = [&](bool ok, const char* what) {
    if (ok) {
      spdlog::info("ok:   {}", what);
    } else {
      spdlog::error("FAIL: {}", what);
      ++failures;
    }
  };

  // 1. WAV encode/decode round trip.
  AudioFrame tone;
  tone.sample_rate = kSampleRate48k;
  tone.channels = 1;
  tone.samples.resize(48000);
  for (std::size_t i = 0; i < tone.samples.size(); ++i) {
    tone.samples[i] = static_cast<int16_t>(
        std::sin(2.0 * kPi * 440.0 * static_cast<double>(i) / kSampleRate48k) * 10000.0);
  }
  const AudioFrame round_tripped = decode_wav_pcm16(encode_wav_pcm16(tone));
  check(round_tripped.sample_rate == tone.sample_rate && round_tripped.channels == 1 &&
            round_tripped.samples == tone.samples,
        "wav encode/decode round trip");

  // 2. Streaming resample 48k -> 16k (1 s in, ~16000 out).
  {
    Resampler resampler(kSampleRate48k, kSampleRate16k, 1);
    std::vector<int16_t> out;
    resampler.process(tone.samples.data(), tone.frames(), out);
    resampler.flush(out);
    check(std::abs(static_cast<double>(out.size()) - 16000.0) < 160.0,
          "resample 48k -> 16k length ~= 16000");
  }

  // 3. Resample 48k -> 24k in two chunks (statefulness across calls).
  {
    Resampler resampler(kSampleRate48k, kSampleRate24k, 1);
    std::vector<int16_t> out;
    const std::size_t half = tone.frames() / 2;
    resampler.process(tone.samples.data(), half, out);
    resampler.process(tone.samples.data() + half, tone.frames() - half, out);
    resampler.flush(out);
    check(std::abs(static_cast<double>(out.size()) - 24000.0) < 240.0,
          "resample 48k -> 24k (chunked) length ~= 24000");
  }

  // 3b. Determinism: two instances, same input -> identical output.
  {
    Resampler first(kSampleRate48k, kSampleRate16k, 1);
    Resampler second(kSampleRate48k, kSampleRate16k, 1);
    std::vector<int16_t> a;
    std::vector<int16_t> b;
    first.process(tone.samples.data(), tone.frames(), a);
    first.flush(a);
    second.process(tone.samples.data(), tone.frames(), b);
    second.flush(b);
    check(a == b && !a.empty(), "resample is deterministic across instances");
  }

  // 4. SPSC ring buffer limits and wrap-around.
  {
    SpscRingBuffer<int16_t> ring(1000);
    std::vector<int16_t> src(1500, 7);
    std::vector<int16_t> dst(1500, 0);
    const std::size_t written = ring.write(src.data(), src.size());
    const std::size_t read = ring.read(dst.data(), dst.size());
    bool all_seven = true;
    for (std::size_t i = 0; i < read; ++i) all_seven = all_seven && dst[i] == 7;
    check(written == ring.capacity() && read == ring.capacity() && all_seven && ring.empty(),
          "spsc ring buffer capacity/limits");
  }

  // 5. Read the bundled OGG if present (libsndfile format coverage).
  {
    try {
      const AudioFrame beep = read_audio_file("beep.ogg");
      spdlog::info("ok:   read beep.ogg ({} Hz, {} ch, {:.2f} s)", beep.sample_rate,
                   beep.channels, beep.duration_seconds());
    } catch (const std::exception& e) {
      spdlog::warn("skip: beep.ogg not readable here ({})", e.what());
    }
  }

  // 6. TTS text normalization.
  {
    const std::string a = hoodie::agent::normalize_for_tts("I have 2.5 apples, *fresh*!");
    check(a == "I have two point five apples, fresh!",
          "normalize: numbers + markdown emphasis");
    const std::string b = hoodie::agent::normalize_for_tts("See [docs](https://x.test) now");
    check(b == "See docs now", "normalize: markdown link");
    check(hoodie::agent::number_to_words(1234LL) == "one thousand two hundred thirty four",
          "normalize: integer to words");
  }

  if (failures == 0) {
    spdlog::info("selftest: PASSED");
  } else {
    spdlog::info("selftest: {} check(s) FAILED", failures);
  }
  return failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  CliOptions options;
  std::string parse_error;
  if (!parse_args(argc, argv, options, parse_error)) {
    std::cerr << "error: " << parse_error << "\n";
    print_usage(argv[0]);
    return 2;
  }

  hoodie::util::init_logging(default_log_level());

  try {
    if (options.selftest) {
      return run_selftest();
    }
    if (!options.norm_text.empty()) {
      std::cout << hoodie::agent::normalize_for_tts(options.norm_text) << "\n";
      return 0;
    }
    if (options.list_devices) {
      hoodie::audio::print_devices(std::cout);
      return 0;
    }

    if (options.play_file.empty() && options.record_file.empty() &&
        !has_api_commands(options) && !has_wake_commands(options) && !options.listen &&
        options.listen_wav.empty()) {
      const hoodie::Config config = load_config(options.config_path);
      spdlog::info("hoodie {} ready", kVersion);
      spdlog::info("loaded configuration from {}", options.config_path);
      spdlog::info("\n{}", config.summary());
      return 0;
    }

    const hoodie::Config config = load_config(options.config_path);

    if (options.listen || !options.listen_wav.empty()) {
      hoodie::app::AssistantOptions assistant_options;
      assistant_options.input_device =
          options.input_device.empty() ? config.audio.input_device : options.input_device;
      assistant_options.output_device =
          options.output_device.empty() ? config.audio.output_device : options.output_device;
      assistant_options.system_prompt = options.system_prompt;
      assistant_options.out_file = options.out_file;
      assistant_options.dump_utterance_dir = options.save_utterances_dir;
      assistant_options.once = options.listen_once;
      hoodie::app::Assistant assistant(config, std::move(assistant_options));
      if (!options.listen_wav.empty()) {
        return assistant.run_file(options.listen_wav);
      }
      return assistant.run();
    }

    if (has_wake_commands(options)) {
      if (options.onnx_probe) {
        return hoodie::app::run_onnx_probe(config);
      }
      if (!options.wakeword_file.empty()) {
        return hoodie::app::run_wakeword_check(config, options.wakeword_file,
                                               options.threshold_override);
      }
      if (!options.endpoint_file.empty()) {
        return hoodie::app::run_endpoint_check(config, options.endpoint_file);
      }
      return hoodie::app::run_vad_check(config, options.vad_file, options.threshold_override);
    }

    if (has_api_commands(options)) {
      hoodie::app::ApiCommandOptions api_options;
      api_options.out_file = options.out_file;
      api_options.output_device =
          options.output_device.empty() ? config.audio.output_device : options.output_device;
      api_options.system_prompt = options.system_prompt;

      if (!options.transcribe_file.empty()) {
        return hoodie::app::run_transcribe(config, api_options, options.transcribe_file);
      }
      if (!options.tool_name.empty()) {
        return hoodie::app::run_tool(config, options.tool_name, options.tool_args);
      }
      if (!options.ask_text.empty()) {
        return hoodie::app::run_ask(config, api_options, options.ask_text);
      }
      if (!options.agent_text.empty()) {
        return hoodie::app::run_agent(config, api_options, options.agent_text);
      }
      if (!options.say_text.empty()) {
        return hoodie::app::run_say(config, api_options, options.say_text);
      }
      return hoodie::app::run_roundtrip(config, api_options, options.roundtrip_file);
    }

    if (!options.record_file.empty()) {
      return run_record(options, config);
    }
    return run_play(options, config);
  } catch (const hoodie::api::ApiError& e) {
    spdlog::error("api error{}: {}", e.status != 0 ? " (HTTP " + std::to_string(e.status) + ")" : "",
                  e.what());
    return 1;
  } catch (const std::exception& e) {
    spdlog::error("failure: {}", e.what());
    return 1;
  }
}
