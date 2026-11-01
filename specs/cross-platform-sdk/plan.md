# Plan: Unified Cross-Platform SIP Video SDK (`cross-platform-sdk`)

**Goal**: Extract a portable, thread-safe C++17 core engine (`sip_core`) with a flat C ABI boundary (`sip_abi`) encapsulating Baresip v4 and libre, isolate SIP lifecycle, account registration, multi-call sessions, and video stream recovery in shared code, and verify iOS zero-regression parity before delivering Android and Desktop bindings.

## Global Constraints

- **Language Standard**: C++17 for `sip_core`; pure C11 `extern "C"` interface for `sip_abi.h` with zero C++ name mangling, standard library types, or compiler-dependent ABIs.
- **Symbol Visibility**: `-fvisibility=hidden` on all core source files; only functions prefixed with `sip_*` are exported publicly with default visibility (`SIP_ABI_EXPORT`).
- **Network & Socket ABI Safety**: All CMake targets must configure `-DUSE_UNIXSOCK=ON` to match the layout of `struct sa` and `struct sip_msg` in `libre` and prevent silent registration hangs.
- **Threading Model**: Dedicated background worker thread invoking `re_main()`; all public C ABI calls from arbitrary caller threads must dispatch onto the worker thread via libre `mqueue` message queues without blocking the UI thread.
- **Teardown Contract**: Calling `sip_client_destroy` must terminate the event loop, join the background worker thread, deregister all event handlers, close user agents, unload modules, dereference libre memory, and leave zero leaked file descriptors, sockets, or threads. Sequential init/destroy cycles in the same process must succeed cleanly.
- **Audio Session Prerequisite**: Mobile platform bindings (iOS `AVAudioSession`, Android `AudioManager`) must configure and activate voice audio sessions before Baresip starts microphone capture.
- **Preserved Compatibility**: Zero regressions against existing iOS test harnesses (`scripts/verify-ios-video-tests.sh`, `scripts/verify-spm-video-sdk.sh`, `scripts/verify-sim-app-launch.sh`).

---

## 1. Frame

### Problem
VoIP engine logic, event polling, and critical protocol bug fixes (such as recovering from remote video freezes during mid-call SDP re-INVITEs, requesting RTCP PLI/FIR keyframes on packet loss, and managing hardware codec pipelines) are tightly coupled to Objective-C++ in `Sources/SipSdk/SipClientBaresipV4.mm`. This forces application developers to reinvent VoIP state machines across Android, Windows, macOS, and Linux, while raw C libraries like Baresip and libre expose global threading loops and low-level pointer management that cause UI hangs, memory corruption, and deadlocks if mishandled.

### Solution
Extract a modular C++17 core (`sip_core`) with an opaque, flat `extern "C"` ABI (`sip_abi`) that runs internal Baresip network and timer polling on a dedicated background thread with thread-safe message queues. Platform bindings (iOS Swift, Android Kotlin, Windows C#, Linux C++) consume this universal C ABI without pointer arithmetic or engine internals leaking into applications.

### User Stories (Milestone 1 — Issue #11 Focus)
- **US1 (Init)**: As an application developer, I want to initialize the SIP SDK with an optional configuration path via a single function call, so that the underlying networking and VoIP engines start up reliably without manual thread setup.
- **US2 (Background Loop)**: As an application developer, I want all internal network and media polling to execute on a dedicated background thread, so that the host application's UI thread is never blocked or frozen.
- **US3 (Teardown)**: As an application developer, I want to shut down the SIP SDK cleanly via a single termination call, so that all active media streams, sockets, and threads are terminated gracefully without crashes or memory leaks.
- **US4 (Thread Safety)**: As an application developer, I want API calls to be thread-safe from any caller thread, so that I do not have to write thread synchronization guards when invoking SDK methods.

*(Overarching Epic stories US5–US37 cover Account Registration #12, Audio Calling #13, Video Media Driver #14, Android #15/#16, and Desktop #17/#18).*

### Out of Scope
1. Web (WASM / WebRTC) signaling or browser compilation in Phase 1 (deferred to Phase 2).
2. SIP over TLS (`USE_OPENSSL=ON`) and SRTP in Phase 1; signaling is standard UDP/TCP.
3. Third-party software codecs (Opus, VP8, VP9); relies on built-in G.711 and hardware H.264.
4. Raw frame rendering callbacks exposed to client apps; turnkey views (`SipVideoView`, `SipSurfaceView`) handle display.

### Implementation Decisions
- **`core/CMakeLists.txt`**: Standard CMake project defining static library `sip_core` (`libsipcore.a`), configuring include paths to `BaresipCore` headers, setting `-DUSE_UNIXSOCK=ON`, `-std=c++17`, and enforcing hidden symbol visibility (`-fvisibility=hidden`).
- **`core/include/sip_abi.h`**: Stable C header declaring opaque types `sip_client_t` and `sip_call_t`, function pointers in `sip_callbacks_t`, and lifecycle functions `sip_client_create`, `sip_client_init`, and `sip_client_destroy`.
- **`core/src/engine.h` & `core/src/engine.cpp`**: `sip_core::Engine` class wrapping `libre_init()`, `conf_path_set()`, `conf_configure()`, `re_thread_async_init()`, `baresip_init()`, dedicated `std::thread` executing `re_main()`, `mqueue` command dispatcher for cross-thread calls, and comprehensive teardown sequence (`ua_close`, `baresip_close`, `libre_close`).
- **`core/src/sip_abi.cpp`**: Pure C ABI wrapper forwarding `sip_client_*` calls to internal `sip_core::Engine` instances.

### Testing Decisions
- **Lifecycle Integration Test (`core/tests/test_lifecycle.cpp`)**: Spawns an automated binary that creates, initializes, and destroys the engine 5 times sequentially in the same process, asserting clean thread termination, socket closure, and absence of deadlocks.
- **Symbol Visibility Check**: Audits `libsipcore.a` with `nm -gU` to guarantee zero C++ mangled symbols or internal libre symbols leak into the global namespace.
- **iOS Parity Regression**: Verifies that existing `scripts/verify-ios-video-tests.sh` passes 100% throughout the extraction.

---

## 2. Seam

The test seam attaches at the **Universal C ABI Boundary (`sip_abi.h`)**:
- Callers interact exclusively via opaque pointer `sip_client_t*` and C functions (`sip_client_create`, `sip_client_init`, `sip_client_destroy`).
- Tests compile directly against `sip_abi.h` and link against `libsipcore.a` without depending on C++ headers or internal Baresip structures.
- On Apple platforms (macOS / iOS Simulator), tests execute in the Simulator environment using `xcrun simctl spawn` to validate native Baresip networking, thread safety, and socket binding.

---

## 3. Milestones Roadmap

- [x] **Milestone 1 — C++ Core Extraction & iOS Parity Lock**
  - [x] **Slice 1 (Issue #11)**: Core Engine Lifecycle, C ABI Scaffold & Automated Lifecycle Verification
  - [x] **Slice 2 (Issue #12)**: SIP Account Registration, MD5 Authentication & Presence State Machine (detailed in [`../account-registration/plan.md`](../account-registration/plan.md))
  - [x] **Slice 3 (Issue #13)**: Audio Calling Session, G.711 Negotiation & Telephony Controls (Hold, Mute, DTMF)
  - [x] **Slice 4 (Issue #14)**: Video Media Driver (`IVideoCaptureDriver`, `IVideoRendererDriver`, `IVideoCodecDriver`), Resilient Stream Recovery (re-INVITE `recvonly` preservation & RTCP PLI keyframe request on packet loss), and iOS Parity Lock
- [ ] **Milestone 2 — Android Platform SDK (AAR + MediaCodec)**
  - [x] **Slice 5 (Issue #15)**: Android Platform SDK & Audio Calling Slice (detailed in [`../android-audio-calling/plan.md`](../android-audio-calling/plan.md))
  - [ ] **Slice 6 (Issue #16)**: Android Hardware Video & SurfaceView Rendering Slice
- [ ] **Milestone 3 — Desktop Platform SDKs (macOS, Linux, Windows)** (Issues #17, #18)

---

## 4. Vertical Slices (Milestone 1 · Issue #11 Focus)

### Slice 1 — Core Engine Lifecycle, C ABI Scaffold & Automated Lifecycle Verification (Issue #11)

- **User stories covered**: US1, US2, US3, US4
- **Device tier**: iOS Simulator (`arm64`) & macOS host compilation tier
- **Files**:
  - `core/CMakeLists.txt`
  - `core/include/sip_abi.h`
  - `core/src/engine.h`
  - `core/src/engine.cpp`
  - `core/src/sip_abi.cpp`
  - `core/tests/test_lifecycle.cpp`
  - `scripts/verify-core-lifecycle.sh`
- **Interfaces**:
  - `sip_client_t *sip_client_create(const sip_callbacks_t *callbacks, void *user_data);`
  - `int sip_client_init(sip_client_t *client, const char *config_path);`
  - `void sip_client_destroy(sip_client_t *client);`
  - `typedef struct sip_callbacks_t { ... } sip_callbacks_t;`
- **Acceptance criteria**:
  1. **Given** a clean build environment, **when** `cmake --build` is executed for `sip_core`, **then** `libsipcore.a` static library is produced with zero compilation or linker errors, and `nm -gU libsipcore.a | grep -v " _sip_"` confirms only `sip_*` symbols are exported publicly.
  2. **Given** an allocated `sip_client_t` instance, **when** `sip_client_init(client, config_path)` is called, **then** libre and Baresip v4 initialize, an internal worker thread starts polling `re_main()`, an internal `mqueue` command queue is established, and `sip_client_init` returns 0.
  3. **Given** an active initialized `sip_client_t`, **when** `sip_client_destroy(client)` is called, **then** a termination event is posted to the worker loop, the worker thread joins cleanly, all libre and baresip subsystems are closed, and the client memory is freed without deadlock.
  4. **Given** the automated lifecycle test runner executing on iOS Simulator, **when** initializing, executing an asynchronous command dispatch, and destroying the engine 5 consecutive times in the same process, **then** all 5 cycles succeed with exit code 0, leaving zero dangling threads or open sockets.
  5. **Given** the existing iOS test suite, **when** `bash scripts/verify-ios-video-tests.sh` is executed alongside Slice 1, **then** 100% of existing tests pass with zero regressions.
- **Verify**: `bash scripts/verify-core-lifecycle.sh`
- **Blocked-by**: None

---

## 5. Expand → Migrate → Contract Refactoring Strategy

To maintain an unbroken green build across the repository:

1. **Expand (Slice 1 / Issue #11)**:
   - Create `core/` with CMake configuration, C ABI headers, and `sip_core::Engine` implementation side-by-side with the current `Sources/SipSdk/SipClientBaresipV4.mm`.
   - Keep existing iOS target (`SipSdk`) intact and fully green while validating `sip_core` in isolation.
2. **Migrate (Slices 2–4 / Issues #12–#14)**:
   - Slices 2 & 3 implement account registration and call state machines in `core/`.
   - Slice 4 introduces the abstract `media_driver` interface, wires iOS `VideoToolbox` / `AVCaptureSession` / `SipVideoView` through `media_driver`, and refactors `Sources/SipSdk/SipClientBaresipV4.mm` to act as an Objective-C++ adapter over `sip_abi`.
3. **Contract (Post-Slice 4)**:
   - Remove duplicate monolithic Baresip lifecycle and SIP session code from `SipClientBaresipV4.mm`, retaining only the thin Objective-C++ bridge to `sip_abi`.
   - Verify `scripts/verify-spm-video-sdk.sh`, `scripts/verify-ios-video-tests.sh`, and `scripts/verify-sim-app-launch.sh` with zero regressions.

---

## 6. Review & Refutation Checklist

- [x] All 4 user stories for Issue #11 (US1, US2, US3, US4) are covered in Slice 1.
- [x] Write set contains 5 production files and 2 test/script files; no multi-risk bundling.
- [x] Every slice includes exact file paths, explicit function signatures, observable Given/When/Then acceptance criteria, and exact verification commands.
- [x] Zero placeholders (no TODO, TBD, or vague error handling descriptions).
- [x] Header matches spec constraints verbatim (`-DUSE_UNIXSOCK=ON`, C++17, C11 C ABI, symbol visibility).
- [x] Verified existing iOS test suite runs green before starting changes (`ALL_IOS_VIDEO_TESTS_PASSED`).
