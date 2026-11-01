// Verifies the WSS->SIP path to iptel.org: dummy creds -> WebSocket connects -> SIP-level response.
// Usage: node specs/web-test-page/verify-wss.mjs
import { createRequire } from 'module';
const require = createRequire(import.meta.url);
const { chromium } = require('/Users/lion/.nvm/versions/node/v24.13.1/lib/node_modules/playwright');

const URL = 'http://localhost:5101/';
const WAIT_MS = 45000;
let browser;

try {
  try {
    browser = await chromium.launch({ headless: true });
  } catch {
    browser = await chromium.launch({ headless: true, channel: 'chrome' });
  }
  const page = await browser.newPage();
  page.on('console', (m) => { if (m.type() === 'error') console.log('console.error:', m.text()); });
  page.on('pageerror', (e) => console.log('pageerror:', e.message));
  await page.goto(URL, { waitUntil: 'networkidle', timeout: 30000 });

  await page.fill('#sipUri', 'devin-probe@iptel.org');
  await page.fill('#sipPass', 'not-a-real-password');
  await page.click('#btnRegister');

  let logText = '';
  const deadline = Date.now() + WAIT_MS;
  while (Date.now() < deadline) {
    logText = await page.locator('#log').innerText();
    if (logText.includes('Registration failed') || /Registered as/.test(logText)) break;
    await page.waitForTimeout(1000);
  }

  console.log('--- page log ---');
  console.log(logText);
  console.log('----------------');

  const connected = logText.includes('UA connected') || logText.includes('WebSocket connected');
  const sipAnswer = logText.includes('Registration failed') || /Registered as|Registered:/.test(logText);

  let fail = 0;
  const expect = (name, ok) => { console.log(`${ok ? 'PASS' : 'FAIL'} ${name}`); if (!ok) fail++; };
  expect('transport connected to wss://sip.iptel.org:8081', connected);
  expect('SIP-level response (registered or auth challenge)', sipAnswer);

  process.exitCode = fail === 0 ? 0 : 1;
} finally {
  if (browser) await browser.close();
}
