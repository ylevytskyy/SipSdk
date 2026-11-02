#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

SIM_SDK_PATH="$(xcrun --sdk iphonesimulator --show-sdk-path)"
SIM_PLATFORM_PATH="$(xcrun --sdk iphonesimulator --show-sdk-platform-path)"

echo "==> 1. Compiling SipSdk unit & integration tests for arm64 iOS Simulator"
swift build \
  --build-tests \
  --triple arm64-apple-ios17.0-simulator \
  -Xcc -isysroot -Xcc "$SIM_SDK_PATH" \
  -Xcxx -isysroot -Xcxx "$SIM_SDK_PATH"

TEST_BUNDLE="$(find "$ROOT_DIR/.build" -name "SipSdkTests.xctest" | head -n 1)"
if [[ -z "$TEST_BUNDLE" || ! -d "$TEST_BUNDLE" ]]; then
  echo "ERROR: Could not locate built SipSdkTests.xctest" >&2
  exit 1
fi

SIM_UDID="$(xcrun simctl list devices booted | grep -Eo '[0-9A-F]{8}-([0-9A-F]{4}-){3}[0-9A-F]{12}' | head -n 1)"
if [[ -z "$SIM_UDID" ]]; then
  echo "ERROR: No booted iOS Simulator found. Please boot an iOS Simulator (e.g. xcrun simctl boot <UDID>)" >&2
  exit 1
fi

XCTEST_BIN="$SIM_PLATFORM_PATH/Developer/Library/Xcode/Agents/xctest"
if [[ ! -f "$XCTEST_BIN" ]]; then
  echo "ERROR: xctest agent binary not found at $XCTEST_BIN" >&2
  exit 1
fi

echo "==> 2. Spawning SipSdkTests on booted iOS Simulator ($SIM_UDID)"
xcrun simctl spawn "$SIM_UDID" "$XCTEST_BIN" "$TEST_BUNDLE"

echo "ALL_IOS_VIDEO_TESTS_PASSED"
