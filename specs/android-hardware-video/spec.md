# Spec: Android Hardware Video & SurfaceView Rendering (`android-hardware-video`)

## 1. Problem & Architecture Overview

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

Glossary: [`../../CONTEXT.md`](../../CONTEXT.md). ADR: [`../../docs/adr/0001-cross-platform-cpp-core-c-abi.md`](../../docs/adr/0001-cross-platform-cpp-core-c-abi.md). Parent Epic: Issue #10. Issue: #16.

---

## 2. Public API Specification

### Kotlin Public API (`platforms/android/sipsdk/src/main/kotlin/com/sipsdk/`)

```kotlin
package com.sipsdk

import android.content.Context
import android.util.AttributeSet
import android.view.SurfaceView

enum class SipCameraPosition(val value: Int) {
    FRONT(0),
    BACK(1);

    companion object {
        fun fromInt(value: Int): SipCameraPosition = if (value == 1) BACK else FRONT
    }
}

class SipSurfaceView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
    defStyleAttr: Int = 0
) : SurfaceView(context, attrs, defStyleAttr) {
    val videoWidth: Int
    val videoHeight: Int
    val renderedFrameCount: Long

    fun clear()
}

class SipClient(
    val username: String,
    val domain: String,
    val context: Context? = null
) {
    // Video Views
    var remoteVideoView: SipSurfaceView?
    var localVideoView: SipSurfaceView?

    // Camera & Video Controls
    var cameraPosition: SipCameraPosition
    var isLocalVideoEnabled: Boolean
    val isLocalPreviewActive: Boolean

    fun switchCamera(): Int
    fun setCamera(position: SipCameraPosition): Int
    fun setLocalVideoEnabled(enabled: Boolean): Int
    fun startLocalPreview(): Int
    fun stopLocalPreview(): Int

    // Video Calling
    fun makeCall(uri: String, withVideo: Boolean = false): SipCall?
}

class SipCall internal constructor(
    internal val nativeHandle: Long,
    val callId: String,
    val remoteUri: String,
    val peerDisplayName: String? = null,
    val hasVideo: Boolean = false,
    val isOutgoing: Boolean = true
) {
    fun answer(withVideo: Boolean = false): Int
    fun requestKeyframe(): Int
}
```

### JNI Native Boundary (`platforms/android/sipsdk/src/main/cpp/`)

```cpp
extern "C" {
// Video codec & surface binding
JNIEXPORT jint JNICALL Java_com_sipsdk_SipClient_nativeSetRemoteSurface(JNIEnv *env, jobject thiz, jlong handle, jobject surface);
JNIEXPORT jint JNICALL Java_com_sipsdk_SipClient_nativeSetLocalSurface(JNIEnv *env, jobject thiz, jlong handle, jobject surface);

// Camera and local preview controls
JNIEXPORT jint JNICALL Java_com_sipsdk_SipClient_nativeSwitchCamera(JNIEnv *env, jobject thiz, jlong handle);
JNIEXPORT jint JNICALL Java_com_sipsdk_SipClient_nativeSetCamera(JNIEnv *env, jobject thiz, jlong handle, jint position);
JNIEXPORT jint JNICALL Java_com_sipsdk_SipClient_nativeSetLocalVideoEnabled(JNIEnv *env, jobject thiz, jlong handle, jboolean enabled);
JNIEXPORT jboolean JNICALL Java_com_sipsdk_SipClient_nativeIsLocalVideoEnabled(JNIEnv *env, jobject thiz, jlong handle);
JNIEXPORT jint JNICALL Java_com_sipsdk_SipClient_nativeStartLocalPreview(JNIEnv *env, jobject thiz, jlong handle);
JNIEXPORT jint JNICALL Java_com_sipsdk_SipClient_nativeStopLocalPreview(JNIEnv *env, jobject thiz, jlong handle);
JNIEXPORT jboolean JNICALL Java_com_sipsdk_SipClient_nativeIsLocalPreviewActive(JNIEnv *env, jobject thiz, jlong handle);

// Camera frame injection
JNIEXPORT void JNICALL Java_com_sipsdk_SipCameraCapture_nativeDeliverFrame(
    JNIEnv *env, jclass clazz, jlong clientHandle,
    jint width, jint height,
    jobject yBuffer, jobject uBuffer, jobject vBuffer,
    jint yStride, jint uStride, jint vStride,
    jlong timestampNs);

// Call keyframe request
JNIEXPORT jint JNICALL Java_com_sipsdk_SipCall_nativeRequestKeyframe(JNIEnv *env, jobject thiz, jlong callHandle);
}
```

---

## 3. Global Constraints & Non-Functional Requirements

- **NDK Toolchain & Minimum API**: Android NDK r26 / r27, minimum API 21 (`minSdk = 21`, `compileSdk = 34`).
- **Hardware Acceleration**: Encoding and decoding use `AMediaCodec` (`libmediandk.so`). Decoding renders directly to `ANativeWindow` without intermediate pixel buffer copies on the CPU.
- **RFC Compliance**: RFC 6184 H.264 RTP payload format (Single NAL unit mode and FU-A fragmentation units), Constrained Baseline Profile (`42e01f`).
- **Aspect Ratio Fidelity**: `SipSurfaceView` preserves video aspect ratio (4:3 or 16:9) without distortion or stretching.
- **Main Looper Dispatch**: All listener callbacks (`onCallMediaChanged`, `onCallStateChanged`) must be dispatched to `Looper.getMainLooper()`.
- **Zero iOS Regressions**: Existing iOS video test suites (`scripts/verify-ios-video-tests.sh`, `scripts/verify-spm-video-sdk.sh`, `scripts/verify-sim-app-launch.sh`) and core video tests must remain 100% green.
