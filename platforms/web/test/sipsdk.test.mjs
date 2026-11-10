// SDK contract tests: drive SipClient / SipCall through a fake JsSIP and check the
// states, events and JsSIP calls the README promises.
import { test } from 'node:test';
import assert from 'node:assert/strict';

import { SipClient, SipCall, SipCallState, SipRegistrationState, sanitizeSdp } from '../src/sipsdk.js';
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
