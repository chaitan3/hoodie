# hoodie

hoodie is a C++20 voice assistant for Linux and small single-board computers. It
keeps the latency- and privacy-sensitive work local - PortAudio capture and
playback, openWakeWord, Silero VAD, endpointing, the turn state machine, and the
tool-calling loop - and sends only the audio and text that need a model to
OpenAI-compatible services on the LAN: ASR and TTS to audio.cpp, and chat
completions to llama.cpp. Streaming is available per service (live ASR, SSE chat,
and raw-PCM TTS), and every streaming tier has a simpler buffered counterpart so
the same binary can run against plain request/response servers.

## What it does

- Listens for a wake word, records one utterance, and transcribes it.
- Runs an OpenAI-style tool loop (`get_time`, `get_weather`, `play_music`,
  `search`) over a remote chat-completions model.
- Splits the reply into speakable chunks and plays synthesized audio as it
  arrives.
- Can start the LLM speculatively on a stable ASR partial and discard the result
  if the final transcript differs.
- Provides hardware-free WAV replay and diagnostics for every stage.

Barge-in and acoustic echo cancellation are not implemented yet. While the
assistant is thinking or speaking, microphone audio is deliberately discarded
rather than fed back into the wake detector.

## Requirements

| Component | Notes |
|---|---|
| Linux | 64-bit. Raspberry Pi 4 (2 GB+) or Pi 5 with 64-bit Raspberry Pi OS is a good target. |
| C++20 compiler | GCC 11+ or Clang 14+. |
| CMake | 3.24 or newer, plus Ninja. |
| PortAudio | Microphone and speaker I/O. |
| libsndfile | WAV/OGG/FLAC decoding and WAV output. |
| soxr | Stateful sample-rate conversion. |
| libcurl | HTTP, SSE, chunked upload, live ASR. |
| ONNX Runtime | openWakeWord and Silero VAD. |
| Python 3 + curl | Model/test downloads, ONNX Runtime install, and mock-server suites. |
| `yt-dlp` | Optional, only for the `play_music` tool. |

The first CMake configure downloads its small third-party dependencies
(`spdlog` and `nlohmann/json`) with FetchContent, so it needs network access
once. Runtime ASR, TTS, and LLM calls stay on your network.

## Build

### Debian / Ubuntu / Raspberry Pi OS

```bash
sudo apt update
sudo apt install -y \
  build-essential cmake ninja-build pkg-config git curl \
  portaudio19-dev libsndfile1-dev libsoxr-dev libcurl4-openssl-dev \
  python3 alsa-utils
```

ONNX Runtime is not packaged on Raspberry Pi OS. Install Microsoft's prebuilt
aarch64 library:

```bash
sudo ./scripts/install_onnxruntime.sh
```

If you cannot use `sudo`, install to a prefix you own and tell CMake where it is:

```bash
./scripts/install_onnxruntime.sh 1.20.1 "$HOME/ort"

cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DONNXRUNTIME_ROOT="$HOME/ort"
export LD_LIBRARY_PATH="$HOME/ort/lib:$LD_LIBRARY_PATH"
```

On a 2 GB Pi 4, build with `-j2` rather than `-j$(nproc)`.

### Arch Linux

```bash
sudo pacman -S --needed \
  base-devel cmake ninja pkg-config git curl \
  portaudio libsndfile soxr python onnxruntime-cpu alsa-utils
```

### Compile

```bash
./scripts/fetch_models.sh

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

`fetch_models.sh` puts the openWakeWord and Silero VAD ONNX files in `models/`,
and downloads the official openWakeWord test clip used by the test suites. The
models and test audio are gitignored.

The binary is `build/hoodie`.

## Configure

```bash
cp config/hoodie.example.json config/hoodie.json
$EDITOR config/hoodie.json
```

`config/hoodie.json` is gitignored. A fully streaming service configuration
looks like this:

```json
{
  "server": {
    "base_url": "",
    "audio_base_url": "http://<audio-host>:8080",
    "llm_base_url": "http://<llm-host>:8080",
    "asr_model": "<audio.cpp-asr-id>",
    "tts_model": "<audio.cpp-tts-id>",
    "llm_model": "<llama.cpp-model-id>",
    "tts_pcm_rate": 24000,
    "tts_pcm_channels": 1
  },
  "streaming": {
    "asr_mode": "live",
    "tts_mode": "stream",
    "llm_mode": "stream"
  }
}
```

For a conservative first run, set all three modes to `buffered` and turn them on
one at a time. Keep the existing `wakeword`, `audio`, and `assistant` blocks from
the example file. Points to get right:

- Base URLs do **not** include `/v1`. hoodie derives the endpoint paths:
  `/v1/audio/transcriptions`, `/v1/audio/transcriptions/live`,
  `/v1/audio/speech`, and `/v1/chat/completions`.
- `server.base_url` can be empty when both `audio_base_url` and `llm_base_url`
  are set.
- `audio_base_url` is shared by ASR and TTS. `llm_base_url` is used by the LLM.
- Model IDs must match the IDs configured on the servers. hoodie does not send a
  separate ASR/TTS language or voice field.
- The TTS request carries the model and text plus the PCM format fields, but no
  voice. Configure a default voice on audio.cpp (for example a Kokoro preset)
  rather than expecting hoodie to choose one.
- For streaming TTS, `tts_pcm_rate` and `tts_pcm_channels` must match the raw
  PCM the server emits. Raw PCM has no header, so a mismatch changes pitch and
  speed. Buffered TTS assumes 24 kHz mono for raw PCM; a decodable container is
  safer if the server uses another rate in buffered mode.
- `server.llm_cache_prompt` enables llama.cpp's prompt cache extension for SSE
  chat. Leave it on for llama.cpp; other servers may ignore it.

Keep secrets out of the JSON file:

```bash
export HOODIE_API_KEY='...'          # default for both services
export HOODIE_AUDIO_API_KEY='...'    # optional, ASR + TTS only
export HOODIE_LLM_API_KEY='...'      # optional, LLM only
```

`HOODIE_AUDIO_BASE_URL`, `HOODIE_LLM_BASE_URL`, and `HOODIE_BASE_URL` override
the corresponding config values. `HOODIE_CONFIG` selects a different config
file. The startup summary prints hosts, models, and redacted keys.

### Audio devices

```bash
./build/hoodie --list-devices
```

Set `audio.input_device` and `audio.output_device` to a device name (substring
match) or an index. Leave them empty to use the system default.
`audio.capture_sample_rate` defaults to 48000.
If a codec only exposes stereo capture, hoodie opens it at the device channel
count and downmixes to mono.

The wake/VAD model paths are relative to the process working directory. If you
run hoodie from outside the repository, either use absolute paths in
`config/hoodie.json` or set the systemd `WorkingDirectory`.

## Run

Start with the hardware-free checks:

```bash
./build/hoodie --selftest
./build/hoodie --list-devices
```

Record and replay a microphone sample:

```bash
./build/hoodie -c config/hoodie.json --record /tmp/mic.wav --seconds 3
./build/hoodie -c config/hoodie.json --play /tmp/mic.wav
```

Run the assistant:

```bash
./build/hoodie -c config/hoodie.json --listen
```

Say the wake word, pause briefly, then speak the command. `Ctrl-C` stops the
loop. To handle exactly one turn and exit:

```bash
./build/hoodie -c config/hoodie.json --listen --once
```

Replay a WAV through the full state machine without a microphone:

```bash
./build/hoodie -c config/hoodie.json \
  --listen-wav /tmp/wake-and-command.wav \
  --out /tmp/reply.wav --once
```

The replay file must contain enough real room tone before the wake word for the
real-context gate (about 2.4 seconds), followed by the wake word and command.
`--listen-wav` runs faster than real time but uses the same wake, VAD,
endpointing, ASR, agent, and TTS code as the live loop.

`--out` writes reply audio instead of playing it. On a multi-turn `--listen`
run the file is overwritten each turn, so use `--once` when inspecting it.

### One-shot service checks

```bash
./build/hoodie -c config/hoodie.json --transcribe /tmp/mic.wav
./build/hoodie -c config/hoodie.json --ask "Say hello in five words"
./build/hoodie -c config/hoodie.json --say "Hello from hoodie" --out /tmp/tts.wav
./build/hoodie -c config/hoodie.json --agent "What time is it?" --out /tmp/agent.wav
./build/hoodie -c config/hoodie.json --roundtrip /tmp/mic.wav --out /tmp/reply.wav
```

`--agent` and `--say` play audio by default; add `--out` for headless use.
`--roundtrip` prints the transcript and reply and then speaks the reply.
`--tool get_time`, `--tool get_weather`, `--tool 'play_music' --args '{"phrase":"..."}'`,
and `--norm "text"` are useful for testing individual pieces.

## Common flags

### Runtime and audio

| Flag | Meaning |
|---|---|
| `-c, --config <path>` | Config file. Default: `config/hoodie.json`, or `$HOODIE_CONFIG`. |
| `--listen` | Run the live microphone loop. |
| `--once` | Stop after one completed turn. |
| `--listen-wav <file>` | Replay a WAV through the voice state machine. |
| `--out <file>` | Write synthesized reply audio to a WAV instead of playing it. |
| `--save-utterances <dir>` | Save each captured utterance as a WAV for inspection. |
| `--min-utterance-ms <n>` | Ignore end-of-speech before `n` ms (default from config: 1500). |
| `--wav-block-frames <n>` | Block size used by `--listen-wav`; useful for edge-case tests. |
| `--input-device <d>` | Override `audio.input_device` for this run. |
| `--output-device <d>` | Override `audio.output_device` for this run. |
| `--list-devices` | Print PortAudio devices and exit. |
| `--record <file>` | Record the microphone to a WAV, then exit. |
| `--seconds <n>` | Recording length; default 5. |
| `--play <file>` | Play an audio file, then exit. |
| `--selftest` | Run hardware-free WAV, resampling, ring-buffer, and TTS-normalization checks. |

### Server and agent

| Flag | Meaning |
|---|---|
| `--transcribe <file>` | Send one audio file to ASR and print the text. |
| `--ask <text>` | One plain LLM request, no tools. Prints the reply. |
| `--agent <text>` | Run the tool-calling agent and speak the reply (or write `--out`). |
| `--say <text>` | Synthesize text and play it (or write `--out`). |
| `--roundtrip <file>` | ASR -> agent -> TTS on one audio file. |
| `--system <text>` | Override the default system prompt for this run. |
| `--norm <text>` | Print TTS-normalized text and exit. |
| `--tool <name>` | Invoke a built-in tool directly. |
| `--args <json>` | Arguments for `--tool`; default `{}`. |

### Wake word and VAD diagnostics

| Flag | Meaning |
|---|---|
| `--wakeword <file>` | Run the wake-word detector over an audio file and print the maximum score. |
| `--vad <file>` | Run Silero VAD over an audio file. |
| `--endpoint <file>` | Run VAD + endpointer and print speech segments. |
| `--threshold <x>` | Override the wake/VAD detection threshold (`0..1`). |
| `--onnx-probe` | Inspect the configured ONNX models' inputs and outputs. |
| `-h, --help` | Print the full usage text. |
| `-V, --version` | Print the version. |

## Streaming and buffered modes

The `streaming` block selects one transport mode per service at startup. Modes
are independent; any combination is valid. The defaults are all `buffered` for
conservative bring-up.

```json
"streaming": {
  "asr_mode": "buffered",
  "tts_mode": "buffered",
  "llm_mode": "buffered"
}
```

| Service | `buffered` | Streaming mode |
|---|---|---|
| ASR | Buffers the whole utterance, uploads one WAV after end-of-speech, returns one final transcript. | `asr_mode: "live"` starts a chunked PCM upload at wake, consumes SSE deltas while the user speaks, and commits at the endpoint. |
| TTS | Splits the reply into speakable chunks and requests each chunk as a complete clip. | `tts_mode: "stream"` requests raw PCM with `stream_format=audio` and plays each response chunk as it arrives. |
| LLM | One non-streaming chat request; the complete reply is split and spoken. | `llm_mode: "stream"` consumes SSE token deltas and feeds them to the splitter immediately. |

### What "streaming" changes

- **Live ASR** removes the post-endpoint upload wait and provides cumulative
  partials. The final transcript is still committed at end-of-speech. Partials
  are what make speculative LLM starts possible.
- **Streaming TTS** lowers time-to-first-audio. The first audio chunk triggers
  the `Speaking` state; playback continues while later chunks arrive.
- **Streaming LLM** lets the sentence splitter hand the first sentence to TTS
  before the model has finished the whole reply.

### Fallback behavior

- **ASR has an automatic fallback.** Live ASR retains every pushed frame. If the
  live start, upload, or commit fails before a final transcript has been
  delivered, hoodie replays the same utterance through the buffered endpoint on
  the same audio host. Once a final transcript exists, it never replays, so the
  agent and tools are not run twice.
- **TTS and LLM do not auto-fallback.** A mid-stream failure fails the turn.
  Silently switching transports could duplicate audio or re-run work; set the
  mode to `buffered` if the server does not support the streaming path.
- The mode is chosen once at startup and is not switched after a failure.

### Live ASR and proxies

Live ASR uses an HTTP/1.1 chunked request body and an SSE response on the same
connection. A reverse proxy that buffers request bodies breaks this. If you see:

```text
live transcription requires an incrementally delivered body (Transfer-Encoding: chunked)
```

point `server.audio_base_url` directly at audio.cpp, or configure nginx with:

```nginx
proxy_http_version 1.1;
proxy_request_buffering off;
proxy_buffering off;
```

hoodie already forces HTTP/1.1 for live requests and removes the `Expect:
100-continue` header.

### Speculative LLM

Speculation is off by default. It requires `asr_mode: "live"` because it needs
partial transcripts.

```json
"assistant": {
  "speculate": true,
  "speculate_stable_ms": 400,
  "speculate_silence_ms": 250,
  "speculate_min_words": 2
}
```

When a partial has been stable for `speculate_stable_ms`, VAD has been silent for
`speculate_silence_ms`, and the partial has at least `speculate_min_words` words,
hoodie sends one LLM request in the background. The speculative request never
executes tools and never produces audio. After the final transcript arrives, the
normalized final and speculative transcripts must match, and the request must
have completed without tool calls or errors. Otherwise the result is discarded
and the normal agent runs. Speculation can reduce the delay after the user stops
speaking, but it doubles LLM work on some turns.

### Recommended rollout

1. Start with all three modes `buffered` and verify audio, wake, and one-shot
   service calls.
2. Turn on `llm_mode: "stream"` and watch time-to-first-audio.
3. Turn on `tts_mode: "stream"` and verify `tts_pcm_rate`/channels against the
   server.
4. Turn on `asr_mode: "live"` and verify the direct-URL/proxy setup.
5. Try `assistant.speculate` last, after the final transcript is reliable.

## Debugging

### Logging

```bash
HOODIE_LOG_LEVEL=debug ./build/hoodie -c config/hoodie.json --listen
```

Levels are `trace`, `debug`, `info`, `warn`, `error`, `critical`, and `off`.
Useful log lines include:

- `wake word '...' detected` with the score.
- `state: idle -> listening` and later transitions at debug level.
- `utterance: ...` with duration, sample rate, peak, RMS, and `ring_dropped`.
- `asr: ... (N s)` with the active client description and commit time.
- `transcript: ...`, `agent: N step(s)`, `reply: ...`.
- `speculation: started`, `used`, or `discarded`.
- Selected clients at startup, for example `asr: live (buffered fallback)
  selected`.

### Isolate a stage

Use the one-shot commands before debugging the live loop:

```bash
# Hardware
./build/hoodie --list-devices
./build/hoodie -c config/hoodie.json --record /tmp/mic.wav --seconds 3
./build/hoodie -c config/hoodie.json --play /tmp/mic.wav

# Detection
./build/hoodie -c config/hoodie.json --wakeword /tmp/mic.wav
./build/hoodie -c config/hoodie.json --vad /tmp/mic.wav
./build/hoodie -c config/hoodie.json --endpoint /tmp/mic.wav

# Services
./build/hoodie -c config/hoodie.json --transcribe /tmp/mic.wav
./build/hoodie -c config/hoodie.json --ask "ping"
./build/hoodie -c config/hoodie.json --say "hello" --out /tmp/tts.wav
./build/hoodie -c config/hoodie.json --agent "what time is it?" --out /tmp/agent.wav
./build/hoodie -c config/hoodie.json --roundtrip /tmp/mic.wav --out /tmp/reply.wav
```

### Inspect what the assistant heard

```bash
./build/hoodie -c config/hoodie.json --listen \
  --save-utterances /tmp/utts
```

Each turn writes `utterance-0001.wav`, `utterance-0002.wav`, and so on. The WAV
is the captured utterance at the capture rate. Listen to it, feed it back
through `--transcribe`, or run `--wakeword`/`--vad`/`--endpoint` on it to find
out whether the problem is capture, detection, or ASR.

The utterance log's `peak` and `rms` distinguish silence/clipping from a
recognition problem. A nonzero `ring_dropped` means the host could not keep up
with capture, or the process was blocked longer than the capture ring could
buffer.

### Test with WAV replay

`--listen-wav` drives the real state machine without a microphone and without
real-time pacing. Build a fixture with a silent lead-in, the wake word, and the
command. `tests/listen_test.sh` is a good template for generating one.
`--wav-block-frames 1` forces the smallest input blocks and is a good regression
check.

### Mock-server test suites

```bash
./tests/wake_test.sh
./tests/agent_test.sh
./tests/integration.sh
./tests/listen_test.sh
```

These are shell suites, not CTest targets. All expect a built `build/hoodie`;
the wake and listen suites also need the models and test audio fetched by
`scripts/fetch_models.sh`. The agent/integration/listen suites start
`tests/mock_openai_server.py` on localhost and need `python3` and `curl`; the
listen test also generates its own WAV fixtures. Ports default to 8098 and 8099
and can be moved with `HOODIE_TEST_PORT`. Run them in the order above when
changing audio, transport, or state-machine code.

### Live ASR checklist

- `server.audio_base_url` points at audio.cpp itself, not an HTTPS proxy that
  buffers uploads.
- The audio.cpp model is configured with `"mode": "streaming"` and a live-capable
  family.
- `server.asr_model` matches the audio.cpp model ID exactly.
- The server supports `POST /v1/audio/transcriptions/live` with a chunked body.
- `HOODIE_LOG_LEVEL=debug` shows a live client description and partial deltas.
- On failure, the log reports the real HTTP status and response body. If the
  error mentions `Transfer-Encoding: chunked`, fix the proxy first.

## Troubleshooting

| Symptom | Likely cause / fix |
|---|---|
| `onnxruntime not found` at configure | Install ONNX Runtime, run `sudo ldconfig` if needed, or pass `-DONNXRUNTIME_ROOT=<prefix>`. |
| `libonnxruntime.so... cannot open shared object` | Run `sudo ldconfig`, or set `LD_LIBRARY_PATH` for a custom prefix. |
| `no audio devices available` | Add the user to the `audio` group and re-login; check `arecord -l`/`aplay -l`. |
| Device rejects the configured rate/channels | Change `audio.capture_sample_rate` or use `--input-device`/`--output-device`. |
| Device busy / `Pa_OpenStream failed` | Another process (PipeWire, PulseAudio, browser) holds the device. Stop it or choose another device. |
| Wake word never fires | Check `--wakeword` on a recording; lower `wakeword.threshold` slightly; verify the correct input device and that the mic is not clipping. |
| Wake word fires on room noise | Keep `wakeword.require_real_context: true`; check `--wakeword` on a noise recording; raise the threshold. |
| Wake fires but ASR gets a fragment | Increase `assistant.min_utterance_ms` (or `--min-utterance-ms`); inspect `--save-utterances`. |
| Transcript is empty | Listen to the saved utterance, check peak/RMS, and try `--transcribe` on it. If the file is fine, check the ASR model and language settings on the server. |
| No TTS audio | Try `--say "hello" --out /tmp/tts.wav`, then `--play`. Check the TTS model ID, the server's default voice, and `tts_pcm_rate`/channels. |
| Reply audio is pitched or fast/slow | Streaming PCM rate/channel mismatch. Match `tts_pcm_rate` and `tts_pcm_channels` to the server. Buffered raw PCM is assumed to be 24 kHz mono. |
| Agent never calls a tool | Confirm the LLM server/model supports OpenAI tool calls and that its chat template exposes them. |
| `play_music` does nothing | Install `yt-dlp` and check network access; the tool runs it without a shell. |
| Weather/search fails | These tools use internet services (NWS and DuckDuckGo), not the LAN model hosts. |
| First turn is slow | The server may be lazily loading models; wake gating and ONNX warm-up also affect the first turn. |
| Live ASR reports chunked-body error | Direct-connect to audio.cpp or disable proxy request buffering. |

## Run on a Raspberry Pi or other SBC

Use a 64-bit OS. The prebuilt ONNX Runtime is aarch64; a 32-bit OS will not work.
A Pi 4 with 2 GB or more, or a Pi 5, is the practical target. A Pi Zero is not
recommended.

```bash
sudo apt install -y \
  build-essential cmake ninja-build pkg-config git curl \
  portaudio19-dev libsndfile1-dev libsoxr-dev libcurl4-openssl-dev \
  python3 alsa-utils
sudo ./scripts/install_onnxruntime.sh
./scripts/fetch_models.sh

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
```

A simple systemd unit:

```ini
# /etc/systemd/system/hoodie.service
[Unit]
Description=hoodie voice assistant
After=network-online.target sound.target
Wants=network-online.target

[Service]
User=pi
SupplementaryGroups=audio
WorkingDirectory=/home/pi/hoodie
EnvironmentFile=/etc/hoodie.env
ExecStart=/home/pi/hoodie/build/hoodie -c /home/pi/hoodie/config/hoodie.json --listen
Restart=on-failure
RestartSec=3

[Install]
WantedBy=multi-user.target
```

`/etc/hoodie.env` can contain `HOODIE_API_KEY=...` and any per-service URL/key
overrides. Then:

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now hoodie
journalctl -u hoodie -f
```

## Status and next steps

Implemented: local audio I/O and resampling, wake word with a real-context gate,
Silero VAD and endpointing, the four-state voice loop, buffered ASR/TTS/LLM,
live ASR with buffered replay fallback, SSE LLM streaming, raw-PCM streaming
TTS, sentence-level pipelining, the tool loop, and optional speculative LLM
starts.

Not yet implemented: barge-in, acoustic echo cancellation, and a Realtime
WebSocket speech-to-speech adapter. See [ARCHITECTURE.md](ARCHITECTURE.md) for
the design and the reasoning behind the transport and fallback choices.
