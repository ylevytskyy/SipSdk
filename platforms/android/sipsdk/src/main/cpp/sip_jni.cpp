#include <jni.h>
#include <android/log.h>
#include <string>
#include <mutex>
#include "sip_abi.h"

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

} // extern "C"
