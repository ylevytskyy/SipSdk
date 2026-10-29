# Modern Baresip / Swift Package Manager migration

## Current state

The default branch remains on the working taresip 0.1.1 CocoaPods integration. The native-build pipeline has been merged, but the app must not switch dependencies until the current Baresip adapter, package integration, and SIP runtime behavior have been validated.

The scripts/build-baresip-xcframework.sh script pins and verifies these upstream revisions:

| Component | Release | Pinned commit |
|---|---|---|
| [baresip](https://github.com/baresip/baresip) | v4.12.0 | 9cfab81550acc4bd38d6580d79b45cc6eea65cd6 |
| [re](https://github.com/baresip/re) | v4.12.0 | 65555dad439de9d6a81b3ee6092b9c096585249b |

The script builds a static core for iOS device arm64 and iOS Simulator arm64 + x86_64, creates an XCFramework, and generates a ZIP and SHA-256 checksum. CI performs link smoke tests against the public startup/call APIs for device arm64 and both Simulator architectures. The distribution ZIP includes the upstream Baresip and libre license texts. The pinned libre checkout receives two deterministic iOS build adjustments: its Apple/OpenSSL-disabled build includes the TLS API stubs required by UA allocation, and its legacy HAVE_RESOLV resolver path is disabled because the required resolver symbols are unavailable in the iOS SDK.

The tag-triggered workflow .github/workflows/publish-baresip-xcframework.yml rebuilds the pinned sources and publishes a GitHub Release containing the XCFramework ZIP, generated SHA-256 file, build metadata, and standalone license files. Use the exact checksum from the published release when defining the SwiftPM binary target; never use a temporary Actions artifact URL.

## Adapter port staged for validation

TareSIPDemo/SipClientBaresipV4.mm ports the existing SipClientDelegate contract to the Baresip v4 event/call API. It is intentionally not added to TareSIPDemo.xcodeproj yet: the current CocoaPods app still compiles against the old API, and that working path must remain intact until the release-backed SwiftPM dependency is available.

The native-build workflow runs scripts/check-baresip-adapter.sh, which syntax-checks the new Objective-C++ source and compiles/links it against the freshly built XCFramework for iOS device arm64 and iOS Simulator arm64/x86_64. This catches API mismatch and unresolved native symbols, but it does not prove successful integration into the full demo app or functional SIP/audio behavior.

The paired TareSIPDemo/config.baresip-v4 is a minimal configuration for the modules included in the current XCFramework build: g711, audiounit, account, stun, turn, ice, and srtp. The adapter stores the v4 config under a separate Documents/baresip-v4 directory so an older Documents/config left by the CocoaPods app is not accidentally reused.

### API differences handled by the adapter

- ua_init takes four parameters (software, udp, tcp, tls); this build passes tls=false because TLS is disabled in the packaged engine.
- uag_event_register / UA_EVENT_* have been replaced by bevent_register / BEVENT_*. The callback reads the call and remote URI using current Baresip accessors.
- ua_connect takes five parameters.
- ua_answer and ua_hold_answer require an explicit enum vidmode.
- The AOR is built without embedding the password in the SIP URI; authentication credentials are set on the allocated account.
- The event loop is cancelled and awaited before stack teardown, rather than closing the engine while re_main may still be active.

## Next steps

1. Review and merge the adapter compile-check PR only when both the existing CocoaPods app build and the Baresip v4 build/syntax check pass.
2. Publish the XCFramework with a baresip-core-v* tag after the build and release workflows are present on the tagged commit. Confirm the release ZIP, checksum, metadata and license assets.
3. Add a SwiftPM binaryTarget using the real release URL and exact generated checksum. Migrate the demo target to SwiftPM in a separate change and stop compiling the legacy SipClient.mm when compiling the v4 adapter.
4. Test on a physical iPhone against a controlled SIP server/account: registration success/failure, incoming and outgoing calls, answer/hangup, DTMF, audio both ways, repeated start/stop, and cleanup after failed initialization. Only then remove Podfile / Podfile.lock.

## Build locally

On macOS with Xcode and CMake installed:

    ./scripts/build-baresip-xcframework.sh

By default, output is written to .build/baresip-xcframework. To choose another destination:

    ./scripts/build-baresip-xcframework.sh "$PWD/dist/baresip"

### Current scope limitation

TLS is disabled (USE_OPENSSL=OFF) to avoid linking a host-built OpenSSL into iOS. The current build is an initial SIP UDP/TCP baseline and does not support SIP-over-TLS. CI compile/link smoke tests prove native API linkage and expected system-library dependencies, not registration or media behavior.
