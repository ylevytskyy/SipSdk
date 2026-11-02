#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

EVIDENCE_DIR="$ROOT_DIR/specs/ios-video-phone/evidence"
mkdir -p "$EVIDENCE_DIR"

DERIVED_SIM="$(mktemp -d "${TMPDIR:-/tmp}/sipphone-sim-launch.XXXXXX")"
trap 'rm -rf "$DERIVED_SIM"' EXIT

SIM_UDID="$(xcrun simctl list devices booted | grep -Eo '[0-9A-F]{8}-([0-9A-F]{4}-){3}[0-9A-F]{12}' | head -n 1)"
if [[ -z "$SIM_UDID" ]]; then
  echo "ERROR: No booted iOS Simulator found" >&2
  exit 1
fi

echo "==> Building SipPhone.app for simulator $SIM_UDID"
xcodebuild \
  -project platforms/ios/sample/SipPhone.xcodeproj \
  -scheme SipPhone \
  -configuration Debug \
  -sdk iphonesimulator \
  -destination "id=$SIM_UDID" \
  -derivedDataPath "$DERIVED_SIM" \
  ARCHS=arm64 \
  ONLY_ACTIVE_ARCH=YES \
  CODE_SIGNING_ALLOWED=NO \
  build >/dev/null

SIM_APP="$DERIVED_SIM/Build/Products/Debug-iphonesimulator/SipPhone.app"
BUNDLE_ID="com.sipsdk.sample.SipPhone"

echo "==> Installing SipPhone.app on simulator $SIM_UDID"
xcrun simctl terminate "$SIM_UDID" "$BUNDLE_ID" >/dev/null 2>&1 || true
xcrun simctl install "$SIM_UDID" "$SIM_APP"

echo "==> Launching SipPhone.app"
xcrun simctl launch "$SIM_UDID" "$BUNDLE_ID"

sleep 2

DIALER_PNG="$EVIDENCE_DIR/sim-sipphone-screen.png"
xcrun simctl io "$SIM_UDID" screenshot "$DIALER_PNG"
test -s "$DIALER_PNG"

echo "SIM_APP_LAUNCH_OK"
