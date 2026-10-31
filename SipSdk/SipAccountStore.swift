//
//  SipAccountStore.swift
//  SipSdk
//
//  Persists SIP account configuration in UserDefaults with fallback to bundled SipCredentials.plist.
//

import Foundation

@objcMembers
final class SipAccountProfile: NSObject, Codable {
    var username: String
    var domain: String
    var password: String
    var displayName: String
    var port: Int
    var transport: String
    var videoEnabledByDefault: Bool

    init(username: String,
         domain: String,
         password: String,
         displayName: String = "",
         port: Int = 5060,
         transport: String = "udp",
         videoEnabledByDefault: Bool = true) {
        self.username = username
        self.domain = domain
        self.password = password
        self.displayName = displayName
        self.port = port > 0 ? port : 5060
        self.transport = transport.isEmpty ? "udp" : transport.lowercased()
        self.videoEnabledByDefault = videoEnabledByDefault
        super.init()
    }

    var isValid: Bool {
        !username.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty &&
        !domain.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
    }

    var aorDisplay: String {
        let cleanUser = username.trimmingCharacters(in: .whitespacesAndNewlines)
        let cleanDomain = domain.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !cleanUser.isEmpty, !cleanDomain.isEmpty else { return "Unconfigured" }
        return "sip:\(cleanUser)@\(cleanDomain)"
    }
}

@objcMembers
final class SipAccountStore: NSObject {
    static let shared = SipAccountStore()

    private let defaults: UserDefaults
    private let storageKey = "com.wiseland.SipSdk.accountProfile.v1"

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
        super.init()
    }

    func hasSavedProfile() -> Bool {
        defaults.data(forKey: storageKey) != nil
    }

    func loadProfile() -> SipAccountProfile? {
        if let data = defaults.data(forKey: storageKey),
           let decoded = try? JSONDecoder().decode(SipAccountProfile.self, from: data),
           decoded.isValid {
            return decoded
        }
        return Self.bundledPlistProfile()
    }

    func saveProfile(_ profile: SipAccountProfile) {
        if let encoded = try? JSONEncoder().encode(profile) {
            defaults.set(encoded, forKey: storageKey)
        }
    }

    func clearSavedProfile() {
        defaults.removeObject(forKey: storageKey)
    }

    static func bundledPlistProfile(bundle: Bundle = .main) -> SipAccountProfile? {
        guard let url = bundle.url(forResource: "SipCredentials", withExtension: "plist"),
              let dictionary = NSDictionary(contentsOf: url) as? [String: Any],
              let username = dictionary["username"] as? String,
              let domain = dictionary["domain"] as? String,
              let password = dictionary["password"] as? String,
              !username.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty,
              !domain.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else {
            return nil
        }
        let displayName = (dictionary["displayName"] as? String) ?? ""
        let port = (dictionary["port"] as? Int) ?? 5060
        let transport = (dictionary["transport"] as? String) ?? "udp"
        return SipAccountProfile(username: username,
                                 domain: domain,
                                 password: password,
                                 displayName: displayName,
                                 port: port,
                                 transport: transport,
                                 videoEnabledByDefault: true)
    }
}
