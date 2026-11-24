package com.sipsdk

import android.content.Context
import java.util.UUID
import java.util.concurrent.Executors
import java.util.concurrent.ScheduledFuture
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicReference

/**
 * Parsed incoming push notification payload supporting unified cross-platform keys:
 * - `sip_call_id` (alias `call_id`)
 * - `caller_uri` (aliases `from_uri`, `from`)
 * - `display_name` (alias `caller_name`)
 * - `has_video` (alias `video`)
 */
data class SipPushCallPayload(
    val sipCallId: String,
    val callerUri: String,
    val displayName: String,
    val hasVideo: Boolean,
    val rawData: Map<String, String> = emptyMap()
) {
    companion object {
        fun parse(data: Map<String, String>): SipPushCallPayload {
            val sipCallId = (data["sip_call_id"] ?: data["call_id"])
                ?.takeIf { it.isNotBlank() }
                ?: UUID.randomUUID().toString()
            val callerUri = (data["caller_uri"] ?: data["from_uri"] ?: data["from"])
                ?.takeIf { it.isNotBlank() }
                ?: "Unknown"
            val displayName = (data["display_name"] ?: data["caller_name"])
                ?.takeIf { it.isNotBlank() }
                ?: callerUri
            val rawVideo = data["has_video"] ?: data["video"]
            val hasVideo = when (rawVideo?.trim()?.lowercase()) {
                "1", "true", "yes" -> true
                else -> false
            }
            return SipPushCallPayload(
                sipCallId = sipCallId,
                callerUri = callerUri,
                displayName = displayName,
                hasVideo = hasVideo,
                rawData = data
            )
        }
    }
}

/**
 * Tracks a ringing Telecom call opened by an incoming VoIP push notification until either
 * the matching SIP `INVITE` arrives or the missed-call timeout elapses.
 */
class SipPushCallHandle internal constructor(
    val payload: SipPushCallPayload,
    private val client: SipClient,
    private val telecom: SipTelecomManagerCoordinator?
) {
    private enum class State { PENDING, ATTACHED, MISSED, DECLINED }

    private val stateRef = AtomicReference(State.PENDING)
    private val userAnsweredBeforeInvite = AtomicBoolean(false)
    private val attachedVideoChoice = AtomicBoolean(payload.hasVideo)
    private val attachedCall = AtomicReference<SipCall?>(null)
    @Volatile
    private var timeoutFuture: ScheduledFuture<*>? = null

    val isAttached: Boolean get() = stateRef.get() == State.ATTACHED
    val isMissed: Boolean get() = stateRef.get() == State.MISSED
    val isDeclined: Boolean get() = stateRef.get() == State.DECLINED

    internal fun setTimeoutFuture(future: ScheduledFuture<*>) {
        timeoutFuture = future
    }

    internal fun buildTelecomCallbacks(): SipTelecomCallCallbacks = object : SipTelecomCallCallbacks {
        override fun onAnswer(isVideo: Boolean) {
            val call = attachedCall.get()
            if (call != null) {
                call.answer(isVideo)
            } else {
                attachedVideoChoice.set(isVideo)
                userAnsweredBeforeInvite.set(true)
            }
        }

        override fun onDisconnect() {
            val call = attachedCall.get()
            if (call != null) {
                call.hangup(0, "Hangup")
            } else if (stateRef.compareAndSet(State.PENDING, State.DECLINED)) {
                cancelTimeout()
                client.removePendingPushHandle(payload.sipCallId, this@SipPushCallHandle)
                telecom?.disconnect(payload.sipCallId, SipDisconnectReason.REJECTED)
            }
        }

        override fun onSetActive() {
            attachedCall.get()?.setHold(false)
        }

        override fun onSetInactive() {
            attachedCall.get()?.setHold(true)
        }

        override fun onMuteChanged(isMuted: Boolean) {
            client.isMicrophoneMuted = isMuted
        }
    }

    internal fun onMatchingInviteArrived(call: SipCall): Boolean {
        while (true) {
            when (stateRef.get()) {
                State.MISSED, State.ATTACHED, null -> return false
                State.DECLINED -> {
                    cancelTimeout()
                    attachedCall.set(call)
                    call.hangup(486, "Decline")
                    return true
                }
                State.PENDING -> {
                    if (stateRef.compareAndSet(State.PENDING, State.ATTACHED)) {
                        cancelTimeout()
                        attachedCall.set(call)
                        if (userAnsweredBeforeInvite.get()) {
                            call.answer(attachedVideoChoice.get())
                        }
                        return true
                    }
                }
            }
        }
    }

    internal fun onTimeout() {
        if (!stateRef.compareAndSet(State.PENDING, State.MISSED)) {
            return
        }
        client.removePendingPushHandle(payload.sipCallId, this)
        telecom?.disconnect(payload.sipCallId, SipDisconnectReason.MISSED)
        client.notifyPushCallMissed(payload.sipCallId, payload.callerUri)
    }

    internal fun cancelReplaced() {
        if (stateRef.compareAndSet(State.PENDING, State.MISSED)) {
            cancelTimeout()
            telecom?.disconnect(payload.sipCallId, SipDisconnectReason.ERROR)
        } else {
            cancelTimeout()
        }
    }

    fun cancelTimeout() {
        timeoutFuture?.cancel(false)
        timeoutFuture = null
    }
}

/**
 * Firebase/push-provider-agnostic handler for waking Android Telecom on incoming VoIP push messages.
 */
object SipPushHandler {
    private val scheduler = Executors.newSingleThreadScheduledExecutor { r ->
        Thread(r, "SipPushTimeout").apply { isDaemon = true }
    }

    fun handleIncomingCallPush(
        context: Context?,
        client: SipClient,
        data: Map<String, String>,
        timeoutMs: Long = 10_000L,
        autoRegister: Boolean = true
    ): SipPushCallHandle {
        val payload = SipPushCallPayload.parse(data)

        if (client.telecomCoordinator == null && context != null) {
            client.telecomCoordinator = CoreTelecomCoordinator(context.applicationContext)
        }
        val coordinator = client.telecomCoordinator

        val handle = SipPushCallHandle(payload, client, coordinator)
        client.registerPendingPushHandle(payload.sipCallId, handle)

        coordinator?.addCall(
            callId = payload.sipCallId,
            displayName = payload.displayName,
            address = payload.callerUri,
            isOutgoing = false,
            isVideo = payload.hasVideo,
            callbacks = handle.buildTelecomCallbacks()
        )

        if (autoRegister && !client.isRegistered) {
            client.register()
        }

        val future = scheduler.schedule(
            { handle.onTimeout() },
            timeoutMs.coerceAtLeast(50L),
            TimeUnit.MILLISECONDS
        )
        handle.setTimeoutFuture(future)
        return handle
    }
}
