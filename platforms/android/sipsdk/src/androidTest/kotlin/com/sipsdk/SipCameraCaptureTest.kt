package com.sipsdk

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import java.nio.ByteBuffer

@RunWith(AndroidJUnit4::class)
class SipCameraCaptureTest {

    private val context get() = InstrumentationRegistry.getInstrumentation().targetContext

    @Test
    fun testCameraCaptureAndControlsLifecycle() {
        val client = SipClient("1001", TestConfig.host, context).apply {
            password = "test1001"
            port = 5060
            transport = "udp"
        }

        assertEquals(0, client.start())

        // Initial camera position & video state
        assertEquals(SipCameraPosition.FRONT, client.cameraPosition)
        assertTrue(client.isLocalVideoEnabled)
        assertFalse(client.isLocalPreviewActive)

        // Start local preview
        assertEquals(0, client.startLocalPreview())
        assertTrue("Local preview must be active", client.isLocalPreviewActive)

        // Switch camera
        assertEquals(0, client.switchCamera())
        assertEquals(SipCameraPosition.BACK, client.cameraPosition)

        assertEquals(0, client.switchCamera())
        assertEquals(SipCameraPosition.FRONT, client.cameraPosition)

        // Set camera explicitly
        assertEquals(0, client.setCamera(SipCameraPosition.BACK))
        assertEquals(SipCameraPosition.BACK, client.cameraPosition)

        // Toggle local video enabled / disabled
        assertEquals(0, client.setLocalVideoEnabled(false))
        assertFalse("Local video must be disabled", client.isLocalVideoEnabled)

        assertEquals(0, client.setLocalVideoEnabled(true))
        assertTrue("Local video must be enabled", client.isLocalVideoEnabled)

        // Stop local preview
        assertEquals(0, client.stopLocalPreview())
        assertFalse("Local preview must not be active", client.isLocalPreviewActive)

        // Test direct frame delivery via nativeDeliverFrame
        val width = 320
        val height = 240
        val yBuf = ByteBuffer.allocateDirect(width * height)
        val uBuf = ByteBuffer.allocateDirect((width / 2) * (height / 2))
        val vBuf = ByteBuffer.allocateDirect((width / 2) * (height / 2))
        for (i in 0 until width * height) yBuf.put(128.toByte())
        for (i in 0 until (width / 2) * (height / 2)) {
            uBuf.put(128.toByte())
            vBuf.put(128.toByte())
        }
        yBuf.flip()
        uBuf.flip()
        vBuf.flip()

        // Call nativeDeliverFrame directly
        SipCameraCapture.nativeDeliverFrame(
            clientHandle = 0L, // Safe with 0
            width = width,
            height = height,
            yBuffer = yBuf,
            uBuffer = uBuf,
            vBuffer = vBuf,
            yStride = width,
            uStride = width / 2,
            vStride = width / 2,
            timestampNs = System.nanoTime()
        )

        client.stop()
    }
}
