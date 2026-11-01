# PRD: Unified Cross-Platform SIP Video SDK (`cross-platform-sdk`)

## Problem Statement

Application developers building real-time communication tools across iOS, Android, macOS, Windows, and Linux face immense complexity when integrating native SIP telephony and hardware-accelerated video. 

Currently, VoIP engine logic and critical protocol workarounds—such as recovering from remote video freezes during mid-call SDP re-INVITEs, requesting RTCP PLI/FIR keyframes on packet loss, managing hardware codec sessions, synchronizing background event loops with the UI thread, and executing clean engine teardown—are tightly coupled to platform-specific code (such as Objective-C++ on iOS). 

Because these bug fixes and session lifecycles are embedded in platform-specific code, developers must re-invent and re-debug the same complex VoIP state machines for every target operating system. Existing raw C SIP libraries (such as Baresip and libre) provide building blocks, but expose low-level pointer arithmetic, fragile global threading loops, and steep integration hurdles that leak complexity into the consuming applications.

Developers need a single, portable, cross-platform SIP SDK that hides the underlying engine complexity behind a simple, high-level interface (Init/Shutdown, Register/Unregister, Make Call, Answer/Hangup/Hold/Resume, DTMF, and typed UI-thread events), while providing plug-and-play, hardware-accelerated video views.

---

## Solution

Extract a unified, portable C++17 core engine (`sip_core`) with a flat C ABI (`sip_abi`) that encapsulates Baresip v4 and libre, isolates all SIP protocol quirks and video stream recovery mechanisms in one shared layer, and provides clean language bindings for each native ecosystem:

1. **Encapsulated Engine Core**: A single shared C++17 engine running on a dedicated worker loop thread with a thread-safe message queue, managing account registration, multi-call states, re-INVITE stream preservation, packet loss recovery, and clean stack shutdown.
2. **Universal C ABI Boundary**: A clean `extern "C"` interface exporting opaque handles (`sip_client_t`, `sip_call_t`) and typed callback dispatchers, allowing Swift, Kotlin/JNI, C#, and C++ to bind with zero overhead.
3. **Pluggable Media Driver Architecture**: Abstract driver interfaces for video capture, video rendering, and codecs, allowing platforms to supply native hardware acceleration (`VideoToolbox` on Apple, `MediaCodec` on Android, `Media Foundation` on Windows, `V4L2` on Linux).
4. **Turnkey Platform Video Views**: Pre-built native UI components (`SipVideoView` for Apple, `SipSurfaceView` for Android, `SipVideoPanel` for Windows/Linux) that handle camera capture, hardware decoding, and aspect-ratio rendering automatically without exposing raw frame buffers to app developers.
5. **Mobile-First Phased Delivery**: Milestone 1 extracts the C++ core and verifies 100% zero-regression parity against the existing iOS test suite and Asterisk PBX; Milestone 2 delivers the Android SDK (`.aar` + `MediaCodec`); Milestone 3 delivers desktop targets (macOS, Linux, Windows).

---

## User Stories

### SDK Initialization & Lifecycle
1. As an application developer, I want to initialize the SIP SDK with an optional configuration path via a single function call, so that the underlying networking and VoIP engines start up reliably without manual thread setup.
2. As an application developer, I want all internal network and media polling to execute on a dedicated background thread, so that the host application's UI thread is never blocked or frozen.
3. As an application developer, I want to shut down the SIP SDK cleanly via a single termination call, so that all active media streams, sockets, and threads are terminated gracefully without crashes or memory leaks.
4. As an application developer, I want API calls to be thread-safe from any caller thread, so that I do not have to write thread synchronization guards when invoking SDK methods.

### Account Registration & Presence
5. As a VoIP user, I want to register my SIP account with username, domain, password, transport (UDP/TCP), and port, so that I can send and receive calls through my SIP registrar.
6. As a VoIP user, I want the SDK to handle digest authentication and nonce challenges automatically, so that I do not need to parse or compute HTTP/SIP authentication headers manually.
7. As an application developer, I want to receive typed registration state events (`Unregistered`, `Registering`, `Registered`, `Failed`) with status codes and reason strings, so that I can update the app's connection badge in the UI.
8. As a VoIP user, I want to unregister my account on demand, so that the SIP proxy stops routing incoming calls to my device when I go offline.
9. As a mobile app developer, I want the SDK to handle automatic re-registration refreshes before registration expiry, so that my account remains reachable while the application is active.

### Outgoing Call Management
10. As a VoIP user, I want to initiate an outgoing audio-only call by specifying a remote SIP URI, so that I can speak with remote parties without opening my camera.
11. As a VoIP user, I want to initiate an outgoing video call by specifying a remote SIP URI, so that I can conduct a two-way video conversation with the remote party.
12. As an application developer, I want outgoing call initiation to return a dedicated call session object, so that I can track and control the specific call instance independently.
13. As an application developer, I want to receive `Connecting`, `Ringing`, `Established`, and `Closed` state transitions on the UI thread, so that I can update call progress indicators and play ringback tones.
14. As a VoIP user, I want to cancel an outgoing call before the remote peer answers, so that I can abort accidental dials immediately.

### Incoming Call Handling
15. As a VoIP user, I want to receive an incoming call notification when a remote party calls my SIP identity, so that I can choose to accept or decline the call.
16. As an application developer, I want incoming call notifications to deliver a populated call session containing the caller's URI, display name, and video capability flag, so that I can display a rich incoming call screen.
17. As a VoIP user, I want to answer an incoming call with video enabled, so that both audio and video streams are established immediately.
18. As a VoIP user, I want to answer an incoming video call in audio-only mode, so that I can speak privately without transmitting video.
19. As a VoIP user, I want to decline or reject an incoming call with standard SIP status codes (e.g. 486 Busy Here or 603 Decline), so that the caller is promptly informed that I cannot answer.

### In-Call Controls & Telephony
20. As a VoIP user, I want to hang up an active call at any time, so that the SIP session and media channels are immediately torn down.
21. As a VoIP user, I want to place an active call on hold, so that the remote party hears hold music or silence and two-way media is paused.
22. As a VoIP user, I want to resume a held call, so that two-way audio and video resume smoothly without renegotiation artifacts.
23. As a VoIP user, I want to mute and unmute my microphone during a call, so that I can prevent ambient room noise from reaching the remote peer.
24. As a VoIP user, I want to send DTMF tones (digits 0-9, *, #) via RFC 4733 / SIP INFO during an active call, so that I can navigate interactive voice response (IVR) phone menus.
25. As an application developer, I want to query the live call duration in seconds, so that I can display an accurate in-call timer in the user interface.
26. As an application developer, I want to handle multiple simultaneous calls (call waiting / hold one and answer another) without session collisions, so that my app supports enterprise telephony workflows.

### Video Pipeline, Codec & Stream Recovery
27. As a mobile VoIP user, I want video encoding and decoding to use native hardware acceleration (`VideoToolbox` on Apple, `MediaCodec` on Android), so that video calls do not overheat my phone or drain the battery.
28. As a video caller, I want remote video to continue rendering smoothly even when the remote endpoint sends an in-dialog re-INVITE offering `recvonly`, so that the video stream does not freeze or black out mid-call.
29. As a video caller, I want the SDK to automatically detect RTP packet loss and trigger RTCP PLI/FIR keyframe requests, so that visual corruption is quickly cleared by the remote encoder.
30. As an application developer, I want to drop a turnkey video widget (`SipVideoView` on Apple, `SipSurfaceView` on Android) into my UI layout, so that local camera preview and remote video render automatically with proper aspect ratios.
31. As a mobile video caller, I want to toggle between the front and rear cameras during an active call, so that I can show my surroundings without dropping the video feed.
32. As a video caller, I want to turn my camera feed on or off dynamically during an ongoing call, so that I can temporarily disable my video without ending the call.

### Multi-Platform & Developer Experience
33. As an iOS/macOS developer, I want to consume the SDK as an idiomatic Swift package with Swift classes and protocols, so that I do not have to interact with C pointers or headers.
34. As an Android developer, I want to consume the SDK as an Android Archive (`.aar`) with Kotlin classes and listeners, so that it integrates seamlessly into modern Android Gradle builds.
35. As a Windows developer, I want to consume the SDK as a .NET / C# library with standard events, so that I can build desktop telephony applications with WPF or WinUI.
36. As a Linux developer, I want to build and link against standard shared/static libraries via CMake, so that I can embed SIP calling into embedded and desktop Linux systems.
37. As an SDK consumer, I want all event delegate callbacks to be dispatched directly on the platform's main UI thread, so that I never encounter multithreaded UI state corruption crashes.

---

## Implementation Decisions

### 1. Architecture: Portable C++17 Core with Flat C ABI Boundary
- The core engine (`core/`) is written in standard C++17, encapsulating Baresip v4.12.0 and `libre` v4.12.0.
- All internal Baresip polling runs on a dedicated background thread invoking `re_main()`. A thread-safe message queue (`libre`'s `mqueue`) marshals public API calls from arbitrary caller threads onto the event loop thread.
- The core exposes a clean `extern "C"` ABI boundary (`sip_abi.h`) using opaque pointer handles (`sip_client_t*`, `sip_call_t*`) and function pointer callbacks. No C++ types, standard library templates, or compiler-specific mangling are exposed across the boundary.

### 2. Client & Call Session Object Model
- **`SipClient`**: Owns engine initialization, global configuration, network transports, account credentials, and outgoing call creation.
- **`SipCall`**: Represents an individual call session, owning session-specific state, direction, duration, video mode, hold state, and DTMF dispatch.
- **`SipClientListener`**: A typed listener/delegate interface delivering events marshaled to the platform UI thread:
  - `onRegistrationStateChanged(state, statusCode, reason)`
  - `onIncomingCall(call)`
  - `onCallStateChanged(call, state, statusCode, reason)`
  - `onCallMediaChanged(call, width, height, isOnHold, isMuted)`
  - `onDtmfReceived(call, digit)`

### 3. Pluggable Media Driver Architecture (`media_driver`)
- Video and audio hardware layers are abstracted behind C++ interface drivers:
  - `IVideoCaptureDriver`: Manages camera hardware capture and delivers raw `VID_FMT_YUV420P` frames to Baresip's `vidsrc`.
  - `IVideoRendererDriver`: Receives decoded remote `vidframe` buffers and renders them to the display surface.
  - `IVideoCodecDriver`: Hardware-accelerated H.264 encoder/decoder driver registered in Baresip's `vidcodecl`.
- Platform packages supply native implementations:
  - **Apple (iOS / macOS)**: `VideoToolbox` H.264 codec, `AVCaptureSession` capture, and `SipVideoView` CoreAnimation/Metal renderer.
  - **Android**: `MediaCodec` H.264 codec, Camera2 capture, and `SipSurfaceView` OpenGL/Surface renderer.
  - **Windows / Linux**: `Media Foundation` / `V4L2` / `DirectX` / `OpenGL` drivers.

### 4. Protocol Baseline & Recovery Mechanics
- **Signaling**: SIP over UDP and TCP with full RFC 3261 compliance.
- **NAT Traversal**: STUN, TURN, and ICE enabled by default via `libre`.
- **Audio Codec**: G.711 (PCMA and PCMU) included out-of-the-box via built-in `g711` module.
- **Video Codec**: H.264 (RFC 6184 Single NAL and FU-A packetization, Constrained Baseline Profile `42e01f`).
- **Stream Preservation**: Mid-call re-INVITEs offering `recvonly` or modified SDP keep the video RX pipeline enabled and re-assert the display driver.
- **Keyframe Recovery**: Decoder packet loss detection triggers RTCP PLI/FIR keyframe requests automatically to resolve frozen frames.

### 5. Repository Topology & Phased Delivery Sequence
- **Monorepo Structure**:
  - `core/`: C++17 shared engine, C ABI headers, and top-level `CMakeLists.txt`.
  - `platforms/ios/`: SwiftPM package, Objective-C++ thin wrapper, and iOS Video Phone application.
  - `platforms/android/`: Android Gradle project, Kotlin JNI wrapper, and demo application.
  - `platforms/desktop/`: CMake desktop targets (macOS, Linux, Windows C# bindings).
  - `scripts/`: Shared multi-platform verification and PBX test scripts.
- **Rollout Sequence**:
  - **Milestone 1**: C++ Core extraction and iOS zero-regression verification against existing test suites.
  - **Milestone 2**: Android SDK (`.aar` + `MediaCodec` + Camera2 + `SipSurfaceView`).
  - **Milestone 3**: Desktop platform SDKs (macOS, Linux, Windows).
  - **Phase 2**: Web target (WASM / WebRTC).

---

## Testing Decisions

### What Makes a Good Test
Tests must verify observable external behavior and contracts rather than internal private variables:
- Verify that calling `init` transitions the engine to ready without throwing or deadlocking.
- Verify that `register` with valid SIP credentials against Asterisk PBX yields `SIP_REG_STATE_REGISTERED`.
- Verify that placing a video call establishes bidirectional media with negotiated `m=video H264` SDP.
- Verify that sending an in-dialog re-INVITE (`recvonly`) does not cause video display termination.
- Verify that simulating RTP packet loss triggers an RTCP PLI keyframe request and recovers decoding without crash.
- Verify that hanging up releases all media sessions and returns the client to an idle state.

### Modules to Test
1. **`sip_core::Engine` & Lifecycle**: Verified by headless C++ test runner testing initialization, restart, and shutdown loops.
2. **`sip_core::AccountManager`**: Verified by automated registration against Asterisk Docker PBX (`scripts/verify-pbx.sh`).
3. **`sip_core::CallSession`**: Verified by automated loopback call tests covering audio and H.264 video.
4. **`media_driver` & Recovery**: Verified by H.264 encode/decode round-trip tests and packet loss recovery tests.
5. **Platform Bindings (iOS Swift / Android Kotlin)**: Verified by automated simulator test runners (`scripts/verify-ios-video-tests.sh` and Android instrumented tests).

### Prior Art in Codebase
- [`Tests/SipSdkRunner/main.mm`](../../Tests/SipSdkRunner/main.mm): Existing automated test runner compiling and testing Baresip v4 initialization, account registration, audio loopback, H.264 video loopback, re-INVITE handling, and packet loss recovery.
- [`scripts/verify-ios-video-tests.sh`](../../scripts/verify-ios-video-tests.sh): Automated shell harness spawning the iOS Simulator test runner and checking exit codes.
- [`scripts/verify-pbx.sh`](../../scripts/verify-pbx.sh): Health and SIP signaling test script validating registration and video echo calls against Asterisk 22 PBX.

---

## Out of Scope

1. **Web (WASM / WebRTC) in Phase 1**: Browser sandbox networking requires WebRTC and WebSocket signaling. Explicitly deferred to Phase 2.
2. **SIP over TLS (SIPS) and SRTP in Phase 1**: TLS (`USE_OPENSSL=ON`) requires cross-compiling OpenSSL or MbedTLS across all 5 OSes. Phase 1 focuses on standard UDP/TCP signaling.
3. **Third-Party Software Codecs (Opus, VP8, VP9, AV1)**: Phase 1 relies strictly on standard G.711 audio and hardware-accelerated H.264.
4. **Server-Side Media Proxy / PBX Development**: The SDK is purely client-side; testing uses the existing Asterisk 22 container.
5. **Cross-Platform UI Framework Bindings (Flutter / React Native)**: Framework wrappers are out of scope until native platform SDKs are validated.

---

## Further Notes

- **ABI Safety**: To maintain struct size stability across libre builds, all CMake targets must preserve `-DUSE_UNIXSOCK=ON` to match the layout of `struct sa` and `struct sip_msg`.
- **Audio Session Prerequisite**: On mobile operating systems (iOS and Android), the audio hardware session (e.g. `AVAudioSession` VoiceChat mode) must be activated prior to starting audio media capture to avoid premature call termination.
