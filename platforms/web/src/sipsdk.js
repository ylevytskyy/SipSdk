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

export const SipRecordingMode = Object.freeze({
  AudioStereo: 0,
  AudioMono: 1,
  AudioVideo: 2,
});

export const SipRecordingState = Object.freeze({
  Idle: 0,
  Recording: 1,
  Failed: 2,
});

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

/** One SIP call session. Created by SipClient.makeCall or delivered by incomingCall. */
export class SipCall extends Emitter {
  #client;
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
  #recorder = null;
  #recorderCtx = null;
  #recordChunks = [];
  #recordingMode = null;
  #recordingStopPromise = null;

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
  get isInConference() { return this.#inConference; }
  get isRecording() { return this.#recorder !== null; }
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

  /**
   * Starts local browser recording using MediaRecorder + Web Audio API.
   * AudioStereo maps local mic -> left channel and remote audio -> right channel.
   */
  startRecording({ mode = SipRecordingMode.AudioStereo, mimeType } = {}) {
    this.#assertOpen();
    if (this.#recorder) throw new Error('SipSdk: call is already recording');
    const MediaRecorderCtor = globalThis.MediaRecorder;
    if (!MediaRecorderCtor) throw new Error('SipSdk: MediaRecorder is not available');

    const tracks = [];
    const AudioCtx = globalThis.AudioContext || globalThis.webkitAudioContext;
    if (AudioCtx && (this.#localStream || this.#remoteStream)) {
      const ctx = new AudioCtx();
      const dest = ctx.createMediaStreamDestination();
      if (mode === SipRecordingMode.AudioMono) {
        if (this.#localStream?.getAudioTracks?.().length) {
          ctx.createMediaStreamSource(new MediaStream(this.#localStream.getAudioTracks())).connect(dest);
        }
        if (this.#remoteStream?.getAudioTracks?.().length) {
          ctx.createMediaStreamSource(new MediaStream(this.#remoteStream.getAudioTracks())).connect(dest);
        }
      } else {
        const merger = ctx.createChannelMerger(2);
        merger.connect(dest);
        if (this.#localStream?.getAudioTracks?.().length) {
          ctx.createMediaStreamSource(new MediaStream(this.#localStream.getAudioTracks())).connect(merger, 0, 0);
        }
        if (this.#remoteStream?.getAudioTracks?.().length) {
          ctx.createMediaStreamSource(new MediaStream(this.#remoteStream.getAudioTracks())).connect(merger, 0, 1);
        }
      }
      for (const t of dest.stream.getAudioTracks()) tracks.push(t);
      this.#recorderCtx = ctx;
    } else if (this.#remoteStream?.getAudioTracks?.().length) {
      tracks.push(...this.#remoteStream.getAudioTracks());
    } else if (this.#localStream?.getAudioTracks?.().length) {
      tracks.push(...this.#localStream.getAudioTracks());
    }

    if (mode === SipRecordingMode.AudioVideo && this.#remoteStream?.getVideoTracks?.().length) {
      tracks.push(...this.#remoteStream.getVideoTracks());
    }
    if (tracks.length === 0) {
      if (this.#recorderCtx) {
        this.#recorderCtx.close?.().catch?.(() => {});
        this.#recorderCtx = null;
      }
      throw new Error('SipSdk: no active media tracks available to record');
    }

    const combined = new MediaStream(tracks);
    const options = {};
    if (mimeType) options.mimeType = mimeType;
    const rec = new MediaRecorderCtor(combined, options);
    this.#recordChunks = [];
    this.#recordingMode = mode;
    this.#recorder = rec;
    rec.ondataavailable = (ev) => {
      if (ev?.data && ev.data.size > 0) this.#recordChunks.push(ev.data);
    };
    rec.start(250);
    this.emit('recordingStateChanged', { call: this, state: SipRecordingState.Recording, errorCode: 0 });
    this.#client.emit('recordingStateChanged', { call: this, state: SipRecordingState.Recording, errorCode: 0 });
  }

  /** Stops recording and resolves with the recorded Blob (or null if idle). */
  stopRecording() {
    if (!this.#recorder) return Promise.resolve(null);
    if (this.#recordingStopPromise) return this.#recordingStopPromise;
    const rec = this.#recorder;
    const ctx = this.#recorderCtx;
    this.#recorder = null;
    this.#recorderCtx = null;
    this.#recordingStopPromise = new Promise((resolve) => {
      const finish = () => {
        const type = rec.mimeType || (this.#recordingMode === SipRecordingMode.AudioVideo ? 'video/webm' : 'audio/webm');
        const blob = new Blob(this.#recordChunks, { type });
        this.#recordChunks = [];
        this.#recordingMode = null;
        this.#recordingStopPromise = null;
        if (ctx) ctx.close?.().catch?.(() => {});
        this.emit('recordingStateChanged', { call: this, state: SipRecordingState.Idle, blob, errorCode: 0 });
        this.#client.emit('recordingStateChanged', { call: this, state: SipRecordingState.Idle, blob, errorCode: 0 });
        resolve(blob);
      };
      rec.onstop = finish;
      try {
        if (rec.state !== 'inactive') rec.stop();
        else finish();
      } catch {
        finish();
      }
    });
    return this.#recordingStopPromise;
  }

  /** Blind call transfer (SIP REFER) to target extension, user@host, or sip: URI. */
  transfer(target) {
    this.#assertOpen();
    if (!target) throw new Error('SipSdk: transfer target is required');
    if (typeof this.#session.refer !== 'function') throw new Error('SipSdk: session does not support REFER');
    const ok = this.#session.refer(toSipUri(target, this.#client._domain), {
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
      this.stopRecording();
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

/** Owns the SIP user agent: registration, outgoing calls and incoming call delivery. */
export class SipClient extends Emitter {
  #JsSIP;
  #started = false;
  #ua = null;
  #registered = false;
  #domain = '';
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
    this.accountAor = '';
    this.pcConfig = { iceServers: [] };
  }

  #maxCalls = 4;
  get maxCalls() { return this.#maxCalls; }
  set maxCalls(value) { this.#maxCalls = Math.max(1, Math.min(8, Number(value) || 4)); }
  get isRegistered() { return this.#registered; }
  get isStarted() { return this.#started; }
  get calls() { return [...this.#calls]; }
  get conferenceCalls() { return [...this.#conferenceCalls]; }
  /** @internal */
  get _domain() { return this.#domain; }

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
   *          displayName?: string, iceServers?: RTCIceServer[], maxCalls?: number}} account
   */
  register({ username, domain, password = '', wsUri, displayName = '', iceServers = [], maxCalls = 4 }) {
    if (!this.#started) throw new Error('SipSdk: call start() before register()');
    if (!username) throw new Error('SipSdk: username is required');
    if (!domain) throw new Error('SipSdk: domain is required');
    if (!wsUri) throw new Error('SipSdk: wsUri is required');
    if (this.#ua) this.#ua.stop();

    this.maxCalls = maxCalls;
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
      if (this.#openCalls().length >= this.#maxCalls) {
        try {
          e.session.terminate({ status_code: 486, reason_phrase: 'Busy Here' });
        } catch { /* ignore */ }
        const callerUri = e.session?.remote_identity?.uri?.toString() || '';
        this.emit('callRejectedBusy', { callerUri });
        return;
      }
      const call = new SipCall(this, e.session, { outgoing: false, video: offerHasVideo(e.request?.body) });
      this.#calls.add(call);
      this.emit('incomingCall', { call });
    });
    ua.on('newMessage', (e) => {
      if (this.#ua !== ua || e.originator !== 'remote') return;
      const contentType = (e.request?.getHeader?.('Content-Type') || 'text/plain').split(';')[0].trim();
      this.emit('messageReceived', {
        from: e.message?.remote_identity?.uri?.toString() || '',
        contentType,
        body: e.request?.body || '',
      });
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
    if (this.#openCalls().length >= this.#maxCalls) {
      const err = new Error('SipSdk: maximum concurrent calls reached (Busy)');
      err.code = 'EBUSY';
      err.statusCode = 486;
      throw err;
    }
    this._autoHoldOthers(null);
    const session = this.#ua.call(toSipUri(target, this.#domain), {
      mediaConstraints: { audio: true, video: video ? VIDEO_CONSTRAINTS : false },
      pcConfig: this.pcConfig,
    });
    const call = new SipCall(this, session, { outgoing: true, video: !!video });
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
   * Sends a SIP MESSAGE (RFC 3428). target is an extension ("1002"), user@host, or a sip: URI.
   * The result arrives as a 'messageStatusChanged' event { id, status, code, reason }.
   * @param {string} target
   * @param {string} body text, or a serialized body for other content types
   * @param {{contentType?: string}} [options] MIME type, default "text/plain"
   * @returns {number} local message id
   */
  sendMessage(target, body, { contentType = 'text/plain' } = {}) {
    if (!this.#started) throw new Error('SipSdk: call start() before sendMessage()');
    if (!this.#ua || !this.#registered) throw new Error('SipSdk: register an account before sendMessage()');
    if (!target) throw new Error('SipSdk: target is required');
    if (!body) throw new Error('SipSdk: message body is required');
    if (!CONTENT_TYPE.test(contentType)) throw new Error(`SipSdk: invalid content type "${contentType}"`);

    const id = this.#nextMessageId++;
    const report = (status, e) => {
      const code = e?.response?.status_code || 0;
      const reason = e?.response?.reason_phrase || e?.cause || '';
      this.emit('messageStatusChanged', { id, status, code, reason });
    };
    this.#ua.sendMessage(toSipUri(target, this.#domain), body, {
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
