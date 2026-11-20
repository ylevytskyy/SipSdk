//
//  SipPushRegistry.swift
//  SipSdk (iOS Platform)
//

import Foundation
import PushKit

/// Incoming VoIP Push notification payload data.
public struct SipIncomingPushPayload: @unchecked Sendable {
    public let sipCallId: String?
    public let callerUri: String
    public let callerName: String?
    public let hasVideo: Bool
    public let uuid: UUID
    public let rawPayload: [AnyHashable: Any]

    public init(
        callerUri: String,
        callerName: String? = nil,
        hasVideo: Bool = false,
        uuid: UUID = UUID(),
        sipCallId: String? = nil,
        rawPayload: [AnyHashable: Any] = [:]
    ) {
        self.sipCallId = sipCallId
        self.callerUri = callerUri
        self.callerName = callerName
        self.hasVideo = hasVideo
        self.uuid = uuid
        self.rawPayload = rawPayload
    }

    /// Parses unified push keys (`sip_call_id`/`call_id`, `caller_uri`/`from_uri`/`from`,
    /// `display_name`/`caller_name`, `has_video`/`video`).
    public init(dictionary: [AnyHashable: Any]) {
        let sipCallId = (dictionary["sip_call_id"] as? String)
            ?? (dictionary["call_id"] as? String)
        let callerUri = (dictionary["caller_uri"] as? String)
            ?? (dictionary["from_uri"] as? String)
            ?? (dictionary["from"] as? String)
            ?? "Unknown"
        let callerName = (dictionary["display_name"] as? String)
            ?? (dictionary["caller_name"] as? String)
        let hasVideo = Self.parseBool(dictionary["has_video"])
            ?? Self.parseBool(dictionary["video"])
            ?? false

        var callUuid = UUID()
        if let uuidStr = (dictionary["uuid"] as? String)
            ?? (dictionary["call_id"] as? String)
            ?? (dictionary["sip_call_id"] as? String),
           let parsed = UUID(uuidString: uuidStr) {
            callUuid = parsed
        }

        self.init(
            callerUri: callerUri,
            callerName: callerName,
            hasVideo: hasVideo,
            uuid: callUuid,
            sipCallId: sipCallId,
            rawPayload: dictionary
        )
    }

    private static func parseBool(_ value: Any?) -> Bool? {
        if let boolVal = value as? Bool { return boolVal }
        if let intVal = value as? Int { return intVal != 0 }
        if let strVal = value as? String {
            switch strVal.trimmingCharacters(in: .whitespacesAndNewlines).lowercased() {
            case "1", "true", "yes": return true
            case "0", "false", "no": return false
            default: return nil
            }
        }
        return nil
    }
}

/// VoIP PushKit registry manager for waking the app up on incoming SIP calls.
public final class SipPushRegistry: NSObject, PKPushRegistryDelegate, @unchecked Sendable {
    public static let shared = SipPushRegistry()

    private var pushRegistry: PKPushRegistry?
    public private(set) var voipToken: String?

    public var onTokenUpdated: ((String) -> Void)?
    public var onIncomingPush: ((SipIncomingPushPayload, @escaping () -> Void) -> Void)?

    private override init() {
        super.init()
    }

    /// Starts observing VoIP push notifications.
    public func start() {
        DispatchQueue.main.async { [weak self] in
            guard let self = self else { return }
            let registry = PKPushRegistry(queue: DispatchQueue.main)
            registry.delegate = self
            registry.desiredPushTypes = [.voIP]
            self.pushRegistry = registry
        }
    }

    /// Stops observing VoIP push notifications.
    public func stop() {
        DispatchQueue.main.async { [weak self] in
            self?.pushRegistry?.desiredPushTypes = []
            self?.pushRegistry = nil
        }
    }

    // MARK: - PKPushRegistryDelegate

    public func pushRegistry(_ registry: PKPushRegistry, didUpdate pushCredentials: PKPushCredentials, for type: PKPushType) {
        guard type == .voIP else { return }
        let token = pushCredentials.token.map { String(format: "%02.2hhx", $0) }.joined()
        self.voipToken = token
        NSLog("[SipSdk] Received VoIP Push Token: %@", token)
        onTokenUpdated?(token)
    }

    public func pushRegistry(_ registry: PKPushRegistry, didInvalidatePushTokenFor type: PKPushType) {
        if type == .voIP {
            self.voipToken = nil
        }
    }

    public func pushRegistry(
        _ registry: PKPushRegistry,
        didReceiveIncomingPushWith payload: PKPushPayload,
        for type: PKPushType,
        completion: @escaping () -> Void
    ) {
        guard type == .voIP else {
            completion()
            return
        }

        let incomingPayload = SipIncomingPushPayload(dictionary: payload.dictionaryPayload)

        if let handler = onIncomingPush {
            handler(incomingPayload, completion)
        } else {
            completion()
        }
    }
}
