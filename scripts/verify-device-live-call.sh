#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

echo "==> Discovering connected physical iOS device via xcrun devicectl"
DEVICE_JSON="$(mktemp "${TMPDIR:-/tmp}/sipsdk-devices.XXXXXX.json")"
DERIVED_DEV="$(mktemp -d "${TMPDIR:-/tmp}/sipsdk-device-run.XXXXXX")"
CONSOLE_LOG="$(mktemp "${TMPDIR:-/tmp}/sipsdk-device-console.XXXXXX.log")"
trap 'rm -rf "$DEVICE_JSON" "$DERIVED_DEV" "$CONSOLE_LOG"' EXIT

xcrun devicectl list devices --json-output "$DEVICE_JSON" >/dev/null

DEVICE_UDID="$(python3 -c '
import json, sys
data = json.load(open(sys.argv[1]))
devices = data.get("result", {}).get("devices", [])
for d in devices:
    props = d.get("connectionProperties", {})
    hw = d.get("hardwareProperties", {})
    name = d.get("deviceProperties", {}).get("name", "")
    if hw.get("reality") == "physical" and props.get("tunnelState") in ("connected", "available"):
        if "iPhone 11 Pro" in name:
            print(hw.get("udid") or d.get("identifier"))
            sys.exit(0)
for d in devices:
    props = d.get("connectionProperties", {})
    hw = d.get("hardwareProperties", {})
    if hw.get("reality") == "physical" and props.get("tunnelState") in ("connected", "available"):
        print(hw.get("udid") or d.get("identifier"))
        sys.exit(0)
' "$DEVICE_JSON")"

if [[ -z "$DEVICE_UDID" ]]; then
  echo "ERROR: No connected physical iOS device found" >&2
  exit 1
fi

echo "==> Building signed SipSdk.app for physical device $DEVICE_UDID"
xcodebuild \
  -project SipSdk.xcodeproj \
  -scheme SipSdk \
  -configuration Debug \
  -destination "id=$DEVICE_UDID" \
  -derivedDataPath "$DERIVED_DEV" \
  -allowProvisioningUpdates \
  build >/dev/null

DEV_APP="$DERIVED_DEV/Build/Products/Debug-iphoneos/SipSdk.app"
test -f "$DEV_APP/SipSdk"

echo "==> Installing SipSdk.app onto physical device $DEVICE_UDID"
xcrun devicectl device install app --device "$DEVICE_UDID" "$DEV_APP"

echo "==> Launching SipSdk.app on physical device $DEVICE_UDID and streaming console verification"
(
  SIPSDK_UITEST_MODE="device_verify" \
  SIPSDK_DEVICE_CALL_URI="sip:echo@iptel.org" \
  xcrun devicectl device process launch \
    --device "$DEVICE_UDID" \
    --terminate-existing \
    --console \
    --environment-variables '{"SIPSDK_UITEST_MODE":"device_verify","SIPSDK_DEVICE_CALL_URI":"sip:echo@iptel.org"}' \
    com.wiseland.SipSdk >"$CONSOLE_LOG" 2>&1
) &
LAUNCH_PID=$!

for _ in {1..45}; do
  if grep -q "DEVICE_LIVE_CALL_VERIFIED" "$CONSOLE_LOG" 2>/dev/null; then
    break
  fi
  sleep 0.5
done

kill "$LAUNCH_PID" >/dev/null 2>&1 || true
wait "$LAUNCH_PID" >/dev/null 2>&1 || true

cat "$CONSOLE_LOG"

grep -q "SIP video stack started (H264+avcapture+iosview=true)" "$CONSOLE_LOG"
grep -q "SIPSDK_VERIFY: REMOTE_FRAMES_OK" "$CONSOLE_LOG"
grep -q "SIPSDK_VERIFY: LOCAL_FRAMES_OK" "$CONSOLE_LOG"
grep -q "SIPSDK_VERIFY: CONTROLS_EXERCISED_OK" "$CONSOLE_LOG"
grep -q "SIPSDK_VERIFY: CALLKIT_READY_OK" "$CONSOLE_LOG"
grep -q "SIPSDK_VERIFY: H264_ROUNDTRIP_OK" "$CONSOLE_LOG"
grep -q "SIPSDK_VERIFY: DEVICE_LIVE_CALL_VERIFIED" "$CONSOLE_LOG"

echo "PHYSICAL_DEVICE_VIDEO_CALL_VERIFIED"
