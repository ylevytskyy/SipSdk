#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

swift package resolve

ARTIFACT_XCFW="$ROOT_DIR/.build/artifacts/sipsdk/BaresipCore/BaresipCore.xcframework"
SIM_LIB="$ARTIFACT_XCFW/ios-arm64_x86_64-simulator/libBaresipCore.a"
HEADER_DIR="$ARTIFACT_XCFW/ios-arm64_x86_64-simulator/Headers"
SIM_SDK_PATH="$(xcrun --sdk iphonesimulator --show-sdk-path)"

WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/sipsdk-sim-tests.XXXXXX")"
trap 'rm -rf "$WORK_DIR"' EXIT

APP_DIR="$WORK_DIR/SipSdkTestHost.app"
mkdir -p "$APP_DIR"

echo "==> Compiling Swift stores and CallKit manager"
xcrun --sdk iphonesimulator swiftc \
  -target arm64-apple-ios15.0-simulator \
  -sdk "$SIM_SDK_PATH" \
  -module-name SipSdkAppStores \
  -import-objc-header "$ROOT_DIR/SipSdk/SipSdk-Bridging-Header.h" \
  -Xcc -I"$ROOT_DIR/Sources/SipSdk/include" \
  -Xcc -I"$HEADER_DIR" \
  -emit-objc-header-path "$WORK_DIR/SipSdkAppStores-Swift.h" \
  -parse-as-library \
  -whole-module-optimization \
  -c \
  "$ROOT_DIR/SipSdk/SipAccountStore.swift" \
  "$ROOT_DIR/SipSdk/SipCallHistoryStore.swift" \
  "$ROOT_DIR/SipSdk/SipCallKitManager.swift" \
  -o "$WORK_DIR/SipSdkAppStores.o"

echo "==> Compiling SipSdk automated simulator test binary"
xcrun --sdk iphonesimulator clang++ \
  -x objective-c++ \
  -std=c++17 \
  -fobjc-arc \
  -fblocks \
  -target arm64-apple-ios15.0-simulator \
  -isysroot "$SIM_SDK_PATH" \
  -I "$HEADER_DIR" \
  -I "$ROOT_DIR/Sources/SipSdk/include" \
  -I "$WORK_DIR" \
  "$ROOT_DIR/Sources/SipSdk/SipClientBaresipV4.mm" \
  "$ROOT_DIR/Tests/SipSdkRunner/main.mm" \
  -x none "$WORK_DIR/SipSdkAppStores.o" "$SIM_LIB" \
  -L"$(xcode-select -p)/Toolchains/XcodeDefault.xctoolchain/usr/lib/swift/iphonesimulator" \
  -L/usr/lib/swift \
  -Wl,-rpath,/usr/lib/swift \
  -framework Foundation \
  -framework UIKit \
  -framework CoreFoundation \
  -framework SystemConfiguration \
  -framework CoreAudio \
  -framework AudioToolbox \
  -framework AVFoundation \
  -framework CFNetwork \
  -framework CoreMedia \
  -framework CoreVideo \
  -framework CoreGraphics \
  -framework VideoToolbox \
  -framework QuartzCore \
  -framework CallKit \
  -lz -lm \
  -o "$APP_DIR/SipSdkTestHost"

cp "$ROOT_DIR/Sources/SipSdkResources/Resources/config.baresip-v4" "$APP_DIR/config.baresip-v4"

cat > "$APP_DIR/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleExecutable</key>
  <string>SipSdkTestHost</string>
  <key>CFBundleIdentifier</key>
  <string>com.wiseland.SipSdk.TestHost</string>
  <key>CFBundleName</key>
  <string>SipSdkTestHost</string>
  <key>CFBundlePackageType</key>
  <string>APPL</string>
  <key>CFBundleShortVersionString</key>
  <string>1.0</string>
  <key>CFBundleVersion</key>
  <string>1</string>
  <key>LSRequiresIPhoneOS</key>
  <true/>
</dict>
</plist>
PLIST

xcrun codesign --force --sign - "$APP_DIR" >/dev/null 2>&1 || true

# Find a booted simulator UDID
SIM_UDID="$(xcrun simctl list devices booted | grep -Eo '[0-9A-F]{8}-([0-9A-F]{4}-){3}[0-9A-F]{12}' | head -n 1)"
if [[ -z "$SIM_UDID" ]]; then
  echo "ERROR: No booted iOS Simulator found" >&2
  exit 1
fi

echo "==> Running SipSdkTestHost on simulator $SIM_UDID"
xcrun simctl spawn "$SIM_UDID" "$APP_DIR/SipSdkTestHost"
