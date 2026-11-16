package com.sipsdk

/** Status of a message sent with [SipClient.sendMessage]; values match sip_message_status_t. */
enum class SipMessageStatus(val value: Int) {
    /** The server or peer accepted the message (2xx). */
    SENT(0),
    /** Non-2xx final response, transport error, or no registered account. */
    FAILED(1),
    /** Reserved: delivery notification (IMDN, RFC 5438) from the peer. */
    DELIVERED(2),
    /** Reserved: display (read) notification (IMDN, RFC 5438) from the peer. */
    DISPLAYED(3);

    companion object {
        fun fromInt(value: Int): SipMessageStatus = values().firstOrNull { it.value == value } ?: FAILED
    }
}

/** A SIP MESSAGE received for the registered account. */
class SipIncomingMessage(
    /** Sender URI, e.g. "sip:1002@pbx". */
    val from: String,
    /** MIME type, e.g. "text/plain". */
    val contentType: String,
    /** Raw body bytes. */
    val body: ByteArray
) {
    /** Body as UTF-8 text for text types; null otherwise. */
    val text: String?
        get() = if (contentType.startsWith("text/", ignoreCase = true)) String(body, Charsets.UTF_8) else null
}
