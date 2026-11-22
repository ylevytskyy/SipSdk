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

/** Status of a sent SIP MESSAGE; same values as sip_message_status_t. */
export const SipMessageStatus = Object.freeze({
  Sent: 0,
  Failed: 1,
  Delivered: 2, // reserved: IMDN delivery notification (RFC 5438)
  Displayed: 3, // reserved: IMDN display (read) notification (RFC 5438)
});

/** Maximum concurrent SIP accounts hosted by a single SipClient. */
export const SIP_CLIENT_MAX_ACCOUNTS = 8;

const makeError = (message, code) => {
  const err = new Error(message);
  if (code) err.code = code;
  return err;
};

const VIDEO_CONSTRAINTS = { width: { ideal: 640 }, height: { ideal: 480 } };
const CONTENT_TYPE = /^[^\s/;]+\/[^\s/;]+(\s*;.*)?$/;
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

/** One SIP call session. Created by SipClient.makeCall / SipAccount.dial or delivered by incomingCall. */
export class SipCall extends Emitter {
  #client;
  #account;
  #session;
  #state;
  #hasVideo;
  #onHold = false;
  #holdFallback = false;
  #muted = false;
  #inConference = false;
  #originalAudioTrack = null;
  #localVideoEnabled = true;
  #establishedAt = 0;
  #closedAt = 0;
  #elements = {};
  #remoteStream = null;
  #localStream = null;
  #wiredConnection = null;
  #statsTimer = null;
  #prevStats = null;
  #pendingRefer = null;

  constructor(client, session, { outgoing, video, account = null }) {
    super();
    this.#client = client;
    this.#account = account;
    this.#session = session;
    this.#state = outgoing ? SipCallState.Connecting : SipCallState.Incoming;
    this.#hasVideo = video;
    this.id = session.id || `${Date.now()}-${Math.random().toString(36).slice(2)}`;
    this.isOutgoing = outgoing;
    this.remoteUri = session.remote_identity?.uri?.toString() || '';
    this.peerDisplayName = session.remote_identity?.display_name || '';
    this.#wireSession();
  }

  get account() { return this.#account; }
  get state() { return this.#state; }
  get hasVideo() { return this.#hasVideo; }
  get isOnHold() { return this.#onHold; }
  get isMuted() { return this.#muted; }
  get isInConference() { return this.#inConference; }
  get isLocalVideoEnabled() { return this.#localVideoEnabled; }
  get isClosed() { return this.#state === SipCallState.Closed; }
  get isSecure() {
    return !this.isClosed && Boolean(this.#session?.connection || this.#wiredConnection || this.#establishedAt);
  }
  get localStream() { return this.#localStream; }
  get remoteStream() { return this.#remoteStream; }
  /** @internal */
  get _session() { return this.#session; }
  /** @internal */
  _setInConference(inConf) { this.#inConference = !!inConf; }
  /** @internal */
  _getAudioSender() {
    const pc = this.#session?.connection || this.#wiredConnection;
    const sender = pc?.getSenders?.()?.find((s) => s.track?.kind === 'audio') || null;
    if (sender?.track && !this.#originalAudioTrack) {
      this.#originalAudioTrack = sender.track;
    }
    return sender;
  }
  /** @internal */
  _getOriginalAudioTrack() {
    this._getAudioSender();
    return this.#originalAudioTrack;
  }
  /** @internal */
  _restoreOriginalAudioTrack() {
    if (!this.#originalAudioTrack) return;
    const pc = this.#session?.connection || this.#wiredConnection;
    const sender = pc?.getSenders?.()?.find((s) => s.track?.kind === 'audio' || s.track === null);
    if (sender?.replaceTrack) {
      Promise.resolve(sender.replaceTrack(this.#originalAudioTrack)).catch(() => {});
    }
  }

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
    this.#client._autoHoldOthers(this);
    const withVideo = video && this.#hasVideo;
    this.#hasVideo = withVideo;
    this.#session.answer({
      mediaConstraints: { audio: true, video: withVideo ? VIDEO_CONSTRAINTS : false },
      pcConfig: this.#account?._pcConfig || this.#client.pcConfig,
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
    if (this.#inConference) {
      this.#client._setConferenceHold(!!hold);
      return;
    }
    if (!hold) {
      this.#client._autoHoldOthers(this);
    }
    this._setHoldDirect(hold);
  }

  /** @internal */
  _setHoldDirect(hold) {
    this.#assertOpen();
    if (hold) {
      let ok = false;
      try {
        ok = this.#session.hold() !== false;
      } catch {
        ok = false;
      }
      if (!ok) {
        if (this.#localStream) {
          for (const track of this.#localStream.getAudioTracks()) track.enabled = false;
        }
        this.#holdFallback = true;
        this.#onHold = true;
        this.#setState(SipCallState.Hold);
        this.#emitMedia();
      } else {
        this.#holdFallback = false;
      }
    } else {
      if (this.#holdFallback) {
        this.#holdFallback = false;
        if (this.#localStream && !this.#muted) {
          for (const track of this.#localStream.getAudioTracks()) track.enabled = true;
        }
        try {
          this.#session.unhold();
        } catch { /* fallback already local */ }
        this.#onHold = false;
        this.#setState(SipCallState.Established);
        this.#emitMedia();
      } else {
        this.#session.unhold();
      }
    }
  }

  setMuted(muted) {
    this.#assertOpen();
    if (this.#inConference) {
      this.#client._setConferenceLocalMuted(!!muted);
      return;
    }
    if (muted) this.#session.mute({ audio: true });
    else this.#session.unmute({ audio: true });
    this.#muted = !!muted;
    this.#emitMedia();
  }

  /** @internal */
  _setMutedFlag(muted) {
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

  /** Blind call transfer (SIP REFER) to target extension, user@host, or sip: URI. */
  transfer(target) {
    this.#assertOpen();
    if (!target) throw new Error('SipSdk: transfer target is required');
    if (typeof this.#session.refer !== 'function') throw new Error('SipSdk: session does not support REFER');
    const ok = this.#session.refer(toSipUri(target, this.#account?.domain || this.#client._domain), {
      eventHandlers: this.#referHandlers(),
    });
    if (!ok) throw new Error('SipSdk: transfer failed');
  }

  /** Attended call transfer (SIP REFER with Replaces) to an active consultation call. */
  attendedTransfer(consultCall) {
    this.#assertOpen();
    if (!consultCall || consultCall === this || consultCall.isClosed) {
      throw new Error('SipSdk: consultCall must be an open SipCall');
    }
    if (typeof this.#session.refer !== 'function') throw new Error('SipSdk: session does not support REFER');
    const ok = this.#session.refer(consultCall.remoteUri || '', {
      replaces: consultCall._session,
      eventHandlers: this.#referHandlers(),
    });
    if (!ok) throw new Error('SipSdk: attended transfer failed');
  }

  /** Accepts or rejects a pending incoming REFER transfer request. */
  acceptTransfer(accept = true) {
    this.#assertOpen();
    if (!this.#pendingRefer) throw new Error('SipSdk: no pending transfer request');
    const req = this.#pendingRefer;
    this.#pendingRefer = null;
    if (accept) req.accept?.();
    else req.reject?.();
  }

  /** Reads live WebRTC audio/video quality metrics via RTCPeerConnection.getStats(). */
  async getStats() {
    const empty = {
      audioLossRate: 0,
      audioJitterMs: 0,
      audioRttMs: 0,
      audioBitrateKbps: 0,
      audioCodec: '',
      videoLossRate: 0,
      videoJitterMs: 0,
      videoBitrateKbps: 0,
      videoFps: 0,
      videoCodec: '',
    };
    const pc = this.#session?.connection || this.#wiredConnection;
    if (this.isClosed || typeof pc?.getStats !== 'function') return empty;

    const report = await pc.getStats();
    const entries = typeof report?.values === 'function' ? [...report.values()] : [...(report || [])];
    const codecs = new Map();
    for (const item of entries) {
      if (item?.type === 'codec' && item.id) {
        const mime = String(item.mimeType || '').replace(/^(audio|video)\//i, '');
        codecs.set(item.id, mime);
      }
    }

    const stats = { ...empty };
    const nextPrev = this.#prevStats || {};
    let candidateRttMs = 0;

    for (const item of entries) {
      if (!item) continue;
      if (item.type === 'candidate-pair' && (item.nominated || item.state === 'succeeded') && typeof item.currentRoundTripTime === 'number') {
        candidateRttMs = Math.round(item.currentRoundTripTime * 1000);
      } else if (item.type === 'remote-inbound-rtp' && (item.kind === 'audio' || item.mediaType === 'audio')) {
        if (typeof item.roundTripTime === 'number') {
          stats.audioRttMs = Math.round(item.roundTripTime * 1000);
        }
      } else if (item.type === 'inbound-rtp') {
        const kind = item.kind || item.mediaType;
        const rx = Number(item.packetsReceived || 0);
        const lost = Math.max(0, Number(item.packetsLost || 0));
        const lossRate = rx + lost > 0 ? lost / (rx + lost) : 0;
        const jitterMs = Math.round(Number(item.jitter || 0) * 1000);
        const codec = codecs.get(item.codecId) || '';

        const bytes = Number(item.bytesReceived || 0);
        const ts = Number(item.timestamp || 0);
        let kbps = 0;
        const prev = nextPrev[kind];
        if (prev && ts > prev.ts && bytes >= prev.bytes) {
          kbps = Math.round(((bytes - prev.bytes) * 8) / (ts - prev.ts));
        }
        nextPrev[kind] = { bytes, ts };

        if (kind === 'audio') {
          stats.audioLossRate = lossRate;
          stats.audioJitterMs = jitterMs;
          stats.audioBitrateKbps = kbps;
          if (codec) stats.audioCodec = codec;
        } else if (kind === 'video') {
          stats.videoLossRate = lossRate;
          stats.videoJitterMs = jitterMs;
          stats.videoBitrateKbps = kbps;
          stats.videoFps = Math.round(Number(item.framesPerSecond || 0));
          if (codec) stats.videoCodec = codec;
        }
      }
    }
    this.#prevStats = nextPrev;
    if (!stats.audioRttMs && candidateRttMs > 0) stats.audioRttMs = candidateRttMs;
    return stats;
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
    if (state === SipCallState.Established && !this.#establishedAt) {
      this.#establishedAt = this.#client.now();
      this.#startStatsTimer();
    }
    if (state === SipCallState.Closed) {
      this.#closedAt = this.#client.now();
      this.#stopStatsTimer();
      this.#client._removeCall(this);
      this.#detachStreams();
    }
    this.emit('stateChanged', { state, code, reason });
  }

  #startStatsTimer() {
    this.#stopStatsTimer();
    this.#statsTimer = setInterval(() => {
      if (this.isClosed) {
        this.#stopStatsTimer();
        return;
      }
      this.getStats()
        .then((stats) => {
          if (!this.isClosed) this.emit('stats', stats);
        })
        .catch(() => {});
    }, 2000);
    this.#statsTimer.unref?.();
  }

  #stopStatsTimer() {
    if (this.#statsTimer) {
      clearInterval(this.#statsTimer);
      this.#statsTimer = null;
    }
  }

  #referHandlers() {
    const report = (code, reason) => this.emit('transferStatus', { code, reason });
    return {
      trying: (e) => report(e?.status_line?.status_code || 100, e?.status_line?.reason_phrase || 'Trying'),
      progress: (e) => report(e?.status_line?.status_code || 180, e?.status_line?.reason_phrase || 'Ringing'),
      accepted: (e) => report(e?.status_line?.status_code || 200, e?.status_line?.reason_phrase || 'OK'),
      failed: (e) => report(e?.status_line?.status_code || 500, e?.status_line?.reason_phrase || 'Transfer Failed'),
      requestFailed: (e) => report(
        e?.response?.status_code || 500,
        e?.response?.reason_phrase || e?.cause || 'Transfer Failed',
      ),
    };
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
    s.on('refer', (data) => {
      this.#pendingRefer = data;
      const target = data?.request?.refer_to?.uri?.toString?.() || '';
      this.emit('transferRequest', { target });
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

/** One SIP account hosted by a SipClient, backed by its own JsSIP UA instance. */
export class SipAccount {
  #client;
  #ua;
  #id;
  #aor;
  #user;
  #domain;
  #pcConfig;
  #regState = SipRegistrationState.Unregistered;
  #active = true;

  /** @internal */
  constructor(client, { id, aor, user, domain, pcConfig, ua }) {
    this.#client = client;
    this.#id = id;
    this.#aor = aor;
    this.#user = user;
    this.#domain = domain;
    this.#pcConfig = pcConfig;
    this.#ua = ua;
  }

  get id() { return this.#id; }
  get aor() { return this.#aor; }
  get user() { return this.#user; }
  get domain() { return this.#domain; }
  get regState() { return this.#regState; }
  get isRegistered() { return this.#active && this.#regState === SipRegistrationState.Registered; }

  /** @internal */
  get _ua() { return this.#ua; }
  /** @internal */
  get _pcConfig() { return this.#pcConfig; }
  /** @internal */
  get _active() { return this.#active; }
  /** @internal */
  _setRegState(state) { this.#regState = state; }
  /** @internal */
  _deactivate() {
    this.#active = false;
    this.#regState = SipRegistrationState.Unregistered;
  }

  /** Re-registers this account with its registrar. */
  reregister() {
    if (!this.#active || !this.#ua) throw makeError('SipSdk: account is no longer active', 'EINVAL');
    this.#ua.register();
  }

  /** Removes every binding of this account from its registrar. */
  unregister() {
    if (!this.#active || !this.#ua) return;
    this.#ua.unregister({ all: true });
  }

  /**
   * Places an outgoing call from this account.
   * Accepts options `{ video?: boolean }` or a boolean `withVideo`.
   * @returns {SipCall}
   */
  dial(targetUri, options = {}) {
    return this.#client._makeCallFromAccount(this, targetUri, options);
  }

  /** Alias for dial(). */
  makeCall(targetUri, options = {}) {
    return this.dial(targetUri, options);
  }

  /** Sends a SIP MESSAGE (RFC 3428) from this account. */
  sendMessage(peerUri, body, options = {}) {
    return this.#client._sendMessageFromAccount(this, peerUri, body, options);
  }
}

/** Owns one or more SIP user agents: registration, outgoing calls and incoming call delivery. */
export class SipClient extends Emitter {
  #JsSIP;
  #started = false;
  #accounts = [];
  #defaultAccount = null;
  #nextAccountId = 1;
  #defaultPcConfig = { iceServers: [] };
  #calls = new Set();
  #conferenceCalls = new Set();
  #conferenceMuted = false;
  #audioCtx = null;
  #confGraphNodes = [];
  #nextMessageId = 1;

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
  }

  #maxCalls = 4;
  get maxCalls() { return this.#maxCalls; }
  set maxCalls(value) { this.#maxCalls = Math.max(1, Math.min(8, Number(value) || 4)); }
  get defaultAccount() { return this.#defaultAccount; }
  get accounts() { return [...this.#accounts]; }
  get accountCount() { return this.#accounts.length; }
  get isRegistered() { return this.#defaultAccount ? this.#defaultAccount.isRegistered : false; }
  get accountAor() { return this.#defaultAccount ? this.#defaultAccount.aor : ''; }
  get pcConfig() { return this.#defaultAccount?._pcConfig || this.#defaultPcConfig; }
  set pcConfig(value) { this.#defaultPcConfig = value || { iceServers: [] }; }
  get isStarted() { return this.#started; }
  get calls() { return [...this.#calls]; }
  get conferenceCalls() { return [...this.#conferenceCalls]; }
  /** @internal */
  get _domain() { return this.#defaultAccount ? this.#defaultAccount.domain : ''; }

  start() {
    this.#started = true;
  }

  /** Hangs up every open call, stops every user agent and its WebSocket. */
  stop() {
    for (const call of [...this.#calls]) {
      try { call.hangup(); } catch { /* already closing */ }
    }
    for (const account of [...this.#accounts]) {
      account._deactivate();
      try { account._ua?.stop(); } catch { /* ignore */ }
    }
    this.#accounts = [];
    this.#defaultAccount = null;
    this.#started = false;
  }

  /**
   * Adds and registers a new SIP account (up to SIP_CLIENT_MAX_ACCOUNTS = 8).
   * The first added account becomes the default account.
   * @param {{username?: string, user?: string, domain: string, password?: string, wsUri: string,
   *          displayName?: string, iceServers?: RTCIceServer[], maxCalls?: number}} config
   * @returns {SipAccount}
   */
  addAccount(config) {
    if (!this.#started) throw makeError('SipSdk: call start() before addAccount()', 'EIO');
    return this.#addAccountInternal(config, { makeDefault: false });
  }

  /**
   * Hangs up active calls owned by account, stops/unregisters its UA, removes it, and
   * re-elects defaultAccount to the first remaining account (or null).
   */
  removeAccount(account) {
    const idx = this.#accounts.indexOf(account);
    if (idx === -1) throw makeError('SipSdk: account is not active on this client', 'EINVAL');
    for (const call of [...this.#calls]) {
      if (call.account === account && !call.isClosed) {
        try { call.hangup(); } catch { /* already closing */ }
      }
    }
    account._deactivate();
    try { account._ua?.stop(); } catch { /* ignore */ }
    this.#accounts.splice(idx, 1);
    if (this.#defaultAccount === account) {
      this.#defaultAccount = this.#accounts[0] || null;
    }
  }

  /** Switches the default account used by single-account convenience methods. */
  setDefaultAccount(account) {
    if (!account || !this.#accounts.includes(account)) {
      throw makeError('SipSdk: account is not active on this client', 'EINVAL');
    }
    this.#defaultAccount = account;
  }

  /** Reconnects / re-registers every active account after a transport change. */
  resetTransports(reRegister = true) {
    for (const account of this.#accounts) {
      if (!account._active || !account._ua) continue;
      if (typeof account._ua.isConnected === 'function' && !account._ua.isConnected()) {
        account._ua.start();
      }
      if (reRegister) {
        account.reregister();
      }
    }
  }

  /**
   * Registers or replaces the default SIP account over a SIP WebSocket (ws:// or wss://).
   * @param {{username: string, domain: string, password?: string, wsUri: string,
   *          displayName?: string, iceServers?: RTCIceServer[], maxCalls?: number}} account
   * @returns {SipAccount}
   */
  register(config) {
    if (!this.#started) throw makeError('SipSdk: call start() before register()', 'EIO');
    const validated = this.#validateAccountConfig(config);
    const nextAor = `sip:${validated.username}@${validated.domain}`.toLowerCase();
    if (this.#accounts.some((a) => a !== this.#defaultAccount && a.aor.toLowerCase() === nextAor)) {
      throw makeError(`SipSdk: account sip:${validated.username}@${validated.domain} is already active on this client`, 'EEXIST');
    }
    if (this.#defaultAccount) {
      const old = this.#defaultAccount;
      const idx = this.#accounts.indexOf(old);
      old._deactivate();
      try { old._ua?.stop(); } catch { /* ignore */ }
      if (idx !== -1) this.#accounts.splice(idx, 1);
      this.#defaultAccount = this.#accounts[0] || null;
    }
    if (config.maxCalls !== undefined) this.maxCalls = config.maxCalls;
    else this.maxCalls = 4;
    return this.#addAccountInternal(validated, { makeDefault: true, skipValidation: true });
  }

  #validateAccountConfig(config) {
    if (!config || typeof config !== 'object') {
      throw makeError('SipSdk: account config is required', 'EINVAL');
    }
    const username = config.username ?? config.user ?? '';
    const { domain, password = '', wsUri, displayName = '', iceServers = [], maxCalls } = config;
    if (!username) throw makeError('SipSdk: username is required', 'EINVAL');
    if (!domain) throw makeError('SipSdk: domain is required', 'EINVAL');
    if (!wsUri) throw makeError('SipSdk: wsUri is required', 'EINVAL');
    return { username, domain, password, wsUri, displayName, iceServers, maxCalls };
  }

  #addAccountInternal(config, { makeDefault = false, skipValidation = false } = {}) {
    const { username, domain, password, wsUri, displayName, iceServers, maxCalls } = skipValidation
      ? config
      : this.#validateAccountConfig(config);

    const aor = `sip:${username}@${domain}`;
    if (this.#accounts.some((a) => a.aor.toLowerCase() === aor.toLowerCase())) {
      throw makeError(`SipSdk: account ${aor} is already active on this client`, 'EEXIST');
    }
    if (this.#accounts.length >= SIP_CLIENT_MAX_ACCOUNTS) {
      throw makeError(`SipSdk: maximum accounts (${SIP_CLIENT_MAX_ACCOUNTS}) reached`, 'EBUSY');
    }
    if (!skipValidation && maxCalls !== undefined) {
      this.maxCalls = maxCalls;
    }

    const pcConfig = { iceServers };
    const ua = new this.#JsSIP.UA({
      sockets: [new this.#JsSIP.WebSocketInterface(wsUri)],
      uri: aor,
      password: password || undefined,
      display_name: displayName || undefined,
      session_timers: true,
      register: true,
    });

    const account = new SipAccount(this, {
      id: this.#nextAccountId++,
      aor,
      user: username,
      domain,
      pcConfig,
      ua,
    });
    this.#accounts.push(account);
    if (makeDefault || !this.#defaultAccount) {
      this.#defaultAccount = account;
    }

    const report = (state, e) => {
      if (!account._active) return;
      account._setRegState(state);
      const code = e?.response?.status_code || 0;
      const reason = e?.response?.reason_phrase || e?.cause || '';
      this.emit('accountRegistrationStateChanged', { account, state, code, reason });
      if (this.#defaultAccount === account) {
        this.emit('registrationStateChanged', { state, code, reason });
      }
    };
    ua.on('registered', (e) => report(SipRegistrationState.Registered, e));
    ua.on('unregistered', (e) => report(SipRegistrationState.Unregistered, e));
    ua.on('registrationFailed', (e) => report(SipRegistrationState.Failed, e));
    ua.on('newRTCSession', (e) => {
      if (!account._active || e.originator !== 'remote') return;
      if (this.#openCalls().length >= this.#maxCalls) {
        try {
          e.session.terminate({ status_code: 486, reason_phrase: 'Busy Here' });
        } catch { /* ignore */ }
        const callerUri = e.session?.remote_identity?.uri?.toString() || '';
        this.emit('callRejectedBusy', { callerUri });
        return;
      }
      const call = new SipCall(this, e.session, {
        outgoing: false,
        video: offerHasVideo(e.request?.body),
        account,
      });
      this.#calls.add(call);
      this.emit('incomingCall', { call });
    });
    ua.on('newMessage', (e) => {
      if (!account._active || e.originator !== 'remote') return;
      const contentType = (e.request?.getHeader?.('Content-Type') || 'text/plain').split(';')[0].trim();
      const from = e.message?.remote_identity?.uri?.toString() || '';
      const body = e.request?.body || '';
      this.emit('accountMessageReceived', { account, from, fromUri: from, contentType, body });
      this.emit('messageReceived', { from, contentType, body });
    });

    account._setRegState(SipRegistrationState.Registering);
    this.emit('accountRegistrationStateChanged', {
      account,
      state: SipRegistrationState.Registering,
      code: 0,
      reason: '',
    });
    if (this.#defaultAccount === account) {
      this.emit('registrationStateChanged', { state: SipRegistrationState.Registering, code: 0, reason: '' });
    }
    ua.start();
    return account;
  }

  /** Removes every binding of the default account from the registrar. */
  unregister() {
    this.#defaultAccount?.unregister();
  }

  /**
   * Places a call from the default account. target is an extension ("9001"), user@host, or a sip: URI.
   * @returns {SipCall}
   */
  makeCall(target, options = {}) {
    if (!this.#started) throw new Error('SipSdk: call start() before makeCall()');
    if (!this.#defaultAccount || !this.#defaultAccount.isRegistered) {
      throw new Error('SipSdk: register an account before makeCall()');
    }
    return this._makeCallFromAccount(this.#defaultAccount, target, options);
  }

  /** Alias for makeCall(). */
  dial(target, options = {}) {
    return this.makeCall(target, options);
  }

  /** @internal */
  _makeCallFromAccount(account, target, options = {}) {
    if (!this.#started) throw new Error('SipSdk: call start() before makeCall()');
    if (!account || !account._active || !account.isRegistered) {
      throw new Error('SipSdk: register an account before makeCall()');
    }
    if (!target) throw new Error('SipSdk: target is required');
    if (this.#openCalls().length >= this.#maxCalls) {
      const err = new Error('SipSdk: maximum concurrent calls reached (Busy)');
      err.code = 'EBUSY';
      err.statusCode = 486;
      throw err;
    }
    const { video = false } = typeof options === 'boolean' ? { video: options } : (options || {});
    this._autoHoldOthers(null);
    const session = account._ua.call(toSipUri(target, account.domain), {
      mediaConstraints: { audio: true, video: video ? VIDEO_CONSTRAINTS : false },
      pcConfig: account._pcConfig,
    });
    const call = new SipCall(this, session, { outgoing: true, video: !!video, account });
    this.#calls.add(call);
    return call;
  }

  #openCalls() {
    return [...this.#calls].filter((c) => !c.isClosed);
  }

  /** @internal */
  _autoHoldOthers(exceptCall) {
    const exempt = new Set();
    if (exceptCall) exempt.add(exceptCall);
    this._autoHoldOutside(exempt);
  }

  /** @internal */
  _autoHoldOutside(exemptSet) {
    for (const call of this.#calls) {
      if (!exemptSet.has(call) && !call.isClosed && !call.isOnHold && call.state === SipCallState.Established) {
        call._setHoldDirect(true);
      }
    }
    this.#rebuildConferenceAudio();
  }

  /** Merges two open calls into the local audio conference (or adds the non-conference call to an active conference). */
  mergeCalls(callA, callB) {
    if (!callA || !callB || callA === callB || callA.isClosed || callB.isClosed) {
      throw new Error('SipSdk: mergeCalls requires two distinct open calls');
    }
    this.#conferenceCalls.add(callA);
    this.#conferenceCalls.add(callB);
    callA._setInConference(true);
    callB._setInConference(true);

    this._autoHoldOutside(this.#conferenceCalls);
    for (const c of this.#conferenceCalls) {
      if (c.isOnHold) c._setHoldDirect(false);
      if (c.hasVideo) c.setLocalVideoEnabled(false);
    }
    this.#rebuildConferenceAudio();
    this.emit('conferenceChanged', { calls: this.conferenceCalls });
  }

  /** Adds an open call to an already active local audio conference. */
  addToConference(call) {
    if (!call || call.isClosed || this.#conferenceCalls.size < 2) {
      throw new Error('SipSdk: addToConference requires an active conference and an open call');
    }
    this.#conferenceCalls.add(call);
    call._setInConference(true);
    this._autoHoldOutside(this.#conferenceCalls);
    for (const c of this.#conferenceCalls) {
      if (c.isOnHold) c._setHoldDirect(false);
      if (c.hasVideo) c.setLocalVideoEnabled(false);
    }
    this.#rebuildConferenceAudio();
    this.emit('conferenceChanged', { calls: this.conferenceCalls });
  }

  /** Removes a call from the conference and places it on hold; dissolves if 1 leg remains. */
  splitCall(call) {
    if (!call || !this.#conferenceCalls.has(call)) {
      throw new Error('SipSdk: call is not in the conference');
    }
    this.#conferenceCalls.delete(call);
    call._setInConference(false);
    call._restoreOriginalAudioTrack();
    if (!call.isClosed) call._setHoldDirect(true);

    if (this.#conferenceCalls.size === 1) {
      for (const rem of this.#conferenceCalls) {
        rem._setInConference(false);
        rem._restoreOriginalAudioTrack();
      }
      this.#conferenceCalls.clear();
    }
    this.#rebuildConferenceAudio();
    this.emit('conferenceChanged', { calls: this.conferenceCalls });
  }

  /** Ends all calls currently in the local audio conference. */
  leaveConference() {
    const members = [...this.#conferenceCalls];
    this.#conferenceCalls.clear();
    this.#conferenceMuted = false;
    for (const c of members) {
      c._setInConference(false);
      c._restoreOriginalAudioTrack();
    }
    this.#teardownConferenceAudio();
    for (const c of members) {
      if (!c.isClosed) {
        try { c.hangup(); } catch { /* ignore */ }
      }
    }
    this.emit('conferenceChanged', { calls: [] });
  }

  /** @internal */
  _setConferenceHold(hold) {
    if (!hold) {
      this._autoHoldOutside(this.#conferenceCalls);
      for (const c of this.#conferenceCalls) {
        if (!c.isClosed) c._setHoldDirect(false);
      }
    } else {
      for (const c of this.#conferenceCalls) {
        if (!c.isClosed) c._setHoldDirect(true);
      }
    }
    this.#rebuildConferenceAudio();
  }

  /** @internal */
  _setConferenceLocalMuted(muted) {
    this.#conferenceMuted = !!muted;
    for (const c of this.#conferenceCalls) {
      if (!c.isClosed) c._setMutedFlag(this.#conferenceMuted);
    }
    this.#rebuildConferenceAudio();
  }

  #teardownConferenceAudio() {
    for (const node of this.#confGraphNodes) {
      try { node.disconnect?.(); } catch { /* ignore */ }
    }
    this.#confGraphNodes = [];
    if (this.#audioCtx) {
      this.#audioCtx.close?.().catch?.(() => {});
      this.#audioCtx = null;
    }
  }

  #rebuildConferenceAudio() {
    const active = [...this.#conferenceCalls].filter((c) => !c.isClosed && !c.isOnHold);
    if (active.length < 2) {
      this.#teardownConferenceAudio();
      return;
    }
    const AudioCtx = globalThis.AudioContext || globalThis.webkitAudioContext;
    if (typeof AudioCtx !== 'function' || typeof MediaStream === 'undefined') {
      return;
    }
    for (const node of this.#confGraphNodes) {
      try { node.disconnect?.(); } catch { /* ignore */ }
    }
    this.#confGraphNodes = [];
    if (!this.#audioCtx) {
      this.#audioCtx = new AudioCtx();
    }
    const ctx = this.#audioCtx;
    ctx.resume?.().catch?.(() => {});

    const rxSources = new Map();
    for (const c of active) {
      const audioTracks = c.remoteStream?.getAudioTracks?.() || [];
      if (audioTracks.length > 0) {
        const src = ctx.createMediaStreamSource(new MediaStream(audioTracks));
        rxSources.set(c, src);
        this.#confGraphNodes.push(src);
      }
    }

    for (const target of active) {
      const sender = target._getAudioSender();
      const origTrack = target._getOriginalAudioTrack();
      if (!sender?.replaceTrack || !origTrack) continue;

      const dest = ctx.createMediaStreamDestination();
      const localSrc = ctx.createMediaStreamSource(new MediaStream([origTrack]));
      const localGain = ctx.createGain();
      localGain.gain.value = this.#conferenceMuted ? 0 : 1;
      localSrc.connect(localGain);
      localGain.connect(dest);
      this.#confGraphNodes.push(localSrc, localGain, dest);

      for (const [peer, peerSrc] of rxSources.entries()) {
        if (peer === target) continue; // Mix-minus: never send peer its own RX audio back
        peerSrc.connect(dest);
      }

      const mixedTrack = dest.stream.getAudioTracks()[0];
      if (mixedTrack) {
        Promise.resolve(sender.replaceTrack(mixedTrack)).catch(() => {});
      }
    }
  }

  /**
   * Sends a SIP MESSAGE (RFC 3428) from the default account.
   * target is an extension ("1002"), user@host, or a sip: URI.
   * The result arrives as a 'messageStatusChanged' event { id, status, code, reason }.
   * @param {string} target
   * @param {string} body text, or a serialized body for other content types
   * @param {{contentType?: string}} [options] MIME type, default "text/plain"
   * @returns {number} local message id
   */
  sendMessage(target, body, options = {}) {
    if (!this.#started) throw new Error('SipSdk: call start() before sendMessage()');
    if (!this.#defaultAccount || !this.#defaultAccount.isRegistered) {
      throw new Error('SipSdk: register an account before sendMessage()');
    }
    return this._sendMessageFromAccount(this.#defaultAccount, target, body, options);
  }

  /** @internal */
  _sendMessageFromAccount(account, target, body, { contentType = 'text/plain' } = {}) {
    if (!this.#started) throw new Error('SipSdk: call start() before sendMessage()');
    if (!account || !account._active || !account.isRegistered) {
      throw new Error('SipSdk: register an account before sendMessage()');
    }
    if (!target) throw new Error('SipSdk: target is required');
    if (!body) throw new Error('SipSdk: message body is required');
    if (!CONTENT_TYPE.test(contentType)) throw new Error(`SipSdk: invalid content type "${contentType}"`);

    const id = this.#nextMessageId++;
    const report = (status, e) => {
      const code = e?.response?.status_code || 0;
      const reason = e?.response?.reason_phrase || e?.cause || '';
      this.emit('messageStatusChanged', { id, status, code, reason });
    };
    account._ua.sendMessage(toSipUri(target, account.domain), body, {
      contentType,
      eventHandlers: {
        succeeded: (e) => report(SipMessageStatus.Sent, e),
        failed: (e) => report(SipMessageStatus.Failed, e),
      },
    });
    return id;
  }

  /** @internal */
  _removeCall(call) {
    this.#calls.delete(call);
    if (this.#conferenceCalls.has(call)) {
      this.#conferenceCalls.delete(call);
      call._setInConference(false);
      if (this.#conferenceCalls.size === 1) {
        for (const rem of this.#conferenceCalls) {
          rem._setInConference(false);
          rem._restoreOriginalAudioTrack();
        }
        this.#conferenceCalls.clear();
      }
      this.#rebuildConferenceAudio();
      this.emit('conferenceChanged', { calls: this.conferenceCalls });
    }
  }
}
