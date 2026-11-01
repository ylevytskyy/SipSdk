# Plan: Android Platform SDK & Audio Calling (`android-audio-calling`)

**Goal**: Establish the Android SDK packaging pipeline (`platforms/android/`), build `libsipcore.so` across `arm64-v8a`, `armeabi-v7a`, and `x86_64` packaged into a distributable `sipsdk.aar`, provide idiomatic Kotlin JNI wrappers (`SipClient`, `SipCall`, `SipClientListener`), integrate OpenSL ES audio capture/playback and Android `AudioManager` hardware coordination, marshal all callbacks to the Android Main Looper / UI thread, and verify end-to-end registration and audio calling against Asterisk PBX with zero regressions on existing iOS targets.

## Global Constraints

- **Language Standards**: C++17 for `sip_core`; pure C11 `extern "C"` for `sip_abi.h`; Kotlin 1.9+ / Java 17 for Android bindings.
- **NDK Toolchain & Architectures**: Android NDK r26 / r27 with CMake 3.22.1; native slices compiled for `arm64-v8a`, `armeabi-v7a`, and `x86_64`.
- **Packaging Format**: Distributable Android Archive (`sipsdk.aar`) produced by Gradle with stripped native `.so` libraries and public Kotlin symbols.
- **Symbol Visibility**: `-fvisibility=hidden` on all C/C++ sources; only JNI entry points (`Java_com_sipsdk_*`) and public C ABI functions (`_sip_*`) are exported.
- **Network & Socket ABI Safety**: `-DUSE_UNIXSOCK=ON` enforced across all builds to prevent `struct sa` / `struct sip_msg` layout misalignment and silent registration hangs.
- **Threading & UI Thread Guarantee**: Background worker thread executes `re_main()`; all client listener callbacks (`onRegistrationStateChanged`, `onCallStateChanged`, `onIncomingCall`, etc.) must be dispatched to the Android Main Looper / UI thread (`Looper.getMainLooper()`).
- **Audio Session Coordination**: Android `AudioManager.MODE_IN_COMMUNICATION` must be activated before Baresip starts OpenSL ES audio capture and restored to `MODE_NORMAL` upon session completion.
- **Preserved Compatibility**: Zero regressions against existing iOS test harnesses (`scripts/verify-audio-calling.sh`, `scripts/verify-spm-video-sdk.sh`, `scripts/verify-ios-video-tests.sh`).

---

## 1. Frame

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

### User Stories
- **US1 (Init)**: As an Android application developer, I want to initialize the SIP SDK with an optional configuration path and Android Context via a single method call, so that the underlying networking and VoIP engines start up reliably without manual thread setup.
- **US2 (Background Loop)**: As an Android developer, I want all internal network and media polling to execute on a dedicated background thread, so that the Android application UI thread is never blocked or frozen.
- **US3 (Teardown)**: As an Android developer, I want to shut down the SIP SDK cleanly via a single termination call, so that all active media streams, sockets, threads, and JNI global references are terminated gracefully without crashes or memory leaks.
- **US4 (Thread Safety)**: As an Android developer, I want API calls to be thread-safe from any caller thread (background coroutines or UI thread), so that I do not have to write thread synchronization guards when invoking SDK methods.
- **US5 (Registration)**: As an Android VoIP user, I want to register my SIP account with username, domain, password, transport (UDP/TCP), and port, so that I can send and receive calls through my SIP registrar.
- **US6 (Digest Auth)**: As an Android VoIP user, I want the SDK to handle digest authentication and nonce challenges automatically, so that I do not need to parse or compute HTTP/SIP authentication headers manually.
- **US7 (Registration State Events)**: As an Android application developer, I want to receive typed registration state events (`Unregistered`, `Registering`, `Registered`, `Failed`) with status codes and reason strings on the UI thread, so that I can update the app's connection status.
- **US8 (Unregister)**: As an Android VoIP user, I want to unregister my account on demand, so that the SIP proxy stops routing incoming calls to my device when I go offline.
- **US9 (Re-registration)**: As an Android mobile developer, I want the SDK to handle automatic re-registration refreshes before registration expiry, so that my account remains reachable while the application is active.
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
- **US34 (Android AAR & Kotlin)**: As an Android developer, I want to consume the SDK as an Android Archive (`.aar`) with native slices for `arm64-v8a`, `armeabi-v7a`, and `x86_64` and idiomatic Kotlin classes, so that it integrates seamlessly into modern Android Gradle builds.
- **US37 (Main UI Thread Guarantee)**: As an Android SDK consumer, I want all listener callbacks to be dispatched directly on the Android Main Looper / UI thread (`Looper.getMainLooper()`), so that I never encounter multithreaded UI state corruption crashes.

### Out of Scope
1. **Android Hardware Video via MediaCodec**: Reserved for Slice 5 (Issue #16).
2. **Camera2 / CameraX Capture & SipSurfaceView**: Reserved for Issue #16.
3. **SIP over TLS (SIPS) and SRTP**: Phase 1 baseline is standard UDP/TCP signaling.
4. **Desktop Native Targets (macOS, Linux, Windows)**: Reserved for Milestone 3 (Issues #17 & #18).
5. **Web (WASM / WebRTC)**: Explicitly deferred to Phase 2.
6. **Third-Party Software Codecs (Opus, VP8, VP9)**: Phase 1 relies strictly on standard G.711 audio (PCMA/PCMU).

### Implementation Decisions
- **`platforms/android/` Gradle Setup**: Standard Gradle 8.x multi-module project (`settings.gradle.kts` declaring `:sipsdk` and `:sample`). `sipsdk` configures `com.android.library`, `kotlin-android`, `externalNativeBuild.cmake`, and specifies `abiFilters("arm64-v8a", "armeabi-v7a", "x86_64")`.
- **`scripts/build-baresip-android.sh`**: Reproducible NDK build script compiling pinned `libre` (v4.12.0) and `baresip` (v4.12.0) for `arm64-v8a`, `armeabi-v7a`, and `x86_64` with modules `g711;opensles;account;stun;turn;ice;srtp`.
- **`platforms/android/sipsdk/src/main/cpp/CMakeLists.txt`**: Compiles `sip_jni.cpp` along with `core/src/call_session.cpp`, `core/src/engine.cpp`, `core/src/media_driver.cpp`, `core/src/sip_abi.cpp`, linking against the per-ABI `libbaresip.a`, `libre.a`, `-lOpenSLES`, `-llog`, and `-landroid` into `libsipcore.so`.
- **`platforms/android/sipsdk/src/main/cpp/sip_jni.cpp`**: Implements JNI bindings, retains `JavaVM*` and global JNI listener references, implements `dispatch_to_main` delivering tasks to the JVM main thread, and maps native callbacks to Kotlin calls.
- **`platforms/android/sipsdk/src/main/kotlin/com/sipsdk/`**: Kotlin SDK layer providing `SipClient`, `SipCall`, `SipClientListener`, `SipCallState`, `SipRegistrationState`, and `SipAudioManager`.
- **`scripts/verify-android-sdk.sh`**: Unified automated verification harness building `sipsdk.aar`, verifying native slices, checking symbol exports, and running automated tests.

### Testing Decisions
- **AAR Packaging & Multi-Arch Verification**: Extract `sipsdk-release.aar` and assert presence of `jni/arm64-v8a/libsipcore.so`, `jni/armeabi-v7a/libsipcore.so`, `jni/x86_64/libsipcore.so`, and `classes.jar`.
- **Symbol Audit**: Inspect `libsipcore.so` exports across all 3 slices to guarantee no C++ or libre internal symbols leak.
- **Main Looper Assertions**: Assert `Looper.myLooper() == Looper.getMainLooper()` inside listener callbacks.
- **Asterisk 22 PBX Registration & Audio Call**: Live automated verification against Asterisk container (registration for user `1001`, audio call to echo `9001`, duration check, hold/resume, DTMF, hangup).
- **iOS Parity Regression**: Zero regressions on `scripts/verify-audio-calling.sh` and `scripts/verify-spm-video-sdk.sh`.

---

## 2. Seam

The test seams attach at:
1. **Android Kotlin Public API Boundary (`com.sipsdk.SipClient` & `com.sipsdk.SipCall`)**: Primary high-level test seam exercised by automated tests on JVM/Android environment against local Asterisk PBX.
2. **JNI / Shared C ABI Boundary (`libsipcore.so` & `sip_jni.cpp`)**: Secondary seam exercised by dynamic symbol audits and native linkage verification.

---

## 3. Vertical Slices

### Slice 1 — Android SDK Build Pipeline, Multi-Arch NDK Toolchain & Distributable AAR Packaging (Issue #15)

- **User stories covered**: US34, US1, US2, US3, US4
- **Device tier**: Android build/package tier (`arm64-v8a`, `armeabi-v7a`, `x86_64` AAR compilation) & macOS host execution tier
- **Files**:
  - `platforms/android/build.gradle.kts`
  - `platforms/android/settings.gradle.kts`
  - `platforms/android/gradle.properties`
  - `platforms/android/sipsdk/build.gradle.kts`
  - `platforms/android/sipsdk/src/main/AndroidManifest.xml`
  - `platforms/android/sipsdk/src/main/cpp/CMakeLists.txt`
  - `platforms/android/sipsdk/src/main/cpp/sip_jni.cpp`
  - `platforms/android/sipsdk/src/main/kotlin/com/sipsdk/SipSdk.kt`
  - `scripts/build-baresip-android.sh`
  - `scripts/verify-android-sdk.sh`
- **Interfaces**:
  - JNI: `jstring Java_com_sipsdk_SipSdk_nativeGetVersion(JNIEnv *env, jclass clazz)`
  - Kotlin: `object SipSdk { fun getVersion(): String }`
  - Shell: `bash scripts/build-baresip-android.sh`
  - Shell: `bash scripts/verify-android-sdk.sh --slice 1`
- **Acceptance criteria**:
  1. **Given** the Android NDK and Gradle build environment, **when** `./gradlew :sipsdk:assembleRelease` is executed, **then** `sipsdk-release.aar` is generated at `platforms/android/sipsdk/build/outputs/aar/sipsdk-release.aar` with exit code 0.
  2. **Given** the generated `sipsdk-release.aar`, **when** the archive is inspected, **then** it contains native shared libraries for all 3 target architectures: `jni/arm64-v8a/libsipcore.so`, `jni/armeabi-v7a/libsipcore.so`, and `jni/x86_64/libsipcore.so`, along with `classes.jar` containing compiled `com/sipsdk/SipSdk.class`.
  3. **Given** the compiled `libsipcore.so` dynamic library across all architectures, **when** running a dynamic symbol export audit (`llvm-nm -gD` or `nm -gD`), **then** only JNI exported symbols (`Java_com_sipsdk_*`) and public C ABI symbols (`sip_*`) are visible, and internal libre, Baresip, and C++ symbols remain hidden (`-fvisibility=hidden`).
  4. **Given** a consumer application loading `sipsdk.aar`, **when** `SipSdk.getVersion()` is invoked, **then** the library loads successfully via `System.loadLibrary("sipcore")` and returns a non-empty SDK version string without throwing `UnsatisfiedLinkError`.
- **Verify**: `bash scripts/verify-android-sdk.sh --slice 1`
- **Blocked-by**: None

---

### Slice 2 — JNI Bridge, Main-Thread Dispatching & SIP Account Registration (Issue #15)

- **User stories covered**: US5, US6, US7, US8, US9, US37
- **Device tier**: Android build/package tier & Android Simulator / Emulator / Host JVM test tier
- **Files**:
  - `platforms/android/sipsdk/src/main/cpp/sip_jni.cpp`
  - `platforms/android/sipsdk/src/main/kotlin/com/sipsdk/SipClient.kt`
  - `platforms/android/sipsdk/src/main/kotlin/com/sipsdk/SipClientListener.kt`
  - `platforms/android/sipsdk/src/main/kotlin/com/sipsdk/SipRegistrationState.kt`
  - `platforms/android/sipsdk/src/test/kotlin/com/sipsdk/SipClientRegistrationTest.kt`
  - `scripts/verify-android-sdk.sh`
- **Interfaces**:
  - Kotlin `SipClient(val username: String, val domain: String, val context: Context? = null)`:
    - `var password: String?`, `var transport: String`, `var port: Int`, `var displayName: String?`
    - `val aor: String`, `val isRegistered: Boolean`
    - `var listener: SipClientListener?`
    - `fun start(): Int`, `fun stop()`, `fun register(): Int`, `fun unregister(): Int`, `fun destroy()`
  - Kotlin `SipClientListener`:
    - `fun onRegistrationStateChanged(state: SipRegistrationState, code: Int, reason: String)`
  - JNI Native Methods:
    - `jlong Java_com_sipsdk_SipClient_nativeCreate(JNIEnv *, jobject, jobject listener)`
    - `jint Java_com_sipsdk_SipClient_nativeInit(JNIEnv *, jobject, jlong clientHandle, jstring configPath)`
    - `jint Java_com_sipsdk_SipClient_nativeRegister(JNIEnv *, jobject, jlong clientHandle, jstring user, jstring domain, jstring pwd, jstring transport, jint port)`
    - `jint Java_com_sipsdk_SipClient_nativeUnregister(JNIEnv *, jobject, jlong clientHandle)`
    - `jboolean Java_com_sipsdk_SipClient_nativeIsRegistered(JNIEnv *, jobject, jlong clientHandle)`
    - `void Java_com_sipsdk_SipClient_nativeDestroy(JNIEnv *, jobject, jlong clientHandle)`
- **Acceptance criteria**:
  1. **Given** a configured `SipClient` with valid credentials (`user="1001"`, `domain="127.0.0.1"`, `password="1001"`, `port=5060`), **when** `client.start()` and `client.register()` are called, **then** the client performs MD5 digest authentication against Asterisk PBX and transitions through `SipRegistrationState.REGISTERING` to `SipRegistrationState.REGISTERED` (code 200, "OK").
  2. **Given** an active registration event dispatch, **when** `onRegistrationStateChanged` fires on `SipClientListener`, **then** `Looper.myLooper() == Looper.getMainLooper()` evaluates to true, verifying that callbacks are delivered on the Android Main Looper / UI thread.
  3. **Given** a registered `SipClient`, **when** `client.unregister()` is called, **then** a SIP unregister request (Expires: 0) is sent to the registrar, `client.isRegistered` returns false, and `onRegistrationStateChanged` fires `SipRegistrationState.UNREGISTERED`.
  4. **Given** invalid credentials (incorrect password), **when** `client.register()` is called, **then** the server rejects authentication with 401/403 and `onRegistrationStateChanged` fires `SipRegistrationState.FAILED` with the server status code.
- **Verify**: `bash scripts/verify-android-sdk.sh --slice 2`
- **Blocked-by**: Slice 1

---

### Slice 3 — Audio Calling Session, In-Call Telephony Controls & Android Audio Subsystem (Issue #15)

- **User stories covered**: US10, US12, US13, US14, US15, US16, US18, US19, US20, US21, US22, US23, US24, US25
- **Device tier**: Android build/package tier & Android Simulator / Emulator / Host integration test tier
- **Files**:
  - `platforms/android/sipsdk/src/main/cpp/sip_jni.cpp`
  - `platforms/android/sipsdk/src/main/kotlin/com/sipsdk/SipCall.kt`
  - `platforms/android/sipsdk/src/main/kotlin/com/sipsdk/SipCallState.kt`
  - `platforms/android/sipsdk/src/main/kotlin/com/sipsdk/SipAudioManager.kt`
  - `platforms/android/sipsdk/src/test/kotlin/com/sipsdk/SipCallAudioTest.kt`
  - `platforms/android/sample/src/main/AndroidManifest.xml`
  - `platforms/android/sample/src/main/kotlin/com/sipsdk/sample/MainActivity.kt`
  - `scripts/verify-android-sdk.sh`
- **Interfaces**:
  - Kotlin `SipCall`:
    - `val callId: String`, `val remoteUri: String`, `val peerDisplayName: String?`, `val hasVideo: Boolean`, `val isOutgoing: Boolean`, `val isOnHold: Boolean`, `val isClosed: Boolean`, `val durationSeconds: Long`
    - `fun answer(): Int`, `fun hangup(statusCode: Int = 0, reason: String = "Hangup"): Int`, `fun setHold(hold: Boolean): Int`, `fun sendDtmf(digits: String): Int`
  - Kotlin `SipClient`:
    - `fun makeCall(uri: String): SipCall?`, `fun setMuted(muted: Boolean)`, `fun setSpeakerphone(enabled: Boolean)`
    - `var isMicrophoneMuted: Boolean`, `var isSpeakerEnabled: Boolean`
  - Kotlin `SipAudioManager`:
    - `fun prepareAudioSession(): Boolean`, `fun releaseAudioSession()`, `fun setSpeakerphone(enabled: Boolean)`, `fun setMicrophoneMute(muted: Boolean)`
  - JNI Native Methods:
    - `jlong Java_com_sipsdk_SipClient_nativeMakeCall(JNIEnv *, jobject, jlong clientHandle, jstring uri, jboolean withVideo)`
    - `void Java_com_sipsdk_SipClient_nativeSetMicMuted(JNIEnv *, jobject, jlong clientHandle, jboolean muted)`
    - `jint Java_com_sipsdk_SipCall_nativeAnswer(JNIEnv *, jobject, jlong callHandle, jboolean withVideo)`
    - `jint Java_com_sipsdk_SipCall_nativeHangup(JNIEnv *, jobject, jlong callHandle, jint code, jstring reason)`
    - `jint Java_com_sipsdk_SipCall_nativeSetHold(JNIEnv *, jobject, jlong callHandle, jboolean hold)`
    - `jint Java_com_sipsdk_SipCall_nativeSendDtmf(JNIEnv *, jobject, jlong callHandle, jstring digits)`
    - `jlong Java_com_sipsdk_SipCall_nativeGetDuration(JNIEnv *, jobject, jlong callHandle)`
    - `jboolean Java_com_sipsdk_SipCall_nativeIsClosed(JNIEnv *, jobject, jlong callHandle)`
    - `jboolean Java_com_sipsdk_SipCall_nativeIsOnHold(JNIEnv *, jobject, jlong callHandle)`
- **Acceptance criteria**:
  1. **Given** a registered `SipClient`, **when** `client.makeCall("sip:9001@127.0.0.1:5060")` is called, **then** a valid `SipCall` instance is returned, `prepare_audio_session` activates Android `AudioManager.MODE_IN_COMMUNICATION`, the session transitions through `SipCallState.CONNECTING` to `SipCallState.ESTABLISHED`, and G.711 audio streams over OpenSL ES.
  2. **Given** an established call session connected for over 1 second, **when** `call.durationSeconds` is queried, **then** it returns `>= 1`.
  3. **Given** an established call session, **when** `call.setHold(true)` is invoked, **then** media stream direction is updated to `sendonly`/`inactive`, and `onCallMediaChanged` reports `onHold = true`; when `call.setHold(false)` is invoked, media resumes smoothly.
  4. **Given** an active call session, **when** `call.sendDtmf("123#")` is called, **then** RFC 4733 DTMF packets are transmitted to the remote peer without error.
  5. **Given** an active call session, **when** `call.hangup(0, "Hangup")` is called, **then** a SIP BYE is transmitted to the remote peer, the session transitions to `SipCallState.CLOSED`, `call.isClosed` returns true, and `releaseAudioSession` restores Android `AudioManager.MODE_NORMAL`.
  6. **Given** existing iOS test suites, **when** `scripts/verify-audio-calling.sh` and `scripts/verify-spm-video-sdk.sh` are executed alongside Slice 3, **then** 100% of existing Apple tests pass with zero regressions.
- **Verify**: `bash scripts/verify-android-sdk.sh`
- **Blocked-by**: Slice 1, Slice 2
