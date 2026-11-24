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

    // MARK: - CallKit end reports (RES-9)

    /// Starts a registered client whose CallKit calls go to a fake instead of the system.
    @MainActor
    private func startClientWithFakeCallKit(peer: LoopbackPeer) async throws -> (SipClient, FakeCallKitSystem) {
        let client = try await startRegisteredClient(peer: peer)
        let callKit = FakeCallKitSystem()
        client.callKit.system = callKit
        client.callKit.isEnabled = true
        return (client, callKit)
    }

    /// Polls on the main actor until `condition` holds or about five seconds pass.
    @MainActor
    private func waitUntil(_ condition: () -> Bool) async throws {
        var wait = 0
        while !condition() && wait < 50 {
            try await Task.sleep(nanoseconds: 100_000_000)
            wait += 1
        }
    }

    // Given an incoming call that was reported to CallKit,
    // when the caller cancels it before the answer,
    // then CallKit is told once that the call ended.
    @MainActor
    func testCallKitIsToldWhenCallerCancelsIncomingCall() async throws {
        let peer = LoopbackPeer(port: 15097)
        defer { peer.stop() }
        let (client, callKit) = try await startClientWithFakeCallKit(peer: peer)

        peer.sendInvite()
        try await waitUntil { !callKit.reportedIncoming.isEmpty }
        guard let call = client.calls.first else {
            XCTFail("the incoming call never arrived")
            await client.stop()
            return
        }
        XCTAssertEqual(callKit.reportedIncoming, [call.id])

        peer.sendCancel()
        try await waitUntil { call.state.isClosed }
        XCTAssertTrue(call.state.isClosed)
        XCTAssertEqual(callKit.endedCalls, [.init(uuid: call.id, reason: .remoteEnded)])

        await client.stop()
    }

    // Given an outgoing call placed with `makeCall`, which CallKit never saw,
    // when the call ends,
    // then CallKit is not told about it.
    @MainActor
    func testCallKitIsNotToldAboutCallItNeverSaw() async throws {
        let peer = LoopbackPeer(port: 15098)
        defer { peer.stop() }
        let (client, callKit) = try await startClientWithFakeCallKit(peer: peer)

        // The loopback peer always answers with audio and video, so offer both.
        let call = try await client.makeCall(to: "sip:echo@127.0.0.1:15098", withVideo: true)
        try await waitUntil { call.state == .established || call.state.isClosed }
        XCTAssertEqual(call.state, .established)

        await call.hangup()
        try await waitUntil { call.state.isClosed }
        XCTAssertTrue(call.state.isClosed)
        XCTAssertEqual(callKit.endedCalls, [])

        await client.stop()
    }

    // Given an incoming call that was reported to CallKit,
    // when the user ends it from the CallKit UI,
    // then CallKit is not told a second time that the call ended.
    @MainActor
    func testCallKitIsNotToldAgainWhenItEndedTheCall() async throws {
        let peer = LoopbackPeer(port: 15099)
        defer { peer.stop() }
        let (client, callKit) = try await startClientWithFakeCallKit(peer: peer)

        peer.sendInvite()
        try await waitUntil { !callKit.reportedIncoming.isEmpty }
        guard let call = client.calls.first else {
            XCTFail("the incoming call never arrived")
            await client.stop()
            return
        }

        client.callKit.provider(client.callKit.provider, perform: CXEndCallAction(call: call.id))
        try await waitUntil { call.state.isClosed }
        XCTAssertTrue(call.state.isClosed)
        XCTAssertEqual(callKit.endedCalls, [])

        await client.stop()
    }

    // Given an outgoing call started through CallKit,
    // when the app hangs it up directly instead of through CallKit,
    // then CallKit is told that the call ended.
    @MainActor
    func testCallKitIsToldWhenAppHangsUpCallStartedThroughCallKit() async throws {
        let peer = LoopbackPeer(port: 15100)
        defer { peer.stop() }
        let (client, callKit) = try await startClientWithFakeCallKit(peer: peer)

        // The loopback peer always answers with audio and video, so offer both.
        let call = try await client.callKit.startCall(to: "sip:echo@127.0.0.1:15100", withVideo: true)
        try await waitUntil { call.state == .established || call.state.isClosed }
        XCTAssertEqual(call.state, .established)

        await call.hangup()
        try await waitUntil { call.state.isClosed }
        XCTAssertTrue(call.state.isClosed)
        XCTAssertEqual(callKit.endedCalls.map(\.uuid), [call.id])

        await client.stop()
    }

    // Given an incoming call that was reported to CallKit,
    // when the app stops the client during the call,
    // then CallKit is told once that the call ended, with reason failed.
    @MainActor
    func testCallKitIsToldWhenClientStopsDuringCall() async throws {
        let peer = LoopbackPeer(port: 15102)
        defer { peer.stop() }
        let (client, callKit) = try await startClientWithFakeCallKit(peer: peer)

        peer.sendInvite()
        try await waitUntil { !callKit.reportedIncoming.isEmpty }
        guard let call = client.calls.first else {
            XCTFail("the incoming call never arrived")
            await client.stop()
            return
        }

        await client.stop()

        XCTAssertEqual(callKit.endedCalls, [.init(uuid: call.id, reason: .failed)])
    }

    /// A coordinator whose CallKit calls go to a fake, with no SIP engine behind it.
    @MainActor
    private func makeCoordinatorWithFakeCallKit() -> (SipCallKitCoordinator, FakeCallKitSystem) {
        let coordinator = SipCallKitCoordinator(localizedName: "TestCoordinator")
        let callKit = FakeCallKitSystem()
        coordinator.system = callKit
        coordinator.isEnabled = true
        return (coordinator, callKit)
    }

    @MainActor
    private func makeIncomingCall(state: SipCallState = .incoming) -> SipCall {
        SipCall(coreCall: nil, remoteUri: "sip:peer@127.0.0.1", isOutgoing: false, initialState: state)
    }

    // Given a call that CallKit knows about,
    // when the call closes with a SIP or local error code,
    // then CallKit gets the end reason for that code.
    @MainActor
    func testCallKitEndReasonFollowsCloseCode() async throws {
        let (coordinator, callKit) = makeCoordinatorWithFakeCallKit()
        let expected: [(code: Int, reason: CXCallEndedReason)] = [
            (0, .remoteEnded),            // BYE or CANCEL: the core passes no status code
            (200, .remoteEnded),
            (486, .remoteEnded),          // Busy Here
            (487, .remoteEnded),          // Request Terminated
            (600, .remoteEnded),          // Busy Everywhere
            (603, .remoteEnded),          // Decline
            (408, .unanswered),           // Request Timeout
            (480, .unanswered),           // Temporarily Unavailable
            (404, .failed),               // Not Found
            (503, .failed),               // Service Unavailable
            (Int(EIO), .failed),          // local error from the core, not a SIP code
        ]

        for (code, reason) in expected {
            let call = makeIncomingCall()
            try await coordinator.reportIncomingCall(call)
            coordinator.reportCallClosed(call, code: code)
            XCTAssertEqual(callKit.endedCalls.last, .init(uuid: call.id, reason: reason), "close code \(code)")
        }
    }

    // Given an incoming call that closed before its CallKit report ran,
    // when the report runs,
    // then CallKit is not shown the call, so no ringing UI is left behind.
    @MainActor
    func testClosedCallIsNotReportedToCallKitAsIncoming() async throws {
        let (coordinator, callKit) = makeCoordinatorWithFakeCallKit()
        let call = makeIncomingCall(state: .closed(code: 0, reason: "Connection reset by peer"))

        try await coordinator.reportIncomingCall(call)

        XCTAssertEqual(callKit.reportedIncoming, [])
    }

    // Given an incoming call whose CallKit report has not finished yet,
    // when the call closes,
    // then CallKit is still told that the call ended.
    @MainActor
    func testCallKitIsToldWhenCallClosesDuringIncomingReport() async throws {
        let (coordinator, callKit) = makeCoordinatorWithFakeCallKit()
        callKit.holdsCalls = true
        let call = makeIncomingCall()

        let report = Task { try await coordinator.reportIncomingCall(call) }
        try await waitUntil { !callKit.reportedIncoming.isEmpty }
        coordinator.reportCallClosed(call, code: 0)
        callKit.finishHeldCalls()
        try await report.value

        XCTAssertEqual(callKit.endedCalls.map(\.uuid), [call.id])
    }

    // Given an outgoing call whose CallKit start request has not finished yet,
    // when the app hangs the call up,
    // then CallKit is still told that the call ended.
    @MainActor
    func testCallKitIsToldWhenCallClosesDuringStartRequest() async throws {
        let peer = LoopbackPeer(port: 15101)
        defer { peer.stop() }
        let (client, callKit) = try await startClientWithFakeCallKit(peer: peer)
        callKit.holdsCalls = true

        // The loopback peer always answers with audio and video, so offer both.
        let start = Task { try await client.callKit.startCall(to: "sip:echo@127.0.0.1:15101", withVideo: true) }
        try await waitUntil { !callKit.requestedActions.isEmpty }
        guard let call = client.calls.first else {
            XCTFail("the outgoing call was never created")
            callKit.finishHeldCalls()
            await client.stop()
            return
        }

        await call.hangup()
        try await waitUntil { call.state.isClosed }
        callKit.finishHeldCalls()
        _ = try await start.value

        XCTAssertTrue(call.state.isClosed)
        XCTAssertEqual(callKit.endedCalls.map(\.uuid), [call.id])

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
        XCTAssertEqual(coordinator.provider.configuration.maximumCallsPerCallGroup, 4)

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
        XCTAssertEqual(coordinator.provider.configuration.maximumCallsPerCallGroup, 3)

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
    func testLocalAudioConferenceAndCallKitGrouping() async throws {
        let client = SipClient()
        try await client.start()

        let account = SipAccount(
            username: "1001",
            domain: "127.0.0.1",
            password: "test1001",
            transport: .udp,
            port: 5060,
            maxCalls: 4
        )
        try await client.register(account)
        try await Task.sleep(for: .milliseconds(800))

        let call1 = try await client.makeCall(to: "sip:9001@127.0.0.1:5060", withVideo: false)
        try await Task.sleep(for: .milliseconds(800))
        XCTAssertEqual(call1.state, .established)

        let call2 = try await client.makeCall(to: "sip:9001@127.0.0.1:5060", withVideo: false)
        try await Task.sleep(for: .milliseconds(800))
        XCTAssertEqual(call2.state, .established)
        XCTAssertTrue(call1.isOnHold)

        // Merge via CallKit CXSetGroupCallAction path
        let merged = try client.callKit.applyGroupAction(callUUID: call1.id, callUUIDToGroupWith: call2.id)
        XCTAssertTrue(merged)
        try await Task.sleep(for: .milliseconds(800))
        XCTAssertEqual(client.conferenceCalls.count, 2)
        XCTAssertTrue(call1.isInConference)
        XCTAssertTrue(call2.isInConference)
        XCTAssertFalse(call1.isOnHold)
        XCTAssertFalse(call2.isOnHold)

        // Split via CallKit ungroup path (callUUIDToGroupWith == nil)
        let split = try client.callKit.applyGroupAction(callUUID: call2.id, callUUIDToGroupWith: nil)
        XCTAssertTrue(split)
        try await Task.sleep(for: .milliseconds(800))
        XCTAssertEqual(client.conferenceCalls.count, 0)
        XCTAssertFalse(call1.isInConference)
        XCTAssertFalse(call2.isInConference)
        XCTAssertTrue(call2.isOnHold)
        XCTAssertFalse(call1.isOnHold)

        await call1.hangup()
        await call2.hangup()
        try await Task.sleep(for: .milliseconds(400))
        await client.stop()
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

    @MainActor
    func testMultiAccountRegistrationAndRouting() async throws {
        let client = SipClient()
        try await client.start()

        let acc1 = try await client.addAccount(SipAccount(
            username: "1001",
            domain: "127.0.0.1",
            password: "test1001",
            transport: .udp,
            port: 5060
        ))
        let acc2 = try await client.addAccount(SipAccount(
            username: "1003",
            domain: "127.0.0.1",
            password: "test1003",
            transport: .udp,
            port: 5060
        ))
        let initialCount = client.accounts.count
        let initialDefaultId = client.defaultAccountSession?.id
        let acc1Id = acc1.id
        let acc2Id = acc2.id
        XCTAssertEqual(initialCount, 2)
        XCTAssertEqual(initialDefaultId, acc1Id)

        var wait = 0
        while wait < 80 {
            let r1 = acc1.isRegistered
            let r2 = acc2.isRegistered
            if r1 && r2 { break }
            try await Task.sleep(nanoseconds: 100_000_000)
            wait += 1
        }
        let reg1 = acc1.isRegistered
        let reg2 = acc2.isRegistered
        XCTAssertTrue(reg1, "Account 1001 should register")
        XCTAssertTrue(reg2, "Account 1003 should register")

        let call = try await client.makeCall(from: acc2, to: "9001", withVideo: false)
        let callAccId = call.accountSession?.id
        XCTAssertEqual(callAccId, acc2Id)
        var callWait = 0
        while callWait < 80 {
            let st = call.state
            if st == .established || st.isClosed { break }
            try await Task.sleep(nanoseconds: 100_000_000)
            callWait += 1
        }
        let finalCallState = call.state
        XCTAssertEqual(finalCallState, .established)
        await call.hangup()

        try client.setDefaultAccount(acc2)
        let switchedDefaultId = client.defaultAccountSession?.id
        XCTAssertEqual(switchedDefaultId, acc2Id)
        try await client.removeAccount(acc2)
        let afterRemoveCount = client.accounts.count
        let reelectedDefaultId = client.defaultAccountSession?.id
        XCTAssertEqual(afterRemoveCount, 1)
        XCTAssertEqual(reelectedDefaultId, acc1Id)

        try await client.removeAccount(acc1)
        let emptyCount = client.accounts.count
        XCTAssertEqual(emptyCount, 0)
        await client.stop()
    }
}
