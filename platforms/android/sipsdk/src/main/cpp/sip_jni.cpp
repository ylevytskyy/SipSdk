#include <jni.h>
#include <android/log.h>
#include <android/native_window_jni.h>
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
    jmethodID prepareAudioSessionMethodId = nullptr;
    jmethodID releaseAudioSessionMethodId = nullptr;

    sip_client_t *coreClient = nullptr;
    AndroidCaptureState captureState;
    std::mutex mutex;
};

static void jni_dispatch_to_main(void (*task)(void *arg), void *arg, void *user_data) {
    auto *ctx = static_cast<ClientJniContext *>(user_data);
    if (!ctx || !ctx->mainHandlerGlobalRef || !ctx->taskRunnableClassGlobalRef || !ctx->postMethodId) {
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
        guard.env->CallBooleanMethod(ctx->mainHandlerGlobalRef, ctx->postMethodId, runnable);
        guard.env->DeleteLocalRef(runnable);
    } else {
        if (task) task(arg);
    }
}

static void jni_on_reg_state(sip_client_t *client, sip_reg_state_t state, int code, const char *reason, void *user_data) {
    (void)client;
    auto *ctx = static_cast<ClientJniContext *>(user_data);
    if (!ctx || !ctx->javaClientGlobalRef || !ctx->onRegStateMethodId) return;

    JniEnvGuard guard;
    if (!guard.env) return;

    jstring jReason = guard.env->NewStringUTF(reason ? reason : "");
    guard.env->CallVoidMethod(ctx->javaClientGlobalRef, ctx->onRegStateMethodId, (jint)state, (jint)code, jReason);
    if (jReason) guard.env->DeleteLocalRef(jReason);
}

static void jni_on_incoming_call(sip_client_t *client, sip_call_t *call, void *user_data) {
    (void)client;
    auto *ctx = static_cast<ClientJniContext *>(user_data);
    if (!ctx || !ctx->javaClientGlobalRef || !ctx->onIncomingCallMethodId) return;

    JniEnvGuard guard;
    if (!guard.env) return;

    guard.env->CallVoidMethod(ctx->javaClientGlobalRef, ctx->onIncomingCallMethodId, reinterpret_cast<jlong>(call));
}

static void jni_on_call_state(sip_call_t *call, sip_call_state_t state, int code, const char *reason, void *user_data) {
    auto *ctx = static_cast<ClientJniContext *>(user_data);
    if (!ctx || !ctx->javaClientGlobalRef || !ctx->onCallStateMethodId) return;

    JniEnvGuard guard;
    if (!guard.env) return;

    jstring jReason = guard.env->NewStringUTF(reason ? reason : "");
    guard.env->CallVoidMethod(ctx->javaClientGlobalRef, ctx->onCallStateMethodId, reinterpret_cast<jlong>(call), (jint)state, (jint)code, jReason);
    if (jReason) guard.env->DeleteLocalRef(jReason);
}

static void jni_on_call_media(sip_call_t *call, int width, int height, bool on_hold, bool is_muted, void *user_data) {
    auto *ctx = static_cast<ClientJniContext *>(user_data);
    if (!ctx || !ctx->javaClientGlobalRef || !ctx->onCallMediaMethodId) return;

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
    if (!ctx || !ctx->javaClientGlobalRef || !ctx->onDtmfDigitMethodId) return;

    JniEnvGuard guard;
    if (!guard.env) return;

    jstring jDigit = guard.env->NewStringUTF(digit ? digit : "");
    guard.env->CallVoidMethod(ctx->javaClientGlobalRef, ctx->onDtmfDigitMethodId, reinterpret_cast<jlong>(call), jDigit);
    if (jDigit) guard.env->DeleteLocalRef(jDigit);
}

static bool jni_prepare_audio_session(sip_client_t *client, void *user_data) {
    (void)client;
    auto *ctx = static_cast<ClientJniContext *>(user_data);
    if (!ctx || !ctx->javaClientGlobalRef || !ctx->prepareAudioSessionMethodId) return true;

    JniEnvGuard guard;
    if (!guard.env) return true;

    return guard.env->CallBooleanMethod(ctx->javaClientGlobalRef, ctx->prepareAudioSessionMethodId) == JNI_TRUE;
}

static void jni_release_audio_session(sip_client_t *client, void *user_data) {
    (void)client;
    auto *ctx = static_cast<ClientJniContext *>(user_data);
    if (!ctx || !ctx->javaClientGlobalRef || !ctx->releaseAudioSessionMethodId) return;

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
    callbacks.prepare_audio_session = jni_prepare_audio_session;
    callbacks.release_audio_session = jni_release_audio_session;

    ctx->coreClient = sip_client_create(&callbacks, ctx);
    if (ctx->coreClient) {
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

JNIEXPORT void JNICALL
Java_com_sipsdk_SipClient_nativeDestroy(JNIEnv *env, jobject thiz, jlong handle) {
    (void)thiz;
    auto *ctx = reinterpret_cast<ClientJniContext *>(handle);
    if (!ctx) return;

    if (ctx->coreClient) {
        sip_client_destroy(ctx->coreClient);
        ctx->coreClient = nullptr;
    }

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

    delete ctx;
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
