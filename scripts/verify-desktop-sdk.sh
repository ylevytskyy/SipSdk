#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

echo "================================================================="
echo "==> Verifying Desktop Native SDK (macOS & Linux Docker CI)"
echo "================================================================="

# 1. Ensure Asterisk PBX is running and healthy
echo "==> 1. Checking Asterisk PBX status..."
ASTERISK_STATUS="$(docker inspect --format='{{json .State.Health.Status}}' sipsdk-asterisk 2>/dev/null || echo '"not_running"')"
if [[ "$ASTERISK_STATUS" != '"healthy"' ]]; then
  echo "    Starting Asterisk PBX container..."
  bash "$ROOT_DIR/scripts/run-pbx.sh"
fi

# 2. Host Build & Verification (macOS)
echo "==> 2. Verifying Desktop targets on macOS host..."
BUILD_DIR="$ROOT_DIR/.build/desktop"
cmake -S "$ROOT_DIR/platforms/desktop" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" --parallel

echo "==> Running macOS desktop CTest suite..."
(cd "$BUILD_DIR" && ctest --output-on-failure)

echo "==> Running desktop sample application..."
"$BUILD_DIR/sample/desktop_sample" 1001 127.0.0.1 test1001

echo "==> Running sip_cli live registration & audio/video call to Asterisk Echo (9001)..."
"$BUILD_DIR/sip_cli" \
  --user 1003 \
  --domain 127.0.0.1 \
  --password test1003 \
  --port 5060 \
  --call sip:9001@127.0.0.1:5060 \
  --video \
  --duration 3 \
  --camera synthetic

echo "PASS: macOS Desktop host verifications passed cleanly"

# 3. Linux Container Verification (Docker)
echo "==> 3. Verifying Desktop targets inside Linux Docker container..."
DOCKER_IMAGE="sipsdk-desktop-test"
docker build -t "$DOCKER_IMAGE" -f "$ROOT_DIR/platforms/desktop/docker/Dockerfile.linux" "$ROOT_DIR"

echo "==> Running Linux containerized tests against Asterisk PBX..."
docker run --rm \
  --network sipsdk_default \
  -v "$ROOT_DIR:/workspace" \
  -e ASTERISK_HOST=asterisk \
  -e ASTERISK_PORT=5060 \
  "$DOCKER_IMAGE"

echo "PASS: Linux Docker container verifications passed cleanly"

echo "================================================================="
echo "ALL_DESKTOP_SDK_VERIFICATIONS_PASSED"
echo "================================================================="
