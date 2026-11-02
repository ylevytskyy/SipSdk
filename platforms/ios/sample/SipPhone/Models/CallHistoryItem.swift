//
//  CallHistoryItem.swift
//  SipPhone (iOS Sample App)
//

import Foundation

public enum CallDirection: String, Codable, Sendable {
    case outgoing
    case incoming
    case missed
}

public struct CallHistoryItem: Identifiable, Codable, Sendable {
    public let id: UUID
    public let remoteUri: String
    public let displayName: String?
    public let date: Date
    public let durationSeconds: UInt32
    public let direction: CallDirection
    public let hasVideo: Bool

    public init(
        id: UUID = UUID(),
        remoteUri: String,
        displayName: String? = nil,
        date: Date = Date(),
        durationSeconds: UInt32 = 0,
        direction: CallDirection = .outgoing,
        hasVideo: Bool = true
    ) {
        self.id = id
        self.remoteUri = remoteUri
        self.displayName = displayName
        self.date = date
        self.durationSeconds = durationSeconds
        self.direction = direction
        self.hasVideo = hasVideo
    }

    public var formattedDuration: String {
        let m = durationSeconds / 60
        let s = durationSeconds % 60
        return String(format: "%02d:%02d", m, s)
    }

    public var formattedDate: String {
        let formatter = DateFormatter()
        if Calendar.current.isDateInToday(date) {
            formatter.dateFormat = "HH:mm"
        } else {
            formatter.dateFormat = "MMM d, HH:mm"
        }
        return formatter.string(from: date)
    }

    public var displayTitle: String {
        if let name = displayName, !name.isEmpty {
            return name
        }
        if remoteUri.hasPrefix("sip:") {
            let stripped = remoteUri.dropFirst(4)
            if let atIdx = stripped.firstIndex(of: "@") {
                return String(stripped[..<atIdx])
            }
            return String(stripped)
        }
        return remoteUri
    }
}
