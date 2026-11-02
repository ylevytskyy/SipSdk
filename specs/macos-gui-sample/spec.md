# Spec: Full-Fledged macOS GUI Sample (`SipSdkMac`)

## 1. Problem & Architecture Overview

### Problem
While the `SipSdk` repository provides native mobile UI applications (iOS UIKit in `SipSdk/`, Android Jetpack Compose in `platforms/android/sample/`), Windows WPF (`platforms/windows/sample/`), and a headless C++ CLI for desktop (`platforms/desktop/`), there is currently no graphical user interface (GUI) application for macOS desktop users. macOS developers need a turnkey, native macOS sample demonstrating:
1. SIP account configuration and registration status against registrars (including local Asterisk PBX).
2. Keypad dialing, URI entry, and outgoing call initiation.
3. Incoming call alerting and answering with or without video.
4. Active in-call telephony controls (Mute microphone, Hold, DTMF tone transmission, and Hangup).
5. Hardware-accelerated H.264 remote video rendering alongside local camera Picture-in-Picture (PIP) preview.
6. Call history persistence.

### Solution
Deliver `SipSdkMac`, a native SwiftUI macOS application located at `platforms/macos/sample/SipSdkMac.xcodeproj`:
1. **Dedicated Xcode Project**: Standalone project configured for macOS 13.0+ (`arm64`), linking `libsipcore`, `libbaresip.a`, and `libre.a` with macOS App Sandbox / Hardened Runtime camera and microphone entitlements.
2. **Objective-C++ Adapter Bridge**: An adapter (`SipMacClient`, `SipMacCall`, `SipMacVideoView`) wrapping `sip_abi.h` and marshaling events to the main thread with Cocoa types.
3. **Single-Window Sidebar Layout**: Modern `NavigationSplitView` architecture featuring Dialer, Call History, and Account Settings, expanding into full remote video display during active video calls.
4. **Hardware Camera with Synthetic Fallback**: Auto-detects connected webcams (FaceTime HD, USB, Continuity Camera) while falling back to 30 fps synthetic test-pattern frames on headless runners or when camera permissions are denied.
5. **Automated Verification**: Single verification script `scripts/verify-macos-gui.sh` compiling `SipSdkMac.app`, launching it on macOS, registering against local Asterisk PBX, and capturing window screenshots.

---

## 2. Public API & Component Specification

### Objective-C++ Adapter (`platforms/macos/sample/SipSdkMac/Bridge/`)

#### `SipMacClient.h`
```objc
#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>

typedef NS_ENUM(NSInteger, SipMacRegistrationState) {
    SipMacRegistrationStateUnregistered = 0,
    SipMacRegistrationStateRegistering,
    SipMacRegistrationStateRegistered,
    SipMacRegistrationStateFailed
};

typedef NS_ENUM(NSInteger, SipMacCallState) {
    SipMacCallStateIncoming = 0,
    SipMacCallStateRinging,
    SipMacCallStateConnecting,
    SipMacCallStateEstablished,
    SipMacCallStateHold,
    SipMacCallStateClosed
};

@class SipMacCall;
@class SipMacVideoView;

@protocol SipMacClientDelegate <NSObject>
@optional
- (void)client:(id)client didChangeRegistrationState:(SipMacRegistrationState)state code:(NSInteger)code reason:(NSString *)reason;
- (void)client:(id)client didReceiveIncomingCall:(SipMacCall *)call;
- (void)client:(id)client call:(SipMacCall *)call didChangeState:(SipMacCallState)state code:(NSInteger)code reason:(NSString *)reason;
- (void)client:(id)client call:(SipMacCall *)call didUpdateMediaWidth:(int)width height:(int)height;
@end

@interface SipMacClient : NSObject
@property (nonatomic, weak) id<SipMacClientDelegate> delegate;
@property (nonatomic, readonly) BOOL isStarted;
@property (nonatomic, readonly) BOOL isRegistered;
@property (nonatomic, assign) BOOL isMicMuted;

- (instancetype)initWithUsername:(NSString *)username domain:(NSString *)domain;
- (int)startWithConfigPath:(NSString *)configPath;
- (void)stop;

- (int)registerWithPassword:(NSString *)password port:(int)port transport:(NSString *)transport;
- (int)unregisterAccount;

- (SipMacCall *)makeCallToUri:(NSString *)uri withVideo:(BOOL)withVideo;
- (void)setRemoteVideoView:(SipMacVideoView *)videoView;
- (void)setLocalPreviewView:(SipMacVideoView *)previewView;
- (void)useSyntheticCamera:(BOOL)useSynthetic;
- (NSArray<NSString *> *)availableCameras;
- (void)selectCameraByName:(NSString *)cameraName;
@end

@interface SipMacCall : NSObject
@property (nonatomic, readonly, copy) NSString *callId;
@property (nonatomic, readonly, copy) NSString *remoteUri;
@property (nonatomic, readonly) BOOL hasVideo;
@property (nonatomic, readonly) BOOL isOutgoing;
@property (nonatomic, readonly) BOOL isOnHold;
@property (nonatomic, readonly) BOOL isClosed;
@property (nonatomic, readonly) uint32_t duration;

- (int)answerWithVideo:(BOOL)withVideo;
- (int)hangupWithCode:(int)code reason:(NSString *)reason;
- (int)setHold:(BOOL)hold;
- (int)sendDtmf:(NSString *)digits;
- (int)requestKeyframe;
@end

@interface SipMacVideoView : NSView
@property (nonatomic, readonly) CGSize videoSize;
@property (nonatomic, readonly) NSUInteger renderedFrameCount;
- (void)clear;
@end
```

---

## 3. Deliberately Does NOT Do

1. **Mac Catalyst**: Does not build a Catalyst target or reuse the iOS UIKit storyboard/controllers; uses pure macOS AppKit/SwiftUI.
2. **SIP over TLS / SRTP**: TLS remains disabled (`USE_OPENSSL=OFF`) in this milestone; communication uses UDP/TCP and unencrypted RTP.
3. **Opus / Third-Party Codecs**: Relies on standard built-in G.711 (PCMA/PCMU) and H.264.
4. **Cross-Platform C++ GUI (Qt / ImGui)**: Excludes third-party C++ desktop widget libraries in favor of Apple-native SwiftUI.
5. **System CallKit / macOS Continuity Phone Bridge**: System CallKit is iOS-only; macOS handles alerts via native notifications and in-app windows.

---

## 4. Alternatives Considered

1. **Adding macOS Target into Root `SipSdk.xcodeproj`**: Rejected because mixing iOS and macOS target build settings, bridging headers, and legacy CocoaPods settings creates brittle configurations and risks breaking existing iOS CI validation scripts.
2. **Cross-Platform C++ GUI (Qt 6 / Dear ImGui)**: Rejected because it introduces large third-party dependencies, breaks native macOS keyboard/accessibility conventions, and does not provide an idiomatic Apple platform sample.
3. **Pure Swift C++ Interoperability (`-cxx-interoperability-mode=default`)**: Rejected because bridging `std::shared_ptr`, C++ templates, and virtual listener callback classes directly into Swift is compiler-fragile compared to an Objective-C++ adapter.

---

## 5. Open Questions & Resolutions

- **Question**: Which UI framework should be used for the macOS GUI sample?
  - **Resolution**: Native SwiftUI / Swift macOS App.
- **Question**: Where should the project be structured and built?
  - **Resolution**: Dedicated Xcode project at `platforms/macos/sample/SipSdkMac.xcodeproj`.
- **Question**: How should Swift connect to the underlying C ABI / engine?
  - **Resolution**: Objective-C++ adapter layer (`SipMacClient`, `SipMacCall`, `SipMacVideoView`).
- **Question**: What window and navigation architecture should be used?
  - **Resolution**: Modern single-window sidebar (`NavigationSplitView`) with full-screen in-call transition.
- **Question**: How should webcam and video capture be handled?
  - **Resolution**: Auto-detect hardware cameras with dynamic fallback to synthetic test pattern.
- **Question**: How should account credentials and call logs be persisted?
  - **Resolution**: `UserDefaults` with pre-filled Asterisk PBX defaults (`1001@127.0.0.1:5060`).
- **Question**: How should the application be verified in CI?
  - **Resolution**: `scripts/verify-macos-gui.sh` compiling `SipSdkMac.app`, launching the process on macOS, and capturing a desktop screenshot.

---

## 6. Definition of Done

The feature is considered complete when all of the following falsifiable predicates pass:

1. `platforms/macos/sample/SipSdkMac.xcodeproj` compiles cleanly via `xcodebuild` for macOS `arm64` with zero compilation or linker errors.
2. `SipSdkMac.app` launches on macOS, displays the SwiftUI sidebar with Dialer, History, and Account Settings, and registers successfully to local Asterisk PBX (`1001@127.0.0.1:5060`).
3. Placed call to Asterisk Echo (`sip:9001@127.0.0.1:5060`) establishes bidirectional audio and H.264 video, rendering the video feed into `SipMacVideoView` with non-zero `renderedFrameCount`.
4. In-call controls (Mute, Hold, DTMF tone sending, and Hangup) operate cleanly and return the UI to the idle dialer state.
5. `scripts/verify-macos-gui.sh` executes end-to-end on macOS, verifies build and execution, captures a screenshot to `specs/macos-gui-sample/evidence/macos-gui-launch.png`, and exits with code 0.

---

## 7. Next Implementation Step

Create `specs/macos-gui-sample/plan.md` breaking this specification into tracer-bullet vertical slices.
