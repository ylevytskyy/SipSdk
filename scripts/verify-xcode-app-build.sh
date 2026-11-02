#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

swift package resolve

DERIVED_SIM="$(mktemp -d "${TMPDIR:-/tmp}/sipphone-xcode-sim.XXXXXX")"
DERIVED_DEV="$(mktemp -d "${TMPDIR:-/tmp}/sipphone-xcode-dev.XXXXXX")"
trap 'rm -rf "$DERIVED_SIM" "$DERIVED_DEV"' EXIT

echo "==> Building SipPhone.xcodeproj for arm64 iOS Simulator"
xcodebuild \
  -project platforms/ios/sample/SipPhone.xcodeproj \
  -scheme SipPhone \
  -configuration Debug \
  -sdk iphonesimulator \
  -destination 'generic/platform=iOS Simulator' \
  -derivedDataPath "$DERIVED_SIM" \
  ARCHS=arm64 \
  ONLY_ACTIVE_ARCH=YES \
  CODE_SIGNING_ALLOWED=NO \
  build >/dev/null

SIM_APP="$DERIVED_SIM/Build/Products/Debug-iphonesimulator/SipPhone.app"
test -f "$SIM_APP/SipPhone"
test -f "$SIM_APP/SipSdk_SipSdk.bundle/config.baresip-v4"
grep -q "NSCameraUsageDescription" "$SIM_APP/Info.plist"
grep -q "NSMicrophoneUsageDescription" "$SIM_APP/Info.plist"

echo "==> Building SipPhone.xcodeproj for arm64 generic iOS Device"
xcodebuild \
  -project platforms/ios/sample/SipPhone.xcodeproj \
  -scheme SipPhone \
  -configuration Debug \
  -sdk iphoneos \
  -destination 'generic/platform=iOS' \
  -derivedDataPath "$DERIVED_DEV" \
  ARCHS=arm64 \
  ONLY_ACTIVE_ARCH=YES \
  CODE_SIGNING_ALLOWED=NO \
  build >/dev/null

DEV_APP="$DERIVED_DEV/Build/Products/Debug-iphoneos/SipPhone.app"
test -f "$DEV_APP/SipPhone"
test -f "$DEV_APP/SipSdk_SipSdk.bundle/config.baresip-v4"

echo "XCODE_SIM_AND_DEVICE_BUILD_OK"
