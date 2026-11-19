#!/usr/bin/env bash
set -euo pipefail

# Reproducible native engine build for Android targets.
# Versions are pinned to upstream releases by immutable commit SHA.
readonly BARESIP_VERSION="4.12.0"
readonly BARESIP_COMMIT="9cfab81550acc4bd38d6580d79b45cc6eea65cd6"
readonly RE_VERSION="4.12.0"
readonly RE_COMMIT="65555dad439de9d6a81b3ee6092b9c096585249b"
readonly OPENSSL_VERSION="3.3.2"
readonly OPENSSL_TAG="openssl-3.3.2"
readonly OPENSSL_COMMIT="fb7fab9fa6f4869eaa8fbb97e0d593159f03ffe4"
readonly OPUS_VERSION="1.5.2"
readonly OPUS_COMMIT="ddbe48383984d56acd9e1ab6a090c54ca6b735a6"
readonly ANDROID_API="21"
readonly TARGET_ABIS=("arm64-v8a" "armeabi-v7a" "x86_64")
readonly MODULES=(g711 opensles account stun turn ice srtp dtls_srtp opus)

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUTPUT_DIR="${1:-$ROOT_DIR/platforms/android/sipsdk/prebuilt}"
WORK_ROOT="${RUNNER_TEMP:-${TMPDIR:-/tmp}}/sipsdk-android-build-$$"
SOURCE_DIR="$WORK_ROOT/src"

die() {
  echo "ERROR: $*" >&2
  exit 1
}

# Locate Android NDK
if [[ -z "${ANDROID_NDK_HOME:-}" ]]; then
  if [[ -n "${ANDROID_HOME:-}" && -d "$ANDROID_HOME/ndk" ]]; then
    # Pick newest installed NDK
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

[[ -n "${ANDROID_NDK_HOME:-}" && -f "$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" ]] || die \
  "Android NDK not found. Set ANDROID_NDK_HOME or install NDK under ANDROID_HOME."

TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake"
LLVM_AR="$(find "$ANDROID_NDK_HOME/toolchains/llvm/prebuilt" -name 'llvm-ar' | head -n 1)"
LLVM_NM="$(find "$ANDROID_NDK_HOME/toolchains/llvm/prebuilt" -name 'llvm-nm' | head -n 1)"
[[ -x "$LLVM_AR" && -x "$LLVM_NM" ]] || die "Could not locate llvm-ar/llvm-nm in $ANDROID_NDK_HOME"
NDK_BIN_DIR="$(dirname "$LLVM_AR")"

NPROC="$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)"

echo "==> Using Android NDK: $ANDROID_NDK_HOME"
echo "==> Using NDK LLVM bin: $NDK_BIN_DIR"

mkdir -p "$SOURCE_DIR" "$OUTPUT_DIR"
trap 'rm -rf "$WORK_ROOT"' EXIT

clone_pinned_tag() {
  local url="$1"
  local tag="$2"
  local expected_commit="$3"
  local destination="$4"

  git clone --quiet --depth 1 --branch "$tag" "$url" "$destination"
  local actual_commit
  actual_commit="$(git -C "$destination" rev-parse HEAD)"
  [[ "$actual_commit" == "$expected_commit" ]] || die \
    "$url tag $tag resolved to $actual_commit, expected pinned commit $expected_commit"
}

echo "==> Fetching libre $RE_VERSION, Baresip $BARESIP_VERSION, OpenSSL $OPENSSL_VERSION, and Opus $OPUS_VERSION..."
clone_pinned_tag "https://github.com/baresip/re.git" "v$RE_VERSION" "$RE_COMMIT" "$SOURCE_DIR/re"
clone_pinned_tag "https://github.com/baresip/baresip.git" "v$BARESIP_VERSION" "$BARESIP_COMMIT" "$SOURCE_DIR/baresip"
clone_pinned_tag "https://github.com/openssl/openssl.git" "$OPENSSL_TAG" "$OPENSSL_COMMIT" "$SOURCE_DIR/openssl"
clone_pinned_tag "https://github.com/xiph/opus.git" "v$OPUS_VERSION" "$OPUS_COMMIT" "$SOURCE_DIR/opus"

# Avoid CLOCK_MONOTONIC_RAW on Android: QEMU/HVF ARM64 guest kernels can exhibit multi-second backward/forward jumps across vCPU migrations.
if [[ -f "$SOURCE_DIR/re/src/tmr/tmr.c" ]]; then
  python3 -c '
from pathlib import Path
p = Path("'"$SOURCE_DIR"'/re/src/tmr/tmr.c")
text = p.read_text()
assert "#ifdef CLOCK_MONOTONIC_RAW" in text, "Expected CLOCK_MONOTONIC_RAW guard in tmr.c"
p.write_text(text.replace("#ifdef CLOCK_MONOTONIC_RAW", "#if defined(CLOCK_MONOTONIC_RAW) && !defined(__ANDROID__)"))
'
fi

# Avoid duplicate srtp.c.o archive member name between srtp and dtls_srtp static modules
if [[ -f "$SOURCE_DIR/baresip/modules/dtls_srtp/srtp.c" ]]; then
  mv "$SOURCE_DIR/baresip/modules/dtls_srtp/srtp.c" "$SOURCE_DIR/baresip/modules/dtls_srtp/dtls_srtp_stream.c"
  python3 -c '
from pathlib import Path
p = Path("'"$SOURCE_DIR"'/baresip/modules/dtls_srtp/CMakeLists.txt")
p.write_text(p.read_text().replace("dtls.c srtp.c", "dtls.c dtls_srtp_stream.c"))
'
fi

rm -rf "$OUTPUT_DIR/include"

build_slice() {
  local abi="$1"
  local slice_dir="$WORK_ROOT/build-$abi"
  local prefix_dir="$slice_dir/prefix"
  local ssl_target=""

  case "$abi" in
    arm64-v8a)   ssl_target="android-arm64" ;;
    armeabi-v7a) ssl_target="android-arm" ;;
    x86_64)      ssl_target="android-x86_64" ;;
    *) die "Unsupported ABI: $abi" ;;
  esac

  echo "==> Building Android slice: ABI=$abi API=$ANDROID_API ($ssl_target)..."
  mkdir -p "$slice_dir" "$prefix_dir"

  # 1. Build static OpenSSL 3.x
  local openssl_build="$slice_dir/openssl-build"
  mkdir -p "$openssl_build"
  (
    cd "$openssl_build"
    export ANDROID_NDK_ROOT="$ANDROID_NDK_HOME"
    export PATH="$NDK_BIN_DIR:$PATH"
    "$SOURCE_DIR/openssl/Configure" "$ssl_target" \
      "-D__ANDROID_API__=$ANDROID_API" \
      -fPIC \
      no-shared \
      no-tests \
      no-ui-console \
      no-apps \
      --prefix="$prefix_dir" \
      --openssldir="$prefix_dir/ssl" \
      --libdir=lib
    make -j"$NPROC" build_libs
    make install_dev
  )

  # 2. Build static libopus 1.5.2
  local opus_build="$slice_dir/opus-build"
  cmake -S "$SOURCE_DIR/opus" -B "$opus_build" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
    -DANDROID_ABI="$abi" \
    -DANDROID_PLATFORM="android-$ANDROID_API" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DCMAKE_INSTALL_PREFIX="$prefix_dir" \
    -DOPUS_BUILD_SHARED_LIBRARY=OFF \
    -DOPUS_BUILD_TESTING=OFF \
    -DOPUS_BUILD_PROGRAMS=OFF \
    -DOPUS_ENABLE_FLOAT_API=ON
  cmake --build "$opus_build" --target opus --parallel
  cmake --install "$opus_build"

  # 3. Build static libre with OpenSSL
  local re_build="$slice_dir/re-build"
  cmake -S "$SOURCE_DIR/re" -B "$re_build" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
    -DANDROID_ABI="$abi" \
    -DANDROID_PLATFORM="android-$ANDROID_API" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DCMAKE_INSTALL_PREFIX="$prefix_dir" \
    -DCMAKE_PREFIX_PATH="$prefix_dir" \
    -DUSE_OPENSSL=ON \
    -DOPENSSL_ROOT_DIR="$prefix_dir" \
    -DOPENSSL_INCLUDE_DIR="$prefix_dir/include" \
    -DOPENSSL_CRYPTO_LIBRARY="$prefix_dir/lib/libcrypto.a" \
    -DOPENSSL_SSL_LIBRARY="$prefix_dir/lib/libssl.a" \
    -DHAVE_RESOLV=OFF \
    -DUSE_UNIXSOCK=ON \
    -DUSE_REM=ON \
    -DUSE_SIP=ON \
    -DLIBRE_BUILD_STATIC=ON \
    -DLIBRE_BUILD_SHARED=OFF

  cmake --build "$re_build" --target re --parallel
  cmake --install "$re_build"

  # 4. Build static libbaresip with OpenSSL, SRTP, DTLS-SRTP, and Opus
  local baresip_build="$slice_dir/baresip-build"
  local modules_semi
  modules_semi="$(IFS=';'; echo "${MODULES[*]}")"
  cmake -S "$SOURCE_DIR/baresip" -B "$baresip_build" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
    -DANDROID_ABI="$abi" \
    -DANDROID_PLATFORM="android-$ANDROID_API" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DCMAKE_PREFIX_PATH="$prefix_dir" \
    -DUSE_OPENSSL=ON \
    -DOPENSSL_ROOT_DIR="$prefix_dir" \
    -DOPENSSL_INCLUDE_DIR="$prefix_dir/include" \
    -DOPENSSL_CRYPTO_LIBRARY="$prefix_dir/lib/libcrypto.a" \
    -DOPENSSL_SSL_LIBRARY="$prefix_dir/lib/libssl.a" \
    -DOPUS_FOUND=TRUE \
    -DOPUS_INCLUDE_DIR="$prefix_dir/include/opus" \
    -DOPUS_INCLUDE_DIRS="$prefix_dir/include/opus" \
    -DOPUS_LIBRARY="$prefix_dir/lib/libopus.a" \
    -DOPUS_LIBRARIES="$prefix_dir/lib/libopus.a" \
    -DCMAKE_C_FLAGS="-DUSE_TLS=1 -DUSE_DTLS=1 -I$prefix_dir/include -I$prefix_dir/include/opus" \
    -Dre_DIR="$prefix_dir/lib/cmake/re" \
    -DRE_INCLUDE_DIR="$prefix_dir/include/re" \
    -DRE_LIBRARY="$prefix_dir/lib/libre.a" \
    -DSTATIC=ON \
    "-DMODULES=$modules_semi" \
    -DAPP_MODULES=

  cmake --build "$baresip_build" --target baresip --parallel

  # Ensure every requested static module is archived inside libbaresip.a
  local archive_symbols
  archive_symbols="$("$LLVM_NM" -g --defined-only "$baresip_build/libbaresip.a")"
  local -a missing_module_objects=()
  local module
  for module in "${MODULES[@]}"; do
    if grep -Eq "[[:space:]][A-TV-Z][[:space:]]_?exports_${module}$" <<< "$archive_symbols"; then
      continue
    fi
    local found_any=0
    while IFS= read -r -d '' object; do
      missing_module_objects+=("$object")
      found_any=1
    done < <(find "$baresip_build" -type f -path "*/CMakeFiles/$module.dir/*" -name '*.o' -print0)
    [[ "$found_any" -eq 1 ]] || die "No compiled objects found for module '$module' ($abi)"
  done

  # Package combined static archives so downstream consumers get baresip + modules + opus + openssl
  local abi_out="$OUTPUT_DIR/$abi"
  mkdir -p "$abi_out/lib" "$OUTPUT_DIR/include"
  cp "$prefix_dir/lib/libre.a" "$abi_out/lib/"

  {
    echo "create $abi_out/lib/libbaresip.a"
    echo "addlib $baresip_build/libbaresip.a"
    if [[ "${#missing_module_objects[@]}" -gt 0 ]]; then
      for obj in "${missing_module_objects[@]}"; do
        echo "addmod $obj"
      done
    fi
    echo "addlib $prefix_dir/lib/libre.a"
    echo "addlib $prefix_dir/lib/libopus.a"
    echo "addlib $prefix_dir/lib/libssl.a"
    echo "addlib $prefix_dir/lib/libcrypto.a"
    echo "save"
    echo "end"
  } | "$LLVM_AR" -M

  # Copy headers cleanly once
  if [[ ! -d "$OUTPUT_DIR/include/re" ]]; then
    cp -R "$prefix_dir/include/re" "$OUTPUT_DIR/include/"
    cp -R "$SOURCE_DIR/baresip/include" "$OUTPUT_DIR/include/baresip"
    cp -R "$prefix_dir/include/openssl" "$OUTPUT_DIR/include/"
    cp -R "$prefix_dir/include/opus" "$OUTPUT_DIR/include/"
  fi

  echo "==> Done slice $abi"
}

for abi in "${TARGET_ABIS[@]}"; do
  build_slice "$abi"
done

echo "==> Android native prebuilt libraries successfully built at $OUTPUT_DIR:"
for abi in "${TARGET_ABIS[@]}"; do
  ls -lh "$OUTPUT_DIR/$abi/lib"
done
