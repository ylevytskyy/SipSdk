package com.sipsdk

class SipCall internal constructor(
    @Volatile internal var nativeHandle: Long,
    val callId: String,
    val remoteUri: String,
    val peerDisplayName: String? = null,
    hasVideo: Boolean = false,
    val isOutgoing: Boolean = true,
    internal val client: SipClient? = null
) {
    val hasVideo: Boolean = hasVideo
        get() = if (nativeHandle != 0L) nativeHasVideo(nativeHandle) else field
    val isOnHold: Boolean
        get() = if (nativeHandle != 0L) nativeIsOnHold(nativeHandle) else false

    val isMuted: Boolean
        get() = if (nativeHandle != 0L) nativeIsMuted(nativeHandle) else false

    val isClosed: Boolean
        get() = if (nativeHandle != 0L) nativeIsClosed(nativeHandle) else true

    val durationSeconds: Long
        get() = if (nativeHandle != 0L) nativeGetDuration(nativeHandle) else 0L

    fun answer(withVideo: Boolean = false): Int {
        if (withVideo) {
            client?.onNativeCaptureStart(640, 480, 30)
        }
        return if (nativeHandle != 0L) nativeAnswer(nativeHandle, withVideo) else -1
    }

    fun hangup(statusCode: Int = 0, reason: String = "Hangup"): Int =
        if (nativeHandle != 0L) nativeHangup(nativeHandle, statusCode, reason) else -1

    fun setHold(hold: Boolean): Int = if (nativeHandle != 0L) nativeSetHold(nativeHandle, hold) else -1

    fun setMuted(muted: Boolean): Int = if (nativeHandle != 0L) nativeSetMuted(nativeHandle, muted) else -1

    fun sendDtmf(digits: String): Int = if (nativeHandle != 0L) nativeSendDtmf(nativeHandle, digits) else -1

    fun requestKeyframe(): Int = if (nativeHandle != 0L) nativeRequestKeyframe(nativeHandle) else -1

    internal fun invalidate() {
        nativeHandle = 0L
    }

    private external fun nativeAnswer(handle: Long, withVideo: Boolean): Int
    private external fun nativeHangup(handle: Long, code: Int, reason: String): Int
    private external fun nativeSetHold(handle: Long, hold: Boolean): Int
    private external fun nativeSetMuted(handle: Long, muted: Boolean): Int
    private external fun nativeSendDtmf(handle: Long, digits: String): Int
    private external fun nativeGetDuration(handle: Long): Long
    private external fun nativeIsClosed(handle: Long): Boolean
    private external fun nativeIsOnHold(handle: Long): Boolean
    private external fun nativeIsMuted(handle: Long): Boolean
    private external fun nativeHasVideo(handle: Long): Boolean
    private external fun nativeRequestKeyframe(handle: Long): Int
}
