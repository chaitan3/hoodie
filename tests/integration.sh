#!/usr/bin/env bash
# Integration test for the buffered ASR/TTS/LLM clients against the mock server.
#
#   tests/integration.sh [build-dir]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${1:-$ROOT/build}"
BIN="$BUILD_DIR/hoodie"
PORT="${HOODIE_TEST_PORT:-8099}"
CONFIG="$ROOT/config/hoodie.example.json"

fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "ok: $*"; }

[[ -x "$BIN" ]] || fail "binary not found: $BIN (build first)"

python3 "$ROOT/tests/mock_openai_server.py" --port "$PORT" >/tmp/hoodie_mock.log 2>&1 &
MOCK_PID=$!
trap 'kill "$MOCK_PID" 2>/dev/null || true' EXIT

for _ in $(seq 1 50); do
  if curl -sf "http://127.0.0.1:$PORT/health" >/dev/null 2>&1; then break; fi
  sleep 0.1
done
curl -sf "http://127.0.0.1:$PORT/health" >/dev/null || fail "mock server did not start"

export HOODIE_BASE_URL="http://127.0.0.1:$PORT"
export HOODIE_API_KEY="test-key"
export HOODIE_LOG_LEVEL="warn"

# --- LLM ---------------------------------------------------------------------
out="$("$BIN" -c "$CONFIG" --ask "ping")"
[[ "$out" == "mock reply: pong" ]] || fail "ask output was: '$out'"
pass "--ask"

# --- ASR ---------------------------------------------------------------------
out="$("$BIN" -c "$CONFIG" --transcribe "$ROOT/beep.ogg")"
[[ "$out" == "mock transcript" ]] || fail "transcribe output was: '$out'"
pass "--transcribe"

# --- TTS (buffered, pcm) -----------------------------------------------------
OUT_WAV="$(mktemp --suffix=.wav)"
"$BIN" -c "$CONFIG" --say "hello" --out "$OUT_WAV" >/dev/null
[[ -s "$OUT_WAV" ]] || fail "tts did not write output"
# 0.4 s at 24 kHz mono 16-bit => ~19200 bytes payload (+44 header)
size=$(stat -c%s "$OUT_WAV")
[[ "$size" -gt 10000 ]] || fail "tts output too small: $size bytes"
pass "--say (wrote $size bytes)"

# --- Round trip --------------------------------------------------------------
out="$("$BIN" -c "$CONFIG" --roundtrip "$ROOT/beep.ogg" --out "$OUT_WAV")"
echo "$out" | grep -q "mock transcript" || fail "roundtrip missing transcript"
echo "$out" | grep -q "mock final reply" || fail "roundtrip missing agent reply"
pass "--roundtrip"

# --- Streaming tiers (SSE LLM + raw-PCM TTS) --------------------------------
STREAM_CONFIG="$(mktemp --suffix=.json)"
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

out="$("$BIN" -c "$STREAM_CONFIG" --ask "ping")"
[[ "$out" == "mock reply: pong" ]] || fail "streaming ask output was: '$out'"
pass "--ask (SSE LLM)"

"$BIN" -c "$STREAM_CONFIG" --say "hello" --out "$OUT_WAV" >/dev/null
[[ -s "$OUT_WAV" ]] || fail "streaming tts wrote no output"
size=$(stat -c%s "$OUT_WAV")
[[ "$size" -gt 10000 ]] || fail "streaming tts output too small: $size bytes"
pass "--say (streaming PCM, wrote $size bytes)"

out="$("$BIN" -c "$STREAM_CONFIG" --roundtrip "$ROOT/beep.ogg" --out "$OUT_WAV")"
echo "$out" | grep -q "mock transcript" || fail "streaming roundtrip missing transcript"
echo "$out" | grep -q "mock final reply" || fail "streaming roundtrip missing agent reply"
pass "--roundtrip (SSE LLM + streaming TTS)"
rm -f "$STREAM_CONFIG"

# --- Live ASR (chunked PCM up, SSE down) ------------------------------------
LIVE_CONFIG="$(mktemp --suffix=.json)"
python3 - "$CONFIG" "$LIVE_CONFIG" "$PORT" <<'PY'
import json, sys
src, dst, port = sys.argv[1], sys.argv[2], sys.argv[3]
with open(src) as f:
    cfg = json.load(f)
cfg["streaming"] = {
    "audio_base_url": f"http://127.0.0.1:{port}",
    "llm_base_url": f"http://127.0.0.1:{port}",
    "asr_mode": "live",
    "tts_mode": "stream",
    "llm_mode": "stream",
    "tts_pcm_rate": 24000,
    "tts_pcm_channels": 1,
    "llm_cache_prompt": True,
}
with open(dst, "w") as f:
    json.dump(cfg, f)
PY

out="$("$BIN" -c "$LIVE_CONFIG" --transcribe "$ROOT/beep.ogg")"
[[ "$out" == "mock transcript" ]] || fail "live asr output was: '$out'"
pass "--transcribe (live ASR)"

out="$("$BIN" -c "$LIVE_CONFIG" --roundtrip "$ROOT/beep.ogg" --out "$OUT_WAV")"
echo "$out" | grep -q "mock transcript" || fail "live roundtrip missing transcript"
echo "$out" | grep -q "mock final reply" || fail "live roundtrip missing agent reply"
pass "--roundtrip (live ASR)"
rm -f "$LIVE_CONFIG"

# --- Live ASR falls back to the buffered endpoint on failure ----------------
FALLBACK_CONFIG="$(mktemp --suffix=.json)"
FALLBACK_LOG="$(mktemp)"
python3 - "$CONFIG" "$FALLBACK_CONFIG" "$PORT" <<'PY'
import json, sys
src, dst, port = sys.argv[1], sys.argv[2], sys.argv[3]
with open(src) as f:
    cfg = json.load(f)
cfg["streaming"] = {
    "audio_base_url": f"http://127.0.0.1:{port}",
    "llm_base_url": f"http://127.0.0.1:{port}",
    "asr_mode": "live",
    "asr_live_path": "/v1/broken/asr",
    "tts_mode": "buffered",
    "llm_mode": "buffered",
}
with open(dst, "w") as f:
    json.dump(cfg, f)
PY

out_file="$FALLBACK_LOG"
"$BIN" -c "$FALLBACK_CONFIG" --transcribe "$ROOT/beep.ogg" >"$out_file" 2>&1 ||
  fail "fallback asr run failed"
grep -q "mock transcript" "$out_file" || fail "fallback asr missing transcript"
grep -q "buffered fallback" "$out_file" || fail "live ASR did not report fallback"
pass "--transcribe (live -> buffered fallback)"
rm -f "$FALLBACK_CONFIG" "$FALLBACK_LOG"

# --- Error handling ----------------------------------------------------------
if HOODIE_BASE_URL="http://127.0.0.1:1" "$BIN" -c "$CONFIG" --ask "ping" >/dev/null 2>&1; then
  fail "expected failure against a dead server"
fi
pass "unreachable server fails cleanly"

echo "integration: PASSED"
