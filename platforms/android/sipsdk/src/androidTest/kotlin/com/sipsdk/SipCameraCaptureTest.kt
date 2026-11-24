package com.sipsdk

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import java.nio.ByteBuffer
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicReference

@RunWith(AndroidJUnit4::class)
class SipCameraCaptureTest {

    private val context get() = InstrumentationRegistry.getInstrumentation().targetContext

    // Given a camera image that is still being delivered on the camera thread,
    // when the capture is stopped,
    // then the image stays valid until its delivery has finished.
    @Test
    fun stopKeepsTheImageBeingDeliveredValidUntilDeliveryFinishes() {
        val capture = SipCameraCapture(context, 0L)
        val deliveryStarted = CountDownLatch(1)
        val firstImage = AtomicBoolean(true)
        val imageValidAtEnd = AtomicReference<Boolean?>(null)

        capture.imageDeliveryOverride = { image ->
            if (firstImage.getAndSet(false)) {
                deliveryStarted.countDown()
                Thread.sleep(300)
                val stillValid = try {
                    image.planes
                    true
                } catch (_: IllegalStateException) {
                    false
                }
                imageValidAtEnd.set(stillValid)
            }
        }

        assertEquals(0, capture.start(640, 480, 30))
        assertTrue("Camera must deliver a frame", deliveryStarted.await(10, TimeUnit.SECONDS))

        capture.stop()

        assertEquals(
            "stop() closed the camera image while it was still being delivered",
            true,
            imageValidAtEnd.get()
        )
    }

    // Given a camera image that is still being delivered while more images arrive,
    // when the capture is stopped,
    // then no further image is delivered after stop() was called.
    @Test
    fun stopDeliversNoFurtherImagesOnceCalled() {
        val capture = SipCameraCapture(context, 0L)
        val deliveryStarted = CountDownLatch(1)
        val firstImage = AtomicBoolean(true)
        val stopCalled = AtomicBoolean(false)
        val deliveriesAfterStop = java.util.concurrent.atomic.AtomicInteger(0)

        capture.imageDeliveryOverride = { _ ->
            if (stopCalled.get()) deliveriesAfterStop.incrementAndGet()
            if (firstImage.getAndSet(false)) {
                deliveryStarted.countDown()
                // Long enough for the camera to queue more images meanwhile.
                Thread.sleep(500)
            }
        }

        assertEquals(0, capture.start(640, 480, 30))
        assertTrue("Camera must deliver a frame", deliveryStarted.await(10, TimeUnit.SECONDS))
        // Let the camera queue the next image behind the delivery in progress.
        Thread.sleep(200)

        stopCalled.set(true)
        capture.stop()

        assertEquals("Images were delivered after stop() was called", 0, deliveriesAfterStop.get())
    }

    // Given a camera image that is still being delivered,
    // when stop() is called on a thread that has been interrupted,
    // then stop() still waits for the delivery, does not throw, and the thread stays interrupted.
    @Test
    fun stopOnAnInterruptedThreadFinishesAndKeepsTheInterrupt() {
        val capture = SipCameraCapture(context, 0L)
        val deliveryStarted = CountDownLatch(1)
        val deliveryFinished = AtomicBoolean(false)
        val firstImage = AtomicBoolean(true)

        capture.imageDeliveryOverride = { _ ->
            if (firstImage.getAndSet(false)) {
                deliveryStarted.countDown()
                Thread.sleep(300)
                deliveryFinished.set(true)
            }
        }

        assertEquals(0, capture.start(640, 480, 30))
        assertTrue("Camera must deliver a frame", deliveryStarted.await(10, TimeUnit.SECONDS))

        var thrown: Throwable? = null
        Thread.currentThread().interrupt()
        try {
            capture.stop()
        } catch (t: Throwable) {
            thrown = t
        }
        val finishedWhenStopReturned = deliveryFinished.get()
        // Read and clear the flag before asserting, so later tests start clean.
        val stillInterrupted = Thread.interrupted()

        assertEquals("stop() must not throw on an interrupted thread", null, thrown)
        assertTrue("stop() must keep the thread's interrupt flag", stillInterrupted)
        assertTrue("stop() returned before the image in delivery was finished", finishedWhenStopReturned)
    }

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
        assertTrue("Camera capture must be running when C ABI start callback fires", client.isCameraCaptureRunning)

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
        assertFalse("Camera capture must stop when C ABI stop callback fires", client.isCameraCaptureRunning)

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
            yRowStride = width,
            uRowStride = width / 2,
            vRowStride = width / 2,
            yPixelStride = 1,
            uPixelStride = 1,
            vPixelStride = 1,
            rotationDegrees = 0,
            mirror = false,
            timestampNs = System.nanoTime()
        )

        // Test with semi-planar NV12/NV21 pixel strides and 270 degree rotation
        val uvInterleaved = ByteBuffer.allocateDirect((width / 2) * (height / 2) * 2)
        for (i in 0 until (width / 2) * (height / 2)) {
            uvInterleaved.put(100.toByte()) // U
            uvInterleaved.put(200.toByte()) // V
        }
        uvInterleaved.flip()

        SipCameraCapture.nativeDeliverFrame(
            clientHandle = 0L,
            width = width,
            height = height,
            yBuffer = yBuf,
            uBuffer = uvInterleaved,
            vBuffer = uvInterleaved,
            yRowStride = width,
            uRowStride = width,
            vRowStride = width,
            yPixelStride = 1,
            uPixelStride = 2,
            vPixelStride = 2,
            rotationDegrees = 270,
            mirror = true,
            timestampNs = System.nanoTime()
        )

        client.stop()
    }
}
