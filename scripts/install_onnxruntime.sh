#!/usr/bin/env bash
# Install a prebuilt ONNX Runtime for the host architecture (used for the
# wake-word and VAD models). Intended for Raspberry Pi / Debian, where there is
# no onnxruntime package.
#
#   sudo scripts/install_onnxruntime.sh                 # -> /usr/local
#   scripts/install_onnxruntime.sh 1.20.1 ~/ort         # local prefix (no sudo)
#
# The prebuilt tarball ships headers at include/*.h; hoodie includes them as
# <onnxruntime/...>, so they are installed under <prefix>/include/onnxruntime/.
set -euo pipefail

VERSION="${1:-1.20.1}"
PREFIX="${2:-/usr/local}"

case "$(uname -m)" in
  aarch64 | arm64) ORT_ARCH="aarch64" ;;
  x86_64 | amd64)  ORT_ARCH="x64" ;;
  *)
    echo "unsupported architecture: $(uname -m)" >&2
    exit 1
    ;;
esac

NAME="onnxruntime-linux-${ORT_ARCH}-${VERSION}"
URL="https://github.com/microsoft/onnxruntime/releases/download/v${VERSION}/${NAME}.tgz"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

echo "downloading ${NAME}..."
curl -sL --fail -o "$WORK/ort.tgz" "$URL"
tar xzf "$WORK/ort.tgz" -C "$WORK"
SRC="$WORK/$NAME"

echo "installing headers to ${PREFIX}/include/onnxruntime and libs to ${PREFIX}/lib"
mkdir -p "$PREFIX/include/onnxruntime" "$PREFIX/lib"
cp "$SRC"/include/*.h "$PREFIX/include/onnxruntime/"
cp -a "$SRC"/lib/* "$PREFIX/lib/"

if [[ "$PREFIX" == "/usr" || "$PREFIX" == "/usr/local" ]]; then
  if command -v ldconfig >/dev/null 2>&1 && [[ "$(id -u)" -eq 0 ]]; then
    ldconfig
  else
    echo "note: run 'sudo ldconfig' so the linker finds libonnxruntime.so" >&2
  fi
else
  echo "note: build with -DONNXRUNTIME_ROOT=$PREFIX and run with LD_LIBRARY_PATH=$PREFIX/lib" >&2
fi

echo "done: ONNX Runtime ${VERSION} (${ORT_ARCH})"
