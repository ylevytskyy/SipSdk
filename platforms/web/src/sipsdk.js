// SipSdk web platform.
//
// Browser SIP audio + H.264 video over WebSocket signaling and WebRTC media, with the
// same SipClient / SipCall model and state values as the native platforms
// (core/include/sip_abi.h). Built on JsSIP 3.x, passed in or read from globalThis.JsSIP.
//
// Events are delivered synchronously on the caller's (browser main) thread.

export const SipRegistrationState = Object.freeze({
  Unregistered: 0,
  Registering: 1,
  Registered: 2,
  Failed: 3,
});

export const SipCallState = Object.freeze({
  Incoming: 0,
  Ringing: 1,
  Connecting: 2,
  Established: 3,
  Hold: 4,
  Closed: 5,
});

const VIDEO_CONSTRAINTS = { width: { ideal: 640 }, height: { ideal: 480 } };
const DTMF_DIGITS = /^[0-9A-D#*]+$/i;

class Emitter {
  #handlers = new Map();

  /** Adds a listener; returns a function that removes it. */
  on(type, fn) {
    if (!this.#handlers.has(type)) this.#handlers.set(type, new Set());
    this.#handlers.get(type).add(fn);
    return () => this.off(type, fn);
  }

  off(type, fn) {
    this.#handlers.get(type)?.delete(fn);
  }

  emit(type, detail) {
    for (const fn of [...(this.#handlers.get(type) || [])]) {
      try {
        fn(detail);
      } catch (err) {
        console.error(`SipSdk: ${type} listener failed`, err);
      }
    }
  }
}

const toSipUri = (target, domain) => {
  if (/^sips?:/i.test(target)) return target;
  return target.includes('@') ? `sip:${target}` : `sip:${target}@${domain}`;
};

const offerHasVideo = (sdp) => /^m=video [1-9]\d*/m.test(sdp || '');

/**
 * Keeps the codecs the SipSdk platforms share (Opus, G.711, telephone-event, one H.264,
 * one VP8) and drops IPv6, TCP and link-local candidates, so Asterisk and Baresip peers
 * accept the browser's offer. Same rules as the web test page (web/index.html).
 */
export function sanitizeSdp(sdp) {
  const lines = sdp.split(/\r?\n/).map((l) => l.trim()).filter(Boolean);
  const allowedCodecs = ['opus', 'pcmu', 'pcma', 'telephone-event'];
  const keepPts = new Set(['0', '8']);
  let h264Count = 0;
  let vp8Count = 0;

  for (const line of lines) {
    const m = line.match(/^a=rtpmap:(\d+)\s+([a-zA-Z0-9_-]+)/i);
    if (!m) continue;
    const pt = m[1];
    const name = m[2].toLowerCase();
    if (allowedCodecs.includes(name)) {
      keepPts.add(pt);
    } else if (name === 'vp8' && vp8Count < 1) {
      keepPts.add(pt);
      vp8Count++;
    } else if (name === 'h264' && h264Count < 1) {
      keepPts.add(pt);
      h264Count++;
    }
  }

  const droppedExtmaps = ['color-space', 'video-timing', 'playout-delay', 'video-content-type', 'toffset', 'repaired-rtp-stream-id'];
  const out = [];
  for (const line of lines) {
    if (line.startsWith('m=')) {
      const parts = line.split(' ');
      out.push([parts[0], parts[1], parts[2], ...parts.slice(3).filter((pt) => keepPts.has(pt))].join(' '));
      continue;
    }
    const ptMatch = line.match(/^a=(?:rtpmap|fmtp|rtcp-fb):(\d+|\*)/);
    if (ptMatch) {
      if (ptMatch[1] === '*' || keepPts.has(ptMatch[1])) out.push(line);
      continue;
    }
    if (line.startsWith('a=extmap:') && droppedExtmaps.some((name) => line.includes(name))) continue;
    if (line.startsWith('a=candidate:')) {
      const parts = line.split(' ');
      const ip = parts[4];
      if (parts[2].toLowerCase() === 'tcp' || !ip || ip.includes(':') || ip.startsWith('169.254.')) continue;
    }
    out.push(line);
  }
  return out.join('\r\n') + '\r\n';
}

/** One SIP call session. Created by SipClient.makeCall or delivered by incomingCall. */
export class SipCall extends Emitter {
  #client;
  #session;
  #state;
  #hasVideo;
  #onHold = false;
  #muted = false;
  #localVideoEnabled = true;
  #establishedAt = 0;
  #closedAt = 0;
  #elements = {};
  #remoteStream = null;
  #localStream = null;
  #wiredConnection = null;

  constructor(client, session, { outgoing, video }) {
    super();
    this.#client = client;
    this.#session = session;
    this.#state = outgoing ? SipCallState.Connecting : SipCallState.Incoming;
    this.#hasVideo = video;
    this.id = session.id || `${Date.now()}-${Math.random().toString(36).slice(2)}`;
    this.isOutgoing = outgoing;
    this.remoteUri = session.remote_identity?.uri?.toString() || '';
    this.peerDisplayName = session.remote_identity?.display_name || '';
    this.#wireSession();
  }

  get state() { return this.#state; }
  get hasVideo() { return this.#hasVideo; }
  get isOnHold() { return this.#onHold; }
  get isMuted() { return this.#muted; }
  get isLocalVideoEnabled() { return this.#localVideoEnabled; }
  get isClosed() { return this.#state === SipCallState.Closed; }
  get localStream() { return this.#localStream; }
  get remoteStream() { return this.#remoteStream; }

  /** Whole seconds since the call was answered; 0 before that, frozen after close. */
  get duration() {
    if (!this.#establishedAt) return 0;
    const end = this.#closedAt || this.#client.now();
    return Math.floor((end - this.#establishedAt) / 1000);
  }

  /** Answers an incoming call; video:false answers audio-only. */
  answer({ video = true } = {}) {
    this.#assertOpen();
    if (this.isOutgoing) throw new Error('SipSdk: answer() is only valid for incoming calls');
    const withVideo = video && this.#hasVideo;
    this.#hasVideo = withVideo;
    this.#session.answer({
      mediaConstraints: { audio: true, video: withVideo ? VIDEO_CONSTRAINTS : false },
      pcConfig: this.#client.pcConfig,
    });
  }

  /**
   * Ends the call. On an unanswered incoming call statusCode declines it
   * (486 Busy Here, 603 Decline); otherwise sends CANCEL or BYE.
   */
  hangup(statusCode = 0, reason = '') {
    this.#assertOpen();
    const options = {};
    if (statusCode) options.status_code = statusCode;
    if (reason) options.reason_phrase = reason;
    this.#session.terminate(options);
  }

  setHold(hold) {
    this.#assertOpen();
    if (hold) this.#session.hold();
    else this.#session.unhold();
  }

  setMuted(muted) {
    this.#assertOpen();
    if (muted) this.#session.mute({ audio: true });
    else this.#session.unmute({ audio: true });
    this.#muted = !!muted;
    this.#emitMedia();
  }

  setLocalVideoEnabled(enabled) {
    this.#assertOpen();
    if (enabled) this.#session.unmute({ video: true });
    else this.#session.mute({ video: true });
    this.#localVideoEnabled = !!enabled;
  }

  /** Sends DTMF digits (0-9, A-D, *, #) as SIP INFO, which Asterisk and Baresip both accept. */
  sendDtmf(digits) {
    this.#assertOpen();
    if (!digits || !DTMF_DIGITS.test(digits)) throw new Error(`SipSdk: invalid DTMF digits "${digits}"`);
    if (!this.#session.isEstablished()) throw new Error('SipSdk: DTMF needs an established call');
    this.#session.sendDTMF(digits, { transportType: 'INFO' });
  }

  /**
   * Binds media elements. remoteVideo / remoteAudio get the remote stream, localVideo the
   * camera preview (muted). Elements can be bound before or after media starts.
   */
  attachMedia({ localVideo = null, remoteVideo = null, remoteAudio = null } = {}) {
    this.#elements = { localVideo, remoteVideo, remoteAudio };
    if (localVideo) localVideo.muted = true;
    if (remoteVideo) {
      remoteVideo.addEventListener('resize', () => this.#emitMedia());
    }
    this.#applyStreams();
  }

  #assertOpen() {
    if (this.isClosed) throw new Error('SipSdk: call is closed');
  }

  #setState(state, code = 0, reason = '') {
    if (this.#state === state || this.isClosed) return;
    this.#state = state;
    if (state === SipCallState.Established && !this.#establishedAt) this.#establishedAt = this.#client.now();
    if (state === SipCallState.Closed) {
      this.#closedAt = this.#client.now();
      this.#client._removeCall(this);
      this.#detachStreams();
    }
    this.emit('stateChanged', { state, code, reason });
  }

  #emitMedia() {
    const v = this.#elements.remoteVideo;
    this.emit('mediaChanged', {
      width: v?.videoWidth || 0,
      height: v?.videoHeight || 0,
      isOnHold: this.#onHold,
      isMuted: this.#muted,
    });
  }

  #wireSession() {
    const s = this.#session;
    s.on('sdp', (e) => {
      if (e.originator === 'local' && e.type === 'offer') e.sdp = sanitizeSdp(e.sdp);
    });
    s.on('progress', (e) => {
      if (e.originator !== 'remote') return;
      this.#setState(SipCallState.Ringing, e.response?.status_code || 180, e.response?.reason_phrase || 'Ringing');
    });
    s.on('accepted', (e) => {
      this.#setState(SipCallState.Established, e.response?.status_code || 200, e.response?.reason_phrase || 'OK');
      this.#captureLocalStream();
    });
    s.on('confirmed', () => this.#captureLocalStream());
    s.on('connecting', () => this.#captureLocalStream());
    s.on('ended', (e) => this.#close(e));
    s.on('failed', (e) => this.#close(e));
    s.on('hold', (e) => {
      if (e.originator !== 'local') return;
      this.#onHold = true;
      this.#setState(SipCallState.Hold);
      this.#emitMedia();
    });
    s.on('unhold', (e) => {
      if (e.originator !== 'local') return;
      this.#onHold = false;
      this.#setState(SipCallState.Established);
      this.#emitMedia();
    });
    s.on('newDTMF', (e) => {
      if (e.originator === 'remote' && e.dtmf?.tone) this.emit('dtmfReceived', { digit: String(e.dtmf.tone) });
    });
    s.on('newInfo', (e) => {
      if (e.originator !== 'remote' || !/dtmf-relay/i.test(e.info?.contentType || '')) return;
      const m = /Signal\s*=\s*([0-9A-D#*])/i.exec(e.info.body || '');
      if (m) this.emit('dtmfReceived', { digit: m[1] });
    });
    s.on('peerconnection', (e) => this.#wireConnection(e.peerconnection));
    if (s.connection) this.#wireConnection(s.connection);
  }

  #close(e) {
    const code = e?.message?.status_code || 0;
    const reason = e?.message?.reason_phrase || e?.cause || '';
    this.#setState(SipCallState.Closed, code, reason);
  }

  #wireConnection(pc) {
    if (!pc || this.#wiredConnection === pc || typeof MediaStream === 'undefined') return;
    this.#wiredConnection = pc;
    this.#remoteStream = new MediaStream();
    pc.addEventListener('track', (e) => {
      if (e.track && !this.#remoteStream.getTracks().includes(e.track)) this.#remoteStream.addTrack(e.track);
      this.#applyStreams();
    });
    this.#captureLocalStream();
  }

  #captureLocalStream() {
    const pc = this.#session.connection || this.#wiredConnection;
    if (!pc?.getSenders || typeof MediaStream === 'undefined') return;
    const tracks = pc.getSenders().map((sender) => sender.track).filter(Boolean);
    const videoTracks = tracks.filter((t) => t.kind === 'video');
    const current = this.#localStream?.getTracks() || [];
    if (videoTracks.length && !videoTracks.every((t) => current.includes(t))) {
      this.#localStream = new MediaStream(videoTracks);
      this.#applyStreams();
    }
  }

  #applyStreams() {
    const { localVideo, remoteVideo, remoteAudio } = this.#elements;
    const play = (el, stream) => {
      if (!el || !stream || el.srcObject === stream) return;
      el.srcObject = stream;
      el.play?.().catch(() => {});
    };
    play(localVideo, this.#localStream);
    play(remoteVideo, this.#remoteStream);
    play(remoteAudio, this.#remoteStream);
  }

  #detachStreams() {
    for (const el of Object.values(this.#elements)) {
      if (el) el.srcObject = null;
    }
  }
}

/** Owns the SIP user agent: registration, outgoing calls and incoming call delivery. */
export class SipClient extends Emitter {
  #JsSIP;
  #started = false;
  #ua = null;
  #registered = false;
  #domain = '';
  #calls = new Set();

  /**
   * @param {object} [options]
   * @param {object} [options.jssip] JsSIP namespace; defaults to globalThis.JsSIP
   * @param {() => number} [options.now] clock in ms, for call duration
   */
  constructor({ jssip = globalThis.JsSIP, now = () => Date.now() } = {}) {
    super();
    if (!jssip?.UA) throw new Error('SipSdk: JsSIP is not loaded');
    this.#JsSIP = jssip;
    this.now = now;
    this.accountAor = '';
    this.pcConfig = { iceServers: [] };
  }

  get isRegistered() { return this.#registered; }
  get isStarted() { return this.#started; }
  get calls() { return [...this.#calls]; }

  start() {
    this.#started = true;
  }

  /** Hangs up every open call, stops the user agent and its WebSocket. */
  stop() {
    for (const call of this.#calls) {
      try { call.hangup(); } catch { /* already closing */ }
    }
    if (this.#ua) this.#ua.stop();
    this.#ua = null;
    this.#registered = false;
    this.#started = false;
  }

  /**
   * Registers a SIP account over a SIP WebSocket (ws:// or wss://).
   * @param {{username: string, domain: string, password?: string, wsUri: string,
   *          displayName?: string, iceServers?: RTCIceServer[]}} account
   */
  register({ username, domain, password = '', wsUri, displayName = '', iceServers = [] }) {
    if (!this.#started) throw new Error('SipSdk: call start() before register()');
    if (!username) throw new Error('SipSdk: username is required');
    if (!domain) throw new Error('SipSdk: domain is required');
    if (!wsUri) throw new Error('SipSdk: wsUri is required');
    if (this.#ua) this.#ua.stop();

    this.#registered = false;
    this.#domain = domain;
    this.accountAor = `sip:${username}@${domain}`;
    this.pcConfig = { iceServers };

    const ua = new this.#JsSIP.UA({
      sockets: [new this.#JsSIP.WebSocketInterface(wsUri)],
      uri: this.accountAor,
      password: password || undefined,
      display_name: displayName || undefined,
      session_timers: true,
      register: true,
    });
    this.#ua = ua;

    const report = (state, e) => {
      if (this.#ua !== ua) return;
      this.#registered = state === SipRegistrationState.Registered;
      const code = e?.response?.status_code || 0;
      const reason = e?.response?.reason_phrase || e?.cause || '';
      this.emit('registrationStateChanged', { state, code, reason });
    };
    ua.on('registered', (e) => report(SipRegistrationState.Registered, e));
    ua.on('unregistered', (e) => report(SipRegistrationState.Unregistered, e));
    ua.on('registrationFailed', (e) => report(SipRegistrationState.Failed, e));
    ua.on('newRTCSession', (e) => {
      if (this.#ua !== ua || e.originator !== 'remote') return;
      const call = new SipCall(this, e.session, { outgoing: false, video: offerHasVideo(e.request?.body) });
      this.#calls.add(call);
      this.emit('incomingCall', { call });
    });

    this.emit('registrationStateChanged', { state: SipRegistrationState.Registering, code: 0, reason: '' });
    ua.start();
  }

  /** Removes every binding of the account from the registrar. */
  unregister() {
    if (!this.#ua) return;
    this.#ua.unregister({ all: true });
  }

  /**
   * Places a call. target is an extension ("9001"), user@host, or a sip: URI.
   * @returns {SipCall}
   */
  makeCall(target, { video = false } = {}) {
    if (!this.#started) throw new Error('SipSdk: call start() before makeCall()');
    if (!this.#ua || !this.#registered) throw new Error('SipSdk: register an account before makeCall()');
    if (!target) throw new Error('SipSdk: target is required');
    const session = this.#ua.call(toSipUri(target, this.#domain), {
      mediaConstraints: { audio: true, video: video ? VIDEO_CONSTRAINTS : false },
      pcConfig: this.pcConfig,
    });
    const call = new SipCall(this, session, { outgoing: true, video: !!video });
    this.#calls.add(call);
    return call;
  }

  /** @internal */
  _removeCall(call) {
    this.#calls.delete(call);
  }
}
