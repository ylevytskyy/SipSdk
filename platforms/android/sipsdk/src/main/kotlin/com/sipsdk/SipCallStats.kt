package com.sipsdk

data class SipCallStats(
    val audioLossRate: Float = 0f,
    val audioJitterMs: Int = 0,
    val audioRttMs: Int = 0,
    val audioBitrateKbps: Int = 0,
    val audioCodec: String = "",
    val videoLossRate: Float = 0f,
    val videoJitterMs: Int = 0,
    val videoBitrateKbps: Int = 0,
    val videoFps: Int = 0,
    val videoCodec: String = ""
)
