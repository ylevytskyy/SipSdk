# SipSdk

An iOS SIP SDK and demo application built around the modular [Baresip](https://github.com/baresip/baresip) engine.

The repository is in a staged migration from the legacy `taresip 0.1.1` CocoaPods integration to a version-pinned Baresip v4 XCFramework distributed through Swift Package Manager (SwiftPM). The modern package is defined in `Package.swift`; the existing demo app remains on the legacy integration until the new adapter passes device-level SIP and audio validation.

## Architecture

### Modern SwiftPM package

```text
Swift / Objective-C application
             |
             v
      SipSdk public API
   Objective-C++ Baresip v4 adapter
             |
             v
       BaresipCore XCFramework
       (Baresip + libre, static)
```

The `SipSdk` SwiftPM product includes the Objective-C++ adapter, the pinned BaresipCore binary dependency, and a small resource target that copies `config.baresip-v4` into the app.

### Legacy demo (kept for compatibility)

```text
Swift UIKit demo
       |
       v
Legacy Objective-C++ wrapper (SipClient.mm)
       |
       v
taresip 0.1.1 CocoaPod
```

The demo is intentionally not switched to the new adapter yet. Keeping the known legacy build available provides a rollback path while SIP behavior is validated on real devices.

## Requirements

- macOS with Xcode and the iOS SDK.
- Swift tools 5.9 or newer.
- For the legacy demo only: Ruby and CocoaPods.
- A SIP account and server that you are authorized to use for testing.

The package retains an iOS deployment target of 12.1. The demo targets iOS 15.0, the lowest deployment target current Xcode accepts. On Apple Silicon the demo's simulator build requires `ARCHS=x86_64` because the `taresip` pod ships no arm64-simulator slice.

## Build the SwiftPM package

From the repository root on macOS:

```bash
swift package dump-package
swift package resolve
```

Open `Package.swift` in Xcode to build the `SipSdk` package for a generic iOS device or iOS Simulator destination.

The first resolution downloads the versioned XCFramework from the GitHub Release. SwiftPM verifies the archive against the checksum in `Package.swift`. The release contains static slices for iOS device arm64 and iOS Simulator arm64/x86_64.

To regenerate the manifest from the published release rather than editing the checksum manually:

```bash
bash scripts/generate-spm-package.sh 4.12.0
```

The script downloads the release checksum sidecar and archive, independently verifies SHA-256, checks the ZIP contents and license files, and then writes `Package.swift`.

## Build the legacy demo

The current `SipSdk` Xcode project still uses CocoaPods:

```bash
gem install cocoapods --version 1.17.0
pod install --deployment --repo-update
open SipSdk.xcworkspace
```

Configure a test SIP account in `SipSdk/SipCallsViewController.swift` before running the app. Do not commit real account credentials. The bundled legacy `SipSdk/config` file contains Baresip module and audio settings; it is not a secrets store.

## Current status and limitations

- The Baresip v4.12.0 XCFramework release is published and pinned by the SwiftPM manifest.
- CI builds and compile/links the modern adapter for iOS device arm64 and iOS Simulator arm64/x86_64.
- The legacy demo remains on `taresip 0.1.1`; migration of the Xcode project is deliberately deferred.
- Real SIP registration, incoming/outgoing calls, two-way audio, DTMF, repeated start/stop and network transitions still require device-level testing against a controlled SIP server.
- TLS is disabled in the current native build (`USE_OPENSSL=OFF`), so SIP over TLS is not supported by this release.
- A green compile/link job does not prove SIP signaling or media behavior.

See [docs/SPM_MIGRATION.md](docs/SPM_MIGRATION.md) for migration details and the remaining validation checklist.

## CI

The repository has separate legacy app-build and modern Baresip XCFramework workflows. The modern workflow builds the pinned native dependency, checks the Objective-C++ adapter against device and Simulator architectures, and validates SwiftPM manifest resolution and the release binary checksum. These checks are compile/package validation only; runtime SIP and audio tests remain outstanding.
