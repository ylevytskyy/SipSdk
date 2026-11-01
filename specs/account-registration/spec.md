# Spec: SIP Account Registration & Presence (`account-registration`)

## 1. Problem & Architecture Overview

### Problem
In the legacy implementation, SIP Address of Record (AOR) formatting, MD5 digest challenge negotiation, registration refresh timers, unregistration sequences, and UI thread event marshaling were embedded inside the iOS-specific Objective-C++ adapter (`Sources/SipSdk/SipClientBaresipV4.mm`). Moving to a cross-platform SDK requires extracting this registration and presence state machine into the portable C++17 core (`sip_core`) behind the clean, pure C ABI (`sip_abi`), while ensuring that all platform consumers (iOS Swift/UIKit, Android Kotlin, Desktop C#/C++) receive thread-safe, main-thread-marshaled registration status events.

### Solution
1. **Shared C++17 Core Engine (`core/`)**:
   - Manage `struct ua*` and `struct account*` lifecycles inside `sip_core::Engine`.
   - Format standard SIP AORs: `sip:<username>@<domain>:<port>[;transport=<transport>]`.
   - Configure authentication credentials (`account_set_auth_user`, `account_set_auth_pass`, `account_set_display_name`).
   - Listen for Baresip registration events (`BEVENT_REGISTERING`, `BEVENT_REGISTER_OK`, `BEVENT_REGISTER_FAIL`, `BEVENT_UNREGISTERING`) via `bevent_register`.
   - Parse SIP status codes and reason phrases from `struct sip_msg` or event payload.
   - Enforce UI thread marshaling: deliver events via an optional platform dispatcher callback (`dispatch_to_main`) in `sip_callbacks_t`, defaulting to Apple Grand Central Dispatch (`dispatch_async(dispatch_get_main_queue(), ...)`) on Apple targets and direct dispatch in headless environments.
2. **Flat C ABI Boundary (`core/include/sip_abi.h`)**:
   - Export pure C functions with default visibility:
     - `int sip_client_register(sip_client_t *client, const char *user, const char *domain, const char *pwd, const char *transport, int port);`
     - `int sip_client_unregister(sip_client_t *client);`
     - `bool sip_client_is_registered(const sip_client_t *client);`
   - Typed callback in `sip_callbacks_t`:
     - `void (*on_reg_state)(sip_client_t *client, sip_reg_state_t state, int code, const char *reason, void *user_data);`
     - `void (*dispatch_to_main)(void (*task)(void *arg), void *arg, void *user_data);`
3. **Platform Integration (`Sources/SipSdk/`)**:
   - Wire `SipClient` methods (`-registry`, `-unregister`, `-isRegistered`) to the underlying C ABI functions.
   - Route `on_reg_state` callbacks into `SipClientDelegate` methods (`onWillRegister:`, `onDidRegister:`, `onFailedRegister:`, `onWillUnRegister:`).

Glossary: [`../../CONTEXT.md`](../../CONTEXT.md). ADR: [`../../docs/adr/0001-cross-platform-cpp-core-c-abi.md`](../../docs/adr/0001-cross-platform-cpp-core-c-abi.md). Parent Epic: Issue #10. Issue: #12.

---

## 2. Public API Specification

### C ABI Extensions (`core/include/sip_abi.h`)

```c
typedef enum {
    SIP_REG_STATE_UNREGISTERED = 0,
    SIP_REG_STATE_REGISTERING,
    SIP_REG_STATE_REGISTERED,
    SIP_REG_STATE_FAILED
} sip_reg_state_t;

typedef struct sip_callbacks_t {
    /** Optional main-thread dispatcher hook for non-Apple platforms or custom event loops. */
    void (*dispatch_to_main)(void (*task)(void *arg), void *arg, void *user_data);

    /** Registration state event callback. */
    void (*on_reg_state)(sip_client_t *client, sip_reg_state_t state, int code, const char *reason, void *user_data);

    void (*on_incoming_call)(sip_client_t *client, sip_call_t *call, void *user_data);
    void (*on_call_state)(sip_call_t *call, sip_call_state_t state, int code, const char *reason, void *user_data);
    void (*on_call_media)(sip_call_t *call, int width, int height, bool on_hold, bool is_muted, void *user_data);
    void (*on_dtmf_digit)(sip_call_t *call, const char *digit, void *user_data);
} sip_callbacks_t;

/**
 * Registers a SIP account with the specified credentials and transport.
 * Dispatches asynchronously onto the core worker event loop.
 *
 * @param client Valid client instance returned from sip_client_create.
 * @param user SIP username / extension (e.g. "1001").
 * @param domain SIP domain or registrar host (e.g. "127.0.0.1" or "sip.iptel.org").
 * @param pwd Account password for MD5 digest authentication.
 * @param transport Protocol transport: "udp" or "tcp" (case-insensitive).
 * @param port SIP port number (e.g. 5060).
 * @return 0 if registration command was queued, or EINVAL on invalid arguments.
 */
SIP_ABI_EXPORT int sip_client_register(sip_client_t *client,
                                      const char *user,
                                      const char *domain,
                                      const char *pwd,
                                      const char *transport,
                                      int port);

/**
 * Unregisters the currently registered account (Expires: 0).
 * Dispatches asynchronously onto the core worker event loop.
 */
SIP_ABI_EXPORT int sip_client_unregister(sip_client_t *client);

/**
 * Checks whether the client currently has a registered SIP user agent.
 */
SIP_ABI_EXPORT bool sip_client_is_registered(const sip_client_t *client);
```

---

## 3. Deliberately Does NOT Do

1. **Does NOT implement SIP over TLS (SIPS)** in this slice (`USE_OPENSSL=OFF`). Supported transports are UDP and TCP.
2. **Does NOT implement multiple simultaneous SIP accounts** on a single `sip_client_t` instance. A single active registered AOR is maintained per client; subsequent `sip_client_register` calls unregister or replace the existing account.
3. **Does NOT implement call session establishment or SDP negotiation** (reserved for Slice 3, Issue #13).
4. **Does NOT implement custom presence publication (PUBLISH/PIDF+XML)** beyond standard SIP REGISTER expires/keep-alive handling.

---

## 4. Alternatives Considered

- **Synchronous Blocking Registration**: Calling thread blocks until 200 OK or 401/403 failure is received from registrar. Rejected because network timeouts would block caller/UI threads.
- **Client-Side Digest Computation**: Caller computes MD5 hash and supplies Authorization header manually. Rejected because Baresip / libre provides robust RFC 2617 / RFC 3261 digest authentication out-of-the-box.
- **Requiring Platform Callers to Manually Marshal Threads**: Making callbacks on raw background worker threads and forcing callers to wrap in UI thread dispatches. Rejected because accidental missing dispatches cause severe UI race conditions and crashes.

---

## 5. Definition of Done

1. `sip_client_register`, `sip_client_unregister`, and `sip_client_is_registered` are exported with `SIP_ABI_EXPORT` in `core/include/sip_abi.h` and verified via `nm -gU libsipcore.a | grep -v " _sip_"`.
2. Automated test `core/tests/test_registration.cpp` passes on iOS Simulator (`arm64`), validating:
   - Registration against Asterisk PBX extension 1001 over UDP with MD5 digest authentication.
   - Delivery of `SIP_REG_STATE_REGISTERING` followed by `SIP_REG_STATE_REGISTERED` (code 200, "OK").
   - Failure state transition on bad credentials (`SIP_REG_STATE_FAILED` with code 401).
   - Clean unregistration (`SIP_REG_STATE_UNREGISTERED`).
   - Callback arrival on the designated UI thread.
3. iOS adapter `Sources/SipSdk/SipClientBaresipV4.mm` routes registration through `sip_client_register` / `sip_client_unregister`.
4. Existing iOS automated test suite (`bash scripts/verify-ios-video-tests.sh`) and app launch (`bash scripts/verify-sim-app-launch.sh`) pass with 100% zero regressions.
