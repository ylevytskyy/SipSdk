# Modern Baresip / Swift Package Manager migration

## Current state

The default branch remains on the working taresip 0.1.1 CocoaPods integration. The native-build pipeline has been merged, but the app must not switch dependencies until the current Baresip adapter, package integration, and SIP runtime behavior have been validated.

The scripts/build-baresip-xcframework.sh script pins and verifies these upstream revisions:

| Component | Release | Pinned commit |
|---|---|---|
| [baresip](https://github.com/baresip/baresip) | v4.12.0 | 9cfab81550acc4bd38d6580d79b45cc6eea65cd6 |
| [re](https://github.com/baresip/re) | v4.12.0 | 65555dad439de9d6a81b3ee6092b9c096585249b |

The script builds a static core for iOS device arm64 and iOS Simulator arm64 + x86_64, creates an XCFramework, and generates a ZIP and SHA-256 checksum. CI performs link smoke tests against the public startup/call APIs for device arm64 and both Simulator architectures. The distribution ZIP includes the upstream Baresip and libre license texts. The pinned libre checkout receives two deterministic iOS build adjustments: its Apple/OpenSSL-disabled build includes the TLS API stubs required by UA allocation, and its legacy HAVE_RESOLV resolver path is disabled because the required resolver symbols are unavailable in the iOS SDK.

The release workflow .github/workflows/publish-baresip-xcframework.yml can publish through either a pushed `baresip-core-vX.Y.Z` tag or the GitHub Actions manual-dispatch UI. The manual path must be run from `master`; it creates an annotated version tag at the exact selected commit and then builds and publishes a GitHub Release containing the XCFramework ZIP, SHA-256 file, build metadata, and standalone license files. Use the exact checksum from the published release when defining the SwiftPM binary target; never use a temporary Actions artifact URL.

## Adapter port staged for validation

Sources/SipSdk/SipClientBaresipV4.mm ports the existing SipClientDelegate contract to the Baresip v4 event/call API. It is intentionally not added to TareSIPDemo.xcodeproj yet: the current CocoaPods app still compiles against the old API, and that working path must remain intact until the release-backed SwiftPM dependency is available.

The native-build workflow runs scripts/check-baresip-adapter.sh, which syntax-checks the new Objective-C++ source and compiles/links it against the freshly built XCFramework for iOS device arm64 and iOS Simulator arm64/x86_64. This catches API mismatch and unresolved native symbols, but it does not prove successful integration into the full demo app or functional SIP/audio behavior.

The paired Sources/SipSdk/Resources/config.baresip-v4 is a minimal configuration for the modules included in the current XCFramework build: g711, audiounit, account, stun, turn, ice, and srtp. The adapter stores the v4 config under a separate Documents/baresip-v4 directory so an older Documents/config left by the CocoaPods app is not accidentally reused. In the eventual SwiftPM package, the config is copied into the generated SipSdk_SipSdk.bundle resource bundle.

### API differences handled by the adapter

- ua_init takes four parameters (software, udp, tcp, tls); this build passes tls=false because TLS is disabled in the packaged engine.
- uag_event_register / UA_EVENT_* have been replaced by bevent_register / BEVENT_*. The callback reads the call and remote URI using current Baresip accessors.
- ua_connect takes five parameters.
- ua_answer and ua_hold_answer require an explicit enum vidmode.
- The AOR is built without embedding the password in the SIP URI; authentication credentials are set on the allocated account.
- The event loop is cancelled and awaited before stack teardown, rather than closing the engine while re_main may still be active.

## Next steps

1. Publish the pinned Baresip XCFramework release from GitHub Actions: open **Actions → Publish Baresip Core XCFramework → Run workflow**, select `master`, and enter `4.12.0`. The workflow creates the annotated `baresip-core-v4.12.0` tag at that master commit and publishes the ZIP, checksum, metadata and license assets. Alternatively, push the same annotated tag locally if you prefer the tag-triggered path.
2. After the release exists, run `bash scripts/generate-spm-package.sh 4.12.0`. The script downloads the published ZIP and checksum sidecar, computes SHA-256 independently, checks the archive contents, and only then generates `Package.swift` with the real URL/checksum. Do not hand-write or guess the checksum.
3. Run `swift package resolve` and build the package for iOS device and Simulator. Then migrate the demo target to the generated SwiftPM package in a separate change and stop compiling the legacy `SipClient.mm` when compiling the v4 adapter.
4. Test on a physical iPhone against a controlled SIP server/account: registration success/failure, incoming and outgoing calls, answer/hangup, DTMF, audio both ways, repeated start/stop, and cleanup after failed initialization. Only then remove Podfile / Podfile.lock.

## Build locally

On macOS with Xcode and CMake installed:

    ./scripts/build-baresip-xcframework.sh

By default, output is written to .build/baresip-xcframework. To choose another destination:

    ./scripts/build-baresip-xcframework.sh "$PWD/dist/baresip"

### Current scope limitation

TLS is disabled (USE_OPENSSL=OFF) to avoid linking a host-built OpenSSL into iOS. The current build is an initial SIP UDP/TCP baseline and does not support SIP-over-TLS. CI compile/link smoke tests prove native API linkage and expected system-library dependencies, not registration or media behavior.
