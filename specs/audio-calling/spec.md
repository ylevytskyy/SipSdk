# Spec: Audio Calling & In-Call Telephony Controls (`audio-calling`)

## 1. Problem & Architecture Overview

### Problem
Outgoing and incoming SIP call session establishment, G.711 SDP audio stream negotiation, session state progression (`Connecting`, `Ringing`, `Established`, `Closed`), and in-call telephony controls (call answer, decline with 486/603 status codes, hangup, microphone mute/unmute, hold/resume, live duration timing, and RFC 4733 DTMF digit generation) are currently coupled to the iOS-specific Objective-C++ adapter (`Sources/SipSdk/SipClientBaresipV4.mm`). To support multi-platform native bindings (Android Kotlin, Windows C#, Linux C++, iOS Swift), these call state machines and telephony controls must be extracted into the portable C++17 core engine (`sip_core`) behind a pure, flat C ABI (`sip_abi`), guaranteeing thread-safe worker execution and main-thread-marshaled UI callbacks.

### Solution
1. **Shared C++17 Core Engine (`core/`)**:
   - Manage dedicated call session objects (`sip_core::CallSession`) in `sip_core::Engine` mapping to Baresip's `struct call*`.
   - Initiate outgoing audio calls via `ua_connect` with `vidmode = VIDMODE_OFF`, negotiating standard G.711 (PCMA/PCMU) SDP audio streams.
   - Listen for Baresip call events (`BEVENT_CALL_INCOMING`, `BEVENT_CALL_OUTGOING`, `BEVENT_CALL_RINGING`, `BEVENT_CALL_PROGRESS`, `BEVENT_CALL_ANSWERED`, `BEVENT_CALL_ESTABLISHED`, `BEVENT_CALL_CLOSED`, `BEVENT_CALL_HOLD`, `BEVENT_CALL_RESUME`, `BEVENT_CALL_DTMF_START`) via `bevent_register`.
   - Implement in-call telephony controls:
     - Answer: `ua_answer(ua, call, VIDMODE_OFF)`
     - Decline / Hangup: `ua_hangup(ua, call, scode, reason)` supporting standard SIP response codes (e.g. `486 Busy Here`, `603 Decline`)
     - Hold / Resume: `call_hold(call, hold)` with SDP direction adjustment (`sendonly` / `inactive`)
     - Microphone Mute / Unmute: `audio_mute(call_audio(call), muted)`
     - DTMF: `call_send_digit(call, key)` and `call_send_digit(call, KEYCODE_REL)`
     - Live Duration: `call_duration(call)` tracked and cached upon session termination
   - Enforce UI thread marshaling: deliver all call state, incoming call, media, and DTMF callbacks via `dispatch_to_main` (or GCD on Apple targets).
2. **Universal Flat C ABI Boundary (`core/include/sip_abi.h`)**:
   - Export pure C functions with default visibility:
     - `sip_call_t *sip_call_make(sip_client_t *client, const char *target_uri, bool with_video);`
     - `int sip_call_answer(sip_call_t *call, bool with_video);`
     - `int sip_call_hangup(sip_call_t *call, int code, const char *reason);`
     - `int sip_call_set_hold(sip_call_t *call, bool hold);`
     - `int sip_call_set_muted(sip_call_t *call, bool muted);`
     - `int sip_call_send_dtmf(sip_call_t *call, const char *digits);`
     - `int sip_client_set_mic_muted(sip_client_t *client, bool muted);`
     - Getters: `sip_call_get_id`, `sip_call_get_remote_uri`, `sip_call_get_peer_display_name`, `sip_call_has_video`, `sip_call_is_outgoing`, `sip_call_is_on_hold`, `sip_call_is_muted`, `sip_call_is_closed`, `sip_call_get_duration`, `sip_call_get_user_data`, `sip_call_set_user_data`, `sip_call_destroy`.
   - Typed callbacks in `sip_callbacks_t`:
     - `void (*on_incoming_call)(sip_client_t *client, sip_call_t *call, void *user_data);`
     - `void (*on_call_state)(sip_call_t *call, sip_call_state_t state, int code, const char *reason, void *user_data);`
     - `void (*on_call_media)(sip_call_t *call, int width, int height, bool on_hold, bool is_muted, void *user_data);`
     - `void (*on_dtmf_digit)(sip_call_t *call, const char *digit, void *user_data);`
3. **Verification & Parity Lock**:
   - Automated C++ integration tests (`core/tests/test_audio_call.cpp`) verifying real SIP calls against Asterisk 22 PBX (echo `9001`) and loopback peers.
   - Verification script (`scripts/verify-audio-calling.sh`).
   - Zero-regression parity lock against the existing iOS test suite (`scripts/verify-ios-video-tests.sh`, `scripts/verify-spm-video-sdk.sh`, `scripts/verify-sim-app-launch.sh`).

Glossary: [`../../CONTEXT.md`](../../CONTEXT.md). ADR: [`../../docs/adr/0001-cross-platform-cpp-core-c-abi.md`](../../docs/adr/0001-cross-platform-cpp-core-c-abi.md). Parent Epic: Issue #10. Issue: #13.

---

## 2. Public API Specification

### C ABI Extensions (`core/include/sip_abi.h`)

```c
typedef struct sip_client_t sip_client_t;
typedef struct sip_call_t sip_call_t;

typedef enum {
    SIP_CALL_STATE_INCOMING = 0,
    SIP_CALL_STATE_RINGING,
    SIP_CALL_STATE_CONNECTING,
    SIP_CALL_STATE_ESTABLISHED,
    SIP_CALL_STATE_HOLD,
    SIP_CALL_STATE_CLOSED
} sip_call_state_t;

typedef struct sip_callbacks_t {
    /** Optional main-thread dispatcher hook for non-Apple platforms or custom event loops. */
    void (*dispatch_to_main)(void (*task)(void *arg), void *arg, void *user_data);

    /** Registration state event callback. */
    void (*on_reg_state)(sip_client_t *client, sip_reg_state_t state, int code, const char *reason, void *user_data);

    /** Incoming call event callback. */
    void (*on_incoming_call)(sip_client_t *client, sip_call_t *call, void *user_data);

    /** Call state transition callback. */
    void (*on_call_state)(sip_call_t *call, sip_call_state_t state, int code, const char *reason, void *user_data);

    /** Call media update callback. */
    void (*on_call_media)(sip_call_t *call, int width, int height, bool on_hold, bool is_muted, void *user_data);

    /** In-band / out-of-band DTMF digit received callback. */
    void (*on_dtmf_digit)(sip_call_t *call, const char *digit, void *user_data);

    /** Optional hook to prepare audio hardware session (e.g. AVAudioSession / AudioManager). */
    bool (*prepare_audio_session)(sip_client_t *client, void *user_data);

    /** Optional hook to release audio hardware session when all calls finish. */
    void (*release_audio_session)(sip_client_t *client, void *user_data);
} sip_callbacks_t;

/**
 * Initiates an outgoing call to the specified target SIP URI.
 *
 * @param client Valid client instance returned from sip_client_create.
 * @param target_uri Destination SIP URI (e.g. "sip:1002@127.0.0.1:5060" or "sip:peer@127.0.0.1:15099").
 * @param with_video True to offer video (VIDMODE_ON); false for audio-only (VIDMODE_OFF).
 * @return Opaque handle to the created sip_call_t session, or nullptr on failure.
 */
SIP_ABI_EXPORT sip_call_t *sip_call_make(sip_client_t *client, const char *target_uri, bool with_video);

/**
 * Answers an incoming call.
 *
 * @param call Valid call session handle.
 * @param with_video True to answer with video; false for audio-only.
 * @return 0 on success, or non-zero error code.
 */
SIP_ABI_EXPORT int sip_call_answer(sip_call_t *call, bool with_video);

/**
 * Hangs up, cancels, or rejects an active or incoming call.
 *
 * @param call Valid call session handle.
 * @param code SIP status code to send (e.g. 0 for default BYE/CANCEL, 486 for Busy Here, 603 for Decline).
 * @param reason Reason phrase (or nullptr for default).
 * @return 0 on success, or non-zero error code.
 */
SIP_ABI_EXPORT int sip_call_hangup(sip_call_t *call, int code, const char *reason);

/**
 * Places the call on hold or resumes it.
 *
 * @param call Valid call session handle.
 * @param hold True to hold; false to resume.
 * @return 0 on success, or non-zero error code.
 */
SIP_ABI_EXPORT int sip_call_set_hold(sip_call_t *call, bool hold);

/**
 * Mutes or unmutes the local microphone for this call.
 *
 * @param call Valid call session handle.
 * @param muted True to mute; false to unmute.
 * @return 0 on success, or non-zero error code.
 */
SIP_ABI_EXPORT int sip_call_set_muted(sip_call_t *call, bool muted);

/**
 * Sends DTMF digits across the active call via RFC 4733.
 *
 * @param call Valid call session handle.
 * @param digits String of DTMF digits (e.g. "123#*").
 * @return 0 on success, or non-zero error code.
 */
SIP_ABI_EXPORT int sip_call_send_dtmf(sip_call_t *call, const char *digits);

/**
 * Sets client-wide microphone mute state.
 */
SIP_ABI_EXPORT int sip_client_set_mic_muted(sip_client_t *client, bool muted);

/**
 * Checks client-wide microphone mute state.
 */
SIP_ABI_EXPORT bool sip_client_is_mic_muted(const sip_client_t *client);

/**
 * Returns the unique SIP Call-ID string. Pointer valid until call session destroyed.
 */
SIP_ABI_EXPORT const char *sip_call_get_id(const sip_call_t *call);

/**
 * Returns the remote peer SIP URI string.
 */
SIP_ABI_EXPORT const char *sip_call_get_remote_uri(const sip_call_t *call);

/**
 * Returns the remote peer display name string (if present in From/To header).
 */
SIP_ABI_EXPORT const char *sip_call_get_peer_display_name(const sip_call_t *call);

/**
 * Returns whether the call has negotiated video capability.
 */
SIP_ABI_EXPORT bool sip_call_has_video(const sip_call_t *call);

/**
 * Returns whether this call was initiated locally (outgoing).
 */
SIP_ABI_EXPORT bool sip_call_is_outgoing(const sip_call_t *call);

/**
 * Returns whether the call is currently on hold.
 */
SIP_ABI_EXPORT bool sip_call_is_on_hold(const sip_call_t *call);

/**
 * Returns whether the call microphone is currently muted.
 */
SIP_ABI_EXPORT bool sip_call_is_muted(const sip_call_t *call);

/**
 * Returns whether the call has reached the CLOSED state.
 */
SIP_ABI_EXPORT bool sip_call_is_closed(const sip_call_t *call);

/**
 * Returns the active call duration in seconds.
 */
SIP_ABI_EXPORT uint32_t sip_call_get_duration(const sip_call_t *call);

/**
 * Gets user data associated with this call handle.
 */
SIP_ABI_EXPORT void *sip_call_get_user_data(const sip_call_t *call);

/**
 * Sets user data associated with this call handle.
 */
SIP_ABI_EXPORT void sip_call_set_user_data(sip_call_t *call, void *user_data);

/**
 * Releases memory for a closed call session handle.
 */
SIP_ABI_EXPORT void sip_call_destroy(sip_call_t *call);
```

---

## 3. Deliberately Does NOT Do

1. **Does NOT implement video codec or display drivers** in this slice (`IVideoCaptureDriver`, `IVideoRendererDriver`, `IVideoCodecDriver` and re-INVITE video stream recovery are reserved for Slice 4, Issue #14).
2. **Does NOT implement SIP over TLS (SIPS) or SRTP** in this slice (`USE_OPENSSL=OFF`). Supported transports are UDP and TCP.
3. **Does NOT implement third-party software codecs (Opus, VP8, VP9)**; relies on standard built-in G.711 (PCMA/PCMU).
4. **Does NOT build Android JNI or Desktop C# wrappers** in this slice (Milestones 2 & 3).

---

## 4. Alternatives Considered

- **Global Singleton Call Functions (`sip_client_call_answer()`)**: Rejected because it prevents multi-call handling and causes session confusion in multi-line / call waiting scenarios. Dedicated opaque `sip_call_t*` handles isolate each conversation.
- **Blocking Synchronous Call APIs**: Rejected because SIP transactions and DNS lookups over UDP can take hundreds of milliseconds; all C ABI operations dispatch asynchronously to the background worker loop and report completion via typed callbacks on the main UI thread.

---

## 5. Definition of Done

1. `libsipcore.a` exports all `sip_call_*` C ABI functions with default visibility and passes symbol leakage audits (`nm -gU | grep -v ' _sip_'`).
2. Automated C++ test runner (`core/tests/test_audio_call.cpp`) verifies outgoing G.711 audio call to Asterisk PBX echo service (`9001`), call duration advancing, hold/resume, microphone muting, DTMF digit sending, and clean hangup.
3. Automated C++ test runner verifies incoming call delivery, caller metadata extraction, decline with 486 Busy Here and 603 Decline, and clean audio answer.
4. All callbacks execute on the platform's main UI thread without deadlocks or thread pool starvations.
5. `bash scripts/verify-audio-calling.sh` passes 100% cleanly.
6. `bash scripts/verify-ios-video-tests.sh` passes 100% with zero regressions.
