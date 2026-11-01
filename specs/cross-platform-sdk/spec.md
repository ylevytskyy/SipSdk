# Spec: Cross-Platform SIP SDK Core Extraction (`cross-platform-sdk`)

## 1. Problem & Architecture Overview

### Problem
The current SIP engine in [`Sources/SipSdk/SipClientBaresipV4.mm`](../../Sources/SipSdk/SipClientBaresipV4.mm) couples Baresip v4.12.0 lifecycle, SIP call session management, re-INVITE video stream recovery, and RTCP PLI/FIR keyframe triggers directly with Apple-specific Objective-C++ and iOS frameworks (`AVFoundation`, `VideoToolbox`, `UIKit`). This prevents reusing these critical VoIP bug fixes and telephony abstractions across Android, Windows, macOS, and Linux.

### Solution
1. **Shared C++17 Core Engine (`core/`)**:
   - Encapsulates Baresip v4 and `libre`, managing engine lifecycle (`init`, `shutdown`), account registration, multi-call sessions, re-INVITE recovery, keyframe requests, and clean teardown.
   - Provides abstract media driver interfaces (`media_driver`) for hardware-accelerated video capture, video rendering, and codecs.
   - Thread-safe command queue decoupling host application threads from the internal Baresip `re_main` event loop.
2. **Flat C ABI Boundary (`sip_abi`)**:
   - Pure `extern "C"` interface exporting functions for engine lifecycle, account management, call control (call, answer, hold, hangup, DTMF), and typed event callbacks.
3. **Pluggable Native Platform Bindings (`platforms/`)**:
   - **iOS / macOS**: Thin Swift wrapper over `sip_abi`, binding Apple `VideoToolbox` H.264 codec, `AVCaptureSession`, and `SipVideoView`.
   - **Android**: Kotlin library via JNI (AAR), binding Android `MediaCodec` H.264, Camera2 capture, and `SipSurfaceView`.
   - **Desktop (Windows / Linux)**: C# (.NET P/Invoke) and C++ bindings with Media Foundation / V4L2 / PulseAudio drivers.

Glossary: [`../../CONTEXT.md`](../../CONTEXT.md). ADR: [`../../docs/adr/0001-cross-platform-cpp-core-c-abi.md`](../../docs/adr/0001-cross-platform-cpp-core-c-abi.md).

---

## 2. Public API Specification

### Core C ABI (`core/include/sip_abi.h`)
```c
typedef struct sip_client_t sip_client_t;
typedef struct sip_call_t sip_call_t;

typedef enum {
    SIP_REG_STATE_UNREGISTERED = 0,
    SIP_REG_STATE_REGISTERING,
    SIP_REG_STATE_REGISTERED,
    SIP_REG_STATE_FAILED
} sip_reg_state_t;

typedef enum {
    SIP_CALL_STATE_INCOMING = 0,
    SIP_CALL_STATE_RINGING,
    SIP_CALL_STATE_CONNECTING,
    SIP_CALL_STATE_ESTABLISHED,
    SIP_CALL_STATE_HOLD,
    SIP_CALL_STATE_CLOSED
} sip_call_state_t;

typedef struct {
    void (*on_reg_state)(sip_client_t *client, sip_reg_state_t state, int code, const char *reason, void *user_data);
    void (*on_incoming_call)(sip_client_t *client, sip_call_t *call, void *user_data);
    void (*on_call_state)(sip_call_t *call, sip_call_state_t state, int code, const char *reason, void *user_data);
    void (*on_call_media)(sip_call_t *call, int width, int height, bool on_hold, bool is_muted, void *user_data);
    void (*on_dtmf_digit)(sip_call_t *call, const char *digit, void *user_data);
} sip_callbacks_t;

// Engine & Account
sip_client_t *sip_client_create(const sip_callbacks_t *callbacks, void *user_data);
int sip_client_init(sip_client_t *client, const char *config_path);
int sip_client_register(sip_client_t *client, const char *user, const char *domain, const char *pwd, const char *transport, int port);
int sip_client_unregister(sip_client_t *client);
void sip_client_destroy(sip_client_t *client);

// Call Control
sip_call_t *sip_call_make(sip_client_t *client, const char *target_uri, bool with_video);
int sip_call_answer(sip_call_t *call, bool with_video);
int sip_call_hangup(sip_call_t *call, int code, const char *reason);
int sip_call_set_hold(sip_call_t *call, bool hold);
int sip_call_send_dtmf(sip_call_t *call, const char *digits);
```

---

## 3. Deliberately Does NOT Do

1. **Does NOT include Web (WASM / WebRTC) in Phase 1**: Browser sandbox networking requires WebRTC and WebSocket signaling; Web support is explicitly deferred to Phase 2.
2. **Does NOT include SIP over TLS (SIPS) or external crypto libraries in Phase 1**: TLS (`USE_OPENSSL=ON`) requires cross-compiling OpenSSL/MbedTLS across all 5 OSes; Phase 1 uses UDP and TCP signaling.
3. **Does NOT bundle third-party software codecs (Opus, VP8, VP9)**: Phase 1 relies on built-in G.711 (PCMA/PCMU) and hardware H.264.
4. **Does NOT require callers to implement raw frame rendering**: Pre-built turnkey video views (`SipVideoView`, `SipSurfaceView`, `SipVideoPanel`) are provided by each platform binding.

---

## 4. Alternatives Considered

- **Pure ANSI C Core**: Lost because the lack of RAII, thread safety abstractions, and strong state encapsulation leaks Baresip pointer complexity to all callers.
- **Cross-Platform UI Framework Plugin (Flutter / React Native)**: Lost because existing native iOS Swift/UIKit and Android Kotlin apps cannot consume Flutter plugins directly without runtime penalties.
- **WebAssembly Core in Phase 1**: Lost because compiling POSIX socket networking to WASM requires complex WebSocket/WebRTC proxying that bloats early milestones.

---

## 5. Open Questions

1. For Milestone 2 (Android), should the camera capture driver build directly on `Camera2` or use the modern Jetpack `CameraX` library?
2. For Milestone 3 (Windows), should the desktop demo UI use WinUI 3 (Windows App SDK) or classic WPF?

---

## 6. Definition of Done

The feature is considered complete when all of the following falsifiable predicates pass:

1. `core/` builds via CMake on macOS into static library `libsipcore.a` with zero compilation or linker errors.
2. `nm -gU libsipcore.a | grep -v " _sip_"` confirms that only symbols matching the `sip_*` C ABI are exported publicly from the core library.
3. The existing iOS test harness [`scripts/verify-ios-video-tests.sh`](../../scripts/verify-ios-video-tests.sh) passes 100% of test cases (Baresip v4 engine initialization, registration, audio loopback, H.264 video loopback, mid-call re-INVITE recovery, packet loss IDR recovery, and clean teardown) with the iOS adapter refactored to consume `libsipcore`.
4. The iOS application builds natively and launches on iOS Simulator (`bash scripts/verify-sim-app-launch.sh`), confirming dialer registration and in-call video screen operation.

---

## 7. Next Implementation Step

Create `core/CMakeLists.txt` and extract the core state machine, Baresip v4 event loop, and C ABI header (`core/include/sip_abi.h`) from `Sources/SipSdk/SipClientBaresipV4.mm`.
