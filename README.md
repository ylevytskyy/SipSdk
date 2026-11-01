# SipSdk

An iOS SIP SDK and demo application built around the modular [Baresip](https://github.com/baresip/baresip) engine.

The repository is in a staged migration from the legacy `taresip 0.1.1` CocoaPods integration to a version-pinned Baresip v4 XCFramework distributed through Swift Package Manager (SwiftPM). The modern package is defined in `Package.swift`; the existing demo app remains on the legacy integration until the new adapter passes device-level SIP and audio validation.

## Architecture

### Modern SwiftPM package

```text
Swift / Objective-C application
             |
             v
      SipSdk public API
   Objective-C++ Baresip v4 adapter
             |
             v
       BaresipCore XCFramework
       (Baresip + libre, static)
```

The `SipSdk` SwiftPM product includes the Objective-C++ adapter, the pinned BaresipCore binary dependency, and a small resource target that copies `config.baresip-v4` into the app.

### Legacy demo (kept for compatibility)

```text
Swift UIKit demo
       |
       v
Legacy Objective-C++ wrapper (SipClient.mm)
       |
       v
taresip 0.1.1 CocoaPod
```

The demo is intentionally not switched to the new adapter yet. Keeping the known legacy build available provides a rollback path while SIP behavior is validated on real devices.

## Requirements

- macOS with Xcode and the iOS SDK.
- Swift tools 5.9 or newer.
- For the legacy demo only: Ruby and CocoaPods.
- A SIP account and server that you are authorized to use for testing.

The package retains an iOS deployment target of 12.1. The demo targets iOS 15.0, the lowest deployment target current Xcode accepts. On Apple Silicon the demo's simulator build requires `ARCHS=x86_64` because the `taresip` pod ships no arm64-simulator slice.

## Build the SwiftPM package

From the repository root on macOS:

```bash
swift package dump-package
swift package resolve
```

Open `Package.swift` in Xcode to build the `SipSdk` package for a generic iOS device or iOS Simulator destination.

The first resolution downloads the versioned XCFramework from the GitHub Release. SwiftPM verifies the archive against the checksum in `Package.swift`. The release contains static slices for iOS device arm64 and iOS Simulator arm64/x86_64.

To regenerate the manifest from the published release rather than editing the checksum manually:

```bash
bash scripts/generate-spm-package.sh 4.12.0
```

The script downloads the release checksum sidecar and archive, independently verifies SHA-256, checks the ZIP contents and license files, and then writes `Package.swift`.

## Build the legacy demo

The current `SipSdk` Xcode project still uses CocoaPods:

```bash
gem install cocoapods --version 1.17.0
pod install --deployment --repo-update
open SipSdk.xcworkspace
```

Configure a test SIP account in `SipSdk/SipCallsViewController.swift` before running the app. Do not commit real account credentials. The bundled legacy `SipSdk/config` file contains Baresip module and audio settings; it is not a secrets store.

## Local Testing PBX (Docker Asterisk 22)

To test SIP audio and H.264 video calls reliably without relying on public servers like `iptel.org`, the repository includes a self-contained, lightweight Docker setup running the latest Asterisk 22 PBX with PJSIP and WebRTC support.

Asterisk automatically bridges standard SIP RTP (native iOS mobile app, Linphone, Jami) and WebRTC DTLS-SRTP (web test page), with H.264 video passthrough.

### Quick Start

Start the PBX container (automatically detects your Mac's LAN IP for physical device testing):

```bash
bash scripts/run-pbx.sh
```

Or using Docker Compose directly:

```bash
docker compose up -d
```

### Stop the PBX

```bash
bash scripts/stop-pbx.sh
# or
docker compose down
```

### Automated Verification

Verify PBX health, UDP registration, WebSocket registration, and video echo call:

```bash
bash scripts/verify-pbx.sh
```

### Pre-Configured Extensions

All passwords are set to `test<extension>`:

| Extension | Target Client | Transport | Protocols & Codecs |
| :--- | :--- | :--- | :--- |
| **1001** | **Mobile App** (`SipSdk` iOS) | UDP / TCP (port `5060`) | Standard SIP, Plain RTP, H.264 Video + Opus / PCMU / PCMA |
| **1002** | **Web Test Client** (`web/index.html`) | WebSocket (`ws://:8088/ws` or `wss://:8089/ws`) | WebRTC, DTLS-SRTP, ICE, H.264 & VP8 Video + Opus / PCMU / PCMA |
| **1003** | **Linphone** Softphone | UDP / TCP (port `5060`) | Standard SIP, Plain RTP, H.264 & VP8 Video |
| **1004** | **Jami** Softphone | UDP / TCP (port `5060`) | Standard SIP, Plain RTP, H.264 & VP8 Video |
| **1005** | **Extra Softphone** (MicroSIP / Zoiper / 2nd device) | UDP / TCP (port `5060`) | Standard SIP, Plain RTP, H.264 & VP8 Video |

### Test Numbers (Dial from Any Client)

- **`9001` or `echo`**: **Two-way Audio + H.264 Video Echo Test**. Automatically answers and reflects both your video stream and audio back to your screen. Great for single-device camera verification!
- **`9002` or `music`**: Music on Hold test stream.
- **Dial any extension (`1001`, `1002`, `1003`, `1004`, `1005`)**: Connect a two-way video call between any two clients (e.g. iPhone 11 Pro ↔ Web Client, or iPhone 11 Pro ↔ Linphone).

---

### Client Configuration Guides

#### 1. iOS Mobile App (`SipSdk` on physical iPhone or Simulator)
1. Open the app on your device or simulator.
2. Tap the **Account Settings** (gear icon) in the top right.
3. Enter:
   - **Username**: `1001`
   - **Password**: `test1001`
   - **Domain**: Your Mac's Wi-Fi LAN IP (e.g. `192.168.0.x`), or `127.0.0.1` if running on Simulator.
   - **Port**: `5060`
   - **Transport**: `UDP`
4. Tap **Save & Register**.
5. Dial `9001` to test video echo, or `1002` to call the web client.

#### 2. Web Test Client (`web/index.html`)
1. Serve the web page:
   ```bash
   python3 -m http.server 5101 --directory web
   ```
2. Open `http://localhost:5101/` in Chrome or Safari.
3. Click the preset button: **`Local Asterisk (1002)`**.
4. Click **Register** (shows `Registered as sip:1002@127.0.0.1`).
5. Click `9001 (Echo Test)` or enter `1001` and click **Call** to start a two-way video call.

#### 3. Linphone Softphone
1. Open Linphone (Desktop or Mobile) -> **Preferences** -> **Manage Accounts** -> **Add Account**.
2. Select **Use SIP Account**:
   - **Username**: `1003`
   - **SIP Domain**: `<Your-Mac-LAN-IP>:5060`
   - **Password**: `test1003`
   - **Transport**: `UDP`
3. Under **Video Settings**, enable **Video**, **Automatically accept video requests**, and ensure **H264** or **VP8** is checked.
4. Call `1001` to video call your iPhone or `9001` for echo.

#### 4. Jami Softphone
1. In Jami -> **Settings** -> **Add an account** -> **Advanced (SIP)**.
2. Enter:
   - **Alias**: `Jami Test`
   - **Hostname**: `<Your-Mac-LAN-IP>`
   - **Username**: `1004`
   - **Password**: `test1004`
3. In **Advanced Account Settings**, set **Registration expiry**: `300`, **Video codecs**: `H264, VP8`.
4. Call `1001` (iOS app) or `1002` (web app).

---

## Current status and limitations

- The Baresip v4.12.0 XCFramework release is published and pinned by the SwiftPM manifest.
- CI builds and compile/links the modern adapter for iOS device arm64 and iOS Simulator arm64/x86_64.
- The legacy demo remains on `taresip 0.1.1`; migration of the Xcode project is deliberately deferred.
- Real SIP registration, incoming/outgoing calls, two-way audio, DTMF, repeated start/stop and network transitions still require device-level testing against a controlled SIP server.
- TLS is disabled in the current native build (`USE_OPENSSL=OFF`), so SIP over TLS is not supported by this release.
- A green compile/link job does not prove SIP signaling or media behavior.

See [docs/SPM_MIGRATION.md](docs/SPM_MIGRATION.md) for migration details and the remaining validation checklist.

## CI

The repository has separate legacy app-build and modern Baresip XCFramework workflows. The modern workflow builds the pinned native dependency, checks the Objective-C++ adapter against device and Simulator architectures, and validates SwiftPM manifest resolution and the release binary checksum. These checks are compile/package validation only; runtime SIP and audio tests remain outstanding.
