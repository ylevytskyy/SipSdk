# 2. Native SwiftUI & Objective-C++ Adapter for macOS GUI Sample

Date: 2026-11-01

## Status
Accepted

## Context
Following the delivery of the C++ Desktop SDK (`platforms/desktop/`), desktop developers on macOS require a full-fledged graphical user interface (GUI) sample demonstrating SIP registration, dialer keypad, call history, active audio/video call controls, and hardware-accelerated video rendering. We needed to choose the UI framework, bridging layer, and project structure.

## Decision
1. Build the macOS GUI sample as a **Native SwiftUI / Swift application** housed in a dedicated Xcode project at `platforms/macos/sample/SipSdkMac.xcodeproj`.
2. Bridge the shared C ABI (`sip_abi.h`) and `sip_core` engine using an **Objective-C++ adapter layer** (`SipMacClient`, `SipMacCall`, `SipMacVideoView`).
3. Layout the application with a **single-window sidebar (`NavigationSplitView`)** featuring Dialer, Call History, and Account Settings, transitioning into full-frame remote video with local camera picture-in-picture (PIP) during active calls.
4. Provide **automatic webcam detection with synthetic test-pattern fallback** for headless machines.
5. Store account settings and recent calls in `UserDefaults`, defaulting to local Asterisk PBX credentials (`1001@127.0.0.1:5060`).

## Consequences
- **Positive**: Idiomatic macOS look and feel, zero third-party GUI dependencies (no Qt or Electron), seamless CALayer video rendering, and clean isolation from the iOS `SipSdk.xcodeproj`.
- **Negative**: Requires maintaining a separate Xcode project for macOS alongside the iOS project and CMake desktop targets.
- **Alternatives Rejected**:
  - *Adding macOS target to root `SipSdk.xcodeproj`*: Rejected to avoid muddying iOS CI build configurations, bridging headers, and CocoaPods legacy settings.
  - *Cross-platform C++ GUI (Qt 6)*: Rejected to avoid enormous third-party framework dependencies and un-idiomatic macOS UI.
  - *Direct Swift C++ interop*: Rejected because C++ `std::shared_ptr` and virtual listener bridging in Swift remain fragile across compiler toolchains compared to standard Objective-C++.
