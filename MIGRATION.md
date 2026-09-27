# Hoodie → pure C++ migration plan

Status: draft for discussion
Decisions assumed (confirmed):

- **Thin client, remote APIs.** C++ owns audio hardware, wake word, VAD, the voice
  state machine, and the LLM tool loop. ASR / LLM / TTS stay on the remote
  OpenAI-compatible server.
- **Adapter layer for both transports.** Implement streaming HTTP first, then a
  Realtime WebSocket adapter. Same interfaces behind both.
- **Raspberry Pi first.** Build and tune on aarch64 (Pi 4/5), desktop secondary.
- **Tool-calling loop lives in C++.** Re-implement equivalent behavior to today's
  `pydantic_ai` agent.

## Progress log

- **Step 1 (done):** buildable C++ skeleton. CMake + FetchContent (`spdlog`,
  `nlohmann/json`), `src/app/config.{h,cpp}` (JSON config, env overrides,
  redacted summary, per-endpoint paths + capability flags), `src/util/logging.*`,
  and a `hoodie` binary that loads config and prints a redacted summary.
  Build: `cmake -S . -B build -G Ninja && cmake --build build`.
- **Config format:** JSON (not TOML) to avoid an extra dependency; example at
  `config/hoodie.example.json`. `config/hoodie.json` is gitignored.
- **Step 2 (done):** audio I/O foundation. `src/audio/`: `audio_format.h`
  (canonical interleaved int16 `AudioFrame`), `ring_buffer.h` (lock-free SPSC),
  `resampler.{h,cpp}` (soxr, streaming, 48k↔16k↔24k), `wav.{h,cpp}`
  (libsndfile file I/O + in-memory PCM16 WAV), `devices.{h,cpp}`
  (PortAudio init/list/resolve-by-name), `capture.{h,cpp}`, `player.{h,cpp}`
  (streaming PCM playback with resample + silence-on-underrun).
  System deps: PortAudio, libsndfile, soxr (via pkg-config).
  CLI diagnostics: `--list-devices`, `--record <file> [--seconds n]`,
  `--play <file>`, and hardware-free `--selftest` (WAV round trip, resampling,
  ring buffer, OGG read) — all passing. `--record`/`--play` need a real device
  and have not been exercised on hardware yet.
- **Step 3 (done):** HTTP/API parity + buffered fallback.
  `src/util/cancel.h` (CancelToken), `src/net/http.{h,cpp}` (libcurl: buffered
  JSON, multipart, streaming chunk callback, cancellation), `src/api/`
  (`openai_types`, `api_error`, `api_util`; `AsrClient`/`TtsClient`/`LlmClient`
  interfaces with `Buffered*` implementations; `make_clients` selects per
  capability and logs the fallback). `src/audio/wav.*` gained in-memory decode
  (`decode_pcm16`, `decode_audio_memory` via libsndfile virtual IO) for TTS
  bodies. CLI: `--transcribe`, `--ask`, `--say [--out]`, `--roundtrip`.
  Tests: `tests/mock_openai_server.py` + `tests/integration.sh` covering all
  three buffered clients and the dead-server path — PASSING.
  Streaming clients (SSE LLM/ASR, chunked PCM TTS) remain for Phase 5; the
  capability flags currently log and fall back to buffered.
- **Step 4 (Phase 2, done):** wake word + VAD + endpointer.
  `src/ml/onnx.{h,cpp}` (ONNX Runtime RAII wrapper), `src/wake/wakeword.{h,cpp}`
  (openWakeWord melspectrogram→embedding→keyword streaming pipeline, 80 ms
  steps), `src/wake/vad.{h,cpp}` (Silero VAD, 40 ms frames, recurrent state),
  `src/wake/endpointer.h` (VAD→speech start/end; fixes the original inverted
  stop-on-onset logic). Models via `scripts/fetch_models.sh`. CLI: `--wakeword`,
  `--vad`, `--endpoint`, `--onnx-probe`. `tests/wake_test.sh` — PASSING
  (alexa_test.wav detected max=1.0; silence/tone rejected; VAD 0.99 on speech
  vs 0.06 on silence; deterministic).

  Two real bugs found and fixed along the way (both caused nondeterministic
  detections — e.g. silence scoring 0.8):
  1. **`std::deque` pointer arithmetic.** The detector extracted 1280-sample
     chunks with `&pending_[0]` + pointer walk; deques aren't contiguous, so it
     read across internal blocks into uninitialized memory. `pending_` is now a
     `std::vector` (contiguous).
  2. **soxr integer I/O is non-deterministic here.** `SOXR_INT16_I` output
     differed run-to-run; switched to `SOXR_FLOAT32_I` (single-threaded runtime)
     with our own int16 conversion. Added a resample-determinism check to
     `--selftest`.
- **Step 5 (Phase 4, done):** agent + tools + TTS normalization.
  `src/agent/text_normalize.{h,cpp}` (number→words, markdown/URL/code stripping),
  `src/agent/tool_registry.{h,cpp}` (JSON-schema tools, `ToolContext`),
  `src/agent/tools.{h,cpp}` (get_time, get_weather, play_music, search),
  `src/agent/agent.{h,cpp}` (tool-call loop with max-steps), `src/util/process.*`
  (shell-free `execvp` runner for yt-dlp). CLI: `--agent`, `--tool`/`--args`,
  `--norm`; `--say`/`--roundtrip` now normalize text and `--roundtrip` uses the
  agent. `tests/agent_test.sh` scripts a get_time tool call via the mock and
  asserts the loop completes — PASSING. `get_weather` and `get_time` verified
  directly; `play_music` needs yt-dlp (not installed) and degrades gracefully;
  `search` hits DuckDuckGo's Instant Answer API (currently bot-blocked from this
  network, returns a clear no-results string).

- **Step 6 (orchestrator, done):** the voice state machine.
  `src/app/assistant.{h,cpp}` (`Idle → Listening → Thinking → Speaking`): live
  PortAudio capture into a lock-free ring, wake word → Silero VAD → endpointer
  to segment a single utterance, then buffered ASR → agent → TTS → playback.
  Playback helpers were factored into `src/app/playback.{h,cpp}` (now shared by
  `--roundtrip`). CLI: `--listen` (live mic, Ctrl-C to stop, `--once` for one
  turn) and `--listen-wav <file>` (replay a WAV through the same state machine,
  no mic). Capture falls back to the device channel count and downmixes when the
  requested mono format is unsupported (stereo-only codecs). Barge-in/AEC are
  still deferred (mic is not consumed while Thinking/Speaking).
  `tests/listen_test.sh` drives wake → ASR → agent → TTS against the mock server
  with no hardware — PASSING.
- **Step 7 (done, wake-word startup gate):** the detector now withholds scores
  until its mel/embedding/keyword windows contain only real audio
  (`wakeword.require_real_context`, default true). Root cause of the false wake:
  `reset()` seeds those windows with synthetic padding, and the first score
  after `warmup_frames` (~0.4 s) could reach ~0.98 on real room tone — a false
  positive that the digital-silence tests never exercised. Gate holds ~2.4 s
  once at startup (76 mel frames + 16 embedding frames). The orchestrator no
  longer resets the detector per reply (that re-armed the gate and the false
  positive); it clears the capture ring instead, so audio buffered during
  ASR/TTS — including our own playback — is never fed to the detector. Set
  `wakeword.require_real_context: false` to restore the old behaviour.
  `tests/wake_test.sh` now uses a realistic lead-in stream (the 0.6 s clip is
  too short to score under the gate), adds a room-tone-noise negative that
  reproduces `max≈0.98 @ 0.48 s` with the gate off, and asserts the toggle —
  PASSING.
- **Step 8 (done, minimum utterance):** end-of-speech is ignored until the
  utterance holds `assistant.min_utterance_ms` (default 1500; CLI
  `--min-utterance-ms`; 0 disables). Fixes the wake-word-tail truncation: the
  tail trips `SpeechStart`, and a natural pause then ends the utterance at
  ~0.6 s — exactly the endpointer's silence window — so ASR received a fragment
  and returned empty. The endpointer keeps running; only its *end* event is
  filtered, so a later command produces a fresh `SpeechStart`/`SpeechEnd` that
  is honored. `assistant.listen_timeout_s` is config too, and the utterance log
  now shows the wall-clock start -> end. `tests/listen_test.sh` adds a pause
  fixture (wake + 1 s gap + command) and asserts a single ~2.3 s turn.
- **Step 9 (done, utterance drop fix):** `process_block` returned early when the
  resampler emitted no output for a small input block, *before* appending to
  `utterance_`. The resampler buffers internally, so those samples still reached
  VAD later - VAD saw the full span while the utterance kept only a fraction
  (observed: 0.6 s of audio across a 3.3 s turn). `utterance_` now accumulates
  from every `Listening` block regardless of resampler output. `run_file()` takes
  a configurable block size and `tests/listen_test.sh` runs the pause fixture
  with 1-frame blocks to force the condition - PASSING.

---

## 1. Current system (baseline to preserve or improve)

Serial, blocking pipeline, one utterance at a time:

```
mic (48k) -> WakeWord (openwakeword, 16k) -> VAD (webrtcvad) -> record whole command
          -> ASR  POST /v1/audio/transcriptions (whole WAV)
          -> LLM  agent (pydantic_ai, blocking, may call tools)
          -> TTS  POST /v1/audio/speech (whole file)
          -> AudioPlayer thread plays file
```

The original Python sources now live under `legacy/` (kept for reference; they
are no longer built or run).

Known defects to fix during the port (do **not** port them):

| # | Issue | Location |
|---|---|---|
| 1 | Failure path references undefined `audio_data`; `perform_task` always returns `True`, so failure handling is dead code | `legacy/main.py:59-62`, `legacy/actions.py:75` |
| 2 | `wait_inactive` stops on speech **onset** (approach looks inverted) instead of after silence | `legacy/vad.py:21-24` |
| 3 | Wake word re-verified by ASR against literal `"alexa"` | `legacy/main.py:47-50` |
| 4 | Wake word downsamples by raw decimation, no anti-alias filter | `legacy/wakeword.py:38` |
| 5 | Hardcoded device IDs, toggled by comments (desktop vs Pi) | `legacy/audio.py:14-29` |
| 6 | Live API key committed in source | `legacy/config.py:1` |
| 7 | `yt-dlp` invoked with `shell=True` on LLM-provided text | `legacy/tools.py:36,48` |
| 8 | No cancellation / barge-in; blocking `requests` | everywhere |

---

## 2. Target architecture

```
                        ┌───────────────────────── VoiceAssistant (state machine) ─────────────────────────┐
                        │                                                                                   │
 mic ─► Capture ─► SPSC ring ─► WakeWord ─► Endpointer ─► AsrClient ─► text ─► Agent ─► tokens ─► SentenceSplitter
                        │        (onnx)      (Silero)          ▲                        │                         │
                        │                                     │                  ToolRegistry                     │
                        │                              (streaming earlier)       (time/weather/music/search)      │
 speaker ◄─ Player ◄─ SPSC ring ◄────────────────────────── TtsClient ◄─────────────────────────────────────────┘
                        │
                        └─ CancelToken ──► aborts in-flight HTTP/WS, flushes ring + player (barge-in)
```

Key properties:

- **Pipelined, not serial.** ASR streams while the user speaks; LLM tokens stream;
  TTS is requested per sentence and played as chunks arrive.
- **Everything behind an interface**, so HTTP / Realtime / (future) local backends
  are interchangeable, with a startup capability probe and graceful fallback.
- **Cancellation is first class** (`CancelToken`) so barge-in and errors never
  leave the player stuck.
- **Canonical audio formats:** capture 48 kHz int16 mono → resample to 16 kHz for
  wake/VAD, 24 kHz for API transport/Realtime/TTS PCM.

### 2.1 State machine

```
Idle ──wake──► Listening ──endpoint──► Thinking ──first token──► Speaking
  ▲                ▲                                                  │
  └──── timeout ───┴──────────────── barge-in / done ────────────────┘
```

- `Idle`: wake word runs on 16 kHz.
- `Listening`: VAD endpointer decides utterance end; streaming ASR already active.
- `Thinking`: LLM + tool loop; TTS starts as soon as the first sentence is ready.
- `Speaking`: streaming PCM playback; mic stays hot for barge-in.
- Any transition cancels the previous stage's token.

---

## 3. Dependency map

| Concern | Python today | C++ choice | Notes |
|---|---|---|---|
| Audio I/O | sounddevice | **PortAudio** | matches sherpa-onnx; device select by name |
| WAV read/write | soundfile | **libsndfile** | |
| Resampling | resampy | **soxr** | also fixes naive decimation |
| Wake word | openwakeword | **onnxruntime** + openwakeword ONNX models | validate mel/embedding preprocessing against Python |
| VAD | webrtcvad | **Silero VAD** (onnxruntime) | better than webrtcvad |
| HTTP client | requests | **libcurl** | chunked + SSE + cancel callback |
| WebSocket | — | **IXWebSocket** | Realtime adapter |
| JSON | stdlib json | **nlohmann/json** | |
| LLM / tools | pydantic_ai | hand-rolled client + tool loop | nlohmann schemas |
| Audio decode | soundfile | **dr_mp3** + **stb_vorbis** (+ libopus) | TTS fallback formats |
| Number→words / TTS text cleanup | inflect | small custom module | plus markdown/emoji/URL stripping |
| Music | yt-dlp subprocess | keep yt-dlp, exec (no shell) | |
| Search | ddgs / Google PSE | libcurl | |
| Logging | logging | **spdlog** | |
| Metrics | — | tiny latency recorder | per-stage |
| Tests | — | **Catch2** + mock server | golden audio |
| Build | pip | **CMake** + vcpkg/FetchContent | aarch64 first |

No `sherpa-onnx` in the thin-client design; it stays an option behind the
`WakeWord`/`Vad`/`AsrClient`/`TtsClient` interfaces if local fallback is wanted later.

---

## 4. Repository layout

```
CMakeLists.txt
cmake/                      deps.cmake, toolchain-aarch64.cmake
config/hoodie.example.json
models/                     openwakeword onnx, silero_vad.onnx
src/
  main.cpp
  app/
    assistant.{h,cpp}       orchestration + state machine
    config.{h,cpp}          env/file config, secrets
    cancel.{h,cpp}          CancelToken
  audio/
    capture.{h,cpp}         PortAudio input
    player.{h,cpp}          streaming PCM output queue
    ring_buffer.h           SPSC lock-free
    resampler.{h,cpp}       soxr wrapper
    wav.{h,cpp}             libsndfile wrapper
    audio_format.h
  wake/
    wakeword.{h,cpp}        openwakeword via onnxruntime
    vad.{h,cpp}             Silero VAD
    endpointer.{h,cpp}      min-speech / silence logic
  net/
    http.{h,cpp}            libcurl wrapper (streaming + cancel)
    sse.{h,cpp}             SSE line parser
    ws.{h,cpp}              IXWebSocket wrapper
  api/
    asr_client.{h,cpp}      interface + SseAsr, BufferedAsr (RealtimeAsr later)
    llm_client.{h,cpp}      interface + SseLlm, BufferedLlm
    tts_client.{h,cpp}      interface + StreamPcmTts, BufferedTts
    openai_types.{h,cpp}
    capabilities.{h,cpp}    startup probe / fallback
  agent/
    agent.{h,cpp}           tool-call loop
    tool_registry.{h,cpp}
    tools/                  time, weather, music, search
    sentence_splitter.{h,cpp}
    text_normalize.{h,cpp}
  util/
    logging.{h,cpp}         spdlog
    metrics.{h,cpp}
tests/
  unit/  integration/  audio/
```

---

## 5. Interfaces (sketch)

```cpp
struct AudioFrame { std::vector<int16_t> pcm; int sample_rate; int channels; };

class WakeWord {
 public:
  virtual ~WakeWord() = default;
  virtual std::optional<WakeEvent> process(const AudioFrame&) = 0;
};

class Endpointer {                 // wraps Vad + timing
 public:
  virtual bool speech_started() const = 0;
  virtual bool utterance_done() const = 0;
  virtual void push(const AudioFrame&) = 0;
};

struct AsrDelta { std::string text; bool is_final; };
class AsrClient {
 public:
  virtual void start(const CancelToken&, std::function<void(AsrDelta)> on_delta) = 0;
  virtual void push(const AudioFrame&) = 0;     // live audio
  virtual void commit() = 0;                    // end of utterance
  virtual void stop() = 0;
};

struct ToolCall { std::string id, name, arguments_json; };
class LlmClient {
 public:
  virtual void chat(const CancelToken&,
                    const std::vector<Message>& history,
                    const ToolSchemaList& tools,
                    std::function<void(const LlmEvent&)> on_event) = 0; // token | tool_call | done
};

class TtsClient {
 public:
  virtual void synthesize(const CancelToken&, const std::string& text,
                          std::function<void(AudioFrame)> on_chunk,
                          std::function<void()> on_done) = 0;
};
```

`VoiceAssistant` owns one instance of each and drives the state machine; backends
are injected (Http vs Realtime), so architecture experiments are config swaps.

---

## 6. Endpoint contracts

**LLM (streaming, tools):**
```
POST {base_url}/chat/completions
{ "model": "...", "stream": true, "stream_options": {"include_usage": true},
  "messages": [...], "tools": [ ...json schema... ] }
# SSE: choices[0].delta.content | delta.tool_calls[i] | finish_reason | [DONE]
```
Accumulate `tool_calls` by index; when `finish_reason == "tool_calls"`, execute
locally, append `role:"tool"` messages, loop. Otherwise stream text to the
sentence splitter.

**TTS (streaming PCM):**
```
POST {base_url}/audio/speech
{ "model": "...", "input": "<sentence>", "voice": "...",
  "response_format": "pcm", "stream": true }
# chunked body -> int16 LE, 24 kHz, mono -> Player
```
Fallback if the server ignores `stream`: buffer, detect content-type, decode
(wav/pcm/mp3/opus), then play.

**ASR — SSE, file-based (chosen):**
```
POST {base_url}/v1/audio/transcriptions   (multipart, stream=true)
-> text/event-stream
   transcript.text.delta  { delta: "..." }   # partial text while file is processed
   transcript.text.done   { text: "..." }    # final transcript
```
The complete utterance WAV is uploaded; only the *response* streams. This is a
deliberate choice over the Realtime WebSocket for now: same endpoint as today, no
session/WS lifecycle, and it still lets partial text appear before the final
transcript. It does **not** reduce "wait for utterance end + upload" latency — that
is the accepted trade-off. Realtime WS remains a future optional tier.

Add a `capabilities` probe at startup; if streaming ASR/TTS/LLM is unsupported,
fall back to buffered mode per service (see §6.1).

### 6.1 Fallback tiering (streaming vs buffered)

Every service has a **buffered floor** that maps 1:1 to the current repo, so the
client works even if the server never streams. Streaming is the canonical
interface; buffered adapters simply emit one-shot deltas. Selection happens
**once per service** at startup (config-declared), never mid-flight.

| Service | Tier 1 (streaming) | Tier 0 (buffered floor = current repo) | Difficulty |
|---|---|---|---|
| LLM | `stream:true` SSE deltas | `POST /v1/chat/completions` no stream → one message | Low |
| TTS | `stream:true` + `response_format:"pcm"` chunked | whole audio body (mp3/wav) → decode → emit one chunk | Low–Medium (needs decoder) |
| ASR | SSE `stream=true` file upload | `POST /v1/audio/transcriptions` → JSON `text` | Low |

Rules:
- `AsrClient` is `start/push/commit`; the buffered adapter buffers `push` frames
  in memory and POSTs on `commit`. The orchestrator never branches.
- Never auto-fallback mid-request (risks duplicate audio / re-running tools).
  Optional one-time latch: first streaming call fails to parse → buffered for the
  rest of the process.
- The buffered adapters **are** Phase 3, and double as the regression oracle
  against the Python client.


---

## 7. Migration phases

### Phase 0 — Prep
- [ ] Rotate and remove the leaked API key; switch to env/secret file.
- [ ] Record a golden audio corpus (wake word yes/no, utterances, silence, noise).
- [ ] Stand up a **mock OpenAI server** emitting canned SSE tokens and PCM chunks.
- [ ] Freeze current behavior with a short spec (latency + transcript snapshots).

### Phase 1 — Skeleton
- [ ] CMake project, C++20, vcpkg/FetchContent deps; aarch64 toolchain file.
- [ ] spdlog, config loader, `CancelToken`, SPSC ring buffer.
- [ ] PortAudio capture + playback; device select by name; WAV read/write (libsndfile).
- [ ] soxr resampling (48k ↔ 16k ↔ 24k).
- Acceptance: record to WAV, play a file, list/select devices on the Pi.

### Phase 2 — Wake word + VAD
- [ ] WakeWord (onnxruntime, openwakeword models); validate preprocessing parity.
- [ ] Silero VAD + Endpointer (min-speech, silence timeout, max length).
- [ ] Fixes defects #2 and #4.
- Acceptance: parity on golden audio; no false wake on the noise set.

### Phase 3 — API parity (non-streaming)
- [ ] libcurl wrapper, SSE parser, nlohmann types, capabilities probe.
- [ ] HTTP ASR / TTS / LLM clients (buffered paths).
- [ ] Full single-turn end-to-end.
- Acceptance: same transcript + audible reply as the Python client.

### Phase 4 — Agent + tools
- [ ] Tool registry with JSON schemas; tool-call loop.
- [ ] Tools: time, weather, music (yt-dlp via exec), search.
- [ ] `text_normalize` (number→words, markdown/emoji/URL stripping for TTS).
- Acceptance: tool calls match Python behavior; music plays end-to-end.

### Phase 5 — Streaming
- [ ] SSE LLM streaming + `sentence_splitter`.
- [ ] Streaming TTS PCM → Player, sentence-by-sentence pipelining.
- [ ] Realtime ASR adapter (open transcription session during Listening).
- [ ] Per-stage latency metrics.
- Acceptance: measurable drop in time-to-first-audio vs baseline.

### Phase 6 — Experience
- [ ] Barge-in: CancelToken aborts in-flight requests, flushes ring + player.
- [ ] Optional AEC (speexdsp / WebRTC APM) for true full duplex.
- [ ] Realtime speech-to-speech adapter.
- Acceptance: interrupting the assistant works cleanly.

### Phase 7 — Cutover
- [ ] systemd unit, config install, auto-restart/watchdog, Pi deploy.
- [ ] Remove Python sources (keep `beep.ogg`, models, config sample).
- Acceptance: Pi boots into the assistant unattended.

Sizing: phases 1–4 are the bulk (~2.5–3k LOC); 5–6 add ~1–1.5k. Cross-cutting
work (ring buffers, cancellation, resampling, test harness) dominates the risk.

---

## 8. Raspberry Pi specifics

- Select ALSA/PortAudio devices by name in config (fixes #5).
- Run wake/VAD at 16 kHz to keep CPU down; resample only at edges.
- Prefer the Realtime/streaming paths; they reduce both latency and memory.
- Build natively on the Pi first; add cross-compile once stable.
- onnxruntime has aarch64 Linux builds; verify thread count (2–4) for the Pi.
- Watch CPU under load; metrics per stage to catch regressions.

---

## 9. Testing strategy

- **Unit:** ring buffer, SSE parser, sentence splitter, text normalizer, tool schema.
- **Integration:** mock server for SSE/PCM/Realtime; adapters tested without network.
- **Audio:** golden wake/VAD corpus, parity against Python outputs.
- **Latency:** record per-stage timings in CI where possible.
- Keep audio device access behind an interface so tests use a synthetic source/sink.

---

## 10. Security

- Rotate the committed key; config from env / `chmod 600` secret file.
- Never log tokens or audio by default; redact auth headers.
- `exec` yt-dlp without a shell; validate the parsed video id.
- TLS verification on (libcurl defaults); consider pinning for the Pi.

---

## 11. Risks / open items

| Risk | Mitigation |
|---|---|
| openwakeword ONNX preprocessing differs from Python | same models + golden-audio parity test |
| Server does not support streaming ASR/TTS | capability probe + buffered fallback |
| `qwen3.5-4b` tool-calling may need server-side parser config | verify early in Phase 4 |
| Realtime endpoint availability on your server | keep it an adapter, not a hard dependency |
| AEC for barge-in is non-trivial | start with mic gating; add AEC in Phase 6 |
| Pi CPU/thermal under always-on load | 16 kHz detection, metrics, thread tuning |

---

## 12. Immediate next steps

1. Rotate the API key and move config out of source.
2. Confirm server capabilities: LLM SSE, TTS `stream`/pcm, ASR `stream`/Realtime.
3. Scaffold the Phase 1 CMake skeleton and the interface headers in §5.
4. Build the mock OpenAI server + golden audio corpus to lock in tests.
