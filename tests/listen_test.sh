#!/usr/bin/env bash
# Voice-loop test: replays a WAV through the full state machine
# (wake -> ASR -> agent -> TTS) against the mock server. No microphone needed.
#
#   tests/listen_test.sh [build-dir]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${1:-$ROOT/build}"
BIN="$BUILD_DIR/hoodie"
PORT="${HOODIE_TEST_PORT:-8098}"
CONFIG="$ROOT/config/hoodie.example.json"
DATA="$ROOT/tests/data"

fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "ok: $*"; }

[[ -x "$BIN" ]] || fail "binary not found: $BIN (build first)"
[[ -s "$ROOT/models/alexa_v0.1.onnx" ]] || fail "models missing; run scripts/fetch_models.sh"
[[ -s "$DATA/alexa_test.wav" ]] || fail "test audio missing; run scripts/fetch_models.sh"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"; [[ -n "${MOCK_PID:-}" ]] && kill "$MOCK_PID" 2>/dev/null || true' EXIT

# Fixture: 3 s lead-in (so the real-context gate is open), the wake word, a
# short gap, a "command" (the clip again), then trailing silence. The gap stays
# under the endpointer's 600 ms silence threshold so the turn is not cut short.
python3 - "$DATA/alexa_test.wav" "$WORK" <<'PY'
import sys, wave

src, out_dir = sys.argv[1], sys.argv[2]
with wave.open(src, "rb") as a:
    ch, sw, fr = a.getnchannels(), a.getsampwidth(), a.getframerate()
    frames = a.readframes(a.getnframes())

def silence(seconds):
    return b"\x00\x00" * int(fr * seconds) * ch

with wave.open(f"{out_dir}/listen.wav", "wb") as w:
    w.setnchannels(ch); w.setsampwidth(sw); w.setframerate(fr)
    w.writeframes(silence(3.0) + frames + silence(0.3) + frames + frames + silence(1.5))

with wave.open(f"{out_dir}/silence.wav", "wb") as w:
    w.setnchannels(ch); w.setsampwidth(sw); w.setframerate(fr)
    w.writeframes(silence(4.0))

# Same, but with a >600 ms pause between the wake word and the command: the
# wake tail's early end-of-speech must be ignored (min_utterance_ms) so the
# command is captured as one turn instead of a truncated fragment.
with wave.open(f"{out_dir}/listen_pause.wav", "wb") as w:
    w.setnchannels(ch); w.setsampwidth(sw); w.setframerate(fr)
    w.writeframes(silence(3.0) + frames + silence(1.0) + frames + silence(1.5))
PY

python3 "$ROOT/tests/mock_openai_server.py" --port "$PORT" >"$WORK/mock.log" 2>&1 &
MOCK_PID=$!
for _ in $(seq 1 50); do
  curl -sf "http://127.0.0.1:$PORT/health" >/dev/null 2>&1 && break
  sleep 0.1
done
curl -sf "http://127.0.0.1:$PORT/health" >/dev/null || fail "mock server did not start"

STREAM_CONFIG="$WORK/streaming.json"
python3 - "$CONFIG" "$STREAM_CONFIG" "$PORT" <<'PY'
import json, sys
src, dst, port = sys.argv[1], sys.argv[2], sys.argv[3]
with open(src) as f:
    cfg = json.load(f)
cfg["streaming"] = {
    "audio_base_url": f"http://127.0.0.1:{port}",
    "llm_base_url": f"http://127.0.0.1:{port}",
    "asr_mode": "buffered",
    "tts_mode": "stream",
    "llm_mode": "stream",
    "tts_pcm_rate": 24000,
    "tts_pcm_channels": 1,
    "llm_cache_prompt": True,
}
with open(dst, "w") as f:
    json.dump(cfg, f)
PY

export HOODIE_BASE_URL="http://127.0.0.1:$PORT"
export HOODIE_API_KEY="test-key"
export HOODIE_LOG_LEVEL="info"

# --- One full turn -----------------------------------------------------------
if ! "$BIN" -c "$CONFIG" --listen-wav "$WORK/listen.wav" --out "$WORK/reply.wav" \
    >"$WORK/run.log" 2>&1; then
  fail "listen run failed: $(tail -1 "$WORK/run.log")"
fi

grep -q "wake word 'alexa' detected" "$WORK/run.log" || fail "no wake detection"
grep -q "transcript: mock transcript" "$WORK/run.log" || fail "missing transcript"
grep -q "reply: mock final reply" "$WORK/run.log" || fail "missing agent reply"
[[ -s "$WORK/reply.wav" ]] || fail "no reply audio written"
size=$(stat -c%s "$WORK/reply.wav")
[[ "$size" -gt 10000 ]] || fail "reply audio too small: $size bytes"
pass "wake -> ASR -> agent -> TTS (reply $size bytes)"

# --- A pause after the wake word must not truncate the command ---------------
if ! "$BIN" -c "$CONFIG" --listen-wav "$WORK/listen_pause.wav" --out "$WORK/pause_reply.wav" \
    >"$WORK/pause.log" 2>&1; then
  fail "pause run failed: $(tail -1 "$WORK/pause.log")"
fi
turns=$(grep -c "transcript:" "$WORK/pause.log" || true)
[[ "$turns" == "1" ]] || fail "expected one turn after a pause, got $turns"
dur=$(grep -oE "\([0-9]+\.[0-9]+ s," "$WORK/pause.log" | head -1 | tr -cd '0-9.')
[[ -n "$dur" ]] || fail "no utterance logged for the pause case"
awk "BEGIN{exit !($dur > 1.5)}" || fail "utterance too short after a pause: ${dur}s"
pass "pause after wake captured as one turn (utterance ${dur}s)"

# --- Streaming pipeline: SSE LLM -> splitter -> raw-PCM TTS ------------------
if ! "$BIN" -c "$STREAM_CONFIG" --listen-wav "$WORK/listen_pause.wav" \
    --out "$WORK/stream_reply.wav" >"$WORK/stream.log" 2>&1; then
  fail "streaming pipeline failed: $(tail -1 "$WORK/stream.log")"
fi
grep -q "transcript: mock transcript" "$WORK/stream.log" || fail "streaming: missing transcript"
grep -q "reply: mock final reply" "$WORK/stream.log" || fail "streaming: missing reply"
[[ -s "$WORK/stream_reply.wav" ]] || fail "streaming: no reply audio"
size=$(stat -c%s "$WORK/stream_reply.wav")
[[ "$size" -gt 10000 ]] || fail "streaming: reply audio too small: $size bytes"
pass "streaming pipeline (SSE LLM + splitter + PCM TTS, reply $size bytes)"

# --- Live ASR pipeline -------------------------------------------------------
LIVE_CONFIG="$WORK/live.json"
python3 - "$STREAM_CONFIG" "$LIVE_CONFIG" <<'PY'
import json, sys
src, dst = sys.argv[1], sys.argv[2]
cfg = json.load(open(src))
cfg["streaming"]["asr_mode"] = "live"
json.dump(cfg, open(dst, "w"))
PY

if ! "$BIN" -c "$LIVE_CONFIG" --listen-wav "$WORK/listen_pause.wav" \
    --out "$WORK/live_reply.wav" >"$WORK/live.log" 2>&1; then
  fail "live asr pipeline failed: $(tail -1 "$WORK/live.log")"
fi
grep -q "transcript: mock transcript" "$WORK/live.log" || fail "live: missing transcript"
grep -q "reply: mock final reply" "$WORK/live.log" || fail "live: missing reply"
[[ -s "$WORK/live_reply.wav" ]] || fail "live: no reply audio"
size=$(stat -c%s "$WORK/live_reply.wav")
[[ "$size" -gt 10000 ]] || fail "live: reply audio too small: $size bytes"
pass "live ASR pipeline (reply $size bytes)"

# --- Tiny input blocks must not drop utterance audio -------------------------
# The resampler emits nothing for very small blocks; the utterance must keep
# those samples anyway (regression: it kept only a fraction of the span).
if ! "$BIN" -c "$CONFIG" --listen-wav "$WORK/listen_pause.wav" --wav-block-frames 1 \
    --out "$WORK/tiny_reply.wav" >"$WORK/tiny.log" 2>&1; then
  fail "tiny-block run failed: $(tail -1 "$WORK/tiny.log")"
fi
dur=$(grep -oE "\([0-9]+\.[0-9]+ s," "$WORK/tiny.log" | head -1 | tr -cd '0-9.')
[[ -n "$dur" ]] || fail "no utterance logged for the tiny-block case"
awk "BEGIN{exit !($dur > 1.5)}" || fail "utterance too short with tiny blocks: ${dur}s"
pass "tiny input blocks keep the full utterance (${dur}s)"

# --- Silence must not start a turn -------------------------------------------
if ! "$BIN" -c "$CONFIG" --listen-wav "$WORK/silence.wav" --out "$WORK/none.wav" \
    >"$WORK/silence.log" 2>&1; then
  fail "silence run failed: $(tail -1 "$WORK/silence.log")"
fi
if grep -q "transcript:" "$WORK/silence.log"; then
  fail "silence triggered a turn"
fi
pass "silence does not trigger the loop"

echo "listen: PASSED"
