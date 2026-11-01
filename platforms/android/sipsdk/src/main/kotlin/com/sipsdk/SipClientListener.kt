package com.sipsdk

interface SipClientListener {
    fun onRegistrationStateChanged(state: SipRegistrationState, code: Int, reason: String) {}
    fun onIncomingCall(call: SipCall) {}
    fun onCallStateChanged(call: SipCall, state: SipCallState, code: Int, reason: String) {}
    fun onCallMediaChanged(call: SipCall, width: Int, height: Int, onHold: Boolean, isMuted: Boolean) {}
    fun onDtmfDigit(call: SipCall, digit: String) {}
}
