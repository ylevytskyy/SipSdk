package com.sipsdk

enum class SipCameraPosition(val value: Int) {
    FRONT(0),
    BACK(1);

    companion object {
        fun fromInt(value: Int): SipCameraPosition = if (value == 1) BACK else FRONT
    }
}
