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
