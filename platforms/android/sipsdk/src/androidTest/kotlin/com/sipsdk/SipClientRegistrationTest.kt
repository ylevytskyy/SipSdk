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

        client.destroy()
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

        val startRc = client.start()
        assertEquals(0, startRc)

        val regRc = client.register()
        assertEquals(0, regRc)

        assertTrue("Timeout waiting for registration failure", failLatch.await(10, TimeUnit.SECONDS))
        assertTrue("Expected FAILED state on invalid credentials", failedStateReceived)
        assertTrue("Expected 401/403 SIP failure code, got $failCode", failCode in listOf(401, 403))
        assertFalse("Client must not report isRegistered == true", client.isRegistered)

        client.destroy()
    }
}
