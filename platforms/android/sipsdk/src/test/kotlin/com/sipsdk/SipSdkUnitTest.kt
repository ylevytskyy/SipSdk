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
        assertEquals(-1, call.sendDtmf("123"))
    }
}
