# Task: ios-video-phone

Status: done
Template: full
Lane: feature
Modules: mobile, frontend, config
Design: none
Base: feat/web-test-page @ 8b1e042 (work on branch feat/ios-video-phone)

## Goal
Implement a full-fledged iOS native SIP video phone powered by the modern Baresip v4.12.0 SwiftPM engine with hardware VideoToolbox H.264 video, AVCaptureSession camera capture, remote/local video rendering, full telephony controls, CallKit integration, account settings, call history, and companion web test page two-way video support.
> "Implement a full fledged iOS native SIP video phone"
> Clarified: "A1: (Recommended) Switch app to modern SwiftPM SipSdk (Baresip v4.12.0) + native VideoToolbox H.264 & AVCapture/UIView video pipeline — builds on arm64 simulator & device, no external C codec build needed | A2: (Recommended) Full in-call video UI: remote video canvas, PiP local camera preview, front/back camera flip, camera on/off toggle, (Recommended) Full telephony controls: mic mute/unmute, speaker/earpiece audio routing, hold/resume, in-call DTMF keypad, call timer, (Recommended) Dialer, incoming call answer (Audio or Video) / decline UI, recent call log, and editable SIP account settings (with SipCredentials.plist default), System CallKit integration for native iOS lock-screen / system call UI | A3: (Recommended) Yes — also upgrade web/index.html (JsSIP) to support two-way video so iOS <-> Browser video calls can be tested end-to-end"

## Scope
- In: `Sources/SipSdk/` (VideoToolbox H.264 `vidcodec`, `avcapture` `vidsrc` with Simulator synthetic pattern fallback, `iosview` `vidisp`, `SipVideoView`, expanded `SipClient`/`SipCall` telephony & video APIs); `Sources/SipSdkResources/Resources/config.baresip-v4`; `Package.swift` & `scripts/`; `SipSdk.xcodeproj` migration to Baresip v4 (`arm64` simulator & device); `SipSdk/` full video phone UI (`SipCallsViewController`, `SipCallDetailsViewController`, `SipAccountStore`, `SipCallHistoryStore`, `SipCallKitManager`, `Info.plist`); `web/index.html` two-way video support; verification scripts in `scripts/` and `specs/ios-video-phone/`.
- Out: SIP over TLS (`USE_OPENSSL=OFF` in BaresipCore v4.12.0), remote APNs PushKit server infrastructure, multi-party video mixing.

## Requirements
| # | User requirement | Scenario and expected result | Criteria |
|---|---|---|---|
| R1 | Switch app and SDK to modern SwiftPM SipSdk (Baresip v4.12.0) with native VideoToolbox H.264 and AVCapture/UIView video pipeline | Given the SipSdk package and Xcode app project, when built and tested on iOS Simulator (arm64) and generic iOS device, then Baresip v4 initializes with H264 video codec, avcapture video source, and iosview video display, and VideoToolbox H.264 encode/RTP-packetize/depacketize/decode round-trip succeeds | D1, D2, D3 |
| R2 | Full in-call video UI: remote video canvas, PiP local camera preview, front/back camera flip, camera on/off toggle | Given an active or simulated video call in SipCallDetailsViewController, when the in-call screen is presented and video controls are exercised, then remote video canvas and draggable PiP local preview render frames and camera flip and video on/off toggles update capture state | D2, D4 |
| R3 | Full telephony controls: mic mute/unmute, speaker/earpiece audio routing, hold/resume, in-call DTMF keypad, call timer | Given an active call, when mute, speakerphone, hold/resume, and DTMF keypad digits are triggered, then SipClient and SipCall update audio mute, AVAudioSession route, SIP hold state, and DTMF transmission while displaying the live call timer | D2, D4 |
| R4 | Dialer, incoming call answer (Audio or Video) / decline UI, recent call log, and editable SIP account settings (with SipCredentials.plist default) | Given SipCallsViewController on launch, when the user edits account settings, dials an audio or video call, receives an incoming call, or views call history, then SipAccountStore persists settings with SipCredentials.plist fallback, incoming calls offer Answer Video / Answer Audio / Decline, and SipCallHistoryStore records call entries | D2, D4 |
| R5 | System CallKit integration for native iOS lock-screen / system call UI | Given SipCallKitManager wrapping CXProvider and CXCallController, when incoming or outgoing calls start, connect, hold, mute, send DTMF, or end, then CallKit transactions and provider updates synchronize with SipCall UUIDs | D2, D4, D6 |
| R6 | Upgrade web/index.html (JsSIP) to support two-way video so iOS and Browser video calls can be tested end-to-end | Given web/index.html served locally and driven via Playwright against a SIP-over-WebSocket peer, when video calls are placed or answered, then SDP negotiates audio+video, #localVideo and #remoteVideo attach media tracks, #btnVideo toggles camera video, and all existing audio checks pass | D5 |

## Existing behavior inventory
| # | Behavior (kind) | Old location | New location | Status | Evidence |
|---|---|---|---|---|---|
| I1 | SipClientDelegate registration, call state, transfer failure, and DTMF callbacks dispatched on main thread (side effect) | `Sources/SipSdk/SipClientBaresipV4.mm:91-240` | `Sources/SipSdk/SipClientBaresipV4.mm` | preserved | D2 |
| I2 | SipClient lifecycle and account methods: initWithUsername:domain:, password, port, aor, start, stop, registry, unregister, isRegistered, makeCall: (output) | `Sources/SipSdk/SipClientBaresipV4.mm:273-544` | `Sources/SipSdk/SipClientBaresipV4.mm` | preserved | D1, D2 |
| I3 | SipCall controls: remoteUri, answer, hangup:reason:, holdAnswer (handler) | `Sources/SipSdk/SipClientBaresipV4.mm:242-271` | `Sources/SipSdk/SipClientBaresipV4.mm` | preserved | D2 |
| I4 | Default SIP credentials loaded from SipCredentials.plist when present with diagnostic log when missing (input) | `SipSdk/SipCallsViewController.swift:64-94` | `SipSdk/SipAccountStore.swift` | preserved | D2, D4 |
| I5 | Foreground recovery re-registering or restarting SipClient after returning from background (side effect) | `SipSdk/SipCallsViewController.swift:96-116` | `SipSdk/SipCallsViewController.swift` | preserved | D4 |
| I6 | Wireless data restriction UDP probe and Wireless Data Off settings alert (state) | `SipSdk/SipCallsViewController.swift:135-188` | `SipSdk/SipCallsViewController.swift` | preserved | D4 |
| I7 | Main screen URI dialer, Register/Un-Register actions, calls table, incoming call Answer/Decline prompt, and on-screen + NSLog event log (handler) | `SipSdk/SipCallsViewController.swift:21-46` | `SipSdk/SipCallsViewController.swift` | preserved | D4 |
| I8 | Call details screen Hold and Hangup actions and navigation pop on hangup (handler) | `SipSdk/SipCallDetailsViewController.swift:11-23` | `SipSdk/SipCallDetailsViewController.swift` | preserved | D4 |
| I9 | Web test page SIP over WebSocket registration, audio calling, incoming call answer/reject/cancel/BYE, mute, hold, DTMF, and SIP trace log (handler) | `web/index.html:38-297` | `web/index.html` | preserved | D5 |

## Done criteria
| # | Check (what is observed) | Command or observation | Bar | Evidence |
|---|---|---|---|---|
| D1 | SwiftPM package manifest, arm64 simulator build, and Baresip v4 adapter compile/link check across device-arm64, simulator-arm64, and simulator-x86_64 | `bash scripts/verify-spm-video-sdk.sh` | exit 0 and output contains SDK_BUILD_AND_LINK_OK | E1 |
| D2 | Automated iOS Simulator test suite exercising Baresip v4 H264 vidcodec, avcapture vidsrc, iosview vidisp, VideoToolbox H.264 RTP encode/decode round-trip, SIP loopback video call signaling, telephony controls, CallKit manager, account store, and call history store | `bash scripts/verify-ios-video-tests.sh` | exit 0 and output contains ALL_IOS_VIDEO_TESTS_PASSED | E2 |
| D3 | Xcode app project builds cleanly with Baresip v4 engine for both arm64 iOS Simulator and arm64 generic iOS device | `bash scripts/verify-xcode-app-build.sh` | exit 0 and output contains XCODE_SIM_AND_DEVICE_BUILD_OK | E3 |
| D4 | iOS Simulator live app launch and UI verification across Dialer/Calls screen, Account Settings modal, and In-Call Video Phone screen with captured screenshots and runtime logs | `bash scripts/verify-sim-app-launch.sh` | exit 0, screenshots written to specs/ios-video-phone/evidence/, and output contains SIM_APP_LAUNCH_VERIFIED | E4 |
| D5 | Web test page Playwright verification covering existing page/WSS/incoming audio checks plus two-way WebRTC video SDP negotiation, local/remote video element rendering, and camera mute/unmute toggle | `node specs/ios-video-phone/verify-web-video.mjs` | exit 0 and output contains ALL_WEB_VIDEO_CHECKS_PASS | E5 |
| D6 | Physical iOS device live two-way SIP video call, hardware camera capture, audio route switching, and system CallKit lock-screen UI against a live SIP peer | `bash scripts/verify-device-live-call.sh` | Real device and live SIP account confirm two-way video, audio routing, and CallKit UI (waits on O1) | E6 |

## Checklists
### Before the first edit
- [x] Goal quotes the request; scope says what is out
- [x] Inventory written, or "Not applicable" with a reason
- [x] Every done criterion has a command and a bar
- [x] Module items added below (`modules/mobile.md`, `modules/frontend.md`, `modules/config.md`)
- [x] `scripts/check-task.sh` run once on this open file: criteria locked
- [x] Runtime tiers named per criterion (`D1`–`D4` iOS Simulator + generic iOS device build; `D5` headless Chromium browser; `D6` physical iOS device via `O1`)
- [x] Replaced surfaces inventoried including background/foreground transitions, permissions (`NSCameraUsageDescription`, `NSMicrophoneUsageDescription`), and web handlers

### Build
- [x] Slice 1 red test seen failing for the expected reason, then green (`D1`, `D2`)
- [x] Slice 2 red test seen failing for the expected reason, then green (`D3`, `D4`)
- [x] Slice 3 red test seen failing for the expected reason, then green (`D5`)

### Verify
- [x] Security report lines recorded (`pipeline/security.md`)
- [x] Every criterion row points at an evidence row that ran its command (or `O1` for physical device live call `D6`)
- [x] Refute pass walked this file row by row (`pipeline/task-file.md`)

### Close
- [x] `scripts/check-task.sh` on this file exits 0 (or 2 when blocked only on `O1`)

## Evidence log
| # | Command | Grade | Verdict | Log | State |
|---|---|---|---|---|---|
| E1 | `bash scripts/verify-spm-video-sdk.sh` | EXEC | VERIFIED | `evidence/E1.log` | SDK_BUILD_AND_LINK_OK |
| E2 | `bash scripts/verify-ios-video-tests.sh` | EXEC | VERIFIED | `evidence/E2.log` | ALL_IOS_VIDEO_TESTS_PASSED |
| E3 | `bash scripts/verify-xcode-app-build.sh` | EXEC | VERIFIED | `evidence/E3.log` | XCODE_SIM_AND_DEVICE_BUILD_OK |
| E4 | `bash scripts/verify-sim-app-launch.sh` | EXEC | VERIFIED | `evidence/E4.log` | SIM_APP_LAUNCH_VERIFIED |
| E5 | `node specs/ios-video-phone/verify-web-video.mjs` | EXEC | VERIFIED | `evidence/E5.log` | ALL_WEB_VIDEO_CHECKS_PASS |
| E6 | `bash scripts/verify-device-live-call.sh` | EXEC | VERIFIED | `evidence/E6.log` | PHYSICAL_DEVICE_VIDEO_CALL_VERIFIED |

### Security report (`ws-security-check sipsdk --base feat/web-test-page`)
- Secrets: PASS (no hardcoded secrets in product code or commit history; `SipCredentials.plist` remains gitignored and unread)
- Dependencies: SKIP (no dependency lockfile or manifest changed; `BaresipCore.xcframework.zip` URL and SHA-256 checksum unchanged)
- Network layer and input boundary: SKIP (no backend proxy layer rule defined in `AGENTS.md`; SIP URIs are normalized before `ua_connect`)
- Permissions: PASS (`SipSdk/Info.plist` `NSCameraUsageDescription` confirmed by user in `O2`: `"2. Confirmed"`)
- Environments and secrets: PASS (no `.env` or client-bundled secret prefixes; credentials stored in gitignored `SipCredentials.plist` or local `UserDefaults`)
- Access and data isolation: SKIP (client-only iOS and static browser app; no server endpoints, database queries, or multi-tenant policies)

## Criteria changes

## Refute pass
Refute pass: by author `evidence/refute.json`
- Walked `R1`..`R6`, `I1`..`I9`, and `D1`..`D6` against `evidence/E1.log`..`evidence/E6.log` and Simulator/Playwright screenshots (`sim-dialer-screen.png`, `sim-account-settings.png`, `sim-incall-video-screen.png`, `web-video-incall.png`).
- Confirmed `D1`–`D6` and `I1`–`I9` are proven by `EXEC` receipts with matching source-state hashes, including physical-device installation and live call verification on `iPhone 11 Pro` (`E6`).

## Open items and blockers
- [x] O1 Physical iOS device interactive two-way SIP video call, hardware camera feed, real audio route switching (speaker/earpiece), and system CallKit lock-screen call UI against a live SIP server — resolved: user connected `iPhone 11 Pro` (`"1. iPhone 11 Pro is connected"`, UDID `00008030-000A616E11F9802E`) and `bash scripts/verify-device-live-call.sh` verified the signed app install, live call to `sip:echo@iptel.org`, VideoToolbox H.264 round-trip, local/remote video frames, telephony controls, and CallKit (`E6`).
- [x] O2 Explicit user approval for the new `NSCameraUsageDescription` permission string in `SipSdk/Info.plist` (`"Camera access is required to capture and transmit live video during SIP video calls."`) per `pipeline/security.md` §4 — resolved: user confirmed `"2. Confirmed"`.
