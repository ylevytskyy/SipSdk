# SipSdk for iOS (Apple Platform)

Native enterprise-grade SIP Video & Audio SDK for iOS 17.0+ powered by modern Swift 5.9/6, Apple VideoToolbox H.264 hardware encoding/decoding, native CallKit, VoIP PushKit, and Baresip v4.12.0 core engine.

## Architectural Overview

`SipSdk` for iOS is architected with a strict modern Swift-first interface designed for swift enterprise integration into iOS applications.

```
┌────────────────────────────────────────────────────────────────────────┐
│                        iOS Application (SwiftUI / UIKit)               │
├────────────────────────────────────────────────────────────────────────┤
│                           SipSdk (Swift)                               │
│  - SipClient (@Observable, @MainActor)                                 │
│  - SipCall (@Observable, @MainActor)                                   │
│  - SipVideoView (SwiftUI UIViewRepresentable & SipVideoUIView)         │
│  - SipCallKitCoordinator (Turnkey CXProvider & CXCallController)       │
│  - SipPushRegistry (VoIP PushKit PKPushRegistry delegate)              │
│  - IosVideoCaptureDriver (AVCaptureSession + 1080p/720p/480p)          │
│  - IosVideoRendererDriver (Direct CALayer YUV420P rendering)           │
│  - IosAudioManager (AVAudioSession PlayAndRecord / VoiceChat)          │
├────────────────────────────────────────────────────────────────────────┤
│                           SipCore (C++20 ABI)                          │
│  - sip_abi.h / SipCore (Common Core Lifecycle, Transports, State)       │
├────────────────────────────────────────────────────────────────────────┤
│                       BaresipCore.xcframework                         │
│  - Baresip v4.12.0 + libre v4.12.0 (Audio/Video Engine, SDP, RTP, SIP) │
│  - Apple VideoToolbox H.264 Hardware Codec                             │
└────────────────────────────────────────────────────────────────────────┘
```

## Key Business & Technical Features

1. **Reactive Modern Swift API (`@Observable`, `@MainActor`, Swift Concurrency)**:
   - Full async/await API for client lifecycle, registrations, and call sessions.
   - Built-in Observation macro support for seamless SwiftUI binding with zero boilerplate.
   - Async event stream (`client.events`) for clean decoupled reactive handling.

2. **Native VideoToolbox H.264 Hardware Acceleration**:
   - Zero-copy hardware video encoding and decoding via Apple's `VideoToolbox` framework.
   - Ultra-low battery consumption and low latency streaming on Apple Silicon (A-series / M-series).
   - High-performance direct `CALayer` video rendering with support for mirroring and aspect-fill/fit scaling.

3. **Turnkey System CallKit Integration (`SipCallKitCoordinator`)**:
   - Built-in `CXProvider` and `CXCallController` coordinator.
   - Native iOS system incoming call lock screen, Recents integration, audio interruption handling, and Bluetooth / CarPlay compatibility out-of-the-box.

4. **VoIP Push Notifications (`SipPushRegistry`)**:
   - Built-in `PKPushRegistry` coordinator for receiving Apple VoIP push notifications.
   - Instant app wake-up on incoming calls with automatic CallKit reporting before socket reconnection.

5. **Enterprise Mobile Handover (`client.resetTransports`)**:
   - Zero-downtime network handover: re-binds UDP/TCP sockets and refreshes SIP registration when switching between Wi-Fi and Cellular networks without dropping ongoing calls or resetting the engine.

6. **Camera & Audio Session Automation**:
   - Automatic `AVAudioSession` configuration (`.playAndRecord`, `.voiceChat`, `.defaultToSpeaker`, `.allowBluetooth`).
   - Front/Back camera switching and standalone local video preview outside calls.

---

## Installation via Swift Package Manager

Add `SipSdk` to your `Package.swift`:

```swift
dependencies: [
    .package(url: "https://github.com/ylevytskyy/SipSdk.git", from: "1.0.0")
]
```

Or in Xcode:
1. File > Add Package Dependencies...
2. Enter the repository URL: `https://github.com/ylevytskyy/SipSdk.git`
3. Select `SipSdk` product for your application target.

---

## Quick Start Guide

### 1. Initialize Client & Register Account

```swift
import SwiftUI
import SipSdk

@main
struct MyApp: App {
    @State private var client = SipClient()

    var body: some Scene {
        WindowGroup {
            ContentView(client: client)
                .task {
                    do {
                        try await client.start()
                        let account = SipAccount(
                            username: "1001",
                            domain: "sip.example.com",
                            password: "securePassword",
                            transport: .udp,
                            port: 5060,
                            displayName: "Alice"
                        )
                        try await client.register(account)
                    } catch {
                        print("SIP error: \(error)")
                    }
                }
        }
    }
}
```

### 2. Make an Outgoing Video Call

```swift
do {
    let call = try await client.makeCall(to: "sip:1002@sip.example.com", withVideo: true)
    print("Call placed with ID: \(call.id)")
} catch {
    print("Failed to place call: \(error)")
}
```

### 3. Render Video Streams in SwiftUI

```swift
struct CallView: View {
    let call: SipCall
    @State private var remoteVideoView = SipVideoUIView()
    @State private var localVideoView = SipVideoUIView()

    var body: some View {
        ZStack {
            // Full-screen remote video
            SipVideoView(videoUIView: remoteVideoView)
                .ignoresSafeArea()
                .onAppear {
                    client.remoteVideoView = remoteVideoView
                }

            // Picture-in-picture local preview
            VStack {
                HStack {
                    Spacer()
                    SipVideoView(videoUIView: localVideoView)
                        .frame(width: 100, height: 140)
                        .cornerRadius(12)
                        .onAppear {
                            client.localVideoView = localVideoView
                        }
                }
                Spacer()
            }
            .padding()
        }
    }
}
```

### 4. Handle Enterprise Network Handover

When network changes occur (e.g. Wi-Fi disconnects and cellular activates):

```swift
client.resetTransports(reRegister: true)
```

---

## Sample Application (`SipPhone`)

A complete SwiftUI sample application is located at `platforms/ios/sample/SipPhone.xcodeproj`:

- **Keypad & Dialer**: Real-time DTMF feedback, customizable destination, one-touch audio/video dialing.
- **In-Call Screen**: Full-screen VideoToolbox remote video rendering, PIP local camera view, live audio route switching, mute, hold, and DTMF keypad sheet.
- **Recents**: Persistent call history with duration and timestamp tracking.
- **Settings**: Account configuration, NAT traversal (STUN / ICE), enterprise network handover demo, and real-time camera preview test.

### Running the Sample App

```bash
# Build & run on booted iOS Simulator:
bash scripts/verify-sim-app-launch.sh
```

---

## Verification & Testing

The SDK includes a comprehensive test suite covering the engine lifecycle, CallKit, PushKit, local preview, and live call integration against a Docker Asterisk PBX:

```bash
# 1. Run automated iOS Simulator test suite
bash scripts/verify-ios-video-tests.sh

# 2. Verify SwiftPM package across simulator (arm64, x86_64) and device (arm64)
bash scripts/verify-spm-video-sdk.sh

# 3. Verify sample application build for simulator and device
bash scripts/verify-xcode-app-build.sh
```
