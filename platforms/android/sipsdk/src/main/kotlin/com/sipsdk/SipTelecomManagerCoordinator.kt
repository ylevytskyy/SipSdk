package com.sipsdk

import android.content.Context
import android.net.Uri
import android.telecom.DisconnectCause
import android.util.Log
import androidx.core.telecom.CallAttributesCompat
import androidx.core.telecom.CallControlResult
import androidx.core.telecom.CallControlScope
import androidx.core.telecom.CallEndpointCompat
import androidx.core.telecom.CallsManager
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.launch
import java.util.concurrent.ConcurrentHashMap

enum class SipTelecomCallState {
    DIALING,
    RINGING,
    ACTIVE,
    HELD,
    DISCONNECTED
}

enum class SipTelecomEndpoint {
    EARPIECE,
    SPEAKER,
    BLUETOOTH,
    WIRED_HEADSET
}

enum class SipDisconnectReason {
    LOCAL,
    REMOTE,
    BUSY,
    REJECTED,
    MISSED,
    ERROR
}

interface SipTelecomCallCallbacks {
    fun onAnswer(isVideo: Boolean)
    fun onDisconnect()
    fun onSetActive()
    fun onSetInactive()
    fun onMuteChanged(isMuted: Boolean) {}
    fun onEndpointChanged(endpoint: SipTelecomEndpoint) {}
}

/**
 * Abstraction over Android Telecom (`androidx.core:core-telecom` [CallsManager]).
 * Hides Jetpack Telecom types behind a coordinator boundary so SIP calls and system
 * Telecom state stay synchronized across API 26+.
 */
interface SipTelecomManagerCoordinator {
    fun registerApp(supportsVideo: Boolean = true)

    fun addCall(
        callId: String,
        displayName: String,
        address: String,
        isOutgoing: Boolean,
        isVideo: Boolean,
        callbacks: SipTelecomCallCallbacks
    )

    fun setActive(callId: String)
    fun setInactive(callId: String)
    fun disconnect(callId: String, reason: SipDisconnectReason = SipDisconnectReason.LOCAL)
    fun requestEndpointChange(callId: String, endpoint: SipTelecomEndpoint)
    fun getCallState(callId: String): SipTelecomCallState?
    fun getEndpoint(callId: String): SipTelecomEndpoint?
}

/**
 * Production Telecom coordinator backed by `androidx.core:core-telecom` (`CallsManager`).
 */
class CoreTelecomCoordinator internal constructor(
    context: Context,
    private val callsManager: CallsManager = CallsManager(context.applicationContext),
    private val scope: CoroutineScope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
) : SipTelecomManagerCoordinator {

    constructor(context: Context) : this(
        context = context,
        callsManager = CallsManager(context.applicationContext),
        scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    )

    private companion object {
        const val TAG = "CoreTelecomCoordinator"
    }

    private data class TrackedCall(
        val callId: String,
        val callbacks: SipTelecomCallCallbacks,
        @Volatile var state: SipTelecomCallState,
        @Volatile var currentEndpoint: SipTelecomEndpoint = SipTelecomEndpoint.EARPIECE,
        @Volatile var controlScope: CallControlScope? = null,
        @Volatile var availableEndpoints: List<CallEndpointCompat> = emptyList(),
        val scopeReady: CompletableDeferred<CallControlScope> = CompletableDeferred()
    )

    private val trackedCalls = ConcurrentHashMap<String, TrackedCall>()
    @Volatile
    private var appRegistered = false
    @Volatile
    private var lastInactiveJob: kotlinx.coroutines.Job? = null

    override fun registerApp(supportsVideo: Boolean) {
        if (appRegistered) return
        val caps = CallsManager.CAPABILITY_BASELINE or
            (if (supportsVideo) CallsManager.CAPABILITY_SUPPORTS_VIDEO_CALLING else 0)
        callsManager.registerAppWithTelecom(caps)
        appRegistered = true
    }

    override fun addCall(
        callId: String,
        displayName: String,
        address: String,
        isOutgoing: Boolean,
        isVideo: Boolean,
        callbacks: SipTelecomCallCallbacks
    ) {
        registerApp(supportsVideo = true)

        val initialState = if (isOutgoing) SipTelecomCallState.DIALING else SipTelecomCallState.RINGING
        val tracked = TrackedCall(
            callId = callId,
            callbacks = callbacks,
            state = initialState
        )
        trackedCalls[callId] = tracked

        val safeUri = runCatching { Uri.parse(address) }.getOrElse { Uri.fromParts("sip", address, null) }
        val attributes = CallAttributesCompat(
            displayName = displayName.ifBlank { address },
            address = safeUri,
            direction = if (isOutgoing) {
                CallAttributesCompat.DIRECTION_OUTGOING
            } else {
                CallAttributesCompat.DIRECTION_INCOMING
            },
            callType = if (isVideo) {
                CallAttributesCompat.CALL_TYPE_VIDEO_CALL
            } else {
                CallAttributesCompat.CALL_TYPE_AUDIO_CALL
            },
            callCapabilities = CallAttributesCompat.SUPPORTS_SET_INACTIVE
        )

        val prevInactiveJob = lastInactiveJob
        scope.launch {
            runCatching { prevInactiveJob?.join() }
            try {
                callsManager.addCall(
                    callAttributes = attributes,
                    onAnswer = { callType ->
                        tracked.state = SipTelecomCallState.ACTIVE
                        callbacks.onAnswer(callType == CallAttributesCompat.CALL_TYPE_VIDEO_CALL)
                    },
                    onDisconnect = { _ ->
                        tracked.state = SipTelecomCallState.DISCONNECTED
                        trackedCalls.remove(callId)
                        callbacks.onDisconnect()
                    },
                    onSetActive = {
                        tracked.state = SipTelecomCallState.ACTIVE
                        callbacks.onSetActive()
                    },
                    onSetInactive = {
                        tracked.state = SipTelecomCallState.HELD
                        callbacks.onSetInactive()
                    }
                ) {
                    tracked.controlScope = this
                    tracked.scopeReady.complete(this)

                    launch {
                        availableEndpoints.collectLatest { endpoints ->
                            tracked.availableEndpoints = endpoints
                        }
                    }
                    launch {
                        currentCallEndpoint.collectLatest { endpoint ->
                            val mapped = mapFromCompatEndpoint(endpoint.type)
                            tracked.currentEndpoint = mapped
                            callbacks.onEndpointChanged(mapped)
                        }
                    }
                    launch {
                        isMuted.collectLatest { muted ->
                            callbacks.onMuteChanged(muted)
                        }
                    }
                }
            } catch (t: Throwable) {
                Log.w(TAG, "Telecom addCall failed for $callId: ${t.message}")
                trackedCalls.remove(callId, tracked)
                if (!tracked.scopeReady.isCompleted) {
                    tracked.scopeReady.completeExceptionally(t)
                }
            }
        }
    }

    override fun setActive(callId: String) {
        val tracked = trackedCalls[callId] ?: return
        tracked.state = SipTelecomCallState.ACTIVE
        scope.launch {
            runCatching {
                val ctrl = tracked.scopeReady.await()
                if (tracked.state == SipTelecomCallState.ACTIVE && trackedCalls[callId] === tracked) {
                    ctrl.setActive()
                }
            }
        }
    }

    override fun setInactive(callId: String) {
        val tracked = trackedCalls[callId] ?: return
        tracked.state = SipTelecomCallState.HELD
        lastInactiveJob = scope.launch {
            runCatching {
                val ctrl = tracked.scopeReady.await()
                if (tracked.state == SipTelecomCallState.HELD && trackedCalls[callId] === tracked) {
                    ctrl.setInactive()
                }
            }
        }
    }

    override fun disconnect(callId: String, reason: SipDisconnectReason) {
        val tracked = trackedCalls.remove(callId) ?: return
        tracked.state = SipTelecomCallState.DISCONNECTED
        val cause = DisconnectCause(mapDisconnectCode(reason))
        scope.launch {
            runCatching {
                val ctrl = tracked.scopeReady.await()
                val res = ctrl.disconnect(cause)
                if (res is CallControlResult.Error) {
                    Log.w(TAG, "Telecom disconnect error for $callId: ${res.errorCode}")
                }
            }
        }
    }

    override fun requestEndpointChange(callId: String, endpoint: SipTelecomEndpoint) {
        val tracked = trackedCalls[callId] ?: return
        tracked.currentEndpoint = endpoint
        val targetType = mapToCompatEndpointType(endpoint)
        scope.launch {
            runCatching {
                val ctrl = tracked.scopeReady.await()
                val match = tracked.availableEndpoints.firstOrNull { it.type == targetType }
                    ?: return@runCatching
                ctrl.requestEndpointChange(match)
            }
        }
    }

    override fun getCallState(callId: String): SipTelecomCallState? = trackedCalls[callId]?.state

    override fun getEndpoint(callId: String): SipTelecomEndpoint? = trackedCalls[callId]?.currentEndpoint

    private fun mapDisconnectCode(reason: SipDisconnectReason): Int = when (reason) {
        SipDisconnectReason.LOCAL -> DisconnectCause.LOCAL
        SipDisconnectReason.REMOTE -> DisconnectCause.REMOTE
        SipDisconnectReason.BUSY -> DisconnectCause.BUSY
        SipDisconnectReason.REJECTED -> DisconnectCause.REJECTED
        SipDisconnectReason.MISSED -> DisconnectCause.MISSED
        SipDisconnectReason.ERROR -> DisconnectCause.ERROR
    }

    private fun mapToCompatEndpointType(endpoint: SipTelecomEndpoint): Int = when (endpoint) {
        SipTelecomEndpoint.EARPIECE -> CallEndpointCompat.TYPE_EARPIECE
        SipTelecomEndpoint.SPEAKER -> CallEndpointCompat.TYPE_SPEAKER
        SipTelecomEndpoint.BLUETOOTH -> CallEndpointCompat.TYPE_BLUETOOTH
        SipTelecomEndpoint.WIRED_HEADSET -> CallEndpointCompat.TYPE_WIRED_HEADSET
    }

    private fun mapFromCompatEndpoint(type: Int): SipTelecomEndpoint = when (type) {
        CallEndpointCompat.TYPE_SPEAKER -> SipTelecomEndpoint.SPEAKER
        CallEndpointCompat.TYPE_BLUETOOTH -> SipTelecomEndpoint.BLUETOOTH
        CallEndpointCompat.TYPE_WIRED_HEADSET -> SipTelecomEndpoint.WIRED_HEADSET
        else -> SipTelecomEndpoint.EARPIECE
    }
}

/**
 * Pure JVM coordinator implementation used in unit tests without an Android OS Telecom stack.
 */
class InMemoryTelecomCoordinator : SipTelecomManagerCoordinator {
    private data class Entry(
        val callbacks: SipTelecomCallCallbacks,
        var state: SipTelecomCallState,
        var endpoint: SipTelecomEndpoint = SipTelecomEndpoint.EARPIECE
    )

    private val entries = ConcurrentHashMap<String, Entry>()
    var isAppRegistered: Boolean = false
        private set

    override fun registerApp(supportsVideo: Boolean) {
        isAppRegistered = true
    }

    override fun addCall(
        callId: String,
        displayName: String,
        address: String,
        isOutgoing: Boolean,
        isVideo: Boolean,
        callbacks: SipTelecomCallCallbacks
    ) {
        registerApp(isVideo)
        entries[callId] = Entry(
            callbacks = callbacks,
            state = if (isOutgoing) SipTelecomCallState.DIALING else SipTelecomCallState.RINGING
        )
    }

    override fun setActive(callId: String) {
        entries[callId]?.state = SipTelecomCallState.ACTIVE
    }

    override fun setInactive(callId: String) {
        entries[callId]?.state = SipTelecomCallState.HELD
    }

    override fun disconnect(callId: String, reason: SipDisconnectReason) {
        entries[callId]?.state = SipTelecomCallState.DISCONNECTED
    }

    override fun requestEndpointChange(callId: String, endpoint: SipTelecomEndpoint) {
        entries[callId]?.endpoint = endpoint
    }

    override fun getCallState(callId: String): SipTelecomCallState? = entries[callId]?.state

    override fun getEndpoint(callId: String): SipTelecomEndpoint? = entries[callId]?.endpoint
}
