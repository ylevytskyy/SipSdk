// Drives the web SDK sample page in headless Chrome against a scripted SIP peer over
// SIP WebSocket. Covers registration (fail, ok, unregister), incoming calls (answer with
// video, answer audio-only, decline), outgoing calls (ringing, answer, cancel), and
// in-call controls (hold, resume, mute, video off, DTMF send/receive, duration, BYE).
// Usage: node platforms/web/test/verify-web-sdk.mjs

import fs from 'node:fs';
import path from 'node:path';
import { SipWsPeer } from './sip-ws-peer.mjs';
import { startStaticServer, launchBrowser, TRACK_PEER_CONNECTIONS, EVIDENCE_DIR, checker } from './browser-harness.mjs';

const { expect, failures } = checker();
const mLines = (sdp) => (sdp || '').split(/\r?\n/).filter((l) => l.startsWith('m=')).join(' & ');
const mediaSection = (sdp, kind) => {
  const parts = (sdp || '').split(/\r?\n(?=m=)/);
  return parts.find((p) => p.startsWith(`m=${kind}`)) || '';
};
const direction = (section) => (/a=(sendrecv|sendonly|recvonly|inactive)/.exec(section) || [])[1] || 'sendrecv';

async function main() {
  fs.mkdirSync(EVIDENCE_DIR, { recursive: true });
  const peer = new SipWsPeer({ rejectUsers: ['9999'] });
  const wsPort = await peer.listen();
  const { server, origin } = await startStaticServer();
  const browser = await launchBrowser();

  try {
    const page = await browser.newPage();
    const consoleErrors = [];
    page.on('console', (m) => { if (m.type() === 'error') consoleErrors.push(m.text()); });
    page.on('pageerror', (e) => consoleErrors.push(`pageerror: ${e.message}`));
    await page.addInitScript(TRACK_PEER_CONNECTIONS);
    await page.goto(`${origin}/platforms/web/sample/?ws=ws://127.0.0.1:${wsPort}&domain=test.invalid`, { waitUntil: 'networkidle' });

    const callState = () => page.locator('#callState').getAttribute('data-state');
    const waitAttr = async (sel, value, timeout = 10000) => {
      try {
        await page.waitForFunction(([s, v]) => document.querySelector(s)?.dataset.state === v, [sel, String(value)], { timeout });
        return true;
      } catch { return false; }
    };

    // R1 — registration failure
    await page.fill('#username', '9999');
    await page.click('#btnRegister');
    expect('403 to REGISTER shows Failed (state 3) with the code',
      await waitAttr('#regStatus', 3) && /403/.test(await page.locator('#regStatus').innerText()),
      await page.locator('#regStatus').innerText());

    // R1 — registration success
    await page.fill('#username', '1002');
    await page.click('#btnRegister');
    expect('200 to REGISTER shows Registered (state 2)', await waitAttr('#regStatus', 2), await page.locator('#regStatus').innerText());
    const reg = peer.received.filter((m) => m.method === 'REGISTER').at(-1);
    expect('REGISTER carries the account AOR', /sip:1002@test\.invalid/.test(reg?.header('to') || ''), reg?.header('to'));
    expect('Call button enabled after registration', await page.locator('#btnCall').isEnabled());

    // R3 — incoming call answered with video
    let since = peer.received.length;
    peer.invite({ video: true, displayName: 'Mobile Peer' });
    await page.locator('#incoming').waitFor({ state: 'visible', timeout: 10000 });
    expect('incomingCall shows the caller name and URI',
      /Mobile Peer.*sip:1001@test\.invalid/.test(await page.locator('#incomingFrom').innerText()),
      await page.locator('#incomingFrom').innerText());
    await page.click('#btnAnswer');
    let ok = await peer.waitFor((m) => m.status === 200 && m.cseqMethod === 'INVITE', { since });
    expect('answer with video sends 200 OK with active m=audio and m=video',
      !!ok && /m=audio [1-9]/.test(ok.body) && /m=video [1-9]/.test(ok.body), mLines(ok?.body));
    expect('the answer sends video (sendrecv)', direction(mediaSection(ok?.body, 'video')) === 'sendrecv', direction(mediaSection(ok?.body, 'video')));
    peer.ack();
    expect('answered call reaches Established (state 3)', await waitAttr('#callState', 3));
    await page.waitForTimeout(500);
    expect('localVideo shows the camera stream',
      await page.evaluate(() => document.getElementById('localVideo').srcObject?.getVideoTracks().length > 0));
    expect('remoteVideo and remoteAudio are bound to the remote stream',
      await page.evaluate(() => {
        const v = document.getElementById('remoteVideo').srcObject;
        return !!v && v === document.getElementById('remoteAudio').srcObject && v.getTracks().length > 0;
      }));

    // R4 — hold / resume
    since = peer.received.length;
    await page.click('#btnHold');
    const holdInvite = await peer.waitFor((m) => m.method === 'INVITE' && /;tag=/.test(m.header('to')), { since });
    expect('hold sends a re-INVITE with a=sendonly', !!holdInvite && direction(mediaSection(holdInvite.body, 'audio')) === 'sendonly',
      direction(mediaSection(holdInvite?.body, 'audio')));
    expect('call reaches Hold (state 4)', await waitAttr('#callState', 4));
    await page.waitForTimeout(800);
    expect('call is still on Hold after the re-INVITE answer', (await callState()) === '4');
    expect('hold button reads Resume', (await page.locator('#btnHold').innerText()) === 'Resume');
    since = peer.received.length;
    await page.click('#btnHold');
    const resumeInvite = await peer.waitFor((m) => m.method === 'INVITE' && /;tag=/.test(m.header('to')), { since });
    expect('resume sends a re-INVITE with a=sendrecv', !!resumeInvite && direction(mediaSection(resumeInvite.body, 'audio')) === 'sendrecv',
      direction(mediaSection(resumeInvite?.body, 'audio')));
    expect('call returns to Established (state 3)', await waitAttr('#callState', 3));

    // R4 — mute and video off
    await page.click('#btnMute');
    expect('mute disables the microphone track', await page.evaluate(() => window.__pcs.at(-1).getSenders()
      .filter((s) => s.track?.kind === 'audio').every((s) => s.track.enabled === false)));
    expect('mute button reads Unmute', (await page.locator('#btnMute').innerText()) === 'Unmute');
    await page.click('#btnMute');
    expect('unmute enables the microphone track', await page.evaluate(() => window.__pcs.at(-1).getSenders()
      .filter((s) => s.track?.kind === 'audio').every((s) => s.track.enabled === true)));
    await page.click('#btnVideo');
    expect('video off disables the camera track', await page.evaluate(() => window.__pcs.at(-1).getSenders()
      .filter((s) => s.track?.kind === 'video').every((s) => s.track.enabled === false)));
    await page.click('#btnVideo');
    expect('video on enables the camera track', await page.evaluate(() => window.__pcs.at(-1).getSenders()
      .filter((s) => s.track?.kind === 'video').every((s) => s.track.enabled === true)));

    // R4 — DTMF both ways
    since = peer.received.length;
    await page.click('[data-dtmf="5"]');
    const info = await peer.waitFor((m) => m.method === 'INFO', { since });
    expect('DTMF 5 reaches the peer as SIP INFO Signal=5', !!info && /Signal=5/.test(info.body), info?.body.trim());
    peer.info('7');
    await page.waitForFunction(() => document.getElementById('dtmfReceived').textContent === '7', null, { timeout: 5000 }).catch(() => {});
    expect('remote DTMF 7 fires dtmfReceived', (await page.locator('#dtmfReceived').innerText()) === '7');

    // R4 — duration
    await page.waitForTimeout(1500);
    const durationText = await page.locator('#callDuration').innerText();
    const sdkDuration = await page.evaluate(() => window.sipClient.calls[0]?.duration);
    expect('call duration counts seconds', durationText !== '0:00' && sdkDuration >= 2, `${durationText}, duration=${sdkDuration}`);

    await page.screenshot({ path: path.join(EVIDENCE_DIR, 'web-sdk-incall.png'), fullPage: true });

    // R4 — remote BYE
    since = peer.received.length;
    peer.bye();
    const byeOk = await peer.waitFor((m) => m.status === 200 && m.cseqMethod === 'BYE', { since });
    expect('remote BYE is answered 200', !!byeOk);
    await page.locator('#incall').waitFor({ state: 'hidden', timeout: 5000 }).catch(() => {});
    expect('remote BYE closes the call and hides the in-call view', await page.locator('#incall').isHidden());
    expect('closed call leaves the SDK call list', (await page.evaluate(() => window.sipClient.calls.length)) === 0);

    // R3 — decline
    since = peer.received.length;
    peer.invite({ video: true });
    await page.locator('#incoming').waitFor({ state: 'visible', timeout: 10000 });
    await page.click('#btnDecline');
    const decline = await peer.waitFor((m) => m.status && m.status >= 300 && m.cseqMethod === 'INVITE', { since });
    expect('decline answers 603', decline?.status === 603, decline?.startLine);
    await page.locator('#incoming').waitFor({ state: 'hidden', timeout: 5000 }).catch(() => {});
    expect('decline hides the incoming view', await page.locator('#incoming').isHidden());

    // R3 — answer audio-only
    since = peer.received.length;
    peer.invite({ video: true });
    await page.locator('#incoming').waitFor({ state: 'visible', timeout: 10000 });
    await page.click('#btnAnswerAudio');
    ok = await peer.waitFor((m) => m.status === 200 && m.cseqMethod === 'INVITE', { since });
    const audioOnlyVideoDir = direction(mediaSection(ok?.body, 'video'));
    const videoPortZero = /m=video 0 /.test(ok?.body || '');
    expect('answer audio-only sends active audio and does not send video',
      !!ok && /m=audio [1-9]/.test(ok.body) && (videoPortZero || audioOnlyVideoDir === 'recvonly' || audioOnlyVideoDir === 'inactive'),
      `${mLines(ok?.body)} video=${videoPortZero ? 'port 0' : audioOnlyVideoDir}`);
    peer.ack();
    expect('audio-only call reaches Established', await waitAttr('#callState', 3));
    expect('audio-only call has no local camera track', await page.evaluate(() => window.__pcs.at(-1).getSenders()
      .every((s) => !s.track || s.track.kind !== 'video')));
    since = peer.received.length;
    await page.click('#btnHangup');
    const localBye = await peer.waitFor((m) => m.method === 'BYE', { since });
    expect('local hangup sends BYE', !!localBye);
    await page.locator('#incall').waitFor({ state: 'hidden', timeout: 5000 }).catch(() => {});

    // R2 — outgoing video call: ringing, answer, hangup
    since = peer.received.length;
    await page.fill('#target', '9001');
    await page.click('#btnCall');
    let invite = await peer.waitFor((m) => m.method === 'INVITE' && !/;tag=/.test(m.header('to')), { since });
    expect('outgoing INVITE targets sip:9001@test.invalid', /^INVITE sip:9001@test\.invalid/.test(invite?.startLine || ''), invite?.startLine);
    expect('outgoing INVITE offers active m=audio and m=video', !!invite && /m=audio [1-9]/.test(invite.body) && /m=video [1-9]/.test(invite.body), mLines(invite?.body));
    expect('outgoing INVITE offers H264', /a=rtpmap:\d+ H264\/90000/.test(invite?.body || ''));
    expect('outgoing call starts in Connecting (state 2)', (await callState()) === '2');
    peer.ring(invite);
    expect('180 moves the call to Ringing (state 1)', await waitAttr('#callState', 1));
    since = peer.received.length;
    peer.accept(invite);
    expect('200 moves the call to Established (state 3)', await waitAttr('#callState', 3));
    const ack = await peer.waitFor((m) => m.method === 'ACK', { since });
    expect('the browser ACKs the 200', !!ack);
    await page.waitForTimeout(400);
    expect('outgoing call shows local camera preview',
      await page.evaluate(() => document.getElementById('localVideo').srcObject?.getVideoTracks().length > 0));
    since = peer.received.length;
    await page.click('#btnHangup');
    const outBye = await peer.waitFor((m) => m.method === 'BYE', { since });
    expect('hangup of an established outgoing call sends BYE', !!outBye);
    await page.locator('#incall').waitFor({ state: 'hidden', timeout: 5000 }).catch(() => {});
    expect('outgoing call closes after hangup', await page.locator('#incall').isHidden());

    // R2 — cancel before answer
    since = peer.received.length;
    await page.click('#btnCall');
    invite = await peer.waitFor((m) => m.method === 'INVITE' && !/;tag=/.test(m.header('to')), { since });
    peer.ring(invite);
    await waitAttr('#callState', 1);
    await page.click('#btnHangup');
    const cancel = await peer.waitFor((m) => m.method === 'CANCEL', { since });
    expect('hangup before answer sends CANCEL', !!cancel);
    await page.locator('#incall').waitFor({ state: 'hidden', timeout: 5000 }).catch(() => {});
    expect('cancelled call closes', await page.locator('#incall').isHidden());

    // R1 — unregister
    since = peer.received.length;
    await page.click('#btnUnregister');
    const unreg = await peer.waitFor((m) => m.method === 'REGISTER', { since });
    expect('unregister sends REGISTER with expires 0',
      !!unreg && (/expires=0\b/.test(unreg.header('contact')) || unreg.header('expires') === '0'), unreg?.header('contact'));
    expect('unregister shows Unregistered (state 0)', await waitAttr('#regStatus', 0));

    expect('no console errors', consoleErrors.length === 0, consoleErrors.join(' | '));
  } finally {
    await browser.close();
    server.close();
    peer.close();
  }

  if (failures() === 0) {
    console.log('ALL_WEB_SDK_CHECKS_PASS');
  } else {
    console.log(`${failures()} FAILURES`);
    process.exitCode = 1;
  }
}

// Exit outright: a failure before the try block (browser launch) leaves the peer and server open.
main().catch((e) => { console.error('driver error:', e); process.exit(1); });
