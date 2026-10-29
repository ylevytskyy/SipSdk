#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
OUTPUT_DIR="$1"
if [[ -z "$OUTPUT_DIR" ]]; then
  echo "usage: check-baresip-adapter.sh <build-output-dir>" >&2
  exit 2
fi
HEADER_DIR="$OUTPUT_DIR/headers"
SOURCE="$ROOT_DIR/TareSIPDemo/SipClientBaresipV4.mm"
SDK_PATH="$(xcrun --sdk iphoneos --show-sdk-path)"

[[ -f "$HEADER_DIR/BaresipCore.h" ]] || {
  echo "Missing generated BaresipCore.h at $HEADER_DIR" >&2
  exit 1
}

echo "==> Syntax-checking SipClientBaresipV4.mm against current Baresip headers"
xcrun --sdk iphoneos clang++ \
  -x objective-c++ \
  -std=c++17 \
  -fobjc-arc \
  -fblocks \
  -fsyntax-only \
  -target arm64-apple-ios12.1 \
  -isysroot "$SDK_PATH" \
  -I "$HEADER_DIR" \
  "$SOURCE"
