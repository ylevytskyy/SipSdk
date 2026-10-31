// Verifies the test page can RECEIVE a call end-to-end without real credentials:
// a minimal SIP-over-WebSocket server accepts the page's REGISTER, then sends an
// INVITE; Playwright answers and we drive the call through ACK and BYE.
// Usage: node specs/web-test-page/verify-incoming.mjs
import { createRequire } from 'module';
import http from 'http';
import crypto from 'crypto';
import fs from 'fs';
import path from 'path';
import { fileURLToPath } from 'url';

const require = createRequire(import.meta.url);
const { chromium } = require('/Users/lion/.nvm/versions/node/v24.13.1/lib/node_modules/playwright');

const ROOT = path.dirname(fileURLToPath(import.meta.url));
const WEB_DIR = path.resolve(ROOT, '../../web');
const AOR = 'sip:page-user@iptel.org';
const CALLER = 'sip:caller@fake.invalid';
const TIMEOUT_MS = 30000;

// ---------- minimal WebSocket server (RFC 6455, text frames only) ----------

function acceptKey(key) {
  return crypto.createHash('sha1')
    .update(key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11')
    .digest('base64');
}

function encodeFrame(opcode, payload) {
  const data = Buffer.isBuffer(payload) ? payload : Buffer.from(payload);
  const len = data.length;
  let header;
  if (len < 126) {
    header = Buffer.from([0x80 | opcode, len]);
  } else if (len < 65536) {
    header = Buffer.alloc(4);
    header[0] = 0x80 | opcode; header[1] = 126; header.writeUInt16BE(len, 2);
  } else {
    header = Buffer.alloc(10);
    header[0] = 0x80 | opcode; header[1] = 127; header.writeBigUInt64BE(BigInt(len), 2);
  }
  return Buffer.concat([header, data]);
}

class WsConn {
  constructor(socket, onText) {
    this.socket = socket;
    this.onText = onText;
    this.buf = Buffer.alloc(0);
    this.fragments = [];
    socket.on('data', (d) => this.feed(d));
    socket.on('error', () => {});
  }

  feed(data) {
    this.buf = Buffer.concat([this.buf, data]);
    for (;;) {
      if (this.buf.length < 2) return;
      const fin = (this.buf[0] & 0x80) !== 0;
      const op = this.buf[0] & 0x0f;
      const masked = (this.buf[1] & 0x80) !== 0;
      let len = this.buf[1] & 0x7f;
      let off = 2;
      if (len === 126) {
        if (this.buf.length < 4) return;
        len = this.buf.readUInt16BE(2); off = 4;
      } else if (len === 127) {
        if (this.buf.length < 10) return;
        len = Number(this.buf.readBigUInt64BE(2)); off = 10;
      }
      const maskOff = off;
      if (masked) off += 4;
      if (this.buf.length < off + len) return;
      let payload = this.buf.slice(off, off + len);
      if (masked) {
        const mask = this.buf.slice(maskOff, maskOff + 4);
        payload = Buffer.from(payload);
        for (let i = 0; i < payload.length; i++) payload[i] ^= mask[i % 4];
      }
      this.buf = this.buf.slice(off + len);
      if (op === 0x8) { this.socket.end(encodeFrame(0x8, Buffer.alloc(0))); return; }
      if (op === 0x9) { this.socket.write(encodeFrame(0xA, payload)); continue; }
      if (op === 0x1 || op === 0x0) {
        this.fragments.push(payload);
        if (fin) {
          this.onText(Buffer.concat(this.fragments).toString('utf8'));
          this.fragments = [];
        }
      }
    }
  }

  send(text) { this.socket.write(encodeFrame(0x1, text)); }
}

// ---------- SIP helpers ----------

function parseSip(raw) {
  const sep = raw.indexOf('\r\n\r\n');
  const head = sep === -1 ? raw : raw.slice(0, sep);
  const body = sep === -1 ? '' : raw.slice(sep + 4);
  const lines = head.split('\r\n');
  const headers = {};
  for (const line of lines.slice(1)) {
    const i = line.indexOf(':');
    if (i < 0) continue;
    const k = line.slice(0, i).trim().toLowerCase();
    (headers[k] = headers[k] || []).push(line.slice(i + 1).trim());
  }
  return { startLine: lines[0], headers, body };
}

const hdr = (m, k) => (m.headers[k] ? m.headers[k][0] : undefined);
const tagOf = (m, k) => { const v = hdr(m, k) || ''; const t = /;tag=([^;\s>]+)/.exec(v); return t ? t[1] : undefined; };

function sipResponse(req, code, reason, extra = [], body = '') {
  const to = hdr(req, 'to');
  const lines = [
    `SIP/2.0 ${code} ${reason}`,
    `Via: ${hdr(req, 'via')}`,
    `From: ${hdr(req, 'from')}`,
    `To: ${/;tag=/.test(to) ? to : `${to};tag=srv${Date.now()}`}`,
    `Call-ID: ${hdr(req, 'call-id')}`,
    `CSeq: ${hdr(req, 'cseq')}`,
    ...extra,
    `Content-Length: ${Buffer.byteLength(body)}`,
    '',
    body,
  ];
  return lines.join('\r\n');
}

const OFFER_SDP = [
  'v=0',
  'o=fakecaller 1 1 IN IP4 203.0.113.10',
  's=fake-call',
  'c=IN IP4 203.0.113.10',
  't=0 0',
  'm=audio 40000 UDP/TLS/RTP/SAVPF 0 8 101',
  'a=rtpmap:0 PCMU/8000',
  'a=rtpmap:8 PCMA/8000',
  'a=rtpmap:101 telephone-event/8000',
  'a=fmtp:101 0-16',
  'a=sendrecv',
  'a=rtcp-mux',
  'a=ice-ufrag:fakeufrag0001',
  'a=ice-pwd:fakeicepassword00000001',
  'a=fingerprint:sha-256 00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00',
  'a=setup:actpass',
  'a=mid:0',
].join('\r\n') + '\r\n';

// ---------- test driver ----------

const events = [];
const note = (s) => { events.push(s); console.log(`  [sip-server] ${s}`); };

async function main() {
  let fail = 0;
  const expect = (name, ok, detail = '') => {
    console.log(`${ok ? 'PASS' : 'FAIL'} ${name}${detail ? ' — ' + detail : ''}`);
    if (!ok) fail++;
  };

  // Static file server for web/ (page + vendored jssip).
  const pageServer = http.createServer((req, res) => {
    const p = req.url === '/' ? '/index.html' : req.url.split('?')[0];
    const file = path.join(WEB_DIR, path.normalize(p));
    if (!file.startsWith(WEB_DIR) || !fs.existsSync(file) || !fs.statSync(file).isFile()) {
      res.writeHead(404); res.end('not found'); return;
    }
    const type = file.endsWith('.js') ? 'text/javascript' : file.endsWith('.html') ? 'text/html' : 'application/octet-stream';
    res.writeHead(200, { 'Content-Type': type });
    fs.createReadStream(file).pipe(res);
  });
  await new Promise((r) => pageServer.listen(0, '127.0.0.1', r));
  const pagePort = pageServer.address().port;

  // SIP-over-WebSocket server.
  let ws = null;
  let registeredContact = null;
  let cur = null; // current inbound call we placed: { state, callId, branch, fromTag, toTag, contact, waiters }
  const sipServer = http.createServer((req, res) => { res.writeHead(404); res.end(); });
  sipServer.on('upgrade', (req, socket) => {
    const key = req.headers['sec-websocket-key'];
    const proto = (req.headers['sec-websocket-protocol'] || '').includes('sip') ? 'sip' : null;
    if (!proto) { socket.destroy(); return; }
    socket.write(
      'HTTP/1.1 101 Switching Protocols\r\n' +
      'Upgrade: websocket\r\n' +
      'Connection: Upgrade\r\n' +
      `Sec-WebSocket-Accept: ${acceptKey(key)}\r\n` +
      'Sec-WebSocket-Protocol: sip\r\n\r\n'
    );
    socket.setNoDelay(true);
    ws = new WsConn(socket, onSipMessage);
    note('WebSocket upgrade (subprotocol sip)');
  });
  await new Promise((r) => sipServer.listen(0, '127.0.0.1', r));
  const wsPort = sipServer.address().port;

  const waitFor = (pred) =>
    new Promise((res) => cur.waiters.push({ pred, res }));
  const withTimeout = (p, ms = TIMEOUT_MS) =>
    Promise.race([p.then((m) => m.startLine), new Promise((r) => setTimeout(() => r(false), ms))]);

  function sendInvite() {
    const ts = Date.now();
    cur = {
      state: 'invited',
      callId: `call-${ts}@fake.invalid`,
      branch: `z9hG4bKinv${ts}`,
      fromTag: `caller${ts}`,
      toTag: null,
      contact: null,
      waiters: [],
    };
    const msg = [
      `INVITE ${registeredContact} SIP/2.0`,
      `Via: SIP/2.0/WS fake.invalid;branch=${cur.branch}`,
      'Max-Forwards: 70',
      `From: "Test Caller" <${CALLER}>;tag=${cur.fromTag}`,
      `To: <${AOR}>`,
      `Call-ID: ${cur.callId}`,
      'CSeq: 1 INVITE',
      `Contact: <${CALLER};transport=ws>`,
      'Content-Type: application/sdp',
      `Content-Length: ${Buffer.byteLength(OFFER_SDP)}`,
      '',
      OFFER_SDP,
    ].join('\r\n');
    note(`INVITE sent -> ${registeredContact}`);
    ws.send(msg);
  }

  function sendCancel() {
    const msg = [
      `CANCEL ${registeredContact} SIP/2.0`,
      `Via: SIP/2.0/WS fake.invalid;branch=${cur.branch}`,
      'Max-Forwards: 70',
      `From: "Test Caller" <${CALLER}>;tag=${cur.fromTag}`,
      `To: <${AOR}>`,
      `Call-ID: ${cur.callId}`,
      'CSeq: 1 CANCEL',
      'Content-Length: 0',
      '',
      '',
    ].join('\r\n');
    note('CANCEL sent');
    ws.send(msg);
  }

  function sendAck() {
    const msg = [
      `ACK ${cur.contact} SIP/2.0`,
      `Via: SIP/2.0/WS fake.invalid;branch=z9hG4bKack${Date.now()}`,
      'Max-Forwards: 70',
      `From: "Test Caller" <${CALLER}>;tag=${cur.fromTag}`,
      `To: <${AOR}>;tag=${cur.toTag}`,
      `Call-ID: ${cur.callId}`,
      'CSeq: 1 ACK',
      'Content-Length: 0',
      '',
      '',
    ].join('\r\n');
    ws.send(msg);
    cur.state = 'confirmed';
    note('ACK sent (call established at SIP level)');
  }

  function sendBye() {
    const msg = [
      `BYE ${cur.contact} SIP/2.0`,
      `Via: SIP/2.0/WS fake.invalid;branch=z9hG4bKbye${Date.now()}`,
      'Max-Forwards: 70',
      `From: "Test Caller" <${CALLER}>;tag=${cur.fromTag}`,
      `To: <${AOR}>;tag=${cur.toTag}`,
      `Call-ID: ${cur.callId}`,
      'CSeq: 2 BYE',
      'Content-Length: 0',
      '',
      '',
    ].join('\r\n');
    note('BYE sent');
    ws.send(msg);
  }

  function onSipMessage(raw) {
    const m = parseSip(raw);
    if (!m.startLine) return;

    if (m.startLine.startsWith('REGISTER')) {
      const contact = hdr(m, 'contact') || '';
      const uriMatch = /<([^>]+)>/.exec(contact);
      registeredContact = uriMatch ? uriMatch[1] : contact;
      const resp = sipResponse(m, 200, 'OK', [`Contact: <${registeredContact}>;expires=600`]);
      note(`REGISTER -> 200 OK (contact ${registeredContact})`);
      ws.send(resp);
      return;
    }

    if (m.startLine.startsWith('OPTIONS')) {
      ws.send(sipResponse(m, 200, 'OK'));
      return;
    }

    if (m.startLine.startsWith('ACK')) {
      note('ACK received');
      return;
    }

    if (/^SIP\/2\.0/.test(m.startLine)) {
      const cseq = hdr(m, 'cseq') || '';
      const code = /^SIP\/2\.0 (\d\d\d)/.exec(m.startLine)[1];
      note(`${m.startLine}${m.body ? ` (+${m.body.length}b body)` : ''}`);
      if (cseq.endsWith('INVITE')) {
        if (code === '180' || /^18\d/.test(code)) {
          if (cur.state === 'invited') cur.state = 'ringing';
        } else if (code.startsWith('2')) {
          if (cur.state !== 'answered') {
            cur.state = 'answered';
            cur.toTag = tagOf(m, 'to');
            const c = hdr(m, 'contact') || '';
            const cm = /<([^>]+)>/.exec(c);
            cur.contact = cm ? cm[1] : registeredContact;
            note(`To tag ${cur.toTag}, Contact ${cur.contact}`);
          }
        } else if (/^[3-6]/.test(code)) {
          cur.state = 'rejected';
        }
      } else if (cseq.endsWith('BYE')) {
        if (code.startsWith('2')) cur.state = 'bye-done';
      }
      for (const w of cur.waiters) {
        if (!w.done && w.pred(m)) { w.done = true; w.res(m); }
      }
      return;
    }

    note(`other: ${m.startLine}`);
  }

  // Browser drive.
  let browser;
  try {
    try {
      browser = await chromium.launch({
        headless: true,
        args: [
          '--use-fake-device-for-media-stream',
          '--use-fake-ui-for-media-stream',
          '--autoplay-policy=no-user-gesture-required',
        ],
      });
    } catch {
      browser = await chromium.launch({
        headless: true, channel: 'chrome',
        args: [
          '--use-fake-device-for-media-stream',
          '--use-fake-ui-for-media-stream',
          '--autoplay-policy=no-user-gesture-required',
        ],
      });
    }
    const page = await browser.newPage();
    const consoleErrors = [];
    page.on('console', (m2) => { if (m2.type() === 'error') consoleErrors.push(m2.text()); });
    page.on('pageerror', (e) => consoleErrors.push(`pageerror: ${e.message}`));

    await page.goto(`http://127.0.0.1:${pagePort}/`, { waitUntil: 'networkidle', timeout: TIMEOUT_MS });

    await page.fill('#sipUri', AOR.replace(/^sip:/, ''));
    await page.fill('#sipPass', 'test-password');
    await page.fill('#wsUri', `ws://127.0.0.1:${wsPort}`);
    await page.click('#btnRegister');

    // Wait for REGISTER to reach our server and the 200 OK to land in the page.
    const deadline = Date.now() + TIMEOUT_MS;
    let logText = '';
    while (Date.now() < deadline) {
      logText = await page.locator('#log').innerText();
      if (/Registered:|Registration failed/.test(logText)) break;
      await page.waitForTimeout(250);
    }
    expect('page registered against fake SIP server', /Registered:/.test(logText),
      logText.split('\n').slice(-3).join(' | '));

    // === Scenario A: inbound call answered ===
    sendInvite();
    const pRing = waitFor((m) => /^SIP\/2\.0 18\d/.test(m.startLine));
    const pOk = waitFor((m) => /^SIP\/2\.0 2\d\d/.test(m.startLine) && (hdr(m, 'cseq') || '').endsWith('INVITE'));

    const incomingVisible = await page.locator('#incoming')
      .waitFor({ state: 'visible', timeout: TIMEOUT_MS }).then(() => true).catch(() => false);
    expect('A: incoming-call UI shown', incomingVisible);
    const fromText = await page.locator('#incomingFrom').innerText().catch(() => '');
    expect('A: caller shown in UI', fromText.includes(CALLER), fromText);
    expect('A: server got 180 Ringing', !!(await withTimeout(pRing)));

    await page.click('#btnAnswer');
    const okResp = await withTimeout(pOk);
    expect('A: server got 200 OK with SDP answer', !!okResp);

    if (okResp) {
      sendAck();
      await page.waitForTimeout(1000);
    }
    const inCallVisible = await page.locator('#incall').isVisible();
    expect('A: in-call UI shown after answer', inCallVisible);
    const status = await page.locator('#callStatus').innerText();
    expect('A: call status shows connected', /Connected/.test(status), status);
    logText = await page.locator('#log').innerText();
    expect('A: page log confirms ACK', logText.includes('Call confirmed'),
      logText.split('\n').slice(-4).join(' | '));
    expect('A: no call failure in log', !logText.includes('Call failed'),
      logText.split('\n').slice(-4).join(' | '));

    // Remote side hangs up.
    if (cur.state === 'confirmed') {
      const pBye = waitFor((m) => /^SIP\/2\.0 2\d\d/.test(m.startLine) && (hdr(m, 'cseq') || '').endsWith('BYE'));
      sendBye();
      expect('A: server got 200 OK for BYE', !!(await withTimeout(pBye)));
      await page.waitForTimeout(500);
      logText = await page.locator('#log').innerText();
      expect('A: remote BYE -> page logged Call ended', logText.includes('Call ended'));
      expect('A: in-call UI cleared after BYE', !(await page.locator('#incall').isVisible()));
    }

    // === Scenario B: inbound call rejected ===
    sendInvite();
    const pRingB = waitFor((m) => /^SIP\/2\.0 18\d/.test(m.startLine));
    const pRej = waitFor((m) => /^SIP\/2\.0 [3-6]\d\d/.test(m.startLine) && (hdr(m, 'cseq') || '').endsWith('INVITE'));
    await page.locator('#incoming').waitFor({ state: 'visible', timeout: TIMEOUT_MS }).catch(() => {});
    expect('B: incoming-call UI shown for second call', await page.locator('#incoming').isVisible());
    await page.click('#btnReject');
    const rej = await withTimeout(pRej);
    expect('B: server got final rejection for rejected call', !!rej, String(rej));
    logText = await page.locator('#log').innerText();
    expect('B: rejection is 486/603-class', /4\d\d|6\d\d/.test(String(rej)), String(rej));
    await page.waitForTimeout(300);
    expect('B: incoming UI cleared after reject', !(await page.locator('#incoming').isVisible()));
    void pRingB;

    // === Scenario C: caller cancels while ringing ===
    sendInvite();
    const pCancelled = waitFor((m) => /^SIP\/2\.0 487/.test(m.startLine) && (hdr(m, 'cseq') || '').endsWith('INVITE'));
    const pCancelOk = waitFor((m) => /^SIP\/2\.0 2\d\d/.test(m.startLine) && (hdr(m, 'cseq') || '').endsWith('CANCEL'));
    await page.locator('#incoming').waitFor({ state: 'visible', timeout: TIMEOUT_MS }).catch(() => {});
    expect('C: incoming-call UI shown for third call', await page.locator('#incoming').isVisible());
    sendCancel();
    expect('C: server got 487 Request Terminated', !!(await withTimeout(pCancelled)));
    expect('C: server got 200 OK for CANCEL', !!(await withTimeout(pCancelOk)));
    await page.waitForTimeout(500);
    expect('C: incoming UI cleared after CANCEL', !(await page.locator('#incoming').isVisible()));
    logText = await page.locator('#log').innerText();
    expect('C: page logged call failure', logText.includes('Call failed'),
      logText.split('\n').slice(-4).join(' | '));

    expect('no console errors', consoleErrors.length === 0, consoleErrors.join(' | '));
  } finally {
    if (browser) await browser.close();
    pageServer.close();
    sipServer.close();
  }

  console.log(fail === 0 ? 'ALL PASS' : `${fail} FAILURES`);
  process.exitCode = fail === 0 ? 0 : 1;
}

main().catch((e) => { console.error('driver error:', e); process.exitCode = 1; });
