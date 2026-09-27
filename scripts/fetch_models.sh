#!/usr/bin/env bash
# Download the ONNX models hoodie needs (openWakeWord feature + keyword models
# and Silero VAD) into models/. Safe to re-run; existing files are kept.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="${1:-$ROOT/models}"
BASE="https://github.com/dscripka/openWakeWord/releases/download/v0.5.1"

mkdir -p "$DEST"
for model in melspectrogram.onnx embedding_model.onnx alexa_v0.1.onnx silero_vad.onnx; do
  if [[ -s "$DEST/$model" ]]; then
    echo "have $model"
  else
    echo "fetching $model..."
    curl -sL --fail -o "$DEST/$model" "$BASE/$model"
  fi
done
echo "models in $DEST"

# Official openWakeWord test clip, used by tests/wake_test.sh.
TEST_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/tests/data"
mkdir -p "$TEST_DIR"
if [[ ! -s "$TEST_DIR/alexa_test.wav" ]]; then
  echo "fetching alexa_test.wav..."
  curl -sL --fail -o "$TEST_DIR/alexa_test.wav" \
    https://raw.githubusercontent.com/dscripka/openWakeWord/main/tests/data/alexa_test.wav
fi
echo "test audio in $TEST_DIR"
