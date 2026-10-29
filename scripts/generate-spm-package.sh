#!/usr/bin/env bash
set -euo pipefail

usage() {
  echo "Usage: $0 <release-version> (example: $0 4.12.0)" >&2
}

if [[ $# -ne 1 || -z "$1" ]]; then
  usage
  exit 2
fi

VERSION="$1"
if [[ ! "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
  echo "ERROR: version must use MAJOR.MINOR.PATCH format" >&2
  exit 2
fi

TAG="baresip-core-v$VERSION"
BASE_URL="https://github.com/ylevytskyy/SipSdk/releases/download/$TAG"
ZIP_NAME="BaresipCore.xcframework.zip"
CHECKSUM_NAME="$ZIP_NAME.sha256"
WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT

command -v curl >/dev/null || { echo "ERROR: curl is required" >&2; exit 1; }
command -v shasum >/dev/null || { echo "ERROR: shasum is required" >&2; exit 1; }
command -v unzip >/dev/null || { echo "ERROR: unzip is required" >&2; exit 1; }

echo "==> Fetching checksum for GitHub Release $TAG"
curl --fail --location --silent --show-error \
  "$BASE_URL/$CHECKSUM_NAME" -o "$WORK_DIR/$CHECKSUM_NAME"

EXPECTED_CHECKSUM="$(awk 'NR == 1 { print $1 }' "$WORK_DIR/$CHECKSUM_NAME" | tr '[:upper:]' '[:lower:]')"
if [[ ! "$EXPECTED_CHECKSUM" =~ ^[0-9a-f]{64}$ ]]; then
  echo "ERROR: release checksum sidecar does not contain a valid SHA-256 digest" >&2
  cat "$WORK_DIR/$CHECKSUM_NAME" >&2
  exit 1
fi

echo "==> Downloading release archive and independently verifying SHA-256"
curl --fail --location --silent --show-error \
  "$BASE_URL/$ZIP_NAME" -o "$WORK_DIR/$ZIP_NAME"

ACTUAL_CHECKSUM="$(shasum -a 256 "$WORK_DIR/$ZIP_NAME" | awk '{ print $1 }' | tr '[:upper:]' '[:lower:]')"
if [[ "$ACTUAL_CHECKSUM" != "$EXPECTED_CHECKSUM" ]]; then
  echo "ERROR: downloaded ZIP checksum mismatch" >&2
  echo "Expected: $EXPECTED_CHECKSUM" >&2
  echo "Actual:   $ACTUAL_CHECKSUM" >&2
  exit 1
fi

if ! unzip -Z1 "$WORK_DIR/$ZIP_NAME" | grep -q '^BaresipCore\.xcframework/'; then
  echo "ERROR: release ZIP does not contain BaresipCore.xcframework/" >&2
  exit 1
fi

REPOSITORY_ROOT="$(cd "$(dirname "\${BASH_SOURCE[0]}")/.." && pwd)"
cat > "$REPOSITORY_ROOT/Package.swift" <<MANIFEST
// swift-tools-version: 5.9
import PackageDescription

let package = Package(
    name: "SipSdk",
    platforms: [
        .iOS("12.1")
    ],
    products: [
        .library(name: "SipSdk", targets: ["SipSdk"])
    ],
    targets: [
        .binaryTarget(
            name: "BaresipCore",
            url: "$BASE_URL/$ZIP_NAME",
            checksum: "$EXPECTED_CHECKSUM"
        ),
        .target(
            name: "SipSdk",
            dependencies: ["BaresipCore"],
            path: "Sources/SipSdk",
            publicHeadersPath: "include",
            linkerSettings: [
                .linkedFramework("Foundation"),
                .linkedFramework("CoreFoundation"),
                .linkedFramework("SystemConfiguration"),
                .linkedFramework("CoreAudio"),
                .linkedFramework("AudioToolbox"),
                .linkedFramework("AVFoundation"),
                .linkedFramework("CFNetwork"),
                .linkedFramework("CoreMedia"),
                .linkedLibrary("z"),
                .linkedLibrary("m")
            ]
        )
    ],
    cxxLanguageStandard: .cxx17
)
MANIFEST

echo "==> Wrote $REPOSITORY_ROOT/Package.swift"
echo "    Release:  $TAG"
echo "    Checksum: $EXPECTED_CHECKSUM"
echo "Review the generated manifest and run: swift package resolve"
