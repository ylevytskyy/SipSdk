# Plan: Full-Fledged macOS GUI Sample (`SipSdkMac`) (`macos-gui-sample`)

**Goal**: Deliver a turnkey, production-grade native macOS SwiftUI application housed in `platforms/macos/sample/SipSdkMac.xcodeproj` that provides full SIP telephony controls, dialpad, account settings, call history, and hardware-accelerated remote/local video rendering backed by `sip_core` and the C ABI, verified with automated build and runtime screenshot scripts (`scripts/verify-macos-gui.sh`).

## Global Constraints

- **Platform Target**: macOS 13.0+ (Ventura and newer, `arm64`).
- **UI Framework**: Native SwiftUI with AppKit bridges (`NSViewRepresentable`).
- **Engine Bridging**: Objective-C++ adapter layer (`SipMacClient`, `SipMacCall`, `SipMacVideoView`) wrapping `sip_abi.h` / `core`.
- **Prebuilts & Linkage**: Link against macOS static libraries in `platforms/desktop/prebuilt/darwin-arm64/lib/` (`libbaresip.a`, `libre.a`) and `libsipcore.a`.
- **System Frameworks**: `Foundation`, `AppKit`, `AVFoundation`, `CoreMedia`, `CoreVideo`, `CoreAudio`, `AudioToolbox`, `VideoToolbox`, `QuartzCore`, `SystemConfiguration`, `CFNetwork`.
- **ABI & Socket Invariant**: `-DUSE_UNIXSOCK=ON` strictly enforced on all targets to guarantee `struct sa` and `struct sip_msg` layout compatibility.
- **Sandboxing & Permissions**: App entitlements and `Info.plist` with `NSCameraUsageDescription` and `NSMicrophoneUsageDescription`.
- **Zero Regression**: Must preserve all existing mobile and desktop targets and pass all test suites.

---

## Dependency Graph

```
+-----------------------------------------------------------------------------------+
| Slice 1: Objective-C++ Engine Adapter (SipMacClient, SipMacCall, SipMacVideoView) |
| platforms/macos/sample/SipSdkMac/Bridge/                                          |
+-----------------------------------------------------------------------------------+
                                    |
                                    v
+-----------------------------------------------------------------------------------+
| Slice 2: SwiftUI Models & State Management (SipAppModel, AccountStore, History)   |
| platforms/macos/sample/SipSdkMac/Models/ & Views/Representables/                  |
+-----------------------------------------------------------------------------------+
                                    |
                                    v
+-----------------------------------------------------------------------------------+
| Slice 3: SwiftUI Desktop Views & Window Layout (Dialer, InCall, History, Settings)|
| platforms/macos/sample/SipSdkMac/Views/ & SipSdkMacApp.swift                      |
+-----------------------------------------------------------------------------------+
                                    |
                                    v
+-----------------------------------------------------------------------------------+
| Slice 4: Standalone Xcode Project Packaging (SipSdkMac.xcodeproj)                |
| platforms/macos/sample/SipSdkMac.xcodeproj, Info.plist, Entitlements              |
+-----------------------------------------------------------------------------------+
                                    |
                                    v
+-----------------------------------------------------------------------------------+
| Slice 5: Automated Verification Script & Live PBX Runtime Smoke Test              |
| scripts/verify-macos-gui.sh, desktop screenshot evidence, clean exit              |
+-----------------------------------------------------------------------------------+
```

---

### Slice 1: Objective-C++ Engine Adapter (`SipMacClient`, `SipMacCall`, `SipMacVideoView`)

- **Files**:
  - `platforms/macos/sample/SipSdkMac/Bridge/SipMacClient.h`
  - `platforms/macos/sample/SipSdkMac/Bridge/SipMacClient.mm`
  - `platforms/macos/sample/SipSdkMac/Bridge/SipMacCall.h`
  - `platforms/macos/sample/SipSdkMac/Bridge/SipMacCall.mm`
  - `platforms/macos/sample/SipSdkMac/Bridge/SipMacVideoView.h`
  - `platforms/macos/sample/SipSdkMac/Bridge/SipMacVideoView.mm`
  - `platforms/macos/sample/SipSdkMac/Bridge/SipSdkMac-Bridging-Header.h`
- **Interfaces**:
  - `SipMacClient`:
    - `initWithUsername:domain:`
    - `startWithConfigPath: -> int`, `stop`
    - `registerWithPassword:port:transport: -> int`, `unregisterAccount -> int`
    - `makeCallToUri:withVideo: -> SipMacCall *`
    - `setRemoteVideoView:`, `setLocalPreviewView:`
    - `useSyntheticCamera:`, `availableCameras -> NSArray<NSString *>`, `selectCameraByName:`
    - `delegate: id<SipMacClientDelegate>`
  - `SipMacCall`:
    - `callId`, `remoteUri`, `hasVideo`, `isOutgoing`, `isOnHold`, `isClosed`, `duration`
    - `answerWithVideo:`, `hangupWithCode:reason:`, `setHold:`, `sendDtmf:`, `requestKeyframe`
  - `SipMacVideoView`:
    - `NSView` subclass with `wantsLayer = YES`, `renderYUV420WithY:...` converting to `CGImage` assigned to `layer.contents`, `clear`.
- **Acceptance Criteria**:
  - Given valid credentials, When `startWithConfigPath:` and `registerWithPassword:` are called, Then registration callback fires with `SipMacRegistrationStateRegistered` (code 200).
  - Given an active video call, When YUV420 frames arrive from `sip_abi`, Then `SipMacVideoView` renders them to `layer.contents` and updates `renderedFrameCount`.
- **Verify**: Compile bridge files with clang++ test runner linking `libsipcore.a`, `libbaresip.a`, `libre.a`.
- **Blocked by**: None

---

### Slice 2: SwiftUI Models & State Management

- **Files**:
  - `platforms/macos/sample/SipSdkMac/Models/SipAccountStore.swift`
  - `platforms/macos/sample/SipSdkMac/Models/SipCallHistoryStore.swift`
  - `platforms/macos/sample/SipSdkMac/Models/SipAppModel.swift`
  - `platforms/macos/sample/SipSdkMac/Views/SipVideoViewRepresentable.swift`
- **Interfaces**:
  - `SipAccountStore`: Loads/persists `username`, `domain`, `password`, `port`, `transport` to `UserDefaults`, pre-populating with `1001@127.0.0.1:5060`.
  - `SipCallHistoryStore`: Loads/appends `CallRecord` (id, remoteUri, direction, duration, timestamp, reason) to `UserDefaults`.
  - `SipAppModel: ObservableObject`: Exposes `registrationState`, `activeCall`, `callDurationText`, `isMuted`, `isOnHold`, `availableCameras`, `selectedCamera`, `incomingCall`.
  - `SipVideoViewRepresentable: NSViewRepresentable`: Bridges `SipMacVideoView` to SwiftUI views with resize gravity and clear hooks.
- **Acceptance Criteria**:
  - Given account credentials in `UserDefaults`, When `SipAppModel` initializes, Then it registers callbacks and attempts registration with `SipMacClient`.
  - When an incoming or outgoing call connects, `SipAppModel.activeCall` updates, triggering view transitions and starting the duration timer.
- **Verify**: Swift compiler syntax and unit tests via `swiftc`.
- **Blocked by**: Slice 1

---

### Slice 3: SwiftUI Desktop Views & Window Layout

- **Files**:
  - `platforms/macos/sample/SipSdkMac/Views/MainSplitView.swift`
  - `platforms/macos/sample/SipSdkMac/Views/DialerView.swift`
  - `platforms/macos/sample/SipSdkMac/Views/InCallView.swift`
  - `platforms/macos/sample/SipSdkMac/Views/IncomingCallBanner.swift`
  - `platforms/macos/sample/SipSdkMac/Views/CallHistoryView.swift`
  - `platforms/macos/sample/SipSdkMac/Views/AccountSettingsView.swift`
  - `platforms/macos/sample/SipSdkMac/SipSdkMacApp.swift`
- **Interfaces**:
  - `MainSplitView`: Sidebar with items (Dialer, History, Settings) + content area; when `activeCall != nil`, displays `InCallView`.
  - `DialerView`: Numeric dialpad grid (0-9, *, #), backspace, URI text field, Audio Call button, Video Call button, registration badge.
  - `InCallView`: Remote video view (`SipVideoViewRepresentable`), local PIP preview, call duration, Mute, Hold, Keypad, Hangup buttons.
  - `AccountSettingsView`: Text fields for SIP account parameters, Camera picker, Register / Unregister buttons.
  - `CallHistoryView`: List of past calls with redial button and clear history button.
  - `SipSdkMacApp: App`: Standard macOS SwiftUI App scene (`WindowGroup`).
- **Acceptance Criteria**:
  - Given the app is running, clicking dialpad numbers updates the URI field.
  - Starting a call switches to `InCallView` showing call controls and video surface.
  - Incoming call displays an alert banner with Answer / Decline buttons.
- **Verify**: Swift compilation of view components.
- **Blocked by**: Slice 2

---

### Slice 4: Standalone Xcode Project Packaging (`SipSdkMac.xcodeproj`)

- **Files**:
  - `platforms/macos/sample/SipSdkMac.xcodeproj/project.pbxproj`
  - `platforms/macos/sample/SipSdkMac/Info.plist`
  - `platforms/macos/sample/SipSdkMac/SipSdkMac.entitlements`
- **Configuration**:
  - Target: `SipSdkMac` (macOS Application, Deployment Target 13.0, `arm64`).
  - Frameworks: `Foundation`, `AppKit`, `AVFoundation`, `CoreMedia`, `CoreVideo`, `CoreAudio`, `AudioToolbox`, `VideoToolbox`, `QuartzCore`, `Security`, `SystemConfiguration`, `CFNetwork`.
  - Static libraries: `libsipcore.a`, `libbaresip.a`, `libre.a`.
  - Header search paths: `core/include`, `platforms/desktop/prebuilt/include`, `platforms/desktop/prebuilt/include/re`, `platforms/desktop/prebuilt/include/baresip`.
  - Preprocessor macros: `USE_UNIXSOCK=1`.
  - Entitlements: `com.apple.security.device.camera = YES`, `com.apple.security.device.microphone = YES`, `com.apple.security.network.client = YES`, `com.apple.security.network.server = YES`.
- **Acceptance Criteria**:
  - Given macOS host, When `xcodebuild -project platforms/macos/sample/SipSdkMac.xcodeproj -scheme SipSdkMac -configuration Release build` is executed, Then the build finishes with `** BUILD SUCCEEDED **` and generates `SipSdkMac.app`.
- **Verify**: `xcodebuild -project platforms/macos/sample/SipSdkMac.xcodeproj -scheme SipSdkMac build`
- **Blocked by**: Slice 1, Slice 2, Slice 3

---

### Slice 5: Automated Verification Script & Live PBX Runtime Smoke Test

- **Files**:
  - `scripts/verify-macos-gui.sh`
  - `specs/macos-gui-sample/evidence/macos-gui-launch.png`
- **Script Workflow**:
  1. Checks and starts Asterisk PBX if needed.
  2. Compiles `libsipcore.a` (CMake) and `SipSdkMac.xcodeproj` (`xcodebuild`).
  3. Launches `SipSdkMac.app` on macOS.
  4. Verifies app is running, waits for Asterisk SIP registration (1001@127.0.0.1:5060).
  5. Captures window screenshot using `screencapture` to `specs/macos-gui-sample/evidence/macos-gui-launch.png`.
  6. Terminates the running app cleanly.
  7. Exits code 0 on success.
- **Acceptance Criteria**:
  - Given `bash scripts/verify-macos-gui.sh` is executed, Then it compiles cleanly, launches `SipSdkMac.app`, registers to Asterisk, captures screenshot evidence, and terminates without error.
- **Verify**: `bash scripts/verify-macos-gui.sh`
- **Blocked by**: Slice 4
