# AGENTS.md — sipsdk · iOS SIP SDK (SwiftPM + Baresip v4 XCFramework) + legacy TareSIPDemo app

Instructions for any coding agent working in this repo, in any harness. This is the project
layer; the shared engineering core and task pipeline live in the workspace library.
Precedence: direct user instruction > this file > the shared core.

## Shared core

Shared engineering core and task pipeline: `../../agents/CORE.md`, `../../agents/pipeline.md`
— for a harness without its own process layer. A harness that has one uses it; this file
is project facts only.

## Status
Staged migration from legacy `taresip 0.1.1` (CocoaPods) to a pinned Baresip v4.12.0
XCFramework via SwiftPM. The modern package compiles; the demo deliberately stays on the
legacy integration as a rollback path until device-level SIP/audio validation passes.
Runtime SIP (registration, calls, two-way audio, DTMF, start/stop, network transitions) is
NOT yet validated — see the checklist in `docs/SPM_MIGRATION.md`. TLS is disabled in the
current native build (`USE_OPENSSL=OFF`): no SIP over TLS.

## Stack & tooling
- SwiftPM package (`Package.swift`, tools 5.9): `SipSdk` ObjC++ adapter target
  (`Sources/SipSdk`, public headers in `include/`), `SipSdkResources` (copies
  `config.baresip-v4`), binary target `BaresipCore` fetched from a GitHub Release and
  pinned by checksum.
- iOS deployment target: package 12.1; demo 15.0 (bumped from 12.1 — Xcode 27 will not
  build targets below 15.0).
- Demo app `TareSIPDemo.xcodeproj` + `TareSIPDemo.xcworkspace` on CocoaPods
  (`taresip 0.1.1`). Do NOT migrate it to the new adapter until device validation passes.
- Never commit real SIP credentials; the demo configures a test account in
  `TareSIPDemo/SipCallsViewController.swift` (`TareSIPDemo/SipCredentials.plist` is
  gitignored). `TareSIPDemo/config` holds Baresip module/audio settings, not secrets.

## Commands
- Verify (compile package for iOS Simulator): 
  `swift build --triple arm64-apple-ios15.0-simulator -Xcc -isysroot -Xcc "$(xcrun --sdk iphonesimulator --show-sdk-path)"`
  — first run resolves + downloads the XCFramework (~2 MB). Triple uses 15.0 because
  Xcode 27 rejects simulator targets below 15.0; the package minimum stays 12.1.
- Manifest check: `swift package dump-package`, `swift package resolve`.
- Regenerate `Package.swift` for a new release: `bash scripts/generate-spm-package.sh <version>`.
- Adapter compile-check script: `scripts/check-baresip-adapter.sh <build-output-dir>` —
  needs a locally built XCFramework from `scripts/build-baresip-xcframework.sh` (CI runs
  this; local run is heavy).
- Legacy demo: `pod install --deployment --repo-update`, then build
  `TareSIPDemo.xcworkspace` / scheme `TareSIPDemo` in Xcode. Verified commands
  (2026-10-31, Xcode 27):
  - Device: `xcodebuild build -workspace TareSIPDemo.xcworkspace -scheme TareSIPDemo -destination 'generic/platform=iOS' CODE_SIGNING_ALLOWED=NO`
  - Simulator (x86_64 only): same with `-destination 'generic/platform=iOS Simulator' ARCHS=x86_64`
    — the taresip pod's fat libs have no arm64-simulator slice (arm64 is device-tagged),
    so an arm64 sim build cannot link; x86_64 links the pod's sim slice.
  - `TareSIPDemo/SipCredentials.plist` must exist (copy from `SipCredentials.example.plist`)
    — it is a build resource; the app runs without it but the build needs the file.
- Web test page (`web/index.html` + vendored `web/jssip-3.13.9.min.js` — jssip 3.13.9
  ships no UMD bundle, so it is esbuild-bundled as an IIFE global; rebuild with
  `npx esbuild node_modules/jssip/lib/JsSIP.js --bundle --minify --format=iife --global-name=JsSIP`).
  SIP over `wss://sip.iptel.org:8081`, audio only:
  serve with `python3 -m http.server 5101 --directory web`, open `http://localhost:5101`
  (localhost is a secure context, so mic access works over plain HTTP). Test targets:
  `sip:echo@iptel.org`, `sip:music@iptel.org`, or the app's own iptel.org AOR. Video phase
  is deferred — the SDK has no video codec and the adapter pins `VIDMODE_OFF`.

## Repo layout & gotchas
- `Sources/SipSdk/SipClientBaresipV4.mm` — ObjC++ Baresip v4 adapter; `include/SipClient.h` — public API.
- `Sources/SipSdkResources/` — resource bundle + `Resources/config.baresip-v4`.
- `TareSIPDemo/` — legacy UIKit demo (Swift) on the taresip pod.
- `.github/workflows/` — `ios-build.yml` (legacy demo CI), `baresip-xcframework.yml` +
  `publish-baresip-xcframework.yml` (modern XCFramework build/check/publish).
- `xcodebuild` at the repo root resolves `TareSIPDemo.xcodeproj`, not the package — use
  `swift build`/`swift package` for the modern side.
- No test targets exist in the package or the demo — verification is compile/link only.

## Device tiers
- Compile/package validation → local `swift build` (verified) and CI.
- Legacy demo UI → device build or x86_64 simulator build (arm64 sim cannot link the
  taresip pod — see Commands).
- SIP signaling/media (registration, calls, audio, DTMF, start/stop, network transitions)
  → real device against a controlled SIP server; outstanding.
- SIP over TLS → unsupported in this release (`USE_OPENSSL=OFF`).
- E2E test tool: none.

## Delivery
- CI: GitHub Actions — `ios-build.yml` builds the legacy demo on push/PR to `master`;
  `baresip-xcframework.yml` builds the XCFramework and checks the adapter on PR.
- Release: GitHub Releases — `BaresipCore.xcframework.zip` published by
  `publish-baresip-xcframework.yml`, pinned in `Package.swift`.
- Deploy target: library consumed via SwiftPM; demo installed to a device via Xcode.
- Environments: unknown (single local machine + GitHub).
- Secrets location: none used; SIP test credentials must not be committed
  (`TareSIPDemo/SipCredentials.plist` gitignored).
- Monitoring: unknown (none).
