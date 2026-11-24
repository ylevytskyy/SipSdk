#!/usr/bin/env bash
# Run the SipSdkTests suite under AddressSanitizer on a booted iOS Simulator.
#   bash scripts/verify-ios-asan-tests.sh              # full suite under ASan
#   bash scripts/verify-ios-asan-tests.sh --self-test  # prove a planted overflow is caught
# Set SIM_UDID to pick a simulator; default is the first booted one.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

MODE="${1:-}"
SCRATCH="$ROOT_DIR/.build-asan"
TRIPLE="arm64-apple-ios17.0-simulator"

SIM_SDK_PATH="$(xcrun --sdk iphonesimulator --show-sdk-path)"
SIM_PLATFORM_PATH="$(xcrun --sdk iphonesimulator --show-sdk-platform-path)"
CLANG_RT_DIR="$(dirname "$(xcrun --find clang)")/../lib/clang"
ASAN_DYLIB="$(find "$CLANG_RT_DIR" -name libclang_rt.asan_iossim_dynamic.dylib | head -n 1)"
if [[ -z "$ASAN_DYLIB" ]]; then
  echo "ERROR: libclang_rt.asan_iossim_dynamic.dylib not found under $CLANG_RT_DIR" >&2
  exit 1
fi

SIM_UDID="${SIM_UDID:-$(xcrun simctl list devices booted | grep -Eo '[0-9A-F]{8}-([0-9A-F]{4}-){3}[0-9A-F]{12}' | head -n 1)}"
if [[ -z "$SIM_UDID" ]]; then
  echo "ERROR: No booted iOS Simulator found. Boot one first (xcrun simctl boot <UDID>)" >&2
  exit 1
fi

export SIMCTL_CHILD_DYLD_INSERT_LIBRARIES="$ASAN_DYLIB"
export SIMCTL_CHILD_ASAN_OPTIONS="halt_on_error=1:abort_on_error=0:detect_stack_use_after_return=1:strict_string_checks=1:verbosity=1"

# Runs the given command on the simulator, tees its output to a log and fails on any ASan report.
run_checked() {
  local log
  log="$(mktemp -t sipsdk-asan)"
  set +e
  xcrun simctl spawn "$SIM_UDID" "$@" 2>&1 | tee "$log"
  local status="${PIPESTATUS[0]}"
  set -e
  RUN_LOG="$log"
  RUN_STATUS="$status"
}

if [[ "$MODE" == "--self-test" ]]; then
  echo "==> Self-test: planted heap-buffer-overflow on simulator ($SIM_UDID)"
  WORK="$(mktemp -d -t sipsdk-asan-self)"
  cat > "$WORK/overflow.c" <<'EOF'
#include <stdlib.h>
#include <stdio.h>
int main(void) {
  volatile char *p = malloc(8);
  p[8] = 1; /* one byte past the end */
  printf("overflow not caught\n");
  free((void *)p);
  return 0;
}
EOF
  xcrun clang -target "$TRIPLE" -isysroot "$SIM_SDK_PATH" -fsanitize=address -g -O0 \
    -o "$WORK/overflow" "$WORK/overflow.c"
  run_checked "$WORK/overflow"
  if [[ "$RUN_STATUS" -ne 0 ]] && grep -q "ERROR: AddressSanitizer: heap-buffer-overflow" "$RUN_LOG"; then
    echo "ASAN_SELF_TEST_PASSED"
    exit 0
  fi
  echo "ERROR: planted overflow was not reported (exit $RUN_STATUS)" >&2
  exit 1
fi

echo "==> 1. Compiling SipSdk tests with AddressSanitizer for arm64 iOS Simulator"
swift build \
  --build-tests \
  --sanitize=address \
  --scratch-path "$SCRATCH" \
  --triple "$TRIPLE" \
  -Xcc -isysroot -Xcc "$SIM_SDK_PATH" \
  -Xcxx -isysroot -Xcxx "$SIM_SDK_PATH"

TEST_BUNDLE="$(find "$SCRATCH" -name "SipSdkTests.xctest" -type d | head -n 1)"
if [[ -z "$TEST_BUNDLE" ]]; then
  echo "ERROR: Could not locate built SipSdkTests.xctest under $SCRATCH" >&2
  exit 1
fi
if ! otool -L "$TEST_BUNDLE/SipSdkTests" | grep "libclang_rt.asan_iossim_dynamic" >/dev/null; then
  echo "ERROR: $TEST_BUNDLE is not instrumented with AddressSanitizer" >&2
  exit 1
fi

XCTEST_BIN="$SIM_PLATFORM_PATH/Developer/Library/Xcode/Agents/xctest"
echo "==> 2. Running SipSdkTests under AddressSanitizer on simulator ($SIM_UDID)"
run_checked "$XCTEST_BIN" "$TEST_BUNDLE"

if ! grep -q "AddressSanitizer Init done" "$RUN_LOG"; then
  echo "ERROR: no AddressSanitizer runtime output; the runtime was not loaded" >&2
  exit 1
fi
echo "ASAN_RUNTIME_LOADED"

if grep -q "ERROR: AddressSanitizer" "$RUN_LOG"; then
  echo "ERROR: AddressSanitizer reported a memory error (see above)" >&2
  exit 1
fi
if [[ "$RUN_STATUS" -ne 0 ]]; then
  echo "ERROR: SipSdkTests failed under AddressSanitizer (exit $RUN_STATUS)" >&2
  exit 1
fi

echo "ALL_IOS_ASAN_TESTS_PASSED"
