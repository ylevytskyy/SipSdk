package com.sipsdk

interface SipClientListener {
    fun onRegistrationStateChanged(state: SipRegistrationState, code: Int, reason: String) {}
    fun onIncomingCall(call: SipCall) {}
    fun onCallStateChanged(call: SipCall, state: SipCallState, code: Int, reason: String) {}
    fun onCallMediaChanged(call: SipCall, width: Int, height: Int, onHold: Boolean, isMuted: Boolean) {}
    fun onDtmfDigit(call: SipCall, digit: String) {}
    fun onCallStats(call: SipCall, stats: SipCallStats) {}
    fun onTransferStatus(call: SipCall, code: Int, reason: String) {}
    fun onTransferRequest(call: SipCall, target: String) {}
    fun onMessageReceived(message: SipIncomingMessage) {}
    fun onMessageStatusChanged(messageId: Long, status: SipMessageStatus, code: Int, reason: String) {}
    fun onCallRejectedBusy(callerUri: String) {}
    fun onPushCallMissed(callId: String, callerUri: String) {}
}
