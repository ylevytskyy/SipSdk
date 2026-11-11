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
The native iOS implementation is consolidated under `platforms/ios/` with a modern Swift 5.9/6
SDK (`platforms/ios/Sources/SipSdk`), comprehensive unit & PBX integration test suite
(`platforms/ios/Tests/SipSdkTests`), and a modern SwiftUI sample application
(`platforms/ios/sample/SipPhone.xcodeproj`). It runs on the Baresip v4.12.0 XCFramework
(`BaresipCore.xcframework`) and the common portable C++ core (`core/` / `SipCore`) with
native Apple `VideoToolbox` H.264 hardware encoding/decoding, `AVCaptureSession` +
Simulator synthetic test-pattern camera capture, `SipVideoView` CALayer YUV420P rendering,
full reactive telephony controls (`@Observable @MainActor SipClient` and `SipCall`),
CallKit integration (`SipCallKitCoordinator`), VoIP PushKit (`SipPushRegistry`),
zero-downtime network handover (`resetTransports`), and call history.
The companion web test page (`web/index.html`) supports two-way WebRTC audio + H.264 video
over `wss://sip.iptel.org:8081`. TLS on the native C stack remains disabled (`USE_OPENSSL=OFF`):
no SIP over TLS in `BaresipCore`.

## Stack & tooling
- SwiftPM package (`Package.swift`, tools 5.9): `SipSdk` modern Swift target
  (`platforms/ios/Sources/SipSdk`), portable `SipCore` C++20 target (`core/`),
  and binary target `BaresipCore` pinned by SHA-256 checksum.
- iOS deployment target: iOS 17.0+; sample app (`platforms/ios/sample/SipPhone.xcodeproj`)
  Swift 5.0+, builds for both `arm64` iOS Simulator and `arm64` generic iOS Device.
- Never commit real SIP credentials. `AccountStore` defaults to local Docker Asterisk PBX
  (`1001` / `test1001`) and persists user edits to `UserDefaults`.

## Commands
- Build and verify Windows Platform SDK (.NET / C# Bindings, Media Foundation drivers, and WPF Sample):
  `bash scripts/verify-windows-sdk.sh`
- Compile Baresip v4.12.0 and libre v4.12.0 static prebuilt libraries for Windows target:
  `bash scripts/build-baresip-windows.sh`
- Build and verify Desktop Native Targets (macOS host & Linux Docker CI runner, CTest suite, and `sip_cli` against Asterisk PBX):
  `bash scripts/verify-desktop-sdk.sh`
- Build and verify Native macOS GUI Sample (SwiftUI app `SipSdkMac.xcodeproj`, live launch & registration against Asterisk, window screenshot):
  `bash scripts/verify-macos-gui.sh`
- Compile Baresip v4.12.0 and libre v4.12.0 static prebuilts for desktop targets:
  `bash scripts/build-baresip-desktop.sh`
- Build and verify Android Platform SDK (`sipsdk.aar`), multi-arch slices (`arm64-v8a`, `armeabi-v7a`, `x86_64`), symbol export audit, registration & audio calling test suites against Asterisk PBX:
  `bash scripts/verify-android-sdk.sh`
- Compile Baresip v4.12.0 and libre v4.12.0 static prebuilt libraries for Android ABIs (`arm64-v8a`, `armeabi-v7a`, `x86_64`):
  `bash scripts/build-baresip-android.sh`
- Verify SwiftPM iOS SDK across slices (`arm64-simulator`, `x86_64-simulator`, `arm64-device`):
  `bash scripts/verify-spm-video-sdk.sh`
- Run automated iOS Simulator test suite (engine lifecycle, VideoToolbox H.264 RTP round-trip, `SipVideoView`, `SipClient`/`SipCall` controls, `SipCallKitCoordinator`, `SipPushRegistry`, Asterisk PBX registration & video echo call):
  `bash scripts/verify-ios-video-tests.sh`
- Run the same iOS Simulator test suite under AddressSanitizer (build in `.build-asan`; `--self-test` proves a planted overflow is caught):
  `bash scripts/verify-ios-asan-tests.sh`
- Build `SipPhone.xcodeproj` for both `arm64` iOS Simulator and `arm64` generic iOS device:
  `bash scripts/verify-xcode-app-build.sh`
- Launch `SipPhone.app` on a booted iOS Simulator and verify Keypad / Home screen with screenshots:
  `bash scripts/verify-sim-app-launch.sh`
- Manifest check: `swift package dump-package`, `swift package resolve`.
- Regenerate `Package.swift` for a new release: `bash scripts/generate-spm-package.sh <version>`.
- Build and verify Web Platform SDK (`platforms/web`, JsSIP + WebRTC: unit tests, headless Chrome sample check; `--pbx` adds a live Asterisk 1002 → 9001 call):
  `bash scripts/verify-web-sdk.sh`
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
- `platforms/ios/Sources/SipSdk` — Modern Swift 5.9/6 iOS SDK (`SipClient.swift`, `SipCall.swift`, `SipVideoView.swift`, `SipCallKitCoordinator.swift`, `SipPushRegistry.swift`, `IosVideoCapture.swift`, `IosVideoRenderer.swift`, `IosAudioManager.swift`).
- `platforms/ios/sample/SipPhone` — Modern SwiftUI sample application with `SipPhone.xcodeproj`.
- `platforms/ios/Tests/SipSdkTests` — Automated iOS unit and Asterisk PBX video integration test suite.
- `core/` — Portable C++20 core engine (`call_session.cpp`, `engine.cpp`, `media_driver.cpp`, `sip_abi.cpp`) with flat C ABI (`core/include/sip_abi.h`).
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
