# ADR 0001: Portable C++17 Core with Flat C ABI Boundary for Cross-Platform SIP SDK

## Context
The current SIP Video Phone implementation in [`Sources/SipSdk/SipClientBaresipV4.mm`](../../Sources/SipSdk/SipClientBaresipV4.mm) couples Baresip v4 C APIs, SIP call lifecycle logic, video re-INVITE handling, and RTCP keyframe triggers directly to Apple-specific Objective-C++ and iOS frameworks (`AVFoundation`, `VideoToolbox`, `UIKit`). To deliver a unified SIP SDK across iOS, Android, macOS, Windows, and Linux, the core engine must be decoupled from Apple-specific runtime bindings while avoiding duplicate implementations of SIP state machines and bug fixes per operating system.

## Decision
We extract a shared C++17 engine (`sip_core`) with a flat `extern "C"` ABI boundary (`sip_abi`):
1. **Core Language**: C++17 encapsulating Baresip v4 and `libre`, managing user-agent initialization, call state machine, re-INVITE recovery, keyframe requests, and clean teardown.
2. **Export Boundary**: A stable, pure C ABI (`extern "C"`) function and callback table, ensuring zero-overhead FFI bindings for Swift (iOS/macOS), Kotlin/Java via JNI (Android), C# via P/Invoke (Windows), and C++ (Linux).
3. **Pluggable Media Drivers**: Abstract driver interfaces (`IVideoCaptureDriver`, `IVideoRendererDriver`, `IVideoCodecDriver`) in `sip_core` connecting to turnkey platform-native hardware views (`SipVideoView` on Apple, `SipSurfaceView` on Android, `SipVideoPanel` on Windows/Linux).
4. **Phased Rollout**: Milestone 1 extracts `sip_core` and verifies iOS with zero test regressions; Milestone 2 builds the Android SDK (`.aar` + `MediaCodec`); Milestone 3 expands to desktop platforms. Web (WASM / WebRTC) is explicitly deferred to Phase 2.

## Consequences
- **Positive**: Bug fixes in SIP lifecycle, media negotiation, or protocol recovery are implemented once in `sip_core` and immediately benefit all platforms.
- **Positive**: Zero overhead across platform boundaries; no language-specific VM or garbage collection inside the real-time media path.
- **Negative**: Platform UI views and hardware codec drivers (`MediaCodec`, `VideoToolbox`, `Media Foundation`) must be maintained per platform ecosystem.

## Alternatives Considered
- **Pure ANSI C Core**: Lacks RAII, clean concurrency synchronization (`std::mutex`, `std::thread`), and modern event typing, pushing memory management overhead into call sites.
- **Cross-Platform UI Framework Plugin (Flutter / React Native)**: Cannot be consumed directly by existing native iOS (Swift/UIKit) or Android (Kotlin) applications without bringing in a large runtime dependency.
- **Rust Core with UniFFI**: Introduces complex unsafe FFI bridging to wrap Baresip's C APIs, only to expose another FFI layer back to Swift and Kotlin.

## Status
Accepted.
