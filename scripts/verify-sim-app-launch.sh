#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

EVIDENCE_DIR="$ROOT_DIR/specs/ios-video-phone/evidence"
mkdir -p "$EVIDENCE_DIR"

DERIVED_SIM="$(mktemp -d "${TMPDIR:-/tmp}/sipsdk-sim-launch.XXXXXX")"
trap 'rm -rf "$DERIVED_SIM"' EXIT

SIM_UDID="$(xcrun simctl list devices booted | grep -Eo '[0-9A-F]{8}-([0-9A-F]{4}-){3}[0-9A-F]{12}' | head -n 1)"
if [[ -z "$SIM_UDID" ]]; then
  echo "ERROR: No booted iOS Simulator found" >&2
  exit 1
fi

echo "==> Building SipSdk.app for simulator $SIM_UDID"
xcodebuild \
  -project SipSdk.xcodeproj \
  -scheme SipSdk \
  -configuration Debug \
  -sdk iphonesimulator \
  -destination "id=$SIM_UDID" \
  -derivedDataPath "$DERIVED_SIM" \
  ARCHS=arm64 \
  ONLY_ACTIVE_ARCH=YES \
  CODE_SIGNING_ALLOWED=NO \
  build >/dev/null

SIM_APP="$DERIVED_SIM/Build/Products/Debug-iphonesimulator/SipSdk.app"
BUNDLE_ID="com.wiseland.SipSdk"

echo "==> Installing SipSdk.app on simulator $SIM_UDID"
xcrun simctl terminate "$SIM_UDID" "$BUNDLE_ID" >/dev/null 2>&1 || true
xcrun simctl install "$SIM_UDID" "$SIM_APP"

STATUS_FILE="$DERIVED_SIM/ui-verification-status.txt"
rm -f "$STATUS_FILE"

echo "==> Launching SipSdk.app in Dialer mode"
SIMCTL_CHILD_SIPSDK_UITEST_MODE="dialer" \
SIMCTL_CHILD_SIPSDK_UITEST_STATUS_FILE="$STATUS_FILE" \
  xcrun simctl launch "$SIM_UDID" "$BUNDLE_ID"

for _ in {1..30}; do
  if [[ -f "$STATUS_FILE" ]] && grep -q "DIALER_READY" "$STATUS_FILE"; then
    break
  fi
  sleep 0.5
done

grep -q "DIALER_READY" "$STATUS_FILE"
DIALER_PNG="$EVIDENCE_DIR/sim-dialer-screen.png"
xcrun simctl io "$SIM_UDID" screenshot "$DIALER_PNG"
test -s "$DIALER_PNG"

echo "==> Launching SipSdk.app in Account Settings modal mode"
xcrun simctl terminate "$SIM_UDID" "$BUNDLE_ID" >/dev/null 2>&1 || true
rm -f "$STATUS_FILE"

SIMCTL_CHILD_SIPSDK_UITEST_MODE="account" \
SIMCTL_CHILD_SIPSDK_UITEST_STATUS_FILE="$STATUS_FILE" \
  xcrun simctl launch "$SIM_UDID" "$BUNDLE_ID"

for _ in {1..30}; do
  if [[ -f "$STATUS_FILE" ]] && grep -q "ACCOUNT_MODAL_READY" "$STATUS_FILE"; then
    break
  fi
  sleep 0.5
done

grep -q "ACCOUNT_MODAL_READY" "$STATUS_FILE"
ACCOUNT_PNG="$EVIDENCE_DIR/sim-account-settings.png"
xcrun simctl io "$SIM_UDID" screenshot "$ACCOUNT_PNG"
test -s "$ACCOUNT_PNG"

echo "==> Launching SipSdk.app in In-Call Video Phone mode"
xcrun simctl terminate "$SIM_UDID" "$BUNDLE_ID" >/dev/null 2>&1 || true
rm -f "$STATUS_FILE"

SIMCTL_CHILD_SIPSDK_UITEST_MODE="incall_video" \
SIMCTL_CHILD_SIPSDK_UITEST_STATUS_FILE="$STATUS_FILE" \
  xcrun simctl launch "$SIM_UDID" "$BUNDLE_ID"

for _ in {1..40}; do
  if [[ -f "$STATUS_FILE" ]] && grep -q "INCALL_VIDEO_READY" "$STATUS_FILE"; then
    break
  fi
  sleep 0.5
done

cat "$STATUS_FILE"
grep -q "INCALL_VIDEO_READY" "$STATUS_FILE"
grep -q "REMOTE_FRAMES_OK" "$STATUS_FILE"
grep -q "LOCAL_FRAMES_OK" "$STATUS_FILE"
grep -q "CONTROLS_EXERCISED_OK" "$STATUS_FILE"
grep -q "CALLKIT_READY_OK" "$STATUS_FILE"

INCALL_PNG="$EVIDENCE_DIR/sim-incall-video-screen.png"
xcrun simctl io "$SIM_UDID" screenshot "$INCALL_PNG"
test -s "$INCALL_PNG"

echo "Screenshots saved:"
ls -lh "$DIALER_PNG" "$ACCOUNT_PNG" "$INCALL_PNG"
echo "SIM_APP_LAUNCH_VERIFIED"
