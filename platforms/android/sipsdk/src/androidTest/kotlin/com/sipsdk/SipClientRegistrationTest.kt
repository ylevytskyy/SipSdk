package com.sipsdk

import android.os.Looper
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

@RunWith(AndroidJUnit4::class)
class SipClientRegistrationTest {

    private val context get() = InstrumentationRegistry.getInstrumentation().targetContext

    @Test
    fun testRegistrationAndUnregistration() {
        val client = SipClient("1001", TestConfig.host, context).apply {
            password = "test1001"
            port = 5060
            transport = "udp"
        }

        val regLatch = CountDownLatch(1)
        var registeredStateReceived = false
        var looperIsMainOnReg = false

        val unregLatch = CountDownLatch(1)
        var unregisteredStateReceived = false

        client.listener = object : SipClientListener {
            override fun onRegistrationStateChanged(state: SipRegistrationState, code: Int, reason: String) {
                if (Looper.myLooper() == Looper.getMainLooper()) {
                    looperIsMainOnReg = true
                }
                if (state == SipRegistrationState.REGISTERED && code == 200) {
                    registeredStateReceived = true
                    regLatch.countDown()
                } else if (state == SipRegistrationState.UNREGISTERED) {
                    unregisteredStateReceived = true
                    unregLatch.countDown()
                }
            }
        }

        try {
            val startRc = client.start()
            assertEquals(0, startRc)

            val regRc = client.register()
            assertEquals(0, regRc)

            assertTrue("Timeout waiting for SIP registration", regLatch.await(10, TimeUnit.SECONDS))
            assertTrue("Expected REGISTERED state", registeredStateReceived)
            assertTrue("Callback must be dispatched on Main Looper", looperIsMainOnReg)
            assertTrue("Client must report isRegistered == true", client.isRegistered)

            // Now unregister
            val unregRc = client.unregister()
            assertEquals(0, unregRc)

            assertTrue("Timeout waiting for SIP unregistration", unregLatch.await(10, TimeUnit.SECONDS))
            assertTrue("Expected UNREGISTERED state", unregisteredStateReceived)

            var isReg = client.isRegistered
            val deadline = System.currentTimeMillis() + 2000
            while (isReg && System.currentTimeMillis() < deadline) {
                Thread.sleep(50)
                isReg = client.isRegistered
            }
            assertFalse("Client must report isRegistered == false", isReg)
        } finally {
            client.destroy()
        }
    }

    @Test
    fun testRegistrationFailureWithInvalidCredentials() {
        val client = SipClient("1001", TestConfig.host, context).apply {
            password = "wrong-password-999"
            port = 5060
            transport = "udp"
        }

        val failLatch = CountDownLatch(1)
        var failedStateReceived = false
        var failCode = 0

        client.listener = object : SipClientListener {
            override fun onRegistrationStateChanged(state: SipRegistrationState, code: Int, reason: String) {
                if (state == SipRegistrationState.FAILED) {
                    failedStateReceived = true
                    failCode = code
                    failLatch.countDown()
                }
            }
        }

        try {
            val startRc = client.start()
            assertEquals(0, startRc)

            val regRc = client.register()
            assertEquals(0, regRc)

            assertTrue("Timeout waiting for registration failure", failLatch.await(10, TimeUnit.SECONDS))
            assertTrue("Expected FAILED state on invalid credentials", failedStateReceived)
            assertTrue("Expected 401/403 SIP failure code, got $failCode", failCode in listOf(401, 403))
            assertFalse("Client must not report isRegistered == true", client.isRegistered)
        } finally {
            client.destroy()
        }
    }

    @Test
    fun testMessageRoundTrip() {
        val client = SipClient("1001", TestConfig.host, context).apply {
            password = "test1001"
            port = 5060
            transport = "udp"
        }

        val regLatch = CountDownLatch(1)
        val statusLatch = CountDownLatch(1)
        val receiveLatch = CountDownLatch(1)
        var receivedStatus: SipMessageStatus? = null
        var receivedCode = 0
        var receivedMsg: SipIncomingMessage? = null

        client.listener = object : SipClientListener {
            override fun onRegistrationStateChanged(state: SipRegistrationState, code: Int, reason: String) {
                if (state == SipRegistrationState.REGISTERED) {
                    regLatch.countDown()
                }
            }

            override fun onMessageStatusChanged(messageId: Long, status: SipMessageStatus, code: Int, reason: String) {
                receivedStatus = status
                receivedCode = code
                statusLatch.countDown()
            }

            override fun onMessageReceived(message: SipIncomingMessage) {
                receivedMsg = message
                receiveLatch.countDown()
            }
        }

        try {
            assertEquals(0, client.start())
            assertEquals(0, client.register())
            assertTrue("Timeout waiting for SIP registration", regLatch.await(10, TimeUnit.SECONDS))

            val msgId = client.sendMessage("1001", "hello from Android")
            assertTrue("Expected positive message id", msgId > 0)
            assertTrue("Timeout waiting for message status", statusLatch.await(10, TimeUnit.SECONDS))
            assertEquals(SipMessageStatus.SENT, receivedStatus)
            assertTrue("Expected 2xx status code, got $receivedCode", receivedCode in 200..299)

            assertTrue("Timeout waiting for echoed message", receiveLatch.await(10, TimeUnit.SECONDS))
            assertEquals("text/plain", receivedMsg?.contentType)
            assertEquals("hello from Android", receivedMsg?.text)
            assertTrue("Expected sender sip:1001@, got ${receivedMsg?.from}", receivedMsg?.from?.startsWith("sip:1001@") == true)

            client.unregister()
        } finally {
            client.destroy()
        }
    }

    @Test
    fun testTlsRegistrationRejectsInvalidCaAndSucceedsWhenUnverified() {
        val strictClient = SipClient("1004", TestConfig.host, context).apply {
            password = "test1004"
            port = 5061
            transport = "tls"
            tlsCaFile = "/nonexistent/ca.pem"
            verifyTlsCert = true
            mediaEnc = "srtp-mand"
        }
        val failLatch = CountDownLatch(1)
        var gotFailure = false
        strictClient.listener = object : SipClientListener {
            override fun onRegistrationStateChanged(state: SipRegistrationState, code: Int, reason: String) {
                if (state == SipRegistrationState.FAILED) {
                    gotFailure = true
                    failLatch.countDown()
                }
            }
        }
        try {
            assertEquals(0, strictClient.start())
            assertEquals(0, strictClient.register())
            assertTrue("Expected TLS registration to fail on untrusted cert when verifyTlsCert=true", failLatch.await(10, TimeUnit.SECONDS))
            assertTrue(gotFailure)
        } finally {
            strictClient.destroy()
        }
        Thread.sleep(300)

        val client = SipClient("1004", TestConfig.host, context).apply {
            password = "test1004"
            port = 5061
            transport = "tls"
            verifyTlsCert = false
            mediaEnc = "srtp-mand"
        }
        val regLatch = CountDownLatch(1)
        var registered = false
        var lastState = SipRegistrationState.UNREGISTERED
        var lastCode = 0
        var lastReason = ""
        client.listener = object : SipClientListener {
            override fun onRegistrationStateChanged(state: SipRegistrationState, code: Int, reason: String) {
                lastState = state
                lastCode = code
                lastReason = reason
                if (state == SipRegistrationState.REGISTERED && code == 200) {
                    registered = true
                    regLatch.countDown()
                }
            }
        }
        try {
            assertEquals(0, client.start())
            assertEquals(0, client.register())
            val regOk = regLatch.await(15, TimeUnit.SECONDS)
            assertTrue(
                "Timeout waiting for TLS registration (lastState=$lastState code=$lastCode reason=$lastReason)",
                regOk
            )
            assertTrue(registered)
            client.unregister()
        } finally {
            client.destroy()
        }
    }
}
