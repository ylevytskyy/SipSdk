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

    @Test
    fun testMultiCallConfigBusyListenerAndTelecomContract() {
        val client = SipClient("1001", "10.0.2.2")
        assertEquals(4, client.maxCalls)
        client.maxCalls = 2
        assertEquals(2, client.maxCalls)

        var busyUri: String? = null
        val listener = object : SipClientListener {
            override fun onCallRejectedBusy(callerUri: String) {
                busyUri = callerUri
            }
        }
        listener.onCallRejectedBusy("sip:1002@pbx")
        assertEquals("sip:1002@pbx", busyUri)

        val err: Throwable = SipBusyException("Max concurrent calls reached")
        assertTrue(err is IllegalStateException)

        val fakeTelecom: SipTelecomManagerCoordinator = InMemoryTelecomCoordinator()
        val callId1 = "call-out-1"
        val callId2 = "call-in-2"
        fakeTelecom.addCall(
            callId = callId1,
            displayName = "Alice",
            address = "sip:1002@pbx",
            isOutgoing = true,
            isVideo = false,
            callbacks = object : SipTelecomCallCallbacks {
                override fun onAnswer(isVideo: Boolean) {}
                override fun onDisconnect() {}
                override fun onSetActive() {}
                override fun onSetInactive() {}
            }
        )
        fakeTelecom.addCall(
            callId = callId2,
            displayName = "Bob",
            address = "sip:1003@pbx",
            isOutgoing = false,
            isVideo = false,
            callbacks = object : SipTelecomCallCallbacks {
                override fun onAnswer(isVideo: Boolean) {}
                override fun onDisconnect() {}
                override fun onSetActive() {}
                override fun onSetInactive() {}
            }
        )
        fakeTelecom.setActive(callId1)
        fakeTelecom.setInactive(callId1)
        fakeTelecom.setActive(callId2)
        fakeTelecom.requestEndpointChange(callId2, SipTelecomEndpoint.SPEAKER)
        fakeTelecom.disconnect(callId1, SipDisconnectReason.LOCAL)
        fakeTelecom.disconnect(callId2, SipDisconnectReason.REMOTE)
        assertEquals(SipTelecomCallState.DISCONNECTED, fakeTelecom.getCallState(callId1))
        assertEquals(SipTelecomCallState.DISCONNECTED, fakeTelecom.getCallState(callId2))
        assertEquals(SipTelecomEndpoint.SPEAKER, fakeTelecom.getEndpoint(callId2))
    }

    @Test
    fun testPushPayloadUnifiedKeysAndMissedTimeout() {
        val unified = SipPushCallPayload.parse(
            mapOf(
                "sip_call_id" to "call-xyz@pbx",
                "from_uri" to "sip:alice@pbx.local",
                "display_name" to "Alice",
                "has_video" to "true"
            )
        )
        assertEquals("call-xyz@pbx", unified.sipCallId)
        assertEquals("sip:alice@pbx.local", unified.callerUri)
        assertEquals("Alice", unified.displayName)
        assertTrue(unified.hasVideo)

        val aliases = SipPushCallPayload.parse(
            mapOf(
                "call_id" to "legacy-id",
                "caller_uri" to "sip:bob@pbx.local",
                "caller_name" to "Bob",
                "video" to "0"
            )
        )
        assertEquals("legacy-id", aliases.sipCallId)
        assertEquals("sip:bob@pbx.local", aliases.callerUri)
        assertEquals("Bob", aliases.displayName)
        assertFalse(aliases.hasVideo)

        val missedLatch = java.util.concurrent.CountDownLatch(1)
        var missedId = ""
        var missedUri = ""
        val telecom = InMemoryTelecomCoordinator()
        val client = SipClient("1001", "10.0.2.2").apply {
            telecomCoordinator = telecom
            listener = object : SipClientListener {
                override fun onPushCallMissed(callId: String, callerUri: String) {
                    missedId = callId
                    missedUri = callerUri
                    missedLatch.countDown()
                }
            }
        }

        val handle = SipPushHandler.handleIncomingCallPush(
            context = null,
            client = client,
            data = mapOf(
                "sip_call_id" to "missing-invite-123",
                "caller_uri" to "sip:carol@pbx.local",
                "display_name" to "Carol",
                "has_video" to "false"
            ),
            timeoutMs = 200L,
            autoRegister = false
        )
        assertEquals(SipTelecomCallState.RINGING, telecom.getCallState("missing-invite-123"))
        assertTrue("Expected missed push timeout", missedLatch.await(2, java.util.concurrent.TimeUnit.SECONDS))
        assertEquals("missing-invite-123", missedId)
        assertEquals("sip:carol@pbx.local", missedUri)
        assertEquals(SipTelecomCallState.DISCONNECTED, telecom.getCallState("missing-invite-123"))
        assertTrue(handle.isMissed)

        // Pre-INVITE decline removes handle cleanly and rejects late INVITE without firing onIncomingCall
        var lateIncomingFired = false
        val declineClient = SipClient("1001", "10.0.2.2").apply {
            telecomCoordinator = telecom
            listener = object : SipClientListener {
                override fun onIncomingCall(call: SipCall) {
                    lateIncomingFired = true
                }
            }
        }
        val declinedHandle = SipPushHandler.handleIncomingCallPush(
            context = null,
            client = declineClient,
            data = mapOf("sip_call_id" to "declined-before-invite", "caller_uri" to "sip:dave@pbx.local"),
            timeoutMs = 2_000L,
            autoRegister = false
        )
        declinedHandle.buildTelecomCallbacks().onDisconnect()
        assertTrue(declinedHandle.isDeclined)
        assertEquals(SipTelecomCallState.DISCONNECTED, telecom.getCallState("declined-before-invite"))
        assertFalse(declineClient.attachPendingPushCall("declined-before-invite", SipCall(0L, "declined-before-invite", "sip:dave@pbx.local")))
        assertFalse(lateIncomingFired)

        // Duplicate push with same sip_call_id replaces prior timer safely
        val dupFirst = SipPushHandler.handleIncomingCallPush(
            context = null,
            client = declineClient,
            data = mapOf("sip_call_id" to "dup-call-id", "caller_uri" to "sip:eve@pbx.local"),
            timeoutMs = 100L,
            autoRegister = false
        )
        val dupSecond = SipPushHandler.handleIncomingCallPush(
            context = null,
            client = declineClient,
            data = mapOf("sip_call_id" to "dup-call-id", "caller_uri" to "sip:eve@pbx.local"),
            timeoutMs = 2_000L,
            autoRegister = false
        )
        Thread.sleep(200)
        assertTrue(declineClient.attachPendingPushCall("dup-call-id", SipCall(0L, "dup-call-id", "sip:eve@pbx.local")))
        assertTrue(dupSecond.isAttached)
        assertFalse(dupSecond.isMissed)
        assertTrue(dupFirst.isMissed)
    }

    @Test
    fun testMultiAccountConfigModelAndValidation() {
        val cfg = SipAccountConfig(username = "1001", domain = "10.0.2.2", password = "secret")
        assertEquals("1001", cfg.username)
        assertEquals("10.0.2.2", cfg.domain)
        assertEquals("secret", cfg.password)
        assertEquals("udp", cfg.transport)
        assertEquals(5060, cfg.port)
        assertTrue(cfg.verifyTlsCert)
        assertEquals("", cfg.mediaEnc)
        assertEquals(4, cfg.maxCalls)

        val client = SipClient("1001", "10.0.2.2")
        assertTrue(client.accounts.isEmpty())
        assertEquals(0, client.accountCount)
        assertEquals(null, client.defaultAccount)

        try {
            client.addAccount(SipAccountConfig(username = "", domain = "10.0.2.2"))
            org.junit.Assert.fail("Expected IllegalArgumentException for empty username")
        } catch (_: IllegalArgumentException) {}

        try {
            client.addAccount(SipAccountConfig(username = "1001", domain = ""))
            org.junit.Assert.fail("Expected IllegalArgumentException for empty domain")
        } catch (_: IllegalArgumentException) {}

        try {
            client.addAccount(SipAccountConfig(username = "1001", domain = "10.0.2.2", port = 70000))
            org.junit.Assert.fail("Expected IllegalArgumentException for out-of-range port")
        } catch (_: IllegalArgumentException) {}

        val acc = SipAccount(
            client = client,
            nativeHandle = 0L,
            ownsNativeHandle = false,
            cachedId = 42L,
            cachedAor = "sip:1001@10.0.2.2:5060",
            cachedUser = "1001",
            cachedDomain = "10.0.2.2"
        )
        assertEquals(42L, acc.id)
        assertEquals("sip:1001@10.0.2.2:5060", acc.aor)
        assertEquals("1001", acc.user)
        assertEquals("10.0.2.2", acc.domain)
        assertFalse(acc.isRegistered)
        assertEquals(SipRegistrationState.UNREGISTERED, acc.registrationState)
        assertEquals(-1, acc.reregister())
        assertEquals(-1, acc.unregister())
        assertEquals(-1, client.removeAccount(acc))
        assertEquals(-1, client.setDefaultAccount(acc))

        try {
            acc.sendMessage("", "hello")
            org.junit.Assert.fail("Expected IllegalArgumentException for empty peerUri")
        } catch (_: IllegalArgumentException) {}

        try {
            acc.sendMessage("1003", "")
            org.junit.Assert.fail("Expected IllegalArgumentException for empty text")
        } catch (_: IllegalArgumentException) {}

        try {
            acc.sendMessage("1003", "hello")
            org.junit.Assert.fail("Expected IllegalStateException when account handle is closed")
        } catch (_: IllegalStateException) {}

        acc.close()
        acc.destroy()

        val call = SipCall(nativeHandle = 0L, callId = "c1", remoteUri = "sip:1003@10.0.2.2", client = client)
        assertEquals(null, call.account)

        var regCallbackAccount: SipAccount? = null
        var msgCallbackAccount: SipAccount? = null
        var msgCallbackText: String? = null
        val listener = object : SipClientListener {
            override fun onAccountRegistrationStateChanged(
                account: SipAccount,
                state: SipRegistrationState,
                code: Int,
                reason: String
            ) {
                regCallbackAccount = account
            }

            override fun onAccountMessageReceived(account: SipAccount, message: SipIncomingMessage) {
                msgCallbackAccount = account
                msgCallbackText = message.text
            }
        }
        listener.onAccountRegistrationStateChanged(acc, SipRegistrationState.REGISTERED, 200, "OK")
        listener.onAccountMessageReceived(acc, SipIncomingMessage("sip:1003@pbx", "text/plain", "hi".toByteArray()))
        assertEquals(acc, regCallbackAccount)
        assertEquals(acc, msgCallbackAccount)
        assertEquals("hi", msgCallbackText)
    }
}
