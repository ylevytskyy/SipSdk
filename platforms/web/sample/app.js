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
let incomingCall = null;
let durationTimer = null;
let busyTimer = null;
const boundCallIds = new Set();

function showBusyBanner(msg) {
  const el = $('busyBanner');
  el.textContent = msg;
  el.hidden = false;
  clearTimeout(busyTimer);
  busyTimer = setTimeout(() => { el.hidden = true; }, 4000);
}

function updateCallButtonState() {
  $('btnCall').disabled = !client.isRegistered || client.calls.length >= client.maxCalls;
}

function renderLines() {
  const container = $('callLines');
  const open = client.calls.filter((c) => c.state !== SipCallState.Closed);
  const swapBtn = $('btnSwap');
  container.innerHTML = '';
  if (open.length <= 1) {
    container.hidden = true;
    swapBtn.hidden = true;
    return;
  }
  container.hidden = false;
  swapBtn.hidden = false;
  for (const line of open) {
    const btn = document.createElement('button');
    btn.className = line === call ? '' : 'secondary';
    btn.textContent = `${line.peerDisplayName || line.remoteUri} · ${line.isOnHold ? 'On Hold' : 'Active'}`;
    btn.onclick = () => switchToCall(line);
    container.appendChild(btn);
  }
}

function focusCall(c) {
  call = c;
  $('callPeer').textContent = c.peerDisplayName || c.remoteUri;
  $('dtmfReceived').textContent = '—';
  c.attachMedia({ localVideo: $('localVideo'), remoteVideo: $('remoteVideo'), remoteAudio: $('remoteAudio') });
  showState(c.state, 0, '');
  $('btnMute').textContent = c.isMuted ? 'Unmute' : 'Mute';
  $('btnHold').textContent = (c.isOnHold || c.state === SipCallState.Hold) ? 'Resume' : 'Hold';
  $('btnVideo').textContent = c.isLocalVideoEnabled ? 'Video off' : 'Video on';
  clearInterval(durationTimer);
  durationTimer = setInterval(() => {
    if (!call) return;
    const d = call.duration;
    $('callDuration').textContent = `${Math.floor(d / 60)}:${String(d % 60).padStart(2, '0')}`;
  }, 500);
  renderLines();
}

function switchToCall(target) {
  if (!target || target.state === SipCallState.Closed) return;
  try {
    if (target.isOnHold) target.setHold(false);
    focusCall(target);
  } catch (err) {
    log(err.message);
  }
}

client.on('registrationStateChanged', ({ state, code, reason }) => {
  const el = $('regStatus');
  el.dataset.state = String(state);
  el.className = 'status' + (state === SipRegistrationState.Registered ? ' ok' : state === SipRegistrationState.Failed ? ' err' : '');
  el.textContent = REG_NAMES[state] + (code ? ` (${code} ${reason})` : reason ? ` (${reason})` : '');
  const registered = state === SipRegistrationState.Registered;
  updateCallButtonState();
  $('btnUnregister').disabled = !registered;
  $('btnRegister').disabled = registered || state === SipRegistrationState.Registering;
  log(`Registration: ${el.textContent}`);
});

client.on('callRejectedBusy', ({ callerUri }) => {
  const msg = `Busy: rejected incoming call from ${callerUri}`;
  log(msg);
  showBusyBanner(msg);
});

client.on('incomingCall', ({ call: incoming }) => {
  incomingCall = incoming;
  log(`Incoming call from ${incoming.peerDisplayName || ''} ${incoming.remoteUri}${incoming.hasVideo ? ' (video)' : ''}`);
  $('incomingFrom').textContent = `From: ${incoming.peerDisplayName ? incoming.peerDisplayName + ' ' : ''}<${incoming.remoteUri}>`;
  $('incoming').hidden = false;
  bindCall(incoming);
});

function bindCall(c) {
  if (!boundCallIds.has(c.id)) {
    boundCallIds.add(c.id);
    c.on('stateChanged', ({ state, code, reason }) => {
      if (c === call) showState(state, code, reason);
      log(`Call ${CALL_NAMES[state]}${code ? ` ${code}` : ''}${reason ? ` ${reason}` : ''}`);
      if (state === SipCallState.Established || state === SipCallState.Hold) {
        if (incomingCall === c) {
          incomingCall = null;
          $('incoming').hidden = true;
        }
        if (state === SipCallState.Established) {
          focusCall(c);
        }
        $('incall').hidden = false;
        if (c === call) $('btnHold').textContent = state === SipCallState.Hold ? 'Resume' : 'Hold';
        renderLines();
      }
      if (state === SipCallState.Closed) {
        if (incomingCall === c) {
          incomingCall = null;
          $('incoming').hidden = true;
        }
        const remaining = client.calls.filter((x) => x !== c && x.state !== SipCallState.Closed);
        if (remaining.length > 0) {
          if (c === call) {
            focusCall(remaining.find((x) => !x.isOnHold) || remaining[0]);
          } else {
            renderLines();
          }
          updateCallButtonState();
        } else {
          endCall();
        }
      }
    });
    c.on('mediaChanged', ({ width, height, isOnHold, isMuted }) => {
      if (width && height) log(`Remote video ${width}x${height}`);
      if (c === call) {
        $('btnMute').textContent = isMuted ? 'Unmute' : 'Mute';
        $('btnHold').textContent = isOnHold ? 'Resume' : 'Hold';
      }
      renderLines();
    });
    c.on('dtmfReceived', ({ digit }) => {
      if (c === call) $('dtmfReceived').textContent = digit;
      log(`DTMF received: ${digit}`);
    });
    c.on('stats', (s) => {
      if (c !== call) return;
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
  }

  if (c.isOutgoing || !call) {
    focusCall(c);
    if (c.isOutgoing) $('incall').hidden = false;
  } else {
    renderLines();
  }
  updateCallButtonState();
}

function showState(state, code, reason) {
  const el = $('callState');
  el.dataset.state = String(state);
  el.textContent = CALL_NAMES[state] + (code ? ` (${code}${reason ? ' ' + reason : ''})` : '');
}

function endCall() {
  clearInterval(durationTimer);
  call = null;
  incomingCall = null;
  $('incoming').hidden = true;
  $('incall').hidden = true;
  $('callLines').hidden = true;
  $('btnSwap').hidden = true;
  updateCallButtonState();
  $('btnMute').textContent = 'Mute';
  $('btnVideo').textContent = 'Video off';
  $('btnHold').textContent = 'Hold';
  $('callDuration').textContent = '0:00';
  $('callStats').textContent = 'Secure: SRTP/DTLS · Waiting for stats...';
}

const guard = (fn) => () => {
  try { fn(); } catch (err) {
    log(err.message);
    if (err.code === 'EBUSY') showBusyBanner(err.message);
  }
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
$('btnAnswer').onclick = guard(() => {
  const target = incomingCall || call;
  incomingCall = null;
  target.answer({ video: $('withVideo').checked });
  focusCall(target);
});
$('btnAnswerAudio').onclick = guard(() => {
  const target = incomingCall || call;
  incomingCall = null;
  target.answer({ video: false });
  focusCall(target);
});
$('btnDecline').onclick = guard(() => {
  const target = incomingCall || call;
  incomingCall = null;
  $('incoming').hidden = true;
  target.hangup(603, 'Decline');
});
$('btnSwap').onclick = guard(() => {
  const other = client.calls.find((c) => c !== call && c.state !== SipCallState.Closed);
  if (other) switchToCall(other);
});
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
