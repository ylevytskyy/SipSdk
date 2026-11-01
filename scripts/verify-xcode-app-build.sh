#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

swift package resolve

DERIVED_SIM="$(mktemp -d "${TMPDIR:-/tmp}/sipsdk-xcode-sim.XXXXXX")"
DERIVED_DEV="$(mktemp -d "${TMPDIR:-/tmp}/sipsdk-xcode-dev.XXXXXX")"
trap 'rm -rf "$DERIVED_SIM" "$DERIVED_DEV"' EXIT

echo "==> Building SipSdk.xcodeproj for arm64 iOS Simulator"
xcodebuild \
  -project SipSdk.xcodeproj \
  -scheme SipSdk \
  -configuration Debug \
  -sdk iphonesimulator \
  -destination 'generic/platform=iOS Simulator' \
  -derivedDataPath "$DERIVED_SIM" \
  ARCHS=arm64 \
  ONLY_ACTIVE_ARCH=YES \
  CODE_SIGNING_ALLOWED=NO \
  build

SIM_APP="$DERIVED_SIM/Build/Products/Debug-iphonesimulator/SipSdk.app"
test -f "$SIM_APP/SipSdk"
test -f "$SIM_APP/config.baresip-v4"
grep -q "NSCameraUsageDescription" "$SIM_APP/Info.plist"
grep -q "NSMicrophoneUsageDescription" "$SIM_APP/Info.plist"

echo "==> Building SipSdk.xcodeproj for arm64 generic iOS Device"
xcodebuild \
  -project SipSdk.xcodeproj \
  -scheme SipSdk \
  -configuration Debug \
  -sdk iphoneos \
  -destination 'generic/platform=iOS' \
  -derivedDataPath "$DERIVED_DEV" \
  ARCHS=arm64 \
  ONLY_ACTIVE_ARCH=YES \
  CODE_SIGNING_ALLOWED=NO \
  build

DEV_APP="$DERIVED_DEV/Build/Products/Debug-iphoneos/SipSdk.app"
test -f "$DEV_APP/SipSdk"
test -f "$DEV_APP/config.baresip-v4"

echo "XCODE_SIM_AND_DEVICE_BUILD_OK"
