#!/usr/bin/env bash
set -euo pipefail

# Reproducible native engine build for Windows desktop (MSVC or MinGW cross-compiler).
# Pinned to upstream v4.12.0 releases by immutable commit SHA.
readonly BARESIP_VERSION="4.12.0"
readonly BARESIP_COMMIT="9cfab81550acc4bd38d6580d79b45cc6eea65cd6"
readonly RE_VERSION="4.12.0"
readonly RE_COMMIT="65555dad439de9d6a81b3ee6092b9c096585249b"
readonly OPUS_VERSION="1.5.2"
readonly OPUS_COMMIT="ddbe48383984d56acd9e1ab6a090c54ca6b735a6"

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PREBUILT_ROOT="${1:-$ROOT_DIR/platforms/windows/prebuilt}"
OUTPUT_DIR="$PREBUILT_ROOT/windows-x86_64"
WORK_ROOT="${RUNNER_TEMP:-${TMPDIR:-/tmp}}/sipsdk-windows-build-$$"
SOURCE_DIR="$WORK_ROOT/src"

die() {
  echo "ERROR: $*" >&2
  exit 1
}

command -v git >/dev/null || die "git is required"
command -v cmake >/dev/null || die "cmake is required"

# Detect toolchain: MSVC or MinGW
TOOLCHAIN_ARGS=()
AR_CMD="ar"
IS_MSVC=0

if [[ "${MSVC:-0}" == "1" ]] || command -v cl.exe >/dev/null 2>&1; then
  echo "==> Using MSVC toolchain for Windows build"
  IS_MSVC=1
  TOOLCHAIN_ARGS+=("-A" "x64")
elif command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then
  echo "==> Using MinGW-w64 toolchain (x86_64-w64-mingw32)"
  cat > "$WORK_ROOT-toolchain.cmake" <<'EOF'
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
EOF
  TOOLCHAIN_ARGS+=("-DCMAKE_TOOLCHAIN_FILE=$WORK_ROOT-toolchain.cmake")
  AR_CMD="x86_64-w64-mingw32-ar"
else
  die "Neither MSVC (cl.exe) nor x86_64-w64-mingw32-gcc found"
fi

OPENSSL_CMAKE_ARGS=()
if [[ -n "${OPENSSL_ROOT_DIR:-}" ]]; then
  OPENSSL_CMAKE_ARGS+=("-DOPENSSL_ROOT_DIR=$OPENSSL_ROOT_DIR")
fi

mkdir -p "$SOURCE_DIR" "$OUTPUT_DIR/lib" "$PREBUILT_ROOT/include"
trap 'rm -rf "$WORK_ROOT" "$WORK_ROOT-toolchain.cmake" 2>/dev/null || true' EXIT

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

echo "==> Fetching libre $RE_VERSION, Baresip $BARESIP_VERSION, and Opus $OPUS_VERSION for Windows..."
clone_pinned_tag "https://github.com/baresip/re.git" "v$RE_VERSION" "$RE_COMMIT" "$SOURCE_DIR/re"
clone_pinned_tag "https://github.com/baresip/baresip.git" "v$BARESIP_VERSION" "$BARESIP_COMMIT" "$SOURCE_DIR/baresip"
clone_pinned_tag "https://github.com/xiph/opus.git" "v$OPUS_VERSION" "$OPUS_COMMIT" "$SOURCE_DIR/opus"

# Avoid duplicate srtp.c.o archive member name between srtp and dtls_srtp static modules
if [[ -f "$SOURCE_DIR/baresip/modules/dtls_srtp/srtp.c" ]]; then
  mv "$SOURCE_DIR/baresip/modules/dtls_srtp/srtp.c" "$SOURCE_DIR/baresip/modules/dtls_srtp/dtls_srtp_stream.c"
  python3 -c '
from pathlib import Path
p = Path("'"$SOURCE_DIR"'/baresip/modules/dtls_srtp/CMakeLists.txt")
p.write_text(p.read_text().replace("dtls.c srtp.c", "dtls.c dtls_srtp_stream.c"))
'
fi

echo "==> Building static libopus for Windows..."
mkdir -p "$WORK_ROOT/opus-build" "$WORK_ROOT/prefix"
cmake -S "$SOURCE_DIR/opus" -B "$WORK_ROOT/opus-build" \
  "${TOOLCHAIN_ARGS[@]}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$WORK_ROOT/prefix" \
  -DOPUS_BUILD_SHARED_LIBRARY=OFF \
  -DOPUS_BUILD_TESTING=OFF \
  -DOPUS_BUILD_PROGRAMS=OFF \
  -DOPUS_ENABLE_FLOAT_API=ON
cmake --build "$WORK_ROOT/opus-build" --config Release --target opus --parallel
cmake --install "$WORK_ROOT/opus-build" --config Release

echo "==> Configuring libre for Windows with OpenSSL..."
mkdir -p "$WORK_ROOT/re-build"
cmake -S "$SOURCE_DIR/re" -B "$WORK_ROOT/re-build" \
  "${TOOLCHAIN_ARGS[@]}" \
  "${OPENSSL_CMAKE_ARGS[@]}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$WORK_ROOT/prefix" \
  -DCMAKE_PREFIX_PATH="$WORK_ROOT/prefix" \
  -DUSE_OPENSSL=ON \
  -DHAVE_RESOLV=OFF \
  -DUSE_UNIXSOCK=ON \
  -DUSE_REM=ON \
  -DUSE_SIP=ON \
  -DLIBRE_BUILD_STATIC=ON \
  -DLIBRE_BUILD_SHARED=OFF

echo "==> Building and installing libre..."
cmake --build "$WORK_ROOT/re-build" --config Release --target re --parallel
cmake --install "$WORK_ROOT/re-build" --config Release

# Windows modules
BARESIP_MODULES="g711;account;stun;turn;ice;srtp;dtls_srtp;opus"

if [[ "$IS_MSVC" -eq 1 ]]; then
  OPUS_LIB_FILE="$WORK_ROOT/prefix/lib/opus.lib"
  RE_LIB_FILE="$WORK_ROOT/prefix/lib/re.lib"
  EXTRA_C_FLAGS="-DUSE_TLS=1 -DUSE_DTLS=1 /I$WORK_ROOT/prefix/include /I$WORK_ROOT/prefix/include/opus"
else
  OPUS_LIB_FILE="$WORK_ROOT/prefix/lib/libopus.a"
  RE_LIB_FILE="$WORK_ROOT/prefix/lib/libre.a"
  EXTRA_C_FLAGS="-DUSE_TLS=1 -DUSE_DTLS=1 -I$WORK_ROOT/prefix/include -I$WORK_ROOT/prefix/include/opus"
fi

echo "==> Configuring Baresip for Windows with modules: $BARESIP_MODULES..."
mkdir -p "$WORK_ROOT/baresip-build"
cmake -S "$SOURCE_DIR/baresip" -B "$WORK_ROOT/baresip-build" \
  "${TOOLCHAIN_ARGS[@]}" \
  "${OPENSSL_CMAKE_ARGS[@]}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DUSE_OPENSSL=ON \
  -DOPUS_FOUND=TRUE \
  -DOPUS_INCLUDE_DIR="$WORK_ROOT/prefix/include/opus" \
  -DOPUS_INCLUDE_DIRS="$WORK_ROOT/prefix/include/opus" \
  -DOPUS_LIBRARY="$OPUS_LIB_FILE" \
  -DOPUS_LIBRARIES="$OPUS_LIB_FILE" \
  -DCMAKE_C_FLAGS="$EXTRA_C_FLAGS" \
  -DCMAKE_PREFIX_PATH="$WORK_ROOT/prefix" \
  -Dre_DIR="$WORK_ROOT/prefix/lib/cmake/re" \
  -DRE_INCLUDE_DIR="$WORK_ROOT/prefix/include/re" \
  -DRE_LIBRARY="$RE_LIB_FILE" \
  -DSTATIC=ON \
  "-DMODULES=$BARESIP_MODULES" \
  -DAPP_MODULES=

echo "==> Building Baresip..."
cmake --build "$WORK_ROOT/baresip-build" --config Release --target baresip --parallel

echo "==> Packaging static prebuilts..."
if [[ "$IS_MSVC" -eq 1 ]]; then
  BARESIP_LIB_FILE="$WORK_ROOT/baresip-build/Release/baresip.lib"
  [[ -f "$BARESIP_LIB_FILE" ]] || BARESIP_LIB_FILE="$WORK_ROOT/baresip-build/baresip.lib"
  cp "$RE_LIB_FILE" "$OUTPUT_DIR/lib/libre.lib"
  cp "$OPUS_LIB_FILE" "$OUTPUT_DIR/lib/libopus.lib"
  lib.exe /NOLOGO /OUT:"$OUTPUT_DIR/lib/libbaresip.lib" "$BARESIP_LIB_FILE" "$RE_LIB_FILE" "$OPUS_LIB_FILE"
else
  cp "$RE_LIB_FILE" "$OUTPUT_DIR/lib/"

  "$AR_CMD" -M <<MRI_EOF
create $OUTPUT_DIR/lib/libbaresip.a
addlib $WORK_ROOT/baresip-build/libbaresip.a
addlib $WORK_ROOT/prefix/lib/libre.a
addlib $WORK_ROOT/prefix/lib/libopus.a
save
end
MRI_EOF
fi

cp -R "$WORK_ROOT/prefix/include/re" "$PREBUILT_ROOT/include/"
cp -R "$SOURCE_DIR/baresip/include" "$PREBUILT_ROOT/include/baresip"
cp -f "$WORK_ROOT/prefix/include/re"/* "$PREBUILT_ROOT/include/" 2>/dev/null || true
cp -f "$SOURCE_DIR/baresip/include"/* "$PREBUILT_ROOT/include/" 2>/dev/null || true

echo "==> Windows Prebuilts successfully built:"
ls -lh "$OUTPUT_DIR/lib"
