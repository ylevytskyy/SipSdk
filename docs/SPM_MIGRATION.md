# Modern Baresip / Swift Package Manager migration

## Current state

The merged default branch remains on `taresip 0.1.1` and CocoaPods. This is intentional until the new native engine is built and the Objective-C++ adapter is ported and tested. Do not point a SwiftPM binary target at a placeholder URL or remove the working legacy build before the replacement exists.

The first migration milestone is a reproducible `BaresipCore.xcframework` build. `scripts/build-baresip-xcframework.sh` pins and verifies these upstream revisions:

| Component | Release | Pinned commit |
|---|---|---|
| [baresip](https://github.com/baresip/baresip) | v4.12.0 | `9cfab81550acc4bd38d6580d79b45cc6eea65cd6` |
| [re](https://github.com/baresip/re) | v4.12.0 | `65555dad439de9d6a81b3ee6092b9c096585249b` |

The script builds a static core for iOS device arm64 and iOS Simulator arm64 + x86_64, creates an XCFramework, and generates a ZIP and SHA-256 checksum. It performs link smoke tests against the public startup/call APIs for device arm64 and both Simulator architectures, and includes the upstream Baresip and libre license texts in the ZIP. The pinned libre checkout receives two deterministic iOS build adjustments: its Apple/OpenSSL-disabled build includes the TLS API stubs required by UA allocation, and its legacy `HAVE_RESOLV` resolver path is disabled because the required resolver symbols are unavailable in the iOS SDK. The pull-request workflow uploads the output as a short-lived build artifact for validation. A separate tag-triggered workflow in `.github/workflows/publish-baresip-xcframework.yml` rebuilds the same pinned sources and publishes a versioned GitHub Release containing the XCFramework ZIP, its generated SHA-256 file, build metadata, and standalone license files. Use a real published release asset and its exact checksum when defining the SwiftPM binary target; do not use a temporary Actions artifact URL.

## Why the app adapter cannot be carried over unchanged

The old wrapper targets a 2016 API. Current Baresip v4.12.0 has source-level incompatibilities, not just a packaging difference:

- `ua_init` now takes four parameters (`software, udp, tcp, tls`); the old wrapper passes five.
- `uag_event_register` / `UA_EVENT_*` were replaced by `bevent_register` / `BEVENT_*`. The callback now receives `enum bevent_ev`, `struct bevent *`, and the callback context. Calls and event text must be extracted using the current event accessors.
- `ua_connect` now takes five parameters; the old wrapper passes six.
- `ua_answer` and `ua_hold_answer` require an explicit `enum vidmode`.
- The current iOS audio driver is `audiounit`; the old bundled config names `coreaudio`. The legacy config also lists many codec/app modules that are not selected in the minimal current build.

The adapter migration must preserve the existing public `SipClientDelegate` contract while mapping current Baresip events and call references correctly. It should be compiled against the generated XCFramework headers, then tested for registration, incoming/outgoing calls, call closure, DTMF, and audio on a physical device.

## Planned sequence

1. Build and inspect the XCFramework artifact from CI; verify device arm64 and Simulator arm64/x86_64, and pass the device plus both Simulator link smoke tests.
2. Port `SipClient.mm` to the current Baresip event/call API and trim/update the demo config to match selected modules.
3. Add focused adapter tests and document runtime test prerequisites (SIP server/account and a physical iOS device).
4. After the native-build CI is green and the artifact has been inspected, create a `baresip-core-v*` tag on a commit containing the build and release workflows. The tag workflow rebuilds the framework and publishes the ZIP, checksum, and metadata as a GitHub Release. Only after that release exists, add a SwiftPM `binaryTarget` using the published asset URL and exact generated checksum; then migrate the demo project from CocoaPods and remove `Podfile` / `Podfile.lock`.
5. Keep the legacy build available until the new package builds and a real SIP audio call passes.

## Build locally

On macOS with Xcode and CMake installed:

```bash
./scripts/build-baresip-xcframework.sh
```

By default, output is written to `.build/baresip-xcframework`. To choose another destination:

```bash
./scripts/build-baresip-xcframework.sh "$PWD/dist/baresip"
```

### Current scope limitation

TLS is disabled in this first reproducible build (`USE_OPENSSL=OFF`) to avoid linking a host-built OpenSSL into iOS. This enables an initial SIP UDP/TCP audio baseline. Decide on an iOS-compatible TLS implementation and package/link it separately before claiming SIP-over-TLS support. The CI compile/link smoke tests prove native API linkage and expected system-library dependencies, not functional registration or media behavior.
