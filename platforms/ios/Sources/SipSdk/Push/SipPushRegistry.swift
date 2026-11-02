//
//  SipPushRegistry.swift
//  SipSdk (iOS Platform)
//

import Foundation
import PushKit

/// Incoming VoIP Push notification payload data.
public struct SipIncomingPushPayload: @unchecked Sendable {
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
        rawPayload: [AnyHashable: Any] = [:]
    ) {
        self.callerUri = callerUri
        self.callerName = callerName
        self.hasVideo = hasVideo
        self.uuid = uuid
        self.rawPayload = rawPayload
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

        let dict = payload.dictionaryPayload
        let callerUri = (dict["caller_uri"] as? String) ?? (dict["from"] as? String) ?? "Unknown"
        let callerName = (dict["caller_name"] as? String) ?? (dict["display_name"] as? String)
        let hasVideo = (dict["video"] as? Bool) ?? (dict["has_video"] as? Bool) ?? false

        var callUuid = UUID()
        if let uuidStr = dict["call_id"] as? String, let parsed = UUID(uuidString: uuidStr) {
            callUuid = parsed
        }

        let incomingPayload = SipIncomingPushPayload(
            callerUri: callerUri,
            callerName: callerName,
            hasVideo: hasVideo,
            uuid: callUuid,
            rawPayload: dict
        )

        if let handler = onIncomingPush {
            handler(incomingPayload, completion)
        } else {
            completion()
        }
    }
}
