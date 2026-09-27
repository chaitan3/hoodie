#!/usr/bin/env bash
# Wake-word + VAD/endpointer test. Runs the detector over known audio and
# asserts deterministic, expected outcomes (no microphone required).
#
#   tests/wake_test.sh [build-dir]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${1:-$ROOT/build}"
BIN="$BUILD_DIR/hoodie"
CONFIG="$ROOT/config/hoodie.example.json"
DATA="$ROOT/tests/data"

fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "ok: $*"; }

[[ -x "$BIN" ]] || fail "binary not found: $BIN (build first)"
[[ -s "$ROOT/models/alexa_v0.1.onnx" ]] || fail "models missing; run scripts/fetch_models.sh"
[[ -s "$DATA/alexa_test.wav" ]] || fail "test audio missing; run scripts/fetch_models.sh"
export HOODIE_LOG_LEVEL="${HOODIE_LOG_LEVEL:-warn}"

# Generate silence/tone fixtures if absent (pure stdlib).
if [[ ! -s "$DATA/silence.wav" || ! -s "$DATA/tone.wav" ]]; then
  python3 - "$DATA" <<'PY'
import math, struct, sys, wave
data = sys.argv[1]
def write(path, samples, rate=16000):
    with wave.open(path, 'wb') as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(rate)
        w.writeframes(struct.pack('<%dh' % len(samples), *samples))
n = int(16000 * 1.5)
write(f"{data}/silence.wav", [0] * n)
write(f"{data}/tone.wav", [int(math.sin(2 * math.pi * 440 * i / 16000) * 12000) for i in range(n)])
PY
fi

# --- Wake word ---------------------------------------------------------------
out="$("$BIN" -c "$CONFIG" --wakeword "$DATA/alexa_test.wav")"
echo "$out" | grep -q "detected=yes" || fail "wake word not detected: $out"
pass "wake word detected ($out)"

out="$("$BIN" -c "$CONFIG" --wakeword "$DATA/silence.wav")"
echo "$out" | grep -q "detected=no" || fail "silence false positive: $out"
pass "silence rejected ($out)"

out="$("$BIN" -c "$CONFIG" --wakeword "$DATA/tone.wav")"
echo "$out" | grep -q "detected=no" || fail "tone false positive: $out"
pass "tone rejected ($out)"

# Determinism: same input -> identical output across runs.
a="$("$BIN" -c "$CONFIG" --wakeword "$DATA/alexa_test.wav")"
b="$("$BIN" -c "$CONFIG" --wakeword "$DATA/alexa_test.wav")"
[[ "$a" == "$b" ]] || fail "wake word output not deterministic: '$a' vs '$b'"
pass "wake word deterministic"

# --- VAD ---------------------------------------------------------------------
out="$("$BIN" -c "$CONFIG" --vad "$DATA/alexa_test.wav")"
echo "$out" | grep -q "detected=yes" || fail "VAD missed speech: $out"
pass "VAD detects speech ($out)"

out="$("$BIN" -c "$CONFIG" --vad "$DATA/silence.wav")"
echo "$out" | grep -q "detected=no" || fail "VAD saw speech in silence: $out"
pass "VAD rejects silence ($out)"

# --- Endpointer ---------------------------------------------------------------
out="$("$BIN" -c "$CONFIG" --endpoint "$DATA/alexa_test.wav")"
echo "$out" | grep -qE "segments=[1-9]" || fail "endpointer found no segment: $out"
pass "endpointer segments ($out)"

out="$("$BIN" -c "$CONFIG" --endpoint "$DATA/silence.wav")"
echo "$out" | grep -q "segments=0" || fail "endpointer found speech in silence: $out"
pass "endpointer ignores silence"

echo "wake: PASSED"
