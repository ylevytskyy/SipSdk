# Modern Baresip / Swift Package Manager migration

## Current state

The repository is migrating from the legacy `taresip 0.1.1` CocoaPods integration to a release-backed Baresip v4 SwiftPM package. The migration is deliberately staged: the existing demo remains on CocoaPods until the new adapter has been integrated and exercised on a physical iOS device.

The pinned native build uses:

| Component | Release | Pinned commit |
|---|---|---|
| [baresip](https://github.com/baresip/baresip) | v4.12.0 | `9cfab81550acc4bd38d6580d79b45cc6eea65cd6` |
| [re](https://github.com/baresip/re) | v4.12.0 | `65555dad439de9d6a81b3ee6092b9c096585249b` |

The published [Baresip Core XCFramework release](https://github.com/ylevytskyy/SipSdk/releases/tag/baresip-core-v4.12.0) contains a static XCFramework for iOS device arm64 and iOS Simulator arm64/x86_64, the ZIP checksum, build metadata and upstream license texts.

`Package.swift` pins the release URL and SHA-256 checksum. The checksum is the GitHub Release asset digest for `BaresipCore.xcframework.zip`; SwiftPM checks it when resolving the binary target. The manifest can be regenerated and independently verified from the published checksum sidecar and ZIP:

    bash scripts/generate-spm-package.sh 4.12.0

Do not hand-write or guess a checksum. The generator verifies the sidecar format, independently hashes the downloaded ZIP, checks the ZIP archive, and verifies that the XCFramework Info.plist and both license files are present before writing the manifest.

## Package contents

The SwiftPM package exposes one `SipSdk` library product containing two targets:

- `SipSdk`: the Objective-C++ adapter in `Sources/SipSdk`, with its public header and the pinned `BaresipCore` binary dependency.
- `SipSdkResources`: a small Swift resource target that copies `Resources/config.baresip-v4` into its generated resource bundle. The Objective-C++ adapter loads `SipSdk_SipSdkResources.bundle` from the app bundle.

The resource is kept in a separate Swift target because the SwiftPM 5.9 manifest API used by the Xcode 15.4 toolchain does not expose the `resources` parameter for the Clang/Objective-C++ target initializer. The resource target is part of the same library product, so consumers add only the `SipSdk` package product.

The package links the required Apple frameworks/system libraries and declares iOS 12.1 as its minimum deployment target.

On macOS with Xcode:

    swift package dump-package
    swift package resolve

Open `Package.swift` in Xcode to build for generic iOS device and iOS Simulator destinations. A consuming application can add this checkout as a local Swift package while the app migration is being validated. A separate semantic-version tag for distributing the SipSdk source package remotely should be created only after this package change is merged and validated; the `baresip-core-v4.12.0` tag versions the binary dependency, not the SipSdk package itself.

## Adapter port staged for validation

`Sources/SipSdk/SipClientBaresipV4.mm` ports the existing `SipClientDelegate` contract to the Baresip v4 event/call API. It is intentionally not added to `TareSIPDemo.xcodeproj` yet: the current CocoaPods app still compiles against the old API, and that working path must remain intact until the new package is validated.

The modern-build workflow:
- builds the pinned Baresip/libre sources and creates the XCFramework;
- syntax-checks the manifest generator;
- resolves the checked-in SwiftPM package (including archive checksum verification);
- syntax-checks and compiles/links the adapter for iOS device arm64 and iOS Simulator arm64/x86_64.

These checks catch API mismatches, missing native symbols and package/archive integrity issues. They do not prove successful integration into the full demo app or functional SIP/audio behavior.

### API differences handled by the adapter

- `ua_init` takes four parameters (software, udp, tcp, tls); this build passes `tls=false` because TLS is disabled in the packaged engine.
- `uag_event_register` / `UA_EVENT_*` have been replaced by `bevent_register` / `BEVENT_*`. The callback reads the call and remote URI using current Baresip accessors.
- `ua_connect` takes five parameters.
- `ua_answer` and `ua_hold_answer` require an explicit `vidmode` enum.
- The AOR is built without embedding the password in the SIP URI; authentication credentials are set on the allocated account.
- The event loop is cancelled and awaited before stack teardown, rather than closing the engine while `re_main` may still be active.

## Next steps

### 1. Validate SwiftPM packaging

The manifest and release are now in place. CI must pass the SwiftPM parse/resolve steps, including the binary archive checksum validation. On a developer Mac, also open the package in Xcode and build both a generic iOS device and an iOS Simulator destination.

### 2. Integrate the package into the demo in a separate change

Update the Xcode project to consume the local SwiftPM package and compile the modern adapter instead of the legacy `SipClient.mm`. Keep the legacy source and CocoaPods setup available until the modern path is proven. Avoid compiling both implementations of the same public `SipClient` API into one target.

### 3. Perform physical-device SIP validation

Use a controlled SIP server/account and record results for:

- successful registration and explicit authentication failure;
- outgoing and incoming calls, answer, hangup and remote hangup;
- two-way audio, mute/unmute and DTMF;
- repeated start/stop and cleanup after partial initialization failure;
- Wi-Fi/mobile network changes, packet loss and recovery;
- background/foreground transitions and audio-session interruptions.

Do not remove the legacy integration until this matrix passes.

### 4. Harden before production use

Review Baresip's process-global engine lifecycle and document whether multiple `SipClient` instances are supported. Validate callback threading and delegate lifetime, error propagation and teardown races. Define the production transport/security requirements: TLS and certificate validation are not available in the current release; assess SRTP policy, ICE/STUN/TURN configuration and background-call behavior as separate acceptance items.

## Native build

On macOS with Xcode and CMake installed:

    ./scripts/build-baresip-xcframework.sh

By default, output is written to `.build/baresip-xcframework`. To choose another destination:

    ./scripts/build-baresip-xcframework.sh "$PWD/dist/baresip"

## Current scope limitation

TLS is disabled (`USE_OPENSSL=OFF`) to avoid linking a host-built OpenSSL into iOS. The current build is an initial SIP UDP/TCP baseline and does not support SIP-over-TLS. CI compile/link smoke tests prove native API linkage and expected system-library dependencies, not registration, call setup or media behavior.
