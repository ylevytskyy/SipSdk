package com.sipsdk

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.media.MediaMuxer
import java.io.File
import java.nio.ByteBuffer

enum class SipRecordingMode(val nativeValue: Int) {
    AUDIO_STEREO(0),
    AUDIO_MONO(1),
    AUDIO_VIDEO(2);

    companion object {
        fun fromInt(v: Int): SipRecordingMode = values().firstOrNull { it.nativeValue == v } ?: AUDIO_STEREO
    }
}

enum class SipRecordingState(val nativeValue: Int) {
    IDLE(0),
    RECORDING(1),
    FAILED(2);

    companion object {
        fun fromInt(v: Int): SipRecordingState = values().firstOrNull { it.nativeValue == v } ?: FAILED
    }
}

/**
 * Native Android MP4 muxer (MediaCodec H.264 + AAC -> MediaMuxer) bridged to
 * `sip_media_recorder_driver_t` for `SipRecordingMode.AUDIO_VIDEO`.
 */
internal class AndroidMediaRecorder {
    private val lock = Any()
    private var muxer: MediaMuxer? = null
    private var audioEncoder: MediaCodec? = null
    private var videoEncoder: MediaCodec? = null
    private var audioTrackIndex = -1
    private var videoTrackIndex = -1
    private var muxerStarted = false
    private var active = false
    private var sampleRate = 48000
    private var channels = 2
    private var videoWidth = 0
    private var videoHeight = 0

    fun start(path: String, sampleRate: Int, channels: Int, videoWidth: Int, videoHeight: Int): Int {
        synchronized(lock) {
            if (active) return 114 // EALREADY
            return try {
                File(path).delete()
                this.sampleRate = if (sampleRate > 0) sampleRate else 48000
                this.channels = channels.coerceIn(1, 2)
                this.videoWidth = videoWidth
                this.videoHeight = videoHeight
                this.audioTrackIndex = -1
                this.videoTrackIndex = -1
                this.muxerStarted = false

                muxer = MediaMuxer(path, MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4)

                val aFmt = MediaFormat.createAudioFormat(
                    MediaFormat.MIMETYPE_AUDIO_AAC,
                    this.sampleRate,
                    this.channels
                ).apply {
                    setInteger(MediaFormat.KEY_AAC_PROFILE, MediaCodecInfo.CodecProfileLevel.AACObjectLC)
                    setInteger(MediaFormat.KEY_BIT_RATE, 128_000)
                    setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, 16384)
                }
                audioEncoder = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_AUDIO_AAC).apply {
                    configure(aFmt, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
                    start()
                }

                if (videoWidth > 0 && videoHeight > 0) {
                    configureVideoEncoderLocked(videoWidth, videoHeight)
                }
                active = true
                0
            } catch (_: Throwable) {
                stopLocked()
                5 // EIO
            }
        }
    }

    private fun configureVideoEncoderLocked(w: Int, h: Int) {
        if (videoEncoder != null || muxerStarted) return
        try {
            val vFmt = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, w, h).apply {
                setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Flexible)
                setInteger(MediaFormat.KEY_BIT_RATE, (w * h * 4).coerceIn(300_000, 4_000_000))
                setInteger(MediaFormat.KEY_FRAME_RATE, 30)
                setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 2)
            }
            videoEncoder = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_AVC).apply {
                configure(vFmt, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
                start()
            }
            videoWidth = w
            videoHeight = h
        } catch (_: Throwable) {
            videoEncoder = null
        }
    }

    fun writeAudio(pcmBytes: ByteArray, timestampNs: Long) {
        synchronized(lock) {
            val enc = audioEncoder ?: return
            if (!active || pcmBytes.isEmpty()) return
            try {
                val inIdx = enc.dequeueInputBuffer(0)
                if (inIdx >= 0) {
                    val buf = enc.getInputBuffer(inIdx)
                    if (buf != null) {
                        buf.clear()
                        val toCopy = minOf(buf.remaining(), pcmBytes.size)
                        buf.put(pcmBytes, 0, toCopy)
                        enc.queueInputBuffer(inIdx, 0, toCopy, timestampNs / 1000L, 0)
                    }
                }
                drainEncoderLocked(enc, isVideo = false)
            } catch (_: Throwable) {
            }
        }
    }

    fun writeVideo(i420Bytes: ByteArray, width: Int, height: Int, timestampNs: Long) {
        synchronized(lock) {
            if (!active || i420Bytes.isEmpty() || width <= 0 || height <= 0) return
            if (videoEncoder == null && !muxerStarted) {
                configureVideoEncoderLocked(width, height)
            }
            val enc = videoEncoder ?: return
            try {
                val inIdx = enc.dequeueInputBuffer(0)
                if (inIdx >= 0) {
                    val buf = enc.getInputBuffer(inIdx)
                    if (buf != null) {
                        buf.clear()
                        val toCopy = minOf(buf.remaining(), i420Bytes.size)
                        buf.put(i420Bytes, 0, toCopy)
                        enc.queueInputBuffer(inIdx, 0, toCopy, timestampNs / 1000L, 0)
                    }
                }
                drainEncoderLocked(enc, isVideo = true)
            } catch (_: Throwable) {
            }
        }
    }

    private fun drainEncoderLocked(encoder: MediaCodec, isVideo: Boolean) {
        val m = muxer ?: return
        val info = MediaCodec.BufferInfo()
        while (true) {
            val outIdx = encoder.dequeueOutputBuffer(info, 0)
            if (outIdx == MediaCodec.INFO_TRY_AGAIN_LATER) break
            if (outIdx == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                if (!muxerStarted) {
                    val trackIdx = m.addTrack(encoder.outputFormat)
                    if (isVideo) videoTrackIndex = trackIdx else audioTrackIndex = trackIdx
                    val needVideo = (videoEncoder != null)
                    if (audioTrackIndex >= 0 && (!needVideo || videoTrackIndex >= 0)) {
                        m.start()
                        muxerStarted = true
                    }
                }
            } else if (outIdx >= 0) {
                val outBuf: ByteBuffer? = encoder.getOutputBuffer(outIdx)
                if (outBuf != null && (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG) == 0 &&
                    info.size > 0 && muxerStarted
                ) {
                    val trackIdx = if (isVideo) videoTrackIndex else audioTrackIndex
                    if (trackIdx >= 0) {
                        outBuf.position(info.offset)
                        outBuf.limit(info.offset + info.size)
                        m.writeSampleData(trackIdx, outBuf, info)
                    }
                }
                encoder.releaseOutputBuffer(outIdx, false)
            }
        }
    }

    fun stop(): Int {
        synchronized(lock) {
            stopLocked()
            return 0
        }
    }

    private fun stopLocked() {
        active = false
        try { audioEncoder?.stop() } catch (_: Throwable) {}
        try { audioEncoder?.release() } catch (_: Throwable) {}
        audioEncoder = null
        try { videoEncoder?.stop() } catch (_: Throwable) {}
        try { videoEncoder?.release() } catch (_: Throwable) {}
        videoEncoder = null
        if (muxerStarted) {
            try { muxer?.stop() } catch (_: Throwable) {}
        }
        try { muxer?.release() } catch (_: Throwable) {}
        muxer = null
        muxerStarted = false
        audioTrackIndex = -1
        videoTrackIndex = -1
    }
}
