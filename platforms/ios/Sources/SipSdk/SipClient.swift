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
            DispatchQueue.main.async {
                task(arg)
            }
        }

        callbacks.on_reg_state = { client, state, code, reason, userData in
            guard let userData = userData else { return }
            let sdk = Unmanaged<SipClient>.fromOpaque(userData).takeUnretainedValue()
            let reasonStr = reason != nil ? String(cString: reason!) : ""
            sdk.handleRegState(state: state, code: Int(code), reason: reasonStr)
        }

        callbacks.on_incoming_call = { client, call, userData in
            guard let userData = userData, let call = call else { return }
            let sdk = Unmanaged<SipClient>.fromOpaque(userData).takeUnretainedValue()
            sdk.handleIncomingCall(callHandle: call)
        }

        callbacks.on_call_state = { call, state, code, reason, userData in
            guard let userData = userData, let call = call else { return }
            let sdk = Unmanaged<SipClient>.fromOpaque(userData).takeUnretainedValue()
            let reasonStr = reason != nil ? String(cString: reason!) : ""
            sdk.handleCallState(callHandle: call, state: state, code: Int(code), reason: reasonStr)
        }

        callbacks.on_call_media = { call, width, height, onHold, isMuted, userData in
            guard let userData = userData, let call = call else { return }
            let sdk = Unmanaged<SipClient>.fromOpaque(userData).takeUnretainedValue()
            sdk.handleCallMedia(callHandle: call, width: Int(width), height: Int(height), onHold: onHold, isMuted: isMuted)
        }

        callbacks.on_dtmf_digit = { call, digit, userData in
            guard let userData = userData, let call = call else { return }
            let sdk = Unmanaged<SipClient>.fromOpaque(userData).takeUnretainedValue()
            let digitStr = digit != nil ? String(cString: digit!) : ""
            sdk.handleDtmf(callHandle: call, digit: digitStr)
        }

        callbacks.prepare_audio_session = { client, userData in
            return IosAudioManager.shared.prepareCallAudioSession()
        }

        callbacks.release_audio_session = { client, userData in
            IosAudioManager.shared.releaseCallAudioSessionIfIdle()
        }

        callbacks.on_call_stats = { call, statsPtr, userData in
            guard let userData = userData, let call = call, let statsPtr = statsPtr else { return }
            let sdk = Unmanaged<SipClient>.fromOpaque(userData).takeUnretainedValue()
            let coreStats = statsPtr.pointee
            let stats = SipCallStats(
                audioCodec: withUnsafeBytes(of: coreStats.audio_codec) { p in
                    p.baseAddress != nil ? String(cString: p.baseAddress!.assumingMemoryBound(to: CChar.self)) : ""
                },
                audioBitrateKbps: coreStats.audio_bitrate_kbps,
                audioLossRate: coreStats.audio_loss_rate,
                audioJitterMs: coreStats.audio_jitter_ms,
                audioRttMs: coreStats.audio_rtt_ms,
                videoCodec: withUnsafeBytes(of: coreStats.video_codec) { p in
                    p.baseAddress != nil ? String(cString: p.baseAddress!.assumingMemoryBound(to: CChar.self)) : ""
                },
                videoBitrateKbps: coreStats.video_bitrate_kbps,
                videoLossRate: coreStats.video_loss_rate,
                videoJitterMs: coreStats.video_jitter_ms,
                videoFps: coreStats.video_fps
            )
            sdk.handleCallStats(callHandle: call, stats: stats)
        }

        callbacks.on_transfer_status = { call, code, reason, userData in
            guard let userData = userData, let call = call else { return }
            let sdk = Unmanaged<SipClient>.fromOpaque(userData).takeUnretainedValue()
            let reasonStr = reason != nil ? String(cString: reason!) : ""
            sdk.handleTransferStatus(callHandle: call, code: Int(code), reason: reasonStr)
        }

        callbacks.on_transfer_request = { call, referTo, hasReplaces, userData in
            guard let userData = userData, let call = call else { return }
            let sdk = Unmanaged<SipClient>.fromOpaque(userData).takeUnretainedValue()
            let referToStr = referTo != nil ? String(cString: referTo!) : ""
            sdk.handleTransferRequest(callHandle: call, referToUri: referToStr, hasReplaces: hasReplaces)
        }

        let context = Unmanaged.passUnretained(self).toOpaque()
        guard let client = sip_client_create(&callbacks, context) else {
            throw SipError.coreError(-1, "sip_client_create returned null")
        }
        self.coreClient = client

        var messageCallbacks = sip_message_callbacks_t()
        messageCallbacks.on_message_received = { _, fromUri, contentType, body, bodyLen, userData in
            guard let userData = userData else { return }
            let sdk = Unmanaged<SipClient>.fromOpaque(userData).takeUnretainedValue()
            let from = fromUri != nil ? String(cString: fromUri!) : ""
            let ctype = contentType != nil ? String(cString: contentType!) : "text/plain"
            let data = (body != nil && bodyLen > 0) ? Data(bytes: body!, count: bodyLen) : Data()
            sdk.handleMessageReceived(SipIncomingMessage(from: from, contentType: ctype, body: data))
        }
        messageCallbacks.on_message_status = { _, messageId, status, code, reason, userData in
            guard let userData = userData else { return }
            let sdk = Unmanaged<SipClient>.fromOpaque(userData).takeUnretainedValue()
            let reasonStr = reason != nil ? String(cString: reason!) : ""
            sdk.handleMessageStatus(id: messageId, status: status, code: Int(code), reason: reasonStr)
        }
        _ = sip_client_set_message_callbacks(client, &messageCallbacks)

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
            throw SipError.coreError(Int(initRc), "sip_client_init failed")
        }

        self.isStarted = true
    }

    /// Stops SIP engine, ends active calls, and frees all resources.
    public func stop() async {
        guard isStarted else { return }

        // Hangup any calls
        for call in calls {
            await call.hangup()
        }
        calls.removeAll()

        captureDriver.stop()
        remoteVideoView?.clearVideo()
        localVideoView?.clearVideo()

        if let client = coreClient {
            sip_client_destroy(client)
            self.coreClient = nil
        }

        // Let any already-queued main-queue dispatch blocks drain while self is still alive
        try? await Task.sleep(nanoseconds: 50_000_000)

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

        var cConfig = sip_account_config_t()
        account.username.withCString { userPtr in
            account.domain.withCString { domainPtr in
                account.password.withCString { pwdPtr in
                    account.transport.rawValue.withCString { tpPtr in
                        cConfig.user = userPtr
                        cConfig.domain = domainPtr
                        cConfig.pwd = pwdPtr
                        cConfig.transport = tpPtr
                        cConfig.port = Int32(account.port)
                        cConfig.reg_interval = account.regInterval
                        cConfig.verify_tls_cert = account.verifyTlsCert

                        // Optional strings
                        let dname = account.displayName?.cString(using: .utf8)
                        let ob = account.outboundProxy?.cString(using: .utf8)
                        let stun = account.stunServer?.cString(using: .utf8)
                        let stunUser = account.stunUser?.cString(using: .utf8)
                        let stunPass = account.stunPassword?.cString(using: .utf8)
                        let nat = account.mediaNat?.cString(using: .utf8)
                        let tlsCa = account.tlsCaFile?.cString(using: .utf8)
                        let menc = account.mediaEnc?.cString(using: .utf8)

                        dname?.withUnsafeBufferPointer { cConfig.display_name = $0.baseAddress }
                        ob?.withUnsafeBufferPointer { cConfig.outbound_proxy = $0.baseAddress }
                        stun?.withUnsafeBufferPointer { cConfig.stun_server = $0.baseAddress }
                        stunUser?.withUnsafeBufferPointer { cConfig.stun_user = $0.baseAddress }
                        stunPass?.withUnsafeBufferPointer { cConfig.stun_pass = $0.baseAddress }
                        nat?.withUnsafeBufferPointer { cConfig.media_nat = $0.baseAddress }
                        tlsCa?.withUnsafeBufferPointer { cConfig.tls_ca_file = $0.baseAddress }
                        menc?.withUnsafeBufferPointer { cConfig.media_enc = $0.baseAddress }

                        _ = sip_client_register_account(client, &cConfig)
                    }
                }
            }
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

        let dialUri = sipUri(for: destination)

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

        sip_call_set_user_data(cCall, Unmanaged.passUnretained(sipCall).toOpaque())
        calls.append(sipCall)

        eventContinuation.yield(.callStateChanged(sipCall, .connecting))
        return sipCall
    }

    /// Turns an extension ("1002"), user@host, or SIP URI into a SIP URI.
    private func sipUri(for destination: String) -> String {
        if destination.hasPrefix("sip:") {
            return destination
        }
        if !destination.contains("@"), let domain = currentAccount?.domain {
            return "sip:\(destination)@\(domain)"
        }
        return "sip:\(destination)"
    }

    // MARK: - Messaging

    /// Sends a UTF-8 text message (SIP MESSAGE, `text/plain`).
    /// - Returns: Local message id; its result arrives as `.messageStatusChanged` on `events`.
    @discardableResult
    public func sendMessage(to destination: String, text: String) throws -> UInt64 {
        try sendMessage(to: destination, body: Data(text.utf8), contentType: "text/plain")
    }

    /// Sends a SIP MESSAGE with any MIME content type (e.g. `application/json`).
    /// - Returns: Local message id; its result arrives as `.messageStatusChanged` on `events`.
    @discardableResult
    public func sendMessage(to destination: String, body: Data, contentType: String) throws -> UInt64 {
        guard isStarted, let client = coreClient else {
            throw SipError.engineNotStarted
        }
        guard !destination.isEmpty else {
            throw SipError.invalidParameter("Message destination is empty")
        }
        guard !body.isEmpty else {
            throw SipError.invalidParameter("Message body is empty")
        }

        let uri = sipUri(for: destination)
        var messageId: UInt64 = 0
        let rc = body.withUnsafeBytes { raw -> Int32 in
            sip_client_send_message(client, uri, contentType,
                                    raw.bindMemory(to: UInt8.self).baseAddress, raw.count, &messageId)
        }
        guard rc == 0 else {
            throw SipError.messageSendFailed(Int(rc), "sip_client_send_message failed for URI: \(uri)")
        }
        return messageId
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

        sip_call_set_user_data(callHandle, Unmanaged.passUnretained(sipCall).toOpaque())
        calls.append(sipCall)

        _ = IosAudioManager.shared.prepareCallAudioSession()

        eventContinuation.yield(.incomingCall(sipCall))
        eventContinuation.yield(.callStateChanged(sipCall, .incoming))

        // Automatically report to CallKit coordinator
        Task {
            do {
                try await callKit.reportIncomingCall(sipCall)
            } catch {
                NSLog("[SipSdk] CallKit incoming report failed: %@", error.localizedDescription)
            }
        }
    }

    private func handleCallState(callHandle: OpaquePointer, state: sip_call_state_t, code: Int, reason: String) {
        let sipCall = findOrCreateCall(for: callHandle)

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
            IosAudioManager.shared.releaseCallAudioSessionIfIdle()
        }
    }

    private func handleCallMedia(callHandle: OpaquePointer, width: Int, height: Int, onHold: Bool, isMuted: Bool) {
        let sipCall = findOrCreateCall(for: callHandle)
        sipCall.updateMedia(width: width, height: height, onHold: onHold, isMuted: isMuted)
        eventContinuation.yield(.mediaUpdated(sipCall, width: width, height: height, onHold: onHold, isMuted: isMuted))
    }

    private func handleDtmf(callHandle: OpaquePointer, digit: String) {
        let sipCall = findOrCreateCall(for: callHandle)
        eventContinuation.yield(.dtmfReceived(sipCall, digit: digit))
    }

    private func handleCallStats(callHandle: OpaquePointer, stats: SipCallStats) {
        if let sipCall = calls.first(where: { $0.coreCall == callHandle }) {
            sipCall.updateStats(stats)
            eventContinuation.yield(.callStatsUpdated(sipCall, stats))
        }
    }

    private func handleTransferStatus(callHandle: OpaquePointer, code: Int, reason: String) {
        if let call = calls.first(where: { $0.coreCall == callHandle }) {
            eventContinuation.yield(.transferStatusReceived(call, code: code, reason: reason))
        }
    }

    private func handleTransferRequest(callHandle: OpaquePointer, referToUri: String, hasReplaces: Bool) {
        if let call = calls.first(where: { $0.coreCall == callHandle }) {
            eventContinuation.yield(.transferRequested(call, referToUri: referToUri, hasReplaces: hasReplaces))
        }
    }

    private func handleMessageReceived(_ message: SipIncomingMessage) {
        eventContinuation.yield(.messageReceived(message))
    }

    private func handleMessageStatus(id: UInt64, status: sip_message_status_t, code: Int, reason: String) {
        let newStatus = SipMessageStatus(rawValue: Int(status.rawValue)) ?? .failed
        eventContinuation.yield(.messageStatusChanged(id: id, status: newStatus, code: code, reason: reason))
    }

    private func findOrCreateCall(for callHandle: OpaquePointer) -> SipCall {
        let userData = sip_call_get_user_data(callHandle)
        if let userData = userData {
            return Unmanaged<SipCall>.fromOpaque(userData).takeUnretainedValue()
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
        sip_call_set_user_data(callHandle, Unmanaged.passUnretained(sipCall).toOpaque())
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
