package com.sipsdk

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test

class SipSdkUnitTest {

    @Test
    fun testRegistrationStateEnumMapping() {
        assertEquals(SipRegistrationState.UNREGISTERED, SipRegistrationState.fromInt(0))
        assertEquals(SipRegistrationState.REGISTERING, SipRegistrationState.fromInt(1))
        assertEquals(SipRegistrationState.REGISTERED, SipRegistrationState.fromInt(2))
        assertEquals(SipRegistrationState.FAILED, SipRegistrationState.fromInt(3))
        assertEquals(SipRegistrationState.UNREGISTERED, SipRegistrationState.fromInt(99))
    }

    @Test
    fun testCallStateEnumMapping() {
        assertEquals(SipCallState.INCOMING, SipCallState.fromInt(0))
        assertEquals(SipCallState.RINGING, SipCallState.fromInt(1))
        assertEquals(SipCallState.CONNECTING, SipCallState.fromInt(2))
        assertEquals(SipCallState.ESTABLISHED, SipCallState.fromInt(3))
        assertEquals(SipCallState.HOLD, SipCallState.fromInt(4))
        assertEquals(SipCallState.CLOSED, SipCallState.fromInt(5))
        assertEquals(SipCallState.CLOSED, SipCallState.fromInt(99))
    }

    @Test
    fun testSipClientAorConstruction() {
        val client = SipClient("1001", "10.0.2.2")
        client.port = 5060
        assertEquals("sip:1001@10.0.2.2:5060", client.aor)
        assertEquals("1001", client.username)
        assertEquals("10.0.2.2", client.domain)
        assertFalse(client.isRegistered)
        assertFalse(client.isMicrophoneMuted)
        assertFalse(client.isSpeakerEnabled)
    }

    @Test
    fun testSipCallModelProperties() {
        val call = SipCall(
            nativeHandle = 0L,
            callId = "test-call-123",
            remoteUri = "sip:9001@10.0.2.2:5060",
            peerDisplayName = "Echo Service",
            hasVideo = false,
            isOutgoing = true
        )
        assertEquals("test-call-123", call.callId)
        assertEquals("sip:9001@10.0.2.2:5060", call.remoteUri)
        assertEquals("Echo Service", call.peerDisplayName)
        assertFalse(call.hasVideo)
        assertTrue(call.isOutgoing)
        assertTrue(call.isClosed) // nativeHandle == 0L defaults to true
        assertEquals(0L, call.durationSeconds)
        assertEquals(-1, call.answer())
        assertEquals(-1, call.hangup())
        assertEquals(-1, call.setHold(true))
        assertEquals(-1, call.setMuted(true))
        assertFalse(call.isMuted)
        assertEquals(-1, call.sendDtmf("123"))
    }

    @Test
    fun testCanonicalSipCallInstanceAcrossCallbacks() {
        val client = SipClient("1001", "10.0.2.2")
        var incomingCall: SipCall? = null
        val stateCalls = mutableListOf<SipCall>()
        var mediaCall: SipCall? = null
        var dtmfCall: SipCall? = null

        client.listener = object : SipClientListener {
            override fun onIncomingCall(call: SipCall) {
                incomingCall = call
            }

            override fun onCallStateChanged(call: SipCall, state: SipCallState, code: Int, reason: String) {
                stateCalls.add(call)
            }

            override fun onCallMediaChanged(call: SipCall, width: Int, height: Int, onHold: Boolean, isMuted: Boolean) {
                mediaCall = call
            }

            override fun onDtmfDigit(call: SipCall, digit: String) {
                dtmfCall = call
            }
        }

        val fakeHandle = 42L
        client.onNativeIncomingCall(fakeHandle)
        assertNotNull(incomingCall)

        client.onNativeCallState(fakeHandle, 3, 200, "OK")
        client.onNativeCallMedia(fakeHandle, 640, 480, false, false)
        client.onNativeDtmfDigit(fakeHandle, "5")
        client.onNativeCallState(fakeHandle, 5, 200, "Normal Clearing")

        assertEquals(2, stateCalls.size)
        org.junit.Assert.assertSame(incomingCall, stateCalls[0])
        org.junit.Assert.assertSame(incomingCall, mediaCall)
        org.junit.Assert.assertSame(incomingCall, dtmfCall)
        org.junit.Assert.assertSame(incomingCall, stateCalls[1])
    }

    @Test
    fun testNativeCaptureStartStopAndCallCloseLifecycle() {
        val client = SipClient("1001", "10.0.2.2")
        assertFalse(client.isCameraCaptureRunning)

        assertEquals(0, client.onNativeCaptureStart(640, 480, 30))
        assertTrue(client.isCameraCaptureRunning)

        client.onNativeCaptureStop()
        assertFalse(client.isCameraCaptureRunning)

        // Start capture again, then simulate call close when local preview is not active
        assertEquals(0, client.onNativeCaptureStart(640, 480, 30))
        assertTrue(client.isCameraCaptureRunning)
        client.onNativeCallState(0L, 5, 200, "OK")
        assertFalse(client.isCameraCaptureRunning)

        // Answering an incoming call with video starts capture, and closing the call stops it
        var incomingCall: SipCall? = null
        client.listener = object : SipClientListener {
            override fun onIncomingCall(call: SipCall) {
                incomingCall = call
            }
        }
        client.onNativeIncomingCall(0L)
        assertNotNull(incomingCall)
        incomingCall!!.answer(withVideo = true)
        assertTrue(client.isCameraCaptureRunning)

        client.onNativeCallState(0L, 5, 200, "Ended")
        assertFalse(client.isCameraCaptureRunning)
    }
}
