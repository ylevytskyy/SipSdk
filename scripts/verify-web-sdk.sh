#!/usr/bin/env bash
# Verifies the web platform SDK (platforms/web):
#   1. SDK contract unit tests (Node, fake JsSIP).
#   2. Sample page in headless Chrome against a scripted SIP-over-WebSocket peer.
#   3. Optional, with --pbx: live call against the local Asterisk PBX (scripts/run-pbx.sh).
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

echo "================================================================="
echo "==> Verifying Web Platform SDK (JsSIP + WebRTC)"
echo "================================================================="

echo "==> 1. SDK unit tests..."
node --test platforms/web/test/sipsdk.test.mjs
echo "PASS unit tests"

echo "==> 2. Browser check (sample page + scripted SIP peer)..."
node platforms/web/test/verify-web-sdk.mjs
echo "PASS browser check"

if [[ "${1:-}" == "--pbx" ]]; then
  echo "==> 3. Live Asterisk check (1002 -> 9001 echo)..."
  node platforms/web/test/verify-web-sdk-pbx.mjs "${2:-127.0.0.1}"
  echo "PASS live Asterisk check"
fi

echo "==> Web Platform SDK verified."
