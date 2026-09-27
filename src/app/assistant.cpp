#include "app/assistant.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <csignal>
#include <exception>
#include <thread>
#include <utility>

#include "agent/agent.h"
#include "agent/text_normalize.h"
#include "agent/tools.h"
#include "app/playback.h"
#include "audio/wav.h"

namespace hoodie::app {
namespace {

std::atomic<bool> g_interrupted{false};

void handle_signal(int) { g_interrupted.store(true, std::memory_order_relaxed); }

constexpr const char* kDefaultSystemPrompt =
    "You are a helpful assistant. Reply in one short sentence.";
constexpr auto kPollInterval = std::chrono::milliseconds(5);
constexpr std::size_t kScratchSamples = 8192;  // interleaved capture samples
constexpr std::size_t kBlockFrames = 1024;     // run_file block size (frames)

wake::WakeWordConfig make_wake_config(const Config& config) {
  wake::WakeWordConfig wc;
  wc.melspectrogram_model = config.wakeword.melspectrogram_model;
  wc.embedding_model = config.wakeword.embedding_model;
  wc.keyword_model = config.wakeword.keyword_model;
  wc.keyword = config.wakeword.keyword;
  wc.threshold = static_cast<float>(config.wakeword.threshold);
  wc.warmup_frames = config.wakeword.warmup_frames;
  return wc;
}

double elapsed_seconds(std::chrono::steady_clock::time_point since) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count();
}

}  // namespace

Assistant::Assistant(const Config& config, AssistantOptions options)
    : config_(config),
      options_(std::move(options)),
      clients_(api::make_clients(config, http_)),
      wake_(make_wake_config(config)),
      vad_(config.wakeword.vad_model, /*intra_op_threads=*/1),
      endpointer_(wake::Endpointer::Options{},
                  [this](wake::Endpointer::Event event, std::size_t index) {
                    on_endpoint(event, index);
                  }),
      ring_(1u << 16),
      scratch_(kScratchSamples) {}

Assistant::~Assistant() = default;

void Assistant::stop() {
  stopping_.store(true, std::memory_order_relaxed);
  cancel_.cancel();
}

const char* Assistant::state_name() const {
  switch (state_) {
    case State::Idle: return "idle";
    case State::Listening: return "listening";
    case State::Thinking: return "thinking";
    case State::Speaking: return "speaking";
  }
  return "?";
}

void Assistant::set_state(State state) {
  if (state_ == state) return;
  spdlog::debug("state: {} -> {}", state_name(), [&] {
    switch (state) {
      case State::Idle: return "idle";
      case State::Listening: return "listening";
      case State::Thinking: return "thinking";
      case State::Speaking: return "speaking";
    }
    return "?";
  }());
  state_ = state;
}

void Assistant::on_endpoint(wake::Endpointer::Event event, std::size_t /*frame_index*/) {
  if (event == wake::Endpointer::Event::SpeechEnd) utterance_ready_ = true;
}

void Assistant::enter_listening() {
  utterance_ = audio::AudioFrame{};
  utterance_.sample_rate = capture_rate_;
  utterance_.channels = 1;
  utterance_ready_ = false;
  vad_.reset();
  endpointer_.reset();
  listening_since_ = std::chrono::steady_clock::now();
  set_state(State::Listening);
}

void Assistant::reset_turn() {
  utterance_ready_ = false;
  utterance_ = audio::AudioFrame{};
  utterance_.sample_rate = capture_rate_;
  utterance_.channels = 1;
  wake_.reset();
  vad_.reset();
  endpointer_.reset();
  set_state(State::Idle);
}

audio::AudioFrame Assistant::make_mono_block(std::size_t samples) {
  const std::size_t ch = static_cast<std::size_t>(std::max(1, capture_channels_));
  const std::size_t whole = samples - (samples % ch);

  audio::AudioFrame mono;
  mono.sample_rate = capture_rate_;
  mono.channels = 1;
  if (ch == 1) {
    mono.samples.assign(scratch_.begin(), scratch_.begin() + static_cast<std::ptrdiff_t>(whole));
  } else {
    mono.samples.resize(whole / ch);
    for (std::size_t i = 0; i < mono.samples.size(); ++i) {
      int sum = 0;
      for (std::size_t c = 0; c < ch; ++c) sum += scratch_[i * ch + c];
      mono.samples[i] = static_cast<int16_t>(sum / static_cast<int>(ch));
    }
  }
  return mono;
}

void Assistant::process_block(const audio::AudioFrame& mono) {
  pcm16k_.clear();
  to_16k_->process(mono.samples.data(), mono.frames(), pcm16k_);
  if (pcm16k_.empty()) return;

  switch (state_) {
    case State::Idle: {
      const std::optional<float> score = wake_.process(pcm16k_.data(), pcm16k_.size());
      if (score.has_value() && *score >= wake_.threshold()) {
        spdlog::info("wake word '{}' detected (score {:.2f})", wake_.keyword(), *score);
        enter_listening();
      }
      break;
    }
    case State::Listening: {
      utterance_.samples.insert(utterance_.samples.end(), mono.samples.begin(), mono.samples.end());
      const std::vector<float> probs = vad_.process(pcm16k_.data(), pcm16k_.size());
      for (float p : probs) endpointer_.push(p);
      break;
    }
    case State::Thinking:
    case State::Speaking:
      break;  // not consuming input while a response is in flight
  }
}

void Assistant::respond() {
  set_state(State::Thinking);
  spdlog::info("utterance: {:.2f} s", utterance_.duration_seconds());

  std::string transcript;
  try {
    const auto t0 = std::chrono::steady_clock::now();
    transcript = clients_.asr->transcribe(utterance_, cancel_);
    spdlog::info("asr: {} ({:.2f} s)", clients_.asr->description(), elapsed_seconds(t0));
  } catch (const std::exception& e) {
    spdlog::error("asr failed: {}", e.what());
    reset_turn();
    return;
  }

  if (transcript.empty()) {
    spdlog::info("no speech recognised; returning to wake word");
    reset_turn();
    return;
  }
  spdlog::info("transcript: {}", transcript);

  agent::Agent agent(*clients_.llm, agent::builtin_tools(http_, config_),
                     agent::AgentOptions{
                         options_.system_prompt.empty() ? kDefaultSystemPrompt : options_.system_prompt,
                         /*model=*/"", /*max_steps=*/6});
  agent::ToolContext tool_context;
  agent::Agent::Result result;
  try {
    const auto t0 = std::chrono::steady_clock::now();
    result = agent.run(transcript, tool_context, cancel_);
    spdlog::info("agent: {} step(s) ({:.2f} s)", result.steps, elapsed_seconds(t0));
  } catch (const std::exception& e) {
    spdlog::error("agent failed: {}", e.what());
    reset_turn();
    return;
  }

  if (result.text.empty()) {
    spdlog::warn("agent returned an empty reply");
    reset_turn();
    return;
  }
  spdlog::info("reply: {}", result.text);

  set_state(State::Speaking);
  try {
    const auto t0 = std::chrono::steady_clock::now();
    const audio::AudioFrame speech =
        clients_.tts->synthesize(agent::normalize_for_tts(result.text), cancel_);
    spdlog::info("tts: {:.2f} s of audio ({:.2f} s)", speech.duration_seconds(),
                 elapsed_seconds(t0));
    play_or_write(speech, options_.out_file, options_.output_device);
  } catch (const std::exception& e) {
    spdlog::error("tts failed: {}", e.what());
    reset_turn();
    return;
  }

  if (tool_context.music.has_value()) {
    play_music_request(*tool_context.music, options_.out_file, options_.output_device);
  }

  reset_turn();
}

int Assistant::run() {
  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);
  g_interrupted.store(false, std::memory_order_relaxed);

  audio::AudioCapture::Options capture_options;
  capture_options.device = options_.input_device;
  capture_options.sample_rate = config_.audio.capture_sample_rate;
  capture_options.channels = 1;

  capture_ = std::make_unique<audio::AudioCapture>(capture_options);
  capture_->start([this](const int16_t* samples, std::size_t frames, int channels) {
    ring_.write(samples, frames * static_cast<std::size_t>(channels));
  });
  capture_rate_ = capture_->sample_rate();
  capture_channels_ = capture_->channels();
  to_16k_ = std::make_unique<audio::Resampler>(capture_rate_, audio::kSampleRate16k, 1);

  spdlog::info("listening for '{}' (input '{}', {} Hz, {} ch) - Ctrl-C to stop", wake_.keyword(),
               capture_->device_name(), capture_rate_, capture_channels_);

  const auto started = std::chrono::steady_clock::now();
  while (!(stopping_.load(std::memory_order_relaxed) ||
           g_interrupted.load(std::memory_order_relaxed))) {
    const std::size_t got = ring_.read(scratch_.data(), scratch_.size());
    if (got == 0) {
      if (state_ == State::Listening && !endpointer_.in_speech() &&
          elapsed_seconds(listening_since_) > options_.listen_timeout_seconds) {
        spdlog::info("no speech after wake; returning to wake word");
        reset_turn();
      }
      if (options_.idle_timeout_seconds > 0.0 && state_ == State::Idle &&
          elapsed_seconds(started) > options_.idle_timeout_seconds) {
        spdlog::info("idle timeout reached");
        break;
      }
      std::this_thread::sleep_for(kPollInterval);
      continue;
    }

    process_block(make_mono_block(got));
    if (utterance_ready_) {
      respond();
      if (options_.once) break;
    }
  }

  capture_->stop();
  capture_.reset();
  spdlog::info("stopped");
  return 0;
}

int Assistant::run_file(const std::string& wav_path) {
  const audio::AudioFrame raw = audio::read_audio_file(wav_path);
  if (raw.empty()) {
    spdlog::error("listen: no audio in {}", wav_path);
    return 1;
  }

  const audio::AudioFrame mono = audio::to_mono(raw);
  capture_rate_ = mono.sample_rate > 0 ? mono.sample_rate : audio::kSampleRate16k;
  capture_channels_ = 1;
  to_16k_ = std::make_unique<audio::Resampler>(capture_rate_, audio::kSampleRate16k, 1);

  spdlog::info("replaying '{}' ({} Hz, {:.2f} s) through the state machine", wav_path,
               capture_rate_, mono.duration_seconds());

  for (std::size_t offset = 0; offset < mono.samples.size(); offset += kBlockFrames) {
    if (stopping_.load(std::memory_order_relaxed) ||
        g_interrupted.load(std::memory_order_relaxed)) {
      break;
    }
    const std::size_t count = std::min(kBlockFrames, mono.samples.size() - offset);
    audio::AudioFrame block;
    block.sample_rate = capture_rate_;
    block.channels = 1;
    block.samples.assign(mono.samples.begin() + static_cast<std::ptrdiff_t>(offset),
                         mono.samples.begin() + static_cast<std::ptrdiff_t>(offset + count));

    process_block(block);
    if (utterance_ready_) {
      respond();
      if (options_.once) break;
    }
  }
  return 0;
}

}  // namespace hoodie::app
