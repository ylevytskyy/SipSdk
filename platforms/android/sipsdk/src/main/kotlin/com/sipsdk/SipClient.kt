package com.sipsdk

import android.content.Context
import android.os.Handler
import android.os.Looper
import java.io.File
import java.util.concurrent.ConcurrentHashMap

class SipBusyException(message: String) : IllegalStateException(message)

class SipClient(
    val username: String,
    val domain: String,
    val context: Context? = null
) {
    companion object {
        init {
            try {
                System.loadLibrary("sipcore")
            } catch (e: UnsatisfiedLinkError) {
                // Allows loading without native lib if running in mock/isolated unit tests
            }
        }

        fun getVersion(): String = SipSdk.getVersion()
    }

    var password: String? = null
    var displayName: String? = null
    var transport: String = "udp"
    var port: Int = 5060
    var tlsCaFile: String? = null
    var verifyTlsCert: Boolean = true
    var mediaEnc: String = ""
    var maxCalls: Int = 4
        set(value) {
            field = value.coerceIn(1, 8)
        }
    var lastDialErrorCode: Int = 0
        private set
    var listener: SipClientListener? = null
    var telecomCoordinator: SipTelecomManagerCoordinator? = null

    private val activeCallsMap = ConcurrentHashMap<Long, SipCall>()
    private val pendingPushHandles = ConcurrentHashMap<String, SipPushCallHandle>()
    val activeCalls: List<SipCall>
        get() = activeCallsMap.values.filter { !it.isClosed }

    val audioManager: SipAudioManager? = context?.let { SipAudioManager(it) }

    val aor: String
        get() = "sip:$username@$domain:$port"

    val isRegistered: Boolean
        get() = if (nativeHandle != 0L) nativeIsRegistered(nativeHandle) else false

    var isMicrophoneMuted: Boolean = false
        set(value) {
            field = value
            if (nativeHandle != 0L) {
                nativeSetMicMuted(nativeHandle, value)
            }
            audioManager?.setMicrophoneMute(value)
        }

    var isSpeakerEnabled: Boolean = false
        set(value) {
            field = value
            audioManager?.setSpeakerphone(value)
        }

    var cameraPosition: SipCameraPosition = SipCameraPosition.FRONT
        set(value) {
            field = value
            if (nativeHandle != 0L) {
                nativeSetCamera(nativeHandle, value.value)
            }
            cameraCapture?.setCamera(value)
        }

    var isLocalVideoEnabled: Boolean = true
        set(value) {
            field = value
            if (nativeHandle != 0L) {
                nativeSetLocalVideoEnabled(nativeHandle, value)
            }
            cameraCapture?.setEnabled(value)
        }

    val isLocalPreviewActive: Boolean
        get() = if (nativeHandle != 0L) nativeIsLocalPreviewActive(nativeHandle) else false

    var remoteVideoView: SipSurfaceView? = null
        set(value) {
            field?.setSurfaceListener(null)
            field = value
            value?.setSurfaceListener(object : SipSurfaceView.SurfaceListener {
                override fun onSurfaceAvailable(surface: android.view.Surface) {
                    if (nativeHandle != 0L) {
                        nativeSetRemoteSurface(nativeHandle, surface)
                    }
                }

                override fun onSurfaceDestroyed() {
                    if (nativeHandle != 0L) {
                        nativeSetRemoteSurface(nativeHandle, null)
                    }
                }
            })
            val cur = value?.currentSurface
            if (cur != null && cur.isValid && nativeHandle != 0L) {
                nativeSetRemoteSurface(nativeHandle, cur)
            }
        }

    var localVideoView: SipSurfaceView? = null
        set(value) {
            field?.setSurfaceListener(null)
            field = value
            value?.setSurfaceListener(object : SipSurfaceView.SurfaceListener {
                override fun onSurfaceAvailable(surface: android.view.Surface) {
                    cameraCapture?.setPreviewSurface(surface)
                }

                override fun onSurfaceDestroyed() {
                    cameraCapture?.setPreviewSurface(null)
                }
            })
            val cur = value?.currentSurface
            if (cur != null && cur.isValid) {
                cameraCapture?.setPreviewSurface(cur)
            }
        }

    private var cameraCapture: SipCameraCapture? = null

    private var nativeHandle: Long = 0L
    private val mainHandler: Handler? = try {
        val looper = Looper.getMainLooper()
        if (looper != null) Handler(looper) else null
    } catch (_: Throwable) {
        null
    }

    private fun prepareDefaultConfigDirectory(configDir: String?): String {
        val targetDir = if (configDir != null) {
            File(configDir)
        } else {
            val base = context?.filesDir ?: File(System.getProperty("java.io.tmpdir") ?: "/tmp")
            File(base, "baresip")
        }
        if (!targetDir.exists()) {
            targetDir.mkdirs()
        }

        val configFile = File(targetDir, "config")
        val needWrite = !configFile.exists() || !configFile.readText().let {
            it.contains("opensles") && it.contains("opus.so") && it.contains("dtls_srtp.so")
        }
        if (needWrite) {
            val defaultConfig = buildString {
                appendLine("# Baresip configuration generated by SipSdk Android")
                appendLine("poll_method             epoll")
                appendLine("sip_trans_bsize         128")
                appendLine("")
                appendLine("# Audio OpenSL ES")
                appendLine("audio_player            opensles,nil")
                appendLine("audio_source            opensles,nil")
                appendLine("audio_srate             8000-48000")
                appendLine("audio_channels          1-2")
                appendLine("")
                appendLine("# Video")
                appendLine("video_size              640x480")
                appendLine("video_bitrate           512000")
                appendLine("video_fps               30.00")
                appendLine("video_fullscreen        no")
                appendLine("")
                appendLine("# RTP / Jitter buffer")
                appendLine("rtp_tos                 184")
                appendLine("rtcp_enable             yes")
                appendLine("rtcp_mux                no")
                appendLine("jitter_buffer_delay     5-10")
                appendLine("")
                appendLine("# Static modules")
                appendLine("module                  opus.so")
                appendLine("module                  g711.so")
                appendLine("module                  opensles.so")
                appendLine("module                  stun.so")
                appendLine("module                  turn.so")
                appendLine("module                  ice.so")
                appendLine("module                  srtp.so")
                appendLine("module                  dtls_srtp.so")
                appendLine("module_tmp              account.so")
            }
            try {
                configFile.writeText(defaultConfig)
            } catch (_: Throwable) {
                // If writing fails, Baresip will fall back to in-memory config
            }
        }
        return targetDir.absolutePath
    }

    fun start(configDir: String? = null): Int {
        if (nativeHandle == 0L) {
            nativeHandle = nativeCreate()
            if (nativeHandle == 0L) return -1
            val resolvedConfigDir = prepareDefaultConfigDirectory(configDir)
            val res = nativeInit(nativeHandle, resolvedConfigDir)
            if (res != 0) {
                nativeDestroy(nativeHandle)
                nativeHandle = 0L
                return res
            }
            nativeSetAudioDriver(nativeHandle, "opensles", "", "opensles", "")
            val cur = remoteVideoView?.currentSurface
            if (cur != null && cur.isValid) {
                nativeSetRemoteSurface(nativeHandle, cur)
            }
        }
        return 0
    }

    fun stop() {
        if (nativeHandle != 0L) {
            activeCallsMap.clear()
            cameraCapture?.stop()
            cameraCapture = null
            audioManager?.releaseAudioSession()
            nativeDestroy(nativeHandle)
            nativeHandle = 0L
        }
    }

    fun switchCamera(): Int {
        val newPos = if (cameraPosition == SipCameraPosition.FRONT) {
            SipCameraPosition.BACK
        } else {
            SipCameraPosition.FRONT
        }
        cameraPosition = newPos
        return if (nativeHandle != 0L) nativeSwitchCamera(nativeHandle) else 0
    }

    fun setCamera(position: SipCameraPosition): Int {
        cameraPosition = position
        return if (nativeHandle != 0L) nativeSetCamera(nativeHandle, position.value) else 0
    }

    fun setLocalVideoEnabled(enabled: Boolean): Int {
        isLocalVideoEnabled = enabled
        return if (nativeHandle != 0L) nativeSetLocalVideoEnabled(nativeHandle, enabled) else 0
    }

    fun startLocalPreview(): Int {
        if (nativeHandle == 0L) {
            val rc = start()
            if (rc != 0) return rc
        }
        if (cameraCapture == null) {
            cameraCapture = SipCameraCapture(context, nativeHandle)
            val cur = localVideoView?.currentSurface
            if (cur != null && cur.isValid) {
                cameraCapture?.setPreviewSurface(cur)
            }
        }
        cameraCapture?.start()
        return nativeStartLocalPreview(nativeHandle)
    }

    fun stopLocalPreview(): Int {
        cameraCapture?.stop()
        return if (nativeHandle != 0L) nativeStopLocalPreview(nativeHandle) else 0
    }

    fun register(): Int {
        if (nativeHandle == 0L) {
            val rc = start()
            if (rc != 0) return rc
        }
        return nativeRegisterAccount(
            nativeHandle,
            username,
            domain,
            password ?: "",
            displayName ?: "",
            transport,
            port,
            tlsCaFile ?: "",
            verifyTlsCert,
            mediaEnc,
            maxCalls
        )
    }

    fun unregister(): Int {
        if (nativeHandle == 0L) return -1
        return nativeUnregister(nativeHandle)
    }

    fun resetTransports(reRegister: Boolean = true): Int {
        return if (nativeHandle != 0L) nativeResetTransports(nativeHandle, reRegister) else -1
    }

    fun dial(uri: String, withVideo: Boolean = false): SipCall {
        val call = makeCall(uri, withVideo)
        if (call != null) return call
        if (lastDialErrorCode == 16) {
            throw SipBusyException("Maximum concurrent calls reached ($maxCalls)")
        }
        throw IllegalStateException("Failed to dial $uri (error $lastDialErrorCode)")
    }

    fun makeCall(uri: String, withVideo: Boolean = false): SipCall? {
        if (nativeHandle == 0L) {
            val rc = start()
            if (rc != 0) {
                lastDialErrorCode = rc
                return null
            }
        }
        if (withVideo && cameraCapture == null) {
            cameraCapture = SipCameraCapture(context, nativeHandle)
            val cur = localVideoView?.currentSurface
            if (cur != null && cur.isValid) {
                cameraCapture?.setPreviewSurface(cur)
            }
            cameraCapture?.start()
        }
        val statusOut = IntArray(1)
        val callHandle = nativeDialCall(nativeHandle, uri, withVideo, statusOut)
        lastDialErrorCode = statusOut[0]
        if (callHandle == 0L) return null
        activeCallsMap.values.filter { !it.isClosed && !it.isOnHold }.forEach { existing ->
            telecomCoordinator?.setInactive(existing.callId)
        }
        audioManager?.prepareAudioSession()
        val call = activeCallsMap.getOrPut(callHandle) {
            SipCall(
                nativeHandle = callHandle,
                callId = nativeCallGetId(callHandle),
                remoteUri = nativeCallGetRemoteUri(callHandle),
                peerDisplayName = nativeCallGetPeerDisplayName(callHandle),
                hasVideo = withVideo,
                isOutgoing = true
            )
        }
        telecomCoordinator?.addCall(
            callId = call.callId,
            displayName = call.peerDisplayName ?: uri,
            address = call.remoteUri,
            isOutgoing = true,
            isVideo = withVideo,
            callbacks = createTelecomCallbacks(call)
        )
        return call
    }

    private fun createTelecomCallbacks(call: SipCall): SipTelecomCallCallbacks = object : SipTelecomCallCallbacks {
        override fun onAnswer(isVideo: Boolean) {
            call.answer(isVideo)
        }

        override fun onDisconnect() {
            call.hangup(0, "Hangup")
        }

        override fun onSetActive() {
            if (call.isOnHold && !call.isClosed) {
                call.setHold(false)
            }
        }

        override fun onSetInactive() {
            if (!call.isOnHold && !call.isClosed) {
                call.setHold(true)
            }
        }

        override fun onMuteChanged(isMuted: Boolean) {
            isMicrophoneMuted = isMuted
        }
    }

    /**
     * Sends a UTF-8 text message (SIP MESSAGE, text/plain). [target] is an extension
     * ("1002"), user@host, or a sip: URI. Returns the local message id; the result arrives
     * through [SipClientListener.onMessageStatusChanged].
     */
    fun sendMessage(target: String, text: String): Long {
        require(text.isNotEmpty()) { "Message text cannot be empty" }
        return sendMessage(target, text.toByteArray(Charsets.UTF_8), "text/plain")
    }

    /** Sends a SIP MESSAGE with any MIME content type (e.g. "application/json"). */
    fun sendMessage(target: String, body: ByteArray, contentType: String): Long {
        require(target.isNotEmpty()) { "Target cannot be empty" }
        require(body.isNotEmpty()) { "Message body cannot be empty" }
        require(contentType.isNotEmpty()) { "Content type cannot be empty" }
        check(nativeHandle != 0L) { "SipClient is not started" }
        val uri = when {
            target.startsWith("sip:") -> target
            target.contains("@") -> "sip:$target"
            else -> "sip:$target@$domain"
        }
        val result = nativeSendMessage(nativeHandle, uri, contentType, body)
        check(result > 0) { "Failed to send message to $uri (error ${-result})" }
        return result
    }

    fun setMuted(muted: Boolean) {
        isMicrophoneMuted = muted
    }

    fun setSpeakerphone(enabled: Boolean) {
        isSpeakerEnabled = enabled
    }

    fun setAudioDriver(srcMod: String, srcDev: String = "", playMod: String, playDev: String = ""): Int {
        return if (nativeHandle != 0L) {
            nativeSetAudioDriver(nativeHandle, srcMod, srcDev, playMod, playDev)
        } else {
            -1
        }
    }

    fun getAudioDriver(): Pair<String, String> {
        if (nativeHandle == 0L) return Pair("", "")
        val arr = nativeGetAudioDriver(nativeHandle) ?: return Pair("", "")
        val src = arr.getOrNull(0) ?: ""
        val play = arr.getOrNull(1) ?: ""
        return Pair(src, play)
    }

    fun destroy() {
        stop()
    }

    // Callbacks from JNI dispatched on main thread
    @JvmSynthetic
    fun onNativeRegState(state: Int, code: Int, reason: String) {
        val regState = SipRegistrationState.fromInt(state)
        listener?.onRegistrationStateChanged(regState, code, reason)
    }

    @JvmSynthetic
    fun onNativeCallState(callHandle: Long, state: Int, code: Int, reason: String) {
        val call = activeCallsMap.getOrPut(callHandle) {
            SipCall(
                nativeHandle = callHandle,
                callId = nativeCallGetId(callHandle),
                remoteUri = nativeCallGetRemoteUri(callHandle),
                peerDisplayName = nativeCallGetPeerDisplayName(callHandle),
                hasVideo = nativeCallHasVideo(callHandle),
                isOutgoing = nativeCallIsOutgoing(callHandle)
            )
        }
        val callState = SipCallState.fromInt(state)
        when (callState) {
            SipCallState.ESTABLISHED -> telecomCoordinator?.setActive(call.callId)
            SipCallState.HOLD -> telecomCoordinator?.setInactive(call.callId)
            SipCallState.CLOSED -> {
                activeCallsMap.remove(callHandle)
                val disconnectReason = when (code) {
                    486 -> SipDisconnectReason.BUSY
                    487, 603 -> SipDisconnectReason.REJECTED
                    else -> SipDisconnectReason.REMOTE
                }
                telecomCoordinator?.disconnect(call.callId, disconnectReason)
                if (activeCallsMap.isEmpty()) {
                    audioManager?.releaseAudioSession()
                }
            }
            else -> {}
        }
        listener?.onCallStateChanged(call, callState, code, reason)
    }

    internal fun registerPendingPushHandle(callId: String, handle: SipPushCallHandle) {
        pendingPushHandles[callId] = handle
    }

    internal fun removePendingPushHandle(callId: String) {
        pendingPushHandles.remove(callId)
    }

    internal fun notifyPushCallMissed(callId: String, callerUri: String) {
        listener?.onPushCallMissed(callId, callerUri)
    }

    fun attachPendingPushCall(sipCallId: String, call: SipCall): Boolean {
        val handle = pendingPushHandles.remove(sipCallId) ?: return false
        return handle.onMatchingInviteArrived(call)
    }

    @JvmSynthetic
    fun onNativeIncomingCall(callHandle: Long) {
        val call = activeCallsMap.getOrPut(callHandle) {
            SipCall(
                nativeHandle = callHandle,
                callId = nativeCallGetId(callHandle),
                remoteUri = nativeCallGetRemoteUri(callHandle),
                peerDisplayName = nativeCallGetPeerDisplayName(callHandle),
                hasVideo = nativeCallHasVideo(callHandle),
                isOutgoing = false
            )
        }
        val attachedToPush = attachPendingPushCall(call.callId, call)
        if (!attachedToPush) {
            telecomCoordinator?.addCall(
                callId = call.callId,
                displayName = call.peerDisplayName ?: call.remoteUri,
                address = call.remoteUri,
                isOutgoing = false,
                isVideo = call.hasVideo,
                callbacks = createTelecomCallbacks(call)
            )
        }
        listener?.onIncomingCall(call)
    }

    @JvmSynthetic
    fun onNativeCallMedia(callHandle: Long, width: Int, height: Int, onHold: Boolean, isMuted: Boolean) {
        val action = Runnable {
            if (width > 0 && height > 0) {
                remoteVideoView?.setVideoDimensions(width, height)
                remoteVideoView?.incrementRenderedFrameCount()
            }
            val call = SipCall(
                nativeHandle = callHandle,
                callId = nativeCallGetId(callHandle),
                remoteUri = nativeCallGetRemoteUri(callHandle),
                peerDisplayName = nativeCallGetPeerDisplayName(callHandle),
                hasVideo = nativeCallHasVideo(callHandle) || (width > 0 && height > 0),
                isOutgoing = nativeCallIsOutgoing(callHandle)
            )
            listener?.onCallMediaChanged(call, width, height, onHold, isMuted)
        }
        if (Looper.myLooper() == Looper.getMainLooper()) {
            action.run()
        } else if (mainHandler != null) {
            mainHandler.post(action)
        } else {
            action.run()
        }
    }

    @JvmSynthetic
    fun onNativeDtmfDigit(callHandle: Long, digit: String) {
        val call = SipCall(
            nativeHandle = callHandle,
            callId = nativeCallGetId(callHandle),
            remoteUri = nativeCallGetRemoteUri(callHandle),
            peerDisplayName = nativeCallGetPeerDisplayName(callHandle),
            hasVideo = nativeCallHasVideo(callHandle),
            isOutgoing = nativeCallIsOutgoing(callHandle)
        )
        listener?.onDtmfDigit(call, digit)
    }

    @JvmSynthetic
    fun onNativeCallStats(
        callHandle: Long,
        audioLossRate: Float,
        audioJitterMs: Int,
        audioRttMs: Int,
        audioBitrateKbps: Int,
        audioCodec: String,
        videoLossRate: Float,
        videoJitterMs: Int,
        videoBitrateKbps: Int,
        videoFps: Int,
        videoCodec: String
    ) {
        val call = SipCall(
            nativeHandle = callHandle,
            callId = nativeCallGetId(callHandle),
            remoteUri = nativeCallGetRemoteUri(callHandle),
            peerDisplayName = nativeCallGetPeerDisplayName(callHandle),
            hasVideo = nativeCallHasVideo(callHandle),
            isOutgoing = nativeCallIsOutgoing(callHandle)
        )
        val stats = SipCallStats(
            audioLossRate = audioLossRate,
            audioJitterMs = audioJitterMs,
            audioRttMs = audioRttMs,
            audioBitrateKbps = audioBitrateKbps,
            audioCodec = audioCodec,
            videoLossRate = videoLossRate,
            videoJitterMs = videoJitterMs,
            videoBitrateKbps = videoBitrateKbps,
            videoFps = videoFps,
            videoCodec = videoCodec
        )
        listener?.onCallStats(call, stats)
    }

    @JvmSynthetic
    fun onNativeTransferStatus(callHandle: Long, code: Int, reason: String) {
        val call = SipCall(
            nativeHandle = callHandle,
            callId = nativeCallGetId(callHandle),
            remoteUri = nativeCallGetRemoteUri(callHandle),
            peerDisplayName = nativeCallGetPeerDisplayName(callHandle),
            hasVideo = nativeCallHasVideo(callHandle),
            isOutgoing = nativeCallIsOutgoing(callHandle)
        )
        listener?.onTransferStatus(call, code, reason)
    }

    @JvmSynthetic
    fun onNativeTransferRequest(callHandle: Long, target: String) {
        val call = SipCall(
            nativeHandle = callHandle,
            callId = nativeCallGetId(callHandle),
            remoteUri = nativeCallGetRemoteUri(callHandle),
            peerDisplayName = nativeCallGetPeerDisplayName(callHandle),
            hasVideo = nativeCallHasVideo(callHandle),
            isOutgoing = nativeCallIsOutgoing(callHandle)
        )
        listener?.onTransferRequest(call, target)
    }

    // Text from the network arrives as UTF-8 bytes; malformed bytes decode to U+FFFD.
    @JvmSynthetic
    fun onNativeMessageReceived(from: ByteArray, contentType: ByteArray, body: ByteArray) {
        listener?.onMessageReceived(
            SipIncomingMessage(String(from, Charsets.UTF_8), String(contentType, Charsets.UTF_8), body)
        )
    }

    @JvmSynthetic
    fun onNativeMessageStatus(messageId: Long, status: Int, code: Int, reason: ByteArray) {
        listener?.onMessageStatusChanged(messageId, SipMessageStatus.fromInt(status), code, String(reason, Charsets.UTF_8))
    }

    @JvmSynthetic
    fun onNativeCallRejectedBusy(callerUri: ByteArray) {
        listener?.onCallRejectedBusy(String(callerUri, Charsets.UTF_8))
    }

    @JvmSynthetic
    fun onNativePrepareAudioSession(): Boolean {
        return audioManager?.prepareAudioSession() ?: true
    }

    @JvmSynthetic
    fun onNativeReleaseAudioSession() {
        audioManager?.releaseAudioSession()
    }

    // Native methods
    private external fun nativeCreate(): Long
    private external fun nativeInit(handle: Long, configPath: String?): Int
    private external fun nativeRegister(handle: Long, user: String, domain: String, pwd: String, transport: String, port: Int): Int
    private external fun nativeRegisterAccount(
        handle: Long,
        user: String,
        domain: String,
        pwd: String,
        displayName: String,
        transport: String,
        port: Int,
        tlsCaFile: String,
        verifyTlsCert: Boolean,
        mediaEnc: String,
        maxCalls: Int
    ): Int
    private external fun nativeUnregister(handle: Long): Int
    private external fun nativeResetTransports(handle: Long, reRegister: Boolean): Int
    private external fun nativeIsRegistered(handle: Long): Boolean
    private external fun nativeSetMicMuted(handle: Long, muted: Boolean)
    private external fun nativeMakeCall(handle: Long, uri: String, withVideo: Boolean): Long
    private external fun nativeDialCall(handle: Long, uri: String, withVideo: Boolean, outStatus: IntArray): Long
    /** Returns the message id (> 0) or a negative errno-style code. */
    private external fun nativeSendMessage(handle: Long, uri: String, contentType: String, body: ByteArray): Long
    private external fun nativeDestroy(handle: Long)

    private external fun nativeCallGetId(callHandle: Long): String
    private external fun nativeCallGetRemoteUri(callHandle: Long): String
    private external fun nativeCallGetPeerDisplayName(callHandle: Long): String?
    private external fun nativeCallIsOutgoing(callHandle: Long): Boolean
    private external fun nativeCallHasVideo(callHandle: Long): Boolean
    private external fun nativeSetRemoteSurface(handle: Long, surface: Any?): Int

    private external fun nativeSwitchCamera(handle: Long): Int
    private external fun nativeSetCamera(handle: Long, position: Int): Int
    private external fun nativeSetLocalVideoEnabled(handle: Long, enabled: Boolean): Int
    private external fun nativeIsLocalVideoEnabled(handle: Long): Boolean
    private external fun nativeStartLocalPreview(handle: Long): Int
    private external fun nativeStopLocalPreview(handle: Long): Int
    private external fun nativeIsLocalPreviewActive(handle: Long): Boolean

    private external fun nativeSetAudioDriver(handle: Long, srcMod: String, srcDev: String, playMod: String, playDev: String): Int
    private external fun nativeGetAudioDriver(handle: Long): Array<String>?
}
