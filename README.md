# SipSdk — Multi-Platform Native SIP Video & Audio SDK

A high-performance, enterprise-grade multi-platform SIP Video and Audio SDK built around a portable C++20 core engine (`core/` / `sip_abi.h`) and modern platform-native implementations powered by [Baresip](https://github.com/baresip/baresip) v4.12.0 and [libre](https://github.com/baresip/re) v4.12.0.

Supported platforms:
- **iOS** (`platforms/ios`): Native Swift 5.9/6 SDK (`@Observable`, `@MainActor`, Swift Concurrency), Apple `VideoToolbox` H.264 hardware encoding/decoding, native `CallKit`, `PushKit` VoIP wake-up, and SwiftUI `SipPhone` sample application.
- **Android** (`platforms/android`): Kotlin / Java SDK (`sipsdk.aar`), `MediaCodec` H.264 hardware encoding/decoding, OpenSL ES / AAudio, multi-arch support (`arm64-v8a`, `armeabi-v7a`, `x86_64`), and sample app.
- **macOS** (`platforms/macos`): Native SwiftUI desktop application (`SipSdkMac.xcodeproj`), VideoToolbox H.264 hardware acceleration, and CoreAudio drivers.
- **Windows** (`platforms/windows`): Modern .NET 8 / C# bindings and WPF Video Phone sample app with Media Foundation drivers and WASAPI audio.
- **Desktop Native / Linux** (`platforms/desktop`): Headless C++ client, CTest automation suite, and Docker Asterisk PBX integration.
- **Web** (`web/index.html`): JsSIP WebRTC companion test page over WebSocket / DTLS-SRTP.

---

## Repository Layout

```
├── core/                           # Portable C++20 core engine & flat C ABI
│   ├── include/sip_abi.h           # Unified C ABI for all platform FFI bindings
│   ├── src/engine.cpp              # Common user-agent lifecycle & registration
│   ├── src/call_session.cpp        # Call state machine & SDP negotiation
│   └── src/media_driver.cpp        # VideoToolbox / MediaCodec / Media Foundation
├── platforms/
│   ├── ios/                        # iOS Platform Target (iOS 17.0+)
│   │   ├── Sources/SipSdk/         # Modern Swift 5.9/6 SDK (@Observable, CallKit, PushKit)
│   │   ├── Tests/SipSdkTests/      # Automated unit & Asterisk PBX video integration tests
│   │   ├── sample/SipPhone/        # Modern SwiftUI sample phone app (SipPhone.xcodeproj)
│   │   └── README.md               # iOS integration guide & documentation
│   ├── android/                    # Android Platform Target (AAR, JNI, MediaCodec)
│   ├── macos/                      # macOS Desktop Platform Target (SwiftUI)
│   ├── windows/                    # Windows .NET / C# Platform Target (WPF)
│   └── desktop/                    # Desktop C++ CLI & prebuilts (macOS/Linux)
├── docker/asterisk/                # Local Asterisk 22 Video PBX for automated testing
├── scripts/                        # Multi-platform build & verification scripts
└── Package.swift                   # SwiftPM manifest for iOS SipSdk & SipCore
```

---

## iOS SDK Quick Start (`platforms/ios`)

The iOS SDK provides a turnkey, reactive Swift 5.9/6 API for iOS 17.0+:

```swift
import SwiftUI
import SipSdk

@MainActor
class PhoneModel: ObservableObject {
    let client = SipClient()

    func connect() async throws {
        try await client.start()
        let account = SipAccount(
            username: "1001",
            domain: "127.0.0.1",
            password: "test1001",
            transport: .udp,
            port: 5060,
            displayName: "iPhone"
        )
        try await client.register(account)
    }

    func call(destination: String) async throws {
        let call = try await client.makeCall(to: destination, withVideo: true)
        print("Connected to \(call.remoteUri)")
    }
}
```

See [platforms/ios/README.md](platforms/ios/README.md) for full documentation, CallKit setup, PushKit configuration, and sample app details.

---

## Local Testing PBX (Docker Asterisk 22)

To test SIP audio and H.264 video calls reliably without public servers, the repository includes an Asterisk 22 PBX with PJSIP and WebRTC bridging.

### Quick Start

```bash
# Start PBX container (with Mac LAN IP detection):
bash scripts/run-pbx.sh

# Stop PBX:
bash scripts/stop-pbx.sh

# Verify PBX health, UDP registration (1001), and video echo call (9001):
bash scripts/verify-pbx.sh
```

### Pre-Configured Extensions (Passwords: `test<ext>`)

| Extension | Target Client | Transport | Protocols & Codecs |
| :--- | :--- | :--- | :--- |
| **1001** | **Mobile App** (iOS `SipPhone` / Android) | UDP / TCP (`5060`) | Standard SIP, RTP, H.264 Video + Opus / PCMU / PCMA |
| **1002** | **Web Test Client** (`web/index.html`) | WebSocket (`ws://:8088/ws`) | WebRTC, DTLS-SRTP, ICE, H.264 & VP8 Video |
| **1003** | **macOS App** / Linphone | UDP / TCP (`5060`) | Standard SIP, RTP, H.264 Video |
| **1004** | **Windows App** (WPF) | UDP / TCP (`5060`) | Standard SIP, RTP, H.264 Video |
| **1005** | **Secondary Device** | UDP / TCP (`5060`) | Standard SIP, RTP, H.264 Video |
| **9001** | **Echo Test** | Any | Two-way Audio + H.264 Video Echo Test |

---

## Verification Commands

Run the automated test suites across platforms:

```bash
# iOS Platform SDK & Tests:
bash scripts/verify-spm-video-sdk.sh      # SwiftPM package build across architectures
bash scripts/verify-ios-video-tests.sh    # Simulator unit & Asterisk PBX video integration tests
bash scripts/verify-xcode-app-build.sh    # SipPhone sample app build for simulator & device
bash scripts/verify-sim-app-launch.sh     # Launch SipPhone on booted simulator & screenshot

# Desktop & macOS Platform:
bash scripts/verify-desktop-sdk.sh        # C++ core CTest suite & CLI against Asterisk
bash scripts/verify-macos-gui.sh          # Native macOS SwiftUI app build & live registration

# Android Platform:
bash scripts/verify-android-sdk.sh        # Android AAR, slices, registration & call tests

# Windows Platform:
bash scripts/verify-windows-sdk.sh        # .NET / C# SDK, Media Foundation & WPF sample
```
