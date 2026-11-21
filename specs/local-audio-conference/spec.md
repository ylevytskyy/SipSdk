# Local Audio Conference & Call Merging (`local-audio-conference`)

First milestone of Phase 3. Builds on Phase 2 (`specs/enterprise-telephony/spec.md`, merged in PR #47).
VP8 / H.265 video codecs and multiple SIP accounts per `SipClient` remain separate follow-up specs.

## Scope
1. **Local Audio Conference in `core/` (`sip_abi.h`)**
   - One active local audio conference per `SipClient`, supporting `2..max_calls` remote legs (default `max_calls = 4`, range `1..8`).
   - **API:**
     - `sip_client_merge_calls(sip_client_t *client, sip_call_t *call_a, sip_call_t *call_b)` — creates a conference from two open calls, or adds a non-conference call to the existing conference when one of the arguments is already in the conference.
     - `sip_client_add_to_conference(sip_client_t *client, sip_call_t *call)` — adds an open call to the active conference (or returns `EINVAL` if no conference is active).
     - `sip_client_split_call(sip_client_t *client, sip_call_t *call)` — removes one call from the conference and places it on hold; if only 1 leg remains in the conference, dissolves the conference and leaves that remaining leg as a normal active call.
     - `sip_client_leave_conference(sip_client_t *client)` — ends all calls currently in the conference and clears conference state.
     - `sip_call_is_in_conference(const sip_call_t *call)` — returns `true` while the call belongs to the active conference.
     - `on_conference_changed(sip_client_t *client, sip_call_t *const *calls, size_t count, void *user_data)` — fires whenever conference membership changes (created, leg added, leg split/closed, dissolved).
   - **Mix-minus `aufilt` engine (ADR 0007):**
     - Registered in `baresip_aufiltl()` without rebuilding any Baresip prebuilts.
     - Mixes decoded S16LE PCM frames across all un-held conference legs, resampling cleanly when legs negotiate different sample rates (for example, 8 kHz G.711 mixed with 48 kHz Opus) and clamping on overflow.
     - Each remote leg's outgoing encoder receives `local_mic + sum(other_conference_legs_rx)` (minus its own audio).
     - Local playout hears `sum(all_conference_legs_rx)` without double-playing.
   - **Group semantics (extends ADR 0006):**
     - Merging un-holds all participating legs simultaneously (bypassing single-call auto-hold for conference members).
     - Dialing or answering a standalone call while a conference is active auto-holds every leg in the conference.
     - Resuming any held conference leg resumes all legs in the conference and auto-holds any standalone active call.
     - Muting the local mic mutes local audio to all conference legs while remote peers continue hearing each other.
     - Merging a video call keeps audio mixed and pauses video on merged legs (audio-only conference).
     - If a conference leg closes remotely or locally, it is removed; if only 1 leg remains, the conference dissolves automatically and that leg remains `ESTABLISHED`.

2. **Platform Bindings & Sample UI (All 6 Platforms)**
   - **iOS (`platforms/ios`)**: Expose `mergeCalls`, `splitCall`, `isInConference`, `conferenceCalls` on `SipClient` / `SipCall`. Enable `supportsGrouping = true` and `maximumCallsPerCallGroup = maxCalls` in `SipCallKitCoordinator` and wire `CXSetGroupCallAction` to `mergeCalls` / `splitCall`. Add Merge / Split controls to `SipPhone` `InCallView`.
   - **macOS (`platforms/macos`)**: Expose conference methods on `SipMacClient` and Merge / Split buttons in `SipSdkMac` `InCallView`.
   - **Android (`platforms/android`)**: Expose `mergeCalls`, `splitCall`, `leaveConference`, `isInConference`, and `onConferenceChanged` over JNI in `SipClient` / `SipCall` / `SipClientListener`, keep `SipTelecomManagerCoordinator` audio routing active during conference, and add Merge / Split controls to the sample `InCallScreen`.
   - **Desktop C++ (`platforms/desktop`)**: Expose `merge_calls`, `split_call`, `leave_conference`, `is_in_conference`, and `on_conference_changed` on `sip::Client` / `sip::Call` / `sip::IClientListener`.
   - **Windows .NET (`platforms/windows/dotnet`)**: Bind the new `sip_abi.h` conference functions in `SipNativeAbi.cs`, expose `MergeCalls` / `SplitCall` / `LeaveConference` / `ConferenceChanged` on `SipClient` and `IsInConference` on `SipCall`.
   - **Web (`platforms/web/src/sipsdk.js`)**: Implement local mix-minus audio conferencing using the Web Audio API (`AudioContext` + per-peer `MediaStreamAudioDestinationNode` + `RTCRtpSender.replaceTrack`), matching the same group hold, mute, split, and auto-dissolve rules, plus Merge / Split controls in `platforms/web/sample/`.

## Definition of done
Each item is a check that can fail:
- **Core mixer unit test (`core_conference_mixer_test`)**: Injects 3 distinct sine waves (local mic 440 Hz, peer A 1000 Hz, peer B 1800 Hz), including mixed sample rates (8 kHz + 48 kHz), and asserts via Goertzel frequency energy checks that:
  - peer A TX contains 440 Hz + 1800 Hz and suppresses 1000 Hz (at least 30 dB below target tones);
  - peer B TX contains 440 Hz + 1000 Hz and suppresses 1800 Hz;
  - local mute suppresses 440 Hz in both peer A TX and peer B TX while 1000 Hz <-> 1800 Hz cross-mixing remains active;
  - clipping/saturation stays bounded to `[-32768, 32767]`.
- **Core live PBX CTest (`sip_core_audio_call_test` / `sip_core_conference_test`) against Docker Asterisk (`9001` + `9002`)**:
  - Merges two live calls (`9001` and `9002`); asserts both stay `ESTABLISHED` simultaneously (`is_in_conference == true`) and receive RTP packets;
  - Splits one leg; asserts it moves to `HOLD`, the conference dissolves (`count == 0`), and the remaining leg stays `ESTABLISHED`;
  - Dialing a 3rd call while a 2-leg conference is active auto-holds both conference legs; resuming a conference leg auto-holds the 3rd call and resumes both conference legs.
- **All platform verify scripts pass with conference tests**:
  - `bash scripts/verify-desktop-sdk.sh`
  - `bash scripts/verify-ios-video-tests.sh` (including CallKit `supportsGrouping == true` and `CXSetGroupCallAction` mapping)
  - `bash scripts/verify-macos-gui.sh`
  - `bash scripts/verify-android-sdk.sh` (API 26 + API 34 emulators)
  - `bash scripts/verify-windows-sdk.sh`
  - `bash scripts/verify-web-sdk.sh --pbx` (Web Audio mix-minus unit test + live Asterisk `9001` + `9002` merge/split in Playwright)

## Open questions
None (all scope, conference model, group-hold, mute, video, and Web parity decisions resolved).

## Deliberately does NOT do
- **Server-side SIP conferencing (RFC 4353 / Asterisk ConfBridge).** Mixing is strictly local on the client.
- **Multiple independent conferences per `SipClient`.** Only one active local conference at a time.
- **Local video MCU / video compositing.** Conference legs are audio-only; video stays paused/off when merged.
- **Rebuilding Baresip static prebuilts (`BaresipCore.xcframework` / Android / Desktop / Windows).** Uses existing exported `aufilt_register`.
- **VP8 / H.265 codecs and multi-account `SipClient`.** Separate Phase 3 specs.

## Alternatives considered
- **Enabling Baresip's upstream `mixminus` module:** Lost because none of the 4 platform prebuilts include it; enabling it would require rebuilding and republishing `BaresipCore.xcframework` and all static libraries before any feature work, and gives less direct control over unit-testing PCM mix-minus isolation.
- **Strict 3-way hard cap (2 remote legs max):** Lost because the same mix-minus accumulator handles `2..max_calls` legs cleanly and aligns with `max_calls` from Phase 2.
- **Native-only (deferring Web SDK):** Lost to preserve full 6-platform parity established in Phase 1 and Phase 2.

## Next implementation step
Add the C++20 `ConferenceMixer` + `aufilt` mix-minus engine and Goertzel tone-isolation unit tests (`core_conference_mixer_test`) in `core/`, before wiring the C ABI and platform bindings.
