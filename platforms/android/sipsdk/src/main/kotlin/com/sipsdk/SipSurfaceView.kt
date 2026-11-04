package com.sipsdk

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.util.AttributeSet
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import java.util.concurrent.atomic.AtomicLong

class SipSurfaceView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
    defStyleAttr: Int = 0
) : SurfaceView(context, attrs, defStyleAttr), SurfaceHolder.Callback {

    var videoWidth: Int = 0
        private set
    var videoHeight: Int = 0
        private set

    private val frameCount = AtomicLong(0L)
    val renderedFrameCount: Long get() = frameCount.get()

    private var surfaceListener: SurfaceListener? = null
    var currentSurface: Surface? = null
        private set

    var isMirrored: Boolean = false
    var rotationDegrees: Int = 0

    interface SurfaceListener {
        fun onSurfaceAvailable(surface: Surface)
        fun onSurfaceDestroyed()
    }

    init {
        holder.addCallback(this)
    }

    fun setSurfaceListener(listener: SurfaceListener?) {
        surfaceListener = listener
        currentSurface?.let {
            if (it.isValid) {
                listener?.onSurfaceAvailable(it)
            }
        }
    }

    fun setVideoDimensions(width: Int, height: Int) {
        if (width <= 0 || height <= 0) return
        if (videoWidth != width || videoHeight != height) {
            videoWidth = width
            videoHeight = height
            post { requestLayout() }
        }
    }

    fun incrementRenderedFrameCount() {
        frameCount.incrementAndGet()
    }

    fun clear() {
        val canvas: Canvas? = try {
            holder.lockCanvas()
        } catch (_: Exception) {
            null
        }
        if (canvas != null) {
            canvas.drawColor(Color.BLACK)
            try {
                holder.unlockCanvasAndPost(canvas)
            } catch (_: Exception) {}
        }
    }

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        if (videoWidth <= 0 || videoHeight <= 0) {
            super.onMeasure(widthMeasureSpec, heightMeasureSpec)
            return
        }

        val defaultWidth = getDefaultSize(videoWidth, widthMeasureSpec)
        val defaultHeight = getDefaultSize(videoHeight, heightMeasureSpec)

        if (defaultWidth <= 0 || defaultHeight <= 0) {
            super.onMeasure(widthMeasureSpec, heightMeasureSpec)
            return
        }

        val viewAspect = defaultWidth.toDouble() / defaultHeight.toDouble()
        val videoAspect = videoWidth.toDouble() / videoHeight.toDouble()

        val finalWidth: Int
        val finalHeight: Int

        if (videoAspect > viewAspect) {
            // Video is wider than view -> match width, adjust height
            finalWidth = defaultWidth
            finalHeight = (defaultWidth / videoAspect).toInt()
        } else {
            // Video is taller than view -> match height, adjust width
            finalHeight = defaultHeight
            finalWidth = (defaultHeight * videoAspect).toInt()
        }

        setMeasuredDimension(finalWidth, finalHeight)
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        currentSurface = holder.surface
        surfaceListener?.onSurfaceAvailable(holder.surface)
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        currentSurface = holder.surface
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        surfaceListener?.onSurfaceDestroyed()
        currentSurface = null
    }
}
