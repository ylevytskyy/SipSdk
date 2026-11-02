#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

echo "==> 1. Checking SwiftPM manifest"
swift package dump-package >/dev/null
swift package resolve

SIM_SDK_PATH="$(xcrun --sdk iphonesimulator --show-sdk-path)"

echo "==> 2. Building SwiftPM package for arm64 iOS Simulator"
swift build \
  --triple arm64-apple-ios17.0-simulator \
  -Xcc -isysroot -Xcc "$SIM_SDK_PATH" \
  -Xcxx -isysroot -Xcxx "$SIM_SDK_PATH"

echo "==> 3. Building SwiftPM package for x86_64 iOS Simulator"
swift build \
  --triple x86_64-apple-ios17.0-simulator \
  -Xcc -isysroot -Xcc "$SIM_SDK_PATH" \
  -Xcxx -isysroot -Xcxx "$SIM_SDK_PATH"

echo "==> 4. Building SipSdk package for arm64 generic iOS Device"
xcodebuild \
  -project platforms/ios/sample/SipPhone.xcodeproj \
  -scheme SipSdk \
  -configuration Debug \
  -sdk iphoneos \
  -destination 'generic/platform=iOS' \
  ARCHS=arm64 \
  ONLY_ACTIVE_ARCH=YES \
  CODE_SIGNING_ALLOWED=NO \
  build >/dev/null

echo "SDK_BUILD_AND_LINK_OK"
