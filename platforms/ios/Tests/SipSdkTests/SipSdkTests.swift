//
//  SipSdkTests.swift
//  SipSdkTests
//

import XCTest
import CallKit
import AVFoundation
@testable import SipSdk

final class SipSdkTests: XCTestCase {

    @MainActor
    func testClientInstantiationAndDefaults() {
        let client = SipClient()
        XCTAssertFalse(client.isStarted)
        XCTAssertEqual(client.registrationState, .unregistered)
        XCTAssertNil(client.currentAccount)
        XCTAssertTrue(client.calls.isEmpty)
        XCTAssertNil(client.activeCall)
        XCTAssertFalse(client.isMicrophoneMuted)
        XCTAssertEqual(client.cameraPosition, .front)
        XCTAssertTrue(client.isLocalVideoEnabled)
        XCTAssertNotNil(client.callKit)
        XCTAssertNotNil(client.captureDriver)
        XCTAssertNotNil(client.remoteRendererDriver)
        XCTAssertNotNil(client.localRendererDriver)
    }

    func testAccountAorFormatting() {
        let account = SipAccount(username: "1001", domain: "127.0.0.1", transport: .udp, port: 5060)
        XCTAssertEqual(account.aor, "sip:1001@127.0.0.1:5060")

        let tcpAccount = SipAccount(username: "1002", domain: "sip.example.com", transport: .tcp, port: 5061)
        XCTAssertEqual(tcpAccount.aor, "sip:1002@sip.example.com:5061;transport=tcp")

        let wsAccount = SipAccount(username: "1002", domain: "sip.example.com", transport: .ws, port: 8088)
        XCTAssertEqual(wsAccount.aor, "sip:1002@sip.example.com:8088;transport=ws")

        let wssAccount = SipAccount(username: "1002", domain: "sip.example.com", transport: .wss, port: 8089)
        XCTAssertEqual(wssAccount.aor, "sip:1002@sip.example.com:8089;transport=wss")

        let fullAccount = SipAccount(
            username: "alice",
            domain: "sip.provider.com",
            password: "secret",
            transport: .udp,
            port: 5060,
            displayName: "Alice",
            outboundProxy: "sip:proxy.provider.com:5060",
            stunServer: "stun:stun.l.google.com:19302",
            mediaNat: "ice",
            regInterval: 1800
        )
        XCTAssertEqual(fullAccount.displayName, "Alice")
        XCTAssertEqual(fullAccount.outboundProxy, "sip:proxy.provider.com:5060")
        XCTAssertEqual(fullAccount.mediaNat, "ice")
        XCTAssertEqual(fullAccount.regInterval, 1800)
    }

    @MainActor
    func testVideoViewRenderingAndClear() {
        let videoView = SipVideoUIView(frame: CGRect(x: 0, y: 0, width: 640, height: 480))
        XCTAssertEqual(videoView.renderedFrameCount, 0)
        XCTAssertEqual(videoView.videoSize, .zero)

        // Render test pattern
        videoView.renderTestPattern(width: 640, height: 480)

        let exp = expectation(description: "Frame rendered on main queue")
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) {
            XCTAssertGreaterThanOrEqual(videoView.renderedFrameCount, 1)
            XCTAssertEqual(videoView.videoSize, CGSize(width: 640, height: 480))
            exp.fulfill()
        }
        wait(for: [exp], timeout: 1.0)

        // Clear video
        videoView.clearVideo()
        let clearExp = expectation(description: "Video cleared")
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) {
            XCTAssertEqual(videoView.videoSize, .zero)
            clearExp.fulfill()
        }
        wait(for: [clearExp], timeout: 1.0)
    }

    @MainActor
    func testVideoCaptureDriverSyntheticDelivery() {
        let captureDriver = IosVideoCaptureDriver()
        XCTAssertFalse(captureDriver.isRunning)
        XCTAssertTrue(captureDriver.isEnabled)

        final class TestContext {
            var deliveredCount = 0
            var exp: XCTestExpectation?
        }
        let ctx = TestContext()
        let exp = expectation(description: "Synthetic frame received")
        ctx.exp = exp

        captureDriver.setFrameHandler({ framePtr, arg in
            guard let arg = arg, let frame = framePtr?.pointee else { return }
            let ctx = Unmanaged<TestContext>.fromOpaque(arg).takeUnretainedValue()
            if frame.width == 640 && frame.height == 480 {
                ctx.deliveredCount += 1
                if ctx.deliveredCount == 1 {
                    DispatchQueue.main.async {
                        ctx.exp?.fulfill()
                    }
                }
            }
        }, arg: Unmanaged.passUnretained(ctx).toOpaque())

        let rc = captureDriver.start(width: 640, height: 480, fps: 30)
        XCTAssertEqual(rc, 0)
        XCTAssertTrue(captureDriver.isRunning)

        wait(for: [exp], timeout: 2.0)

        captureDriver.stop()
        XCTAssertFalse(captureDriver.isRunning)
    }

    @MainActor
    func testAudioSessionManager() {
        let audioMgr = IosAudioManager.shared
        XCTAssertFalse(audioMgr.isSpeakerEnabled)

        audioMgr.setSpeakerphone(true)
        XCTAssertTrue(audioMgr.isSpeakerEnabled)

        audioMgr.setSpeakerphone(false)
        XCTAssertFalse(audioMgr.isSpeakerEnabled)

        let ok = audioMgr.prepareCallAudioSession()
        XCTAssertTrue(ok)
        audioMgr.releaseCallAudioSessionIfIdle()
    }

    @MainActor
    func testCallKitCoordinatorConfiguration() {
        let client = SipClient()
        let coordinator = SipCallKitCoordinator(localizedName: "TestCoordinator", client: client)

        XCTAssertNotNil(coordinator.provider)
        XCTAssertNotNil(coordinator.callController)
        XCTAssertTrue(coordinator.provider.configuration.supportsVideo)
        XCTAssertEqual(coordinator.provider.configuration.maximumCallsPerCallGroup, 1)

        let fakeCall = SipCall(
            coreCall: nil,
            remoteUri: "sip:1002@127.0.0.1",
            peerDisplayName: "Bob",
            isOutgoing: false,
            hasVideo: true,
            initialState: .incoming
        )

        XCTAssertEqual(fakeCall.remoteUri, "sip:1002@127.0.0.1")
        XCTAssertEqual(fakeCall.peerDisplayName, "Bob")
        XCTAssertFalse(fakeCall.isOutgoing)
        XCTAssertTrue(fakeCall.hasVideo)
        XCTAssertEqual(fakeCall.state, .incoming)
    }

    func testPushRegistryPayloadParsing() {
        let payloadDict: [AnyHashable: Any] = [
            "caller_uri": "sip:alice@domain.com",
            "caller_name": "Alice Smith",
            "has_video": true,
            "call_id": "4B5B4958-3843-4560-A6E6-34CDCE432658"
        ]

        let parsed = SipIncomingPushPayload(
            callerUri: payloadDict["caller_uri"] as! String,
            callerName: payloadDict["caller_name"] as? String,
            hasVideo: payloadDict["has_video"] as! Bool,
            uuid: UUID(uuidString: payloadDict["call_id"] as! String)!,
            rawPayload: payloadDict
        )

        XCTAssertEqual(parsed.callerUri, "sip:alice@domain.com")
        XCTAssertEqual(parsed.callerName, "Alice Smith")
        XCTAssertTrue(parsed.hasVideo)
        XCTAssertEqual(parsed.uuid, UUID(uuidString: "4B5B4958-3843-4560-A6E6-34CDCE432658"))
    }

    @MainActor
    func testEngineLifecycleAndLocalPreview() async throws {
        let client = SipClient()

        let localView = SipVideoUIView(frame: CGRect(x: 0, y: 0, width: 320, height: 240))
        let remoteView = SipVideoUIView(frame: CGRect(x: 0, y: 0, width: 640, height: 480))
        client.localVideoView = localView
        client.remoteVideoView = remoteView

        try await client.start()
        XCTAssertTrue(client.isStarted)

        // Test camera controls
        XCTAssertEqual(client.cameraPosition, .front)
        client.switchCamera()
        XCTAssertEqual(client.cameraPosition, .back)
        client.switchCamera()
        XCTAssertEqual(client.cameraPosition, .front)

        // Test microphone mute toggle
        XCTAssertFalse(client.isMicrophoneMuted)
        client.isMicrophoneMuted = true
        XCTAssertTrue(client.isMicrophoneMuted)
        client.isMicrophoneMuted = false
        XCTAssertFalse(client.isMicrophoneMuted)

        // Test local video stream toggle
        XCTAssertTrue(client.isLocalVideoEnabled)
        client.isLocalVideoEnabled = false
        XCTAssertFalse(client.isLocalVideoEnabled)
        client.isLocalVideoEnabled = true
        XCTAssertTrue(client.isLocalVideoEnabled)

        // Test standalone local video preview
        XCTAssertFalse(client.isLocalVideoPreviewActive)
        client.startLocalVideoPreview()
        XCTAssertTrue(client.isLocalVideoPreviewActive)

        // Allow preview frames to flow into localVideoView
        try await Task.sleep(nanoseconds: 300_000_000)
        XCTAssertGreaterThan(localView.renderedFrameCount, 0)

        client.stopLocalVideoPreview()
        XCTAssertFalse(client.isLocalVideoPreviewActive)

        // Test network handover transport reset
        client.resetTransports(reRegister: false)

        // Test sendMessage argument validation while started
        XCTAssertThrowsError(try client.sendMessage(to: "sip:1002@127.0.0.1", text: "")) { error in
            guard case SipError.invalidParameter = error else {
                XCTFail("Expected invalidParameter for empty text, got \(error)")
                return
            }
        }
        XCTAssertThrowsError(try client.sendMessage(to: "", text: "hi")) { error in
            guard case SipError.invalidParameter = error else {
                XCTFail("Expected invalidParameter for empty target, got \(error)")
                return
            }
        }
        XCTAssertThrowsError(try client.sendMessage(to: "sip:1002@127.0.0.1", body: Data("x".utf8), contentType: "badtype")) { error in
            guard case SipError.messageSendFailed = error else {
                XCTFail("Expected messageSendFailed for invalid content type, got \(error)")
                return
            }
        }

        await client.stop()
        XCTAssertFalse(client.isStarted)
    }

    @MainActor
    func testAsteriskPbxRegistrationAndCall() async throws {
        // Only run against local PBX if Asterisk is reachable on port 5060
        let client = SipClient()
        try await client.start()

        let account = SipAccount(
            username: "1001",
            domain: "127.0.0.1",
            password: "test1001",
            transport: .udp,
            port: 5060,
            displayName: "iOS Test Client"
        )

        try await client.register(account)

        // Wait for registration
        var regCount = 0
        while !client.registrationState.isRegistered && regCount < 30 {
            try await Task.sleep(nanoseconds: 100_000_000)
            regCount += 1
        }

        if client.registrationState.isRegistered {
            print("Successfully registered 1001 against local Asterisk PBX")
            XCTAssertTrue(client.registrationState.isRegistered)

            // Test network handover re-registration
            client.resetTransports(reRegister: true)
            try await Task.sleep(nanoseconds: 500_000_000)

            // Place audio/video call to Asterisk Echo (9001)
            let call = try await client.makeCall(to: "sip:9001@127.0.0.1:5060", withVideo: true)
            XCTAssertEqual(call.remoteUri, "sip:9001@127.0.0.1:5060")
            XCTAssertTrue(call.isOutgoing)

            // Wait for call to connect
            var waitCallCount = 0
            while !call.state.isConnected && !call.state.isClosed && waitCallCount < 40 {
                try await Task.sleep(nanoseconds: 100_000_000)
                waitCallCount += 1
            }

            if call.state == .established {
                print("Call established with 9001 video echo")
                XCTAssertEqual(call.state, .established)

                // Test DTMF
                try call.sendDTMF("123#")

                // Test request keyframe
                call.requestKeyframe()

                // Test mute
                call.setMuted(true)
                XCTAssertTrue(call.isMuted)
                call.setMuted(false)
                XCTAssertFalse(call.isMuted)

                // Hold call
                try await call.setHold(true)
                try await Task.sleep(nanoseconds: 200_000_000)
                try await call.setHold(false)
                try await Task.sleep(nanoseconds: 200_000_000)

                await call.hangup()
                try await Task.sleep(nanoseconds: 300_000_000)
            }

            // SIP MESSAGE round-trip (1001 -> 1001) and 404 negative case
            var receivedMessages: [SipIncomingMessage] = []
            var messageStatuses: [(id: UInt64, status: SipMessageStatus, code: Int, reason: String)] = []
            let eventTask = Task { @MainActor in
                for await event in client.events {
                    switch event {
                    case .messageReceived(let msg):
                        receivedMessages.append(msg)
                    case .messageStatusChanged(let id, let status, let code, let reason):
                        messageStatuses.append((id, status, code, reason))
                    default:
                        break
                    }
                }
            }

            let text = "hello from iOS"
            let msgId = try client.sendMessage(to: "1001", text: text)
            XCTAssertGreaterThan(msgId, 0)

            var msgWait = 0
            while (receivedMessages.isEmpty || !messageStatuses.contains(where: { $0.id == msgId })) && msgWait < 50 {
                try await Task.sleep(nanoseconds: 100_000_000)
                msgWait += 1
            }

            let sentStatus = messageStatuses.first(where: { $0.id == msgId })
            XCTAssertNotNil(sentStatus, "Expected messageStatusChanged for sent message")
            XCTAssertEqual(sentStatus?.status, .sent)
            XCTAssertTrue((200..<300).contains(sentStatus?.code ?? 0))

            let incoming = receivedMessages.first(where: { $0.text == text })
            XCTAssertNotNil(incoming, "Expected echoed SIP MESSAGE")
            XCTAssertTrue(incoming?.from.hasPrefix("sip:1001@") ?? false)
            XCTAssertEqual(incoming?.contentType, "text/plain")

            let unknownId = try client.sendMessage(to: "sip:9999@127.0.0.1:5060", text: "unknown")
            var failWait = 0
            while !messageStatuses.contains(where: { $0.id == unknownId }) && failWait < 50 {
                try await Task.sleep(nanoseconds: 100_000_000)
                failWait += 1
            }
            let failedStatus = messageStatuses.first(where: { $0.id == unknownId })
            XCTAssertEqual(failedStatus?.status, .failed)
            XCTAssertEqual(failedStatus?.code, 404)

            eventTask.cancel()

            await client.unregister()
            try await Task.sleep(nanoseconds: 200_000_000)
        } else {
            print("Asterisk PBX not responding on 127.0.0.1:5060; skipping live call portion")
        }

        await client.stop()
        try await Task.sleep(nanoseconds: 200_000_000)
    }

    @MainActor
    func testSipMessageTypesAndValidation() {
        XCTAssertEqual(SipMessageStatus(rawValue: 0), .sent)
        XCTAssertEqual(SipMessageStatus(rawValue: 1), .failed)
        XCTAssertEqual(SipMessageStatus(rawValue: 2), .delivered)
        XCTAssertEqual(SipMessageStatus(rawValue: 3), .displayed)

        let textMsg = SipIncomingMessage(from: "sip:1002@pbx", contentType: "text/plain", body: Data("hello".utf8))
        XCTAssertEqual(textMsg.text, "hello")
        let upperTextMsg = SipIncomingMessage(from: "sip:1002@pbx", contentType: "TEXT/HTML", body: Data("<p>hi</p>".utf8))
        XCTAssertEqual(upperTextMsg.text, "<p>hi</p>")
        let jsonMsg = SipIncomingMessage(from: "sip:1002@pbx", contentType: "application/json", body: Data("{}".utf8))
        XCTAssertNil(jsonMsg.text)

        let client = SipClient()
        XCTAssertThrowsError(try client.sendMessage(to: "sip:1002@127.0.0.1", text: "hi")) { error in
            guard case SipError.engineNotStarted = error else {
                XCTFail("Expected engineNotStarted, got \(error)")
                return
            }
        }
    }

    @MainActor
    func testTlsSrtpOpusTelemetryAndCallTransfer() async throws {
        // 1. Verify SipAccount TLS & SRTP properties and AOR
        let tlsAccount = SipAccount(
            username: "1004",
            domain: "127.0.0.1",
            password: "test1004",
            transport: .tls,
            port: 5061,
            tlsCaFile: "/nonexistent/ca.pem",
            verifyTlsCert: true,
            mediaEnc: "srtp-mand"
        )
        XCTAssertEqual(tlsAccount.aor, "sip:1004@127.0.0.1:5061;transport=tls")
        XCTAssertEqual(tlsAccount.tlsCaFile, "/nonexistent/ca.pem")
        XCTAssertTrue(tlsAccount.verifyTlsCert)
        XCTAssertEqual(tlsAccount.mediaEnc, "srtp-mand")

        // 2. Verify CallStats & Transfer error handling on unattached SipCall
        let dummyCall = SipCall(
            coreCall: nil,
            remoteUri: "sip:1002@127.0.0.1",
            isOutgoing: true
        )
        XCTAssertFalse(dummyCall.isSecure)
        XCTAssertNil(dummyCall.getStats())
        XCTAssertThrowsError(try dummyCall.transfer(to: "sip:1003@127.0.0.1"))
        XCTAssertThrowsError(try dummyCall.attendedTransfer(with: dummyCall))
        XCTAssertThrowsError(try dummyCall.acceptTransfer(false))

        // 3. Verify TLS certificate validation failure when verifyTlsCert=true and CA file is invalid
        let client = SipClient()
        try await client.start()
        try await client.register(tlsAccount)

        var waitFail = 0
        while waitFail < 20 {
            if case .failed = client.registrationState {
                break
            }
            try await Task.sleep(nanoseconds: 100_000_000)
            waitFail += 1
        }
        if case .failed = client.registrationState {
            // Expected failure for missing CA certificate when verifyTlsCert is true
        } else {
            XCTFail("Expected registrationState to fail with missing CA certificate, got \(client.registrationState)")
        }

        // 4. Live TLS + SRTP + Opus + Telemetry test against local Asterisk (if reachable)
        let validTlsAccount = SipAccount(
            username: "1004",
            domain: "127.0.0.1",
            password: "test1004",
            transport: .tls,
            port: 5061,
            verifyTlsCert: false,
            mediaEnc: "srtp-mand"
        )
        try await client.register(validTlsAccount)

        var regWait = 0
        while !client.registrationState.isRegistered && regWait < 30 {
            try await Task.sleep(nanoseconds: 100_000_000)
            regWait += 1
        }

        if client.registrationState.isRegistered {
            var statsEvents: [SipCallStats] = []
            let statsTask = Task { @MainActor in
                for await event in client.events {
                    if case .callStatsUpdated(_, let st) = event {
                        statsEvents.append(st)
                    }
                }
            }

            let tlsCall = try await client.makeCall(to: "sip:9001@127.0.0.1:5061;transport=tls", withVideo: false)
            var callWait = 0
            while !tlsCall.state.isConnected && !tlsCall.state.isClosed && callWait < 40 {
                try await Task.sleep(nanoseconds: 100_000_000)
                callWait += 1
            }

            if tlsCall.state == .established {
                XCTAssertTrue(tlsCall.isSecure, "TLS/SRTP call must report isSecure == true")
                try await Task.sleep(nanoseconds: 2_500_000_000)
                let pulled = tlsCall.getStats()
                XCTAssertNotNil(pulled)
                XCTAssertEqual(pulled?.audioCodec, "opus")
                XCTAssertGreaterThan(pulled?.audioBitrateKbps ?? 0, 0)
                XCTAssertFalse(statsEvents.isEmpty, "Expected periodic callStatsUpdated event")
                await tlsCall.hangup()
                try await Task.sleep(nanoseconds: 300_000_000)
            }
            statsTask.cancel()
            await client.unregister()
        }

        await client.stop()
    }

    @MainActor
    func testMultiCallConfigurationCallKitAndPushParsing() {
        let defaultAccount = SipAccount(username: "1001", domain: "127.0.0.1")
        XCTAssertEqual(defaultAccount.maxCalls, 4)

        let cappedAccount = SipAccount(username: "1001", domain: "127.0.0.1", maxCalls: 2)
        XCTAssertEqual(cappedAccount.maxCalls, 2)

        let coordinator = SipCallKitCoordinator(localizedName: "SipSdkTest", client: nil, maxCalls: 3)
        XCTAssertEqual(coordinator.provider.configuration.maximumCallGroups, 3)
        XCTAssertEqual(coordinator.provider.configuration.maximumCallsPerCallGroup, 1)

        let unified = SipIncomingPushPayload(dictionary: [
            "sip_call_id": "abc-123@pbx",
            "from_uri": "sip:alice@pbx.local",
            "display_name": "Alice",
            "has_video": true
        ])
        XCTAssertEqual(unified.sipCallId, "abc-123@pbx")
        XCTAssertEqual(unified.callerUri, "sip:alice@pbx.local")
        XCTAssertEqual(unified.callerName, "Alice")
        XCTAssertTrue(unified.hasVideo)

        let legacyUuid = "550e8400-e29b-41d4-a716-446655440000"
        let alias = SipIncomingPushPayload(dictionary: [
            "call_id": legacyUuid,
            "caller_uri": "sip:bob@pbx.local",
            "caller_name": "Bob",
            "video": false
        ])
        let clientForPush = SipClient()
        let pushUuid = UUID(uuidString: legacyUuid)!
        clientForPush.registerPendingPushCall(sipCallId: "abc-123@pbx", uuid: pushUuid)

        XCTAssertEqual(alias.sipCallId, legacyUuid)
        XCTAssertEqual(alias.uuid.uuidString.lowercased(), legacyUuid)
        XCTAssertEqual(alias.callerUri, "sip:bob@pbx.local")
        XCTAssertEqual(alias.callerName, "Bob")
        XCTAssertFalse(alias.hasVideo)
    }

    @MainActor
    func testMultiCallLiveSwapAndBusyCap() async throws {
        let client = SipClient()
        try await client.start()

        let multiAccount = SipAccount(
            username: "1001",
            domain: "127.0.0.1",
            password: "test1001",
            transport: .udp,
            port: 5060,
            maxCalls: 2
        )
        try await client.register(multiAccount)

        var regWait = 0
        while !client.registrationState.isRegistered && regWait < 30 {
            try await Task.sleep(nanoseconds: 100_000_000)
            regWait += 1
        }
        guard client.registrationState.isRegistered else {
            await client.stop()
            throw XCTSkip("Asterisk PBX not reachable on 127.0.0.1:5060")
        }

        var busyCallerUri: String?
        client.onCallRejectedBusy = { uri in
            busyCallerUri = uri
        }

        // 1. Two concurrent calls + auto-hold + swap + CallKit swap
        let call1 = try await client.makeCall(to: "9001", withVideo: false)
        var w1 = 0
        while call1.state != .established && !call1.state.isClosed && w1 < 50 {
            try await Task.sleep(nanoseconds: 100_000_000)
            w1 += 1
        }
        XCTAssertEqual(call1.state, .established)

        let call2 = try await client.makeCall(to: "9001", withVideo: false)
        var w2 = 0
        while (call2.state != .established || !call1.isOnHold) && w2 < 50 {
            try await Task.sleep(nanoseconds: 100_000_000)
            w2 += 1
        }
        XCTAssertTrue(call1.isOnHold, "call1 must auto-hold when dialing call2")
        XCTAssertEqual(call2.state, .established)
        XCTAssertFalse(call2.isOnHold)

        try await call1.setHold(false)
        var wSwap = 0
        while (call1.isOnHold || !call2.isOnHold) && wSwap < 40 {
            try await Task.sleep(nanoseconds: 100_000_000)
            wSwap += 1
        }
        XCTAssertFalse(call1.isOnHold, "call1 should resume on swap")
        XCTAssertTrue(call2.isOnHold, "call2 should auto-hold on swap")

        // Invoke CallKit hold coordinator handler to unhold call2 and swap back
        let handled = try await client.callKit.applyHoldAction(callUUID: call2.id, onHold: false)
        XCTAssertTrue(handled)
        var wSwapBack = 0
        while (call2.isOnHold || !call1.isOnHold) && wSwapBack < 40 {
            try await Task.sleep(nanoseconds: 100_000_000)
            wSwapBack += 1
        }
        XCTAssertFalse(call2.isOnHold, "call2 should resume via CallKit hold handler")
        XCTAssertTrue(call1.isOnHold, "call1 should auto-hold via CallKit swap")

        // 2. Enforce maxCalls = 2: outgoing throws SipError.busy
        do {
            _ = try await client.makeCall(to: "9001", withVideo: false)
            XCTFail("Expected SipError.busy when exceeding maxCalls=2")
        } catch SipError.busy {
            // Expected
        }

        // 3. Free one slot while keeping call1 open (CoreAudio stays warm), trigger 9003 callback,
        //    close the trigger quickly, and refill the 2nd slot before Asterisk originates at ~3s.
        await call2.hangup()
        var wCall2Close = 0
        while !call2.state.isClosed && wCall2Close < 40 {
            try await Task.sleep(nanoseconds: 50_000_000)
            wCall2Close += 1
        }
        XCTAssertTrue(call2.state.isClosed)

        let cbTrigger = try await client.makeCall(to: "9003", withVideo: false)
        var cbWait = 0
        while cbTrigger.state != .established && !cbTrigger.state.isClosed && cbWait < 40 {
            try await Task.sleep(nanoseconds: 50_000_000)
            cbWait += 1
        }
        await cbTrigger.hangup()
        var cbCloseWait = 0
        while !cbTrigger.state.isClosed && cbCloseWait < 40 {
            try await Task.sleep(nanoseconds: 50_000_000)
            cbCloseWait += 1
        }
        XCTAssertTrue(cbTrigger.state.isClosed, "9003 trigger should close cleanly")

        let call3 = try await client.makeCall(to: "9001", withVideo: false)
        var wCall3 = 0
        while call3.state != .established && !call3.state.isClosed && wCall3 < 50 {
            try await Task.sleep(nanoseconds: 50_000_000)
            wCall3 += 1
        }
        XCTAssertEqual(call3.state, .established)

        var wBusy = 0
        while busyCallerUri == nil && wBusy < 80 {
            try await Task.sleep(nanoseconds: 100_000_000)
            wBusy += 1
        }
        XCTAssertNotNil(busyCallerUri, "Expected onCallRejectedBusy when 9003 callback arrives at maxCalls=2")
        XCTAssertFalse(call1.state.isClosed, "call1 must remain connected after 486 rejection")
        XCTAssertFalse(call3.state.isClosed, "call3 must remain connected after 486 rejection")

        await call1.hangup()
        await call3.hangup()
        try await Task.sleep(nanoseconds: 300_000_000)
        await client.stop()
    }
}
