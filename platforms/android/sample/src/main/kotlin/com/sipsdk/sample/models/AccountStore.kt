package com.sipsdk.sample.models

import android.content.Context
import android.content.SharedPreferences
import android.os.Build

class AccountStore(context: Context) {
    private val prefs: SharedPreferences =
        context.getSharedPreferences("com.sipsdk.sample.account", Context.MODE_PRIVATE)

    private val isEmulator: Boolean = Build.FINGERPRINT.startsWith("generic")
            || Build.FINGERPRINT.startsWith("unknown")
            || Build.MODEL.contains("google_sdk")
            || Build.MODEL.contains("Emulator")
            || Build.MODEL.contains("Android SDK built for")
            || Build.MANUFACTURER.contains("Genymotion")
            || (Build.BRAND.startsWith("generic") && Build.DEVICE.startsWith("generic"))
            || "google_sdk" == Build.PRODUCT

    private val defaultHost: String = if (isEmulator) "10.0.2.2" else "192.168.0.107"

    var account: SipAccount = load()
        set(value) {
            field = value
            save(value)
        }

    private fun load(): SipAccount {
        val raw = prefs.getString("account_data", null)
        return if (raw != null) {
            var acc = SipAccount.fromJson(raw)
            if (!isEmulator && acc.domain == "10.0.2.2") {
                acc = acc.copy(domain = defaultHost)
            }
            acc
        } else {
            defaultAccount()
        }
    }

    private fun save(acc: SipAccount) {
        prefs.edit().putString("account_data", acc.toJson()).apply()
    }

    fun resetToDefaults() {
        val def = defaultAccount()
        account = def
    }

    private fun defaultAccount(): SipAccount {
        return SipAccount(
            username = "1001",
            domain = defaultHost,
            password = "test1001",
            transport = "udp",
            port = 5060,
            displayName = if (isEmulator) "Android Emulator 1001" else "Samsung 1001",
            outboundProxy = null,
            stunServer = null,
            useIce = false,
            regInterval = 3600
        )
    }
}
