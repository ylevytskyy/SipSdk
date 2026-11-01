// Verifies the web test page supports both the existing audio controls (I9) and
// two-way WebRTC video (R6 / D5):
//   1. All existing controls plus #enableVideo, #localVideo, #remoteVideo, #btnVideo render cleanly.
//   2. Inbound audio+video SIP INVITE over WebSocket is answered with SDP containing m=audio and m=video.
//   3. Local camera preview (#localVideo) attaches a video track and #btnVideo toggles video mute/unmute.
//   4. Outgoing video call from the page sends an INVITE whose SDP offer includes m=audio and m=video (H264).
// Usage: node specs/ios-video-phone/verify-web-video.mjs

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
const EVIDENCE_DIR = path.resolve(ROOT, 'evidence');
const AOR = 'sip:page-user@iptel.org';
const CALLER = 'sip:ios-video-caller@fake.invalid';
const TIMEOUT_MS = 30000;

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

const VIDEO_OFFER_SDP = [
  'v=0',
  'o=fakecaller 1 1 IN IP4 203.0.113.10',
  's=fake-video-call',
  'c=IN IP4 203.0.113.10',
  't=0 0',
  'a=group:BUNDLE 0 1',
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
  'm=video 40002 UDP/TLS/RTP/SAVPF 96',
  'a=rtpmap:96 H264/90000',
  'a=fmtp:96 profile-level-id=42e01f;packetization-mode=1',
  'a=sendrecv',
  'a=rtcp-mux',
  'a=ice-ufrag:fakeufrag0001',
  'a=ice-pwd:fakeicepassword00000001',
  'a=fingerprint:sha-256 00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00',
  'a=setup:actpass',
  'a=mid:1',
].join('\r\n') + '\r\n';

async function main() {
  fs.mkdirSync(EVIDENCE_DIR, { recursive: true });
  let fail = 0;
  const expect = (name, ok, detail = '') => {
    console.log(`${ok ? 'PASS' : 'FAIL'} ${name}${detail ? ' — ' + detail : ''}`);
    if (!ok) fail++;
  };

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

  let ws = null;
  let registeredContact = null;
  let cur = null;
  let lastOutgoingInvite = null;
  const outgoingInviteWaiters = [];

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
  });
  await new Promise((r) => sipServer.listen(0, '127.0.0.1', r));
  const wsPort = sipServer.address().port;

  const waitFor = (pred) =>
    new Promise((res) => cur.waiters.push({ pred, res }));
  const withTimeout = (p, ms = TIMEOUT_MS) =>
    Promise.race([p.then((m) => m), new Promise((r) => setTimeout(() => r(null), ms))]);

  function sendVideoInvite() {
    const ts = Date.now();
    cur = {
      state: 'invited',
      callId: `video-call-${ts}@fake.invalid`,
      branch: `z9hG4bKvid${ts}`,
      fromTag: `caller${ts}`,
      toTag: null,
      contact: null,
      waiters: [],
    };
    const msg = [
      `INVITE ${registeredContact} SIP/2.0`,
      `Via: SIP/2.0/WS fake.invalid;branch=${cur.branch}`,
      'Max-Forwards: 70',
      `From: "iOS Video Caller" <${CALLER}>;tag=${cur.fromTag}`,
      `To: <${AOR}>`,
      `Call-ID: ${cur.callId}`,
      'CSeq: 1 INVITE',
      `Contact: <${CALLER};transport=ws>`,
      'Content-Type: application/sdp',
      `Content-Length: ${Buffer.byteLength(VIDEO_OFFER_SDP)}`,
      '',
      VIDEO_OFFER_SDP,
    ].join('\r\n');
    ws.send(msg);
  }

  function sendAck() {
    const msg = [
      `ACK ${cur.contact} SIP/2.0`,
      `Via: SIP/2.0/WS fake.invalid;branch=z9hG4bKack${Date.now()}`,
      'Max-Forwards: 70',
      `From: "iOS Video Caller" <${CALLER}>;tag=${cur.fromTag}`,
      `To: <${AOR}>;tag=${cur.toTag}`,
      `Call-ID: ${cur.callId}`,
      'CSeq: 1 ACK',
      'Content-Length: 0',
      '',
      '',
    ].join('\r\n');
    ws.send(msg);
    cur.state = 'confirmed';
  }

  function sendBye() {
    const msg = [
      `BYE ${cur.contact} SIP/2.0`,
      `Via: SIP/2.0/WS fake.invalid;branch=z9hG4bKbye${Date.now()}`,
      'Max-Forwards: 70',
      `From: "iOS Video Caller" <${CALLER}>;tag=${cur.fromTag}`,
      `To: <${AOR}>;tag=${cur.toTag}`,
      `Call-ID: ${cur.callId}`,
      'CSeq: 2 BYE',
      'Content-Length: 0',
      '',
      '',
    ].join('\r\n');
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
      ws.send(resp);
      return;
    }

    if (m.startLine.startsWith('INVITE')) {
      lastOutgoingInvite = m;
      for (const w of outgoingInviteWaiters) w(m);
      ws.send(sipResponse(m, 486, 'Busy Here'));
      return;
    }

    if (m.startLine.startsWith('ACK') || m.startLine.startsWith('OPTIONS')) {
      if (m.startLine.startsWith('OPTIONS')) ws.send(sipResponse(m, 200, 'OK'));
      return;
    }

    if (/^SIP\/2\.0/.test(m.startLine) && cur) {
      const cseq = hdr(m, 'cseq') || '';
      const code = /^SIP\/2\.0 (\d\d\d)/.exec(m.startLine)[1];
      if (cseq.endsWith('INVITE')) {
        if (code.startsWith('2')) {
          cur.state = 'answered';
          cur.toTag = tagOf(m, 'to');
          const c = hdr(m, 'contact') || '';
          const cm = /<([^>]+)>/.exec(c);
          cur.contact = cm ? cm[1] : registeredContact;
        }
      }
      for (const w of cur.waiters) {
        if (!w.done && w.pred(m)) { w.done = true; w.res(m); }
      }
    }
  }

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
    page.on('console', (m) => { if (m.type() === 'error') consoleErrors.push(m.text()); });
    page.on('pageerror', (e) => consoleErrors.push(`pageerror: ${e.message}`));

    await page.goto(`http://127.0.0.1:${pagePort}/`, { waitUntil: 'networkidle', timeout: TIMEOUT_MS });

    // 1. Verify existing audio elements + new video elements
    expect('#enableVideo checkbox present', (await page.locator('#enableVideo').count()) === 1);
    expect('#enableVideo checked by default', await page.locator('#enableVideo').isChecked().catch(() => false));
    expect('#localVideo element present', (await page.locator('#localVideo').count()) === 1);
    expect('#remoteVideo element present', (await page.locator('#remoteVideo').count()) === 1);
    expect('#btnVideo toggle button present', (await page.locator('#btnVideo').count()) === 1);
    expect('12 DTMF buttons preserved', (await page.locator('[data-dtmf]').count()) === 12);

    // 2. Register against local SIP-over-WebSocket server
    await page.fill('#sipUri', AOR.replace(/^sip:/, ''));
    await page.fill('#sipPass', 'test-password');
    await page.fill('#wsUri', `ws://127.0.0.1:${wsPort}`);
    await page.click('#btnRegister');

    const deadline = Date.now() + TIMEOUT_MS;
    let logText = '';
    while (Date.now() < deadline) {
      logText = await page.locator('#log').innerText();
      if (/Registered:/.test(logText)) break;
      await page.waitForTimeout(200);
    }
    expect('registered over WebSocket', /Registered:/.test(logText));

    // 3. Inbound video call answered with audio+video SDP
    sendVideoInvite();
    const pOk = waitFor((m) => /^SIP\/2\.0 2\d\d/.test(m.startLine) && (hdr(m, 'cseq') || '').endsWith('INVITE'));
    await page.locator('#incoming').waitFor({ state: 'visible', timeout: TIMEOUT_MS });
    await page.click('#btnAnswer');

    const okMsg = await withTimeout(pOk);
    expect('200 OK received for inbound video call', !!okMsg);
    const answerSdp = okMsg ? okMsg.body : '';
    expect('SDP answer includes active m=audio and m=video (non-zero port)',
      answerSdp.includes('m=audio') && /m=video [1-9]\d*/.test(answerSdp),
      answerSdp.split('\r\n').filter((l) => l.startsWith('m=')).join(' & '));

    if (okMsg) {
      sendAck();
      await page.waitForTimeout(800);
    }

    const hasLocalVideoStream = await page.evaluate(() => {
      const lv = document.getElementById('localVideo');
      return !!(lv && lv.srcObject && lv.srcObject.getVideoTracks().length > 0);
    });
    expect('#localVideo attaches local camera MediaStream with video track', hasLocalVideoStream);

    // 4. Exercise #btnVideo camera mute/unmute toggle
    await page.click('#btnVideo');
    const btnVideoOffText = await page.locator('#btnVideo').innerText();
    expect('#btnVideo toggles to Video On / camera muted state', /Video On|Unmute Video/i.test(btnVideoOffText), btnVideoOffText);
    await page.click('#btnVideo');
    const btnVideoOnText = await page.locator('#btnVideo').innerText();
    expect('#btnVideo toggles back to Video Off / camera active state', /Video Off|Mute Video/i.test(btnVideoOnText), btnVideoOnText);

    await page.screenshot({ path: path.join(EVIDENCE_DIR, 'web-video-incall.png'), fullPage: true });

    // Clean up inbound call via BYE
    const pBye = waitFor((m) => /^SIP\/2\.0 2\d\d/.test(m.startLine) && (hdr(m, 'cseq') || '').endsWith('BYE'));
    sendBye();
    await withTimeout(pBye);
    await page.waitForTimeout(400);

    // 5. Outgoing video call sends INVITE with m=video
    const pOutInvite = new Promise((r) => outgoingInviteWaiters.push(r));
    await page.click('#btnCall');
    const outInvite = await withTimeout(pOutInvite);
    expect('outgoing call sends SIP INVITE with active m=audio and m=video',
      !!(outInvite && outInvite.body.includes('m=audio') && /m=video [1-9]\d*/.test(outInvite.body)),
      outInvite ? outInvite.body.split('\r\n').filter((l) => l.startsWith('m=')).join(' & ') : 'no INVITE');

    expect('no console errors', consoleErrors.length === 0, consoleErrors.join(' | '));
  } finally {
    if (browser) await browser.close();
    pageServer.close();
    sipServer.close();
  }

  if (fail === 0) {
    console.log('ALL_WEB_VIDEO_CHECKS_PASS');
    process.exit(0);
  } else {
    console.log(`${fail} FAILURES`);
    process.exit(1);
  }
}

main().catch((e) => { console.error('driver error:', e); process.exitCode = 1; });
