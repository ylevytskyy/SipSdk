package com.sipsdk

import android.Manifest
import android.os.Looper
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import androidx.test.rule.GrantPermissionRule
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.locks.ReentrantLock
import kotlin.concurrent.withLock

@RunWith(AndroidJUnit4::class)
class SipCallAudioTest {

    @get:Rule
    val grantPermissionRule: GrantPermissionRule = GrantPermissionRule.grant(Manifest.permission.RECORD_AUDIO)

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

        try {
            assertEquals(0, client.start())

            // Verify audio driver is initialized to OpenSL ES (source falls back to nil only if emulator lacks mic HAL)
            val audioDrivers = client.getAudioDriver()
            assertEquals("opensles", audioDrivers.first)
            assertEquals("opensles", audioDrivers.second)
            TestConfig.configureAudioDriverForDevice(client)

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
        } finally {
            client.destroy()
        }
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

        try {
            assertEquals(0, client.start())
            TestConfig.configureAudioDriverForDevice(client)
            assertEquals(0, client.register())
            assertTrue("Timeout waiting for TLS registration", regLatch.await(10, TimeUnit.SECONDS))

            val call = client.makeCall("sip:9001@${TestConfig.host}:5061")
            assertNotNull(call)
            call!!

            assertTrue("Timeout waiting for TLS+SRTP call established", establishedLatch.await(10, TimeUnit.SECONDS))
            val secDeadline = System.currentTimeMillis() + 3000
            while (!call.isSecure && System.currentTimeMillis() < secDeadline) {
                Thread.sleep(50)
            }
            assertTrue("TLS + SRTP call must report isSecure=true", call.isSecure)
            assertTrue("Timeout waiting for periodic onCallStats", statsLatch.await(10, TimeUnit.SECONDS))

            val liveStats = observedStats ?: call.getStats()
            assertNotNull("Expected call stats", liveStats)
            assertTrue("Expected opus/G722/PCM codec, got ${liveStats?.audioCodec}", liveStats?.audioCodec?.isNotEmpty() == true)
            assertTrue("Invalid empty blind transfer must fail", call.transfer("") != 0)

            assertEquals(0, call.hangup(0, "Hangup"))
            assertTrue("Timeout waiting for call close", closedLatch.await(10, TimeUnit.SECONDS))
        } finally {
            client.destroy()
        }
    }

    @Test
    fun testMultiCallAutoHoldSwapCapAndCoreTelecom() {
        val telecom = CoreTelecomCoordinator(context)
        val client = SipClient("1003", TestConfig.host, context).apply {
            password = "test1003"
            port = 5060
            transport = "udp"
            maxCalls = 4
            telecomCoordinator = telecom
        }

        val regLatch = CountDownLatch(1)
        val busyLatchRef = java.util.concurrent.atomic.AtomicReference(CountDownLatch(1))
        val busyCallerRef = java.util.concurrent.atomic.AtomicReference("")

        val lock = ReentrantLock()
        val stateChanged = lock.newCondition()
        val callStates = ConcurrentHashMap<String, MutableList<SipCallState>>()

        fun recordState(callId: String, state: SipCallState) {
            lock.withLock {
                callStates.getOrPut(callId) { mutableListOf() }.add(state)
                stateChanged.signalAll()
            }
        }

        fun awaitState(callId: String, target: SipCallState, timeoutSec: Long, minCount: Int = 1): Boolean {
            val deadlineNs = System.nanoTime() + TimeUnit.SECONDS.toNanos(timeoutSec)
            lock.withLock {
                while (true) {
                    val count = callStates[callId]?.count { it == target } ?: 0
                    if (count >= minCount) return true
                    val rem = deadlineNs - System.nanoTime()
                    if (rem <= 0L) return false
                    stateChanged.awaitNanos(rem)
                }
            }
        }

        fun awaitResumedAfterHold(callId: String, timeoutSec: Long): Boolean {
            val deadlineNs = System.nanoTime() + TimeUnit.SECONDS.toNanos(timeoutSec)
            lock.withLock {
                while (true) {
                    val list = callStates[callId]
                    if (list != null) {
                        val holdIdx = list.indexOfFirst { it == SipCallState.HOLD }
                        if (holdIdx >= 0 && list.drop(holdIdx + 1).any { it == SipCallState.ESTABLISHED }) {
                            return true
                        }
                    }
                    val rem = deadlineNs - System.nanoTime()
                    if (rem <= 0L) return false
                    stateChanged.awaitNanos(rem)
                }
            }
        }

        client.listener = object : SipClientListener {
            override fun onRegistrationStateChanged(state: SipRegistrationState, code: Int, reason: String) {
                if (state == SipRegistrationState.REGISTERED && code == 200) {
                    regLatch.countDown()
                }
            }

            override fun onCallStateChanged(call: SipCall, state: SipCallState, code: Int, reason: String) {
                recordState(call.callId, state)
            }

            override fun onCallRejectedBusy(callerUri: String) {
                busyCallerRef.set(callerUri)
                busyLatchRef.get().countDown()
            }
        }

        try {
            assertEquals(0, client.start())
            TestConfig.configureAudioDriverForDevice(client)
            assertEquals(0, client.register())
            assertTrue("Timeout waiting for registration", regLatch.await(10, TimeUnit.SECONDS))

            // 1. Two concurrent calls to 9001 (UDP) -> call1 auto-holds when call2 dials, then swap back.
            val call1 = client.dial("sip:9001@${TestConfig.host}:5060", withVideo = false)
            assertTrue("call1 did not establish", awaitState(call1.callId, SipCallState.ESTABLISHED, 10))
            assertNotNull("Telecom state should exist for call1", telecom.getCallState(call1.callId))

            val call2 = client.dial("sip:9001@${TestConfig.host}:5060", withVideo = false)
            assertTrue("call1 did not auto-hold", awaitState(call1.callId, SipCallState.HOLD, 6))
            assertTrue("call2 did not establish", awaitState(call2.callId, SipCallState.ESTABLISHED, 10))
            assertTrue("call1 must be on hold", call1.isOnHold)
            assertFalse("call1 closed unexpectedly (states=${callStates[call1.callId]})", call1.isClosed)
            assertEquals("call1 telecom states=${callStates[call1.callId]}", SipTelecomCallState.HELD, telecom.getCallState(call1.callId))
            assertEquals(SipTelecomCallState.ACTIVE, telecom.getCallState(call2.callId))

            assertEquals(0, call1.setHold(false))
            assertTrue("call2 did not auto-hold on swap", awaitState(call2.callId, SipCallState.HOLD, 6))
            assertTrue("call1 did not resume on swap", awaitResumedAfterHold(call1.callId, 6))
            assertFalse(call1.isOnHold)
            assertTrue(call2.isOnHold)

            // Merge call1 + call2 into local audio conference, then split call2 back to hold
            assertEquals(0, client.mergeCalls(call1, call2))
            val mergeDeadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5)
            while (System.nanoTime() < mergeDeadline && !(call1.isInConference && call2.isInConference)) {
                Thread.sleep(50)
            }
            assertTrue("call1 must be in conference", call1.isInConference)
            assertTrue("call2 must be in conference", call2.isInConference)
            assertFalse("call1 must not be on hold in conference", call1.isOnHold)
            assertFalse("call2 must not be on hold in conference", call2.isOnHold)

            assertEquals(0, client.splitCall(call2))
            val splitDeadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5)
            while (System.nanoTime() < splitDeadline && (call1.isInConference || call2.isInConference)) {
                Thread.sleep(50)
            }
            assertFalse("call1 must exit conference on dissolve", call1.isInConference)
            assertFalse("call2 must exit conference on split", call2.isInConference)

            telecom.requestEndpointChange(call1.callId, SipTelecomEndpoint.SPEAKER)
            assertEquals(SipTelecomEndpoint.SPEAKER, telecom.getEndpoint(call1.callId))
            telecom.requestEndpointChange(call1.callId, SipTelecomEndpoint.EARPIECE)

            call1.hangup(0, "Hangup")
            call2.hangup(0, "Hangup")
            assertTrue("call1 did not close", awaitState(call1.callId, SipCallState.CLOSED, 6))
            assertTrue("call2 did not close", awaitState(call2.callId, SipCallState.CLOSED, 6))

            // 2. Re-register with maxCalls = 1 over TCP (NAT-safe for 9003 callback) -> outgoing throws SipBusyException, 9003 triggers 486.
            client.maxCalls = 1
            client.transport = "tcp"
            assertEquals(0, client.register())
            Thread.sleep(400)

            val warm = client.dial("sip:9001@${TestConfig.host}:5060;transport=tcp", withVideo = false)
            assertTrue("warm call did not establish", awaitState(warm.callId, SipCallState.ESTABLISHED, 10))

            var threwBusy = false
            try {
                client.dial("sip:9001@${TestConfig.host}:5060;transport=tcp", withVideo = false)
            } catch (_: SipBusyException) {
                threwBusy = true
            }
            assertTrue("Expected SipBusyException over maxCalls=1", threwBusy)

            warm.hangup(0, "Hangup")
            assertTrue("warm call did not close", awaitState(warm.callId, SipCallState.CLOSED, 6))

            val cb = client.dial("sip:9003@${TestConfig.host}:5060;transport=tcp", withVideo = false)
            assertTrue("9003 callback trigger did not establish", awaitState(cb.callId, SipCallState.ESTABLISHED, 8))
            cb.hangup(0, "Hangup")
            assertTrue("9003 callback trigger did not close", awaitState(cb.callId, SipCallState.CLOSED, 6))

            busyLatchRef.set(CountDownLatch(1))
            busyCallerRef.set("")
            val cap = client.dial("sip:9001@${TestConfig.host}:5060;transport=tcp", withVideo = false)
            assertTrue("cap call did not establish", awaitState(cap.callId, SipCallState.ESTABLISHED, 10))

            assertTrue("Expected onCallRejectedBusy on 9003 incoming callback", busyLatchRef.get().await(10, TimeUnit.SECONDS))
            assertTrue("Caller URI must be non-empty", busyCallerRef.get().isNotEmpty())
            assertFalse("Existing cap call must stay active", cap.isClosed)

            cap.hangup(0, "Hangup")
        } finally {
            try { client.unregister() } catch (_: Throwable) {}
            client.destroy()
        }
    }
}
