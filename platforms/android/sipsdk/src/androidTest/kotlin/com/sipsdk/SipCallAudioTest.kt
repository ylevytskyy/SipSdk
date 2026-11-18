package com.sipsdk

import android.os.Looper
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

@RunWith(AndroidJUnit4::class)
class SipCallAudioTest {

    private val context get() = InstrumentationRegistry.getInstrumentation().targetContext

    @Test
    fun testAudioCallEchoSessionLifecycle() {
        val client = SipClient("1001", TestConfig.host, context).apply {
            password = "test1001"
            port = 5060
            transport = "udp"
        }

        val regLatch = CountDownLatch(1)
        val establishedLatch = CountDownLatch(1)
        val closedLatch = CountDownLatch(1)
        val holdLatch = CountDownLatch(1)

        var callEstablished = false
        var callClosed = false
        var holdReceived = false
        var looperIsMainOnCallState = false

        client.listener = object : SipClientListener {
            override fun onRegistrationStateChanged(state: SipRegistrationState, code: Int, reason: String) {
                if (state == SipRegistrationState.REGISTERED && code == 200) {
                    regLatch.countDown()
                }
            }

            override fun onCallStateChanged(call: SipCall, state: SipCallState, code: Int, reason: String) {
                if (Looper.myLooper() == Looper.getMainLooper()) {
                    looperIsMainOnCallState = true
                }
                when (state) {
                    SipCallState.ESTABLISHED -> {
                        callEstablished = true
                        establishedLatch.countDown()
                    }
                    SipCallState.CLOSED -> {
                        callClosed = true
                        closedLatch.countDown()
                    }
                    else -> {}
                }
            }

            override fun onCallMediaChanged(call: SipCall, width: Int, height: Int, onHold: Boolean, isMuted: Boolean) {
                if (onHold) {
                    holdReceived = true
                    holdLatch.countDown()
                }
            }
        }

        assertEquals(0, client.start())

        // Verify audio driver is initialized to OpenSL ES
        val audioDrivers = client.getAudioDriver()
        assertEquals("opensles", audioDrivers.first)
        assertEquals("opensles", audioDrivers.second)

        assertEquals(0, client.register())
        assertTrue("Timeout waiting for registration", regLatch.await(10, TimeUnit.SECONDS))

        // Initiate call to Asterisk audio echo service extension 9001
        val call = client.makeCall("sip:9001@${TestConfig.host}:5060")
        assertNotNull("Expected non-null SipCall instance", call)
        call!!

        assertEquals("sip:9001@${TestConfig.host}:5060", call.remoteUri)
        assertTrue("Outgoing flag must be true", call.isOutgoing)
        assertFalse("Call must not be closed yet", call.isClosed)

        // Wait for call establishment
        assertTrue("Timeout waiting for call to be established", establishedLatch.await(10, TimeUnit.SECONDS))
        assertTrue("Call must be established", callEstablished)
        assertTrue("Call state callback must be dispatched on Main Looper", looperIsMainOnCallState)
        assertTrue("Audio session must be active", client.audioManager?.isAudioSessionActive == true)

        // Allow audio to stream and duration counter to tick
        var duration = 0L
        for (i in 0..40) {
            Thread.sleep(100)
            duration = call.durationSeconds
            if (duration >= 1L) break
        }
        assertTrue("Call duration must be >= 1 second, got $duration", duration >= 1L)

        // Telephony controls: microphone mute and speakerphone
        client.setMuted(true)
        assertTrue("Client must report isMicrophoneMuted == true", client.isMicrophoneMuted)
        client.setMuted(false)
        assertFalse("Client must report isMicrophoneMuted == false", client.isMicrophoneMuted)

        client.setSpeakerphone(true)
        assertTrue("Client must report isSpeakerEnabled == true", client.isSpeakerEnabled)
        client.setSpeakerphone(false)
        assertFalse("Client must report isSpeakerEnabled == false", client.isSpeakerEnabled)

        // Hold and resume
        val holdRc = call.setHold(true)
        assertEquals("setHold(true) must succeed", 0, holdRc)
        assertTrue("Timeout waiting for hold media event", holdLatch.await(5, TimeUnit.SECONDS))
        assertTrue("Hold media event received", holdReceived)

        val resumeRc = call.setHold(false)
        assertEquals("setHold(false) must succeed", 0, resumeRc)

        // DTMF transmission
        val dtmfRc = call.sendDtmf("123#")
        assertEquals("sendDtmf must return 0", 0, dtmfRc)

        // Hangup
        val hangupRc = call.hangup(0, "Hangup")
        assertEquals("hangup must return 0", 0, hangupRc)

        assertTrue("Timeout waiting for call to close", closedLatch.await(10, TimeUnit.SECONDS))
        assertTrue("Call must reach CLOSED state", callClosed)
        assertTrue("call.isClosed must be true", call.isClosed)

        // Audio session must be released after call teardown
        assertFalse("Audio session must be released after call terminates", client.audioManager?.isAudioSessionActive == true)

        client.destroy()
    }

    @Test
    fun testTlsSrtpOpusStatsAndTransferValidation() {
        val client = SipClient("1004", TestConfig.host, context).apply {
            password = "test1004"
            port = 5061
            transport = "tls"
            verifyTlsCert = false
            mediaEnc = "srtp-mand"
        }

        val regLatch = CountDownLatch(1)
        val establishedLatch = CountDownLatch(1)
        val statsLatch = CountDownLatch(1)
        val closedLatch = CountDownLatch(1)
        var observedStats: SipCallStats? = null

        client.listener = object : SipClientListener {
            override fun onRegistrationStateChanged(state: SipRegistrationState, code: Int, reason: String) {
                if (state == SipRegistrationState.REGISTERED && code == 200) {
                    regLatch.countDown()
                }
            }

            override fun onCallStateChanged(call: SipCall, state: SipCallState, code: Int, reason: String) {
                when (state) {
                    SipCallState.ESTABLISHED -> establishedLatch.countDown()
                    SipCallState.CLOSED -> closedLatch.countDown()
                    else -> {}
                }
            }

            override fun onCallStats(call: SipCall, stats: SipCallStats) {
                observedStats = stats
                statsLatch.countDown()
            }
        }

        assertEquals(0, client.start())
        assertEquals(0, client.register())
        assertTrue("Timeout waiting for TLS registration", regLatch.await(10, TimeUnit.SECONDS))

        val call = client.makeCall("sip:9001@${TestConfig.host}:5061")
        assertNotNull(call)
        call!!

        assertTrue("Timeout waiting for TLS+SRTP call established", establishedLatch.await(10, TimeUnit.SECONDS))
        assertTrue("TLS + SRTP call must report isSecure=true", call.isSecure)
        assertTrue("Timeout waiting for periodic onCallStats", statsLatch.await(6, TimeUnit.SECONDS))

        val liveStats = observedStats ?: call.getStats()
        assertNotNull("Expected call stats", liveStats)
        assertTrue("Expected opus/G722/PCM codec, got ${liveStats?.audioCodec}", liveStats?.audioCodec?.isNotEmpty() == true)
        assertTrue("Invalid empty blind transfer must fail", call.transfer("") != 0)

        assertEquals(0, call.hangup(0, "Hangup"))
        assertTrue("Timeout waiting for call close", closedLatch.await(10, TimeUnit.SECONDS))
        client.destroy()
    }

    @Test
    fun testMultiCallAutoHoldSwapCapAndCoreTelecom() {
        val telecom = CoreTelecomCoordinator(context)
        val client = SipClient("1001", TestConfig.host, context).apply {
            password = "test1001"
            port = 5060
            transport = "udp"
            maxCalls = 4
            telecomCoordinator = telecom
        }

        val regLatch = CountDownLatch(1)
        val c1Estab = CountDownLatch(1)
        val c1Hold = CountDownLatch(1)
        val c2Estab = CountDownLatch(1)
        val c2Hold = CountDownLatch(1)
        val swapEstab = CountDownLatch(1)
        val busyLatch = CountDownLatch(1)

        var call1Id = ""
        var call2Id = ""
        var busyCaller = ""

        client.listener = object : SipClientListener {
            override fun onRegistrationStateChanged(state: SipRegistrationState, code: Int, reason: String) {
                if (state == SipRegistrationState.REGISTERED && code == 200) {
                    regLatch.countDown()
                }
            }

            override fun onCallStateChanged(call: SipCall, state: SipCallState, code: Int, reason: String) {
                when (call.callId) {
                    call1Id -> when (state) {
                        SipCallState.ESTABLISHED -> {
                            c1Estab.countDown()
                            if (c2Estab.count == 0L) swapEstab.countDown()
                        }
                        SipCallState.HOLD -> c1Hold.countDown()
                        else -> {}
                    }
                    call2Id -> when (state) {
                        SipCallState.ESTABLISHED -> c2Estab.countDown()
                        SipCallState.HOLD -> c2Hold.countDown()
                        else -> {}
                    }
                }
            }

            override fun onCallRejectedBusy(callerUri: String) {
                busyCaller = callerUri
                busyLatch.countDown()
            }
        }

        assertEquals(0, client.start())
        assertEquals(0, client.register())
        assertTrue("Timeout waiting for registration", regLatch.await(10, TimeUnit.SECONDS))

        // 1. Two concurrent calls to 9001 -> call1 auto-holds when call2 dials, then swap back.
        val call1 = client.dial("sip:9001@${TestConfig.host}:5060", withVideo = false)
        call1Id = call1.callId
        assertTrue("call1 did not establish", c1Estab.await(10, TimeUnit.SECONDS))
        assertNotNull("Telecom state should exist for call1", telecom.getCallState(call1Id))

        val call2 = client.dial("sip:9001@${TestConfig.host}:5060", withVideo = false)
        call2Id = call2.callId
        assertTrue("call1 did not auto-hold", c1Hold.await(6, TimeUnit.SECONDS))
        assertTrue("call2 did not establish", c2Estab.await(10, TimeUnit.SECONDS))
        assertTrue("call1 must be on hold", call1.isOnHold)
        assertEquals(SipTelecomCallState.HELD, telecom.getCallState(call1Id))
        assertEquals(SipTelecomCallState.ACTIVE, telecom.getCallState(call2Id))

        assertEquals(0, call1.setHold(false))
        assertTrue("call2 did not auto-hold on swap", c2Hold.await(6, TimeUnit.SECONDS))
        assertTrue("call1 did not resume on swap", swapEstab.await(6, TimeUnit.SECONDS))
        assertFalse(call1.isOnHold)
        assertTrue(call2.isOnHold)

        telecom.requestEndpointChange(call1Id, SipTelecomEndpoint.SPEAKER)
        assertEquals(SipTelecomEndpoint.SPEAKER, telecom.getEndpoint(call1Id))
        telecom.requestEndpointChange(call1Id, SipTelecomEndpoint.EARPIECE)

        call1.hangup(0, "Hangup")
        call2.hangup(0, "Hangup")
        Thread.sleep(400)

        // 2. Re-register with maxCalls = 1 -> outgoing throws SipBusyException, 9003 callback triggers 486.
        client.maxCalls = 1
        assertEquals(0, client.register())
        Thread.sleep(400)

        val warmEstab = CountDownLatch(1)
        val warmClosed = CountDownLatch(1)
        val cbEstab = CountDownLatch(1)
        val cbClosed = CountDownLatch(1)
        val capEstab = CountDownLatch(1)
        var warmId = ""
        var cbId = ""
        var capId = ""

        client.listener = object : SipClientListener {
            override fun onCallStateChanged(call: SipCall, state: SipCallState, code: Int, reason: String) {
                when (call.callId) {
                    warmId -> when (state) {
                        SipCallState.ESTABLISHED -> warmEstab.countDown()
                        SipCallState.CLOSED -> warmClosed.countDown()
                        else -> {}
                    }
                    cbId -> when (state) {
                        SipCallState.ESTABLISHED -> cbEstab.countDown()
                        SipCallState.CLOSED -> cbClosed.countDown()
                        else -> {}
                    }
                    capId -> if (state == SipCallState.ESTABLISHED) capEstab.countDown()
                }
            }

            override fun onCallRejectedBusy(callerUri: String) {
                busyCaller = callerUri
                busyLatch.countDown()
            }
        }

        val warm = client.dial("sip:9001@${TestConfig.host}:5060", withVideo = false)
        warmId = warm.callId
        assertTrue("warm call did not establish", warmEstab.await(10, TimeUnit.SECONDS))

        var threwBusy = false
        try {
            client.dial("sip:9001@${TestConfig.host}:5060", withVideo = false)
        } catch (_: SipBusyException) {
            threwBusy = true
        }
        assertTrue("Expected SipBusyException over maxCalls=1", threwBusy)

        warm.hangup(0, "Hangup")
        assertTrue("warm call did not close", warmClosed.await(6, TimeUnit.SECONDS))

        val cb = client.dial("sip:9003@${TestConfig.host}:5060", withVideo = false)
        cbId = cb.callId
        assertTrue("9003 callback trigger did not establish", cbEstab.await(8, TimeUnit.SECONDS))
        cb.hangup(0, "Hangup")
        assertTrue("9003 callback trigger did not close", cbClosed.await(6, TimeUnit.SECONDS))

        val cap = client.dial("sip:9001@${TestConfig.host}:5060", withVideo = false)
        capId = cap.callId
        assertTrue("cap call did not establish", capEstab.await(10, TimeUnit.SECONDS))

        assertTrue("Expected onCallRejectedBusy on 9003 incoming callback", busyLatch.await(10, TimeUnit.SECONDS))
        assertTrue("Caller URI must be non-empty", busyCaller.isNotEmpty())
        assertFalse("Existing cap call must stay active", cap.isClosed)

        cap.hangup(0, "Hangup")
        client.destroy()
    }
}
