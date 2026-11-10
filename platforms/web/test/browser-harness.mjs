// Shared helpers for the browser checks: a static server for the sample page and a
// headless Chrome with fake camera and microphone.

import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';

export const REPO_ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../../..');
export const EVIDENCE_DIR = path.resolve(path.dirname(fileURLToPath(import.meta.url)), 'evidence');

const SERVED = [path.join(REPO_ROOT, 'platforms/web/'), path.join(REPO_ROOT, 'web/')];
const TYPES = { '.js': 'text/javascript', '.mjs': 'text/javascript', '.html': 'text/html', '.json': 'application/json' };

/** Serves platforms/web and web/ from the repo root (the sample loads ../../../web/jssip). */
export async function startStaticServer(host = '127.0.0.1') {
  const server = http.createServer((req, res) => {
    let p = decodeURIComponent(req.url.split('?')[0]);
    if (p.endsWith('/')) p += 'index.html';
    const file = path.join(REPO_ROOT, path.normalize(p));
    if (!SERVED.some((dir) => file.startsWith(dir)) || !fs.existsSync(file) || !fs.statSync(file).isFile()) {
      res.writeHead(404); res.end('not found'); return;
    }
    res.writeHead(200, { 'Content-Type': TYPES[path.extname(file)] || 'application/octet-stream' });
    fs.createReadStream(file).pipe(res);
  });
  await new Promise((r) => server.listen(0, host, r));
  return { server, origin: `http://${host}:${server.address().port}` };
}

function loadPlaywright() {
  try {
    return createRequire(import.meta.url)('playwright');
  } catch {
    const globalRoot = execFileSync('npm', ['root', '-g'], { encoding: 'utf8' }).trim();
    return createRequire(path.join(globalRoot, 'noop.js'))('playwright');
  }
}

export async function launchBrowser() {
  const { chromium } = loadPlaywright();
  const args = [
    '--use-fake-device-for-media-stream',
    '--use-fake-ui-for-media-stream',
    '--autoplay-policy=no-user-gesture-required',
  ];
  try {
    return await chromium.launch({ headless: true, args });
  } catch {
    return chromium.launch({ headless: true, channel: 'chrome', args });
  }
}

/** Records every RTCPeerConnection the page creates in window.__pcs. */
export const TRACK_PEER_CONNECTIONS = () => {
  const Native = window.RTCPeerConnection;
  window.__pcs = [];
  window.RTCPeerConnection = function (...a) {
    const pc = new Native(...a);
    window.__pcs.push(pc);
    return pc;
  };
  window.RTCPeerConnection.prototype = Native.prototype;
};

export function checker() {
  let failures = 0;
  const expect = (name, ok, detail = '') => {
    console.log(`${ok ? 'PASS' : 'FAIL'} ${name}${detail ? ' — ' + detail : ''}`);
    if (!ok) failures++;
    return ok;
  };
  return { expect, failures: () => failures };
}
