# SipSdk (TareSIP Demo for iOS)

> Objective-C++ and Swift demo client for the BareSIP modular SIP engine, integrated through the legacy `taresip` CocoaPod.

This repository currently contains an iOS demo application and a thin Objective-C++ wrapper around the native SIP API. It is not yet a standalone, versioned binary SDK.

## Architecture

```text
Swift UIKit application
        |
        | Objective-C bridging header
        v
Objective-C++ wrapper (SipClient.mm)
        |
        | native C API
        v
TareSIP / BareSIP libraries (SIP, RTP, audio and codecs)
```

## Features

- SIP registration and deregistration.
- Incoming and outgoing audio calls.
- Call lifecycle callbacks exposed through `SipClientDelegate`.
- DTMF start/end events.
- Swift interoperability through `SipClient.h`.

## Requirements

- macOS with Xcode and the iOS Simulator SDK.
- Ruby and CocoaPods.
- An SIP account and server that you are authorized to use for testing.

The app was originally created with Swift 4.2 and an iOS deployment target of 12.1. It relies on `taresip 0.1.1`, whose CocoaPods release dates to 2016. This native dependency is the primary compatibility risk; do not blindly upgrade it or swap it for current BareSIP libraries without checking the C API and binary/linking compatibility.

## Build

From the repository root:

```bash
gem install cocoapods --version 1.17.0
pod install --repo-update
open TareSIPDemo.xcworkspace
```

In Xcode, select the `TareSIPDemo` scheme and an iOS Simulator or connected device, then build. For a command-line simulator build:

```bash
xcodebuild \
  -workspace TareSIPDemo.xcworkspace \
  -scheme TareSIPDemo \
  -configuration Debug \
  -sdk iphonesimulator \
  -destination 'generic/platform=iOS Simulator' \
  ARCHS=x86_64 \
  ONLY_ACTIVE_ARCH=YES \
  CODE_SIGNING_ALLOWED=NO \
  build
```

A GitHub Actions workflow runs the same dependency installation and simulator build on pushes and pull requests.

## SIP configuration

Configure a test SIP account in `TareSIPDemo/SipCallsViewController.swift` before running the app. Do not commit real account credentials. The bundled `TareSIPDemo/config` file contains BareSIP module and audio settings; it is not a secrets store.

## Known limitations / next steps

- The `taresip` 0.1.1 pod is old and has no automated tests published on CocoaPods. Its bundled static libraries include an x86_64 iOS Simulator slice and an arm64 device slice, but no arm64 iOS Simulator slice. CI therefore builds the legacy x86_64 Simulator architecture and arm64 device architecture separately. Supporting the native arm64 Simulator requires rebuilding the native dependencies as an XCFramework with a proper Simulator slice.
- The repository does not yet commit a generated `Podfile.lock`. Although `taresip` is pinned to an exact version, the lockfile should still be committed as part of the next dependency-management pass.
- The sample app still uses legacy Swift / Xcode project settings. Replace the placeholder SIP username, domain, and password in `SipCallsViewController.swift` with a dedicated local configuration mechanism before running it. Do not commit real credentials.
- The repository is a demonstration client; production VoIP behavior, background audio/call handling, security, and runtime error handling need separate validation.

## CI

The `iOS Build` GitHub Actions workflow installs the pinned CocoaPods version, resolves the pinned `taresip` dependency, lists the Xcode workspace schemes, and builds both the legacy x86_64 Simulator slice and the arm64 iOS-device slice. CI also emits warnings from the legacy native headers and mixed Objective-C ABI metadata. A green result verifies compilation/linking only; SIP registration and real call audio must still be tested against a controlled SIP server and device.
