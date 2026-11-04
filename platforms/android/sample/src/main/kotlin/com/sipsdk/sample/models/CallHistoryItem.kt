package com.sipsdk.sample.models

import java.text.SimpleDateFormat
import java.util.Calendar
import java.util.Date
import java.util.Locale
import java.util.UUID
import org.json.JSONObject

enum class CallDirection {
    OUTGOING,
    INCOMING,
    MISSED
}

data class CallHistoryItem(
    val id: String = UUID.randomUUID().toString(),
    val remoteUri: String,
    val displayName: String? = null,
    val timestamp: Long = System.currentTimeMillis(),
    val durationSeconds: Long = 0L,
    val direction: CallDirection = CallDirection.OUTGOING,
    val hasVideo: Boolean = true
) {
    val formattedDuration: String
        get() {
            val m = durationSeconds / 60
            val s = durationSeconds % 60
            return String.format(Locale.US, "%02d:%02d", m, s)
        }

    val formattedDate: String
        get() {
            val itemCal = Calendar.getInstance().apply { timeInMillis = timestamp }
            val nowCal = Calendar.getInstance()
            val isToday = itemCal.get(Calendar.YEAR) == nowCal.get(Calendar.YEAR) &&
                    itemCal.get(Calendar.DAY_OF_YEAR) == nowCal.get(Calendar.DAY_OF_YEAR)

            val pattern = if (isToday) "HH:mm" else "MMM d, HH:mm"
            return SimpleDateFormat(pattern, Locale.getDefault()).format(Date(timestamp))
        }

    val displayTitle: String
        get() {
            if (!displayName.isNullOrBlank()) {
                return displayName
            }
            if (remoteUri.startsWith("sip:")) {
                val stripped = remoteUri.removePrefix("sip:")
                val atIdx = stripped.indexOf('@')
                return if (atIdx != -1) stripped.substring(0, atIdx) else stripped
            }
            return remoteUri
        }

    fun toJson(): JSONObject {
        val obj = JSONObject()
        obj.put("id", id)
        obj.put("remoteUri", remoteUri)
        obj.put("displayName", displayName ?: "")
        obj.put("timestamp", timestamp)
        obj.put("durationSeconds", durationSeconds)
        obj.put("direction", direction.name)
        obj.put("hasVideo", hasVideo)
        return obj
    }

    companion object {
        fun fromJson(obj: JSONObject): CallHistoryItem {
            val dirStr = obj.optString("direction", CallDirection.OUTGOING.name)
            val dir = try {
                CallDirection.valueOf(dirStr)
            } catch (_: Exception) {
                CallDirection.OUTGOING
            }
            return CallHistoryItem(
                id = obj.optString("id", UUID.randomUUID().toString()),
                remoteUri = obj.optString("remoteUri", ""),
                displayName = obj.optString("displayName").takeIf { it.isNotEmpty() },
                timestamp = obj.optLong("timestamp", System.currentTimeMillis()),
                durationSeconds = obj.optLong("durationSeconds", 0L),
                direction = dir,
                hasVideo = obj.optBoolean("hasVideo", true)
            )
        }
    }
}
