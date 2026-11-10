// A scripted SIP peer behind a SIP-over-WebSocket server (RFC 7118), for driving the
// web SDK in a real browser without a PBX. It answers REGISTER, OPTIONS, INFO, BYE and
// re-INVITE itself and lets the test send INVITE / INFO / BYE and wait for messages.

import http from 'node:http';
import crypto from 'node:crypto';

function encodeFrame(opcode, payload) {
  const data = Buffer.from(payload);
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
      let payload = Buffer.from(this.buf.subarray(off, off + len));
      if (masked) {
        const mask = this.buf.subarray(maskOff, maskOff + 4);
        for (let i = 0; i < payload.length; i++) payload[i] ^= mask[i % 4];
      }
      this.buf = this.buf.subarray(off + len);
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

export function parseSip(raw) {
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
  const startLine = lines[0];
  const status = /^SIP\/2\.0 (\d{3})/.exec(startLine);
  const method = status ? null : startLine.split(' ')[0];
  const cseq = (headers.cseq || [''])[0];
  return {
    raw, startLine, headers, body, method,
    status: status ? Number(status[1]) : null,
    cseqMethod: cseq.split(' ')[1] || '',
    header: (k) => (headers[k] ? headers[k][0] : ''),
  };
}

const tagOf = (value) => (/;tag=([^;\s>]+)/.exec(value || '') || [])[1];
const uriOf = (value) => (/<([^>]+)>/.exec(value || '') || [])[1] || value;

export const PEER_ICE = [
  'a=ice-ufrag:peerufrag0001',
  'a=ice-pwd:peericepassword0000000001',
  'a=fingerprint:sha-256 00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00:00',
];

/** An audio+video offer the way a WebRTC peer behind Asterisk sends it. */
export function peerOffer({ video = true } = {}) {
  const lines = [
    'v=0', 'o=peer 1 1 IN IP4 203.0.113.10', 's=peer', 'c=IN IP4 203.0.113.10', 't=0 0',
    `a=group:BUNDLE 0${video ? ' 1' : ''}`,
    'm=audio 40000 UDP/TLS/RTP/SAVPF 0 8 101',
    'a=rtpmap:0 PCMU/8000', 'a=rtpmap:8 PCMA/8000', 'a=rtpmap:101 telephone-event/8000', 'a=fmtp:101 0-16',
    'a=sendrecv', 'a=rtcp-mux', ...PEER_ICE, 'a=setup:actpass', 'a=mid:0',
  ];
  if (video) {
    lines.push(
      'm=video 40002 UDP/TLS/RTP/SAVPF 96',
      'a=rtpmap:96 H264/90000', 'a=fmtp:96 profile-level-id=42e01f;packetization-mode=1',
      'a=sendrecv', 'a=rtcp-mux', ...PEER_ICE, 'a=setup:actpass', 'a=mid:1',
    );
  }
  return lines.join('\r\n') + '\r\n';
}

/**
 * Builds an answer to a browser offer: same m-lines and codecs, peer ICE/DTLS
 * credentials, flipped direction and DTLS role, no candidates or SSRCs.
 */
let answerVersion = 1;
export function answerFor(offer, setup = 'active') {
  const flip = { sendonly: 'recvonly', recvonly: 'sendonly', sendrecv: 'sendrecv', inactive: 'inactive' };
  const out = [];
  for (const line of offer.split(/\r?\n/).filter(Boolean)) {
    if (line.startsWith('o=')) { out.push(`o=peer 1 ${answerVersion++} IN IP4 203.0.113.10`); continue; }
    if (/^a=(ice-ufrag|ice-pwd|fingerprint|candidate|end-of-candidates|ssrc|ssrc-group|msid|ice-options|extmap-allow-mixed|rtcp-rsize)/.test(line)) continue;
    if (line.startsWith('a=setup:')) { out.push(`a=setup:${setup}`); continue; }
    const dir = /^a=(sendonly|recvonly|sendrecv|inactive)$/.exec(line);
    if (dir) { out.push(`a=${flip[dir[1]]}`); continue; }
    out.push(line);
    if (line.startsWith('m=')) out.push(...PEER_ICE);
  }
  return out.join('\r\n') + '\r\n';
}

export class SipWsPeer {
  constructor({ rejectUsers = [] } = {}) {
    this.rejectUsers = rejectUsers;
    this.ws = null;
    this.contact = null;
    this.received = [];
    this.waiters = [];
    this.dialog = null;
    this.cseq = 1;
  }

  async listen() {
    this.server = http.createServer((req, res) => { res.writeHead(404); res.end(); });
    this.server.on('upgrade', (req, socket) => {
      if (!(req.headers['sec-websocket-protocol'] || '').includes('sip')) { socket.destroy(); return; }
      const accept = crypto.createHash('sha1')
        .update(req.headers['sec-websocket-key'] + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
      socket.write('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n'
        + `Sec-WebSocket-Accept: ${accept}\r\nSec-WebSocket-Protocol: sip\r\n\r\n`);
      socket.setNoDelay(true);
      this.ws = new WsConn(socket, (raw) => this.#onMessage(parseSip(raw)));
    });
    await new Promise((r) => this.server.listen(0, '127.0.0.1', r));
    return this.server.address().port;
  }

  close() { this.server?.close(); this.server?.closeAllConnections?.(); }

  /** Resolves with the next received message matching pred (or one already seen since `since`). */
  waitFor(pred, { timeout = 15000, since = this.received.length } = {}) {
    const hit = this.received.slice(since).find(pred);
    if (hit) return Promise.resolve(hit);
    return new Promise((resolve) => {
      const w = { pred, resolve };
      this.waiters.push(w);
      setTimeout(() => { this.waiters = this.waiters.filter((x) => x !== w); resolve(null); }, timeout);
    });
  }

  respond(req, code, reason, { extra = [], body = '', toTag } = {}) {
    let to = req.header('to');
    if (!tagOf(to) && code > 100) to = `${to};tag=${toTag || `peer${Date.now()}`}`;
    const lines = [
      `SIP/2.0 ${code} ${reason}`,
      ...(req.headers.via || []).map((v) => `Via: ${v}`),
      `From: ${req.header('from')}`,
      `To: ${to}`,
      `Call-ID: ${req.header('call-id')}`,
      `CSeq: ${req.header('cseq')}`,
      ...extra,
    ];
    if (body) lines.push('Content-Type: application/sdp');
    lines.push(`Content-Length: ${Buffer.byteLength(body)}`, '', body);
    this.ws.send(lines.join('\r\n'));
  }

  /** Sends an INVITE from the peer to the registered browser contact. */
  invite({ video = true, displayName = 'Mobile Peer', user = '1001' } = {}) {
    const ts = `${Date.now()}${Math.floor(Math.random() * 1000)}`;
    this.dialog = {
      callId: `peer-${ts}@test.invalid`,
      localTag: `peer${ts}`,
      remoteTag: null,
      remoteTarget: this.contact,
      localUri: `sip:${user}@test.invalid`,
      displayName,
      remoteUri: this.aor,
    };
    this.cseq = 1;
    const body = peerOffer({ video });
    this.#sendRequest('INVITE', { body, contentType: 'application/sdp', uri: this.contact, toTag: false });
  }

  ack() { this.#sendRequest('ACK', { cseq: this.cseq }); }
  bye() { this.#sendRequest('BYE'); }
  info(digit) {
    this.#sendRequest('INFO', { body: `Signal=${digit}\r\nDuration=160\r\n`, contentType: 'application/dtmf-relay' });
  }

  #sendRequest(method, { body = '', contentType = '', uri, toTag = true, cseq } = {}) {
    const d = this.dialog;
    if (cseq === undefined) cseq = method === 'INVITE' && !toTag ? this.cseq : ++this.cseq;
    const lines = [
      `${method} ${uri || d.remoteTarget} SIP/2.0`,
      `Via: SIP/2.0/WS test.invalid;branch=z9hG4bK${method.toLowerCase()}${Date.now()}${Math.floor(Math.random() * 1e6)}`,
      'Max-Forwards: 70',
      `From: "${d.displayName}" <${d.localUri}>;tag=${d.localTag}`,
      `To: <${d.remoteUri}>${toTag && d.remoteTag ? `;tag=${d.remoteTag}` : ''}`,
      `Call-ID: ${d.callId}`,
      `CSeq: ${cseq} ${method}`,
      `Contact: <${d.localUri};transport=ws>`,
    ];
    if (contentType) lines.push(`Content-Type: ${contentType}`);
    lines.push(`Content-Length: ${Buffer.byteLength(body)}`, '', body);
    this.ws.send(lines.join('\r\n'));
  }

  #onMessage(m) {
    if (!m.startLine) return;
    this.received.push(m);

    if (m.method === 'REGISTER') {
      const user = (/sip:([^@]+)@/.exec(m.header('to')) || [])[1];
      if (this.rejectUsers.includes(user)) {
        this.respond(m, 403, 'Forbidden');
      } else {
        const contact = m.header('contact');
        const expires = /expires=0\b/.test(contact) || m.header('expires') === '0' ? 0 : 600;
        if (expires) {
          this.contact = uriOf(contact);
          this.aor = uriOf(m.header('to'));
        }
        // A 200 lists the bindings left. After removing them (Contact: * or expires=0) there
        // are none, so no Contact, as Asterisk does; JsSIP cannot parse "*" in a response.
        const extra = expires ? [`Contact: ${contact.split(';expires')[0]};expires=${expires}`] : [];
        this.respond(m, 200, 'OK', { extra });
      }
    } else if (m.method === 'INVITE' && tagOf(m.header('to'))) {
      // re-INVITE (hold / resume) inside an existing dialog. Keep the DTLS roles of the
      // first exchange: the peer was passive when it made the first offer.
      const peerOffered = this.dialog && this.dialog.callId === m.header('call-id');
      this.respond(m, 200, 'OK', {
        extra: ['Contact: <sip:peer@test.invalid;transport=ws>'],
        body: answerFor(m.body, peerOffered ? 'passive' : 'active'),
      });
    } else if (m.method === 'OPTIONS' || m.method === 'INFO' || m.method === 'BYE' || m.method === 'UPDATE') {
      this.respond(m, 200, 'OK');
    } else if (m.method === 'CANCEL') {
      this.respond(m, 200, 'OK');
      const invite = [...this.received].reverse().find((x) => x.method === 'INVITE' && x.header('call-id') === m.header('call-id'));
      if (invite) this.respond(invite, 487, 'Request Terminated', { toTag: this.pendingTag });
    } else if (m.status && m.cseqMethod === 'INVITE' && this.dialog && m.header('call-id') === this.dialog.callId && m.status >= 200 && m.status < 300) {
      this.dialog.remoteTag = tagOf(m.header('to'));
      this.dialog.remoteTarget = uriOf(m.header('contact')) || this.contact;
    }

    for (const w of [...this.waiters]) {
      if (w.pred(m)) {
        this.waiters = this.waiters.filter((x) => x !== w);
        w.resolve(m);
      }
    }
  }

  /** Answers a browser INVITE: 180, then (when answer) 200 with an SDP answer. */
  ring(invite) {
    this.pendingTag = `peer${Date.now()}`;
    this.respond(invite, 180, 'Ringing', { toTag: this.pendingTag, extra: ['Contact: <sip:peer@test.invalid;transport=ws>'] });
  }

  accept(invite) {
    this.respond(invite, 200, 'OK', {
      toTag: this.pendingTag,
      extra: ['Contact: <sip:peer@test.invalid;transport=ws>'],
      body: answerFor(invite.body),
    });
  }
}
