# Enterprise Telephony: Multi-Call and Android System Calling (`enterprise-telephony`)

Phase 2 of the commercial roadmap. Phase 1 (TLS, SRTP, DTLS-SRTP, REFER transfer) is
`specs/production-voice-security/spec.md`. Phase 3 (VP8, H.265, local 3-way mixer) gets its own spec.

**Depends on:** `production-voice-security` merged first. Attended transfer there already uses a held
call plus a consult call; this spec builds on that and does not redo it.

## Scope
1. **Multi-Call (Lines)** on every platform: `core/` (`sip_abi.h`), `ios`, `macos`, `android`,
   `desktop`, `windows`, and `web` (`platforms/web/src/sipsdk.js`, which does not use the core and
   implements the same rules itself).
   - Two or more concurrent `SipCall` sessions on the single account of one `SipClient`.
   - **One audio-active call (core-enforced auto-hold).** Answering or dialing while a call has audio
     first sends a hold re-INVITE for that call. Resuming a held call holds the other one (swap).
     If the hold re-INVITE fails, the core still stops local audio send and playout for the old
     call, then reports the hold error via `on_call_media`. The answer or dial goes ahead.
   - **Call waiting.** An incoming INVITE during an active call is reported as a normal incoming call.
   - **Cap.** Account setting `max_calls`, default 4, range 1..8, mapped to libbaresip
     `config.call.max_calls`. A call counts from creation until it is closed. At the cap:
     incoming INVITE gets `486 Busy Here` and the app gets `on_call_rejected_busy`
     (caller URI); `sip_call_dial` returns `EBUSY`.
   - **iOS CallKit.** `SipCallKitCoordinator` sets `maximumCallGroups = max_calls`, keeps
     `maximumCallsPerCallGroup = 1` and `supportsGrouping = false` (no merge until the Phase 3 mixer),
     and maps CallKit hold / swap / "Hold & Accept" to the core rules.
2. **Android system calling (`SipTelecomManagerCoordinator`)** in `platforms/android/sipsdk`.
   - Built on Jetpack `androidx.core:core-telecom` `CallsManager`, pinned to a stable 1.0.x version,
     behind our own coordinator interface. See ADR 0005.
   - `sipsdk` `minSdk` rises from 21 to 26.
   - Mirrors `SipCallKitCoordinator`: outgoing and incoming calls are added to Telecom; answer,
     reject, hang up, hold, unhold, mute, DTMF and audio endpoint changes (earpiece, speaker,
     Bluetooth, wired) flow both ways; system "Hold & Accept" maps to the core auto-hold.
3. **Android push wake-up (`SipPushHandler`)**.
   - `SipPushHandler.handleIncomingCallPush(data: Map<String, String>)`, called by the host app's own
     `FirebaseMessagingService`. The SDK has no Firebase dependency.
   - It adds the call to Telecom at once (ringing UI), starts the client, registers, and links the
     arriving INVITE to that call by SIP Call-ID from the payload.
   - If no matching INVITE arrives within a timeout (default 10 s), the Telecom call ends as
     missed and the app gets an event.

## Definition of done
Each item is a check that can fail.
- `bash scripts/verify-android-sdk.sh` exits 0 and, without a physical device, boots an
  **API 34** and an **API 26** emulator (Google APIs images) and runs `connectedDebugAndroidTest`
  on both against the Docker Asterisk PBX (`10.0.2.2`). Tests assert:
  - an outgoing and an incoming call each appear in `core-telecom` with the matching state
    (dialing, ringing, active, held, disconnected);
  - with two calls, answering the second holds the first, and swap leaves exactly one call un-held;
  - the call over `max_calls` is answered `486 Busy Here` by the core and `on_call_rejected_busy` fires;
  - `SipPushHandler` with a fake payload shows a ringing Telecom call and connects to the matching
    INVITE; without an INVITE, the call ends as missed after the timeout.
- Core CTest (`sip_core_registration_test` or a new multi-call test) against Asterisk asserts:
  two concurrent calls, auto-hold on answer and dial, swap, `486` at the cap, `EBUSY` on dial at the
  cap, and local audio stopped for the old call when its hold re-INVITE is rejected.
- Each platform's verify script passes with a multi-call test (two calls, auto-hold, swap, cap):
  `verify-ios-video-tests.sh`, `verify-macos-gui.sh`, `verify-desktop-sdk.sh`,
  `verify-windows-sdk.sh`, and `verify-web-sdk.sh --pbx`.
- iOS tests assert `SipCallKitCoordinator` reports `maximumCallGroups == max_calls` and maps a
  CallKit hold / swap action to the core.
- Manual evidence in `specs/enterprise-telephony/evidence/` from one physical Android device:
  lock-screen incoming UI, Bluetooth headset routing, and a real FCM wake-up from a killed app.

## Resolved decisions (from `grill-deep`)
- **`core-telecom` version:** Pin `androidx.core:core-telecom:1.0.1` (latest stable 1.0.x on Google Maven) with `minSdk = 26`.
- **Unified push payload keys:** Both Android `SipPushHandler` and iOS `SipPushRegistry` accept `sip_call_id` (alias `call_id`, opaque SIP `Call-ID` string; iOS also preserves optional UUID parsing), `caller_uri` (aliases `from_uri`, `from`), `display_name` (alias `caller_name`), and `has_video` (alias `video`).
- **Asterisk multi-call / `486` test driver:** Add delayed-callback extension `9003` to `docker/asterisk/config/extensions.conf` (answers, waits ~2 s while the single-process test fills `max_calls` via `9001`, and originates an `INVITE` back to the caller) to test real PBX incoming Call Waiting and `486 Busy Here`; use an in-test loopback SIP socket only for protocol-error edge cases (e.g., rejected hold re-INVITE).
- **Android emulator targets:** Verify `verify-android-sdk.sh` on both `system-images;android-26;google_apis;arm64-v8a` and `system-images;android-34;google_apis;arm64-v8a`.

## Open questions
None (all open questions resolved).

## Deliberately does NOT do
- **Multiple accounts per `SipClient`.** Single `m_ua` stays; a later spec.
- **Merging calls / local audio mixing.** That is the Phase 3 3-way mixer.
- **Firebase inside the SDK, or a server-side push gateway.** The host app owns FCM; the PBX or gateway sends the push.
- **System call UI on Android below API 26.** `minSdk` is 26.
- **More than one call with audio at a time.**
- **VP8, H.265.** Phase 3.

## Alternatives considered
- **Raw self-managed `ConnectionService` + `TelecomManager`**: lost because we would write and test
  audio routing, hold and foreground-service handling that `core-telecom` already provides.
- **App-enforced hold (answer fails with `EBUSY` while another call has audio)**: lost because each
  app and each of six platforms would re-implement the same hold-then-answer logic.
- **Mobile-only multi-call first**: lost by user decision; all platforms ship together, like Phase 1.
- **Full FCM integration in the SDK**: lost because it forces Firebase on every consumer and clashes
  with apps that have their own `FirebaseMessagingService`.

## Next implementation step
Add `max_calls` and the auto-hold rule to `core/` with a failing CTest for two concurrent calls
against Asterisk, before touching any platform binding.
