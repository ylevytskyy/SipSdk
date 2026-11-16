# 0004. Static OpenSSL 3.x and Mandatory SRTP Default for TLS

- **Status**: Accepted
- **Date**: 2026-11-14

## Context
All four native Baresip build scripts (`build-baresip-{xcframework,desktop,android,windows}.sh`) currently pass `-DUSE_OPENSSL=OFF`, leaving SIP over TLS and DTLS-SRTP disabled.

## Decision
Build and link **OpenSSL 3.x statically** (`-DUSE_OPENSSL=ON`) across iOS, macOS/Linux, Android, and Windows, rebuild and re-pin `BaresipCore.xcframework`, default `verify_tls_cert` to `true` (with optional custom CA PEM), and default `media_enc` to `"srtp-mand"` whenever `transport` is `"tls"` (keeping `"none"` for `"udp"`/`"tcp"`).

## Consequences
- `libre` v4.12.0 uses its native OpenSSL backend on every target without per-OS TLS patches.
- App binary sizes increase slightly due to static OpenSSL 3.x, and `Package.swift` must be regenerated with the new `BaresipCore.xcframework.zip` SHA-256 checksum.
- TLS accounts never silently send unencrypted RTP unless the caller explicitly sets `media_enc = "none"`.

## Alternatives
- **Platform-native TLS (`Network.framework` / `SChannel` / BoringSSL)**: Rejected because `libre` v4.12.0 does not ship native `Network.framework` or `SChannel` TLS backends, which would require maintaining custom C patches across four platforms.
- **Opportunistic SRTP (`"srtp"`) default on TLS**: Rejected because strict SIP-TLS PBX profiles reject unencrypted fallback and opportunistic mode hides misconfigured media encryption.
