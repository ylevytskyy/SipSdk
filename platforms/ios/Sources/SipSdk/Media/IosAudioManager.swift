//
//  IosAudioManager.swift
//  SipSdk (iOS Platform)
//

import Foundation
import AVFoundation

/// The `AVAudioSession` calls that `IosAudioManager` makes. Tests replace it with a fake.
protocol CallAudioSession: AnyObject {
    func setCategory(_ category: AVAudioSession.Category, mode: AVAudioSession.Mode, options: AVAudioSession.CategoryOptions) throws
    func setActive(_ active: Bool, options: AVAudioSession.SetActiveOptions) throws
    func overrideOutputAudioPort(_ portOverride: AVAudioSession.PortOverride) throws
}

extension AVAudioSession: CallAudioSession {}

/// Manages native iOS VoIP hardware audio session configuration and routing.
///
/// `activeCallCount` sums independent holders, each pairing prepare with release:
/// `SipClient` once per call, the core engine once per period with active calls,
/// and CallKit once per audio activation. The session is deactivated at zero.
public final class IosAudioManager: @unchecked Sendable {
    public static let shared = IosAudioManager(session: { AVAudioSession.sharedInstance() })

    private let lock = NSLock()
    private let session: () -> CallAudioSession
    private(set) var activeCallCount = 0
    private(set) var isSessionActive = false

    /// Guarded by `lock`.
    private var speakerEnabled = false

    public var isSpeakerEnabled: Bool {
        get {
            lock.lock()
            defer { lock.unlock() }
            return speakerEnabled
        }
        set { setSpeakerphone(newValue) }
    }

    init(session: @escaping () -> CallAudioSession) {
        self.session = session
        setupNotifications()
    }

    deinit {
        NotificationCenter.default.removeObserver(self)
    }

    /// Prepares and activates AVAudioSession for VoIP calling.
    @discardableResult
    public func prepareCallAudioSession() -> Bool {
        lock.lock()
        defer { lock.unlock() }

        activeCallCount += 1
        if isSessionActive {
            return true
        }

        let session = self.session()
        do {
            var options: AVAudioSession.CategoryOptions = [.allowBluetoothHFP, .allowBluetoothA2DP]
            if speakerEnabled {
                options.insert(.defaultToSpeaker)
            }

            try session.setCategory(.playAndRecord, mode: .voiceChat, options: options)
            try session.setActive(true, options: [])
            isSessionActive = true
        } catch {
            // A failed activation is not an active call; callers do not release it.
            activeCallCount -= 1
            NSLog("[SipSdk] Failed to activate VoIP AVAudioSession: %@", error.localizedDescription)
            return false
        }

        // The session is active; a failed output route only logs.
        applySpeakerphone(speakerEnabled)
        return true
    }

    /// Releases AVAudioSession when all calls are ended.
    public func releaseCallAudioSessionIfIdle() {
        lock.lock()
        defer { lock.unlock() }

        if activeCallCount > 0 {
            activeCallCount -= 1
        }

        guard activeCallCount == 0, isSessionActive else { return }

        let session = self.session()
        do {
            try session.setActive(false, options: [.notifyOthersOnDeactivation])
            isSessionActive = false
        } catch {
            NSLog("[SipSdk] Failed to deactivate VoIP AVAudioSession: %@", error.localizedDescription)
        }
    }

    /// Toggles between ear-piece and speakerphone.
    public func setSpeakerphone(_ enabled: Bool) {
        lock.lock()
        speakerEnabled = enabled
        lock.unlock()
        applySpeakerphone(enabled)
    }

    private func applySpeakerphone(_ enabled: Bool) {
        let session = self.session()
        do {
            if enabled {
                try session.overrideOutputAudioPort(.speaker)
            } else {
                try session.overrideOutputAudioPort(.none)
            }
        } catch {
            NSLog("[SipSdk] Failed to set speakerphone override: %@", error.localizedDescription)
        }
    }

    private func setupNotifications() {
        NotificationCenter.default.addObserver(
            self,
            selector: #selector(handleRouteChange),
            name: AVAudioSession.routeChangeNotification,
            object: nil
        )
    }

    @objc private func handleRouteChange(notification: Notification) {
        guard let userInfo = notification.userInfo,
              let reasonValue = userInfo[AVAudioSessionRouteChangeReasonKey] as? UInt,
              let reason = AVAudioSession.RouteChangeReason(rawValue: reasonValue) else { return }

        if reason == .oldDeviceUnavailable {
            // Re-apply speakerphone state if Bluetooth or headphones disconnected
            lock.lock()
            let speaker = speakerEnabled
            lock.unlock()
            applySpeakerphone(speaker)
        }
    }
}
