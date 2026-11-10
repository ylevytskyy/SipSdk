// Live check of the web SDK against the local Asterisk PBX (docker compose, see
// scripts/run-pbx.sh): registers extension 1002 over ws://<host>:8088/ws, calls the
// 9001 audio+video echo, holds and resumes, sends DTMF, hangs up and unregisters.
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

    await page.fill('#target', '9001');
    await page.check('#withVideo');
    await page.click('#btnCall');
    expect('call to 9001 reaches Established (state 3)', await waitState('#callState', 3, 20000), await page.locator('#callState').innerText());

    await page.waitForTimeout(5000);
    expect('call is still Established after 5 s', (await callState()) === '3', await page.locator('#callState').innerText());

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
