package com.sipsdk.sample

import android.util.Log
import com.google.firebase.messaging.FirebaseMessagingService
import com.google.firebase.messaging.RemoteMessage
import com.sipsdk.CoreTelecomCoordinator
import com.sipsdk.SipCall
import com.sipsdk.SipCallState
import com.sipsdk.SipClient
import com.sipsdk.SipClientListener
import com.sipsdk.SipPushHandler
import com.sipsdk.SipRegistrationState
import com.sipsdk.sample.models.AccountStore

class SampleSipPushService : FirebaseMessagingService() {

    companion object {
        private const val TAG = "SampleSipPushService"

        @Volatile
        var sharedClient: SipClient? = null
    }

    override fun onNewToken(token: String) {
        super.onNewToken(token)
        Log.i(TAG, "FCM_TOKEN=$token")
    }

    override fun onMessageReceived(message: RemoteMessage) {
        super.onMessageReceived(message)
        val data = message.data
        Log.i(TAG, "FCM_PUSH_RECEIVED from=${message.from} data=$data")
        if (data.isEmpty()) return

        val client = sharedClient ?: createBackgroundClient().also { sharedClient = it }
        val handle = SipPushHandler.handleIncomingCallPush(
            context = applicationContext,
            client = client,
            data = data,
            timeoutMs = 20_000L,
            autoRegister = true
        )
        Log.i(
            TAG,
            "FCM_PUSH_HANDLED callId=${handle.payload.sipCallId} caller=${handle.payload.callerUri} telecomState=${client.telecomCoordinator?.getCallState(handle.payload.sipCallId)}"
        )

        SampleIncomingCallNotifier.showIncomingCallNotification(
            context = applicationContext,
            callId = handle.payload.sipCallId,
            displayName = handle.payload.displayName,
            callerUri = handle.payload.callerUri,
            hasVideo = handle.payload.hasVideo
        )
    }

    private fun createBackgroundClient(): SipClient {
        val account = AccountStore(applicationContext).account
        return SipClient(account.username, account.domain, applicationContext).apply {
            password = account.password
            displayName = account.displayName
            transport = account.transport
            port = account.port
            telecomCoordinator = CoreTelecomCoordinator(applicationContext)
            listener = object : SipClientListener {
                override fun onRegistrationStateChanged(state: SipRegistrationState, code: Int, reason: String) {
                    Log.i(TAG, "BG_REG state=$state code=$code reason=$reason")
                }

                override fun onIncomingCall(call: SipCall) {
                    Log.i(TAG, "BG_INCOMING callId=${call.callId} remote=${call.remoteUri}")
                }

                override fun onCallStateChanged(call: SipCall, state: SipCallState, code: Int, reason: String) {
                    Log.i(TAG, "BG_CALL_STATE callId=${call.callId} state=$state code=$code reason=$reason")
                    if (state == SipCallState.CLOSED) {
                        SampleIncomingCallNotifier.cancel(applicationContext)
                    }
                }

                override fun onPushCallMissed(callId: String, callerUri: String) {
                    Log.i(TAG, "BG_PUSH_MISSED callId=$callId caller=$callerUri")
                    SampleIncomingCallNotifier.cancel(applicationContext)
                }
            }
            start()
        }
    }
}
