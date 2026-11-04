# SipSdk — Multi-Platform Native SIP Video & Audio SDK

A high-performance, enterprise-grade multi-platform SIP Video and Audio SDK built around a portable C++20 core engine (`core/` / `core/include/sip_abi.h`) and modern platform-native implementations powered by [Baresip](https://github.com/baresip/baresip) v4.12.0 and [libre](https://github.com/baresip/re) v4.12.0.

`SipSdk` delivers platform-native bindings, hardware-accelerated H.264 video pipelines, and production-ready sample applications across **iOS**, **Android**, **macOS**, **Windows**, **Linux**, and the **Web**.

---

## Architectural Blueprint

The SDK decouples application code and platform UI from VoIP protocol complexity. A shared C++20 engine manages background event loops, SIP registrations, digest authentication, call state machines, and media streaming. Native platform wrappers bind to a flat C ABI (`sip_abi.h`) with zero marshalling overhead.

```
┌────────────────────────────────────────────────────────────────────────────────────────┐
│                                Application & UI Layer                                  │
├─────────────────┬──────────────────┬─────────────────┬─────────────────┬───────────────┤
│    iOS App      │   Android App    │    macOS App    │   Windows App   │  Linux / CLI  │
│  (SwiftUI /     │ (Jetpack Compose │ (SwiftUI Native │   (WPF .NET 8   │ (Headless C++ │
│    UIKit)       │   / Material3)   │   Desktop)      │      XAML)      │   / sip_cli)  │
├─────────────────┴──────────────────┴─────────────────┴─────────────────┴───────────────┤
│                             Native Language Bindings                                   │
├─────────────────┬──────────────────┬─────────────────┬─────────────────┬───────────────┤
│   Swift 5.9/6   │  Kotlin / Java   │   Objective-C++ │    C# / .NET 8  │  Modern C++20 │
│  (@Observable,  │   (JNI Bridge,   │    (AppKit,     │   (P/Invoke,    │  (IClient-    │
│    CallKit)     │  SipSurfaceView) │   SipMacClient) │  SipClient.cs)  │   Listener)   │
├─────────────────┴──────────────────┴─────────────────┴─────────────────┴───────────────┤
│                         Unified Flat C ABI (sip_abi.h)                                 │
│        sip_client_create / sip_client_init / sip_client_register_account               │
│        sip_call_make / sip_call_answer / sip_call_hangup / sip_call_send_dtmf          │
├────────────────────────────────────────────────────────────────────────────────────────┤
│                          Portable C++20 Core Engine                                    │
│   - Engine Lifecycle & Threaded Worker Loop (engine.cpp)                               │
│   - Call State Machine, Hold/Mute & SDP Negotiation (call_session.cpp)                 │
│   - Pluggable Video & Audio Drivers (media_driver.cpp)                                 │
│   - RTCP PLI/FIR Packet-Loss Recovery & Re-INVITE Stream Preservation                  │
├────────────────────────────────────────────────────────────────────────────────────────┤
│                   Baresip v4.12.0 + libre v4.12.0 Stack Core                           │
│   SIP Signaling, Digest Auth, SDP O/A, RTP/RTCP, STUN / TURN / ICE, Jitter Buffer      │
├────────────────────────────────────────────────────────────────────────────────────────┤
│                    Hardware Media Drivers & Audio Subsystems                           │
│  Apple VideoToolbox  │  Android MediaCodec  │  Windows Media Foundation  │  Linux V4L2 │
│    AVAudioSession    │    OpenSL / AAudio   │       WASAPI Audio         │  ALSA/Pulse │
└──────────────────────┴──────────────────────┴────────────────────────────┴─────────────┘
```

---

## Supported Platforms & Capabilities

| Platform | Minimum OS | Language & Framework | Hardware Video Codec | Audio Subsystem | Native Video Component | Sample Application |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **iOS** | iOS 17.0+ | Swift 5.9/6 (`@Observable`, `@MainActor`) | Apple VideoToolbox (H.264) | `AVAudioSession` (`VoiceChat`) | `SipVideoView` (`CALayer` YUV420P) | `platforms/ios/sample/SipPhone` |
| **Android** | API 24+ (Android 7.0+) | Kotlin / Java (NDK JNI) | Android MediaCodec (H.264) | OpenSL ES / AAudio | `SipSurfaceView` (`SurfaceView`) | `platforms/android/sample` |
| **macOS** | macOS 14.0+ | Swift 5.9/6 & Objective-C++ | Apple VideoToolbox (H.264) | CoreAudio | `SipMacVideoView` (`CALayer`) | `platforms/macos/sample/SipSdkMac` |
| **Windows** | Windows 10/11 | C# / .NET 8 & P/Invoke | Media Foundation (H.264) | WASAPI Audio | Direct Image / Pixel Buffer | `platforms/windows/sample/SipSdk.WpfSample` |
| **Desktop / Linux** | Ubuntu 22.04+ / macOS | Portable C++20 (CMake) | Software / V4L2 Camera | PulseAudio / ALSA | Direct YUV420P Frame Callback | `platforms/desktop/src/cli` (`sip_cli`) |
| **Web** | Modern Browsers | JavaScript / TypeScript | WebRTC H.264 / VP8 | Web Audio API | HTML5 `<video>` Elements | `web/index.html` (JsSIP v3.13.9) |

---

## Repository Layout

```
├── core/                               # Portable C++20 core engine & flat C ABI
│   ├── include/sip_abi.h               # Stable C ABI for all platform FFI bindings
│   ├── src/engine.cpp                  # Background worker loop & account registration
│   ├── src/call_session.cpp            # Call state machine, SDP negotiation, DTMF
│   ├── src/media_driver.cpp            # Pluggable hardware video & audio drivers
│   └── src/sip_abi.cpp                 # C ABI implementation routing into C++ core
├── platforms/
│   ├── ios/                            # iOS Platform Target (iOS 17.0+)
│   │   ├── Sources/SipSdk/             # Modern Swift SDK (@Observable, CallKit, PushKit)
│   │   ├── Tests/SipSdkTests/          # Automated XCTest suite & PBX integration tests
│   │   ├── sample/SipPhone/            # SwiftUI sample phone app (SipPhone.xcodeproj)
│   │   └── README.md                   # Detailed iOS integration documentation
│   ├── android/                        # Android Platform Target (API 24+)
│   │   ├── sipsdk/                     # Android AAR library module (Kotlin, JNI, MediaCodec)
│   │   └── sample/                     # Modern Jetpack Compose sample application
│   ├── macos/                          # macOS Desktop Target (macOS 14.0+)
│   │   └── sample/SipSdkMac/           # Native SwiftUI desktop phone app (SipSdkMac.xcodeproj)
│   ├── windows/                        # Windows Platform Target (.NET 8 & C++)
│   │   ├── dotnet/                     # C# / .NET 8 class library (SipSdk.sln)
│   │   ├── src/                        # Media Foundation H.264 encoder/decoder drivers
│   │   └── sample/SipSdk.WpfSample/    # Modern WPF Video Phone sample app
│   └── desktop/                        # Desktop Native / Linux Target (C++20)
│       ├── include/sip/                # Desktop C++ SDK headers
│       ├── src/cli/                    # Headless CLI phone tool (sip_cli)
│       ├── tests/                      # Desktop CTest automated test suite
│       └── docker/                     # Linux Docker container verification harness
├── docker/asterisk/                    # Local Asterisk 22 Video PBX for automated testing
├── scripts/                            # Multi-platform build and verification scripts
├── web/                                # JsSIP WebRTC companion test page
└── Package.swift                       # SwiftPM manifest for iOS SipSdk, SipCore & BaresipCore
```

---

## Platform Quick Starts

### 1. iOS (`platforms/ios`)

The iOS SDK provides modern Swift Concurrency, observation, and turnkey CallKit integration:

```swift
import SwiftUI
import SipSdk

@MainActor
final class PhoneViewModel: ObservableObject {
    let client = SipClient()

    func initializeAndRegister() async throws {
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

    func makeVideoCall(to targetUri: String) async throws -> SipCall {
        return try await client.makeCall(to: targetUri, withVideo: true)
    }
}
```

Video rendering in SwiftUI:

```swift
struct CallView: View {
    @State private var remoteVideoView = SipVideoUIView()
    @State private var localVideoView = SipVideoUIView()

    var body: some View {
        ZStack {
            SipVideoView(videoUIView: remoteVideoView)
                .ignoresSafeArea()
            VStack {
                HStack {
                    Spacer()
                    SipVideoView(videoUIView: localVideoView)
                        .frame(width: 110, height: 150)
                        .cornerRadius(12)
                }
                Spacer()
            }
            .padding()
        }
    }
}
```

See [platforms/ios/README.md](platforms/ios/README.md) for CallKit setup, PushKit VoIP background wake-up, and zero-downtime network handover (`resetTransports`).

---

### 2. Android (`platforms/android`)

The Android SDK packages into `sipsdk.aar` with high-performance `MediaCodec` hardware video encoding/decoding and `SurfaceView` rendering:

```kotlin
import com.sipsdk.SipClient
import com.sipsdk.SipClientListener
import com.sipsdk.SipCall
import com.sipsdk.SipCallState
import com.sipsdk.SipRegistrationState

val client = SipClient(username = "1001", domain = "10.0.2.2", context = context).apply {
    password = "test1001"
    transport = "udp"
    port = 5060
}

client.listener = object : SipClientListener {
    override fun onRegistrationStateChanged(state: SipRegistrationState, code: Int, reason: String) {
        println("Registration: $state ($code $reason)")
    }

    override fun onIncomingCall(call: SipCall) {
        call.answer(withVideo = true)
    }

    override fun onCallStateChanged(call: SipCall, state: SipCallState, code: Int, reason: String) {
        println("Call state: $state")
    }
}

// Start engine and register
client.start()
client.register()

// Make an outgoing video call
val call = client.makeCall(targetUri = "sip:9001@10.0.2.2:5060", withVideo = true)
```

Attach video rendering surfaces:

```kotlin
// Remote video rendering surface
client.remoteVideoView = findViewById<SipSurfaceView>(R.id.remoteSurface)

// Local camera preview surface
client.localVideoView = findViewById<SipSurfaceView>(R.id.localSurface)
client.startLocalPreview()
```

---

### 3. Windows (.NET 8 / C#) (`platforms/windows`)

Idiomatic C# bindings wrapping `sip_abi.h` with Windows Media Foundation hardware video acceleration:

```csharp
using SipSdk;

using var client = new SipClient();

client.RegistrationStateChanged += (sender, e) => {
    Console.WriteLine($"Registration: {e.State} ({e.Code} {e.Reason})");
};

client.IncomingCall += (sender, e) => {
    Console.WriteLine($"Incoming call from: {e.Call.RemoteUri}");
    e.Call.Answer(withVideo: true);
};

// Initialize engine and register account
client.Start();
client.Register("1004", "127.0.0.1", "test1004", transport: "udp", port: 5060);

// Place an outgoing call
var call = client.MakeCall("sip:9001@127.0.0.1:5060", withVideo: true);
call.StateChanged += (sender, e) => {
    Console.WriteLine($"Call state: {e.State}");
};
```

---

### 4. macOS Native (`platforms/macos`)

A native macOS SwiftUI application powered by `AVCaptureSession`, `VideoToolbox`, and `CoreAudio`:

- Located at `platforms/macos/sample/SipSdkMac.xcodeproj`
- Full dialer keypad, live camera device selection, active in-call window, DTMF transmission, and call history.
- Built and verified via `bash scripts/verify-macos-gui.sh`.

---

### 5. Desktop Native & Linux CLI (`platforms/desktop`)

Portable modern C++20 library and headless command-line client:

```cpp
#include <sip/desktop.hpp>

auto client = std::make_shared<sip::Client>("1003", "127.0.0.1");
client->set_password("test1003");
client->set_listener(std::make_shared<MyClientListener>());

client->start();
auto call = client->make_call("sip:9001@127.0.0.1:5060", /*with_video=*/true);
```

Using the headless `sip_cli` tool:

```bash
# Register against PBX and initiate a 5-second video echo call with synthetic camera:
./.build/desktop/sip_cli \
  --user 1003 \
  --domain 127.0.0.1 \
  --password test1003 \
  --port 5060 \
  --call sip:9001@127.0.0.1:5060 \
  --video \
  --duration 5 \
  --camera synthetic
```

---

### 6. WebRTC Companion Test Client (`web/index.html`)

For testing cross-protocol audio and H.264 video calls between native apps and browsers:

```bash
# Serve web client directory
python3 -m http.server 5101 --directory web

# Open http://localhost:5101 in any modern browser
```

The web client connects to Asterisk over WebSocket (`ws://localhost:8088/ws`) using pre-configured account `1002` / `test1002`.

---

## Local Testing PBX (Docker Asterisk 22)

To test SIP audio and H.264 video calls reliably without external servers, the repository includes an Asterisk 22 PBX container with PJSIP and WebRTC bridging.

### Quick Start

```bash
# Start PBX container (with automatic Mac/host LAN IP NAT detection):
bash scripts/run-pbx.sh

# Stop PBX container:
bash scripts/stop-pbx.sh

# Verify PBX health, UDP registration (1001), and video echo call (9001):
bash scripts/verify-pbx.sh
```

### Pre-Configured Extensions (Passwords: `test<ext>`)

| Extension | Target Client | Transport | Protocols & Codecs |
| :--- | :--- | :--- | :--- |
| **1001** | **Mobile App** (iOS `SipPhone` / Android) | UDP / TCP (`5060`) | Standard SIP, RTP, H.264 Video + Opus / PCMU / PCMA |
| **1002** | **Web Test Client** (`web/index.html`) | WebSocket (`ws://:8088/ws`) | WebRTC, DTLS-SRTP, ICE, H.264 & VP8 Video |
| **1003** | **macOS App** / Linphone / CLI | UDP / TCP (`5060`) | Standard SIP, RTP, H.264 Video |
| **1004** | **Windows App** (WPF / .NET) | UDP / TCP (`5060`) | Standard SIP, RTP, H.264 Video |
| **1005** | **Secondary Device** | UDP / TCP (`5060`) | Standard SIP, RTP, H.264 Video |
| **9001** | **Echo Test** | Any Transport | Two-way Audio + H.264 Video Echo Test |

---

## Automated Verification Harnesses

Run the automated test suites across all platforms:

```bash
# iOS Platform SDK & Tests:
bash scripts/verify-spm-video-sdk.sh      # SwiftPM package build across slices (sim arm64/x86_64, device arm64)
bash scripts/verify-ios-video-tests.sh    # Simulator unit & Asterisk PBX video integration tests
bash scripts/verify-xcode-app-build.sh    # SipPhone sample app build for simulator & device
bash scripts/verify-sim-app-launch.sh     # Launch SipPhone on booted iOS Simulator & verify screenshot

# Android Platform SDK:
bash scripts/verify-android-sdk.sh        # Build sipsdk.aar, verify ABIs, registration & call tests

# macOS Platform:
bash scripts/verify-macos-gui.sh          # Build SwiftUI macOS app & verify live echo call with screenshot

# Windows Platform (.NET 8 & C++):
bash scripts/verify-windows-sdk.sh        # Build C ABI DLL, Media Foundation tests, .NET SDK & unit tests

# Desktop Native & Linux CI:
bash scripts/verify-desktop-sdk.sh        # Desktop C++ CTest suite, CLI against Asterisk, Linux Docker CI

# Local Asterisk PBX:
bash scripts/verify-pbx.sh                # Test Asterisk UDP & WebSocket endpoints and echo dialplan
```

---

## Core C ABI Reference (`core/include/sip_abi.h`)

All native wrappers communicate with the shared C++ engine through a thread-safe C ABI. Key functions include:

- **Lifecycle**: `sip_client_create()`, `sip_client_init()`, `sip_client_destroy()`
- **Registration**: `sip_client_register()`, `sip_client_register_account()`, `sip_client_unregister()`, `sip_client_is_registered()`
- **Call Session**: `sip_call_make()`, `sip_call_answer()`, `sip_call_hangup()`, `sip_call_set_hold()`, `sip_call_is_on_hold()`
- **Media Controls**: `sip_client_set_mic_muted()`, `sip_client_is_mic_muted()`, `sip_call_send_dtmf()`, `sip_client_switch_camera()`, `sip_client_set_camera()`
- **Custom Drivers**: `sip_client_set_video_capture_driver()`, `sip_client_set_video_renderer_driver()`, `sip_client_set_audio_driver()`

---

## License

This project is licensed under the Apache License 2.0.
