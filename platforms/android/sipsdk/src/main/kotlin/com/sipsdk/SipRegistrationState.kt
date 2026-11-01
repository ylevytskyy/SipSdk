package com.sipsdk

enum class SipRegistrationState(val value: Int) {
    UNREGISTERED(0),
    REGISTERING(1),
    REGISTERED(2),
    FAILED(3);

    companion object {
        fun fromInt(value: Int): SipRegistrationState = entries.firstOrNull { it.value == value } ?: UNREGISTERED
    }
}
