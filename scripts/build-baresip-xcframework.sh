#!/usr/bin/env bash
set -euo pipefail

# Reproducible native engine build for Apple platforms.
# Versions are pinned to upstream v4.12.0 releases by immutable commit SHA.
readonly BARESIP_VERSION="4.12.0"
readonly BARESIP_COMMIT="9cfab81550acc4bd38d6580d79b45cc6eea65cd6"
readonly RE_VERSION="4.12.0"
readonly RE_COMMIT="65555dad439de9d6a81b3ee6092b9c096585249b"
readonly IOS_DEPLOYMENT_TARGET="12.1"
readonly MODULES=(g711 audiounit account stun turn ice srtp)

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUTPUT_DIR="${1:-$ROOT_DIR/.build/baresip-xcframework}"
WORK_ROOT="${RUNNER_TEMP:-${TMPDIR:-/tmp}}/sipsdk-baresip-build-$$"
SOURCE_DIR="$WORK_ROOT/src"
BUILD_DIR="$WORK_ROOT/build"

die() {
  echo "ERROR: $*" >&2
  exit 1
}

command -v git >/dev/null || die "git is required"
command -v cmake >/dev/null || die "cmake is required"
command -v xcrun >/dev/null || die "Xcode command line tools are required"
command -v xcodebuild >/dev/null || die "xcodebuild is required"

mkdir -p "$SOURCE_DIR" "$BUILD_DIR" "$OUTPUT_DIR"
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

echo "==> Fetching Baresip $BARESIP_VERSION and libre $RE_VERSION"
clone_pinned_tag \
  "https://github.com/baresip/re.git" "v$RE_VERSION" "$RE_COMMIT" "$SOURCE_DIR/re"
clone_pinned_tag \
  "https://github.com/baresip/baresip.git" "v$BARESIP_VERSION" "$BARESIP_COMMIT" "$SOURCE_DIR/baresip"

# Upstream's Apple CMake branch selects the Apple AES/HMAC implementations
# but omits tls/stub.c when OpenSSL is disabled. Baresip still references
# tls_add_certf during UA allocation, so include the no-TLS API stubs explicitly.
python3 - "$SOURCE_DIR/re/CMakeLists.txt" <<'PATCH_TLS_STUB'
from pathlib import Path
import sys

path = Path(sys.argv[1])
source = path.read_text()
needle = """elseif(APPLE)
  list(APPEND SRCS
    src/aes/apple/aes.c
    src/hmac/apple/hmac.c
  )
"""
replacement = """elseif(APPLE)
  list(APPEND SRCS
    src/aes/apple/aes.c
    src/hmac/apple/hmac.c
    src/tls/stub.c
  )
"""
if needle not in source:
    raise SystemExit("Pinned libre CMake Apple branch changed; review TLS stub patch")
path.write_text(source.replace(needle, replacement, 1))
PATCH_TLS_STUB

build_slice() {
  local sdk="$1"
  local arch="$2"
  local slice="$3"
  local sdk_path
  local prefix="$slice/prefix"
  local re_build="$slice/re-build"
  local baresip_build="$slice/baresip-build"

  mkdir -p "$slice" "$prefix"
  sdk_path="$(xcrun --sdk "$sdk" --show-sdk-path)"
  echo "==> Building for SDK=$sdk ARCH=$arch"

  cmake -S "$SOURCE_DIR/re" -B "$re_build" \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_SYSROOT="$sdk_path" \
    -DCMAKE_OSX_ARCHITECTURES="$arch" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$IOS_DEPLOYMENT_TARGET" \
    -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DUSE_OPENSSL=OFF \
    -DHAVE_RESOLV=OFF \
    -DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=TRUE \
    -DUSE_UNIXSOCK=OFF \
    -DUSE_TRACE=OFF \
    -DUSE_REM=ON \
    -DUSE_SIP=ON \
    -DUSE_RTMP=OFF \
    -DUSE_BFCP=OFF \
    -DUSE_PCP=OFF \
    -DLIBRE_BUILD_STATIC=ON \
    -DLIBRE_BUILD_SHARED=OFF
  cmake --build "$re_build" --target re --parallel
  cmake --install "$re_build"

  cmake -S "$SOURCE_DIR/baresip" -B "$baresip_build" \
    -DCMAKE_SYSTEM_NAME=iOS \
    -DCMAKE_OSX_SYSROOT="$sdk_path" \
    -DCMAKE_OSX_ARCHITECTURES="$arch" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$IOS_DEPLOYMENT_TARGET" \
    -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_MACOSX_BUNDLE=OFF \
    -DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=TRUE \
    -DCMAKE_PREFIX_PATH="$prefix" \
    -Dre_DIR="$prefix/lib/cmake/re" \
    -DRE_INCLUDE_DIR="$prefix/include/re" \
    -DRE_LIBRARY="$prefix/lib/libre.a" \
    -DSTATIC=ON \
    "-DMODULES=g711;audiounit;account;stun;turn;ice;srtp" \
    -DAPP_MODULES=
  cmake --build "$baresip_build" --target baresip --parallel

  # CMake may already add selected object-library modules to libbaresip.a.
  # Add only modules whose export symbol is not defined in the archive.
  local archive_symbols
  archive_symbols="$(xcrun nm -g "$baresip_build/libbaresip.a")"
  local -a missing_module_objects=()
  local has_missing_module_objects=0
  local module
  for module in "${MODULES[@]}"; do
    if grep -Eq "[[:space:]][A-TV-Z][[:space:]]_?exports_${module}$" <<< "$archive_symbols"; then
      continue
    fi

    local -a found=()
    while IFS= read -r -d '' object; do
      found+=("$object")
    done < <(
      find "$baresip_build" -type f \
        -path "*/CMakeFiles/$module.dir/*" -name '*.o' -print0
    )

    (("${#found[@]}" > 0)) || die "No compiled objects found for module '$module' ($sdk/$arch)"
    missing_module_objects+=("${found[@]}")
    has_missing_module_objects=1
  done

  [[ -f "$baresip_build/libbaresip.a" ]] || die "libbaresip.a was not produced"
  [[ -f "$prefix/lib/libre.a" ]] || die "libre.a was not installed"

  mkdir -p "$slice/output"
  if [[ "$has_missing_module_objects" -eq 1 ]]; then
    xcrun libtool -static -o "$slice/output/libBaresipCore.a" \
      "$baresip_build/libbaresip.a" "$prefix/lib/libre.a" "${missing_module_objects[@]}"
  else
    # Bash 3.2 treats expanding an empty array under set -u as an unbound variable.
    xcrun libtool -static -o "$slice/output/libBaresipCore.a" \
      "$baresip_build/libbaresip.a" "$prefix/lib/libre.a"
  fi

  xcrun lipo "$slice/output/libBaresipCore.a" -verify_arch "$arch"
}

rm -rf "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR"
build_slice iphoneos arm64 "$BUILD_DIR/iphoneos-arm64"
build_slice iphonesimulator arm64 "$BUILD_DIR/iphonesimulator-arm64"
build_slice iphonesimulator x86_64 "$BUILD_DIR/iphonesimulator-x86_64"

# Device and Simulator binaries stay separate. Merge only Simulator
# architectures, which share the same platform variant.
mkdir -p "$OUTPUT_DIR/headers" "$OUTPUT_DIR/simulator"
cp -R "$SOURCE_DIR/re/include/." "$OUTPUT_DIR/headers/"
cp "$SOURCE_DIR/baresip/include/baresip.h" "$OUTPUT_DIR/headers/baresip.h"
cat > "$OUTPUT_DIR/headers/BaresipCore.h" <<'HEADER'
#ifndef BARE_SIP_CORE_H
#define BARE_SIP_CORE_H
#include "re.h"
#include "baresip.h"
#endif
HEADER
cat > "$OUTPUT_DIR/headers/module.modulemap" <<'MODULEMAP'
module BaresipCore {
  umbrella header "BaresipCore.h"
  export *
  module * { export * }
}
MODULEMAP

xcrun lipo -create \
  "$BUILD_DIR/iphonesimulator-arm64/output/libBaresipCore.a" \
  "$BUILD_DIR/iphonesimulator-x86_64/output/libBaresipCore.a" \
  -output "$OUTPUT_DIR/simulator/libBaresipCore.a"

xcodebuild -create-xcframework \
  -library "$BUILD_DIR/iphoneos-arm64/output/libBaresipCore.a" \
  -headers "$OUTPUT_DIR/headers" \
  -library "$OUTPUT_DIR/simulator/libBaresipCore.a" \
  -headers "$OUTPUT_DIR/headers" \
  -output "$OUTPUT_DIR/BaresipCore.xcframework"

# Link the public startup/call APIs for both Apple platforms. This catches
# missing transitive system-framework dependencies before publishing an archive
# that compiles but cannot be consumed by an application.
cat > "$BUILD_DIR/link-smoke.c" <<'SMOKE'
#include <BaresipCore.h>

#define KEEP_SYMBOL(symbol) __typeof__(&symbol) volatile keep_##symbol = &symbol
KEEP_SYMBOL(libre_init);
KEEP_SYMBOL(libre_close);
KEEP_SYMBOL(conf_configure);
KEEP_SYMBOL(conf_modules);
KEEP_SYMBOL(conf_close);
KEEP_SYMBOL(baresip_init);
KEEP_SYMBOL(baresip_close);
KEEP_SYMBOL(ua_init);
KEEP_SYMBOL(ua_close);
KEEP_SYMBOL(ua_connect);
KEEP_SYMBOL(ua_answer);
KEEP_SYMBOL(ua_hold_answer);
KEEP_SYMBOL(bevent_register);
KEEP_SYMBOL(bevent_unregister);

int main(void)
{
    return keep_libre_init == 0;
}
SMOKE

link_smoke() {
  local sdk="$1"
  local target="$2"
  local library="$3"
  local sdk_path
  local label="$4"

  sdk_path="$(xcrun --sdk "$sdk" --show-sdk-path)"
  echo "==> Link smoke test for $label ($target)"
  xcrun --sdk "$sdk" clang \
    -target "$target" \
    -isysroot "$sdk_path" \
    -I "$OUTPUT_DIR/headers" \
    "$BUILD_DIR/link-smoke.c" \
    "$library" \
    -framework CoreAudio \
    -framework AudioToolbox \
    -framework SystemConfiguration \
    -framework CoreFoundation \
    -lz -lm \
    -o "$BUILD_DIR/link-smoke-$label"
}

link_smoke iphoneos "arm64-apple-ios$IOS_DEPLOYMENT_TARGET" \
  "$BUILD_DIR/iphoneos-arm64/output/libBaresipCore.a" device-arm64
link_smoke iphonesimulator "arm64-apple-ios$IOS_DEPLOYMENT_TARGET-simulator" \
  "$OUTPUT_DIR/simulator/libBaresipCore.a" simulator-arm64

cat > "$OUTPUT_DIR/build-metadata.txt" <<METADATA
Baresip version: $BARESIP_VERSION
Baresip commit: $BARESIP_COMMIT
libre version: $RE_VERSION
libre commit: $RE_COMMIT
iOS deployment target: $IOS_DEPLOYMENT_TARGET
Baresip modules: ${MODULES[*]}
Device architectures: arm64
Simulator architectures: arm64, x86_64
TLS: disabled (USE_OPENSSL=OFF); SIP UDP/TCP is available, SIP TLS needs a separate Apple-compatible TLS decision.
METADATA

(
  cd "$OUTPUT_DIR"
  ditto -c -k --sequesterRsrc --keepParent BaresipCore.xcframework BaresipCore.xcframework.zip
  shasum -a 256 BaresipCore.xcframework.zip > BaresipCore.xcframework.zip.sha256
)

echo "==> Produced:"
echo "    $OUTPUT_DIR/BaresipCore.xcframework"
echo "    $OUTPUT_DIR/BaresipCore.xcframework.zip"
echo "    $OUTPUT_DIR/BaresipCore.xcframework.zip.sha256"
echo "==> Simulator architectures:"
xcrun lipo -archs "$OUTPUT_DIR/simulator/libBaresipCore.a"
