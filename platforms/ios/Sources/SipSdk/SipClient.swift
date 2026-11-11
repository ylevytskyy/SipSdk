//
//  SipClient.swift
//  SipSdk (iOS Platform)
//

import Foundation
import UIKit
import SipCore
import AVFoundation

/// Top-level coordinator managing the SIP VoIP engine, accounts, active calls, and media hardware.
@MainActor
@Observable
public final class SipClient {
    public private(set) var isStarted: Bool = false
    public private(set) var registrationState: SipRegistrationState = .unregistered
    public private(set) var currentAccount: SipAccount?
    public private(set) var calls: [SipCall] = []

    public var activeCall: SipCall? {
        calls.first(where: { !$0.state.isClosed })
    }

    public var isMicrophoneMuted: Bool = false {
        didSet {
            if let client = coreClient {
                _ = sip_client_set_mic_muted(client, isMicrophoneMuted)
            }
        }
    }

    public var isSpeakerEnabled: Bool {
        get { IosAudioManager.shared.isSpeakerEnabled }
        set { IosAudioManager.shared.setSpeakerphone(newValue) }
    }

    public var cameraPosition: SipCameraPosition = .front {
        didSet {
            if let client = coreClient {
                _ = sip_client_set_camera(client, Int32(cameraPosition.rawValue))
            }
            captureDriver.setCamera(cameraPosition)
        }
    }

    public var isLocalVideoEnabled: Bool = true {
        didSet {
            if let client = coreClient {
                _ = sip_client_set_local_video_enabled(client, isLocalVideoEnabled)
            }
            captureDriver.setEnabled(isLocalVideoEnabled)
        }
    }

    public var isLocalVideoPreviewActive: Bool {
        guard let client = coreClient else { return false }
        return sip_client_is_local_preview_active(client)
    }

    public weak var remoteVideoView: SipVideoUIView? {
        didSet {
            remoteRendererDriver.videoView = remoteVideoView
        }
    }

    public weak var localVideoView: SipVideoUIView? {
        didSet {
            localRendererDriver.videoView = localVideoView
            localVideoView?.isMirrored = (cameraPosition == .front)
        }
    }

    /// Turnkey CallKit coordinator.
    public let callKit: SipCallKitCoordinator

    /// Native video capture and rendering drivers.
    public let captureDriver = IosVideoCaptureDriver()
    public let remoteRendererDriver = IosVideoRendererDriver()
    public let localRendererDriver = IosVideoRendererDriver()

    /// Async stream for reactive event handling.
    public let events: AsyncStream<SipEvent>
    private let eventContinuation: AsyncStream<SipEvent>.Continuation

    /// Internal opaque handle to C ABI client.
    @ObservationIgnored nonisolated(unsafe) internal var coreClient: OpaquePointer?

    /// Callback context handed to the C core for the current engine run.
    @ObservationIgnored private var callbackToken: SipClientCallbackToken?

    public init() {
        var continuation: AsyncStream<SipEvent>.Continuation!
        self.events = AsyncStream<SipEvent> { cont in
            continuation = cont
        }
        self.eventContinuation = continuation
        let coordinator = SipCallKitCoordinator(localizedName: "SipSdk")
        self.callKit = coordinator
        coordinator.client = self
    }

    deinit {
        eventContinuation.finish()
        if let client = coreClient {
            sip_client_destroy(client)
        }
    }

    // MARK: - Engine Lifecycle

    /// Initializes Baresip v4 C engine, media drivers, and background worker event loop.
    public func start(configPath: String? = nil) async throws {
        guard !isStarted else { return }

        let resolvedConfigPath: String
        if let configPath = configPath {
            resolvedConfigPath = configPath
        } else {
            resolvedConfigPath = try prepareDefaultConfigDirectory()
        }

        var callbacks = sip_callbacks_t()
        callbacks.dispatch_to_main = { task, arg, userData in
            guard let task = task else { return }
            guard let userData = userData else {
                DispatchQueue.main.async { task(arg) }
                return
            }
            // Runs on the core worker, which sip_client_destroy joins before the
            // client drops its token, so the token is alive here. Each queued task
            // keeps it alive until the task has run.
            let token = Unmanaged<SipClientCallbackToken>.fromOpaque(userData).retain()
            DispatchQueue.main.async {
                task(arg)
                token.release()
            }
        }

        callbacks.on_reg_state = { client, state, code, reason, userData in
            guard let sdk = SipClientCallbackToken.client(from: userData) else { return }
            let reasonStr = reason != nil ? String(cString: reason!) : ""
            sdk.handleRegState(state: state, code: Int(code), reason: reasonStr)
        }

        callbacks.on_incoming_call = { client, call, userData in
            guard let call = call, let sdk = SipClientCallbackToken.client(from: userData) else { return }
            sdk.handleIncomingCall(callHandle: call)
        }

        callbacks.on_call_state = { call, state, code, reason, userData in
            guard let call = call, let sdk = SipClientCallbackToken.client(from: userData) else { return }
            let reasonStr = reason != nil ? String(cString: reason!) : ""
            sdk.handleCallState(callHandle: call, state: state, code: Int(code), reason: reasonStr)
        }

        callbacks.on_call_media = { call, width, height, onHold, isMuted, userData in
            guard let call = call, let sdk = SipClientCallbackToken.client(from: userData) else { return }
            sdk.handleCallMedia(callHandle: call, width: Int(width), height: Int(height), onHold: onHold, isMuted: isMuted)
        }

        callbacks.on_dtmf_digit = { call, digit, userData in
            guard let call = call, let sdk = SipClientCallbackToken.client(from: userData) else { return }
            let digitStr = digit != nil ? String(cString: digit!) : ""
            sdk.handleDtmf(callHandle: call, digit: digitStr)
        }

        callbacks.prepare_audio_session = { client, userData in
            return IosAudioManager.shared.prepareCallAudioSession()
        }

        callbacks.release_audio_session = { client, userData in
            IosAudioManager.shared.releaseCallAudioSessionIfIdle()
        }

        let token = SipClientCallbackToken(client: self)
        guard let client = sip_client_create(&callbacks, Unmanaged.passUnretained(token).toOpaque()) else {
            throw SipError.coreError(-1, "sip_client_create returned null")
        }
        self.coreClient = client
        self.callbackToken = token

        // Configure AudioUnit
        _ = sip_client_set_audio_driver(client, "audiounit", "", "audiounit", "")

        // Configure video drivers
        var cCap = captureDriver.makeCDriver()
        _ = sip_client_set_video_capture_driver(client, &cCap)

        var cRem = remoteRendererDriver.makeCDriver()
        _ = sip_client_set_remote_renderer(client, &cRem)

        var cLoc = localRendererDriver.makeCDriver()
        _ = sip_client_set_local_renderer(client, &cLoc)

        let initRc = sip_client_init(client, resolvedConfigPath)
        guard initRc == 0 else {
            sip_client_destroy(client)
            self.coreClient = nil
            detachCallbackToken()
            throw SipError.coreError(Int(initRc), "sip_client_init failed")
        }

        self.isStarted = true
    }

    /// Stops SIP engine, ends active calls, and frees all resources.
    public func stop() async {
        guard isStarted else { return }

        // Hang up any calls. Their close events are dropped once the engine
        // stops, so tell CallKit here that they ended.
        for call in calls {
            await call.hangup()
            callKit.reportCallEnded(call, reason: .failed)
        }
        calls.removeAll()

        captureDriver.stop()
        remoteVideoView?.clearVideo()
        localVideoView?.clearVideo()

        if let client = coreClient {
            sip_client_destroy(client)
            self.coreClient = nil
        }
        detachCallbackToken()

        IosAudioManager.shared.releaseCallAudioSessionIfIdle()
        registrationState = .unregistered
        isStarted = false
    }

    // MARK: - Account Registration & Handover

    /// Registers a SIP account profile with optional display name, outbound proxy, STUN/TURN/ICE NAT traversal.
    public func register(_ account: SipAccount) async throws {
        guard isStarted, let client = coreClient else {
            throw SipError.engineNotStarted
        }

        self.currentAccount = account
        self.registrationState = .registering
        eventContinuation.yield(.registrationChanged(.registering))

        account.withCAccountConfig { cConfig in
            _ = sip_client_register_account(client, &cConfig)
        }
    }

    /// Unregisters the current account.
    public func unregister() async {
        guard let client = coreClient else { return }
        _ = sip_client_unregister(client)
        registrationState = .unregistered
        eventContinuation.yield(.registrationChanged(.unregistered))
    }

    /// Resets network sockets for seamless network handover (Wi-Fi <-> Cellular) and triggers re-registration.
    public func resetTransports(reRegister: Bool = true) {
        guard let client = coreClient else { return }
        _ = sip_client_reset_transports(client, reRegister)
    }

    // MARK: - Call Operations

    /// Places an outgoing audio or video SIP call.
    @discardableResult
    public func makeCall(to destination: String, withVideo: Bool = true) async throws -> SipCall {
        guard isStarted, let client = coreClient else {
            throw SipError.engineNotStarted
        }

        var dialUri = destination
        if !dialUri.hasPrefix("sip:") {
            if !dialUri.contains("@"), let domain = currentAccount?.domain {
                dialUri = "sip:\(dialUri)@\(domain)"
            } else {
                dialUri = "sip:\(dialUri)"
            }
        }

        _ = IosAudioManager.shared.prepareCallAudioSession()

        guard let cCall = sip_call_make(client, dialUri, withVideo) else {
            IosAudioManager.shared.releaseCallAudioSessionIfIdle()
            throw SipError.callCreationFailed("sip_call_make returned null for URI: \(dialUri)")
        }

        let sipCall = SipCall(
            coreCall: cCall,
            remoteUri: dialUri,
            isOutgoing: true,
            hasVideo: withVideo,
            initialState: .connecting
        )

        calls.append(sipCall)

        eventContinuation.yield(.callStateChanged(sipCall, .connecting))
        return sipCall
    }

    // MARK: - Video & Camera Controls

    /// Switches between front and back cameras.
    public func switchCamera() {
        cameraPosition = (cameraPosition == .front) ? .back : .front
    }

    /// Starts local camera preview outside of a call.
    public func startLocalVideoPreview() {
        guard let client = coreClient else { return }
        _ = sip_client_start_local_preview(client)
    }

    /// Stops local camera preview.
    public func stopLocalVideoPreview() {
        guard let client = coreClient else { return }
        _ = sip_client_stop_local_preview(client)
    }

    // MARK: - Internal Callbacks

    private func handleRegState(state: sip_reg_state_t, code: Int, reason: String) {
        let newState: SipRegistrationState
        switch state {
        case SIP_REG_STATE_UNREGISTERED:
            newState = .unregistered
        case SIP_REG_STATE_REGISTERING:
            newState = .registering
        case SIP_REG_STATE_REGISTERED:
            newState = .registered
        case SIP_REG_STATE_FAILED:
            newState = .failed(code: code, reason: reason)
        default:
            newState = .unregistered
        }

        self.registrationState = newState
        eventContinuation.yield(.registrationChanged(newState))
    }

    /// Drops events that the stopped engine queued on the main queue before it stopped.
    private func detachCallbackToken() {
        callbackToken?.client = nil
        callbackToken = nil
    }

    private func handleIncomingCall(callHandle: OpaquePointer) {
        let remoteUriC = sip_call_get_remote_uri(callHandle)
        let remoteUri = remoteUriC != nil ? String(cString: remoteUriC!) : "Unknown"

        let peerNameC = sip_call_get_peer_display_name(callHandle)
        let peerName = peerNameC != nil ? String(cString: peerNameC!) : nil

        let hasVideo = sip_call_has_video(callHandle)

        let sipCall = SipCall(
            coreCall: callHandle,
            remoteUri: remoteUri,
            peerDisplayName: peerName,
            isOutgoing: false,
            hasVideo: hasVideo,
            initialState: .incoming
        )

        calls.append(sipCall)

        _ = IosAudioManager.shared.prepareCallAudioSession()

        eventContinuation.yield(.incomingCall(sipCall))
        eventContinuation.yield(.callStateChanged(sipCall, .incoming))

        // Automatically report to CallKit coordinator
        if callKit.isEnabled {
            Task {
                do {
                    try await callKit.reportIncomingCall(sipCall)
                } catch {
                    NSLog("[SipSdk] CallKit incoming report failed: %@", error.localizedDescription)
                }
            }
        }
    }

    private func handleCallState(callHandle: OpaquePointer, state: sip_call_state_t, code: Int, reason: String) {
        guard let sipCall = findOrCreateCall(for: callHandle) else { return }

        let newState: SipCallState
        switch state {
        case SIP_CALL_STATE_INCOMING:
            newState = .incoming
        case SIP_CALL_STATE_RINGING:
            newState = .ringing
        case SIP_CALL_STATE_CONNECTING:
            newState = .connecting
        case SIP_CALL_STATE_ESTABLISHED:
            newState = .established
        case SIP_CALL_STATE_HOLD:
            newState = .hold
        case SIP_CALL_STATE_CLOSED:
            newState = .closed(code: code, reason: reason)
        default:
            newState = .closed(code: code, reason: reason)
        }

        sipCall.updateState(newState)
        eventContinuation.yield(.callStateChanged(sipCall, newState))

        if newState.isClosed {
            calls.removeAll(where: { $0.id == sipCall.id })
            sip_call_destroy(callHandle)
            sipCall.coreCall = nil
            callKit.reportCallClosed(sipCall, code: code)
            IosAudioManager.shared.releaseCallAudioSessionIfIdle()
        }
    }

    private func handleCallMedia(callHandle: OpaquePointer, width: Int, height: Int, onHold: Bool, isMuted: Bool) {
        guard let sipCall = findOrCreateCall(for: callHandle) else { return }
        sipCall.updateMedia(width: width, height: height, onHold: onHold, isMuted: isMuted)
        eventContinuation.yield(.mediaUpdated(sipCall, width: width, height: height, onHold: onHold, isMuted: isMuted))
    }

    private func handleDtmf(callHandle: OpaquePointer, digit: String) {
        guard let sipCall = findOrCreateCall(for: callHandle) else { return }
        eventContinuation.yield(.dtmfReceived(sipCall, digit: digit))
    }

    /// Finds the live `SipCall` for a core handle. `calls` holds the only strong
    /// references, so a handle that already closed and left `calls` is ignored
    /// instead of being wrapped again or destroyed twice.
    private func findOrCreateCall(for callHandle: OpaquePointer) -> SipCall? {
        if let existing = calls.first(where: { $0.coreCall == callHandle }) {
            return existing
        }
        if sip_call_is_closed(callHandle) {
            return nil
        }

        let remoteUriC = sip_call_get_remote_uri(callHandle)
        let remoteUri = remoteUriC != nil ? String(cString: remoteUriC!) : "Unknown"
        let isOutgoing = sip_call_is_outgoing(callHandle)
        let hasVideo = sip_call_has_video(callHandle)

        let sipCall = SipCall(
            coreCall: callHandle,
            remoteUri: remoteUri,
            isOutgoing: isOutgoing,
            hasVideo: hasVideo,
            initialState: .connecting
        )
        calls.append(sipCall)
        return sipCall
    }

    // MARK: - Config Helper

    private func prepareDefaultConfigDirectory() throws -> String {
        let fileManager = FileManager.default
        guard let docs = fileManager.urls(for: .documentDirectory, in: .userDomainMask).first else {
            throw SipError.invalidParameter("Cannot access DocumentDirectory")
        }

        let configDir = docs.appendingPathComponent("baresip-v4", isDirectory: true)
        try fileManager.createDirectory(at: configDir, withIntermediateDirectories: true)

        let destination = configDir.appendingPathComponent("config")

        // Discover bundled config.baresip-v4 from module bundle or main bundle
        var bundledPath: String?
        #if SWIFT_PACKAGE
        bundledPath = Bundle.module.path(forResource: "config", ofType: "baresip-v4")
        #endif
        if bundledPath == nil {
            bundledPath = Bundle.main.path(forResource: "config", ofType: "baresip-v4")
        }

        if let bundledPath = bundledPath {
            if fileManager.fileExists(atPath: destination.path) {
                try? fileManager.removeItem(at: destination)
            }
            try fileManager.copyItem(atPath: bundledPath, toPath: destination.path)
        }

        return configDir.path
    }
}

/// Context the C core passes back to every callback. It holds the client weakly,
/// so events still queued on the main queue after `stop()` or after the client
/// is released are dropped instead of touching a freed `SipClient`.
final class SipClientCallbackToken: @unchecked Sendable {
    weak var client: SipClient?

    init(client: SipClient) {
        self.client = client
    }

    static func client(from userData: UnsafeMutableRawPointer?) -> SipClient? {
        guard let userData = userData else { return nil }
        return Unmanaged<SipClientCallbackToken>.fromOpaque(userData).takeUnretainedValue().client
    }
}
