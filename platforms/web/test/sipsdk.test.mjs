// SDK contract tests: drive SipClient / SipCall through a fake JsSIP and check the
// states, events and JsSIP calls the README promises.
import { test } from 'node:test';
import assert from 'node:assert/strict';

import {
  SIP_CLIENT_MAX_ACCOUNTS,
  SipAccount,
  SipClient,
  SipCall,
  SipCallState,
  SipMessageStatus,
  SipRecordingMode,
  SipRecordingState,
  SipRegistrationState,
  sanitizeSdp,
} from '../src/sipsdk.js';
import { createFakeJsSIP, FakeSession } from './fake-jssip.mjs';

const ACCOUNT = { username: '1002', domain: 'pbx.local', password: 'pw', wsUri: 'ws://pbx.local:8088/ws' };
const VIDEO_OFFER = 'v=0\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111\r\nm=video 9 UDP/TLS/RTP/SAVPF 96\r\n';
const AUDIO_OFFER = 'v=0\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111\r\nm=video 0 UDP/TLS/RTP/SAVPF 96\r\n';

function setup() {
  const fake = createFakeJsSIP();
  const clock = { ms: 1_000_000 };
  const client = new SipClient({ jssip: fake.JsSIP, now: () => clock.ms });
  const events = [];
  client.on('registrationStateChanged', (e) => events.push(e));
  return { fake, clock, client, events };
}

function registered() {
  const ctx = setup();
  ctx.client.start();
  ctx.client.register(ACCOUNT);
  ctx.ua = ctx.fake.uas[0];
  ctx.ua.emit('registered', { response: { status_code: 200, reason_phrase: 'OK' } });
  return ctx;
}

function incoming(ctx, body) {
  const session = new FakeSession({ direction: 'incoming', uri: 'sip:1001@pbx.local', displayName: 'Alice' });
  let delivered = null;
  ctx.client.on('incomingCall', ({ call }) => { delivered = call; });
  ctx.ua.emit('newRTCSession', { originator: 'remote', session, request: { body } });
  return { session, call: delivered };
}

function stateLog(call) {
  const log = [];
  call.on('stateChanged', (e) => log.push(e));
  return log;
}

test('constructor rejects a missing JsSIP', () => {
  assert.throws(() => new SipClient({ jssip: null }), /JsSIP is not loaded/);
});

test('register before start throws', () => {
  const { client } = setup();
  assert.throws(() => client.register(ACCOUNT), /start\(\) before register/);
});

test('register configures the UA and reports Registering then Registered', () => {
  const { fake, client, events } = setup();
  client.start();
  client.register(ACCOUNT);
  const ua = fake.uas[0];
  assert.equal(ua.config.uri, 'sip:1002@pbx.local');
  assert.equal(ua.config.sockets[0].url, 'ws://pbx.local:8088/ws');
  assert.equal(ua.config.password, ACCOUNT.password);
  assert.equal(ua.started, true);
  assert.equal(client.accountAor, 'sip:1002@pbx.local');
  ua.emit('registered', { response: { status_code: 200, reason_phrase: 'OK' } });
  assert.deepEqual(events, [
    { state: SipRegistrationState.Registering, code: 0, reason: '' },
    { state: SipRegistrationState.Registered, code: 200, reason: 'OK' },
  ]);
  assert.equal(client.isRegistered, true);
});

test('registration failure reports Failed with the response code', () => {
  const { fake, client, events } = setup();
  client.start();
  client.register(ACCOUNT);
  fake.uas[0].emit('registrationFailed', { response: { status_code: 403, reason_phrase: 'Forbidden' } });
  assert.deepEqual(events.at(-1), { state: SipRegistrationState.Failed, code: 403, reason: 'Forbidden' });
  assert.equal(client.isRegistered, false);
});

test('register again stops the old UA and ignores its events', () => {
  const { fake, client, events } = setup();
  client.start();
  client.register(ACCOUNT);
  client.register(ACCOUNT);
  assert.equal(fake.uas[0].stopped, true);
  const before = events.length;
  fake.uas[0].emit('registered', {});
  assert.equal(events.length, before);
});

test('unregister removes all bindings', () => {
  const { ua, client } = registered();
  client.unregister();
  assert.deepEqual(ua.unregisterCalls, [{ all: true }]);
});

test('makeCall before registration throws', () => {
  const { client } = setup();
  client.start();
  client.register(ACCOUNT);
  assert.throws(() => client.makeCall('9001'), /register an account/);
});

test('makeCall builds the target URI from an extension, user@host or sip: URI', () => {
  const { ua, client } = registered();
  client.makeCall('9001');
  client.makeCall('bob@example.org');
  client.makeCall('sip:carol@example.org');
  assert.deepEqual(ua.outgoing.map((s) => s.remote_identity.uri.toString()), [
    'sip:9001@pbx.local',
    'sip:bob@example.org',
    'sip:carol@example.org',
  ]);
});

test('makeCall asks for video only when video is true', () => {
  const { ua, client } = registered();
  const audio = client.makeCall('9001');
  const video = client.makeCall('9001', { video: true });
  assert.equal(ua.outgoing[0].callOptions.mediaConstraints.video, false);
  assert.deepEqual(ua.outgoing[1].callOptions.mediaConstraints.video, { width: { ideal: 640 }, height: { ideal: 480 } });
  assert.equal(audio.hasVideo, false);
  assert.equal(video.hasVideo, true);
  assert.equal(audio.isOutgoing, true);
  assert.equal(audio.state, SipCallState.Connecting);
  assert.equal(client.calls.length, 2);
});

test('outgoing call goes Ringing, Established, Closed and duration freezes at close', () => {
  const { ua, client, clock } = registered();
  const call = client.makeCall('9001');
  const log = stateLog(call);
  const session = ua.outgoing[0];
  session.emit('progress', { originator: 'remote', response: { status_code: 180, reason_phrase: 'Ringing' } });
  assert.equal(call.duration, 0);
  session.emit('accepted', { response: { status_code: 200, reason_phrase: 'OK' } });
  clock.ms += 5_500;
  assert.equal(call.duration, 5);
  session.emit('ended', { cause: 'BYE' });
  clock.ms += 10_000;
  assert.equal(call.duration, 5);
  assert.deepEqual(log, [
    { state: SipCallState.Ringing, code: 180, reason: 'Ringing' },
    { state: SipCallState.Established, code: 200, reason: 'OK' },
    { state: SipCallState.Closed, code: 0, reason: 'BYE' },
  ]);
  assert.equal(call.isClosed, true);
  assert.equal(client.calls.length, 0);
});

test('failed call closes with the SIP response code', () => {
  const { ua, client } = registered();
  const call = client.makeCall('9001');
  const log = stateLog(call);
  ua.outgoing[0].emit('failed', { message: { status_code: 486, reason_phrase: 'Busy Here' }, cause: 'Busy' });
  assert.deepEqual(log, [{ state: SipCallState.Closed, code: 486, reason: 'Busy Here' }]);
});

test('incoming video call is delivered in Incoming state with peer details', () => {
  const ctx = registered();
  const { call } = incoming(ctx, VIDEO_OFFER);
  assert.ok(call instanceof SipCall);
  assert.equal(call.state, SipCallState.Incoming);
  assert.equal(call.isOutgoing, false);
  assert.equal(call.hasVideo, true);
  assert.equal(call.remoteUri, 'sip:1001@pbx.local');
  assert.equal(call.peerDisplayName, 'Alice');
  assert.equal(ctx.client.calls.length, 1);
});

test('incoming offer with a disabled video line is audio-only', () => {
  const ctx = registered();
  const { session, call } = incoming(ctx, AUDIO_OFFER);
  assert.equal(call.hasVideo, false);
  call.answer();
  assert.equal(session.answered.mediaConstraints.video, false);
});

test('answer with video:false answers a video offer audio-only', () => {
  const ctx = registered();
  const { session, call } = incoming(ctx, VIDEO_OFFER);
  call.answer({ video: false });
  assert.deepEqual(session.answered.mediaConstraints, { audio: true, video: false });
  assert.equal(call.hasVideo, false);
});

test('answer defaults to video on a video offer', () => {
  const ctx = registered();
  const { session, call } = incoming(ctx, VIDEO_OFFER);
  call.answer();
  assert.deepEqual(session.answered.mediaConstraints.video, { width: { ideal: 640 }, height: { ideal: 480 } });
});

test('answer on an outgoing call throws', () => {
  const { client } = registered();
  const call = client.makeCall('9001');
  assert.throws(() => call.answer(), /only valid for incoming calls/);
});

test('hangup with a status code declines; plain hangup sends no code', () => {
  const ctx = registered();
  const { session, call } = incoming(ctx, VIDEO_OFFER);
  call.hangup(486);
  assert.deepEqual(session.terminated, { status_code: 486 });
  const out = ctx.client.makeCall('9001');
  out.hangup();
  assert.deepEqual(ctx.ua.outgoing[0].terminated, {});
});

test('controls on a closed call throw', () => {
  const { ua, client } = registered();
  const call = client.makeCall('9001');
  ua.outgoing[0].emit('ended', {});
  assert.throws(() => call.hangup(), /call is closed/);
  assert.throws(() => call.setHold(true), /call is closed/);
});

test('hold and unhold move between Hold and Established and report media state', () => {
  const { ua, client } = registered();
  const call = client.makeCall('9001');
  const session = ua.outgoing[0];
  session.emit('accepted', {});
  const log = stateLog(call);
  const media = [];
  call.on('mediaChanged', (e) => media.push(e));
  call.setHold(true);
  session.emit('hold', { originator: 'local' });
  assert.equal(call.state, SipCallState.Hold);
  assert.equal(call.isOnHold, true);
  call.setHold(false);
  session.emit('unhold', { originator: 'local' });
  assert.equal(call.state, SipCallState.Established);
  assert.deepEqual(session.holdCalls, ['hold', 'unhold']);
  assert.deepEqual(log.map((e) => e.state), [SipCallState.Hold, SipCallState.Established]);
  assert.deepEqual(media.map((e) => e.isOnHold), [true, false]);
});

test('remote hold does not change the local hold state', () => {
  const { ua, client } = registered();
  const call = client.makeCall('9001');
  ua.outgoing[0].emit('accepted', {});
  ua.outgoing[0].emit('hold', { originator: 'remote' });
  assert.equal(call.state, SipCallState.Established);
  assert.equal(call.isOnHold, false);
});

test('setMuted mutes audio and setLocalVideoEnabled mutes video', () => {
  const { ua, client } = registered();
  const call = client.makeCall('9001', { video: true });
  const media = [];
  call.on('mediaChanged', (e) => media.push(e));
  call.setMuted(true);
  assert.equal(call.isMuted, true);
  call.setMuted(false);
  call.setLocalVideoEnabled(false);
  assert.equal(call.isLocalVideoEnabled, false);
  assert.deepEqual(ua.outgoing[0].muteCalls, [
    ['mute', { audio: true }],
    ['unmute', { audio: true }],
    ['mute', { video: true }],
  ]);
  assert.deepEqual(media.map((e) => e.isMuted), [true, false]);
});

test('sendDtmf validates digits, needs an established call, and sends SIP INFO', () => {
  const { ua, client } = registered();
  const call = client.makeCall('9001');
  const session = ua.outgoing[0];
  assert.throws(() => call.sendDtmf('12x'), /invalid DTMF/);
  assert.throws(() => call.sendDtmf(''), /invalid DTMF/);
  assert.throws(() => call.sendDtmf('1'), /established call/);
  session.established = true;
  call.sendDtmf('12#*A');
  assert.deepEqual(session.dtmfSent, [['12#*A', { transportType: 'INFO' }]]);
});

test('remote DTMF from RFC 2833 events and dtmf-relay INFO is reported', () => {
  const { ua, client } = registered();
  const call = client.makeCall('9001');
  const digits = [];
  call.on('dtmfReceived', (e) => digits.push(e.digit));
  const session = ua.outgoing[0];
  session.emit('newDTMF', { originator: 'remote', dtmf: { tone: '5' } });
  session.emit('newInfo', { originator: 'remote', info: { contentType: 'application/dtmf-relay', body: 'Signal=#\r\nDuration=160\r\n' } });
  session.emit('newDTMF', { originator: 'local', dtmf: { tone: '9' } });
  assert.deepEqual(digits, ['5', '#']);
});

test('stop hangs up open calls and stops the UA', () => {
  const { ua, client } = registered();
  client.makeCall('9001');
  client.stop();
  assert.deepEqual(ua.outgoing[0].terminated, {});
  assert.equal(ua.stopped, true);
  assert.equal(client.isRegistered, false);
  assert.equal(client.isStarted, false);
});

test('local SDP offers are sanitized before they are sent', () => {
  const { ua, client } = registered();
  client.makeCall('9001');
  const e = { originator: 'local', type: 'offer', sdp: 'v=0\r\nm=audio 9 RTP/AVP 111 9\r\na=rtpmap:111 opus/48000/2\r\na=rtpmap:9 G722/8000\r\n' };
  ua.outgoing[0].emit('sdp', e);
  assert.equal(e.sdp, 'v=0\r\nm=audio 9 RTP/AVP 111\r\na=rtpmap:111 opus/48000/2\r\n');
});

test('sanitizeSdp keeps shared codecs and one H.264, drops other codecs and bad candidates', () => {
  const input = [
    'v=0',
    'm=audio 9 UDP/TLS/RTP/SAVPF 111 9 0 8 101',
    'a=rtpmap:111 opus/48000/2',
    'a=rtpmap:9 G722/8000',
    'a=rtpmap:0 PCMU/8000',
    'a=rtpmap:8 PCMA/8000',
    'a=rtpmap:101 telephone-event/8000',
    'a=candidate:1 1 udp 2122260223 192.168.1.10 50000 typ host',
    'a=candidate:2 1 udp 2122260223 fe80::1 50001 typ host',
    'a=candidate:3 1 tcp 1518280447 192.168.1.10 9 typ host tcptype active',
    'a=candidate:4 1 udp 2122260223 169.254.3.4 50002 typ host',
    'm=video 9 UDP/TLS/RTP/SAVPF 96 102 104 98',
    'a=rtpmap:96 VP8/90000',
    'a=rtpmap:102 H264/90000',
    'a=fmtp:102 profile-level-id=42e01f',
    'a=rtcp-fb:102 nack',
    'a=rtpmap:104 H264/90000',
    'a=fmtp:104 profile-level-id=640c1f',
    'a=rtpmap:98 VP9/90000',
    'a=extmap:5 http://www.webrtc.org/experiments/rtp-hdrext/playout-delay',
    'a=extmap:1 urn:ietf:params:rtp-hdrext:sdes:mid',
  ].join('\r\n');
  const expected = [
    'v=0',
    'm=audio 9 UDP/TLS/RTP/SAVPF 111 0 8 101',
    'a=rtpmap:111 opus/48000/2',
    'a=rtpmap:0 PCMU/8000',
    'a=rtpmap:8 PCMA/8000',
    'a=rtpmap:101 telephone-event/8000',
    'a=candidate:1 1 udp 2122260223 192.168.1.10 50000 typ host',
    'm=video 9 UDP/TLS/RTP/SAVPF 96 102',
    'a=rtpmap:96 VP8/90000',
    'a=rtpmap:102 H264/90000',
    'a=fmtp:102 profile-level-id=42e01f',
    'a=rtcp-fb:102 nack',
    'a=extmap:1 urn:ietf:params:rtp-hdrext:sdes:mid',
  ].join('\r\n') + '\r\n';
  assert.equal(sanitizeSdp(input), expected);
});

test('sendMessage before register throws', () => {
  const { client } = setup();
  client.start();
  assert.throws(() => client.sendMessage('1001', 'hi'), /register an account before sendMessage/);
});

test('sendMessage rejects an empty target, empty body and a bad content type', () => {
  const { client } = registered();
  assert.throws(() => client.sendMessage('', 'hi'), /target is required/);
  assert.throws(() => client.sendMessage('1001', ''), /body is required/);
  assert.throws(() => client.sendMessage('1001', 'hi', { contentType: 'text\r\nX: 1' }), /invalid content type/);
  assert.throws(() => client.sendMessage('1001', 'hi', { contentType: 'plain' }), /invalid content type/);
});

test('sendMessage sends a SIP MESSAGE and reports Sent with the response code', () => {
  const { client, ua } = registered();
  const statuses = [];
  client.on('messageStatusChanged', (e) => statuses.push(e));
  const id = client.sendMessage('1001', 'hello');
  assert.ok(id > 0);
  const sent = ua.messages[0];
  assert.equal(sent.target, 'sip:1001@pbx.local');
  assert.equal(sent.body, 'hello');
  assert.equal(sent.options.contentType, 'text/plain');
  sent.options.eventHandlers.succeeded({ originator: 'remote', response: { status_code: 202, reason_phrase: 'Accepted' } });
  assert.deepEqual(statuses, [{ id, status: SipMessageStatus.Sent, code: 202, reason: 'Accepted' }]);
});

test('sendMessage keeps a rich content type and reports Failed with code and cause', () => {
  const { client, ua } = registered();
  const statuses = [];
  client.on('messageStatusChanged', (e) => statuses.push(e));
  const id = client.sendMessage('sip:1003@pbx.local', '{"a":1}', { contentType: 'application/json' });
  const sent = ua.messages[0];
  assert.equal(sent.target, 'sip:1003@pbx.local');
  assert.equal(sent.options.contentType, 'application/json');
  sent.options.eventHandlers.failed({ originator: 'remote', response: { status_code: 415, reason_phrase: 'Unsupported Media Type' }, cause: 'Rejected' });
  assert.deepEqual(statuses, [{ id, status: SipMessageStatus.Failed, code: 415, reason: 'Unsupported Media Type' }]);
});

test('a failed send without a response reports Failed with code 0 and the cause', () => {
  const { client, ua } = registered();
  const statuses = [];
  client.on('messageStatusChanged', (e) => statuses.push(e));
  const id = client.sendMessage('1001', 'hello');
  ua.messages[0].options.eventHandlers.failed({ originator: 'system', cause: 'Request Timeout' });
  assert.deepEqual(statuses, [{ id, status: SipMessageStatus.Failed, code: 0, reason: 'Request Timeout' }]);
});

test('an incoming remote message emits messageReceived; local echoes are ignored', () => {
  const { client, ua } = registered();
  const received = [];
  client.on('messageReceived', (e) => received.push(e));
  const request = {
    body: 'hi there',
    getHeader: (name) => (name.toLowerCase() === 'content-type' ? 'text/plain;charset=UTF-8' : undefined),
  };
  ua.emit('newMessage', {
    originator: 'remote',
    message: { remote_identity: { uri: { toString: () => 'sip:1003@pbx.local' } } },
    request,
  });
  ua.emit('newMessage', { originator: 'local', message: {}, request });
  assert.deepEqual(received, [{ from: 'sip:1003@pbx.local', contentType: 'text/plain', body: 'hi there' }]);
});

test('getStats parses WebRTC audio and video stats and detects bitrate delta', async () => {
  const { ua, client } = registered();
  const call = client.makeCall('9001', { video: true });
  const session = ua.outgoing[0];
  session.established = true;
  session.emit('accepted', {});

  let tick = 1000;
  session.connection = {
    async getStats() {
      const bytesAudio = tick === 1000 ? 10_000 : 26_000; // +16 KB in 2s = 64 kbps
      const bytesVideo = tick === 1000 ? 50_000 : 175_000; // +125 KB in 2s = 500 kbps
      return new Map([
        ['c-a', { id: 'c-a', type: 'codec', mimeType: 'audio/opus' }],
        ['c-v', { id: 'c-v', type: 'codec', mimeType: 'video/H264' }],
        ['cp', { id: 'cp', type: 'candidate-pair', nominated: true, currentRoundTripTime: 0.025 }],
        ['in-a', { id: 'in-a', type: 'inbound-rtp', kind: 'audio', codecId: 'c-a', packetsReceived: 98, packetsLost: 2, jitter: 0.012, bytesReceived: bytesAudio, timestamp: tick }],
        ['rem-a', { id: 'rem-a', type: 'remote-inbound-rtp', kind: 'audio', roundTripTime: 0.018 }],
        ['in-v', { id: 'in-v', type: 'inbound-rtp', kind: 'video', codecId: 'c-v', packetsReceived: 190, packetsLost: 10, jitter: 0.020, bytesReceived: bytesVideo, framesPerSecond: 30, timestamp: tick }],
      ]);
    },
  };

  assert.equal(call.isSecure, true);
  const s1 = await call.getStats();
  assert.equal(s1.audioCodec, 'opus');
  assert.equal(s1.videoCodec, 'H264');
  assert.equal(s1.audioRttMs, 18);
  assert.equal(s1.audioJitterMs, 12);
  assert.ok(Math.abs(s1.audioLossRate - 0.02) < 1e-6);
  assert.equal(s1.videoFps, 30);

  tick = 3000;
  const s2 = await call.getStats();
  assert.equal(s2.audioBitrateKbps, 64);
  assert.equal(s2.videoBitrateKbps, 500);
  call.hangup();
  session.emit('ended', {});
});

test('blind transfer, attended transfer, and incoming REFER work over JsSIP refer', () => {
  const { ua, client } = registered();
  const call1 = client.makeCall('1001');
  const s1 = ua.outgoing[0];
  s1.established = true;
  const statuses = [];
  call1.on('transferStatus', (e) => statuses.push(e));

  assert.throws(() => call1.transfer(''), /transfer target is required/);
  call1.transfer('1003');
  assert.equal(s1.referCalls[0][0], 'sip:1003@pbx.local');
  s1.referCalls[0][1].eventHandlers.trying({ status_line: { status_code: 100, reason_phrase: 'Trying' } });
  s1.referCalls[0][1].eventHandlers.accepted({ status_line: { status_code: 200, reason_phrase: 'OK' } });
  assert.deepEqual(statuses, [
    { code: 100, reason: 'Trying' },
    { code: 200, reason: 'OK' },
  ]);

  const call2 = client.makeCall('1004');
  const s2 = ua.outgoing[1];
  s2.established = true;
  call1.attendedTransfer(call2);
  assert.equal(s1.referCalls[1][0], 'sip:1004@pbx.local');
  assert.equal(s1.referCalls[1][1].replaces, s2);

  const requests = [];
  call2.on('transferRequest', (e) => requests.push(e));
  let accepted = false;
  let rejected = false;
  s2.emit('refer', {
    request: { refer_to: { uri: { toString: () => 'sip:9001@pbx.local' } } },
    accept: () => { accepted = true; },
    reject: () => { rejected = true; },
  });
  assert.deepEqual(requests, [{ target: 'sip:9001@pbx.local' }]);
  call2.acceptTransfer(true);
  assert.equal(accepted, true);
  assert.equal(rejected, false);
});

test('multi-call auto-hold, swap, and maxCalls cap (EBUSY / 486)', () => {
  const ctx = registered();
  const { client, fake } = ctx;
  const ua = ctx.ua;
  assert.equal(client.maxCalls, 4);

  const call1 = client.makeCall('9001');
  const s1 = ua.outgoing[0];
  s1.established = true;
  s1.emit('accepted', {});

  const call2 = client.makeCall('9002');
  assert.deepEqual(s1.holdCalls, ['hold']);
  s1.emit('hold', { originator: 'local' });
  assert.equal(call1.isOnHold, true);

  const s2 = ua.outgoing[1];
  s2.established = true;
  s2.emit('accepted', {});

  call1.setHold(false);
  assert.deepEqual(s2.holdCalls, ['hold']);
  assert.deepEqual(s1.holdCalls, ['hold', 'unhold']);
  s2.emit('hold', { originator: 'local' });
  s1.emit('unhold', { originator: 'local' });
  assert.equal(call2.isOnHold, true);
  assert.equal(call1.isOnHold, false);

  // Unanswered incoming call (Incoming) must not be placed on hold by _autoHoldOthers when dialing.
  const waiting = incoming(ctx, AUDIO_OFFER);
  assert.equal(waiting.call.state, SipCallState.Incoming);
  const call2b = client.makeCall('9003');
  s1.emit('hold', { originator: 'local' });
  assert.equal(waiting.call.isOnHold, false);
  assert.equal(waiting.call.state, SipCallState.Incoming);
  assert.deepEqual(waiting.session.holdCalls, []);
  call2b.hangup();
  ua.outgoing.at(-1).emit('ended', {});
  waiting.call.hangup();
  waiting.session.emit('ended', {});
  call1.setHold(false);
  s1.emit('unhold', { originator: 'local' });
  const holdLenBeforeAnswer = s1.holdCalls.length;

  // Incoming call answer auto-holds call1
  const inc = incoming(ctx, AUDIO_OFFER);
  assert.ok(inc.call);
  inc.call.answer({ video: false });
  assert.deepEqual(s1.holdCalls.slice(holdLenBeforeAnswer), ['hold']);
  s1.emit('hold', { originator: 'local' });

  call1.hangup(); s1.emit('ended', {});
  call2.hangup(); s2.emit('ended', {});
  inc.call.hangup(); inc.session.emit('ended', {});

  // Enforce maxCalls = 1: outgoing throws EBUSY, incoming rejects 486 + callRejectedBusy
  client.register({ ...ACCOUNT, maxCalls: 1 });
  const ua2 = fake.uas.at(-1);
  ua2.emit('registered', { response: { status_code: 200, reason_phrase: 'OK' } });
  assert.equal(client.maxCalls, 1);

  const active = client.makeCall('9001');
  ua2.outgoing[0].established = true;
  ua2.outgoing[0].emit('accepted', {});

  assert.throws(() => client.makeCall('9002'), (err) => err.code === 'EBUSY');

  const busyEvents = [];
  client.on('callRejectedBusy', (e) => busyEvents.push(e));
  let incomingDelivered = false;
  client.on('incomingCall', () => { incomingDelivered = true; });
  const busySession = new FakeSession({ direction: 'incoming', uri: 'sip:1009@pbx.local', displayName: 'Busy' });
  ua2.emit('newRTCSession', { originator: 'remote', session: busySession, request: { body: AUDIO_OFFER } });

  assert.equal(incomingDelivered, false);
  assert.deepEqual(busySession.terminated, { status_code: 486, reason_phrase: 'Busy Here' });
  assert.deepEqual(busyEvents, [{ callerUri: 'sip:1009@pbx.local' }]);

  active.hangup();
  ua2.outgoing[0].emit('ended', {});
});

test('local audio conference: merge, group hold/resume, mute, split dissolve, and leave', () => {
  const ctx = registered();
  const { client, ua } = ctx;
  const confEvents = [];
  client.on('conferenceChanged', (e) => confEvents.push(e.calls.length));

  const c1 = client.makeCall('9001');
  const s1 = ua.outgoing[0];
  s1.established = true;
  s1.emit('accepted', {});

  const c2 = client.makeCall('9001');
  const s2 = ua.outgoing[1];
  s1.emit('hold', { originator: 'local' });
  s2.established = true;
  s2.emit('accepted', {});
  assert.equal(c1.isOnHold, true);

  // Merge c1 + c2 -> c1 unholds, both in conference
  client.mergeCalls(c1, c2);
  s1.emit('unhold', { originator: 'local' });
  assert.equal(c1.isInConference, true);
  assert.equal(c2.isInConference, true);
  assert.equal(c1.isOnHold, false);
  assert.equal(c2.isOnHold, false);
  assert.equal(client.conferenceCalls.length, 2);
  assert.equal(confEvents.at(-1), 2);

  // Local mute on conference leg sets muted flag on both legs without tearing down peer mix
  c1.setMuted(true);
  assert.equal(c1.isMuted, true);
  assert.equal(c2.isMuted, true);

  // Dialing 3rd call auto-holds BOTH conference legs; resuming c1 resumes BOTH and holds c3
  const c3 = client.makeCall('9001');
  const s3 = ua.outgoing[2];
  s1.emit('hold', { originator: 'local' });
  s2.emit('hold', { originator: 'local' });
  s3.established = true;
  s3.emit('accepted', {});
  assert.equal(c1.isOnHold, true);
  assert.equal(c2.isOnHold, true);
  assert.equal(c3.isInConference, false);

  c1.setHold(false);
  s3.emit('hold', { originator: 'local' });
  s1.emit('unhold', { originator: 'local' });
  s2.emit('unhold', { originator: 'local' });
  assert.equal(c1.isOnHold, false);
  assert.equal(c2.isOnHold, false);
  assert.equal(c3.isOnHold, true);

  // Split c2 -> c2 holds, conference dissolves, c1 remains active
  client.splitCall(c2);
  s2.emit('hold', { originator: 'local' });
  assert.equal(c1.isInConference, false);
  assert.equal(c2.isInConference, false);
  assert.equal(c2.isOnHold, true);
  assert.equal(client.conferenceCalls.length, 0);
  assert.equal(confEvents.at(-1), 0);

  // Re-merge and leaveConference terminates both
  client.mergeCalls(c1, c2);
  s2.emit('unhold', { originator: 'local' });
  client.leaveConference();
  assert.equal(client.conferenceCalls.length, 0);
  assert.ok(s1.terminated);
  assert.ok(s2.terminated);
  c3.hangup();
  s3.emit('ended', {});
});

test('local call recording: startRecording, double-start guard, stopRecording Blob, and auto-stop on close', async () => {
  const origMR = globalThis.MediaRecorder;
  const origMS = globalThis.MediaStream;
  const origAC = globalThis.AudioContext;

  class FakeMediaStream {
    constructor(tracks = []) { this._tracks = [...tracks]; }
    addTrack(t) { this._tracks.push(t); }
    getTracks() { return this._tracks; }
    getAudioTracks() { return this._tracks.filter((t) => t.kind === 'audio'); }
    getVideoTracks() { return this._tracks.filter((t) => t.kind === 'video'); }
  }
  class FakeMediaRecorder {
    constructor(stream, opts = {}) {
      this.stream = stream;
      this.mimeType = opts.mimeType || 'audio/webm';
      this.state = 'inactive';
      this.ondataavailable = null;
      this.onstop = null;
    }
    start() {
      this.state = 'recording';
      if (this.ondataavailable) {
        this.ondataavailable({ data: new Blob([new Uint8Array([1, 2, 3, 4])], { type: this.mimeType }) });
      }
    }
    stop() {
      this.state = 'inactive';
      if (this.onstop) this.onstop();
    }
  }

  globalThis.MediaStream = FakeMediaStream;
  globalThis.MediaRecorder = FakeMediaRecorder;
  delete globalThis.AudioContext;

  try {
    const { client, ua } = registered();

    const states = [];
    client.on('recordingStateChanged', (e) => states.push(e.state));

    const call = client.makeCall('9001', { video: true });
    const sess = ua.outgoing[0];
    const remoteStream = new FakeMediaStream([{ kind: 'audio' }, { kind: 'video' }]);
    sess.connection = {
      getReceivers: () => [{ track: { kind: 'audio' } }, { track: { kind: 'video' } }],
      getSenders: () => [{ track: { kind: 'audio' } }],
      addEventListener: (ev, fn) => {
        if (ev === 'track') {
          fn({ track: { kind: 'audio' }, streams: [remoteStream] });
          fn({ track: { kind: 'video' }, streams: [remoteStream] });
        }
      },
    };
    sess.emit('peerconnection', { peerconnection: sess.connection });
    sess.established = true;
    sess.emit('accepted', {});

    call.startRecording({ mode: SipRecordingMode.AudioVideo });
    assert.equal(call.isRecording, true);
    assert.throws(() => call.startRecording({ mode: SipRecordingMode.AudioStereo }), /already recording/);

    const blob = await call.stopRecording();
    assert.equal(call.isRecording, false);
    assert.ok(blob instanceof Blob);
    assert.equal(blob.size, 4);
    assert.deepEqual(states, [SipRecordingState.Recording, SipRecordingState.Idle]);

    call.hangup();
    sess.emit('ended', {});
  } finally {
    if (origMR !== undefined) globalThis.MediaRecorder = origMR; else delete globalThis.MediaRecorder;
    if (origMS !== undefined) globalThis.MediaStream = origMS; else delete globalThis.MediaStream;
    if (origAC !== undefined) globalThis.AudioContext = origAC; else delete globalThis.AudioContext;
  }
});

// ---- Multi-account registration & routing ----

const ACCOUNT_2 = { username: '1005', domain: 'pbx2.local', password: 'pw2', wsUri: 'ws://pbx2.local:8088/ws' };

test('multi-account: simultaneous registration of 2 accounts on one SipClient', () => {
  const { fake, client, events } = setup();
  const accEvents = [];
  client.on('accountRegistrationStateChanged', (e) => accEvents.push(e));

  assert.throws(() => client.addAccount(ACCOUNT), (err) => err.code === 'EIO');
  client.start();

  const acc1 = client.addAccount(ACCOUNT);
  const acc2 = client.addAccount(ACCOUNT_2);
  assert.ok(acc1 instanceof SipAccount);
  assert.ok(acc2 instanceof SipAccount);
  assert.notEqual(acc1.id, acc2.id);
  assert.equal(acc1.aor, 'sip:1002@pbx.local');
  assert.equal(acc1.user, '1002');
  assert.equal(acc1.domain, 'pbx.local');
  assert.equal(acc2.aor, 'sip:1005@pbx2.local');
  assert.equal(client.accountCount, 2);
  assert.equal(client.defaultAccount, acc1);
  assert.equal(client.accountAor, 'sip:1002@pbx.local');

  // accounts getter returns a shallow copy
  const list = client.accounts;
  assert.deepEqual(list, [acc1, acc2]);
  list.pop();
  assert.equal(client.accountCount, 2);

  fake.uas[0].emit('registered', { response: { status_code: 200, reason_phrase: 'OK' } });
  fake.uas[1].emit('registered', { response: { status_code: 200, reason_phrase: 'OK' } });

  assert.equal(acc1.isRegistered, true);
  assert.equal(acc1.regState, SipRegistrationState.Registered);
  assert.equal(acc2.isRegistered, true);
  assert.equal(acc2.regState, SipRegistrationState.Registered);
  assert.equal(client.isRegistered, true);

  // accountRegistrationStateChanged fires for both; legacy registrationStateChanged only for defaultAccount
  assert.equal(accEvents.filter((e) => e.account === acc1 && e.state === SipRegistrationState.Registered).length, 1);
  assert.equal(accEvents.filter((e) => e.account === acc2 && e.state === SipRegistrationState.Registered).length, 1);
  assert.deepEqual(events, [
    { state: SipRegistrationState.Registering, code: 0, reason: '' },
    { state: SipRegistrationState.Registered, code: 200, reason: 'OK' },
  ]);
});

test('multi-account: dialing from a specific account and call.account attribution (outgoing & incoming)', () => {
  const { fake, client } = setup();
  client.start();
  const acc1 = client.addAccount(ACCOUNT);
  const acc2 = client.addAccount(ACCOUNT_2);
  const ua1 = fake.uas[0];
  const ua2 = fake.uas[1];
  ua1.emit('registered', { response: { status_code: 200, reason_phrase: 'OK' } });
  ua2.emit('registered', { response: { status_code: 200, reason_phrase: 'OK' } });

  // Outgoing via default (acc1) and specific (acc2)
  const out1 = client.makeCall('9001');
  assert.equal(out1.account, acc1);
  assert.equal(ua1.outgoing[0].remote_identity.uri.toString(), 'sip:9001@pbx.local');

  const out2 = acc2.dial('9002', true);
  assert.equal(out2.account, acc2);
  assert.equal(out2.hasVideo, true);
  assert.equal(ua2.outgoing[0].remote_identity.uri.toString(), 'sip:9002@pbx2.local');

  // Transfer uses the call's owning account domain
  ua2.outgoing[0].established = true;
  out2.transfer('1009');
  assert.equal(ua2.outgoing[0].referCalls[0][0], 'sip:1009@pbx2.local');

  // Incoming on acc2 is attributed to acc2
  const incomingCalls = [];
  client.on('incomingCall', ({ call }) => incomingCalls.push(call));
  const incSession = new FakeSession({ direction: 'incoming', uri: 'sip:1001@pbx2.local', displayName: 'Bob' });
  ua2.emit('newRTCSession', { originator: 'remote', session: incSession, request: { body: AUDIO_OFFER } });
  assert.equal(incomingCalls.length, 1);
  assert.equal(incomingCalls[0].account, acc2);
});

test('multi-account: cross-account auto-hold, conference merge, and client-wide maxCalls cap', () => {
  const { fake, client } = setup();
  client.start();
  const acc1 = client.addAccount({ ...ACCOUNT, maxCalls: 2 });
  const acc2 = client.addAccount(ACCOUNT_2);
  const ua1 = fake.uas[0];
  const ua2 = fake.uas[1];
  ua1.emit('registered', { response: { status_code: 200, reason_phrase: 'OK' } });
  ua2.emit('registered', { response: { status_code: 200, reason_phrase: 'OK' } });

  // Call 1 on acc1 becomes Established
  const c1 = acc1.dial('9001');
  const s1 = ua1.outgoing[0];
  s1.established = true;
  s1.emit('accepted', {});

  // Call 2 on acc2 auto-holds Call 1 on acc1
  const c2 = acc2.dial('9002');
  const s2 = ua2.outgoing[0];
  assert.deepEqual(s1.holdCalls, ['hold']);
  s1.emit('hold', { originator: 'local' });
  assert.equal(c1.isOnHold, true);
  s2.established = true;
  s2.emit('accepted', {});

  // Client-wide maxCalls=2 rejects a 3rd outgoing call on either account AND incoming call with 486
  assert.throws(() => acc1.dial('9003'), (err) => err.code === 'EBUSY');
  assert.throws(() => acc2.dial('9003'), (err) => err.code === 'EBUSY');
  const busySession = new FakeSession({ direction: 'incoming', uri: 'sip:1009@pbx2.local' });
  ua2.emit('newRTCSession', { originator: 'remote', session: busySession, request: { body: AUDIO_OFFER } });
  assert.deepEqual(busySession.terminated, { status_code: 486, reason_phrase: 'Busy Here' });

  // Cross-account local audio conference merge unholds c1 and keeps both in conference
  client.mergeCalls(c1, c2);
  s1.emit('unhold', { originator: 'local' });
  assert.equal(c1.isInConference, true);
  assert.equal(c2.isInConference, true);
  assert.equal(c1.isOnHold, false);
  assert.equal(c2.isOnHold, false);
});

test('multi-account: per-account SIP MESSAGE send and accountMessageReceived', () => {
  const { fake, client } = setup();
  client.start();
  const acc1 = client.addAccount(ACCOUNT);
  const acc2 = client.addAccount(ACCOUNT_2);
  const ua1 = fake.uas[0];
  const ua2 = fake.uas[1];
  ua1.emit('registered', { response: { status_code: 200, reason_phrase: 'OK' } });
  ua2.emit('registered', { response: { status_code: 200, reason_phrase: 'OK' } });

  const id = acc2.sendMessage('1001', 'hello from acc2', { contentType: 'text/plain' });
  assert.ok(id > 0);
  assert.equal(ua1.messages.length, 0);
  assert.equal(ua2.messages.length, 1);
  assert.equal(ua2.messages[0].target, 'sip:1001@pbx2.local');
  assert.equal(ua2.messages[0].body, 'hello from acc2');

  const accMsgs = [];
  const legacyMsgs = [];
  client.on('accountMessageReceived', (e) => accMsgs.push(e));
  client.on('messageReceived', (e) => legacyMsgs.push(e));

  const req = {
    body: 'ping acc2',
    getHeader: (name) => (name.toLowerCase() === 'content-type' ? 'text/plain;charset=UTF-8' : undefined),
  };
  ua2.emit('newMessage', {
    originator: 'remote',
    message: { remote_identity: { uri: { toString: () => 'sip:1001@pbx2.local' } } },
    request: req,
  });

  assert.equal(accMsgs.length, 1);
  assert.equal(accMsgs[0].account, acc2);
  assert.equal(accMsgs[0].fromUri, 'sip:1001@pbx2.local');
  assert.equal(accMsgs[0].from, 'sip:1001@pbx2.local');
  assert.equal(accMsgs[0].contentType, 'text/plain');
  assert.equal(accMsgs[0].body, 'ping acc2');
  assert.deepEqual(legacyMsgs, [{ from: 'sip:1001@pbx2.local', contentType: 'text/plain', body: 'ping acc2' }]);
});

test('multi-account: setDefaultAccount, removeAccount, reregister, resetTransports, EEXIST, and 8-account cap', () => {
  const { fake, client } = setup();
  client.start();
  const acc1 = client.addAccount(ACCOUNT);
  const acc2 = client.addAccount(ACCOUNT_2);
  const ua1 = fake.uas[0];
  const ua2 = fake.uas[1];
  ua1.emit('registered', { response: { status_code: 200, reason_phrase: 'OK' } });
  ua2.emit('registered', { response: { status_code: 200, reason_phrase: 'OK' } });

  // Duplicate active AOR rejected with EEXIST
  assert.throws(() => client.addAccount({ ...ACCOUNT }), (err) => err.code === 'EEXIST');

  // setDefaultAccount switches default used by makeCall / sendMessage
  client.setDefaultAccount(acc2);
  assert.equal(client.defaultAccount, acc2);
  assert.equal(client.accountAor, 'sip:1005@pbx2.local');
  const c2 = client.makeCall('9001');
  assert.equal(c2.account, acc2);
  const c1 = acc1.dial('9002');
  assert.equal(c1.account, acc1);

  // reregister & resetTransports
  acc1.reregister();
  assert.equal(ua1.registerCalls, 1);
  client.resetTransports(true);
  assert.equal(ua1.registerCalls, 2);
  assert.equal(ua2.registerCalls, 1);

  // removeAccount hangs up owned calls, leaves other accounts' calls intact, stops UA, re-elects default
  const accEvents = [];
  client.on('accountRegistrationStateChanged', (e) => accEvents.push(e));
  client.removeAccount(acc2);
  assert.deepEqual(ua2.outgoing[0].terminated, {});
  assert.equal(ua1.outgoing[0].terminated, null);
  assert.equal(ua2.stopped, true);
  assert.equal(client.accountCount, 1);
  assert.equal(client.defaultAccount, acc1);

  // Late events from removed UA are ignored
  ua2.emit('registered', {});
  assert.equal(accEvents.length, 0);

  // setDefaultAccount / removeAccount on inactive account throw
  assert.throws(() => client.setDefaultAccount(acc2), (err) => err.code === 'EINVAL');
  assert.throws(() => client.removeAccount(acc2), (err) => err.code === 'EINVAL');

  // Remove last account -> defaultAccount becomes null
  client.removeAccount(acc1);
  assert.equal(client.accountCount, 0);
  assert.equal(client.defaultAccount, null);
  assert.equal(client.isRegistered, false);

  // Enforce SIP_CLIENT_MAX_ACCOUNTS = 8 cap (EBUSY on 9th)
  assert.equal(SIP_CLIENT_MAX_ACCOUNTS, 8);
  for (let i = 1; i <= SIP_CLIENT_MAX_ACCOUNTS; i++) {
    client.addAccount({ username: `200${i}`, domain: 'pbx.local', wsUri: 'ws://pbx.local:8088/ws' });
  }
  assert.equal(client.accountCount, 8);
  assert.throws(
    () => client.addAccount({ username: '2009', domain: 'pbx.local', wsUri: 'ws://pbx.local:8088/ws' }),
    (err) => err.code === 'EBUSY',
  );

  // stop() cleans up all accounts
  client.stop();
  assert.equal(client.accountCount, 0);
  assert.equal(client.defaultAccount, null);
});
