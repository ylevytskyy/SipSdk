//
//  SipCallKitCoordinator.swift
//  SipSdk (iOS Platform)
//

import Foundation
import CallKit
import AVFoundation

/// The CallKit calls the coordinator makes. Tests swap in a fake, because the
/// real provider and call controller do not run under XCTest.
@MainActor
protocol CallKitSystem: AnyObject {
    func reportNewIncomingCall(with uuid: UUID, update: CXCallUpdate) async throws
    func reportCall(with uuid: UUID, endedAt: Date?, reason: CXCallEndedReason)
    func request(_ transaction: CXTransaction) async throws
}

/// Sends the coordinator's calls to the real CallKit provider and call controller.
@MainActor
final class SystemCallKit: CallKitSystem {
    private let provider: CXProvider
    private let callController: CXCallController

    init(provider: CXProvider, callController: CXCallController) {
        self.provider = provider
        self.callController = callController
    }

    func reportNewIncomingCall(with uuid: UUID, update: CXCallUpdate) async throws {
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            provider.reportNewIncomingCall(with: uuid, update: update) { error in
                if let error = error {
                    continuation.resume(throwing: error)
                } else {
                    continuation.resume()
                }
            }
        }
    }

    func reportCall(with uuid: UUID, endedAt: Date?, reason: CXCallEndedReason) {
        provider.reportCall(with: uuid, endedAt: endedAt, reason: reason)
    }

    func request(_ transaction: CXTransaction) async throws {
        try await callController.request(transaction)
    }
}

/// Turnkey coordinator bridging iOS system telephony (CallKit) with `SipClient`.
@MainActor
public final class SipCallKitCoordinator: NSObject, CXProviderDelegate {
    public let provider: CXProvider
    public let callController: CXCallController
    public weak var client: SipClient?
    public var isEnabled: Bool = (NSClassFromString("XCTestCase") == nil)

    /// Where CallKit calls go. Tests replace it with a fake.
    var system: CallKitSystem

    /// Calls that CallKit knows about and that have not ended yet.
    private var callKitCallIDs: Set<UUID> = []

    public init(localizedName: String = "SipSdk", client: SipClient? = nil) {
        self.client = client
        self.callController = CXCallController()

        let config = CXProviderConfiguration()
        config.supportsVideo = true
        config.maximumCallsPerCallGroup = 1
        config.supportedHandleTypes = [.generic]
        config.includesCallsInRecents = true

        self.provider = CXProvider(configuration: config)
        self.system = SystemCallKit(provider: provider, callController: callController)
        super.init()
        self.provider.setDelegate(self, queue: nil)
    }

    deinit {
        provider.invalidate()
    }

    /// Reports incoming SIP call to CallKit system UI (lock screen banner, full-screen incoming alert).
    public func reportIncomingCall(_ call: SipCall) async throws {
        // A call that closed before this report ran would ring in CallKit forever.
        guard isEnabled, !call.state.isClosed else { return }
        let update = CXCallUpdate()
        let handle = CXHandle(type: .generic, value: call.remoteUri)
        update.remoteHandle = handle
        update.localizedCallerName = call.peerDisplayName ?? call.remoteUri
        update.hasVideo = call.hasVideo
        update.supportsHolding = true
        update.supportsGrouping = false
        update.supportsUngrouping = false
        update.supportsDTMF = true

        // Track the call before the report suspends, so a close that arrives
        // during the report still ends the call in CallKit.
        callKitCallIDs.insert(call.id)
        try await system.reportNewIncomingCall(with: call.id, update: update)
    }

    /// Requests CallKit to start an outgoing call.
    public func startCall(to recipient: String, withVideo: Bool = true) async throws -> SipCall {
        guard let client = client else {
            throw SipError.invalidParameter("SipClient not attached to coordinator")
        }

        let call = try await client.makeCall(to: recipient, withVideo: withVideo)
        let handle = CXHandle(type: .generic, value: recipient)
        let startAction = CXStartCallAction(call: call.id, handle: handle)
        startAction.isVideo = withVideo

        let transaction = CXTransaction(action: startAction)
        // Track the call before the request suspends, so a close that arrives
        // during the request still ends the call in CallKit.
        callKitCallIDs.insert(call.id)
        try await system.request(transaction)
        return call
    }

    /// Ends CallKit call session.
    public func endCall(_ call: SipCall) async throws {
        let endAction = CXEndCallAction(call: call.id)
        let transaction = CXTransaction(action: endAction)
        try await system.request(transaction)
    }

    /// Tells CallKit that a SIP call closed, so the system call UI does not stay up.
    func reportCallClosed(_ call: SipCall, code: Int) {
        reportCallEnded(call, reason: Self.endedReason(forCloseCode: code))
    }

    /// Tells CallKit that a call it knows about ended. Calls CallKit never saw,
    /// or already ended itself, are skipped.
    func reportCallEnded(_ call: SipCall, reason: CXCallEndedReason) {
        guard callKitCallIDs.remove(call.id) != nil else { return }
        system.reportCall(with: call.id, endedAt: Date(), reason: reason)
    }

    /// Maps the code a call closed with to a CallKit end reason. The core passes
    /// a SIP status code, 0 for a BYE or CANCEL, or a local errno value below 100.
    static func endedReason(forCloseCode code: Int) -> CXCallEndedReason {
        switch code {
        case 408, 480:
            return .unanswered
        case 0, 200..<300, 486, 487, 600, 603:
            return .remoteEnded
        default:
            return .failed
        }
    }

    // MARK: - CXProviderDelegate

    nonisolated public func providerDidReset(_ provider: CXProvider) {
        Task { @MainActor in
            guard self.isEnabled, let client = self.client else { return }
            for call in client.calls {
                await call.hangup(statusCode: 487, reason: "CallKit Reset")
            }
        }
    }

    nonisolated public func provider(_ provider: CXProvider, perform action: CXStartCallAction) {
        action.fulfill()
    }

    nonisolated public func provider(_ provider: CXProvider, perform action: CXAnswerCallAction) {
        Task { @MainActor in
            guard let client = self.client,
                  let call = client.calls.first(where: { $0.id == action.callUUID }) else {
                action.fail()
                return
            }

            do {
                try await call.answer(withVideo: call.hasVideo)
                action.fulfill()
            } catch {
                action.fail()
            }
        }
    }

    nonisolated public func provider(_ provider: CXProvider, perform action: CXEndCallAction) {
        Task { @MainActor in
            // CallKit is ending this call itself, so it must not be told again on close.
            self.callKitCallIDs.remove(action.callUUID)
            guard let client = self.client,
                  let call = client.calls.first(where: { $0.id == action.callUUID }) else {
                action.fulfill()
                return
            }

            await call.hangup()
            action.fulfill()
        }
    }

    nonisolated public func provider(_ provider: CXProvider, perform action: CXSetHeldCallAction) {
        Task { @MainActor in
            guard let client = self.client,
                  let call = client.calls.first(where: { $0.id == action.callUUID }) else {
                action.fail()
                return
            }

            do {
                try await call.setHold(action.isOnHold)
                action.fulfill()
            } catch {
                action.fail()
            }
        }
    }

    nonisolated public func provider(_ provider: CXProvider, perform action: CXSetMutedCallAction) {
        Task { @MainActor in
            guard let client = self.client,
                  let call = client.calls.first(where: { $0.id == action.callUUID }) else {
                action.fail()
                return
            }

            call.setMuted(action.isMuted)
            action.fulfill()
        }
    }

    nonisolated public func provider(_ provider: CXProvider, perform action: CXPlayDTMFCallAction) {
        Task { @MainActor in
            guard let client = self.client,
                  let call = client.calls.first(where: { $0.id == action.callUUID }) else {
                action.fail()
                return
            }

            do {
                try call.sendDTMF(action.digits)
                action.fulfill()
            } catch {
                action.fail()
            }
        }
    }

    nonisolated public func provider(_ provider: CXProvider, didActivate audioSession: AVAudioSession) {
        // CallKit has activated AVAudioSession. Audio hardware is ready.
        _ = IosAudioManager.shared.prepareCallAudioSession()
    }

    nonisolated public func provider(_ provider: CXProvider, didDeactivate audioSession: AVAudioSession) {
        IosAudioManager.shared.releaseCallAudioSessionIfIdle()
    }
}
