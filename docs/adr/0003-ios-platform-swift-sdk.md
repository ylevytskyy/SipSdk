# ADR 0003: Native Swift 5.9/6 iOS Platform Architecture and Legacy Code Clean-up

## Context
The initial iOS implementation in this repository mixed legacy Objective-C++ delegates (`Sources/SipSdk/SipClientBaresipV4.mm`), CocoaPods infrastructure (`Podfile`, `Podfile.lock`), storyboard-based UIKit controllers (`SipSdk/SipCallsViewController.swift`, `Main.storyboard`), and duplicate C headers across multiple directories. Concurrently, native platforms (`platforms/android`, `platforms/windows`, `platforms/macos`, `platforms/desktop`) were established with modern platform-specific directories consuming the shared `core` C ABI (`sip_abi.h`).

To maximize commercial and enterprise value for iOS developers and align with the unified repo layout, the iOS implementation needed to be refactored into `platforms/ios` using modern Swift paradigms (`@Observable`, `@MainActor`, Swift Concurrency, SwiftUI, SF Symbols), native enterprise services (CallKit, PushKit, zero-downtime mobile handover), and clean removal of dead code.

## Decision
1. **Repository Alignment (`platforms/ios`)**:
   - Staged the iOS SDK at `platforms/ios/Sources/SipSdk` and unit/integration tests at `platforms/ios/Tests/SipSdkTests`.
   - Created a modern SwiftUI sample application at `platforms/ios/sample/SipPhone` with `SipPhone.xcodeproj`.
   - Linked the shared portable C++ engine via SwiftPM `SipCore` (`core/`).

2. **Modern Swift-First API (iOS 17.0+)**:
   - Replaced legacy Objective-C delegates with `@MainActor @Observable` classes (`SipClient`, `SipCall`).
   - Integrated Swift Concurrency: all asynchronous operations (`start()`, `register()`, `makeCall()`, `answer()`, `hangup()`) use `async/await` and throw typed errors (`SipError`).
   - Provided an asynchronous event stream `client.events` (`AsyncStream<SipEvent>`) for reactive observation.

3. **Enterprise Telecom Integrations**:
   - **CallKit**: Built-in `SipCallKitCoordinator` managing `CXProvider` and `CXCallController` for native iOS lock screen, background incoming call handling, and Bluetooth routing.
   - **VoIP PushKit**: `SipPushRegistry` wrapper handling incoming Apple VoIP push notifications and ensuring instant CallKit reporting.
   - **Zero-Downtime Network Handover**: Added `client.resetTransports(reRegister:)` backed by additive C ABI `sip_client_reset_transports` to seamlessly switch between Wi-Fi and Cellular networks.

4. **Hardware-Accelerated Video**:
   - Native Apple `VideoToolbox` H.264 encode/decode in the media driver.
   - `SipVideoUIView` and SwiftUI `SipVideoView` `UIViewRepresentable` for direct CALayer YUV420P frame rendering with aspect-ratio management and mirroring.
   - `IosVideoCaptureDriver` bridging `AVCaptureSession` (with synthetic fallback on simulator) into the core engine.

5. **Legacy Cleanup**:
   - Removed CocoaPods (`Podfile`, `Podfile.lock`, `Pods/`, `SipSdk.xcworkspace`).
   - Removed obsolete root `SipSdk/` legacy UIKit app, storyboards, and duplicate `sip_abi.h` headers.
   - Removed legacy `Sources/` and `Tests/SipSdkRunner/`.
   - Updated all verification scripts and CI workflows to use standard `swift build`, `xctest`, and `xcodebuild`.

## Consequences
- **Positive**: Consistent repository structure across all platforms (`platforms/ios`, `platforms/android`, `platforms/macos`, `platforms/windows`, `platforms/desktop`).
- **Positive**: Enterprise-ready SDK easily consumable by modern SwiftUI and UIKit iOS applications via Swift Package Manager.
- **Positive**: Zero legacy technical debt, no deprecated CocoaPods, no dual-engine or uninitialized memory hazards.
- **Negative**: Minimum deployment target raised to iOS 17.0 (justified by `@Observable` macro and modern Apple telecom APIs).

## Status
Accepted.
