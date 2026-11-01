package com.sipsdk

class SipCall internal constructor(
    internal val nativeHandle: Long,
    val callId: String,
    val remoteUri: String,
    val peerDisplayName: String? = null,
    val hasVideo: Boolean = false,
    val isOutgoing: Boolean = true
) {
    val isOnHold: Boolean
        get() = if (nativeHandle != 0L) nativeIsOnHold(nativeHandle) else false

    val isClosed: Boolean
        get() = if (nativeHandle != 0L) nativeIsClosed(nativeHandle) else true

    val durationSeconds: Long
        get() = if (nativeHandle != 0L) nativeGetDuration(nativeHandle) else 0L

    fun answer(): Int = if (nativeHandle != 0L) nativeAnswer(nativeHandle, false) else -1

    fun hangup(statusCode: Int = 0, reason: String = "Hangup"): Int =
        if (nativeHandle != 0L) nativeHangup(nativeHandle, statusCode, reason) else -1

    fun setHold(hold: Boolean): Int = if (nativeHandle != 0L) nativeSetHold(nativeHandle, hold) else -1

    fun sendDtmf(digits: String): Int = if (nativeHandle != 0L) nativeSendDtmf(nativeHandle, digits) else -1

    private external fun nativeAnswer(handle: Long, withVideo: Boolean): Int
    private external fun nativeHangup(handle: Long, code: Int, reason: String): Int
    private external fun nativeSetHold(handle: Long, hold: Boolean): Int
    private external fun nativeSendDtmf(handle: Long, digits: String): Int
    private external fun nativeGetDuration(handle: Long): Long
    private external fun nativeIsClosed(handle: Long): Boolean
    private external fun nativeIsOnHold(handle: Long): Boolean
}
