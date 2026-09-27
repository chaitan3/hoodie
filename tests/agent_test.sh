#!/usr/bin/env bash
# Agent + tools test: exercises the tool-calling loop against the mock server.
# The mock returns a get_time tool call on the first round and a final answer
# once a tool result is present, so a passing run proves the loop executed the
# tool and fed the result back.
#
#   tests/agent_test.sh [build-dir]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${1:-$ROOT/build}"
BIN="$BUILD_DIR/hoodie"
PORT="${HOODIE_TEST_PORT:-8098}"
CONFIG="$ROOT/config/hoodie.example.json"

fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "ok: $*"; }

[[ -x "$BIN" ]] || fail "binary not found: $BIN (build first)"

# --- text normalization (no server) -----------------------------------------
out="$("$BIN" --norm "It is 2.5 km, see [map](https://x.test) **now**")"
[[ "$out" == "It is two point five km, see map now" ]] || fail "norm output: '$out'"
pass "--norm"

# --- agent loop --------------------------------------------------------------
python3 "$ROOT/tests/mock_openai_server.py" --port "$PORT" >/tmp/hoodie_agent_mock.log 2>&1 &
MOCK_PID=$!
trap 'kill "$MOCK_PID" 2>/dev/null || true' EXIT

for _ in $(seq 1 50); do
  curl -sf "http://127.0.0.1:$PORT/health" >/dev/null 2>&1 && break
  sleep 0.1
done
curl -sf "http://127.0.0.1:$PORT/health" >/dev/null || fail "mock server did not start"

export HOODIE_BASE_URL="http://127.0.0.1:$PORT"
export HOODIE_API_KEY="test-key"
export HOODIE_LOG_LEVEL="warn"

OUT_WAV="$(mktemp --suffix=.wav)"
out="$("$BIN" -c "$CONFIG" --agent "what time is it?" --out "$OUT_WAV")"
echo "$out" | grep -q "mock final reply" || fail "agent reply: '$out'"
[[ -s "$OUT_WAV" ]] || fail "agent did not synthesize speech"
pass "agent tool loop ($out)"

rm -f "$OUT_WAV"
echo "agent: PASSED"
