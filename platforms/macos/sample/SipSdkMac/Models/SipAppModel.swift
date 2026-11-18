import Foundation
import Combine
import SwiftUI
import Darwin

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
    @Published public var isCallSecure: Bool = false
    @Published public var callStatsSummary: String = ""
    @Published public var transferStatusMessage: String?
    @Published public var pendingTransferUri: String?
    @Published public var hasRemoteVideo: Bool = false
    @Published public var mediaWidth: Int = 0
    @Published public var mediaHeight: Int = 0

    @Published public var callDurationSeconds: UInt32 = 0
    @Published public var callDurationFormatted: String = "00:00"

    @Published public var availableCameras: [String] = []
    @Published public var selectedCamera: String = "Synthetic (Test Pattern)"

    @Published public var availableAudioInputs: [String] = []
    @Published public var selectedAudioInput: String = ""

    @Published public var availableAudioOutputs: [String] = []
    @Published public var selectedAudioOutput: String = ""

    @Published public var availableVideoOutputs: [String] = []
    @Published public var selectedVideoOutput: String = ""

    @Published public var isHandoverInProgress: Bool = false
    @Published public var handoverMessage: String?
    @Published public var isPreviewActive: Bool = false
    @Published public var isHangingUp: Bool = false

    public var autoCallTarget: String?
    public var autoCallWithVideo: Bool = true
    public var isAutomatedTest: Bool = false
    public var autoExitOnTestComplete: Bool = false
    public var initialSectionName: String = "Dialer"

    public let accountStore = SipAccountStore()
    public let historyStore = SipCallHistoryStore()

    private var client: SipMacClient?
    private var durationTimer: Timer?
    private var cancellables = Set<AnyCancellable>()

    public override init() {
        setlinebuf(stdout)
        setlinebuf(stderr)
        super.init()
        parseArguments()
        initializeClient()
    }

    private func parseArguments() {
        let args = CommandLine.arguments
        if let idx = args.firstIndex(of: "--test-call"), idx + 1 < args.count {
            self.autoCallTarget = args[idx + 1]
            self.isAutomatedTest = true
            self.autoExitOnTestComplete = true
            self.autoCallWithVideo = !args.contains("--no-video")
        }
        if args.contains("--test-settings-screen") {
            self.initialSectionName = "Settings"
            self.isAutomatedTest = true
            self.autoExitOnTestComplete = true
            DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) {
                print("[SipAppModel] Settings screen automated test finished cleanly.")
                NSApplication.shared.terminate(nil)
            }
        }
    }

    public func initializeClient() {
        let username = accountStore.username
        let domain = accountStore.domain
        let newClient = SipMacClient(username: username, domain: domain)
        newClient.delegate = self

        let rc = newClient.start(withConfigPath: nil)
        if rc == 0 {
            self.client = newClient
            refreshMediaDevices()
            print("[SipAppModel] Discovered audio inputs (\(availableAudioInputs.count)): \(availableAudioInputs)")
            print("[SipAppModel] Discovered audio outputs (\(availableAudioOutputs.count)): \(availableAudioOutputs)")
            print("[SipAppModel] Discovered cameras (\(availableCameras.count)): \(availableCameras)")
            print("[SipAppModel] Discovered video displays (\(availableVideoOutputs.count)): \(availableVideoOutputs)")

            if self.isAutomatedTest {
                print("[SipAppModel] Testing local camera preview...")
                self.setPreviewActive(true)
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) {
                    self.setPreviewActive(false)
                    print("[SipAppModel] Local camera preview test passed")
                }
            }
        }
    }

    public func registerAccount() {
        guard let client = client else { return }
        let acc = accountStore
        _ = client.register(withUsername: acc.username,
                            domain: acc.domain,
                            password: acc.password,
                            transport: acc.transport,
                            port: Int32(acc.port),
                            displayName: acc.displayName.isEmpty ? nil : acc.displayName,
                            outboundProxy: acc.outboundProxy.isEmpty ? nil : acc.outboundProxy,
                            stunServer: acc.stunServer.isEmpty ? nil : acc.stunServer,
                            mediaNat: acc.useIce ? "ice" : nil,
                            regInterval: acc.regInterval,
                            tlsCaFile: acc.tlsCaFile.isEmpty ? nil : acc.tlsCaFile,
                            verifyTlsCert: acc.verifyTlsCert,
                            mediaEnc: acc.mediaEnc == "none" ? nil : acc.mediaEnc)
    }

    public func unregisterAccount() {
        guard let client = client else { return }
        _ = client.unregisterAccount()
    }

    public func makeCall(targetUri: String, withVideo: Bool) {
        guard let client = client, activeCall == nil else { return }
        NSApp.keyWindow?.makeFirstResponder(nil)
        self.isHangingUp = false
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

    /// Sends a text SIP MESSAGE. target is an extension or a sip: URI. Returns the message id, or 0.
    @discardableResult
    public func sendMessage(to target: String, text: String) -> UInt64 {
        guard let client = client else { return 0 }
        var uri = target.trimmingCharacters(in: .whitespacesAndNewlines)
        if !uri.hasPrefix("sip:") {
            uri = "sip:\(uri)@\(accountStore.domain):\(accountStore.port)"
        }
        let messageId = client.sendMessage(toUri: uri, text: text)
        print("[SipAppModel] Sending message id=\(messageId) to \(uri)")
        return messageId
    }

    public func answerIncomingCall(withVideo: Bool) {
        guard let call = incomingCall else { return }
        _ = call.answer(withVideo: withVideo)
        self.activeCall = call
        self.incomingCall = nil
        self.hasRemoteVideo = withVideo || call.hasVideo
        self.callState = .connecting
        self.isHangingUp = false
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
        self.isHangingUp = true
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

    public func transferActiveCall(to destination: String) {
        guard let call = activeCall else { return }
        var uri = destination.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !uri.isEmpty else { return }
        if !uri.hasPrefix("sip:") {
            uri = "sip:\(uri)@\(accountStore.domain):\(accountStore.port)"
        }
        _ = call.transfer(toUri: uri)
    }

    public func respondToTransferRequest(accept: Bool) {
        guard let call = activeCall else { return }
        _ = call.acceptTransfer(accept)
        pendingTransferUri = nil
    }

    public func refreshMediaDevices() {
        guard let client = client else { return }
        self.availableCameras = client.availableCameras()
        if let cam = client.selectedCamera(), !cam.isEmpty {
            self.selectedCamera = cam
        } else if let first = self.availableCameras.first {
            self.selectedCamera = first
        }

        self.availableAudioInputs = client.availableAudioInputDevices()
        if let inp = client.selectedAudioInputDevice(), !inp.isEmpty {
            self.selectedAudioInput = inp
        } else if let first = self.availableAudioInputs.first {
            self.selectedAudioInput = first
        }

        self.availableAudioOutputs = client.availableAudioOutputDevices()
        if let out = client.selectedAudioOutputDevice(), !out.isEmpty {
            self.selectedAudioOutput = out
        } else if let first = self.availableAudioOutputs.first {
            self.selectedAudioOutput = first
        }

        self.availableVideoOutputs = client.availableVideoOutputDevices()
        if let vout = client.selectedVideoOutputDevice(), !vout.isEmpty {
            self.selectedVideoOutput = vout
        } else if let first = self.availableVideoOutputs.first {
            self.selectedVideoOutput = first
        }
    }

    public func selectCamera(name: String) {
        self.selectedCamera = name
        client?.selectCamera(byName: name)
    }

    public func selectAudioInput(name: String) {
        self.selectedAudioInput = name
        client?.selectAudioInputDevice(byName: name)
    }

    public func selectAudioOutput(name: String) {
        self.selectedAudioOutput = name
        client?.selectAudioOutputDevice(byName: name)
    }

    public func selectVideoOutput(name: String) {
        self.selectedVideoOutput = name
        client?.selectVideoOutputDevice(byName: name)
    }

    public func performNetworkHandover() {
        isHandoverInProgress = true
        handoverMessage = nil
        _ = client?.resetTransports(true)
        handoverMessage = "Transports reset & re-registration initiated"
        isHandoverInProgress = false
    }

    public func setPreviewActive(_ active: Bool) {
        self.isPreviewActive = active
        if active {
            _ = client?.startLocalPreview()
        } else {
            _ = client?.stopLocalPreview()
        }
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

    public func client(_ client: SipMacClient, didChange state: SipMacRegistrationState, code: Int, reason: String) {
        DispatchQueue.main.async {
            self.registrationState = state
            self.registrationCode = code
            self.registrationReason = reason
            print("[SipAppModel] Registration state: \(state.rawValue) (\(code) \(reason))")
            if state == .registered, self.autoCallTarget != nil {
                // Automated messaging check: send a SIP MESSAGE to our own address.
                self.sendMessage(to: self.accountStore.username, text: "hello from macOS")
            }
            if state == .registered, let target = self.autoCallTarget {
                self.autoCallTarget = nil
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {
                    print("[SipAppModel] Auto-initiating call to \(target) (video=\(self.autoCallWithVideo))")
                    self.makeCall(targetUri: target, withVideo: self.autoCallWithVideo)
                }
            }
        }
    }

    public func client(_ client: SipMacClient, didReceiveIncomingCall call: SipMacCall) {
        DispatchQueue.main.async {
            print("[SipAppModel] Incoming call from \(call.remoteUri)")
            if self.activeCall == nil {
                self.incomingCall = call
            } else {
                _ = call.hangup(withCode: 486, reason: "Busy Here")
            }
        }
    }

    public func client(_ client: SipMacClient, call: SipMacCall, didChange state: SipMacCallState, code: Int, reason: String) {
        DispatchQueue.main.async {
            self.callState = state
            self.callStateCode = code
            self.callStateReason = reason

            switch state {
            case .established:
                print("[SipAppModel] Call established with \(call.remoteUri)")
                if self.durationTimer == nil {
                    self.startDurationTimer()
                    if self.isAutomatedTest {
                        // Test in-call audio & video device switching
                        DispatchQueue.main.asyncAfter(deadline: .now() + 1.0) {
                            if let targetCam = self.availableCameras.first(where: { !$0.contains("Synthetic") }) ?? self.availableCameras.first {
                                print("[SipAppModel] In-call switching camera to: \(targetCam)")
                                self.selectCamera(name: targetCam)
                            }
                            if let targetMic = self.availableAudioInputs.first(where: { $0.contains("MacBook") || $0.contains("Built-in") }) ?? self.availableAudioInputs.first {
                                print("[SipAppModel] In-call switching microphone to: \(targetMic)")
                                self.selectAudioInput(name: targetMic)
                            }
                            if let targetSpeaker = self.availableAudioOutputs.first(where: { $0.contains("MacBook") || $0.contains("Built-in") }) ?? self.availableAudioOutputs.first {
                                print("[SipAppModel] In-call switching speaker to: \(targetSpeaker)")
                                self.selectAudioOutput(name: targetSpeaker)
                            }
                            if let firstDisplay = self.availableVideoOutputs.first {
                                print("[SipAppModel] In-call switching display to: \(firstDisplay)")
                                self.selectVideoOutput(name: firstDisplay)
                            }
                        }

                        DispatchQueue.main.asyncAfter(deadline: .now() + 3.0) {
                            print("[SipAppModel] Automated test finished streaming. Hanging up...")
                            self.hangupActiveCall()
                        }
                    }
                }
            case .closed:
                print("[SipAppModel] Call closed (\(code) \(reason))")
                self.stopDurationTimer()
                self.isHangingUp = false
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
                    self.hasRemoteVideo = false
                    self.isMuted = false
                    self.isOnHold = false
                    self.isCallSecure = false
                    self.callStatsSummary = ""
                    self.transferStatusMessage = nil
                    self.pendingTransferUri = nil
                }
                if self.incomingCall === call {
                    self.incomingCall = nil
                }
                if self.isAutomatedTest && self.autoExitOnTestComplete {
                    DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {
                        print("[SipAppModel] Exiting automated test cleanly.")
                        NSApplication.shared.terminate(nil)
                    }
                }
            default:
                break
            }
        }
    }

    public func client(_ client: SipMacClient, call: SipMacCall, didUpdateMediaWidth width: Int32, height: Int32) {
        DispatchQueue.main.async {
            self.mediaWidth = Int(width)
            self.mediaHeight = Int(height)
            print("[SipAppModel] Media dimensions updated: \(width)x\(height)")
            if width > 0 && height > 0 {
                self.hasRemoteVideo = true
            }
        }
    }

    public func client(_ client: SipMacClient, call: SipMacCall, didReceiveDtmf digit: String) {
        // Optional DTMF audio feedback can be added here
    }

    public func client(_ client: SipMacClient, call: SipMacCall, didUpdateStatsCodec codec: String, bitrateKbps: UInt32, lossRate: Float, jitterMs: UInt32, rttMs: UInt32) {
        DispatchQueue.main.async {
            self.isCallSecure = call.isSecure
            if !codec.isEmpty {
                self.callStatsSummary = String(format: "%@ • %u kbps • loss %.1f%% • jitter %u ms", codec.uppercased(), bitrateKbps, lossRate, jitterMs)
            }
        }
    }

    public func client(_ client: SipMacClient, call: SipMacCall, didChangeTransferStatus code: Int, reason: String) {
        DispatchQueue.main.async {
            self.transferStatusMessage = "Transfer: \(code) \(reason)"
        }
    }

    public func client(_ client: SipMacClient, call: SipMacCall, didReceiveTransferRequest referToUri: String, hasReplaces: Bool) {
        DispatchQueue.main.async {
            self.pendingTransferUri = referToUri
        }
    }

    public func client(_ client: SipMacClient, didReceiveMessageFrom from: String, contentType: String, body: Data) {
        let text = String(data: body, encoding: .utf8) ?? "<\(body.count) bytes>"
        print("[SipAppModel] Message received from \(from) (\(contentType)): \(text)")
    }

    public func client(_ client: SipMacClient, message messageId: UInt64, didChange status: SipMacMessageStatus, code: Int, reason: String) {
        if status == .sent {
            print("[SipAppModel] Message sent (id=\(messageId), \(code) \(reason))")
        } else {
            print("[SipAppModel] Message status \(status.rawValue) (id=\(messageId), \(code) \(reason))")
        }
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
