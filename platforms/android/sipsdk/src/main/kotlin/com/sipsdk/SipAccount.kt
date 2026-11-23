package com.sipsdk

import java.util.concurrent.atomic.AtomicBoolean

data class SipAccountConfig(
    val username: String,
    val domain: String,
    val password: String = "",
    val displayName: String? = null,
    val transport: String = "udp",
    val port: Int = 5060,
    val tlsCaFile: String? = null,
    val verifyTlsCert: Boolean = true,
    val mediaEnc: String = "",
    val maxCalls: Int = 4,
    val outboundProxy: String? = null,
    val stunServer: String? = null,
    val stunUser: String? = null,
    val stunPass: String? = null,
    val mediaNat: String? = null,
    val regInterval: Int = 0
)

class SipAccount internal constructor(
    private val client: SipClient,
    nativeHandle: Long,
    private val ownsNativeHandle: Boolean = true,
    cachedId: Long = 0L,
    cachedAor: String = "",
    cachedUser: String = "",
    cachedDomain: String = ""
) : AutoCloseable {

    @Volatile
    internal var nativeHandle: Long = nativeHandle
        private set

    private val closed = AtomicBoolean(false)

    val id: Long = if (nativeHandle != 0L) nativeAccountGetId(nativeHandle) else cachedId
    val aor: String = if (nativeHandle != 0L) nativeAccountGetAor(nativeHandle) else cachedAor
    val user: String = if (nativeHandle != 0L) nativeAccountGetUser(nativeHandle) else cachedUser
    val domain: String = if (nativeHandle != 0L) nativeAccountGetDomain(nativeHandle) else cachedDomain

    val isRegistered: Boolean
        get() {
            val h = nativeHandle
            return if (h != 0L) nativeAccountIsRegistered(h) else false
        }

    val registrationState: SipRegistrationState
        get() {
            val h = nativeHandle
            return if (h != 0L) SipRegistrationState.fromInt(nativeAccountGetRegState(h)) else SipRegistrationState.UNREGISTERED
        }

    fun reregister(): Int {
        val h = nativeHandle
        return if (h != 0L) nativeAccountReregister(h) else -1
    }

    fun unregister(): Int {
        val h = nativeHandle
        return if (h != 0L) nativeAccountUnregister(h) else -1
    }

    fun dial(targetUri: String, withVideo: Boolean = false): SipCall {
        return client.dialOnAccount(this, targetUri, withVideo)
    }

    fun makeCall(targetUri: String, withVideo: Boolean = false): SipCall? {
        return client.makeCallOnAccount(this, targetUri, withVideo)
    }

    fun sendMessage(peerUri: String, text: String): Long {
        require(text.isNotEmpty()) { "Message text cannot be empty" }
        return sendMessage(peerUri, text.toByteArray(Charsets.UTF_8), "text/plain")
    }

    fun sendMessage(peerUri: String, body: ByteArray, contentType: String = "text/plain"): Long {
        require(peerUri.isNotEmpty()) { "Peer URI cannot be empty" }
        require(body.isNotEmpty()) { "Message body cannot be empty" }
        require(contentType.isNotEmpty()) { "Content type cannot be empty" }
        val h = nativeHandle
        check(h != 0L) { "SipAccount is closed" }
        val uri = when {
            peerUri.startsWith("sip:") -> peerUri
            peerUri.contains("@") -> "sip:$peerUri"
            else -> "sip:$peerUri@$domain"
        }
        val result = nativeAccountSendMessage(h, uri, contentType, body)
        check(result > 0) { "Failed to send message to $uri (error ${-result})" }
        return result
    }

    override fun close() {
        if (closed.compareAndSet(false, true)) {
            val h = nativeHandle
            nativeHandle = 0L
            if (h != 0L) {
                client.onAccountClosed(h, this)
                if (ownsNativeHandle) {
                    nativeAccountDestroy(h)
                }
            }
        }
    }

    fun destroy() {
        close()
    }

    override fun equals(other: Any?): Boolean {
        if (this === other) return true
        if (other !is SipAccount) return false
        return if (id != 0L && other.id != 0L) id == other.id else nativeHandle == other.nativeHandle
    }

    override fun hashCode(): Int {
        return if (id != 0L) id.hashCode() else nativeHandle.hashCode()
    }

    override fun toString(): String = "SipAccount(id=$id, aor=$aor)"

    internal fun dialNative(targetUri: String, withVideo: Boolean, outStatus: IntArray): Long {
        val h = nativeHandle
        if (h == 0L) {
            if (outStatus.isNotEmpty()) outStatus[0] = 22
            return 0L
        }
        return nativeAccountDial(h, targetUri, withVideo, outStatus)
    }

    private external fun nativeAccountReregister(accountHandle: Long): Int
    private external fun nativeAccountUnregister(accountHandle: Long): Int
    private external fun nativeAccountDial(accountHandle: Long, targetUri: String, withVideo: Boolean, outStatus: IntArray): Long
    private external fun nativeAccountSendMessage(accountHandle: Long, peerUri: String, contentType: String, body: ByteArray): Long
    private external fun nativeAccountGetId(accountHandle: Long): Long
    private external fun nativeAccountGetAor(accountHandle: Long): String
    private external fun nativeAccountGetUser(accountHandle: Long): String
    private external fun nativeAccountGetDomain(accountHandle: Long): String
    private external fun nativeAccountIsRegistered(accountHandle: Long): Boolean
    private external fun nativeAccountGetRegState(accountHandle: Long): Int
    private external fun nativeAccountDestroy(accountHandle: Long)
}
