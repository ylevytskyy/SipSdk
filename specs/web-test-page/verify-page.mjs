// Verifies the test page loads cleanly: JsSIP present, no console errors, all controls rendered.
// Usage: node specs/web-test-page/verify-page.mjs
import { createRequire } from 'module';
const require = createRequire(import.meta.url);
const { chromium } = require('/Users/lion/.nvm/versions/node/v24.13.1/lib/node_modules/playwright');

const URL = 'http://localhost:5101/';
const errors = [];
let browser;

try {
  try {
    browser = await chromium.launch({ headless: true });
  } catch {
    browser = await chromium.launch({ headless: true, channel: 'chrome' });
  }
  const page = await browser.newPage();
  page.on('console', (m) => { if (m.type() === 'error') errors.push(m.text()); });
  page.on('pageerror', (e) => errors.push(`pageerror: ${e.message}`));

  await page.goto(URL, { waitUntil: 'networkidle', timeout: 30000 });

  const checks = {
    jssip: await page.evaluate(() => typeof window.JsSIP === 'object' && typeof window.JsSIP.UA === 'function'),
    sipUri: await page.locator('#sipUri').count() === 1,
    sipPass: await page.locator('#sipPass').count() === 1,
    wsUri: await page.locator('#wsUri').inputValue(),
    btnRegister: await page.locator('#btnRegister').isEnabled(),
    target: await page.locator('#target').inputValue(),
    quickDials: await page.locator('[data-dial]').count(),
    dtmfKeys: await page.locator('[data-dtmf]').count(),
    remoteAudio: await page.locator('#remoteAudio').count() === 1,
    callDisabled: await page.locator('#btnCall').isDisabled(),
    logText: await page.locator('#log').innerText(),
  };

  let fail = 0;
  const expect = (name, ok, detail = '') => {
    console.log(`${ok ? 'PASS' : 'FAIL'} ${name}${detail ? ' — ' + detail : ''}`);
    if (!ok) fail++;
  };

  expect('JsSIP global + UA ctor', checks.jssip);
  expect('no console errors', errors.length === 0, errors.join(' | '));
  expect('#sipUri present', checks.sipUri);
  expect('#sipPass present', checks.sipPass);
  expect('#wsUri default', checks.wsUri === 'wss://sip.iptel.org:8081', checks.wsUri);
  expect('Register enabled', checks.btnRegister);
  expect('Call disabled pre-register', checks.callDisabled);
  expect('target default echo', checks.target === 'sip:echo@iptel.org', checks.target);
  expect('2 quick-dial buttons', checks.quickDials === 2, String(checks.quickDials));
  expect('12 DTMF keys', checks.dtmfKeys === 12, String(checks.dtmfKeys));
  expect('remoteAudio element', checks.remoteAudio);
  expect('boot log line', checks.logText.includes('Page ready'), JSON.stringify(checks.logText));

  await page.screenshot({ path: 'specs/web-test-page/evidence/page.png', fullPage: true });
  console.log(fail === 0 ? 'ALL PASS' : `${fail} FAILURES`);
  process.exitCode = fail === 0 ? 0 : 1;
} finally {
  if (browser) await browser.close();
}
