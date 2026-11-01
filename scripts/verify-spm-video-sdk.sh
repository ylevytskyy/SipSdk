#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

echo "==> 1. Checking SwiftPM manifest"
swift package dump-package >/dev/null
swift package resolve

echo "==> 2. Building SwiftPM package for arm64 iOS Simulator"
SIM_SDK_PATH="$(xcrun --sdk iphonesimulator --show-sdk-path)"
swift build \
  --triple arm64-apple-ios15.0-simulator \
  -Xcc -isysroot -Xcc "$SIM_SDK_PATH" \
  -Xcxx -isysroot -Xcxx "$SIM_SDK_PATH"

echo "==> 3. Running check-baresip-adapter.sh across device-arm64, simulator-arm64, and simulator-x86_64"
ARTIFACT_XCFW="$ROOT_DIR/.build/artifacts/sipsdk/BaresipCore/BaresipCore.xcframework"
[[ -d "$ARTIFACT_XCFW" ]] || { echo "ERROR: Missing $ARTIFACT_XCFW" >&2; exit 1; }

ADAPTER_CHECK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/sipsdk-adapter-check.XXXXXX")"
trap 'rm -rf "$ADAPTER_CHECK_DIR"' EXIT

ln -s "$ARTIFACT_XCFW" "$ADAPTER_CHECK_DIR/BaresipCore.xcframework"
ln -s "$ARTIFACT_XCFW/ios-arm64/Headers" "$ADAPTER_CHECK_DIR/headers"

bash "$ROOT_DIR/scripts/check-baresip-adapter.sh" "$ADAPTER_CHECK_DIR"

echo "SDK_BUILD_AND_LINK_OK"
