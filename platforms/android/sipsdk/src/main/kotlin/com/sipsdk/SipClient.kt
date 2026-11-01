package com.sipsdk

import android.content.Context
import android.os.Handler
import android.os.Looper

class SipClient(
    val username: String,
    val domain: String,
    val context: Context? = null
) {
    companion object {
        init {
            try {
                System.loadLibrary("sipcore")
            } catch (e: UnsatisfiedLinkError) {
                // Allows loading without native lib if running in mock/isolated unit tests
            }
        }

        fun getVersion(): String = SipSdk.getVersion()
    }

    var password: String? = null
    var displayName: String? = null
    var transport: String = "udp"
    var port: Int = 5060
    var listener: SipClientListener? = null

    val audioManager: SipAudioManager? = context?.let { SipAudioManager(it) }

    val aor: String
        get() = "sip:$username@$domain:$port"

    val isRegistered: Boolean
        get() = if (nativeHandle != 0L) nativeIsRegistered(nativeHandle) else false

    var isMicrophoneMuted: Boolean = false
        set(value) {
            field = value
            if (nativeHandle != 0L) {
                nativeSetMicMuted(nativeHandle, value)
            }
            audioManager?.setMicrophoneMute(value)
        }

    var isSpeakerEnabled: Boolean = false
        set(value) {
            field = value
            audioManager?.setSpeakerphone(value)
        }

    private var nativeHandle: Long = 0L
    private val mainHandler: Handler? = try {
        val looper = Looper.getMainLooper()
        if (looper != null) Handler(looper) else null
    } catch (_: Throwable) {
        null
    }

    fun start(configDir: String? = null): Int {
        if (nativeHandle == 0L) {
            nativeHandle = nativeCreate()
            if (nativeHandle == 0L) return -1
            val resolvedConfigDir = configDir ?: context?.filesDir?.absolutePath
            val res = nativeInit(nativeHandle, resolvedConfigDir)
            if (res != 0) {
                nativeDestroy(nativeHandle)
                nativeHandle = 0L
                return res
            }
        }
        return 0
    }

    fun stop() {
        if (nativeHandle != 0L) {
            audioManager?.releaseAudioSession()
            nativeDestroy(nativeHandle)
            nativeHandle = 0L
        }
    }

    fun register(): Int {
        if (nativeHandle == 0L) {
            val rc = start()
            if (rc != 0) return rc
        }
        return nativeRegister(
            nativeHandle,
            username,
            domain,
            password ?: "",
            transport,
            port
        )
    }

    fun unregister(): Int {
        if (nativeHandle == 0L) return -1
        return nativeUnregister(nativeHandle)
    }

    fun makeCall(uri: String): SipCall? {
        if (nativeHandle == 0L) {
            val rc = start()
            if (rc != 0) return null
        }
        val callHandle = nativeMakeCall(nativeHandle, uri, false)
        if (callHandle == 0L) return null
        audioManager?.prepareAudioSession()
        return SipCall(
            nativeHandle = callHandle,
            callId = nativeCallGetId(callHandle),
            remoteUri = nativeCallGetRemoteUri(callHandle),
            peerDisplayName = nativeCallGetPeerDisplayName(callHandle),
            hasVideo = false,
            isOutgoing = true
        )
    }

    fun setMuted(muted: Boolean) {
        isMicrophoneMuted = muted
    }

    fun setSpeakerphone(enabled: Boolean) {
        isSpeakerEnabled = enabled
    }

    fun destroy() {
        stop()
    }

    // Callbacks from JNI dispatched on main thread
    @JvmSynthetic
    fun onNativeRegState(state: Int, code: Int, reason: String) {
        val regState = SipRegistrationState.fromInt(state)
        listener?.onRegistrationStateChanged(regState, code, reason)
    }

    @JvmSynthetic
    fun onNativeCallState(callHandle: Long, state: Int, code: Int, reason: String) {
        val call = SipCall(
            nativeHandle = callHandle,
            callId = nativeCallGetId(callHandle),
            remoteUri = nativeCallGetRemoteUri(callHandle),
            peerDisplayName = nativeCallGetPeerDisplayName(callHandle),
            hasVideo = false,
            isOutgoing = nativeCallIsOutgoing(callHandle)
        )
        val callState = SipCallState.fromInt(state)
        if (callState == SipCallState.CLOSED) {
            audioManager?.releaseAudioSession()
        }
        listener?.onCallStateChanged(call, callState, code, reason)
    }

    @JvmSynthetic
    fun onNativeIncomingCall(callHandle: Long) {
        val call = SipCall(
            nativeHandle = callHandle,
            callId = nativeCallGetId(callHandle),
            remoteUri = nativeCallGetRemoteUri(callHandle),
            peerDisplayName = nativeCallGetPeerDisplayName(callHandle),
            hasVideo = false,
            isOutgoing = false
        )
        listener?.onIncomingCall(call)
    }

    @JvmSynthetic
    fun onNativeCallMedia(callHandle: Long, width: Int, height: Int, onHold: Boolean, isMuted: Boolean) {
        val call = SipCall(
            nativeHandle = callHandle,
            callId = nativeCallGetId(callHandle),
            remoteUri = nativeCallGetRemoteUri(callHandle),
            peerDisplayName = nativeCallGetPeerDisplayName(callHandle),
            hasVideo = (width > 0 && height > 0),
            isOutgoing = nativeCallIsOutgoing(callHandle)
        )
        listener?.onCallMediaChanged(call, width, height, onHold, isMuted)
    }

    @JvmSynthetic
    fun onNativeDtmfDigit(callHandle: Long, digit: String) {
        val call = SipCall(
            nativeHandle = callHandle,
            callId = nativeCallGetId(callHandle),
            remoteUri = nativeCallGetRemoteUri(callHandle),
            peerDisplayName = nativeCallGetPeerDisplayName(callHandle),
            hasVideo = false,
            isOutgoing = nativeCallIsOutgoing(callHandle)
        )
        listener?.onDtmfDigit(call, digit)
    }

    @JvmSynthetic
    fun onNativePrepareAudioSession(): Boolean {
        return audioManager?.prepareAudioSession() ?: true
    }

    @JvmSynthetic
    fun onNativeReleaseAudioSession() {
        audioManager?.releaseAudioSession()
    }

    // Native methods
    private external fun nativeCreate(): Long
    private external fun nativeInit(handle: Long, configPath: String?): Int
    private external fun nativeRegister(handle: Long, user: String, domain: String, pwd: String, transport: String, port: Int): Int
    private external fun nativeUnregister(handle: Long): Int
    private external fun nativeIsRegistered(handle: Long): Boolean
    private external fun nativeSetMicMuted(handle: Long, muted: Boolean)
    private external fun nativeMakeCall(handle: Long, uri: String, withVideo: Boolean): Long
    private external fun nativeDestroy(handle: Long)

    private external fun nativeCallGetId(callHandle: Long): String
    private external fun nativeCallGetRemoteUri(callHandle: Long): String
    private external fun nativeCallGetPeerDisplayName(callHandle: Long): String?
    private external fun nativeCallIsOutgoing(callHandle: Long): Boolean
}
