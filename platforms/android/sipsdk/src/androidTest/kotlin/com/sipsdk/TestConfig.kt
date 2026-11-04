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
}
