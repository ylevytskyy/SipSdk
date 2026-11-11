package com.sipsdk

import android.content.Context
import android.graphics.ImageFormat
import android.hardware.camera2.*
import android.media.ImageReader
import android.os.Handler
import android.os.HandlerThread
import android.util.Log
import java.nio.ByteBuffer
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean

class SipCameraCapture(
    private val context: Context?,
    private val clientHandle: Long
) {
    companion object {
        private const val TAG = "SipCameraCapture"
        private const val CLOSE_WAIT_MS = 2000L

        init {
            try {
                System.loadLibrary("sipcore")
            } catch (_: UnsatisfiedLinkError) {}
        }

        @JvmStatic
        external fun nativeDeliverFrame(
            clientHandle: Long,
            width: Int,
            height: Int,
            yBuffer: ByteBuffer,
            uBuffer: ByteBuffer,
            vBuffer: ByteBuffer,
            yRowStride: Int,
            uRowStride: Int,
            vRowStride: Int,
            yPixelStride: Int,
            uPixelStride: Int,
            vPixelStride: Int,
            rotationDegrees: Int,
            mirror: Boolean,
            timestampNs: Long
        )
    }

    var previewRotationDegrees: Int = 0
        private set

    private var backgroundThread: HandlerThread? = null
    private var backgroundHandler: Handler? = null
    private var cameraDevice: CameraDevice? = null
    private var captureSession: CameraCaptureSession? = null
    private var imageReader: ImageReader? = null
    private var previewSurface: android.view.Surface? = null

    private val isRunningFlag = AtomicBoolean(false)
    private val isEnabledFlag = AtomicBoolean(true)
    private var currentPositionVal = SipCameraPosition.FRONT

    val isRunning: Boolean get() = isRunningFlag.get()
    val isEnabled: Boolean get() = isEnabledFlag.get()
    val currentPosition: SipCameraPosition get() = currentPositionVal

    private var width: Int = 640
    private var height: Int = 480
    private var fps: Int = 30

    /**
     * Test hook. When set, each camera image is handed to it on the camera
     * thread instead of being delivered to native code.
     */
    @Volatile
    internal var imageDeliveryOverride: ((android.media.Image) -> Unit)? = null

    fun start(width: Int = 640, height: Int = 480, fps: Int = 30): Int {
        if (isRunningFlag.get()) return 0
        this.width = width
        this.height = height
        this.fps = fps

        startBackgroundThread()
        isRunningFlag.set(true)

        val cameraManager = context?.getSystemService(Context.CAMERA_SERVICE) as? CameraManager
        if (cameraManager != null) {
            try {
                openCamera(cameraManager)
            } catch (e: Exception) {
                Log.w(TAG, "Camera2 open failed, falling back to synthetic generator: ${e.message}")
                startSyntheticGenerator()
            }
        } else {
            startSyntheticGenerator()
        }
        return 0
    }

    fun stop() {
        if (!isRunningFlag.getAndSet(false)) return

        stopSyntheticGenerator()
        closeCamera()
        stopBackgroundThread()
    }

    fun switchCamera(): Int {
        val newPos = if (currentPositionVal == SipCameraPosition.FRONT) {
            SipCameraPosition.BACK
        } else {
            SipCameraPosition.FRONT
        }
        return setCamera(newPos)
    }

    fun setCamera(position: SipCameraPosition): Int {
        if (currentPositionVal == position) return 0
        currentPositionVal = position
        if (isRunningFlag.get()) {
            closeCamera()
            val cameraManager = context?.getSystemService(Context.CAMERA_SERVICE) as? CameraManager
            if (cameraManager != null) {
                try {
                    openCamera(cameraManager)
                } catch (e: Exception) {
                    Log.w(TAG, "Reopening camera failed: ${e.message}")
                }
            }
        }
        return 0
    }

    fun setEnabled(enabled: Boolean) {
        isEnabledFlag.set(enabled)
    }

    fun setPreviewSurface(surface: android.view.Surface?) {
        previewSurface = surface
        if (isRunningFlag.get()) {
            closeCamera()
            val cameraManager = context?.getSystemService(Context.CAMERA_SERVICE) as? CameraManager
            if (cameraManager != null) {
                try {
                    openCamera(cameraManager)
                } catch (e: Exception) {
                    Log.w(TAG, "Reopening camera with preview surface failed: ${e.message}")
                }
            }
        }
    }

    private fun openCamera(cameraManager: CameraManager) {
        val targetFacing = if (currentPositionVal == SipCameraPosition.FRONT) {
            CameraCharacteristics.LENS_FACING_FRONT
        } else {
            CameraCharacteristics.LENS_FACING_BACK
        }

        var selectedCameraId: String? = null
        for (id in cameraManager.cameraIdList) {
            val chars = cameraManager.getCameraCharacteristics(id)
            val facing = chars.get(CameraCharacteristics.LENS_FACING)
            if (facing == targetFacing) {
                selectedCameraId = id
                break
            }
        }
        if (selectedCameraId == null && cameraManager.cameraIdList.isNotEmpty()) {
            selectedCameraId = cameraManager.cameraIdList[0]
        }
        if (selectedCameraId == null) {
            startSyntheticGenerator()
            return
        }

        val chars = cameraManager.getCameraCharacteristics(selectedCameraId)
        val sensorOrientation = chars.get(CameraCharacteristics.SENSOR_ORIENTATION) ?: 0
        val facing = chars.get(CameraCharacteristics.LENS_FACING)
        val isFront = (facing == CameraCharacteristics.LENS_FACING_FRONT)

        val displayManager = context?.getSystemService(Context.DISPLAY_SERVICE) as? android.hardware.display.DisplayManager
        val defaultDisplay = displayManager?.getDisplay(android.view.Display.DEFAULT_DISPLAY)
        val displayRotation = defaultDisplay?.rotation ?: android.view.Surface.ROTATION_0

        val displayDegrees = when (displayRotation) {
            android.view.Surface.ROTATION_0 -> 0
            android.view.Surface.ROTATION_90 -> 90
            android.view.Surface.ROTATION_180 -> 180
            android.view.Surface.ROTATION_270 -> 270
            else -> 0
        }

        var rotation = displayDegrees
        if (!isFront) {
            rotation = (360 - rotation) % 360
        }
        previewRotationDegrees = (sensorOrientation + rotation) % 360

        imageReader = ImageReader.newInstance(width, height, ImageFormat.YUV_420_888, 2).apply {
            setOnImageAvailableListener({ reader ->
                val image = try {
                    reader.acquireLatestImage()
                } catch (_: Exception) {
                    null
                } ?: return@setOnImageAvailableListener

                // After stop(), drop images that were queued before the close ran.
                val deliveryOverride = imageDeliveryOverride
                if (isRunningFlag.get() && deliveryOverride != null) {
                    deliveryOverride(image)
                } else if (isRunningFlag.get() && isEnabledFlag.get() && clientHandle != 0L) {
                    val planes = image.planes
                    if (planes.size >= 3) {
                        val isFrontCam = (currentPositionVal == SipCameraPosition.FRONT)
                        nativeDeliverFrame(
                            clientHandle = clientHandle,
                            width = image.width,
                            height = image.height,
                            yBuffer = planes[0].buffer,
                            uBuffer = planes[1].buffer,
                            vBuffer = planes[2].buffer,
                            yRowStride = planes[0].rowStride,
                            uRowStride = planes[1].rowStride,
                            vRowStride = planes[2].rowStride,
                            yPixelStride = planes[0].pixelStride,
                            uPixelStride = planes[1].pixelStride,
                            vPixelStride = planes[2].pixelStride,
                            rotationDegrees = previewRotationDegrees,
                            mirror = isFrontCam,
                            timestampNs = image.timestamp
                        )
                    }
                }
                image.close()
            }, backgroundHandler)
        }

        try {
            cameraManager.openCamera(selectedCameraId, object : CameraDevice.StateCallback() {
                override fun onOpened(camera: CameraDevice) {
                    cameraDevice = camera
                    createCaptureSession(camera)
                }

                override fun onDisconnected(camera: CameraDevice) {
                    camera.close()
                    cameraDevice = null
                }

                override fun onError(camera: CameraDevice, error: Int) {
                    camera.close()
                    cameraDevice = null
                    Log.w(TAG, "Camera error $error, switching to synthetic generator")
                    startSyntheticGenerator()
                }
            }, backgroundHandler)
        } catch (_: SecurityException) {
            Log.w(TAG, "Missing CAMERA permission, falling back to synthetic generator")
            startSyntheticGenerator()
        }
    }

    private fun createCaptureSession(camera: CameraDevice) {
        val readerSurface = imageReader?.surface ?: return
        val targetSurfaces = mutableListOf(readerSurface)
        previewSurface?.let { if (it.isValid) targetSurfaces.add(it) }

        try {
            camera.createCaptureSession(targetSurfaces, object : CameraCaptureSession.StateCallback() {
                override fun onConfigured(session: CameraCaptureSession) {
                    try {
                        captureSession = session
                        val requestBuilder = camera.createCaptureRequest(CameraDevice.TEMPLATE_RECORD).apply {
                            addTarget(readerSurface)
                            previewSurface?.let { if (it.isValid) addTarget(it) }
                        }
                        session.setRepeatingRequest(requestBuilder.build(), null, backgroundHandler)
                    } catch (e: Exception) {
                        Log.w(TAG, "Failed to set repeating request on configured session: ${e.message}")
                    }
                }

                override fun onConfigureFailed(session: CameraCaptureSession) {
                    Log.w(TAG, "Camera capture session configuration failed")
                }
            }, backgroundHandler)
        } catch (e: Exception) {
            Log.w(TAG, "Failed to create capture session: ${e.message}")
        }
    }

    private fun closeCamera() {
        val session = captureSession
        val device = cameraDevice
        val reader = imageReader
        captureSession = null
        cameraDevice = null
        imageReader = null

        // Close on the camera thread: onImageAvailable runs there, so the
        // reader cannot be closed while an image from it is being delivered.
        runOnCameraThreadAndWait {
            try {
                session?.close()
            } catch (_: Exception) {}
            try {
                device?.close()
            } catch (_: Exception) {}
            reader?.close()
        }
    }

    /**
     * Runs [block] on the camera thread and waits up to [CLOSE_WAIT_MS] for it.
     * If the wait times out, [block] still runs later on the camera thread.
     * Runs inline when called on the camera thread or when there is none.
     */
    private fun runOnCameraThreadAndWait(block: () -> Unit) {
        val handler = backgroundHandler
        if (handler == null || handler.looper.thread === Thread.currentThread()) {
            block()
            return
        }
        val done = CountDownLatch(1)
        val posted = handler.post {
            try {
                block()
            } finally {
                done.countDown()
            }
        }
        if (!posted) {
            block()
            return
        }
        if (!awaitUninterruptibly(done, CLOSE_WAIT_MS)) {
            Log.w(TAG, "Camera resources will close on the camera thread when the current frame finishes")
        }
    }

    // Synthetic frame generator fallback
    private var syntheticThread: Thread? = null
    private val syntheticRunning = AtomicBoolean(false)

    private fun startSyntheticGenerator() {
        if (syntheticRunning.getAndSet(true)) return
        syntheticThread = Thread({
            val ySize = width * height
            val uvSize = (width / 2) * (height / 2)
            val yBuf = ByteBuffer.allocateDirect(ySize)
            val uBuf = ByteBuffer.allocateDirect(uvSize)
            val vBuf = ByteBuffer.allocateDirect(uvSize)

            var tick = 0
            while (syntheticRunning.get() && isRunningFlag.get()) {
                if (isEnabledFlag.get() && clientHandle != 0L) {
                    yBuf.clear()
                    uBuf.clear()
                    vBuf.clear()

                    val barWidth = (width / 8).coerceAtLeast(1)
                    val kBarY = intArrayOf(235, 210, 170, 145, 106, 81, 41, 24)
                    val kBarU = intArrayOf(128, 16, 166, 54, 202, 90, 240, 128)
                    val kBarV = intArrayOf(128, 146, 16, 34, 222, 240, 110, 128)

                    // Fill Y
                    for (y in 0 until height) {
                        for (x in 0 until width) {
                            val barIdx = ((x / barWidth) + (tick / 10)) % 8
                            yBuf.put(kBarY[barIdx].toByte())
                        }
                    }

                    // Fill UV
                    val uvW = width / 2
                    val uvH = height / 2
                    for (y in 0 until uvH) {
                        for (x in 0 until uvW) {
                            val barIdx = (((x * 2) / barWidth) + (tick / 10)) % 8
                            uBuf.put(kBarU[barIdx].toByte())
                            vBuf.put(kBarV[barIdx].toByte())
                        }
                    }

                    yBuf.flip()
                    uBuf.flip()
                    vBuf.flip()

                    nativeDeliverFrame(
                        clientHandle = clientHandle,
                        width = width,
                        height = height,
                        yBuffer = yBuf,
                        uBuffer = uBuf,
                        vBuffer = vBuf,
                        yRowStride = width,
                        uRowStride = uvW,
                        vRowStride = uvW,
                        yPixelStride = 1,
                        uPixelStride = 1,
                        vPixelStride = 1,
                        rotationDegrees = 0,
                        mirror = false,
                        timestampNs = System.nanoTime()
                    )
                    tick++
                }

                try {
                    val sleepMs = if (fps > 0) 1000L / fps else 33L
                    Thread.sleep(sleepMs)
                } catch (_: InterruptedException) {
                    break
                }
            }
        }, "SipSyntheticCamera").apply {
            isDaemon = true
            start()
        }
    }

    private fun stopSyntheticGenerator() {
        syntheticRunning.set(false)
        syntheticThread?.interrupt()
        syntheticThread = null
    }

    /**
     * Waits like [CountDownLatch.await], but an interrupt does not end the wait
     * early; the thread's interrupt flag is restored before returning.
     */
    private fun awaitUninterruptibly(latch: CountDownLatch, timeoutMs: Long): Boolean {
        val deadline = System.nanoTime() + TimeUnit.MILLISECONDS.toNanos(timeoutMs)
        var interrupted = false
        try {
            while (true) {
                val remaining = deadline - System.nanoTime()
                if (remaining <= 0) return latch.count == 0L
                try {
                    return latch.await(remaining, TimeUnit.NANOSECONDS)
                } catch (_: InterruptedException) {
                    interrupted = true
                }
            }
        } finally {
            if (interrupted) Thread.currentThread().interrupt()
        }
    }

    /** Like [Thread.join] with a timeout, but an interrupt does not end the wait early. */
    private fun joinUninterruptibly(thread: Thread, timeoutMs: Long) {
        val deadline = System.nanoTime() + TimeUnit.MILLISECONDS.toNanos(timeoutMs)
        var interrupted = false
        try {
            while (thread.isAlive) {
                val remainingMs = TimeUnit.NANOSECONDS.toMillis(deadline - System.nanoTime())
                if (remainingMs <= 0) return
                try {
                    thread.join(remainingMs)
                } catch (_: InterruptedException) {
                    interrupted = true
                }
            }
        } finally {
            if (interrupted) Thread.currentThread().interrupt()
        }
    }

    private fun startBackgroundThread() {
        try {
            val thread = HandlerThread("SipCameraBackground")
            thread.start()
            val looper = thread.looper
            if (looper != null) {
                backgroundHandler = Handler(looper)
            }
            backgroundThread = thread
        } catch (_: Throwable) {
            backgroundThread = null
            backgroundHandler = null
        }
    }

    private fun stopBackgroundThread() {
        try {
            backgroundThread?.let { thread ->
                thread.quitSafely()
                joinUninterruptibly(thread, 500)
            }
        } catch (_: Throwable) {}
        backgroundThread = null
        backgroundHandler = null
    }
}
