# Plan: Video Media Driver & Resilient Streaming (`video-streaming`)

**Goal**: Extract the pluggable media driver interface (`IVideoCaptureDriver`, `IVideoRendererDriver`, `IVideoCodecDriver` and C ABI equivalents) into `sip_core`, encapsulate resilient video streaming state machines (mid-call re-INVITE `recvonly` RX preservation & RTCP PLI/FIR keyframe recovery on packet loss) in shared C++ code, connect the native iOS pipeline (`VideoToolbox` H.264, `AVCaptureSession`, `SipVideoView`), and achieve 100% zero-regression parity lock across the entire iOS test suite and app launch harness.

## Global Constraints

- **Language Standard**: C++17 for `sip_core`; pure C11 `extern "C"` interface for `sip_abi.h` with zero C++ name mangling, standard library types, or compiler-dependent ABIs.
- **Symbol Visibility**: `-fvisibility=hidden` on all core source files; only functions prefixed with `sip_*` are exported publicly with default visibility (`SIP_ABI_EXPORT`).
- **Network & Socket ABI Safety**: All CMake targets must configure `-DUSE_UNIXSOCK=ON` to match the layout of `struct sa` and `struct sip_msg` in `libre` and prevent silent registration hangs.
- **Threading Model**: Dedicated background worker thread invoking `re_main()`; all public C ABI calls from arbitrary caller threads must dispatch onto the worker thread via libre `mqueue` message queues without blocking the UI thread.
- **UI Thread Dispatch**: All callbacks (`on_call_state`, `on_incoming_call`, `on_call_media`, `on_dtmf_digit`, `on_reg_state`) must be delivered directly on the platform main UI thread (`dispatch_to_main` or GCD).
- **Audio Session Prerequisite**: Mobile platform bindings (`AVAudioSession` on iOS) must configure and activate voice audio sessions before Baresip starts microphone capture.
- **Preserved Compatibility**: Zero regressions against existing iOS test harnesses (`scripts/verify-ios-video-tests.sh`, `scripts/verify-sim-app-launch.sh`, `scripts/verify-spm-video-sdk.sh`, `scripts/verify-xcode-app-build.sh`).

---

## 1. Frame

### Problem
VoIP video stream management, hardware codec acceleration (`VideoToolbox` H.264), camera capture (`AVCaptureSession`), video display rendering (`SipVideoView`), and critical resilient streaming bug fixes (preserving the video RX pipeline on mid-call in-dialog re-INVITEs offering `recvonly`, and requesting RTCP PLI/FIR keyframes upon RTP packet loss) are currently tightly coupled to Objective-C++ in `Sources/SipSdk/SipClientBaresipV4.mm`.

This tight coupling prevents multi-platform reuse across Android, macOS, Windows, and Linux. Furthermore, `SipClientBaresipV4.mm` duplicates low-level Baresip lifecycle and SIP session code that has already been extracted into the portable C++17 core (`sip_core`) and universal C ABI (`sip_abi`).

### Solution
1. **Abstract Media Driver Architecture (`media_driver`)**:
   - Define abstract C++ interfaces and pure C ABI driver structs in `sip_core`:
     - `IVideoCaptureDriver` (`sip_video_capture_driver_t`): Hardware camera capture, camera switching, local preview, and raw YUV420P frame delivery to Baresip's `vidsrc`.
     - `IVideoRendererDriver` (`sip_video_renderer_driver_t`): Decoded remote video frame delivery to platform display surfaces (`SipVideoView`), aspect ratio resize events, and surface clearing.
     - `IVideoCodecDriver` (`sip_video_codec_driver_t`): Hardware codec registration bridge to Baresip's `vidcodecl`.
   - Implement built-in synthetic/mock drivers in `sip_core` for headless testing on any OS.
2. **Resilient Video Streaming in C++ Core**:
   - **re-INVITE `recvonly` RX Stream Preservation**: When remote peers or PBX systems send mid-call in-dialog re-INVITEs offering `a=recvonly`, `sip_core::Engine` intercepts `BEVENT_CALL_REMOTE_SDP` and `BEVENT_CALL_LOCAL_SDP`, re-asserts `rdir = SDP_SENDRECV`, re-enables stream RX, reactivates `video_start_display`, and triggers `video_req_keyframe` so remote video display never freezes.
   - **RTCP PLI/FIR Packet Loss Recovery**: Detect packet loss or missing NALUs during RTP depacketization; return `EPROTO` to prompt Baresip to transmit an RTCP Picture Loss Indication (PLI) or Full Intra Request (FIR) to the remote encoder, rate-limited to at most one request per 500 ms (`last_picup_ms`). Export explicit C ABI function `sip_call_request_keyframe(call)`.
3. **Turnkey iOS Platform Binding & Parity Lock**:
   - Implement the `media_driver` interfaces using Apple `VideoToolbox` H.264 hardware encoding/decoding, `AVCaptureSession` / simulator synthetic camera feed, and `SipVideoView` rendering.
   - Unify `SipClient` and `SipCall` over `sip_abi`, delegating account registration, lifecycle, calls, and telephony controls to `sip_core` while preserving 100% API compatibility with `Sources/SipSdk/include/SipClient.h`.
   - Verify zero regressions against `scripts/verify-ios-video-tests.sh`, `scripts/verify-sim-app-launch.sh`, `scripts/verify-spm-video-sdk.sh`, and `scripts/verify-xcode-app-build.sh`.

### User Stories
- **US11 (Outgoing Video Call)**: As a VoIP user, I want to initiate an outgoing video call with negotiated H.264 video and G.711 audio, so that I can see and hear the remote peer simultaneously.
- **US17 (Incoming Video Call)**: As a VoIP user, I want to answer an incoming video call with two-way video enabled, so that both parties share live camera feeds immediately.
- **US27 (Hardware Video Acceleration)**: As a mobile VoIP user, I want video encoding and decoding to use native hardware acceleration (`VideoToolbox` on Apple), so that video calls do not overheat my phone or drain the battery.
- **US28 (re-INVITE `recvonly` Stream Preservation)**: As a video caller, I want remote video to continue rendering smoothly even when the remote endpoint or PBX sends an in-dialog re-INVITE offering `recvonly`, so that the video stream does not freeze or black out mid-call.
- **US29 (RTP Packet Loss Keyframe Recovery)**: As a video caller, I want the SDK to automatically detect RTP packet loss and trigger RTCP PLI/FIR keyframe requests, so that visual corruption is quickly cleared by the remote encoder upon the next IDR frame.
- **US30 (Turnkey Platform Video Views)**: As an application developer, I want to drop a turnkey video widget (`SipVideoView` on Apple) into my UI layout, so that local camera preview and remote video render automatically with proper aspect ratios.
- **US31 (Camera Switching)**: As a mobile video caller, I want to toggle between the front and rear cameras during an active call, so that I can show my surroundings without dropping the video feed.
- **US32 (Dynamic Video Mute / Toggle)**: As a video caller, I want to turn my camera feed on or off dynamically during an ongoing call, so that I can temporarily disable my video without ending the call.
- **US37 (Main UI Thread Guarantee)**: As an SDK consumer, I want all event delegate callbacks to be dispatched directly on the platform's main UI thread, so that I never encounter multithreaded UI state corruption crashes.

### Out of Scope
1. Android JNI bindings and MediaCodec driver (reserved for Milestone 2, Issues #15 & #16).
2. Desktop Windows / Linux media drivers (reserved for Milestone 3, Issues #17 & #18).
3. Web (WASM / WebRTC) media integration in Phase 1 (deferred to Phase 2).
4. SIP over TLS (`USE_OPENSSL=ON`) or SRTP (deferred to security milestone).
5. Third-party software codecs (Opus, VP8, VP9); relies on built-in G.711 and hardware H.264.

### Implementation Decisions
- **`core/include/media_driver.h`**: Portable C++ header declaring `IVideoCaptureDriver`, `IVideoRendererDriver`, `IVideoCodecDriver`, and `VideoFrame`.
- **`core/src/media_driver.h` & `core/src/media_driver.cpp`**: Bridge between Baresip's C plugin structures (`vidsrc_st`, `vidisp_st`, `vidcodec`) and the C++ driver interfaces. Includes synthetic/mock test patterns for automated test execution.
- **`core/include/sip_abi.h`**: Stable C declarations for `sip_video_frame_t`, `sip_video_capture_driver_t`, `sip_video_renderer_driver_t`, `sip_video_codec_driver_t`, `sip_client_set_video_capture_driver`, `sip_client_set_remote_renderer`, `sip_client_set_local_renderer`, `sip_client_set_video_codec_driver`, `sip_call_request_keyframe`, `sip_client_switch_camera`, `sip_client_set_camera`, `sip_client_set_local_video_enabled`, `sip_client_is_local_video_enabled`, `sip_client_start_local_preview`, `sip_client_stop_local_preview`, `sip_client_is_local_preview_active`.
- **`core/src/engine.h` & `core/src/engine.cpp`**: Implement media driver lifecycle in `sip_core::Engine`. Intercept `BEVENT_CALL_REMOTE_SDP` and `BEVENT_CALL_LOCAL_SDP` to execute `preserve_video_rx_and_display(call)`. Handle video call answer and SDP negotiation (`VIDMODE_ON`).
- **`core/src/call_session.h` & `core/src/call_session.cpp`**: Add `request_keyframe()` dispatching `video_req_keyframe` onto Baresip's `struct video*`.
- **`Sources/SipSdk/SipClientBaresipV4.mm`**: Implement native iOS drivers (`VideoToolbox` H.264, `AVCaptureSession`, `SipVideoView`), bind them to `sip_core` via `sip_abi`, and migrate `SipClient` and `SipCall` to use `sip_abi` for calls, registration, and media.
- **`core/tests/test_video_streaming.cpp`**: Automated C++ test verifying driver registration, video SDP negotiation with Asterisk echo extension `9001`, mid-call re-INVITE `recvonly` handling, and packet loss keyframe generation.
- **`scripts/verify-video-streaming.sh`**: Verification script compiling and running `sip_core_video_streaming_test` on booted iOS Simulator.

### Testing Decisions
- **Automated C++ Video Test Runner (`core/tests/test_video_streaming.cpp`)**: Validates the core media driver abstraction, Asterisk video echo call, mid-call re-INVITE recovery, and simulated packet loss recovery.
- **Symbol Visibility Check**: Audits `libsipcore.a` with `nm -gU` to ensure only `_sip_*` symbols are publicly exported.
- **iOS Parity Regression Suite (`scripts/verify-ios-video-tests.sh`)**: Runs the full suite of loopback registrar, audio call, video call, re-INVITE call, VideoToolbox H.264 round-trip, packet loss recovery, SipVideoView, CallKit, SipAccountStore, SipCallHistoryStore.
- **Simulator App Launch (`scripts/verify-sim-app-launch.sh`)**: Boots `SipSdk.app` in Dialer, Account Settings, and In-Call Video Phone modes on iOS Simulator, verifying UI rendering and saving screenshot evidence.

---

## 2. Seam

The test seams attach at:
1. **Universal C ABI Boundary (`core/include/sip_abi.h` and static library `libsipcore.a`)**: Exercised directly by `core/tests/test_video_streaming.cpp` on iOS Simulator / macOS host.
2. **Platform Seam (`Sources/SipSdk/include/SipClient.h`)**: Exercised by `Tests/SipSdkRunner/main.mm` and `SipSdk.app` UI automation on iOS Simulator.

---

## 3. Milestones Roadmap

- [x] **Milestone 1 — C++ Core Extraction & iOS Parity Lock**
  - [x] **Slice 1 (Issue #11)**: Core Engine Lifecycle, C ABI Scaffold & Automated Lifecycle Verification
  - [x] **Slice 2 (Issue #12)**: SIP Account Registration, MD5 Authentication & Presence State Machine
  - [x] **Slice 3 (Issue #13)**: Audio Calling Session, G.711 Negotiation & Telephony Controls
  - [x] **Slice 4 (Issue #14)**: Video Media Driver, Resilient Stream Recovery & iOS Parity Lock
- [ ] **Milestone 2 — Android Platform SDK (AAR + MediaCodec)** (Issues #15, #16)
- [ ] **Milestone 3 — Desktop Platform SDKs (macOS, Linux, Windows)** (Issues #17, #18)

---

## 4. Vertical Slices (Issue #14 Breakdown)

### Slice 1 — Core Pluggable Media Driver Abstraction & C ABI (`video-media-driver-core`)

- **User stories covered**: US11, US17, US30, US37
- **Device tier**: iOS Simulator (`arm64`) & macOS host compilation tier
- **Files**:
  - `core/include/media_driver.h`
  - `core/include/sip_abi.h`
  - `core/src/media_driver.h`
  - `core/src/media_driver.cpp`
  - `core/src/engine.h`
  - `core/src/engine.cpp`
  - `core/src/sip_abi.cpp`
  - `core/CMakeLists.txt`
  - `core/tests/test_video_streaming.cpp`
  - `scripts/verify-video-streaming.sh`
- **Interfaces**:
  - `int sip_client_set_video_capture_driver(sip_client_t *client, const sip_video_capture_driver_t *driver);`
  - `int sip_client_set_remote_renderer(sip_client_t *client, const sip_video_renderer_driver_t *renderer);`
  - `int sip_client_set_local_renderer(sip_client_t *client, const sip_video_renderer_driver_t *renderer);`
  - `int sip_client_set_video_codec_driver(sip_client_t *client, const sip_video_codec_driver_t *driver);`
  - `sip_core::IVideoCaptureDriver`, `sip_core::IVideoRendererDriver`, `sip_core::IVideoCodecDriver`
- **Acceptance criteria**:
  1. **Given** an initialized `sip_client_t` instance, **when** registering custom capture, renderer, and codec drivers before or after initialization, **then** the drivers are attached to `sip_core::Engine` and Baresip's `vidsrc`, `vidisp`, and `vidcodecl` modules are registered without error.
  2. **Given** an outgoing video call initiated with `sip_call_make(client, uri, true)`, **when** SDP negotiation completes with Asterisk PBX video echo extension `9001` or loopback peer, **then** `m=video` is negotiated with H.264 payload, and the capture driver's `start` hook is invoked.
  3. **Given** incoming decoded video frames delivered by Baresip's video stream, **when** received by the core display module, **then** the registered `IVideoRendererDriver::render_frame` callback is invoked with valid frame dimensions and non-null plane buffers.
  4. **Given** the automated video streaming test script, **when** `bash scripts/verify-video-streaming.sh` is executed on iOS Simulator, **then** driver registration, media negotiation, and frame delivery pass with exit code 0.
- **Verify**: `bash scripts/verify-video-streaming.sh`
- **Blocked-by**: None

---

### Slice 2 — Resilient Video Stream Recovery in C++ Core (`video-resilient-recovery`)

- **User stories covered**: US28, US29, US31, US32
- **Device tier**: iOS Simulator (`arm64`) & macOS host compilation tier
- **Files**:
  - `core/include/sip_abi.h`
  - `core/src/engine.h`
  - `core/src/engine.cpp`
  - `core/src/call_session.h`
  - `core/src/call_session.cpp`
  - `core/tests/test_video_streaming.cpp`
  - `scripts/verify-video-streaming.sh`
- **Interfaces**:
  - `SIP_ABI_EXPORT int sip_call_request_keyframe(sip_call_t *call);`
  - `SIP_ABI_EXPORT int sip_client_switch_camera(sip_client_t *client);`
  - `SIP_ABI_EXPORT int sip_client_set_camera(sip_client_t *client, int position);`
  - `SIP_ABI_EXPORT int sip_client_set_local_video_enabled(sip_client_t *client, bool enabled);`
  - `SIP_ABI_EXPORT bool sip_client_is_local_video_enabled(const sip_client_t *client);`
  - `SIP_ABI_EXPORT int sip_client_start_local_preview(sip_client_t *client);`
  - `SIP_ABI_EXPORT int sip_client_stop_local_preview(sip_client_t *client);`
  - `SIP_ABI_EXPORT bool sip_client_is_local_preview_active(const sip_client_t *client);`
- **Acceptance criteria**:
  1. **Given** an established two-way video call receiving a mid-call in-dialog re-INVITE offering `a=recvonly`, **when** `BEVENT_CALL_REMOTE_SDP` or `BEVENT_CALL_LOCAL_SDP` is intercepted by `sip_core::Engine`, **then** `im->rdir` is restored to `SDP_SENDRECV`, stream RX remains enabled, `video_start_display` is re-asserted, and remote video frames continue rendering with zero freeze.
  2. **Given** simulated RTP packet loss in incoming video packets, **when** an incomplete Access Unit is encountered during depacketization, **then** `EPROTO` is returned to Baresip's video stream, triggering an immediate RTCP PLI/FIR keyframe request rate-limited to at most one per 500 ms.
  3. **Given** an application invoking `sip_call_request_keyframe(call)`, **when** called on an active call session, **then** a keyframe request is dispatched directly onto Baresip's video stream on the worker thread.
  4. **Given** camera toggles (`sip_client_switch_camera`, `sip_client_set_local_video_enabled`, `sip_client_start_local_preview`), **when** invoked from any caller thread, **then** the active capture driver updates camera state and routes local preview frames without call interruption.
- **Verify**: `bash scripts/verify-video-streaming.sh`
- **Blocked-by**: Slice 1

---

### Slice 3 — Native iOS Video Media Drivers (`ios-media-driver-adapter`)

- **User stories covered**: US27, US30
- **Device tier**: iOS Simulator (`arm64`) & generic iOS Device (`arm64`)
- **Files**:
  - `Sources/SipSdk/include/sip_abi.h`
  - `Sources/SipSdk/SipClientBaresipV4.mm`
  - `Sources/SipSdk/include/SipClient.h`
  - `SipSdk/SipClient.h`
- **Interfaces**:
  - `IosVideoCaptureDriver`: Bridges `AVCaptureSession` (device) and 30 fps synthetic generator (simulator) to `sip_video_capture_driver_t`
  - `IosVideoRendererDriver`: Bridges decoded `sip_video_frame_t` buffers to `SipVideoView` CoreAnimation rendering
  - `IosVideoCodecDriver`: Bridges Apple `VideoToolbox` H.264 encoder/decoder to `sip_video_codec_driver_t`
- **Acceptance criteria**:
  1. **Given** `SipClient` on iOS Simulator or Device, **when** initialized, **then** `VideoToolbox` H.264 codec driver, `AVCaptureSession` capture driver, and `SipVideoView` renderer driver are registered with `sip_core` via `sip_client_set_*_driver`.
  2. **Given** a decoded remote video frame delivered by the core engine, **when** forwarded to `IosVideoRendererDriver`, **then** `SipVideoView` renders the YUV420P frame and updates `videoSize` on the main UI thread.
  3. **Given** `VideoToolbox` H.264 compression session, **when** encoding test frames with width 640 and height 480, **then** valid NALUs are generated and round-trip decoded cleanly through the decompression session.
- **Verify**: `bash scripts/verify-ios-video-tests.sh`
- **Blocked-by**: Slice 2

---

### Slice 4 — iOS Binding Unification, Contract Refactoring & Parity Lock (`ios-parity-lock`)

- **User stories covered**: US11, US17, US27, US28, US29, US30, US31, US32, US37
- **Device tier**: iOS Simulator (`arm64`) & iOS Device (`arm64`)
- **Files**:
  - `Sources/SipSdk/SipClientBaresipV4.mm`
  - `Tests/SipSdkRunner/main.mm`
  - `scripts/verify-ios-video-tests.sh`
  - `scripts/verify-sim-app-launch.sh`
  - `scripts/verify-spm-video-sdk.sh`
  - `scripts/verify-xcode-app-build.sh`
- **Interfaces**:
  - `SipClient` delegating all operations (`start`, `stop`, `registry`, `unregister`, `makeCall:withVideo:`, `setMuted:`, `switchCamera`, `startLocalVideoPreview`, `stopLocalVideoPreview`) to `sip_client_*` and `sip_call_*`
  - `SipCall` delegating `hangup:reason:`, `sendDtmf:`, `setHold:`, `setMuted:`, `requestKeyframe` to `sip_call_*`
- **Acceptance criteria**:
  1. **Given** the automated iOS Simulator test suite (`scripts/verify-ios-video-tests.sh`), **when** executed against the unified `sip_core` / `sip_abi` bridge, **then** 100% of test cases pass (init, `SipVideoView` rendering, registration, audio loopback, video loopback, re-INVITE recvonly, H.264 round-trip, packet loss recovery, CallKit, `SipAccountStore`, `SipCallHistoryStore`).
  2. **Given** `scripts/verify-sim-app-launch.sh`, **when** launched on booted iOS Simulator, **then** Dialer, Account Settings modal, and In-Call Video Phone screens launch and render successfully with evidence screenshots saved.
  3. **Given** `scripts/verify-spm-video-sdk.sh` and `scripts/verify-xcode-app-build.sh`, **when** built for iOS Simulator and generic iOS device, **then** compilation and linking succeed with zero errors.
- **Verify**: `bash scripts/verify-ios-video-tests.sh && bash scripts/verify-sim-app-launch.sh && bash scripts/verify-spm-video-sdk.sh && bash scripts/verify-xcode-app-build.sh`
- **Blocked-by**: Slice 3

---

## 5. Expand → Migrate → Contract Refactoring Strategy

1. **Expand (Slices 1 & 2)**:
   - Introduce abstract C++ `media_driver.h` and C ABI `sip_abi.h` multimedia driver declarations.
   - Implement `media_driver.cpp` in `sip_core` with mock/synthetic drivers and encapsulate resilient re-INVITE `recvonly` recovery and RTCP PLI packet loss recovery in `sip_core::Engine`.
   - Validate `sip_core` video streaming in isolation via `scripts/verify-video-streaming.sh`.
   - The existing iOS codebase (`Sources/SipSdk/SipClientBaresipV4.mm`) remains untouched and fully functional during this phase.

2. **Migrate (Slice 3)**:
   - Implement the `media_driver` interfaces in `Sources/SipSdk/SipClientBaresipV4.mm` by connecting `VideoToolbox` H.264, `AVCaptureSession`, and `SipVideoView`.
   - Synchronize `Sources/SipSdk/include/sip_abi.h` with `core/include/sip_abi.h`.
   - Link `sip_core` sources or static library into `SipSdk` builds.

3. **Contract (Slice 4)**:
   - Remove redundant monolithic Baresip lifecycle and SIP session code from `SipClientBaresipV4.mm`, turning it into a clean Objective-C++ adapter over `sip_abi`.
   - Execute all verification harnesses (`verify-ios-video-tests.sh`, `verify-sim-app-launch.sh`, `verify-spm-video-sdk.sh`, `verify-xcode-app-build.sh`) to guarantee zero regressions.

---

## 6. Review & Refutation Checklist

- [x] **Requirement Traceability**: Every acceptance criterion from GitHub Issue #14 is mapped directly to a vertical slice.
- [x] **No Placeholders**: Exact file paths, function signatures, Given/When/Then acceptance criteria, and verification commands are fully specified.
- [x] **API & Struct Alignment**: `sip_video_frame_t`, `sip_video_capture_driver_t`, `sip_video_renderer_driver_t`, `sip_video_codec_driver_t`, and `sip_call_request_keyframe` maintain consistent signatures across C ABI and C++ headers.
- [x] **ABI Safety**: `-DUSE_UNIXSOCK=ON` is strictly enforced to preserve libre `struct sa` and `struct sip_msg` layout.
- [x] **Threading & Concurrency**: All C ABI calls marshal through libre's `mqueue` to the dedicated worker thread; all UI callbacks are marshaled to the platform main thread.
- [x] **Hardware Audio Session**: `prepare_audio_session` hook remains active before media initialization.
- [x] **Zero Regressions**: Parity lock enforced by running all existing iOS test and app launch scripts.
