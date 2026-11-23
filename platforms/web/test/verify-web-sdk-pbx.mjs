// Live check of the web SDK against the local Asterisk PBX (docker compose, see
// scripts/run-pbx.sh): registers extension 1002 over ws://<host>:8088/ws, calls the
// 9001 audio+video echo, holds and resumes, sends DTMF, hangs up and unregisters.
// Before the call it sends a SIP MESSAGE to 1002 itself and waits for it to come back.
// Usage: node platforms/web/test/verify-web-sdk-pbx.mjs [pbx-host]   (default 127.0.0.1)

import { startStaticServer, launchBrowser, checker } from './browser-harness.mjs';

const PBX_HOST = process.argv[2] || process.env.SIPSDK_PBX_HOST || '127.0.0.1';
const { expect, failures } = checker();

async function main() {
  const { server, origin } = await startStaticServer();
  const browser = await launchBrowser();
  try {
    const page = await browser.newPage();
    const consoleErrors = [];
    page.on('console', (m) => { if (m.type() === 'error') consoleErrors.push(m.text()); });
    page.on('pageerror', (e) => consoleErrors.push(`pageerror: ${e.message}`));
    await page.goto(`${origin}/platforms/web/sample/?ws=ws://${PBX_HOST}:8088/ws&domain=${PBX_HOST}`, { waitUntil: 'networkidle' });

    const waitState = async (sel, value, timeout = 15000) => {
      try {
        await page.waitForFunction(([s, v]) => document.querySelector(s)?.dataset.state === v, [sel, String(value)], { timeout });
        return true;
      } catch { return false; }
    };
    const callState = () => page.locator('#callState').getAttribute('data-state');

    await page.click('#btnRegister');
    expect('1002 registers with Asterisk (state 2)', await waitState('#regStatus', 2), await page.locator('#regStatus').innerText());

    // SIP MESSAGE to our own extension: Asterisk routes it back over the WebSocket.
    const msg = await page.evaluate(async () => {
      const client = window.sipClient;
      const text = `hello from web ${Date.now()}`;
      const timeout = (ms) => new Promise((resolve) => setTimeout(() => resolve(null), ms));
      const received = new Promise((resolve) => {
        const off = client.on('messageReceived', (e) => { if (e.body === text) { off(); resolve(e); } });
      });
      let id = 0;
      const status = new Promise((resolve) => {
        const off = client.on('messageStatusChanged', (e) => { if (e.id === id) { off(); resolve(e); } });
      });
      id = client.sendMessage('1002', text);
      return { id, text, status: await Promise.race([status, timeout(10000)]), received: await Promise.race([received, timeout(10000)]) };
    });
    expect('text message round trip',
      msg.status?.status === 0 && msg.status.code >= 200 && msg.status.code < 300 &&
        msg.received?.contentType === 'text/plain' && /^sip:1002@/.test(msg.received?.from || ''),
      JSON.stringify(msg));

    await page.fill('#target', '9001');
    await page.check('#withVideo');
    await page.click('#btnCall');
    expect('call to 9001 reaches Established (state 3)', await waitState('#callState', 3, 20000), await page.locator('#callState').innerText());

    await page.waitForTimeout(5000);
    expect('call is still Established after 5 s', (await callState()) === '3', await page.locator('#callState').innerText());
    const statsText = await page.locator('#callStats').innerText();
    expect('live WebRTC call stats populated (SRTP/DTLS + codec)', /SRTP\/DTLS/.test(statsText) && /kbps/.test(statsText), statsText);

    const frames = await page.evaluate(async () => {
      const v = document.getElementById('remoteVideo');
      for (let i = 0; i < 20 && !(v.getVideoPlaybackQuality?.().totalVideoFrames > 0); i++) {
        await new Promise((r) => setTimeout(r, 500));
      }
      return { frames: v.getVideoPlaybackQuality?.().totalVideoFrames || 0, width: v.videoWidth, height: v.videoHeight };
    });
    expect('remote echo video frames decoded > 0', frames.frames > 0, `${frames.frames} frames ${frames.width}x${frames.height}`);

    await page.click('[data-dtmf="1"]');
    expect('DTMF send logged without error', /DTMF sent: 1/.test(await page.locator('#log').innerText()));

    await page.click('#btnHold');
    expect('hold reaches Hold (state 4)', await waitState('#callState', 4));
    await page.waitForTimeout(1500);
    expect('call stays on Hold after Asterisk answers the re-INVITE', (await callState()) === '4');
    await page.click('#btnHold');
    expect('resume returns to Established (state 3)', await waitState('#callState', 3));
    await page.waitForTimeout(1500);
    expect('call stays Established after resume', (await callState()) === '3');

    await page.click('#btnHangup');
    await page.locator('#incall').waitFor({ state: 'hidden', timeout: 10000 }).catch(() => {});
    expect('hangup closes the call', await page.locator('#incall').isHidden());
    expect('Closed is logged', /Call Closed/.test(await page.locator('#log').innerText()));

    // Multi-call auto-hold and swap against Asterisk 9001
    const multi = await page.evaluate(async () => {
      const c = window.sipClient;
      const waitEstab = (call) => new Promise((res) => {
        if (call.state === 3) return res(true);
        const timer = setTimeout(() => res(false), 12000);
        const off = call.on('stateChanged', ({ state }) => {
          if (state === 3) { clearTimeout(timer); off(); res(true); }
        });
      });
      const waitHold = (call) => new Promise((res) => {
        if (call.state === 4) return res(true);
        const timer = setTimeout(() => res(false), 8000);
        const off = call.on('stateChanged', ({ state }) => {
          if (state === 4) { clearTimeout(timer); off(); res(true); }
        });
      });
      const c1 = c.makeCall('9001', { video: false });
      const e1 = await waitEstab(c1);
      const c2 = c.makeCall('9001', { video: false });
      const h1 = await waitHold(c1);
      const e2 = await waitEstab(c2);
      c1.setHold(false);
      const h2 = await waitHold(c2);
      const r1 = await waitEstab(c1);
      const c1HeldFinal = c1.isOnHold;
      const c2HeldFinal = c2.isOnHold;
      c1.hangup();
      c2.hangup();
      await new Promise((r) => setTimeout(r, 400));
      return { e1, h1, e2, h2, r1, c1HeldFinal, c2HeldFinal };
    });
    expect('live Asterisk multi-call auto-hold and swap',
      multi.e1 && multi.h1 && multi.e2 && multi.h2 && multi.r1 && !multi.c1HeldFinal && multi.c2HeldFinal,
      JSON.stringify(multi));

    // Multi-account: add 1005 alongside 1002 on the same SipClient, verify registration & per-account MESSAGE
    const multiAcc = await page.evaluate(async (host) => {
      const c = window.sipClient;
      const timeout = (ms) => new Promise((r) => setTimeout(() => r(null), ms));
      const acc1 = c.defaultAccount;
      const regDone = new Promise((resolve) => {
        const off = c.on('accountRegistrationStateChanged', (e) => {
          if (e.account.user === '1005' && e.state === 2) { off(); resolve(true); }
          if (e.account.user === '1005' && e.state === 3) { off(); resolve(false); }
        });
      });
      const acc2 = c.addAccount({
        username: '1005',
        domain: host,
        password: 'test1005',
        wsUri: `ws://${host}:8088/ws`,
      });
      const registered2 = await Promise.race([regDone, timeout(10000)]);
      const countWhileTwo = c.accountCount;

      const text = `multi-account msg ${Date.now()}`;
      const msgReceived = new Promise((resolve) => {
        const off = c.on('accountMessageReceived', (e) => {
          if (e.body === text) { off(); resolve({ toUser: e.account.user, fromUri: e.fromUri, body: e.body }); }
        });
      });
      acc2.sendMessage('1002', text);
      const delivered = await Promise.race([msgReceived, timeout(10000)]);

      c.removeAccount(acc2);
      return {
        acc1User: acc1?.user,
        acc2User: acc2.user,
        registered2,
        countWhileTwo,
        delivered,
        countAfterRemove: c.accountCount,
        defaultAfterRemove: c.defaultAccount?.user,
      };
    }, PBX_HOST);
    expect('live Asterisk multi-account (1002 + 1005) registration & per-account MESSAGE',
      multiAcc.acc1User === '1002' && multiAcc.acc2User === '1005' && multiAcc.registered2 === true &&
        multiAcc.countWhileTwo === 2 && multiAcc.delivered?.toUser === '1002' &&
        /^sip:1005@/.test(multiAcc.delivered?.fromUri || '') &&
        multiAcc.countAfterRemove === 1 && multiAcc.defaultAfterRemove === '1002',
      JSON.stringify(multiAcc));

    await page.click('#btnUnregister');
    expect('unregister reaches Unregistered (state 0)', await waitState('#regStatus', 0));
    expect('no console errors', consoleErrors.length === 0, consoleErrors.join(' | '));
  } finally {
    await browser.close();
    server.close();
  }

  if (failures() === 0) {
    console.log('ALL_WEB_SDK_PBX_CHECKS_PASS');
  } else {
    console.log(`${failures()} FAILURES`);
    process.exitCode = 1;
  }
}

// Exit outright: a failure before the try block (browser launch) leaves the server open.
main().catch((e) => { console.error('driver error:', e); process.exit(1); });
