//
//  SipCall.swift
//  SipSdk (iOS Platform)
//

import Foundation
import CoreGraphics
import SipCore

/// Represents an individual active or signaling SIP call session.
@MainActor
@Observable
public final class SipCall: Identifiable {
    /// Unique identifier matching CallKit CXCall UUID.
    public let id: UUID

    /// Unique SIP Call-ID string.
    public private(set) var callId: String

    /// Remote party SIP URI (e.g. "sip:1002@127.0.0.1:5060").
    public private(set) var remoteUri: String

    /// Remote party display name if present in SIP From/To headers.
    public private(set) var peerDisplayName: String?

    /// True if call was placed locally; false if incoming.
    public let isOutgoing: Bool

    /// Current lifecycle state.
    public private(set) var state: SipCallState

    /// True if bidirectional video is negotiated.
    public private(set) var hasVideo: Bool

    /// True if call is currently placed on hold.
    public private(set) var isOnHold: Bool

    /// True if local microphone is muted for this call.
    public private(set) var isMuted: Bool

    /// Active call duration in seconds.
    public private(set) var durationSeconds: UInt32 = 0

    /// Dimensions of remote video stream.
    public private(set) var videoSize: CGSize = .zero

    /// Timestamp when call reached established state.
    public private(set) var establishedAt: Date?

    /// Internal pointer to core C call session.
    @ObservationIgnored nonisolated(unsafe) internal var coreCall: OpaquePointer?

    internal init(
        id: UUID = UUID(),
        coreCall: OpaquePointer?,
        remoteUri: String,
        peerDisplayName: String? = nil,
        isOutgoing: Bool,
        hasVideo: Bool = false,
        initialState: SipCallState = .connecting
    ) {
        self.id = id
        self.coreCall = coreCall
        self.remoteUri = remoteUri
        self.peerDisplayName = peerDisplayName
        self.isOutgoing = isOutgoing
        self.hasVideo = hasVideo
        self.state = initialState
        self.isOnHold = false
        self.isMuted = false

        if let coreCall = coreCall {
            let cid = sip_call_get_id(coreCall)
            self.callId = cid != nil ? String(cString: cid!) : id.uuidString
        } else {
            self.callId = id.uuidString
        }
    }

    deinit {
        // Any teardown of opaque call pointers handled by client
    }

    /// Answers an incoming call.
    public func answer(withVideo: Bool = true) async throws {
        guard let call = coreCall else {
            throw SipError.invalidParameter("Call session handle is invalid")
        }

        let rc = sip_call_answer(call, withVideo)
        if rc != 0 {
            throw SipError.coreError(Int(rc), "Failed to answer call")
        }
        hasVideo = withVideo
    }

    /// Hangs up, cancels, or rejects the call session.
    public func hangup(statusCode: Int = 0, reason: String? = nil) async {
        guard let call = coreCall else { return }
        _ = sip_call_hangup(call, Int32(statusCode), reason)
    }

    /// Toggles hold state for this call session.
    public func setHold(_ hold: Bool) async throws {
        guard let call = coreCall else {
            throw SipError.invalidParameter("Call session handle is invalid")
        }

        let rc = sip_call_set_hold(call, hold)
        if rc != 0 {
            throw SipError.coreError(Int(rc), "Failed to toggle hold")
        }
        isOnHold = hold
    }

    /// Mutes or unmutes local microphone for this call.
    public func setMuted(_ muted: Bool) {
        guard let call = coreCall else { return }
        _ = sip_call_set_muted(call, muted)
        isMuted = muted
    }

    /// Sends DTMF tones across active session (RFC 4733).
    public func sendDTMF(_ digits: String) throws {
        guard let call = coreCall else {
            throw SipError.invalidParameter("Call session handle is invalid")
        }
        let rc = sip_call_send_dtmf(call, digits)
        if rc != 0 {
            throw SipError.coreError(Int(rc), "Failed to send DTMF")
        }
    }

    /// Requests an immediate RTCP PLI/FIR keyframe from remote peer.
    public func requestKeyframe() {
        guard let call = coreCall else { return }
        _ = sip_call_request_keyframe(call)
    }

    // MARK: - Internal State Updates

    internal func updateState(_ newState: SipCallState) {
        self.state = newState
        if newState == .established && establishedAt == nil {
            establishedAt = Date()
        }
        if let call = coreCall {
            self.isOnHold = sip_call_is_on_hold(call)
            self.isMuted = sip_call_is_muted(call)
            self.durationSeconds = sip_call_get_duration(call)
            self.hasVideo = sip_call_has_video(call)
        }
    }

    internal func updateMedia(width: Int, height: Int, onHold: Bool, isMuted: Bool) {
        self.videoSize = CGSize(width: width, height: height)
        self.isOnHold = onHold
        self.isMuted = isMuted
        if width > 0 && height > 0 {
            self.hasVideo = true
        }
    }

    public func updateDuration() {
        if let call = coreCall {
            self.durationSeconds = sip_call_get_duration(call)
        }
    }
}
