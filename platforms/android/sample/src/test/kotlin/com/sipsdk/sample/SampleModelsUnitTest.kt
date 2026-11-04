package com.sipsdk.sample

import com.sipsdk.sample.models.CallDirection
import com.sipsdk.sample.models.CallHistoryItem
import com.sipsdk.sample.models.SipAccount
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class SampleModelsUnitTest {

    @Test
    fun testSipAccountDefaultValues() {
        val account = SipAccount()
        assertEquals("1001", account.username)
        assertEquals("10.0.2.2", account.domain)
        assertEquals("test1001", account.password)
        assertEquals("udp", account.transport)
        assertEquals(5060, account.port)
        assertEquals("sip:1001@10.0.2.2:5060", account.aor)
        assertFalse(account.useIce)
        assertEquals(3600, account.regInterval)
    }

    @Test
    fun testSipAccountJsonRoundTrip() {
        val original = SipAccount(
            username = "1002",
            domain = "192.168.1.100",
            password = "secretPassword",
            transport = "tcp",
            port = 5061,
            displayName = "Android User",
            outboundProxy = "sip:proxy.example.com",
            stunServer = "stun:stun.l.google.com:19302",
            useIce = true,
            regInterval = 1800
        )
        val json = original.toJson()
        val restored = SipAccount.fromJson(json)

        assertEquals(original.username, restored.username)
        assertEquals(original.domain, restored.domain)
        assertEquals(original.password, restored.password)
        assertEquals(original.transport, restored.transport)
        assertEquals(original.port, restored.port)
        assertEquals(original.displayName, restored.displayName)
        assertEquals(original.outboundProxy, restored.outboundProxy)
        assertEquals(original.stunServer, restored.stunServer)
        assertEquals(original.useIce, restored.useIce)
        assertEquals(original.regInterval, restored.regInterval)
        assertEquals("sip:1002@192.168.1.100:5061", restored.aor)
    }

    @Test
    fun testCallHistoryItemFormattingAndDisplayTitle() {
        val item1 = CallHistoryItem(
            remoteUri = "sip:9001@10.0.2.2:5060",
            displayName = "Echo Service",
            durationSeconds = 75L,
            direction = CallDirection.OUTGOING,
            hasVideo = true
        )
        assertEquals("Echo Service", item1.displayTitle)
        assertEquals("01:15", item1.formattedDuration)
        assertTrue(item1.hasVideo)
        assertEquals(CallDirection.OUTGOING, item1.direction)

        val item2 = CallHistoryItem(
            remoteUri = "sip:1002@10.0.2.2",
            displayName = null,
            durationSeconds = 9L,
            direction = CallDirection.INCOMING,
            hasVideo = false
        )
        assertEquals("1002", item2.displayTitle)
        assertEquals("00:09", item2.formattedDuration)
        assertFalse(item2.hasVideo)
        assertEquals(CallDirection.INCOMING, item2.direction)

        val item3 = CallHistoryItem(
            remoteUri = "+1234567890",
            displayName = "",
            durationSeconds = 0L,
            direction = CallDirection.MISSED,
            hasVideo = true
        )
        assertEquals("+1234567890", item3.displayTitle)
        assertEquals("00:00", item3.formattedDuration)
        assertEquals(CallDirection.MISSED, item3.direction)
    }

    @Test
    fun testCallHistoryItemJsonRoundTrip() {
        val item = CallHistoryItem(
            id = "custom-id-456",
            remoteUri = "sip:alice@sip.iptel.org",
            displayName = "Alice Wonderland",
            timestamp = 1700000000000L,
            durationSeconds = 142L,
            direction = CallDirection.MISSED,
            hasVideo = true
        )
        val json = item.toJson()
        val restored = CallHistoryItem.fromJson(json)

        assertEquals("custom-id-456", restored.id)
        assertEquals("sip:alice@sip.iptel.org", restored.remoteUri)
        assertEquals("Alice Wonderland", restored.displayName)
        assertEquals(1700000000000L, restored.timestamp)
        assertEquals(142L, restored.durationSeconds)
        assertEquals("02:22", restored.formattedDuration)
        assertEquals(CallDirection.MISSED, restored.direction)
        assertTrue(restored.hasVideo)
    }
}
