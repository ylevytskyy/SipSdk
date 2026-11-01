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
class SipCallVideoTest {

    private val context get() = InstrumentationRegistry.getInstrumentation().targetContext

    @Test
    fun testTwoWayVideoCallWithAsteriskEchoAndSurfaceView() {
        val client = SipClient("1001", "10.0.2.2", context).apply {
            password = "test1001"
            port = 5060
            transport = "udp"
        }

        val remoteSurface = SipSurfaceView(context)
        val localSurface = SipSurfaceView(context)
        client.remoteVideoView = remoteSurface
        client.localVideoView = localSurface

        val regLatch = CountDownLatch(1)
        val establishedLatch = CountDownLatch(1)
        val mediaLatch = CountDownLatch(1)
        val closedLatch = CountDownLatch(1)

        var callEstablished = false
        var callClosed = false
        var mediaReceived = false
        var mediaWidth = 0
        var mediaHeight = 0
        var looperIsMainOnMedia = false

        client.listener = object : SipClientListener {
            override fun onRegistrationStateChanged(state: SipRegistrationState, code: Int, reason: String) {
                if (state == SipRegistrationState.REGISTERED && code == 200) {
                    regLatch.countDown()
                }
            }

            override fun onCallStateChanged(call: SipCall, state: SipCallState, code: Int, reason: String) {
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
                if (Looper.myLooper() == Looper.getMainLooper()) {
                    looperIsMainOnMedia = true
                }
                if (width > 0 && height > 0) {
                    mediaReceived = true
                    mediaWidth = width
                    mediaHeight = height
                    mediaLatch.countDown()
                }
            }
        }

        assertEquals(0, client.start())
        assertEquals(0, client.startLocalPreview())
        assertTrue("Local preview must be active", client.isLocalPreviewActive)

        assertEquals(0, client.register())
        assertTrue("Timeout waiting for registration", regLatch.await(10, TimeUnit.SECONDS))

        // Initiate video call to Asterisk video echo service extension 9001
        val call = client.makeCall("sip:9001@10.0.2.2:5060", withVideo = true)
        assertNotNull("Expected non-null SipCall instance", call)
        call!!

        assertEquals("sip:9001@10.0.2.2:5060", call.remoteUri)
        assertTrue("Outgoing flag must be true", call.isOutgoing)
        assertTrue("hasVideo flag must be true", call.hasVideo)
        assertFalse("Call must not be closed yet", call.isClosed)

        // Wait for call establishment
        assertTrue("Timeout waiting for video call to be established", establishedLatch.await(10, TimeUnit.SECONDS))
        assertTrue("Call must be established", callEstablished)

        // Wait for video media resolution negotiation
        assertTrue("Timeout waiting for video media parameters", mediaLatch.await(10, TimeUnit.SECONDS))
        assertTrue("Video media must be received", mediaReceived)
        assertTrue("Media width must be > 0, got $mediaWidth", mediaWidth > 0)
        assertTrue("Media height must be > 0, got $mediaHeight", mediaHeight > 0)
        assertTrue("onCallMediaChanged must be dispatched on Main Looper", looperIsMainOnMedia)

        // Assert SipSurfaceView dimensions updated
        assertEquals("remoteSurface videoWidth must match mediaWidth", mediaWidth, remoteSurface.videoWidth)
        assertEquals("remoteSurface videoHeight must match mediaHeight", mediaHeight, remoteSurface.videoHeight)
        assertTrue("remoteSurface renderedFrameCount must be >= 1", remoteSurface.renderedFrameCount >= 1L)

        // Mid-call camera flipping
        assertEquals(0, client.switchCamera())
        assertEquals(SipCameraPosition.BACK, client.cameraPosition)

        assertEquals(0, client.switchCamera())
        assertEquals(SipCameraPosition.FRONT, client.cameraPosition)

        // RTCP PLI keyframe request
        val kfRes = call.requestKeyframe()
        assertEquals("sip_call_request_keyframe must return 0", 0, kfRes)

        // Dynamic local video toggle
        assertEquals(0, client.setLocalVideoEnabled(false))
        assertFalse(client.isLocalVideoEnabled)
        assertEquals(0, client.setLocalVideoEnabled(true))
        assertTrue(client.isLocalVideoEnabled)

        // Hangup cleanly
        assertEquals(0, call.hangup())
        assertTrue("Timeout waiting for call to close", closedLatch.await(5, TimeUnit.SECONDS))
        assertTrue("Call must be closed", callClosed)

        client.stop()
    }
}
