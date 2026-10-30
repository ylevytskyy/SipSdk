# SipSdk

An iOS SIP SDK and demo application built around the modular [Baresip](https://github.com/baresip/baresip) engine.

The `TareSIPDemo` app is now wired to the modern Baresip v4 implementation through the repository's local Swift Package Manager (SwiftPM) package. The previous `taresip 0.1.1` CocoaPods wrapper, its config and lockfile remain tracked as legacy rollback material, but they are excluded from the active app target.

## Architecture

```text
Swift UIKit demo
       |
       | Objective-C public API
       v
SipSdk Objective-C++ Baresip v4 adapter
       |
       +---- SipSdkResources (config.baresip-v4)
       |
       v
BaresipCore XCFramework (Baresip + libre)
```

The SwiftPM package pins BaresipCore to the published `baresip-core-v4.12.0` release and its SHA-256 checksum. The Xcode project references the repository root as a local package and links its `SipSdk` library product.

## Requirements

- macOS with Xcode 15.4 or newer.
- Swift tools 5.9 or newer.
- A SIP account and server that you are authorized to use for testing.

The app and package retain an iOS deployment target of 12.1.

## Build the demo

Open `TareSIPDemo.xcodeproj` in Xcode. Xcode resolves the local Swift package and downloads the pinned BaresipCore XCFramework.

From the repository root:

```bash
swift package dump-package
xcodebuild -resolvePackageDependencies \
  -project TareSIPDemo.xcodeproj \
  -scheme TareSIPDemo

xcodebuild \
  -project TareSIPDemo.xcodeproj \
  -scheme TareSIPDemo \
  -configuration Debug \
  -sdk iphonesimulator \
  -destination 'generic/platform=iOS Simulator' \
  CODE_SIGNING_ALLOWED=NO \
  build
```

Configure a test SIP account in `TareSIPDemo/SipCallsViewController.swift` before running the app. Do not commit real account credentials. The modern adapter loads `config.baresip-v4` from the SwiftPM resource bundle into `Documents/baresip-v4`, separate from the legacy `Documents/config`.

## Legacy rollback

The old `TareSIPDemo/SipClient.mm` / `SipClient.h`, `TareSIPDemo/config`, `Podfile` and `Podfile.lock` remain tracked under the legacy integration. They are intentionally excluded from the current app target and build phases to prevent duplicate `SipClient` implementations and linking both SIP engines into one app.

To roll back the app integration, revert the demo SwiftPM integration change (or check out its parent revision) before running `pod install --deployment`. Do not delete the legacy files until the Baresip v4 implementation has passed device-level validation.

## Current status and limitations

- The main demo target now references the release-pinned SwiftPM package and modern Baresip v4 adapter.
- CI builds the demo for iOS device arm64 and iOS Simulator arm64/x86_64.
- SIP registration, incoming/outgoing calls, two-way audio, DTMF, repeated start/stop, network transitions and background behavior still require testing on a physical iPhone against a controlled SIP server.
- TLS is disabled in the current native build (`USE_OPENSSL=OFF`), so SIP over TLS is not supported by this release.
- A green compile/link job does not prove SIP signaling or media behavior.

See [docs/SPM_MIGRATION.md](docs/SPM_MIGRATION.md) for the migration and runtime validation checklist.
