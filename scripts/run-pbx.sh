#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

echo "==> Detecting host network IP for NAT traversal..."
HOST_IP="$(ipconfig getifaddr en0 2>/dev/null || ipconfig getifaddr en1 2>/dev/null || ipconfig getifaddr en15 2>/dev/null || true)"
if [[ -z "$HOST_IP" ]]; then
  HOST_IP="127.0.0.1"
  echo "    Warning: Could not detect LAN IP on en0/en1. Defaulting to $HOST_IP"
else
  echo "    Detected LAN IP: $HOST_IP"
fi

export EXTERNAL_IP="$HOST_IP"

echo "==> Starting Asterisk PBX container (docker compose up -d --build)..."
docker compose up -d --build

echo "==> Waiting for Asterisk PBX healthcheck..."
for i in {1..30}; do
  STATUS="$(docker inspect --format='{{json .State.Health.Status}}' sipsdk-asterisk 2>/dev/null || echo '"starting"')"
  if [[ "$STATUS" == '"healthy"' ]]; then
    echo "    Asterisk PBX is healthy and ready!"
    break
  fi
  sleep 1
done

echo ""
echo "================================================================================"
echo "                   LOCAL ASTERISK VIDEO PBX IS RUNNING                          "
echo "================================================================================"
echo "Host LAN IP: $HOST_IP"
echo "SIP UDP/TCP Port: 5060"
echo "WebSocket Port:   8088 (ws://) and 8089 (wss://)"
echo "RTP Media Ports:  10000-10099 UDP"
echo ""
echo "PRE-CONFIGURED EXTENSIONS:"
echo "--------------------------------------------------------------------------------"
echo "1. Mobile App (SipSdk iOS on Simulator or Physical iPhone):"
echo "   - Username: 1001"
echo "   - Password: test1001"
echo "   - Domain:   $HOST_IP (or 127.0.0.1 on Simulator)"
echo "   - Port:     5060 (UDP or TCP)"
echo "   - Codecs:   H.264 Video + Opus/PCMU/PCMA Audio"
echo ""
echo "2. Web Test Client (web/index.html):"
echo "   - Open http://localhost:5101/ in your browser"
echo "   - Click preset button: 'Local Asterisk (1002)'"
echo "   - Click 'Register'"
echo "   - (Configured: URI 1002@$HOST_IP, Secret test1002, WS ws://$HOST_IP:8088/ws)"
echo ""
echo "3. Linphone Softphone:"
echo "   - Username: 1003"
echo "   - Password: test1003"
echo "   - Domain:   $HOST_IP:5060 (Transport: UDP or TCP)"
echo ""
echo "4. Jami Softphone:"
echo "   - Username: 1004"
echo "   - Password: test1004"
echo "   - Domain:   $HOST_IP:5060 (Transport: UDP or TCP)"
echo ""
echo "5. Extra Softphone (MicroSIP / Zoiper / Bria / 2nd device):"
echo "   - Username: 1005"
echo "   - Password: test1005"
echo "   - Domain:   $HOST_IP:5060"
echo ""
echo "TEST NUMBERS (DIAL FROM ANY CLIENT):"
echo "--------------------------------------------------------------------------------"
echo " - 9001 or 'echo':  Two-way Audio + H.264 Video Echo test (reflects video back!)"
echo " - 9002 or 'music': Music on Hold service"
echo " - Dial any extension (1001, 1002, 1003, 1004, 1005) to call between devices!"
echo "================================================================================"
echo "To view live logs: docker logs -f sipsdk-asterisk"
echo "To stop PBX:       bash scripts/stop-pbx.sh (or docker compose down)"
echo "================================================================================"
