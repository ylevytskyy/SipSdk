#!/usr/bin/env bash
set -euo pipefail

# Automated verification harness for Android Platform SDK (sipsdk.aar)
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ANDROID_DIR="$ROOT_DIR/platforms/android"
PREBUILT_DIR="$ANDROID_DIR/sipsdk/prebuilt"
TARGET_SLICE="${1:-all}"

echo "==> Running Android Platform SDK Verification (Target: $TARGET_SLICE)"

# Locate Android NDK & SDK
if [[ -z "${ANDROID_NDK_HOME:-}" ]]; then
  if [[ -n "${ANDROID_HOME:-}" && -d "$ANDROID_HOME/ndk" ]]; then
    LATEST_NDK="$(find "$ANDROID_HOME/ndk" -mindepth 1 -maxdepth 1 -type d | sort -V | tail -n 1)"
    if [[ -n "$LATEST_NDK" ]]; then
      export ANDROID_NDK_HOME="$LATEST_NDK"
    fi
  elif [[ -d "$HOME/Library/Android/sdk/ndk" ]]; then
    LATEST_NDK="$(find "$HOME/Library/Android/sdk/ndk" -mindepth 1 -maxdepth 1 -type d | sort -V | tail -n 1)"
    if [[ -n "$LATEST_NDK" ]]; then
      export ANDROID_NDK_HOME="$LATEST_NDK"
    fi
  fi
fi

if [[ -z "${ANDROID_HOME:-}" && -d "$HOME/Library/Android/sdk" ]]; then
  export ANDROID_HOME="$HOME/Library/Android/sdk"
fi

[[ -n "${ANDROID_NDK_HOME:-}" ]] || { echo "ERROR: ANDROID_NDK_HOME not found" >&2; exit 1; }
[[ -n "${ANDROID_HOME:-}" ]] || { echo "ERROR: ANDROID_HOME not found" >&2; exit 1; }

LLVM_NM="$(find "$ANDROID_NDK_HOME/toolchains/llvm" -name "llvm-nm" | head -n 1)"
[[ -n "$LLVM_NM" && -x "$LLVM_NM" ]] || { echo "ERROR: llvm-nm not found in NDK" >&2; exit 1; }

ensure_pbx_running() {
  echo "==> Checking Asterisk PBX container health..."
  local status
  status="$(docker inspect --format='{{json .State.Health.Status}}' sipsdk-asterisk 2>/dev/null || echo '"not_running"')"
  if [[ "$status" != '"healthy"' ]]; then
    echo "    Asterisk container is not running or not healthy ($status). Starting..."
    bash "$ROOT_DIR/scripts/run-pbx.sh"
  fi
}

ensure_android_device_ready() {
  echo "==> Checking for connected Android device or emulator..."
  local device
  device="$(adb devices | grep -E '\b(device)\b' | head -n 1 | cut -f1 || true)"
  if [[ -z "$device" ]]; then
    echo "==> No running device found. Starting Android emulator (Android_16_36.1)..."
    "$ANDROID_HOME/emulator/emulator" -avd Android_16_36.1 -no-window -no-audio -no-boot-anim -gpu swiftshader_indirect >/dev/null 2>&1 &
    echo "==> Waiting for emulator boot completion..."
    adb wait-for-device shell 'while [[ "$(getprop sys.boot_completed)" != "1" ]]; do sleep 1; done'
    echo "PASS: Android emulator booted and ready"
  else
    echo "PASS: Using running Android device/emulator: $device"
  fi
}

# Step 1: Ensure Baresip prebuilts exist
for abi in arm64-v8a armeabi-v7a x86_64; do
  if [[ ! -f "$PREBUILT_DIR/$abi/lib/libbaresip.a" || ! -f "$PREBUILT_DIR/$abi/lib/libre.a" ]]; then
    echo "==> Prebuilts missing for $abi, running build-baresip-android.sh..."
    bash "$ROOT_DIR/scripts/build-baresip-android.sh"
    break
  fi
done

# Step 2: Slice 1 verification (Gradle AAR Build & Multi-Arch Packaging)
if [[ "$TARGET_SLICE" == "all" || "$TARGET_SLICE" == "--slice" && "${2:-}" == "1" || "$TARGET_SLICE" == "1" ]]; then
  echo "==> [Slice 1] Building sipsdk.aar with Gradle..."
  cd "$ANDROID_DIR"
  
  ./gradlew :sipsdk:assembleRelease

  AAR_PATH="$ANDROID_DIR/sipsdk/build/outputs/aar/sipsdk-release.aar"
  if [[ ! -f "$AAR_PATH" ]]; then
    echo "ERROR: Expected $AAR_PATH to exist" >&2
    exit 1
  fi
  echo "PASS: Generated $AAR_PATH ($(du -h "$AAR_PATH" | cut -f1))"

  # Unpack AAR and inspect architectures
  VERIFY_DIR="/tmp/sipsdk-aar-verify-$$"
  mkdir -p "$VERIFY_DIR"
  trap 'rm -rf "$VERIFY_DIR"' EXIT

  unzip -q "$AAR_PATH" -d "$VERIFY_DIR"

  for abi in arm64-v8a armeabi-v7a x86_64; do
    SO_FILE="$VERIFY_DIR/jni/$abi/libsipcore.so"
    if [[ ! -f "$SO_FILE" ]]; then
      echo "ERROR: Missing native shared library $SO_FILE in AAR" >&2
      exit 1
    fi
    echo "PASS: Found jni/$abi/libsipcore.so ($(du -h "$SO_FILE" | cut -f1))"

    # Dynamic symbol audit
    LEAKED="$("$LLVM_NM" -gD "$SO_FILE" | grep ' T ' | grep -v ' Java_com_sipsdk_' | grep -v ' sip_' | grep -v ' _sip_' | grep -v ' JNI_OnLoad' | grep -v ' JNI_OnUnload' || true)"
    if [[ -n "$LEAKED" ]]; then
      echo "ERROR: Leaked unexpected symbols in $abi libsipcore.so:" >&2
      echo "$LEAKED" >&2
      exit 1
    fi
    echo "PASS: Symbol audit clean for $abi (only JNI and sip_* symbols exported)"
  done

  if [[ ! -f "$VERIFY_DIR/classes.jar" ]]; then
    echo "ERROR: Missing classes.jar in AAR" >&2
    exit 1
  fi
  echo "PASS: classes.jar verified in AAR"
  echo "==> [Slice 1] ALL CHECKS PASSED!"
fi

# Step 3: Slice 2 verification (SIP Registration Tests)
if [[ "$TARGET_SLICE" == "all" || "$TARGET_SLICE" == "--slice" && "${2:-}" == "2" || "$TARGET_SLICE" == "2" ]]; then
  echo "==> [Slice 2] Running Kotlin SDK Registration Tests..."
  ensure_pbx_running
  ensure_android_device_ready

  cd "$ANDROID_DIR"
  ./gradlew :sipsdk:testReleaseUnitTest
  ./gradlew :sipsdk:connectedDebugAndroidTest -Pandroid.testInstrumentationRunnerArguments.class=com.sipsdk.SipClientRegistrationTest
  echo "==> [Slice 2] ALL CHECKS PASSED!"
fi

# Step 4: Slice 3 verification (Audio Calling Tests & Sample App Build)
if [[ "$TARGET_SLICE" == "all" || "$TARGET_SLICE" == "--slice" && "${2:-}" == "3" || "$TARGET_SLICE" == "3" ]]; then
  echo "==> [Slice 3] Running Kotlin SDK Audio Calling Tests & Sample App Build..."
  ensure_pbx_running
  ensure_android_device_ready

  cd "$ANDROID_DIR"
  ./gradlew :sipsdk:connectedDebugAndroidTest -Pandroid.testInstrumentationRunnerArguments.class=com.sipsdk.SipCallAudioTest
  ./gradlew :sample:assembleDebug
  echo "==> [Slice 3] ALL CHECKS PASSED!"
fi

# Step 5: Slice 4 verification (MediaCodec H.264 Codec Driver)
if [[ "$TARGET_SLICE" == "all" || "$TARGET_SLICE" == "--slice" && "${2:-}" == "4" || "$TARGET_SLICE" == "4" ]]; then
  echo "==> [Slice 4] Running MediaCodec H.264 Driver Tests..."
  ensure_pbx_running
  ensure_android_device_ready

  cd "$ANDROID_DIR"
  ./gradlew :sipsdk:connectedDebugAndroidTest -Pandroid.testInstrumentationRunnerArguments.class=com.sipsdk.SipMediaCodecTest
  echo "==> [Slice 4] ALL CHECKS PASSED!"
fi

# Step 6: Slice 5 verification (Camera2 Capture Driver & Dynamic Controls)
if [[ "$TARGET_SLICE" == "all" || "$TARGET_SLICE" == "--slice" && "${2:-}" == "5" || "$TARGET_SLICE" == "5" ]]; then
  echo "==> [Slice 5] Running Camera2 Capture Driver Tests..."
  ensure_android_device_ready

  cd "$ANDROID_DIR"
  ./gradlew :sipsdk:connectedDebugAndroidTest -Pandroid.testInstrumentationRunnerArguments.class=com.sipsdk.SipCameraCaptureTest
  echo "==> [Slice 5] ALL CHECKS PASSED!"
fi

# Step 7: Slice 6 verification (Two-Way Video Calling & SipSurfaceView)
if [[ "$TARGET_SLICE" == "all" || "$TARGET_SLICE" == "--slice" && "${2:-}" == "6" || "$TARGET_SLICE" == "6" ]]; then
  echo "==> [Slice 6] Running Two-Way Video Calling & SurfaceView Tests..."
  ensure_pbx_running
  ensure_android_device_ready

  cd "$ANDROID_DIR"
  ./gradlew :sipsdk:connectedDebugAndroidTest -Pandroid.testInstrumentationRunnerArguments.class=com.sipsdk.SipCallVideoTest
  ./gradlew :sample:assembleDebug
  echo "==> [Slice 6] ALL CHECKS PASSED!"
fi

echo "=================================================="
echo "==> ANDROID PLATFORM SDK VERIFICATION SUCCESSFUL <=="
echo "=================================================="
