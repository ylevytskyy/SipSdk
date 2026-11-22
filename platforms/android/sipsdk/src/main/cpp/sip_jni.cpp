#include <jni.h>
#include <android/log.h>
#include <android/native_window_jni.h>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <string>
#include <mutex>
#include "sip_abi.h"
#include "media_codec_driver.h"

#define LOG_TAG "SipSdkJni"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static JavaVM *g_jvm = nullptr;

struct JniEnvGuard {
    JNIEnv *env = nullptr;
    bool attached = false;

    JniEnvGuard() {
        if (!g_jvm) return;
        jint res = g_jvm->GetEnv((void **)&env, JNI_VERSION_1_6);
        if (res == JNI_EDETACHED) {
            JavaVMAttachArgs args;
            args.version = JNI_VERSION_1_6;
            args.name = (char *)"SipSdkWorker";
            args.group = nullptr;
            if (g_jvm->AttachCurrentThread(&env, &args) == JNI_OK) {
                attached = true;
            } else {
                env = nullptr;
            }
        }
    }

    ~JniEnvGuard() {
        if (attached && g_jvm) {
            g_jvm->DetachCurrentThread();
        }
    }
};

struct AndroidCaptureState {
    void (*frame_handler)(const sip_video_frame_t *frame, void *arg){nullptr};
    void *frame_arg{nullptr};
    std::atomic<bool> enabled{true};
    std::atomic<int> camera_position{0};
    std::mutex mutex;
    std::vector<uint8_t> yPlane;
    std::vector<uint8_t> uPlane;
    std::vector<uint8_t> vPlane;
};

static int android_capture_start(void *user_data, int width, int height, int fps) {
    (void)user_data; (void)width; (void)height; (void)fps;
    return 0;
}

static void android_capture_stop(void *user_data) {
    (void)user_data;
}

static void android_capture_set_frame_handler(void *user_data, void (*handler)(const sip_video_frame_t *, void *), void *arg) {
    auto *st = static_cast<AndroidCaptureState *>(user_data);
    if (!st) return;
    std::lock_guard<std::mutex> lock(st->mutex);
    st->frame_handler = handler;
    st->frame_arg = arg;
}

static void android_capture_switch_camera(void *user_data) {
    auto *st = static_cast<AndroidCaptureState *>(user_data);
    if (!st) return;
    st->camera_position = (st->camera_position.load() == 0) ? 1 : 0;
}

static void android_capture_set_camera(void *user_data, int position) {
    auto *st = static_cast<AndroidCaptureState *>(user_data);
    if (!st) return;
    st->camera_position = position;
}

static void android_capture_set_enabled(void *user_data, bool enabled) {
    auto *st = static_cast<AndroidCaptureState *>(user_data);
    if (!st) return;
    st->enabled = enabled;
}

static bool android_capture_is_enabled(void *user_data) {
    auto *st = static_cast<AndroidCaptureState *>(user_data);
    return st ? st->enabled.load() : true;
}

struct DestroyWaiter {
    std::mutex mutex;
    std::condition_variable cv;
    bool done{false};
};

struct ClientJniContext {
    jobject javaClientGlobalRef = nullptr;
    jobject mainHandlerGlobalRef = nullptr;
    jclass clientClassGlobalRef = nullptr;
    jclass taskRunnableClassGlobalRef = nullptr;

    jmethodID postMethodId = nullptr;
    jmethodID taskRunnableCtorId = nullptr;
    jmethodID onRegStateMethodId = nullptr;
    jmethodID onCallStateMethodId = nullptr;
    jmethodID onIncomingCallMethodId = nullptr;
    jmethodID onCallMediaMethodId = nullptr;
    jmethodID onDtmfDigitMethodId = nullptr;
    jmethodID onCallStatsMethodId = nullptr;
    jmethodID onTransferStatusMethodId = nullptr;
    jmethodID onTransferRequestMethodId = nullptr;
    jmethodID onConferenceChangedMethodId = nullptr;
    jmethodID onCallRejectedBusyMethodId = nullptr;
    jmethodID onMessageReceivedMethodId = nullptr;
    jmethodID onMessageStatusMethodId = nullptr;
    jmethodID onRecordingStateMethodId = nullptr;
    jmethodID onRecorderStartMethodId = nullptr;
    jmethodID onRecorderAudioMethodId = nullptr;
    jmethodID onRecorderVideoMethodId = nullptr;
    jmethodID onRecorderStopMethodId = nullptr;
    jmethodID prepareAudioSessionMethodId = nullptr;
    jmethodID releaseAudioSessionMethodId = nullptr;

    sip_client_t *coreClient = nullptr;
    AndroidCaptureState captureState;
    std::atomic<bool> destroyed{false};
    std::mutex mutex;
    std::shared_ptr<DestroyWaiter> destroyWaiter{std::make_shared<DestroyWaiter>()};
};

static void jni_cleanup_context_on_main(void *arg) {
    auto *ctx = static_cast<ClientJniContext *>(arg);
    if (!ctx) return;

    auto waiter = ctx->destroyWaiter;
    JNIEnv *env = nullptr;
    bool attached = false;
    if (g_jvm) {
        if (g_jvm->GetEnv((void **)&env, JNI_VERSION_1_6) == JNI_EDETACHED) {
            JavaVMAttachArgs args{JNI_VERSION_1_6, (char *)"SipSdkCleanup", nullptr};
            if (g_jvm->AttachCurrentThread(&env, &args) == JNI_OK) {
                attached = true;
            } else {
                env = nullptr;
            }
        }
    }

    if (env) {
        if (ctx->javaClientGlobalRef) {
            env->DeleteGlobalRef(ctx->javaClientGlobalRef);
            ctx->javaClientGlobalRef = nullptr;
        }
        if (ctx->mainHandlerGlobalRef) {
            env->DeleteGlobalRef(ctx->mainHandlerGlobalRef);
            ctx->mainHandlerGlobalRef = nullptr;
        }
        if (ctx->clientClassGlobalRef) {
            env->DeleteGlobalRef(ctx->clientClassGlobalRef);
            ctx->clientClassGlobalRef = nullptr;
        }
        if (ctx->taskRunnableClassGlobalRef) {
            env->DeleteGlobalRef(ctx->taskRunnableClassGlobalRef);
            ctx->taskRunnableClassGlobalRef = nullptr;
        }
    }

    if (attached && g_jvm) {
        g_jvm->DetachCurrentThread();
    }

    delete ctx;

    if (waiter) {
        {
            std::lock_guard<std::mutex> lock(waiter->mutex);
            waiter->done = true;
        }
        waiter->cv.notify_all();
    }
}

static void jni_dispatch_to_main(void (*task)(void *arg), void *arg, void *user_data) {
    auto *ctx = static_cast<ClientJniContext *>(user_data);
    if (!ctx || ctx->destroyed.load()) return;
    if (!ctx->mainHandlerGlobalRef || !ctx->taskRunnableClassGlobalRef || !ctx->postMethodId) {
        if (task) task(arg);
        return;
    }

    JniEnvGuard guard;
    if (!guard.env) {
        if (task) task(arg);
        return;
    }

    jobject runnable = guard.env->NewObject(
        ctx->taskRunnableClassGlobalRef,
        ctx->taskRunnableCtorId,
        reinterpret_cast<jlong>(task),
        reinterpret_cast<jlong>(arg)
    );

    if (runnable) {
        jboolean posted = guard.env->CallBooleanMethod(ctx->mainHandlerGlobalRef, ctx->postMethodId, runnable);
        if (posted != JNI_TRUE && task) {
            task(arg);
        }
        guard.env->DeleteLocalRef(runnable);
    } else {
        if (task) task(arg);
    }
}

static void jni_on_reg_state(sip_client_t *client, sip_reg_state_t state, int code, const char *reason, void *user_data) {
    (void)client;
    auto *ctx = static_cast<ClientJniContext *>(user_data);
    if (!ctx || ctx->destroyed.load() || !ctx->javaClientGlobalRef || !ctx->onRegStateMethodId) return;

    JniEnvGuard guard;
    if (!guard.env) return;

    jstring jReason = guard.env->NewStringUTF(reason ? reason : "");
    guard.env->CallVoidMethod(ctx->javaClientGlobalRef, ctx->onRegStateMethodId, (jint)state, (jint)code, jReason);
    if (jReason) guard.env->DeleteLocalRef(jReason);
}

static void jni_on_incoming_call(sip_client_t *client, sip_call_t *call, void *user_data) {
    (void)client;
    auto *ctx = static_cast<ClientJniContext *>(user_data);
    if (!ctx || ctx->destroyed.load() || !ctx->javaClientGlobalRef || !ctx->onIncomingCallMethodId) return;

    JniEnvGuard guard;
    if (!guard.env) return;

    guard.env->CallVoidMethod(ctx->javaClientGlobalRef, ctx->onIncomingCallMethodId, reinterpret_cast<jlong>(call));
}

static void jni_on_call_state(sip_call_t *call, sip_call_state_t state, int code, const char *reason, void *user_data) {
    auto *ctx = static_cast<ClientJniContext *>(user_data);
    if (!ctx || ctx->destroyed.load() || !ctx->javaClientGlobalRef || !ctx->onCallStateMethodId) return;

    JniEnvGuard guard;
    if (!guard.env) return;

    jstring jReason = guard.env->NewStringUTF(reason ? reason : "");
    guard.env->CallVoidMethod(ctx->javaClientGlobalRef, ctx->onCallStateMethodId, reinterpret_cast<jlong>(call), (jint)state, (jint)code, jReason);
    if (jReason) guard.env->DeleteLocalRef(jReason);
}

static void jni_on_call_media(sip_call_t *call, int width, int height, bool on_hold, bool is_muted, void *user_data) {
    auto *ctx = static_cast<ClientJniContext *>(user_data);
    if (!ctx || ctx->destroyed.load() || !ctx->javaClientGlobalRef || !ctx->onCallMediaMethodId) return;

    JniEnvGuard guard;
    if (!guard.env) return;

    guard.env->CallVoidMethod(
        ctx->javaClientGlobalRef,
        ctx->onCallMediaMethodId,
        reinterpret_cast<jlong>(call),
        (jint)width,
        (jint)height,
        (jboolean)on_hold,
        (jboolean)is_muted
    );
}

static void jni_on_dtmf_digit(sip_call_t *call, const char *digit, void *user_data) {
    auto *ctx = static_cast<ClientJniContext *>(user_data);
    if (!ctx || ctx->destroyed.load() || !ctx->javaClientGlobalRef || !ctx->onDtmfDigitMethodId) return;

    JniEnvGuard guard;
    if (!guard.env) return;

    jstring jDigit = guard.env->NewStringUTF(digit ? digit : "");
    guard.env->CallVoidMethod(ctx->javaClientGlobalRef, ctx->onDtmfDigitMethodId, reinterpret_cast<jlong>(call), jDigit);
    if (jDigit) guard.env->DeleteLocalRef(jDigit);
}

static void jni_on_call_stats(sip_call_t *call, const sip_call_stats_t *stats, void *user_data) {
    auto *ctx = static_cast<ClientJniContext *>(user_data);
    if (!ctx || ctx->destroyed.load() || !ctx->javaClientGlobalRef || !ctx->onCallStatsMethodId || !stats) return;

    JniEnvGuard guard;
    if (!guard.env) return;

    jstring jAudioCodec = guard.env->NewStringUTF(stats->audio_codec);
    jstring jVideoCodec = guard.env->NewStringUTF(stats->video_codec);
    guard.env->CallVoidMethod(
        ctx->javaClientGlobalRef,
        ctx->onCallStatsMethodId,
        reinterpret_cast<jlong>(call),
        (jfloat)stats->audio_loss_rate,
        (jint)stats->audio_jitter_ms,
        (jint)stats->audio_rtt_ms,
        (jint)stats->audio_bitrate_kbps,
        jAudioCodec,
        (jfloat)stats->video_loss_rate,
        (jint)stats->video_jitter_ms,
        (jint)stats->video_bitrate_kbps,
        (jint)stats->video_fps,
        jVideoCodec
    );
    if (jAudioCodec) guard.env->DeleteLocalRef(jAudioCodec);
    if (jVideoCodec) guard.env->DeleteLocalRef(jVideoCodec);
}

static void jni_on_transfer_status(sip_call_t *call, int code, const char *reason, void *user_data) {
    auto *ctx = static_cast<ClientJniContext *>(user_data);
    if (!ctx || ctx->destroyed.load() || !ctx->javaClientGlobalRef || !ctx->onTransferStatusMethodId) return;

    JniEnvGuard guard;
    if (!guard.env) return;

    jstring jReason = guard.env->NewStringUTF(reason ? reason : "");
    guard.env->CallVoidMethod(
        ctx->javaClientGlobalRef,
        ctx->onTransferStatusMethodId,
        reinterpret_cast<jlong>(call),
        (jint)code,
        jReason
    );
    if (jReason) guard.env->DeleteLocalRef(jReason);
}

static void jni_on_transfer_request(sip_call_t *call, const char *target_uri, bool has_replaces, void *user_data) {
    (void)has_replaces;
    auto *ctx = static_cast<ClientJniContext *>(user_data);
    if (!ctx || ctx->destroyed.load() || !ctx->javaClientGlobalRef || !ctx->onTransferRequestMethodId) return;

    JniEnvGuard guard;
    if (!guard.env) return;

    jstring jTarget = guard.env->NewStringUTF(target_uri ? target_uri : "");
    guard.env->CallVoidMethod(
        ctx->javaClientGlobalRef,
        ctx->onTransferRequestMethodId,
        reinterpret_cast<jlong>(call),
        jTarget
    );
    if (jTarget) guard.env->DeleteLocalRef(jTarget);
}

// Network-supplied text goes to Kotlin as bytes: NewStringUTF aborts under CheckJNI
// on invalid modified UTF-8, and a remote peer controls these bytes.
static jbyteArray jni_bytes(JNIEnv *env, const void *data, size_t len) {
    jbyteArray arr = env->NewByteArray(static_cast<jsize>(len));
    if (arr && data && len > 0) {
        env->SetByteArrayRegion(arr, 0, static_cast<jsize>(len), reinterpret_cast<const jbyte *>(data));
    }
    return arr;
}

static void jni_on_call_rejected_busy(sip_client_t *client, const char *caller_uri, void *user_data) {
    (void)client;
    auto *ctx = static_cast<ClientJniContext *>(user_data);
    if (!ctx || ctx->destroyed.load() || !ctx->javaClientGlobalRef || !ctx->onCallRejectedBusyMethodId) return;

    JniEnvGuard guard;
    if (!guard.env) return;

    const char *caller = caller_uri ? caller_uri : "";
    jbyteArray jCaller = jni_bytes(guard.env, caller, std::strlen(caller));
    if (jCaller) {
        guard.env->CallVoidMethod(ctx->javaClientGlobalRef, ctx->onCallRejectedBusyMethodId, jCaller);
        guard.env->DeleteLocalRef(jCaller);
    }
}

static void jni_on_conference_changed(sip_client_t *client, sip_call_t *const *calls, size_t count, void *user_data) {
    (void)client;
    auto *ctx = static_cast<ClientJniContext *>(user_data);
    if (!ctx || ctx->destroyed.load() || !ctx->javaClientGlobalRef || !ctx->onConferenceChangedMethodId) return;

    JniEnvGuard guard;
    if (!guard.env) return;

    jlongArray arr = guard.env->NewLongArray(static_cast<jsize>(count));
    if (!arr) return;
    if (count > 0 && calls) {
        std::vector<jlong> handles(count);
        for (size_t i = 0; i < count; ++i) {
            handles[i] = reinterpret_cast<jlong>(calls[i]);
        }
        guard.env->SetLongArrayRegion(arr, 0, static_cast<jsize>(count), handles.data());
    }
    guard.env->CallVoidMethod(ctx->javaClientGlobalRef, ctx->onConferenceChangedMethodId, arr);
    guard.env->DeleteLocalRef(arr);
}

static void jni_on_message_received(sip_client_t *client, const char *from_uri, const char *content_type,
                                    const uint8_t *body, size_t body_len, void *user_data) {
    (void)client;
    auto *ctx = static_cast<ClientJniContext *>(user_data);
    if (!ctx || ctx->destroyed.load() || !ctx->javaClientGlobalRef || !ctx->onMessageReceivedMethodId) return;

    JniEnvGuard guard;
    if (!guard.env) return;

    const char *from = from_uri ? from_uri : "";
    const char *ctype = content_type ? content_type : "text/plain";
    jbyteArray jFrom = jni_bytes(guard.env, from, std::strlen(from));
    jbyteArray jType = jni_bytes(guard.env, ctype, std::strlen(ctype));
    jbyteArray jBody = jni_bytes(guard.env, body, body_len);
    if (jFrom && jType && jBody) {
        guard.env->CallVoidMethod(ctx->javaClientGlobalRef, ctx->onMessageReceivedMethodId, jFrom, jType, jBody);
    }
    if (jFrom) guard.env->DeleteLocalRef(jFrom);
    if (jType) guard.env->DeleteLocalRef(jType);
    if (jBody) guard.env->DeleteLocalRef(jBody);
}

static void jni_on_message_status(sip_client_t *client, uint64_t message_id, sip_message_status_t status,
                                  int code, const char *reason, void *user_data) {
    (void)client;
    auto *ctx = static_cast<ClientJniContext *>(user_data);
    if (!ctx || ctx->destroyed.load() || !ctx->javaClientGlobalRef || !ctx->onMessageStatusMethodId) return;

    JniEnvGuard guard;
    if (!guard.env) return;

    const char *r = reason ? reason : "";
    jbyteArray jReason = jni_bytes(guard.env, r, std::strlen(r));
    if (jReason) {
        guard.env->CallVoidMethod(ctx->javaClientGlobalRef, ctx->onMessageStatusMethodId,
                                  static_cast<jlong>(message_id), (jint)status, (jint)code, jReason);
        guard.env->DeleteLocalRef(jReason);
    }
}

static void jni_on_recording_state(sip_client_t *client, sip_call_t *call, sip_recording_state_t state,
                                   const char *path, int error_code, void *user_data) {
    (void)client;
    auto *ctx = static_cast<ClientJniContext *>(user_data);
    if (!ctx || ctx->destroyed.load() || !ctx->javaClientGlobalRef || !ctx->onRecordingStateMethodId) return;

    JniEnvGuard guard;
    if (!guard.env) return;

    const char *p = path ? path : "";
    jbyteArray jPath = jni_bytes(guard.env, p, std::strlen(p));
    if (jPath) {
        guard.env->CallVoidMethod(ctx->javaClientGlobalRef, ctx->onRecordingStateMethodId,
                                  reinterpret_cast<jlong>(call), (jint)state, jPath, (jint)error_code);
        guard.env->DeleteLocalRef(jPath);
    }
}

static int jni_recorder_start(void *user_data, const char *path, int sample_rate, int channels,
                              int video_width, int video_height) {
    auto *ctx = static_cast<ClientJniContext *>(user_data);
    if (!ctx || ctx->destroyed.load() || !ctx->javaClientGlobalRef || !ctx->onRecorderStartMethodId) return EIO;
    JniEnvGuard guard;
    if (!guard.env) return EIO;
    const char *p = path ? path : "";
    jbyteArray jPath = jni_bytes(guard.env, p, std::strlen(p));
    if (!jPath) return ENOMEM;
    jint rc = guard.env->CallIntMethod(ctx->javaClientGlobalRef, ctx->onRecorderStartMethodId,
                                       jPath, (jint)sample_rate, (jint)channels, (jint)video_width, (jint)video_height);
    guard.env->DeleteLocalRef(jPath);
    return (int)rc;
}

static void jni_recorder_write_audio(void *user_data, const int16_t *pcm, size_t sample_count, uint64_t ts_ns) {
    auto *ctx = static_cast<ClientJniContext *>(user_data);
    if (!ctx || ctx->destroyed.load() || !ctx->javaClientGlobalRef || !ctx->onRecorderAudioMethodId || !pcm || sample_count == 0) return;
    JniEnvGuard guard;
    if (!guard.env) return;
    jbyteArray jPcm = jni_bytes(guard.env, pcm, sample_count * sizeof(int16_t));
    if (jPcm) {
        guard.env->CallVoidMethod(ctx->javaClientGlobalRef, ctx->onRecorderAudioMethodId,
                                  jPcm, static_cast<jlong>(ts_ns));
        guard.env->DeleteLocalRef(jPcm);
    }
}

static void jni_recorder_write_video(void *user_data, const sip_video_frame_t *frame) {
    auto *ctx = static_cast<ClientJniContext *>(user_data);
    if (!ctx || ctx->destroyed.load() || !ctx->javaClientGlobalRef || !ctx->onRecorderVideoMethodId || !frame) return;
    if (!frame->y || !frame->u || !frame->v || frame->width <= 0 || frame->height <= 0) return;
    JniEnvGuard guard;
    if (!guard.env) return;
    const int w = frame->width;
    const int h = frame->height;
    const int uv_w = w / 2;
    const int uv_h = h / 2;
    std::vector<uint8_t> packed;
    packed.reserve(static_cast<size_t>(w * h + 2 * uv_w * uv_h));
    for (int r = 0; r < h; ++r) {
        const uint8_t *row = frame->y + static_cast<size_t>(r) * static_cast<size_t>(frame->y_stride);
        packed.insert(packed.end(), row, row + w);
    }
    for (int r = 0; r < uv_h; ++r) {
        const uint8_t *row = frame->u + static_cast<size_t>(r) * static_cast<size_t>(frame->u_stride);
        packed.insert(packed.end(), row, row + uv_w);
    }
    for (int r = 0; r < uv_h; ++r) {
        const uint8_t *row = frame->v + static_cast<size_t>(r) * static_cast<size_t>(frame->v_stride);
        packed.insert(packed.end(), row, row + uv_w);
    }
    jbyteArray jFrame = jni_bytes(guard.env, packed.data(), packed.size());
    if (jFrame) {
        guard.env->CallVoidMethod(ctx->javaClientGlobalRef, ctx->onRecorderVideoMethodId,
                                  jFrame, (jint)w, (jint)h, static_cast<jlong>(frame->timestamp_ns));
        guard.env->DeleteLocalRef(jFrame);
    }
}

static int jni_recorder_stop(void *user_data) {
    auto *ctx = static_cast<ClientJniContext *>(user_data);
    if (!ctx || ctx->destroyed.load() || !ctx->javaClientGlobalRef || !ctx->onRecorderStopMethodId) return 0;
    JniEnvGuard guard;
    if (!guard.env) return 0;
    return (int)guard.env->CallIntMethod(ctx->javaClientGlobalRef, ctx->onRecorderStopMethodId);
}

static bool jni_prepare_audio_session(sip_client_t *client, void *user_data) {
    (void)client;
    auto *ctx = static_cast<ClientJniContext *>(user_data);
    if (!ctx || ctx->destroyed.load() || !ctx->javaClientGlobalRef || !ctx->prepareAudioSessionMethodId) return true;

    JniEnvGuard guard;
    if (!guard.env) return true;

    return guard.env->CallBooleanMethod(ctx->javaClientGlobalRef, ctx->prepareAudioSessionMethodId) == JNI_TRUE;
}

static void jni_release_audio_session(sip_client_t *client, void *user_data) {
    (void)client;
    auto *ctx = static_cast<ClientJniContext *>(user_data);
    if (!ctx || ctx->destroyed.load() || !ctx->javaClientGlobalRef || !ctx->releaseAudioSessionMethodId) return;

    JniEnvGuard guard;
    if (!guard.env) return;

    guard.env->CallVoidMethod(ctx->javaClientGlobalRef, ctx->releaseAudioSessionMethodId);
}

JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *reserved) {
    (void)reserved;
    g_jvm = vm;
    LOGI("SipSdk JNI loaded");
    return JNI_VERSION_1_6;
}

JNIEXPORT void JNI_OnUnload(JavaVM *vm, void *reserved) {
    (void)vm;
    (void)reserved;
    g_jvm = nullptr;
    LOGI("SipSdk JNI unloaded");
}

extern "C" {

JNIEXPORT jstring JNICALL
Java_com_sipsdk_SipSdk_nativeGetVersion(JNIEnv *env, jclass clazz) {
    (void)clazz;
    return env->NewStringUTF("4.12.0-cross-platform");
}

JNIEXPORT void JNICALL
Java_com_sipsdk_NativeTaskRunnable_nativeRun(JNIEnv *env, jobject thiz, jlong taskPtr, jlong argPtr) {
    (void)env;
    (void)thiz;
    auto task = reinterpret_cast<void (*)(void *)>(taskPtr);
    auto arg = reinterpret_cast<void *>(argPtr);
    if (task) {
        task(arg);
    }
}

JNIEXPORT jlong JNICALL
Java_com_sipsdk_SipClient_nativeCreate(JNIEnv *env, jobject thiz) {
    auto *ctx = new ClientJniContext();
    ctx->javaClientGlobalRef = env->NewGlobalRef(thiz);

    // Get SipClient class and method IDs
    jclass clientClass = env->GetObjectClass(thiz);
    ctx->clientClassGlobalRef = reinterpret_cast<jclass>(env->NewGlobalRef(clientClass));
    ctx->onRegStateMethodId = env->GetMethodID(clientClass, "onNativeRegState", "(IILjava/lang/String;)V");
    ctx->onCallStateMethodId = env->GetMethodID(clientClass, "onNativeCallState", "(JIILjava/lang/String;)V");
    ctx->onIncomingCallMethodId = env->GetMethodID(clientClass, "onNativeIncomingCall", "(J)V");
    ctx->onCallMediaMethodId = env->GetMethodID(clientClass, "onNativeCallMedia", "(JIIZZ)V");
    ctx->onDtmfDigitMethodId = env->GetMethodID(clientClass, "onNativeDtmfDigit", "(JLjava/lang/String;)V");
    ctx->onCallStatsMethodId = env->GetMethodID(clientClass, "onNativeCallStats", "(JFIIILjava/lang/String;FIIILjava/lang/String;)V");
    ctx->onTransferStatusMethodId = env->GetMethodID(clientClass, "onNativeTransferStatus", "(JILjava/lang/String;)V");
    ctx->onTransferRequestMethodId = env->GetMethodID(clientClass, "onNativeTransferRequest", "(JLjava/lang/String;)V");
    ctx->onCallRejectedBusyMethodId = env->GetMethodID(clientClass, "onNativeCallRejectedBusy", "([B)V");
    ctx->onConferenceChangedMethodId = env->GetMethodID(clientClass, "onNativeConferenceChanged", "([J)V");
    ctx->onMessageReceivedMethodId = env->GetMethodID(clientClass, "onNativeMessageReceived", "([B[B[B)V");
    ctx->onMessageStatusMethodId = env->GetMethodID(clientClass, "onNativeMessageStatus", "(JII[B)V");
    ctx->onRecordingStateMethodId = env->GetMethodID(clientClass, "onNativeRecordingState", "(JI[BI)V");
    ctx->onRecorderStartMethodId = env->GetMethodID(clientClass, "onNativeRecorderStart", "([BIIII)I");
    ctx->onRecorderAudioMethodId = env->GetMethodID(clientClass, "onNativeRecorderAudio", "([BJ)V");
    ctx->onRecorderVideoMethodId = env->GetMethodID(clientClass, "onNativeRecorderVideo", "([BIIJ)V");
    ctx->onRecorderStopMethodId = env->GetMethodID(clientClass, "onNativeRecorderStop", "()I");
    ctx->prepareAudioSessionMethodId = env->GetMethodID(clientClass, "onNativePrepareAudioSession", "()Z");
    ctx->releaseAudioSessionMethodId = env->GetMethodID(clientClass, "onNativeReleaseAudioSession", "()V");

    // Get mainHandler field from SipClient
    jfieldID handlerField = env->GetFieldID(clientClass, "mainHandler", "Landroid/os/Handler;");
    if (handlerField) {
        jobject handlerObj = env->GetObjectField(thiz, handlerField);
        if (handlerObj) {
            ctx->mainHandlerGlobalRef = env->NewGlobalRef(handlerObj);
            jclass handlerClass = env->GetObjectClass(handlerObj);
            ctx->postMethodId = env->GetMethodID(handlerClass, "post", "(Ljava/lang/Runnable;)Z");
            env->DeleteLocalRef(handlerClass);
            env->DeleteLocalRef(handlerObj);
        }
    }

    // Get NativeTaskRunnable class and ctor
    jclass taskClass = env->FindClass("com/sipsdk/NativeTaskRunnable");
    if (taskClass) {
        ctx->taskRunnableClassGlobalRef = reinterpret_cast<jclass>(env->NewGlobalRef(taskClass));
        ctx->taskRunnableCtorId = env->GetMethodID(taskClass, "<init>", "(JJ)V");
        env->DeleteLocalRef(taskClass);
    }
    env->DeleteLocalRef(clientClass);

    sip_callbacks_t callbacks = {};
    callbacks.dispatch_to_main = jni_dispatch_to_main;
    callbacks.on_reg_state = jni_on_reg_state;
    callbacks.on_incoming_call = jni_on_incoming_call;
    callbacks.on_call_state = jni_on_call_state;
    callbacks.on_call_media = jni_on_call_media;
    callbacks.on_dtmf_digit = jni_on_dtmf_digit;
    callbacks.on_call_stats = jni_on_call_stats;
    callbacks.on_transfer_status = jni_on_transfer_status;
    callbacks.on_transfer_request = jni_on_transfer_request;
    callbacks.on_call_rejected_busy = jni_on_call_rejected_busy;
    callbacks.on_conference_changed = jni_on_conference_changed;
    callbacks.prepare_audio_session = jni_prepare_audio_session;
    callbacks.release_audio_session = jni_release_audio_session;

    ctx->coreClient = sip_client_create(&callbacks, ctx);
    if (ctx->coreClient) {
        sip_message_callbacks_t messageCallbacks = {};
        messageCallbacks.on_message_received = jni_on_message_received;
        messageCallbacks.on_message_status = jni_on_message_status;
        sip_client_set_message_callbacks(ctx->coreClient, &messageCallbacks);

        sip_recording_callbacks_t recCallbacks = {};
        recCallbacks.on_recording_state = jni_on_recording_state;
        sip_client_set_recording_callbacks(ctx->coreClient, &recCallbacks);

        sip_media_recorder_driver_t recDriver = {};
        recDriver.user_data = ctx;
        recDriver.start = jni_recorder_start;
        recDriver.write_audio = jni_recorder_write_audio;
        recDriver.write_video = jni_recorder_write_video;
        recDriver.stop = jni_recorder_stop;
        sip_client_set_media_recorder_driver(ctx->coreClient, &recDriver);

        sip_video_codec_driver_t codecDriver = sipsdk::AndroidMediaCodecDriver::get_c_driver();
        sip_client_set_video_codec_driver(ctx->coreClient, &codecDriver);

        sip_video_capture_driver_t capDriver{};
        capDriver.user_data = &ctx->captureState;
        capDriver.start = android_capture_start;
        capDriver.stop = android_capture_stop;
        capDriver.set_frame_handler = android_capture_set_frame_handler;
        capDriver.switch_camera = android_capture_switch_camera;
        capDriver.set_camera = android_capture_set_camera;
        capDriver.set_enabled = android_capture_set_enabled;
        capDriver.is_enabled = android_capture_is_enabled;
        sip_client_set_video_capture_driver(ctx->coreClient, &capDriver);

        // OpenSL ES audio driver
        sip_client_set_audio_driver(ctx->coreClient, "opensles", "", "opensles", "");
    }
    return reinterpret_cast<jlong>(ctx);
}

JNIEXPORT jint JNICALL
Java_com_sipsdk_SipClient_nativeInit(JNIEnv *env, jobject thiz, jlong handle, jstring configPath) {
    (void)thiz;
    auto *ctx = reinterpret_cast<ClientJniContext *>(handle);
    if (!ctx || !ctx->coreClient) return -1;

    const char *cfg = configPath ? env->GetStringUTFChars(configPath, nullptr) : nullptr;
    int res = sip_client_init(ctx->coreClient, cfg);
    if (cfg) env->ReleaseStringUTFChars(configPath, cfg);
    return res;
}

JNIEXPORT jint JNICALL
Java_com_sipsdk_SipClient_nativeRegister(JNIEnv *env, jobject thiz, jlong handle,
                                        jstring user, jstring domain, jstring pwd,
                                        jstring transport, jint port) {
    (void)thiz;
    auto *ctx = reinterpret_cast<ClientJniContext *>(handle);
    if (!ctx || !ctx->coreClient) return -1;

    const char *u = user ? env->GetStringUTFChars(user, nullptr) : "";
    const char *d = domain ? env->GetStringUTFChars(domain, nullptr) : "";
    const char *p = pwd ? env->GetStringUTFChars(pwd, nullptr) : "";
    const char *t = transport ? env->GetStringUTFChars(transport, nullptr) : "udp";

    int res = sip_client_register(ctx->coreClient, u, d, p, t, port);

    if (user) env->ReleaseStringUTFChars(user, u);
    if (domain) env->ReleaseStringUTFChars(domain, d);
    if (pwd) env->ReleaseStringUTFChars(pwd, p);
    if (transport) env->ReleaseStringUTFChars(transport, t);
    return res;
}

JNIEXPORT jint JNICALL
Java_com_sipsdk_SipClient_nativeRegisterAccount(JNIEnv *env, jobject thiz, jlong handle,
                                               jstring user, jstring domain, jstring pwd,
                                               jstring displayName, jstring transport, jint port,
                                               jstring tlsCaFile, jboolean verifyTlsCert,
                                               jstring mediaEnc, jint maxCalls) {
    (void)thiz;
    auto *ctx = reinterpret_cast<ClientJniContext *>(handle);
    if (!ctx || !ctx->coreClient) return -1;

    const char *u = user ? env->GetStringUTFChars(user, nullptr) : "";
    const char *d = domain ? env->GetStringUTFChars(domain, nullptr) : "";
    const char *p = pwd ? env->GetStringUTFChars(pwd, nullptr) : "";
    const char *dn = displayName ? env->GetStringUTFChars(displayName, nullptr) : "";
    const char *t = transport ? env->GetStringUTFChars(transport, nullptr) : "udp";
    const char *ca = tlsCaFile ? env->GetStringUTFChars(tlsCaFile, nullptr) : "";
    const char *me = mediaEnc ? env->GetStringUTFChars(mediaEnc, nullptr) : "";

    sip_account_config_t cfg{};
    cfg.user = u;
    cfg.domain = d;
    cfg.pwd = p;
    cfg.transport = t;
    cfg.port = port;
    cfg.display_name = (dn && dn[0]) ? dn : nullptr;
    cfg.tls_ca_file = (ca && ca[0]) ? ca : nullptr;
    cfg.verify_tls_cert = (verifyTlsCert == JNI_TRUE);
    cfg.media_enc = (me && me[0]) ? me : nullptr;
    cfg.max_calls = maxCalls > 0 ? maxCalls : 4;

    int res = sip_client_register_account(ctx->coreClient, &cfg);

    if (user) env->ReleaseStringUTFChars(user, u);
    if (domain) env->ReleaseStringUTFChars(domain, d);
    if (pwd) env->ReleaseStringUTFChars(pwd, p);
    if (displayName) env->ReleaseStringUTFChars(displayName, dn);
    if (transport) env->ReleaseStringUTFChars(transport, t);
    if (tlsCaFile) env->ReleaseStringUTFChars(tlsCaFile, ca);
    if (mediaEnc) env->ReleaseStringUTFChars(mediaEnc, me);
    return res;
}

JNIEXPORT jint JNICALL
Java_com_sipsdk_SipClient_nativeUnregister(JNIEnv *env, jobject thiz, jlong handle) {
    (void)env;
    (void)thiz;
    auto *ctx = reinterpret_cast<ClientJniContext *>(handle);
    if (!ctx || !ctx->coreClient) return -1;
    return sip_client_unregister(ctx->coreClient);
}

JNIEXPORT jboolean JNICALL
Java_com_sipsdk_SipClient_nativeIsRegistered(JNIEnv *env, jobject thiz, jlong handle) {
    (void)env;
    (void)thiz;
    auto *ctx = reinterpret_cast<ClientJniContext *>(handle);
    if (!ctx || !ctx->coreClient) return JNI_FALSE;
    return sip_client_is_registered(ctx->coreClient) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL
Java_com_sipsdk_SipClient_nativeResetTransports(JNIEnv *env, jobject thiz, jlong handle, jboolean reRegister) {
    (void)env;
    (void)thiz;
    auto *ctx = reinterpret_cast<ClientJniContext *>(handle);
    if (!ctx || !ctx->coreClient) return -1;
    return sip_client_reset_transports(ctx->coreClient, reRegister ? true : false);
}

JNIEXPORT void JNICALL
Java_com_sipsdk_SipClient_nativeSetMicMuted(JNIEnv *env, jobject thiz, jlong handle, jboolean muted) {
    (void)env;
    (void)thiz;
    auto *ctx = reinterpret_cast<ClientJniContext *>(handle);
    if (!ctx || !ctx->coreClient) return;
    sip_client_set_mic_muted(ctx->coreClient, muted == JNI_TRUE);
}

JNIEXPORT jlong JNICALL
Java_com_sipsdk_SipClient_nativeMakeCall(JNIEnv *env, jobject thiz, jlong handle, jstring uri, jboolean withVideo) {
    (void)thiz;
    auto *ctx = reinterpret_cast<ClientJniContext *>(handle);
    if (!ctx || !ctx->coreClient) return 0;

    const char *target = uri ? env->GetStringUTFChars(uri, nullptr) : "";
    sip_call_t *call = sip_call_make(ctx->coreClient, target, withVideo == JNI_TRUE);
    if (uri) env->ReleaseStringUTFChars(uri, target);

    return reinterpret_cast<jlong>(call);
}

JNIEXPORT jlong JNICALL
Java_com_sipsdk_SipClient_nativeDialCall(JNIEnv *env, jobject thiz, jlong handle, jstring uri,
                                         jboolean withVideo, jintArray outStatus) {
    (void)thiz;
    auto *ctx = reinterpret_cast<ClientJniContext *>(handle);
    if (!ctx || !ctx->coreClient) {
        if (outStatus && env->GetArrayLength(outStatus) > 0) {
            jint v = EINVAL;
            env->SetIntArrayRegion(outStatus, 0, 1, &v);
        }
        return 0;
    }

    const char *target = uri ? env->GetStringUTFChars(uri, nullptr) : "";
    sip_call_t *call = nullptr;
    int rc = sip_call_dial(ctx->coreClient, target, withVideo == JNI_TRUE, &call);
    if (uri) env->ReleaseStringUTFChars(uri, target);

    if (outStatus && env->GetArrayLength(outStatus) > 0) {
        jint status = (rc == EBUSY) ? 16 : rc;
        env->SetIntArrayRegion(outStatus, 0, 1, &status);
    }
    return reinterpret_cast<jlong>(call);
}

JNIEXPORT jint JNICALL
Java_com_sipsdk_SipClient_nativeMergeCalls(JNIEnv *env, jobject thiz, jlong handle,
                                           jlong callAHandle, jlong callBHandle) {
    (void)env; (void)thiz;
    auto *ctx = reinterpret_cast<ClientJniContext *>(handle);
    auto *callA = reinterpret_cast<sip_call_t *>(callAHandle);
    auto *callB = reinterpret_cast<sip_call_t *>(callBHandle);
    if (!ctx || !ctx->coreClient || !callA || !callB) return EINVAL;
    return sip_client_merge_calls(ctx->coreClient, callA, callB);
}

JNIEXPORT jint JNICALL
Java_com_sipsdk_SipClient_nativeAddToConference(JNIEnv *env, jobject thiz, jlong handle, jlong callHandle) {
    (void)env; (void)thiz;
    auto *ctx = reinterpret_cast<ClientJniContext *>(handle);
    auto *call = reinterpret_cast<sip_call_t *>(callHandle);
    if (!ctx || !ctx->coreClient || !call) return EINVAL;
    return sip_client_add_to_conference(ctx->coreClient, call);
}

JNIEXPORT jint JNICALL
Java_com_sipsdk_SipClient_nativeSplitCall(JNIEnv *env, jobject thiz, jlong handle, jlong callHandle) {
    (void)env; (void)thiz;
    auto *ctx = reinterpret_cast<ClientJniContext *>(handle);
    auto *call = reinterpret_cast<sip_call_t *>(callHandle);
    if (!ctx || !ctx->coreClient || !call) return EINVAL;
    return sip_client_split_call(ctx->coreClient, call);
}

JNIEXPORT jint JNICALL
Java_com_sipsdk_SipClient_nativeLeaveConference(JNIEnv *env, jobject thiz, jlong handle) {
    (void)env; (void)thiz;
    auto *ctx = reinterpret_cast<ClientJniContext *>(handle);
    if (!ctx || !ctx->coreClient) return EINVAL;
    return sip_client_leave_conference(ctx->coreClient);
}

JNIEXPORT jlong JNICALL
Java_com_sipsdk_SipClient_nativeSendMessage(JNIEnv *env, jobject thiz, jlong handle, jstring uri,
                                           jstring contentType, jbyteArray body) {
    (void)thiz;
    auto *ctx = reinterpret_cast<ClientJniContext *>(handle);
    if (!ctx || !ctx->coreClient || !uri || !contentType || !body) return -EINVAL;

    const char *target = env->GetStringUTFChars(uri, nullptr);
    const char *ctype = env->GetStringUTFChars(contentType, nullptr);
    jsize len = env->GetArrayLength(body);
    jbyte *bytes = env->GetByteArrayElements(body, nullptr);

    uint64_t messageId = 0;
    int rc = sip_client_send_message(ctx->coreClient, target, ctype,
                                     reinterpret_cast<const uint8_t *>(bytes), static_cast<size_t>(len), &messageId);

    env->ReleaseByteArrayElements(body, bytes, JNI_ABORT);
    env->ReleaseStringUTFChars(contentType, ctype);
    env->ReleaseStringUTFChars(uri, target);
    return rc == 0 ? static_cast<jlong>(messageId) : -static_cast<jlong>(rc);
}

JNIEXPORT void JNICALL
Java_com_sipsdk_SipClient_nativeDestroy(JNIEnv *env, jobject thiz, jlong handle) {
    (void)thiz;
    auto *ctx = reinterpret_cast<ClientJniContext *>(handle);
    if (!ctx) return;

    ctx->destroyed.store(true);

    if (ctx->coreClient) {
        sip_client_destroy(ctx->coreClient);
        ctx->coreClient = nullptr;
    }

    if (ctx->mainHandlerGlobalRef && ctx->taskRunnableClassGlobalRef && ctx->postMethodId && ctx->taskRunnableCtorId) {
        jobject runnable = env->NewObject(
            ctx->taskRunnableClassGlobalRef,
            ctx->taskRunnableCtorId,
            reinterpret_cast<jlong>(jni_cleanup_context_on_main),
            reinterpret_cast<jlong>(ctx)
        );
        if (runnable) {
            jboolean posted = env->CallBooleanMethod(ctx->mainHandlerGlobalRef, ctx->postMethodId, runnable);
            env->DeleteLocalRef(runnable);
            if (posted == JNI_TRUE) {
                return;
            }
        }
    }

    jni_cleanup_context_on_main(ctx);
}

JNIEXPORT jstring JNICALL
Java_com_sipsdk_SipClient_nativeCallGetId(JNIEnv *env, jobject thiz, jlong callHandle) {
    (void)thiz;
    auto *call = reinterpret_cast<sip_call_t *>(callHandle);
    const char *id = call ? sip_call_get_id(call) : "";
    return env->NewStringUTF(id ? id : "");
}

JNIEXPORT jstring JNICALL
Java_com_sipsdk_SipClient_nativeCallGetRemoteUri(JNIEnv *env, jobject thiz, jlong callHandle) {
    (void)thiz;
    auto *call = reinterpret_cast<sip_call_t *>(callHandle);
    const char *uri = call ? sip_call_get_remote_uri(call) : "";
    return env->NewStringUTF(uri ? uri : "");
}

JNIEXPORT jstring JNICALL
Java_com_sipsdk_SipClient_nativeCallGetPeerDisplayName(JNIEnv *env, jobject thiz, jlong callHandle) {
    (void)thiz;
    auto *call = reinterpret_cast<sip_call_t *>(callHandle);
    const char *name = call ? sip_call_get_peer_display_name(call) : nullptr;
    return name ? env->NewStringUTF(name) : nullptr;
}

JNIEXPORT jboolean JNICALL
Java_com_sipsdk_SipClient_nativeCallIsOutgoing(JNIEnv *env, jobject thiz, jlong callHandle) {
    (void)env;
    (void)thiz;
    auto *call = reinterpret_cast<sip_call_t *>(callHandle);
    return (call && sip_call_is_outgoing(call)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_sipsdk_SipClient_nativeCallHasVideo(JNIEnv *env, jobject thiz, jlong callHandle) {
    (void)env;
    (void)thiz;
    auto *call = reinterpret_cast<sip_call_t *>(callHandle);
    return (call && sip_call_has_video(call)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_sipsdk_SipCall_nativeHasVideo(JNIEnv *env, jobject thiz, jlong callHandle) {
    (void)env;
    (void)thiz;
    auto *call = reinterpret_cast<sip_call_t *>(callHandle);
    return (call && sip_call_has_video(call)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL
Java_com_sipsdk_SipCall_nativeAnswer(JNIEnv *env, jobject thiz, jlong callHandle, jboolean withVideo) {
    (void)env;
    (void)thiz;
    auto *call = reinterpret_cast<sip_call_t *>(callHandle);
    if (!call) return -1;
    return sip_call_answer(call, withVideo == JNI_TRUE);
}

JNIEXPORT jint JNICALL
Java_com_sipsdk_SipCall_nativeHangup(JNIEnv *env, jobject thiz, jlong callHandle, jint code, jstring reason) {
    (void)thiz;
    auto *call = reinterpret_cast<sip_call_t *>(callHandle);
    if (!call) return -1;
    const char *r = reason ? env->GetStringUTFChars(reason, nullptr) : "Hangup";
    int res = sip_call_hangup(call, code, r);
    if (reason) env->ReleaseStringUTFChars(reason, r);
    return res;
}

JNIEXPORT jint JNICALL
Java_com_sipsdk_SipCall_nativeSetHold(JNIEnv *env, jobject thiz, jlong callHandle, jboolean hold) {
    (void)env;
    (void)thiz;
    auto *call = reinterpret_cast<sip_call_t *>(callHandle);
    if (!call) return -1;
    return sip_call_set_hold(call, hold == JNI_TRUE);
}

JNIEXPORT jint JNICALL
Java_com_sipsdk_SipCall_nativeSendDtmf(JNIEnv *env, jobject thiz, jlong callHandle, jstring digits) {
    (void)thiz;
    auto *call = reinterpret_cast<sip_call_t *>(callHandle);
    if (!call) return -1;
    const char *d = digits ? env->GetStringUTFChars(digits, nullptr) : "";
    int res = sip_call_send_dtmf(call, d);
    if (digits) env->ReleaseStringUTFChars(digits, d);
    return res;
}

JNIEXPORT jlong JNICALL
Java_com_sipsdk_SipCall_nativeGetDuration(JNIEnv *env, jobject thiz, jlong callHandle) {
    (void)env;
    (void)thiz;
    auto *call = reinterpret_cast<sip_call_t *>(callHandle);
    return call ? static_cast<jlong>(sip_call_get_duration(call)) : 0L;
}

JNIEXPORT jboolean JNICALL
Java_com_sipsdk_SipCall_nativeIsClosed(JNIEnv *env, jobject thiz, jlong callHandle) {
    (void)env;
    (void)thiz;
    auto *call = reinterpret_cast<sip_call_t *>(callHandle);
    return (call && sip_call_is_closed(call)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_sipsdk_SipCall_nativeIsOnHold(JNIEnv *env, jobject thiz, jlong callHandle) {
    (void)env;
    (void)thiz;
    auto *call = reinterpret_cast<sip_call_t *>(callHandle);
    return (call && sip_call_is_on_hold(call)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL
Java_com_sipsdk_SipClient_nativeSetRemoteSurface(JNIEnv *env, jobject thiz, jlong handle, jobject surface) {
    (void)thiz;
    auto *ctx = reinterpret_cast<ClientJniContext *>(handle);
    if (!ctx) return -1;

    ANativeWindow *window = surface ? ANativeWindow_fromSurface(env, surface) : nullptr;
    sipsdk::AndroidMediaCodecDriver::set_remote_window(window);
    if (window) {
        ANativeWindow_release(window);
    }
    return 0;
}

JNIEXPORT jint JNICALL
Java_com_sipsdk_SipCall_nativeRequestKeyframe(JNIEnv *env, jobject thiz, jlong callHandle) {
    (void)env;
    (void)thiz;
    auto *call = reinterpret_cast<sip_call_t *>(callHandle);
    if (!call) return -1;
    return sip_call_request_keyframe(call);
}

JNIEXPORT jboolean JNICALL
Java_com_sipsdk_SipCall_nativeIsSecure(JNIEnv *env, jobject thiz, jlong callHandle) {
    (void)env;
    (void)thiz;
    auto *call = reinterpret_cast<sip_call_t *>(callHandle);
    return (call && sip_call_is_secure(call)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_com_sipsdk_SipCall_nativeIsInConference(JNIEnv *env, jobject thiz, jlong callHandle) {
    (void)env;
    (void)thiz;
    auto *call = reinterpret_cast<sip_call_t *>(callHandle);
    return (call && sip_call_is_in_conference(call)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL
Java_com_sipsdk_SipCall_nativeStartRecording(JNIEnv *env, jobject thiz, jlong callHandle, jstring jPath, jint mode) {
    (void)thiz;
    auto *call = reinterpret_cast<sip_call_t *>(callHandle);
    if (!call || !jPath) return EINVAL;
    const char *path = env->GetStringUTFChars(jPath, nullptr);
    int rc = sip_call_start_recording(call, path, static_cast<sip_recording_mode_t>(mode));
    env->ReleaseStringUTFChars(jPath, path);
    return (jint)rc;
}

JNIEXPORT jint JNICALL
Java_com_sipsdk_SipCall_nativeStopRecording(JNIEnv *env, jobject thiz, jlong callHandle) {
    (void)env;
    (void)thiz;
    auto *call = reinterpret_cast<sip_call_t *>(callHandle);
    if (!call) return EINVAL;
    return (jint)sip_call_stop_recording(call);
}

JNIEXPORT jboolean JNICALL
Java_com_sipsdk_SipCall_nativeIsRecording(JNIEnv *env, jobject thiz, jlong callHandle) {
    (void)env;
    (void)thiz;
    auto *call = reinterpret_cast<sip_call_t *>(callHandle);
    return (call && sip_call_is_recording(call)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jstring JNICALL
Java_com_sipsdk_SipCall_nativeGetRecordingPath(JNIEnv *env, jobject thiz, jlong callHandle) {
    (void)thiz;
    auto *call = reinterpret_cast<sip_call_t *>(callHandle);
    const char *p = call ? sip_call_get_recording_path(call) : "";
    return env->NewStringUTF(p ? p : "");
}

JNIEXPORT jobject JNICALL
Java_com_sipsdk_SipCall_nativeGetStats(JNIEnv *env, jobject thiz, jlong callHandle) {
    (void)thiz;
    auto *call = reinterpret_cast<sip_call_t *>(callHandle);
    if (!call) return nullptr;
    sip_call_stats_t stats{};
    if (sip_call_get_stats(call, &stats) != 0) return nullptr;

    jclass cls = env->FindClass("com/sipsdk/SipCallStats");
    if (!cls) return nullptr;
    jmethodID ctor = env->GetMethodID(cls, "<init>", "(FIIILjava/lang/String;FIIILjava/lang/String;)V");
    if (!ctor) {
        env->DeleteLocalRef(cls);
        return nullptr;
    }
    jstring jAudioCodec = env->NewStringUTF(stats.audio_codec);
    jstring jVideoCodec = env->NewStringUTF(stats.video_codec);
    jobject obj = env->NewObject(
        cls, ctor,
        (jfloat)stats.audio_loss_rate,
        (jint)stats.audio_jitter_ms,
        (jint)stats.audio_rtt_ms,
        (jint)stats.audio_bitrate_kbps,
        jAudioCodec,
        (jfloat)stats.video_loss_rate,
        (jint)stats.video_jitter_ms,
        (jint)stats.video_bitrate_kbps,
        (jint)stats.video_fps,
        jVideoCodec
    );
    if (jAudioCodec) env->DeleteLocalRef(jAudioCodec);
    if (jVideoCodec) env->DeleteLocalRef(jVideoCodec);
    env->DeleteLocalRef(cls);
    return obj;
}

JNIEXPORT jint JNICALL
Java_com_sipsdk_SipCall_nativeTransfer(JNIEnv *env, jobject thiz, jlong callHandle, jstring target) {
    (void)thiz;
    auto *call = reinterpret_cast<sip_call_t *>(callHandle);
    if (!call) return -1;
    const char *t = target ? env->GetStringUTFChars(target, nullptr) : "";
    int res = sip_call_transfer(call, t);
    if (target) env->ReleaseStringUTFChars(target, t);
    return res;
}

JNIEXPORT jint JNICALL
Java_com_sipsdk_SipCall_nativeAttendedTransfer(JNIEnv *env, jobject thiz, jlong callHandle, jlong consultHandle) {
    (void)env;
    (void)thiz;
    auto *call = reinterpret_cast<sip_call_t *>(callHandle);
    auto *consult = reinterpret_cast<sip_call_t *>(consultHandle);
    if (!call || !consult) return -1;
    return sip_call_attended_transfer(call, consult);
}

JNIEXPORT jint JNICALL
Java_com_sipsdk_SipCall_nativeAcceptTransfer(JNIEnv *env, jobject thiz, jlong callHandle, jboolean accept) {
    (void)env;
    (void)thiz;
    auto *call = reinterpret_cast<sip_call_t *>(callHandle);
    if (!call) return -1;
    return sip_call_accept_transfer(call, accept == JNI_TRUE);
}

JNIEXPORT jint JNICALL
Java_com_sipsdk_SipClient_nativeSwitchCamera(JNIEnv *env, jobject thiz, jlong handle) {
    (void)env;
    (void)thiz;
    auto *ctx = reinterpret_cast<ClientJniContext *>(handle);
    if (!ctx || !ctx->coreClient) return -1;
    return sip_client_switch_camera(ctx->coreClient);
}

JNIEXPORT jint JNICALL
Java_com_sipsdk_SipClient_nativeSetCamera(JNIEnv *env, jobject thiz, jlong handle, jint position) {
    (void)env;
    (void)thiz;
    auto *ctx = reinterpret_cast<ClientJniContext *>(handle);
    if (!ctx || !ctx->coreClient) return -1;
    return sip_client_set_camera(ctx->coreClient, position);
}

JNIEXPORT jint JNICALL
Java_com_sipsdk_SipClient_nativeSetLocalVideoEnabled(JNIEnv *env, jobject thiz, jlong handle, jboolean enabled) {
    (void)env;
    (void)thiz;
    auto *ctx = reinterpret_cast<ClientJniContext *>(handle);
    if (!ctx || !ctx->coreClient) return -1;
    return sip_client_set_local_video_enabled(ctx->coreClient, enabled == JNI_TRUE);
}

JNIEXPORT jboolean JNICALL
Java_com_sipsdk_SipClient_nativeIsLocalVideoEnabled(JNIEnv *env, jobject thiz, jlong handle) {
    (void)env;
    (void)thiz;
    auto *ctx = reinterpret_cast<ClientJniContext *>(handle);
    if (!ctx || !ctx->coreClient) return JNI_FALSE;
    return sip_client_is_local_video_enabled(ctx->coreClient) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL
Java_com_sipsdk_SipClient_nativeStartLocalPreview(JNIEnv *env, jobject thiz, jlong handle) {
    (void)env;
    (void)thiz;
    auto *ctx = reinterpret_cast<ClientJniContext *>(handle);
    if (!ctx || !ctx->coreClient) return -1;
    return sip_client_start_local_preview(ctx->coreClient);
}

JNIEXPORT jint JNICALL
Java_com_sipsdk_SipClient_nativeStopLocalPreview(JNIEnv *env, jobject thiz, jlong handle) {
    (void)env;
    (void)thiz;
    auto *ctx = reinterpret_cast<ClientJniContext *>(handle);
    if (!ctx || !ctx->coreClient) return -1;
    return sip_client_stop_local_preview(ctx->coreClient);
}

JNIEXPORT jboolean JNICALL
Java_com_sipsdk_SipClient_nativeIsLocalPreviewActive(JNIEnv *env, jobject thiz, jlong handle) {
    (void)env;
    (void)thiz;
    auto *ctx = reinterpret_cast<ClientJniContext *>(handle);
    if (!ctx || !ctx->coreClient) return JNI_FALSE;
    return sip_client_is_local_preview_active(ctx->coreClient) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL
Java_com_sipsdk_SipClient_nativeSetAudioDriver(JNIEnv *env, jobject thiz, jlong handle,
                                              jstring jSrcMod, jstring jSrcDev,
                                              jstring jPlayMod, jstring jPlayDev) {
    (void)thiz;
    auto *ctx = reinterpret_cast<ClientJniContext *>(handle);
    if (!ctx || !ctx->coreClient) return -1;

    const char *srcMod = jSrcMod ? env->GetStringUTFChars(jSrcMod, nullptr) : nullptr;
    const char *srcDev = jSrcDev ? env->GetStringUTFChars(jSrcDev, nullptr) : nullptr;
    const char *playMod = jPlayMod ? env->GetStringUTFChars(jPlayMod, nullptr) : nullptr;
    const char *playDev = jPlayDev ? env->GetStringUTFChars(jPlayDev, nullptr) : nullptr;

    int res = sip_client_set_audio_driver(ctx->coreClient, srcMod, srcDev, playMod, playDev);

    if (jSrcMod && srcMod) env->ReleaseStringUTFChars(jSrcMod, srcMod);
    if (jSrcDev && srcDev) env->ReleaseStringUTFChars(jSrcDev, srcDev);
    if (jPlayMod && playMod) env->ReleaseStringUTFChars(jPlayMod, playMod);
    if (jPlayDev && playDev) env->ReleaseStringUTFChars(jPlayDev, playDev);
    return res;
}

JNIEXPORT jobjectArray JNICALL
Java_com_sipsdk_SipClient_nativeGetAudioDriver(JNIEnv *env, jobject thiz, jlong handle) {
    (void)thiz;
    auto *ctx = reinterpret_cast<ClientJniContext *>(handle);
    if (!ctx || !ctx->coreClient) return nullptr;

    char srcMod[64] = {0};
    char playMod[64] = {0};
    int res = sip_client_get_audio_driver(ctx->coreClient, srcMod, sizeof(srcMod), playMod, sizeof(playMod));
    if (res != 0) return nullptr;

    jclass stringClass = env->FindClass("java/lang/String");
    jobjectArray arr = env->NewObjectArray(2, stringClass, nullptr);
    env->SetObjectArrayElement(arr, 0, env->NewStringUTF(srcMod));
    env->SetObjectArrayElement(arr, 1, env->NewStringUTF(playMod));
    env->DeleteLocalRef(stringClass);
    return arr;
}

static void processAndRotateYuvFrame(
    const uint8_t *srcY, const uint8_t *srcU, const uint8_t *srcV,
    int inW, int inH,
    int yRowStride, int uRowStride, int vRowStride,
    int yPixelStride, int uPixelStride, int vPixelStride,
    int rotationDegrees, bool mirror,
    uint8_t *dstY, uint8_t *dstU, uint8_t *dstV,
    int &outW, int &outH)
{
    int rot = (rotationDegrees % 360 + 360) % 360;
    if (rot == 90 || rot == 270) {
        outW = inH;
        outH = inW;
    } else {
        outW = inW;
        outH = inH;
    }

    int inChromaW = inW / 2;
    int inChromaH = inH / 2;
    int outChromaW = outW / 2;
    int outChromaH = outH / 2;

    // Fast path: upright, no mirroring, standard contiguous planar I420
    if (rot == 0 && !mirror &&
        yPixelStride == 1 && uPixelStride == 1 && vPixelStride == 1 &&
        yRowStride == inW && uRowStride == inChromaW && vRowStride == inChromaW) {
        memcpy(dstY, srcY, static_cast<size_t>(inW * inH));
        memcpy(dstU, srcU, static_cast<size_t>(inChromaW * inChromaH));
        memcpy(dstV, srcV, static_cast<size_t>(inChromaW * inChromaH));
        return;
    }

    // Process Y Plane
    for (int y = 0; y < outH; ++y) {
        uint8_t *dstRow = dstY + y * outW;
        for (int x = 0; x < outW; ++x) {
            int mx = mirror ? (outW - 1 - x) : x;
            int sx = 0, sy = 0;
            switch (rot) {
                case 90:
                    sx = y;
                    sy = (inH - 1) - mx;
                    break;
                case 180:
                    sx = (inW - 1) - mx;
                    sy = (inH - 1) - y;
                    break;
                case 270:
                    sx = (inW - 1) - y;
                    sy = mx;
                    break;
                case 0:
                default:
                    sx = mx;
                    sy = y;
                    break;
            }
            dstRow[x] = srcY[sy * yRowStride + sx * yPixelStride];
        }
    }

    // Process U and V Planes with proper pixelStride de-interleaving
    for (int cy = 0; cy < outChromaH; ++cy) {
        uint8_t *dstURow = dstU + cy * outChromaW;
        uint8_t *dstVRow = dstV + cy * outChromaW;
        for (int cx = 0; cx < outChromaW; ++cx) {
            int mcx = mirror ? (outChromaW - 1 - cx) : cx;
            int scx = 0, scy = 0;
            switch (rot) {
                case 90:
                    scx = cy;
                    scy = (inChromaH - 1) - mcx;
                    break;
                case 180:
                    scx = (inChromaW - 1) - mcx;
                    scy = (inChromaH - 1) - cy;
                    break;
                case 270:
                    scx = (inChromaW - 1) - cy;
                    scy = mcx;
                    break;
                case 0:
                default:
                    scx = mcx;
                    scy = cy;
                    break;
            }
            dstURow[cx] = srcU[scy * uRowStride + scx * uPixelStride];
            dstVRow[cx] = srcV[scy * vRowStride + scx * vPixelStride];
        }
    }
}

JNIEXPORT void JNICALL
Java_com_sipsdk_SipCameraCapture_nativeDeliverFrame(
    JNIEnv *env, jclass clazz, jlong clientHandle,
    jint width, jint height,
    jobject yBuffer, jobject uBuffer, jobject vBuffer,
    jint yRowStride, jint uRowStride, jint vRowStride,
    jint yPixelStride, jint uPixelStride, jint vPixelStride,
    jint rotationDegrees, jboolean mirror,
    jlong timestampNs)
{
    (void)clazz;
    auto *ctx = reinterpret_cast<ClientJniContext *>(clientHandle);
    if (!ctx || !ctx->coreClient) return;

    const uint8_t *yPtr = static_cast<const uint8_t *>(env->GetDirectBufferAddress(yBuffer));
    const uint8_t *uPtr = static_cast<const uint8_t *>(env->GetDirectBufferAddress(uBuffer));
    const uint8_t *vPtr = static_cast<const uint8_t *>(env->GetDirectBufferAddress(vBuffer));
    if (!yPtr || !uPtr || !vPtr) return;

    std::lock_guard<std::mutex> lock(ctx->captureState.mutex);
    if (!ctx->captureState.frame_handler) return;

    int outW = width;
    int outH = height;
    int rot = (rotationDegrees % 360 + 360) % 360;
    if (rot == 90 || rot == 270) {
        outW = height;
        outH = width;
    }

    size_t ySize = static_cast<size_t>(outW * outH);
    size_t uvSize = static_cast<size_t>((outW / 2) * (outH / 2));
    if (ctx->captureState.yPlane.size() != ySize) {
        ctx->captureState.yPlane.resize(ySize);
        ctx->captureState.uPlane.resize(uvSize);
        ctx->captureState.vPlane.resize(uvSize);
    }

    processAndRotateYuvFrame(
        yPtr, uPtr, vPtr,
        width, height,
        yRowStride, uRowStride, vRowStride,
        yPixelStride, uPixelStride, vPixelStride,
        rotationDegrees, mirror,
        ctx->captureState.yPlane.data(),
        ctx->captureState.uPlane.data(),
        ctx->captureState.vPlane.data(),
        outW, outH
    );

    sip_video_frame_t vf{};
    vf.width = outW;
    vf.height = outH;
    vf.y = ctx->captureState.yPlane.data();
    vf.u = ctx->captureState.uPlane.data();
    vf.v = ctx->captureState.vPlane.data();
    vf.y_stride = outW;
    vf.u_stride = outW / 2;
    vf.v_stride = outW / 2;
    vf.timestamp_ns = timestampNs > 0 ? static_cast<uint64_t>(timestampNs) : 0;

    ctx->captureState.frame_handler(&vf, ctx->captureState.frame_arg);
}

} // extern "C"
