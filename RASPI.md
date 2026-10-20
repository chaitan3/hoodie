# Building and testing hoodie on a Raspberry Pi

Target: **Raspberry Pi 4 (2 GB+) or Pi 5, 64-bit Raspberry Pi OS** (Bookworm or
newer). A 32-bit OS will not work: the prebuilt ONNX Runtime is aarch64 only.
A Pi Zero / Zero 2 is not recommended for this app.

Everything here is a native build on the Pi. Cross-compiling is possible but not
needed for the first bring-up.

---

## 1. System packages

```bash
sudo apt update
sudo apt install -y \
  build-essential cmake ninja-build pkg-config git curl \
  portaudio19-dev libsndfile1-dev libsoxr-dev libcurl4-openssl-dev \
  alsa-utils python3
```

- `portaudio19-dev` / `libsndfile1-dev` / `libsoxr-dev` — audio I/O, codecs, resampling
- `libcurl4-openssl-dev` — HTTP
- `alsa-utils` — `aplay -l` / `arecord -l` for finding your mic/speaker
- `python3` — only for the test mock server

`cmake` must be **3.24+**. Raspberry Pi OS Bookworm ships 3.25; if yours is older,
install a newer CMake from Kitware or use `pipx install cmake`.

---

## 2. ONNX Runtime (wake word + VAD)

There is no `onnxruntime` package in Raspberry Pi OS, so install Microsoft's
prebuilt aarch64 build:

```bash
cd hoodie
sudo scripts/install_onnxruntime.sh          # defaults to 1.20.1, installs to /usr/local
```

This downloads `onnxruntime-linux-aarch64-<ver>.tgz`, installs headers to
`/usr/local/include/onnxruntime/` and libraries to `/usr/local/lib/`, then runs
`ldconfig`. Verify:

```bash
ls /usr/local/include/onnxruntime/onnxruntime_cxx_api.h
ls /usr/local/lib/libonnxruntime.so
```

**No sudo / custom location?** Install to a prefix you own and point CMake at it:

```bash
scripts/install_onnxruntime.sh 1.20.1 ~/ort
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DONNXRUNTIME_ROOT=$HOME/ort
# and at runtime:
export LD_LIBRARY_PATH=$HOME/ort/lib:$LD_LIBRARY_PATH
```

---

## 3. Build

```bash
cd hoodie
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
```

- Use `-j2` on a 2 GB Pi 4 to avoid running out of memory; `-j4` is fine on 4–8 GB.
- The first configure clones `spdlog` and `nlohmann/json` via CMake FetchContent,
  so it needs **network access at configure time**. Subsequent builds are offline.
- Confirm ONNX Runtime was found — the configure log prints:
  `onnxruntime: /usr/local/lib/libonnxruntime.so (include /usr/local/include)`.

The build produces `build/hoodie`.

---

## 4. Models

Wake-word / VAD model files are not committed. Fetch them:

```bash
./scripts/fetch_models.sh
ls models/          # melspectrogram.onnx embedding_model.onnx alexa_v0.1.onnx silero_vad.onnx
```

This also downloads `tests/data/alexa_test.wav` (the official openWakeWord test clip).

---

## 5. Configuration

```bash
cp config/hoodie.example.json config/hoodie.json
$EDITOR config/hoodie.json
```

Keep the API key out of the file — use the environment instead:

```bash
export HOODIE_API_KEY=...
# or put it in /etc/hoodie.env and load it from your shell / systemd unit
```

Find your device names, then set them in `config/hoodie.json`:

```bash
./build/hoodie --list-devices
```

```jsonc
{
  "audio": {
    "input_device": "USB PnP Sound Device",   // substring match; "" = system default
    "output_device": "Headphones",            // or "bcm2835 Headphones"
    "capture_sample_rate": 48000              // match what your mic supports
  }
}
```

If a device rejects the rate/channels, the error names the device and the
requested format — adjust `capture_sample_rate` (48000, 44100, 16000 are common).

---

## 6. Test — no microphone needed

Run these in order. Each one exercises a layer.

### 6.1 DSP / codec self-test (no hardware, no models)
```bash
./build/hoodie --selftest
# expects: "selftest: PASSED"
```
Verifies WAV round-trip, resampling (and its determinism), the SPSC ring buffer,
OGG read, and TTS text normalization.

### 6.2 Wake word / VAD / endpointer
```bash
./build/hoodie -c config/hoodie.example.json --wakeword tests/data/alexa_test.wav
# -> alexa: max=1 at=0.48s detected=yes
./build/hoodie -c config/hoodie.example.json --vad tests/data/alexa_test.wav
./build/hoodie -c config/hoodie.example.json --endpoint tests/data/alexa_test.wav

./tests/wake_test.sh          # full suite; expects "wake: PASSED"
```

### 6.3 Audio hardware (mic + speaker)
```bash
./build/hoodie --record /tmp/pi.wav --seconds 3
./build/hoodie --play /tmp/pi.wav
```
Use `--input-device` / `--output-device` to try devices without editing config.
Recording 3 s then playing it back confirms PortAudio → WAV → playback.

---

## 7. Test — server round trips

Set the key and point at your server (defaults to `***REMOVED***`):

```bash
export HOODIE_API_KEY=...
export HOODIE_LOG_LEVEL=info
CFG=config/hoodie.example.json

./build/hoodie -c $CFG --ask "say hello"            # plain chat, no tools
./build/hoodie -c $CFG --tool get_weather           # direct tool check
./build/hoodie -c $CFG --say "hello there" --out /tmp/tts.wav   # TTS -> file
./build/hoodie -c $CFG --transcribe /tmp/pi.wav     # ASR on your recording
./build/hoodie -c $CFG --agent "what time is it?" --out /tmp/agent.wav
./build/hoodie -c $CFG --roundtrip /tmp/pi.wav --out /tmp/reply.wav   # ASR -> agent -> TTS
```

Use `--out` when you want the audio written to a file rather than played.

### 7.1 Mock-server suites (offline, deterministic)
```bash
./tests/integration.sh    # buffered ASR/TTS/LLM + dead-server path
./tests/agent_test.sh     # tool-calling loop
```
Both start `tests/mock_openai_server.py` on localhost and need `python3` + `curl`.

---

## 8. Manual end-to-end (the closest thing to the full assistant today)

The always-on listen loop is **not implemented yet** (that arrives with the
streaming pipeline). Until then, chain the stages by hand:

```bash
# 1. Say the wake word, then record your command
./build/hoodie --record /tmp/cmd.wav --seconds 4

# 2. Transcribe + run the agent + speak the reply
./build/hoodie -c config/hoodie.json --roundtrip /tmp/cmd.wav
```

To confirm the wake word fires on real speech, record yourself saying "Alexa":

```bash
./build/hoodie --record /tmp/alexa.wav --seconds 2
./build/hoodie -c config/hoodie.json --wakeword /tmp/alexa.wav
```

---

## 9. Running as a service (later)

Once a `--listen` command exists, a systemd unit will look like:

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
EnvironmentFile=/etc/hoodie.env        # HOODIE_API_KEY=...
ExecStart=/home/pi/hoodie/build/hoodie -c /home/pi/hoodie/config/hoodie.json --listen
Restart=on-failure
RestartSec=3

[Install]
WantedBy=multi-user.target
```

---

## 10. Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `onnxruntime not found` at configure | Run `scripts/install_onnxruntime.sh`, or pass `-DONNXRUNTIME_ROOT=<prefix>`; ensure `ldconfig` ran |
| `libonnxruntime.so.1: cannot open shared object` | `sudo ldconfig`, or set `LD_LIBRARY_PATH` for a custom prefix |
| `no audio devices available` | Add your user to `audio`: `sudo usermod -aG audio $USER`, re-login; check `arecord -l` |
| `device '...' does not support ... Hz` | Change `audio.capture_sample_rate` in the config |
| `Pa_OpenStream failed` / busy | Another process holds the device (PulseAudio/PipeWire); stop it or use a different device |
| ALSA `cannot find card` warnings | Harmless probe noise from PortAudio on a headless Pi |
| Configure fails cloning spdlog/json | No network at configure time; connect once, or install `libspdlog-dev`/`nlohmann-json3-dev` and adjust `cmake/deps.cmake` |
| Wake word never fires on your voice | Try `--threshold 0.4`; ensure the mic isn't clipping; verify `--list-devices` picks the right input |
| Very slow first wake detection | ONNX Runtime warm-up; the first inference is slower than steady state |

---

## 11. What is and isn't ready

**Ready to test on the Pi:** audio capture/playback, WAV I/O, resampling,
wake word, Silero VAD + endpointer, the `--listen` loop, buffered ASR/TTS/LLM,
streaming SSE LLM + raw-PCM TTS, live ASR with buffered fallback, the
tool-calling agent, TTS text normalization, and optional speculative LLM starts
(`assistant.speculate`).

**Not yet implemented:** barge-in/AEC (Phase 6) and the optional Realtime /
speech-to-speech adapter. The transport interfaces and buffered fallbacks are in
place.
