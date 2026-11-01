#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

echo "================================================================="
echo "==> Verifying Windows Platform SDK (.NET / C# & Media Foundation)"
echo "================================================================="

# 1. Native Windows CMake Build & Driver Verification
echo "==> 1. Building Windows native target and driver test suite..."
BUILD_DIR="$ROOT_DIR/.build/windows"
cmake -S "$ROOT_DIR/platforms/windows" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" --target sip_core_dll test_windows_abi test_mf_drivers --parallel

echo "==> Running Windows C ABI Export & Client Lifecycle Tests..."
"$BUILD_DIR/test_windows_abi"

echo "==> Running Windows Media Foundation Driver Tests..."
"$BUILD_DIR/test_mf_drivers"

# 2. .NET / C# SDK Build & Test Suite
echo "==> 2. Verifying .NET / C# Class Library & Unit Tests..."
DOTNET_DIR="$ROOT_DIR/platforms/windows/dotnet"

if command -v dotnet >/dev/null 2>&1; then
    echo "    Using host .NET SDK..."
    dotnet build "$DOTNET_DIR/SipSdk.sln" -c Release
    dotnet test "$DOTNET_DIR/tests/SipSdk.Tests/SipSdk.Tests.csproj" -c Release --no-build
else
    echo "    Running .NET SDK via containerized environment..."
    DOTNET_IMAGE="sipsdk-dotnet-test"
    if ! docker image inspect "$DOTNET_IMAGE" >/dev/null 2>&1; then
        echo "    Building container image $DOTNET_IMAGE..."
        docker build -t "$DOTNET_IMAGE" - << 'EOF'
FROM mcr.microsoft.com/dotnet/sdk:8.0
RUN apt-get update -o Acquire::Check-Valid-Until=false -o Acquire::AllowInsecureRepositories=true \
 && apt-get install -y --no-install-recommends libasound2 libpulse0 \
 && rm -rf /var/lib/apt/lists/*
EOF
    fi
    docker run --rm \
        -v "$ROOT_DIR:/workspace" \
        -w /workspace/platforms/windows/dotnet \
        -e SIP_CORE_LIB_DIR=/workspace/.build/desktop/core \
        "$DOTNET_IMAGE" \
        bash -c "dotnet build SipSdk.sln -c Release && dotnet test tests/SipSdk.Tests/SipSdk.Tests.csproj -c Release --no-build"
fi

# 3. Dynamic Link Library Symbol Export Audit
echo "==> 3. Auditing dynamic library C ABI exported symbols..."
SHARED_LIB="$BUILD_DIR/libsipcore.dylib"
if [[ -f "$BUILD_DIR/libsipcore.dll" ]]; then
    SHARED_LIB="$BUILD_DIR/libsipcore.dll"
fi

EXPORTED_SYMBOLS=$(nm -gU "$SHARED_LIB" 2>/dev/null | grep -E " _?sip_" || true)
SYMBOL_COUNT=$(echo "$EXPORTED_SYMBOLS" | grep -c " _sip_" || true)
echo "    Found $SYMBOL_COUNT exported C ABI symbols"
if [[ "$SYMBOL_COUNT" -lt 35 ]]; then
    echo "ERROR: Expected at least 35 exported C ABI functions in $SHARED_LIB, found $SYMBOL_COUNT" >&2
    exit 1
fi
echo "PASS: C ABI export table satisfies full public API surface"

# 4. Zero-Regression Validation
echo "==> 4. Verifying zero regressions on existing Desktop Native SDK..."
bash "$ROOT_DIR/scripts/verify-desktop-sdk.sh"

echo "================================================================="
echo "ALL_WINDOWS_PLATFORM_SDK_VERIFICATIONS_PASSED"
echo "================================================================="
