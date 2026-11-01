# Plan: Android Hardware Video & SurfaceView Rendering (`android-hardware-video`)

**Goal**: Implement the hardware-accelerated video pipeline for the Android Platform SDK (`platforms/android/`), integrating Android `MediaCodec` for hardware H.264 encoding/decoding, Camera2 for camera frame capture and camera flipping, and a turnkey `SipSurfaceView` widget for local preview and remote video rendering, verified end-to-end against Asterisk video echo (`9001`) with zero regressions on existing iOS targets.

## Global Constraints

- **Language Standards**: C++17 for `sip_core` and NDK drivers; pure C11 `extern "C"` for `sip_abi.h`; Kotlin 1.9+ / Java 17 for Android SDK.
- **NDK Toolchain & Minimum API**: Android NDK r26 / r27 with CMake 3.22.1; minimum API 21 (`minSdk = 21`, `compileSdk = 34`).
- **Hardware Acceleration**: Encoding and decoding use Android `MediaCodec` via NDK C API (`<media/NdkMediaCodec.h>`, `<media/NdkMediaFormat.h>`, linking `-lmediandk` and `-landroid`).
- **Symbol Visibility**: `-fvisibility=hidden` on all C/C++ sources; only JNI entry points (`Java_com_sipsdk_*`) and public C ABI functions (`_sip_*`) are exported.
- **Network & Socket ABI Safety**: `-DUSE_UNIXSOCK=ON` enforced across all builds to prevent `struct sa` / `struct sip_msg` layout misalignment and silent registration hangs.
- **Threading & UI Thread Guarantee**: Background worker thread executes `re_main()`; all client listener callbacks (`onCallMediaChanged`, `onCallStateChanged`, `onIncomingCall`) must be dispatched to the Android Main Looper / UI thread (`Looper.getMainLooper()`).
- **Aspect Ratio Fidelity**: `SipSurfaceView` preserves video aspect ratio (4:3 or 16:9) without distortion or stretching.
- **Preserved Compatibility**: Zero regressions against existing iOS test harnesses (`scripts/verify-ios-video-tests.sh`, `scripts/verify-spm-video-sdk.sh`, `scripts/verify-sim-app-launch.sh`) and core video streaming tests (`scripts/verify-video-streaming.sh`).

---

## 1. Frame

### Problem
While the portable C++17 core engine (`sip_core`), universal flat C ABI (`sip_abi`), and Android audio calling slice (`#15`) provide account registration, audio calling, and telephony controls for Android applications, Android developers still lack native video communication capabilities.

Currently, video calls on Android cannot negotiate H.264 video streams or render video frames. Video hardware acceleration on Android requires integration with `MediaCodec` (via the Android NDK `AMediaCodec` C API) for real-time H.264 encoding and decoding without thermal throttling. Camera capture requires Android `Camera2` integration to deliver live camera frames to both local preview and outgoing video streams, as well as mid-call camera flipping between front and rear cameras. Furthermore, application developers need a turnkey `SipSurfaceView` UI widget that automatically maintains aspect ratio and renders local and remote video streams with GPU acceleration, eliminating raw buffer management in host applications.

### Solution
Deliver the hardware-accelerated video pipeline for the Android Platform SDK (`platforms/android/`):
1. **Android `MediaCodec` Hardware H.264 Codec Driver**:
   - Implement `AndroidMediaCodecDriver` in C++ utilizing NDK `<media/NdkMediaCodec.h>` and `<media/NdkMediaFormat.h>` (linked against `libmediandk.so` and `libandroid.so`).
   - Register H.264 encoder and decoder plugins (`packetization-mode=1` and `packetization-mode=0`, profile-level-id `42e01f`) with Baresip's `baresip_vidcodecl()`.
   - Encoder: Converts captured YUV420 frames into RFC 6184 Single NAL and FU-A RTP packets.
   - Decoder: Reassembles incoming RTP NAL units into Access Units, decodes via hardware `AMediaCodec`, and renders directly to an attached `ANativeWindow` or outputs decoded video frames.
   - Keyframe Recovery: Responds to packet loss by triggering RTCP PLI keyframe requests rate-limited to at most one per 500 ms.
2. **Camera2 Video Capture Driver & Mid-Call Controls**:
   - Create `SipCameraCapture` leveraging Android `Camera2` API (`CameraManager`, `CameraDevice`, `ImageReader`) running on a dedicated background thread.
   - Deliver YUV_420_888 frame buffers to the native capture driver (`sip_video_capture_driver_t`) and local preview.
   - Implement camera flip action (`SipClient.switchCamera()`, `SipClient.setCamera()`) seamlessly switching between front and back camera sensors during an active call.
   - Implement local video mute/unmute (`SipClient.setLocalVideoEnabled()`) and standalone local camera preview (`startLocalPreview()`, `stopLocalPreview()`).
   - Provide synthetic test pattern generator fallback for headless automated testing and emulator environments without physical cameras.
3. **Turnkey `SipSurfaceView` UI Widget**:
   - Create `SipSurfaceView` extending Android `SurfaceView` and implementing `SurfaceHolder.Callback`.
   - Manage aspect ratio automatically (`onMeasure` logic for 4:3 and 16:9 aspect ratios).
   - Bind to SDK via `client.remoteVideoView = surfaceView` and `client.localVideoView = surfaceView`.
   - Forward native window surfaces (`ANativeWindow_fromSurface`) to the hardware decoder and local renderer for zero-copy GPU rendering.
4. **End-to-End Two-Way Video Calling**:
   - Enable video call negotiation: `client.makeCall(uri, withVideo = true)` and `call.answer(withVideo = true)`.
   - Expose explicit keyframe recovery: `call.requestKeyframe()`.
   - Verify two-way video calling between Android and Asterisk PBX video echo (`9001`) with live frame reception, aspect ratio maintenance, camera flip, and clean teardown.

### User Stories
- **US11 (Outgoing Video Call)**: As an Android VoIP user, I want to initiate an outgoing video call with negotiated H.264 video and G.711 audio, so that I can see and hear the remote peer simultaneously.
- **US17 (Incoming Video Call)**: As an Android VoIP user, I want to answer an incoming video call with two-way video enabled, so that both parties share live camera feeds immediately.
- **US18 (Audio Answer to Video Call)**: As an Android VoIP user, I want to answer an incoming video call in audio-only mode, so that I can speak privately without transmitting video.
- **US27 (Hardware Video Acceleration)**: As an Android VoIP user, I want video encoding and decoding to use native hardware acceleration (`MediaCodec`), so that video calls stream smoothly without overheating my device or causing battery drain.
- **US28 (re-INVITE `recvonly` Stream Preservation)**: As an Android video caller, I want remote video to continue rendering smoothly even when the remote endpoint or PBX sends an in-dialog re-INVITE offering `recvonly`, so that the video stream does not freeze or black out mid-call.
- **US29 (RTP Packet Loss Keyframe Recovery)**: As an Android video caller, I want the SDK to automatically detect RTP packet loss and trigger RTCP PLI/FIR keyframe requests, so that visual corruption is quickly cleared upon the next IDR frame.
- **US30 (Turnkey Platform Video Views)**: As an Android application developer, I want to drop a turnkey video widget (`SipSurfaceView`) into my layout, so that local camera preview and remote video render automatically with proper aspect ratios.
- **US31 (Camera Switching)**: As an Android mobile video caller, I want to toggle between the front and rear cameras during an active call, so that I can show my surroundings without dropping the video feed.
- **US32 (Dynamic Video Mute / Toggle)**: As an Android video caller, I want to turn my camera feed on or off dynamically during an ongoing call, so that I can temporarily disable my video without ending the call.
- **US34 (Android AAR & Kotlin)**: As an Android developer, I want to consume video calling features via idiomatic Kotlin APIs packaged inside `sipsdk.aar`, so that integration requires no native C/JNI boilerplate.
- **US37 (Main UI Thread Guarantee)**: As an Android SDK consumer, I want all video event callbacks (`onCallMediaChanged`) to be dispatched directly on the Android Main Looper / UI thread (`Looper.getMainLooper()`), so that I never encounter multithreaded UI state corruption crashes.

### Out of Scope
1. **Desktop Native Targets (macOS, Linux, Windows)**: Reserved for Milestone 3 (Issues #17 & #18).
2. **Web (WASM / WebRTC)**: Explicitly deferred to Phase 2.
3. **SIP over TLS (SIPS) and SRTP**: Phase 1 baseline is standard UDP/TCP signaling.
4. **Third-Party Software Codecs (Opus, VP8, VP9)**: Phase 1 relies strictly on standard G.711 audio and hardware H.264.
5. **Custom Raw Frame Shaders / Filters**: Turnkey rendering delivers direct hardware surface presentation; custom OpenGL shader pipelines are out of scope.

### Implementation Decisions
- **`platforms/android/sipsdk/src/main/cpp/media_codec_driver.h` & `media_codec_driver.cpp`**: Implements `AndroidMediaCodecDriver` wrapping NDK `AMediaCodec` for H.264 encoding and decoding. Links `-lmediandk` and `-landroid` in `CMakeLists.txt`. Registers `gH264MediaCodecMode1` (`packetization-mode=1`) and `gH264MediaCodecMode0` (`packetization-mode=0`) with Baresip's `baresip_vidcodecl()`.
- **`platforms/android/sipsdk/src/main/cpp/sip_jni.cpp`**: Exposes JNI methods for surface binding (`nativeSetRemoteSurface`, `nativeSetLocalSurface`), camera controls (`nativeSwitchCamera`, `nativeSetCamera`, `nativeSetLocalVideoEnabled`, `nativeStartLocalPreview`, `nativeStopLocalPreview`), frame delivery (`nativeDeliverFrame`), and call keyframe request (`nativeCallRequestKeyframe`).
- **`platforms/android/sipsdk/src/main/kotlin/com/sipsdk/SipCameraCapture.kt`**: Manages Android Camera2 capture session, switching between front (`LENS_FACING_FRONT`) and rear (`LENS_FACING_BACK`) sensors, delivering frames via `ImageReader`, and falling back to synthetic test pattern in headless emulator runs.
- **`platforms/android/sipsdk/src/main/kotlin/com/sipsdk/SipSurfaceView.kt`**: Turnkey `SurfaceView` managing surface lifecycle, window surface passing to JNI (`ANativeWindow_fromSurface`), aspect ratio measuring, and frame clearing.
- **`platforms/android/sipsdk/src/main/kotlin/com/sipsdk/SipClient.kt` & `SipCall.kt`**: Extended with `remoteVideoView`, `localVideoView`, `switchCamera()`, `setCamera()`, `setLocalVideoEnabled()`, `startLocalPreview()`, `stopLocalPreview()`, `makeCall(uri, withVideo = true)`, `answer(withVideo = true)`, and `requestKeyframe()`.
- **`scripts/verify-android-sdk.sh`**: Extended with automated video test execution targets for MediaCodec, camera capture, and Asterisk 9001 two-way video echo.

### Testing Decisions
- **MediaCodec Unit & Integration Test (`SipMediaCodecTest.kt`)**: Verifies hardware H.264 codec initialization, encoding YUV frames into RFC 6184 NAL units (SPS/PPS and IDR frames), and decoding NAL packets into video frames without crashes or memory leaks.
- **Camera Capture Driver Test (`SipCameraCaptureTest.kt`)**: Verifies camera discovery, front/back sensor selection, frame delivery callbacks, camera flip action, and synthetic fallback.
- **End-to-End Two-Way Video Call Test (`SipCallVideoTest.kt`)**: Registers against local Asterisk PBX, places outgoing video call to echo extension `9001`, asserts `ESTABLISHED` state, asserts `call.hasVideo == true`, asserts `onCallMediaChanged` receives positive dimensions, asserts frames render onto `SipSurfaceView`, executes camera flip, requests RTCP keyframe, and cleanly hangs up.
- **Sample App Build & UI Verification**: Verifies `sample/MainActivity.kt` compiles with video controls and UI elements.

---

## 2. Seam

The test seams attach at:
1. **Android Kotlin Public API Boundary (`com.sipsdk.SipClient`, `com.sipsdk.SipCall`, and `com.sipsdk.SipSurfaceView`)**: Primary high-level test seam exercised by Android connected instrumented tests on emulator against local Asterisk PBX.
2. **JNI / NDK Shared Library Boundary (`libsipcore.so` & `media_codec_driver.cpp`)**: Secondary seam exercised by dynamic symbol audits (`llvm-nm -gD`) and native codec lifecycle verification.

---

## 3. Vertical Slices

### Slice 1 — Android MediaCodec Hardware H.264 Codec Driver (`android-mediacodec-driver`)

- **User stories covered**: US27, US29, US34
- **Device tier**: Android build/package tier & Android Simulator / Emulator / Host test tier
- **Files**:
  - `platforms/android/sipsdk/src/main/cpp/CMakeLists.txt`
  - `platforms/android/sipsdk/src/main/cpp/media_codec_driver.h`
  - `platforms/android/sipsdk/src/main/cpp/media_codec_driver.cpp`
  - `platforms/android/sipsdk/src/main/cpp/sip_jni.cpp`
  - `platforms/android/sipsdk/src/androidTest/kotlin/com/sipsdk/SipMediaCodecTest.kt`
  - `scripts/verify-android-sdk.sh`
- **Interfaces**:
  - C++ `AndroidMediaCodecDriver : public sip_core::IVideoCodecDriver`:
    - `int register_codec() override`
    - `void unregister_codec() override`
  - NDK MediaCodec: `<media/NdkMediaCodec.h>` (`AMediaCodec_createEncoderByType`, `AMediaCodec_createDecoderByType`, `AMediaCodec_configure`, `AMediaCodec_start`)
  - JNI: `Java_com_sipsdk_SipCall_nativeRequestKeyframe(JNIEnv *, jobject, jlong callHandle)`
  - Shell: `bash scripts/verify-android-sdk.sh --slice 4`
- **Acceptance criteria**:
  1. **Given** `libsipcore.so` built with CMake, **when** inspected with `llvm-nm -gD`, **then** it links against `libmediandk.so` and `libandroid.so`, exporting only JNI and `sip_*` symbols with zero leaked internal C++ symbols.
  2. **Given** an initialized `SipClient`, **when** the client starts, **then** `AndroidMediaCodecDriver` registers `H264` (`packetization-mode=1` and `0`) with Baresip's `baresip_vidcodecl()` without error.
  3. **Given** raw YUV420 video frames delivered to the encoder, **when** encoded by `AMediaCodec`, **then** RFC 6184 Single NAL and FU-A RTP packets (including SPS, PPS, and IDR keyframes) are generated with valid RTP timestamps.
  4. **Given** an active call experiencing packet loss, **when** `call.requestKeyframe()` is invoked, **then** an RTCP Picture Loss Indication (PLI) request is dispatched to prompt an immediate IDR frame.
- **Verify**: `bash scripts/verify-android-sdk.sh --slice 4`
- **Blocked-by**: None

---

### Slice 2 — Camera2 Video Capture Driver & Dynamic Camera Controls (`android-camera-capture`)

- **User stories covered**: US31, US32, US34
- **Device tier**: Android build/package tier & Android Simulator / Emulator / Host test tier
- **Files**:
  - `platforms/android/sipsdk/src/main/kotlin/com/sipsdk/SipCameraPosition.kt`
  - `platforms/android/sipsdk/src/main/kotlin/com/sipsdk/SipCameraCapture.kt`
  - `platforms/android/sipsdk/src/main/kotlin/com/sipsdk/SipClient.kt`
  - `platforms/android/sipsdk/src/main/cpp/sip_jni.cpp`
  - `platforms/android/sipsdk/src/androidTest/kotlin/com/sipsdk/SipCameraCaptureTest.kt`
  - `scripts/verify-android-sdk.sh`
- **Interfaces**:
  - Kotlin `enum class SipCameraPosition(val value: Int) { FRONT(0), BACK(1) }`
  - Kotlin `SipCameraCapture(context: Context, clientHandle: Long)`:
    - `fun start(width: Int = 640, height: Int = 480, fps: Int = 30): Int`
    - `fun stop()`
    - `fun switchCamera(): Int`
    - `fun setCamera(position: SipCameraPosition): Int`
    - `fun setEnabled(enabled: Boolean)`
    - `val isRunning: Boolean`, `val isEnabled: Boolean`, `val currentPosition: SipCameraPosition`
  - Kotlin `SipClient`:
    - `var cameraPosition: SipCameraPosition`, `var isLocalVideoEnabled: Boolean`, `val isLocalPreviewActive: Boolean`
    - `fun switchCamera(): Int`, `fun setCamera(position: SipCameraPosition): Int`
    - `fun setLocalVideoEnabled(enabled: Boolean): Int`
    - `fun startLocalPreview(): Int`, `fun stopLocalPreview(): Int`
  - JNI Native Methods:
    - `jint Java_com_sipsdk_SipClient_nativeSwitchCamera(JNIEnv *, jobject, jlong handle)`
    - `jint Java_com_sipsdk_SipClient_nativeSetCamera(JNIEnv *, jobject, jlong handle, jint position)`
    - `jint Java_com_sipsdk_SipClient_nativeSetLocalVideoEnabled(JNIEnv *, jobject, jlong handle, jboolean enabled)`
    - `jboolean Java_com_sipsdk_SipClient_nativeIsLocalVideoEnabled(JNIEnv *, jobject, jlong handle)`
    - `jint Java_com_sipsdk_SipClient_nativeStartLocalPreview(JNIEnv *, jobject, jlong handle)`
    - `jint Java_com_sipsdk_SipClient_nativeStopLocalPreview(JNIEnv *, jobject, jlong handle)`
    - `jboolean Java_com_sipsdk_SipClient_nativeIsLocalPreviewActive(JNIEnv *, jobject, jlong handle)`
    - `void Java_com_sipsdk_SipCameraCapture_nativeDeliverFrame(JNIEnv *, jclass, jlong handle, jint width, jint height, jobject yBuf, jobject uBuf, jobject vBuf, jint yStride, jint uStride, jint vStride, jlong timestampNs)`
- **Acceptance criteria**:
  1. **Given** an initialized `SipClient`, **when** `client.startLocalPreview()` is called, **then** `SipCameraCapture` initializes the camera sensor (or synthetic test generator on emulator), begins capturing frames, and `client.isLocalPreviewActive` evaluates to true.
  2. **Given** an active camera capture session with front camera selected, **when** `client.switchCamera()` is called, **then** the capture session flips to the rear camera sensor, and `client.cameraPosition` reflects `SipCameraPosition.BACK`.
  3. **Given** an active camera capture session, **when** `client.setLocalVideoEnabled(false)` is invoked, **then** frame transmission is paused and `client.isLocalVideoEnabled` returns false; when set back to true, frame delivery resumes seamlessly.
  4. **Given** the automated camera test suite, **when** `bash scripts/verify-android-sdk.sh --slice 5` is executed on Android emulator, **then** camera initialization, frame dispatch, camera flipping, and teardown succeed with exit code 0.
- **Verify**: `bash scripts/verify-android-sdk.sh --slice 5`
- **Blocked-by**: Slice 1

---

### Slice 3 — Turnkey `SipSurfaceView` & End-to-End Two-Way Video Calling (`android-surfaceview-video-calling`)

- **User stories covered**: US11, US17, US18, US28, US30, US34, US37
- **Device tier**: Android build/package tier & Android Simulator / Emulator / Host test tier
- **Files**:
  - `platforms/android/sipsdk/src/main/kotlin/com/sipsdk/SipSurfaceView.kt`
  - `platforms/android/sipsdk/src/main/kotlin/com/sipsdk/SipCall.kt`
  - `platforms/android/sipsdk/src/main/kotlin/com/sipsdk/SipClient.kt`
  - `platforms/android/sipsdk/src/main/cpp/sip_jni.cpp`
  - `platforms/android/sample/src/main/kotlin/com/sipsdk/sample/MainActivity.kt`
  - `platforms/android/sipsdk/src/androidTest/kotlin/com/sipsdk/SipCallVideoTest.kt`
  - `scripts/verify-android-sdk.sh`
- **Interfaces**:
  - Kotlin `class SipSurfaceView(context: Context, attrs: AttributeSet? = null) : SurfaceView(context, attrs)`:
    - `val videoWidth: Int`, `val videoHeight: Int`, `val renderedFrameCount: Long`
    - `fun clear()`
  - Kotlin `SipClient`:
    - `var remoteVideoView: SipSurfaceView?`, `var localVideoView: SipSurfaceView?`
    - `fun makeCall(uri: String, withVideo: Boolean = false): SipCall?`
  - Kotlin `SipCall`:
    - `fun answer(withVideo: Boolean = false): Int`
    - `fun requestKeyframe(): Int`
  - JNI Native Methods:
    - `jint Java_com_sipsdk_SipClient_nativeSetRemoteSurface(JNIEnv *, jobject, jlong handle, jobject surface)`
    - `jint Java_com_sipsdk_SipClient_nativeSetLocalSurface(JNIEnv *, jobject, jlong handle, jobject surface)`
    - `jint Java_com_sipsdk_SipCall_nativeRequestKeyframe(JNIEnv *, jobject, jlong callHandle)`
- **Acceptance criteria**:
  1. **Given** an assigned `SipSurfaceView` attached to `client.remoteVideoView`, **when** video frames are received, **then** `onMeasure` enforces aspect ratio preservation without distortion, and `renderedFrameCount` increments with each displayed frame.
  2. **Given** a registered `SipClient`, **when** `client.makeCall("sip:9001@10.0.2.2:5060", withVideo = true)` is called, **then** SDP negotiates `m=video` with H.264 payload alongside G.711 audio, `call.hasVideo` returns true, and `onCallMediaChanged` receives positive dimensions (`width > 0`, `height > 0`).
  3. **Given** an active two-way video call against Asterisk PBX video echo (`9001`), **when** video packets are received, **then** remote frames render onto `remoteVideoView`, camera flip switches sensors mid-call without freezing video, and `call.requestKeyframe()` triggers an RTCP PLI request.
  4. **Given** the sample application in `platforms/android/sample/`, **when** `./gradlew :sample:assembleDebug` is run, **then** the sample app compiles cleanly with local and remote `SipSurfaceView` UI widgets.
  5. **Given** the full verification suite, **when** `bash scripts/verify-android-sdk.sh` is executed, **then** all slices (1 through 6) pass 100% of test cases with zero regressions on existing iOS test harnesses.
- **Verify**: `bash scripts/verify-android-sdk.sh`
- **Blocked-by**: Slice 1, Slice 2

---

## 4. Review & Refutation Checklist

- [x] All 11 user stories for Issue #16 (US11, US17, US18, US27, US28, US29, US30, US31, US32, US34, US37) are covered across the 3 slices.
- [x] Each slice writes <= 5 production files and contains focused behaviors.
- [x] Exact file paths, explicit function signatures, observable Given/When/Then acceptance criteria, and exact verification commands specified.
- [x] Zero placeholders (no TODO, TBD, or vague error handling descriptions).
- [x] Header matches spec constraints verbatim (`minSdk = 21`, `compileSdk = 34`, `-lmediandk`, `-DUSE_UNIXSOCK=ON`).
- [x] Verification scripts assert zero regressions on existing iOS suites.
