#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

echo "==> Resolving Swift packages to ensure BaresipCore is present"
swift package resolve

ARTIFACT_XCFW="$ROOT_DIR/.build/artifacts/sipsdk/BaresipCore/BaresipCore.xcframework"
SIM_LIB="$ARTIFACT_XCFW/ios-arm64_x86_64-simulator/libBaresipCore.a"
HEADER_DIR="$ARTIFACT_XCFW/ios-arm64_x86_64-simulator/Headers"
SIM_SDK_PATH="$(xcrun --sdk iphonesimulator --show-sdk-path)"

if [[ ! -f "$SIM_LIB" || ! -d "$HEADER_DIR" ]]; then
  echo "ERROR: BaresipCore artifacts not found at $ARTIFACT_XCFW" >&2
  exit 1
fi

BUILD_DIR="$ROOT_DIR/.build/core-sim-build"
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"

echo "==> Configuring core engine with CMake for iOS Simulator"
cmake -S "$ROOT_DIR/core" -B "$BUILD_DIR" \
  -DCMAKE_SYSTEM_NAME=iOS \
  -DCMAKE_OSX_SYSROOT="$SIM_SDK_PATH" \
  -DCMAKE_OSX_ARCHITECTURES="arm64" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET="15.0" \
  -DCMAKE_BUILD_TYPE=Release \
  -DBARESIP_INCLUDE_DIR="$HEADER_DIR" \
  -DBARESIP_LIBRARY="$SIM_LIB"

echo "==> Building sip_core static library and test runner"
cmake --build "$BUILD_DIR" --parallel

if [[ ! -f "$BUILD_DIR/libsipcore.a" ]]; then
  echo "ERROR: Expected $BUILD_DIR/libsipcore.a to exist" >&2
  exit 1
fi

echo "==> Checking symbol visibility: only _sip_* must be exported from libsipcore.a"
LEAKED_SYMBOLS="$(xcrun nm -m "$BUILD_DIR/libsipcore.a" | grep -E '^[0-9a-fA-F]+ \([^)]+\) external ' | grep -v ' _sip_' || true)"
if [[ -n "$LEAKED_SYMBOLS" ]]; then
  echo "ERROR: Found exported symbols in libsipcore.a that do not match the C ABI (_sip_*):" >&2
  echo "$LEAKED_SYMBOLS" >&2
  exit 1
fi
echo "PASS: Symbol audit clean (only _sip_* symbols exported)"

# Find booted simulator
SIM_UDID="$(xcrun simctl list devices booted | grep -Eo '[0-9A-F]{8}-([0-9A-F]{4}-){3}[0-9A-F]{12}' | head -n 1)"
if [[ -z "$SIM_UDID" ]]; then
  echo "ERROR: No booted iOS Simulator found. Please boot a simulator first." >&2
  exit 1
fi

# Prepare temporary configuration directory
CONFIG_DIR="$(mktemp -d "${TMPDIR:-/tmp}/sipcore-conf.XXXXXX")"
trap 'rm -rf "$CONFIG_DIR"' EXIT
cp "$ROOT_DIR/platforms/ios/Sources/SipSdk/Resources/config.baresip-v4" "$CONFIG_DIR/config"

echo "==> Running lifecycle test runner on booted simulator ($SIM_UDID)"
TEST_BUNDLE="$BUILD_DIR/sip_core_lifecycle_test.app"
TEST_BIN="$TEST_BUNDLE/sip_core_lifecycle_test"

if [[ -d "$TEST_BUNDLE" ]]; then
  xcrun codesign --force --sign - "$TEST_BUNDLE" >/dev/null 2>&1 || true
elif [[ -f "$BUILD_DIR/sip_core_lifecycle_test" ]]; then
  TEST_BIN="$BUILD_DIR/sip_core_lifecycle_test"
  xcrun codesign --force --sign - "$TEST_BIN" >/dev/null 2>&1 || true
else
  echo "ERROR: Cannot find compiled test binary in $BUILD_DIR" >&2
  exit 1
fi

xcrun simctl spawn "$SIM_UDID" "$TEST_BIN" "$CONFIG_DIR"

echo "==> Verifying zero regressions against existing iOS video test suite"
bash "$ROOT_DIR/scripts/verify-ios-video-tests.sh"

echo "==> ALL_CORE_LIFECYCLE_VERIFICATIONS_PASSED"
