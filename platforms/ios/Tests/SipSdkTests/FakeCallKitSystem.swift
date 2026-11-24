//
//  FakeCallKitSystem.swift
//  SipSdkTests
//

import Foundation
import CallKit
@testable import SipSdk

/// Records what the coordinator tells CallKit, in place of the real provider.
@MainActor
final class FakeCallKitSystem: CallKitSystem {
    struct EndedCall: Equatable, CustomStringConvertible {
        let uuid: UUID
        let reason: CXCallEndedReason

        var description: String { "ended \(uuid) with reason \(reason.rawValue)" }
    }

    private(set) var reportedIncoming: [UUID] = []
    private(set) var endedCalls: [EndedCall] = []
    private(set) var requestedActions: [CXAction] = []

    /// When true, incoming reports and requests wait for `finishHeldCalls()`, like a slow CallKit.
    var holdsCalls = false
    private var heldCalls: [CheckedContinuation<Void, Never>] = []

    func reportNewIncomingCall(with uuid: UUID, update: CXCallUpdate) async throws {
        reportedIncoming.append(uuid)
        await waitWhileHeld()
    }

    func reportCall(with uuid: UUID, endedAt: Date?, reason: CXCallEndedReason) {
        endedCalls.append(EndedCall(uuid: uuid, reason: reason))
    }

    func request(_ transaction: CXTransaction) async throws {
        requestedActions.append(contentsOf: transaction.actions)
        await waitWhileHeld()
    }

    /// Lets every held report and request finish.
    func finishHeldCalls() {
        holdsCalls = false
        let held = heldCalls
        heldCalls.removeAll()
        held.forEach { $0.resume() }
    }

    private func waitWhileHeld() async {
        guard holdsCalls else { return }
        await withCheckedContinuation { heldCalls.append($0) }
    }
}
