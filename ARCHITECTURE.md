# hoodie architecture

hoodie is a thin, local voice client. The audio device, wake word, voice
activity detection, endpointing, state machine, and tool loop run in the C++
process. Automatic speech recognition (ASR), text-to-speech (TTS), and the
language model (LLM) are remote OpenAI-compatible services on the LAN. The
design goal is a predictable, debuggable voice loop on a small board without a
local GPU, with streaming where it reduces latency and a buffered floor where it
does not.

## 1. Goals and non-goals

### Goals

- Keep microphone and speaker handling, wake detection, VAD, and turn control
  local.
- Keep ASR, TTS, and LLM inference replaceable behind small client interfaces.
- Use streaming transports when they measurably improve latency, without
  requiring them for bring-up.
- Make every stage testable without a microphone through WAV replay and a mock
  OpenAI-compatible server.
- Fail a turn cleanly rather than leave the player, ASR stream, or HTTP request
  stuck.
- Keep secrets out of source and out of logs.

### Non-goals (current)

- Local ASR, TTS, or LLM inference on the board.
- Barge-in or acoustic echo cancellation.
- A Realtime WebSocket speech-to-speech adapter.
- Multi-user or multi-session handling.
- Hot reloading configuration while the voice loop is running.

## 2. System context

```text
                    +--------------------------------------+
 microphone ------> |                                      |
                    |              hoodie                  |
 speaker <--------- |  (C++20, PortAudio, ONNX Runtime)     |
                    |                                      |
                    +----------+---------------+-----------+
                               |               |
                     HTTP / SSE|               |HTTP / SSE
                               v               v
                    +------------------+   +------------------+
                    |    audio.cpp     |   |    llama.cpp     |
                    |  ASR + TTS       |   |  chat + tools    |
                    +------------------+   +------------------+
```

The client assumes two base URLs and API keys: one for ASR/TTS and one for the
LLM. They can be the same host or different hosts. The protocol is the
OpenAI-compatible subset described in section 6. Endpoint paths are derived
from the selected transport mode rather than configured separately.

## 3. Runtime modules

| Area | Files | Responsibility |
|---|---|---|
| Application | `src/app/assistant.*` | Voice state machine and turn orchestration. |
| Application | `src/app/responder.*` | LLM text -> splitter -> ordered TTS worker -> player/file. |
| Application | `src/app/speculator.*` | One background LLM request on a stable ASR partial. |
| Application | `src/app/config.*` | JSON config, validation, env overrides, redacted summary. |
| Application | `src/app/api_commands.*`, `wake_commands.*`, `playback.*` | One-shot CLI commands and playback helpers. |
| Audio | `src/audio/` | PortAudio capture/playback, SPSC ring, soxr resampling, WAV I/O, device discovery. |
| Detection | `src/wake/wakeword.*` | openWakeWord mel -> embedding -> keyword pipeline. |
| Detection | `src/wake/vad.*` | Silero VAD with recurrent state. |
| Detection | `src/wake/endpointer.h` | VAD probabilities -> speech start/end events. |
| ML | `src/ml/onnx.*` | ONNX Runtime RAII wrapper and tensor helpers. |
| Network | `src/net/http.*`, `sse.*` | libcurl request variants and incremental SSE parsing. |
| API clients | `src/api/` | ASR/TTS/LLM interfaces, buffered and streaming implementations, OpenAI types. |
| Agent | `src/agent/agent.*`, `tool_registry.*`, `tools.*` | Tool-calling loop, JSON schemas, built-in tools. |
| Agent | `src/agent/sentence_splitter.*`, `text_normalize.*` | Speakable chunking and TTS text cleanup. |
| Utility | `src/util/` | Cancellation token, logging, shell-free subprocess runner. |

The dependency direction is roughly `app -> api/agent/wake/audio -> net/ml/util`.
`net` and `audio` do not depend on the application state machine, which keeps the
transport and hardware layers independently testable.

## 4. Turn lifecycle and state machine

```text
          wake
 Idle -----------> Listening ----------> Thinking ----------> Speaking
  ^                    |                     |                   |
  |                    |                     | first audio       |
  |                    |                     +-------------------+
  |                    |                                         |
  +--------------------+---------------- done / error / timeout --+
```

The state enum is `Idle`, `Listening`, `Thinking`, and `Speaking`.

1. **Idle.** The capture callback writes interleaved PCM into a lock-free SPSC
   ring. The main loop drains it, downmixes to mono, resamples to 16 kHz, and
   feeds 1280-sample (80 ms) blocks to openWakeWord. A rising-edge score above
   the threshold enters `Listening`. The detector is not reset between turns;
   after a reply its old windows still contain the wake word, so a hysteresis
   release (`score < threshold * 0.5`) and the rising-edge guard prevent an
   immediate re-trigger.
2. **Listening.** The same 16 kHz stream feeds Silero VAD in 640-sample (40 ms)
   frames and the endpointer. The captured audio is also appended to the
   utterance buffer at the capture rate. With live ASR, every 16 kHz block is
   pushed to the ASR worker as it arrives. A finished endpointer event sets
   `utterance_ready_`.
3. **Thinking.** `respond()` commits the ASR stream, obtains the final
   transcript, optionally confirms or discards a speculative LLM answer, and
   runs the tool-calling agent. The responder may already be synthesizing text
   while the agent is still producing tokens.
4. **Speaking.** The first TTS audio chunk (played or written) transitions to
   `Speaking`. Playback continues until the responder finishes and the player
   drains.
5. **Reset.** The turn stops ASR and speculation, clears the capture ring and
   utterance, resets VAD/endpointer, and returns to `Idle`. The wake detector is
   intentionally not reset: re-seeding its windows would re-arm the startup
   gate after every turn. Instead, `process_block()` keeps feeding it during
   Listening/Thinking/Speaking while ignoring its scores. That flushes the old
   wake word out of the keyword window, so it cannot re-trigger as the stale
   context slides out after the reply.

### Timing gates

- **Minimum utterance.** The endpointer's `SpeechEnd` is ignored until
  `assistant.min_utterance_ms` (default 1500 ms) has elapsed. The check uses the
  larger of accumulated utterance audio and wall-clock time since wake, because
  WAV replay is faster than real time while live capture can deliver less audio
  than wall-clock time. This prevents the wake-word tail's early pause from
  truncating the command.
- **Endpointer.** Five consecutive speech frames (200 ms) confirm speech; fifteen
  consecutive silence frames (600 ms) confirm the end; 375 frames (15 s) force an
  end.
- **Listen timeout.** If the endpointer never produces an end, the loop processes
  whatever was captured after `assistant.listen_timeout_s` if it has at least
  0.3 s of audio, otherwise it returns to wake.
- **Wake gate.** `wakeword.require_real_context` withholds scores until the mel
  and embedding windows contain only real audio. Startup padding can otherwise
  score a false positive on the first ungated frame. In practice this is about
  2.4 s after start; tests include a 3 s lead-in before the wake word.

## 5. Audio path

The canonical in-process format is interleaved signed 16-bit PCM in
`audio::AudioFrame`.

```text
device capture (48 kHz or configured rate)
    -> mono downmix
    -> utterance buffer (capture rate, for duration, diagnostics, and WAV dumps)
    -> stateful soxr resample to 16 kHz
         -> openWakeWord (80 ms blocks)
         -> Silero VAD (40 ms frames)
         -> endpointer
         -> ASR (16 kHz mono s16le; buffered ASR wraps it in a WAV at commit)

TTS response (24 kHz mono by default, raw s16le)
    -> player ring
    -> stateful soxr resample to the device rate
    -> PortAudio output callback
```

Details:

- Capture requests mono. Some codecs expose only stereo; capture then opens at
  the device channel count and the application averages channels to mono.
- `Resampler` is stateful across calls and uses soxr in float mode with explicit
  int16 conversion. This replaced naive decimation and made resampling
  deterministic across runs.
- The capture-rate utterance is retained for duration, diagnostics, and
  `--save-utterances`; it is useful for checking microphone quality. ASR consumes
  the 16 kHz resampled stream. Buffered ASR retains that stream and wraps it in a
  WAV at commit.
- Streaming TTS writes raw PCM with no container header. `server.tts_pcm_rate`
  and `server.tts_pcm_channels` are the only description of that stream, so they
  must match the server. Buffered TTS currently assumes 24 kHz mono when the
  response body is raw PCM; a WAV/OGG/FLAC response is decoded by libsndfile.
- Capture and playback callbacks are real-time paths: they use the ring buffer
  and avoid allocation or blocking. The player fills silence on underrun and
  counts underruns.

## 6. Network and transport abstraction

The service boundary is three interfaces:

```cpp
class AsrClient {
  virtual AsrCapabilities capabilities() const = 0;
  virtual void start(const CancelToken&, std::function<void(const AsrDelta&)>) = 0;
  virtual void push(const AudioFrame&) = 0;
  virtual void commit() = 0;
  virtual void stop() = 0;
};

class TtsClient {
  virtual bool streaming() const = 0;
  virtual void synthesize(const std::string& text, const CancelToken&,
                          std::function<void(const AudioFrame&)> on_chunk,
                          std::function<void()> on_done) = 0;
};

class LlmClient {
  virtual bool streaming() const = 0;
  virtual void chat(const ChatRequest&, const CancelToken&,
                    const std::function<void(const LlmEvent&)>& on_event) = 0;
};
```

`make_clients()` chooses one implementation per service from `streaming.*_mode`
at startup. The orchestrator only sees the interfaces, so it never branches on
transport mode.

### Endpoint contracts

| Service | Mode | Request | Response |
|---|---|---|---|
| ASR | `buffered` | `POST /v1/audio/transcriptions`, multipart WAV | JSON `{"text": "..."}` |
| ASR | `live` | `POST /v1/audio/transcriptions/live?model=...&sample_rate=16000&channels=1&sample_format=s16le`, chunked s16le body | SSE `transcript.text.delta` and `transcript.text.done` |
| TTS | `buffered` | `POST /v1/audio/speech` with `response_format: "pcm"` | Whole PCM/container body |
| TTS | `stream` | Same path with `response_format: "pcm"` and `stream_format: "audio"` | Chunked raw PCM |
| LLM | either | `POST /v1/chat/completions` | JSON message, or SSE deltas when `stream: true` |

Base URLs must not include `/v1`. `join_url()` adds exactly one slash between
the base and the derived path.

### ASR fallback

`LiveAsrClient` starts a full-duplex libcurl request at wake. A worker thread
reads PCM from a queue and uploads it while an SSE parser consumes deltas. The
partial text is cumulative. `commit()` marks the input done, joins the worker,
and returns only after the final transcript has been delivered.

`FallbackAsrClient` wraps the live client and a buffered client on the same audio
host. It retains every frame that was pushed. If live start, push, or commit
fails before a final transcript, it starts the buffered client and replays the
retained utterance. If a final transcript was already delivered, a later failure
is propagated instead of replaying, so the agent and tools cannot run twice. The
fallback is safe because nothing downstream runs before the final ASR result.

### Why TTS and LLM do not auto-fallback

A mid-stream TTS failure can already have played audio; replaying the sentence
would duplicate it. A mid-stream LLM failure can already have produced text or
tool calls; replaying the request could duplicate tool side effects. Both paths
therefore fail the turn and let the next wake start cleanly. The operator selects
`buffered` when the server does not provide the streaming path.

### HTTP implementation

`HttpClient` is a small synchronous libcurl wrapper. Each call creates its own
`CURL` easy handle, so a single wrapper instance can be used by the ASR worker,
the LLM request, and the TTS worker concurrently. It supports:

- buffered JSON and multipart requests,
- streaming response callbacks (SSE and TTS PCM),
- full-duplex chunked request + streaming response (live ASR),
- cancellation through progress, read, and write callbacks.

The live path forces HTTP/1.1 and sends an empty `Expect` header. The server must
receive the request body incrementally; an HTTPS proxy that buffers the upload
will reject it before audio.cpp sees it. `post_duplex` keeps only a small leading
response body for error diagnostics and streams the rest to the SSE parser.

## 7. LLM and tool loop

`agent::Agent` is a minimal OpenAI-style function-calling loop:

1. Build `messages` with the system prompt and user transcript.
2. Send the registered tool schemas (if any).
3. Collect `Token` events into content and `ToolCall` events into a list.
4. If there are no tool calls, return the content.
5. Otherwise execute each tool locally, append a `role: "tool"` message with the
   result, and repeat, up to six steps.

With a streaming LLM, `Token` events go directly to the responder as they arrive.
With a buffered LLM, the complete content arrives as one token event. Tool calls
are assembled from SSE deltas by index before they are emitted, so a function
name or argument JSON split across chunks is handled correctly.

Built-in tools:

| Tool | Behavior |
|---|---|
| `get_time` | Local wall-clock time. |
| `get_weather` | NWS forecast for a fixed grid (`BOX/73,90`). Needs internet. |
| `play_music` | Uses `yt-dlp` search to resolve a song; the caller plays it after the turn. No shell is involved. |
| `search` | DuckDuckGo Instant Answer API; a short answer, not a general web search. Needs internet. |

Tool errors are returned to the model as text rather than aborting the process.
An unknown tool or malformed arguments become a tool error message. The agent
stops at the step limit and logs a warning.

`server.llm_cache_prompt` adds llama.cpp's `cache_prompt` extension to SSE
requests. Other OpenAI-compatible servers may ignore the field.

## 8. TTS pipeline

`Responder` turns the LLM stream into speech without waiting for the whole reply:

```text
LLM token events
    -> SentenceSplitter
    -> queue of speakable chunks
    -> one TTS worker (preserves order)
    -> normalize_for_tts
    -> TtsClient::synthesize
    -> Player (streaming) or collected WAV (--out)
```

The splitter cuts at sentence-ending punctuation, at a clause boundary once at
least six words have accumulated, and forcibly at twelve words so an
unpunctuated reply cannot stall audio. It avoids common abbreviations and
ellipsis runs.

There is one TTS worker per turn. Multiple concurrent TTS requests would reorder
sentences and compete for the server; a single ordered queue is simpler and
still overlaps synthesis with LLM generation. The player is created lazily on
the first chunk, so `--say`/`--out` paths do not need an audio device.

`normalize_for_tts` expands numbers, strips Markdown emphasis/links/code, and
removes characters that should not be spoken. It is applied per chunk in the
live responder and to the whole reply in the one-shot commands.

## 9. Speculative LLM

Speculation targets the gap between the last ASR partial and the final
transcript:

- Requires `asr_mode: "live"` and an ASR client with `partials`.
- The partial must have at least `speculate_min_words`, be unchanged for
  `speculate_stable_ms`, and follow at least `speculate_silence_ms` of VAD
  silence.
- One LLM request runs on a separate thread with the same system prompt and tool
  schemas. It buffers content and notes tool calls, but never executes a tool and
  never produces audio.
- At the endpoint, the final transcript and speculative transcript are compared
  after lowercasing and removing punctuation/whitespace. The reply is used only
  if they match, the request finished within 5 s, it had no tool calls, and it
  did not fail.
- Otherwise the speculative result is discarded and the normal agent runs.

This is a latency optimization, not a correctness dependency. It can double LLM
work on turns where the final transcript changes, which is why it is off by
default.

## 10. Concurrency and cancellation

| Thread / callback | Work |
|---|---|
| PortAudio capture callback | Writes interleaved samples to the SPSC ring. No allocation, no blocking. |
| PortAudio playback callback | Drains the player ring; fills silence on underrun. |
| Main loop | Drains capture, resamples, runs wake/VAD/endpointer, drives state transitions. |
| Live ASR worker | Uploads PCM through libcurl and parses SSE. |
| TTS worker | Dequeues speakable chunks, synthesizes, feeds the player. |
| Speculation worker | Runs one LLM request on a stable partial. |
| LLM request | Runs on the main thread (agent) or the speculation worker. |

Shared state is protected by small mutexes or atomics: the ASR text is guarded by
`asr_text_mutex_`, the TTS queue by the responder mutex, and the state by an
atomic. The capture and player rings are single-producer/single-consumer.

`CancelToken` is an atomic flag checked by HTTP callbacks and synthesis paths.
`Assistant::stop()` sets the stopping flag and cancels in-flight work. Signals
set a global interrupted flag. This keeps shutdown and failed turns from leaving
a request or player thread running.

The current voice loop is **half-duplex**. Capture keeps running while the
assistant thinks and speaks, but no new turn starts in `Thinking` or `Speaking`.
The wake detector still consumes those blocks so its streaming context stays
current, but its scores are ignored; `reset_turn()` then clears the queued audio
before returning to `Idle`. Without that context advance, the frozen keyword
window still contains the original wake word and can dip and rise again as it
slides out, firing right after the reply. The assistant's own output cannot
start a new turn through the ignored scores, but true barge-in still needs echo
cancellation: otherwise the microphone hears the assistant and interrupts it.
The `CancelToken` and player/HTTP stop paths are the foundation for that future
work.

## 11. Configuration

The config is JSON. The top-level blocks are:

| Block | Purpose |
|---|---|
| `server` | Base URLs, default/per-service API keys, model IDs, TTS PCM format, LLM prompt cache. |
| `streaming` | Only `asr_mode`, `tts_mode`, `llm_mode`. |
| `wakeword` | Keyword, threshold, ONNX model paths, warm-up, real-context gate. |
| `audio` | Input/output device and capture sample rate. |
| `assistant` | Minimum utterance, listen timeout, speculation tuning. |

Design rules:

- `server.base_url` is the default host for both services. `audio_base_url` and
  `llm_base_url` override it per service. `base_url` may be empty if both
  overrides are present.
- `api_key` is the default. `audio_api_key` and `llm_api_key` override it for
  ASR/TTS and LLM respectively.
- Endpoint paths are protocol details derived from the mode. There are no
  `*_path` fields.
- There is no capability probe block. The selected mode determines which path
  and request shape is used; a server that does not support it fails the turn.
- `Config::load()` validates required hosts, positive PCM rates, non-negative
  timing values, and the allowed mode strings. `Config::summary()` redacts keys.
- Environment variables override file values after parsing:
  `HOODIE_BASE_URL`, `HOODIE_AUDIO_BASE_URL`, `HOODIE_LLM_BASE_URL`,
  `HOODIE_API_KEY`, `HOODIE_AUDIO_API_KEY`, `HOODIE_LLM_API_KEY`.
- `Config::load_or_default()` lets commands run without a config file;
  malformed JSON still fails.

Mode is fixed for the process lifetime. Changing it requires a restart, which
keeps each turn's transport behavior predictable.

## 12. Design decisions and tradeoffs

| Decision | Why | Cost |
|---|---|---|
| Thin C++ client, remote inference | Keeps the board cool and responsive; reuses the LAN model servers and their GPUs. | Needs a working network and server for every turn. |
| Streaming-first interfaces with buffered adapters | The orchestrator has one code path; simple servers still work. | More client implementations and tests. |
| Select transport once at startup | Avoids surprising behavior and duplicate side effects mid-turn. | A server change requires a restart, and no transparent TTS/LLM fallback. |
| ASR live with buffered replay | Removes the upload wait and enables partials while keeping a safe fallback before any downstream work. | Retains the whole utterance in memory and doubles ASR work on a live failure. |
| Chunked HTTP + SSE for live ASR, not WebSocket | Reuses the same HTTP client and server endpoint; no session lifecycle or WS dependency. | Requires a server/proxy that does not buffer request bodies; no persistent full-duplex speech session. |
| SSE for LLM tokens | Standard OpenAI-compatible mechanism; works with llama.cpp and many servers. | Tool-call deltas must be reassembled carefully. |
| Raw PCM for streaming TTS | Zero decode latency and no container parsing on the hot path. | The client must be configured with the exact rate/channels; no header to validate. |
| Sentence-level TTS pipelining | Starts audio before the full LLM reply; preserves sentence order with one worker. | Time-to-first-audio is bounded by the first sentence, not the first acoustic frame. |
| Speculation is opt-in and match-checked | Can hide some ASR finalization latency without risking wrong tools/audio. | Extra LLM work; only useful with live ASR partials. |
| 16 kHz wake/VAD/ASR, capture at device rate | Keeps detection and ASR bandwidth/compute low while preserving a clean capture for buffered ASR and diagnostics. | Two resampling stages and careful stateful resampler handling. |
| Wake real-context gate and minimum utterance | Both address observed startup/tail false positives rather than papering over them with a higher threshold. | Adds a short startup delay and requires lead-in audio in replay fixtures. |
| JSON config with derived paths | One source of truth for hosts/keys/models; no duplicated endpoint settings. | Changing a server's protocol requires a code change, not just a config edit. |
| Shell test suites + mock server | Tests run on the target and exercise the real HTTP/audio state machine without a live model server. | Not integrated with CTest; needs Python/curl and free ports. |

## 13. Failure handling and observability

- ASR/TTS/LLM errors throw `ApiError` or `std::runtime_error`; the turn catches
  them, aborts the responder, resets the turn, and returns to wake.
- Live ASR errors include the HTTP status, response body, and a proxy-specific
  hint when the server reports a non-incremental body.
- TTS errors stop the responder; the player is stopped and the turn is reset.
- Tool errors are returned to the model as tool output; the agent can recover.
- The capture callback only drops samples when the ring is full; the count is
  reported in the utterance log as `ring_dropped`.
- Logs are structured enough to isolate a stage: client selection at startup,
  wake score, state transitions at debug level, utterance statistics, ASR
  timing, transcript, agent step count, reply, and speculation outcome.
- The config summary prints hosts and models with redacted keys.

## 14. Testing strategy

The core is tested without a microphone:

- `--selftest` checks WAV encode/decode, resampling length and determinism, the
  SPSC ring, OGG reading, and TTS normalization.
- `tests/wake_test.sh` checks wake detection, silence/tone/room-tone rejection,
  the real-context gate toggle, determinism, VAD, and endpointing.
- `tests/agent_test.sh` scripts a `get_time` tool call through the mock server and
  verifies the loop feeds the result back.
- `tests/integration.sh` covers buffered ASR/TTS/LLM, SSE LLM, streaming PCM TTS,
  live ASR, live-to-buffered fallback, and a dead server.
- `tests/listen_test.sh` replays generated WAVs through the full state machine:
  a normal turn, a pause after wake, streaming services, live ASR, matched and
  mismatched speculation, tiny input blocks, and silence rejection.

The mock server (`tests/mock_openai_server.py`) implements the relevant
OpenAI-compatible endpoints plus a scripted live ASR stream and a `--fail-live`
mode. The WAV fixtures are generated deterministically by the shell tests; the
wake clip is downloaded by `scripts/fetch_models.sh`.

Hardware checks remain manual: `--list-devices`, `--record`, `--play`, and a
live `--listen` run with `--save-utterances`.

## 15. Legacy and inherited fixes

The original Python client is kept under `legacy/` for reference and is not
built. The C++ port deliberately does not carry forward these defects:

| Legacy issue | C++ behavior |
|---|---|
| VAD stopped on speech onset | `Endpointer` confirms start and stops after sustained silence. |
| Failure path referenced undefined data | Errors propagate as exceptions and fail the turn. |
| Wake word re-verified by literal ASR text | Wake detection is independent; ASR handles the command only. |
| Naive decimation for 16 kHz | Stateful soxr resampling, tested for determinism. |
| Hardcoded device IDs | Device names/indices come from config or CLI flags. |
| Committed API key | Keys come from config/env; the summary redacts them. |
| `shell=True` for `yt-dlp` | `execvp` with argument vectors; no shell. |
| No cancellation | `CancelToken` and explicit stop paths for HTTP, ASR, TTS, and playback. |

## 16. Future work

- **Barge-in and AEC.** Add echo cancellation before consuming microphone audio
  during playback; then cancel ASR/LLM/TTS and flush the player on a new wake or
  speech onset. The existing cancellation and stop paths are the starting point.
- **Realtime WebSocket adapter.** A speech-to-speech or full-duplex transcription
  adapter behind the same interfaces, without changing the state machine.
- **Per-stage latency metrics.** The logs already expose stage timings; a small
  metrics recorder would make regressions visible without reading logs.
- **Packaging and deployment.** A systemd unit, config installation, and a
  watchdog are documented in the README; packaging is still manual.
- **Buffered PCM format cleanup.** Pass `tts_pcm_rate`/channels through the
  buffered TTS client instead of assuming 24 kHz mono.
