//
//  SipCallKitManager.swift
//  SipSdk
//
//  Wraps Apple CallKit (CXProvider + CXCallController) for native iOS system call UI
//  and audio/video call state synchronization.
//

import Foundation
import CallKit
import AVFoundation

@objc protocol SipCallKitManagerDelegate: AnyObject {
    @objc optional func callKitDidAnswerCall(uuid: UUID)
    @objc optional func callKitDidEndCall(uuid: UUID)
    @objc optional func callKitDidSetHeld(uuid: UUID, onHold: Bool)
    @objc optional func callKitDidSetMuted(uuid: UUID, muted: Bool)
    @objc optional func callKitDidPlayDtmf(uuid: UUID, digits: String)
}

@objcMembers
final class SipCallKitManager: NSObject {
    static let shared = SipCallKitManager()

    weak var delegate: SipCallKitManagerDelegate?

    private let provider: CXProvider
    private let callController = CXCallController()
    private var trackedCalls: [UUID: SipCall] = [:]
    private(set) var lastReportedEvent: String = "idle"

    override init() {
        let config = CXProviderConfiguration()
        config.supportsVideo = true
        config.maximumCallGroups = 2
        config.maximumCallsPerCallGroup = 2
        config.supportedHandleTypes = [.generic]
        provider = CXProvider(configuration: config)
        super.init()
        provider.setDelegate(self, queue: .main)
    }

    var isReady: Bool {
        provider.configuration.supportsVideo
    }

    func registerCall(_ call: SipCall) {
        trackedCalls[call.uuid] = call
    }

    func call(for uuid: UUID) -> SipCall? {
        trackedCalls[uuid]
    }

    func reportIncomingCall(_ call: SipCall, completion: ((Error?) -> Void)? = nil) {
        registerCall(call)
        let update = CXCallUpdate()
        update.remoteHandle = CXHandle(type: .generic, value: call.remoteUri)
        update.localizedCallerName = call.remoteUri
        update.hasVideo = call.hasVideo
        update.supportsHolding = true
        update.supportsDTMF = true
        lastReportedEvent = "incoming:\(call.remoteUri):video=\(call.hasVideo)"

        provider.reportNewIncomingCall(with: call.uuid, update: update) { error in
            completion?(error)
        }
    }

    func reportOutgoingCallStarted(_ call: SipCall, completion: ((Error?) -> Void)? = nil) {
        registerCall(call)
        let handle = CXHandle(type: .generic, value: call.remoteUri)
        let startAction = CXStartCallAction(call: call.uuid, handle: handle)
        startAction.isVideo = call.hasVideo
        let transaction = CXTransaction(action: startAction)
        lastReportedEvent = "outgoing_start:\(call.remoteUri):video=\(call.hasVideo)"

        callController.request(transaction) { [weak self] error in
            self?.provider.reportOutgoingCall(with: call.uuid, startedConnectingAt: Date())
            completion?(error)
        }
    }

    func reportOutgoingCallConnected(_ call: SipCall) {
        registerCall(call)
        lastReportedEvent = "connected:\(call.remoteUri)"
        provider.reportOutgoingCall(with: call.uuid, connectedAt: Date())
    }

    func reportCallEnded(_ call: SipCall, reason: CXCallEndedReason = .remoteEnded) {
        lastReportedEvent = "ended:\(call.remoteUri)"
        provider.reportCall(with: call.uuid, endedAt: Date(), reason: reason)
        trackedCalls.removeValue(forKey: call.uuid)
    }

    func requestEndCall(_ call: SipCall, completion: ((Error?) -> Void)? = nil) {
        let action = CXEndCallAction(call: call.uuid)
        let transaction = CXTransaction(action: action)
        lastReportedEvent = "request_end:\(call.remoteUri)"
        callController.request(transaction) { [weak self] error in
            if error != nil {
                // Simulator fallback when system CallKit daemon rejects transaction
                call.hangup(487, reason: "Request Terminated")
                self?.trackedCalls.removeValue(forKey: call.uuid)
            }
            completion?(error)
        }
    }

    func requestHold(_ call: SipCall, onHold: Bool, completion: ((Error?) -> Void)? = nil) {
        let action = CXSetHeldCallAction(call: call.uuid, onHold: onHold)
        let transaction = CXTransaction(action: action)
        lastReportedEvent = "request_hold:\(onHold)"
        callController.request(transaction) { error in
            if error != nil {
                _ = call.setHold(onHold)
            }
            completion?(error)
        }
    }

    func requestMute(_ call: SipCall, muted: Bool, completion: ((Error?) -> Void)? = nil) {
        let action = CXSetMutedCallAction(call: call.uuid, muted: muted)
        let transaction = CXTransaction(action: action)
        lastReportedEvent = "request_mute:\(muted)"
        callController.request(transaction) { error in
            completion?(error)
        }
    }
}

extension SipCallKitManager: CXProviderDelegate {
    func providerDidReset(_ provider: CXProvider) {
        trackedCalls.removeAll()
        lastReportedEvent = "reset"
    }

    func provider(_ provider: CXProvider, perform action: CXStartCallAction) {
        provider.reportOutgoingCall(with: action.callUUID, startedConnectingAt: Date())
        action.fulfill()
    }

    func provider(_ provider: CXProvider, perform action: CXAnswerCallAction) {
        if let call = trackedCalls[action.callUUID] {
            call.answer(withVideo: call.hasVideo)
        }
        delegate?.callKitDidAnswerCall?(uuid: action.callUUID)
        action.fulfill()
    }

    func provider(_ provider: CXProvider, perform action: CXEndCallAction) {
        if let call = trackedCalls[action.callUUID] {
            call.hangup(487, reason: "Request Terminated")
            trackedCalls.removeValue(forKey: action.callUUID)
        }
        delegate?.callKitDidEndCall?(uuid: action.callUUID)
        action.fulfill()
    }

    func provider(_ provider: CXProvider, perform action: CXSetHeldCallAction) {
        if let call = trackedCalls[action.callUUID] {
            _ = call.setHold(action.isOnHold)
        }
        delegate?.callKitDidSetHeld?(uuid: action.callUUID, onHold: action.isOnHold)
        action.fulfill()
    }

    func provider(_ provider: CXProvider, perform action: CXSetMutedCallAction) {
        delegate?.callKitDidSetMuted?(uuid: action.callUUID, muted: action.isMuted)
        action.fulfill()
    }

    func provider(_ provider: CXProvider, perform action: CXPlayDTMFCallAction) {
        if let call = trackedCalls[action.callUUID] {
            _ = call.sendDtmf(action.digits)
        }
        delegate?.callKitDidPlayDtmf?(uuid: action.callUUID, digits: action.digits)
        action.fulfill()
    }

    func provider(_ provider: CXProvider, didActivate audioSession: AVAudioSession) {
        lastReportedEvent = "audio_activated"
    }

    func provider(_ provider: CXProvider, didDeactivate audioSession: AVAudioSession) {
        lastReportedEvent = "audio_deactivated"
    }
}
