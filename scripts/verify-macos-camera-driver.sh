#!/usr/bin/env bash
# Builds and runs the macOS sample camera driver unit tests
# (platforms/macos/sample/Tests/test_camera_driver.mm). No PBX or camera needed:
# the tests use the synthetic test-pattern camera.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

BRIDGE_DIR="$ROOT_DIR/platforms/macos/sample/SipSdkMac/Bridge"
PREBUILT_DIR="$ROOT_DIR/platforms/desktop/prebuilt"
BUILD_DIR="$ROOT_DIR/.build/macos_camera_driver_test"
TEST_BIN="$BUILD_DIR/test_camera_driver"
mkdir -p "$BUILD_DIR"

echo "==> Building macOS camera driver tests..."
xcrun clang++ \
  -std=c++17 -fobjc-arc -g -O1 \
  -DUSE_UNIXSOCK=1 -DSIP_CORE_BUILDING_LIB=1 \
  -I "$ROOT_DIR/core/include" \
  -I "$ROOT_DIR/core/src" \
  -I "$PREBUILT_DIR/include" \
  -I "$PREBUILT_DIR/include/re" \
  -I "$PREBUILT_DIR/include/baresip" \
  -I "$BRIDGE_DIR" \
  "$ROOT_DIR/platforms/macos/sample/Tests/test_camera_driver.mm" \
  "$BRIDGE_DIR/SipMacCall.mm" \
  "$BRIDGE_DIR/SipMacVideoView.mm" \
  "$ROOT_DIR/core/src/call_session.cpp" \
  "$ROOT_DIR/core/src/engine.cpp" \
  "$ROOT_DIR/core/src/media_driver.cpp" \
  "$ROOT_DIR/core/src/sip_abi.cpp" \
  -L "$PREBUILT_DIR/darwin-arm64/lib" -lbaresip -lre -lz -lm \
  -framework Foundation \
  -framework AppKit \
  -framework AVFoundation \
  -framework AudioToolbox \
  -framework CFNetwork \
  -framework CoreAudio \
  -framework CoreFoundation \
  -framework CoreMedia \
  -framework CoreVideo \
  -framework QuartzCore \
  -framework SystemConfiguration \
  -framework VideoToolbox \
  -o "$TEST_BIN"

echo "==> Running macOS camera driver tests..."
"$TEST_BIN"
