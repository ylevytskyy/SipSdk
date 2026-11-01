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
        val client = SipClient("1001", "10.0.2.2", context).apply {
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
        assertEquals(0, client.register())
        assertTrue("Timeout waiting for registration", regLatch.await(10, TimeUnit.SECONDS))

        // Initiate call to Asterisk audio echo service extension 9001
        val call = client.makeCall("sip:9001@10.0.2.2:5060")
        assertNotNull("Expected non-null SipCall instance", call)
        call!!

        assertEquals("sip:9001@10.0.2.2:5060", call.remoteUri)
        assertTrue("Outgoing flag must be true", call.isOutgoing)
        assertFalse("Call must not be closed yet", call.isClosed)

        // Wait for call establishment
        assertTrue("Timeout waiting for call to be established", establishedLatch.await(10, TimeUnit.SECONDS))
        assertTrue("Call must be established", callEstablished)
        assertTrue("Call state callback must be dispatched on Main Looper", looperIsMainOnCallState)
        assertTrue("Audio session must be active", client.audioManager?.isAudioSessionActive == true)

        // Allow audio to stream and duration counter to tick
        Thread.sleep(1500)
        val duration = call.durationSeconds
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
}
