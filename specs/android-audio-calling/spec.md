# Spec: Android Platform SDK & Audio Calling (`android-audio-calling`)

## 1. Problem & Architecture Overview

### Problem
While the portable C++17 core engine (`sip_core`) and universal flat C ABI (`sip_abi`) decouple Baresip v4 and libre from iOS-specific Objective-C++ code, Android application developers still lack an idiomatic, turnkey mobile SDK. 

Currently, there is no Android build packaging pipeline to compile Baresip v4, libre, and `sip_core` for modern Android target architectures (`arm64-v8a`, `armeabi-v7a`, `x86_64`), no distributable Android Archive (`.aar`), and no language-native Kotlin/Java interface. Consuming raw C ABI pointers (`sip_client_t*`, `sip_call_t*`) directly in Android apps exposes Java/Kotlin code to manual JNI memory management, dangerous multithreading race conditions across Java and native threads, audio subsystem crashes from improper Android `AudioManager` mode coordination, and premature call termination if audio hardware sessions are not initialized prior to media start.

### Solution
Deliver the Android Platform SDK (`platforms/android/`) providing a complete Kotlin JNI wrapper and distributable `sipsdk.aar` for audio calling:
1. **Multi-Arch Native NDK Build Pipeline**:
   - Establish Gradle 8.x + CMake NDK build toolchain in `platforms/android/` compiling `libre`, `libbaresip` (with `g711`, `opensles`, `account`, `stun`, `turn`, `ice`, `srtp`), and `sip_core` into `libsipcore.so`.
   - Produce a production-ready Android Archive (`sipsdk.aar`) packaging native slices for `arm64-v8a`, `armeabi-v7a`, and `x86_64` alongside compiled Kotlin classes and manifest permissions (`RECORD_AUDIO`, `INTERNET`, `ACCESS_NETWORK_STATE`, `MODIFY_AUDIO_SETTINGS`).
   - Enforce symbol visibility: only JNI entry points (`Java_com_sipsdk_*`) and public C ABI functions (`_sip_*`) are exported; all internal libre and C++ symbols remain hidden (`-fvisibility=hidden`).
2. **Idiomatic Kotlin Object Model (`SipClient` & `SipCall`)**:
   - Create high-level Kotlin classes mirroring the established `SipClient` and `SipCall` contracts.
   - Global client lifecycle and account management: `SipClient.init()`, `register()`, `unregister()`, `isRegistered()`, `setMuted()`, `setSpeakerphone()`, and `destroy()`.
   - Discrete call session lifecycle: `SipCall.answer()`, `hangup(statusCode, reason)`, `setHold()`, `sendDtmf()`, and duration queries.
3. **Android Main Looper / UI Thread Guarantee**:
   - Bridge JNI callback notifications (`on_reg_state`, `on_call_state`, `on_incoming_call`, `on_call_media`, `on_dtmf_digit`) to the Android UI thread via an internal `Handler(Looper.getMainLooper())` through the C core's `dispatch_to_main` callback hook.
   - Prevent multithreaded UI state corruption crashes when consuming apps manipulate Android Views directly inside SDK callbacks.
4. **Android Audio Subsystem Integration (OpenSL ES + `AudioManager`)**:
   - Route two-way G.711 (PCMA/PCMU) audio via Baresip's native `opensles` module for low-latency audio capture and playback across all Android API levels.
   - Implement `SipAudioManager` coordinating with the Android audio hardware session: activates `AudioManager.MODE_IN_COMMUNICATION` during active calls via `prepare_audio_session`, restores normal mode upon session teardown via `release_audio_session`, and manages audio routing between earpiece and speakerphone.
5. **Comprehensive Verification**:
   - Automated verification script (`scripts/verify-android-sdk.sh`) compiling `sipsdk.aar`, inspecting multi-arch native libraries, validating symbol exports, and executing automated registration and audio echo tests against local Asterisk PBX (extension 9001).

Glossary: [`../../CONTEXT.md`](../../CONTEXT.md). ADR: [`../../docs/adr/0001-cross-platform-cpp-core-c-abi.md`](../../docs/adr/0001-cross-platform-cpp-core-c-abi.md). Parent Epic: Issue #10. Issue: #15.

---

## 2. Public API Specification

### Kotlin Public API (`platforms/android/sipsdk/src/main/kotlin/com/sipsdk/`)

```kotlin
package com.sipsdk

import android.content.Context

enum class SipRegistrationState {
    UNREGISTERED,
    REGISTERING,
    REGISTERED,
    FAILED
}

enum class SipCallState {
    INCOMING,
    RINGING,
    CONNECTING,
    ESTABLISHED,
    HOLD,
    CLOSED
}

interface SipClientListener {
    fun onRegistrationStateChanged(state: SipRegistrationState, code: Int, reason: String) {}
    fun onIncomingCall(call: SipCall) {}
    fun onCallStateChanged(call: SipCall, state: SipCallState, code: Int, reason: String) {}
    fun onCallMediaChanged(call: SipCall, width: Int, height: Int, onHold: Boolean, isMuted: Boolean) {}
    fun onDtmfDigit(call: SipCall, digit: String) {}
}

class SipClient(
    val username: String,
    val domain: String,
    val context: Context? = null
) {
    var password: String? = null
    var displayName: String? = null
    var transport: String = "udp"
    var port: Int = 5060
    var listener: SipClientListener? = null

    val aor: String
    val isRegistered: Boolean
    var isMicrophoneMuted: Boolean
    var isSpeakerEnabled: Boolean

    fun start(): Int
    fun stop()
    fun register(): Int
    fun unregister(): Int
    fun makeCall(uri: String): SipCall?
    fun setMuted(muted: Boolean)
    fun setSpeakerphone(enabled: Boolean)
    fun destroy()

    companion object {
        fun getVersion(): String
    }
}

class SipCall internal constructor(
    val callId: String,
    val remoteUri: String,
    val peerDisplayName: String?,
    val hasVideo: Boolean,
    val isOutgoing: Boolean
) {
    val isOnHold: Boolean
    val isClosed: Boolean
    val durationSeconds: Long

    fun answer(): Int
    fun hangup(statusCode: Int = 0, reason: String = "Hangup"): Int
    fun setHold(hold: Boolean): Int
    fun sendDtmf(digits: String): Int
}
```

### JNI Native Boundary (`platforms/android/sipsdk/src/main/cpp/sip_jni.cpp`)

Exports standard JNI function signatures with default visibility:
- `Java_com_sipsdk_SipClient_nativeCreate(JNIEnv *, jobject, jobject listener)`
- `Java_com_sipsdk_SipClient_nativeInit(JNIEnv *, jobject, jlong clientHandle, jstring configPath)`
- `Java_com_sipsdk_SipClient_nativeRegister(JNIEnv *, jobject, jlong clientHandle, jstring user, jstring domain, jstring pwd, jstring transport, jint port)`
- `Java_com_sipsdk_SipClient_nativeUnregister(JNIEnv *, jobject, jlong clientHandle)`
- `Java_com_sipsdk_SipClient_nativeIsRegistered(JNIEnv *, jobject, jlong clientHandle)`
- `Java_com_sipsdk_SipClient_nativeMakeCall(JNIEnv *, jobject, jlong clientHandle, jstring uri, jboolean withVideo)`
- `Java_com_sipsdk_SipClient_nativeSetMicMuted(JNIEnv *, jobject, jlong clientHandle, jboolean muted)`
- `Java_com_sipsdk_SipClient_nativeDestroy(JNIEnv *, jobject, jlong clientHandle)`
- `Java_com_sipsdk_SipCall_nativeAnswer(JNIEnv *, jobject, jlong callHandle, jboolean withVideo)`
- `Java_com_sipsdk_SipCall_nativeHangup(JNIEnv *, jobject, jlong callHandle, jint code, jstring reason)`
- `Java_com_sipsdk_SipCall_nativeSetHold(JNIEnv *, jobject, jlong callHandle, jboolean hold)`
- `Java_com_sipsdk_SipCall_nativeSendDtmf(JNIEnv *, jobject, jlong callHandle, jstring digits)`
- `Java_com_sipsdk_SipCall_nativeGetDuration(JNIEnv *, jobject, jlong callHandle)`
- `Java_com_sipsdk_SipCall_nativeIsClosed(JNIEnv *, jobject, jlong callHandle)`
- `Java_com_sipsdk_SipCall_nativeIsOnHold(JNIEnv *, jobject, jlong callHandle)`

---

## 3. User Stories

### SDK Initialization & Lifecycle
- **US1 (Init)**: As an Android application developer, I want to initialize the SIP SDK with an optional configuration path and Android Context via a single method call, so that the underlying networking and VoIP engines start up reliably without manual thread setup.
- **US2 (Background Loop)**: As an Android developer, I want all internal network and media polling to execute on a dedicated background thread, so that the Android application UI thread is never blocked or frozen.
- **US3 (Teardown)**: As an Android developer, I want to shut down the SIP SDK cleanly via a single termination call, so that all active media streams, sockets, threads, and JNI global references are terminated gracefully without crashes or memory leaks.
- **US4 (Thread Safety)**: As an Android developer, I want API calls to be thread-safe from any caller thread (background coroutines or UI thread), so that I do not have to write thread synchronization guards when invoking SDK methods.

### SIP Account Registration & Presence
- **US5 (Registration)**: As an Android VoIP user, I want to register my SIP account with username, domain, password, transport (UDP/TCP), and port, so that I can send and receive calls through my SIP registrar.
- **US6 (Digest Auth)**: As an Android VoIP user, I want the SDK to handle digest authentication and nonce challenges automatically, so that I do not need to parse or compute HTTP/SIP authentication headers manually.
- **US7 (Registration State Events)**: As an Android application developer, I want to receive typed registration state events (`Unregistered`, `Registering`, `Registered`, `Failed`) with status codes and reason strings on the UI thread, so that I can update the app's connection status.
- **US8 (Unregister)**: As an Android VoIP user, I want to unregister my account on demand, so that the SIP proxy stops routing incoming calls to my device when I go offline.
- **US9 (Re-registration)**: As an Android mobile developer, I want the SDK to handle automatic re-registration refreshes before registration expiry, so that my account remains reachable while the application is active.

### Audio Calling & Telephony Controls
- **US10 (Outgoing Audio Call)**: As an Android VoIP user, I want to initiate an outgoing audio-only call by specifying a remote SIP URI, so that I can speak with remote parties without opening my camera.
- **US12 (Call Session Handle)**: As an Android application developer, I want outgoing call initiation to return a dedicated `SipCall` session object, so that I can track and control the specific call instance independently.
- **US13 (Call State Transitions)**: As an Android application developer, I want to receive `Connecting`, `Ringing`, `Established`, and `Closed` state transitions on the Android Main Looper / UI thread, so that I can update call progress indicators and play ringback tones.
- **US14 (Cancel Outgoing Call)**: As an Android VoIP user, I want to cancel an outgoing call before the remote peer answers, so that I can abort accidental dials immediately.
- **US15 (Incoming Call Notification)**: As an Android VoIP user, I want to receive an incoming call notification when a remote party calls my SIP identity, so that I can choose to accept or decline the call.
- **US16 (Incoming Call Metadata)**: As an Android application developer, I want incoming call notifications to deliver a populated `SipCall` containing the caller's URI, display name, and video capability flag, so that I can display a rich incoming call screen.
- **US18 (Audio Answer)**: As an Android VoIP user, I want to answer an incoming call in audio-only mode, so that I can speak privately without transmitting video.
- **US19 (Decline Call)**: As an Android VoIP user, I want to decline or reject an incoming call with standard SIP status codes (e.g. 486 Busy Here or 603 Decline), so that the caller is promptly informed that I cannot answer.
- **US20 (Hangup)**: As an Android VoIP user, I want to hang up an active call at any time, so that the SIP session and media channels are immediately torn down.
- **US21 (Hold)**: As an Android VoIP user, I want to place an active call on hold, so that the remote party hears hold music or silence and two-way media is paused.
- **US22 (Resume)**: As an Android VoIP user, I want to resume a held call, so that two-way audio resumes smoothly without renegotiation artifacts.
- **US23 (Mute/Unmute Mic)**: As an Android VoIP user, I want to mute and unmute my microphone during a call, so that I can prevent ambient room noise from reaching the remote peer.
- **US24 (DTMF Transmission)**: As an Android VoIP user, I want to send DTMF tones (digits 0-9, *, #) via RFC 4733 during an active call, so that I can navigate interactive voice response (IVR) phone menus.
- **US25 (Call Duration)**: As an Android application developer, I want to query the live call duration in seconds, so that I can display an accurate in-call timer in the user interface.

### Android Packaging & Platform Guarantee
- **US34 (Android AAR & Kotlin)**: As an Android developer, I want to consume the SDK as an Android Archive (`.aar`) with native slices for `arm64-v8a`, `armeabi-v7a`, and `x86_64` and idiomatic Kotlin classes, so that it integrates seamlessly into modern Android Gradle builds.
- **US37 (Main UI Thread Guarantee)**: As an Android SDK consumer, I want all listener callbacks to be dispatched directly on the Android Main Looper / UI thread (`Looper.getMainLooper()`), so that I never encounter multithreaded UI state corruption crashes.

---

## 4. Out of Scope

1. **Android Hardware Video via MediaCodec**: Hardware H.264 video encoding and decoding via Android `MediaCodec` is explicitly reserved for Slice 5 (Issue #16).
2. **Camera2 / CameraX Capture & SipSurfaceView**: Camera capture and rendering views on Android are reserved for Issue #16.
3. **SIP over TLS (SIPS) and SRTP**: TLS (`USE_OPENSSL=ON`) requires cross-compiling OpenSSL / MbedTLS across all OSes; Phase 1 focuses on standard UDP/TCP signaling.
4. **Desktop Native Targets (macOS, Linux, Windows)**: Reserved for Milestone 3 (Issues #17 & #18).
5. **Web (WASM / WebRTC)**: Explicitly deferred to Phase 2.
6. **Third-Party Software Codecs (Opus, VP8, VP9)**: Phase 1 relies strictly on standard G.711 audio (PCMA/PCMU).

---

## 5. Non-Functional Requirements & Performance Targets

- **Binary Size & Arch Support**: `sipsdk.aar` must contain compiled `libsipcore.so` for `arm64-v8a`, `armeabi-v7a`, and `x86_64`. Stripped binary size per architecture slice should not exceed 3.5 MB.
- **Symbol Visibility**: Symbol audit of `libsipcore.so` must confirm that only `Java_com_sipsdk_*` and `_sip_*` symbols are exported. All internal Baresip, libre, and C++ symbols must remain hidden.
- **Audio Latency**: OpenSL ES audio capture and playback latency on Android devices/emulators must not exceed 60 ms buffer delay.
- **Thread Safety**: All Kotlin public methods (`start`, `register`, `makeCall`, `hangup`, `setHold`, etc.) must be safely callable from any thread (UI or background worker thread) without race conditions.
- **Main Thread Delivery**: 100% of `SipClientListener` callbacks must arrive with `Looper.myLooper() == Looper.getMainLooper()`.
- **Clean Teardown**: Teardown via `SipClient.destroy()` must join worker threads, delete JNI global references, release `AudioManager` mode, close sockets, and leave zero memory leaks.

---

## 6. Testing Strategy & Acceptance Criteria

- **AAR Packaging & Multi-Arch Verification**: Gradle task `:sipsdk:assembleRelease` compiles and packages `sipsdk-release.aar`. Automated unpack script verifies presence of `jni/arm64-v8a/libsipcore.so`, `jni/armeabi-v7a/libsipcore.so`, `jni/x86_64/libsipcore.so`, and `classes.jar`.
- **Symbol Export Audit**: Inspects exported dynamic symbols (`llvm-nm -gD` or `nm -gD`) across all `.so` slices to verify zero leaked C++ or libre symbols.
- **JNI Unit Tests**: JVM / Android unit test runner testing `SipClient` creation, configuration properties, state transitions, and listener dispatching.
- **Asterisk 22 PBX Registration & Audio Call Test**: Automated test registering account `1001`, initiating an audio call to echo extension `9001`, asserting state transitions (`CONNECTING` -> `ESTABLISHED`), verifying duration timer increment, performing mute/unmute and hold/resume, and hanging up cleanly with BYE.
- **iOS Parity Lock**: Executing `scripts/verify-audio-calling.sh` and `scripts/verify-spm-video-sdk.sh` guarantees zero regressions to existing Apple targets.
