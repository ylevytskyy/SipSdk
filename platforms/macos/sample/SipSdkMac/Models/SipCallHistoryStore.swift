import Foundation

public struct CallRecord: Identifiable, Codable {
    public let id: UUID
    public let remoteUri: String
    public let direction: String // "incoming" or "outgoing"
    public let hasVideo: Bool
    public let durationSeconds: UInt32
    public let timestamp: Date
    public let status: String

    public init(id: UUID = UUID(),
                remoteUri: String,
                direction: String,
                hasVideo: Bool,
                durationSeconds: UInt32,
                timestamp: Date = Date(),
                status: String) {
        self.id = id
        self.remoteUri = remoteUri
        self.direction = direction
        self.hasVideo = hasVideo
        self.durationSeconds = durationSeconds
        self.timestamp = timestamp
        self.status = status
    }
}

public final class SipCallHistoryStore: ObservableObject {
    private static let key = "sipsdk.mac.call_history"

    @Published public private(set) var records: [CallRecord] = []

    public init() {
        load()
    }

    public func addRecord(remoteUri: String,
                          direction: String,
                          hasVideo: Bool,
                          durationSeconds: UInt32,
                          status: String) {
        let record = CallRecord(remoteUri: remoteUri,
                                direction: direction,
                                hasVideo: hasVideo,
                                durationSeconds: durationSeconds,
                                status: status)
        records.insert(record, at: 0)
        save()
    }

    public func clear() {
        records.removeAll()
        save()
    }

    private func save() {
        if let encoded = try? JSONEncoder().encode(records) {
            UserDefaults.standard.set(encoded, forKey: Self.key)
        }
    }

    private func load() {
        if let data = UserDefaults.standard.data(forKey: Self.key),
           let decoded = try? JSONDecoder().decode([CallRecord].self, from: data) {
            self.records = decoded
        }
    }
}
