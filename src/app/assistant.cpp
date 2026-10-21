#include "app/assistant.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
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
#include "app/speculator.h"
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

std::size_t word_count(const std::string& text) {
  std::size_t count = 0;
  bool in_word = false;
  for (unsigned char c : text) {
    if (std::isspace(c)) {
      in_word = false;
    } else if (!in_word) {
      in_word = true;
      ++count;
    }
  }
  return count;
}

// Compare ASR partial vs final without being derailed by case or punctuation.
std::string normalize_transcript(const std::string& text) {
  std::string out;
  bool pending_space = false;
  for (unsigned char c : text) {
    if (std::isalnum(c)) {
      if (pending_space && !out.empty()) out.push_back(' ');
      pending_space = false;
      out.push_back(static_cast<char>(std::tolower(c)));
    } else if (std::isspace(c)) {
      pending_space = true;
    }
  }
  return out;
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

Assistant::~Assistant() {
  stop_speculation();
  clients_.asr->stop();
}

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
  stop_speculation();
  utterance_ = audio::AudioFrame{};
  utterance_.sample_rate = capture_rate_;
  utterance_.channels = 1;
  utterance_ready_ = false;
  {
    std::lock_guard<std::mutex> lock(asr_text_mutex_);
    asr_partial_.clear();
    asr_final_.clear();
  }
  partial_snapshot_.clear();
  last_partial_audio_ = 0.0;
  last_speech_audio_ = 0.0;
  vad_.reset();
  endpointer_.reset();

  try {
    clients_.asr->start(cancel_, [this](const api::AsrDelta& delta) {
      std::lock_guard<std::mutex> lock(asr_text_mutex_);
      if (delta.is_final) {
        asr_final_ = delta.text;
      } else {
        asr_partial_ = delta.text;
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
  stop_speculation();
  clients_.asr->stop();
  utterance_ready_ = false;
  utterance_ = audio::AudioFrame{};
  utterance_.sample_rate = capture_rate_;
  utterance_.channels = 1;
  // Drop queued audio so it cannot be replayed after the turn. The detector is
  // deliberately NOT reset: re-seeding its windows would re-arm the startup
  // gate after every reply. Its context is kept current by process_block() in
  // the busy states, where scores are ignored.
  ring_.clear();
  vad_.reset();
  endpointer_.reset();
  set_state(State::Idle);
}

void Assistant::maybe_start_speculation(double now_audio) {
  if (!options_.speculate || speculation_started_) return;
  if (!clients_.asr->capabilities().partials) return;

  std::string partial;
  {
    std::lock_guard<std::mutex> lock(asr_text_mutex_);
    partial = asr_partial_;
  }
  if (partial != partial_snapshot_) {
    partial_snapshot_ = partial;
    last_partial_audio_ = now_audio;
  }
  if (partial.empty()) return;
  if (word_count(partial) < static_cast<std::size_t>(options_.speculate_min_words)) return;
  if (now_audio - last_partial_audio_ < options_.speculate_stable_ms / 1000.0) return;
  if (now_audio - last_speech_audio_ < options_.speculate_silence_ms / 1000.0) return;

  speculation_started_ = true;
  speculated_transcript_ = partial;
  speculator_ = std::make_unique<Speculator>(
      *clients_.llm, agent::builtin_tools(http_, config_),
      options_.system_prompt.empty() ? kDefaultSystemPrompt : options_.system_prompt);
  speculator_->start(speculated_transcript_, cancel_);
  spdlog::info("speculation: started on '{}'", speculated_transcript_);
}

void Assistant::stop_speculation() {
  if (speculator_) {
    speculator_->stop();
    speculator_.reset();
  }
  speculation_started_ = false;
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

  // Keep the wake detector's streaming windows current in every state. Scores
  // are only evaluated in Idle. If the detector is simply frozen during a turn,
  // its windows still contain the wake word when we return to Idle; as the old
  // window slides out it can dip below the release threshold and then score high
  // again, firing spuriously right after the reply.
  const std::optional<float> wake_score =
      pcm16k_.empty() ? std::nullopt : wake_.process(pcm16k_.data(), pcm16k_.size());

  switch (state_.load()) {
    case State::Idle: {
      if (wake_score.has_value()) {
        const float value = *wake_score;
        const float threshold = wake_.threshold();
        // Trigger on a rising edge only. Re-arm once the score has fallen well
        // below the threshold, so a single lingering high frame cannot re-fire.
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
        const double now_audio = utterance_.duration_seconds();
        for (float p : probs) {
          if (p >= 0.5f) last_speech_audio_ = now_audio;
          endpointer_.push(p);
        }
        maybe_start_speculation(now_audio);
      }
      break;
    }
    case State::Thinking:
    case State::Speaking:
      // No turn is consuming this audio, but wake_score above has already
      // advanced the detector's context so it is current when we return to Idle.
      break;
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

  // Save the captured utterance at the capture rate for inspection. ASR consumes
  // the resampled 16 kHz stream; buffered ASR wraps that stream in a WAV at commit.
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
    {
      std::lock_guard<std::mutex> lock(asr_text_mutex_);
      transcript = asr_final_;
    }
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

  // Confirm or discard a speculative LLM answer against the final transcript.
  std::string speculative_reply;
  bool use_speculation = false;
  if (speculation_started_) {
    const bool match =
        speculator_ != nullptr &&
        normalize_transcript(transcript) == normalize_transcript(speculated_transcript_);
    if (match && speculator_->wait(std::chrono::milliseconds(5000)) &&
        !speculator_->has_tools() && !speculator_->failed()) {
      speculative_reply = speculator_->text();
      use_speculation = !speculative_reply.empty();
    }
    spdlog::info("speculation: {} (match={})", use_speculation ? "used" : "discarded", match);
  }
  stop_speculation();

  agent::Agent agent(*clients_.llm, agent::builtin_tools(http_, config_),
                     agent::AgentOptions{
                         options_.system_prompt.empty() ? kDefaultSystemPrompt : options_.system_prompt,
                         /*model=*/"", /*max_steps=*/6});
  agent::ToolContext tool_context;
  agent::Agent::Result result;

  Responder responder(*clients_.tts, options_.out_file, options_.output_device);
  responder.start(cancel_, [this] { set_state(State::Speaking); });

  if (use_speculation) {
    result.text = speculative_reply;
    result.steps = 1;
    spdlog::info("agent: speculative reply (no tool steps)");
    responder.push_text(speculative_reply);
  } else {
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
