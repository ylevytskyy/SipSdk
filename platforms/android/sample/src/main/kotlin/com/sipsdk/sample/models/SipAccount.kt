package com.sipsdk.sample.models

import org.json.JSONObject

data class SipAccount(
    val username: String = "1001",
    val domain: String = "10.0.2.2",
    val password: String = "test1001",
    val transport: String = "udp",
    val port: Int = 5060,
    val displayName: String? = "Android 1001",
    val outboundProxy: String? = null,
    val stunServer: String? = null,
    val useIce: Boolean = false,
    val regInterval: Int = 3600
) {
    val aor: String
        get() = "sip:$username@$domain:$port"

    fun toJson(): String {
        val obj = JSONObject()
        obj.put("username", username)
        obj.put("domain", domain)
        obj.put("password", password)
        obj.put("transport", transport)
        obj.put("port", port)
        obj.put("displayName", displayName ?: "")
        obj.put("outboundProxy", outboundProxy ?: "")
        obj.put("stunServer", stunServer ?: "")
        obj.put("useIce", useIce)
        obj.put("regInterval", regInterval)
        return obj.toString()
    }

    companion object {
        fun fromJson(jsonStr: String): SipAccount {
            return try {
                val obj = JSONObject(jsonStr)
                SipAccount(
                    username = obj.optString("username", "1001"),
                    domain = obj.optString("domain", "10.0.2.2"),
                    password = obj.optString("password", "test1001"),
                    transport = obj.optString("transport", "udp"),
                    port = obj.optInt("port", 5060),
                    displayName = obj.optString("displayName", "Android 1001").takeIf { it.isNotEmpty() },
                    outboundProxy = obj.optString("outboundProxy").takeIf { it.isNotEmpty() },
                    stunServer = obj.optString("stunServer").takeIf { it.isNotEmpty() },
                    useIce = obj.optBoolean("useIce", false),
                    regInterval = obj.optInt("regInterval", 3600)
                )
            } catch (_: Exception) {
                SipAccount()
            }
        }
    }
}
