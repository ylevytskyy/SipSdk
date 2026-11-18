import { SipClient, SipCallState, SipRegistrationState } from '../src/sipsdk.js';

const $ = (id) => document.getElementById(id);
const REG_NAMES = Object.fromEntries(Object.entries(SipRegistrationState).map(([k, v]) => [v, k]));
const CALL_NAMES = Object.fromEntries(Object.entries(SipCallState).map(([k, v]) => [v, k]));

const log = (msg) => {
  const el = $('log');
  el.textContent += `[${new Date().toLocaleTimeString()}] ${msg}\n`;
  el.scrollTop = el.scrollHeight;
};

// Default to the local Asterisk PBX (extension 1002, WebRTC endpoint).
const pbxHost = location.hostname || '127.0.0.1';
const params = new URLSearchParams(location.search);
$('domain').value = params.get('domain') || pbxHost;
$('wsUri').value = params.get('ws') || `ws://${pbxHost}:8088/ws`;

const client = new SipClient();
client.start();
window.sipClient = client; // exposed for automated checks

let call = null;
let durationTimer = null;

client.on('registrationStateChanged', ({ state, code, reason }) => {
  const el = $('regStatus');
  el.dataset.state = String(state);
  el.className = 'status' + (state === SipRegistrationState.Registered ? ' ok' : state === SipRegistrationState.Failed ? ' err' : '');
  el.textContent = REG_NAMES[state] + (code ? ` (${code} ${reason})` : reason ? ` (${reason})` : '');
  const registered = state === SipRegistrationState.Registered;
  $('btnCall').disabled = !registered || !!call;
  $('btnUnregister').disabled = !registered;
  $('btnRegister').disabled = registered || state === SipRegistrationState.Registering;
  log(`Registration: ${el.textContent}`);
});

client.on('incomingCall', ({ call: incoming }) => {
  if (call) {
    log(`Busy: declining ${incoming.remoteUri}`);
    incoming.hangup(486, 'Busy Here');
    return;
  }
  log(`Incoming call from ${incoming.peerDisplayName || ''} ${incoming.remoteUri}${incoming.hasVideo ? ' (video)' : ''}`);
  $('incomingFrom').textContent = `From: ${incoming.peerDisplayName ? incoming.peerDisplayName + ' ' : ''}<${incoming.remoteUri}>`;
  $('incoming').hidden = false;
  bindCall(incoming);
});

function bindCall(c) {
  call = c;
  $('btnCall').disabled = true;
  $('callPeer').textContent = c.peerDisplayName || c.remoteUri;
  $('dtmfReceived').textContent = '—';
  c.attachMedia({ localVideo: $('localVideo'), remoteVideo: $('remoteVideo'), remoteAudio: $('remoteAudio') });
  showState(c.state, 0, '');

  c.on('stateChanged', ({ state, code, reason }) => {
    showState(state, code, reason);
    log(`Call ${CALL_NAMES[state]}${code ? ` ${code}` : ''}${reason ? ` ${reason}` : ''}`);
    if (state === SipCallState.Established || state === SipCallState.Hold) {
      $('incoming').hidden = true;
      $('incall').hidden = false;
      $('btnHold').textContent = state === SipCallState.Hold ? 'Resume' : 'Hold';
    }
    if (state === SipCallState.Closed) endCall();
  });
  c.on('mediaChanged', ({ width, height, isOnHold, isMuted }) => {
    if (width && height) log(`Remote video ${width}x${height}`);
    $('btnMute').textContent = isMuted ? 'Unmute' : 'Mute';
    $('btnHold').textContent = isOnHold ? 'Resume' : 'Hold';
  });
  c.on('dtmfReceived', ({ digit }) => {
    $('dtmfReceived').textContent = digit;
    log(`DTMF received: ${digit}`);
  });
  c.on('stats', (s) => {
    const lock = c.isSecure ? 'SRTP/DTLS' : 'Unencrypted';
    const codec = s.audioCodec || '—';
    const vCodec = s.videoCodec ? ` · ${s.videoCodec} ${s.videoBitrateKbps} kbps ${s.videoFps} fps` : '';
    $('callStats').textContent = `${lock} · ${codec} ${s.audioBitrateKbps} kbps · RTT ${s.audioRttMs} ms · Jitter ${s.audioJitterMs} ms · Loss ${(s.audioLossRate * 100).toFixed(1)}%${vCodec}`;
  });
  c.on('transferStatus', ({ code, reason }) => {
    log(`Transfer status: ${code} ${reason}`);
  });
  c.on('transferRequest', ({ target }) => {
    log(`Incoming transfer request to ${target} (accepting)`);
    try { c.acceptTransfer(true); } catch (err) { log(err.message); }
  });

  if (c.isOutgoing) $('incall').hidden = false;
  durationTimer = setInterval(() => {
    const d = c.duration;
    $('callDuration').textContent = `${Math.floor(d / 60)}:${String(d % 60).padStart(2, '0')}`;
  }, 500);
}

function showState(state, code, reason) {
  const el = $('callState');
  el.dataset.state = String(state);
  el.textContent = CALL_NAMES[state] + (code ? ` (${code}${reason ? ' ' + reason : ''})` : '');
}

function endCall() {
  clearInterval(durationTimer);
  call = null;
  $('incoming').hidden = true;
  $('incall').hidden = true;
  $('btnCall').disabled = !client.isRegistered;
  $('btnMute').textContent = 'Mute';
  $('btnVideo').textContent = 'Video off';
  $('btnHold').textContent = 'Hold';
  $('callDuration').textContent = '0:00';
  $('callStats').textContent = 'Secure: SRTP/DTLS · Waiting for stats...';
}

const guard = (fn) => () => {
  try { fn(); } catch (err) { log(err.message); }
};

$('btnRegister').onclick = guard(() => client.register({
  username: $('username').value.trim(),
  domain: $('domain').value.trim(),
  password: $('password').value,
  wsUri: $('wsUri').value.trim(),
  displayName: $('displayName').value.trim(),
}));
$('btnUnregister').onclick = guard(() => client.unregister());
$('btnCall').onclick = guard(() => {
  const target = $('target').value.trim();
  const video = $('withVideo').checked;
  log(`Calling ${target}${video ? ' with video' : ''}`);
  bindCall(client.makeCall(target, { video }));
});
$('btnAnswer').onclick = guard(() => call.answer({ video: $('withVideo').checked }));
$('btnAnswerAudio').onclick = guard(() => call.answer({ video: false }));
$('btnDecline').onclick = guard(() => call.hangup(603, 'Decline'));
$('btnHangup').onclick = guard(() => call.hangup());
$('btnMute').onclick = guard(() => call.setMuted(!call.isMuted));
$('btnHold').onclick = guard(() => call.setHold(!call.isOnHold));
$('btnTransfer').onclick = guard(() => {
  const target = $('transferTarget').value.trim();
  call.transfer(target);
  log(`Blind transfer requested -> ${target}`);
});
$('btnVideo').onclick = guard(() => {
  const enable = !call.isLocalVideoEnabled;
  call.setLocalVideoEnabled(enable);
  $('btnVideo').textContent = enable ? 'Video off' : 'Video on';
});
document.querySelectorAll('[data-dtmf]').forEach((b) => {
  b.onclick = guard(() => {
    call.sendDtmf(b.dataset.dtmf);
    log(`DTMF sent: ${b.dataset.dtmf}`);
  });
});

window.addEventListener('beforeunload', () => client.stop());
log('Sample ready. Press Register.');
