package com.sipsdk

import android.content.Context
import android.media.AudioAttributes
import android.media.AudioFocusRequest
import android.media.AudioManager
import android.os.Build

class SipAudioManager(private val context: Context?) {
    private val audioManager = context?.getSystemService(Context.AUDIO_SERVICE) as? AudioManager
    private var previousAudioMode: Int = AudioManager.MODE_NORMAL
    private var previousSpeakerState: Boolean = false
    private var isSessionActive: Boolean = false
    private var focusRequest: AudioFocusRequest? = null

    @Synchronized
    fun prepareAudioSession(): Boolean {
        if (audioManager == null) return true
        if (isSessionActive) return true

        previousAudioMode = audioManager.mode
        @Suppress("DEPRECATION")
        previousSpeakerState = audioManager.isSpeakerphoneOn

        val focusGranted = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val playbackAttributes = AudioAttributes.Builder()
                .setUsage(AudioAttributes.USAGE_VOICE_COMMUNICATION)
                .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
                .build()
            val request = AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN_TRANSIENT)
                .setAudioAttributes(playbackAttributes)
                .setAcceptsDelayedFocusGain(false)
                .setOnAudioFocusChangeListener { /* handle focus changes if needed */ }
                .build()
            focusRequest = request
            audioManager.requestAudioFocus(request) == AudioManager.AUDIOFOCUS_REQUEST_GRANTED
        } else {
            @Suppress("DEPRECATION")
            audioManager.requestAudioFocus(
                null,
                AudioManager.STREAM_VOICE_CALL,
                AudioManager.AUDIOFOCUS_GAIN_TRANSIENT
            ) == AudioManager.AUDIOFOCUS_REQUEST_GRANTED
        }

        audioManager.mode = AudioManager.MODE_IN_COMMUNICATION
        isSessionActive = true
        return focusGranted
    }

    @Synchronized
    fun releaseAudioSession() {
        if (audioManager == null || !isSessionActive) return

        @Suppress("DEPRECATION")
        audioManager.isSpeakerphoneOn = previousSpeakerState
        audioManager.mode = previousAudioMode

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            focusRequest?.let { audioManager.abandonAudioFocusRequest(it) }
            focusRequest = null
        } else {
            @Suppress("DEPRECATION")
            audioManager.abandonAudioFocus(null)
        }

        isSessionActive = false
    }

    fun setSpeakerphone(enabled: Boolean) {
        @Suppress("DEPRECATION")
        audioManager?.isSpeakerphoneOn = enabled
    }

    fun setMicrophoneMute(muted: Boolean) {
        audioManager?.isMicrophoneMute = muted
    }

    val isSpeakerphoneOn: Boolean
        get() {
            @Suppress("DEPRECATION")
            return audioManager?.isSpeakerphoneOn ?: false
        }

    val isMicrophoneMute: Boolean
        get() = audioManager?.isMicrophoneMute ?: false

    val isAudioSessionActive: Boolean
        get() = isSessionActive
}
