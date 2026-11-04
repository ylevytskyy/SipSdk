package com.sipsdk.sample.models

import android.content.Context
import android.content.SharedPreferences
import org.json.JSONArray

class CallHistoryStore(context: Context) {
    private val prefs: SharedPreferences =
        context.getSharedPreferences("com.sipsdk.sample.history", Context.MODE_PRIVATE)

    fun loadHistory(): List<CallHistoryItem> {
        val raw = prefs.getString("history_items", null) ?: return emptyList()
        return try {
            val array = JSONArray(raw)
            val list = mutableListOf<CallHistoryItem>()
            for (i in 0 until array.length()) {
                list.add(CallHistoryItem.fromJson(array.getJSONObject(i)))
            }
            list
        } catch (_: Exception) {
            emptyList()
        }
    }

    fun saveHistory(items: List<CallHistoryItem>) {
        val array = JSONArray()
        for (item in items) {
            array.put(item.toJson())
        }
        prefs.edit().putString("history_items", array.toString()).apply()
    }
}
