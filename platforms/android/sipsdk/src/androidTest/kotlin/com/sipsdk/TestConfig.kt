package com.sipsdk

import androidx.test.platform.app.InstrumentationRegistry

object TestConfig {
    val host: String
        get() {
            val arg = try {
                InstrumentationRegistry.getArguments().getString("sip_host")
            } catch (_: Exception) {
                null
            }
            if (!arg.isNullOrBlank()) return arg
            val isEmulator = android.os.Build.FINGERPRINT.startsWith("generic") ||
                             android.os.Build.FINGERPRINT.startsWith("unknown") ||
                             android.os.Build.HARDWARE.contains("goldfish") ||
                             android.os.Build.HARDWARE.contains("ranchu") ||
                             android.os.Build.MODEL.contains("google_sdk") ||
                             android.os.Build.MODEL.contains("Emulator") ||
                             android.os.Build.MODEL.contains("Android SDK built for")
            return if (isEmulator) "10.0.2.2" else "192.168.0.107"
        }

    fun hasAudioCaptureHardware(): Boolean {
        val rate = 48000
        val ch = android.media.AudioFormat.CHANNEL_IN_STEREO
        val fmt = android.media.AudioFormat.ENCODING_PCM_16BIT
        val bufSize = android.media.AudioRecord.getMinBufferSize(rate, ch, fmt)
        if (bufSize <= 0) return false
        return try {
            val rec = android.media.AudioRecord(
                android.media.MediaRecorder.AudioSource.DEFAULT,
                rate,
                ch,
                fmt,
                bufSize
            )
            val ok = rec.state == android.media.AudioRecord.STATE_INITIALIZED
            rec.release()
            ok
        } catch (_: Throwable) {
            false
        }
    }

    /** Keeps OpenSL ES input+output when capture HAL works; falls back source to nil on headless API 26 emulators. */
    fun configureAudioDriverForDevice(client: SipClient): Boolean {
        val hasMic = hasAudioCaptureHardware()
        if (!hasMic) {
            client.setAudioDriver("nil", "", "opensles", "")
        }
        return hasMic
    }
}
