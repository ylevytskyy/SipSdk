# Plan: Audio Calling & In-Call Telephony Controls (`audio-calling`)

**Goal**: Implement outgoing and incoming audio-only call sessions through the portable C++17 core engine (`sip_core`) and universal C ABI (`sip_abi`), negotiate G.711 (PCMA/PCMU) SDP audio streams, support session progress (`Connecting`, `Ringing`, `Established`, `Closed`), provide full in-call telephony controls (answer, decline with 486/603, hangup, microphone mute/unmute, hold/resume, live duration timer, DTMF transmission), marshal all state transitions to the main UI thread, and lock zero-regression parity across existing iOS test suites.

## Global Constraints

- **Language Standard**: C++17 for `sip_core`; pure C11 `extern "C"` interface for `sip_abi.h` with zero C++ name mangling, standard library types, or compiler-dependent ABIs.
- **Symbol Visibility**: `-fvisibility=hidden` on all core source files; only functions prefixed with `sip_*` are exported publicly with default visibility (`SIP_ABI_EXPORT`).
- **Network & Socket ABI Safety**: All CMake targets must configure `-DUSE_UNIXSOCK=ON` to match the layout of `struct sa` and `struct sip_msg` in `libre` and prevent silent registration hangs.
- **Threading Model**: Dedicated background worker thread invoking `re_main()`; all public C ABI calls from arbitrary caller threads must dispatch onto the worker thread via libre `mqueue` message queues without blocking the UI thread.
- **UI Thread Dispatch**: Call callbacks (`on_call_state`, `on_incoming_call`, `on_call_media`, `on_dtmf_digit`) must be marshaled to the platform main UI thread (`dispatch_get_main_queue()` on Apple targets or via `dispatch_to_main` hook).
- **Audio Session Activation**: Before media startup on mobile platforms, `prepare_audio_session` hook or Apple `AVAudioSession` activation must be triggered to avoid premature call drops.
- **Preserved Compatibility**: Zero regressions against existing iOS test harnesses (`scripts/verify-ios-video-tests.sh`, `scripts/verify-spm-video-sdk.sh`, `scripts/verify-sim-app-launch.sh`).

---

## 1. Frame

### Problem
Outgoing and incoming SIP call session establishment, G.711 SDP audio stream negotiation, session state progression (`Connecting`, `Ringing`, `Established`, `Closed`), and in-call telephony controls (call answer, decline with 486/603 status codes, hangup, microphone mute/unmute, hold/resume, live duration timing, and RFC 4733 DTMF digit generation) are currently coupled to the iOS-specific Objective-C++ adapter (`Sources/SipSdk/SipClientBaresipV4.mm`). To support multi-platform native bindings (Android Kotlin, Windows C#, Linux C++, iOS Swift), these call state machines and telephony controls must be extracted into the portable C++17 core engine (`sip_core`) behind a pure, flat C ABI (`sip_abi`), guaranteeing thread-safe worker execution and main-thread-marshaled UI callbacks.

### Solution
Extract the call session subsystem into `sip_core::Engine` and `sip_core::CallSession`:
1. Encapsulate `struct call*` allocation and lifecycle inside `sip_core::CallSession`, managing unique call IDs, remote peer URIs, peer display names, duration tracking, mute status, hold status, and direction.
2. Provide outgoing call placement via `sip_call_make(client, uri, with_video)` dispatching to `ua_connect` on the background worker thread with `vidmode = VIDMODE_OFF` for audio calls, negotiating standard G.711 (PCMA/PCMU) SDP audio streams.
3. Listen for Baresip call events (`BEVENT_CALL_INCOMING`, `BEVENT_CALL_OUTGOING`, `BEVENT_CALL_RINGING`, `BEVENT_CALL_PROGRESS`, `BEVENT_CALL_ANSWERED`, `BEVENT_CALL_ESTABLISHED`, `BEVENT_CALL_CLOSED`, `BEVENT_CALL_HOLD`, `BEVENT_CALL_RESUME`, `BEVENT_CALL_DTMF_START`) via `bevent_register`, mapping them into typed `sip_call_state_t` values.
4. Implement in-call telephony controls:
   - Call answer in audio mode: `sip_call_answer(call, false)`
   - Call decline with custom status codes (486 Busy Here / 603 Decline) and hangup: `sip_call_hangup(call, code, reason)`
   - Call hold and resume: `sip_call_set_hold(call, hold)`
   - Call microphone mute/unmute: `sip_call_set_muted(call, muted)` and `sip_client_set_mic_muted(client, muted)`
   - DTMF transmission: `sip_call_send_dtmf(call, digits)`
   - Duration tracking: `sip_call_get_duration(call)`
5. Export pure C ABI functions in `core/include/sip_abi.h` with default visibility and zero C++ name mangling.
6. Marshal all `on_call_state`, `on_incoming_call`, `on_call_media`, and `on_dtmf_digit` callbacks to the platform main UI thread (`dispatch_async` on Apple or caller-provided `dispatch_to_main` callback).
7. Verify 100% zero-regression parity across the iOS test suite and Asterisk 22 PBX.

### User Stories
- **US10 (Outgoing Audio Call)**: As a VoIP user, I want to initiate an outgoing audio-only call by specifying a remote SIP URI, so that I can speak with remote parties without opening my camera.
- **US12 (Call Session Handle)**: As an application developer, I want outgoing call initiation to return a dedicated call session object, so that I can track and control the specific call instance independently.
- **US13 (Call State Transitions)**: As an application developer, I want to receive `Connecting`, `Ringing`, `Established`, and `Closed` state transitions on the UI thread, so that I can update call progress indicators and play ringback tones.
- **US14 (Cancel Outgoing Call)**: As a VoIP user, I want to cancel an outgoing call before the remote peer answers, so that I can abort accidental dials immediately.
- **US15 (Incoming Call Notification)**: As a VoIP user, I want to receive an incoming call notification when a remote party calls my SIP identity, so that I can choose to accept or decline the call.
- **US16 (Incoming Call Metadata)**: As an application developer, I want incoming call notifications to deliver a populated call session containing the caller's URI, display name, and video capability flag, so that I can display a rich incoming call screen.
- **US18 (Audio Answer)**: As a VoIP user, I want to answer an incoming video or audio call in audio-only mode, so that I can speak privately without transmitting video.
- **US19 (Decline Call)**: As a VoIP user, I want to decline or reject an incoming call with standard SIP status codes (e.g. 486 Busy Here or 603 Decline), so that the caller is promptly informed that I cannot answer.
- **US20 (Hangup)**: As a VoIP user, I want to hang up an active call at any time, so that the SIP session and media channels are immediately torn down.
- **US21 (Hold)**: As a VoIP user, I want to place an active call on hold, so that the remote party hears hold music or silence and two-way media is paused.
- **US22 (Resume)**: As a VoIP user, I want to resume a held call, so that two-way audio resumes smoothly without renegotiation artifacts.
- **US23 (Mute/Unmute Mic)**: As a VoIP user, I want to mute and unmute my microphone during a call, so that I can prevent ambient room noise from reaching the remote peer.
- **US24 (DTMF Transmission)**: As a VoIP user, I want to send DTMF tones (digits 0-9, *, #) via RFC 4733 during an active call, so that I can navigate interactive voice response (IVR) phone menus.
- **US25 (Call Duration)**: As an application developer, I want to query the live call duration in seconds, so that I can display an accurate in-call timer in the user interface.
- **US26 (Multi-Call Safety)**: As an application developer, I want to handle multiple simultaneous calls without session collisions, so that my app supports enterprise telephony workflows.
- **US37 (Main UI Thread Guarantee)**: As an SDK consumer, I want all event delegate callbacks to be dispatched directly on the platform's main UI thread, so that I never encounter multithreaded UI state corruption crashes.

### Out of Scope
1. Video capture drivers, video rendering drivers, and H.264 video codec pipelines (reserved for Slice 4, Issue #14).
2. SIP over TLS (`USE_OPENSSL=ON`) or SRTP (deferred to security milestone).
3. Third-party software codecs (Opus, VP8, VP9); relies on built-in G.711 (PCMA/PCMU).
4. Android JNI or Desktop C# platform bindings (reserved for Milestones 2 & 3).

### Implementation Decisions
- **`core/include/sip_abi.h`**: Declare `sip_call_make`, `sip_call_answer`, `sip_call_hangup`, `sip_call_set_hold`, `sip_call_set_muted`, `sip_call_send_dtmf`, `sip_client_set_mic_muted`, and session query getters (`sip_call_get_id`, `sip_call_get_remote_uri`, `sip_call_get_peer_display_name`, `sip_call_has_video`, `sip_call_is_outgoing`, `sip_call_is_on_hold`, `sip_call_is_muted`, `sip_call_is_closed`, `sip_call_get_duration`, `sip_call_get_user_data`, `sip_call_set_user_data`, `sip_call_destroy`).
- **`core/src/call_session.h` & `core/src/call_session.cpp`**: Portable C++ `sip_core::CallSession` wrapping Baresip `struct call*`. Maintains thread-safe atomic flags, timestamps, and string metadata.
- **`core/src/engine.h` & `core/src/engine.cpp`**: Manage active sessions map `std::unordered_map<struct call*, std::shared_ptr<CallSession>> m_calls`. Intercept Baresip call events in `ua_event_handler`, translate to `sip_call_state_t`, and dispatch via `dispatch_to_main` (or GCD).
- **`core/src/sip_abi.cpp`**: Forward C ABI calls to `sip_core::Engine` and `sip_core::CallSession`.
- **`core/tests/test_audio_call.cpp`**: Comprehensive automated C++ integration test suite verifying outbound audio call to Asterisk echo extension 9001, G.711 audio negotiation, live duration timer, hold/resume, mute/unmute, DTMF digits, hangup, inbound call ringing, 486 Busy decline, 603 Decline, and inbound answer.
- **`scripts/verify-audio-calling.sh`**: Shell harness compiling the core library and test runner with CMake for iOS Simulator, running symbol export audit (`nm -gU`), running test suite against local Asterisk PBX, and running `scripts/verify-ios-video-tests.sh`.

### Testing Decisions
- **Asterisk 22 PBX Echo (Extension 9001)**: Live SIP signaling and RTP audio loopback verification against local Asterisk Docker container.
- **SIP Response Codes**: Assert 486 Busy Here and 603 Decline response codes on call decline.
- **Main Thread Delivery**: Callback assertions checking `pthread_main_np()` or dispatcher counters.
- **iOS Parity Regression**: Re-running `scripts/verify-ios-video-tests.sh` to ensure zero regressions in the existing app.

---

## 2. Seam

The test seams attach at:
1. **Universal C ABI Boundary (`core/include/sip_abi.h` and static library `libsipcore.a`)**: Exercised directly by `core/tests/test_audio_call.cpp` on iOS Simulator / macOS host.
2. **Platform Seam (`Sources/SipSdk/include/SipClient.h`)**: Exercised by `Tests/SipSdkRunner/main.mm` on iOS Simulator.

---

## 3. Vertical Slices

### Slice 1 — Outgoing Audio Calling Session, G.711 SDP Negotiation & Lifecycle (Issue #13)
- **User stories covered**: US10, US12, US13, US14, US20, US25, US37
- **Device tier**: iOS Simulator (`arm64`) & macOS host compilation tier
- **Files**:
  - `core/include/sip_abi.h`
  - `core/src/call_session.h`
  - `core/src/call_session.cpp`
  - `core/src/engine.h`
  - `core/src/engine.cpp`
  - `core/src/sip_abi.cpp`
  - `core/CMakeLists.txt`
  - `core/tests/test_audio_call.cpp`
  - `scripts/verify-audio-calling.sh`
- **Interfaces**:
  - `sip_call_t *sip_call_make(sip_client_t *client, const char *target_uri, bool with_video);`
  - `int sip_call_hangup(sip_call_t *call, int code, const char *reason);`
  - `const char *sip_call_get_id(const sip_call_t *call);`
  - `const char *sip_call_get_remote_uri(const sip_call_t *call);`
  - `bool sip_call_is_outgoing(const sip_call_t *call);`
  - `bool sip_call_is_closed(const sip_call_t *call);`
  - `uint32_t sip_call_get_duration(const sip_call_t *call);`
  - `void *sip_call_get_user_data(const sip_call_t *call);`
  - `void sip_call_set_user_data(sip_call_t *call, void *user_data);`
  - `void sip_call_destroy(sip_call_t *call);`
  - Callbacks:
    - `void (*on_call_state)(sip_call_t *call, sip_call_state_t state, int code, const char *reason, void *user_data);`
    - `bool (*prepare_audio_session)(sip_client_t *client, void *user_data);`
    - `void (*release_audio_session)(sip_client_t *client, void *user_data);`
- **Acceptance criteria**:
  1. **Given** an initialized and registered client, **when** `sip_call_make(client, "sip:9001@127.0.0.1:5060", false)` is called, **then** a valid `sip_call_t` handle is returned, `sip_call_is_outgoing` returns true, and the session transitions through `SIP_CALL_STATE_CONNECTING` to `SIP_CALL_STATE_ESTABLISHED` (code 200, "OK") negotiating G.711 PCMA/PCMU audio.
  2. **Given** an active established call session, **when** the call remains connected for over 1 second, **then** `sip_call_get_duration(call)` increments and returns `>= 1`.
  3. **Given** an established call session, **when** `sip_call_hangup(call, 0, "Hangup")` is called, **then** a SIP BYE is transmitted to the remote peer, `on_call_state` fires `SIP_CALL_STATE_CLOSED`, `sip_call_is_closed(call)` returns true, and audio resources are released.
  4. **Given** an active call session before remote answer, **when** `sip_call_hangup(call, 0, "Cancel")` is invoked, **then** a SIP CANCEL is dispatched and the call immediately terminates with `SIP_CALL_STATE_CLOSED`.
  5. **Given** an environment with main thread callback dispatching configured, **then** every `on_call_state` callback executes on the designated main UI thread.
- **Verify**: `bash scripts/verify-audio-calling.sh`
- **Blocked-by**: None (Issue #12 is resolved)

---

### Slice 2 — In-Call Telephony Controls: Microphone Mute, Hold/Resume & DTMF Digits
- **User stories covered**: US21, US22, US23, US24, US26
- **Device tier**: iOS Simulator (`arm64`) & macOS host compilation tier
- **Files**:
  - `core/include/sip_abi.h`
  - `core/src/call_session.h`
  - `core/src/call_session.cpp`
  - `core/src/engine.h`
  - `core/src/engine.cpp`
  - `core/src/sip_abi.cpp`
  - `core/tests/test_audio_call.cpp`
- **Interfaces**:
  - `int sip_call_set_hold(sip_call_t *call, bool hold);`
  - `bool sip_call_is_on_hold(const sip_call_t *call);`
  - `int sip_call_set_muted(sip_call_t *call, bool muted);`
  - `bool sip_call_is_muted(const sip_call_t *call);`
  - `int sip_client_set_mic_muted(sip_client_t *client, bool muted);`
  - `bool sip_client_is_mic_muted(const sip_client_t *client);`
  - `int sip_call_send_dtmf(sip_call_t *call, const char *digits);`
  - Callbacks:
    - `void (*on_call_media)(sip_call_t *call, int width, int height, bool on_hold, bool is_muted, void *user_data);`
    - `void (*on_dtmf_digit)(sip_call_t *call, const char *digit, void *user_data);`
- **Acceptance criteria**:
  1. **Given** an established audio call session, **when** `sip_call_set_muted(call, true)` is called, **then** `sip_call_is_muted(call)` returns true, microphone audio transmission stops, and `on_call_media` reports `is_muted = true`; when unmuted, audio transmission resumes.
  2. **Given** an established audio call session, **when** `sip_call_set_hold(call, true)` is called, **then** a SIP re-INVITE with `sendonly` or `inactive` is negotiated, `sip_call_is_on_hold(call)` returns true, and `on_call_state` transitions to `SIP_CALL_STATE_HOLD`; when `sip_call_set_hold(call, false)` is called, the call resumes to `SIP_CALL_STATE_ESTABLISHED`.
  3. **Given** an established audio call session, **when** `sip_call_send_dtmf(call, "123#*")` is called, **then** RFC 4733 RTP telephone-event packets are transmitted to the remote peer, returning 0 without error.
  4. **Given** multiple active call sessions, **when** hold or mute is applied to one call, **then** the other call's state and audio streams remain completely independent without cross-talk or state collision.
- **Verify**: `bash scripts/verify-audio-calling.sh`
- **Blocked-by**: Slice 1

---

### Slice 3 — Incoming Call Signaling, Caller Metadata, Answer & Decline (486/603) with iOS Parity Lock
- **User stories covered**: US15, US16, US18, US19, US20, US37
- **Device tier**: iOS Simulator (`arm64`) & macOS host compilation tier
- **Files**:
  - `core/include/sip_abi.h`
  - `core/src/call_session.h`
  - `core/src/call_session.cpp`
  - `core/src/engine.h`
  - `core/src/engine.cpp`
  - `core/src/sip_abi.cpp`
  - `core/tests/test_audio_call.cpp`
  - `Sources/SipSdk/include/sip_abi.h`
  - `SipSdk/sip_abi.h`
  - `Tests/SipSdkRunner/main.mm`
- **Interfaces**:
  - `int sip_call_answer(sip_call_t *call, bool with_video);`
  - `const char *sip_call_get_peer_display_name(const sip_call_t *call);`
  - `bool sip_call_has_video(const sip_call_t *call);`
  - Callback:
    - `void (*on_incoming_call)(sip_client_t *client, sip_call_t *call, void *user_data);`
- **Acceptance criteria**:
  1. **Given** an incoming SIP INVITE from a remote party, **when** received by the engine, **then** `on_incoming_call` fires on the main UI thread with a valid `sip_call_t` session where `sip_call_is_outgoing` is false, `sip_call_get_remote_uri` returns caller AOR, and `sip_call_has_video` is false for audio offers.
  2. **Given** an incoming call session in `SIP_CALL_STATE_INCOMING`, **when** `sip_call_hangup(call, 486, "Busy Here")` is invoked, **then** a SIP 486 Busy Here response is sent, the caller receives 486, and the session transitions to `SIP_CALL_STATE_CLOSED`.
  3. **Given** an incoming call session in `SIP_CALL_STATE_INCOMING`, **when** `sip_call_hangup(call, 603, "Decline")` is invoked, **then** a SIP 603 Decline response is sent, the caller receives 603, and the session transitions to `SIP_CALL_STATE_CLOSED`.
  4. **Given** an incoming audio call session, **when** `sip_call_answer(call, false)` is invoked, **then** a SIP 200 OK answer is sent with G.711 audio SDP, two-way audio begins, and state transitions to `SIP_CALL_STATE_ESTABLISHED`.
  5. **Given** the complete test suite including existing iOS app builds and tests (`scripts/verify-ios-video-tests.sh`, `scripts/verify-spm-video-sdk.sh`, `scripts/verify-sim-app-launch.sh`), **when** executed after the C ABI and engine extensions, **then** all pass with zero regressions.
- **Verify**: `bash scripts/verify-audio-calling.sh && bash scripts/verify-ios-video-tests.sh`
- **Blocked-by**: Slice 2

---

## 4. Review & Verification Matrix

| Slice | Title | Blocked By | User Stories | Production Files Written | Verification Command |
|---|---|---|---|---|---|
| **Slice 1** | Outgoing Audio Calling Session, G.711 SDP Negotiation & Lifecycle | None | US10, US12, US13, US14, US20, US25, US37 | 7 files | `bash scripts/verify-audio-calling.sh` |
| **Slice 2** | In-Call Telephony Controls: Microphone Mute, Hold/Resume & DTMF Digits | Slice 1 | US21, US22, US23, US24, US26 | 5 files | `bash scripts/verify-audio-calling.sh` |
| **Slice 3** | Incoming Call Signaling, Caller Metadata, Answer & Decline (486/603) with iOS Parity Lock | Slice 2 | US15, US16, US18, US19, US20, US37 | 8 files | `bash scripts/verify-audio-calling.sh && bash scripts/verify-ios-video-tests.sh` |
