# Production Voice, Security & Call Control (`production-voice-security`)

## Scope
Add three market-baseline capabilities across `core/` (`sip_abi.h`), all native platform SDKs (`ios`, `macos`, `android`, `desktop`, `windows`), `platforms/web`, and `docker/asterisk`:
1. **SIP over TLS + SRTP** (`USE_OPENSSL=ON`, static OpenSSL 3.x, `verify_tls_cert = true` default, optional custom CA PEM, `media_enc` defaulting to `"srtp-mand"` on TLS).
2. **Wideband Audio (Opus / G.722) + Call Quality Stats** (`libopus` bundled in all 4 native build scripts with `opus/48000/2` > `G722/16000/1` > `PCMU/PCMA`, plus `sip_call_stats_t` via `sip_call_get_stats` pull and 2-second `on_call_stats` push callback).
3. **Blind & Attended Call Transfer (`REFER`)** (`sip_call_transfer`, `sip_call_attended_transfer`, `NOTIFY` `message/sipfrag` status via `on_transfer_status`, and app-approved `on_transfer_request` / `sip_call_accept_transfer` with optional auto-accept).

## Definition of done
- All four `scripts/build-baresip-*.sh` scripts compile `libre`/`baresip` with `-DUSE_OPENSSL=ON` and `libopus`, and `BaresipCore.xcframework` is rebuilt and pinned in `Package.swift`.
- `docker/asterisk` runs with TLS (port 5061), SDES-SRTP test profile, `opus` codec enabled, and transfer target extensions (`1004`/`1005`).
- Automated tests on `core` (`sip_core_registration_test` / CTest), `ios` (`verify-ios-video-tests.sh`), `macos` (`verify-macos-gui.sh`), `desktop` (`verify-desktop-sdk.sh`), `windows` (`verify-windows-sdk.sh`), `android` (`testReleaseUnitTest` + `connectedDebugAndroidTest` when device attached), and `web` (`verify-web-sdk.sh --pbx`) pass assertions verifying:
  - TLS registration + SRTP call succeeds with valid CA and fails when `verify_tls_cert = true` against an untrusted cert.
  - A call with `media_enc = "dtls_srtp"` negotiates `UDP/TLS/RTP/SAVP(F)` with an `a=fingerprint` in SDP and exchanges audio (non-zero `audio_bitrate_kbps`) against the Asterisk DTLS test endpoint.
  - Opus is negotiated in SDP when offered and `sip_call_get_stats` / `on_call_stats` reports non-zero `audio_bitrate_kbps` and `audio_codec == "opus"`.
  - Blind `REFER` and attended `REFER` with `Replaces` complete with `200 OK` `sipfrag` status, and incoming `REFER` triggers `on_transfer_request` without auto-dialing when auto-accept is off.

## Open questions
None. All design branches are resolved.

## Deliberately does NOT do
- **Certificate pinning / mutual TLS (mTLS client certs)**: Deferred; only system CA + optional custom CA PEM verification is in scope.
- **Multi-party conferencing / local audio mixing**: Deferred to a separate milestone.
- **SIP Presence, BLF, and MWI (`SUBSCRIBE` / `NOTIFY` outside `REFER`)**: Deferred.
- **Multi-account registration** (`m_ua` remains single-account per `SipClient`).
- **Android `TelecomManager` (`ConnectionService`) / FCM push wakeup**: Deferred.
- **Mid-call video upgrade (`re-INVITE` adding `m=video`) & screen sharing**: Deferred.
- **IMDN delivery/read receipts, typing indicators (`isComposing`), and MSRP file/group chat**: Deferred per `docs/messaging.md`.

## Alternatives considered
- **Platform-native TLS backends (`Network.framework` / `SChannel` / BoringSSL)**: Lost because `libre` v4.12.0 supports OpenSSL natively on all four OS targets without custom C patches.
- **Pull-only or push-only call stats**: Lost because pull-only forces every app to manage its own polling timer, while push-only blocks synchronous one-off reads.
- **Auto-accepting incoming `REFER` by default**: Lost because silently dialing peer-supplied `Refer-To` URIs creates security and toll-fraud risk.

## Next implementation step
Update `scripts/build-baresip-{xcframework,desktop,android,windows}.sh` to build static OpenSSL 3.x (`-DUSE_OPENSSL=ON`) and `libopus`, then verify the rebuilt `libre`/`libbaresip` libraries link cleanly into `core/`.
