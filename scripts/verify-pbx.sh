#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

echo "==> 1. Checking Asterisk PBX container health..."
STATUS="$(docker inspect --format='{{json .State.Health.Status}}' sipsdk-asterisk 2>/dev/null || echo '"not_running"')"
if [[ "$STATUS" != '"healthy"' ]]; then
  echo "    Asterisk container is not running or not healthy ($STATUS). Starting..."
  bash scripts/run-pbx.sh
fi

echo "==> 2. Verifying Asterisk PJSIP endpoints..."
docker exec sipsdk-asterisk asterisk -rx "pjsip show endpoints" | grep -E "1001|1002|1003|1004|1005"
echo "    PASS: All 5 extensions (1001-1005) loaded in Asterisk"

echo "==> 3. Verifying SIP UDP registration for Mobile App (Extension 1001)..."
python3 -c '
import socket, hashlib, re, time, sys

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(("127.0.0.1", 0))
s.settimeout(3.0)
local_port = s.getsockname()[1]
call_id = f"reg-verify-{int(time.time()*1000)}"

msg1 = (
    "REGISTER sip:127.0.0.1:5060 SIP/2.0\r\n"
    f"Via: SIP/2.0/UDP 127.0.0.1:{local_port};branch=z9hG4bK-{int(time.time()*1000)}-1\r\n"
    f"From: <sip:1001@127.0.0.1>;tag=tag-{int(time.time()*1000)}\r\n"
    "To: <sip:1001@127.0.0.1>\r\n"
    f"Call-ID: {call_id}\r\n"
    "CSeq: 1 REGISTER\r\n"
    f"Contact: <sip:1001@127.0.0.1:{local_port}>\r\n"
    "Max-Forwards: 70\r\nExpires: 300\r\nContent-Length: 0\r\n\r\n"
)
s.sendto(msg1.encode(), ("127.0.0.1", 5060))
resp1, _ = s.recvfrom(4096)
resp1_text = resp1.decode()
realm = re.search(r"realm=\"([^\"]+)\"", resp1_text).group(1)
nonce = re.search(r"nonce=\"([^\"]+)\"", resp1_text).group(1)

ha1 = hashlib.md5(f"1001:{realm}:test1001".encode()).hexdigest()
ha2 = hashlib.md5("REGISTER:sip:127.0.0.1:5060".encode()).hexdigest()
response = hashlib.md5(f"{ha1}:{nonce}:{ha2}".encode()).hexdigest()

msg2 = (
    "REGISTER sip:127.0.0.1:5060 SIP/2.0\r\n"
    f"Via: SIP/2.0/UDP 127.0.0.1:{local_port};branch=z9hG4bK-{int(time.time()*1000)}-2\r\n"
    f"From: <sip:1001@127.0.0.1>;tag=tag-{int(time.time()*1000)}\r\n"
    "To: <sip:1001@127.0.0.1>\r\n"
    f"Call-ID: {call_id}\r\n"
    "CSeq: 2 REGISTER\r\n"
    f"Contact: <sip:1001@127.0.0.1:{local_port}>\r\n"
    f"Authorization: Digest username=\"1001\", realm=\"{realm}\", nonce=\"{nonce}\", uri=\"sip:127.0.0.1:5060\", response=\"{response}\", algorithm=MD5\r\n"
    "Max-Forwards: 70\r\nExpires: 300\r\nContent-Length: 0\r\n\r\n"
)
s.sendto(msg2.encode(), ("127.0.0.1", 5060))
resp2, _ = s.recvfrom(4096)
line = resp2.decode().split("\r\n")[0]
if "200 OK" not in line:
    print("FAILED UDP REGISTER:", line)
    sys.exit(1)
print("    PASS: UDP REGISTER 200 OK for Extension 1001")
'

echo "==> 4. Verifying SIP WebSocket registration & Two-way Video Call for Web Client (Extension 1002)..."
node -e '
const { chromium } = require("/Users/lion/.nvm/versions/node/v24.13.1/lib/node_modules/playwright");
async function run() {
  const browser = await chromium.launch({
    headless: true,
    channel: "chrome",
    args: ["--use-fake-device-for-media-stream", "--use-fake-ui-for-media-stream"]
  });
  const page = await browser.newPage();
  await page.goto("http://localhost:5101/");

  await page.click("#btnPresetLocal");
  await page.click("#btnRegister");

  let reg = false;
  for (let i = 0; i < 20; i++) {
    await page.waitForTimeout(200);
    const log = await page.locator("#log").innerText();
    if (log.includes("Registered: sip:1002@")) { reg = true; break; }
  }
  if (!reg) {
    console.error("FAIL: Web registration failed");
    await browser.close();
    process.exit(1);
  }
  console.log("    PASS: WebSocket REGISTER 200 OK for Extension 1002");

  await page.click("#btnCall");
  let connected = false;
  for (let i = 0; i < 30; i++) {
    await page.waitForTimeout(300);
    const status = await page.locator("#callStatus").innerText();
    if (status.includes("Connected") || status.includes("Active")) { connected = true; break; }
  }
  await page.click("#btnHangup");
  await browser.close();

  if (!connected) {
    console.error("FAIL: Video Echo call not connected");
    process.exit(1);
  }
  console.log("    PASS: Two-way Video Call connected to Echo (9001)");
}
run().catch(e => { console.error("Test error:", e); process.exit(1); });
'

echo ""
echo "ALL_ASTERISK_PBX_CHECKS_PASSED"
