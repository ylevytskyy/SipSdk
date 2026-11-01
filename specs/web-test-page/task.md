# Task: web-test-page

Status: blocked
Template: full
Lane: feature
Modules: none (standalone static test page; no backend)
Design: none
Base: master @ acb3d27 (work on branch feat/web-test-page, stacked on chore/xcode27-deployment-target)

## Goal
Build a simple test web page for testing calls between the mobile app and the page.
> "реалізовувати тестову вебсторінку для того, щоб можна було тестувати відеодзвінки між мобільною апкою і цією простою вебсторінкою"
> Clarified: "ні, лише вебсторінка, будемо використовувати iptel.org для тестування. спочатку робимо сторінку і валідуємо аудіо, а відео окремо. Сторінка потрібна тільки на цьому Mac"

## Scope
- In: `web/index.html` — a single-file JsSIP client (register, call/answer/hangup, mute, hold, DTMF, remote audio, log) against iptel.org's WSS endpoint; `projects.yaml` run/port entries; `AGENTS.md` command notes; `.gitignore` specs evidence.
- Out: video (separate phase — SDK has no video codec anyway), SIP server infrastructure (iptel.org used as-is), mobile-app changes, publishing/deploy.

## Requirements
| # | User requirement | Scenario and expected result | Criteria |
|---|---|---|---|
| R1 | Test web page to call between mobile app and the page via iptel.org, audio first, localhost only | Given the page is served on localhost and a real iptel.org account, when the user registers and dials a SIP URI (e.g. the app's AOR or echo@iptel.org), then a SIP session with audio is established and remote audio plays | D1, D2, D3, D4 |

## Existing behavior inventory
Not applicable: adds new files only (`web/`, `specs/web-test-page/`); other edits are docs/registry (AGENTS.md, projects.yaml, .gitignore), no existing product code or UI changes (E3).

## Done criteria
| # | Check (what is observed) | Command or observation | Bar | Evidence |
|---|---|---|---|---|
| D1 | Page serves on localhost | `curl -s http://localhost:5101/ -o /tmp/sipsdk-page.html && grep -c 'SipSdk test client' /tmp/sipsdk-page.html` | exit 0, count >= 1 | E1 |
| D2 | Page initializes: JsSIP global present, zero console errors, all controls rendered | `node specs/web-test-page/verify-page.mjs` (Playwright: load, console capture, DOM check) | exit 0 — script asserts and exits non-zero on failure | E2 |
| D3 | WSS transport to iptel.org works: page logs a successful connect and a SIP-level response after Register click | `node specs/web-test-page/verify-wss.mjs` (Playwright: fill dummy creds, click Register, read #log) | log contains `UA connected` and `Registration failed` or `Registered` (a SIP auth response proves the ws→SIP path; real auth needs real creds — O1) | E4 |
| D4 | Live audio call with a real iptel.org account | user-driven: register + call sip:echo@iptel.org | user confirms audio — open item, needs user's account (O1) | O1 |

## Checklists
### Before the first edit
- [x] Goal quotes the request; scope says what is out
- [x] Inventory written, or "Not applicable" with a reason
- [x] Every done criterion has a command and a bar
- [x] Module items added below — none apply
- [x] `scripts/check-task.sh` run once on this open file: criteria locked

### Build
- [x] Static page written; no test infra exists for browser SIP — verification is live-run evidence

### Verify
- [x] Security report lines recorded (`pipeline/security.md`): `ws-security-check sipsdk --base master` → secrets PASS, dependencies REVIEW (Podfile.lock: only the `PODFILE CHECKSUM` line changed, taresip stays 0.1.1 — reviewed, benign), network SKIP (no --network-layer; page adds one outbound endpoint `wss://sip.iptel.org:8081`, which is the tool's purpose), permissions PASS
- [ ] Every criterion row points at an evidence row that ran its command — D4 waits on O1
- [x] Refute pass walked this file row by row (`pipeline/task-file.md`)

### Close
- [ ] `scripts/check-task.sh` on this file exits 0

## Evidence log
| # | Command | Grade | Verdict | Log | State |
|---|---|---|---|---|---|
| E1 | `curl -s http://localhost:5101/ -o /tmp/sipsdk-page.html && grep -c 'SipSdk test client' /tmp/sipsdk-page.html` | EXEC | VERIFIED | `evidence/e1-serve.log` | feat/web-test-page + web/,specs/ |
| E2 | `node specs/web-test-page/verify-page.mjs` | EXEC | VERIFIED | `evidence/e2-page.log` | feat/web-test-page + web/,specs/ |
| E3 | `git diff master --stat` | EXEC | VERIFIED | `evidence/e3-diffstat.log` | feat/web-test-page |
| E4 | `node specs/web-test-page/verify-wss.mjs` | EXEC | VERIFIED | `evidence/e4-wss.log` | feat/web-test-page + web/,specs/ |

## Criteria changes
- Modules — 'design' removed from the parenthetical: the checker treats the word as a design-task flag. Meaning unchanged. user: "Так, приймаю всі три" (2026-10-31)
- D1 — command rewritten pipe-free (`-o` to a temp file + `grep -c`): the checker requires the log's first line to match the criterion command verbatim and a `|` cannot appear in a table cell; the check and bar are unchanged (page must still contain 'SipSdk test client'). Not weaker. user: "Так, приймаю всі три" (2026-10-31)
- D3 — bar reworded to what JsSIP 3.13 actually logs: `UA connected` (transport) + `Registration failed`/`Registered` (SIP response). The locked wording named `WebSocket connected`, which JsSIP 3.13 never emits; the semantic bar (connect + SIP-level response proves the ws→SIP path) is unchanged. Not weaker. user: "Так, приймаю всі три" (2026-10-31)
- Note (not a contract change): `web/jssip-3.13.9.min.js` (vendored esbuild bundle) added under the existing `web/` scope entry; D2's command cell trimmed to just the script path (the parenthetical was descriptive).

## Refute pass
Refute pass: by author (same-author — no delegate available for browser runs) · report: `evidence/refute.json` · no rows broken.
Self-review notes: E1/E2/E4 logs re-read — bars met verbatim; D3's "Registration failed: Not Found" is a real SIP 404 from iptel.org (proves transport, not auth); D4 not claimed — left open.

## Open items and blockers
- [ ] O1 Live audio call (real registration + two-way audio to `sip:echo@iptel.org` and to the mobile app) — needs: the user's iptel.org credentials entered in the page, plus the app registered on iptel.org; only the user can supply these.
