package com.sipsdk

enum class SipCallState(val value: Int) {
    INCOMING(0),
    RINGING(1),
    CONNECTING(2),
    ESTABLISHED(3),
    HOLD(4),
    CLOSED(5);

    companion object {
        fun fromInt(value: Int): SipCallState = entries.firstOrNull { it.value == value } ?: CLOSED
    }
}
