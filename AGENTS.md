# AGENTS.md — sipsdk · iOS Native SIP Video Phone (SwiftPM + Baresip v4.12.0 + VideoToolbox H.264)

Instructions for any coding agent working in this repo, in any harness. This is the project
layer; the shared engineering core and task pipeline live in the workspace library.
Precedence: direct user instruction > this file > the shared core.

## Language

Use simple, clear English for this project: replies, UI text, design materials,
documentation, and commit messages. This project rule overrides global language
preferences unless the user explicitly requests another language. Use short
sentences and familiar words. Keep code identifiers and quoted output unchanged.

## Shared core

Shared engineering core and task pipeline: `../../agents/CORE.md`, `../../agents/pipeline.md`
— for a harness without its own process layer. A harness that has one uses it; this file
is project facts only.

## Status
Both the SwiftPM `SipSdk` package (`Package.swift`) and the iOS Video Phone application
(`SipSdk.xcodeproj`) run on the modern Baresip v4.12.0 XCFramework (`BaresipCore.xcframework`)
with native Apple `VideoToolbox` H.264 encoding/decoding (`vidcodec`), `AVCaptureSession` +
Simulator synthetic test-pattern camera capture (`vidsrc` `"avcapture"`), `SipVideoView` UIKit
rendering (`vidisp` `"iosview"`), full telephony controls, CallKit (`SipCallKitManager`),
editable SIP account settings (`SipAccountStore`), and call history (`SipCallHistoryStore`).
The companion web test page (`web/index.html`) supports two-way WebRTC audio + H.264 video
over `wss://sip.iptel.org:8081`. TLS on the native C stack remains disabled (`USE_OPENSSL=OFF`):
no SIP over TLS in `BaresipCore`.

## Stack & tooling
- SwiftPM package (`Package.swift`, tools 5.9): `SipSdk` ObjC++ adapter target
  (`Sources/SipSdk/SipClientBaresipV4.mm`, public headers in `Sources/SipSdk/include/`),
  `SipSdkResources` (copies `config.baresip-v4`), binary target `BaresipCore` fetched from
  GitHub Release `baresip-core-v4.12.0` and pinned by SHA-256 checksum.
- iOS deployment target: package 12.1; Xcode app (`SipSdk.xcodeproj`) 15.0, Swift 5.0,
  builds for both `arm64` iOS Simulator and `arm64` iOS Device.
- Never commit real SIP credentials; `SipSdk/SipCredentials.plist` is gitignored.
  `SipAccountStore` loads user-edited settings from `UserDefaults` and falls back to
  `SipCredentials.plist`.

## Commands
- Build and verify Desktop Native Targets (macOS host & Linux Docker CI runner, CTest suite, and `sip_cli` against Asterisk PBX):
  `bash scripts/verify-desktop-sdk.sh`
- Compile Baresip v4.12.0 and libre v4.12.0 static prebuilts for desktop targets:
  `bash scripts/build-baresip-desktop.sh`
- Build and verify Android Platform SDK (`sipsdk.aar`), multi-arch slices (`arm64-v8a`, `armeabi-v7a`, `x86_64`), symbol export audit, registration & audio calling test suites against Asterisk PBX:
  `bash scripts/verify-android-sdk.sh`
- Compile Baresip v4.12.0 and libre v4.12.0 static prebuilt libraries for Android ABIs (`arm64-v8a`, `armeabi-v7a`, `x86_64`):
  `bash scripts/build-baresip-android.sh`
- Verify SwiftPM SDK & Baresip v4 adapter across all slices (`device-arm64`, `simulator-arm64`, `simulator-x86_64`):
  `bash scripts/verify-spm-video-sdk.sh`
- Run automated iOS Simulator test suite (Baresip v4 engine, VideoToolbox H.264 RTP round-trip, `SipVideoView`, `SipClient`/`SipCall` controls, `SipCallKitManager`, `SipAccountStore`, `SipCallHistoryStore`):
  `bash scripts/verify-ios-video-tests.sh`
- Build `SipSdk.xcodeproj` for both `arm64` iOS Simulator and `arm64` generic iOS device:
  `bash scripts/verify-xcode-app-build.sh`
- Launch `SipSdk.app` on a booted iOS Simulator and verify Dialer, Account Settings modal, and In-Call Video Phone screens with screenshots:
  `bash scripts/verify-sim-app-launch.sh`
- Manifest check: `swift package dump-package`, `swift package resolve`.
- Regenerate `Package.swift` for a new release: `bash scripts/generate-spm-package.sh <version>`.
- Web test page (`web/index.html` + vendored `web/jssip-3.13.9.min.js`):
  serve with `python3 -m http.server 5101 --directory web`, open `http://localhost:5101`.
  Automated Playwright checks (no real credentials required):
  - Audio + Video SDP & UI verification: `node specs/ios-video-phone/verify-web-video.mjs`
  - Incoming call signaling check: `node specs/web-test-page/verify-incoming.mjs`
- Local Asterisk 22 Video PBX (Docker):
  - Start PBX (with automatic host LAN IP NAT detection): `bash scripts/run-pbx.sh` (or `docker compose up -d`)
  - Stop PBX: `bash scripts/stop-pbx.sh` (or `docker compose down`)
  - Verify PBX health, UDP registration (1001), WebSocket registration (1002), and video echo call (9001): `bash scripts/verify-pbx.sh`

## Repo layout & gotchas
- `Sources/SipSdk/SipClientBaresipV4.mm` — ObjC++ Baresip v4 adapter + `VideoToolbox` H.264 `vidcodec` + `avcapture` `vidsrc` + `iosview` `vidisp` + `SipVideoView`; `Sources/SipSdk/include/SipClient.h` (mirrored at `SipSdk/SipClient.h`) — public API.
- `Sources/SipSdkResources/` — resource bundle + `Resources/config.baresip-v4`.
- `SipSdk/` — iOS Video Phone UIKit app (`SipCallsViewController.swift`, `SipCallDetailsViewController.swift`, `SipAccountStore.swift`, `SipCallHistoryStore.swift`, `SipCallKitManager.swift`, `CallCell.swift`).
- `Tests/SipSdkRunner/main.mm` — automated iOS Simulator test runner compiled and spawned by `scripts/verify-ios-video-tests.sh`.
- ABI trap: libre's public `struct sa` changes size with `HAVE_UNIXSOCK` (`re_sa.h` union member `sockaddr_un`). Build flags that flip it (`USE_UNIXSOCK=OFF`) change `struct sa`/`struct sip_msg` layout for every consumer that compiles the headers without the matching define — `msg->scode` then reads 0 and `BEVENT_REGISTER_OK`/`FAIL` never fire ("Register hangs"). Keep `-DUSE_UNIXSOCK=ON` in `scripts/build-baresip-xcframework.sh`.

## Device tiers
- Compile/package validation → `bash scripts/verify-spm-video-sdk.sh` and `bash scripts/verify-xcode-app-build.sh`.
- Simulator runtime & UI verification → `bash scripts/verify-ios-video-tests.sh` and `bash scripts/verify-sim-app-launch.sh`.
- Web client verification → `node specs/ios-video-phone/verify-web-video.mjs`.
- Physical device live two-way SIP video call, hardware camera feed, audio route switching, and system CallKit lock-screen UI → physical iOS device with live SIP credentials (`bash scripts/verify-device-live-call.sh`).
- SIP over TLS → unsupported in this release (`USE_OPENSSL=OFF`).
- E2E test tool: `simctl` + custom runner on iOS Simulator; Playwright (`chromium`) for `web/index.html`.

## Delivery
- CI: GitHub Actions — `ios-build.yml`, `baresip-xcframework.yml`, `publish-baresip-xcframework.yml`.
- Release: GitHub Releases — `BaresipCore.xcframework.zip` published by
  `publish-baresip-xcframework.yml`, pinned in `Package.swift`.
- Deploy target: library consumed via SwiftPM; app installed to iOS Simulator or physical device via Xcode.
- Environments: local macOS + iOS Simulator / physical iOS device + GitHub Actions.
- Secrets location: none committed; SIP test credentials in gitignored `SipSdk/SipCredentials.plist` or runtime `UserDefaults`.
- Monitoring: unknown (none).

## Call audio regression
- Native calls need a configured and active `AVAudioSession` (`PlayAndRecord` / `VoiceChat`) before Baresip starts microphone capture. Missing activation can close a call immediately after the remote answer.
- `bash scripts/verify-ios-video-tests.sh` includes loopback peers that answer outgoing audio and H.264 video calls and assert that they stay connected. These checks do not prove two-way physical-device media; verify that separately with a real peer.
