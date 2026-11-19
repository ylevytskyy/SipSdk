package com.sipsdk

import android.Manifest
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import androidx.test.rule.GrantPermissionRule
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

@RunWith(AndroidJUnit4::class)
class SipPushHandlerTest {

    @get:Rule
    val grantPermissionRule: GrantPermissionRule = GrantPermissionRule.grant(Manifest.permission.RECORD_AUDIO)

    private val context get() = InstrumentationRegistry.getInstrumentation().targetContext

    @Test
    fun testPushAttachMatchingInviteAndMissedTimeoutOnDevice() {
        val telecom = CoreTelecomCoordinator(context)
        val client = SipClient("1003", TestConfig.host, context).apply {
            password = "test1003"
            port = 5060
            transport = "tcp"
            telecomCoordinator = telecom
        }

        val missedLatch = CountDownLatch(1)
        var missedId = ""
        client.listener = object : SipClientListener {
            override fun onPushCallMissed(callId: String, callerUri: String) {
                missedId = callId
                missedLatch.countDown()
            }
        }

        // 1. Timeout case: push arrives, no matching INVITE -> Telecom disconnects as MISSED
        val handle = SipPushHandler.handleIncomingCallPush(
            context = context,
            client = client,
            data = mapOf(
                "sip_call_id" to "device-timeout-call-1",
                "caller_uri" to "sip:1002@${TestConfig.host}",
                "display_name" to "Timeout Caller",
                "has_video" to "false"
            ),
            timeoutMs = 500L,
            autoRegister = false
        )

        assertTrue("Expected onPushCallMissed after 500ms", missedLatch.await(5, TimeUnit.SECONDS))
        assertEquals("device-timeout-call-1", missedId)
        assertTrue(handle.isMissed)

        // 2. Live push attachment verification: register against Asterisk 1001, trigger 9003 callback,
        //    pre-register push handle once callback INVITE arrives or verify attachPendingPushCall by Call-ID.
        val regLatch = CountDownLatch(1)
        val cbEstab = CountDownLatch(1)
        val incomingLatch = CountDownLatch(1)
        var incomingCall: SipCall? = null

        client.listener = object : SipClientListener {
            override fun onRegistrationStateChanged(state: SipRegistrationState, code: Int, reason: String) {
                if (state == SipRegistrationState.REGISTERED && code == 200) regLatch.countDown()
            }

            override fun onCallStateChanged(call: SipCall, state: SipCallState, code: Int, reason: String) {
                if (state == SipCallState.ESTABLISHED && call.isOutgoing) cbEstab.countDown()
            }

            override fun onIncomingCall(call: SipCall) {
                incomingCall = call
                incomingLatch.countDown()
            }
        }

        try {
            assertEquals(0, client.start())
            TestConfig.configureAudioDriverForDevice(client)
            assertEquals(0, client.register())
            assertTrue("Registration timed out", regLatch.await(10, TimeUnit.SECONDS))

            val matchedHandle = SipPushHandler.handleIncomingCallPush(
                context = context,
                client = client,
                data = mapOf(
                    "sip_call_id" to "live-match-id",
                    "from_uri" to "sip:anonymous@anonymous.invalid",
                    "display_name" to "Callback Caller",
                    "has_video" to "false"
                ),
                timeoutMs = 8_000L,
                autoRegister = false
            )
            assertFalse(matchedHandle.isAttached)
            assertTrue(client.attachPendingPushCall("live-match-id", SipCall(0L, "live-match-id", "sip:anonymous@anonymous.invalid")))
            assertTrue(matchedHandle.isAttached)
            assertFalse(matchedHandle.isMissed)

            val cb = client.dial("sip:9003@${TestConfig.host}:5060", withVideo = false)
            assertTrue("9003 trigger failed to establish", cbEstab.await(8, TimeUnit.SECONDS))
            cb.hangup(0, "Hangup")
            assertTrue("Expected incoming call from 9003 callback", incomingLatch.await(10, TimeUnit.SECONDS))
            assertTrue(incomingCall != null)
            incomingCall?.hangup(486, "Done")
        } finally {
            try { client.unregister() } catch (_: Throwable) {}
            client.destroy()
        }
    }
}
