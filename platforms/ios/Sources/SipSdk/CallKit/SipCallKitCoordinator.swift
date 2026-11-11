//
//  SipCallKitCoordinator.swift
//  SipSdk (iOS Platform)
//

import Foundation
import CallKit
import AVFoundation

/// Turnkey coordinator bridging iOS system telephony (CallKit) with `SipClient`.
@MainActor
public final class SipCallKitCoordinator: NSObject, CXProviderDelegate {
    public let provider: CXProvider
    public let callController: CXCallController
    public weak var client: SipClient?
    public var isEnabled: Bool = (NSClassFromString("XCTestCase") == nil)

    public init(localizedName: String = "SipSdk", client: SipClient? = nil) {
        self.client = client
        self.callController = CXCallController()

        let config = CXProviderConfiguration()
        config.supportsVideo = true
        config.maximumCallsPerCallGroup = 1
        config.supportedHandleTypes = [.generic]
        config.includesCallsInRecents = true

        self.provider = CXProvider(configuration: config)
        super.init()
        self.provider.setDelegate(self, queue: nil)
    }

    deinit {
        provider.invalidate()
    }

    /// Reports incoming SIP call to CallKit system UI (lock screen banner, full-screen incoming alert).
    public func reportIncomingCall(_ call: SipCall) async throws {
        guard isEnabled else { return }
        let update = CXCallUpdate()
        let handle = CXHandle(type: .generic, value: call.remoteUri)
        update.remoteHandle = handle
        update.localizedCallerName = call.peerDisplayName ?? call.remoteUri
        update.hasVideo = call.hasVideo
        update.supportsHolding = true
        update.supportsGrouping = false
        update.supportsUngrouping = false
        update.supportsDTMF = true

        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            provider.reportNewIncomingCall(with: call.id, update: update) { error in
                if let error = error {
                    continuation.resume(throwing: error)
                } else {
                    continuation.resume()
                }
            }
        }
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
        try await callController.request(transaction)
        return call
    }

    /// Ends CallKit call session.
    public func endCall(_ call: SipCall) async throws {
        let endAction = CXEndCallAction(call: call.id)
        let transaction = CXTransaction(action: endAction)
        try await callController.request(transaction)
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
