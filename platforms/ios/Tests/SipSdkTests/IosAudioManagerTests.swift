//
//  IosAudioManagerTests.swift
//  SipSdkTests
//

import XCTest
import AVFoundation
@testable import SipSdk

/// Records the audio session calls `IosAudioManager` makes, and can fail an activation.
final class FakeCallAudioSession: CallAudioSession {
    private(set) var activations: [Bool] = []
    private(set) var portOverrides: [AVAudioSession.PortOverride] = []
    var failNextActivation = false
    var failOutputRoute = false

    func setCategory(_ category: AVAudioSession.Category, mode: AVAudioSession.Mode, options: AVAudioSession.CategoryOptions) throws {}

    func setActive(_ active: Bool, options: AVAudioSession.SetActiveOptions) throws {
        if active && failNextActivation {
            failNextActivation = false
            throw NSError(domain: "FakeCallAudioSession", code: 1)
        }
        activations.append(active)
    }

    func overrideOutputAudioPort(_ portOverride: AVAudioSession.PortOverride) throws {
        if failOutputRoute {
            throw NSError(domain: "FakeCallAudioSession", code: 2)
        }
        portOverrides.append(portOverride)
    }
}

final class IosAudioManagerTests: XCTestCase {

    // Given an audio session that fails to activate once,
    // when a call prepares the session (failing), prepares it again (succeeding) and releases it,
    // then the session is deactivated, because the failed attempt was not counted as a call.
    func testFailedActivationIsNotCountedAsAnActiveCall() {
        let fake = FakeCallAudioSession()
        fake.failNextActivation = true
        let manager = IosAudioManager(session: { fake })

        XCTAssertFalse(manager.prepareCallAudioSession())
        XCTAssertTrue(manager.prepareCallAudioSession())
        manager.releaseCallAudioSessionIfIdle()

        XCTAssertEqual(fake.activations, [true, false])
        XCTAssertEqual(manager.activeCallCount, 0)
    }

    // Given an audio session whose output route cannot be set,
    // when a call prepares the session,
    // then the prepare succeeds and counts the call, so releasing it deactivates the session.
    func testOutputRouteFailureDoesNotFailThePrepare() {
        let fake = FakeCallAudioSession()
        fake.failOutputRoute = true
        let manager = IosAudioManager(session: { fake })

        XCTAssertTrue(manager.prepareCallAudioSession())
        XCTAssertEqual(manager.activeCallCount, 1)
        manager.releaseCallAudioSessionIfIdle()

        XCTAssertEqual(fake.activations, [true, false])
    }

    // Given the speaker is off,
    // when the app turns the speakerphone on,
    // then the speaker route is applied once and the state reads as on.
    func testTurningSpeakerphoneOnAppliesTheRouteOnce() {
        let fake = FakeCallAudioSession()
        let manager = IosAudioManager(session: { fake })

        manager.setSpeakerphone(true)

        XCTAssertEqual(fake.portOverrides, [.speaker])
        XCTAssertTrue(manager.isSpeakerEnabled)
    }
}
