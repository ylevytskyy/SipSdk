#!/usr/bin/env bash
set -euo pipefail

echo "================================================================="
echo "Running SipSdk Desktop Native Linux CI Test Suite"
echo "OS: $(cat /etc/os-release | grep PRETTY_NAME | cut -d= -f2 | tr -d '\"')"
echo "Arch: $(uname -m)"
echo "================================================================="

ROOT_DIR="/workspace"
cd "$ROOT_DIR"

# 1. Setup headless PulseAudio with dummy sink
pulseaudio --system -D --disallow-exit --disallow-module-loading=0 || pulseaudio -D --exit-idle-time=-1 --disallow-exit --disable-shm || true

# 2. Build Baresip Linux prebuilts if missing
ARCH="$(uname -m)"
case "$ARCH" in
  x86_64|amd64) ARCH="x86_64" ;;
  arm64|aarch64) ARCH="arm64" ;;
esac
LINUX_SLICE="linux-${ARCH}"

if [[ ! -f "$ROOT_DIR/platforms/desktop/prebuilt/$LINUX_SLICE/lib/libbaresip.a" ]]; then
  echo "==> Building Baresip desktop prebuilt for Linux ($LINUX_SLICE)..."
  bash "$ROOT_DIR/scripts/build-baresip-desktop.sh" "$ROOT_DIR/platforms/desktop/prebuilt"
fi

# 3. Configure and build desktop targets
echo "==> Building desktop CMake targets on Linux..."
BUILD_DIR="/tmp/desktop-linux-build"
rm -rf "$BUILD_DIR"
cmake -S "$ROOT_DIR/platforms/desktop" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" --parallel

# 4. Resolve Asterisk host to IP and run native CTest / unit tests
ASTERISK_HOST="${ASTERISK_HOST:-asterisk}"
ASTERISK_PORT="${ASTERISK_PORT:-5060}"

if [[ "$ASTERISK_HOST" != *.*.*.* ]]; then
  RESOLVED_IP="$(python3 -c "import socket; print(socket.gethostbyname('$ASTERISK_HOST'))" 2>/dev/null || echo "$ASTERISK_HOST")"
  echo "==> Resolved Asterisk host '$ASTERISK_HOST' to '$RESOLVED_IP'"
  export ASTERISK_HOST="$RESOLVED_IP"
fi

echo "==> Running desktop native unit tests on Linux..."
cd "$BUILD_DIR"
ctest --output-on-failure

# 5. Live PBX test against Asterisk
echo "==> Running headless CLI call against Asterisk ($ASTERISK_HOST:$ASTERISK_PORT)..."
"$BUILD_DIR/sip_cli" \
  --user 1003 \
  --domain "$ASTERISK_HOST" \
  --password test1003 \
  --port "$ASTERISK_PORT" \
  --call "sip:9001@${ASTERISK_HOST}:${ASTERISK_PORT}" \
  --duration 3 \
  --audio-driver dummy \
  --camera synthetic

echo "================================================================="
echo "ALL_LINUX_DESKTOP_TESTS_PASSED"
echo "================================================================="
