# Plan: Full-Fledged iOS Native SIP Video Phone (`ios-video-phone`)

## Milestones

- [ ] **M1 — Native Baresip v4 Video Pipeline & Telephony SDK (`Sources/SipSdk`)**: Hardware `VideoToolbox` H.264 codec (`vidcodec`), `AVCaptureSession` + Simulator synthetic pattern source (`vidsrc`), `SipVideoView` renderer (`vidisp`), and expanded `SipClient`/`SipCall` Objective-C++ APIs (video call, answer with video, mute, speakerphone, hold, DTMF, camera flip/toggle, CallKit UUID).
- [ ] **M2 — Full iOS Video Phone App, CallKit, Account & Call History (`SipSdk/` & `SipSdk.xcodeproj`)**: Migrate `SipSdk.xcodeproj` to Baresip v4 (`arm64` simulator + device), add `SipAccountStore`, `SipCallHistoryStore`, `SipCallKitManager`, redesigned `SipCallsViewController` (account editor, dialer, call history, incoming call audio/video prompt, log), and full-screen `SipCallDetailsViewController` (remote video canvas, draggable PiP local preview, call timer, DTMF keypad, mute/video/flip/speaker/hold/hangup controls).
- [ ] **M3 — Web Test Page (`web/index.html`) Two-Way Video Interop & End-to-End Verification**: Upgrade `web/index.html` with local/remote `<video>` elements, H.264 video call controls, and automated Playwright + Simulator verification.

---

## Vertical Slices

### Slice 1 — Native Baresip v4 Video Pipeline & Expanded `SipClient` / `SipCall` API (M1)
- **User stories covered**: US1, US2
- **Write set**:
  - `Sources/SipSdk/include/SipClient.h`
  - `Sources/SipSdk/SipClientBaresipV4.mm`
  - `Sources/SipSdkResources/Resources/config.baresip-v4`
  - `Package.swift`
  - `scripts/generate-spm-package.sh`
  - `scripts/check-baresip-adapter.sh`
  - `scripts/verify-spm-video-sdk.sh`
  - `scripts/verify-ios-video-tests.sh`
  - `Tests/SipSdkRunner/main.mm`
- **Interfaces**:
  - `SipVideoView : UIView` (`-renderPixelBuffer:`, `-clearVideo`, `videoSize`)
  - `SipClient`: `displayName`, `transport`, `videoEnabledByDefault`, `microphoneMuted`, `speakerEnabled`, `localVideoEnabled`, `cameraPosition`, `remoteVideoView`, `localVideoView`, `-makeCall:withVideo:`, `-switchCamera`, `-setSpeakerphone:`, `-setMuted:`, `-hasVideoCodecRegistered`, `-hasVideoSourceRegistered`, `-hasVideoDisplayRegistered`, `+testH264RoundTripWithWidth:height:`
  - `SipCall`: `callId`, `hasVideo`, `isOutgoing`, `onHold`, `durationSeconds`, `uuid`, `-answerWithVideo:`, `-setHold:`, `-sendDtmf:`, `-setVideoEnabled:`, `-requestKeyframe`
- **Acceptance criteria**:
  1. Given a started `SipClient`, when `hasVideoCodecRegistered`, `hasVideoSourceRegistered`, and `hasVideoDisplayRegistered` are queried, then all three return `YES` (`H264`, `avcapture`, `iosview`).
  2. Given a `640x480` YUV420P frame, when encoded via `VideoToolbox` H.264 into RFC 6184 RTP packets and decoded back, then the decoder outputs a valid `640x480` YUV420P frame (`+testH264RoundTripWithWidth:height:` returns `YES`).
  3. Given a started `SipClient`, when `makeCall:withVideo:YES`, `setMuted:`, `setHold:`, `sendDtmf:`, `switchCamera`, and `setLocalVideoEnabled:` are invoked, then the call state and media flags update deterministically without error.
- **Verification command**: `bash scripts/verify-spm-video-sdk.sh` and `bash scripts/verify-ios-video-tests.sh` (Simulator tier)
- **Blocked-by**: none
- **Fact owners**: `SipClient` owns engine/registration/media device state; `SipCall` owns per-call signaling/hold/video state.
- **Inventory rows**: `I1`, `I2`, `I3`
- **Old types**: Legacy `SipSdk/SipClient.h` and `SipSdk/SipClient.mm` (replaced in target build by `Sources/SipSdk/include/SipClient.h` and `Sources/SipSdk/SipClientBaresipV4.mm`).
- **Observable failure**: Calling `makeCall:withVideo:YES` fails to include `m=video` / `H264` in SDP or crashes when encoding/decoding video frames.
- **Substitutions**: Simulator uses an animated synthetic YUV420P camera pattern in `avcapture` when no hardware `AVCaptureDevice` is present (real camera verified on device via `O1`).

### Slice 2 — iOS Video Phone App UI, CallKit, Account Store, and Call History (M2)
- **User stories covered**: US2, US3, US4
- **Write set**:
  - `SipSdk.xcodeproj/project.pbxproj`
  - `SipSdk/SipSdk-Bridging-Header.h`
  - `SipSdk/Info.plist`
  - `SipSdk/SipAccountStore.swift`
  - `SipSdk/SipCallHistoryStore.swift`
  - `SipSdk/SipCallKitManager.swift`
  - `SipSdk/SipCallsViewController.swift`
  - `SipSdk/SipCallDetailsViewController.swift`
  - `SipSdk/CallCell.swift`
  - `scripts/verify-xcode-app-build.sh`
  - `scripts/verify-sim-app-launch.sh`
- **Interfaces**:
  - `SipAccountProfile` & `SipAccountStore` (`load()`, `save(_:)`, `hasCredentials`)
  - `SipCallRecord` & `SipCallHistoryStore` (`records`, `add(_:)`, `clear()`)
  - `SipCallKitManager` (`reportIncomingCall(_:completion:)`, `reportOutgoingCallStarted(_:)`, `reportCallConnected(_:)`, `reportCallEnded(_:)`, `setHeld(_:onHold:)`, `setMuted(_:muted:)`)
  - `SipCallsViewController` & `SipCallDetailsViewController`
- **Acceptance criteria**:
  1. Given `SipSdk.xcodeproj`, when built for `arm64` iOS Simulator and `arm64` generic iOS device, then both compile and link against `BaresipCore.xcframework` with zero errors.
  2. Given the app launched on Simulator (`iPhone 18 Pro`), when the main screen, Account Settings modal, and In-Call Video Phone screen are driven, then local/remote `SipVideoView` canvases, PiP preview, DTMF keypad, and call controls render cleanly and Baresip v4 initializes with `H264`.
- **Verification command**: `bash scripts/verify-xcode-app-build.sh` and `bash scripts/verify-sim-app-launch.sh` (Simulator tier; real device CallKit/camera/audio in `O1`)
- **Blocked-by**: Slice 1
- **Fact owners**: `SipAccountStore` owns persisted SIP account settings; `SipCallHistoryStore` owns recent call records; `SipCallKitManager` owns CallKit `CXProvider`/`CXCallController` transactions; `SipCallsViewController` owns the call list and registration UI; `SipCallDetailsViewController` owns the active in-call video/telephony UI.
- **Inventory rows**: `I4`, `I5`, `I6`, `I7`, `I8`
- **Observable failure**: App fails to link on `arm64` simulator, loses existing storyboard outlet bindings, or fails to render video preview and call controls.
- **Substitutions**: none.

### Slice 3 — Companion Web Test Page (`web/index.html`) Two-Way Video Upgrade (M3)
- **User stories covered**: US5
- **Write set**:
  - `web/index.html`
  - `specs/ios-video-phone/verify-web-video.mjs`
  - `AGENTS.md`
  - `docs/SPM_MIGRATION.md`
- **Interfaces**:
  - DOM controls in `web/index.html`: `#enableVideo` checkbox, `#localVideo`, `#remoteVideo`, `#btnVideo` (camera mute/unmute), `#btnAnswerAudio`, plus preserved `#btnRegister`, `#btnUnregister`, `#btnCall`, `#btnAnswer`, `#btnReject`, `#btnHangup`, `#btnMute`, `#btnHold`, `[data-dtmf]`.
- **Acceptance criteria**:
  1. Given `web/index.html` connected to the test SIP-over-WebSocket server, when an incoming or outgoing video call is answered, then the SDP offer/answer includes `m=video`, `#localVideo` and `#remoteVideo` attach `MediaStream` tracks, `#btnVideo` toggles the local video track enabled state, and all existing audio/incoming/WSS checks pass with zero console errors.
- **Verification command**: `node specs/ios-video-phone/verify-web-video.mjs`
- **Blocked-by**: Slice 1
- **Fact owners**: `web/index.html` session controller owns WebRTC `RTCPeerConnection` and local/remote video element bindings.
- **Inventory rows**: `I9`
- **Observable failure**: Video calls from/to `web/index.html` strip video m-lines, fail to render `#remoteVideo`/`#localVideo`, or break existing audio-only Playwright tests.
- **Substitutions**: Headless Chromium uses `--use-fake-device-for-media-stream` synthetic camera/mic feed during automated Playwright runs.

---

## Progress

- (pending execution)
