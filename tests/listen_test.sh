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
    w.writeframes(silence(3.0) + frames + silence(0.3) + frames + silence(1.5))

with wave.open(f"{out_dir}/silence.wav", "wb") as w:
    w.setnchannels(ch); w.setsampwidth(sw); w.setframerate(fr)
    w.writeframes(silence(4.0))
PY

python3 "$ROOT/tests/mock_openai_server.py" --port "$PORT" >"$WORK/mock.log" 2>&1 &
MOCK_PID=$!
for _ in $(seq 1 50); do
  curl -sf "http://127.0.0.1:$PORT/health" >/dev/null 2>&1 && break
  sleep 0.1
done
curl -sf "http://127.0.0.1:$PORT/health" >/dev/null || fail "mock server did not start"

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
