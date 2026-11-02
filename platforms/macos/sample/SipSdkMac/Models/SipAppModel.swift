import Foundation
import Combine
import SwiftUI

public final class SipAppModel: NSObject, ObservableObject, SipMacClientDelegate {
    @Published public var registrationState: SipMacRegistrationState = .unregistered
    @Published public var registrationCode: Int = 0
    @Published public var registrationReason: String = ""

    @Published public var activeCall: SipMacCall?
    @Published public var incomingCall: SipMacCall?
    @Published public var callState: SipMacCallState = .closed
    @Published public var callStateCode: Int = 0
    @Published public var callStateReason: String = ""

    @Published public var isMuted: Bool = false
    @Published public var isOnHold: Bool = false
    @Published public var hasRemoteVideo: Bool = false
    @Published public var mediaWidth: Int = 0
    @Published public var mediaHeight: Int = 0

    @Published public var callDurationSeconds: UInt32 = 0
    @Published public var callDurationFormatted: String = "00:00"

    @Published public var availableCameras: [String] = []
    @Published public var selectedCamera: String = "Synthetic (Test Pattern)"

    public let accountStore = SipAccountStore()
    public let historyStore = SipCallHistoryStore()

    private var client: SipMacClient?
    private var durationTimer: Timer?
    private var cancellables = Set<AnyCancellable>()

    public override init() {
        super.init()
        initializeClient()
    }

    public func initializeClient() {
        let username = accountStore.username
        let domain = accountStore.domain
        let newClient = SipMacClient(username: username, domain: domain)
        newClient.delegate = self

        let rc = newClient.start(withConfigPath: nil)
        if rc == 0 {
            self.client = newClient
            self.availableCameras = newClient.availableCameras()
            if let first = self.availableCameras.first {
                self.selectedCamera = first
            }
        }
    }

    public func registerAccount() {
        guard let client = client else { return }
        _ = client.register(withPassword: accountStore.password,
                            port: Int32(accountStore.port),
                            transport: accountStore.transport)
    }

    public func unregisterAccount() {
        guard let client = client else { return }
        _ = client.unregisterAccount()
    }

    public func makeCall(targetUri: String, withVideo: Bool) {
        guard let client = client, activeCall == nil else { return }
        var uri = targetUri.trimmingCharacters(in: .whitespacesAndNewlines)
        if !uri.hasPrefix("sip:") {
            uri = "sip:\(uri)@\(accountStore.domain):\(accountStore.port)"
        }

        if let call = client.makeCall(toUri: uri, withVideo: withVideo) {
            self.activeCall = call
            self.hasRemoteVideo = withVideo
            self.callState = .connecting
            self.isMuted = false
            self.isOnHold = false
            self.callDurationSeconds = 0
            self.callDurationFormatted = "00:00"
        }
    }

    public func answerIncomingCall(withVideo: Bool) {
        guard let call = incomingCall else { return }
        _ = call.answer(withVideo: withVideo)
        self.activeCall = call
        self.incomingCall = nil
        self.hasRemoteVideo = withVideo || call.hasVideo
        self.callState = .connecting
    }

    public func declineIncomingCall() {
        guard let call = incomingCall else { return }
        _ = call.hangup(withCode: 603, reason: "Decline")
        historyStore.addRecord(remoteUri: call.remoteUri,
                               direction: "incoming",
                               hasVideo: call.hasVideo,
                               durationSeconds: 0,
                               status: "Declined")
        self.incomingCall = nil
    }

    public func hangupActiveCall() {
        guard let call = activeCall else { return }
        _ = call.hangup(withCode: 0, reason: "Hangup")
    }

    public func toggleMute() {
        guard let call = activeCall else { return }
        let nextMuted = !isMuted
        _ = call.setMuted(nextMuted)
        self.isMuted = nextMuted
    }

    public func toggleHold() {
        guard let call = activeCall else { return }
        let nextHold = !isOnHold
        _ = call.setHold(nextHold)
        self.isOnHold = nextHold
    }

    public func sendDtmf(_ digit: String) {
        guard let call = activeCall else { return }
        _ = call.sendDtmf(digit)
    }

    public func selectCamera(name: String) {
        self.selectedCamera = name
        client?.selectCamera(byName: name)
    }

    public func attachVideoView(_ view: SipMacVideoView, type: SipVideoViewRepresentable.ViewType) {
        switch type {
        case .remote:
            client?.setRemoteVideoView(view)
        case .localPreview:
            client?.setLocalPreviewView(view)
        }
    }

    // MARK: - SipMacClientDelegate

    public func client(_ client: Any, didChangeRegistrationState state: SipMacRegistrationState, code: Int, reason: String) {
        DispatchQueue.main.async {
            self.registrationState = state
            self.registrationCode = code
            self.registrationReason = reason
        }
    }

    public func client(_ client: Any, didReceiveIncomingCall call: SipMacCall) {
        DispatchQueue.main.async {
            if self.activeCall == nil {
                self.incomingCall = call
            } else {
                _ = call.hangup(withCode: 486, reason: "Busy Here")
            }
        }
    }

    public func client(_ client: Any, call: SipMacCall, didChangeState state: SipMacCallState, code: Int, reason: String) {
        DispatchQueue.main.async {
            self.callState = state
            self.callStateCode = code
            self.callStateReason = reason

            switch state {
            case .established:
                self.startDurationTimer()
            case .closed:
                self.stopDurationTimer()
                let dur = self.callDurationSeconds
                let dir = call.isOutgoing ? "outgoing" : "incoming"
                let status = dur > 0 ? "Completed" : (call.isOutgoing ? "Cancelled" : "Missed")
                self.historyStore.addRecord(remoteUri: call.remoteUri,
                                            direction: dir,
                                            hasVideo: call.hasVideo,
                                            durationSeconds: dur,
                                            status: status)
                if self.activeCall === call {
                    self.activeCall = nil
                }
                if self.incomingCall === call {
                    self.incomingCall = nil
                }
            default:
                break
            }
        }
    }

    public func client(_ client: Any, call: SipMacCall, didUpdateMediaWidth width: Int32, height: Int32) {
        DispatchQueue.main.async {
            self.mediaWidth = Int(width)
            self.mediaHeight = Int(height)
            if width > 0 && height > 0 {
                self.hasRemoteVideo = true
            }
        }
    }

    public func client(_ client: Any, call: SipMacCall, didReceiveDtmf digit: String) {
        // Optional DTMF audio feedback can be added here
    }

    // MARK: - Duration Timer

    private func startDurationTimer() {
        stopDurationTimer()
        callDurationSeconds = 0
        callDurationFormatted = "00:00"
        durationTimer = Timer.scheduledTimer(withTimeInterval: 1.0, repeats: true) { [weak self] _ in
            guard let self = self else { return }
            self.callDurationSeconds += 1
            let mins = self.callDurationSeconds / 60
            let secs = self.callDurationSeconds % 60
            self.callDurationFormatted = String(format: "%02d:%02d", mins, secs)
        }
    }

    private func stopDurationTimer() {
        durationTimer?.invalidate()
        durationTimer = nil
    }
}
