# Modern Baresip / Swift Package Manager migration

## Current state

The `TareSIPDemo` Xcode target is now wired to the release-backed Baresip v4 SwiftPM package. The legacy `taresip 0.1.1` CocoaPods files remain in the repository as a rollback path but are excluded from the active app target.

The pinned native build uses:

| Component | Release | Pinned commit |
|---|---|---|
| [baresip](https://github.com/baresip/baresip) | v4.12.0 | `9cfab81550acc4bd38d6580d79b45cc6eea65cd6` |
| [re](https://github.com/baresip/re) | v4.12.0 | `65555dad439de9d6a81b3ee6092b9c096585249b` |

The published [Baresip Core XCFramework release](https://github.com/ylevytskyy/SipSdk/releases/tag/baresip-core-v4.12.0) contains static slices for iOS device arm64 and iOS Simulator arm64/x86_64. `Package.swift` pins its URL and SHA-256 checksum.

## App integration

`TareSIPDemo.xcodeproj` references the repository root as a local Swift package and links the `SipSdk` library product. The Swift bridging header imports the package's public Objective-C API:

    @import SipSdk;

The active app target no longer compiles `TareSIPDemo/SipClient.mm`, links the CocoaPods-generated `Pods_TareSIPDemo.framework`, or copies the legacy `TareSIPDemo/config` resource. The old `SipClient.mm`, `SipClient.h` and config are grouped in Xcode as legacy files and remain tracked alongside `Podfile` and `Podfile.lock`.

The app workspace no longer references the generated Pods project. To restore the CocoaPods integration, first revert the SwiftPM demo integration change (or check out its parent revision), then run `pod install --deployment`. Do not run `pod install` on top of the active SwiftPM project because that would reintroduce the old native dependency into the app target.

The modern adapter loads `config.baresip-v4` from the SwiftPM resource bundle into `Documents/baresip-v4`. It searches the app's resource bundles by filename rather than hardcoding SwiftPM's generated bundle name. This keeps the v4 config separate from the legacy `Documents/config`.

## Build and CI

On macOS with Xcode 15.4 or newer:

    swift package dump-package
    xcodebuild -resolvePackageDependencies -project TareSIPDemo.xcodeproj -scheme TareSIPDemo

Open `TareSIPDemo.xcodeproj` in Xcode to build for generic iOS device and iOS Simulator destinations. GitHub Actions performs the package manifest check, resolves the local SwiftPM product and remote binary XCFramework, then builds the demo for:
- iOS device arm64;
- iOS Simulator arm64;
- iOS Simulator x86_64.

These are compilation and linking checks only. They do not prove successful SIP registration or working real-time audio.

## Required physical-device validation

Use a controlled SIP server/account and record results for:

- successful registration and explicit authentication failure;
- outgoing and incoming calls, answer, hangup and remote hangup;
- two-way audio, mute/unmute and DTMF;
- repeated start/stop and cleanup after partial initialization failure;
- Wi-Fi/mobile network changes, packet loss and recovery;
- background/foreground transitions and audio-session interruptions.

Do not remove the legacy integration until this matrix passes.

## Production hardening

Review Baresip's process-global engine lifecycle and document whether multiple `SipClient` instances are supported. Validate callback threading and delegate lifetime, error propagation and teardown races. TLS and certificate validation are not available in the current release; assess SRTP policy, ICE/STUN/TURN configuration and background-call behavior as separate acceptance items.

## Native build and scope

The native Baresip XCFramework can be rebuilt on macOS with Xcode and CMake:

    ./scripts/build-baresip-xcframework.sh

TLS is disabled (`USE_OPENSSL=OFF`) in this initial UDP/TCP baseline, so SIP-over-TLS is not supported. CI compile/link smoke tests prove native API linkage and package integration, not registration, call setup or media behavior.
