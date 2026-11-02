//
//  IosAudioManager.swift
//  SipSdk (iOS Platform)
//

import Foundation
import AVFoundation

/// Manages native iOS VoIP hardware audio session configuration and routing.
public final class IosAudioManager: @unchecked Sendable {
    public static let shared = IosAudioManager()

    private let lock = NSLock()
    private var activeCallCount = 0
    private var isSessionActive = false

    public var isSpeakerEnabled: Bool = false {
        didSet {
            applySpeakerphone(isSpeakerEnabled)
        }
    }

    private init() {
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

        let session = AVAudioSession.sharedInstance()
        do {
            var options: AVAudioSession.CategoryOptions = [.allowBluetoothHFP, .allowBluetoothA2DP]
            if isSpeakerEnabled {
                options.insert(.defaultToSpeaker)
            }

            try session.setCategory(.playAndRecord, mode: .voiceChat, options: options)
            try session.setActive(true, options: [])
            isSessionActive = true

            if isSpeakerEnabled {
                try session.overrideOutputAudioPort(.speaker)
            } else {
                try session.overrideOutputAudioPort(.none)
            }
            return true
        } catch {
            NSLog("[SipSdk] Failed to activate VoIP AVAudioSession: %@", error.localizedDescription)
            return false
        }
    }

    /// Releases AVAudioSession when all calls are ended.
    public func releaseCallAudioSessionIfIdle() {
        lock.lock()
        defer { lock.unlock() }

        if activeCallCount > 0 {
            activeCallCount -= 1
        }

        guard activeCallCount == 0, isSessionActive else { return }

        let session = AVAudioSession.sharedInstance()
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
        isSpeakerEnabled = enabled
        lock.unlock()
        applySpeakerphone(enabled)
    }

    private func applySpeakerphone(_ enabled: Bool) {
        let session = AVAudioSession.sharedInstance()
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
            let speaker = isSpeakerEnabled
            lock.unlock()
            applySpeakerphone(speaker)
        }
    }
}
