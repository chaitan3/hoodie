#include "app/assistant.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <thread>
#include <utility>

#include "agent/agent.h"
#include "agent/text_normalize.h"
#include "agent/tools.h"
#include "app/playback.h"
#include "app/responder.h"
#include "audio/wav.h"

namespace hoodie::app {
namespace {

std::atomic<bool> g_interrupted{false};

void handle_signal(int) { g_interrupted.store(true, std::memory_order_relaxed); }

constexpr const char* kDefaultSystemPrompt =
    "You are a helpful assistant. Reply in one short sentence.";
constexpr auto kPollInterval = std::chrono::milliseconds(5);
constexpr std::size_t kScratchSamples = 8192;  // interleaved capture samples

wake::WakeWordConfig make_wake_config(const Config& config) {
  wake::WakeWordConfig wc;
  wc.melspectrogram_model = config.wakeword.melspectrogram_model;
  wc.embedding_model = config.wakeword.embedding_model;
  wc.keyword_model = config.wakeword.keyword_model;
  wc.keyword = config.wakeword.keyword;
  wc.threshold = static_cast<float>(config.wakeword.threshold);
  wc.warmup_frames = config.wakeword.warmup_frames;
  wc.require_real_context = config.wakeword.require_real_context;
  return wc;
}

double elapsed_seconds(std::chrono::steady_clock::time_point since) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count();
}

std::string wall_clock(std::chrono::system_clock::time_point when) {
  const std::time_t seconds = std::chrono::system_clock::to_time_t(when);
  std::tm local{};
  localtime_r(&seconds, &local);
  char buffer[16];
  std::strftime(buffer, sizeof(buffer), "%H:%M:%S", &local);
  return buffer;
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

Assistant::~Assistant() { clients_.asr->stop(); }

void Assistant::stop() {
  stopping_.store(true, std::memory_order_relaxed);
  cancel_.cancel();
}

const char* Assistant::state_name() const {
  switch (state_.load()) {
    case State::Idle: return "idle";
    case State::Listening: return "listening";
    case State::Thinking: return "thinking";
    case State::Speaking: return "speaking";
  }
  return "?";
}

void Assistant::set_state(State state) {
  if (state_.load() == state) return;
  spdlog::debug("state: {} -> {}", state_name(), [&] {
    switch (state) {
      case State::Idle: return "idle";
      case State::Listening: return "listening";
      case State::Thinking: return "thinking";
      case State::Speaking: return "speaking";
    }
    return "?";
  }());
  state_.store(state);
}

void Assistant::on_endpoint(wake::Endpointer::Event event, std::size_t /*frame_index*/) {
  if (event != wake::Endpointer::Event::SpeechEnd) return;
  // The wake-word tail trips SpeechStart, so a short pause right after the wake
  // ends the utterance before the command. Ignore end-of-speech until the
  // "time since wake" reaches min_utterance_seconds. Use the larger of the
  // accumulated audio and wall-clock: replaying a file runs far faster than
  // real time, while a live capture stream can deliver less audio than
  // wall-clock (idle/pacing), which would otherwise never reach the threshold.
  const double since_wake =
      std::max(utterance_.duration_seconds(), elapsed_seconds(listening_since_));
  if (since_wake < options_.min_utterance_seconds) {
    spdlog::debug("ignoring end at {:.2f}s (< min {:.2f}s)", since_wake,
                  options_.min_utterance_seconds);
    return;
  }
  utterance_ready_ = true;
}

void Assistant::enter_listening() {
  utterance_ = audio::AudioFrame{};
  utterance_.sample_rate = capture_rate_;
  utterance_.channels = 1;
  utterance_ready_ = false;
  asr_partial_.clear();
  asr_final_.clear();
  vad_.reset();
  endpointer_.reset();

  try {
    clients_.asr->start(cancel_, [this](const api::AsrDelta& delta) {
      if (delta.is_final) {
        asr_final_ = delta.text;
      } else {
        asr_partial_ = delta.text;
        spdlog::debug("asr partial: {}", asr_partial_);
      }
    });
  } catch (const std::exception& e) {
    spdlog::error("asr start failed: {}", e.what());
    reset_turn();
    return;
  }

  listening_since_ = std::chrono::steady_clock::now();
  utterance_started_at_ = std::chrono::system_clock::now();
  set_state(State::Listening);
}

void Assistant::reset_turn() {
  clients_.asr->stop();
  utterance_ready_ = false;
  utterance_ = audio::AudioFrame{};
  utterance_.sample_rate = capture_rate_;
  utterance_.channels = 1;
  // Drop audio captured while we were busy (ASR/agent/TTS, including our own
  // playback) so it is never fed back into the wake detector. The detector is
  // deliberately NOT reset: re-seeding its windows would re-arm the startup
  // gate - and its false positive - after every reply.
  ring_.clear();
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

  switch (state_.load()) {
    case State::Idle: {
      if (pcm16k_.empty()) break;
      const std::optional<float> score = wake_.process(pcm16k_.data(), pcm16k_.size());
      if (score.has_value()) {
        const float value = *score;
        const float threshold = wake_.threshold();
        // Trigger on a rising edge only. Because the detector is not reset
        // between turns, its windows still hold the wake word for ~1 s after a
        // reply and would otherwise re-fire immediately; re-arm once the score
        // has fallen well below the threshold.
        if (!wake_high_ && value >= threshold) {
          wake_high_ = true;
          spdlog::info("wake word '{}' detected (score {:.2f})", wake_.keyword(), value);
          enter_listening();
        } else if (wake_high_ && value < threshold * 0.5f) {
          wake_high_ = false;
        }
      }
      break;
    }
    case State::Listening: {
      // Keep every captured sample. The resampler buffers internally and may
      // emit nothing for a small input block, but that audio still belongs to
      // the utterance - skipping it made the utterance far shorter than the
      // audio VAD actually heard (e.g. 0.6 s of audio across a 3 s span).
      utterance_.samples.insert(utterance_.samples.end(), mono.samples.begin(),
                                mono.samples.end());
      if (!pcm16k_.empty()) {
        audio::AudioFrame pcm16;
        pcm16.sample_rate = audio::kSampleRate16k;
        pcm16.channels = 1;
        pcm16.samples = pcm16k_;
        try {
          clients_.asr->push(pcm16);
        } catch (const std::exception& e) {
          spdlog::warn("asr push failed: {}", e.what());
        }

        const std::vector<float> probs = vad_.process(pcm16k_.data(), pcm16k_.size());
        for (float p : probs) endpointer_.push(p);
      }
      break;
    }
    case State::Thinking:
    case State::Speaking:
      break;  // not consuming input while a response is in flight
  }
}

void Assistant::dump_utterance() {
  if (options_.dump_utterance_dir.empty()) return;

  std::error_code error;
  std::filesystem::create_directories(options_.dump_utterance_dir, error);
  if (error) {
    spdlog::warn("cannot create '{}': {}", options_.dump_utterance_dir, error.message());
    return;
  }

  char name[32];
  std::snprintf(name, sizeof(name), "utterance-%04d.wav", ++utterance_counter_);
  const std::filesystem::path path =
      std::filesystem::path(options_.dump_utterance_dir) / name;

  // Write exactly the bytes that will be POSTed to the ASR service.
  const std::vector<uint8_t> wav = audio::encode_wav_pcm16(utterance_);
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    spdlog::warn("cannot write '{}'", path.string());
    return;
  }
  out.write(reinterpret_cast<const char*>(wav.data()), static_cast<std::streamsize>(wav.size()));
  spdlog::info("saved utterance -> {} ({} Hz, {} ch, {:.2f} s)", path.string(),
               utterance_.sample_rate, utterance_.channels, utterance_.duration_seconds());
}

void Assistant::respond() {
  set_state(State::Thinking);

  // Cheap capture diagnostics: a garbage/silent utterance shows up in peak/rms,
  // and ring_dropped reveals whether the live loop is outrunning capture.
  int peak = 0;
  double sum_sq = 0.0;
  for (int16_t sample : utterance_.samples) {
    peak = std::max(peak, std::abs(static_cast<int>(sample)));
    sum_sq += static_cast<double>(sample) * static_cast<double>(sample);
  }
  const double rms = utterance_.samples.empty()
                         ? 0.0
                         : std::sqrt(sum_sq / static_cast<double>(utterance_.samples.size()));
  spdlog::info(
      "utterance: {} -> {} ({:.2f} s, {} samples @ {} Hz, peak={} rms={:.1f}, ring_dropped={})",
      wall_clock(utterance_started_at_), wall_clock(std::chrono::system_clock::now()),
      utterance_.duration_seconds(), utterance_.samples.size(), utterance_.sample_rate, peak, rms,
      ring_dropped_.load(std::memory_order_relaxed));
  dump_utterance();

  std::string transcript;
  try {
    const auto t0 = std::chrono::steady_clock::now();
    clients_.asr->commit();
    transcript = asr_final_;
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

  Responder responder(*clients_.tts, options_.out_file, options_.output_device);
  responder.start(cancel_, [this] { set_state(State::Speaking); });

  try {
    const auto t0 = std::chrono::steady_clock::now();
    result = agent.run(transcript, tool_context, cancel_,
                       [&](const std::string& token) { responder.push_text(token); });
    spdlog::info("agent: {} step(s) ({:.2f} s)", result.steps, elapsed_seconds(t0));
  } catch (const std::exception& e) {
    responder.abort();
    spdlog::error("agent failed: {}", e.what());
    reset_turn();
    return;
  }

  if (result.text.empty()) {
    responder.abort();
    spdlog::warn("agent returned an empty reply");
    reset_turn();
    return;
  }
  spdlog::info("reply: {}", result.text);

  responder.finish();
  if (responder.failed()) {
    spdlog::error("tts failed: {}", responder.error());
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
    const std::size_t count = frames * static_cast<std::size_t>(channels);
    const std::size_t written = ring_.write(samples, count);
    if (written < count) {
      ring_dropped_.fetch_add(count - written, std::memory_order_relaxed);
    }
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
      if (state_.load() == State::Listening && !endpointer_.in_speech() &&
          elapsed_seconds(listening_since_) > options_.listen_timeout_seconds) {
        if (utterance_.duration_seconds() >= 0.3) {
          // Safety net: the endpointer never produced an end we could honor, but
          // we did capture speech - process what we have rather than discarding.
          spdlog::warn("no end within {:.1f}s; processing {:.2f}s captured so far",
                       options_.listen_timeout_seconds, utterance_.duration_seconds());
          respond();
          if (options_.once) break;
        } else {
          spdlog::info("no speech after wake; returning to wake word");
          reset_turn();
        }
      }
      if (options_.idle_timeout_seconds > 0.0 && state_.load() == State::Idle &&
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

  const std::size_t block_frames =
      static_cast<std::size_t>(std::max(1, options_.file_block_frames));
  for (std::size_t offset = 0; offset < mono.samples.size(); offset += block_frames) {
    if (stopping_.load(std::memory_order_relaxed) ||
        g_interrupted.load(std::memory_order_relaxed)) {
      break;
    }
    const std::size_t count = std::min(block_frames, mono.samples.size() - offset);
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
