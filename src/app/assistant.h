#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "api/clients.h"
#include "app/config.h"
#include "audio/audio_format.h"
#include "audio/capture.h"
#include "audio/resampler.h"
#include "audio/ring_buffer.h"
#include "net/http.h"
#include "util/cancel.h"
#include "wake/endpointer.h"
#include "wake/vad.h"
#include "wake/wakeword.h"

namespace hoodie::app {

struct AssistantOptions {
  std::string input_device;    // empty = config.audio.input_device
  std::string output_device;   // empty = config.audio.output_device
  std::string system_prompt;
  std::string out_file;        // when set, write the reply audio here instead of playing
  std::string dump_utterance_dir;  // when set, save each utterance WAV sent to ASR
  bool once = false;           // handle a single turn, then return
  double idle_timeout_seconds = 0.0;    // 0 = wait forever for the wake word
  double listen_timeout_seconds = 8.0;  // max silence after wake before giving up
};

// The voice state machine:
//
//   Idle ──wake──► Listening ──endpoint──► Thinking ──► Speaking ──► Idle
//
// Idle feeds 16 kHz mono to the wake-word detector; Listening feeds it to
// Silero VAD + the endpointer while buffering the utterance at the capture
// rate; the finished utterance goes ASR -> agent -> TTS and is played back.
//
// Buffered clients only for now: ASR runs after the utterance ends, so a turn
// is serial. Streaming adapters drop in behind the same client interfaces.
class Assistant {
 public:
  Assistant(const Config& config, AssistantOptions options);
  ~Assistant();

  Assistant(const Assistant&) = delete;
  Assistant& operator=(const Assistant&) = delete;

  // Live microphone loop. Runs until stopped, times out, or (options.once) one
  // turn completes. Returns a process exit code.
  int run();

  // Offline driver: replay a WAV file through the same state machine. Lets the
  // whole loop be exercised without audio hardware.
  int run_file(const std::string& wav_path);

  // Request a clean stop from another thread (also cancels in-flight requests).
  void stop();

 private:
  enum class State { Idle, Listening, Thinking, Speaking };

  void on_endpoint(wake::Endpointer::Event event, std::size_t frame_index);
  void process_block(const audio::AudioFrame& mono);
  audio::AudioFrame make_mono_block(std::size_t samples);
  void enter_listening();
  void reset_turn();
  void respond();
  void dump_utterance();
  void set_state(State state);
  const char* state_name() const;

  const Config& config_;
  AssistantOptions options_;

  net::HttpClient http_;
  api::ClientSet clients_;

  wake::WakeWordDetector wake_;
  wake::SileroVad vad_;
  wake::Endpointer endpointer_;

  // Members touched by the PortAudio callback (ring_) are declared before the
  // capture stream so the stream is destroyed first.
  audio::SpscRingBuffer<int16_t> ring_;
  std::vector<int16_t> scratch_;  // interleaved capture samples
  std::vector<int16_t> pcm16k_;   // resampled mono 16 kHz
  std::unique_ptr<audio::Resampler> to_16k_;
  std::unique_ptr<audio::AudioCapture> capture_;

  audio::AudioFrame utterance_;  // mono, at the capture rate
  int capture_rate_ = 0;
  int capture_channels_ = 1;

  State state_ = State::Idle;
  bool utterance_ready_ = false;
  bool wake_high_ = false;  // rising-edge guard so a stale context does not re-trigger
  int utterance_counter_ = 0;
  std::chrono::steady_clock::time_point listening_since_{};

  util::CancelToken cancel_;
  std::atomic<bool> stopping_{false};
  std::atomic<std::size_t> ring_dropped_{0};  // capture samples dropped under load
};

}  // namespace hoodie::app
