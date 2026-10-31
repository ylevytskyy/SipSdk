//
//  SipCallHistoryStore.swift
//  SipSdk
//
//  Persists recent audio and video SIP call log entries in UserDefaults.
//

import Foundation

@objcMembers
final class SipCallHistoryEntry: NSObject, Codable {
    let id: String
    let remoteUri: String
    let isOutgoing: Bool
    let hasVideo: Bool
    let timestamp: Date
    let durationSeconds: Int
    let outcome: String

    init(id: String = UUID().uuidString,
         remoteUri: String,
         isOutgoing: Bool,
         hasVideo: Bool,
         timestamp: Date = Date(),
         durationSeconds: Int,
         outcome: String) {
        self.id = id
        self.remoteUri = remoteUri
        self.isOutgoing = isOutgoing
        self.hasVideo = hasVideo
        self.timestamp = timestamp
        self.durationSeconds = max(0, durationSeconds)
        self.outcome = outcome
        super.init()
    }

    var formattedSummary: String {
        let directionIcon = isOutgoing ? "↗" : "↙"
        let mediaBadge = hasVideo ? "Video" : "Audio"
        let mins = durationSeconds / 60
        let secs = durationSeconds % 60
        let durationText = durationSeconds > 0 ? String(format: "%02d:%02d", mins, secs) : outcome
        return "\(directionIcon) [\(mediaBadge)] \(remoteUri) (\(durationText))"
    }
}

@objcMembers
final class SipCallHistoryStore: NSObject {
    static let shared = SipCallHistoryStore()

    private let defaults: UserDefaults
    private let storageKey = "com.wiseland.SipSdk.callHistory.v1"
    private let maxEntries = 50

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
        super.init()
    }

    func entries() -> [SipCallHistoryEntry] {
        guard let data = defaults.data(forKey: storageKey),
              let list = try? JSONDecoder().decode([SipCallHistoryEntry].self, from: data) else {
            return []
        }
        return list
    }

    @discardableResult
    func recordCall(remoteUri: String,
                    isOutgoing: Bool,
                    hasVideo: Bool,
                    durationSeconds: Int,
                    outcome: String) -> SipCallHistoryEntry {
        let entry = SipCallHistoryEntry(remoteUri: remoteUri,
                                        isOutgoing: isOutgoing,
                                        hasVideo: hasVideo,
                                        durationSeconds: durationSeconds,
                                        outcome: outcome)
        var current = entries()
        current.insert(entry, at: 0)
        if current.count > maxEntries {
            current = Array(current.prefix(maxEntries))
        }
        if let data = try? JSONEncoder().encode(current) {
            defaults.set(data, forKey: storageKey)
        }
        return entry
    }

    func clearAll() {
        defaults.removeObject(forKey: storageKey)
    }
}
