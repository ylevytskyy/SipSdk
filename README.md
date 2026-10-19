# SipSdk (TareSIP Demo for iOS)

> Objective-C++ & Swift VoIP Client SDK powered by the BareSIP modular SIP engine.

`SipSdk` provides an easy-to-use iOS integration layer for BareSIP. It bridges native C SIP/SDP/RTP/audio-driver internals into clean Objective-C and Swift abstractions, enabling background VoIP calls, session management, and DTMF signaling.

---

## 🏛 Architecture

```
┌──────────────────────────────────────────────┐
│             Swift UI Application             │
│   (SipCallsViewController, Call Details)     │
└──────────────────────┬───────────────────────┘
                       │ Bridging Header
                       ▼
┌──────────────────────────────────────────────┐
│           Objective-C++ SIP SDK              │
│       (SipClient.mm, SipClientDelegate)      │
└──────────────────────┬───────────────────────┘
                       │ Native C FFI / CocoaPods
                       ▼
┌──────────────────────────────────────────────┐
│             BareSIP / Libre / Librem         │
│     (AudioUnit, SIP, RTP/RTCP, Codecs)       │
└──────────────────────────────────────────────┘
```

---

## 🚀 Features

- **Protocol Support:** Full SIP signaling, registration, call state machine, and bidirectional audio streaming via iOS AudioUnit.
- **Event-Driven Delegate:** Clean `SipClientDelegate` protocol covering registration events, incoming calls, ringing, established sessions, and DTMF start/end.
- **Swift Interoperability:** Clang-bridged headers allowing idiomatic Swift consumption without leaking low-level C pointers.
- **Dependency Management:** Integrated via CocoaPods (`taresip`).

---

## 📋 Delegate API Overview

```objc
@protocol SipClientDelegate <NSObject>
@optional
- (void)onDidRegister:(SipClient *)sipSdk;
- (void)onFailedRegister:(SipClient *)sipSdk;
- (void)onCallIncoming:(SipCall *)sipCall;
- (void)onCallRinging:(SipCall *)sipCall;
- (void)onCallEstablished:(SipCall *)sipCall;
- (void)onCallClosed:(SipCall *)sipCall;
- (void)onCallDtmfStart:(SipCall *)sipCall;
@end
```

---

## 🛠 Quick Start

1. Install dependencies via CocoaPods:
   ```bash
   pod install
   ```
2. Open `TareSIPDemo.xcworkspace` in Xcode.
3. Configure your SIP server credentials in `config` or via client initialization, then build and run on a physical device or simulator.
