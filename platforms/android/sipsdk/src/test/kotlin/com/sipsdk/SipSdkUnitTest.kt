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

    @Test
    fun testMessageStatusEnumMapping() {
        assertEquals(SipMessageStatus.SENT, SipMessageStatus.fromInt(0))
        assertEquals(SipMessageStatus.FAILED, SipMessageStatus.fromInt(1))
        assertEquals(SipMessageStatus.DELIVERED, SipMessageStatus.fromInt(2))
        assertEquals(SipMessageStatus.DISPLAYED, SipMessageStatus.fromInt(3))
        assertEquals(SipMessageStatus.FAILED, SipMessageStatus.fromInt(99))
    }

    @Test
    fun testIncomingMessageTextDecoding() {
        val textMsg = SipIncomingMessage("sip:1002@pbx", "text/plain", "héllo".toByteArray(Charsets.UTF_8))
        assertEquals("héllo", textMsg.text)

        val upperMsg = SipIncomingMessage("sip:1002@pbx", "TEXT/PLAIN", "hi".toByteArray(Charsets.UTF_8))
        assertEquals("hi", upperMsg.text)

        val jsonMsg = SipIncomingMessage("sip:1002@pbx", "application/json", "{}".toByteArray(Charsets.UTF_8))
        assertEquals(null, jsonMsg.text)
    }

    @Test
    fun testSendMessageValidation() {
        val client = SipClient("1001", "10.0.2.2")
        try {
            client.sendMessage("", "hi")
            org.junit.Assert.fail("Expected IllegalArgumentException for empty target")
        } catch (_: IllegalArgumentException) {}

        try {
            client.sendMessage("1002", "")
            org.junit.Assert.fail("Expected IllegalArgumentException for empty text")
        } catch (_: IllegalArgumentException) {}

        try {
            client.sendMessage("1002", byteArrayOf(1), "")
            org.junit.Assert.fail("Expected IllegalArgumentException for empty contentType")
        } catch (_: IllegalArgumentException) {}

        try {
            client.sendMessage("1002", "hi")
            org.junit.Assert.fail("Expected IllegalStateException when not started")
        } catch (_: IllegalStateException) {}
    }

    @Test
    fun testTlsSrtpDefaultsAndCallStatsModel() {
        val client = SipClient("1004", "10.0.2.2")
        assertTrue(client.verifyTlsCert)
        assertEquals(null, client.tlsCaFile)
        assertEquals("", client.mediaEnc)

        client.transport = "tls"
        client.port = 5061
        client.verifyTlsCert = false
        client.mediaEnc = "srtp-mand"
        assertEquals("tls", client.transport)
        assertEquals(5061, client.port)
        assertFalse(client.verifyTlsCert)
        assertEquals("srtp-mand", client.mediaEnc)

        val stats = SipCallStats(
            audioLossRate = 0.01f,
            audioJitterMs = 5,
            audioRttMs = 22,
            audioBitrateKbps = 48,
            audioCodec = "opus",
            videoLossRate = 0f,
            videoJitterMs = 8,
            videoBitrateKbps = 450,
            videoFps = 30,
            videoCodec = "H264"
        )
        assertEquals("opus", stats.audioCodec)
        assertEquals(48, stats.audioBitrateKbps)
        assertEquals("H264", stats.videoCodec)

        val call = SipCall(nativeHandle = 0L, callId = "id", remoteUri = "sip:9001@10.0.2.2")
        assertFalse(call.isSecure)
        assertEquals(null, call.getStats())
        assertEquals(-1, call.transfer("1002"))
        assertEquals(-1, call.attendedTransfer(call))
        assertEquals(-1, call.acceptTransfer(true))
    }
}
