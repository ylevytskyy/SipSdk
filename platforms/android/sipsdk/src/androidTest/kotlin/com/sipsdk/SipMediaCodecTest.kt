package com.sipsdk

import android.os.Looper
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

@RunWith(AndroidJUnit4::class)
class SipMediaCodecTest {

    private val context get() = InstrumentationRegistry.getInstrumentation().targetContext

    @Test
    fun testMediaCodecDriverRegistrationAndKeyframeRequest() {
        val client = SipClient("1001", "10.0.2.2", context).apply {
            password = "test1001"
            port = 5060
            transport = "udp"
        }

        val regLatch = CountDownLatch(1)
        val establishedLatch = CountDownLatch(1)
        val closedLatch = CountDownLatch(1)

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
        }

        assertEquals(0, client.start())
        assertEquals(0, client.register())
        assertTrue("Timeout waiting for SIP registration", regLatch.await(10, TimeUnit.SECONDS))

        // Initiate video call to Asterisk PBX video echo (extension 9001)
        val call = client.makeCall("sip:9001@10.0.2.2:5060", withVideo = true)
        assertNotNull("Expected non-null SipCall", call)
        call!!
        assertTrue("Expected hasVideo == true", call.hasVideo)

        assertTrue("Timeout waiting for video call establishment", establishedLatch.await(10, TimeUnit.SECONDS))

        // Request an RTCP PLI keyframe recovery
        val kfRes = call.requestKeyframe()
        assertEquals("sip_call_request_keyframe should succeed", 0, kfRes)

        // Hang up call cleanly
        assertEquals(0, call.hangup())
        assertTrue("Timeout waiting for call to close", closedLatch.await(5, TimeUnit.SECONDS))

        client.stop()
    }
}
