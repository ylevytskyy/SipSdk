package com.sipsdk

object SipSdk {
    init {
        System.loadLibrary("sipcore")
    }

    @JvmStatic
    external fun nativeGetVersion(): String

    fun getVersion(): String = nativeGetVersion()
}
