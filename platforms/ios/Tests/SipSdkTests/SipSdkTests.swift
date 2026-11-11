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

        fullAccount.withCAccountConfig { cConfig in
            XCTAssertEqual(String(cString: cConfig.user), "alice")
            XCTAssertEqual(String(cString: cConfig.domain), "sip.provider.com")
            XCTAssertEqual(String(cString: cConfig.pwd), "secret")
            XCTAssertEqual(String(cString: cConfig.transport), "udp")
            XCTAssertEqual(cConfig.port, 5060)
            XCTAssertEqual(String(cString: cConfig.display_name), "Alice")
            XCTAssertEqual(String(cString: cConfig.outbound_proxy), "sip:proxy.provider.com:5060")
            XCTAssertEqual(String(cString: cConfig.stun_server), "stun:stun.l.google.com:19302")
            XCTAssertNil(cConfig.stun_user)
            XCTAssertNil(cConfig.stun_pass)
            XCTAssertEqual(String(cString: cConfig.media_nat), "ice")
            XCTAssertEqual(cConfig.reg_interval, 1800)
        }
    }

    @MainActor
    func testRegisterWithOptionalAccountFieldsOverLoopback() async throws {
        let peer = LoopbackPeer(port: 15091)
        peer.start()
        defer { peer.stop() }

        var boundWait = 0
        while !peer.isBound && boundWait < 20 {
            try await Task.sleep(nanoseconds: 50_000_000)
            boundWait += 1
        }
        XCTAssertTrue(peer.isBound)

        let client = SipClient()
        try await client.start()

        let account = SipAccount(
            username: "alice",
            domain: "sip.example.invalid",
            password: "secret",
            transport: .udp,
            port: 15091,
            displayName: "Alice Wonderland",
            outboundProxy: "sip:127.0.0.1:15091",
            stunServer: "stun:127.0.0.1:3478",
            stunUser: "stunuser",
            stunPassword: "stunpass",
            mediaNat: "stun",
            regInterval: 600
        )

        try await client.register(account)

        var waitReg = 0
        while peer.lastRegisterRequest.isEmpty && waitReg < 30 {
            try await Task.sleep(nanoseconds: 100_000_000)
            waitReg += 1
        }

        XCTAssertTrue(peer.lastRegisterRequest.contains("REGISTER sip:sip.example.invalid:15091 SIP/2.0"))
        XCTAssertTrue(peer.lastRegisterRequest.contains("\"Alice Wonderland\""))

        await client.stop()
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

        // stop() waits for the in-flight frame, so no frame arrives after it returns.
        let countAtStop = ctx.deliveredCount
        Thread.sleep(forTimeInterval: 0.2)
        XCTAssertEqual(ctx.deliveredCount, countAtStop)
    }

    // MARK: - Callback lifetimes (RES-4)

    /// Blocks the main thread so events the core worker posts stay queued on the main queue.
    private func blockMainQueue(seconds: TimeInterval) {
        Thread.sleep(forTimeInterval: seconds)
    }

    @MainActor
    private func startRegisteredClient(peer: LoopbackPeer) async throws -> SipClient {
        peer.start()
        var boundWait = 0
        while !peer.isBound && boundWait < 20 {
            try await Task.sleep(nanoseconds: 50_000_000)
            boundWait += 1
        }
        XCTAssertTrue(peer.isBound)

        let client = SipClient()
        try await client.start()
        try await client.register(SipAccount(username: "bob", domain: "127.0.0.1", transport: .udp, port: Int(peer.port)))

        var regWait = 0
        while !client.registrationState.isRegistered && regWait < 50 {
            try await Task.sleep(nanoseconds: 100_000_000)
            regWait += 1
        }
        XCTAssertTrue(client.registrationState.isRegistered)
        return client
    }

    @MainActor
    func testIncomingCallIsTrackedWhileRunning() async throws {
        let peer = LoopbackPeer(port: 15093)
        defer { peer.stop() }
        let client = try await startRegisteredClient(peer: peer)

        peer.sendInvite()

        var wait = 0
        while client.calls.isEmpty && wait < 50 {
            try await Task.sleep(nanoseconds: 100_000_000)
            wait += 1
        }
        XCTAssertEqual(client.calls.count, 1)
        XCTAssertEqual(client.calls.first?.state, .incoming)
        XCTAssertEqual(client.calls.first?.isOutgoing, false)

        await client.stop()
    }

    @MainActor
    func testOutgoingCallStateUpdatesTheSameSipCall() async throws {
        let peer = LoopbackPeer(port: 15094)
        defer { peer.stop() }
        let client = try await startRegisteredClient(peer: peer)

        let call = try await client.makeCall(to: "sip:echo@127.0.0.1:15094", withVideo: true)

        var wait = 0
        while call.state != .established && !call.state.isClosed && wait < 50 {
            try await Task.sleep(nanoseconds: 100_000_000)
            wait += 1
        }
        XCTAssertEqual(call.state, .established)
        XCTAssertEqual(client.calls.count, 1)
        XCTAssertTrue(client.calls.first === call)

        await call.hangup()
        wait = 0
        while !call.state.isClosed && wait < 50 {
            try await Task.sleep(nanoseconds: 100_000_000)
            wait += 1
        }
        XCTAssertTrue(call.state.isClosed)
        XCTAssertTrue(client.calls.isEmpty)

        await client.stop()
    }

    // Given two overlapping outgoing calls,
    // when both calls end,
    // then the shared audio manager holds no more active calls than before the calls.
    @MainActor
    func testAudioSessionIsReleasedAfterOverlappingCallsEnd() async throws {
        let peer = LoopbackPeer(port: 15096)
        defer { peer.stop() }
        let client = try await startRegisteredClient(peer: peer)
        let audio = IosAudioManager.shared
        let countBefore = audio.activeCallCount

        // The loopback peer always answers with audio and video, so offer both.
        let first = try await client.makeCall(to: "sip:echo@127.0.0.1:15096", withVideo: true)
        let second = try await client.makeCall(to: "sip:echo2@127.0.0.1:15096", withVideo: true)

        var wait = 0
        while (first.state != .established || second.state != .established) && wait < 50 {
            try await Task.sleep(nanoseconds: 100_000_000)
            wait += 1
        }
        XCTAssertEqual(first.state, .established)
        XCTAssertEqual(second.state, .established)

        await first.hangup()
        await second.hangup()
        wait = 0
        while (!first.state.isClosed || !second.state.isClosed || audio.activeCallCount != countBefore) && wait < 50 {
            try await Task.sleep(nanoseconds: 100_000_000)
            wait += 1
        }
        XCTAssertTrue(first.state.isClosed)
        XCTAssertTrue(second.state.isClosed)
        XCTAssertEqual(audio.activeCallCount, countBefore, "audio session still counts a call after both calls ended")

        await client.stop()
    }

    @MainActor
    func testEventsQueuedBeforeStopAreDroppedAfterStop() async throws {
        let peer = LoopbackPeer(port: 15092)
        defer { peer.stop() }
        let client = try await startRegisteredClient(peer: peer)

        peer.sendInvite()
        blockMainQueue(seconds: 1.0)
        // The client answered the INVITE, so its incoming call event is queued.
        XCTAssertTrue(peer.lastRequest.hasPrefix("SIP/2.0 1"), "INVITE not handled: \(peer.lastRequest.prefix(40))")

        await client.stop()
        try await Task.sleep(nanoseconds: 300_000_000)

        XCTAssertTrue(client.calls.isEmpty)
        XCTAssertEqual(client.registrationState, .unregistered)
    }

    @MainActor
    func testEventsQueuedBeforeClientIsFreedAreDropped() async throws {
        let peer = LoopbackPeer(port: 15095)
        defer { peer.stop() }
        var client: SipClient? = try await startRegisteredClient(peer: peer)
        weak var weakClient = client
        _ = weakClient

        peer.sendInvite()
        blockMainQueue(seconds: 1.0)
        XCTAssertTrue(peer.lastRequest.hasPrefix("SIP/2.0 1"), "INVITE not handled: \(peer.lastRequest.prefix(40))")

        // Free the client without stop(); deinit destroys the core client.
        client = nil
        XCTAssertNil(weakClient)

        // Run the queued events. Under --asan a freed-client access fails here.
        try await Task.sleep(nanoseconds: 300_000_000)
    }

    func testVideoCaptureDriverStopWaitsForInFlightFrame() {
        let captureDriver = IosVideoCaptureDriver()

        final class BlockingContext {
            let entered = DispatchSemaphore(value: 0)
            let release = DispatchSemaphore(value: 0)
            let countLock = NSLock()
            var calls = 0
        }
        let ctx = BlockingContext()

        captureDriver.setFrameHandler({ _, arg in
            guard let arg = arg else { return }
            let ctx = Unmanaged<BlockingContext>.fromOpaque(arg).takeUnretainedValue()
            ctx.countLock.lock()
            ctx.calls += 1
            let isFirst = ctx.calls == 1
            ctx.countLock.unlock()
            if isFirst {
                ctx.entered.signal()
                ctx.release.wait()
            }
        }, arg: Unmanaged.passUnretained(ctx).toOpaque())

        XCTAssertEqual(captureDriver.start(width: 64, height: 48, fps: 30), 0)
        XCTAssertEqual(ctx.entered.wait(timeout: .now() + 2), .success)

        let stopped = DispatchSemaphore(value: 0)
        DispatchQueue.global().async {
            captureDriver.stop()
            stopped.signal()
        }
        let stoppedEarly = stopped.wait(timeout: .now() + 0.3) == .success
        XCTAssertFalse(stoppedEarly, "stop() returned while a frame callback was still running")
        ctx.release.signal()
        if !stoppedEarly {
            XCTAssertEqual(stopped.wait(timeout: .now() + 2), .success)
        }

        ctx.countLock.lock()
        let callsAtStop = ctx.calls
        ctx.countLock.unlock()
        Thread.sleep(forTimeInterval: 0.2)
        ctx.countLock.lock()
        XCTAssertEqual(ctx.calls, callsAtStop, "frame callback ran after stop() returned")
        ctx.countLock.unlock()

        withExtendedLifetime(ctx) {}
    }

    func testVideoCaptureDriverDeliversNoFramesWhileDisabled() {
        let captureDriver = IosVideoCaptureDriver()

        final class CountingContext {
            let countLock = NSLock()
            var calls = 0
        }
        let ctx = CountingContext()

        captureDriver.setFrameHandler({ _, arg in
            guard let arg = arg else { return }
            let ctx = Unmanaged<CountingContext>.fromOpaque(arg).takeUnretainedValue()
            ctx.countLock.lock()
            ctx.calls += 1
            ctx.countLock.unlock()
        }, arg: Unmanaged.passUnretained(ctx).toOpaque())

        captureDriver.setEnabled(false)
        XCTAssertEqual(captureDriver.start(width: 64, height: 48, fps: 30), 0)
        Thread.sleep(forTimeInterval: 0.3)
        captureDriver.stop()

        ctx.countLock.lock()
        XCTAssertEqual(ctx.calls, 0, "frames reached the handler while capture was disabled")
        ctx.countLock.unlock()

        withExtendedLifetime(ctx) {}
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

            await client.unregister()
            try await Task.sleep(nanoseconds: 200_000_000)
        } else {
            print("Asterisk PBX not responding on 127.0.0.1:5060; skipping live call portion")
        }

        await client.stop()
        try await Task.sleep(nanoseconds: 200_000_000)
    }
}
