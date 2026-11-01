# Spec: Full-Fledged iOS Native SIP Video Phone (`ios-video-phone`)

## 1. Problem & Solution Framing

### Problem
`SipSdk` has a staged Baresip v4.12.0 SwiftPM adapter (`Sources/SipSdk/SipClientBaresipV4.mm`), while the iOS app (`SipSdk.xcodeproj`) still links the legacy `taresip 0.1.1` CocoaPods library (which lacks an `arm64` simulator slice, has no video codec, and hardcodes `VIDMODE_OFF`). The app UI is a minimal 2019 prototype with only a URI text field, Register/Unregister buttons, a basic call table, and a Hold/Hangup detail screen, and the companion web test page (`web/index.html`) is locked to audio-only calls.

### Solution
1. **Migrate `SipSdk.xcodeproj` to the modern Baresip v4.12.0 engine (`BaresipCore.xcframework` + `Sources/SipSdk`)** so both the SwiftPM package and the iOS app share a single implementation that builds natively for `arm64` iOS Simulator and `arm64` iOS devices.
2. **Implement a complete native iOS video pipeline inside `Sources/SipSdk`**:
   - **Hardware H.264 `vidcodec` (`VideoToolbox` + `re_h264`)**: Real-time Constrained Baseline (`42e01f`) H.264 encoding via `VTCompressionSession`, RFC 6184 RTP packetization (`h264_nal_send` with Single NAL and FU-A fragmentation), RFC 6184 depacketization (Single NAL, STAP-A, FU-A) and decoding via `VTDecompressionSession` into `VID_FMT_YUV420P`.
   - **Camera Capture `vidsrc` (`avcapture`)**: `AVCaptureSession` front/back camera capture on device with automatic 30 fps animated test-pattern fallback on Simulator, delivering `VID_FMT_YUV420P` frames to Baresip and rendering local preview to `SipVideoView`.
   - **Remote Video Display `vidisp` (`iosview`)**: Converts decoded remote `vidframe` buffers (`YUV420P` / `NV12` / `RGB32`) and renders them onto the bound remote `SipVideoView`.
3. **Expand the `SipClient` / `SipCall` Objective-C++ API**:
   - Outgoing audio or video calls (`makeCall:withVideo:`), incoming call answer with or without video (`answerWithVideo:`), microphone mute (`setMuted:`), speakerphone routing (`setSpeakerphone:` via `AVAudioSession`), hold/resume (`setHold:`), DTMF transmission (`sendDtmf:`), camera position switching (`switchCamera`), local video enable/disable (`setLocalVideoEnabled:`), CallKit UUID correlation (`uuid`), and video view binding (`remoteVideoView`, `localVideoView`).
4. **Build the full-fledged iOS SIP Video Phone UI & CallKit integration (`SipSdk/`)**:
   - **Main Phone Screen (`SipCallsViewController`)**: Registration status badge, editable Account Settings modal (`SipAccountStore` with `SipCredentials.plist` default fallback), URI dialer with quick-dial targets, separate **Video Call** and **Audio Call** buttons, incoming call alert with **Answer Video / Answer Audio / Decline**, active & recent call history list (`SipCallHistoryStore`), and collapsible live diagnostic log.
   - **In-Call Video Phone Screen (`SipCallDetailsViewController`)**: Full-screen remote video canvas (`SipVideoView`), draggable Picture-in-Picture (PiP) local camera preview (`SipVideoView`), live call duration timer, remote URI & call state header, and full control bar (Mute, Camera On/Off, Flip Camera, Speaker, Hold, DTMF 12-key keypad, Hangup).
   - **System CallKit Integration (`SipCallKitManager`)**: `CXProvider` + `CXCallController` reporting incoming/outgoing calls (with video flag), answering, ending, holding, muting, and playing DTMF with graceful fallback on Simulator.
5. **Upgrade Companion Web Test Page (`web/index.html`) for Two-Way Video**:
   - Add WebRTC video support (H.264 preferred) with local and remote `<video>` elements, video call toggle, and in-call camera mute/unmute while preserving all existing audio/signaling controls and Playwright tests.

Glossary: [`../../CONTEXT.md`](../../CONTEXT.md).

## 2. User Stories & Acceptance Criteria

- **US1 — Native H.264 Video Codec, Capture, and Display in `SipSdk`**
  - **Given** a started `SipClient` instance on Baresip v4.12.0,
  - **When** the engine initializes its media subsystems,
  - **Then** `H264` (`packetization-mode=1` and `0`) is registered in `baresip_vidcodecl()`, `avcapture` is registered in `baresip_vidsrcl()`, `iosview` is registered in `baresip_vidispl()`, and encoding a `640x480` `vidframe` through `VideoToolbox` into RFC 6184 RTP packets and decoding those packets back reconstructs a valid `640x480` `vidframe`.

- **US2 — Outgoing & Incoming SIP Video and Audio Calls with Full Telephony Controls**
  - **Given** two SIP endpoints (or loopback UA / SIP peer),
  - **When** a video call is placed via `makeCall:withVideo:YES` and answered via `answerWithVideo:YES`,
  - **Then** the SDP offer/answer negotiates `m=video` with `H264/90000`, `call.hasVideo` is `YES`, and the call supports microphone mute/unmute, speakerphone toggle, hold/resume, DTMF digit transmission, camera flip, and local video toggle.

- **US3 — Editable SIP Account Settings & Call History**
  - **Given** the iOS app launched with or without `SipCredentials.plist`,
  - **When** the user opens Account Settings, saves SIP credentials (username, domain, password, port, display name, transport), or completes a call,
  - **Then** `SipAccountStore` persists the non-secret account profile and re-initializes `SipClient`, while `SipCallHistoryStore` records the call direction, remote URI, video mode, timestamp, and duration.

- **US4 — Full-Fledged In-Call Video Phone UI & System CallKit Integration**
  - **Given** an active or incoming SIP video call in the iOS app,
  - **When** the call is presented on screen (and reported to `SipCallKitManager`),
  - **Then** `SipCallDetailsViewController` renders the remote video canvas, draggable PiP local preview, call timer, DTMF keypad, and control buttons (Mute, Video, Flip, Speaker, Hold, Keypad, Hangup), and `SipCallKitManager` synchronizes answer/end/hold/mute/DTMF actions with `SipCall`.

- **US5 — Companion Web Test Page (`web/index.html`) Two-Way Video Interop**
  - **Given** `web/index.html` loaded in a browser against a SIP-over-WebSocket server,
  - **When** the user places or answers a video call,
  - **Then** JsSIP negotiates audio+video SDP (`m=video`), attaches remote and local video streams to `#remoteVideo` and `#localVideo`, and supports toggling camera video (`#btnVideo`), audio mute (`#btnMute`), hold (`#btnHold`), DTMF, and hangup.

## 3. Architectural & Implementation Decisions

1. **Single Baresip v4 Engine Across SwiftPM and Xcode App**:
   - *Decision*: Point `SipSdk.xcodeproj` directly at `Sources/SipSdk/SipClientBaresipV4.mm`, `Sources/SipSdk/include/SipClient.h`, `Sources/SipSdkResources/Resources/config.baresip-v4`, and the resolved `BaresipCore.xcframework` instead of the legacy `taresip 0.1.1` CocoaPods archive.
   - *Tradeoff*: We accept retiring the legacy `taresip 0.1.1` linkage from `SipSdk.xcodeproj` in exchange for `arm64` simulator support, Baresip v4.12.0 stability, and a single shared native codebase.
2. **Apple `VideoToolbox` H.264 Codec Registered via `vidcodec_register`**:
   - *Decision*: Implement H.264 encoding/decoding using iOS hardware `VideoToolbox` (`VTCompressionSession` / `VTDecompressionSession`) paired with `libre`'s built-in `re_h264` RFC 6184 packetizer (`h264_nal_send`, `h264_fu_hdr_decode`, `h264_stap_decode_annexb`) rather than compiling software `libx264`/`FFmpeg` into `BaresipCore.xcframework`.
   - *Tradeoff*: We accept an Apple-platform dependency (`VideoToolbox.framework`) in exchange for zero third-party GPL/LGPL codec licensing issues, hardware acceleration, and keeping `BaresipCore.xcframework` small (~2 MB).

## 4. Alternatives Considered

- **Compile `ffmpeg`/`libx264` into `BaresipCore.xcframework`**: Rejected because it bloats the binary archive, introduces x264 GPL licensing constraints, and duplicates hardware H.264 acceleration already built into every iOS device and Simulator via `VideoToolbox`.
- **Keep `SipSdk.xcodeproj` on `taresip 0.1.1` CocoaPods**: Rejected because `taresip` lacks an `arm64` simulator slice (cannot run on Apple Silicon iOS Simulators) and uses an obsolete 2019 Baresip 0.x API.

## 5. Out of Scope (Deliberately Does NOT Do)

- SIP over TLS (`sips:`) — disabled in `BaresipCore` v4.12.0 (`USE_OPENSSL=OFF`).
- APNs VoIP Push notification server infrastructure (`PushKit` server-side wakeups).
- Multi-party video conferencing / video mixing (`vidmix`).

## 6. Open Questions

None — all architecture, feature scope, and web test page interop questions were resolved with the user on 2026-10-31.

## 7. Definition of Done & Next Step

- **Definition of Done**: See [`task.md`](./task.md) Done Criteria (`D1`–`D6`) and Existing Behavior Inventory (`I1`–`I9`).
- **Next Implementation Step**: Open and lock `specs/ios-video-phone/task.md`, then execute Slice 1 (Baresip v4 `VideoToolbox` H.264 codec, `avcapture` source, `iosview` display, and extended `SipClient`/`SipCall` APIs) test-first.
