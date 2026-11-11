#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

echo "================================================================="
echo "==> Verifying Native macOS GUI Sample (SwiftUI + Baresip Core)"
echo "================================================================="

# 1. Ensure Asterisk PBX is running and healthy
echo "==> 1. Checking Asterisk PBX status..."
ASTERISK_STATUS="$(docker inspect --format='{{json .State.Health.Status}}' sipsdk-asterisk 2>/dev/null || echo '"not_running"')"
if [[ "$ASTERISK_STATUS" != '"healthy"' ]]; then
  echo "    Starting Asterisk PBX container..."
  bash "$ROOT_DIR/scripts/run-pbx.sh"
fi

# 1b. Camera driver unit tests (no PBX or camera needed)
echo "==> 1b. Running macOS camera driver unit tests..."
bash "$ROOT_DIR/scripts/verify-macos-camera-driver.sh"

# 2. Build SipSdkMac.xcodeproj
echo "==> 2. Building SipSdkMac.xcodeproj..."
BUILD_DIR="$ROOT_DIR/.build/macos_gui"
mkdir -p "$BUILD_DIR"

xcodebuild \
  -project "$ROOT_DIR/platforms/macos/sample/SipSdkMac.xcodeproj" \
  -scheme SipSdkMac \
  -configuration Release \
  -derivedDataPath "$BUILD_DIR" \
  build

APP_PATH="$BUILD_DIR/Build/Products/Release/SipSdkMac.app"
EXECUTABLE="$APP_PATH/Contents/MacOS/SipSdkMac"

if [[ ! -x "$EXECUTABLE" ]]; then
  echo "ERROR: Executable not found at $EXECUTABLE" >&2
  exit 1
fi

echo "PASS: SipSdkMac.app built successfully"

# 3. Launch the GUI Application directly on Settings screen to verify Settings UI & Media Devices
echo "==> 3. Launching SipSdkMac.app on Settings screen..."
EVIDENCE_DIR="$ROOT_DIR/specs/macos-gui-sample/evidence"
mkdir -p "$EVIDENCE_DIR"

SETTINGS_LOG="$EVIDENCE_DIR/macos-gui-settings.log"
"$EXECUTABLE" --test-settings-screen > "$SETTINGS_LOG" 2>&1 &
SETTINGS_PID=$!

sleep 0.8
SETTINGS_SCREENSHOT="$EVIDENCE_DIR/macos-gui-settings.png"
screencapture -x -o "$SETTINGS_SCREENSHOT" || screencapture -x "$SETTINGS_SCREENSHOT"

wait "$SETTINGS_PID" || true

if ! grep -q "Settings screen automated test finished cleanly" "$SETTINGS_LOG"; then
  echo "ERROR: Settings screen launch failed" >&2
  cat "$SETTINGS_LOG" >&2
  exit 1
fi
echo "PASS: Settings screen launched and verified ($SETTINGS_SCREENSHOT)"

# 4. Launch the GUI Application with automated call to Asterisk Echo (9001)
echo "==> 4. Launching SipSdkMac.app on macOS host with automated echo call..."
APP_LOG="$EVIDENCE_DIR/macos-gui.log"

# Launch in background with output redirected to log
"$EXECUTABLE" --test-call 9001 > "$APP_LOG" 2>&1 &
APP_PID=$!

CLEANUP() {
  if kill -0 "$APP_PID" 2>/dev/null; then
    echo "==> Terminating SipSdkMac process ($APP_PID)..."
    kill -TERM "$APP_PID" 2>/dev/null || true
    sleep 1
    kill -9 "$APP_PID" 2>/dev/null || true
  fi
}
trap CLEANUP EXIT

echo "    Waiting for registration and active in-call video streaming..."
# Give it 3.5 seconds to register, connect call, and start rendering video
sleep 3.5

# Check that process is still running and hasn't crashed
if ! kill -0 "$APP_PID" 2>/dev/null; then
  echo "ERROR: SipSdkMac process terminated prematurely or crashed on launch" >&2
  cat "$APP_LOG" >&2
  exit 1
fi

echo "PASS: SipSdkMac process running healthy (PID: $APP_PID)"

# 5. Capture Window Screenshot of active in-call GUI
echo "==> 5. Capturing desktop in-call window screenshot evidence..."
SCREENSHOT_PATH="$EVIDENCE_DIR/macos-gui-launch.png"
screencapture -x -o "$SCREENSHOT_PATH" || screencapture -x "$SCREENSHOT_PATH"

if [[ ! -s "$SCREENSHOT_PATH" ]]; then
  echo "ERROR: Failed to capture screenshot evidence" >&2
  exit 1
fi

SCREENSHOT_SIZE="$(stat -f%z "$SCREENSHOT_PATH")"
echo "PASS: Captured screenshot to $SCREENSHOT_PATH ($SCREENSHOT_SIZE bytes)"

echo "    Waiting for automated call completion and clean exit..."
wait "$APP_PID" || true

# 6. Verify log milestones
echo "==> 6. Verifying telemetry and media log evidence..."
cat "$APP_LOG"

if ! grep -q "Registration state: 2 (200 200 OK)" "$APP_LOG"; then
  echo "ERROR: Missing registration success in log" >&2
  exit 1
fi

if ! grep -q "Call established" "$APP_LOG"; then
  echo "ERROR: Missing Call established in log" >&2
  exit 1
fi

if ! grep -q "Discovered audio inputs" "$APP_LOG"; then
  echo "ERROR: Missing audio input discovery in log" >&2
  exit 1
fi

if ! grep -q "Discovered audio outputs" "$APP_LOG"; then
  echo "ERROR: Missing audio output discovery in log" >&2
  exit 1
fi

if ! grep -q "Discovered cameras" "$APP_LOG"; then
  echo "ERROR: Missing camera discovery in log" >&2
  exit 1
fi

if ! grep -q "Discovered video displays" "$APP_LOG"; then
  echo "ERROR: Missing video display discovery in log" >&2
  exit 1
fi

if ! grep -q "In-call switching" "$APP_LOG"; then
  echo "ERROR: Missing in-call device switching in log" >&2
  exit 1
fi

if ! grep -q "Exiting automated test cleanly" "$APP_LOG"; then
  echo "ERROR: App did not complete automated test cycle" >&2
  exit 1
fi

echo "PASS: All registration, media device discovery/switching, and call signaling milestones verified"

echo "================================================================="
echo "ALL_MACOS_GUI_VERIFICATIONS_PASSED"
echo "================================================================="
